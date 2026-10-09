// COHORT M (M3/M7/M9) — bounded measurement harness: synthetic legacy datadir
// generator.
//
// Writes a synthetic LEGACY block-index datadir:
//   <out_datadir>/txleveldb/   (leveldb: "blockindex" records + hashBestChain)
//   <out_datadir>/blk0001.dat  (the serialized block for EVERY record, at the
//                               record's own nBlockPos slot)
//
// With the block file present, BOTH builders run the AUTHORITATIVE capability
// (exact nSize from block data) — the capability production migration uses — so
// the two paths can be compared byte-for-byte, and the production migration path
// can be exercised end-to-end on a datadir of a chosen size.
//
// Usage: cohortm-gensnapshot <out_datadir> <N> [--side-every K] [--no-blk]
//
// This tool is a DIAGNOSTIC. It never reads or writes any real datadir.

#include "main.h"
#include "ui_interface.h"
#include "checkpoints.h"
#include "wallet.h"

#include <leveldb/db.h>
#include <leveldb/options.h>

#include <boost/filesystem.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace fs = boost::filesystem;

// Globals required to link the shared engine OBJS (mirrors the other tools).
CWallet* pwalletMain;
CClientUIInterface uiInterface;
bool fConfChange = false;
bool fEnforceCanonical = true;
bool fUseFastIndex = true;
unsigned int nDerivationMethodIndex = 0;
unsigned int nMinerSleep = 5000;
unsigned int nNodeLifespan = 7;
enum Checkpoints::CPMode CheckpointsMode = Checkpoints::STRICT;
void Shutdown(void* parg) { exit(0); }
void StartShutdown() { exit(0); }

static const unsigned BLOCK_SLOT = 4096; // bytes per block slot in blk0001.dat
static const unsigned BLOCK_BASE = 4096; // first slot offset

struct GenRec
{
    int64_t slot;
    uint256 hash;
    CDiskBlockIndex disk;
};

static CBlock HeaderFromRecord(const CDiskBlockIndex& bi)
{
    CBlock b;
    b.nVersion = bi.nVersion;
    b.hashPrevBlock = bi.hashPrev;
    b.hashMerkleRoot = bi.hashMerkleRoot;
    b.nTime = bi.nTime;
    b.nBits = bi.nBits;
    b.nNonce = bi.nNonce;
    return b;
}

