// Copyright (c) 2012-2013 The Peercoin developers
// Copyright (c) 2017-2021 The Denarius developers
// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/assign/list_of.hpp>

#include "kernel.h"
#include "cold_hot_seam.h"
#include "blockindex_authoritative_live.h"
#include "blockindex_authoritative_startup.h"
#include "blockindex_shadow_startup.h"
#include "txdb.h"
#include "main.h"
#include "ibdmetrics.h"
#include "wallet.h"
#include "init.h"

using namespace std;

typedef std::map<int, unsigned int> MapModifierCheckpoints;

// Hard checkpoints of stake modifiers to ensure they are deterministic
static std::map<int, unsigned int> mapStakeModifierCheckpoints =
    boost::assign::map_list_of
        //( 0, 0x0e00670b )
        ( 100000, 0xcf12d0aa )
        ( 150000, 0xf82ed306 )
		( 200000, 0xc4bdc2b5 )
		( 300000, 0x277bdc9c )
        ( 400000, 0x4acf11a7 )
        ( 500000, 0xe7031a9d )
        ( 600000, 0x3a2dc65d )
        ( 700000, 0x231e46a9 )
        ( 800000, 0x81c6576c )
        ( 900000, 0xa9cc8eb9 )
        ( 1000000, 0x5e1a8a47 )
        ( 1250000, 0xac256c99 )
        ( 1500000, 0xfa70e840 )
        ( 1840000, 0xda8d97e2 )
        ( 1848420, 0x628c1cb7 )
        ( 2000000, 0x38a611f6 )
        ( 2080000, 0xae3db09c )
        ( 2198000, 0xd404caf7 )
        ( 2250000, 0x182e564d )
        ( 2500000, 0xd4445400 )
        ( 2750000, 0x316254f2 )
        ( 3000000, 0x0388f231 )
        ( 3250000, 0xb1ad3c9a )
        ( 3500000, 0x5173956c )
        ( 3750000, 0x85f34d15 )
        ( 4000000, 0x766ad216 )
        ( 4250000, 0xb279f13e )
        ( 4500000, 0x7660d0c4 )
        ( 4750000, 0x6f190913 )
        ( 5000000, 0xc8bcbfb6 )
        ( 5250000, 0x6d6f1999 )
        ( 5500000, 0x09e3eb62 )
        ( 5750000, 0x1098297b )
        ( 6000000, 0xb69b2173 )
        ( 6250000, 0x26326c94 )
        ( 6500000, 0x475ef328 )
        ( 6750000, 0xc3c3435a )
    ;

// Hard checkpoints of stake modifiers to ensure they are deterministic (testNet)
static std::map<int, unsigned int> mapStakeModifierCheckpointsTestNet =
    boost::assign::map_list_of
        ( 9999999999, 0x4038ad82 ) //technically two
    ;

static std::map<int, uint64_t> mapStakeModifierCache;
static CCriticalSection cs_stakeModifierCache;

void CacheStakeModifier(int nHeight, uint64_t nStakeModifier)
{
    int nCheckpoint = (nHeight / 1000) * 1000;
    LOCK(cs_stakeModifierCache);
    if (mapStakeModifierCache.find(nCheckpoint) == mapStakeModifierCache.end())
    {
        mapStakeModifierCache[nCheckpoint] = nStakeModifier;
        if (fDebug && fHybridSPV)
            printf("HybridSPV: Cached stake modifier at height %d: 0x%016" PRIx64"\n",
                   nCheckpoint, nStakeModifier);
    }
}

bool GetCachedStakeModifier(int nHeight, uint64_t& nStakeModifier)
{
    int nCheckpoint = (nHeight / 1000) * 1000;
    LOCK(cs_stakeModifierCache);

    auto it = mapStakeModifierCache.find(nCheckpoint);
    if (it != mapStakeModifierCache.end())
    {
        nStakeModifier = it->second;
        return true;
    }

    auto checkpointIt = mapStakeModifierCheckpoints.find(nCheckpoint);
    if (checkpointIt != mapStakeModifierCheckpoints.end())
    {
        nStakeModifier = checkpointIt->second;
        return true;
    }

    return false;
}

// Get time weight
int64_t GetWeight(int64_t nIntervalBeginning, int64_t nIntervalEnd)
{
    // Kernel hash weight starts from 0 at the min age
    // this change increases active coins participating the hash and helps
    // to secure the network when proof-of-stake difficulty is low

    int64_t nWeight = nIntervalEnd - nIntervalBeginning - nStakeMinAge;
    if (nWeight < 0)
        return 0;
    return min(nWeight, (int64_t)nStakeMaxAge);
}

// Get the last stake modifier and its generation time from a given block
static bool GetLastStakeModifier(const CBlockIndex* pindex, uint64_t& nStakeModifier, int64_t& nModifierTime)
{
    if (!pindex)
        return error("GetLastStakeModifier: null pindex");
    while (pindex && pindex->pprev && !pindex->GeneratedStakeModifier())
        pindex = pindex->pprev;
    if (!pindex->GeneratedStakeModifier())
        return error("GetLastStakeModifier: no generation at genesis block");
    nStakeModifier = pindex->nStakeModifier;
    nModifierTime = pindex->GetBlockTime();
    return true;
}

// Get selection interval section (in seconds)
static int64_t GetStakeModifierSelectionIntervalSection(int nSection)
{
    if (nSection < 0 || nSection >= 64)
        return 0;
    return (nModifierInterval * 63 / (63 + ((63 - nSection) * (MODIFIER_INTERVAL_RATIO - 1))));
}

// Get stake modifier selection interval (in seconds)
static int64_t GetStakeModifierSelectionIntervalInternal()
{
    int64_t nSelectionInterval = 0;
    for (int nSection=0; nSection<64; nSection++)
        nSelectionInterval += GetStakeModifierSelectionIntervalSection(nSection);
    return nSelectionInterval;
}

// Exported non-static wrapper so by-value navigation (cold_hot_seam.cpp) can
// reuse the exact legacy selection-interval arithmetic.
int64_t GetStakeModifierSelectionInterval()
{
    return GetStakeModifierSelectionIntervalInternal();
}

// ---------------------------------------------------------------------------
// F1 — authoritative stake-modifier BY-VALUE cutover.
//
// Stake-modifier truth must not depend on CBlockIndex residency: no fixed
// height window can serve this algorithm, because both halves are bounded by
// CHAIN TIME, not by a block count:
//   * the "last generated modifier" walk  <= nModifierInterval  (600 s)
//   * the generation candidate window     =  nSelectionInterval (21135 s)
// At 1 s spacing those are ~600 and ~21135 blocks. Deepening tip-window (BlockIndexHotOwner) or
// materializing ancestors is therefore architecturally wrong.
//
// ONE selection algorithm, TWO input providers:
//   * VALIDATE_RECORD_INDEX (resident): reproduces the legacy behaviour exactly,
//     including the legacy mapBlockIndex guard (expressed as record data, so the
//     legacy error/abort ORDER is preserved bit-for-bit);
//   * by-value: walks the logical parent chain by value (cold_hot_seam) and
//     never touches mapBlockIndex / pprev / CBlockIndex materialization.
// Authority failures fail closed: a partial/truncated candidate set is never
// used to compute a modifier.
//
// A candidate is fully describable by value: the algorithm reads only time,
// hash, hashProof, nFlags (=> IsProofOfStake / GetStakeEntropyBit) and height.
// Pointer identity is never needed by any consumer.
// ---------------------------------------------------------------------------
struct StakeModifierCandidateValue
{
    int64_t nTime;
    uint256 hash;
    uint256 hashProof;
    unsigned int nFlags;
    int nHeight;
    // Provider guarantee that this candidate resolves to a block-index record.
    // The resident provider mirrors the legacy mapBlockIndex guard (a false
    // value reproduces the legacy error and aborts selection); the by-value
    // provider always has the record by construction.
    bool fHaveRecord;

    StakeModifierCandidateValue() : nTime(0), hash(0), hashProof(0), nFlags(0), nHeight(0), fHaveRecord(false) {}

    bool IsProofOfStake() const { return (nFlags & CBlockIndex::BLOCK_PROOF_OF_STAKE) != 0; }
    unsigned int GetStakeEntropyBit() const { return (nFlags & CBlockIndex::BLOCK_STAKE_ENTROPY) >> 1; }
};

// Candidate ordering used by both providers: exactly the legacy
// `std::sort(vector<pair<int64_t,uint256> >)` order (time first, then hash).
// Candidate keys are unique (a hash identifies one block), so the sorted
// sequence is uniquely determined independently of the sort algorithm.
struct StakeModifierCandidateLess
{
    bool operator()(const StakeModifierCandidateValue& a, const StakeModifierCandidateValue& b) const
    {
        if (a.nTime != b.nTime)
            return a.nTime < b.nTime;
        return a.hash < b.hash;
    }
};

// Round-invariant selection hash for one candidate: hashProof || prevModifier,
// PoS-adjusted (>>32). Identical construction to the legacy inline precompute.
static uint256 StakeModifierSelectionHash(const StakeModifierCandidateValue& c, uint64_t nStakeModifierPrev)
{
    CDataStream ss(SER_GETHASH, 0);
    ss << c.hashProof << nStakeModifierPrev;
    uint256 h = Hash(ss.begin(), ss.end());
    if (c.IsProofOfStake())
        h >>= 32;
    return h;
}

