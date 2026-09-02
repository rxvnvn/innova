// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.

#ifndef INNOVA_BLOCKINDEX_DERIVED_STATE_H
#define INNOVA_BLOCKINDEX_DERIVED_STATE_H

#include "fixed_blockindex_store.h"

#include <boost/filesystem/path.hpp>
#include <boost/shared_ptr.hpp>
#include "sync.h"
#include <stdint.h>
#include <string>
#include <vector>

// Phase A.10.1b-fix1: Integrated, content-bound derived-state companion store.
//
// Purpose: provide O(1) authoritative lookup of startup-derived values
// (nChainTrust, nStakeModifierChecksum, nStakeModifierTime, nSize) without
// constructing the all-history legacy CBlockIndex graph or performing
// per-query O(depth) reconstruction.
//
// Design: generation-bound companion file (derived.dat) indexed by RecordId,
// co-located with records.dat/active.dat/hashindex in the same generation
// directory. Independently versioned from the V1 228-byte record format.
//
// V1 generations without derived.dat remain readable for shadow/legacy mode
// but cannot serve V2 authoritative startup derived state.
//
// V2 format changes from V1:
//   - uint256 chainTrust encoded as TRUE little-endian (raw 32 bytes)
//   - nSize field added (uint32, availability-flagged)
//   - Content binding (SHA256 of tip hash + record count + generation)
//   - Entry size: 56 bytes (was 52)
//   - Header size: 80 bytes (was 48)
//
// Fields persisted:
//   nChainTrust (uint256)           — always available, load-bearing for chain selection
//   nStakeModifierChecksum (uint32) — always available, consensus/checkpoint critical
//   nStakeModifierTime (int64)      — availability-flagged, memo only
//   nSize (uint32)                  — availability-flagged, exact serialized block size

// ---- derived.dat V2 persistent format ----
// Fixed-size, little-endian, ABI-independent.
//
// Header (80 bytes):
//   [0]  / 8  / magic            "INNBDRV1"
//   [8]  / 4  / format_version   uint32 LE = 2
//   [12] / 4  / schema_version   uint32 LE = 2
//   [16] / 4  / header_size      uint32 LE = 80
//   [20] / 4  / entry_size       uint32 LE = 56
//   [24] / 8  / generation       uint64 LE
//   [32] / 8  / entry_count      uint64 LE
//   [40] / 32 / content_binding  SHA256(tipHash || recordCount || generation)
//   [72] / 8  / reserved         uint64 LE = 0
//
// Entry (56 bytes), indexed by RecordId (1-based):
//   [0]  / 32 / chainTrust           uint256 TRUE LE (raw bytes)
//   [32] / 4  / stakeModifierChecksum uint32 LE
//   [36] / 8  / stakeModifierTime    int64 LE
//   [44] / 4  / nSize                uint32 LE
//   [48] / 4  / flags                uint32 LE
//                                  bit 0: hasStakeModifierTime
//                                  bit 1: hasBlockSize
//   [52] / 4  / checksum             CRC32 over bytes [0..52)

static const uint32_t BLOCK_INDEX_DERIVED_FORMAT_VERSION = 2;
static const uint32_t BLOCK_INDEX_DERIVED_SCHEMA_VERSION = 2;
static const uint32_t BLOCK_INDEX_DERIVED_HEADER_SIZE_V2 = 80;
static const uint32_t BLOCK_INDEX_DERIVED_ENTRY_SIZE_V2 = 56;
static const char* const BLOCK_INDEX_DERIVED_FILE_NAME = "derived.dat";

// V1 constants for backward compatibility (read-only)
static const uint32_t BLOCK_INDEX_DERIVED_FORMAT_VERSION_V1 = 1;
static const uint32_t BLOCK_INDEX_DERIVED_SCHEMA_VERSION_V1 = 1;
static const uint32_t BLOCK_INDEX_DERIVED_HEADER_SIZE_V1 = 48;
static const uint32_t BLOCK_INDEX_DERIVED_ENTRY_SIZE_V1 = 52;

