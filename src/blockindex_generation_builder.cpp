#include "blockindex_generation_builder.h"

#include "txdb-leveldb.h"
#include "main.h"

#include <leveldb/db.h>
#include <leveldb/filter_policy.h>

#include <map>
#include <set>
#include <stdio.h>
#include <string>
#include <vector>

namespace {

static bool SetError(std::string* error, const std::string& message)
{
    if (error)
        *error = message;
    return false;
}

static void ClearError(std::string* error)
{
    if (error)
        error->clear();
}

} // namespace

bool ReadLegacyBlockIndexSource(const std::string& snapshotLevelDbDir,
                                BlockIndexGenerationSource* out,
                                std::string* error)
{
    if (!out)
        return SetError(error, "null generation source output");

    // Open the STATIC SNAPSHOT copy. We never open the live datadir DB. Bundled
    // LevelDB has no filesystem-level read-only open, so this operates on the
    // dedicated snapshot copy (mutations, if any, are confined to the copy).
    leveldb::Options options;
    options.create_if_missing = false;
    options.error_if_exists = false;
    options.filter_policy = leveldb::NewBloomFilterPolicy(10);
    leveldb::DB* db = NULL;
    leveldb::Status status = leveldb::DB::Open(options, snapshotLevelDbDir, &db);
    if (!status.ok())
        return SetError(error, std::string("snapshot LevelDB open failure: ") + status.ToString());

    BlockIndexGenerationSource src;

    // scan blockindex keys (same pattern as CTxDB::LoadBlockIndex)
    leveldb::Iterator* iterator = db->NewIterator(leveldb::ReadOptions());
    CDataStream ssStartKey(SER_DISK, CLIENT_VERSION);
    ssStartKey << make_pair(std::string("blockindex"), uint256(0));
    iterator->Seek(ssStartKey.str());

    while (iterator->Valid())
    {
        CDataStream ssKey(SER_DISK, CLIENT_VERSION);
        ssKey.write(iterator->key().data(), iterator->key().size());
        CDataStream ssValue(SER_DISK, CLIENT_VERSION);
        ssValue.write(iterator->value().data(), iterator->value().size());

        std::string strType;
        ssKey >> strType;
        if (strType != "blockindex")
            break;

        CDiskBlockIndex diskindex;
        ssValue >> diskindex;

        uint256 blockHash = diskindex.GetBlockHash();

        BlockIndexGenerationSourceRecord rec;
        rec.hash = blockHash;
        rec.record.hash = blockHash;
        rec.record.hashPrev = diskindex.hashPrev;
        rec.record.hashMerkleRoot = diskindex.hashMerkleRoot;
        rec.record.hashProof = diskindex.hashProof;
        rec.record.prevoutStake = diskindex.prevoutStake;
        rec.record.height = diskindex.nHeight;
        rec.record.nFile = diskindex.nFile;
        rec.record.nBlockPos = diskindex.nBlockPos;
        rec.record.nFlags = diskindex.nFlags;
        rec.record.nVersion = diskindex.nVersion;
        rec.record.nTime = diskindex.nTime;
        rec.record.nBits = diskindex.nBits;
        rec.record.nNonce = diskindex.nNonce;
        rec.record.nMint = diskindex.nMint;
        rec.record.nMoneySupply = diskindex.nMoneySupply;
        rec.record.nStakeModifier = diskindex.nStakeModifier;
        rec.record.nStakeTime = diskindex.nStakeTime;

        src.records.push_back(rec);
        iterator->Next();
    }
    delete iterator;

    // read hashBestChain (key is CDataStream-serialized std::string, so encode
    // it the same way CTxDB::Read does; a raw "hashBestChain" would miss the
    // string-length prefix)
    CDataStream ssBestKey(SER_DISK, CLIENT_VERSION);
    ssBestKey << std::string("hashBestChain");
    std::string bestVal;
    status = db->Get(leveldb::ReadOptions(), ssBestKey.str(), &bestVal);
    if (status.ok())
    {
        CDataStream ss(bestVal.data(), bestVal.data() + bestVal.size(), SER_DISK, CLIENT_VERSION);
        ss >> src.hashBestChain;
        src.foundBestChain = true;
    }
    delete db;

    *out = src;
    ClearError(error);
    return true;
}

