// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.

#include "blockindex_generation_builder_lm.h"

#include "txdb-leveldb.h"
#include "main.h"
#include "kernel.h"
#include "dag.h"

#include <leveldb/db.h>
#include <leveldb/filter_policy.h>

#include <boost/filesystem.hpp>

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace fs = boost::filesystem;

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

// Read a single CDiskBlockIndex record from a snapshot LevelDB cursor into a
// BlockIndexRecord (the same field mapping as ReadLegacyBlockIndexSource).
static bool DecodeToRecord(const leveldb::Slice& value, BlockIndexRecord* out)
{
    CDataStream ssValue(SER_DISK, CLIENT_VERSION);
    ssValue.write(value.data(), value.size());
    CDiskBlockIndex diskindex;
    ssValue >> diskindex;
    uint256 blockHash = diskindex.GetBlockHash();
    BlockIndexRecord& r = *out;
    r.hash = blockHash;
    r.hashPrev = diskindex.hashPrev;
    r.hashMerkleRoot = diskindex.hashMerkleRoot;
    r.hashProof = diskindex.hashProof;
    r.prevoutStake = diskindex.prevoutStake;
    r.height = diskindex.nHeight;
    r.nFile = diskindex.nFile;
    r.nBlockPos = diskindex.nBlockPos;
    r.nFlags = diskindex.nFlags;
    r.nVersion = diskindex.nVersion;
    r.nTime = diskindex.nTime;
    r.nBits = diskindex.nBits;
    r.nNonce = diskindex.nNonce;
    r.nMint = diskindex.nMint;
    r.nMoneySupply = diskindex.nMoneySupply;
    r.nStakeModifier = diskindex.nStakeModifier;
    r.nStakeTime = diskindex.nStakeTime;
    return true;
}

} // namespace

BlockIndexGenerationBuilderLM::BlockIndexGenerationBuilderLM()
{
}

BlockIndexGenerationBuilderLM::~BlockIndexGenerationBuilderLM()
{
}

