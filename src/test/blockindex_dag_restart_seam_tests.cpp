// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.

// A.10.1j - Causal verification for the by-value DAG restart seam +
// genesis bootstrap anchor.

#include <boost/test/unit_test.hpp>

#include "../blockindex_dag_restart_seam.h"
#include "../dag_source_binding_verifier.h"
#include "../dag_logical_authority.h"
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

#include <openssl/sha.h>

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
static bool ComputeDirectDagDigest(const std::string& dbDir, unsigned char out[32])
{
    leveldb::Options options;
    options.create_if_missing = false;
    leveldb::DB* db = NULL;
    leveldb::Status st = leveldb::DB::Open(options, dbDir, &db);
    if (!st.ok() || !db) return false;
    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    CDataStream prefix(SER_DISK, CLIENT_VERSION);
    prefix << std::string("daglinks");
    std::string p = prefix.str();
    leveldb::Iterator* it = db->NewIterator(leveldb::ReadOptions());
    it->Seek(p);
    while (it->Valid())
    {
        if (it->key().ToString().compare(0, p.size(), p) != 0) break;
        try
        {
            CDataStream key(SER_DISK, CLIENT_VERSION);
            key.write(it->key().data(), it->key().size());
            std::pair<std::string, uint256> pairKey;
            key >> pairKey;
            CDataStream value(SER_DISK, CLIENT_VERSION);
            value.write(it->value().data(), it->value().size());
            CBlockDAGData data;
            value >> data;
            SHA256_Update(&ctx, pairKey.second.begin(), 32);
            uint32_t count = (uint32_t)data.vDAGParents.size();
            SHA256_Update(&ctx, &count, 4);
            for (size_t i = 0; i < data.vDAGParents.size(); ++i)
                SHA256_Update(&ctx, data.vDAGParents[i].begin(), 32);
        }
        catch (...) { delete it; delete db; return false; }
        it->Next();
    }
    st = it->status();
    delete it; delete db;
    if (!st.ok()) return false;
    SHA256_Final(out, &ctx);
    return true;
}

// Build an AUTHORITATIVE chain (real blk data) with a configurable number of
// blocks, and open a bootstrap on it. Returns handles.
struct DagFixture
{
    boost::filesystem::path root;
    boost::filesystem::path blockDir;
    boost::filesystem::path dagDbDir;
    std::vector<SyntheticBlockInfo> blocks;
    SyntheticBlockInfo side;
    uint256 danglingDagHash;
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

        side = WriteSyntheticBlock(blockDir, blocks[0].hash, 2000, 0x1d00ffffU, 9001);
        danglingDagHash = uint256((uint64_t)0xdeadbeef);

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
        {
            BlockIndexRecord rec;
            rec.hash = side.hash;
            rec.hashPrev = blocks[0].hash;
            rec.height = 1;
            rec.nVersion = 1;
            rec.nTime = 2000;
            rec.nBits = 0x1d00ffffU;
            rec.nNonce = 9001;
            rec.nFile = side.nFile;
            rec.nBlockPos = side.nBlockPos;
            BlockIndexGenerationSourceRecord sr;
            sr.hash = rec.hash;
            sr.record = rec;
            src.records.push_back(sr);
        }
        src.hashBestChain = blocks[heights].hash;
        src.foundBestChain = true;
        for (int h = 0; h <= heights; ++h)
            src.dagLinks[blocks[h].hash] = std::vector<uint256>();
        src.dagLinks[side.hash] = std::vector<uint256>();
        src.dagLinks[danglingDagHash] = std::vector<uint256>();
        if (heights >= 3)
        {
            src.dagLinks[blocks[3].hash].push_back(blocks[1].hash);
            src.dagLinks[blocks[3].hash].push_back(blocks[2].hash);
        }
        if (heights >= 4)
        {
            src.dagLinks[blocks[4].hash].push_back(blocks[2].hash);
            src.dagLinks[blocks[4].hash].push_back(blocks[3].hash);
        }
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

