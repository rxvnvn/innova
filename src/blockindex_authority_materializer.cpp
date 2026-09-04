// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "blockindex_authority_materializer.h"

#include <string>

namespace {

static bool Fail(std::string* error, const std::string& text) {
    if (error) *error = text;
    return false;
}

static void Clear(std::string* error) {
    if (error) error->clear();
}

} // namespace

BlockIndexAuthorityMaterializer::BlockIndexAuthorityMaterializer(
    const BlockIndexV2Reader* reader,
    const BlockIndexDerivedStateStore* derivedStore,
    uint64_t generation)
    : reader_(reader), derivedStore_(derivedStore), generation_(generation)
{
    // No validation here — Materialize() will fail explicitly if components are not open
}

BlockIndexHotStatus BlockIndexAuthorityMaterializer::Materialize(
    const BlockIndexLogicalId& id,
    BlockIndexHotMaterialized* out) const
{
    if (!out) return BlockIndexHotStatus::MATERIALIZATION_UNAVAILABLE;

    // Initialize output
    out->found = false;
    out->generation = generation_;
    out->hasBlockSize = false;
    out->blockSize = 0;

    // Validate components are open
    if (!reader_ || !reader_->IsOpen()) {
        return BlockIndexHotStatus::AUTHORITY_MISSING;
    }
    if (!derivedStore_ || !derivedStore_->IsOpen()) {
        return BlockIndexHotStatus::MATERIALIZATION_UNAVAILABLE;
    }
    if (!id.IsValid()) {
        return BlockIndexHotStatus::AUTHORITY_MISSING;
    }

    // Step 1: Lookup by hash in V2 reader (hashindex → RecordId → record)
    BlockIndexSnapshot snapshot;
    std::string readerErr;
    BlockIndexV2ReadStatus readStatus = reader_->LookupByHash(id.GetHash(), &snapshot, &readerErr);

    BlockIndexHotStatus mappedStatus = MapV2ReadStatus(readStatus);
    if (mappedStatus != BlockIndexHotStatus::OK) {
        return mappedStatus;
    }

    // snapshot.id now contains the RecordId from the V2 reader
    BlockIndexId recordId = snapshot.id;

    // Step 2: Read derived state for this RecordId
    BlockIndexDerivedEntry derivedEntry;
    std::string derivedErr;
    BlockIndexDerivedLookupStatus derivedStatus = derivedStore_->Read(recordId, &derivedEntry, &derivedErr);

    BlockIndexHotStatus mappedDerivedStatus = MapDerivedStatus(derivedStatus);
    if (mappedDerivedStatus != BlockIndexHotStatus::OK) {
        // Derived state unavailable — chainTrust/blockSize/stake-modifier missing
        // This is a MATERIALIZATION_UNAVAILABLE, not AUTHORITY_MISSING
        return BlockIndexHotStatus::MATERIALIZATION_UNAVAILABLE;
    }

    // Step 3: Populate full BlockIndexSnapshot from snapshot (record fields) + derived
    if (!PopulateSnapshot(recordId, derivedEntry, snapshot)) {
        return BlockIndexHotStatus::CORRUPT_METADATA;
    }

    // Step 4: Fill output
    out->snapshot = snapshot;
    out->generation = generation_;
    out->found = true;

    // Derived blockSize (from derived.dat)
    if (derivedEntry.HasBlockSize()) {
        out->hasBlockSize = true;
        out->blockSize = derivedEntry.nSize;
    }

    return BlockIndexHotStatus::OK;
}

bool BlockIndexAuthorityMaterializer::PopulateSnapshot(
    BlockIndexId recordId,
    const BlockIndexDerivedEntry& derivedEntry,
    BlockIndexSnapshot& snapshot) const
{
    // The snapshot from LookupByHash already has all record fields via SnapshotFromRecord:
    // hash, hashPrev, hashMerkleRoot, height, nFile, nBlockPos, nFlags, nVersion,
    // nTime, nBits, nNonce, nMint, nMoneySupply, nStakeModifier, prevoutStake,
    // nStakeTime, hashProof, fProofOfStake, fInMainChain, hasParent, hashNext.
    // We only need to overlay derived fields.

    // Derived chainTrust (always available if derivedEntry was found)
    snapshot.nChainTrust = derivedEntry.chainTrust;

    // Derived stake modifier checksum
    snapshot.nStakeModifierChecksum = derivedEntry.stakeModifierChecksum;
    snapshot.hasStakeModifierChecksum = true; // V2 derived always has checksum if entry found

    // Derived stake modifier time (availability-flagged)
    snapshot.nStakeModifierTime = derivedEntry.stakeModifierTime;
    snapshot.hasStakeModifierTime = derivedEntry.HasStakeModifierTime();

    // Verify hash consistency (snapshot.hash was set from record in SnapshotFromRecord)
    if (snapshot.hash == uint256(0)) {
        return false;
    }

    return true;
}

BlockIndexHotStatus BlockIndexAuthorityMaterializer::MapV2ReadStatus(
    BlockIndexV2ReadStatus status)
{
    switch (status) {
        case BLOCK_INDEX_V2_READ_FOUND:
            return BlockIndexHotStatus::OK;
        case BLOCK_INDEX_V2_READ_NOT_FOUND:
            return BlockIndexHotStatus::AUTHORITY_MISSING;
        case BLOCK_INDEX_V2_READ_CORRUPT:
            return BlockIndexHotStatus::CORRUPT_METADATA;
        case BLOCK_INDEX_V2_READ_IO_ERROR:
            return BlockIndexHotStatus::MATERIALIZATION_UNAVAILABLE; // No IO_ERROR in HotStatus
        case BLOCK_INDEX_V2_READ_NOT_OPEN:
            return BlockIndexHotStatus::AUTHORITY_MISSING;
        default:
            return BlockIndexHotStatus::MATERIALIZATION_UNAVAILABLE;
    }
}

BlockIndexHotStatus BlockIndexAuthorityMaterializer::MapDerivedStatus(
    BlockIndexDerivedLookupStatus status)
{
    switch (status) {
        case BLOCK_INDEX_DERIVED_LOOKUP_FOUND:
            return BlockIndexHotStatus::OK;
        case BLOCK_INDEX_DERIVED_LOOKUP_NOT_FOUND:
            return BlockIndexHotStatus::MATERIALIZATION_UNAVAILABLE;
        case BLOCK_INDEX_DERIVED_LOOKUP_NOT_OPEN:
            return BlockIndexHotStatus::MATERIALIZATION_UNAVAILABLE;
        case BLOCK_INDEX_DERIVED_LOOKUP_IO_ERROR:
            return BlockIndexHotStatus::MATERIALIZATION_UNAVAILABLE;
        case BLOCK_INDEX_DERIVED_LOOKUP_CORRUPT:
            return BlockIndexHotStatus::CORRUPT_METADATA;
        default:
            return BlockIndexHotStatus::MATERIALIZATION_UNAVAILABLE;
    }
}