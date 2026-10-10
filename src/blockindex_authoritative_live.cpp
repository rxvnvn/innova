// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "blockindex_authoritative_live.h"

#include "blockindex_hot_owner.h"
#include "blockindex_startup_bootstrap.h"
#include "blockindex_accessor.h"
#include "kernel.h"
#include "main.h"

#include <boost/filesystem.hpp>

namespace fs = boost::filesystem;

class BlockIndexAuthoritativeLive::Impl
{
public:
    // base-known callback: parent is known if the base reader (blocks <= S) or
    // the mutable tip (> S) can resolve it by value.
    // baseKnown for the acceptance seam uses the reader.
    const BlockIndexV2Reader*  baseReader;
    std::unique_ptr<BlockIndexTipAuthority> tip;
    std::unique_ptr<BlockIndexLiveTail>     tail;
    std::unique_ptr<BlockIndexLiveAcceptance> seam;
    int horizon;
    uint64_t baseGeneration;
    bool open;
    std::string root;

    // G1: owned full-topology parent materialization (bounded by the live-tail
    // horizon). A MATERIALIZED_CHAIN_ floor is the base tip S (the already-resident
    // boundary); the walk stops there and never reconstructs arbitrary deep history.
    int32_t baseTipHeight;
    uint256 baseTipHash;
    // Owned chain objects + their owner-owned hash identities. Kept for the
    // operation lifetime (the consensus engine dereferences pprev/pnext/pskip);
    // evicted on the next materialization / Close so residency stays bounded.
    std::vector<CBlockIndex*> ownedChain_;
    std::vector<uint256*>      ownedChainHashes_;

    // G1-A: PERSISTENT full-topology residency, keyed by logical hash. Holds the
    // parent chain materialized for AddToBlockIndex so accepted-mapBlockIndex
    // entries' pprev stays valid ACROSS operations (survives
    // ReleaseOperationMaterializations). anchor_ hashes are non-evictable.
    //
    // REPAIR #4 (live-authority residency): the store is BOUNDED. A steady-state
    // KEEP window ends at floor height = liveTipHeight -
    // FullResidentKeepWindow(); older entries are evicted ONLY when they hold no
    // live external identity edge (see ReleaseIdentityEdgesLocked /
    // EvictEligibleLocked: mapBlockIndex-resident children, pinned operations
    // and anchors all block eviction, and a survivor's pnext/pskip/pprev edge to
    // the victim is nulled). Evicted blocks stay fully materializable BY VALUE
    // from the V2 authority via ResolveBlockSnapshot/maturity-style walks (no
    // raw-pprev deep walks are required under the authority contract). KEEP
    // covers the bounded mapBlockIndex span, the LivingTail span and WALK
    // (nMedianTimeSpan+2) with margin.
    // OWNERSHIP CONTRACT (disk-native): the published best-tip working set is
    // owned by the BOUNDED BlockIndexHotOwner below -- identity-keyed
    // (BlockIndexLogicalId / hash), with explicit anchor lifetime, bounded
    // resident count and deterministic release on eviction/destruction. There is
    // NO process-permanent historical CBlockIndex registry here (the former
    // tip-window (BlockIndexHotOwner)/tipWindow identity slots/tip-window anchors maps are GONE); the
    // owner frees every adopted object at eviction or destruction.
    BlockIndexHotOwner tipWindow_;
    size_t          tipWindowPeak_;
    // REPAIR #4: operational bookkeeping for the bounded window (all O(window)
    // or O(1); never O(history)).
    int32_t       tipWindowLiveTipHeight_;  // highest accepted height (tip anchor)

    Impl()
        : baseReader(NULL), horizon(2048), baseGeneration(0),
          open(false), baseTipHeight(-1), baseTipHash(0), tipWindowPeak_(0),
          tipWindowLiveTipHeight_(-1)
    {
    }

    ~Impl()
    {
        for (size_t i = 0; i < ownedChain_.size(); ++i)
        {
            delete ownedChain_[i];
            delete ownedChainHashes_[i];
        }
        // tipWindow_ (BlockIndexHotOwner) owns every adopted tip-window object and
        // frees it in its own destructor; no manual release is required here.
    }
};

