// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.

#ifndef INNOVA_BLOCKINDEX_AUTHORITATIVE_RESTART_H
#define INNOVA_BLOCKINDEX_AUTHORITATIVE_RESTART_H

#include "blockindex_tip.h"
#include "blockindex_live_tail.h"
#include "blockindex_startup_bootstrap.h"

#include <cstdint>
#include <memory>
#include <string>

// P5 — authoritative restart reconstruction from base + blockindex_tip.
//
// The immutable base generation (CURRENT-selected) holds the validated chain up
// to its committed tip S. Blocks accepted live above S were persisted by the
// BlockIndexTipAuthority (P1) into blockindex_tip/ (active + side branches +
// derived + DAG, crash-consistent via tip.meta). On restart the authoritative
// startup must land at the persisted post-S tip, NOT silently regress to S.
//
// This module:
//   1. Opens the base generation (bootstrap already bound).
//   2. Opens blockindex_tip/ under the SAME v2Root, validating base-generation
//      binding (tip.meta.baseGeneration == base generation).
//   3. If the tip is non-empty, materializes the post-S resident live tail
//      (bounded by the config horizon) via the composite materializer, and
//      re-publishes the effective tip (height/hash/trust) to the caller so the
//      authoritative startup sets pindexBest/nBestHeight/hashBestChain to the
//      post-S tip L.
//   4. Fails closed forever-on any base/tip mismatch or corrupt tip state.
//
// No consensus change, no reindex/rescan, no full-history residency. The base
// generation is never mutated; the tip is the mutable extension.

class BlockIndexAuthoritativeRestart
{
public:
    BlockIndexAuthoritativeRestart();
    ~BlockIndexAuthoritativeRestart();

    // Open base (via the bootstrap's generation/reader) + tip. If baseGen is
    // provided (e.g. from the already-open bootstrap), the tip MUST bind to it;
    // pass hasBaseGen=false when the tip is the only source open.
    bool OpenBaseAndTip(const std::string& v2Root,
                        bool hasBaseGen, uint64_t baseGen,
                        BlockIndexStartupBootstrap* bootstrap,  // optional, for the base reader
                        std::string* error);

    // Effective tip after reconciliation:
    //   - If tip is non-empty: the post-S tip (height/hash) from the tip authority.
    //   - If tip empty/absent: the base tip S.
    bool HasPostSTip() const;      // tip authority has records above S
    int32_t EffectiveTipHeight() const;
    uint256 EffectiveTipHash() const;

    // Causal diagnostics.
    uint64_t BaseGeneration() const;
    size_t TipRecordCount() const;

    void Close();

private:
    std::unique_ptr<BlockIndexTipAuthority> tip_;
    uint64_t baseGen_;
    bool hasBase_;
    int32_t effectiveTipHeight_;
    uint256 effectiveTipHash_;
};

#endif // INNOVA_BLOCKINDEX_AUTHORITATIVE_RESTART_H