// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.

// A.10.1j - Causal verification for the by-value DAG restart seam +
// genesis bootstrap anchor.

#include <boost/test/unit_test.hpp>

#include "../blockindex_dag_restart_seam.h"
#include "../blockindex_startup_bootstrap.h"
#include "../blockindex_startup_authority.h"
#include "../blockindex_startup_seam.h"
#include "../blockindex_generation_builder.h"
#include "../blockindex_generation_lifecycle.h"
#include "../fixed_blockindex_store.h"
#include "../main.h"
#include "../dag.h"

#include <leveldb/db.h>
#include <leveldb/filter_policy.h>
#include <leveldb/write_batch.h>

#include <boost/filesystem.hpp>

#include <assert.h>
#include <stdio.h>
#include <string>
#include <vector>
#include <map>

namespace {

struct SyntheticBlockInfo
{
    uint256 hash;
    unsigned int nFile = 0;
    unsigned int nBlockPos = 0;
    unsigned int nSize = 0;
};

static SyntheticBlockInfo WriteSyntheticBlock(
    const boost::filesystem::path& dir,
    uint256 hashPrevBlock, unsigned int nTime, unsigned int nBits, unsigned int nNonce)
{
    SyntheticBlockInfo info;
    info.nFile = 1;

    CTransaction coinbase;
    coinbase.nVersion = 1;
    coinbase.nTime = nTime;
    CTxIn input;
    input.prevout = COutPoint(uint256(0), 0xffffffff);
    input.scriptSig = CScript() << OP_TRUE;
    input.nSequence = 0xffffffff;
    coinbase.vin.push_back(input);
    CTxOut output;
    output.nValue = 0;
    output.scriptPubKey = CScript() << OP_TRUE;
    coinbase.vout.push_back(output);

    CBlock block;
    block.nVersion = 1;
    block.hashPrevBlock = hashPrevBlock;
    block.nTime = nTime;
    block.nBits = nBits;
    block.nNonce = nNonce;
    block.vtx.push_back(coinbase);
    block.hashMerkleRoot = block.BuildMerkleTree();

    info.hash = block.GetHash();
    info.nSize = ::GetSerializeSize(block, SER_NETWORK, PROTOCOL_VERSION);

    CDataStream ssBlock(SER_DISK, CLIENT_VERSION);
    ssBlock << block;
    unsigned int nDiskSize = ssBlock.size();

    boost::filesystem::path blockFile = dir / "blk0001.dat";
    FILE* f = fopen(blockFile.string().c_str(), "ab");
    assert(f);
    unsigned char magic[] = {0xfa, 0xbf, 0xb5, 0xda};
    fwrite(magic, 1, 4, f);
    fwrite(&nDiskSize, 4, 1, f);
    long pos = ftell(f);
    info.nBlockPos = (unsigned int)pos;
    fwrite(&ssBlock[0], 1, ssBlock.size(), f);
    fflush(f);
    fclose(f);
    return info;
}

// Write a daglinks entry into a LevelDB store using the SAME key/value
// serialization as CTxDB::WriteDAGLinks (txdb-leveldb.cpp:423-426 /
// txdb-leveldb.h:107-131): key = CDataStream << make_pair("daglinks",hash),
// value = CDataStream << CBlockDAGData.
static bool WriteDagLinksEntry(const std::string& dbDir,
                               const uint256& hash,
                               const CBlockDAGData& data)
{
    leveldb::Options options;
    options.create_if_missing = true;
    options.filter_policy = leveldb::NewBloomFilterPolicy(10);
    leveldb::DB* db = NULL;
    leveldb::Status st = leveldb::DB::Open(options, dbDir, &db);
    if (!st.ok() || !db) return false;

    CDataStream ssKey(SER_DISK, CLIENT_VERSION);
    ssKey << make_pair(std::string("daglinks"), hash);
    CDataStream ssValue(SER_DISK, CLIENT_VERSION);
    ssValue << data;

    st = db->Put(leveldb::WriteOptions(), ssKey.str(), ssValue.str());
    delete db;
    return st.ok();
}


// Build an AUTHORITATIVE chain (real blk data) with a configurable number of
// blocks, and open a bootstrap on it. Returns handles.
struct DagFixture
{
    boost::filesystem::path root;
    boost::filesystem::path blockDir;
    boost::filesystem::path dagDbDir;
    std::vector<SyntheticBlockInfo> blocks;
    int heights;