// Select a block from the candidate records, excluding already selected hashes,
// with timestamp up to nSelectionIntervalStop. vSelHash[i] is the precomputed
// selection hash for vCandidates[i] so each candidate is hashed exactly once
// across the 64 rounds. Returns false in exactly the legacy failure cases
// (no candidate selectable, or a candidate without a resolved record - the
// legacy error is emitted here, and the caller adds its own round wrapper).
static bool SelectBlockFromCandidateValues(
    const vector<StakeModifierCandidateValue>& vCandidates,
    const vector<uint256>& vSelHash,
    set<uint256>& setSelectedBlocks,
    int64_t nSelectionIntervalStop, size_t* pnSelectedIndex)
{
    bool fSelected = false;
    uint256 hashBest = 0;
    *pnSelectedIndex = 0;
    for (size_t i = 0; i < vCandidates.size(); i++)
    {
        const StakeModifierCandidateValue& candidate = vCandidates[i];
        if (!candidate.fHaveRecord)
            return error("SelectBlockFromCandidates: failed to find block index for candidate block %s", candidate.hash.ToString().c_str());
        if (fSelected && candidate.nTime > nSelectionIntervalStop)
            break;
        if (setSelectedBlocks.count(candidate.hash) > 0)
            continue;
        const uint256& hashSelection = vSelHash[i];
        if (fSelected && hashSelection < hashBest)
        {
            hashBest = hashSelection;
            *pnSelectedIndex = i;
        }
        else if (!fSelected)
        {
            fSelected = true;
            hashBest = hashSelection;
            *pnSelectedIndex = i;
        }
    }
    if (fDebug && GetBoolArg("-printstakemodifier"))
        printf("SelectBlockFromCandidates: selection hash=%s\n", hashBest.ToString().c_str());
    return fSelected;
}

// LEGACY RESIDENT PROVIDER: adapt the ordered candidate hashes into value
// records from mapBlockIndex. A candidate absent from the resident map keeps
// fHaveRecord=false so the selection algorithm emits the exact legacy error at
// the exact legacy position (this is the F1(b) residency dependency).
static bool CollectCandidatesResident(
    const vector<pair<int64_t, uint256> >& vSortedByTimestamp,
    vector<StakeModifierCandidateValue>& vOut)
{
    vOut.clear();
    vOut.reserve(vSortedByTimestamp.size());
    for (size_t i = 0; i < vSortedByTimestamp.size(); i++)
    {
        const PAIRTYPE(int64_t, uint256)& item = vSortedByTimestamp[i];
        StakeModifierCandidateValue c;
        c.hash = item.second;
        c.fHaveRecord = (mapBlockIndex.count(item.second) > 0);
        if (c.fHaveRecord)
        {
            const CBlockIndex* pindex = mapBlockIndex[item.second];
            c.nTime = pindex->GetBlockTime();
            c.hash = pindex->GetBlockHash();
            c.hashProof = pindex->hashProof;
            c.nFlags = pindex->nFlags;
            c.nHeight = pindex->nHeight;
        }
        vOut.push_back(c);
    }
    return true;
}

// AUTHORITATIVE BY-VALUE CONTINUATION (F1(b)): collect the REMAINDER of the
// generation candidate window in the by-value domain, starting at `hashStart`
// (the first block BELOW the live chain's residency floor, obtained from the
// floor's by-value parent edge). Walks logical parents BY VALUE down to the
// exact chain-TIME boundary: no mapBlockIndex, no CBlockIndex materialization,
// no fixed height depth, no residency growth. A partial window is NEVER
// returned: any navigation authority failure aborts (fail closed), so a
// truncated candidate set can never silently yield a different modifier.
// Appends to vOut in walk order; the caller sorts once for both providers.
// Resolve a block's authoritative by-value snapshot WITHOUT any historical
// pointer traversal. Source precedence (singleton-free except the legacy
// fallback): (1) the pinned COLD generation; (2) the navigator's injected
// by-value hot oracle (test or production), whose snapshots carry REAL parent
// edges; (3) only when the hot side is the legacy pointer accessor, the
// authoritative live snapshot (tip-first-then-base). A HOT result is never
// treated as COLD; genesis is never inferred from a resident pprev==NULL
// boundary; zero metadata is never substituted. Returns OK / NOT_FOUND /
// AUTHORITY_FAILURE; callers fail closed on anything but OK.
static ColdHotSeamResult ResolveAuthoritySnapshot(const ColdHotSeamNavigator* nav,
    const uint256& hash, BlockIndexSnapshot* out, std::string* err)
{
    ColdHotSeamSnapshot cur;
    const ColdHotSeamResult r = nav->ResolveLogicalR(BlockIndexLogicalId(hash), &cur, err);
    if (r == COLD_HOT_SEAM_AUTHORITY_FAILURE)
        return COLD_HOT_SEAM_AUTHORITY_FAILURE;
    const bool fByValueHot = (r == COLD_HOT_SEAM_OK) && !cur.ref.IsCold()
                           && nav->HasByValueHotResolver();
    if (r == COLD_HOT_SEAM_OK && (cur.ref.IsCold() || fByValueHot))
    {
        *out = cur.snapshot;      // cold generation, or authoritative by-value hot
        return COLD_HOT_SEAM_OK;
    }
    // Legacy pointer hot side (or cold miss): the live snapshot is authoritative.
    if (BlockIndexAuthoritativeLive* liveAuth = GetAuthoritativeLiveAuthority())
    {
        std::string e2;
        if (liveAuth->ResolveBlockSnapshot(hash, out, &e2) == BlockIndexHotStatus::OK)
            return COLD_HOT_SEAM_OK;
    }
    if (err) *err = "authoritative ancestry: block not resolvable by value";
    return COLD_HOT_SEAM_NOT_FOUND;
}

static bool CollectCandidatesByValue(const ColdHotSeamNavigator* nav,
    const uint256& hashStart, int64_t nSelectionIntervalStart,
    vector<StakeModifierCandidateValue>& vOut, int& nHeightFirstCandidate,
    std::string& err)
{
    uint256 curHash = hashStart;
    for (;;)
    {
        BlockIndexSnapshot snap;
        const ColdHotSeamResult r = ResolveAuthoritySnapshot(nav, curHash, &snap, &err);
        if (r != COLD_HOT_SEAM_OK)
        {
            err = "authoritative candidate collection: by-value ancestry not resolvable";
            return false; // FAIL CLOSED (authority failure OR genuine absence)
        }
        if ((int64_t)snap.nTime < nSelectionIntervalStart)
        {
            // First block BELOW the chain-time window (legacy post-loop pindex).
            nHeightFirstCandidate = snap.height + 1;
            return true;
        }
        StakeModifierCandidateValue c;
        c.nTime = (int64_t)snap.nTime;
        c.hash = snap.hash;
        c.hashProof = snap.hashProof;
        c.nFlags = snap.nFlags;
        c.nHeight = snap.height;
        c.fHaveRecord = true; // authority-sourced: the record exists by construction
        vOut.push_back(c);
        if (!snap.hasParent || snap.hashPrev == 0)
        {
            // Authoritative end of chain (never inferred from a resident boundary).
            nHeightFirstCandidate = 0;
            return true;
        }
        curHash = snap.hashPrev;
    }
}

// NOTE (Stage G final): CollectCandidatesHybrid (PHASE-1 resident pprev walk +
// PHASE-2 by-value continuation) has been PHYSICALLY DELETED. The authoritative
// candidate collection now runs the entire window through CollectCandidatesByValue
// above, whose ancestry source is explicit and singleton-free: the cold
// generation, the navigator's injected by-value hot oracle
// (ColdHotSeamNavigator::HasByValueHotResolver), or - only on the legacy pointer
// hot side - the authoritative live snapshot. No resident pprev traversal
// remains in the authoritative candidate path.

// F1(a): last generated stake modifier for `pindexPrev`.
//
// AUTHORITATIVE BY-VALUE CONTRACT (kernel PHASE-1 elimination). The historical
// resident pprev walk is GONE. The walk is hash-driven and branch-local: at each
// step the block's consensus metadata is read BY VALUE from the bounded hot
// window (accepted / unflushed hot records; reproduces legacy semantics exactly)
// or, below that window, from the authoritative V2 by-value snapshot. Correctness
// does not depend on residency depth, and an unknown identity fails closed.
//
// Returns: 1 = resolved, 0 = no generated modifier in the ancestry (legacy's
// "no generation at genesis block"), -1 = authority failure (fail closed).
static int ResolveLastStakeModifierByValue(const ColdHotSeamNavigator* nav,
    const CBlockIndex* pindexPrev, uint64_t& nStakeModifier, int64_t& nModifierTime,
    std::string& err)
{
    if (!pindexPrev)
        return 0; // legacy: "GetLastStakeModifier: null pindex"
    // F1(a) — KERNEL PHASE-1 ELIMINATION (disk-native consensus cutover).
    // NO resident pprev traversal remains. Ancestry is followed through each
    // block's OWN by-value parent identity (hash), so a side branch follows its
    // own history and never the active chain's. Each step resolves consensus
    // metadata BY VALUE:
    //   (1) the BOUNDED hot window (mapBlockIndex) read by hash under cs_main -
    //       the authority for ACCEPTED / unflushed hot records that have not yet
    //       been persisted into the V2 tip (WriteToDisk + AddToBlockIndex side
    //       blocks). This also reproduces the legacy resident semantics exactly.
    //   (2) the authoritative V2 by-value snapshot (live tip -> cold generation)
    //       below the hot window.
    //   (3) cold-only navigator continuation when no live authority is retained.
    // A hash absent from all authorities is an AUTHORITY FAILURE -> fail closed.
    // No CBlockIndex pointer is retained across a loop iteration, and no raw
    // pprev / pskip / pnext edge is dereferenced.
    BlockIndexAuthoritativeLive* liveAuth = GetAuthoritativeLiveAuthority();
    uint256 h = pindexPrev->GetBlockHash();
    for (int guard = 0; guard < 200000000; ++guard)
    {
        // (1) bounded hot window: read-by-hash, no pointer retention.
        bool    haveHot   = false;
        bool    hotGen    = false;
        uint64_t hotMod   = 0;
        int64_t  hotTime  = 0;
        int     hotHeight = -1;
        uint256 hotPrev;
        {
            LOCK(cs_main);
            std::map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(h);
            if (mi != mapBlockIndex.end() && mi->second)
            {
                const CBlockIndex* o = mi->second;
                haveHot   = true;
                hotGen    = o->GeneratedStakeModifier();
                hotMod    = o->nStakeModifier;
                hotTime   = o->GetBlockTime();
                hotHeight = o->nHeight;
                hotPrev   = o->hashPrevStable; // by-value parent identity
                if (hotPrev == uint256(0) && o->pprev)
                    hotPrev = o->pprev->GetBlockHash(); // legacy-populated object
            }
        }
        bool fFallThroughToV2 = false;
        if (haveHot)
        {
            if (hotGen)
            {
                nStakeModifier = hotMod;
                nModifierTime  = hotTime;
                return 1;
            }
            if (hotHeight == 0)
                return 0; // genuine genesis floor: exactly legacy's error case
            if (hotPrev != uint256(0))
            {
                h = hotPrev;
                continue;
            }
            // Hot block with no by-value parent link (retained-window floor):
            // continue from THIS identity through the V2 authority by value.
            fFallThroughToV2 = true;
        }
        // (2) authoritative V2 by-value snapshot (live tip -> cold generation).
        if (liveAuth)
        {
            BlockIndexSnapshot snap;
            std::string e2;
            const BlockIndexHotStatus st = liveAuth->ResolveBlockSnapshot(h, &snap, &e2);
            if (st != BlockIndexHotStatus::OK)
                return -1; // authority failure -> fail closed
            if (snap.nFlags & CBlockIndex::BLOCK_STAKE_MODIFIER)
            {
                // BLOCK-100000 ROOT CAUSE (2026-10-10): the last-modifier
                // generation TIME must be the generation BLOCK's own header time
                // (snap.nTime), exactly as legacy GetLastStakeModifier does
                // (nModifierTime = pindex->GetBlockTime(); kernel.cpp). Returning
                // snap.nStakeModifierTime here reads the derived-memo field, which
                // is 0 for the bootstrap GENESIS, so block 1 resolves last_t=0,
                // concludes "new modifier interval" vs block 1's time, and falsely
                // REGENERATES the modifier (mod=1, BLOCK_STAKE_MODIFIER set) instead
                // of inheriting genesis' mod=0. The whole checksum recurrence then
                // diverges from block 1 (h1 checksum e1459c6b vs canonical
                // bc4b99b6) and block 100000 fails the 0xcf12d0aa checkpoint.
                // snap.nTime is the correct, always-populated block-header time.
                nStakeModifier = snap.nStakeModifier;
                nModifierTime  = (int64_t)snap.nTime;
                return 1;
            }
            if (snap.height == 0 || snap.hashPrev == 0)
                return 0; // genuine genesis floor
            h = snap.hashPrev;
            continue;
        }
        (void)fFallThroughToV2;
        // (3) no live authority retained: cold-only navigator continuation.
        uint64_t nMod = 0;
        int64_t  nTime = 0;
        const ColdHotSeamResult r = nav->GetLastStakeModifierR(
            BlockIndexLogicalId(h), &nMod, &nTime, &err);
        if (r == COLD_HOT_SEAM_OK)
        {
            nStakeModifier = nMod;
            nModifierTime  = nTime;
            return 1;
        }
        if (r == COLD_HOT_SEAM_NOT_FOUND)
            return 0; // authoritative answer: the ancestry holds no generated modifier
        return -1;    // AUTHORITY_FAILURE (or any non-OK) -> fail closed
    }
    return -1; // guard exhausted -> fail closed
}

