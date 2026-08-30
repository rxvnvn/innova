// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef INNOVA_BLOCKINDEX_NAVIGATION_H
#define INNOVA_BLOCKINDEX_NAVIGATION_H

#include "blockindex_accessor.h"

/**
 * Stable logical identity of a block-index entry.  The hash names the block,
 * independently of its resident or persistent representation.
 */
class BlockIndexLogicalId
{
public:
    BlockIndexLogicalId() : hash(0) {}
    explicit BlockIndexLogicalId(const uint256& hashIn) : hash(hashIn) {}

    bool IsValid() const { return hash != uint256(0); }
    const uint256& GetHash() const { return hash; }

    bool operator==(const BlockIndexLogicalId& other) const { return hash == other.hash; }
    bool operator!=(const BlockIndexLogicalId& other) const { return !(*this == other); }
    bool operator<(const BlockIndexLogicalId& other) const { return hash < other.hash; }

private:
    uint256 hash;
};

enum BlockIndexNavigationDomain
{
    BLOCK_INDEX_NAVIGATION_INVALID = 0,
    BLOCK_INDEX_NAVIGATION_HOT = 1,
    BLOCK_INDEX_NAVIGATION_COLD = 2,
};

/**
 * A resolver reference, deliberately distinct from logical identity.
 *
 * HOT refs have no process-local legacy ID: they resolve by the stable hash.
 * COLD refs bind a RecordId to the immutable generation that owns it.  A cold
 * ref from another generation is invalid for this reader even if its numeric
 * RecordId is in range there.
 */
struct BlockIndexNavigationRef
{
    BlockIndexLogicalId logical;
    BlockIndexNavigationDomain domain;
    uint64_t generation;
    BlockIndexId recordId;

    BlockIndexNavigationRef()
        : logical(), domain(BLOCK_INDEX_NAVIGATION_INVALID), generation(0),
          recordId(BLOCK_INDEX_ID_INVALID) {}

    static BlockIndexNavigationRef Hot(const BlockIndexLogicalId& logical);
    static BlockIndexNavigationRef Cold(const BlockIndexLogicalId& logical,
                                        uint64_t generation, BlockIndexId recordId);

    bool IsValid() const;
    bool IsHot() const { return domain == BLOCK_INDEX_NAVIGATION_HOT && logical.IsValid() && generation == 0 && recordId == BLOCK_INDEX_ID_INVALID; }
    bool IsCold() const { return domain == BLOCK_INDEX_NAVIGATION_COLD && logical.IsValid() && generation != 0 && recordId != BLOCK_INDEX_ID_INVALID; }
    bool MatchesGeneration(uint64_t currentGeneration) const
    {
        return IsCold() && generation == currentGeneration;
    }

    bool operator==(const BlockIndexNavigationRef& other) const
    {
        return logical == other.logical && domain == other.domain &&
               generation == other.generation && recordId == other.recordId;
    }
    bool operator!=(const BlockIndexNavigationRef& other) const { return !(*this == other); }
};

/**
 * Explicit availability contract for V2-derived staking metadata.  Values are
 * only consumable when their corresponding has* flag is true; zero itself is a
 * valid value and never means "available".
 */
struct BlockIndexStakingMetadata
{
    bool hasStakeModifierTime;
    bool hasStakeModifierChecksum;
    int64_t nStakeModifierTime;
    unsigned int nStakeModifierChecksum;

    BlockIndexStakingMetadata()
        : hasStakeModifierTime(false), hasStakeModifierChecksum(false),
          nStakeModifierTime(0), nStakeModifierChecksum(0) {}
};

#endif // INNOVA_BLOCKINDEX_NAVIGATION_H
