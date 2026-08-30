#include "blockindex_navigation.h"

BlockIndexNavigationRef BlockIndexNavigationRef::Hot(const BlockIndexLogicalId& logical)
{
    BlockIndexNavigationRef ref;
    if (!logical.IsValid())
        return ref;
    ref.logical = logical;
    ref.domain = BLOCK_INDEX_NAVIGATION_HOT;
    return ref;
}

BlockIndexNavigationRef BlockIndexNavigationRef::Cold(const BlockIndexLogicalId& logical,
                                                       uint64_t generation,
                                                       BlockIndexId recordId)
{
    BlockIndexNavigationRef ref;
    if (!logical.IsValid() || generation == 0 || recordId == BLOCK_INDEX_ID_INVALID)
        return ref;
    ref.logical = logical;
    ref.domain = BLOCK_INDEX_NAVIGATION_COLD;
    ref.generation = generation;
    ref.recordId = recordId;
    return ref;
}

bool BlockIndexNavigationRef::IsValid() const
{
    return IsHot() || IsCold();
}
