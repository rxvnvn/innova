#ifndef INNOVA_BLOCKINDEX_HASHINDEX_H
#define INNOVA_BLOCKINDEX_HASHINDEX_H

#include "fixed_blockindex_store.h"

#include <stdint.h>
#include <string>
#include <utility>

static const uint32_t BLOCK_INDEX_HASHINDEX_SCHEMA_VERSION = 1;
static const char* const BLOCK_INDEX_HASHINDEX_DIR_NAME = "hashindex";
static const char* const BLOCK_INDEX_HASHINDEX_KIND = "INNBHIX1";
static const char BLOCK_INDEX_HASH_KEY_PREFIX = 'h';
static const size_t BLOCK_INDEX_HASH_KEY_SIZE = 33;
static const size_t BLOCK_INDEX_HASH_VALUE_SIZE = 8;

enum BlockIndexHashLookupStatus
{
    BLOCK_INDEX_HASH_LOOKUP_FOUND = 1,
    BLOCK_INDEX_HASH_LOOKUP_NOT_FOUND = 2,
    BLOCK_INDEX_HASH_LOOKUP_ERROR = 3,
};

bool EncodeBlockIndexHashKey(const uint256& hash, std::string* out, std::string* error);
bool EncodeBlockIndexRecordIdValue(BlockIndexId id, std::string* out, std::string* error);
bool DecodeBlockIndexRecordIdValue(const char* data, size_t size, BlockIndexId* out, std::string* error);

class BlockIndexHashIndex
{
public:
    BlockIndexHashIndex();
    ~BlockIndexHashIndex();
    BlockIndexHashIndex(const BlockIndexHashIndex&) = delete;
    BlockIndexHashIndex& operator=(const BlockIndexHashIndex&) = delete;
    BlockIndexHashIndex(BlockIndexHashIndex&& other);
    BlockIndexHashIndex& operator=(BlockIndexHashIndex&& other);

    static bool Create(const std::string& generationDir, uint64_t generation, BlockIndexHashIndex* out, std::string* error);
    // Open validates metadata and generation and then prohibits wrapper writes.
    // Important: bundled LevelDB does NOT expose a filesystem-level read-only
    // open mode, so this is a logical/API-level read-only contract only.
    static bool Open(const std::string& generationDir, uint64_t expectedGeneration, BlockIndexHashIndex* out, std::string* error);

    bool Put(const uint256& hash, BlockIndexId id, std::string* error);
    BlockIndexHashLookupStatus Lookup(const uint256& hash, BlockIndexId* outId, std::string* error) const;

    void Close();
    uint64_t Generation() const;
    bool IsOpen() const;
    bool IsLogicalReadOnly() const;

private:
    struct SharedState;
    SharedState* state;

    bool OpenInternal(const std::string& generationDir, uint64_t generation, bool create, bool logicalReadOnly, std::string* error);
    bool WriteMetadata(std::string* error);
    bool ReadAndValidateMetadata(uint64_t expectedGeneration, std::string* error);
};

class FixedBlockIndexShadowLookup
{
public:
    FixedBlockIndexShadowLookup();

    static bool Open(const std::string& generationDir, FixedBlockIndexShadowLookup* out, std::string* error);

    BlockIndexHashLookupStatus LookupByHash(const uint256& hash, BlockIndexRecord* outRecord, BlockIndexId* outId, std::string* error) const;

private:
    FixedBlockIndexStore store;
    BlockIndexHashIndex hashIndex;
    bool open;
};

#endif