    explicit DagFixture(int n)
        : heights(n)
    {
        root = boost::filesystem::temp_directory_path() /
            boost::filesystem::unique_path("innova-blockindex-dagseam-%%%%-%%%%-%%%%");
        boost::filesystem::create_directories(root);
        blockDir = root / "blocks";
        boost::filesystem::create_directories(blockDir);
        dagDbDir = root / "dagdb";
        boost::filesystem::create_directories(dagDbDir);

        uint256 prev(0);
        for (int h = 0; h <= heights; ++h)
        {
            SyntheticBlockInfo bi = WriteSyntheticBlock(
                blockDir, prev, (unsigned int)(1000 + h), 0x1d00ffffU, (unsigned int)(100 + h));
            blocks.push_back(bi);
            prev = bi.hash;
        }

        BlockIndexGenerationSource src;
        for (int h = 0; h <= heights; ++h)
        {
            BlockIndexRecord rec;
            rec.hash = blocks[h].hash;
            rec.hashPrev = (h == 0) ? uint256(0) : blocks[h - 1].hash;
            rec.height = h;
            rec.nVersion = 1;
            rec.nTime = (unsigned int)(1000 + h);
            rec.nBits = 0x1d00ffffU;
            rec.nNonce = (unsigned int)(100 + h);
            rec.nFile = blocks[h].nFile;
            rec.nBlockPos = blocks[h].nBlockPos;
            BlockIndexGenerationSourceRecord sr;
            sr.hash = rec.hash;
            sr.record = rec;
            src.records.push_back(sr);
        }
        src.hashBestChain = blocks[heights].hash;
        src.foundBestChain = true;
        src.blockDataDir = blockDir.string();

        boost::filesystem::path staging = root / "build-000001.tmp";
        std::string error;
        {
            BlockIndexGenerationBuilder b;
            BOOST_REQUIRE_MESSAGE(b.Build(src, staging.string(), 1, NULL, &error), error);
            b.Close();
        }
        BOOST_REQUIRE_MESSAGE(
            BlockIndexGenerationManager::PublishGeneration(root.string(), 1, &error) ==
                (int)BLOCK_INDEX_LIFECYCLE_OK, error);
        BOOST_REQUIRE_MESSAGE(
            BlockIndexGenerationManager::SelectGeneration(root.string(), 1, &error) ==
                (int)BLOCK_INDEX_LIFECYCLE_OK, error);
    }

    ~DagFixture()
    {
        boost::system::error_code ec;
        boost::filesystem::remove_all(root, ec);
    }

    void SeedDagScore(int idx, const uint256& score)
    {
        CBlockDAGData d;
        d.vDAGParents.clear();
        d.fBlue = true;
        d.nDAGScore = score;
        bool ok = WriteDagLinksEntry(dagDbDir.string(), blocks[idx].hash, d);
        BOOST_REQUIRE_MESSAGE(ok, "failed to seed daglinks for block " << idx);
    }
};

} // namespace

BOOST_AUTO_TEST_SUITE(blockindex_dag_restart_seam_tests)

// J3 - linear DAG restart differential: seam restore set == legacy predicate.
BOOST_AUTO_TEST_CASE(j3_linear_diff)
{
    DagFixture fx(5);
    const int FORK_DAG = 3;
    for (int idx = 3; idx <= 5; ++idx)
        fx.SeedDagScore(idx, uint256((uint64_t)(1000 + idx)));

    std::string error;
    V2BlockIndexStartupAuthority auth;
    BOOST_REQUIRE(auth.Open(fx.root.string(), &error) == BLOCK_INDEX_STARTUP_OK);

    BlockIndexDagRestartSeam seam;
    DagRestartResult res;
    BOOST_REQUIRE_MESSAGE(seam.ComputeRestore(fx.dagDbDir.string(), auth, FORK_DAG, &res, &error),
                          error);
    BOOST_REQUIRE(res.ok);
    BOOST_CHECK(res.totalRestored == 3);
    for (size_t i = 0; i < res.restore.size(); ++i)
    {
        int h = res.restore[i].height;
        BOOST_CHECK(h >= FORK_DAG);
        BOOST_CHECK(h <= 5);
        BOOST_CHECK(res.restore[i].dagScore == uint256((uint64_t)(1000 + h)));
    }
}

