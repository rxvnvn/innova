// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.

#include "blockindex_startup_authority.h"

#include "main.h"

BlockIndexStartupAuthorityIdentity
LegacyBlockIndexStartupAuthority::Identity() const
{
    BlockIndexStartupAuthorityIdentity out;
    out.kind = BLOCK_INDEX_STARTUP_AUTHORITY_LEGACY;
    out.generationQualified = false;
    out.generation = 0;
    return out;
}

namespace {

BlockIndexStartupRecord ProjectLegacyStartupRecord(const CBlockIndex* pindex)
{
    AssertLockHeld(cs_main);
    BlockIndexStartupRecord out;
    if (pindex == NULL)
        return out;

    out.logicalId = BlockIndexLogicalId(pindex->GetBlockHash());
    out.height = pindex->nHeight;
    out.active = pindex->IsInMainChain();
    out.hasParent = pindex->pprev != NULL;
    if (out.hasParent)
        out.parentLogicalId = BlockIndexLogicalId(pindex->pprev->GetBlockHash());
    out.hasActiveSuccessor = out.active && pindex->pnext != NULL;
    if (out.hasActiveSuccessor)
        out.activeSuccessorLogicalId =
            BlockIndexLogicalId(pindex->pnext->GetBlockHash());

    // Legacy startup computes trust and checksum for every loaded index. Zero is
    // a valid value and is never used as the availability signal.
    out.derived.hasChainTrust = true;
    out.derived.chainTrust = pindex->nChainTrust;
    out.derived.hasStakeModifierChecksum = true;
    out.derived.stakeModifierChecksum = pindex->nStakeModifierChecksum;

    // These fields are in-memory-only memos. In current legacy semantics zero
    // means unset and consumers use their exact fallback paths.
    out.derived.hasStakeModifierTime = pindex->nStakeModifierTime != 0;
    out.derived.stakeModifierTime = pindex->nStakeModifierTime;
    out.derived.hasBlockSize = pindex->nSize != 0;
    out.derived.blockSize = pindex->nSize;
    return out;
}

BlockIndexStartupResult LookupLegacyStartupIndex(const CBlockIndex* pindex)
{
    AssertLockHeld(cs_main);
    if (pindex == NULL)
        return BlockIndexStartupResult::Failure(BLOCK_INDEX_STARTUP_NOT_FOUND);
    return BlockIndexStartupResult::Success(ProjectLegacyStartupRecord(pindex));
}

} // namespace

BlockIndexStartupResult LegacyBlockIndexStartupAuthority::GetTip() const
{
    AssertLockHeld(cs_main);
    return LookupLegacyStartupIndex(pindexBest);
}

BlockIndexStartupResult LegacyBlockIndexStartupAuthority::LookupByHash(
    const BlockIndexLogicalId& id) const
{
    AssertLockHeld(cs_main);
    if (!id.IsValid())
        return BlockIndexStartupResult::Failure(BLOCK_INDEX_STARTUP_NOT_FOUND);
    std::map<uint256, CBlockIndex*>::const_iterator it =
        mapBlockIndex.find(id.GetHash());
    if (it == mapBlockIndex.end())
        return BlockIndexStartupResult::Failure(BLOCK_INDEX_STARTUP_NOT_FOUND);
    return LookupLegacyStartupIndex(it->second);
}

BlockIndexStartupResult LegacyBlockIndexStartupAuthority::LookupActiveByHash(
    const BlockIndexLogicalId& id) const
{
    AssertLockHeld(cs_main);
    BlockIndexStartupResult result = LookupByHash(id);
    if (!result.HasRecord())
        return result;
    if (!result.record.active)
        return BlockIndexStartupResult::Failure(BLOCK_INDEX_STARTUP_NOT_ACTIVE);
    return result;
}

BlockIndexStartupResult LegacyBlockIndexStartupAuthority::GetActiveByHeight(
    int height) const
{
    AssertLockHeld(cs_main);
    if (height < 0 || pindexBest == NULL || height > pindexBest->nHeight)
        return BlockIndexStartupResult::Failure(BLOCK_INDEX_STARTUP_NOT_FOUND);
    const CBlockIndex* pindex = pindexBest->GetAncestor(height);
    if (pindex == NULL)
        return BlockIndexStartupResult::Failure(BLOCK_INDEX_STARTUP_NOT_FOUND);
    if (!pindex->IsInMainChain())
        return BlockIndexStartupResult::Failure(BLOCK_INDEX_STARTUP_NOT_ACTIVE);
    return LookupLegacyStartupIndex(pindex);
}

BlockIndexStartupResult LegacyBlockIndexStartupAuthority::GetParent(
    const BlockIndexLogicalId& child) const
{
    AssertLockHeld(cs_main);
    BlockIndexStartupResult current = LookupByHash(child);
    if (!current.HasRecord())
        return current;
    if (!current.record.hasParent)
        return BlockIndexStartupResult::Failure(BLOCK_INDEX_STARTUP_NOT_FOUND);
    return LookupByHash(current.record.parentLogicalId);
}

BlockIndexStartupResult LegacyBlockIndexStartupAuthority::GetNextActive(
    const BlockIndexLogicalId& current) const
{
    AssertLockHeld(cs_main);
    BlockIndexStartupResult active = LookupActiveByHash(current);
    if (!active.HasRecord())
        return active;
    if (!active.record.hasActiveSuccessor)
        return BlockIndexStartupResult::Failure(BLOCK_INDEX_STARTUP_NOT_FOUND);
    return LookupActiveByHash(active.record.activeSuccessorLogicalId);
}

BlockIndexStartupResult LegacyBlockIndexStartupAuthority::RequireDerivedState(
    const BlockIndexLogicalId& id, unsigned int requirements) const
{
    AssertLockHeld(cs_main);
    BlockIndexStartupResult result = LookupByHash(id);
    if (!result.HasRecord())
        return result;

    const BlockIndexStartupDerivedState& derived = result.record.derived;
    const bool unavailable =
        ((requirements & BLOCK_INDEX_STARTUP_REQUIRE_CHAIN_TRUST) &&
         !derived.hasChainTrust) ||
        ((requirements & BLOCK_INDEX_STARTUP_REQUIRE_STAKE_MODIFIER_CHECKSUM) &&
         !derived.hasStakeModifierChecksum) ||
        ((requirements & BLOCK_INDEX_STARTUP_REQUIRE_STAKE_MODIFIER_TIME) &&
         !derived.hasStakeModifierTime) ||
        ((requirements & BLOCK_INDEX_STARTUP_REQUIRE_BLOCK_SIZE) &&
         !derived.hasBlockSize);
    if (unavailable)
        return BlockIndexStartupResult::Failure(
            BLOCK_INDEX_STARTUP_UNAVAILABLE_DERIVED_STATE);
    return result;
}
