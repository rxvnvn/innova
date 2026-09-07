// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef INNOVA_BLOCKINDEX_AUTHORITATIVE_LIVE_H
#define INNOVA_BLOCKINDEX_AUTHORITATIVE_LIVE_H

#include "blockindex_tip.h"
#include "blockindex_live_tail.h"
#include "blockindex_live_acceptance.h"
#include "blockindex_v2_reader.h"

#include <cstdint>
#include <memory>
#include <set>
#include <string>

// G1 — production live-authority seam for BY_VALUE_AUTHORITATIVE mode.
//
// Closes the Window-1 Finding-2 gap in the REAL production block path:
//   ProcessBlock -> AcceptBlock -> (parent resolution) -> AddToBlockIndex -> ...
// currently resolves every parent via mapBlockIndex.find(hashPrevBlock) and
// rejects ABREJECT_PREV_NOT_FOUND when the parent is authoritative-but-not-
// resident. This module retains the mutable authority the live path needs so
// that above-base blocks S+1..  can cross the authoritative boundary.
//
//   AUTHORITY != MATERIALIZATION != RESIDENCY LIFETIME  (A.10.1d contract).
//   - Authority:  base immutable V2 generation (the reader) + mutable
//                 blockindex_tip (BlockIndexTipAuthority).
//   - Materialization: the composite BlockIndexLiveTailMaterializer resolves a
//                 logical hash by value from tip-then-base, no resident graph.
//   - Residency: bounded BlockIndexLiveTail (Horizon = -blockindexlivetail);
//                 never a consensus/validation/reorg-depth limit.
//
// This is a CACHE/AUTHORITY seam, NOT a consensus change: it never alters how
// a block is validated. It supplies:
//   1. ResolveParent(hash): by-value answer "is the parent known, and where"
//      (base V2 or mutable tip), so the live path does NOT require the parent
//      to be resident in mapBlockIndex.
//   2. MaterializeParent(hash): obtain a bounded resident CBlockIndex for the
//      parent (pinned for the validation lifetime) via the composite tail, so
//      the legacy consensus engine can run unchanged on it.
//   3. Persist accepted block(s) into the mutable tip authority (and keep the
//      live-tail residency bounded), so post-S authority is permanent.
//   4. Orphan recording for a block whose parent resolves by value but is at a
//      non-current height (or unknown), so a later join/reorg is possible.
//
// The retained object lives in g_authoritativeContext (blockindex_authoritative_
// startup) for the daemon lifetime and is bound to the SAME single process-open
// base reader (A.10.1q) + the mutable tip under <v2Root>/blockindex_tip.
//
// blockindexlivetail is a RESIDENCY HORIZON ONLY. A deep valid reorg whose
// ancestry is older than the horizon MUST be served by materializing the needed
// historical blocks from V2 on demand and running them; it is never rejected
// for exceeding N. This module never imposes a reorg-depth cap.

class BlockIndexAuthoritativeLive
{
public:
    BlockIndexAuthoritativeLive();
    ~BlockIndexAuthoritativeLive();

    BlockIndexAuthoritativeLive(const BlockIndexAuthoritativeLive&) = delete;
    BlockIndexAuthoritativeLive& operator=(const BlockIndexAuthoritativeLive&) = delete;

    // Open/reuse the mutable tip under <v2Root>/blockindex_tip, bind the
    // composite tail to the given base reader + tip, set the residency horizon,
    // and arm the acceptance seam. baseReader must be open and MUST outlive this
    // object. If the tip store does not exist it is created (empty, anchored to
    // the base generation). Fails closed on base/tip mismatch or corrupt tip.
    bool Open(const std::string& v2Root,
              const BlockIndexV2Reader* baseReader,
              int livetailHorizon,
              std::string* error);

