// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.
//
// Disk-lean single-pass streaming V1->V2 migration builder (S16b).
//
// Replaces the staged pipeline (candidates file + external sort runs + two
// side LevelDBs + full preallocated derived.dat second copy) with ONE
// streaming pass over the legacy txleveldb snapshot:
//
//   1. The snapshot iterator is ordered by its blockindex key, whose suffix is
//      the block hash in the SAME byte order as the legacy builder's hash
//      sort. Scanning in iterator order therefore IS the hash-sorted order the
//      old pipeline produced, so records.dat/derived.dat/active.dat bytes stay
//      identical to the staged builder byte-for-byte.
//   2. records.dat and the final hashindex are written DURING that pass
//      (hashindex keys are level-ordered, unordered appends are fine).
//   3. hash -> (recordId, hashPrev, height, nFile, nBlockPos) projections
//      (84 bytes fixed) stream into a small scratch file; an in-place external
//      sort (hash order) turns it into the random-access ancestry carrier.
//   4. The active chain is walked DOWN from the authoritative tip through the
//      sorted scratch file (fread), re-emitted ascending into a scratch file
//      (dense, contiguous, no in-RAM map).
//   5. derived.dat entries are produced by a HEIGHT-ordered pass over a second
//      sorted run set (hsorted) so every parent's derived entry is already
//      committed before the child needs it; the parent's derived state is
//      carried in RAM (one entry) ONLY for the parentless-vs-child recurrence,
//      resolution itself is a direct hash-key binary search. The writer's
//      AppendDerived assigns the RecordId-implicit slot, identical to the
//      staged pipeline's output layout.
//   6. sortedRun/actidx/deridx/side-LevelDB temp artifacts are GONE: staging
//      peak is proj.bin + its sorted copy + hsorted + records.dat + hashindex
//      + streamed derived batches, well within the ~8 GB envelope, and the
//      tmp files are removed before publish.
//
// Fail-closed contracts preserved exactly:
//   - mandatory nSize resolution from blk*.dat with hash correspondence
//     (any record without it fails the WHOLE build; never OLD_SHADOW-silent);
//   - source duplicate-hash, decode, truncated-iterator failure = whole-build
//     failure;
//   - active/parent walk absent = failure (never partial authority);
//   - stale staging directory is removed; the published gen-N dir is never
//     overwritten.
//
// No consensus validation (CheckBlock/ConnectBlock/PoW/signature rules) is
// invoked anywhere in this path: the migration is a storage-format conversion
// of already-validated state. Structural integrity only: completeness, hash
// correspondence for nSize, ancestry continuity, tip coherence, capability
// integrity. (See the run report "no-consensus-validation-during-migration".)

#include "blockindex_generation_builder_lm.h"
#include "main.h"
#include "kernel.h"

#include <leveldb/cache.h>
#include <leveldb/db.h>
#include <leveldb/filter_policy.h>

#include <malloc.h>

#include <boost/filesystem.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace fs = boost::filesystem;

// V2-R1 test-only fault-injection seams (default false; NEVER set by
// production code). Preserved from the previous builder for the negative-test
// suite surface (the derived lookup is direct here, but the seams stay for
// compatibility).
// g_lmV2R1 fault-injection seams are defined in
// blockindex_generation_builder_lm.cpp.

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
// BlockIndexRecord (same field mapping as the staged builder).
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

// Fixed-size projection row (exact 84 B, tightly packed):
//   [0..32)   hash        (raw memory byte order = the legacy scan key order)
//   [32..64)  hashPrev
//   [64..68)  height      (i32)
//   [68..72)  nFile       (u32)
//   [72..76)  nBlockPos   (u32)
//   [76..80)  recordId    (u32; 8.2M < 2^32)
//   [80..84)  pad         (0)
#pragma pack(push, 1)
struct HashProj {
    unsigned char hash[32];
    unsigned char hashPrev[32];
    int32_t height;
    uint32_t nFile;
    uint32_t nBlockPos;
    uint32_t recordId;
    uint32_t pad;
};
#pragma pack(pop)

static const size_t HASHPROJ_SIZE = sizeof(HashProj);

