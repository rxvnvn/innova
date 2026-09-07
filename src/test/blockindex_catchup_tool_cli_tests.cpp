// Functional test for the standalone blockindex-catchup-tool CLI.
//
// Proves the committed CLI executable (blockindex_catchup_tool_cli.cpp /
// makefile target blockindex-catchup-tool) performs a REAL S->L catch-up
// against real generated fixtures and lands at exact L, by invoking the
// actual on-disk binary and asserting its CATCHUP_OK output. This complements
// the library-level G2-A..F tests (which call RunBlockIndexCatchup directly)
// by exercising the production executable entry point end-to-end.
//
// This file links into the shared test_innova binary (test_innova.o provides
// the BOOST_MAIN + engine globals), so it must NOT define its own
// BOOST_TEST_MODULE / global fixture / engine globals — that would collide.
#include <boost/test/unit_test.hpp>

#include "blockindex_generation_lifecycle.h"
#include "blockindex_generation_builder.h"
#include "serialize.h"
#include "uint256.h"
#include "main.h"
#include "db.h"
#include "wallet.h"
#include "ui_interface.h"
#include "checkpoints.h"
#include "txdb.h"

#include <leveldb/db.h>
#include <leveldb/filter_policy.h>
#include <boost/filesystem.hpp>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace fs = boost::filesystem;

// NOTE: pwalletMain/uiInterface/fConfChange/fEnforceCanonical/fUseFastIndex/etc.
// are defined by test_innova.o (the shared test module main). They must NOT be
// redefined here — this test links into the same test_innova binary.

// ---- fixture helpers mirroring the library tests ----

static std::string MakeTempDir()
{
    char tmpl[] = "/tmp/innova-catchupcli-XXXXXX";
    char* d = mkdtemp(tmpl);
    BOOST_REQUIRE(d != NULL);
    return std::string(d);
}

// Write a persisted legacy LevelDB 'blockindex'+'hashBestChain' reachable at L.
static std::vector<uint256> WriteLegacyChain(const std::string& legacyDir, int tip)
{
    fs::create_directories(legacyDir);
    leveldb::Options options;
    options.create_if_missing = true;
    options.error_if_exists = true;
    options.filter_policy = leveldb::NewBloomFilterPolicy(10);
    leveldb::DB* db = NULL;
    BOOST_REQUIRE(leveldb::DB::Open(options, legacyDir, &db).ok());
    std::vector<uint256> hashes((size_t)tip + 1);
    std::vector<CDiskBlockIndex> idx((size_t)tip + 1);
    for (int h = 0; h <= tip; ++h)
    {
        CDiskBlockIndex bi;
        bi.nHeight = h;
        bi.nFile = 1;
        bi.nBlockPos = (unsigned)(100 + h);
        bi.hashPrev = (h == 0) ? uint256(0) : hashes[h - 1];
        bi.nVersion = 7;
        bi.nTime = 1700000000u + (unsigned)h;
        bi.nBits = 0x1d00ffff;
        bi.nNonce = (unsigned)h;
        bi.nFlags = 0;
        bi.hashMerkleRoot = uint256(0x1111ULL + h);
        bi.hashProof = uint256(0x2222ULL + h);
        bi.nMint = 100 + h;
        bi.nMoneySupply = 500 + h * 3;
        idx[h] = bi;
        hashes[h] = bi.GetBlockHash();
        if (h > 0) { idx[h] = idx[h]; }
    }
    // Re-write with correct hashPrev linkage (hashPrev must be actual parent hash).
    for (int h = 0; h <= tip; ++h)
    {
        CDiskBlockIndex bi2 = idx[h];
        bi2.hashPrev = (h == 0) ? uint256(0) : hashes[h - 1];
        CDataStream ssKey(SER_DISK, CLIENT_VERSION);
        ssKey << make_pair(std::string("blockindex"), bi2.GetBlockHash());
        CDataStream ssVal(SER_DISK, CLIENT_VERSION);
        ssVal << bi2;
        BOOST_REQUIRE(db->Put(leveldb::WriteOptions(), ssKey.str(), ssVal.str()).ok());
        hashes[h] = bi2.GetBlockHash();
        idx[h] = bi2;
    }
    {
        CDataStream ssKey(SER_DISK, CLIENT_VERSION);
        ssKey << std::string("hashBestChain");
        CDataStream ssVal(SER_DISK, CLIENT_VERSION);
        ssVal << idx[tip].GetBlockHash();
        BOOST_REQUIRE(db->Put(leveldb::WriteOptions(), ssKey.str(), ssVal.str()).ok());
    }
    delete db;
    return hashes;
}

