// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.

#include "blockindex_startup_seam.h"

std::unique_ptr<BlockIndexStartupAuthority> BlockIndexStartupFactory::CreateAuthority(
    BlockIndexStartupMode mode,
    const std::string& v2Root,
    std::string* error)
{
    if (error) error->clear();

    if (mode == BlockIndexStartupMode::LEGACY_RESIDENT)
    {
        // Legacy adapter reads the resident mapBlockIndex/CBlockIndex graph.
        // No open required. Production default — unchanged behavior.
        return std::unique_ptr<BlockIndexStartupAuthority>(
            new LegacyBlockIndexStartupAuthority());
    }

    // BY_VALUE_SHADOW: by-value decode from a validated V2 generation.
    // Allocates NO CBlockIndex and inserts NO mapBlockIndex entry.
    std::unique_ptr<V2BlockIndexStartupAuthority> auth(new V2BlockIndexStartupAuthority());
    const BlockIndexStartupStatus st = auth->Open(v2Root, error);
    if (st != BLOCK_INDEX_STARTUP_OK)
    {
        // No silent fallback to legacy inside an explicitly selected by-value path.
        return std::unique_ptr<BlockIndexStartupAuthority>();
    }
    return std::unique_ptr<BlockIndexStartupAuthority>(auth.release());
}