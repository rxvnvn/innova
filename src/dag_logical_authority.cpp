#include "dag_logical_authority.h"

#include "dag_source_binding_verifier.h"
#include "main.h"
#include "serialize.h"

#include <cstring>

namespace {
static bool Fail(std::string* error, const std::string& text)
{
    if (error) *error = text;
    return false;
}
static void Clear(std::string* error) { if (error) error->clear(); }
}

DagLogicalAuthority::DagLogicalAuthority()
    : open_(false), generation_(0), cacheCapacity_(0), db_(NULL), reader_(NULL), peakCache_(0)
{
}

DagLogicalAuthority::~DagLogicalAuthority()
{
    Close();
}

void DagLogicalAuthority::Close()
{
    LOCK(cs_);
    if (db_)
    {
        delete db_;
        db_ = NULL;
    }
    open_ = false;
    generation_ = 0;
    cacheCapacity_ = 0;
    reader_ = NULL;
    cache_.clear();
    lru_.clear();
    peakCache_ = 0;
}

bool DagLogicalAuthority::Open(const std::string& dagLinksDir,
                               const BlockIndexV2Reader& reader,
                               size_t cacheCapacity,
                               const std::string& tempParent,
                               std::string* error)
{
    Close();
    if (!reader.IsOpen()) return Fail(error, "DAG authority: V2 reader is not open");
    if (cacheCapacity == 0) return Fail(error, "DAG authority: cache capacity is zero");
    if (tempParent.empty()) return Fail(error, "DAG authority: verifier temp parent is empty");

    unsigned char expectedDigest[32];
    if (!reader.GetDAGInputDigest(expectedDigest, error))
        return false;

    DagSourceBindingVerifierOptions options;
    options.tempParent = tempParent;
    DagSourceBindingResult binding = DagSourceBindingVerifier::Verify(
        dagLinksDir, expectedDigest, options);
    if (binding.status != DAG_SOURCE_BINDING_VERIFIED)
    {
        return Fail(error, std::string("DAG authority source binding failed: ") + binding.error);
    }

    leveldb::Options dbOptions;
    dbOptions.create_if_missing = false;
    dbOptions.error_if_exists = false;
    dbOptions.paranoid_checks = true;
    leveldb::DB* opened = NULL;
    leveldb::Status status = leveldb::DB::Open(dbOptions, dagLinksDir, &opened);
    if (!status.ok() || !opened)
        return Fail(error, std::string("DAG authority source open failed: ") + status.ToString());

    LOCK(cs_);
    db_ = opened;
    reader_ = &reader;
    generation_ = reader.Generation();
    cacheCapacity_ = cacheCapacity;
    peakCache_ = 0;
    open_ = true;
    Clear(error);
    return true;
}

void DagLogicalAuthority::CachePut(const DagLogicalRecord& record) const
{
    std::map<uint256, CacheEntry>::iterator found = cache_.find(record.hash);
    if (found != cache_.end())
    {
        found->second.record = record;
        lru_.splice(lru_.begin(), lru_, found->second.lru);
        return;
    }
    while (cache_.size() >= cacheCapacity_ && !lru_.empty())
    {
        uint256 evict = lru_.back();
        lru_.pop_back();
        cache_.erase(evict);
    }
    lru_.push_front(record.hash);
    CacheEntry entry;
    entry.record = record;
    entry.lru = lru_.begin();
    cache_[record.hash] = entry;
    if (cache_.size() > peakCache_) peakCache_ = cache_.size();
}

bool DagLogicalAuthority::ReadDAGValue(const uint256& hash, CBlockDAGData* out,
                                       bool* notFound, std::string* error) const
{
    *notFound = false;
    if (!db_) return Fail(error, "DAG authority: source is unavailable");
    CDataStream key(SER_DISK, CLIENT_VERSION);
    key << make_pair(std::string("daglinks"), hash);
    std::string value;
    leveldb::Status status = db_->Get(leveldb::ReadOptions(), key.str(), &value);
    if (status.IsNotFound())
    {
        *notFound = true;
        Clear(error);
        return true;
    }
    if (!status.ok())
        return Fail(error, std::string("DAG authority source read failed: ") + status.ToString());
    try
    {
        CDataStream stream(SER_DISK, CLIENT_VERSION);
        stream.write(value.data(), value.size());
        stream >> *out;
        if (stream.size() != 0)
            return Fail(error, "DAG authority: trailing bytes in DAG record");
    }
    catch (const std::exception& ex)
    {
        return Fail(error, std::string("DAG authority decode failed: ") + ex.what());
    }
    Clear(error);
    return true;
}

