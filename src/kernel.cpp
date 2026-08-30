// Copyright (c) 2012-2013 The Peercoin developers
// Copyright (c) 2017-2021 The Denarius developers
// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/assign/list_of.hpp>

#include "kernel.h"
#include "cold_hot_seam.h"
#include "blockindex_shadow_startup.h"
#include "txdb.h"
#include "main.h"
#include "wallet.h"
#include "init.h"
#include "nullstake.h"

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

// select a block from the candidate blocks in vSortedByTimestamp, excluding
// already selected blocks in mapSelectedBlocks, and with timestamp up to
// nSelectionIntervalStop. vSelHash[i] is the precomputed selection hash for
// vSortedByTimestamp[i] (round-invariant: hashProof||prevModifier, >>32 for PoS)
// so each candidate is hashed exactly once across the 64 rounds.
static bool SelectBlockFromCandidates(vector<pair<int64_t, uint256> >& vSortedByTimestamp,
    const vector<uint256>& vSelHash,
    map<uint256, const CBlockIndex*>& mapSelectedBlocks,
    int64_t nSelectionIntervalStop, uint64_t nStakeModifierPrev, const CBlockIndex** pindexSelected)
{
    bool fSelected = false;
    uint256 hashBest = 0;
    *pindexSelected = (const CBlockIndex*) 0;
    for (size_t i = 0; i < vSortedByTimestamp.size(); i++)
    {
        const PAIRTYPE(int64_t, uint256)& item = vSortedByTimestamp[i];
        if (!mapBlockIndex.count(item.second))
            return error("SelectBlockFromCandidates: failed to find block index for candidate block %s", item.second.ToString().c_str());
        const CBlockIndex* pindex = mapBlockIndex[item.second];
        if (fSelected && pindex->GetBlockTime() > nSelectionIntervalStop)
            break;
        if (mapSelectedBlocks.count(pindex->GetBlockHash()) > 0)
            continue;
        const uint256& hashSelection = vSelHash[i];
        if (fSelected && hashSelection < hashBest)
        {
            hashBest = hashSelection;
            *pindexSelected = (const CBlockIndex*) pindex;
        }
        else if (!fSelected)
        {
            fSelected = true;
            hashBest = hashSelection;
            *pindexSelected = (const CBlockIndex*) pindex;
        }
    }
    if (fDebug && GetBoolArg("-printstakemodifier"))
        printf("SelectBlockFromCandidates: selection hash=%s\n", hashBest.ToString().c_str());
    return fSelected;
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
    // -stakemodifieropt (default off): recover previous modifier/time in O(1)
    // from the in-memory (chain-own, not serialized) nStakeModifierTime memo
    // instead of the GetLastStakeModifier backward walk. Fallback to legacy when
    // unset (0) / flag off, so semantics are bit-identical.
    bool fOpt = GetBoolArg("-stakemodifieropt", false);
    if (fOpt && pindexPrev && pindexPrev->nStakeModifierTime != 0)
    {
        nStakeModifier = pindexPrev->nStakeModifier;
        nModifierTime  = pindexPrev->nStakeModifierTime;
    }
    else if (!GetLastStakeModifier(pindexPrev, nStakeModifier, nModifierTime))
    {
        return error("ComputeNextStakeModifier: unable to get last modifier");
    }
    if (fDebug)
    {
        printf("ComputeNextStakeModifier: prev modifier=0x%016" PRIx64" time=%s\n", nStakeModifier, DateTimeStrFormat(nModifierTime).c_str());
    }
    if (nModifierTime / nModifierInterval >= pindexPrev->GetBlockTime() / nModifierInterval)
        return true;

    // Sort candidate blocks by timestamp
    vector<pair<int64_t, uint256> > vSortedByTimestamp;
    vSortedByTimestamp.reserve(64 * nModifierInterval / nTargetSpacing);
    int64_t nSelectionInterval = GetStakeModifierSelectionInterval();
    int64_t nSelectionIntervalStart = (pindexPrev->GetBlockTime() / nModifierInterval) * nModifierInterval - nSelectionInterval;
    const CBlockIndex* pindex = pindexPrev;
    while (pindex && pindex->GetBlockTime() >= nSelectionIntervalStart)
    {
        vSortedByTimestamp.push_back(make_pair(pindex->GetBlockTime(), pindex->GetBlockHash()));
        pindex = pindex->pprev;
    }
    int nHeightFirstCandidate = pindex ? (pindex->nHeight + 1) : 0;
    reverse(vSortedByTimestamp.begin(), vSortedByTimestamp.end());
    sort(vSortedByTimestamp.begin(), vSortedByTimestamp.end());

    // Select 64 blocks from candidate blocks to generate stake modifier.
    // Precompute each candidate's selection hash ONCE: it is round-invariant
    // (inputs = pindex->hashProof || nStakeModifier, where nStakeModifier is the
    // previous modifier, constant across all 64 rounds). The precompute applies
    // the PoS >>32 adjustment; the 64 rounds then reuse the exact same value.
    vector<uint256> vSelHash;
    vSelHash.reserve(vSortedByTimestamp.size());
    {
        int64_t nPreUs = GetTimeMicros();
        for (const PAIRTYPE(int64_t, uint256)& item : vSortedByTimestamp)
        {
            const CBlockIndex* pc = mapBlockIndex[item.second];
            CDataStream ss(SER_GETHASH, 0);
            ss << pc->hashProof << nStakeModifier;
            uint256 h = Hash(ss.begin(), ss.end());
            if (pc->IsProofOfStake())
                h >>= 32;
            vSelHash.push_back(h);
        }
        if (fOpt)
        {
            int64_t nPreUs2 = GetTimeMicros() - nPreUs;
            if (nPreUs2 > 250000)
                printf("ComputeNextStakeModifier: precompute_selhash_us=%" PRId64" candidates=%zu\n", nPreUs2, vSelHash.size());
        }
    }

    uint64_t nStakeModifierNew = 0;
    int64_t nSelectionIntervalStop = nSelectionIntervalStart;
    map<uint256, const CBlockIndex*> mapSelectedBlocks;
    for (int nRound=0; nRound<min(64, (int)vSortedByTimestamp.size()); nRound++)
    {
        // add an interval section to the current selection round
        nSelectionIntervalStop += GetStakeModifierSelectionIntervalSection(nRound);
        // select a block from the candidates of current round
        if (!SelectBlockFromCandidates(vSortedByTimestamp, vSelHash, mapSelectedBlocks, nSelectionIntervalStop, nStakeModifier, &pindex))
            return error("ComputeNextStakeModifier: unable to select block at round %d", nRound);
        // write the entropy bit of the selected block
        nStakeModifierNew |= (((uint64_t)pindex->GetStakeEntropyBit()) << nRound);
        // add the selected block from candidates to selected list
        mapSelectedBlocks.insert(make_pair(pindex->GetBlockHash(), pindex));
        if (fDebug && GetBoolArg("-printstakemodifier"))
            printf("ComputeNextStakeModifier: selected round %d stop=%s height=%d bit=%d\n", nRound, DateTimeStrFormat(nSelectionIntervalStop).c_str(), pindex->nHeight, pindex->GetStakeEntropyBit());
    }

    // Print selection map for visualization of the selected blocks
    if (fDebug && GetBoolArg("-printstakemodifier"))
    {
        string strSelectionMap = "";
        // '-' indicates proof-of-work blocks not selected
        strSelectionMap.insert(0, pindexPrev->nHeight - nHeightFirstCandidate + 1, '-');
        pindex = pindexPrev;
        while (pindex && pindex->nHeight >= nHeightFirstCandidate)
        {
            // '=' indicates proof-of-stake blocks not selected
            if (pindex->IsProofOfStake())
                strSelectionMap.replace(pindex->nHeight - nHeightFirstCandidate, 1, "=");
            pindex = pindex->pprev;
        }
        for (const PAIRTYPE(uint256, const CBlockIndex*)& item : mapSelectedBlocks)
        {
            // 'S' indicates selected proof-of-stake blocks
            // 'W' indicates selected proof-of-work blocks
            strSelectionMap.replace(item.second->nHeight - nHeightFirstCandidate, 1, item.second->IsProofOfStake()? "S" : "W");
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
    const BlockIndexLogicalId tipLogical(pindexPrev->GetBlockHash());
    ColdHotSeamSnapshot cur;
    if (nav->ResolveLogicalR(tipLogical, &cur, &err) != COLD_HOT_SEAM_OK)
        return -1;
    for (;;)
    {
        if (cur.snapshot.hash == sourceHash && cur.snapshot.height == source.snapshot.height)
            return 1;
        if (!cur.snapshot.hasParent || cur.snapshot.height <= source.snapshot.height)
            return 0;
        const ColdHotSeamResult pr = nav->GetParentR(cur.ref, &cur, &err);
        if (pr != COLD_HOT_SEAM_OK)
            return -1;
    }
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

    if (tx.nVersion == SHIELDED_TX_VERSION_NULLSTAKE)
    {
        if (tx.nullstakeProof.IsNull() || tx.vShieldedSpend.empty())
            return tx.DoS(100, error("CheckProofOfStake() : NullStake proof missing or no spends"));

        int64_t nWeight = GetWeight((int64_t)tx.nullstakeProof.nBlockTimeFrom,
                                     (int64_t)tx.nullstakeProof.nTimeTx);

        if (!VerifyNullStakeKernelProof(tx.nullstakeProof, tx.vShieldedSpend[0].cv, nBits, nWeight))
            return tx.DoS(1, error("CheckProofOfStake() : NullStake kernel proof verification failed"));

        hashProofOfStake = PedersenKernelHash(tx.nullstakeProof.nStakeModifier,
                                               tx.nullstakeProof.nBlockTimeFrom,
                                               tx.nullstakeProof.nTxPrevOffset,
                                               tx.nullstakeProof.nTxTimePrev,
                                               tx.nullstakeProof.nVoutN,
                                               tx.nullstakeProof.nTimeTx);
        targetProofOfStake = 0;
        return true;
    }

    if (tx.nVersion == SHIELDED_TX_VERSION_NULLSTAKE_V2)
    {
        if (tx.nullstakeProofV2.IsNull() || tx.vShieldedSpend.empty())
            return tx.DoS(100, error("CheckProofOfStake() : NullStake V2 proof missing or no spends"));

        if (!VerifyNullStakeKernelProofV2(tx.nullstakeProofV2, tx.vShieldedSpend[0].cv, nBits))
            return tx.DoS(1, error("CheckProofOfStake() : NullStake V2 kernel proof verification failed"));

        {
            CDataStream ss(SER_GETHASH, 0);
            ss << tx.nullstakeProofV2;
            hashProofOfStake = Hash(ss.begin(), ss.end());
        }
        targetProofOfStake = 0;
        return true;
    }

    if (tx.nVersion == SHIELDED_TX_VERSION_NULLSTAKE_COLD)
    {
        if (tx.nullstakeProofV3.IsNull() || tx.vShieldedSpend.empty())
            return tx.DoS(100, error("CheckProofOfStake() : NullStake V3 proof missing or no spends"));

        if (!VerifyNullStakeKernelProofV3(tx.nullstakeProofV3, tx.vShieldedSpend[0].cv, nBits))
            return tx.DoS(1, error("CheckProofOfStake() : NullStake V3 kernel proof verification failed"));

        {
            CDataStream ss(SER_GETHASH, 0);
            ss << tx.nullstakeProofV3;
            hashProofOfStake = Hash(ss.begin(), ss.end());
        }
        targetProofOfStake = 0;
        return true;
    }

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
                if (wtx.GetDepthInMainChain() < nCoinbaseMaturity)
                    return tx.DoS(10, error("CheckProofOfStake() : SPV stake input needs %d confirmations, has %d",
                                            nCoinbaseMaturity, wtx.GetDepthInMainChain()));
                if (wtx.hashBlock != uint256(0))
                {
                    // A.9a.3d: resolve the historical source block's disk
                    // position by-value (retained navigator) so HybridSPV source
                    // recovery does not require an arbitrary historical
                    // CBlockIndex to be resident in mapBlockIndex. operator[] is
                    // never used for inspection. Safe non-inserting find() only
                    // when no navigator is retained (pre-A.10 fully materialized).
                    CBlock block;
                    bool fHaveBlock = false;
                    const ColdHotSeamNavigator* spvNav = GetBlockIndexStakingNavigator();
                    if (spvNav)
                    {
                        std::string err;
                        ColdHotSeamSnapshot src;
                        if (spvNav->ResolveLogicalR(BlockIndexLogicalId(wtx.hashBlock), &src, &err) == COLD_HOT_SEAM_OK)
                            fHaveBlock = block.ReadFromDisk(src.snapshot.nFile, src.snapshot.nBlockPos, true);
                    }
                    else
                    {
                        std::map<uint256, CBlockIndex*>::const_iterator mi = mapBlockIndex.find(wtx.hashBlock);
                        if (mi != mapBlockIndex.end())
                            fHaveBlock = block.ReadFromDisk(mi->second, true);
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
