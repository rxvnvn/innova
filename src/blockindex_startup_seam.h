// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.

// A.10.1h — Startup selection seam (by-value legacy startup decode separation).
//
// Separates STARTUP METADATA DECODE from CBlockIndex OBJECT ALLOCATION/RESIDENCY.
//
// Two explicit modes:
//
//   LEGACY_RESIDENT (default, production): startup continues via the legacy
//     LoadBlockIndex() path; CreateAuthority returns the
//     LegacyBlockIndexStartupAuthority adapter that reads the resident
//     mapBlockIndex/CBlockIndex graph. Production default behavior is
//     UNCHANGED.
//
//   BY_VALUE_SHADOW (opt-in): decodes historical block-index startup metadata
//     from a VALIDATED V2 generation via V2BlockIndexStartupAuthority. This
//     path allocates NO CBlockIndex, inserts NO mapBlockIndex entry, and
//     builds NO pprev/pnext/pskip pointer graph. It establishes the by-value
//     seam that later phases B (HotOwner lifecycle) and D (authoritative
//     startup experiment) select.
//
// This seam is real production code. It is exercised through direct
// production function tests / injected startup selectors in isolated fixtures,
// not through a broad user-visible daemon flag. No consensus, chain-selection,
// staking, checkpoint, wallet, P2P, or RPC semantics are changed by this seam.

#ifndef INNOVA_BLOCKINDEX_STARTUP_SEAM_H
#define INNOVA_BLOCKINDEX_STARTUP_SEAM_H

#include "blockindex_startup_authority.h"

#include <memory>
#include <string>

enum class BlockIndexStartupMode
{
    LEGACY_RESIDENT = 0,  // default; legacy startup unchanged
    BY_VALUE_SHADOW = 1,  // opt-in by-value decode; no CBlockIndex allocation
};

// Factory owning the startup-authority selection. STATELESS; creation does not
// modify global block-index state. The authoritative-startup authority
// (AUTHORITY) is produced independently of materialization (MATERIALIZATION)
// and of residency lifetime (RESIDENCY LIFETIME); this seam only selects which
// authority an explicit caller consumes.
class BlockIndexStartupFactory
{
public:
    BlockIndexStartupFactory() = delete;

    // Default production mode is always LEGACY_RESIDENT.
    static BlockIndexStartupMode DefaultMode() { return BlockIndexStartupMode::LEGACY_RESIDENT; }

    // Build the startup authority for the requested mode.
    //
    //  LEGACY_RESIDENT -> LegacyBlockIndexStartupAuthority (no open required;
    //    reads the resident mapBlockIndex). Never returns nullptr.
    //
    //  BY_VALUE_SHADOW  -> V2BlockIndexStartupAuthority opened on v2Root.
    //    On any open failure returns nullptr and sets *error. It does NOT
    //    silently fall back to the legacy adapter (no hidden semantic
    //    fallback inside an explicitly selected by-value path).
    //
    // Ownership transfers to the caller.
    static std::unique_ptr<BlockIndexStartupAuthority> CreateAuthority(
        BlockIndexStartupMode mode,
        const std::string& v2Root,
        std::string* error);
};

#endif // INNOVA_BLOCKINDEX_STARTUP_SEAM_H