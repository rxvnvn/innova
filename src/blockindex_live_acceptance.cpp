// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.

#include "blockindex_live_acceptance.h"

namespace {
static bool SetError(std::string* error, const std::string& message)
{
    if (error)
        *error = message;
    return false;
}
} // namespace

BlockIndexLiveAcceptance::BlockIndexLiveAcceptance()
    : baseKnown_(NULL), baseUd_(NULL), tip_(NULL), tail_(NULL)
{
}

BlockIndexLiveAcceptance::~BlockIndexLiveAcceptance()
{
}

void BlockIndexLiveAcceptance::SetSources(BaseKnownFn baseKnown, void* baseUd,
                                          BlockIndexTipAuthority* tip,
                                          BlockIndexLiveTail* tail)
{
    baseKnown_ = baseKnown;
    baseUd_ = baseUd;
    tip_ = tip;
    tail_ = tail;
}

BlockIndexLiveAcceptance::ParentStatus BlockIndexLiveAcceptance::ResolveParent(
    const uint256& parentHash, int* parentHeight, std::string* error) const
{
    if (parentHeight)
        *parentHeight = -1;
    // tip authority first (blocks > S / tip namespace).
    if (tip_ && tip_->IsOpen())
    {
        BlockIndexTipRead tr = tip_->LookupByHash(parentHash, error);
        if (tr.status == BLOCK_INDEX_TIP_OK)
        {
            if (parentHeight)
                *parentHeight = tr.height;
            return PARENT_KNOWN;
        }
        if (tr.status != BLOCK_INDEX_TIP_NOT_FOUND &&
            tr.status != BLOCK_INDEX_TIP_IO_ERROR)
            return PARENT_ERROR;
    }
    // base V2 authority (blocks <= S).
    if (baseKnown_ && baseKnown_(parentHash, baseUd_))
    {
        // height unknown cheaply here; caller only needs KNOWN vs UNKNOWN for
        // the orphan gate. Dense check uses the tip's active chain for >S.
        if (parentHeight)
            *parentHeight = -1; // base; caller uses exact height from V2 if needed
        return PARENT_KNOWN;
    }
    return PARENT_UNKNOWN;
}

bool BlockIndexLiveAcceptance::CanAcceptDense(const uint256& parentHash,
                                              int height,
                                              std::string* error) const
{
    if (!tip_ || !tip_->IsOpen())
        return false;
    // The next active height must be exactly the tip authority's active tip + 1.
    const int curTip = tip_->TipHeight();
    if (height != curTip + 1)
        return false;
    // Parent must be known (base tip anchor or an existing tip active member).
    ParentStatus ps = ResolveParent(parentHash, NULL, error);
    return ps == PARENT_KNOWN;
}

BlockIndexTipStatus BlockIndexLiveAcceptance::RecordOrphan(
    const BlockIndexTipAppend& blk, std::string* error)
{
    if (!tip_ || !tip_->IsOpen())
    {
        SetError(error, "no tip authority");
        return BLOCK_INDEX_TIP_IO_ERROR;
    }
    // side (non-active) append so it is persisted but does not advance tip.
    return tip_->Append(blk, -1, error);
}

int BlockIndexLiveAcceptance::AcceptActive(const BlockIndexTipAppend& blk,
                                           int activeHeight,
                                           std::string* error)
{
    if (!tip_ || !tip_->IsOpen())
    {
        SetError(error, "no tip authority");
        return -1;
    }
    BlockIndexTipStatus st = tip_->Append(blk, activeHeight, error);
    if (st != BLOCK_INDEX_TIP_OK)
        return -1;
    // Refresh live-tail residency for the newly accepted tip (bounded).
    if (tail_)
    {
        BlockIndexLogicalId id(blk.record.hash);
        BlockIndexHotHandle h;
        tail_->Pin(id, &h); // materialize into tail; released -> evictable
        h.Reset();
        tail_->TrimToHorizon(); // residency-only cache policy
    }
    return tip_->TipHeight();
}

BlockIndexTipStatus BlockIndexLiveAcceptance::AcceptSide(
    const BlockIndexTipAppend& blk, std::string* error)
{
    if (!tip_ || !tip_->IsOpen())
    {
        SetError(error, "no tip authority");
        return BLOCK_INDEX_TIP_IO_ERROR;
    }
    BlockIndexTipStatus st = tip_->Append(blk, -1, error);
    if (st != BLOCK_INDEX_TIP_OK)
        return st;
    if (tail_)
    {
        BlockIndexLogicalId id(blk.record.hash);
        BlockIndexHotHandle h;
        tail_->Pin(id, &h);
        h.Reset();
    }
    return BLOCK_INDEX_TIP_OK;
}

const BlockIndexTipAuthority* BlockIndexLiveAcceptance::TipAuthority() const
{
    return tip_;
}

BlockIndexTipStatus BlockIndexLiveAcceptance::ReorgTo(
    int32_t forkHeight,
    const std::vector<BlockIndexTipAppend>& newBranch,
    const std::vector<int32_t>& newHeights,
    std::string* error)
{
    if (!tip_ || !tip_->IsOpen())
    {
        SetError(error, "no tip authority");
        return BLOCK_INDEX_TIP_IO_ERROR;
    }
    if (newBranch.size() != newHeights.size())
    {
        SetError(error, "reorg branch size mismatch");
        return BLOCK_INDEX_TIP_CORRUPT;
    }

    // REORG ACTIVE CHAIN TO THE RECONNECT BRANCH (fork+1..newTip). This works
    // even when branch records were already accepted as SIDE during their own
    // AddToBlockIndex: ReorgActiveTo PROMOTES existing records to active instead
    // of skipping them (idempotent Append cannot reclassify a side record), and
    // appends any branch record not yet present. Fail closed on any inconsistency;
    // on partial application the tip is left at forkHeight, recoverable by rerun.
    return tip_->ReorgActiveTo(forkHeight, newBranch, newHeights, error);
}