// Copyright (c) 2019-2026 The Innovana developers
// Distributed under the MIT/X11 software license.

#include "blockindex_live_tail.h"
#include "blockindex_v2_reader.h"

#include <algorithm>

// ---------------------------------------------------------------------------
// Composite materializer: a logical hash resolves by-value from the tip
// authority (blocks above base tip S) or the base V2 reader (blocks <= S).
// No resident CBlockIndex graph is consulted or constructed.
// ---------------------------------------------------------------------------

BlockIndexLiveTailMaterializer::BlockIndexLiveTailMaterializer(
    const BlockIndexV2Reader* baseReader,
    const BlockIndexTipAuthority* tip)
    : baseReader_(baseReader), tip_(tip)
{
}

bool BlockIndexLiveTailMaterializer::TipToSnapshot(const BlockIndexTipRead& tr,
                                                   BlockIndexSnapshot* out) const
{
    const BlockIndexRecord& r = tr.record;
    const BlockIndexDerivedEntry& d = tr.derived;
    // Reuse the same field mapping as BlockIndexV2Reader::SnapshotFromRecord.
    BlockIndexSnapshot s;
    s.found = true;
    s.id = (tr.status == BLOCK_INDEX_TIP_OK) ? ((uint64_t)0) : BLOCK_INDEX_ID_INVALID;
    s.hash = r.hash;
    s.hashPrev = r.hashPrev;
    s.hashMerkleRoot = r.hashMerkleRoot;
    s.height = r.height;
    s.nFile = r.nFile;
    s.nBlockPos = r.nBlockPos;
    s.nFlags = r.nFlags;
    s.nVersion = r.nVersion;
    s.nTime = r.nTime;
    s.nBits = r.nBits;
    s.nNonce = r.nNonce;
    s.nMint = r.nMint;
    s.nMoneySupply = r.nMoneySupply;
    s.nStakeModifier = r.nStakeModifier;
    s.prevoutStake = r.prevoutStake;
    s.nStakeTime = r.nStakeTime;
    s.hashProof = r.hashProof;
    s.fProofOfStake = (r.prevoutStake.hash != uint256(0));
    s.fInMainChain = tr.active;
    s.hasParent = (r.hashPrev != uint256(0));
    // derived
    s.nChainTrust = d.chainTrust;
    s.nStakeModifierChecksum = d.stakeModifierChecksum;
    s.hasStakeModifierChecksum = true;
    s.nStakeModifierTime = d.stakeModifierTime;
    s.hasStakeModifierTime = d.HasStakeModifierTime();
    *out = s;
    return true;
}

BlockIndexHotStatus BlockIndexLiveTailMaterializer::BaseToSnapshot(
    const BlockIndexLogicalId& id, BlockIndexHotMaterialized* out) const
{
    if (!baseReader_ || !baseReader_->IsOpen())
        return BlockIndexHotStatus::MATERIALIZATION_UNAVAILABLE;
    BlockIndexSnapshot snap;
    std::string err;
    BlockIndexV2ReadStatus st = baseReader_->LookupByHash(id.GetHash(), &snap, &err);
    if (st == BLOCK_INDEX_V2_READ_NOT_FOUND)
        return BlockIndexHotStatus::AUTHORITY_MISSING;
    if (st != BLOCK_INDEX_V2_READ_FOUND)
        return BlockIndexHotStatus::CORRUPT_METADATA;
    // derived (chain trust etc.) from the base derived store is carried in the
    // snapshot only when the reader fills it; V2 base reader already populates
    // trusted fields for AUTHORITATIVE generations.
    out->found = true;
    out->snapshot = snap;
    return BlockIndexHotStatus::OK;
}

BlockIndexHotStatus BlockIndexLiveTailMaterializer::Materialize(
    const BlockIndexLogicalId& id, BlockIndexHotMaterialized* out) const
{
    out->found = false;
    // 1. tip authority first (blocks above S / any tip-namespace hash).
    if (tip_ && tip_->IsOpen())
    {
        BlockIndexTipRead tr = tip_->LookupByHash(id.GetHash(), NULL);
        if (tr.status == BLOCK_INDEX_TIP_OK)
        {
            BlockIndexSnapshot snap;
            if (!TipToSnapshot(tr, &snap))
                return BlockIndexHotStatus::CORRUPT_METADATA;
            out->found = true;
            out->snapshot = snap;
            out->generation = tip_->BaseGeneration();
            out->hasBlockSize = tr.derived.HasBlockSize();
            out->blockSize = tr.derived.nSize;
            return BlockIndexHotStatus::OK;
        }
        // if the hash is not in the tip namespace, fall through to base.
        if (tr.status != BLOCK_INDEX_TIP_NOT_FOUND &&
            tr.status != BLOCK_INDEX_TIP_IO_ERROR)
            return BlockIndexHotStatus::CORRUPT_METADATA;
    }
    // 2. base V2 authority (blocks <= S).
    return BaseToSnapshot(id, out);
}

