#ifndef INNOVA_BLOCKINDEX_ACTIVEINDEX_H
#define INNOVA_BLOCKINDEX_ACTIVEINDEX_H

#include "fixed_blockindex_store.h"

#include <stdint.h>
#include <string>

// Phase A.4 active-chain density/shadow foundation.
//
// active.dat is a dense height -> RecordId mapping for the *active* chain.
// Every active-chain height h in [0, committedTipHeight] has exactly one
// contiguous, non-sparse entry. A RecordId of 0 is invalid and never stored
// inside the committed region. The physical file may hold an uncommitted
// tail beyond the MANIFEST committed tip (an in-progress reorg), mirroring
// records.dat semantics; the shadow consumer refuses to serve it.

static const uint32_t BLOCK_INDEX_ACTIVE_SCHEMA_VERSION = 1;
static const uint32_t BLOCK_INDEX_ACTIVE_HEADER_SIZE_V1 = 40;
static const uint32_t BLOCK_INDEX_ACTIVE_ENTRY_SIZE_V1 = 8;
static const char* const BLOCK_INDEX_ACTIVE_FILE_NAME = "active.dat";

// Explicit, ABI-independent 8-byte little-endian RecordId entry codec.
bool EncodeBlockIndexActiveEntry(BlockIndexId id, std::string* out, std::string* error);
bool DecodeBlockIndexActiveEntry(const char* data, size_t size, BlockIndexId* out, std::string* error);

enum BlockIndexActiveLookupStatus
{
    BLOCK_INDEX_ACTIVE_LOOKUP_FOUND = 1,
    BLOCK_INDEX_ACTIVE_LOOKUP_NOT_FOUND = 2,
    BLOCK_INDEX_ACTIVE_LOOKUP_ERROR = 3,
};

class BlockIndexActiveIndex
{
public:
    BlockIndexActiveIndex();

    // Create writes the versioned active.dat header only (empty dense index).
    static bool Create(const std::string& dir, uint64_t generation, BlockIndexActiveIndex* out, std::string* error);
    // Open validates header, dense physical layout, and generation, then
    // prohibits wrapper mutations (logical read-only).
    static bool Open(const std::string& dir, uint64_t expectedGeneration, BlockIndexActiveIndex* out, std::string* error);

    // Append requires the next height to be exactly physicalHeight + 1
    // (dense, no sparse mappings). id must be a non-zero RecordId.
    bool Append(BlockIndexId id, int32_t height, std::string* error);
    // Batch dense append: appends entries for heights physicalHeight+1 ..
    // physicalHeight+ids.size() with a SINGLE open + single fsync. Byte-identical
    // to sequential Append calls.
    bool AppendBatch(const std::vector<BlockIndexId>& ids, std::string* error);
    // TruncateTo keeps a dense prefix entries[0..height] (height >= -1) and
    // discards every entry above it. Used to isolate a reorg to the affected
    // active tail only.
    bool TruncateTo(int32_t height, std::string* error);
    // OpenWritable opens an existing active.dat for mutation (reorg truncate +
    // reappend) while still validating header + generation + dense layout.
    static bool OpenWritable(const std::string& dir, uint64_t expectedGeneration, BlockIndexActiveIndex* out, std::string* error);
    bool ReadEntry(int32_t height, BlockIndexId* outId, std::string* error) const;

    int64_t PhysicalHeight() const; // highest physically present height, or -1 when empty
    uint64_t Generation() const;
    bool IsOpen() const;
    bool IsLogicalReadOnly() const;

private:
    boost::filesystem::path dirPath;
    boost::filesystem::path activePath;
    uint64_t generation;
    int64_t physicalHeight;
    bool writable;
    bool open;

    bool InitializePaths(const std::string& dir, std::string* error);
    bool WriteHeader(std::string* error);
    bool LoadHeader(uint64_t* fileSize, std::string* error);
    bool OpenExisting(uint64_t expectedGeneration, bool makeWritable, std::string* error);
};

class FixedBlockIndexShadowActiveLookup
{
public:
    FixedBlockIndexShadowActiveLookup();

    // Open binds records.dat + MANIFEST generation to active.dat generation,
    // validates dense non-sparse entries across the committed region, enforces
    // the committed-record boundary and MANIFEST committed-tip coherence.
    static bool Open(const std::string& generationDir, FixedBlockIndexShadowActiveLookup* out, std::string* error);

    // height -> hashindex-free path: active.dat RecordId -> records.dat record,
    // enforcing decoded_record.height == requested height.
    BlockIndexActiveLookupStatus LookupByHeight(int32_t height, BlockIndexRecord* outRecord, BlockIndexId* outId, std::string* error) const;

private:
    FixedBlockIndexStore store;
    BlockIndexActiveIndex active;
    bool open;
};

#endif