// Stake Modifier (hash modifier of proof-of-stake):
// The purpose of stake modifier is to prevent a txout (coin) owner from
// computing future proof-of-stake generated by this txout at the time
// of transaction confirmation. To meet kernel protocol, the txout
// must hash with a future stake modifier to generate the proof.
// Stake modifier consists of bits each of which is contributed from a
// selected block of a given block group in the past.
// The selection of a block is based on a hash of the block's proof-hash and
// the previous stake modifier.
// Stake modifier is recomputed at a fixed time interval instead of every
// block. This is to make it difficult for an attacker to gain control of
// additional bits in the stake modifier, even after generating a chain of
// blocks.
// Fail-closed output discipline for ComputeNextStakeModifier: on ANY failure the
// outputs are reset, so a caller that ignores the false result can never observe
// a partially computed modifier. This matters because the last-modifier value is
// written into nStakeModifier before the candidate selection can still fail.
static bool FailComputeNextStakeModifier(uint64_t& nStakeModifier, bool& fGeneratedStakeModifier)
{
    nStakeModifier = 0;
    fGeneratedStakeModifier = false;
    return false;
}

bool ComputeNextStakeModifier(const CBlockIndex* pindexPrev, uint64_t& nStakeModifier, bool& fGeneratedStakeModifier)
{
    nStakeModifier = 0;
    fGeneratedStakeModifier = false;
    if (!pindexPrev)
    {
        fGeneratedStakeModifier = true;
        return true;  // genesis block's modifier is 0
    }
    // First find current stake modifier and its generation block time
    // if it's not old enough, return the same stake modifier
    int64_t nModifierTime = 0;
    // F1 AUTHORITATIVE BOUNDARY: the authoritative by-value provider must be
    // consulted whenever a production navigator is retained. The
    // -stakemodifieropt memo is an in-memory (chain-own, not serialized)
    // nStakeModifierTime shortcut whose coherence with the frozen by-value
    // authority is NOT provable, so letting it short-circuit here would return a
    // memoised modifier without ever consulting the authority (a silent
    // divergence from consensus truth). The memo is therefore used ONLY in
    // non-authoritative mode; with no navigator retained the legacy semantics
    // below are unchanged.
    const ColdHotSeamNavigator* navMemo = GetBlockIndexStakingNavigator();
    bool fOpt = (navMemo == NULL) && GetBoolArg("-stakemodifieropt", false);
    if (fOpt && pindexPrev && pindexPrev->nStakeModifierTime != 0)
    {
        nStakeModifier = pindexPrev->nStakeModifier;
        nModifierTime  = pindexPrev->nStakeModifierTime;
    }
    else
    {
        // F1(a): whenever a production navigator is retained, resolve the last
        // generated modifier BY VALUE. The walk is bounded by CHAIN TIME
        // (<= nModifierInterval) and never requires the generated ancestor to be
        // resident. Authority failure FAILS CLOSED - it must never silently fall
        // back to the resident pprev walk, which is exactly the retained-floor
        // failure this cutover removes. With no navigator retained the legacy
        // resident walk below is unchanged, so fully resident worlds are
        // bit-identical.
        const ColdHotSeamNavigator* navLast = navMemo;
        if (navLast)
        {
            std::string lerr;
            const int lr = ResolveLastStakeModifierByValue(navLast, pindexPrev,
                                                           nStakeModifier, nModifierTime, lerr);
            if (lr == 0)
            {
                FailComputeNextStakeModifier(nStakeModifier, fGeneratedStakeModifier);
                return error("GetLastStakeModifier: no generation at genesis block (authoritative by-value)");
            }
            if (lr < 0)
            {
                FailComputeNextStakeModifier(nStakeModifier, fGeneratedStakeModifier);
                return error("ComputeNextStakeModifier: unable to get last modifier (authoritative authority failure: %s)", lerr.c_str());
            }
        }
        else if (!GetLastStakeModifier(pindexPrev, nStakeModifier, nModifierTime))
        {
            FailComputeNextStakeModifier(nStakeModifier, fGeneratedStakeModifier);
            return error("ComputeNextStakeModifier: unable to get last modifier");
        }
    }
    if (fDebug)
    {
        printf("ComputeNextStakeModifier: prev modifier=0x%016" PRIx64" time=%s\n", nStakeModifier, DateTimeStrFormat(nModifierTime).c_str());
    }
    if (nModifierTime / nModifierInterval >= pindexPrev->GetBlockTime() / nModifierInterval)
    {
        if (AcceptBlockRejectTraceEnabled() &&
            (pindexPrev->nHeight <= 3 || pindexPrev->nHeight >= 99995))
            printf("SMOD h=%d last_mod=0x%016llx last_t=%lld cur_t=%lld SAME_INTERVAL inherits mod=0x%016llx gen=0\n",
                   pindexPrev->nHeight+1, (unsigned long long)nStakeModifier, (long long)nModifierTime,
                   (long long)pindexPrev->GetBlockTime(), (unsigned long long)nStakeModifier);
        return true;
    }
    if (AcceptBlockRejectTraceEnabled() &&
        (pindexPrev->nHeight <= 3 || pindexPrev->nHeight >= 99995))
        printf("SMOD h=%d last_mod=0x%016llx last_t=%lld cur_t=%lld NEW_INTERVAL -> candidates\n",
               pindexPrev->nHeight+1, (unsigned long long)nStakeModifier, (long long)nModifierTime,
               (long long)pindexPrev->GetBlockTime());

    // Candidate block collection. Bounded by CHAIN TIME (nSelectionIntervalStart)
    // in BOTH providers - never by a fixed height depth: the window can span
    // ~21135 blocks at 1 s spacing and ~1409 at 15 s, and there is no consensus
    // maximum in blocks.
    int64_t nSelectionInterval = GetStakeModifierSelectionInterval();
    int64_t nSelectionIntervalStart = (pindexPrev->GetBlockTime() / nModifierInterval) * nModifierInterval - nSelectionInterval;
    int nHeightFirstCandidate = 0;
    vector<StakeModifierCandidateValue> vCandidates;
    vCandidates.reserve(64 * nModifierInterval / nTargetSpacing);
    const ColdHotSeamNavigator* navCandidates = GetBlockIndexStakingNavigator();
    if (navCandidates)
    {
        // F1(b) AUTHORITATIVE PROVIDER: by-value parent walk. No mapBlockIndex,
        // no CBlockIndex materialization, no residency growth, no fixed depth.
        // A truncated/partial window is NEVER used: any authority failure aborts
        // the whole computation (fail closed), so a short candidate set can never
        // silently yield a different modifier.
        std::string cErr;
        // Stage G final: collect the ENTIRE candidate window BY VALUE from
        // pindexPrev's hash - no resident pprev traversal. Each ancestor resolves
        // through the cold generation, the navigator's authoritative by-value hot
        // oracle, or (legacy pointer hot side) the authoritative live snapshot -
        // never a hidden pointer chain (see CollectCandidatesByValue). Fields and
        // ordering are unchanged.
        if (!CollectCandidatesByValue(navCandidates, pindexPrev->GetBlockHash(),
                                      nSelectionIntervalStart, vCandidates,
                                      nHeightFirstCandidate, cErr))
        {
            FailComputeNextStakeModifier(nStakeModifier, fGeneratedStakeModifier);
            return error("ComputeNextStakeModifier: authoritative candidate collection failed: %s", cErr.c_str());
        }
        // Reproduce the legacy reversal + sort (identical (time,hash)-ascending
        // candidate sequence; the by-value walker appends in descending order).
        reverse(vCandidates.begin(), vCandidates.end());
        sort(vCandidates.begin(), vCandidates.end(), StakeModifierCandidateLess());
    }
    else
    {
        // LEGACY RESIDENT PROVIDER: byte-identical collection, reversal and sort.
        vector<pair<int64_t, uint256> > vSortedByTimestamp;
        vSortedByTimestamp.reserve(64 * nModifierInterval / nTargetSpacing);
        const CBlockIndex* pindex = pindexPrev;
        while (pindex && pindex->GetBlockTime() >= nSelectionIntervalStart)
        {
            vSortedByTimestamp.push_back(make_pair(pindex->GetBlockTime(), pindex->GetBlockHash()));
            pindex = pindex->pprev;
        }
        nHeightFirstCandidate = pindex ? (pindex->nHeight + 1) : 0;
        reverse(vSortedByTimestamp.begin(), vSortedByTimestamp.end());
        sort(vSortedByTimestamp.begin(), vSortedByTimestamp.end());
        CollectCandidatesResident(vSortedByTimestamp, vCandidates);
    }

    // Select 64 blocks from candidate blocks to generate stake modifier.
    // Precompute each candidate's selection hash ONCE: it is round-invariant
    // (inputs = hashProof || nStakeModifier, where nStakeModifier is the previous
    // modifier, constant across all 64 rounds). The precompute applies the PoS
    // >>32 adjustment; the 64 rounds then reuse the exact same value.
    vector<uint256> vSelHash;
    vSelHash.reserve(vCandidates.size());
    {
        int64_t nPreUs = GetTimeMicros();
        for (const StakeModifierCandidateValue& candidate : vCandidates)
            vSelHash.push_back(StakeModifierSelectionHash(candidate, nStakeModifier));
        if (fOpt)
        {
            int64_t nPreUs2 = GetTimeMicros() - nPreUs;
            if (nPreUs2 > 250000)
                printf("ComputeNextStakeModifier: precompute_selhash_us=%" PRId64" candidates=%zu\n", nPreUs2, vSelHash.size());
        }
    }

    uint64_t nStakeModifierNew = 0;
    int64_t nSelectionIntervalStop = nSelectionIntervalStart;
    set<uint256> setSelectedBlocks;
    size_t nSelectedIndex = 0;
    for (int nRound=0; nRound<min(64, (int)vCandidates.size()); nRound++)
    {
        // add an interval section to the current selection round
        nSelectionIntervalStop += GetStakeModifierSelectionIntervalSection(nRound);
        // select a block from the candidates of current round
        if (!SelectBlockFromCandidateValues(vCandidates, vSelHash, setSelectedBlocks, nSelectionIntervalStop, &nSelectedIndex))
        {
            FailComputeNextStakeModifier(nStakeModifier, fGeneratedStakeModifier);
            return error("ComputeNextStakeModifier: unable to select block at round %d", nRound);
        }
        // write the entropy bit of the selected block
        nStakeModifierNew |= (((uint64_t)vCandidates[nSelectedIndex].GetStakeEntropyBit()) << nRound);
        // add the selected block from candidates to selected list
        setSelectedBlocks.insert(vCandidates[nSelectedIndex].hash);
        if (fDebug && GetBoolArg("-printstakemodifier"))
            printf("ComputeNextStakeModifier: selected round %d stop=%s height=%d bit=%d\n", nRound, DateTimeStrFormat(nSelectionIntervalStop).c_str(), vCandidates[nSelectedIndex].nHeight, vCandidates[nSelectedIndex].GetStakeEntropyBit());
    }

    // Print selection map for visualization of the selected blocks
    if (fDebug && GetBoolArg("-printstakemodifier"))
    {
        string strSelectionMap = "";
        // '-' indicates proof-of-work blocks not selected. The candidate set IS
        // the window, so the map is rendered from the value records.
        strSelectionMap.insert(0, (size_t)(pindexPrev->nHeight - nHeightFirstCandidate + 1), '-');
        const size_t nMap = strSelectionMap.size();
        for (size_t i = 0; i < vCandidates.size(); i++)
        {
            const size_t nPos = (size_t)(vCandidates[i].nHeight - nHeightFirstCandidate);
            // '=' indicates proof-of-stake blocks not selected
            if (nPos < nMap && vCandidates[i].IsProofOfStake())
                strSelectionMap.replace(nPos, 1, "=");
        }
        for (size_t i = 0; i < vCandidates.size(); i++)
        {
            if (setSelectedBlocks.count(vCandidates[i].hash) == 0)
                continue;
            const size_t nPos = (size_t)(vCandidates[i].nHeight - nHeightFirstCandidate);
            // 'S' indicates selected proof-of-stake blocks
            // 'W' indicates selected proof-of-work blocks
            if (nPos < nMap)
                strSelectionMap.replace(nPos, 1, vCandidates[i].IsProofOfStake()? "S" : "W");
        }
        printf("ComputeNextStakeModifier: selection height [%d, %d] map %s\n", nHeightFirstCandidate, pindexPrev->nHeight, strSelectionMap.c_str());
    }
    if (fDebug)
    {
        printf("ComputeNextStakeModifier: new modifier=0x%016" PRIx64" time=%s\n", nStakeModifierNew, DateTimeStrFormat(pindexPrev->GetBlockTime()).c_str());
    }

    nStakeModifier = nStakeModifierNew;
    fGeneratedStakeModifier = true;
    return true;
}

