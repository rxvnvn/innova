// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.

#ifndef INNOVA_BLOCKINDEX_LIVE_TAIL_H
#define INNOVA_BLOCKINDEX_LIVE_TAIL_H

#include "blockindex_hot_owner.h"
#include "blockindex_tip.h"
#include "blockindex_accessor.h"

#include <cstdint>
#include <string>
#include <vector>

class BlockIndexV2Reader;

// P2 — bounded HotOwner live-tail.
//
// Objective (2a-approved): bounded RESIDENCY independent of total historical N.
// The proven legacy consensus engine runs on a bounded hot CBlockIndex tail
// around the current/finality/reorg horizon, while historical authority remains
// V2/by-value. This is a RESIDENCY/CACHE horizon ONLY — NEVER a consensus,
// validation, or maximum-reorg-depth limit. A deep valid reorg that requires
// ancestry older than the horizon must materialize it via V2 and run, never be
// rejected for exceeding N.
//
// Composite materializer: a logical block hash resolves to by-value snapshot
// from EITHER the dedicated mutable tip authority (blocks above base tip S,
// RecordId offset past base recordCount) OR the immutable base V2 reader
// (blocks <= S). Both are by-value; no resident CBlockIndex graph is needed.
//
// The live-tail owns a BlockIndexHotOwner whose residency is the bounded window
// [tip - livetail + 1, tip] (plus transient pins during consensus operations and
// permanent anchors). Entries below the horizon become eviction-eligible; they
// remain fully re-materializable on demand via the composite materializer for a
// deep reorg. The tail is therefore a cache, not an authority.

class BlockIndexLiveTailMaterializer : public BlockIndexHotMaterializer
{
public:
    struct Source
    {
        // Base immutable V2 authority (lookup by hash) + derived store.
        // Implemented as lightweight callbacks bound to the caller's reader.
        // owner must outlive the materializer.
    };

    // Construct from the base V2 reader and the mutable tip authority.
    // Both must remain open and outlive this materializer. tip may be the
    // empty base-only tip (no blockindex_tip yet).
    //   baseLookup: callback(hash) -> snapshot (could be NULL => none)
    //   tip:        the open BlockIndexTipAuthority (may be empty)
    BlockIndexLiveTailMaterializer(
        const BlockIndexV2Reader* baseReader,
        const BlockIndexTipAuthority* tip);
    virtual ~BlockIndexLiveTailMaterializer() {}

    // Resolve logical hash -> snapshot. Order: tip first (above S), then base.
    virtual BlockIndexHotStatus Materialize(const BlockIndexLogicalId& id,
                                            BlockIndexHotMaterialized* out) const override;

private:
    const BlockIndexV2Reader* baseReader_;
    const BlockIndexTipAuthority* tip_;

    // Build a BlockIndexSnapshot from a tip-read record + derived entry.
    bool TipToSnapshot(const BlockIndexTipRead& tr, BlockIndexSnapshot* out) const;
    // Build a snapshot from the base V2 reader.
    BlockIndexHotStatus BaseToSnapshot(const BlockIndexLogicalId& id,
                                       BlockIndexHotMaterialized* out) const;
};

// Bounded residency live-tail wrapper around a HotOwner, applying the config
// horizon (residency-only) as eviction policy.
class BlockIndexLiveTail
{
public:
    BlockIndexLiveTail();
    ~BlockIndexLiveTail();

    // Open/config: bind the composite materializer (non-owning refs).
    void SetSources(const BlockIndexV2Reader* baseReader,
                    const BlockIndexTipAuthority* tip);
    void SetHorizon(int n);         // residency horizon (residency-only)
    int  Horizon() const;
    void SetCurrentGeneration(uint64_t gen);

    // Pin a logical block into the live tail (materialize + lifetime claim).
    // Used by consensus operations that need a resident CBlockIndex*.
    BlockIndexHotStatus Pin(const BlockIndexLogicalId& id, BlockIndexHotHandle* out);
    BlockIndexHotStatus LookupResident(const BlockIndexLogicalId& id,
                                       BlockIndexHotHandle* out);
    void ReleasePin(const uint256& hash);
    bool IsResident(const BlockIndexLogicalId& id) const;

    // Evict entries below the horizon (those with height < tip - n + 1) unless
    // pinned/anchor. Returns count evicted. This is a CACHE policy, never a
    // consensus/reorg bound.
    size_t TrimToHorizon();

    // Pin the permanent anchors (best tip + genesis) so they never evict.
    void PinPermanent(const BlockIndexLogicalId& id);

    // Residency counters (for Gate E: bounded independent of historical N).
    size_t ResidentCount() const;
    size_t PinCount() const;
    BlockIndexHotMetrics Metrics() const;

private:
    BlockIndexLiveTailMaterializer* mat_;
    BlockIndexHotOwner owner_;
    bool hasMat_;
    int horizon_;
    const BlockIndexTipAuthority* tipRef_;   // non-owning; for horizon trim
};

#endif // INNOVA_BLOCKINDEX_LIVE_TAIL_H