// ---------------------------------------------------------------------------
// Bounded live-tail wrapper.
// ---------------------------------------------------------------------------

BlockIndexLiveTail::BlockIndexLiveTail()
    : mat_(NULL), hasMat_(false), horizon_(2048), tipRef_(NULL)
{
}

BlockIndexLiveTail::~BlockIndexLiveTail()
{
    delete mat_;
}

void BlockIndexLiveTail::SetSources(const BlockIndexV2Reader* baseReader,
                                    const BlockIndexTipAuthority* tip)
{
    delete mat_;
    mat_ = new BlockIndexLiveTailMaterializer(baseReader, tip);
    owner_.SetMaterializer(mat_);
    hasMat_ = true;
    tipRef_ = tip;
}

void BlockIndexLiveTail::SetHorizon(int n)
{
    horizon_ = (n > 0) ? n : 2048;
}

int BlockIndexLiveTail::Horizon() const
{
    return horizon_;
}

void BlockIndexLiveTail::SetCurrentGeneration(uint64_t gen)
{
    owner_.SetCurrentGeneration(gen);
}

BlockIndexHotStatus BlockIndexLiveTail::Pin(const BlockIndexLogicalId& id,
                                            BlockIndexHotHandle* out)
{
    if (!hasMat_)
        return BlockIndexHotStatus::MATERIALIZATION_UNAVAILABLE;
    return owner_.Pin(id, out);
}

BlockIndexHotStatus BlockIndexLiveTail::LookupResident(const BlockIndexLogicalId& id,
                                                       BlockIndexHotHandle* out)
{
    return owner_.LookupResident(id, out);
}

void BlockIndexLiveTail::ReleasePin(const uint256& hash)
{
    owner_.ReleasePin(hash);
}

bool BlockIndexLiveTail::IsResident(const BlockIndexLogicalId& id) const
{
    return owner_.IsResident(id);
}

void BlockIndexLiveTail::PinPermanent(const BlockIndexLogicalId& id)
{
    owner_.PinPermanent(id);
}

size_t BlockIndexLiveTail::ResidentCount() const
{
    return owner_.ResidentCount();
}

size_t BlockIndexLiveTail::PinCount() const
{
    return owner_.PinCount();
}

BlockIndexHotMetrics BlockIndexLiveTail::Metrics() const
{
    return owner_.Metrics();
}

size_t BlockIndexLiveTail::TrimToHorizon()
{
    // Residency-only cache policy. Evict resident blocks BELOW the horizon
    // (tip height - n + 1) unless pinned/anchor. This NEVER rejects a reorg or
    // validation; evicted entries remain fully re-materializable via the
    // composite materializer (deep-reorg re-materialization path).
    if (!hasMat_ || !tipRef_)
        return 0;
    // Determine current tip height from the tip authority.
    BlockIndexTipRead tip = tipRef_->GetTip();
    if (tip.status != BLOCK_INDEX_TIP_OK)
        return 0;
    const int tipHeight = tip.height;
    const int floor = tipHeight - horizon_ + 1;
    size_t evicted = 0;
    std::vector<BlockIndexLogicalId> candidates = owner_.EvictEligible();
    for (size_t i = 0; i < candidates.size(); ++i)
    {
        // peek height without pinning: query the materializer snapshot.
        BlockIndexHotMaterialized m;
        BlockIndexHotStatus st = mat_->Materialize(candidates[i], &m);
        if (st != BlockIndexHotStatus::OK || !m.found)
            continue;
        const int h = m.snapshot.height;
        if (h < floor)
        {
            BlockIndexHotStatus e = owner_.EvictResident(candidates[i]);
            if (e == BlockIndexHotStatus::OK)
                ++evicted;
        }
    }
    return evicted;
}