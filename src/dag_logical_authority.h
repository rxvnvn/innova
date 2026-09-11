#ifndef INNOVA_DAG_LOGICAL_AUTHORITY_H
#define INNOVA_DAG_LOGICAL_AUTHORITY_H

#include "blockindex_v2_reader.h"
#include "dag.h"
#include "sync.h"

#include <leveldb/db.h>
#include <list>
#include <map>
#include <string>

// R1 typed outcomes. NOT_FOUND is ordinary absence; all verifier/V2 integrity
// failures are AUTHORITY_FAILURE and never fall back to legacy topology.
enum DagLogicalAuthorityStatus
{
    DAG_LOGICAL_AUTHORITY_FOUND = 0,
    DAG_LOGICAL_AUTHORITY_NOT_FOUND,
    DAG_LOGICAL_AUTHORITY_NOT_ACTIVE,
    DAG_LOGICAL_AUTHORITY_FAILURE
};

struct DagLogicalRecord
{
    uint256 hash;
    CBlockDAGData data;
};

struct DagLogicalAuthorityStats
{
    size_t capacity;
    size_t current;
    size_t peak;
    DagLogicalAuthorityStats() : capacity(0), current(0), peak(0) {}
};

// Read-only generation-bound DAG logical authority. It is deliberately not a
// CDAGManager replacement and has no historical CBlockIndex/pointer surface.
class DagLogicalAuthority
{
public:
    DagLogicalAuthority();
    ~DagLogicalAuthority();

    bool Open(const std::string& dagLinksDir,
              const BlockIndexV2Reader& reader,
              size_t cacheCapacity,
              const std::string& tempParent,
              std::string* error);
    void Close();
    bool IsOpen() const { return open_; }
    uint64_t Generation() const { return generation_; }

    DagLogicalAuthorityStatus LookupDAG(const uint256& hash,
                                        DagLogicalRecord* out,
                                        std::string* error) const;
    DagLogicalAuthorityStatus LookupBlock(const uint256& hash,
                                          BlockIndexSnapshot* out,
                                          bool requireActive,
                                          std::string* error) const;
    DagLogicalAuthorityStatus GetDAGScore(const uint256& hash,
                                          uint256* out,
                                          std::string* error) const;
    DagLogicalAuthorityStatus GetSelectedParent(const uint256& hashBlock,
                                                uint256* out,
                                                std::string* error) const;
    DagLogicalAuthorityStats CacheStats() const;

private:
    struct CacheEntry
    {
        DagLogicalRecord record;
        std::list<uint256>::iterator lru;
    };

    bool open_;
    uint64_t generation_;
    size_t cacheCapacity_;
    mutable leveldb::DB* db_;
    const BlockIndexV2Reader* reader_;
    mutable CCriticalSection cs_;
    mutable std::map<uint256, CacheEntry> cache_;
    mutable std::list<uint256> lru_;
    mutable size_t peakCache_;

    void CachePut(const DagLogicalRecord& record) const;
    bool ReadDAGValue(const uint256& hash, CBlockDAGData* out, bool* notFound,
                      std::string* error) const;
};

#endif
