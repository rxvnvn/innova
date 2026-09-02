// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.

#include "blockindex_startup_authority.h"

#include "blockindex_v2_reader.h"
#include "blockindex_derived_state.h"
#include "blockindex_generation_lifecycle.h"
#include "main.h"

// ---- V2 StartupAuthority implementation ----

struct V2BlockIndexStartupAuthority::Impl
{
    BlockIndexV2Reader reader;
    BlockIndexDerivedStateStore derivedStore;
    uint64_t generation;
    bool open;
    bool authoritativeCapable; // A.10.1b-fix2: explicit capability

    Impl() : generation(0), open(false), authoritativeCapable(false) {}
};

V2BlockIndexStartupAuthority::V2BlockIndexStartupAuthority()
    : impl(new Impl())
{
}

// A.10.1b-fix2 W3: explicit destructor releases Impl and owned resources
V2BlockIndexStartupAuthority::~V2BlockIndexStartupAuthority()
{
    Close();
    delete impl;
    impl = NULL;
}

// A.10.1b-fix2 E2: typed open result
BlockIndexStartupStatus V2BlockIndexStartupAuthority::Open(const std::string& root, std::string* error)
{
    Close();

    // Read CURRENT to get selected generation
    BlockIndexCurrentRecord current;
    BlockIndexLifecycleStatus status = BlockIndexGenerationManager::ReadCurrent(root, &current, error);
    if (status == BLOCK_INDEX_LIFECYCLE_NOT_PUBLISHED)
        return BLOCK_INDEX_STARTUP_NOT_FOUND;
    if (status == BLOCK_INDEX_LIFECYCLE_CORRUPT)
        return BLOCK_INDEX_STARTUP_CORRUPT;
    if (status != BLOCK_INDEX_LIFECYCLE_OK)
        return BLOCK_INDEX_STARTUP_IO_ERROR;

    // Validate the generation (includes derived.dat validation if present)
    status = BlockIndexGenerationManager::ValidateGeneration(root, current.generation, error);
    if (status != BLOCK_INDEX_LIFECYCLE_OK)
    {
        // Classify the failure
        if (status == BLOCK_INDEX_LIFECYCLE_MISSING_GENERATION)
            return BLOCK_INDEX_STARTUP_NOT_FOUND;
        if (status == BLOCK_INDEX_LIFECYCLE_CORRUPT)
            return BLOCK_INDEX_STARTUP_CORRUPT;
        return BLOCK_INDEX_STARTUP_IO_ERROR;
    }

    // Check MANIFEST capability
    const std::string genDir = BlockIndexGenerationManager::GenerationPath(root, current.generation);
    FixedBlockIndexOpenOptions storeOpts;
    storeOpts.requireCompleteManifest = true;
    FixedBlockIndexStore store;
    if (!FixedBlockIndexStore::OpenReadOnly(genDir, storeOpts, &store, error))
        return BLOCK_INDEX_STARTUP_IO_ERROR;
    const FixedBlockIndexManifest& manifest = store.GetManifest();

    if (manifest.capability != BLOCK_INDEX_GENERATION_CAPABILITY_AUTHORITATIVE)
    {
        // Old shadow generation: not authoritative-capable
        return BLOCK_INDEX_STARTUP_NOT_AUTHORITATIVE_CAPABLE;
    }

    // Open the V2 reader (reader expects root containing CURRENT, not genDir)
    BlockIndexV2ReaderOptions readerOpts;
    if (!impl->reader.Open(root, readerOpts, error))
        return BLOCK_INDEX_STARTUP_IO_ERROR;

    // Open derived.dat
    if (!BlockIndexDerivedStateStore::OpenReadOnly(genDir, current.generation, &impl->derivedStore, error))
    {
        impl->reader.Close();
        return BLOCK_INDEX_STARTUP_CORRUPT;
    }

    // Verify entry count coherence
    if (impl->derivedStore.EntryCount() != manifest.recordCount)
    {
        impl->reader.Close();
        impl->derivedStore = BlockIndexDerivedStateStore();
        return BLOCK_INDEX_STARTUP_GENERATION_MISMATCH;
    }

    impl->generation = current.generation;
    impl->open = true;
    impl->authoritativeCapable = true;
    return BLOCK_INDEX_STARTUP_OK;
}

