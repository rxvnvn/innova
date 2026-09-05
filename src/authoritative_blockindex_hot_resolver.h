// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.

// A.10.1p - AuthoritativeBlockIndexHotResolver.
//
// Production BY-VALUE hot resolver for ColdHotSeamNavigator.
//
// The navigator's production "hot" side historically used
// LegacyBlockIndexAccessor (cold_hot_seam.h:228) which resolves from the
// resident mapBlockIndex / CBlockIndex* / pprev / pnext graph under cs_main -
// a historical-residency dependency. Phase D R4 was blocked on this.
//
// This resolver implements the navigator's ColdHotHotResolver interface
// (cold_hot_seam.h:56-65) but answers every operation FROM the authoritative
// V2 reader (by-value), never touching mapBlockIndex / CBlockIndex* /
// pprev / pnext / pskip.
//
//   LookupByHash(h)      -> reader.LookupByHash(h)               (snapshot by value)
//   GetActiveByHeight(h) -> reader.GetActiveByHeight(h)
//   GetParentByHash(h)   -> snapshot.hashPrev -> reader.LookupByHash(parent)
//   GetNextActiveByHash(h)-> active height h -> reader.GetActiveByHeight(h+1)
//                            (next-active derived by height, NOT stored pnext)
//   GetTip()             -> reader.GetTip()
//
// Generation coherence: the resolver binds to the SAME open reader used by the
// navigator's cold side (injected). It carries generation; a mismatch/stale
// generation is rejected. No independent CURRENT open, no drift.
//
// Identity: public contract uses pure-hash BlockIndexLogicalId; no process-local
// id crosses the cold/hot seam, so the A.9a.3b-d HOT->COLD rebind bug cannot be
// reintroduced.
//
// No O(N) CBlockIndex mirror, no pointer topology, no O(N) resident hash->object
// table. Historical residency = O(0).

#ifndef INNOVA_AUTHORITATIVE_BLOCKINDEX_HOT_RESOLVER_H
#define INNOVA_AUTHORITATIVE_BLOCKINDEX_HOT_RESOLVER_H

#include "cold_hot_seam.h"        // ColdHotHotResolver
#include "blockindex_v2_reader.h"

#include <stdint.h>
#include <string>

class AuthoritativeBlockIndexHotResolver : public ColdHotHotResolver
{
public:
    explicit AuthoritativeBlockIndexHotResolver(const BlockIndexV2Reader* reader);
    virtual ~AuthoritativeBlockIndexHotResolver() {}

    // --- ColdHotHotResolver interface (all by-value, no mapBlockIndex) ---
    virtual BlockIndexSnapshot LookupByHash(const uint256& hash) const override;
    virtual BlockIndexSnapshot GetActiveByHeight(int height) const override;
    virtual BlockIndexSnapshot GetParentByHash(const uint256& hash) const override;
    virtual BlockIndexSnapshot GetNextActiveByHash(const uint256& hash) const override;
    virtual BlockIndexSnapshot GetTip() const override;

    // Bound generation (0 if no reader).
    uint64_t Generation() const;

private:
    const BlockIndexV2Reader* reader_;
};

#endif // INNOVA_AUTHORITATIVE_BLOCKINDEX_HOT_RESOLVER_H