DagLogicalAuthorityStatus DagLogicalAuthority::LookupDAG(const uint256& hash,
                                                          DagLogicalRecord* out,
                                                          std::string* error) const
{
    if (!out) { Fail(error, "DAG authority: null DAG output"); return DAG_LOGICAL_AUTHORITY_FAILURE; }
    LOCK(cs_);
    if (!open_ || !db_ || !reader_)
    {
        Fail(error, "DAG authority: not open");
        return DAG_LOGICAL_AUTHORITY_FAILURE;
    }
    if (!reader_->IsOpen() || reader_->Generation() != generation_)
    {
        Fail(error, "DAG authority: V2 generation changed");
        return DAG_LOGICAL_AUTHORITY_FAILURE;
    }
    std::map<uint256, CacheEntry>::const_iterator found = cache_.find(hash);
    if (found != cache_.end())
    {
        *out = found->second.record;
        lru_.splice(lru_.begin(), lru_, found->second.lru);
        Clear(error);
        return DAG_LOGICAL_AUTHORITY_FOUND;
    }
    CBlockDAGData data;
    bool notFound = false;
    if (!ReadDAGValue(hash, &data, &notFound, error))
        return DAG_LOGICAL_AUTHORITY_FAILURE;
    if (notFound)
    {
        *out = DagLogicalRecord();
        return DAG_LOGICAL_AUTHORITY_NOT_FOUND;
    }
    out->hash = hash;
    out->data = data;
    CachePut(*out);
    Clear(error);
    return DAG_LOGICAL_AUTHORITY_FOUND;
}

DagLogicalAuthorityStatus DagLogicalAuthority::LookupBlock(const uint256& hash,
                                                            BlockIndexSnapshot* out,
                                                            bool requireActive,
                                                            std::string* error) const
{
    if (!out) { Fail(error, "DAG authority: null block output"); return DAG_LOGICAL_AUTHORITY_FAILURE; }
    LOCK(cs_);
    if (!open_ || !reader_)
    {
        Fail(error, "DAG authority: not open");
        return DAG_LOGICAL_AUTHORITY_FAILURE;
    }
    if (!reader_->IsOpen() || reader_->Generation() != generation_)
    {
        Fail(error, "DAG authority: V2 generation changed");
        return DAG_LOGICAL_AUTHORITY_FAILURE;
    }
    BlockIndexV2ReadStatus status = reader_->LookupByHash(hash, out, error);
    if (status == BLOCK_INDEX_V2_READ_NOT_FOUND)
    {
        *out = BlockIndexSnapshot();
        return DAG_LOGICAL_AUTHORITY_NOT_FOUND;
    }
    if (status != BLOCK_INDEX_V2_READ_FOUND)
    {
        *out = BlockIndexSnapshot();
        return DAG_LOGICAL_AUTHORITY_FAILURE;
    }
    if (requireActive && !out->fInMainChain)
        return DAG_LOGICAL_AUTHORITY_NOT_ACTIVE;
    Clear(error);
    return DAG_LOGICAL_AUTHORITY_FOUND;
}