// ---------------------------------------------------------------------------
// A.9a.3c: production staking-navigation integration for the two-argument path.
//
// When a production ColdHotSeamNavigator has been retained (V2 shadow READY),
// this path resolves the historical source and walks the forward active chain
// by-value, so an arbitrarily old source requires no resident CBlockIndex /
// mapBlockIndex / pnext residency. Authority failures are typed and fail closed
// (they never silently fall back to legacy historical residency). HybridSPV
// cache semantics are preserved exactly at this adapter boundary.
//
// Returns non-null when this adapter actually delegated to the navigator; the
// caller then relies on *result / error and does not fall through to legacy.
// ---------------------------------------------------------------------------
static bool GetKernelStakeModifierNavigated2(uint256 hashBlockFrom,
    uint64_t& nStakeModifier, int& nStakeModifierHeight, int64_t& nStakeModifierTime,
    bool fPrintProofOfStake, ColdHotSeamResult* resultOut)
{
    if (resultOut)
        *resultOut = COLD_HOT_SEAM_AUTHORITY_FAILURE;
    const ColdHotSeamNavigator* nav = GetBlockIndexStakingNavigator();
    if (!nav)
        return false; // no production navigator retained -> callers use legacy

    AssertLockHeld(cs_main);
    const BlockIndexLogicalId logical(hashBlockFrom);
    std::string err;

    // HybridSPV early cache lookup is a CALLER-BOUNDARY policy, reproduced
    // exactly as legacy (kernel.cpp GetKernelStakeModifier 2-arg). We need the
    // source height for the target-height cache key; resolve it first.
    if (fHybridSPV)
    {
        ColdHotSeamSnapshot src;
        if (nav->ResolveLogicalR(logical, &src, &err) != COLD_HOT_SEAM_OK)
        {
            if (resultOut)
                *resultOut = COLD_HOT_SEAM_AUTHORITY_FAILURE;
            return true; // fail closed; caller returns false
        }
        const int nTargetHeight = src.snapshot.height + (int)(GetStakeModifierSelectionInterval() / 180); // ~3 min blocks
        if (GetCachedStakeModifier(nTargetHeight, nStakeModifier))
        {
            // A.9a.3d: reproduce legacy cache-hit semantics EXACTLY. Legacy
            // (kernel.cpp GetKernelStakeModifier 2-arg) sets nStakeModifierHeight
            // and nStakeModifierTime from the SOURCE block before the cache
            // lookup and does NOT advance them on a hit. The navigated adapter
            // must return the cached modifier plus the same height/time.
            nStakeModifierHeight = src.snapshot.height;
            nStakeModifierTime = (int64_t)src.snapshot.nTime;
            if (resultOut)
                *resultOut = COLD_HOT_SEAM_OK;
            return true; // early cache hit, same semantics
        }
    }

    int finalWalkHeight = -1;
    const ColdHotSeamResult r = nav->GetKernelStakeModifierR(
        logical, &nStakeModifier, &nStakeModifierHeight, &nStakeModifierTime,
        fPrintProofOfStake, &err, &finalWalkHeight);
    if (resultOut)
        *resultOut = r;
    if (r == COLD_HOT_SEAM_OK && fHybridSPV && finalWalkHeight >= 0)
        CacheStakeModifier(finalWalkHeight, nStakeModifier);
    return true; // production navigator path owns the outcome (OK or typed fail)
}