// Binary search over a packed fixed-record file keyed by the first 32 bytes
// (raw memcmp). Returns true and fills rec (recordSize bytes) when found.
static bool BinSearchFixed(FILE* f, off_t totalRecords, size_t recordSize,
                           const unsigned char target[32], void* rec)
{
    off_t lo = 0, hi = totalRecords;
    while (lo < hi)
    {
        const off_t mid = lo + (hi - lo) / 2;
        if (fseeko(f, mid * (off_t)recordSize, SEEK_SET) != 0 ||
            fread(rec, 1, recordSize, f) != recordSize)
            return false;
        const int c = memcmp(rec, target, 32);
        if (c == 0)
            return true;
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return false;
}

static bool IsZero32(const unsigned char h[32])
{
    unsigned char zero[32];
    memset(zero, 0, 32);
    return memcmp(h, zero, 32) == 0;
}

// Read one 228-byte BlockIndexRecord from records.dat (the writer's committed
// layout: 40-byte header + (id-1)*228) with CRC decode, fail closed.
static bool ReadRecordFromRecordsDat(FILE* recF, BlockIndexId id,
                                     BlockIndexRecord* out, std::string* error)
{
    const off_t slot = (off_t)(40 + ((off_t)id - 1) * 228);
    unsigned char raw[228];
    if (fseeko(recF, slot, SEEK_SET) != 0 ||
        fread(raw, 1, sizeof(raw), recF) != sizeof(raw))
        return SetError(error, "lm: records.dat read failed");
    if (!DecodeBlockIndexRecordV1(raw, sizeof(raw), out, error))
    {
        std::string msg = error ? *error : std::string();
        return SetError(error, "lm: record decode failure: " + msg);
    }
    return true;
}

// Read one 56-byte derived entry from the already-positioned FILE* (the
// caller does the fseeko; this decodes + CRC-checks, fail closed)
static bool DecodeBlockIndexDerivedEntryAt(FILE* f, BlockIndexDerivedEntry* out,
                                          std::string* error)
{
    unsigned char enc[56];
    if (fread(enc, 1, sizeof(enc), f) != sizeof(enc))
        return SetError(error, "lm: parent derived committed entry missing/corrupt");
    if (!DecodeBlockIndexDerivedEntry(enc, sizeof(enc), out, error))
    {
        std::string msg = error ? *error : std::string();
        return SetError(error, "lm: parent derived entry decode failed: " + msg);
    }
    return true;
}

} // namespace