namespace {

bool BlockIndexAuthoritativeLiveBaseKnown(const uint256& hash, void* ud)
{
    BlockIndexAuthoritativeLive::Impl* self =
        static_cast<BlockIndexAuthoritativeLive::Impl*>(ud);
    if (!self || !self->baseReader || !self->baseReader->IsOpen())
        return false;
    BlockIndexSnapshot sn;
    std::string err;
    BlockIndexV2ReadStatus st = self->baseReader->LookupByHash(hash, &sn, &err);
    return st == BLOCK_INDEX_V2_READ_FOUND;
}

static void ClearError(std::string* error)
{
    if (error) error->clear();
}

static bool SetError(std::string* error, const std::string& message)
{
    if (error) *error = message;
    return false;
}

// ---------------------------------------------------------------------------
// REPAIR #4 - bounded live-authority residency domain (tip-window (BlockIndexHotOwner) window).
//
//   RESIDENCY LIFETIME != AUTHORITY: an object below the KEEP window holds
//   NO consumer identity by construction (mapBlockIndex-retirements already
//   detached it, frontiers resolve by value, and every deep ancestry read is
//   by value through ResolveBlockSnapshot).
//
// All functions in this unnamed namespace are cs_main-serialized by their
// callers (PublishAuthoritativeBestTip -> AddToBlockIndex -> ProcessBlock);
// the Impl does not lock independently. Cost of the steady-state pass is
// O(mapBlockIndex + tip-window (BlockIndexHotOwner)) per accepted block: BOTH maps are bounded
// by the KEEP window, never O(history).
// ---------------------------------------------------------------------------

// REPAIR #4 (P06-R4/D6): the steady-state KEEP window. Covers the bounded
// mapBlockIndex span, the LivingTail (== horizon), WALK (nMedianTimeSpan + 2)
// and reserve for the locator / reorg margins, with a >=2048 semantic floor so
// a small configured horizon never truncates the walking margin.
static int32_t FullResidentKeepWindow(const BlockIndexAuthoritativeLive::Impl* self)
{
    const int32_t KEEP = 2 * (int32_t)self->horizon;
    return (KEEP < 2048) ? 2048 : KEEP;
}


// REPAIR #4: compact topological-hold predicate, shared by the eviction scan
// and the edge release. TRUE when `o` and `victim` are the same identity, or
// when `o` dereferences `victim` through ANY raw topology edge
// (pprev / pskip / pnext).
static inline bool RR4HoldsTopology(const CBlockIndex* o, const CBlockIndex* victim)
{
    if (!o || !victim)
        return false;
    return (o == victim || o->pprev == victim || o->pskip == victim ||
            o->pnext == victim);
}

// Null every raw pprev/pskip/pnext edge that points AT `victim` from a
// still-resident tip-window (BlockIndexHotOwner) object or a bounded mapBlockIndex entry, then
// clear the victim's OWN edges. Contract:
//   * no later resident walk can dereference freed storage;
//   * a survivor whose edge to the victim is nulled terminates its raw walk at
//     the resident floor exactly as the ordinary bounded mapBlockIndex chain
//     does, and any DEEPER ancestry resolves BY VALUE through the V2 authority
//     (ResolveBlockSnapshot / maturity walks) - the fail-closed by-value path;
//   * no legacy fallback chain is left behind after release.
static void RR4ReleaseEdges(BlockIndexAuthoritativeLive::Impl* self,
                            const uint256& victimHash)
{
    CBlockIndex* victim = self->tipWindow_.GetResidentRaw(victimHash);
    if (!victim)
        return;
    std::vector<uint256> residentHashes = self->tipWindow_.OwnedHashes();
    for (size_t ri = 0; ri < residentHashes.size(); ++ri)
    {
        CBlockIndex* o = self->tipWindow_.GetResidentRaw(residentHashes[ri]);
        if (!o || o == victim)
            continue;
        if (o->pprev == victim) o->pprev = NULL;
        if (o->pskip == victim) o->pskip = NULL;
        if (o->pnext == victim) o->pnext = NULL;
    }
    for (std::map<uint256, CBlockIndex*>::iterator rit = mapBlockIndex.begin();
         rit != mapBlockIndex.end(); ++rit)
    {
        CBlockIndex* o = rit->second;
        if (!o || o == victim)
            continue;
        if (RR4HoldsTopology(o, victim))
        {
            if (o->pprev == victim) o->pprev = NULL;
            if (o->pskip == victim) o->pskip = NULL;
            if (o->pnext == victim) o->pnext = NULL;
        }
    }
    victim->pprev = NULL;
    victim->pnext = NULL;
    victim->pskip = NULL;
}

// REPAIR #4 EXECUTION: the single production eviction pass over tip-window (BlockIndexHotOwner),
// driven from PublishAuthoritativeBestTip after each accepted-block residency
// rotation. Caller contract (cs_main-serialized):
//   * tipWindowLiveTipHeight_ = the highest accepted / top-anchored height;
//   * eviction NEVER frees an identity that is still held: anchors,
//     pindexBest / pindexGenesisBlock, and any object reachable from the
//     BOUNDED mapBlockIndex span (as an entry itself, or through a raw
//     pprev / pskip / pnext edge of one) block eviction;
//   * only a surviving RAW edge that points at the victim is nulled (deep
//     ancestry continues BY VALUE through the V2 authority; no legacy
//     fallback chain is left behind);
//   * the victim is erased from tip-window (BlockIndexHotOwner) / tipWindow identity slots and freed;
//     tip-window anchors hashes are never victims;
//   * re-materialization of an evicted identity goes through the existing
//     by-value snapshot path (ResolveBlockSnapshot / FullFromSnapshot).
static void RR4EvictBelowFloorInto(BlockIndexAuthoritativeLive::Impl* self)
{
    if (!self || !self->open || !self->baseReader || !self->baseReader->IsOpen())
        return; // fail closed: never evict without a healthy re-materializable floor
    if (self->tipWindow_.OwnedCount() <= 1)
        return;
    int32_t tipHeight = self->tipWindowLiveTipHeight_;
    if (tipHeight < 0 && self->tip)
        tipHeight = self->tip->TipHeight();
    if (tipHeight < 0)
        return;
    const int32_t floor = tipHeight - FullResidentKeepWindow(self);
    if (floor <= 0)
        return;

    // Collect each identity the bounded mapBlockIndex span currently holds,
    // ONCE (O(map)), so the resident scan below is a lookup, not a nested
    // sweep. A held identity = a promoted map entry itself, or any object a
    // map entry's raw edge dereferences.
    std::set<const CBlockIndex*> heldSet;
    if (pindexBest)
        heldSet.insert(pindexBest);
    if (pindexGenesisBlock)
        heldSet.insert(pindexGenesisBlock);
    for (std::map<uint256, CBlockIndex*>::const_iterator rit = mapBlockIndex.begin();
         rit != mapBlockIndex.end(); ++rit)
    {
        const CBlockIndex* o = rit->second;
        if (!o)
            continue;
        heldSet.insert(o);
        if (o->pprev) heldSet.insert(o->pprev);
        if (o->pskip) heldSet.insert(o->pskip);
        if (o->pnext) heldSet.insert(o->pnext);
        (void)0; // helper contract covered above
    }

    std::vector<uint256> evictIDs;
    std::vector<uint256> residentHashes = self->tipWindow_.OwnedHashes();
    for (size_t ri = 0; ri < residentHashes.size(); ++ri)
    {
        const uint256& h = residentHashes[ri];
        CBlockIndex* victim = self->tipWindow_.GetResidentRaw(h);
        if (!victim || victim->nHeight >= floor)
            continue; // inside the retained window
        if (self->tipWindow_.IsAnchored(h))
            continue; // explicitly anchored (base tip / live-tip authority)
        if (victim == pindexBest || victim == pindexGenesisBlock)
            continue; // permanent global identity holders
        if (heldSet.count(victim))
            continue; // identity-held by the bounded mapBlockIndex topology
        evictIDs.push_back(h);
    }
    if (evictIDs.empty())
        return;
    // FIRST release all survivor edges pointing at the victims (identity-
    // consistent), THEN free. Two phases so a survivor is never left holding a
    // pointer into storage freed within the same pass.
    for (size_t k = 0; k < evictIDs.size(); ++k)
        RR4ReleaseEdges(self, evictIDs[k]);
    for (size_t k = 0; k < evictIDs.size(); ++k)
        self->tipWindow_.ReleaseOwned(evictIDs[k]); // frees object + owned identity
}

} // namespace


BlockIndexAuthoritativeLive::BlockIndexAuthoritativeLive()
    : impl_(new Impl())
{
}

BlockIndexAuthoritativeLive::~BlockIndexAuthoritativeLive()
{
    Close();
    delete impl_;
}

