
// Standalone legacy-chain fixture generator (scale runs only; NOT a test).
//
// Writes a persisted legacy 'blockindex'+hash / 'hashBestChain' LevelDB shaped
// exactly like the G2 catchup-test fixture (real CDiskBlockIndex encoder).
// Links against the engine OBJS (same pattern as the tool CLIs).
//
// invocation: fixtures_gen_scale <leveldbDir> <tip-height>
#include "uint256.h"
#include "main.h"
#include "serialize.h"
#include "ui_interface.h"
#include "checkpoints.h"
#include "wallet.h"
#include <leveldb/db.h>
#include <leveldb/filter_policy.h>
#include <boost/filesystem.hpp>
#include <cstdio>

// Engine globals (mirrors the tool CLI mains).
CWallet* pwalletMain;
CClientUIInterface uiInterface;
bool fConfChange = false;
bool fEnforceCanonical = true;
bool fUseFastIndex = true;
unsigned int nDerivationMethodIndex = 0;
unsigned int nMinerSleep = 5000;
unsigned int nNodeLifespan = 7;
enum Checkpoints::CPMode CheckpointsMode = Checkpoints::STRICT;
void Shutdown(void*) { exit(0); }
void StartShutdown() { exit(0); }

int main(int argc, char** argv)
{
    if (argc < 3) { fprintf(stderr, "usage: gen <leveldbDir> tip\n"); return 2; }
    int tip = atoi(argv[2]);
    boost::filesystem::create_directories(argv[1]);
    leveldb::Options options;
    options.create_if_missing = true;
    options.error_if_exists = true;
    options.filter_policy = leveldb::NewBloomFilterPolicy(10);
    leveldb::DB* db = NULL;
    if (!leveldb::DB::Open(options, argv[1], &db).ok()) { fprintf(stderr, "open fail\n"); return 1; }
    std::vector<uint256> hashes((size_t)tip + 1);
    for (int h = 0; h <= tip; ++h)
    {
        CDiskBlockIndex bi;
        bi.nHeight = h;
        bi.nFile = 1;
        bi.nBlockPos = (unsigned)(100 + h);
        bi.hashPrev = (h == 0) ? uint256(0) : hashes[(size_t)h - 1];
        bi.nVersion = 7;
        bi.nTime = 1700000000u + (unsigned)h;
        bi.nBits = 0x1d00ffff;
        bi.nNonce = (unsigned)h;
        bi.nFlags = 0;
        bi.hashMerkleRoot = uint256(0x1111ULL + h);
        bi.hashProof = uint256(0x2222ULL + h);
        bi.nMint = 100 + h;
        bi.nMoneySupply = 500 + h * 3;
        hashes[(size_t)h] = bi.GetBlockHash();
        CDataStream ssKey(SER_DISK, CLIENT_VERSION);
        ssKey << make_pair(std::string("blockindex"), hashes[(size_t)h]);
        CDataStream ssVal(SER_DISK, CLIENT_VERSION);
        ssVal << bi;
        if (!db->Put(leveldb::WriteOptions(), ssKey.str(), ssVal.str()).ok())
        { fprintf(stderr, "put fail at %d\n", h); return 1; }
        if ((h & 0xFFFF) == 0) { fprintf(stderr, "gen h=%d\n", h); fflush(stderr); }
    }
    {
        CDataStream ssKey(SER_DISK, CLIENT_VERSION);
        ssKey << std::string("hashBestChain");
        CDataStream ssVal(SER_DISK, CLIENT_VERSION);
        ssVal << hashes[(size_t)tip];
        db->Put(leveldb::WriteOptions(), ssKey.str(), ssVal.str());
    }
    delete db;
    fprintf(stdout, "generated %d records\n", tip + 1);
    return 0;
}