static CDiskBlockIndex MakeRecord(int64_t h, int64_t slot, const uint256& parent,
                                  unsigned nonce, unsigned timeAdj)
{
    CDiskBlockIndex bi;
    bi.nHeight = (int)h;
    bi.nFile = 1;
    bi.nBlockPos = BLOCK_BASE + (unsigned)(slot * BLOCK_SLOT);
    bi.hashPrev = parent;
    bi.nVersion = 7;
    bi.nTime = 1700000000u + (unsigned)h + timeAdj;
    bi.nBits = 0x1d00ffff;
    bi.nNonce = nonce;
    bi.nFlags = 0;
    bi.hashMerkleRoot = uint256(0x1111ULL + (uint64_t)h);
    bi.hashProof = uint256(0x2222ULL + (uint64_t)h);
    if (h > 0)
    {
        bi.prevoutStake = COutPoint(uint256((unsigned)h), 0);
        bi.nStakeTime = (uint32_t)(1700000000u + (unsigned)h);
    }
    bi.nMint = 100 + (int64_t)h;
    bi.nMoneySupply = 500 + (int64_t)h * 3;
    return bi;
}

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        fprintf(stderr, "usage: %s <out_datadir> <N> [--side-every K] [--no-blk]\n", argv[0]);
        return 2;
    }
    const std::string outDir = argv[1];
    const int64_t N = atoll(argv[2]);
    int64_t sideEvery = 500;
    bool wantBlk = true;
    for (int i = 3; i < argc; ++i)
    {
        if (std::string(argv[i]) == "--side-every" && i + 1 < argc)
            sideEvery = atoll(argv[++i]);
        else if (std::string(argv[i]) == "--no-blk")
            wantBlk = false;
    }
    if (N <= 0)
    {
        fprintf(stderr, "FATAL: N must be > 0\n");
        return 2;
    }

    // ---- 1. Build the records in memory (N main + side) ----
    std::vector<GenRec> recs;
    recs.reserve((size_t)(N + N / (sideEvery > 0 ? sideEvery : 1) + 1));
    std::vector<uint256> mainHashes(N);
    for (int64_t h = 0; h < N; ++h)
    {
        const uint256 parent = (h == 0) ? uint256(0) : mainHashes[h - 1];
        CDiskBlockIndex bi = MakeRecord(h, h, parent, (unsigned)h, 0);
        GenRec g; g.slot = h; g.disk = bi; g.hash = bi.GetBlockHash();
        mainHashes[h] = g.hash;
        recs.push_back(g);
    }
    // Side branches share the parent with the main block at the same height and
    // get their own slot (so their blk bytes + nSize are exact too).
    int64_t sideCount = 0;
    int64_t nextSlot = N;
    if (sideEvery > 0)
    {
        for (int64_t h = 1; h < N; h += sideEvery)
        {
            CDiskBlockIndex bi = MakeRecord(h, nextSlot, mainHashes[h - 1],
                                            (unsigned)(h ^ 0x5555), 7);
            GenRec g; g.slot = nextSlot; g.disk = bi; g.hash = bi.GetBlockHash();
            recs.push_back(g);
            ++nextSlot; ++sideCount;
        }
    }
    const uint256 bestHash = mainHashes[N - 1];
    if (bestHash == uint256(0))
    {
        fprintf(stderr, "FATAL: zero tip hash\n");
        return 1;
    }

    // ---- 2. Write the leveldb snapshot ----
    boost::system::error_code ec;
    if (fs::exists(outDir))
        fs::remove_all(outDir, ec);
    const std::string dbDir = (fs::path(outDir) / "txleveldb").string();
    fs::create_directories(dbDir, ec);

    leveldb::Options options;
    options.create_if_missing = true;
    options.error_if_exists = false;
    leveldb::DB* db = NULL;
    leveldb::Status st = leveldb::DB::Open(options, dbDir, &db);
    if (!st.ok())
    {
        fprintf(stderr, "FATAL: leveldb open failed: %s\n", st.ToString().c_str());
        return 1;
    }
    for (size_t i = 0; i < recs.size(); ++i)
    {
        CDataStream ssKey(SER_DISK, CLIENT_VERSION);
        ssKey << make_pair(std::string("blockindex"), recs[i].hash);
        CDataStream ssVal(SER_DISK, CLIENT_VERSION);
        ssVal << recs[i].disk;
        leveldb::Status s = db->Put(leveldb::WriteOptions(), ssKey.str(), ssVal.str());
        if (!s.ok())
        {
            fprintf(stderr, "FATAL: leveldb put failed: %s\n", s.ToString().c_str());
            return 1;
        }
    }
    {
        CDataStream ssKey(SER_DISK, CLIENT_VERSION);
        ssKey << std::string("hashBestChain");
        CDataStream ssVal(SER_DISK, CLIENT_VERSION);
        ssVal << bestHash;
        if (!db->Put(leveldb::WriteOptions(), ssKey.str(), ssVal.str()).ok())
        {
            fprintf(stderr, "FATAL: hashBestChain put failed\n");
            return 1;
        }
    }
    if (wantBlk)
        db->CompactRange(NULL, NULL);
    delete db;

    // ---- 3. Write blk0001.dat (one slot per record, hash-verified) ----
    int64_t blkWritten = 0, blkMismatch = 0;
    if (wantBlk)
    {
        const std::string blkPath = (fs::path(outDir) / "blk0001.dat").string();
        FILE* bf = fopen(blkPath.c_str(), "wb");
        if (!bf)
        {
            fprintf(stderr, "FATAL: cannot create %s\n", blkPath.c_str());
            return 1;
        }
        for (size_t i = 0; i < recs.size(); ++i)
        {
            CBlock b = HeaderFromRecord(recs[i].disk);
            if (b.GetHash() != recs[i].hash)
            {
                ++blkMismatch;
                continue;
            }
            const long off = (long)(BLOCK_BASE + recs[i].slot * BLOCK_SLOT);
            if (fseek(bf, off, SEEK_SET) != 0) { fprintf(stderr, "FATAL: fseek\n"); return 1; }
            CDataStream ssVal(SER_DISK, CLIENT_VERSION);
            ssVal << b;
            if (fwrite(&ssVal[0], 1, ssVal.size(), bf) != (size_t)ssVal.size())
            {
                fprintf(stderr, "FATAL: fwrite blk\n");
                return 1;
            }
            ++blkWritten;
        }
        fclose(bf);
    }

    printf("GEN datadir=%s main=%lld side=%lld total=%lld blk_written=%lld blk_mismatch=%lld tip=%s\n",
           outDir.c_str(), (long long)N, (long long)sideCount, (long long)recs.size(),
           (long long)blkWritten, (long long)blkMismatch, bestHash.ToString().c_str());
    fflush(stdout);
    return blkMismatch == 0 ? 0 : 1;
}