// Entry flags
static const uint32_t BLOCK_INDEX_DERIVED_FLAG_HAS_MODIFIER_TIME = (1U << 0);
static const uint32_t BLOCK_INDEX_DERIVED_FLAG_HAS_BLOCK_SIZE    = (1U << 1);

// Unknown flag mask: all known flags
static const uint32_t BLOCK_INDEX_DERIVED_FLAG_KNOWN_MASK =
    BLOCK_INDEX_DERIVED_FLAG_HAS_MODIFIER_TIME |
    BLOCK_INDEX_DERIVED_FLAG_HAS_BLOCK_SIZE;

struct BlockIndexDerivedEntry
{
    uint256 chainTrust;
    uint32_t stakeModifierChecksum;
    int64_t stakeModifierTime;
    uint32_t nSize;
    uint32_t flags;

    BlockIndexDerivedEntry()
        : chainTrust(0), stakeModifierChecksum(0),
          stakeModifierTime(0), nSize(0), flags(0) {}

    bool HasStakeModifierTime() const { return (flags & BLOCK_INDEX_DERIVED_FLAG_HAS_MODIFIER_TIME) != 0; }
    void SetHasStakeModifierTime(bool has)
    {
        if (has)
            flags |= BLOCK_INDEX_DERIVED_FLAG_HAS_MODIFIER_TIME;
        else
            flags &= ~BLOCK_INDEX_DERIVED_FLAG_HAS_MODIFIER_TIME;
    }

    bool HasBlockSize() const { return (flags & BLOCK_INDEX_DERIVED_FLAG_HAS_BLOCK_SIZE) != 0; }
    void SetHasBlockSize(bool has)
    {
        if (has)
            flags |= BLOCK_INDEX_DERIVED_FLAG_HAS_BLOCK_SIZE;
        else
            flags &= ~BLOCK_INDEX_DERIVED_FLAG_HAS_BLOCK_SIZE;
    }
};

struct BlockIndexDerivedHeader
{
    uint32_t formatVersion;
    uint32_t schemaVersion;
    uint32_t headerSize;
    uint32_t entrySize;
    uint64_t generation;
    uint64_t entryCount;
    unsigned char contentBinding[32]; // SHA256(tipHash || recordCount || generation)

    BlockIndexDerivedHeader()
        : formatVersion(BLOCK_INDEX_DERIVED_FORMAT_VERSION),
          schemaVersion(BLOCK_INDEX_DERIVED_SCHEMA_VERSION),
          headerSize(BLOCK_INDEX_DERIVED_HEADER_SIZE_V2),
          entrySize(BLOCK_INDEX_DERIVED_ENTRY_SIZE_V2),
          generation(0), entryCount(0)
    {
        memset(contentBinding, 0, 32);
    }
};

bool EncodeBlockIndexDerivedEntry(const BlockIndexDerivedEntry& entry,
                                  std::vector<unsigned char>* out,
                                  std::string* error);
bool DecodeBlockIndexDerivedEntry(const unsigned char* data, size_t size,
                                  BlockIndexDerivedEntry* out,
                                  std::string* error);

enum BlockIndexDerivedLookupStatus
{
    BLOCK_INDEX_DERIVED_LOOKUP_FOUND = 1,
    BLOCK_INDEX_DERIVED_LOOKUP_NOT_FOUND = 2,
    BLOCK_INDEX_DERIVED_LOOKUP_NOT_OPEN = 3,
    BLOCK_INDEX_DERIVED_LOOKUP_IO_ERROR = 4,
    BLOCK_INDEX_DERIVED_LOOKUP_CORRUPT = 5,
};

// Compute content binding: SHA256(tipHash || recordCount || generation)
// All values in true little-endian byte order.
// LEGACY: metadata-only binding (fix1). Retained for backward compatibility.
bool ComputeDerivedContentBinding(const uint256& tipHash,
                                  uint64_t recordCount,
                                  uint64_t generation,
                                  unsigned char binding[32]);