// The stake modifier used to hash for a stake kernel is chosen as the stake
// modifier about a selection interval later than the coin generating the kernel
bool GetKernelStakeModifier(uint256 hashBlockFrom, uint64_t& nStakeModifier, int& nStakeModifierHeight, int64_t& nStakeModifierTime, bool fPrintProofOfStake)
{
    nStakeModifier = 0;
    ColdHotSeamResult navResult;
    if (GetKernelStakeModifierNavigated2(hashBlockFrom, nStakeModifier,
        nStakeModifierHeight, nStakeModifierTime, fPrintProofOfStake, &navResult))
    {
        // Navigated outcome is authoritative: OK succeeds, authority failure
        // and genuine NOT_FOUND both fail (never a legacy fallback).
        return navResult == COLD_HOT_SEAM_OK;
    }
    std::map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(hashBlockFrom);
    if (mi == mapBlockIndex.end())
        return error("GetKernelStakeModifier() : block not indexed");
    const CBlockIndex* pindexFrom = mi->second;
    nStakeModifierHeight = pindexFrom->nHeight;
    nStakeModifierTime = pindexFrom->GetBlockTime();
    int64_t nStakeModifierSelectionInterval = GetStakeModifierSelectionInterval();
    const CBlockIndex* pindex = pindexFrom;

    if (fHybridSPV)
    {
        int nTargetHeight = pindexFrom->nHeight + (nStakeModifierSelectionInterval / 180);  // ~3 min blocks
        if (GetCachedStakeModifier(nTargetHeight, nStakeModifier))
        {
            if (fDebug)
                printf("GetKernelStakeModifier() : using cached modifier for height %d\n", nTargetHeight);
            return true;
        }
    }

    // loop to find the stake modifier later by a selection interval
    while (nStakeModifierTime < pindexFrom->GetBlockTime() + nStakeModifierSelectionInterval)
    {
        if (!pindex->pnext)
        {   // reached best block; may happen if node is behind on block chain
            // If best block is already past the selection interval, use its modifier
            // (it may have inherited the modifier from an earlier block).
            // This handles chains that stall after a burst: the tip has no generated
            // modifier, but its inherited modifier is still valid for verification.
            if (pindex->GetBlockTime() >= pindexFrom->GetBlockTime() + nStakeModifierSelectionInterval)
            {
                nStakeModifier = pindex->nStakeModifier;
                nStakeModifierHeight = pindex->nHeight;
                nStakeModifierTime = pindex->GetBlockTime();
                return true;
            }
            if (fPrintProofOfStake || (pindex->GetBlockTime() + nStakeMinAge - nStakeModifierSelectionInterval > GetAdjustedTime()))
            {
                return error("GetKernelStakeModifier() : reached best block %s at height %d from block %s",
                    pindex->GetBlockHash().ToString().c_str(), pindex->nHeight, hashBlockFrom.ToString().c_str());
            } else {
                return false;
            };
        };
        pindex = pindex->pnext;
        if (pindex->GeneratedStakeModifier())
        {
            nStakeModifierHeight = pindex->nHeight;
            nStakeModifierTime = pindex->GetBlockTime();
        }
    };

    nStakeModifier = pindex->nStakeModifier;

    if (fHybridSPV)
    {
        CacheStakeModifier(pindex->nHeight, nStakeModifier);
    }

    return true;
};

bool GetKernelStakeModifier(uint256 hashBlockFrom,
    uint64_t& nStakeModifier, int& nStakeModifierHeight,
    int64_t& nStakeModifierTime, bool fPrintProofOfStake);

// ---------------------------------------------------------------------------
// A.9a.3c: three-argument production path. When a production navigator is
// retained, the branch-tip->source ancestry walk and the modifier selection are
// done by-value (O(1) transient memory, no arbitrary CBlockIndex residency),
// with typed fail-closed authority. APX_STREAM exactness is preserved by the
// O(1)-memory streaming derivation in cold_hot_seam.cpp.
// ---------------------------------------------------------------------------
static bool GetKernelStakeModifierNavigated3(uint256 hashBlockFrom,
    const CBlockIndex* pindexPrev, uint64_t& nStakeModifier, int& nStakeModifierHeight,
    int64_t& nStakeModifierTime, bool fPrintProofOfStake, ColdHotSeamResult* resultOut)
{
    if (resultOut)
        *resultOut = COLD_HOT_SEAM_AUTHORITY_FAILURE;
    const ColdHotSeamNavigator* nav = GetBlockIndexStakingNavigator();
    if (!nav || !pindexPrev)
        return false; // no production navigator retained -> legacy
    AssertLockHeld(cs_main);
    const BlockIndexLogicalId source(hashBlockFrom);
    const BlockIndexLogicalId tip(pindexPrev->GetBlockHash());
    std::string err;
    const ColdHotSeamResult r = nav->GetKernelStakeModifierR(
        source, tip, &nStakeModifier, &nStakeModifierHeight, &nStakeModifierTime,
        fPrintProofOfStake, &err);
    if (resultOut)
        *resultOut = r;
    return true; // navigated outcome owns the result (OK or typed fail)
}

bool GetKernelStakeModifier(uint256 hashBlockFrom, const CBlockIndex* pindexPrev,
    uint64_t& nStakeModifier, int& nStakeModifierHeight,
    int64_t& nStakeModifierTime, bool fPrintProofOfStake)
{
    ColdHotSeamResult navResult;
    if (GetKernelStakeModifierNavigated3(hashBlockFrom, pindexPrev, nStakeModifier,
        nStakeModifierHeight, nStakeModifierTime, fPrintProofOfStake, &navResult))
        return navResult == COLD_HOT_SEAM_OK;
    std::map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(hashBlockFrom);
    if (mi == mapBlockIndex.end())
        return error("GetKernelStakeModifier() : block not indexed");
    const CBlockIndex* pindexFrom = mi->second;
    std::vector<const CBlockIndex*> path;
    for (const CBlockIndex* pindex = pindexPrev; pindex; pindex = pindex->pprev)
    {
        path.push_back(pindex);
        if (pindex == pindexFrom)
            break;
    }
    if (path.empty() || path.back() != pindexFrom)
        return error("GetKernelStakeModifier() : stake source is not an ancestor of candidate branch");
    std::reverse(path.begin(), path.end());

    nStakeModifier = pindexFrom->nStakeModifier;
    nStakeModifierHeight = pindexFrom->nHeight;
    nStakeModifierTime = pindexFrom->GetBlockTime();
    const int64_t nTargetTime = pindexFrom->GetBlockTime() +
        GetStakeModifierSelectionInterval();
    for (size_t i = 1; i < path.size(); ++i)
    {
        const CBlockIndex* pindex = path[i];
        if (pindex->GeneratedStakeModifier())
        {
            nStakeModifierHeight = pindex->nHeight;
            nStakeModifierTime = pindex->GetBlockTime();
        }
        if (nStakeModifierTime >= nTargetTime)
        {
            nStakeModifier = pindex->nStakeModifier;
            return true;
        }
    }
    const CBlockIndex* pindexTip = path.back();
    if (pindexTip->GetBlockTime() >= nTargetTime)
    {
        nStakeModifier = pindexTip->nStakeModifier;
        nStakeModifierHeight = pindexTip->nHeight;
        nStakeModifierTime = pindexTip->GetBlockTime();
        return true;
    }
    if (fPrintProofOfStake)
        return error("GetKernelStakeModifier() : candidate branch ends before selection interval");
    return false;
}

// Innova kernel protocol
// coinstake must meet hash target according to the protocol:
// kernel (input 0) must meet the formula
//     hash(nStakeModifier + txPrev.block.nTime + txPrev.offset + txPrev.nTime + txPrev.vout.n + nTime) < bnTarget * nCoinDayWeight
// this ensures that the chance of getting a coinstake is proportional to the
// amount of coin age one owns.
// The reason this hash is chosen is the following:
//   nStakeModifier: scrambles computation to make it very difficult to precompute
//                  future proof-of-stake at the time of the coin's confirmation
//   txPrev.block.nTime: prevent nodes from guessing a good timestamp to
//                       generate transaction for future advantage
//   txPrev.offset: offset of txPrev inside block, to reduce the chance of
//                  nodes generating coinstake at the same time
//   txPrev.nTime: reduce the chance of nodes generating coinstake at the same
//                 time
//   txPrev.vout.n: output number of txPrev, to reduce the chance of nodes
//                  generating coinstake at the same time
//   block/tx hash should not be used here as they can be generated in vast
//   quantities so as to generate blocks faster, degrading the system back into
//   a proof-of-work situation.
//
// A.9a.3d: debug-only source-height lookup that never requires an arbitrary
// cold CBlockIndex to be resident and never inserts a NULL entry through
// mapBlockIndex::operator[]. Prefers the retained navigator by-value (works
// for cold-only sources absent from the resident map); falls back to a safe
// non-inserting find().
static int GetStakeSourceHeightForDebug(const uint256& hashBlockFrom)
{
    AssertLockHeld(cs_main);
    const ColdHotSeamNavigator* nav = GetBlockIndexStakingNavigator();
    if (nav)
    {
        std::string err;
        ColdHotSeamSnapshot snap;
        if (nav->ResolveLogicalR(BlockIndexLogicalId(hashBlockFrom), &snap, &err) == COLD_HOT_SEAM_OK)
            return snap.snapshot.height;
    }
    std::map<uint256, CBlockIndex*>::const_iterator mi = mapBlockIndex.find(hashBlockFrom);
    if (mi != mapBlockIndex.end())
        return mi->second->nHeight;
    return 0;
}