bool BlockIndexAuthoritativeLive::Open(const std::string& v2Root,
                                        const BlockIndexV2Reader* baseReader,
                                        int livetailHorizon,
                                        std::string* error)
{
    if (impl_->open)
    {
        Close();
        impl_->open = false;
    }
    if (!baseReader || !baseReader->IsOpen())
        return SetError(error, "authoritative-live: base reader not open");
    const uint64_t gen = baseReader->Generation();
    if (gen == 0)
        return SetError(error, "authoritative-live: base reader generation 0");

    impl_->baseReader = baseReader;
    impl_->baseGeneration = gen;
    impl_->horizon = (livetailHorizon > 0) ? livetailHorizon : 2048;
    impl_->root = v2Root;

    const uint64_t baseRecordCount = baseReader->RecordCount();
    // Determine the base generation's committed tip height S from the reader.
    const BlockIndexSnapshot tipSnap = baseReader->GetTip();
    const int32_t baseTipHeight = tipSnap.found ? (int32_t)tipSnap.height : -1;
    if (baseTipHeight < 0)
        return SetError(error, "authoritative-live: base generation has no tip");
    impl_->baseTipHeight = baseTipHeight;
    impl_->baseTipHash = tipSnap.found ? tipSnap.hash : uint256(0);

    // Open (or create) the mutable tip store under <v2Root>/blockindex_tip.
    impl_->tip.reset(new BlockIndexTipAuthority());
    fs::path tipMeta = fs::path(v2Root) / "blockindex_tip" / "tip.meta";
    if (fs::exists(tipMeta))
    {
        if (!BlockIndexTipAuthority::Open(v2Root, gen, impl_->tip.get(), error))
        {
            impl_->tip.reset();
            return false; // fail closed on base/tip mismatch or corrupt state
        }
    }
    else
    {
        if (!BlockIndexTipAuthority::Create(v2Root, gen, baseRecordCount,
                                            baseTipHeight, impl_->tip.get(), error))
        {
            impl_->tip.reset();
            return false;
        }
    }

    // Tail composite materializer: base reader (<=S) + tip (> S).
    impl_->tail.reset(new BlockIndexLiveTail());
    impl_->tail->SetSources(impl_->baseReader, impl_->tip.get());
    impl_->tail->SetHorizon(impl_->horizon);
    impl_->tail->SetCurrentGeneration(gen);
    // permanent anchors: best tip (from base reader tip snapshot) + genesis.
    {
        BlockIndexLogicalId best(impl_->baseReader->GetTip().hash);
        // only anchor the base tip if found; genesis is height 0 record
        if (tipSnap.found)
            impl_->tail->PinPermanent(best);
        BlockIndexSnapshot g0;
        std::string gerr;
        if (impl_->baseReader->GetActiveByHeight(0, &g0, &gerr) == BLOCK_INDEX_V2_READ_FOUND && g0.found)
            impl_->tail->PinPermanent(BlockIndexLogicalId(g0.hash));
    }

    // Acceptance seam arm: baseKnown = reader lookup, tip + tail (ud = impl_).
    impl_->seam.reset(new BlockIndexLiveAcceptance());
    impl_->seam->SetSources(&BlockIndexAuthoritativeLiveBaseKnown, impl_,
                            impl_->tip.get(), impl_->tail.get());

    impl_->open = true;
    ClearError(error);
    return true;
}

bool BlockIndexAuthoritativeLive::ResolveParent(const uint256& parentHash,
                                                 int* parentHeight,
                                                 std::string* error) const
{
    if (!impl_->open || !impl_->seam)
        return SetError(error, "authoritative-live: not open");
    const BlockIndexLiveAcceptance::ParentStatus ps =
        impl_->seam->ResolveParent(parentHash, parentHeight, error);
    if (ps == BlockIndexLiveAcceptance::PARENT_ERROR)
        return SetError(error, "authoritative-live: parent resolve error");
    if (ps == BlockIndexLiveAcceptance::PARENT_KNOWN)
    {
        if (error) error->clear();
        return true;
    }
    return false; // PARENT_UNKNOWN: genuine orphan
}
BlockIndexAuthoritativeParentStatus BlockIndexAuthoritativeLive::ResolveParentInfo(
    const uint256& parentHash,
    BlockIndexAuthoritativeParentInfo* out,
    std::string* error) const
{
    if (!out)
    {
        SetError(error, "authoritative-live: null parent info");
        return BLOCK_INDEX_AUTHORITATIVE_PARENT_FAILURE;
    }
    *out = BlockIndexAuthoritativeParentInfo();
    if (!impl_->open || !impl_->baseReader || !impl_->baseReader->IsOpen())
    {
        SetError(error, "authoritative-live: parent authority unavailable");
        return BLOCK_INDEX_AUTHORITATIVE_PARENT_FAILURE;
    }
    if (impl_->baseReader->Generation() != impl_->baseGeneration)
    {
        SetError(error, "authoritative-live: parent authority generation changed");
        return BLOCK_INDEX_AUTHORITATIVE_PARENT_FAILURE;
    }

    // Mutable tip first: this covers bounded active and side records created
    // after the immutable generation was selected.
    if (impl_->tip && impl_->tip->IsOpen())
    {
        BlockIndexTipRead tipRead = impl_->tip->LookupByHash(parentHash, error);
        if (tipRead.status == BLOCK_INDEX_TIP_OK)
        {
            out->hash = tipRead.record.hash;
            out->height = tipRead.height;
            out->proofOfStake = (tipRead.record.prevoutStake.hash != uint256(0));
            out->active = tipRead.active;
            out->nFile = tipRead.record.nFile;
            out->nBlockPos = tipRead.record.nBlockPos;
            ClearError(error);
            return BLOCK_INDEX_AUTHORITATIVE_PARENT_FOUND;
        }
        if (tipRead.status != BLOCK_INDEX_TIP_NOT_FOUND)
        {
            SetError(error, "authoritative-live: mutable parent lookup failed");
            return BLOCK_INDEX_AUTHORITATIVE_PARENT_FAILURE;
        }
    }

    BlockIndexSnapshot snapshot;
    BlockIndexV2ReadStatus baseStatus =
        impl_->baseReader->LookupByHash(parentHash, &snapshot, error);
    if (baseStatus == BLOCK_INDEX_V2_READ_NOT_FOUND)
    {
        *out = BlockIndexAuthoritativeParentInfo();
        return BLOCK_INDEX_AUTHORITATIVE_PARENT_NOT_FOUND;
    }
    if (baseStatus != BLOCK_INDEX_V2_READ_FOUND)
    {
        *out = BlockIndexAuthoritativeParentInfo();
        return BLOCK_INDEX_AUTHORITATIVE_PARENT_FAILURE;
    }
    out->hash = snapshot.hash;
    out->height = snapshot.height;
    out->proofOfStake = snapshot.fProofOfStake;
    out->active = snapshot.fInMainChain;
    out->nFile = snapshot.nFile;
    out->nBlockPos = snapshot.nBlockPos;
    ClearError(error);
    return BLOCK_INDEX_AUTHORITATIVE_PARENT_FOUND;
}