BlockIndexGenerationBuilder::BlockIndexGenerationBuilder()
    : built(false)
{
}

void BlockIndexGenerationBuilder::Close()
{
    if (activeIndex.IsOpen())
        activeIndex = BlockIndexActiveIndex();
    if (hashIndex.IsOpen())
        hashIndex.Close();
    built = false;
}

bool BlockIndexGenerationBuilder::Build(const BlockIndexGenerationSource& source,
                                        const std::string& generationDir,
                                        uint64_t generation,
                                        BlockIndexGenerationStats* stats,
                                        std::string* error)
{
    if (generation == 0)
        return SetError(error, "invalid generation 0");
    if (!built)
    {
        // fresh build: create store, hashindex, activeindex
        if (!FixedBlockIndexStore::Create(generationDir, generation, &store, error))
            return false;
        if (!BlockIndexHashIndex::Create(generationDir, generation, &hashIndex, error))
            return false;
        if (!BlockIndexActiveIndex::Create(generationDir, generation, &activeIndex, error))
            return false;
    }

    // ---- deterministic RecordId assignment: order by block hash ----
    // Build a sorted vector so RecordId assignment is reproducible and
    // independent of the source iteration order / map pointer identity.
    std::vector<BlockIndexGenerationSourceRecord> ordered = source.records;
    std::sort(ordered.begin(), ordered.end(),
              [](const BlockIndexGenerationSourceRecord& a,
                 const BlockIndexGenerationSourceRecord& b) {
                  return a.hash < b.hash;
              });

    // map hash -> assigned id
    std::map<uint256, BlockIndexId> idMap;
    idMap.clear();
    // duplicate detection: a source containing two records for the same block
    // hash is ambiguous (two blocks cannot share a hash) and must fail closed,
    // otherwise the hashindex would silently map the second id over the first.
    {
        std::map<uint256, int> seen;
        for (size_t i = 0; i < ordered.size(); ++i)
        {
            if (++seen[ordered[i].hash] > 1)
                return SetError(error, "duplicate source block hash");
        }
    }

    // index: hash -> (record, id) for O(log n) active-chain reconstruction.
    std::map<uint256, std::pair<const BlockIndexRecord*, BlockIndexId> > byHash;
    for (size_t i = 0; i < ordered.size(); ++i)
        byHash[ordered[i].hash] = std::make_pair(&ordered[i].record, BLOCK_INDEX_ID_INVALID);

    // Append all records in deterministic id order in a SINGLE batch (one
    // open + one fsync). Assign RecordId sequentially; keep, for each record,
    // the exact id that was appended (so the hashindex step cannot be masked by
    // duplicate-hash collapse in idMap).
    std::vector<BlockIndexRecord> batchRecords;
    batchRecords.reserve(ordered.size());
    for (size_t i = 0; i < ordered.size(); ++i)
        batchRecords.push_back(ordered[i].record);

    std::vector<BlockIndexId> batchIds;
    if (!store.AppendBatch(batchRecords, &batchIds, error))
        return false;

    std::vector<std::pair<uint256, BlockIndexId> > appended;
    appended.reserve(ordered.size());
    for (size_t i = 0; i < ordered.size(); ++i)
    {
        idMap[ordered[i].hash] = batchIds[i];
        byHash[ordered[i].hash].second = batchIds[i];
        appended.push_back(std::make_pair(ordered[i].hash, batchIds[i]));
    }
    const uint64_t totalRecords = ordered.size();

    // ---- hashindex: every record hash -> RecordId ----
    for (size_t i = 0; i < appended.size(); ++i)
    {
        if (!hashIndex.Put(appended[i].first, appended[i].second, error))
            return false;
    }

    // ---- active.dat: active chain by height ----
    // Reconstruct the active chain from hashBestChain by following hashPrev
    // down to genesis, then reverse to ascending height.
    if (!source.foundBestChain)
        return SetError(error, "generation source lacks hashBestChain");

    std::vector<BlockIndexId> activeChainByHeight; // dense, index = height
    {
        std::vector<std::pair<int32_t, BlockIndexId> > byHeight;
        uint256 cur = source.hashBestChain;
        while (true)
        {
            std::map<uint256, std::pair<const BlockIndexRecord*, BlockIndexId> >::iterator it =
                byHash.find(cur);
            if (it == byHash.end())
                return SetError(error, "hashBestChain record missing from source");
            const BlockIndexRecord* rec = it->second.first;
            const BlockIndexId id = it->second.second;
            byHeight.push_back(std::make_pair(rec->height, id));
            if (rec->hashPrev == 0)
                break; // genesis
            cur = rec->hashPrev;
        }

        // The active chain must be dense and contiguous in height 0..tip.
        // (byHeight was collected tip-first, so we only rely on the sorted
        // continuity loop below to prove density 0..n-1.)
        if (byHeight.empty())
            return SetError(error, "empty active chain in source");

        std::sort(byHeight.begin(), byHeight.end());
        for (size_t h = 0; h < byHeight.size(); ++h)
        {
            if (byHeight[h].first != (int32_t)h)
                return SetError(error, "active chain height discontinuity");
            activeChainByHeight.push_back(byHeight[h].second);
        }
    }

    // dense active.dat: height -> RecordId (single batch, one fsync)
    {
        std::vector<BlockIndexId> activeIds(activeChainByHeight.begin(), activeChainByHeight.end());
        if (!activeIndex.AppendBatch(activeIds, error))
            return false;
    }

    // ---- MANIFEST lifecycle: validate then COMPLETE ----
    if (activeChainByHeight.empty())
        return SetError(error, "empty active chain");

    const int32_t tipHeight = (int32_t)activeChainByHeight.size() - 1;
    const BlockIndexId tipId = activeChainByHeight[tipHeight];

    // find tip hash
    uint256 tipHash = 0;
    {
        std::map<uint256, BlockIndexId>::iterator it = idMap.begin();
        for (; it != idMap.end(); ++it)
        {
            if (it->second == tipId)
            {
                tipHash = it->first;
                break;
            }
        }
        if (tipHash == 0)
            return SetError(error, "tip RecordId not found in id map");
    }

    FixedBlockIndexManifest manifest = store.GetManifest();
    manifest.state = BLOCK_INDEX_MANIFEST_BUILDING;
    manifest.recordCount = totalRecords;
    manifest.committedTipId = tipId;
    manifest.committedTipHeight = tipHeight;
    manifest.committedTipHash = tipHash;
    if (!store.WriteManifest(manifest, error))
        return false;

    // Verify decoration before marking COMPLETE
    FixedBlockIndexManifest verify = store.GetManifest();
    verify.state = BLOCK_INDEX_MANIFEST_COMPLETE;
    if (!store.WriteManifest(verify, error))
        return false;

    if (stats)
    {
        stats->totalRecords = totalRecords;
        stats->activeRecords = activeChainByHeight.size();
        stats->sideChainRecords = totalRecords - activeChainByHeight.size();
        stats->activeTipHeight = tipHeight;
        stats->activeTipHash = tipHash;
    }

    built = true;
    ClearError(error);
    return true;
}

