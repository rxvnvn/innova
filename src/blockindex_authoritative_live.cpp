// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "blockindex_authoritative_live.h"

#include "blockindex_startup_bootstrap.h"

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
    std::map<uint256, CBlockIndex*> fullResident_;
    std::map<uint256, uint256*>     fullResidentHash_;
    std::set<uint256>               fullResidentAnchor_;

    Impl()
        : baseReader(NULL), horizon(2048), baseGeneration(0),
          open(false), baseTipHeight(-1), baseTipHash(0)
    {
    }

    ~Impl()
    {
        for (size_t i = 0; i < ownedChain_.size(); ++i)
        {
            delete ownedChain_[i];
            delete ownedChainHashes_[i];
        }
        for (std::map<uint256, CBlockIndex*>::iterator it = fullResident_.begin();
             it != fullResident_.end(); ++it)
        {
            delete it->second;
        }
        for (std::map<uint256, uint256*>::iterator it = fullResidentHash_.begin();
             it != fullResidentHash_.end(); ++it)
        {
            delete it->second;
        }
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
    if (s.fProofOfStake)
        p->nFlags |= CBlockIndex::BLOCK_PROOF_OF_STAKE;
    return p;
}
} // namespace

CBlockIndex* BlockIndexAuthoritativeLive::ResolveAndRetainFullParent(
    const uint256& parentHash, std::string* error)
{
    if (!impl_->open || !impl_->baseReader)
    {
        if (error) *error = "authoritative-live: not open";
        return NULL;
    }
    // If already persisted-resident, return the existing object (idempotent).
    {
        std::map<uint256, CBlockIndex*>::iterator it = impl_->fullResident_.find(parentHash);
        if (it != impl_->fullResident_.end())
            return it->second;
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
        if (impl_->fullResident_.count(h))
            continue; // already resident; kept
        uint256* own = new uint256(h);
        CBlockIndex* obj = FullFromSnapshot(path[i], own);
        impl_->fullResident_[h] = obj;
        impl_->fullResidentHash_[h] = own;
        // anchor the base tip (boundary) so it is never evicted
        if (h == impl_->baseTipHash)
            impl_->fullResidentAnchor_.insert(h);
    }
    // Link topology: pprev -> floor, pnext -> tip within the resolved chain AND
    // against already-persistent ancestors so the whole pointer graph is valid.
    // Build height-ordered list path[0] (highest) .. path.back() (floor).
    for (size_t i = 0; i < path.size(); ++i)
    {
        CBlockIndex* o = impl_->fullResident_[path[i].hash];
        const uint256& hPrev = path[i].hashPrev;
        CBlockIndex* prev = (i + 1 < path.size()) ? impl_->fullResident_[path[i + 1].hash] : NULL;
        if (!prev && hPrev != uint256(0))
        {
            std::map<uint256, CBlockIndex*>::iterator p = impl_->fullResident_.find(hPrev);
            if (p != impl_->fullResident_.end())
                prev = p->second;
        }
        o->pprev = prev;
        o->pnext = (i > 0) ? impl_->fullResident_[path[i - 1].hash] : NULL;
        o->pskip = o->pprev;
    }
    // nChainTrust is preserved from the snapshot: FullFromSnapshot sets
    // p->nChainTrust = s.nChainTrust, and the reader now fills s.nChainTrust
    // from the authoritative derived.dat (per-record cumulative trust). No
    // recomputation is needed (and re-accumulating would wrongly drop the
    // heritage of a boundary object that floors at itself). Anchor the base tip.
    for (size_t i = 0; i < path.size(); ++i)
        if (path[i].hash == impl_->baseTipHash)
            impl_->fullResidentAnchor_.insert(path[i].hash);
    return impl_->fullResident_[parentHash];
}

CBlockIndex* BlockIndexAuthoritativeLive::MaterializeParentChain(
    const uint256& hash, BlockIndexHotHandle* out, std::string* error) const
{
    if (!impl_->open || !impl_->baseReader)
    {
        if (error) *error = "authoritative-live: not open";
        return NULL;
    }
    // Do NOT evict here: a single logical block acceptance (possibly a reorg that
    // materializes a whole branch) may resolve many parents that must all stay
    // valid until the enclosing ProcessBlock completes. Residency is bounded by
    // releasing at the end of the operation (ReleaseOperationMaterializations).

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
            // roll back partial owned chain
            for (size_t i = 0; i < impl_->ownedChain_.size(); ++i)
            {
                delete impl_->ownedChain_[i];
                delete impl_->ownedChainHashes_[i];
            }
            impl_->ownedChain_.clear();
            impl_->ownedChainHashes_.clear();
            return NULL;
        }
        path.push_back(s);
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

    // Materialize objects (path[0]=requested parent .. path.back()=base floor S).
    std::vector<CBlockIndex*> objs(path.size(), NULL);
    std::vector<uint256*> owns(path.size(), NULL);
    for (size_t i = 0; i < path.size(); ++i)
    {
        owns[i] = new uint256(path[i].hash);
        objs[i] = FullFromSnapshot(path[i], owns[i]);
        impl_->ownedChain_.push_back(objs[i]);
        impl_->ownedChainHashes_.push_back(owns[i]);
    }
    // Link topology: pprev -> floor, pnext -> tip, pskip -> pprev (conservative).
    for (size_t i = 0; i < objs.size(); ++i)
    {
        objs[i]->pprev = (i + 1 < objs.size()) ? objs[i + 1] : NULL;
        objs[i]->pnext = (i > 0) ? objs[i - 1] : NULL;
        objs[i]->pskip = objs[i]->pprev;
    }

    CBlockIndex* parent = objs.front(); // the requested parent
    if (out)
        *out = BlockIndexHotHandle(); // ownership retained by this authority for the op
    ClearError(error);
    return parent;
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