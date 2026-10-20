// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.
//
// Disk-lean single-pass streaming V1->V2 migration builder (S16b).
//
// The previous staged pipeline held the equivalent of three full copies of the
// index on scratch (candidates file + external sort runs + two side LevelDB
// indexes + a fully preallocated derived.dat = ~10.3 GB measured at the real
// 8.17M scale). This builder cuts the staging peak to under 5 GB with bounded
// (<= ~300 MB) RSS:
//
//   pass 1 (the ONLY full scan): the legacy txleveldb snapshot iterator, which
//     is ordered by the "blockindex" key suffix = the block hash bytes. This is
//     the SAME byte order the staged pipeline's hash sort produced (verified
//     byte-for-byte against the staged builder's published generation).
//     During the scan it streams the FINAL records.dat (batched appends), the
//     FINAL hashindex (LevelDB keys self-order; insertion order is irrelevant),
//     and a fixed 84-byte hash-projection row per record
//     (hash || hashPrev || height || nFile || nBlockPos || recordId).
//
//   Then: one external sort of the projections by (height, hash) gives the
//     height-ordered consumption stream for the derived pass. The projection
//     scratch itself IS the hash-keyed ancestry index (binary-searchable on
//     the raw scan byte order, fixed-size entries, bounded RAM, O(log N)
//     reads). No sorted runs for records, no actidx LevelDB, no deridx
//     LevelDB, no second records copy.
//
//   derived compute pass (height order): each record's derived entry comes
//     from its parent's already-written derived scratch slot (one pread,
//     parent recordId resolved by binary search over the hash index).
//     Bounded state: one DEntry in flight.
//
//   final emit (hash order = RecordId order, sort-free): AppendDerived
//     streams derived.dat exactly as the staged builder did.
//
//   active chain (height order): the real tip from hashBestChain fails closed
//     when absent; the pprev walk resolves every parent through the on-disk
//     hash index; active.dat is fed ascending 0..tip.
//
// Everything except records.dat / hashindex / derived.dat + MANIFEST lives
// under <stagingDir>/.lm-tmp and is removed BEFORE finalize/publish, so the
// published generation directory holds only the canonical artifacts.
//
// Fail-closed contracts preserved exactly:
//   - mandatory nSize resolution from blk*.dat with hash correspondence: any
//     record without it fails the WHOLE build (never a silent OLD_SHADOW);
//   - duplicate hash / decode error / truncated snapshot = whole-build failure;
//   - disconnected parent / missing ancestry hash / height discontinuity /
//     non-genesis parentless record = whole-build failure;
//   - stale staging is removed by the writer; published gen-N is never
//     overwritten (PublishGeneration refuses).
//
// No consensus validation (CheckBlock/ConnectBlock/PoW/signature/transaction
// rules) is invoked anywhere in this path: the migration is a storage-format
// conversion of already-validated state. Structural integrity only (see the
// run report section "no-consensus-validation-during-migration").

#include "blockindex_generation_builder_lm.h"

#include "main.h"
#include "kernel.h"

#include <leveldb/cache.h>
#include <leveldb/db.h>
#include <leveldb/filter_policy.h>
#include <leveldb/write_batch.h>

#include <malloc.h>

#include <boost/filesystem.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace fs = boost::filesystem;

// V2-R1 test-only fault-injection seams (default false; NEVER set by
// production code). Preserved for the negative-test surface.
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

// Derived scratch entry (native ABI, disposable same-process scratch — NOT a
// persisted format). One per record in the derived compute pass, keyed
// implicitly by recordId.
struct DEntry {
    uint256 chainTrust;
    uint32_t checksum;
    int64_t modTime;
    bool hasModTime;
    uint32_t nSize;
    bool hasBlockSize;
};

static const size_t DENTRY_SIZE = sizeof(DEntry);

// Binary search over a packed fixed-record file keyed by the first 32 bytes
// (raw memcmp). Returns true and fills \p rec (recordSize bytes) when found.
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

static std::string HashBytesToHex(const unsigned char h[32])
{
    static const char* hexd = "0123456789abcdef";
    std::string s;
    s.reserve(64);
    for (int i = 0; i < 32; ++i)
    {
        s.push_back(hexd[h[i] >> 4]);
        s.push_back(hexd[h[i] & 0xf]);
    }
    return s;
}

static bool IsZero32(const unsigned char h[32])
{
    unsigned char zero[32];
    memset(zero, 0, 32);
    return memcmp(h, zero, 32) == 0;
}

} // namespace

BlockIndexGenerationBuilderLM::BlockIndexGenerationBuilderLM()
{
}