    // ---- parent resolution (by value) ----
    // Resolve a parent hash across base V2 + mutable tip. Returns true and sets
    // parentHeight (>=0; -1 if only KNOWN, not height-resolved cheaply) when the
    // parent is known by-value; false when unknown (genuine orphan) or error.
    bool ResolveParent(const uint256& parentHash, int* parentHeight,
                       std::string* error) const;

    // Materialize a bounded resident CBlockIndex for a logical hash via the
    // composite live tail (tip-then-base). Returns a pinned handle; the returned
    // CBlockIndex* is valid while the handle is alive. Sparse-hot topology is
    // served by the tail; caller must hold the handle for the whole operation.
    // BlockIndexHotStatus is returned so a caller can fail closed on authority or
    // materialization failure (never silently fall back to legacy residency).
    BlockIndexHotStatus Materialize(const uint256& hash,
                                    BlockIndexHotHandle* out) const;

    // Materialize a FULL-TOPOLOGY CBlockIndex parent (pprev/pnext/pskip linked)
    // for `hash`, pinned for the operation lifetime. The chain is materialized by
    // value down to the base boundary (bounded by the live-tail horizon; a deep
    // ancestry beyond the horizon is served by the by-value walk, never a reorg
    // cap). Returns the parent CBlockIndex* (valid while `out` is alive) or NULL +
    // error on authority/materialization failure (fail-closed).
    CBlockIndex* MaterializeParentChain(const uint256& hash,
                                        BlockIndexHotHandle* out,
                                        std::string* error) const;

    // ---- live acceptance / persistence ----
    // Persist an accepted ACTIVE block (already consensus-validated by the live
    // engine) into the mutable tip + refresh the bounded live tail. activeHeight
    // is the block's global active height (must equal tip height + 1).
    bool AcceptActive(const BlockIndexRecord& rec,
                      const BlockIndexDerivedEntry& derived,
                      int activeHeight, std::string* error);

    // Persist an accepted SIDE (non-active) block (validated but not best chain).
    bool AcceptSide(const BlockIndexRecord& rec,
                    const BlockIndexDerivedEntry& derived,
                    std::string* error);

    // Record an accepted block whose parent is unknown (orphan) into the tip as a
    // side record (persisted for a later join), mirroring legacy mapOrphanBlocks.
    bool RecordOrphan(const BlockIndexRecord& rec,
                      const BlockIndexDerivedEntry& derived,
                      std::string* error);

    // Reorg the ACTIVE chain to forkHeight (disconnect the branch above it), then
    // connect newBranch (in order, with global active heights) as the active chain.
    // Side records are retained; fail-closed on any partial application (the tip is
    // left at the truncate point, recoverable by re-running). The caller has already
    // validated + chosen the new branch as best.
    bool ReorgTo(int32_t forkHeight,
                 const std::vector<BlockIndexRecord>& newBranchRecs,
                 const std::vector<BlockIndexDerivedEntry>& newBranchDerived,
                 const std::vector<int32_t>& newHeights,
                 std::string* error);

    // ---- introspection ----
    const BlockIndexTipAuthority* TipAuthority() const;
    BlockIndexTipAuthority* TipAuthorityMutable();
    const BlockIndexLiveTail& Tail() const;
    int Horizon() const;
    uint64_t BaseGeneration() const;
    bool IsOpen() const;

    void Close();

public:
    // PIMPL state; public so the base-known callback (void*) can reach internals.
    class Impl;

private:
    Impl* impl_;
    // baseKnown callback state owner (Impl is the ud payload).
    friend bool BlockIndexAuthoritativeLiveBaseKnown(const uint256& hash, void* ud);
};

// Production accessor (NULL when NOT in authoritative mode). The object is
// retained process-lifetime by the authoritative startup context and bound to
// the single process-open base reader. Callers must NOT free it.
BlockIndexAuthoritativeLive* GetAuthoritativeLiveAuthority();

#endif // INNOVA_BLOCKINDEX_AUTHORITATIVE_LIVE_H