BlockIndexHotStatus BlockIndexAuthoritativeLive::Materialize(
    const uint256& hash, BlockIndexHotHandle* out) const
{
    if (!impl_->open || !impl_->tail)
        return BlockIndexHotStatus::AUTHORITY_MISSING;
    if (!out)
        return BlockIndexHotStatus::MATERIALIZATION_UNAVAILABLE;
    return impl_->tail->Pin(BlockIndexLogicalId(hash), out);
}

BlockIndexHotStatus BlockIndexAuthoritativeLive::ResolveBlockSnapshot(
    const uint256& hash, BlockIndexSnapshot* out, std::string* error) const
{
    if (error) error->clear();
    if (!impl_->open || !impl_->baseReader || !impl_->tip)
        return BlockIndexHotStatus::AUTHORITY_MISSING;
    if (!out)
        return BlockIndexHotStatus::MATERIALIZATION_UNAVAILABLE;
    // Use the SINGLE composite current-tail materializer (tip-then-base,
    // by-value) — the same logical authority seam used by the live acceptance
    // path — so a post-generation retained hash resolves from the mutable tip,
    // and a generation/old hash falls back to the immutable base reader. No
    // residency is allocated (this is a pure by-value snapshot read).
    BlockIndexLiveTailMaterializer mat(impl_->baseReader, impl_->tip.get());
    BlockIndexHotMaterialized m;
    const BlockIndexHotStatus st = mat.Materialize(BlockIndexLogicalId(hash), &m);
    if (st == BlockIndexHotStatus::OK)
    {
        if (!m.found || m.snapshot.hash != hash)
        {
            if (error) *error = "authoritative-live: current-tail snapshot identity mismatch for " + hash.GetHex();
            return BlockIndexHotStatus::CORRUPT_METADATA;
        }
        *out = m.snapshot;
        return BlockIndexHotStatus::OK;
    }
    if (error) *error = "authoritative-live: current-tail snapshot resolve failed (status=" + std::to_string((int)st) + ")";
    return (st == BlockIndexHotStatus::AUTHORITY_MISSING) ? st : BlockIndexHotStatus::CORRUPT_METADATA;
}

// R2-REPAIR (C1): by-value MedianTimePast (see header for the full rationale).
bool BlockIndexAuthoritativeLive::ResolveMedianTimePastByValue(
    const uint256& hash, int64_t* out, std::string* error) const
{
    if (error) error->clear();
    if (!out)
        return false;
    if (!impl_->open || !impl_->baseReader || !impl_->tip)
    {
        if (error) *error = "authoritative-live: median-time-past authority closed (fail closed)";
        return false;
    }
    // Byte-equivalent to CBlockIndex::GetMedianTimePast() (main.h:1601): up to
    // nMedianTimeSpan=11 block-times walking backward from `hash` (the parent),
    // packed from the back of the window, sorted, element [(filled)/2].
    const int nSpan = CBlockIndex::nMedianTimeSpan; // 11
    int64_t pmedian[128];
    int64_t* pbegin = &pmedian[nSpan];
    int64_t* pend = &pmedian[nSpan];
    int filled = 0;
    uint256 cur = hash;
    while (filled < nSpan && cur != uint256(0))
    {
        BlockIndexSnapshot s;
        std::string err2;
        const BlockIndexHotStatus st = ResolveBlockSnapshot(cur, &s, &err2);
        if (st != BlockIndexHotStatus::OK)
        {
            if (error)
                *error = "median-time-past by-value: ancestor resolve failed for " +
                         cur.ToString() + ": " + err2;
            return false;
        }
        *(--pbegin) = (int64_t)s.nTime;
        ++filled;
        cur = s.hashPrev;
    }
    std::sort(const_cast<int64_t*>(pbegin), const_cast<int64_t*>(pend));
    *out = pbegin[(pend - pbegin) / 2];
    return true;
}

namespace {
// Build a full-topology CBlockIndex from a by-value snapshot (scalar fields; the
// caller links pprev/pnext/pskip).
CBlockIndex* FullFromSnapshot(const BlockIndexSnapshot& s, uint256* ownHash)
{
    CBlockIndex* p = new CBlockIndex();
    *ownHash = s.hash;
    p->phashBlock = ownHash;
    p->pprev = NULL;
    p->pnext = NULL;
    p->pskip = NULL;
    p->nHeight     = s.height;
    p->nFile       = s.nFile;
    p->nBlockPos   = s.nBlockPos;
    // R1/R2 core rewrite: by-value parent identity for the persistence path.
    p->hashPrevStable = s.hashPrev;
    p->nChainTrust = s.nChainTrust;
    p->hashProof   = s.hashProof;
    p->hashMerkleRoot = s.hashMerkleRoot;
    p->prevoutStake   = s.prevoutStake;
    p->nStakeTime     = s.nStakeTime;
    p->nVersion   = s.nVersion;
    p->nTime      = s.nTime;
    p->nBits      = s.nBits;
    p->nNonce     = s.nNonce;
    p->nMint      = s.nMint;
    p->nMoneySupply = s.nMoneySupply;
    p->nStakeModifier = s.nStakeModifier;
    // G1-A: the by-value snapshot carries the authority's nFlags (incl.
    // BLOCK_STAKE_MODIFIER / BLOCK_GENERATED for genesis + stake blocks). These
    // MUST be preserved on the materialized object, or the legacy consensus
    // walks (GetLastStakeModifier/GeneratedStakeModifier, IsProofOfStake,
    // ComputeNextStakeModifier) diverge for a boundary accept. Prior to this a
    // materialized genesis lost its BLOCK_STAKE_MODIFIER flag (nFlags=0),
    // breaking the boundary block's stake-modifier walk.
    p->nFlags = s.nFlags;
    if (s.hasStakeModifierTime)
        p->nStakeModifierTime = s.nStakeModifierTime;
    if (s.hasStakeModifierChecksum)
        p->nStakeModifierChecksum = s.nStakeModifierChecksum;
    // BLOCK-100000 RECURRENCE FIX: the live/tip materializer must ALWAYS surface
    // the canonical genesis stake-modifier checksum. On a fresh datadir the
    // genesis derived-checksum entry can be absent in the live/tip store, so
    // hasStakeModifierChecksum==false leaves nStakeModifierChecksum=0 here and
    // block 1 (and the whole live chain) computes a NON-canonical checksum
    // recurrence -> the 100000 stake-modifier checkpoint rejects. Force the
    // canonical value (GetStakeModifierChecksum(genesis)==0x0e00670b) whenever
    // we materialize the canonical genesis block.
    if (p->nHeight == 0 && p->GetBlockHash() == GetGenesisBlockHash())
        p->nStakeModifierChecksum = GetStakeModifierChecksum(p);
    if (s.fProofOfStake)
        p->nFlags |= CBlockIndex::BLOCK_PROOF_OF_STAKE;
    return p;
}
} // namespace