void V2BlockIndexStartupAuthority::Close()
{
    if (impl)
    {
        impl->reader.Close();
        impl->derivedStore = BlockIndexDerivedStateStore();
        impl->generation = 0;
        impl->open = false;
    }
}

bool V2BlockIndexStartupAuthority::IsOpen() const
{
    return impl && impl->open;
}

bool V2BlockIndexStartupAuthority::IsAuthoritativeCapable() const
{
    return impl && impl->open && impl->authoritativeCapable;
}

BlockIndexStartupAuthorityIdentity V2BlockIndexStartupAuthority::Identity() const
{
    BlockIndexStartupAuthorityIdentity out;
    if (!impl || !impl->open)
    {
        out.kind = BLOCK_INDEX_STARTUP_AUTHORITY_INVALID;
        return out;
    }
    out.kind = BLOCK_INDEX_STARTUP_AUTHORITY_V2;
    out.generationQualified = true;
    out.generation = impl->generation;
    return out;
}

namespace {

BlockIndexStartupResult MakeV2Failure(BlockIndexStartupStatus status)
{
    return BlockIndexStartupResult::Failure(status);
}

BlockIndexStartupResult ProjectV2Record(const BlockIndexV2Reader& reader,
                                         const BlockIndexDerivedStateStore& derivedStore,
                                         BlockIndexId id,
                                         const BlockIndexSnapshot& snapshot)
{
    if (!snapshot.found)
        return MakeV2Failure(BLOCK_INDEX_STARTUP_NOT_FOUND);

    BlockIndexStartupRecord rec;
    rec.logicalId = BlockIndexLogicalId(snapshot.hash);
    rec.height = snapshot.height;
    rec.active = snapshot.fInMainChain;
    rec.hasParent = snapshot.hasParent;
    if (rec.hasParent)
        rec.parentLogicalId = BlockIndexLogicalId(snapshot.hashPrev);

    // Get active successor
    if (rec.active && snapshot.height < (int)reader.RecordCount())
    {
        BlockIndexSnapshot next;
        std::string err;
        if (reader.GetNextActive(id, &next, &err) && next.found)
        {
            rec.hasActiveSuccessor = true;
            rec.activeSuccessorLogicalId = BlockIndexLogicalId(next.hash);
        }
    }

    // Read derived state
    BlockIndexDerivedEntry derivedEntry;
    std::string derivedErr;
    BlockIndexDerivedLookupStatus derivedStatus = derivedStore.Read(id, &derivedEntry, &derivedErr);
    if (derivedStatus == BLOCK_INDEX_DERIVED_LOOKUP_FOUND)
    {
        rec.derived.hasChainTrust = true;
        rec.derived.chainTrust = derivedEntry.chainTrust;
        rec.derived.hasStakeModifierChecksum = true;
        rec.derived.stakeModifierChecksum = derivedEntry.stakeModifierChecksum;
        rec.derived.hasStakeModifierTime = derivedEntry.HasStakeModifierTime();
        rec.derived.stakeModifierTime = derivedEntry.stakeModifierTime;
        rec.derived.hasBlockSize = derivedEntry.HasBlockSize();
        rec.derived.blockSize = derivedEntry.nSize;
    }
    else if (derivedStatus == BLOCK_INDEX_DERIVED_LOOKUP_CORRUPT)
    {
        return MakeV2Failure(BLOCK_INDEX_STARTUP_CORRUPT);
    }
    else if (derivedStatus == BLOCK_INDEX_DERIVED_LOOKUP_IO_ERROR)
    {
        return MakeV2Failure(BLOCK_INDEX_STARTUP_IO_ERROR);
    }
    // NOT_FOUND or NOT_OPEN: derived state unavailable (not an error for basic lookup)

    return BlockIndexStartupResult::Success(rec);
}

} // namespace