bool CheckStakeKernelHash(const CBlockIndex* pindexPrev, unsigned int nBits, const CBlock& blockFrom, unsigned int nTxPrevOffset, const CTransaction& txPrev, const COutPoint& prevout, unsigned int nTimeTx, uint256& hashProofOfStake, uint256& targetProofOfStake, bool fPrintProofOfStake)
{
    if (nTimeTx < txPrev.nTime)  // Transaction timestamp violation
        return error("CheckStakeKernelHash() : nTime violation");

    unsigned int nTimeBlockFrom = blockFrom.GetBlockTime();
    if (nTimeBlockFrom + nStakeMinAge > nTimeTx) // Min age requirement
        return error("CheckStakeKernelHash() : min age violation");

    if (nBestHeight >= FORK_HEIGHT_TIGHTER_DRIFT)
    {
        unsigned int nMaxAge = 90 * 24 * 60 * 60; // 90 days
        if (nTimeTx > nTimeBlockFrom + nMaxAge)
            return error("CheckStakeKernelHash() : max age violation (coin too old: %u > %u + %u)",
                         nTimeTx, nTimeBlockFrom, nMaxAge);
    }

    CBigNum bnTargetPerCoinDay;
    bnTargetPerCoinDay.SetCompact(nBits);
    if (bnTargetPerCoinDay <= 0)
        return error("CheckStakeKernelHash() : invalid nBits");
    int64_t nValueIn = txPrev.vout[prevout.n].nValue;

    uint256 hashBlockFrom = blockFrom.GetHash();

    int64_t nCoinWeight = GetWeight((int64_t)txPrev.nTime, (int64_t)nTimeTx);
    CBigNum bnTargetProduct = CBigNum(nValueIn) * nCoinWeight * bnTargetPerCoinDay;
    CBigNum bnCoinDayWeight = CBigNum(nValueIn) * nCoinWeight / COIN / (24 * 60 * 60);
    targetProofOfStake = (bnCoinDayWeight * bnTargetPerCoinDay).getuint256();

    // Calculate hash
    CDataStream ss(SER_GETHASH, 0);
    uint64_t nStakeModifier = 0;
    int nStakeModifierHeight = 0;
    int64_t nStakeModifierTime = 0;

    if (!GetKernelStakeModifier(hashBlockFrom, pindexPrev, nStakeModifier, nStakeModifierHeight, nStakeModifierTime, fPrintProofOfStake))
        return false;

    ss << nStakeModifier;

    ss << nTimeBlockFrom << nTxPrevOffset << txPrev.nTime << prevout.n << nTimeTx;
    hashProofOfStake = Hash(ss.begin(), ss.end());

    if (fPrintProofOfStake)
    {
        int nHeight = 0;
        nHeight = GetStakeSourceHeightForDebug(hashBlockFrom);
        printf("CheckStakeKernelHash() : using modifier 0x%016" PRIx64" at height=%d timestamp=%s for block from height=%d timestamp=%s\n",
            nStakeModifier, nStakeModifierHeight,
            DateTimeStrFormat(nStakeModifierTime).c_str(),
            nHeight,
            DateTimeStrFormat(blockFrom.GetBlockTime()).c_str());
        printf("CheckStakeKernelHash() : check modifier=0x%016" PRIx64" nTimeBlockFrom=%u nTxPrevOffset=%u nTimeTxPrev=%u nPrevout=%u nTimeTx=%u hashProof=%s\n",
            nStakeModifier,
            nTimeBlockFrom, nTxPrevOffset, txPrev.nTime, prevout.n, nTimeTx,
            hashProofOfStake.ToString().c_str());

        CBigNum nTry = CBigNum(hashProofOfStake);
        CBigNum nTar = bnCoinDayWeight * bnTargetPerCoinDay;
        printf("try    %s\n                    target %s\n", nTry.ToString().c_str(), nTar.ToString().c_str());
    };

    // Now check if proof-of-stake hash meets target protocol
    // Use cross-multiplication to avoid integer division precision loss for small coins:
    //   hash > coinDayWeight * target  where  coinDayWeight = value * weight / COIN / 86400
    // is equivalent to:
    //   hash * COIN * 86400 > value * weight * target
    if (CBigNum(hashProofOfStake) * COIN * (24 * 60 * 60) > bnTargetProduct)
        return false;
    if (fDebug && !fPrintProofOfStake)
    {
        int nHeight = 0;
        nHeight = GetStakeSourceHeightForDebug(hashBlockFrom);
        printf("CheckStakeKernelHash() : using modifier 0x%016" PRIx64" at height=%d timestamp=%s for block from height=%d timestamp=%s\n",
            nStakeModifier, nStakeModifierHeight,
            DateTimeStrFormat(nStakeModifierTime).c_str(),
            nHeight,
            DateTimeStrFormat(blockFrom.GetBlockTime()).c_str());
        printf("CheckStakeKernelHash() : pass modifier=0x%016" PRIx64" nTimeBlockFrom=%u nTxPrevOffset=%u nTimeTxPrev=%u nPrevout=%u nTimeTx=%u hashProof=%s\n",
            nStakeModifier,
            nTimeBlockFrom, nTxPrevOffset, txPrev.nTime, prevout.n, nTimeTx,
            hashProofOfStake.ToString().c_str());
    };
    return true;
}

static bool IsBlockInCandidateAncestry(const CBlockIndex* pindexBlock,
    const CBlockIndex* pindexPrev)
{
    if (!pindexBlock)
        return false;
    for (const CBlockIndex* pindex = pindexPrev; pindex; pindex = pindex->pprev)
        if (pindex == pindexBlock)
            return true;
    return false;
}

// A.9a.3c: navigator-backed candidate-ancestry check. Given a candidate branch
// tip and a source block hash, resolves whether the source block is an ancestor
// of the candidate branch entirely by-value (stable logical identity + parent
// navigation), so an arbitrarily old source requires NO resident CBlockIndex /
// continuous pprev topology. Returns:
//   1  -> source confirmed an ancestor of the branch (by-value).
//   0  -> source is NOT an ancestor.
//  -1  -> navigator unavailable or an authority failure occurred (caller must
//         fall back to the legacy pointer check; never mis-validate).
static int IsBlockInCandidateAncestryNavigated(const uint256& sourceHash,
    const CBlockIndex* pindexPrev)
{
    const ColdHotSeamNavigator* nav = GetBlockIndexStakingNavigator();
    if (!nav || !pindexPrev || sourceHash == uint256(0))
        return -1;
    AssertLockHeld(cs_main);

    std::string err;
    const BlockIndexLogicalId sourceLogical(sourceHash);
    ColdHotSeamSnapshot source;
    const ColdHotSeamResult srcR = nav->ResolveLogicalR(sourceLogical, &source, &err);
    if (srcR == COLD_HOT_SEAM_AUTHORITY_FAILURE)
        return -1; // stale/corrupt/divergent authority -> FAIL CLOSED
    if (srcR != COLD_HOT_SEAM_OK)
    {
        // Genuine NOT_FOUND in both domains => not an ancestor (legacy: a
        // block absent from the index is not in the candidate ancestry).
        return 0;
    }

    // Walk the candidate branch tip backward by-value until we reach the source
    // or drop to a height at/below the source (a valid chain only extends
    // upward, so once we pass the source height without a match the source is
    // not on this branch).
    // Walk is O(1): membership = (tip's ancestor at source.height == source).
    // CORRECTION #2 (2026-10-10): previously this resolved the tip then walked DOWN
    // ONE PARENT SNAPSHOT PER LOOKUP (GetParentR) until the source height -- an
    // O(distance-from-tip) cost repeated on EVERY accepted PoS block. Instrumented
    // counters measured ~4,181 GetParentR ancestry steps per accepted block
    // (stake_source_ancestry_steps 32.6M vs calls 7.9k). GetAncestorR resolves the
    // tip's ancestor at a given height in O(1) for the active chain (active.dat /
    // GetActiveByHeight), giving bit-equivalent membership without the per-ancestor
    // reads. Side branches fall back to the O(depth) walk inside GetAncestorR --
    // identical result, moved to the rare side-chain case only. Consensus
    // accepted/rejected is unchanged; every authority failure fails closed.
    const BlockIndexLogicalId tipLogical(pindexPrev->GetBlockHash());
    ColdHotSeamSnapshot tip;
    {
        std::string errTip;
        if (nav->ResolveLogicalR(tipLogical, &tip, &errTip) != COLD_HOT_SEAM_OK)
            return -1;
        if (!tip.snapshot.found)
            return -1; // FAIL CLOSED on unresolvable tip
    }
    if (source.snapshot.height > tip.snapshot.height)
        return 0; // source deeper than tip -> cannot be an ancestor
    ibdmetrics::Get().stake_source_ancestry_calls.fetch_add(1, std::memory_order_relaxed);
    ColdHotSeamSnapshot ancestor;
    {
        std::string errAnc;
        const ColdHotSeamResult ar = nav->GetAncestorR(
            BlockIndexNavigationRef::Hot(tipLogical), (int)source.snapshot.height,
            &ancestor, &errAnc);
        if (ar != COLD_HOT_SEAM_OK)
            return -1; // authority failure / corrupt -> FAIL CLOSED
    }
    if (!ancestor.snapshot.found)
        return 0;
    return (ancestor.snapshot.hash == sourceHash &&
            ancestor.snapshot.height == source.snapshot.height) ? 1 : 0;
}

StakingAncestorStatus GetStakingAncestorSnapshot(const CBlockIndex* pindexPrev, int targetHeight,
    uint256* hashOut, unsigned int* nTimeOut, unsigned int* nFlagsOut)
{
    if (hashOut) *hashOut = uint256(0);
    if (nTimeOut) *nTimeOut = 0;
    if (nFlagsOut) *nFlagsOut = 0;
    const ColdHotSeamNavigator* nav = GetBlockIndexStakingNavigator();
    if (!nav || !pindexPrev || targetHeight < 0 || targetHeight > pindexPrev->nHeight)
        return STAKING_ANCESTOR_NO_NAVIGATOR; // pre-A.10 fully-materialized: legacy fallback valid

    const BlockIndexLogicalId tipLogical(pindexPrev->GetBlockHash());
    std::string err;
    ColdHotSeamSnapshot res;
    const ColdHotSeamResult r = nav->GetAncestorR(BlockIndexNavigationRef::Hot(tipLogical),
                                                  targetHeight, &res, &err);
    if (r == COLD_HOT_SEAM_AUTHORITY_FAILURE)
        return STAKING_ANCESTOR_AUTHORITY_FAILURE; // NEVER legacy fallback
    if (r == COLD_HOT_SEAM_NOT_FOUND || r == COLD_HOT_SEAM_END_OF_ACTIVE_CHAIN)
        return STAKING_ANCESTOR_NOT_FOUND; // genuine absence; note is not an ancestor
    if (r != COLD_HOT_SEAM_OK)
        return STAKING_ANCESTOR_AUTHORITY_FAILURE; // any other non-OK -> fail closed
    if (!res.snapshot.found || res.snapshot.height != targetHeight)
        return STAKING_ANCESTOR_NOT_FOUND;
    if (hashOut) *hashOut = res.snapshot.hash;
    if (nTimeOut) *nTimeOut = res.snapshot.nTime;
    if (nFlagsOut) *nFlagsOut = res.snapshot.nFlags;
    return STAKING_ANCESTOR_OK;
}