CBlockIndex* BlockIndexAuthoritativeLive::PublishAuthoritativeBestTip(
    const uint256& parentHash, std::string* error)
{
    if (!impl_->open || !impl_->baseReader)
    {
        if (error) *error = "authoritative-live: not open";
        return NULL;
    }
    // If already adopted, return the existing owned object (idempotent).
    {
        CBlockIndex* existing = impl_->tipWindow_.GetResidentRaw(parentHash);
        if (existing)
            return existing;
    }

    // Walk the parent chain by value (tip-then-base) down to (and including)
    // the floor needed for the boundary block's OWN consensus walks. The base
    // tip S itself floors the ORIGINAL walk (MaterializeParentChain stops at S),
    // but a boundary accept's ConnectBlock/AcceptBlock walks GET_MEDIAN_TIME_SPAN
    // (11) pprev ancestors for median-time, plus a few for difficulty/stake.
    // Those ancestors are BELOW S. Serving them means the retained chain must
    // extend a bounded constant depth below S (never O(N)): ancestry within the
    // consensus walk window is materialized and retained so the block's median
    // time / difficulty / stake-modifier checks match legacy exactly. Entries
    // below this bounded window are never resident (deep history stays V2-only).
    std::vector<BlockIndexSnapshot> path;             // path[0] = parent (highest)
    std::string rerr;
    uint256 cur = parentHash;
    bool reachedFloor = false;
    // Walk down to height floorHeight = max(0, baseTipHeight - WALK) so the
    // retained chain covers the median-time (11) + difficulty/stake margin.
    const int WALK = CBlockIndex::nMedianTimeSpan + 2; // 13
    const int floorHeight = std::max(0, impl_->baseTipHeight - WALK);
    for (int guard = 0; guard < 20 * 1000 * 1000; ++guard)
    {
        bool stop = false;
        if (cur == uint256(0))
        {
            reachedFloor = true; // reached genesis root
            stop = true;
        }
        if (stop)
            break;
        BlockIndexSnapshot s;
        bool found = false;
        if (impl_->tip && impl_->tip->IsOpen())
        {
            BlockIndexTipRead tr = impl_->tip->LookupByHash(cur, error);
            if (tr.status == BLOCK_INDEX_TIP_OK)
            {
                s.found = true;
                s.hash = tr.record.hash;
                s.hashPrev = tr.record.hashPrev;
                s.hashMerkleRoot = tr.record.hashMerkleRoot;
                s.height = tr.record.height;
                s.nFile = tr.record.nFile;
                s.nBlockPos = tr.record.nBlockPos;
                s.nFlags = tr.record.nFlags;
                s.nVersion = tr.record.nVersion;
                s.nTime = tr.record.nTime;
                s.nBits = tr.record.nBits;
                s.nNonce = tr.record.nNonce;
                s.nMint = tr.record.nMint;
                s.nMoneySupply = tr.record.nMoneySupply;
                s.nStakeModifier = tr.record.nStakeModifier;
                s.prevoutStake = tr.record.prevoutStake;
                s.nStakeTime = tr.record.nStakeTime;
                s.hashProof = tr.record.hashProof;
                s.fProofOfStake = (tr.record.prevoutStake.hash != uint256(0));
                s.hasParent = (tr.record.hashPrev != uint256(0));
                s.nChainTrust = tr.derived.chainTrust;
                s.nStakeModifierChecksum = tr.derived.stakeModifierChecksum;
                s.hasStakeModifierChecksum = true;
                s.nStakeModifierTime = tr.derived.stakeModifierTime;
                s.hasStakeModifierTime = tr.derived.HasStakeModifierTime();
                found = true;
            }
        }
        if (!found)
        {
            BlockIndexV2ReadStatus st = impl_->baseReader->LookupByHash(cur, &s, error);
            if (st == BLOCK_INDEX_V2_READ_FOUND && s.found)
                found = true;
        }
        if (!found)
        {
            if (error) *error = "authoritative-live(retain): walk lookup failed at " + cur.ToString();
            return NULL;
        }
        path.push_back(s);
        // R2-STARTUP: bound the retained materialization to the live-tail
        // horizon. When the mutable tip has grown far beyond the immutable
        // base (e.g. gen-1 is minimal and the whole chain lives in the tip),
        // walking all the way back to the base floor is O(height) and builds
        // an O(path) anon vector behind a bounded hot window — the startup OOM
        // at ~2M blocks under 1 GiB. Deeper ancestry is never resident; it is
        // served BY VALUE via the V2 reader. The boundary block's own consensus
        // walks (median-time 11, difficulty, stake) reach only ~13 ancestors,
        // all within the tail, so capping at the horizon changes no outcome.
        if (path.size() >= (size_t)impl_->horizon)
        {
            reachedFloor = true;
            break;
        }
        // Reached the bounded consensus-walk floor below S.
        if (s.height <= floorHeight)
        {
            reachedFloor = true;
            break;
        }
        cur = s.hashPrev;
    }
    if (!reachedFloor || path.empty())
    {
        if (error) *error = "authoritative-live(retain): floor mismatch/empty";
        return NULL;
    }

    // Materialize into the PERSISTENT store (topology linked), reusing any
    // ancestor that is already persisted-resident. path[0]=parent .. path.back()=floor S.
    for (size_t i = 0; i < path.size(); ++i)
    {
        const uint256& h = path[i].hash;
        if (impl_->tipWindow_.GetResidentRaw(h))
            continue; // already adopted; kept
        uint256 ownTmp(h);
        CBlockIndex* obj = FullFromSnapshot(path[i], &ownTmp);
        // OWNERSHIP: adopt under the bounded hot owner. phashBlock is re-pointed
        // at the owner-stable identity slot (std::map node address). The base tip
        // (boundary) is anchored so the eviction pass never frees it.
        impl_->tipWindow_.AdoptOwned(BlockIndexLogicalId(h), obj, h,
                                     h == impl_->baseTipHash);
    }
    if (impl_->tipWindow_.OwnedCount() > impl_->tipWindowPeak_)
        impl_->tipWindowPeak_ = impl_->tipWindow_.OwnedCount();
    // Link topology: pprev -> floor, pnext -> tip within the resolved chain AND
    // against already-persistent ancestors so the whole pointer graph is valid.
    // Build height-ordered list path[0] (highest) .. path.back() (floor).
    for (size_t i = 0; i < path.size(); ++i)
    {
        CBlockIndex* o = impl_->tipWindow_.GetResidentRaw(path[i].hash);
        const uint256& hPrev = path[i].hashPrev;
        CBlockIndex* prev = (i + 1 < path.size()) ? impl_->tipWindow_.GetResidentRaw(path[i + 1].hash) : NULL;
        if (!prev && hPrev != uint256(0))
            prev = impl_->tipWindow_.GetResidentRaw(hPrev);
        o->pprev = prev;
        o->pnext = (i > 0) ? impl_->tipWindow_.GetResidentRaw(path[i - 1].hash) : NULL;
        o->pskip = NULL; // constructed by the skip pass below (BuildSkip semantics)
    }
    // Skip links must satisfy the SAME invariant ordinary resident chains do:
    // pskip is the node at GetSkipHeight(h) when that node is materialized, and
    // NULL when it is not. Substituting pprev (the previous construction) made
    // GetAncestor's height counter disagree with the pointer, so ancestor
    // lookups (CBlockLocator::Set, median-time/difficulty/stake walks, RPC) could
    // return SILENTLY WRONG nodes. Ascending height order (floor first) so each
    // step sees already-correct lower links; a target below the retained floor is
    // not materialized and correctly yields NULL.
    for (size_t i = path.size(); i-- > 0; )
        impl_->tipWindow_.GetResidentRaw(path[i].hash)->BuildSkip();
    // nChainTrust is preserved from the snapshot: FullFromSnapshot sets
    // p->nChainTrust = s.nChainTrust, and the reader now fills s.nChainTrust
    // from the authoritative derived.dat (per-record cumulative trust). No
    // recomputation is needed (and re-accumulating would wrongly drop the
    // heritage of a boundary object that floors at itself). Anchor the base tip.
    // (base-tip anchoring was applied at adoption time by AdoptOwned).
    // REPAIR #4 (P06-R4/D6): the caller resolves HEIGHTS inside the tip
    // authority by-hash-by-pointer only for the stacked accepted child. The
    // resident objects exposed hereafter hold bindings to the accepted tip's
    // pprev; deepen the tip-window (BlockIndexHotOwner) rotation only when the authority floor
    // has moved (KEEP window over the live tip height). pindexNew's OWN ppex
    // pre-call retention is bound to the mapBlockIndex entry (bounded by the
    // retired-floor contract at tipHeight - horizon + 1), never to a
    // tip-window (BlockIndexHotOwner) deep boundary below it.
    {
        CBlockIndex* top = impl_->tipWindow_.GetResidentRaw(parentHash);
        if (top && top->nHeight > impl_->tipWindowLiveTipHeight_)
            impl_->tipWindowLiveTipHeight_ = top->nHeight;
        RR4EvictBelowFloorInto(impl_);
    }
    return impl_->tipWindow_.GetResidentRaw(parentHash);
}