    void SeedR1DagData()
    {
        for (int h = 0; h <= heights; ++h)
        {
            CBlockDAGData d;
            if (h == 3)
            {
                d.vDAGParents.push_back(blocks[1].hash);
                d.vDAGParents.push_back(blocks[2].hash);
            }
            else if (h == 4)
            {
                d.vDAGParents.push_back(blocks[2].hash);
                d.vDAGParents.push_back(blocks[3].hash);
            }
            d.fBlue = true;
            d.nDAGScore = (h == 1 || h == 2) ? uint256((uint64_t)101) : uint256((uint64_t)(100 + h));
            bool ok = WriteDagLinksEntry(dagDbDir.string(), blocks[h].hash, d);
            BOOST_REQUIRE_MESSAGE(ok, "failed to seed R1a daglinks for block " << h);
        }
        {
            CBlockDAGData sideData;
            sideData.fBlue = true;
            sideData.nDAGScore = uint256((uint64_t)199);
            bool ok = WriteDagLinksEntry(dagDbDir.string(), side.hash, sideData);
            BOOST_REQUIRE_MESSAGE(ok, "failed to seed R1a side daglinks");
        }
        {
            CBlockDAGData dangling;
            dangling.fBlue = true;
            dangling.nDAGScore = uint256((uint64_t)31337);
            bool ok = WriteDagLinksEntry(dagDbDir.string(), danglingDagHash, dangling);
            BOOST_REQUIRE_MESSAGE(ok, "failed to seed R1a dangling daglinks");
        }
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

BOOST_AUTO_TEST_CASE(r1a_external_sort_digest_parity_and_bounds)
{
    uint256 small(1), larger(256);
    CDataStream smallKey(SER_DISK, CLIENT_VERSION), largeKey(SER_DISK, CLIENT_VERSION);
    smallKey << make_pair(std::string("daglinks"), small);
    largeKey << make_pair(std::string("daglinks"), larger);
    BOOST_CHECK(small < larger);
    BOOST_CHECK(smallKey.str() > largeKey.str()); // serialized-key order differs

    DagFixture fx(12);
    fx.SeedR1DagData();
    std::string error;
    V2BlockIndexStartupAuthority auth;
    BOOST_REQUIRE(auth.Open(fx.root.string(), &error) == BLOCK_INDEX_STARTUP_OK);
    unsigned char expected[32];
    BOOST_REQUIRE(auth.ReaderPtr()->GetDAGInputDigest(expected, &error));
    unsigned char direct[32];
    BOOST_REQUIRE(ComputeDirectDagDigest(fx.dagDbDir.string(), direct));
    BOOST_CHECK(memcmp(expected, direct, 32) != 0);

    DagSourceBindingVerifierOptions options;
    options.chunkBytes = 128;
    options.maxRecordsPerChunk = 1;
    options.maxOpenRuns = 2;
    options.tempParent = (fx.root / "r1a-temp").string();
    DagSourceBindingResult result = DagSourceBindingVerifier::Verify(
        fx.dagDbDir.string(), expected, options);
    BOOST_REQUIRE_MESSAGE(result.status == DAG_SOURCE_BINDING_VERIFIED, result.error);
    BOOST_CHECK_EQUAL(result.recordsProcessed, (uint64_t)15);
    BOOST_CHECK(result.runCount >= 15);
    BOOST_CHECK(result.mergePasses >= 1);
    BOOST_CHECK(result.maxOpenRunsObserved <= 2U);
    BOOST_CHECK(result.peakChunkRecords <= 1U);
    BOOST_CHECK(result.temporaryBytesWritten > 0U);
    BOOST_TEST_MESSAGE("R1a metrics records=" << result.recordsProcessed
                       << " bytes=" << result.bytesProcessed
                       << " temp_bytes=" << result.temporaryBytesWritten
                       << " peak_chunk_bytes=" << result.peakChunkBytes
                       << " peak_chunk_records=" << result.peakChunkRecords
                       << " runs=" << result.runCount
                       << " merge_passes=" << result.mergePasses
                       << " max_open_runs=" << result.maxOpenRunsObserved);
    for (int h = 0; h <= fx.heights; ++h)
        BOOST_CHECK(mapBlockIndex.find(fx.blocks[h].hash) == mapBlockIndex.end());
}

BOOST_AUTO_TEST_CASE(r1a_empty_corrupt_missing_and_mismatch_failures)
{
    boost::filesystem::path root = boost::filesystem::temp_directory_path() /
        boost::filesystem::unique_path("innova-blockindex-r1a-empty-%%%%-%%%%");
    boost::filesystem::path empty = root / "empty";
    boost::filesystem::create_directories(empty);
    leveldb::Options create;
    create.create_if_missing = true;
    leveldb::DB* emptyDb = NULL;
    BOOST_REQUIRE(leveldb::DB::Open(create, empty.string(), &emptyDb).ok());
    delete emptyDb;
    unsigned char emptyDigest[32];
    SHA256_CTX emptyCtx;
    SHA256_Init(&emptyCtx);
    SHA256_Final(emptyDigest, &emptyCtx);
    DagSourceBindingVerifierOptions options;
    options.tempParent = (root / "tmp").string();
    DagSourceBindingResult emptyResult = DagSourceBindingVerifier::Verify(
        empty.string(), emptyDigest, options);
    BOOST_CHECK_EQUAL((int)emptyResult.status, (int)DAG_SOURCE_BINDING_VERIFIED);

    DagFixture fx(3);
    fx.SeedR1DagData();
    std::string error;
    V2BlockIndexStartupAuthority auth;
    BOOST_REQUIRE(auth.Open(fx.root.string(), &error) == BLOCK_INDEX_STARTUP_OK);
    unsigned char expected[32];
    BOOST_REQUIRE(auth.ReaderPtr()->GetDAGInputDigest(expected, &error));
    options.tempParent = (fx.root / "tmp2").string();
    unsigned char wrong[32];
    memset(wrong, 0, sizeof(wrong));
    DagSourceBindingResult mismatch = DagSourceBindingVerifier::Verify(
        fx.dagDbDir.string(), wrong, options);
    BOOST_CHECK_EQUAL((int)mismatch.status, (int)DAG_SOURCE_BINDING_DIGEST_MISMATCH);

    DagSourceBindingResult missing = DagSourceBindingVerifier::Verify(
        (fx.root / "missing").string(), expected, options);
    BOOST_CHECK_EQUAL((int)missing.status, (int)DAG_SOURCE_BINDING_SOURCE_UNAVAILABLE);

    boost::filesystem::path corrupt = fx.root / "corrupt";
    boost::filesystem::create_directories(corrupt);
    leveldb::DB* corruptDb = NULL;
    BOOST_REQUIRE(leveldb::DB::Open(create, corrupt.string(), &corruptDb).ok());
    CDataStream badKey(SER_DISK, CLIENT_VERSION);
    badKey << make_pair(std::string("daglinks"), fx.blocks[0].hash);
    BOOST_REQUIRE(corruptDb->Put(leveldb::WriteOptions(), badKey.str(), "corrupt").ok());
    delete corruptDb;
    DagSourceBindingResult corruptResult = DagSourceBindingVerifier::Verify(
        corrupt.string(), expected, options);
    BOOST_CHECK_EQUAL((int)corruptResult.status, (int)DAG_SOURCE_BINDING_DECODE_FAILURE);

    boost::system::error_code ec;
    boost::filesystem::remove_all(root, ec);
}


BOOST_AUTO_TEST_CASE(r1_dag_logical_authority_by_value_parity_and_side_branch)
{
    DagFixture fx(6);
    fx.SeedR1DagData();
    std::string error;
    V2BlockIndexStartupAuthority startup;
    BOOST_REQUIRE(startup.Open(fx.root.string(), &error) == BLOCK_INDEX_STARTUP_OK);

    DagLogicalAuthority authority;
    BOOST_REQUIRE_MESSAGE(authority.Open(fx.dagDbDir.string(), *startup.ReaderPtr(), 2,
                                           (fx.root / "authority-temp").string(), &error), error);

    DagLogicalRecord dag3;
    BOOST_REQUIRE_EQUAL((int)authority.LookupDAG(fx.blocks[3].hash, &dag3, &error),
                        (int)DAG_LOGICAL_AUTHORITY_FOUND);
    BOOST_REQUIRE_EQUAL(dag3.data.vDAGParents.size(), (size_t)2);

    uint256 score;
    BOOST_REQUIRE_EQUAL((int)authority.GetDAGScore(fx.blocks[1].hash, &score, &error),
                        (int)DAG_LOGICAL_AUTHORITY_FOUND);
    BOOST_CHECK(score == uint256((uint64_t)101));
    BOOST_REQUIRE_EQUAL((int)authority.GetDAGScore(fx.blocks[4].hash, &score, &error),
                        (int)DAG_LOGICAL_AUTHORITY_FOUND);
    BOOST_CHECK(score == uint256((uint64_t)104));

    uint256 selected;
    BOOST_REQUIRE_EQUAL((int)authority.GetSelectedParent(fx.blocks[3].hash, &selected, &error),
                        (int)DAG_LOGICAL_AUTHORITY_FOUND);
    uint256 expectedTie = (fx.blocks[1].hash < fx.blocks[2].hash) ? fx.blocks[1].hash : fx.blocks[2].hash;
    BOOST_CHECK(selected == expectedTie);
    BOOST_REQUIRE_EQUAL((int)authority.GetSelectedParent(fx.blocks[4].hash, &selected, &error),
                        (int)DAG_LOGICAL_AUTHORITY_FOUND);
    BOOST_CHECK(selected == fx.blocks[3].hash);

    BlockIndexSnapshot active;
    BOOST_REQUIRE_EQUAL((int)authority.LookupBlock(fx.blocks[3].hash, &active, true, &error),
                        (int)DAG_LOGICAL_AUTHORITY_FOUND);
    BOOST_CHECK(active.fInMainChain);
    BlockIndexSnapshot side;
    BOOST_CHECK_EQUAL((int)authority.LookupBlock(fx.side.hash, &side, true, &error),
                      (int)DAG_LOGICAL_AUTHORITY_NOT_ACTIVE);
    BOOST_CHECK_EQUAL((int)authority.LookupBlock(fx.side.hash, &side, false, &error),
                      (int)DAG_LOGICAL_AUTHORITY_FOUND);
    BOOST_CHECK(!side.fInMainChain);

    // Isolated legacy oracle: same persisted logical records through CDAGManager's
    // test-only insertion surface; production R1 authority never calls this path.
    CDAGManager legacy;
    CBlockDAGData p1, p2, p3, p4;
    p1.nDAGScore = uint256((uint64_t)101);
    p2.nDAGScore = uint256((uint64_t)101);
    p3.nDAGScore = uint256((uint64_t)103);
    p4.nDAGScore = uint256((uint64_t)104);
    p3.vDAGParents.push_back(fx.blocks[1].hash);
    p3.vDAGParents.push_back(fx.blocks[2].hash);
    p4.vDAGParents.push_back(fx.blocks[2].hash);
    p4.vDAGParents.push_back(fx.blocks[3].hash);
    legacy.SetDAGDataForTest(fx.blocks[1].hash, p1);
    legacy.SetDAGDataForTest(fx.blocks[2].hash, p2);
    legacy.SetDAGDataForTest(fx.blocks[3].hash, p3);
    legacy.SetDAGDataForTest(fx.blocks[4].hash, p4);
    BOOST_CHECK(legacy.GetSelectedParent(fx.blocks[3].hash) == expectedTie);
    BOOST_CHECK(legacy.GetSelectedParent(fx.blocks[4].hash) == fx.blocks[3].hash);
    legacy.ClearDAGDataForTest();

    DagLogicalRecord missing;
    BOOST_CHECK_EQUAL((int)authority.LookupDAG(uint256((uint64_t)0x123456), &missing, &error),
                      (int)DAG_LOGICAL_AUTHORITY_NOT_FOUND);
    BOOST_CHECK_EQUAL((int)authority.GetDAGScore(uint256((uint64_t)0x123456), &score, &error),
                      (int)DAG_LOGICAL_AUTHORITY_NOT_FOUND);
    BOOST_CHECK_EQUAL((int)authority.GetDAGScore(fx.danglingDagHash, &score, &error),
                      (int)DAG_LOGICAL_AUTHORITY_FAILURE);

    for (int h = 0; h <= fx.heights; ++h)
    {
        BOOST_CHECK(mapBlockIndex.find(fx.blocks[h].hash) == mapBlockIndex.end());
        DagLogicalRecord record;
        BOOST_CHECK_EQUAL((int)authority.LookupDAG(fx.blocks[h].hash, &record, &error),
                          (int)DAG_LOGICAL_AUTHORITY_FOUND);
    }
    BOOST_CHECK(mapBlockIndex.find(fx.side.hash) == mapBlockIndex.end());
    DagLogicalAuthorityStats stats = authority.CacheStats();
    BOOST_CHECK_EQUAL(stats.capacity, (size_t)2);
    BOOST_CHECK(stats.current <= stats.capacity);
    BOOST_CHECK(stats.peak <= stats.capacity);
}

BOOST_AUTO_TEST_CASE(r1_dag_logical_authority_generation_change_fails_closed)
{
    DagFixture fx(4);
    fx.SeedR1DagData();
    std::string error;
    V2BlockIndexStartupAuthority startup;
    BOOST_REQUIRE(startup.Open(fx.root.string(), &error) == BLOCK_INDEX_STARTUP_OK);
    DagLogicalAuthority authority;
    BOOST_REQUIRE(authority.Open(fx.dagDbDir.string(), *startup.ReaderPtr(), 2,
                                  (fx.root / "generation-temp").string(), &error));
    const_cast<BlockIndexV2Reader*>(startup.ReaderPtr())->Close();
    DagLogicalRecord record;
    BOOST_CHECK_EQUAL((int)authority.LookupDAG(fx.blocks[1].hash, &record, &error),
                      (int)DAG_LOGICAL_AUTHORITY_FAILURE);
}

BOOST_AUTO_TEST_CASE(r1_dag_logical_authority_cache_bound_scales)
{
    const int sizes[] = {3, 12};
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i)
    {
        DagFixture fx(sizes[i]);
        fx.SeedR1DagData();
        std::string error;
        V2BlockIndexStartupAuthority startup;
        BOOST_REQUIRE(startup.Open(fx.root.string(), &error) == BLOCK_INDEX_STARTUP_OK);
        DagLogicalAuthority authority;
        BOOST_REQUIRE_MESSAGE(authority.Open(fx.dagDbDir.string(), *startup.ReaderPtr(), 2,
                                               (fx.root / "cache-temp").string(), &error), error);
        for (int h = 0; h <= fx.heights; ++h)
        {
            DagLogicalRecord record;
            BOOST_REQUIRE_EQUAL((int)authority.LookupDAG(fx.blocks[h].hash, &record, &error),
                                (int)DAG_LOGICAL_AUTHORITY_FOUND);
        }
        DagLogicalRecord sideRecord;
        BOOST_REQUIRE_EQUAL((int)authority.LookupDAG(fx.side.hash, &sideRecord, &error),
                            (int)DAG_LOGICAL_AUTHORITY_FOUND);
        DagLogicalAuthorityStats stats = authority.CacheStats();
        BOOST_CHECK_EQUAL(stats.capacity, (size_t)2);
        BOOST_CHECK(stats.current <= 2U);
        BOOST_CHECK(stats.peak <= 2U);
    }
}

BOOST_AUTO_TEST_CASE(r1_dag_logical_authority_binding_fail_closed)
{
    DagFixture fx(4);
    fx.SeedR1DagData();
    std::string error;
    V2BlockIndexStartupAuthority startup;
    BOOST_REQUIRE(startup.Open(fx.root.string(), &error) == BLOCK_INDEX_STARTUP_OK);
    unsigned char wrong[32];
    memset(wrong, 0, sizeof(wrong));
    DagSourceBindingVerifierOptions bindingOptions;
    bindingOptions.tempParent = (fx.root / "binding-temp").string();
    DagSourceBindingResult bad = DagSourceBindingVerifier::Verify(
        fx.dagDbDir.string(), wrong, bindingOptions);
    BOOST_CHECK_EQUAL((int)bad.status, (int)DAG_SOURCE_BINDING_DIGEST_MISMATCH);

    DagLogicalAuthority authority;
    BOOST_CHECK(!authority.Open((fx.root / "missing-daglinks").string(), *startup.ReaderPtr(),
                                2, (fx.root / "authority-temp").string(), &error));
    BOOST_CHECK(!authority.IsOpen());
}

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
