// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.

#ifndef INNOVA_BLOCKINDEX_LIVE_ACCEPTANCE_H
#define INNOVA_BLOCKINDEX_LIVE_ACCEPTANCE_H

#include "blockindex_tip.h"
#include "blockindex_live_tail.h"
#include "blockindex_accessor.h"

#include <cstdint>
#include <string>
#include <vector>

// P3 — live post-S acceptance seam.
//
// Closes the Window-1 blocker: in BY_VALUE_AUTHORITATIVE mode mapBlockIndex is
// empty, so the legacy live path's parent resolution (mapBlockIndex.find) would
// reject the first post-S block as "prev not found". This seam provides the
// by-value parent resolution + persistence the live path needs:
//
//   - ResolveParent(hash): answer "is this block known, and where" across
//     base V2 (<= S) + tip authority (> S), WITHOUT any resident graph.
//   - CanAcceptDense(childPrev, height): a block whose parent resolves and
//     whose height is exactly parent.height+1 is dense-acceptable.
//   - AcceptBlock(record, derived): persist an accepted/validated block into
//     the tip authority and refresh the live-tail residency.
//
// Transformative property (the Window-1 proof's converse): the S+1 child of the
// generation-tip anchor IS acceptable because its parent (the anchor) resolves
// by-value from the base. Subsequent S+2.. resolve via the tip as they append.
//
// This is a CACHE/AUTHORITY seam, not a consensus change: it never alters how a
// block is validated (PoW/PoS/DAG/checkpoints all stay in the legacy live
// path). It only supplies the parent resolution + post-S persistence that the
// live path was missing in authoritative mode.
//
// References:
//   - tip_      : the persistent mutable tip authority (P1)
//   - tail_     : the bounded live-tail (P2) that provides resident CBlockIndex
//                 to the consensus engine for the accepted window
//   - baseKnown : callback -> whether a hash is known in the base V2 generation

class BlockIndexLiveAcceptance
{
public:
    // baseKnown: returns true if hash is in the immutable base generation (<= S).
    // Must be cheap/by-value. Non-owning; must outlive this seam.
    typedef bool (*BaseKnownFn)(const uint256& hash, void* ud);

    BlockIndexLiveAcceptance();
    ~BlockIndexLiveAcceptance();

    void SetSources(BaseKnownFn baseKnown, void* baseUd,
                    BlockIndexTipAuthority* tip,
                    BlockIndexLiveTail* tail);

    // Enum result for parent resolution.
    enum ParentStatus
    {
        PARENT_KNOWN = 0,        // parent found (base or tip)
        PARENT_UNKNOWN = 1,      // parent not found anywhere (orphan for now)
        PARENT_ERROR = 2,        // storage error
    };

    // Resolve a parent hash across base+tip by-value.
    ParentStatus ResolveParent(const uint256& parentHash, int* parentHeight,
                               std::string* error) const;

    // Can a block with hashPrev=parent and given height be accepted as the next
    // dense active block? Requires: parent KNOWN, height == parentHeight+1.
    bool CanAcceptDense(const uint256& parentHash, int height,
                        std::string* error) const;

    // A block whose parent resolved as UNKNOWN is an orphan: accept it into the
    // tip authority as a SIDE (non-active) record so it is persisted for a later
    // reorg/join, mirroring legacy mapOrphanBlocks. Returns OK; does not advance
    // the active tip.
    BlockIndexTipStatus RecordOrphan(const BlockIndexTipAppend& blk,
                                     std::string* error);

    // Persist an accepted ACTIVE block into the tip authority (advance tip) and
    // refresh live-tail residency. The caller (live path) has ALREADY validated
    // the block (PoW/PoS/DAG/checkpoints). Returns the new tip height.
    int AcceptActive(const BlockIndexTipAppend& blk, int activeHeight,
                     std::string* error);

    // Persist an accepted SIDE (non-active) block (validated, but not best
    // chain). Does not advance active tip; recorded for reorg/join.
    BlockIndexTipStatus AcceptSide(const BlockIndexTipAppend& blk,
                                   std::string* error);

    // Causal test assist: expose the underlying tip authority.
    const BlockIndexTipAuthority* TipAuthority() const;

    // P4 — Reorg in the bounded tip authority.
    // Reorg the ACTIVE chain to a fork height (disconnect the branch above it),
    // then connect the provided new branch's blocks as the active chain. Side
    // blocks (the old branch, and any non-active) remain persisted as records
    // (like legacy mapBlockIndex keeps them) so a later reorg can re-join them.
    //
    // newBranch: the blocks of the new active chain from forkHeight+1 upward,
    //            in order, with their active global heights in newHeights. The
    //            first element's parent must be the active block at forkHeight.
    // This is a CACHE/AUTHORITY reorg representation, NOT a consensus change: the
    // caller (live path) has already validated the new branch and chosen it as
    // best. The seam only persists the new active topology by-value.
    //
    // Returns BLOCK_INDEX_TIP_OK on success; tip authority reflects the new tip.
    // On failure the tip is left at the truncate point (fail-closed; no partial
    // new branch applied), recoverable by re-running.
    BlockIndexTipStatus ReorgTo(int32_t forkHeight,
                                const std::vector<BlockIndexTipAppend>& newBranch,
                                const std::vector<int32_t>& newHeights,
                                std::string* error);

private:
    BaseKnownFn baseKnown_;
    void* baseUd_;
    BlockIndexTipAuthority* tip_;
    BlockIndexLiveTail* tail_;
};

#endif // INNOVA_BLOCKINDEX_LIVE_ACCEPTANCE_H