CBlockIndex* BlockIndexAuthoritativeLive::MaterializeParentChainInto(
    const uint256& hash,
    std::vector<CBlockIndex*>* objs,
    std::vector<uint256*>* owns,
    std::string* error,
    bool* walkLookupFailed) const
{
    if (walkLookupFailed) *walkLookupFailed = false;
    if (!impl_->open || !impl_->baseReader)
    {
        if (error) *error = "authoritative-live: not open";
        return NULL;
    }
    if (!objs || !owns)
    {
        if (error) *error = "authoritative-live: null output container";
        return NULL;
    }
    objs->clear();
    owns->clear();

    // Walk parent by value (tip-then-base) down to (and including) the
    // bounded floor needed for the boundary block's OWN consensus walks.
    // The base tip S is the logical boundary, but the boundary accept's
    // AcceptBlock/ConnectBlock walks GET_MEDIAN_TIME_SPAN (11) pprev ancestors
    // (median time) plus difficulty/stake margin below S. So the walk must cover
    // a BOUNDED constant depth below S (never O(N)); deep history stays V2-only.
    std::vector<BlockIndexSnapshot> path; // path[0] = requested parent (highest height)
    uint256 cur = hash;
    bool reachedFloor = false;
    const int WALK = CBlockIndex::nMedianTimeSpan + 2; // 13
    const int floorHeight = std::max(0, impl_->baseTipHeight - WALK);
    for (int guard = 0; guard < 20 * 1000 * 1000; ++guard)
    {
        bool stop = false;
        if (cur == uint256(0))
        {
            reachedFloor = true; // reached genesis root
            stop = true;
        }
        if (stop)
            break;
        BlockIndexSnapshot s;
        bool found = false;
        // tip authority first (blocks > S).
        if (impl_->tip && impl_->tip->IsOpen())
        {
            BlockIndexTipRead tr = impl_->tip->LookupByHash(cur, error);
            if (tr.status == BLOCK_INDEX_TIP_OK)
            {
                // convert tip read -> snapshot (mirror BlockIndexLiveTailMaterializer::TipToSnapshot)
                s.found = true;
                s.id = (uint64_t)0;
                s.hash = tr.record.hash;
                s.hashPrev = tr.record.hashPrev;
                s.hashMerkleRoot = tr.record.hashMerkleRoot;
                s.height = tr.record.height;
                s.nFile = tr.record.nFile;
                s.nBlockPos = tr.record.nBlockPos;
                s.nFlags = tr.record.nFlags;
                s.nVersion = tr.record.nVersion;
                s.nTime = tr.record.nTime;
                s.nBits = tr.record.nBits;
                s.nNonce = tr.record.nNonce;
                s.nMint = tr.record.nMint;
                s.nMoneySupply = tr.record.nMoneySupply;
                s.nStakeModifier = tr.record.nStakeModifier;
                s.prevoutStake = tr.record.prevoutStake;
                s.nStakeTime = tr.record.nStakeTime;
                s.hashProof = tr.record.hashProof;
                s.fProofOfStake = (tr.record.prevoutStake.hash != uint256(0));
                s.hasParent = (tr.record.hashPrev != uint256(0));
                s.nChainTrust = tr.derived.chainTrust;
                s.nStakeModifierChecksum = tr.derived.stakeModifierChecksum;
                s.hasStakeModifierChecksum = true;
                s.nStakeModifierTime = tr.derived.stakeModifierTime;
                s.hasStakeModifierTime = tr.derived.HasStakeModifierTime();
                found = true;
            }
        }
        if (!found)
        {
            // base V2 reader (blocks <= S).
            BlockIndexV2ReadStatus st = impl_->baseReader->LookupByHash(cur, &s, error);
            if (st == BLOCK_INDEX_V2_READ_FOUND && s.found)
                found = true;
        }
        if (!found)
        {
            if (error) *error = "authoritative-live: chain walk lookup failed at " + cur.ToString();
            if (walkLookupFailed) *walkLookupFailed = true;
            return NULL;
        }
        path.push_back(s);
        // R2-STARTUP: bound the retained materialization to the live-tail
        // horizon. When the mutable tip has grown far beyond the immutable
        // base (e.g. gen-1 is minimal and the whole chain lives in the tip),
        // walking all the way back to the base floor is O(height) and builds
        // an O(path) anon vector behind a bounded hot window — the startup OOM
        // at ~2M blocks under 1 GiB. Deeper ancestry is never resident; it is
        // served BY VALUE via the V2 reader. The boundary block's own consensus
        // walks (median-time 11, difficulty, stake) reach only ~13 ancestors,
        // all within the tail, so capping at the horizon changes no outcome.
        if (path.size() >= (size_t)impl_->horizon)
        {
            reachedFloor = true;
            break;
        }
        // Reached the bounded consensus-walk floor below S.
        if (s.height <= floorHeight)
        {
            reachedFloor = true;
            break;
        }
        cur = s.hashPrev;
    }
    if (!reachedFloor)
    {
        if (error) *error = "authoritative-live: reached floor mismatch for " + hash.ToString();
        return NULL;
    }
    if (path.empty())
    {
        if (error) *error = "authoritative-live: empty chain";
        return NULL;
    }

    // Materialize objects (path[0]=requested parent .. path.back()=base floor S)
    // into the CALLER-owned containers. No operation-global store registration:
    // ownership and lifetime are exactly the caller's.
    for (size_t i = 0; i < path.size(); ++i)
    {
        uint256* own = new uint256(path[i].hash);
        CBlockIndex* obj = FullFromSnapshot(path[i], own);
        objs->push_back(obj);
        owns->push_back(own);
    }
    // Link topology: pprev -> floor, pnext -> tip. Skip links are constructed by
    // the same BuildSkip semantics ordinary resident chains use (ascending height
    // order, floor first): pskip == the node at GetSkipHeight(h) when that node
    // is materialized here, else NULL. Never a substituted pprev.
    for (size_t i = 0; i < objs->size(); ++i)
    {
        (*objs)[i]->pprev = (i + 1 < objs->size()) ? (*objs)[i + 1] : NULL;
        (*objs)[i]->pnext = (i > 0) ? (*objs)[i - 1] : NULL;
        (*objs)[i]->pskip = NULL;
    }
    for (size_t i = objs->size(); i-- > 0; )
        (*objs)[i]->BuildSkip();
    ClearError(error);
    return objs->front(); // the requested parent
}