// A.10.1b-fix2: Compute generation root that commits to actual component
// content. This replaces the metadata-only binding for authoritative generations.
//
// generationRoot = SHA256(
//   domain "GENROOT" (8 bytes) ||
//   version uint32 LE (1) ||
//   generation uint64 LE ||
//   committedTipHash (32 bytes) ||
//   recordCount uint64 LE ||
//   recordsDigest (32 bytes) ||
//   activeDigest (32 bytes) ||
//   hashIndexDigest (32 bytes) ||
//   derivedEntriesDigest (32 bytes) ||
//   dagInputDigest (32 bytes)
// )
//
// Component digests are SHA256 of the canonical byte representation of each
// component's content (entries only, not headers).
bool ComputeGenerationRoot(uint64_t generation,
                           const uint256& tipHash,
                           uint64_t recordCount,
                           const unsigned char recordsDigest[32],
                           const unsigned char activeDigest[32],
                           const unsigned char hashIndexDigest[32],
                           const unsigned char derivedEntriesDigest[32],
                           const unsigned char dagInputDigest[32],
                           unsigned char root[32]);

class BlockIndexDerivedStateStore
{
public:
    BlockIndexDerivedStateStore();

    // Create a new derived.dat for writing (generation-bound, V2 format).
    static bool Create(const std::string& dir, uint64_t generation,
                       const unsigned char contentBinding[32],
                       BlockIndexDerivedStateStore* out, std::string* error);

    // Open an existing derived.dat for reading (generation-bound).
    // Accepts V2 format only. V1 files are rejected.
    static bool OpenReadOnly(const std::string& dir, uint64_t expectedGeneration,
                             BlockIndexDerivedStateStore* out, std::string* error);

    // Append a single entry (RecordId is implicit: entryCount + 1).
    bool Append(const BlockIndexDerivedEntry& entry, std::string* error);

    // Batch append (single fsync).
    bool AppendBatch(const std::vector<BlockIndexDerivedEntry>& entries,
                     std::string* error);

    // Read entry by RecordId (1-based).
    BlockIndexDerivedLookupStatus Read(BlockIndexId id,
                                       BlockIndexDerivedEntry* out,
                                       std::string* error) const;

    // Finalize: write header with entry count. Must be called after all
    // appends and before opening for reading.
    bool Finalize(std::string* error);

    uint64_t EntryCount() const;
    uint64_t Generation() const;
    bool IsOpen() const;
    uint32_t FormatVersion() const;
    uint32_t SchemaVersion() const;

    // Validate content binding against expected values.
    bool ValidateContentBinding(const uint256& expectedTipHash,
                                uint64_t expectedRecordCount,
                                std::string* error) const;

    // A.10.1b-fix2: Update content binding (used by builder before Finalize
    // when generation root is computed from component digests).
    void SetContentBinding(const unsigned char binding[32]);

    // A.10.1b-fix3: Read content binding (used by validation to compare
    // against independently recomputed generation root).
    void GetContentBinding(unsigned char out[32]) const;

private:
    struct ReadHandle;
    boost::shared_ptr<ReadHandle> readHandle;
    boost::filesystem::path dirPath;
    boost::filesystem::path derivedPath;
    uint64_t generation;
    uint64_t entryCount;
    unsigned char contentBinding[32];
    uint32_t formatVersion;
    uint32_t schemaVersion;
    bool writable;
    bool open;

    bool InitializePaths(const std::string& dir, std::string* error);
    bool WriteHeader(std::string* error);
    bool LoadHeader(uint64_t* fileSize, std::string* error);
    bool ValidateHeader(const BlockIndexDerivedHeader& header,
                        std::string* error) const;
};

#endif // INNOVA_BLOCKINDEX_DERIVED_STATE_H
