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
                delete actIdx;
                return false;
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
        // NOTE: actIdx stays open for M3 (active walk) and M4 (derived).
        // writer_ also open; its records/hashindex handles are separate files.
    }
    (void)as_;

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
    delete actIdx;

    // Flush any remaining buffered active/derived batches to disk.
    if (!writer_.Flush(error))
        return false;

    ClearError(error);
    return true;
}