bool BlockIndexGenerationBuilderLM::Build(const std::string& snapshotLevelDbDir,
                                          const std::string& blockDataDir,
                                          uint64_t generation,
                                          const std::string& stagingDir,
                                          std::string* error)
{
    // M2: Open snapshot read-only (dedicated copy, never the live datadir).
    leveldb::Options options;
    options.create_if_missing = false;
    options.error_if_exists = false;
    options.filter_policy = leveldb::NewBloomFilterPolicy(10);
    leveldb::DB* db = NULL;
    leveldb::Status status = leveldb::DB::Open(options, snapshotLevelDbDir, &db);
    if (!status.ok())
        return SetError(error, std::string("lm: snapshot LevelDB open failure: ") +
                               status.ToString());

    // Open the shared writer (bound memory).
    if (!writer_.OpenTarget(stagingDir, generation, error))
    {
        delete db;
        return false;
    }

    // Read hashBestChain (authoritative active-chain tip) before we stream.
    uint256 hashBestChain = 0;
    {
        CDataStream ssBestKey(SER_DISK, CLIENT_VERSION);
        ssBestKey << std::string("hashBestChain");
        std::string bestVal;
        leveldb::Status bs = db->Get(leveldb::ReadOptions(), ssBestKey.str(), &bestVal);
        if (!bs.ok())
        {
            delete db;
            return SetError(error, "lm: hashBestChain not found in snapshot");
        }
        CDataStream ss(bestVal.data(), bestVal.data() + bestVal.size(), SER_DISK, CLIENT_VERSION);
        ss >> hashBestChain;
    }

    // ---- Pass 1: scan ALL blockindex keys, decode, stream to records + a temp
    // external sort key file so RecordId assignment order matches the legacy
    // builder (hash-sorted) for byte parity. ----
    // To keep memory O(1) in N, we write (hash, record) to a temp file instead
    // of accumulating in a vector, then external-sort by hash. The writer's
    // AppendRecord assigns RecordIds sequentially in the order we feed it.
    const fs::path tmpDir = fs::path(stagingDir) / ".lm-tmp";
    boost::system::error_code ec;
    if (!fs::create_directories(tmpDir, ec) && ec)
    {
        delete db;
        return SetError(error, "lm: create tmp dir failed");
    }
    const std::string recordsTmp = (tmpDir / "records.candidates").string();

    {
        FILE* rf = fopen(recordsTmp.c_str(), "wb");
        if (!rf)
        {
            delete db;
            return SetError(error, "lm: open records candidate file failed");
        }
        leveldb::Iterator* it = db->NewIterator(leveldb::ReadOptions());
        CDataStream ssStartKey(SER_DISK, CLIENT_VERSION);
        ssStartKey << make_pair(std::string("blockindex"), uint256(0));
        it->Seek(ssStartKey.str());
        uint64_t count = 0;
        while (it->Valid())
        {
            CDataStream ssKey(SER_DISK, CLIENT_VERSION);
            ssKey.write(it->key().data(), it->key().size());
            std::string strType;
            ssKey >> strType;
            if (strType != "blockindex")
                break;
            BlockIndexRecord rec;
            if (!DecodeToRecord(it->value(), &rec))
            {
                delete it;
                fclose(rf);
                delete db;
                return SetError(error, "lm: decode blockindex failed");
            }
            // write (32-byte hash || 228-byte record) to temp
            if (fwrite(rec.hash.begin(), 1, 32, rf) != 32 ||
                fwrite(&rec, 1, sizeof(BlockIndexRecord), rf) != sizeof(BlockIndexRecord))
            {
                delete it;
                fclose(rf);
                delete db;
                return SetError(error, "lm: write candidate failed");
            }
            ++count;
            it->Next();
        }
        delete it;
        fclose(rf);
        (void)count;
    }
    delete db; // snapshot DB can close now; we have the records on disk

    // ---- External sort of candidates by hash (bounded RAM: merge chunks). ----
    // Read candidates in fixed-size chunks, sort each in RAM, write sorted
    // runs to temp; then k-way merge. For simplicity and correctness (N small in
    // tests; for real N we stream), we do a chunked inverted-index that keeps
    // only ONE sorted run per chunk and merges via a min-heap of chunk heads.
    // This keeps peak RAM = O(chunk), independent of N.
    const size_t kChunk = 65536;
    std::vector<std::string> sortedRuns;
    {
        FILE* rf = fopen(recordsTmp.c_str(), "rb");
        if (!rf)
            return SetError(error, "lm: reopen candidates for sort failed");
        std::vector<std::pair<uint256, BlockIndexRecord> > chunk;
        chunk.reserve(kChunk);
        for (;;)
        {
            BlockIndexRecord rec;
            uint256 h;
            size_t n = fread(h.begin(), 1, 32, rf);
            if (n != 32)
                break;
            if (fread(&rec, 1, sizeof(BlockIndexRecord), rf) != sizeof(BlockIndexRecord))
            {
                fclose(rf);
                return SetError(error, "lm: truncated candidate (record)");
            }
            chunk.push_back(std::make_pair(h, rec));
            if (chunk.size() >= kChunk)
            {
                std::sort(chunk.begin(), chunk.end(),
                          [](const std::pair<uint256,BlockIndexRecord>& a,
                             const std::pair<uint256,BlockIndexRecord>& b) { return a.first < b.first; });
                std::string runPath = (tmpDir / (strprintf("run-%zu.bin", sortedRuns.size()))).string();
                FILE* wf = fopen(runPath.c_str(), "wb");
                if (!wf)
                {
                    fclose(rf);
                    return SetError(error, "lm: open run file failed");
                }
                for (size_t i = 0; i < chunk.size(); ++i)
                    if (fwrite(&chunk[i], 1, sizeof(chunk[i]), wf) != sizeof(chunk[i]))
                    {
                        fclose(wf);
                        fclose(rf);
                        return SetError(error, "lm: write run failed");
                    }
                fclose(wf);
                sortedRuns.push_back(runPath);
                chunk.clear();
            }
        }
        if (!chunk.empty())
        {
            std::sort(chunk.begin(), chunk.end(),
                      [](const std::pair<uint256,BlockIndexRecord>& a,
                         const std::pair<uint256,BlockIndexRecord>& b) { return a.first < b.first; });
            std::string runPath = (tmpDir / (strprintf("run-%zu.bin", sortedRuns.size()))).string();
            FILE* wf = fopen(runPath.c_str(), "wb");
            if (!wf)
            {
                fclose(rf);
                return SetError(error, "lm: open run file failed");
            }
            for (size_t i = 0; i < chunk.size(); ++i)
                if (fwrite(&chunk[i], 1, sizeof(chunk[i]), wf) != sizeof(chunk[i]))
                {
                    fclose(wf);
                    fclose(rf);
                    return SetError(error, "lm: write run failed");
                }
            fclose(wf);
            sortedRuns.push_back(runPath);
        }
        fclose(rf);
    }

    // ---- k-way merge of sorted runs, feeding records+hashindex to writer. ----
    // Buffer one record per run head; repeatedly pick min hash, append.
    // (Records must be unique by hash; duplicates => fail.)
    // Also build an EXTERNAL disk-backed LevelDB index hash -> (RecordId +
    // record) so active-chain (M3) and derived (M4) can look up any block by
    // hash in O(1) with bounded RAM (externalized, class C).
    leveldb::Options lopt;
    lopt.create_if_missing = true;
    lopt.error_if_exists = true;
    lopt.filter_policy = leveldb::NewBloomFilterPolicy(10);
    const std::string actIdxPath = (tmpDir / "actidx").string();
    std::string airr;
    if (!boost::filesystem::exists(actIdxPath))
        boost::filesystem::create_directories(actIdxPath, ec);
    leveldb::DB* actIdx = NULL;
    leveldb::Status as_ = leveldb::DB::Open(lopt, actIdxPath, &actIdx);
    if (!as_.ok())
        return SetError(error, "lm: create actidx failed: " + as_.ToString());
    {
        std::vector<FILE*> runFiles(sortedRuns.size());
        std::vector<std::pair<uint256, BlockIndexRecord>*> heads(sortedRuns.size());
        std::vector<std::pair<uint256, BlockIndexRecord> > headStorage(sortedRuns.size());
        for (size_t r = 0; r < sortedRuns.size(); ++r)
        {
            runFiles[r] = fopen(sortedRuns[r].c_str(), "rb");
            if (!runFiles[r])
            {
                for (size_t j = 0; j < r; ++j) if (runFiles[j]) fclose(runFiles[j]);
                delete actIdx;
                return SetError(error, "lm: open run for merge failed");
            }
            // read one head
            size_t got = fread(&headStorage[r], 1, sizeof(headStorage[r]), runFiles[r]);
            heads[r] = (got == sizeof(headStorage[r])) ? &headStorage[r] : NULL;
        }
        uint64_t total = 0;
        uint256 lastHash = 0;
        leveldb::WriteBatch actBatch;
        int actBatchCount = 0;
        // RecordId-hash temp file so M4 can emit derived in RecordId order
        // (RecordId order == merge order == uint256-sorted, NOT LevelDB byte
        // order). Disk-backed, O(N) on disk, O(1) RAM.
        const std::string idhashPath = (tmpDir / "idhash.bin").string();
        FILE* idf = fopen(idhashPath.c_str(), "wb");
        if (!idf)
        {
            delete actIdx;
            return SetError(error, "lm: open idhash file failed");
        }
        for (;;)
        {
            // find min head
            int best = -1;
            for (size_t r = 0; r < heads.size(); ++r)
                if (heads[r] && (best < 0 || heads[r]->first < heads[best]->first))
                    best = (int)r;
            if (best < 0)
                break;
            if (heads[best]->first == lastHash && lastHash != 0)
            {
                // duplicate hash -> fail closed
                for (size_t r = 0; r < runFiles.size(); ++r) if (runFiles[r]) fclose(runFiles[r]);
                delete actIdx;
                return SetError(error, "lm: duplicate block hash in source");
            }
            // stream to writer (AppendRecord + PutHashIndex; writer batches)
            BlockIndexId id;
            if (!writer_.AppendRecord(heads[best]->second, &id, error) ||
                !writer_.PutHashIndex(heads[best]->first, id, error))
            {
                for (size_t r = 0; r < runFiles.size(); ++r) if (runFiles[r]) fclose(runFiles[r]);
                fclose(idf);
                delete actIdx;
                return false;
            }
            // record id -> hash for M4 RecordId-order derived emission
            if (fwrite(&id, 1, sizeof(id), idf) != sizeof(id) ||
                fwrite(heads[best]->first.begin(), 1, 32, idf) != 32)
            {
                for (size_t r = 0; r < runFiles.size(); ++r) if (runFiles[r]) fclose(runFiles[r]);
                fclose(idf);
                delete actIdx;
                return SetError(error, "lm: write idhash failed");
            }
            // external index: key = 32-byte hash, value = (RecordId || record)
            std::string key(heads[best]->first.begin(), heads[best]->first.end());
            std::string val;
            val.resize(sizeof(BlockIndexId) + sizeof(BlockIndexRecord));
            memcpy(&val[0], &id, sizeof(id));
            memcpy(&val[sizeof(BlockIndexId)], &heads[best]->second, sizeof(BlockIndexRecord));
            actBatch.Put(leveldb::Slice(key), leveldb::Slice(val));
            ++actBatchCount;
            if (actBatchCount >= 4096)
            {
                leveldb::Status ws = actIdx->Write(leveldb::WriteOptions(), &actBatch);
                if (!ws.ok())
                {
                    for (size_t r = 0; r < runFiles.size(); ++r) if (runFiles[r]) fclose(runFiles[r]);
                    delete actIdx;
                    return SetError(error, "lm: actidx write failed: " + ws.ToString());
                }
                actBatch.Clear();
                actBatchCount = 0;
            }
            lastHash = heads[best]->first;
            ++total;
            // advance this run head
            size_t got = fread(&headStorage[best], 1, sizeof(headStorage[best]), runFiles[best]);
            heads[best] = (got == sizeof(headStorage[best])) ? &headStorage[best] : NULL;
        }
        if (actBatchCount > 0)
        {
            leveldb::Status ws = actIdx->Write(leveldb::WriteOptions(), &actBatch);
            if (!ws.ok())
            {
                for (size_t r = 0; r < runFiles.size(); ++r) if (runFiles[r]) fclose(runFiles[r]);
                delete actIdx;
                return SetError(error, "lm: actidx final write failed: " + ws.ToString());
            }
            actBatch.Clear();
            actBatchCount = 0;
        }
        for (size_t r = 0; r < runFiles.size(); ++r) if (runFiles[r]) fclose(runFiles[r]);
        fclose(idf);
        // NOTE: actIdx stays open for M3 (active walk) and M4 (derived).
        // writer_ also open; its records/hashindex handles are separate files.
    }
    (void)as_;
    const std::string idhashTmp = (tmpDir / "idhash.bin").string();

    // ---- M3: streamed active-chain construction ----
    // Walk hashBestChain -> hashPrev down to genesis using the external actIdx
    // (O(1) per step, bounded RAM). Write (height, RecordId) pairs tip-first to
    // a temp file; then read backward (ascending height) and feed active.dat
    // via the writer. Peak RAM O(1): only the active-pair temp file is O(N) and
    // it is disk-backed.
    const std::string activeTmp = (tmpDir / "active.pairs").string();
    {
        FILE* af = fopen(activeTmp.c_str(), "wb");
        if (!af)
        {
            delete actIdx;
            return SetError(error, "lm: open active pair file failed");
        }
        uint256 cur = hashBestChain;
        bool reachedGen = false;
        for (;;)
        {
            std::string key(cur.begin(), cur.end());
            std::string val;
            leveldb::Status gs = actIdx->Get(leveldb::ReadOptions(),
                                             leveldb::Slice(key), &val);
            if (!gs.ok())
            {
                fclose(af);
                delete actIdx;
                return SetError(error, "lm: active chain hash missing from index");
            }
            if (val.size() < sizeof(BlockIndexId) + sizeof(BlockIndexRecord))
            {
                fclose(af);
                delete actIdx;
                return SetError(error, "lm: active index value corrupt");
            }
            BlockIndexId id;
            memcpy(&id, &val[0], sizeof(BlockIndexId));
            BlockIndexRecord rec;
            memcpy(&rec, &val[sizeof(BlockIndexId)], sizeof(BlockIndexRecord));
            // write (height, RecordId) tip-first: height int32 + id 8 bytes
            if (fwrite(&rec.height, 1, sizeof(rec.height), af) != sizeof(rec.height) ||
                fwrite(&id, 1, sizeof(id), af) != sizeof(id))
            {
                fclose(af);
                delete actIdx;
                return SetError(error, "lm: write active pair failed");
            }
            if (rec.hashPrev == 0)
            {
                if (rec.height != 0)
                {
                    fclose(af);
                    delete actIdx;
                    return SetError(error, "lm: active genesis height != 0");
                }
                reachedGen = true;
                break;
            }
            cur = rec.hashPrev;
        }
        fclose(af);
        if (!reachedGen)
        {
            delete actIdx;
            return SetError(error, "lm: active chain did not reach genesis");
        }

        // Read backward (ascending height): file has height-descending pairs.
        // Validate height continuity 0..tip and feed active.dat ascending.
        FILE* rf = fopen(activeTmp.c_str(), "rb");
        if (!rf)
        {
            delete actIdx;
            return SetError(error, "lm: reopen active pair file failed");
        }
        fseek(rf, 0, SEEK_END);
        long fsz = ftell(rf);
        long pairBytes = 12; // int32 height + 8-byte BlockIndexId
        long pairCount = fsz / pairBytes;
        int64_t expectedH = 0;
        for (long idx = pairCount - 1; idx >= 0; --idx)
        {
            fseek(rf, idx * pairBytes, SEEK_SET);
            int32_t h; BlockIndexId id;
            if (fread(&h, 1, sizeof(h), rf) != sizeof(h) ||
                fread(&id, 1, sizeof(id), rf) != sizeof(id))
            {
                fclose(rf);
                delete actIdx;
                return SetError(error, "lm: read active pair (backward) failed");
            }
            if (h != expectedH)
            {
                fclose(rf);
                delete actIdx;
                return SetError(error, "lm: active chain height discontinuity at " +
                                strprintf("%lld", (long long)expectedH));
            }
            if (!writer_.AppendActive(id, h, error))
            {
                fclose(rf);
                delete actIdx;
                return false;
            }
            ++expectedH;
        }
        fclose(rf);
    }

    // ---- M4: streamed derived-state construction ----
    // Derived values (chainTrust/checksum/memo) must be computed in HEIGHT
    // order (child after parent). All records (main + side) have derived.
    // Steps (bounded RAM, external disk):
    //   a. Iterate actIdx (all records by hash) -> write (height | hash) to a
    //      temp file, external-sort by height.
    //   b. In height order, for each record: compute chainTrust (parent trust +
    //      block trust), checksum, modifier-time memo against a temp
    //      hash->derived LevelDB (external).
    //   c. Emit derived.dat in RecordId order (hash-sorted) by reading each
    //      record's derived from the temp map.
    {
        // a. height-sorted hash list (external sort)
        const std::string hSortTmp = (tmpDir / "hsort.bin").string();
        std::vector<std::string> heightRuns;
        {
            leveldb::Iterator* it = actIdx->NewIterator(leveldb::ReadOptions());
            it->SeekToFirst();
            std::vector<std::pair<int32_t, std::string> > chunk;
            chunk.reserve(kChunk);
            for (; it->Valid(); it->Next())
            {
                std::string hash = it->key().ToString();
                if (it->value().size() < sizeof(BlockIndexId))
                    continue;
                int32_t h = 0;
                memcpy(&h, it->value().data() + sizeof(BlockIndexId) +
                           offsetof(BlockIndexRecord, height), sizeof(h));
                chunk.push_back(std::make_pair(h, hash));
                if (chunk.size() >= kChunk)
                {
                    std::sort(chunk.begin(), chunk.end());
                    std::string rp = (tmpDir / (strprintf("hrun-%zu.bin", heightRuns.size()))).string();
                    FILE* wf = fopen(rp.c_str(), "wb");
                    if (!wf) { delete it; return SetError(error, "lm: open hrun failed"); }
                    for (size_t i = 0; i < chunk.size(); ++i)
                        if (fwrite(&chunk[i].first, 1, sizeof(chunk[i].first), wf) != sizeof(chunk[i].first) ||
                            fwrite(chunk[i].second.data(), 1, chunk[i].second.size(), wf) != chunk[i].second.size())
                        {
                            fclose(wf); delete it;
                            return SetError(error, "lm: write hrun failed");
                        }
                    fclose(wf);
                    heightRuns.push_back(rp);
                    chunk.clear();
                }
            }
            if (!chunk.empty())
            {
                std::sort(chunk.begin(), chunk.end());
                std::string rp = (tmpDir / (strprintf("hrun-%zu.bin", heightRuns.size()))).string();
                FILE* wf = fopen(rp.c_str(), "wb");
                if (!wf) { delete it; return SetError(error, "lm: open hrun failed"); }
                for (size_t i = 0; i < chunk.size(); ++i)
                    if (fwrite(&chunk[i].first, 1, sizeof(chunk[i].first), wf) != sizeof(chunk[i].first) ||
                        fwrite(chunk[i].second.data(), 1, chunk[i].second.size(), wf) != chunk[i].second.size())
                    {
                        fclose(wf); delete it;
                        return SetError(error, "lm: write hrun failed");
                    }
                fclose(wf);
                heightRuns.push_back(rp);
            }
            delete it;
            // save hash size (32)
            (void)hSortTmp;
        }

        // k-way merge of height runs (ascending height) into hsort.bin
        std::string hSortedPath = hSortTmp;
        {
            FILE* out = fopen(hSortedPath.c_str(), "wb");
            if (!out) return SetError(error, "lm: open hsort out failed");
            std::vector<FILE*> rfs(heightRuns.size());
            std::vector<std::pair<int32_t, std::string> > heads(heightRuns.size());
            std::vector<bool> alive(heightRuns.size(), false);
            for (size_t r = 0; r < heightRuns.size(); ++r)
            {
                rfs[r] = fopen(heightRuns[r].c_str(), "rb");
                if (!rfs[r]) { fclose(out); return SetError(error, "lm: open hrun for merge failed"); }
                int32_t h; char hashBuf[32];
                if (fread(&h, 1, 4, rfs[r]) == 4 && fread(hashBuf, 1, 32, rfs[r]) == 32)
                {
                    heads[r].first = h;
                    heads[r].second.assign(hashBuf, 32);
                    alive[r] = true;
                }
            }
            for (;;)
            {
                int best = -1;
                for (size_t r = 0; r < alive.size(); ++r)
                    if (alive[r] && (best < 0 || heads[r].first < heads[best].first))
                        best = (int)r;
                if (best < 0) break;
                if (fwrite(&heads[best].first, 1, 4, out) != 4 ||
                    fwrite(heads[best].second.data(), 1, 32, out) != 32)
                {
                    for (size_t r = 0; r < rfs.size(); ++r) if (rfs[r]) fclose(rfs[r]);
                    fclose(out);
                    return SetError(error, "lm: write hsort failed");
                }
                int32_t h; char hashBuf[32];
                if (fread(&h, 1, 4, rfs[best]) == 4 && fread(hashBuf, 1, 32, rfs[best]) == 32)
                {
                    heads[best].first = h;
                    heads[best].second.assign(hashBuf, 32);
                }
                else
                {
                    alive[best] = false;
                    fclose(rfs[best]);
                    rfs[best] = NULL; // avoid double-fclose in the cleanup loop
                }
            }
            for (size_t r = 0; r < rfs.size(); ++r) if (rfs[r]) fclose(rfs[r]);
            fclose(out);
        }

        // b. compute derived in height order via temp hash->derived LevelDB
        const std::string derIdxPath = (tmpDir / "deridx").string();
        if (!boost::filesystem::exists(derIdxPath))
            boost::filesystem::create_directories(derIdxPath, ec);
        leveldb::Options dopts;
        dopts.create_if_missing = true;
        dopts.error_if_exists = true;
        leveldb::DB* derIdx = NULL;
        leveldb::Status ds = leveldb::DB::Open(dopts, derIdxPath, &derIdx);
        if (!ds.ok())
            return SetError(error, "lm: open deridx failed: " + ds.ToString());
        struct DEntry {
            uint256 chainTrust; uint32_t checksum; int64_t modTime; bool hasModTime;
        };
        bool postDag = GetForkHeightDAG() >= 0; // use runtime fork height
        {
            FILE* rf = fopen(hSortedPath.c_str(), "rb");
            if (!rf) { delete derIdx; return SetError(error, "lm: open hsort for derive failed"); }
            for (;;)
            {
                int32_t h; char hashBuf[32];
                if (fread(&h, 1, 4, rf) != 4) break;
                if (fread(hashBuf, 1, 32, rf) != 32) { fclose(rf); delete derIdx; return SetError(error, "lm: hsort short read"); }
                std::string key(hashBuf, 32);
                std::string val;
                if (!actIdx->Get(leveldb::ReadOptions(), leveldb::Slice(key), &val).ok())
                {
                    fclose(rf); delete derIdx;
                    return SetError(error, "lm: record missing in actIdx during derive");
                }
                if (val.size() < sizeof(BlockIndexId) + sizeof(BlockIndexRecord))
                {
                    fclose(rf); delete derIdx;
                    return SetError(error, "lm: deridx value corrupt");
                }
                BlockIndexRecord rec;
                memcpy(&rec, &val[sizeof(BlockIndexId)], sizeof(BlockIndexRecord));
                DEntry de;
                // chainTrust: parent + blockTrust
                uint256 parentTrust = 0;
                if (rec.hashPrev != uint256(0))
                {
                    std::string pk(rec.hashPrev.begin(), rec.hashPrev.end());
                    std::string pv;
                    if (derIdx->Get(leveldb::ReadOptions(), leveldb::Slice(pk), &pv).ok())
                    {
                        DEntry pd;
                        if (pv.size() >= sizeof(pd))
                        {
                            memcpy(&pd, pv.data(), sizeof(pd));
                            parentTrust = pd.chainTrust;
                        }
                    }
                }
                CBigNum bn; bn.SetCompact(rec.nBits);
                uint256 bt = 0;
                if (bn > 0 && (rec.height < GetForkHeightDAG() || rec.prevoutStake.hash == uint256(0)))
                    bt = ((CBigNum(1)<<256)/(bn+1)).getuint256();
                de.chainTrust = parentTrust + bt;
                // checksum
                unsigned int parentChecksum = 0;
                if (rec.hashPrev != uint256(0))
                {
                    std::string pk(rec.hashPrev.begin(), rec.hashPrev.end());
                    std::string pv;
                    if (derIdx->Get(leveldb::ReadOptions(), leveldb::Slice(pk), &pv).ok())
                    {
                        DEntry pd; if (pv.size() >= sizeof(pd)) { memcpy(&pd, pv.data(), sizeof(pd)); parentChecksum = pd.checksum; }
                    }
                }
                CDataStream ss(SER_GETHASH, 0);
                if (rec.hashPrev != uint256(0)) ss << parentChecksum;
                uint256 proof = (rec.nFlags & CBlockIndex::BLOCK_PROOF_OF_STAKE) ? rec.hashProof : uint256(0);
                ss << rec.nFlags << proof << rec.nStakeModifier;
                uint256 hc = Hash(ss.begin(), ss.end());
                hc >>= (256 - 32);
                de.checksum = hc.Get64();
                // memo
                if (rec.nFlags & CBlockIndex::BLOCK_STAKE_MODIFIER)
                {
                    de.modTime = (int64_t)rec.nTime;
                    de.hasModTime = true;
                }
                else if (rec.hashPrev != uint256(0))
                {
                    std::string pk(rec.hashPrev.begin(), rec.hashPrev.end());
                    std::string pv;
                    if (derIdx->Get(leveldb::ReadOptions(), leveldb::Slice(pk), &pv).ok())
                    {
                        DEntry pd; if (pv.size() >= sizeof(pd)) { memcpy(&pd, pv.data(), sizeof(pd)); de.modTime = pd.modTime; de.hasModTime = pd.hasModTime; }
                    }
                }
                derIdx->Put(leveldb::WriteOptions(), leveldb::Slice(key),
                            leveldb::Slice((const char*)&de, sizeof(de)));
            }
            fclose(rf);
        }
        // c. emit derived.dat in RecordId order (id 1..N), using idhash.bin
//    (id -> hash, written during the merge in RecordId/merge order). This
//    guarantees derived entry i corresponds to the same record as RecordId i.
        {
            FILE* idf = fopen(idhashTmp.c_str(), "rb");
            if (!idf)
            {
                delete derIdx;
                return SetError(error, "lm: open idhash for derived emit failed");
            }
            for (;;)
            {
                BlockIndexId id;
                char hashBuf[32];
                if (fread(&id, 1, sizeof(id), idf) != sizeof(id))
                    break;
                if (fread(hashBuf, 1, 32, idf) != 32)
                {
                    fclose(idf); delete derIdx;
                    return SetError(error, "lm: idhash short read");
                }
                std::string hash(hashBuf, 32);
                std::string pv;
                if (derIdx->Get(leveldb::ReadOptions(), leveldb::Slice(hash), &pv).ok() &&
                    pv.size() >= sizeof(DEntry))
                {
                    DEntry de;
                    memcpy(&de, pv.data(), sizeof(de));
                    BlockIndexDerivedEntry e;
                    e.chainTrust = de.chainTrust;
                    e.stakeModifierChecksum = de.checksum;
                    e.SetHasStakeModifierTime(de.hasModTime);
                    e.stakeModifierTime = de.modTime;
                    // nSize: externalized in M6 (block files); mark unavailable here.
                    e.SetHasBlockSize(false);
                    if (!writer_.AppendDerived(e, error))
                    {
                        fclose(idf); delete derIdx;
                        return false;
                    }
                }
                (void)id;
            }
            fclose(idf);
        }
        delete derIdx;
        (void)postDag;
    }

    delete actIdx;

    // Flush any remaining buffered active/derived batches to disk.
    if (!writer_.Flush(error))
        return false;

    ClearError(error);
    return true;
}