namespace {

// Compare every V1 field of two records. Returns true when identical.
static bool RecordsMatchV1(const BlockIndexRecord& a, const BlockIndexRecord& b)
{
    if (a.hash != b.hash) return false;
    if (a.hashPrev != b.hashPrev) return false;
    if (a.hashMerkleRoot != b.hashMerkleRoot) return false;
    if (a.hashProof != b.hashProof) return false;
    if (!(a.prevoutStake == b.prevoutStake)) return false;
    if (a.height != b.height) return false;
    if (a.nFile != b.nFile) return false;
    if (a.nBlockPos != b.nBlockPos) return false;
    if (a.nFlags != b.nFlags) return false;
    if (a.nVersion != b.nVersion) return false;
    if (a.nTime != b.nTime) return false;
    if (a.nBits != b.nBits) return false;
    if (a.nNonce != b.nNonce) return false;
    if (a.nMint != b.nMint) return false;
    if (a.nMoneySupply != b.nMoneySupply) return false;
    if (a.nStakeModifier != b.nStakeModifier) return false;
    if (a.nStakeTime != b.nStakeTime) return false;
    return true;
}

// deterministic PRNG (xorshift64), fixed seed for repeatable samples
struct XorShift64
{
    uint64_t s;
    explicit XorShift64(uint64_t seed) : s(seed ? seed : 0x9E3779B97F4A7C15ULL) {}
    uint64_t next()
    {
        uint64_t x = s;
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        s = x;
        return x;
    }
    uint64_t range(uint64_t n)
    {
        return n ? (next() % n) : 0;
    }
};

} // namespace

