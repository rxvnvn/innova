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

// Phase A.10.1b: Persisted derived-state companion store.
//
// Purpose: provide O(1) authoritative lookup of startup-derived values
// (nChainTrust, nStakeModifierChecksum, nStakeModifierTime) without
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
// Fields persisted:
//   nChainTrust (uint256)           — always available, load-bearing for chain selection
//   nStakeModifierChecksum (uint32) — always available, consensus/checkpoint critical
//   nStakeModifierTime (int64)      — availability-flagged, memo only
//
// Fields NOT persisted:
//   nSize — never set in current code (txdb-leveldb.cpp:1034 comment is misleading;
//           nSize is always 0 in CBlockIndex after startup; the median block size
//           code at main.cpp:3356-3360 uses fallback 1 for zero)

// ---- derived.dat V1 persistent format ----
// Fixed-size, little-endian, ABI-independent.
//
// Header (48 bytes):
//   [0]  / 8  / magic            "INNBDRV1"
//   [8]  / 4  / format_version   uint32 LE = 1
//   [12] / 4  / schema_version   uint32 LE = 1
//   [16] / 4  / header_size      uint32 LE = 48
//   [20] / 4  / entry_size       uint32 LE = 52
//   [24] / 8  / generation       uint64 LE
//   [32] / 8  / entry_count      uint64 LE
//   [40] / 8  / reserved         uint64 LE = 0
//
// Entry (52 bytes), indexed by RecordId (1-based):
//   [0]  / 32 / chainTrust           uint256 LE
//   [32] / 4  / stakeModifierChecksum uint32 LE
//   [36] / 8  / stakeModifierTime    int64 LE
//   [44] / 4  / flags                uint32 LE (bit 0: hasStakeModifierTime)
//   [48] / 4  / checksum             CRC32 over bytes [0..48)

static const uint32_t BLOCK_INDEX_DERIVED_FORMAT_VERSION = 1;
static const uint32_t BLOCK_INDEX_DERIVED_SCHEMA_VERSION = 1;
static const uint32_t BLOCK_INDEX_DERIVED_HEADER_SIZE_V1 = 48;
static const uint32_t BLOCK_INDEX_DERIVED_ENTRY_SIZE_V1 = 52;
static const char* const BLOCK_INDEX_DERIVED_FILE_NAME = "derived.dat";

// Entry flags
static const uint32_t BLOCK_INDEX_DERIVED_FLAG_HAS_MODIFIER_TIME = (1U << 0);

struct BlockIndexDerivedEntry
{
    uint256 chainTrust;
    uint32_t stakeModifierChecksum;
    int64_t stakeModifierTime;
    uint32_t flags;

    BlockIndexDerivedEntry()
        : chainTrust(0), stakeModifierChecksum(0),
          stakeModifierTime(0), flags(0) {}

    bool HasStakeModifierTime() const { return (flags & BLOCK_INDEX_DERIVED_FLAG_HAS_MODIFIER_TIME) != 0; }
    void SetHasStakeModifierTime(bool has)
    {
        if (has)
            flags |= BLOCK_INDEX_DERIVED_FLAG_HAS_MODIFIER_TIME;
        else
            flags &= ~BLOCK_INDEX_DERIVED_FLAG_HAS_MODIFIER_TIME;
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

    BlockIndexDerivedHeader()
        : formatVersion(BLOCK_INDEX_DERIVED_FORMAT_VERSION),
          schemaVersion(BLOCK_INDEX_DERIVED_SCHEMA_VERSION),
          headerSize(BLOCK_INDEX_DERIVED_HEADER_SIZE_V1),
          entrySize(BLOCK_INDEX_DERIVED_ENTRY_SIZE_V1),
          generation(0), entryCount(0) {}
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

class BlockIndexDerivedStateStore
{
public:
    BlockIndexDerivedStateStore();

    // Create a new derived.dat for writing (generation-bound).
    static bool Create(const std::string& dir, uint64_t generation,
                       BlockIndexDerivedStateStore* out, std::string* error);

    // Open an existing derived.dat for reading (generation-bound).
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

private:
    struct ReadHandle;
    boost::shared_ptr<ReadHandle> readHandle;
    boost::filesystem::path dirPath;
    boost::filesystem::path derivedPath;
    uint64_t generation;
    uint64_t entryCount;
    bool writable;
    bool open;

    bool InitializePaths(const std::string& dir, std::string* error);
    bool WriteHeader(std::string* error);
    bool LoadHeader(uint64_t* fileSize, std::string* error);
    bool ValidateHeader(const BlockIndexDerivedHeader& header,
                        std::string* error) const;
};

#endif // INNOVA_BLOCKINDEX_DERIVED_STATE_H