DagLogicalAuthorityStatus DagLogicalAuthority::GetDAGScore(const uint256& hash,
                                                            uint256* out,
                                                            std::string* error) const
{
    if (!out) { Fail(error, "DAG authority: null score output"); return DAG_LOGICAL_AUTHORITY_FAILURE; }
    *out = 0;
    BlockIndexSnapshot block;
    DagLogicalRecord record;
    DagLogicalAuthorityStatus dagStatus = LookupDAG(hash, &record, error);
    if (dagStatus != DAG_LOGICAL_AUTHORITY_FOUND &&
        dagStatus != DAG_LOGICAL_AUTHORITY_NOT_FOUND)
        return DAG_LOGICAL_AUTHORITY_FAILURE;

    DagLogicalAuthorityStatus blockStatus = LookupBlock(hash, &block, false, error);
    if (blockStatus != DAG_LOGICAL_AUTHORITY_FOUND)
    {
        if (dagStatus == DAG_LOGICAL_AUTHORITY_FOUND)
            return DAG_LOGICAL_AUTHORITY_FAILURE;
        return blockStatus;
    }

    // Exact legacy ComputeDAGScore semantics: post-DAG PoS has score zero.
    if (block.height >= FORK_HEIGHT_DAG && block.fProofOfStake)
    {
        *out = 0;
        Clear(error);
        return DAG_LOGICAL_AUTHORITY_FOUND;
    }

    if (dagStatus == DAG_LOGICAL_AUTHORITY_FOUND)
    {
        *out = record.data.nDAGScore;
        return DAG_LOGICAL_AUTHORITY_FOUND;
    }

    // Exact legacy fallback when mapDAGData has no record.
    *out = block.nChainTrust;
    Clear(error);
    return DAG_LOGICAL_AUTHORITY_FOUND;
}

DagLogicalAuthorityStatus DagLogicalAuthority::GetSelectedParent(const uint256& hashBlock,
                                                                  uint256* out,
                                                                  std::string* error) const
{
    if (!out) { Fail(error, "DAG authority: null selected-parent output"); return DAG_LOGICAL_AUTHORITY_FAILURE; }
    *out = 0;
    DagLogicalRecord record;
    DagLogicalAuthorityStatus status = LookupDAG(hashBlock, &record, error);
    if (status != DAG_LOGICAL_AUTHORITY_FOUND)
        return status;
    if (record.data.vDAGParents.empty())
    {
        Clear(error);
        return DAG_LOGICAL_AUTHORITY_NOT_FOUND;
    }

    uint256 best;
    uint256 bestScore = 0;
    for (size_t i = 0; i < record.data.vDAGParents.size(); ++i)
    {
        const uint256& parent = record.data.vDAGParents[i];
        uint256 parentScore = 0;
        DagLogicalRecord parentRecord;
        DagLogicalAuthorityStatus parentDag = LookupDAG(parent, &parentRecord, error);
        if (parentDag == DAG_LOGICAL_AUTHORITY_FOUND)
        {
            parentScore = parentRecord.data.nDAGScore;
        }
        else if (parentDag == DAG_LOGICAL_AUTHORITY_NOT_FOUND)
        {
            BlockIndexSnapshot parentBlock;
            DagLogicalAuthorityStatus parentBlockStatus = LookupBlock(parent, &parentBlock, false, error);
            if (parentBlockStatus == DAG_LOGICAL_AUTHORITY_FOUND)
            {
                if (!(parentBlock.height >= FORK_HEIGHT_DAG && parentBlock.fProofOfStake))
                    parentScore = parentBlock.nChainTrust;
            }
            else if (parentBlockStatus == DAG_LOGICAL_AUTHORITY_NOT_FOUND)
            {
                // Legacy mapDAGData/mapBlockIndex behavior treats absent optional
                // parent state as effective zero score.
                parentScore = 0;
            }
            else
            {
                return DAG_LOGICAL_AUTHORITY_FAILURE;
            }
        }
        else
        {
            return DAG_LOGICAL_AUTHORITY_FAILURE;
        }

        if (parentScore > bestScore ||
            (parentScore == bestScore && (best == uint256(0) || parent < best)))
        {
            bestScore = parentScore;
            best = parent;
        }
    }
    if (best == uint256(0))
    {
        Clear(error);
        return DAG_LOGICAL_AUTHORITY_NOT_FOUND;
    }
    *out = best;
    Clear(error);
    return DAG_LOGICAL_AUTHORITY_FOUND;
}

DagLogicalAuthorityStats DagLogicalAuthority::CacheStats() const
{
    LOCK(cs_);
    DagLogicalAuthorityStats stats;
    stats.capacity = cacheCapacity_;
    stats.current = cache_.size();
    stats.peak = peakCache_;
    return stats;
}
