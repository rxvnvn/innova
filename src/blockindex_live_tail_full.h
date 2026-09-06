// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef INNOVA_BLOCKINDEX_LIVE_TAIL_FULL_H
#define INNOVA_BLOCKINDEX_LIVE_TAIL_FULL_H

#include "blockindex_hot_owner.h"
#include "blockindex_v2_reader.h"
#include "main.h"

#include <cstdint>
#include <memory>
#include <map>
#include <string>
#include <vector>

// G1 — full-topology live-tail materializer.
//
// The approved 2a architecture keeps the PROVEN legacy live consensus engine on
// a BOUNDED hot CBlockIndex tail, while historical authority stays V2/by-value.
// That engine walks pprev / pnext / pskip (ComputeNextStakeModifier,
// GetLastStakeModifier, GetMedianTimePast, difficulty retarget, SetBestChain,
// Reorganize, ConnectBlock / DisconnectBlock). A sparse-hot object (HotOwner's
// pprev=NULL) cannot feed it. This component materializes a FULLY-LINKED
// CBlockIndex chain (pprev/pnext/pskip set, scalar fields from the by-value
// blockindex snapshot) for the bounded resident tail.
//
// Authority is still by-value: the chain is built once, resident for the pin's
// validation lifetime, and re-materializable on demand. Residency is bounded by
// the horizon -- older materialized objects are evictable (never a reorg-depth
// or consensus limit). A deep reorg that needs ancestry older than the horizon
// materializes that ancestry on demand and runs, it is never rejected for
// exceeding N.
//
// MATERIALIZATION != RESIDENCY: this only builds linked CBlockIndex objects from
// the by-value authority; it makes no consensus decision and changes nothing for
// legacy mode.
class BlockIndexLiveTailFull
{
public:
    BlockIndexLiveTailFull();
    ~BlockIndexLiveTailFull();

    BlockIndexLiveTailFull(const BlockIndexLiveTailFull&) = delete;
    BlockIndexLiveTailFull& operator=(const BlockIndexLiveTailFull&) = delete;

    // Bind to the base V2 reader (by-value authority for chain <= S) and the
    // mutable tip (chain > S). Both must remain open and outlive this object.
    void SetSources(const BlockIndexV2Reader* baseReader, const void* tipUnused);

    // Materialize a full-topology CBlockIndex chain for `tipHash`: walk pprev
    // backward by-value down to (and including) the pinned `floorHash` (usually
    // genesis or the boundary where a permanent anchor already exists). Returns a
    // handle to the TIP object; every ancestor is linked via pprev. The whole
    // chain stays resident while the handle is alive (bounded by full-depth of
    // the walk; caller bounds via floorHash so residency stays bounded).
    // Returns NULL + error on authority/materialization failure (fail-closed).
    CBlockIndex* MaterializeChain(const uint256& tipHash,
                                  const uint256& floorHash,
                                  BlockIndexHotHandle* tipHandle,
                                  std::string* error);

    // Materialize a single sparse-hot object for hash (no topology); useful for
    // the by-value side-branch / orphan representation.
    CBlockIndex* MaterializeSingle(const uint256& hash,
                                   BlockIndexHotHandle* handle,
                                   std::string* error);

    // Number of resident (materialized) objects.
    size_t ResidentCount() const;

private:
    class Impl;
    Impl* impl_;
};

// Production accessor (NULL when NOT in authoritative mode). Process-lifetime
// retained by the authoritative startup context.
class BlockIndexAuthoritativeLive;
BlockIndexLiveTailFull* GetAuthoritativeLiveTailFull();

#endif // INNOVA_BLOCKINDEX_LIVE_TAIL_FULL_H