// R2c.2/S6-repair-cycle-2 (B1): authoritative by-value spend-maturity verdict.
//
// Reproduces the EXACT legacy coinbase/coinstake maturity predicate of
// CTransaction::ConnectInputs (main.cpp:6189-6192) - "is the spent source
// block (identified by its disk position nFile/nBlockPos) among the ancestors
// of startHash at depth < maxDepth?" - WITHOUT pprev chains, CBlockIndex
// materialization, or mapBlockIndex residency:
//
//   MATURITY / ANCESTRY TRUTH != TEMPORARY pprev MATERIALIZATION DEPTH
//
// Each step resolves by value (mutable tip authority first, then the immutable
// base generation); the walk is bounded by the protocol constant maxDepth
// (the caller passes nCoinbaseMaturity), never by chain history. Semantics
// parity with the legacy walk (pinned by the boundary-matrix fixture):
//   - compares depths 0..maxDepth-1 (the legacy loop visits exactly those);
//   - reaching the genesis root before maxDepth => MATURE (the source is not
//     on this chain within the window);
//   - lookup failure, or a non-genesis record without a parent link =>
//     UNAVAILABLE (fail closed: the caller must reject, never assume mature).
// Caller must hold cs_main (same contract as MaterializeParentChainInto).
BlockIndexAuthoritativeMaturityStatus BlockIndexAuthoritativeLive::ResolveSpendMaturity(
    const uint256& startHash, unsigned int srcFile, unsigned int srcBlockPos,
    int maxDepth, int* outDepth, std::string* error) const
{
    if (outDepth) *outDepth = 0;
    if (!impl_->open || !impl_->baseReader)
    {
        if (error) *error = "authoritative-live: not open";
        return BLOCK_INDEX_MATURITY_UNAVAILABLE;
    }
    if (maxDepth <= 0)
        return BLOCK_INDEX_MATURITY_MATURE; // the legacy loop compares nothing

    uint256 cur = startHash;
    int depth = 0;
    for (int guard = 0; guard <= maxDepth; ++guard)
    {
        if (cur == uint256(0))
            return BLOCK_INDEX_MATURITY_MATURE; // reached the genesis root

        BlockIndexSnapshot s;
        bool found = false;
        // tip authority first (blocks > S).
        if (impl_->tip && impl_->tip->IsOpen())
        {
            BlockIndexTipRead tr = impl_->tip->LookupByHash(cur, error);
            if (tr.status == BLOCK_INDEX_TIP_OK)
            {
                s.found = true;
                s.hash = tr.record.hash;
                s.hashPrev = tr.record.hashPrev;
                s.height = tr.record.height;
                s.nFile = tr.record.nFile;
                s.nBlockPos = tr.record.nBlockPos;
                found = true;
            }
        }
        if (!found)
        {
            // base V2 reader (blocks <= S).
            BlockIndexV2ReadStatus st = impl_->baseReader->LookupByHash(cur, &s, error);
            if (st == BLOCK_INDEX_V2_READ_FOUND && s.found)
                found = true;
        }
        if (!found)
        {
            if (error) *error = "authoritative-live: maturity walk lookup failed at " + cur.ToString();
            return BLOCK_INDEX_MATURITY_UNAVAILABLE; // fail closed
        }

        if (s.nFile == srcFile && s.nBlockPos == srcBlockPos)
        {
            if (outDepth) *outDepth = depth;
            return BLOCK_INDEX_MATURITY_IMMATURE;
        }
        if (depth + 1 >= maxDepth)
            return BLOCK_INDEX_MATURITY_MATURE; // compared depths 0..maxDepth-1

        if (s.hashPrev == uint256(0))
        {
            if (s.height == 0)
                return BLOCK_INDEX_MATURITY_MATURE; // genesis root: source not on this chain
            if (error) *error = "authoritative-live: maturity walk parent link missing at height "
                                + std::to_string(s.height);
            return BLOCK_INDEX_MATURITY_UNAVAILABLE; // truncated/corrupt: fail closed
        }
        cur = s.hashPrev;
        ++depth;
    }
    if (error) *error = "authoritative-live: maturity walk guard exhausted for " + startHash.ToString();
    return BLOCK_INDEX_MATURITY_UNAVAILABLE; // fail closed
}