// A.9a.3d: by-value candidate-branch (side-suffix) source-transaction search.
// A resident, forward-walkable chain is never a precondition: branch blocks are
// resolved by stable logical identity + parent navigation (cold records or hot
// tail) and each block is read from disk by its persisted nFile/nBlockPos. This
// removes the arbitrary historical CBlockIndex* / pprev topology requirement.
// Returns false on any authority failure (fail closed; never a pprev fallback)
// or genuine absence of the previous transaction on the branch.
static bool SearchCandidateSuffixNavigated(const ColdHotSeamNavigator* nav,
    const CBlockIndex* pindexPrev, const COutPoint& prevout,
    CTransaction& txPrev, CTxIndex& txindex, CBlock& blockFrom,
    const std::map<uint256, CBlock>* candidateBlocksForTesting)
{
    if (!nav || !pindexPrev)
        return false;
    AssertLockHeld(cs_main);
    ColdHotSeamSnapshot cur;
    {
        std::string err;
        const ColdHotSeamResult r = nav->ResolveLogicalR(
            BlockIndexLogicalId(pindexPrev->GetBlockHash()), &cur, &err);
        if (r != COLD_HOT_SEAM_OK)
            return false; // branch tip not resolvable in either domain -> not an ancestor
    }
    for (;;)
    {
        if (cur.snapshot.fInMainChain)
            break; // reached the connected chain; candidate suffix ends
        CBlock block;
        bool fReadBlock = false;
        if (candidateBlocksForTesting)
        {
            std::map<uint256, CBlock>::const_iterator miBlock =
                candidateBlocksForTesting->find(cur.snapshot.hash);
            if (miBlock != candidateBlocksForTesting->end())
            {
                block = miBlock->second;
                fReadBlock = true;
            }
        }
        else
        {
            fReadBlock = block.ReadFromDisk(cur.snapshot.nFile, cur.snapshot.nBlockPos, true);
        }
        if (!fReadBlock)
            return false;
        unsigned int nTxPos = cur.snapshot.nBlockPos +
            ::GetSerializeSize(CBlock(), SER_DISK, CLIENT_VERSION) -
            (2 * GetSizeOfCompactSize(0)) +
            GetSizeOfCompactSize(block.vtx.size());
        for (std::vector<CTransaction>::const_iterator it = block.vtx.begin();
             it != block.vtx.end(); ++it)
        {
            if (it->GetHash() == prevout.hash)
            {
                txPrev = *it;
                txindex = CTxIndex(
                    CDiskTxPos(cur.snapshot.nFile, cur.snapshot.nBlockPos, nTxPos),
                    txPrev.vout.size());
                blockFrom = block;
                return true;
            }
            nTxPos += ::GetSerializeSize(*it, SER_DISK, CLIENT_VERSION);
        }
        if (!cur.snapshot.hasParent)
            break;
        ColdHotSeamSnapshot parent;
        {
            std::string err;
            const ColdHotSeamResult pr = nav->GetParentR(cur.ref, &parent, &err);
            if (pr != COLD_HOT_SEAM_OK)
                return false; // authority failure -> fail closed (never pprev fallback)
        }
        cur = parent;
    }
    return false;
}

static bool ReadStakeSourceTransactionInternal(const CBlockIndex* pindexPrev,
    const COutPoint& prevout, CTransaction& txPrev, CTxIndex& txindex,
    CBlock& blockFrom, const std::map<uint256, CBlock>* candidateBlocksForTesting)
{
    CTxDB txdb("r");
    if (txPrev.ReadFromDisk(txdb, prevout, txindex) &&
        blockFrom.ReadFromDisk(txindex.pos.nFile, txindex.pos.nBlockPos, false))
    {
        // A.9a.3c: when a production navigator is retained, the source-ancestry
        // authority is resolved by-value (no resident mapBlockIndex/CBlockIndex* /
        // continuous pprev topology). A navigator authority failure FAILS CLOSED
        // (never a residency fallback, never a mis-validation). Only when NO
        // production navigator exists (pre-A.10, everything materialized) do we
        // use the legacy pointer check.
        const ColdHotSeamNavigator* pNav = GetBlockIndexStakingNavigator();
        if (pNav)
        {
            const int navAncestry = IsBlockInCandidateAncestryNavigated(blockFrom.GetHash(), pindexPrev);
            if (navAncestry < 0)
                return false; // authority failure -> fail closed
            return navAncestry == 1;
        }
        std::map<uint256, CBlockIndex*>::const_iterator mi =
            mapBlockIndex.find(blockFrom.GetHash());
        if (mi != mapBlockIndex.end() &&
            IsBlockInCandidateAncestry(mi->second, pindexPrev))
            return true;
    }

    // Transactions from an unconnected side branch are deliberately absent
    // from the connected-chain tx index. Search only the candidate suffix.
    // A.9a.3d: when a production navigator is retained, walk the candidate
    // side-branch by-value (no resident CBlockIndex* / arbitrary historical
    // pprev topology required); an authority failure fails closed.
    const ColdHotSeamNavigator* sideNav = GetBlockIndexStakingNavigator();
    if (sideNav)
        return SearchCandidateSuffixNavigated(sideNav, pindexPrev, prevout,
                                              txPrev, txindex, blockFrom,
                                              candidateBlocksForTesting);
    for (const CBlockIndex* pindex = pindexPrev;
         pindex && !pindex->IsInMainChain(); pindex = pindex->pprev)
    {
        CBlock block;
        bool fReadBlock = false;
        if (candidateBlocksForTesting)
        {
            std::map<uint256, CBlock>::const_iterator miBlock =
                candidateBlocksForTesting->find(pindex->GetBlockHash());
            if (miBlock != candidateBlocksForTesting->end())
            {
                block = miBlock->second;
                fReadBlock = true;
            }
        }
        else
        {
            fReadBlock = block.ReadFromDisk(pindex);
        }
        if (!fReadBlock)
            return false;

        unsigned int nTxPos = pindex->nBlockPos +
            ::GetSerializeSize(CBlock(), SER_DISK, CLIENT_VERSION) -
            (2 * GetSizeOfCompactSize(0)) +
            GetSizeOfCompactSize(block.vtx.size());
        for (std::vector<CTransaction>::const_iterator it = block.vtx.begin();
             it != block.vtx.end(); ++it)

        {
            if (it->GetHash() == prevout.hash)
            {
                txPrev = *it;
                txindex = CTxIndex(
                    CDiskTxPos(pindex->nFile, pindex->nBlockPos, nTxPos),
                    txPrev.vout.size());
                blockFrom = block;
                return true;
            }
            nTxPos += ::GetSerializeSize(*it, SER_DISK, CLIENT_VERSION);
        }
    }
    return false;
}
bool ReadStakeSourceTransaction(const CBlockIndex* pindexPrev,
    const COutPoint& prevout, CTransaction& txPrev, CTxIndex& txindex,
    CBlock& blockFrom)
{
    return ReadStakeSourceTransactionInternal(
        pindexPrev, prevout, txPrev, txindex, blockFrom, NULL);
}

bool ReadStakeSourceTransactionForTesting(const CBlockIndex* pindexPrev,
    const COutPoint& prevout, CTransaction& txPrev, CTxIndex& txindex,
    CBlock& blockFrom, const std::map<uint256, CBlock>& candidateBlocks)
{
    return ReadStakeSourceTransactionInternal(
        pindexPrev, prevout, txPrev, txindex, blockFrom, &candidateBlocks);
}


// Check kernel hash target and coinstake signature
bool CheckProofOfStake(const CBlockIndex* pindexPrev, const CTransaction& tx, unsigned int nBits, uint256& hashProofOfStake, uint256& targetProofOfStake)
{
    if (!tx.IsCoinStake())
        return error("CheckProofOfStake() : called on non-coinstake %s", tx.GetHash().ToString().c_str());

    if (tx.vin.empty())
        return error("CheckProofOfStake() : no inputs for transparent coinstake %s", tx.GetHash().ToString().c_str());
    const CTxIn& txin = tx.vin[0];

    // First try the connected-chain tx index, then the candidate side branch.
    CTransaction txPrev;
    CTxIndex txindex;
    CBlock block;
    if (!ReadStakeSourceTransaction(
            pindexPrev, txin.prevout, txPrev, txindex, block))
    {
        if (fHybridSPV && pwalletMain)
        {
            LOCK(pwalletMain->cs_wallet);
            std::map<uint256, CWalletTx>::iterator wit = pwalletMain->mapWallet.find(txin.prevout.hash);
            if (wit != pwalletMain->mapWallet.end())
            {
                const CWalletTx& wtx = wit->second;
                // A.9a.3e (NEW-N6): establish the wallet transaction's
                // maturity / active-chain / merkle authority BY-VALUE so a
                // deep-old HybridSPV source whose historical CBlockIndex is
                // absent from mapBlockIndex still passes the exact legacy
                // maturity gate and reaches the by-value source-block recovery
                // below. With a retained navigator NO arbitrary historical
                // CBlockIndex residency is required. The pre-A.10 no-navigator
                // fallback keeps the exact legacy GetDepthInMainChain()
                // semantics while all history is still materialized.
                bool fAuthorityMature = false;
                int nAuthorityDepth = 0;
                const ColdHotSeamNavigator* spvNav = GetBlockIndexStakingNavigator();
                if (spvNav)
                {
                    std::string err;
                    const ColdHotSeamResult mr = spvNav->GetHybridSvmMaturityAuthorityR(
                        BlockIndexLogicalId(wtx.hashBlock), wtx.GetHash(),
                        wtx.vMerkleBranch, wtx.nIndex, &nAuthorityDepth, &err);
                    if (mr == COLD_HOT_SEAM_OK)
                        fAuthorityMature = (nAuthorityDepth >= nCoinbaseMaturity);
                    // NOT_FOUND / AUTHORITY_FAILURE leave fAuthorityMature
                    // false -> reject exactly as legacy depth < maturity.
                }
                else
                {
                    nAuthorityDepth = wtx.GetDepthInMainChain();
                    if (nAuthorityDepth >= nCoinbaseMaturity)
                        fAuthorityMature = true;
                }
                if (!fAuthorityMature)
                    return tx.DoS(10, error("CheckProofOfStake() : SPV stake input needs %d confirmations, has %d",
                                            nCoinbaseMaturity, nAuthorityDepth));
                if (wtx.hashBlock != uint256(0))
                {
                    // A.9a.3i: resolve the source block's disk position using
                    // the same authority-first, overlay-aware path that staking
                    // selection/generation uses.  GetStakingSourceDiskPositionR
                    // checks typed source authority, consults the ephemeral
                    // materialization overlay (where recovered arrival
                    // coordinates are published), and falls back to immutable
                    // cold-snapshot coordinates only when no overlay entry
                    // exists.  This closes the E3 gap where a recovered source
                    // with frozen nFile=0 was invisible to validation despite
                    // having correct overlay coordinates.
                    CBlock block;
                    bool fHaveBlock = false;
                    unsigned int srcFile = 0, srcBlockPos = 0;
                    bool srcAvailable = false;
                    if (GetStakingSourceDiskPositionR(*pwalletMain, wtx.hashBlock,
                            &srcFile, &srcBlockPos, &srcAvailable) == COLD_HOT_SEAM_OK
                        && srcAvailable)
                    {
                        fHaveBlock = block.ReadFromDisk(srcFile, srcBlockPos, true);
                        if (fHaveBlock && block.GetHash() != wtx.hashBlock)
                        {
                            pwalletMain->InvalidateStakingMaterialization(wtx.hashBlock);
                            fHaveBlock = false;
                        }
                    }
                    if (fHaveBlock)
                    {
                        unsigned int nTxPos = ::GetSerializeSize(CBlock(), SER_DISK, CLIENT_VERSION)
                                            - (2 * GetSizeOfCompactSize(0))
                                            + GetSizeOfCompactSize(block.vtx.size());
                        bool fFound = false;
                        for (unsigned int i = 0; i < block.vtx.size(); i++)
                        {
                            if (block.vtx[i].GetHash() == txin.prevout.hash)
                            {
                                txPrev = block.vtx[i];
                                fFound = true;
                                break;
                            }
                            nTxPos += ::GetSerializeSize(block.vtx[i], SER_DISK, CLIENT_VERSION);
                        }

                        if (fFound)
                        {
                            if (!VerifySignature(txPrev, tx, 0, SCRIPT_VERIFY_NONE, 0))
                                return tx.DoS(100, error("CheckProofOfStake() : SPV VerifySignature failed on coinstake %s", tx.GetHash().ToString().c_str()));

                            if (!CheckStakeKernelHash(pindexPrev, nBits, block, nTxPos, txPrev, txin.prevout, tx.nTime, hashProofOfStake, targetProofOfStake, fDebug))
                                return tx.DoS(1, error("CheckProofOfStake() : SPV check kernel failed on coinstake %s", tx.GetHash().ToString().c_str()));

                            return true;
                        }
                    }
                }
            }
        }
        return tx.DoS(1, error("CheckProofOfStake() : INFO: read txPrev failed"));  // previous transaction not in main chain, may occur during initial download
    }

    // Verify signature
    if (!VerifySignature(txPrev, tx, 0, SCRIPT_VERIFY_NONE, 0))
        return tx.DoS(100, error("CheckProofOfStake() : VerifySignature failed on coinstake %s", tx.GetHash().ToString().c_str()));

    if (!CheckStakeKernelHash(pindexPrev, nBits, block, txindex.pos.nTxPos - txindex.pos.nBlockPos, txPrev, txin.prevout, tx.nTime, hashProofOfStake, targetProofOfStake, fDebug))
        return tx.DoS(1, error("CheckProofOfStake() : INFO: check kernel failed on coinstake %s, hashProof=%s", tx.GetHash().ToString().c_str(), hashProofOfStake.ToString().c_str())); // may occur during initial download or if behind on block chain sync

    return true;
}

