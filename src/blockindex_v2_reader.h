#ifndef INNOVA_BLOCKINDEX_V2_READER_H
#define INNOVA_BLOCKINDEX_V2_READER_H

#include "blockindex_navigation.h"
#include "blockindex_activeindex.h"
#include "blockindex_hashindex.h"
#include "blockindex_generation_lifecycle.h"

#include <list>
#include <map>

// A.8: one immutable, CURRENT-selected generation; pointer-free historical API.
// This reader is semantic/logical read-only. It does not publish/select/build,
// auto-switch CURRENT, use cs_main, or expose CBlockIndex*.
enum BlockIndexV2ReadStatus
{
    BLOCK_INDEX_V2_READ_FOUND = 1,
    BLOCK_INDEX_V2_READ_NOT_FOUND = 2,
    BLOCK_INDEX_V2_READ_CORRUPT = 3,
    BLOCK_INDEX_V2_READ_IO_ERROR = 4,
    BLOCK_INDEX_V2_READ_NOT_OPEN = 5,
};

struct BlockIndexV2ReaderCacheStats
{
    uint64_t capacityBytes;
    uint64_t entries;
    uint64_t hits;
    uint64_t misses;
    uint64_t evictions;
    uint64_t bytesEstimated;
    BlockIndexV2ReaderCacheStats() : capacityBytes(0), entries(0), hits(0), misses(0), evictions(0), bytesEstimated(0) {}
};

struct BlockIndexV2ReaderOptions
{
    uint64_t cacheCapacityBytes;
    BlockIndexV2ReaderOptions() : cacheCapacityBytes(64ULL * 1024ULL * 1024ULL) {}
};

class BlockIndexV2Reader
{
public:
    BlockIndexV2Reader();
    ~BlockIndexV2Reader();
    bool Open(const std::string& root, const BlockIndexV2ReaderOptions& options, std::string* error);
    void Close();
    bool IsOpen() const;
    uint64_t Generation() const;
    uint64_t RecordCount() const;
    BlockIndexSnapshot GetTip() const;
    bool CurrentSelectionChanged(std::string* error) const;
    BlockIndexV2ReaderCacheStats CacheStats() const;
    BlockIndexV2ReadStatus GetRecordById(BlockIndexId id, BlockIndexSnapshot* out, std::string* error) const;
    BlockIndexV2ReadStatus LookupByHash(const uint256& hash, BlockIndexSnapshot* out, std::string* error) const;
    BlockIndexV2ReadStatus GetActiveByHeight(int height, BlockIndexSnapshot* out, std::string* error) const;
    BlockIndexV2ReadStatus GetNextActive(BlockIndexId id, BlockIndexSnapshot* out, std::string* error) const;
    BlockIndexV2ReadStatus GetParent(BlockIndexId id, BlockIndexSnapshot* out, std::string* error) const;
    BlockIndexV2ReadStatus GetAncestor(BlockIndexId id, int targetHeight, BlockIndexSnapshot* out, std::string* error) const;
    BlockIndexV2ReadStatus GetStakeModifierTime(BlockIndexId id, int64_t* out, std::string* error) const;
    BlockIndexV2ReadStatus GetStakeModifierChecksum(BlockIndexId id, unsigned int* out, std::string* error) const;
    BlockIndexV2ReadStatus GetStakingMetadata(BlockIndexId id, BlockIndexStakingMetadata* out, std::string* error) const;
    BlockIndexV2ReadStatus FindFork(BlockIndexId a, BlockIndexId b, BlockIndexSnapshot* out, std::string* error) const;

private:
    struct CacheEntry { BlockIndexSnapshot snapshot; std::list<BlockIndexId>::iterator lru; CacheEntry() {} };
    mutable CCriticalSection cs;
    bool open;
    std::string rootPath;
    std::string generationPath;
    uint64_t generation;
    FixedBlockIndexManifest manifest;
    FixedBlockIndexStore store;
    BlockIndexActiveIndex active;
    BlockIndexHashIndex hashIndex;
    uint64_t cacheCapacity;
    mutable std::map<BlockIndexId, CacheEntry> cache;
    mutable std::list<BlockIndexId> lru;
    mutable BlockIndexV2ReaderCacheStats stats;
    BlockIndexSnapshot SnapshotFromRecord(BlockIndexId id, const BlockIndexRecord& record, bool activeRecord) const;
    void CachePut(const BlockIndexSnapshot& value) const;
};

#endif