CBlockIndex* BlockIndexAuthoritativeLive::MaterializeParentChain(
    const uint256& hash, BlockIndexHotHandle* out, std::string* error) const
{
    // Operation-scoped variant: same bounded by-value walk, but the objects are
    // retained in the operation-global store and released by
    // ReleaseOperationMaterializations() at the end of the operation. Do NOT
    // evict here: a single logical block acceptance (possibly a reorg that
    // materializes a whole branch) may resolve many parents that must all stay
    // valid until the enclosing ProcessBlock completes.
    std::vector<CBlockIndex*> objs;
    std::vector<uint256*> owns;
    bool walkLookupFailed = false;
    CBlockIndex* parent = MaterializeParentChainInto(hash, &objs, &owns, error, &walkLookupFailed);
    if (!parent)
    {
        // Preserve the accepted fail-closed semantics exactly: a chain walk
        // lookup failure releases the operation-scoped store (the enclosing
        // operation fails closed); floor-mismatch/empty-chain do not.
        if (walkLookupFailed)
        {
            for (size_t i = 0; i < impl_->ownedChain_.size(); ++i)
            {
                delete impl_->ownedChain_[i];
                delete impl_->ownedChainHashes_[i];
            }
            impl_->ownedChain_.clear();
            impl_->ownedChainHashes_.clear();
        }
        return NULL;
    }
    for (size_t i = 0; i < objs.size(); ++i)
    {
        impl_->ownedChain_.push_back(objs[i]);
        impl_->ownedChainHashes_.push_back(owns[i]);
    }
    if (out)
        *out = BlockIndexHotHandle(); // ownership retained by this authority for the op
    return parent;
}

CBlockIndex* ScopedMaterializedChain::Acquire(BlockIndexAuthoritativeLive* live,
                                              const uint256& hash, std::string* error)
{
    Release();
    if (!live)
    {
        if (error) *error = "scoped-materialization: live authority unavailable";
        return NULL;
    }
    parent_ = live->MaterializeParentChainInto(hash, &owned_, &ownedHashes_, error);
    return parent_;
}

void ScopedMaterializedChain::Release()
{
    for (size_t i = 0; i < owned_.size(); ++i)
    {
        delete owned_[i];
        delete ownedHashes_[i];
    }
    owned_.clear();
    ownedHashes_.clear();
    parent_ = NULL;
}

bool BlockIndexAuthoritativeLive::AcceptActive(const BlockIndexRecord& rec,
                                                const BlockIndexDerivedEntry& derived,
                                                int activeHeight,
                                                std::string* error)
{
    if (!impl_->open || !impl_->seam)
        return SetError(error, "authoritative-live: not open");
    BlockIndexTipAppend a;
    a.record = rec;
    a.derived = derived;
    int nt = impl_->seam->AcceptActive(a, activeHeight, error);
    if (nt < 0)
        return false;
    // effective tip = nt
    ClearError(error);
    return true;
}

bool BlockIndexAuthoritativeLive::AcceptSide(const BlockIndexRecord& rec,
                                              const BlockIndexDerivedEntry& derived,
                                              std::string* error)
{
    if (!impl_->open || !impl_->seam)
        return SetError(error, "authoritative-live: not open");
    BlockIndexTipAppend a;
    a.record = rec;
    a.derived = derived;
    BlockIndexTipStatus st = impl_->seam->AcceptSide(a, error);
    return st == BLOCK_INDEX_TIP_OK;
}

bool BlockIndexAuthoritativeLive::RecordOrphan(const BlockIndexRecord& rec,
                                                const BlockIndexDerivedEntry& derived,
                                                std::string* error)
{
    if (!impl_->open || !impl_->seam)
        return SetError(error, "authoritative-live: not open");
    BlockIndexTipAppend a;
    a.record = rec;
    a.derived = derived;
    BlockIndexTipStatus st = impl_->seam->RecordOrphan(a, error);
    return st == BLOCK_INDEX_TIP_OK;
}

bool BlockIndexAuthoritativeLive::ReorgTo(
    int32_t forkHeight,
    const std::vector<BlockIndexRecord>& newBranchRecs,
    const std::vector<BlockIndexDerivedEntry>& newBranchDerived,
    const std::vector<int32_t>& newHeights,
    std::string* error)
{
    if (!impl_->open || !impl_->seam)
        return SetError(error, "authoritative-live: not open");
    if (newBranchRecs.size() != newBranchDerived.size() ||
        newBranchRecs.size() != newHeights.size())
        return SetError(error, "authoritative-live: reorg branch size mismatch");
    std::vector<BlockIndexTipAppend> branch(newBranchRecs.size());
    for (size_t i = 0; i < branch.size(); ++i)
    {
        branch[i].record = newBranchRecs[i];
        branch[i].derived = newBranchDerived[i];
    }
    BlockIndexTipStatus st = impl_->seam->ReorgTo(forkHeight, branch, newHeights, error);
    if (st != BLOCK_INDEX_TIP_OK)
        return false;
    ClearError(error);
    return true;
}

const BlockIndexTipAuthority* BlockIndexAuthoritativeLive::TipAuthority() const
{
    return impl_->tip.get();
}

BlockIndexTipAuthority* BlockIndexAuthoritativeLive::TipAuthorityMutable()
{
    return impl_->tip.get();
}

const BlockIndexLiveTail& BlockIndexAuthoritativeLive::Tail() const
{
    static BlockIndexLiveTail s_empty;
    return impl_->tail ? *impl_->tail : s_empty;
}

int BlockIndexAuthoritativeLive::Horizon() const
{
    return impl_->horizon;
}

size_t BlockIndexAuthoritativeLive::ResidentCount() const
{
    return impl_->tipWindow_.OwnedCount();
}

size_t BlockIndexAuthoritativeLive::ResidentPeak() const
{
    return impl_->tipWindowPeak_;
}

uint64_t BlockIndexAuthoritativeLive::BaseGeneration() const
{
    return impl_->baseGeneration;
}

bool BlockIndexAuthoritativeLive::IsOpen() const
{
    return impl_->open;
}

void BlockIndexAuthoritativeLive::Close()
{
    if (impl_->seam)
    {
        impl_->seam.reset();
    }
    if (impl_->tail)
    {
        impl_->tail.reset();
    }
    if (impl_->tip)
    {
        impl_->tip->Close();
        impl_->tip.reset();
    }
    impl_->baseReader = NULL;
    impl_->open = false;
    ReleaseOperationMaterializations();
}

void BlockIndexAuthoritativeLive::ReleaseOperationMaterializations()
{
    // Free the operation-scoped full-topology parents. Called at the end of each
    // logical block acceptance (authoritative mode) to keep residency bounded to
    // ONE block's worth of ancestors.
    if (!impl_) return;
    for (size_t i = 0; i < impl_->ownedChain_.size(); ++i)
    {
        delete impl_->ownedChain_[i];
        delete impl_->ownedChainHashes_[i];
    }
    impl_->ownedChain_.clear();
    impl_->ownedChainHashes_.clear();
}