// Build an AUTHORITATIVE V2 generation @ S (published+selected).
static std::string BuildGenerationS(const std::vector<uint256>& hashes, int S,
                                    const std::string& rootDir)
{
    BlockIndexGenerationSource src;
    for (int h = 0; h <= S; ++h)
    {
        uint256 parent = (h == 0) ? uint256(0) : hashes[h - 1];
        BlockIndexRecord rec;
        rec.hash = hashes[h];
        rec.hashPrev = parent;
        rec.hashMerkleRoot = uint256(0x1111ULL + h);
        rec.height = h;
        rec.nFile = 1;
        rec.nBlockPos = (unsigned)(100 + h);
        rec.nFlags = 0;
        rec.nVersion = 7;
        rec.nTime = 1700000000u + (unsigned)h;
        rec.nBits = 0x1d00ffff;
        rec.nNonce = (unsigned)h;
        rec.nMint = 100 + h;
        rec.nMoneySupply = 500 + h * 3;
        BlockIndexGenerationSourceRecord s; s.hash = rec.hash; s.record = rec;
        src.records.push_back(s);
    }
    src.hashBestChain = hashes[S];
    src.foundBestChain = true;
    BlockIndexGenerationBuilder b;
    BlockIndexGenerationStats stats;
    std::string error;
    fs::path staging = fs::path(rootDir) / "build-000001.tmp";
    BOOST_REQUIRE_MESSAGE(b.Build(src, staging.string(), 1, &stats, &error), error);
    b.Close();
    BOOST_REQUIRE(BlockIndexGenerationManager::PublishGeneration(rootDir, 1, &error) ==
            (int)BLOCK_INDEX_LIFECYCLE_OK);
    BOOST_REQUIRE(BlockIndexGenerationManager::SelectGeneration(rootDir, 1, &error) ==
            (int)BLOCK_INDEX_LIFECYCLE_OK);
    return rootDir;
}

// Resolve the path to the committed CLI binary (built in the same dir).
static std::string CliBinary()
{
    // test runs from <src>; the CLI is <src>/blockindex-catchup-tool.
    // Use an explicit "./" prefix so popen's non-login shell can find it.
    return "./blockindex-catchup-tool";
}

BOOST_AUTO_TEST_SUITE(blockindex_catchup_tool_cli)

/// G2-CLI: real legacy @ L=8 + generation @ S=5 through the actual CLI
/// executable -> CATCHUP_OK with final_tip==L, and idempotent on retry.
BOOST_AUTO_TEST_CASE(cli_catchup_exact_L_and_retry)
{
    const std::string dir = MakeTempDir();
    const std::string legacyDir = dir + "/legacy";
    const int L = 8;
    std::vector<uint256> hashes = WriteLegacyChain(legacyDir, L);
    const int S = 5;
    const std::string v2Root = BuildGenerationS(hashes, S, dir + "/v2");

    std::string cmd =
        CliBinary() + " " + v2Root + " " + legacyDir + " \"\" 2048";
    char buf[65536];
    FILE* p = popen((cmd + " 2>&1").c_str(), "r");
    BOOST_REQUIRE(p != NULL);
    std::string out;
    while (fgets(buf, sizeof(buf), p)) out += buf;
    int rc = pclose(p);
    int rcExit = (rc == -1) ? -1 : WEXITSTATUS(rc);

    BOOST_CHECK_EQUAL(rcExit, 0);
    BOOST_CHECK_MESSAGE(out.find("CATCHUP_OK") != std::string::npos, out);
    // final_tip must be L (=8).
    BOOST_CHECK_MESSAGE(out.find("final_tip=8") != std::string::npos, out);
    printf("G2-CLI PASS: actual CLI binary caught S=%d..L=%d, final_tip==L:\n%s\n",
           S, L, out.c_str());

    // Retry -> idempotent (same final tip).
    FILE* p2 = popen((cmd + " 2>&1").c_str(), "r");
    BOOST_REQUIRE(p2 != NULL);
    std::string out2;
    while (fgets(buf, sizeof(buf), p2)) out2 += buf;
    int rc2 = pclose(p2);
    BOOST_CHECK_EQUAL((rc2 == -1) ? -1 : WEXITSTATUS(rc2), 0);
    BOOST_CHECK_MESSAGE(out2.find("final_tip=8") != std::string::npos, out2);
    printf("G2-CLI-RETRY PASS: CLI catch-up retry idempotent.\n");
}

BOOST_AUTO_TEST_SUITE_END()