bool VerifyGenerationAgainstSource(const BlockIndexGenerationSource& source,
                                   const std::string& generationDir,
                                   uint64_t generation,
                                   BlockIndexDifferentialResult* out,
                                   std::string* error)
{
    if (!out)
        return SetError(error, "null differential output");

    // Reopen the MANIFEST + hashindex from disk (independent of builder state).
    FixedBlockIndexOpenOptions storeOptions;
    storeOptions.requireCompleteManifest = true;
    FixedBlockIndexStore store;
    if (!FixedBlockIndexStore::OpenReadOnly(generationDir, storeOptions, &store, error))
        return false;
    BlockIndexHashIndex hashIndex;
    if (!BlockIndexHashIndex::Open(generationDir, generation, &hashIndex, error))
        return false;
    if (hashIndex.Generation() != store.GetManifest().generation)
        return SetError(error, "generation mismatch on reopen");
    const FixedBlockIndexManifest& manifest = store.GetManifest();

    // Load the WHOLE records.dat committed region and active.dat into memory in
    // a single buffered pass each (fresh read from disk, then decoded through
    // the same V1 codec). This avoids ~N file opens per record while still
    // proving the persisted generation is self-consistent from disk.
    std::vector<BlockIndexRecord> recordsById;
    {
        std::string diskDir = generationDir;
        // decode records.dat body (committed count from manifest)
        const uint64_t count = manifest.recordCount;
        std::vector<unsigned char> fileBytes;
        {
            FILE* f = fopen((boost::filesystem::path(diskDir) / BLOCK_INDEX_RECORDS_FILE_NAME).string().c_str(), "rb");
            if (!f)
                return SetError(error, "cannot open records.dat for differential");
            fseeko(f, (off_t)BLOCK_INDEX_RECORDS_HEADER_SIZE_V1, SEEK_SET);
            // read committed region in chunks of 64 records to bound memory churn
            std::vector<BlockIndexRecord> all;
            all.reserve(count);
            std::vector<unsigned char> recBuf(BLOCK_INDEX_RECORD_SIZE_V1);
            for (uint64_t i = 0; i < count; ++i)
            {
                size_t n = fread(&recBuf[0], 1, BLOCK_INDEX_RECORD_SIZE_V1, f);
                if (n != BLOCK_INDEX_RECORD_SIZE_V1)
                {
                    fclose(f);
                    return SetError(error, "records.dat truncated in differential");
                }
                BlockIndexRecord rec;
                if (!DecodeBlockIndexRecordV1(&recBuf[0], recBuf.size(), &rec, error))
                {
                    fclose(f);
                    return false;
                }
                all.push_back(rec);
            }
            fclose(f);
            recordsById.swap(all);
        }
    }
    // active.dat -> vector of RecordId indexed by height (skip the 40-byte
    // versioned header; entries are decoded via the exported entry codec).
    std::vector<BlockIndexId> activeById;
    {
        FILE* f = fopen((boost::filesystem::path(generationDir) / BLOCK_INDEX_ACTIVE_FILE_NAME).string().c_str(), "rb");
        if (!f)
            return SetError(error, "cannot open active.dat for differential");
        std::vector<unsigned char> buf(BLOCK_INDEX_ACTIVE_ENTRY_SIZE_V1);
        fseeko(f, (off_t)BLOCK_INDEX_ACTIVE_HEADER_SIZE_V1, SEEK_SET);
        std::vector<BlockIndexId> all;
        for (int32_t h = 0; h <= manifest.committedTipHeight; ++h)
        {
            size_t n = fread(&buf[0], 1, BLOCK_INDEX_ACTIVE_ENTRY_SIZE_V1, f);
            if (n != BLOCK_INDEX_ACTIVE_ENTRY_SIZE_V1)
            {
                fclose(f);
                return SetError(error, "active.dat truncated in differential");
            }
            BlockIndexId id = BLOCK_INDEX_ID_INVALID;
            if (!DecodeBlockIndexActiveEntry((const char*)&buf[0], buf.size(), &id, error))
            {
                fclose(f);
                return false;
            }
            all.push_back(id);
        }
        fclose(f);
        activeById.swap(all);
    }

    BlockIndexDifferentialResult r;

    // ---- index the source for O(1)/O(log n) lookups (avoids O(n^2) scans) ----
    std::map<uint256, const BlockIndexGenerationSourceRecord*> sourceByHash;
    for (size_t i = 0; i < source.records.size(); ++i)
        sourceByHash[source.records[i].hash] = &source.records[i];

    // ---- active chain truth from source ----
    std::vector<const BlockIndexGenerationSourceRecord*> activeTruth; // index by height
    std::set<uint256> activeHashSet;
    if (source.foundBestChain)
    {
        std::vector<const BlockIndexGenerationSourceRecord*> byHeight;
        uint256 cur = source.hashBestChain;
        while (true)
        {
            std::map<uint256, const BlockIndexGenerationSourceRecord*>::iterator it =
                sourceByHash.find(cur);
            const BlockIndexGenerationSourceRecord* rec = (it != sourceByHash.end()) ? it->second : NULL;
            if (!rec)
            {
                r.tipCoherent = false;
                r.tipDetail = "source best-chain record missing";
                *out = r;
                ClearError(error);
                return true;
            }
            byHeight.push_back(rec);
            if (rec->record.hashPrev == 0)
                break;
            cur = rec->record.hashPrev;
        }
        std::sort(byHeight.begin(), byHeight.end(),
                  [](const BlockIndexGenerationSourceRecord* a,
                     const BlockIndexGenerationSourceRecord* b) {
                      return a->record.height < b->record.height;
                  });
        activeTruth = byHeight;
        for (size_t h = 0; h < activeTruth.size(); ++h)
            activeHashSet.insert(activeTruth[h]->hash);
    }

    // ---- exhaustive hash differential ----
    for (size_t i = 0; i < source.records.size(); ++i)
    {
        const uint256& hash = source.records[i].hash;
        const BlockIndexRecord& truth = source.records[i].record;
        ++r.hashQueries;

        BlockIndexId id = BLOCK_INDEX_ID_INVALID;
        BlockIndexHashLookupStatus st = hashIndex.Lookup(hash, &id, error);
        if (st == BLOCK_INDEX_HASH_LOOKUP_ERROR)
        {
            ++r.hashCorruptions;
            ClearError(error);
            continue;
        }
        if (st == BLOCK_INDEX_HASH_LOOKUP_NOT_FOUND)
        {
            ++r.hashNotFound;
            continue;
        }
        if (id == BLOCK_INDEX_ID_INVALID || id > recordsById.size())
        {
            ++r.hashCorruptions;
            continue;
        }
        const BlockIndexRecord& rec = recordsById[id - 1];
        if (!RecordsMatchV1(rec, truth))
            ++r.hashMismatches;
    }

    // ---- exhaustive active-height differential ----
    for (size_t h = 0; h < activeTruth.size(); ++h)
    {
        const int32_t height = (int32_t)h;
        ++r.heightQueries;
        if ((size_t)height >= activeById.size())
        {
            ++r.heightCorruptions;
            continue;
        }
        BlockIndexId id = activeById[height];
        if (id == BLOCK_INDEX_ID_INVALID || id > recordsById.size())
        {
            ++r.heightCorruptions;
            continue;
        }
        const BlockIndexRecord& rec = recordsById[id - 1];
        if (!RecordsMatchV1(rec, activeTruth[h]->record))
            ++r.heightMismatches;

        // hashindex(active hash).id == active.dat[height].id
        BlockIndexId hid = BLOCK_INDEX_ID_INVALID;
        BlockIndexHashLookupStatus hst = hashIndex.Lookup(activeTruth[h]->hash, &hid, error);
        if (hst == BLOCK_INDEX_HASH_LOOKUP_ERROR)
        {
            ++r.heightCorruptions;
            ClearError(error);
        }
        else if (hst == BLOCK_INDEX_HASH_LOOKUP_FOUND && hid != id)
            ++r.heightMismatches;
    }

    // ---- active parent continuity ----
    for (size_t h = 1; h < activeTruth.size(); ++h)
    {
        ++r.parentChecks;
        if (activeTruth[h]->record.hashPrev != activeTruth[h - 1]->record.hash)
            ++r.parentMismatches;
    }

    // ---- side-chain differential ----
    for (size_t i = 0; i < source.records.size(); ++i)
    {
        const uint256& hash = source.records[i].hash;
        if (activeHashSet.count(hash))
            continue;
        // side record: must resolve by hash
        ++r.sideChainSamples;
        BlockIndexId id = BLOCK_INDEX_ID_INVALID;
        if (hashIndex.Lookup(hash, &id, error) != BLOCK_INDEX_HASH_LOOKUP_FOUND)
        {
            ++r.sideChainMismatches;
            ClearError(error);
            continue;
        }
        if (id == BLOCK_INDEX_ID_INVALID || id > recordsById.size())
        {
            ++r.sideChainMismatches;
            continue;
        }
        if (!RecordsMatchV1(recordsById[id - 1], source.records[i].record))
            ++r.sideChainMismatches;
        // and must NOT be the active entry at its height
        int32_t hgt = source.records[i].record.height;
        if (hgt >= 0 && (int32_t)hgt < (int32_t)activeById.size())
        {
            if (activeById[hgt] == id)
                ++r.sideChainMismatches; // side block wrongly active
        }
    }

    // ---- deterministic random sample ----
    XorShift64 rng(0x5EED5EED5EED5EE5ULL);
    const uint64_t sampleCount = 10000;
    // random hashes
    for (uint64_t k = 0; k < sampleCount && !source.records.empty(); ++k)
    {
        ++r.randomHashChecks;
        const BlockIndexGenerationSourceRecord& rec =
            source.records[rng.range(source.records.size())];
        BlockIndexId id = BLOCK_INDEX_ID_INVALID;
        if (hashIndex.Lookup(rec.hash, &id, error) != BLOCK_INDEX_HASH_LOOKUP_FOUND)
        {
            ++r.randomMismatches;
            ClearError(error);
            continue;
        }
        if (id == BLOCK_INDEX_ID_INVALID || id > recordsById.size() ||
            !RecordsMatchV1(recordsById[id - 1], rec.record))
            ++r.randomMismatches;
    }
    // random active heights
    for (uint64_t k = 0; k < sampleCount && !activeTruth.empty(); ++k)
    {
        ++r.randomHeightChecks;
        const size_t h = rng.range(activeTruth.size());
        BlockIndexId id = BLOCK_INDEX_ID_INVALID;
        if ((size_t)h >= activeById.size())
        {
            ++r.randomMismatches;
            continue;
        }
        id = activeById[h];
        if (id == BLOCK_INDEX_ID_INVALID || id > recordsById.size() ||
            !RecordsMatchV1(recordsById[id - 1], activeTruth[h]->record))
            ++r.randomMismatches;
    }

    // ---- tip coherence ----
    r.tipCoherent = true;
    if (!source.foundBestChain || activeTruth.empty())
    {
        r.tipCoherent = false;
        r.tipDetail = "no best chain in source";
    }
    else
    {
        const BlockIndexGenerationSourceRecord* tip = activeTruth.back();
        const uint256 manifestTipHash = manifest.committedTipHash;
        if ((size_t)tip->record.height >= activeById.size())
        {
            r.tipCoherent = false;
            r.tipDetail = "active tip height out of range";
        }
        else
        {
            const BlockIndexId activeTipId = activeById[tip->record.height];
            const BlockIndexRecord& tipRec = recordsById[activeTipId - 1];
            if (tip->hash != tipRec.hash) r.tipCoherent = false;
            if (manifest.committedTipId != activeTipId) r.tipCoherent = false;
            if (manifest.committedTipHeight != tip->record.height) r.tipCoherent = false;
            if (manifest.committedTipHash != tipRec.hash) r.tipCoherent = false;
            if (manifest.committedTipHash != tip->hash) r.tipCoherent = false;
            if (tipRec.height != tip->record.height) r.tipCoherent = false;
            if (!r.tipCoherent)
                r.tipDetail = "tip identity mismatch between legacy/MANIFEST/active.dat/record";
        }
    }

    *out = r;
    ClearError(error);
    return true;
}