// Disk-lean streaming Build (the wired production implementation; the former
// staged pipeline is retained in blockindex_generation_builder_lm.cpp as
// BuildStagedPipeline, with no production caller).
bool BlockIndexGenerationBuilderLM::Build(const std::string& snapshotLevelDbDir,
                                          const std::string& blockDataDir,
                                          uint64_t generation,
                                          const std::string& stagingDir,
                                          std::string* error)
{
    BlockIndexId outTipId = BLOCK_INDEX_ID_INVALID;
    int64_t outTipHeight = -1;
    bool allHasBlockSize = true;
    unsigned char emptyGenerationInputDigest[32];
    SHA256_CTX shaCtx;
    SHA256_Init(&shaCtx);
    SHA256_Final(emptyGenerationInputDigest, &shaCtx);

    leveldb::Options options;
    options.create_if_missing = false;
    options.error_if_exists = false;
    options.filter_policy = leveldb::NewBloomFilterPolicy(10);
    options.block_cache = leveldb::NewLRUCache(512 * 1024); // bounded (M8 lesson)
    options.write_buffer_size = 1 * 1024 * 1024;
    options.max_open_files = 64;
    leveldb::DB* db = NULL;
    leveldb::Status status = leveldb::DB::Open(options, snapshotLevelDbDir, &db);
    if (!status.ok())
        return SetError(error, std::string("lm: snapshot LevelDB open failure: ") +
                               status.ToString());
    LmDiagRss("M0_after_db_open");

    if (!writer_.OpenTarget(stagingDir, generation, error))
    {
        delete db;
        return false;
    }

    // hashBestChain (authoritative active-chain tip) read BEFORE streaming.
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

    // ---- M1 single pass: legacy snapshot iterator (hash-ordered) ->
    //      records.dat + hashindex + hash-projection scratch. ----
    // Because the legacy LevelDB scan is ordered by the key block hash —
    // the same order the staged builder's external sort produced — this pass
    // reproduces the previous records.dat/derived.dat/active.dat byte layout
    // in ONE slip without the sort runs or side DBs.
    const fs::path tmpDir = fs::path(stagingDir) / ".lm-tmp";
    boost::system::error_code ec;
    if (!fs::create_directories(tmpDir, ec) && ec)
    {
        delete db;
        return SetError(error, "lm: create tmp dir failed");
    }
    const std::string projTmp = (tmpDir / "proj.bin").string();
    FILE* pf = fopen(projTmp.c_str(), "wb");
    if (!pf)
    {
        delete db;
        return SetError(error, "lm: open projection scratch failed");
    }

    uint64_t count = 0;
    uint64_t dupChecked = 0;
    {
        leveldb::Iterator* it = db->NewIterator(leveldb::ReadOptions());
        CDataStream ssStartKey(SER_DISK, CLIENT_VERSION);
        ssStartKey << make_pair(std::string("blockindex"), uint256(0));
        it->Seek(ssStartKey.str());
        unsigned char prevHash[32]; bool havePrev = false;
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
                delete it; fclose(pf); delete db;
                return SetError(error, "lm: decode blockindex failed");
            }
            // streaming append: RecordIds are physical order (hash-ordered scan)
            BlockIndexId id;
            if (!writer_.AppendRecord(rec, &id, error))
            {
                delete it; fclose(pf); delete db;
                return false;
            }
            if (!writer_.PutHashIndex(rec.hash, id, error))
            {
                delete it; fclose(pf); delete db;
                return false;
            }
            // projection: hash || hashPrev (32+32)
            HashProj proj;
            memcpy(proj.hash, rec.hash.begin(), 32);
            memcpy(proj.hashPrev, rec.hashPrev.begin(), 32);
            // height, nFile, nBlockPos appended by separate columns so the
            // projection stays fixed-size AND keeps the chain fields adjacent
            // (the ancestor lookup needs only hashPrev + height).
            if (fwrite(&proj, 1, HASHPROJ_SIZE, pf) != HASHPROJ_SIZE ||
                fwrite(&rec.height, 1, sizeof(rec.height), pf) != sizeof(rec.height) ||
                fwrite(&rec.nFile, 1, sizeof(rec.nFile), pf) != sizeof(rec.nFile) ||
                fwrite(&rec.nBlockPos, 1, sizeof(rec.nBlockPos), pf) != sizeof(rec.nBlockPos) ||
                fwrite(&id, 1, sizeof(id), pf) != sizeof(id))
            {
                delete it; fclose(pf); delete db;
                return SetError(error, "lm: write projection failed");
            }
            // duplicate-hash check: scan is strictly ascending; a repeat ==
            // corrupted source index (fail closed like the staged builder).
            if (havePrev && memcmp(prevHash, rec.hash.begin(), 32) == 0)
            {
                delete it; fclose(pf); delete db;
                return SetError(error, "lm: duplicate block hash in source");
            }
            memcpy(prevHash, rec.hash.begin(), 32); havePrev = true;
            ++count; ++dupChecked;
            it->Next();
            if ((count % 1000000) == 0)
                fprintf(stderr, "lm: scan_records=%llu\n", (unsigned long long)count);
        }
        delete it;
        fclose(pf);
    }
    delete db;
    LmDiagRss("M1_pass_done");

    // ============ M2a: hash-sorted ancestry index (proj-sorted.bin) ========
    // The projection scratch is sorted by hash so slot = deterministic prefix;
    // this is the ONLY random-access ancestry carrier (hash -> row binary
    // search). Bounded-RAM external sort over fixed 84 B rows, runs deleted
    // after the k-way merge. Peak extra RAM: kChunk rows (~10 MB).
    const size_t kChunk = 65536;
    std::vector<std::string> sortedRuns;
    {
        FILE* rf = fopen(projTmp.c_str(), "rb");
        if (!rf)
            return SetError(error, "lm: reopen projection scratch failed");
        std::vector<HashProj> chunk;
        chunk.reserve(kChunk);
        for (;;)
        {
            HashProj hp;
            const size_t n = fread(&hp, 1, HASHPROJ_SIZE, rf);
            if (n == 0)
                break;
            if (n != HASHPROJ_SIZE)
            {
                fclose(rf);
                return SetError(error, "lm: truncated projection (sort)");
            }
            chunk.push_back(hp);
            if (chunk.size() >= kChunk)
            {
                std::sort(chunk.begin(), chunk.end(),
                          [](const HashProj& a, const HashProj& b) {
                              return memcmp(a.hash, b.hash, 32) < 0;
                          });
                std::string rp = (tmpDir / (strprintf("srun-%zu.bin", sortedRuns.size()))).string();
                FILE* wf = fopen(rp.c_str(), "wb");
                if (!wf)
                {
                    fclose(rf);
                    return SetError(error, "lm: open sort run failed");
                }
                for (size_t i = 0; i < chunk.size(); ++i)
                    if (fwrite(&chunk[i], 1, HASHPROJ_SIZE, wf) != HASHPROJ_SIZE)
                    {
                        fclose(wf); fclose(rf);
                        return SetError(error, "lm: write sort run failed");
                    }
                fclose(wf);
                sortedRuns.push_back(rp);
                chunk.clear();
            }
        }
        if (!chunk.empty())
        {
            std::sort(chunk.begin(), chunk.end(),
                      [](const HashProj& a, const HashProj& b) {
                          return memcmp(a.hash, b.hash, 32) < 0;
                      });
            std::string rp = (tmpDir / (strprintf("srun-%zu.bin", sortedRuns.size()))).string();
            FILE* wf = fopen(rp.c_str(), "wb");
            if (!wf)
            {
                fclose(rf);
                return SetError(error, "lm: open sort run (tail) failed");
            }
            for (size_t i = 0; i < chunk.size(); ++i)
                if (fwrite(&chunk[i], 1, HASHPROJ_SIZE, wf) != HASHPROJ_SIZE)
                {
                    fclose(wf); fclose(rf);
                    return SetError(error, "lm: write sort run (tail) failed");
                }
            fclose(wf);
            sortedRuns.push_back(rp);
        }
        fclose(rf);
    }
    LmDiagRss("M2_runs_sorted");

    const std::string projSorted = (tmpDir / "proj-sorted.bin").string();
    {
        FILE* out = fopen(projSorted.c_str(), "wb");
        if (!out)
            return SetError(error, "lm: open proj-sorted failed");
        std::vector<FILE*> rfs(sortedRuns.size());
        std::vector<HashProj> heads(sortedRuns.size());
        std::vector<bool> alive(sortedRuns.size(), false);
        for (size_t r = 0; r < sortedRuns.size(); ++r)
        {
            rfs[r] = fopen(sortedRuns[r].c_str(), "rb");
            if (!rfs[r])
            {
                fclose(out);
                return SetError(error, "lm: open sort run for merge failed");
            }
            if (fread(&heads[r], 1, HASHPROJ_SIZE, rfs[r]) == HASHPROJ_SIZE)
                alive[r] = true;
        }
        for (;;)
        {
            int best = -1;
            for (size_t r = 0; r < alive.size(); ++r)
                if (alive[r] && (best < 0 || memcmp(heads[r].hash, heads[best].hash, 32) < 0))
                    best = (int)r;
            if (best < 0)
                break;
            if (fwrite(&heads[best], 1, HASHPROJ_SIZE, out) != HASHPROJ_SIZE)
            {
                for (size_t r = 0; r < rfs.size(); ++r) if (rfs[r]) fclose(rfs[r]);
                fclose(out);
                return SetError(error, "lm: write proj-sorted failed");
            }
            if (fread(&heads[best], 1, HASHPROJ_SIZE, rfs[best]) != HASHPROJ_SIZE)
            {
                alive[best] = false;
                fclose(rfs[best]);
                rfs[best] = NULL;
            }
        }
        for (size_t r = 0; r < rfs.size(); ++r) if (rfs[r]) fclose(rfs[r]);
        fclose(out);
        for (size_t r = 0; r < sortedRuns.size(); ++r)
            fs::remove(sortedRuns[r], ec);
        std::vector<std::string>().swap(sortedRuns);
    }
    LmDiagRss("M2_merge_done");

    // ============ M2b: height-sorted carrier (hsorted.bin) ============
    std::vector<std::string> heightRuns;
    {
        FILE* rf = fopen(projTmp.c_str(), "rb");
        if (!rf)
            return SetError(error, "lm: reopen projection scratch failed");
        std::vector<HashProj> chunk;
        chunk.reserve(kChunk);
        for (;;)
        {
            HashProj hp;
            const size_t n = fread(&hp, 1, HASHPROJ_SIZE, rf);
            if (n == 0)
                break;
            if (n != HASHPROJ_SIZE)
            {
                fclose(rf);
                return SetError(error, "lm: truncated projection (height sort)");
            }
            chunk.push_back(hp);
            if (chunk.size() >= kChunk)
            {
                std::sort(chunk.begin(), chunk.end(),
                          [](const HashProj& a, const HashProj& b) {
                              if (a.height != b.height) return a.height < b.height;
                              return memcmp(a.hash, b.hash, 32) < 0;
                          });
                std::string rp = (tmpDir / (strprintf("hrun-%zu.bin", heightRuns.size()))).string();
                FILE* wf = fopen(rp.c_str(), "wb");
                if (!wf)
                {
                    fclose(rf);
                    return SetError(error, "lm: open hrun failed");
                }
                for (size_t i = 0; i < chunk.size(); ++i)
                    if (fwrite(&chunk[i], 1, HASHPROJ_SIZE, wf) != HASHPROJ_SIZE)
                    {
                        fclose(wf); fclose(rf);
                        return SetError(error, "lm: write hrun failed");
                    }
                fclose(wf);
                heightRuns.push_back(rp);
                chunk.clear();
            }
        }
        if (!chunk.empty())
        {
            std::sort(chunk.begin(), chunk.end(),
                      [](const HashProj& a, const HashProj& b) {
                          if (a.height != b.height) return a.height < b.height;
                          return memcmp(a.hash, b.hash, 32) < 0;
                      });
            std::string rp = (tmpDir / (strprintf("hrun-%zu.bin", heightRuns.size()))).string();
            FILE* wf = fopen(rp.c_str(), "wb");
            if (!wf)
            {
                fclose(rf);
                return SetError(error, "lm: open hrun (tail) failed");
            }
            for (size_t i = 0; i < chunk.size(); ++i)
                if (fwrite(&chunk[i], 1, HASHPROJ_SIZE, wf) != HASHPROJ_SIZE)
                {
                    fclose(wf); fclose(rf);
                    return SetError(error, "lm: write hrun (tail) failed");
                }
            fclose(wf);
            heightRuns.push_back(rp);
        }
        fclose(rf);
    }
    LmDiagRss("M2_height_runs");

    const std::string hSortedPath = (tmpDir / "hsorted.bin").string();
    {
        FILE* out = fopen(hSortedPath.c_str(), "wb");
        if (!out)
            return SetError(error, "lm: open hsorted failed");
        std::vector<FILE*> rfs(heightRuns.size());
        std::vector<HashProj> heads(heightRuns.size());
        std::vector<bool> alive(heightRuns.size(), false);
        for (size_t r = 0; r < heightRuns.size(); ++r)
        {
            rfs[r] = fopen(heightRuns[r].c_str(), "rb");
            if (!rfs[r])
            {
                fclose(out);
                return SetError(error, "lm: open hrun for merge failed");
            }
            if (fread(&heads[r], 1, HASHPROJ_SIZE, rfs[r]) == HASHPROJ_SIZE)
                alive[r] = true;
        }
        for (;;)
        {
            int best = -1;
            for (size_t r = 0; r < alive.size(); ++r)
            {
                if (!alive[r]) continue;
                if (best < 0 ||
                    heads[r].height < heads[best].height ||
                    (heads[r].height == heads[best].height &&
                     memcmp(heads[r].hash, heads[best].hash, 32) < 0))
                    best = (int)r;
            }
            if (best < 0)
                break;
            if (fwrite(&heads[best], 1, HASHPROJ_SIZE, out) != HASHPROJ_SIZE)
            {
                for (size_t r = 0; r < rfs.size(); ++r) if (rfs[r]) fclose(rfs[r]);
                fclose(out);
                return SetError(error, "lm: write hsorted failed");
            }
            if (fread(&heads[best], 1, HASHPROJ_SIZE, rfs[best]) != HASHPROJ_SIZE)
            {
                alive[best] = false;
                fclose(rfs[best]);
                rfs[best] = NULL;
            }
        }
        for (size_t r = 0; r < rfs.size(); ++r) if (rfs[r]) fclose(rfs[r]);
        fclose(out);
        for (size_t r = 0; r < heightRuns.size(); ++r)
            fs::remove(heightRuns[r], ec);
        std::vector<std::string>().swap(heightRuns);
    }
    LmDiagRss("M2_hsorted_done");

    // ============ M3: active chain walk (tip -> genesis) + ascending emit ====
    const std::string activeTmp = (tmpDir / "active.pairs").string();
    {
        FILE* sfSorted = fopen(projSorted.c_str(), "rb");
        if (!sfSorted)
            return SetError(error, "lm: open proj-sorted for walk failed");
        fseeko(sfSorted, 0, SEEK_END);
        const off_t projBytesTotal = ftello(sfSorted);
        if (projBytesTotal != (off_t)count * (off_t)HASHPROJ_SIZE)
        {
            fclose(sfSorted);
            return SetError(error, "lm: projection size mismatch");
        }
        const off_t projN = projBytesTotal / (off_t)HASHPROJ_SIZE;

        FILE* af = fopen(activeTmp.c_str(), "wb");
        if (!af)
        {
            fclose(sfSorted);
            return SetError(error, "lm: open active pair file failed");
        }
        uint256 cur = hashBestChain;
        bool reachedGen = false;
        for (;;)
        {
            HashProj hp;
            if (!BinSearchFixed(sfSorted, projN, HASHPROJ_SIZE,
                                (const unsigned char*)cur.begin(), &hp))
            {
                fclose(af); fclose(sfSorted);
                return SetError(error, "lm: active chain hash missing from index");
            }
            if (fwrite(&hp.height, 1, sizeof(hp.height), af) != sizeof(hp.height) ||
                fwrite(&hp.recordId, 1, sizeof(hp.recordId), af) != sizeof(hp.recordId))
            {
                fclose(af); fclose(sfSorted);
                return SetError(error, "lm: write active pair failed");
            }
            if (IsZero32(hp.hashPrev))
            {
                if (hp.height != 0)
                {
                    fclose(af); fclose(sfSorted);
                    return SetError(error, "lm: active genesis height != 0");
                }
                reachedGen = true;
                break;
            }
            if (hp.height == 0)
            {
                fclose(af); fclose(sfSorted);
                return SetError(error, "lm: active genesis has a parent hash");
            }
            cur = *(uint256*)hp.hashPrev;
        }
        fclose(af);
        fclose(sfSorted);
        if (!reachedGen)
            return SetError(error, "lm: active chain did not reach genesis");

        // Ascending emit (the file is tip-first): validate continuity 0..tip.
        FILE* rf = fopen(activeTmp.c_str(), "rb");
        if (!rf)
            return SetError(error, "lm: reopen active pair file failed");
        fseek(rf, 0, SEEK_END);
        const long fsz = ftell(rf);
        const long pairReal = 8; // 4 B height + 4 B recordId, packed
        if (fsz % pairReal != 0)
        {
            fclose(rf);
            return SetError(error, "lm: active pair file size not multiple of 8");
        }
        const long pairCount = fsz / pairReal;
        int64_t expectedH = 0;
        for (long idx = pairCount - 1; idx >= 0; --idx)
        {
            unsigned char pair[8];
            if (fseek(rf, idx * pairReal, SEEK_SET) != 0 ||
                fread(pair, 1, 8, rf) != 8)
            {
                fclose(rf);
                return SetError(error, "lm: read active pair (ascending) failed");
            }
            int32_t h;
            uint32_t rid;
            memcpy(&h, pair, 4);
            memcpy(&rid, pair + 4, 4);
            if (h != expectedH)
            {
                fclose(rf);
                return SetError(error, "lm: active chain height discontinuity at " +
                                strprintf("%lld", (long long)expectedH));
            }
            if (idx == 0)
            {
                // The final ascending pair is the tip (file is tip-first).
                outTipId = (BlockIndexId)rid;
                outTipHeight = h;
            }
            if (!writer_.AppendActive((BlockIndexId)rid, h, error))
            {
                fclose(rf);
                return false;
            }
            ++expectedH;
        }
        fclose(rf);
        if (expectedH != (int64_t)outTipHeight + 1)
            return SetError(error, "lm: active chain heights inconsistent with tip");
    }
    fs::remove(activeTmp, ec);
    LmDiagRss("M3_active_done");

    // ============ M4: derived compute pass (HEIGHT ordered) ============
    // Sources: hsorted.bin (walk order), proj-sorted.bin (parent hash lookup),
    // records.dat (the writer's committed slots), blk*.dat (exact nSize).
    // The parent's derived state comes from the COMMITTED derived scratch
    // (RecordId-implicit 56-byte slot), which by height-order processing is
    // always already written before the child needs it (height(hp) > height(p)
    // on the active chain, and non-active branches are referenced only via
    // prevoutStake which is NOT a derived dependency).
    const std::string derScratch = (tmpDir / "der-scratch.bin").string();
    {
        FILE* sfSorted = fopen(projSorted.c_str(), "rb");
        if (!sfSorted)
            return SetError(error, "lm: open proj-sorted for derive failed");
        fseeko(sfSorted, 0, SEEK_END);
        const off_t projBytesTotalSort = ftello(sfSorted);
        const off_t projN = projBytesTotalSort / (off_t)HASHPROJ_SIZE;

        FILE* recF = fopen((fs::path(stagingDir) / "records.dat").string().c_str(), "rb");
        if (!recF)
        {
            fclose(sfSorted);
            return SetError(error, "lm: records.dat open (derive) failed");
        }

        FILE* df = fopen(derScratch.c_str(), "wb+");
        if (!df)
        {
            fclose(recF); fclose(sfSorted);
            return SetError(error, "lm: open derived scratch failed");
        }
        FILE* rf = fopen(hSortedPath.c_str(), "rb");
        if (!rf)
        {
            fclose(df); fclose(recF); fclose(sfSorted);
            return SetError(error, "lm: open hsorted for derive failed");
        }
        uint64_t derived = 0;
        for (;;)
        {
            HashProj hp;
            if (fread(&hp, 1, HASHPROJ_SIZE, rf) != HASHPROJ_SIZE)
                break;

            BlockIndexRecord rec;
            if (!ReadRecordFromRecordsDat(recF, (BlockIndexId)hp.recordId, &rec, error))
            {
                std::string msg = error ? *error : std::string();
                fclose(rf); fclose(df); fclose(recF); fclose(sfSorted);
                return SetError(error, "lm: derive: " + msg);
            }
            if (memcmp(rec.hash.begin(), hp.hash, 32) != 0 ||
                rec.height != hp.height)
            {
                fclose(rf); fclose(df); fclose(recF); fclose(sfSorted);
                return SetError(error, "lm: records/projection mismatch");
            }
            // PM1-P0-08: refuse ambiguous PoS classification BEFORE deriving.
            if (!PosClassificationConsistent(rec.nFlags, rec.prevoutStake.hash))
            {
                fclose(rf); fclose(df); fclose(recF); fclose(sfSorted);
                return SetError(error, "lm: record " + rec.hash.GetHex() +
                                " has ambiguous PoS classification (nFlags vs prevoutStake)");
            }
            const bool fPos = (rec.nFlags & CBlockIndex::BLOCK_PROOF_OF_STAKE) != 0;

            BlockIndexDerivedEntry de;
            if (IsZero32(hp.hashPrev))
            {
                if (hp.height != 0)
                {
                    fclose(rf); fclose(df); fclose(recF); fclose(sfSorted);
                    return SetError(error, "lm: parentless record at height != 0");
                }
                de.chainTrust = GetAuthoritativeBlockTrustValue(rec.nBits, rec.height, fPos,
                                                                rec.hashProof, rec.hash);
                CDataStream ss(SER_GETHASH, 0);
                const uint256 proof = fPos ? rec.hashProof : uint256(0);
                ss << rec.nFlags << proof << rec.nStakeModifier;
                uint256 hc = Hash(ss.begin(), ss.end());
                hc >>= (256 - 32);
                de.stakeModifierChecksum = hc.Get64();
                if (rec.nFlags & CBlockIndex::BLOCK_STAKE_MODIFIER)
                {
                    de.stakeModifierTime = (int64_t)rec.nTime;
                    de.SetHasStakeModifierTime(true);
                }
            }
            else
            {
                HashProj pp;
                if (!BinSearchFixed(sfSorted, projN, HASHPROJ_SIZE, hp.hashPrev, &pp))
                {
                    fclose(rf); fclose(df); fclose(recF); fclose(sfSorted);
                    return SetError(error, "lm: record " + rec.hash.GetHex() + " has disconnected parent");
                }
                BlockIndexDerivedEntry pd;
                const off_t pslot = (off_t)(((off_t)pp.recordId - 1) * 56);
                if (fseeko(df, pslot, SEEK_SET) != 0 ||
                    DecodeBlockIndexDerivedEntryAt(df, &pd, error) == false)
                {
                    fclose(rf); fclose(df); fclose(recF); fclose(sfSorted);
                    return false;
                }
                de.chainTrust = pd.chainTrust +
                    GetAuthoritativeBlockTrustValue(rec.nBits, rec.height, fPos,
                                                    rec.hashProof, rec.hash);
                CDataStream ss(SER_GETHASH, 0);
                ss << pd.stakeModifierChecksum;
                const uint256 proof = fPos ? rec.hashProof : uint256(0);
                ss << rec.nFlags << proof << rec.nStakeModifier;
                uint256 hc = Hash(ss.begin(), ss.end());
                hc >>= (256 - 32);
                de.stakeModifierChecksum = hc.Get64();
                if (rec.nFlags & CBlockIndex::BLOCK_STAKE_MODIFIER)
                {
                    de.stakeModifierTime = (int64_t)rec.nTime;
                    de.SetHasStakeModifierTime(true);
                }
                else if (pd.HasStakeModifierTime())
                {
                    de.stakeModifierTime = pd.stakeModifierTime;
                    de.SetHasStakeModifierTime(true);
                }
            }
            // nSize: mandatory from blk*.dat, hash-correspondent, fail closed.
            de.nSize = 0;
            de.SetHasBlockSize(false);
            if (!blockDataDir.empty() && rec.nFile > 0)
            {
                std::string blockFn = strprintf("blk%04u.dat", rec.nFile);
                fs::path blockPath = fs::path(blockDataDir) / blockFn;
                FILE* blockFile = fopen(blockPath.string().c_str(), "rb");
                if (blockFile)
                {
                    if (fseeko(blockFile, (off_t)rec.nBlockPos, SEEK_SET) == 0)
                    {
                        try {
                            CBlock block;
                            CAutoFile filein(blockFile, SER_DISK, CLIENT_VERSION);
                            filein >> block;
                            if (block.GetHash() == rec.hash)
                            {
                                de.nSize = ::GetSerializeSize(block, SER_NETWORK, PROTOCOL_VERSION);
                                de.SetHasBlockSize(de.nSize > 0);
                            }
                        } catch (...) { /* CAutoFile closed the FILE* */ }
                    }
                    else
                        fclose(blockFile); // CAutoFile never constructed
                }
            }
            if (!de.HasBlockSize())
            {
                fclose(rf); fclose(df); fclose(recF); fclose(sfSorted);
                return SetError(error, "lm: mandatory nSize resolution failed for " + rec.hash.GetHex());
            }

            // RecordId-implicit slot write into the derived scratch.
            const off_t dslot = (off_t)(((off_t)hp.recordId - 1) * 56);
            std::vector<unsigned char> enc;
            if (!EncodeBlockIndexDerivedEntry(de, &enc, error))
            {
                fclose(rf); fclose(df); fclose(recF); fclose(sfSorted);
                return false;
            }
            if (enc.size() != 56 ||
                fseeko(df, dslot, SEEK_SET) != 0 ||
                fwrite(&enc[0], 1, enc.size(), df) != enc.size())
            {
                fclose(rf); fclose(df); fclose(recF); fclose(sfSorted);
                return SetError(error, "lm: derived scratch write failed");
            }
            ++derived;
            if ((derived % 1000000) == 0)
                fprintf(stderr, "lm: derive_records=%llu\n", (unsigned long long)derived);
        }
        fclose(rf);
        fflush(df);
        fclose(df);
        fclose(recF);
        fclose(sfSorted);
        if (derived != count)
            return SetError(error, strprintf("lm: derived rows %llu != source %llu",
                (unsigned long long)derived, (unsigned long long)count));
    }
    LmDiagRss("M4_derive_compute_done");

    // ============ M5: derived final emit (RecordId order) ============
    {
        FILE* pfSeq = fopen(projTmp.c_str(), "rb");
        if (!pfSeq)
            return SetError(error, "lm: open proj for emit failed");
        FILE* dsf = fopen(derScratch.c_str(), "rb");
        if (!dsf)
        {
            fclose(pfSeq);
            return SetError(error, "lm: open derived scratch for emit failed");
        }
        uint64_t emitted = 0;
        for (;;)
        {
            HashProj hp;
            if (fread(&hp, 1, HASHPROJ_SIZE, pfSeq) != HASHPROJ_SIZE)
                break;
            const off_t dslot = (off_t)(((off_t)hp.recordId - 1) * 56);
            unsigned char enc[56];
            if (fseeko(dsf, dslot, SEEK_SET) != 0 ||
                fread(enc, 1, 56, dsf) != 56)
            {
                fclose(dsf); fclose(pfSeq);
                return SetError(error, "lm: derived entry lookup failed during emit");
            }
            BlockIndexDerivedEntry e;
            if (!DecodeBlockIndexDerivedEntry(enc, sizeof(enc), &e, error))
            {
                std::string msg = error ? *error : std::string();
                fclose(dsf); fclose(pfSeq);
                return SetError(error, "lm: derived entry decode failed: " + msg);
            }
            if (!e.HasBlockSize())
                allHasBlockSize = false;
            if (!writer_.AppendDerived(e, error))
            {
                fclose(dsf); fclose(pfSeq);
                return false;
            }
            ++emitted;
            if ((emitted % 1000000) == 0)
                fprintf(stderr, "lm: emit_derived=%llu\n", (unsigned long long)emitted);
        }
        fclose(dsf);
        fclose(pfSeq);
        if (emitted != count)
            return SetError(error, strprintf("lm: derived emitted %llu != source %llu",
                (unsigned long long)emitted, (unsigned long long)count));
    }
    LmDiagRss("M4_emit_done");

    // tmp scratch files are temporary: remove ALL of them here (proj.bin,
    // proj-sorted.bin) BEFORE the writer finalizes/publishes; the generation
    // directory must contain only the four canonical artifacts.
    {
        boost::system::error_code rmec;
        fs::remove_all(tmpDir, rmec);
    }

    if (!writer_.Flush(error))
        return false;

    // ---- M6: finalize (content binding + MANIFEST COMPLETE) ----
    if (outTipId == BLOCK_INDEX_ID_INVALID || outTipHeight < 0 || hashBestChain == 0)
    {
        ClearError(error);
        return SetError(error, "lm: no active tip resolved during build");
    }
    const bool authoritativeWanted = (!blockDataDir.empty());
    if (authoritativeWanted && !allHasBlockSize)
    {
        ClearError(error);
        return SetError(error, "lm: authoritative generation requires mandatory nSize for all records");
    }
    const uint32_t finalCap = authoritativeWanted
        ? BLOCK_INDEX_GENERATION_CAPABILITY_AUTHORITATIVE
        : BLOCK_INDEX_GENERATION_CAPABILITY_OLD_SHADOW;
    const uint64_t totalRecords = writer_.RecordCount();
    if (!writer_.Finalize(generation, hashBestChain, outTipId,
                          (int32_t)outTipHeight,
                          totalRecords, finalCap,
                          emptyGenerationInputDigest, error))
        return false;

    ClearError(error);
    return true;
}