// Check whether the coinstake timestamp meets protocol
bool CheckCoinStakeTimestamp(int64_t nTimeBlock, int64_t nTimeTx)
{
    // v0.3 protocol
    return (nTimeBlock == nTimeTx);
}

// Get stake modifier checksum
unsigned int GetStakeModifierChecksum(const CBlockIndex* pindex)
{
    if (!pindex->pprev && pindex->GetBlockHash() != GetGenesisBlockHash())
        return 0;

    // Hash previous checksum with flags, hashProofOfStake and nStakeModifier
    CDataStream ss(SER_GETHASH, 0);
    if (pindex->pprev)
        ss << pindex->pprev->nStakeModifierChecksum;
    ss << pindex->nFlags << (pindex->IsProofOfStake() ? pindex->hashProof : 0) << pindex->nStakeModifier;
    if (pindex->nHeight == 100000)
        printf("KMK_STREAM h=%d nFlags=%08x pos=%d mod=%016llx ss_hex=%s\n",
               pindex->nHeight, (unsigned int)pindex->nFlags, pindex->IsProofOfStake()?1:0,
               (unsigned long long)pindex->nStakeModifier,
               HexStr(ss.begin(), ss.end()).c_str());
    uint256 hashChecksum = Hash(ss.begin(), ss.end());
    hashChecksum >>= (256 - 32);
    return hashChecksum.Get64();
}

// Check stake modifier hard checkpoints
bool CheckStakeModifierCheckpoints(int nHeight, unsigned int nStakeModifierChecksum)
{
    // Regtest intentionally does not enforce mainnet historical stake-modifier checkpoints.
    if (fRegTest)
        return true;

    MapModifierCheckpoints& checkpoints = (fTestNet ? mapStakeModifierCheckpointsTestNet : mapStakeModifierCheckpoints);

    if (checkpoints.count(nHeight))
        return nStakeModifierChecksum == checkpoints[nHeight];
    return true;
}

// =========================================================================
// READ-ONLY mainnet differential verify (HARD GATE #1)
// Walks the active chain reconstructed from the PRODUCTION-loaded block index
// (mapBlockIndex) and compares legacy vs optimized ComputeNextStakeModifier.
// Non-generation blocks check the exact fast-path condition; generation blocks
// dual-compute legacy (64-round) and optimized (precomputed selection hashes).
// Any consensus difference is reported as a hard failure.
// =========================================================================
void VerifyStakeModifierDifferential()
{
    int64_t nT0 = GetTimeMillis();
    std::vector<CBlockIndex*> vActive;
    CBlockIndex* pindexBest = NULL;
    for (std::map<uint256, CBlockIndex*>::iterator it = mapBlockIndex.begin(); it != mapBlockIndex.end(); ++it)
        if (!pindexBest || it->second->nChainTrust > pindexBest->nChainTrust)
            pindexBest = it->second;
    if (!pindexBest)
    {
        printf("VERIFY_STAKEMOD: empty block index\n");
        return;
    }
    for (CBlockIndex* p = pindexBest; p; p = p->pprev)
        vActive.push_back(p);
    std::reverse(vActive.begin(), vActive.end());

    int64_t nBlocksChecked = 0, nNonGenChecks = 0, nGenEvents = 0;
    int64_t nLegacyFail = 0, nOptFail = 0, nGenFlagMism = 0, nModMism = 0;
    int64_t nFallbackUses = 0, nMemoUses = 0;
    uint64_t nLastMod = 0;
    bool fInject = GetBoolArg("-stakemodifierverify_faultinject", false);

    for (size_t i = 1; i < vActive.size(); i++)
    {
        CBlockIndex* pprev = vActive[i-1];
        CBlockIndex* cur  = vActive[i];
        uint64_t modO = 0; bool genO = false;
        // optimized path (uses memo O(1) when set, else falls back to legacy walk)
        mapArgs["-stakemodifieropt"] = "1";
        bool okO = ComputeNextStakeModifier(pprev, modO, genO);
        if (!okO) nOptFail++;
        if (fInject && (int64_t)i == vActive.size()/2) modO ^= 1ULL; // fault: expect mismatch

        // generation-due decision exactly matching legacy:
        // prev-generated-modifier-time/interval < pprev->time/interval.
        // pprev->nStakeModifierTime is our in-memory memo == GetLastStakeModifier's
        // nModifierTime. memo==0 (unset) -> dual-compute to be safe.
        bool due;
        if (pprev->nStakeModifierTime == 0)
            due = true;
        else
            due = (pprev->nStakeModifierTime / nModifierInterval < pprev->GetBlockTime() / nModifierInterval);

        if (!due)
        {
            // non-generation: optimized must equal legacy fast-path (same modifier)
            nNonGenChecks++;
            if (genO != false || modO != nLastMod) { nModMism++; nGenFlagMism++; }
        }
        else
        {
            nGenEvents++;
            // dual-compute legacy independently (falls back to full walk + 64 rounds)
            uint64_t modL = 0; bool genL = false;
            mapArgs["-stakemodifieropt"] = "0";
            bool okL = ComputeNextStakeModifier(pprev, modL, genL);
            if (!okL) nLegacyFail++;
            if (okL && okO)
            {
                if (genL != genO) nGenFlagMism++;
                if (modL != modO) nModMism++;
            }
        }

        // apply to current block exactly as AddToBlockIndex does
        cur->SetStakeModifier(modO, genO);
        cur->nStakeModifierTime = genO ? cur->GetBlockTime()
                                       : (pprev ? pprev->nStakeModifierTime : 0);
        nLastMod = modO;
        if (pprev->nStakeModifierTime == 0) nFallbackUses++; else nMemoUses++;
        nBlocksChecked++;

        if ((nBlocksChecked % 500000) == 0)
            printf("VERIFY_STAKEMOD: progress blocks=%lld gen=%lld mism=%lld\n",
                (long long)nBlocksChecked, (long long)nGenEvents, (long long)(nModMism+nGenFlagMism));
    }

    printf("VERIFY_STAKEMOD: ACTIVE_BLOCKS=%lld records=%zu tip_height=%d tip=%s\n",
        (long long)vActive.size(), mapBlockIndex.size(), vActive.back()->nHeight,
        vActive.back()->GetBlockHash().ToString().c_str());
    printf("VERIFY_STAKEMOD: blocks_checked=%lld non_gen_checks=%lld gen_events=%lld\n",
        (long long)nBlocksChecked, (long long)nNonGenChecks, (long long)nGenEvents);
    printf("VERIFY_STAKEMOD: legacy_fail=%lld opt_fail=%lld\n", (long long)nLegacyFail, (long long)nOptFail);
    printf("VERIFY_STAKEMOD: genflag_mismatches=%lld modifier_mismatches=%lld\n",
        (long long)nGenFlagMism, (long long)nModMism);
    printf("VERIFY_STAKEMOD: fallback_uses=%lld memo_uses=%lld elapsed_ms=%lld\n",
        (long long)nFallbackUses, (long long)nMemoUses, (long long)(GetTimeMillis()-nT0));
    if (nGenFlagMism == 0 && nModMism == 0)
        printf("VERIFY_STAKEMOD: RESULT=MAINNET_ZERO_MISMATCH\n");
    else
        printf("VERIFY_STAKEMOD: RESULT=MAINNET_MISMATCH\n");
}