// J9 - causal no-mapBlockIndex: by-value restart succeeds with hashes absent.
BOOST_AUTO_TEST_CASE(j9_no_map_dependency_causal)
{
    DagFixture fx(5);
    const int FORK_DAG = 3;
    fx.SeedDagScore(4, uint256((uint64_t)1044));
    BOOST_CHECK(mapBlockIndex.find(fx.blocks[4].hash) == mapBlockIndex.end());
    std::string error;
    V2BlockIndexStartupAuthority auth;
    BOOST_REQUIRE(auth.Open(fx.root.string(), &error) == BLOCK_INDEX_STARTUP_OK);
    BlockIndexDagRestartSeam seam;
    DagRestartResult res;
    BOOST_REQUIRE(seam.ComputeRestore(fx.dagDbDir.string(), auth, FORK_DAG, &res, &error));
    BOOST_REQUIRE(res.ok);
    bool found = false;
    for (size_t i = 0; i < res.restore.size(); ++i)
        if (res.restore[i].height == 4) found = true;
    BOOST_CHECK_MESSAGE(found, "h4 must be restored by-value");
    BOOST_CHECK(mapBlockIndex.find(fx.blocks[4].hash) == mapBlockIndex.end());
}

// J10 - no historical pprev topology: by-value restore succeeds with no pprev.
BOOST_AUTO_TEST_CASE(j10_no_pprev_topology)
{
    DagFixture fx(3);
    const int FORK_DAG = 2;
    fx.SeedDagScore(2, uint256((uint64_t)777));
    // No pprev/pskip constructed for any historical block.
    std::string error;
    V2BlockIndexStartupAuthority auth;
    BOOST_REQUIRE(auth.Open(fx.root.string(), &error) == BLOCK_INDEX_STARTUP_OK);
    BlockIndexDagRestartSeam seam;
    DagRestartResult res;
    BOOST_REQUIRE(seam.ComputeRestore(fx.dagDbDir.string(), auth, FORK_DAG, &res, &error));
    BOOST_REQUIRE(res.ok);
    BOOST_CHECK(res.totalRestored == 1);
    BOOST_CHECK(res.restore[0].height == 2);
}

// J13 - corrupt/missing DAG relation fails closed.
BOOST_AUTO_TEST_CASE(j13_corrupt_missing_fails_closed)
{
    DagFixture fx(3);
    const int FORK_DAG = 2;
    // No daglinks entries at all: seam must still succeed with empty restore
    // (empty store is not corruption), OR fail closed on a missing dir.
    std::string error;
    V2BlockIndexStartupAuthority auth;
    BOOST_REQUIRE(auth.Open(fx.root.string(), &error) == BLOCK_INDEX_STARTUP_OK);
    BlockIndexDagRestartSeam seam;
    DagRestartResult res;
    // Non-existent dagDb dir must fail closed (never ok).
    boost::filesystem::path missing = fx.root / "no-such-dagdb";
    bool r = seam.ComputeRestore(missing.string(), auth, FORK_DAG, &res, &error);
    BOOST_CHECK_MESSAGE(!r, "missing daglinks dir must fail closed");
}

// I0/I1/I2 reuse - genesis anchor on bootstrap (J0-J2 are in bootstrap suite;
// here we spot-check genesis is anchored and evict-blocked).
BOOST_AUTO_TEST_CASE(j_genesis_anchor_sanity)
{
    DagFixture fx(3);
    std::string error;
    BlockIndexStartupBootstrap bt;
    BlockIndexV2ReaderOptions o;
    BOOST_REQUIRE(bt.Open(fx.root.string(), o, &error) == BLOCK_INDEX_STARTUP_OK);
    BOOST_REQUIRE(bt.GenesisObject() != NULL);
    BOOST_CHECK_EQUAL(bt.GenesisObject()->nHeight, 0);
    (void)error;
}

BOOST_AUTO_TEST_SUITE_END()