BlockIndexGenerationBuilderLM::~BlockIndexGenerationBuilderLM()
{
}

bool BlockIndexGenerationBuilderLM::BuildStagedPipeline(const std::string& snapshotLevelDbDir,
                                          const std::string& blockDataDir,
                                          uint64_t generation,
                                          const std::string& stagingDir,
                                          std::string* error)
{
    BlockIndexId outTipId = BLOCK_INDEX_ID_INVALID;
    int64_t outTipHeight = -1;
    // Hoisted: becomes false if any derived entry lacks a live nSize -> gates
    // whether M6 may claim AUTHORITATIVE capability (fail closed, same as the
    // staged builder).
    bool allHasBlockSize = true;
    // SHA256("") — the retired DAG input set is permanently empty, exactly as
    // the staged builder produced (dag-inert chain).
    unsigned char emptyGenerationInputDigest[32];
    SHA256_CTX shaCtx;
    SHA256_Init(&shaCtx);
    SHA256_Final(emptyGenerationInputDigest, &shaCtx);

    // ---- pass 1 setup: bounded snapshot open (M8 RSS lesson) ----
    leveldb::Options options;
    options.create_if_missing = false;
    options.error_if_exists = false;
    options.filter_policy = leveldb::NewBloomFilterPolicy(10);
    options.block_cache = leveldb::NewLRUCache(512 * 1024);
    options.write_buffer_size = 1 * 1024 * 1024;
    options.max_open_files = 64;
    leveldb::DB* db = NULL;
    leveldb::Status status = leveldb::DB::Open(options, snapshotLevelDbDir, &db);
    if (!status.ok())
        return SetError(error, std::string("lm: snapshot LevelDB open failure: ") +
                               status.ToString());
    LmDiagRss("M0_after_db_open");

    // Writer creates build-N.tmp fresh (removes stale staging: restartable).
    if (!writer_.OpenTarget(stagingDir, generation, error))
    {
        delete db;
        return false;
    }

    // hashBestChain = the authoritative active-chain tip. Fail closed when
    // absent/ambiguous (no heuristic tip discovery here).
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
        if (hashBestChain == uint256(0))
        {
            delete db;
            return SetError(error, "lm: hashBestChain present but null (ambiguous tip)");
        }
    }
    LmDiagRss("M0_tip_resolved");

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

    // ============ pass 1: the single full scan ============
    uint64_t count = 0;
    std::string scanError;
    bool scanOk = true;
    {
        leveldb::Iterator* it = db->NewIterator(leveldb::ReadOptions());
        CDataStream ssStartKey(SER_DISK, CLIENT_VERSION);
        ssStartKey << make_pair(std::string("blockindex"), uint256(0));
        it->Seek(ssStartKey.str());
        unsigned char prevHash[32];
        bool havePrev = false;
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
                scanOk = false;
                scanError = "lm: decode blockindex failed";
                break;
            }
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
            HashProj proj;
            memset(&proj, 0, sizeof(proj));
            memcpy(proj.hash, rec.hash.begin(), 32);
            memcpy(proj.hashPrev, rec.hashPrev.begin(), 32);
            proj.height = rec.height;
            proj.nFile = rec.nFile;
            proj.nBlockPos = rec.nBlockPos;
            proj.recordId = (uint32_t)id;
            if (fwrite(&proj, 1, HASHPROJ_SIZE, pf) != HASHPROJ_SIZE)
            {
                delete it; fclose(pf); delete db;
                return SetError(error, "lm: write projection failed");
            }
            // Duplicate-hash fail closed: the scan is strictly ascending on the
            // key hash bytes, so any repeat == corrupted source index (the
            // staged builder failed via its k-way merge check).
            if (havePrev && memcmp(prevHash, proj.hash, 32) == 0)
            {
                scanOk = false;
                scanError = "lm: duplicate block hash in source";
                break;
            }
            memcpy(prevHash, proj.hash, 32);
            havePrev = true;
            ++count;
            it->Next();
            if ((count % 1000000) == 0)
                fprintf(stderr, "lm: scan_records=%llu\n", (unsigned long long)count);
        }
        delete it;
    }
    fclose(pf);
    delete db;
    if (!scanOk)
    {
        fs::remove_all(tmpDir, ec);
        return SetError(error, scanError);
    }
    LmDiagRss("M1_pass_done");

    // ============ external height sort of the projections ============
    // Bounded RAM: 64k-row chunks (~5.4 MB), sorted runs, k-way merge with one
    // 84-byte head per run.
    const size_t kChunk = 65536;
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
                return SetError(error, "lm: truncated projection (sort)");
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

    // Open the canonical artifacts for the derived/ancestry passes:
    //   pfIdxRead : proj.bin (hash-ordered ancestry index, binary search)
    //   recF      : records.dat (committed random reads at 40+(id-1)*228)
    // Both are files this process itself wrote during pass 1; committed slots
    // only.
    FILE* pfIdxRead = fopen(projTmp.c_str(), "rb");
    if (!pfIdxRead)
        return SetError(error, "lm: open projection index (read) failed");
    fseeko(pfIdxRead, 0, SEEK_END);
    const off_t projBytesTotal = ftello(pfIdxRead);
    if (projBytesTotal != (off_t)count * (off_t)HASHPROJ_SIZE)
    {
        fclose(pfIdxRead);
        return SetError(error, "lm: projection size mismatch");
    }
    const off_t projN = projBytesTotal / (off_t)HASHPROJ_SIZE;
    FILE* recF = fopen((fs::path(stagingDir) / "records.dat").string().c_str(), "rb");
    if (!recF)
    {
        fclose(pfIdxRead);
        return SetError(error, "lm: records.dat open (derive) failed");
    }

    // ============ derived compute pass (height order) ============
    const std::string derScratch = (tmpDir / "der-scratch.bin").string();
    {
        FILE* df = fopen(derScratch.c_str(), "wb+");
        if (!df)
        {
            fclose(recF); fclose(pfIdxRead);
            return SetError(error, "lm: open derived scratch failed");
        }
        FILE* rf = fopen(hSortedPath.c_str(), "rb");
        if (!rf)
        {
            fclose(df); fclose(recF); fclose(pfIdxRead);
            return SetError(error, "lm: open hsorted for derive failed");
        }
        uint64_t derived = 0;
        for (;;)
        {
            HashProj hp;
            if (fread(&hp, 1, HASHPROJ_SIZE, rf) != HASHPROJ_SIZE)
                break;

            BlockIndexRecord rec;
            {
                const off_t slot = (off_t)(40 + ((off_t)hp.recordId - 1) * 228);
                unsigned char raw[228];
                if (fseeko(recF, slot, SEEK_SET) != 0 ||
                    fread(raw, 1, 228, recF) != 228)
                {
                    fclose(rf); fclose(df); fclose(recF); fclose(pfIdxRead);
                    return SetError(error, "lm: derive: records.dat read failed");
                }
                // decode (CRC-checked, fail closed)
                if (!DecodeBlockIndexRecordV1(raw, sizeof(raw), &rec, error))
                {
                    std::string msg = error ? *error : std::string();
                    fclose(rf); fclose(df); fclose(recF); fclose(pfIdxRead);
                    return SetError(error, "lm: record decode failure during derive: " + msg);
                }
                if (memcmp(rec.hash.begin(), hp.hash, 32) != 0 ||
                    rec.height != hp.height)
                {
                    fclose(rf); fclose(df); fclose(recF); fclose(pfIdxRead);
                    return SetError(error, "lm: records/projection mismatch");
                }
            }
            // PM1-P0-08: refuse ambiguous PoS classification BEFORE deriving.
            if (!PosClassificationConsistent(rec.nFlags, rec.prevoutStake.hash))
            {
                fclose(rf); fclose(df); fclose(recF); fclose(pfIdxRead);
                return SetError(error, "lm: record " + rec.hash.GetHex() +
                                " has ambiguous PoS classification (nFlags vs prevoutStake)");
            }
            const bool fPos = (rec.nFlags & CBlockIndex::BLOCK_PROOF_OF_STAKE) != 0;

            DEntry de;
            memset(&de, 0, sizeof(de));
            if (IsZero32(hp.hashPrev))
            {
                if (hp.height != 0)
                {
                    fclose(rf); fclose(df); fclose(recF); fclose(pfIdxRead);
                    return SetError(error, "lm: parentless record at height != 0");
                }
                de.chainTrust = GetAuthoritativeBlockTrustValue(rec.nBits, rec.height, fPos,
                                                                rec.hashProof, rec.hash);
                CDataStream ss(SER_GETHASH, 0);
                const uint256 proof = fPos ? rec.hashProof : uint256(0);
                ss << rec.nFlags << proof << rec.nStakeModifier;
                uint256 hc = Hash(ss.begin(), ss.end());
                hc >>= (256 - 32);
                de.checksum = hc.Get64();
                if (rec.nFlags & CBlockIndex::BLOCK_STAKE_MODIFIER)
                {
                    de.modTime = (int64_t)rec.nTime;
                    de.hasModTime = true;
                }
            }
            else
            {
                HashProj pp;
                if (!BinSearchFixed(pfIdxRead, projN, HASHPROJ_SIZE, hp.hashPrev, &pp))
                {
                    fclose(rf); fclose(df); fclose(recF); fclose(pfIdxRead);
                    return SetError(error, "lm: record " + rec.hash.GetHex() + " has disconnected parent " +
                                    HashBytesToHex(hp.hashPrev));
                }
                DEntry pd;
                if (g_lmV2R1ForceDerivedLookupError)
                {
                    fclose(rf); fclose(df); fclose(recF); fclose(pfIdxRead);
                    return SetError(error, "injected deridx lookup error");
                }
                const off_t pslot = (off_t)(((off_t)pp.recordId - 1) * DENTRY_SIZE);
                if (fseeko(df, pslot, SEEK_SET) != 0 ||
                    fread(&pd, 1, DENTRY_SIZE, df) != DENTRY_SIZE)
                {
                    fclose(rf); fclose(df); fclose(recF); fclose(pfIdxRead);
                    return SetError(error, "lm: parent derived scratch missing/corrupt");
                }
                de.chainTrust = pd.chainTrust +
                    GetAuthoritativeBlockTrustValue(rec.nBits, rec.height, fPos,
                                                    rec.hashProof, rec.hash);
                CDataStream ss(SER_GETHASH, 0);
                ss << pd.checksum;
                const uint256 proof = fPos ? rec.hashProof : uint256(0);
                ss << rec.nFlags << proof << rec.nStakeModifier;
                uint256 hc = Hash(ss.begin(), ss.end());
                hc >>= (256 - 32);
                de.checksum = hc.Get64();
                if (rec.nFlags & CBlockIndex::BLOCK_STAKE_MODIFIER)
                {
                    de.modTime = (int64_t)rec.nTime;
                    de.hasModTime = true;
                }
                else if (pd.hasModTime)
                {
                    de.modTime = pd.modTime;
                    de.hasModTime = true;
                }
            }
            // nSize: mandatory from blk*.dat, hash-correspondent, fail closed.
            de.nSize = 0;
            de.hasBlockSize = false;
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
                                de.hasBlockSize = (de.nSize > 0);
                            }
                        } catch (...) { /* CAutoFile closed the FILE* */ }
                    }
                    else
                        fclose(blockFile); // CAutoFile never constructed
                }
            }
            if (g_lmV2R1ForceDerivedPutFailure)
            {
                fclose(rf); fclose(df); fclose(recF); fclose(pfIdxRead);
                return SetError(error, "lm: injected deridx put failure");
            }

            // RecordId-implicit slot write.
            const off_t dslot = (off_t)(((off_t)hp.recordId - 1) * DENTRY_SIZE);
            if (fseeko(df, dslot, SEEK_SET) != 0 ||
                fwrite(&de, 1, DENTRY_SIZE, df) != DENTRY_SIZE)
            {
                fclose(rf); fclose(df); fclose(recF); fclose(pfIdxRead);
                return SetError(error, "lm: derived scratch write failed");
            }
            ++derived;
            if ((derived % 1000000) == 0)
                fprintf(stderr, "lm: derive_records=%llu\n", (unsigned long long)derived);
        }
        fclose(rf);
        fflush(df);
        fclose(df);
        if (derived != count)
        {
            fclose(recF); fclose(pfIdxRead);
            return SetError(error, strprintf("lm: derived rows %llu != source %llu",
                (unsigned long long)derived, (unsigned long long)count));
        }
    }
    LmDiagRss("M4_derive_compute_done");

    // ============ final emit: derived.dat in hash (RecordId) order ============
    {
        FILE* pfSeq = fopen(projTmp.c_str(), "rb");
        if (!pfSeq)
        {
            fclose(recF); fclose(pfIdxRead);
            return SetError(error, "lm: open proj for emit failed");
        }
        FILE* dsf = fopen(derScratch.c_str(), "rb");
        if (!dsf)
        {
            fclose(pfSeq); fclose(recF); fclose(pfIdxRead);
            return SetError(error, "lm: open derived scratch for emit failed");
        }
        uint64_t emitted = 0;
        for (;;)
        {
            HashProj hp;
            if (fread(&hp, 1, HASHPROJ_SIZE, pfSeq) != HASHPROJ_SIZE)
                break;
            const off_t dslot = (off_t)(((off_t)hp.recordId - 1) * DENTRY_SIZE);
            DEntry de;
            if (fseeko(dsf, dslot, SEEK_SET) != 0 ||
                fread(&de, 1, DENTRY_SIZE, dsf) != DENTRY_SIZE)
            {
                fclose(dsf); fclose(pfSeq); fclose(recF); fclose(pfIdxRead);
                return SetError(error, "lm: derived entry lookup failed during emit");
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
                fclose(dsf); fclose(pfSeq); fclose(recF); fclose(pfIdxRead);
                return false;
            }
            ++emitted;
            if ((emitted % 1000000) == 0)
                fprintf(stderr, "lm: emit_derived=%llu\n", (unsigned long long)emitted);
        }
        fclose(dsf);
        fclose(pfSeq);
        if (emitted != count)
        {
            fclose(recF); fclose(pfIdxRead);
            return SetError(error, strprintf("lm: derived emitted %llu != source %llu",
                (unsigned long long)emitted, (unsigned long long)count));
        }
    }
    LmDiagRss("M4_emit_done");

    // ============ active chain (height order, tip -> genesis) ============
    const std::string activeTmp = (tmpDir / "active.pairs").string();
    {
        FILE* af = fopen(activeTmp.c_str(), "wb");
        if (!af)
        {
            fclose(recF); fclose(pfIdxRead);
            return SetError(error, "lm: open active pair file failed");
        }
        uint256 cur = hashBestChain;
        bool reachedGen = false;
        for (;;)
        {
            HashProj hp;
            if (!BinSearchFixed(pfIdxRead, projN, HASHPROJ_SIZE,
                                (const unsigned char*)cur.begin(), &hp))
            {
                fclose(af); fclose(recF); fclose(pfIdxRead);
                return SetError(error, "lm: active chain hash missing from index: " +
                                HashBytesToHex(hp.hash));
            }
            if (fwrite(&hp.height, 1, sizeof(hp.height), af) != sizeof(hp.height) ||
                fwrite(&hp.recordId, 1, sizeof(hp.recordId), af) != sizeof(hp.recordId))
            {
                fclose(af); fclose(recF); fclose(pfIdxRead);
                return SetError(error, "lm: write active pair failed");
            }
            if (IsZero32(hp.hashPrev))
            {
                if (hp.height != 0)
                {
                    fclose(af); fclose(recF); fclose(pfIdxRead);
                    return SetError(error, "lm: active genesis height != 0");
                }
                reachedGen = true;
                break;
            }
            cur = *(uint256*)hp.hashPrev;
        }
        fclose(af);
        if (!reachedGen)
        {
            fclose(recF); fclose(pfIdxRead);
            return SetError(error, "lm: active chain did not reach genesis");
        }

        // Ascending emit (the file is tip-first): validate continuity 0..tip.
        FILE* rf = fopen(activeTmp.c_str(), "rb");
        if (!rf)
        {
            fclose(recF); fclose(pfIdxRead);
            return SetError(error, "lm: reopen active pair file failed");
        }
        fseek(rf, 0, SEEK_END);
        const long fsz = ftell(rf);
        const long pairBytes = 12; // i32 height + u32 recordId + pad... actual 8; use packed pairs
        // Actual pair layout on disk: 4 B height + 4 B id = 8 B.
        const long pairReal = 8;
        if (fsz % pairReal != 0)
        {
            fclose(rf); fclose(recF); fclose(pfIdxRead);
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
                fclose(rf); fclose(recF); fclose(pfIdxRead);
                return SetError(error, "lm: read active pair (ascending) failed");
            }
            int32_t h;
            uint32_t rid;
            memcpy(&h, pair, 4);
            memcpy(&rid, pair + 4, 4);
            if (h != expectedH)
            {
                fclose(rf); fclose(recF); fclose(pfIdxRead);
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
                fclose(rf); fclose(recF); fclose(pfIdxRead);
                return false;
            }
            ++expectedH;
        }
        fclose(rf);
        if (expectedH != (int64_t)outTipHeight + 1)
        {
            fclose(recF); fclose(pfIdxRead);
            return SetError(error, "lm: active chain heights inconsistent with tip");
        }
    }
    fs::remove(activeTmp, ec);
    LmDiagRss("M3_active_done");

    fclose(recF);
    fclose(pfIdxRead);

    // ============ scratch removal BEFORE finalize/publish ============
    {
        boost::system::error_code rmec;
        fs::remove_all(tmpDir, rmec);
    }

    if (!writer_.Flush(error))
        return false;

    // ============ M6: finalize ============
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
    const uint64_t totalRecords = writer_.RecordCount(); // before Finalize closes
    if (!writer_.Finalize(generation, hashBestChain, outTipId,
                          (int32_t)outTipHeight,
                          totalRecords, finalCap,
                          emptyGenerationInputDigest, error))
        return false;

    ClearError(error);
    return true;
}