BlockIndexStartupResult V2BlockIndexStartupAuthority::GetTip() const
{
    if (!impl || !impl->open)
        return MakeV2Failure(BLOCK_INDEX_STARTUP_IO_ERROR);

    BlockIndexSnapshot tip = impl->reader.GetTip();
    if (!tip.found)
        return MakeV2Failure(BLOCK_INDEX_STARTUP_NOT_FOUND);

    return ProjectV2Record(impl->reader, impl->derivedStore, tip.id, tip);
}

BlockIndexStartupResult V2BlockIndexStartupAuthority::LookupByHash(const BlockIndexLogicalId& id) const
{
    if (!impl || !impl->open)
        return MakeV2Failure(BLOCK_INDEX_STARTUP_IO_ERROR);
    if (!id.IsValid())
        return MakeV2Failure(BLOCK_INDEX_STARTUP_NOT_FOUND);

    BlockIndexSnapshot snapshot;
    std::string err;
    BlockIndexV2ReadStatus status = impl->reader.LookupByHash(id.GetHash(), &snapshot, &err);
    if (status == BLOCK_INDEX_V2_READ_NOT_FOUND)
        return MakeV2Failure(BLOCK_INDEX_STARTUP_NOT_FOUND);
    if (status != BLOCK_INDEX_V2_READ_FOUND)
        return MakeV2Failure(BLOCK_INDEX_STARTUP_CORRUPT);

    return ProjectV2Record(impl->reader, impl->derivedStore, snapshot.id, snapshot);
}

BlockIndexStartupResult V2BlockIndexStartupAuthority::LookupActiveByHash(const BlockIndexLogicalId& id) const
{
    BlockIndexStartupResult result = LookupByHash(id);
    if (!result.HasRecord())
        return result;
    if (!result.record.active)
        return MakeV2Failure(BLOCK_INDEX_STARTUP_NOT_ACTIVE);
    return result;
}

BlockIndexStartupResult V2BlockIndexStartupAuthority::GetActiveByHeight(int height) const
{
    if (!impl || !impl->open)
        return MakeV2Failure(BLOCK_INDEX_STARTUP_IO_ERROR);

    BlockIndexSnapshot snapshot;
    std::string err;
    BlockIndexV2ReadStatus status = impl->reader.GetActiveByHeight(height, &snapshot, &err);
    if (status == BLOCK_INDEX_V2_READ_NOT_FOUND)
        return MakeV2Failure(BLOCK_INDEX_STARTUP_NOT_FOUND);
    if (status != BLOCK_INDEX_V2_READ_FOUND)
        return MakeV2Failure(BLOCK_INDEX_STARTUP_CORRUPT);

    return ProjectV2Record(impl->reader, impl->derivedStore, snapshot.id, snapshot);
}

BlockIndexStartupResult V2BlockIndexStartupAuthority::GetParent(const BlockIndexLogicalId& child) const
{
    BlockIndexStartupResult current = LookupByHash(child);
    if (!current.HasRecord())
        return current;
    if (!current.record.hasParent)
        return MakeV2Failure(BLOCK_INDEX_STARTUP_NOT_FOUND);
    return LookupByHash(current.record.parentLogicalId);
}

BlockIndexStartupResult V2BlockIndexStartupAuthority::GetNextActive(const BlockIndexLogicalId& current) const
{
    BlockIndexStartupResult active = LookupActiveByHash(current);
    if (!active.HasRecord())
        return active;
    if (!active.record.hasActiveSuccessor)
        return MakeV2Failure(BLOCK_INDEX_STARTUP_NOT_FOUND);
    return LookupActiveByHash(active.record.activeSuccessorLogicalId);
}

BlockIndexStartupResult V2BlockIndexStartupAuthority::RequireDerivedState(
    const BlockIndexLogicalId& id, unsigned int requirements) const
{
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

// ---- Legacy StartupAuthority (existing) ----

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
