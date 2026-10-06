// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.

#include "blockindex_generation_builder_lm.h"

#include "txdb-leveldb.h"
#include "main.h"
#include "kernel.h"

#include <leveldb/cache.h>
#include <leveldb/db.h>
#include <leveldb/filter_policy.h>

#include <malloc.h>

#include <boost/filesystem.hpp>

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace fs = boost::filesystem;

// V2-R1 test-only fault-injection seams (default false; NEVER set by production
// code). Deterministic PM1-P0-10 negative tests need to surface a local derIdx
// I/O error and a checked Put failure without waiting for a real disk fault.
bool g_lmV2R1ForceDerivedLookupError = false;
bool g_lmV2R1ForceDerivedPutFailure = false;

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

// Phase memory instrumentation (M8 diagnosis): print VmHWM (lifetime peak) +
// current RSS. Not consensus; diag-only.
static void LmDiagRss(const char* phase)
{
    FILE* f = fopen("/proc/self/status", "r");
    if (!f) return;
    char line[256];
    long hwm = -1, rss = -1;
    while (fgets(line, sizeof(line), f))
    {
        if (strncmp(line, "VmHWM:", 6) == 0) hwm = atol(line + 6);
        else if (strncmp(line, "VmRSS:", 6) == 0) rss = atol(line + 6);
    }
    fclose(f);
    fprintf(stderr, "LM_RSS phase=%s hwm_kb=%ld rss_kb=%ld\n", phase, hwm, rss);
#if defined(__GLIBC__)
    malloc_trim(0);
#endif
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

// Per-record derived scratch entry for the external hash -> derived LevelDB used
// during M4. Disposable same-process scratch (native ABI), not a persisted format.
struct DEntry {
    uint256 chainTrust; uint32_t checksum; int64_t modTime; bool hasModTime;
    uint32_t nSize; bool hasBlockSize;
};

// V2-R1 (PM1-P0-10): typed local-store lookup result. A missing key and a
// storage/corruption error must NOT collapse to the same "absent" outcome.
enum LmDerivedLookupStatus { LM_DERIVED_FOUND = 0, LM_DERIVED_NOT_FOUND = 1, LM_DERIVED_ERROR = 2 };

static LmDerivedLookupStatus LookupDerivedEntry(leveldb::DB* derIdx, const std::string& key,
                                                DEntry* out, std::string* error)
{
    if (g_lmV2R1ForceDerivedLookupError)
    {
        if (error) *error = "injected deridx lookup error";
        return LM_DERIVED_ERROR;
    }
    std::string val;
    leveldb::Status st = derIdx->Get(leveldb::ReadOptions(), leveldb::Slice(key), &val);
    if (st.IsNotFound())
        return LM_DERIVED_NOT_FOUND;
    if (!st.ok())
    {
        if (error) *error = st.ToString();
        return LM_DERIVED_ERROR;
    }
    if (val.size() < sizeof(DEntry))
    {
        if (error) *error = "derived entry value corrupt";
        return LM_DERIVED_ERROR;
    }
    memcpy(out, val.data(), sizeof(DEntry));
    return LM_DERIVED_FOUND;
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
    BlockIndexId outTipId = BLOCK_INDEX_ID_INVALID;
    int64_t outTipHeight = -1;
    // Hoisted: becomes false if any derived entry lacks a live nSize -> gates
    // whether M6 may claim AUTHORITATIVE capability.
    bool allHasBlockSize = true;
    // Empty DAG input digest (SHA256 of zero bytes) — the retired DAG input set is
    // permanently empty, exactly as the old builder produced. Cache it once for the
    // AUTHORITATIVE binding.
    unsigned char emptyDagDigest[32];
    SHA256_CTX dagCtx;
    SHA256_Init(&dagCtx);
    SHA256_Final(emptyDagDigest, &dagCtx);

    // M2: Open snapshot read-only (dedicated copy, never the live datadir).
    // CRITICAL (M8 RSS fix): the snapshot is a ~3GB / 8M-entry DB; opening it
    // with DEFAULT LevelDB options loads a huge table cache + index/filter
    // blocks for every SST -> a transient ~1.9GB RSS spike (the 1.87GiB HWM).
    // Bound the snapshot's block cache + max_open_files so its table cache
    // stays small and memory remains independent of N.
    leveldb::Options options;
    options.create_if_missing = false;
    options.error_if_exists = false;
    options.filter_policy = leveldb::NewBloomFilterPolicy(10);
    options.block_cache = leveldb::NewLRUCache(512 * 1024); // bounded
    options.write_buffer_size = 1 * 1024 * 1024;
    options.max_open_files = 64;
    leveldb::DB* db = NULL;
    leveldb::Status status = leveldb::DB::Open(options, snapshotLevelDbDir, &db);
    if (!status.ok())
        return SetError(error, std::string("lm: snapshot LevelDB open failure: ") +
                               status.ToString());
    LmDiagRss("M2_after_db_open");

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
    LmDiagRss("M2_runs_sorted");

    // ---- k-way merge of sorted runs, feeding records+hashindex to writer. ----
    // Buffer one record per run head; repeatedly pick min hash, append.
    // (Records must be unique by hash; duplicates => fail.)
    // Also build an EXTERNAL disk-backed LevelDB index hash -> (RecordId +
    // record) so active-chain (M3) and derived (M4) can look up any block by
    // hash in O(1) with bounded RAM (externalized, class C).
    // Use BOUNDED LevelDB cache/buffer: an 8M-entry external index must not
    // retain a large block cache or write buffer -> keep RAM independent of N.
    leveldb::Options lopt;
    lopt.create_if_missing = true;
    lopt.error_if_exists = true;
    lopt.filter_policy = leveldb::NewBloomFilterPolicy(10);
    lopt.block_cache = leveldb::NewLRUCache(512 * 1024);   // 512KB, bounded
    lopt.write_buffer_size = 1 * 1024 * 1024;              // 1MB, bounded
    lopt.max_open_files = 64;
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
            // Write the actIdx external index in larger batches. This DB is opened
            // with a BOUNDED 1MB write_buffer (RAM independent of N). A ~1MB batch
            // (4096 records) equalled that buffer, so every Write produced a full
            // memtable flush and L0 file, and the resulting flush/compaction churn
            // dominated the merge phase (measured: 195 flushes, ~45% of the merge
            // wall time at K=13). ~4MB batches (16384 records) cut the flush count
            // (195 -> 48) and the merge time ~45%, with bounded extra RAM (~4MB
            // batch buffer; measured peak +~12MB, still well within the bounded
            // migration footprint). No change to indexed content or ordering.
            if (actBatchCount >= 16384)
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
            if ((total % 1000000) == 0)
            {
                long hw = -1;
                FILE* sf = fopen("/proc/self/status", "r");
                if (sf)
                {
                    char ln[256];
                    while (fgets(ln, sizeof(ln), sf))
                        if (strncmp(ln, "VmHWM:", 6) == 0) { hw = atol(ln + 6); break; }
                    fclose(sf);
                }
                fprintf(stderr, "LM_RSS merge_records=%llu hwm_kb=%ld\n",
                        (unsigned long long)total, hw);
            }
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
    LmDiagRss("M2_merge_done");

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
            if (idx == 0)
            {
                outTipId = id;
                outTipHeight = h;
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
    LmDiagRss("M3_active_done");

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
        dopts.filter_policy = leveldb::NewBloomFilterPolicy(10);
        dopts.block_cache = leveldb::NewLRUCache(512 * 1024); // bounded
        dopts.write_buffer_size = 1 * 1024 * 1024;
        dopts.max_open_files = 64;
        leveldb::DB* derIdx = NULL;
        leveldb::Status ds = leveldb::DB::Open(dopts, derIdxPath, &derIdx);
        if (!ds.ok())
            return SetError(error, "lm: open deridx failed: " + ds.ToString());
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

                // PM1-P0-08: refuse ambiguous PoS classification BEFORE deriving
                // trust (legacy nFlags classification vs V2 prevoutStake inference).
                if (!PosClassificationConsistent(rec.nFlags, rec.prevoutStake.hash))
                {
                    fclose(rf); delete derIdx;
                    return SetError(error, "lm: record " + rec.hash.GetHex() +
                                    " has ambiguous PoS classification (nFlags vs prevoutStake)");
                }
                const bool fPos = (rec.nFlags & CBlockIndex::BLOCK_PROOF_OF_STAKE) != 0;

                // ONE surviving trust rule (identical to CBlockIndex::GetBlockTrust
                // and GetAuthoritativeBlockTrust: POEM entropy + post-DAG PoS zero).
                const uint256 blockTrust =
                    GetAuthoritativeBlockTrustValue(rec.nBits, rec.height, fPos, rec.hashProof, rec.hash);

                // Required parent derived state. In height order a nonzero-hashPrev
                // parent MUST already be derived; absence == disconnected topology
                // (rejected by the regular builder too). A local storage ERROR is
                // distinct from NOT_FOUND and always fails closed.
                uint256 parentTrust = 0;
                uint32_t parentChecksum = 0;
                int64_t parentModTime = 0;
                bool parentHasModTime = false;
                if (rec.hashPrev != uint256(0))
                {
                    DEntry pd;
                    std::string perr;
                    std::string pk(rec.hashPrev.begin(), rec.hashPrev.end());
                    LmDerivedLookupStatus ps = LookupDerivedEntry(derIdx, pk, &pd, &perr);
                    if (ps == LM_DERIVED_NOT_FOUND)
                    {
                        fclose(rf); delete derIdx;
                        return SetError(error, "lm: record " + rec.hash.GetHex() +
                                        " has disconnected parent " + rec.hashPrev.GetHex());
                    }
                    if (ps == LM_DERIVED_ERROR)
                    {
                        fclose(rf); delete derIdx;
                        return SetError(error, "lm: parent derived lookup failed: " + perr);
                    }
                    parentTrust = pd.chainTrust;
                    parentChecksum = pd.checksum;
                    parentModTime = pd.modTime;
                    parentHasModTime = pd.hasModTime;
                }

                DEntry de;
                de.chainTrust = parentTrust + blockTrust;
                // checksum (parent checksum comes from the single checked lookup above)
                CDataStream ss(SER_GETHASH, 0);
                if (rec.hashPrev != uint256(0)) ss << parentChecksum;
                uint256 proof = (rec.nFlags & CBlockIndex::BLOCK_PROOF_OF_STAKE) ? rec.hashProof : uint256(0);
                ss << rec.nFlags << proof << rec.nStakeModifier;
                uint256 hc = Hash(ss.begin(), ss.end());
                hc >>= (256 - 32);
                de.checksum = hc.Get64();
                // memo (parent memo from the single checked lookup; genesis/no-parent
                // -> deterministic 0/false, matching the regular builder)
                if (rec.nFlags & CBlockIndex::BLOCK_STAKE_MODIFIER)
                {
                    de.modTime = (int64_t)rec.nTime;
                    de.hasModTime = true;
                }
                else if (rec.hashPrev != uint256(0))
                {
                    de.modTime = parentModTime;
                    de.hasModTime = parentHasModTime;
                }
                else
                {
                    de.modTime = 0;
                    de.hasModTime = false;
                }
                de.nSize = 0; de.hasBlockSize = false;
                // A.10.1b-fix2 C1 (authoritative parity): compute exact nSize
                // from block files, identical to the old trusted builder.
                // Read the block from blk{file}.dat using nFile/nBlockPos,
                // deserialize and compute GetSerializeSize(SER_NETWORK).
                if (!blockDataDir.empty() && rec.nFile > 0)
                {
                    std::string blockFn = strprintf("blk%04u.dat", rec.nFile);
                    boost::filesystem::path blockPath =
                        boost::filesystem::path(blockDataDir) / blockFn;
                    FILE* blockFile = fopen(blockPath.string().c_str(), "rb");
                    if (blockFile)
                    {
                        if (fseeko(blockFile, (off_t)rec.nBlockPos, SEEK_SET) == 0)
                        {
                            // CAutoFile OWNS blockFile and closes it on scope exit
                            // (success or exception). Never fclose() here.
                            try {
                                CBlock block;
                                CAutoFile filein(blockFile, SER_DISK, CLIENT_VERSION);
                                filein >> block;
                                if (block.GetHash() == rec.hash)
                                {
                                    de.nSize = ::GetSerializeSize(block, SER_NETWORK, PROTOCOL_VERSION);
                                    de.hasBlockSize = (de.nSize > 0);
                                }
                            } catch (...) { de.nSize = 0; de.hasBlockSize = false; }
                        }
                        else
                        {
                            fclose(blockFile); // CAutoFile never constructed
                            de.nSize = 0; de.hasBlockSize = false;
                        }
                    }
                    else { de.nSize = 0; de.hasBlockSize = false; }
                }
                if (g_lmV2R1ForceDerivedPutFailure ||
                    !derIdx->Put(leveldb::WriteOptions(), leveldb::Slice(key),
                                 leveldb::Slice((const char*)&de, sizeof(de))).ok())
                {
                    fclose(rf); delete derIdx;
                    return SetError(error, "lm: deridx write failed");
                }
            }
            fclose(rf);
        }
        LmDiagRss("M4_derive_compute_done");
        // Release actIdx now: step-c (emit) and M6 (finalize) do NOT need it.
        // This removes the simultaneous actIdx+derIdx LevelDB cache footprint.
        delete actIdx;
        actIdx = NULL;
        // Release the height-sorted + hrun temp files' in-RAM handles (FILE*
        // already closed by the merge block; drop the path strings too).
        {
            std::vector<std::string>().swap(heightRuns);
            // also free the hsrort.bin FILE handle? it's a path-only read; no
            // retained FILE*. The defragged derIdx below is the main cost.
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
                DEntry de;
                std::string derr;
                LmDerivedLookupStatus es = LookupDerivedEntry(derIdx, hash, &de, &derr);
                if (es != LM_DERIVED_FOUND)
                {
                    fclose(idf); delete derIdx;
                    return SetError(error, std::string("lm: derived entry ") +
                                    (es == LM_DERIVED_NOT_FOUND ? "missing" : "lookup failed") +
                                    " during emit" +
                                    (derr.empty() ? std::string() : (" " + derr)));
                }
                if (!de.hasBlockSize)
                    allHasBlockSize = false;
                BlockIndexDerivedEntry e;
                e.chainTrust = de.chainTrust;
                e.stakeModifierChecksum = de.checksum;
                e.SetHasStakeModifierTime(de.hasModTime);
                e.stakeModifierTime = de.modTime;
                e.nSize = de.nSize;
                e.SetHasBlockSize(de.hasBlockSize);
                if (!writer_.AppendDerived(e, error))
                {
                    fclose(idf); delete derIdx;
                    return false;
                }
                (void)id;
            }
            fclose(idf);
        }
        delete derIdx;
        LmDiagRss("M4_emit_done");
    }

    delete actIdx;  // already NULL (released after M4 compute); no-op
    LmDiagRss("M6_finalize_about");

    // Flush any remaining buffered active/derived batches to disk.
    if (!writer_.Flush(error))
        return false;

    // ---- M6: finalize (content binding + MANIFEST COMPLETE) ----
    // The writer.Finalize sets the derived content binding (shadow or root) and
    // writes the COMPLETE MANIFEST with the committed tip. Capability is
    // OLD_SHADOW unless exact nSize (AUTHORITATIVE) was provided via blockDataDir
    // (the nSize path is externalized; here we mark OLD_SHADOW if no block data).
    if (outTipId == BLOCK_INDEX_ID_INVALID || outTipHeight < 0 || hashBestChain == 0)
    {
        ClearError(error);
        return SetError(error, "lm: no active tip resolved during build");
    }
    const bool authoritativeWanted = (!blockDataDir.empty());
    // AUTHORITATIVE is only valid when every derived entry has a real nSize.
    // Fail closed otherwise (must not silently downgrade an authoritative build,
    // matching the old builder's requirement).
    if (authoritativeWanted && !allHasBlockSize)
    {
        ClearError(error);
        return SetError(error, "lm: authoritative generation requires mandatory nSize for all records");
    }
    const uint32_t finalCap = authoritativeWanted
        ? BLOCK_INDEX_GENERATION_CAPABILITY_AUTHORITATIVE
        : BLOCK_INDEX_GENERATION_CAPABILITY_OLD_SHADOW;
    const uint64_t totalRecords = writer_.RecordCount(); // before Finalize closes
    // The retired DAG input set is permanently empty, so its digest is
    // SHA256(zero bytes) == e3b0c442..., byte-matching the old builder's manifested
    // dagInputDigest. (See function-scope emptyDagDigest.)
    if (!writer_.Finalize(generation, hashBestChain, outTipId,
                          (int32_t)outTipHeight,
                          totalRecords, finalCap,
                          emptyDagDigest, error))
        return false;

    // COHORT M: drop the builder's scratch directory. The external-sort runs,
    // the temporary actidx/deridx LevelDBs and the idhash/hsort files all live
    // under <stagingDir>/.lm-tmp, and <stagingDir> is renamed into the immutable
    // generation on publish. Leaving it would ship O(N) scratch INSIDE the
    // published generation (measured ~3x the canonical generation size), so it
    // must be removed once the generation is finalized.
    {
        boost::system::error_code rmec;
        fs::remove_all(tmpDir, rmec);
    }

    ClearError(error);
    return true;
}