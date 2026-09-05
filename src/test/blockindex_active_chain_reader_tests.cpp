// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.

// A.10.1k/D-prereq - Causal verification for by-value active-chain iteration
// (hreg rebuild + wallet rescan migration substrate).

#include <boost/test/unit_test.hpp>

#include "../blockindex_active_chain_reader.h"
#include "../blockindex_generation_builder.h"
#include "../blockindex_generation_lifecycle.h"
#include "../main.h"

#include <boost/filesystem.hpp>

#include <assert.h>
#include <stdio.h>
#include <string>
#include <vector>

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

struct ReaderFixture
{
    boost::filesystem::path root;
    boost::filesystem::path blockDir;
    std::vector<SyntheticBlockInfo> blocks;
    int tipHeight;

    explicit ReaderFixture(int n)
        : tipHeight(n)
    {
        root = boost::filesystem::temp_directory_path() /
            boost::filesystem::unique_path("innova-acr-%%%%-%%%%-%%%%");
        boost::filesystem::create_directories(root);
        blockDir = root / "blocks";
        boost::filesystem::create_directories(blockDir);
        uint256 prev(0);
        for (int h = 0; h <= tipHeight; ++h)
        {
            SyntheticBlockInfo bi = WriteSyntheticBlock(
                blockDir, prev, (unsigned int)(1000 + h), 0x1d00ffffU, (unsigned int)(100 + h));
            blocks.push_back(bi);
            prev = bi.hash;
        }
        BlockIndexGenerationSource src;
        for (int h = 0; h <= tipHeight; ++h)
        {
            BlockIndexRecord rec;
            rec.hash = blocks[h].hash;
            rec.hashPrev = (h == 0) ? uint256(0) : blocks[h - 1].hash;
            rec.height = h;
            rec.nVersion = 1; rec.nTime = (unsigned int)(1000 + h);
            rec.nBits = 0x1d00ffffU; rec.nNonce = (unsigned int)(100 + h);
            rec.nFile = blocks[h].nFile; rec.nBlockPos = blocks[h].nBlockPos;
            BlockIndexGenerationSourceRecord sr; sr.hash = rec.hash; sr.record = rec;
            src.records.push_back(sr);
        }
        src.hashBestChain = blocks[tipHeight].hash;
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

    ~ReaderFixture()
    {
        boost::system::error_code ec;
        boost::filesystem::remove_all(root, ec);
    }
};

} // namespace

BOOST_AUTO_TEST_SUITE(blockindex_active_chain_reader_tests)

// K1 - by-value active-height iteration resolves the SAME hashes as the chain,
// in exact height order, with zero mapBlockIndex entries and no pnext for any
// historical block. This is the causal precondition for the hreg/rescan
// migration.
BOOST_AUTO_TEST_CASE(k1_by_value_active_height_no_map_no_pnext_causal)
{
    ReaderFixture fx(5);
    std::string error;
    BlockIndexActiveChainReader reader;
    BOOST_REQUIRE_MESSAGE(reader.Open((fx.root / "gen-000001").string(), 1, &error), error);
    BOOST_REQUIRE(reader.IsOpen());

    // Causal precondition: none of the historical hashes are resident.
    for (int h = 0; h <= 5; ++h)
        BOOST_CHECK(mapBlockIndex.find(fx.blocks[h].hash) == mapBlockIndex.end());

    // Active height matches.
    BOOST_CHECK_EQUAL((int)reader.GetActiveHeight(), 5);

    // Each active height resolves by-value in EXACT height order to the same hash.
    for (int h = 0; h <= 5; ++h)
    {
        BlockIndexActiveBlock ab;
        BOOST_REQUIRE_MESSAGE(reader.LookupByHeight(h, &ab, &error), "lookup h=" << h << ": " << error);
        BOOST_CHECK_EQUAL(ab.height, h);
        BOOST_CHECK(ab.hash == fx.blocks[h].hash);
        BOOST_CHECK(ab.nFile == 1);
        BOOST_CHECK(ab.nBlockPos == fx.blocks[h].nBlockPos);
    }

    // After by-value iteration, mapBlockIndex is STILL empty (no hidden graph).
    for (int h = 0; h <= 5; ++h)
        BOOST_CHECK(mapBlockIndex.find(fx.blocks[h].hash) == mapBlockIndex.end());
}

// K2 - by-value iteration is O(1) resident per height (no accumulating cache):
// reader holds no resident CBlockIndex; resident map remains empty though the
// reader visited every active height. Boundedness is structural.
BOOST_AUTO_TEST_CASE(k2_bounded_iteration_no_accumulation)
{
    ReaderFixture fx(40);
    std::string error;
    BlockIndexActiveChainReader reader;
    BOOST_REQUIRE(reader.Open((fx.root / "gen-000001").string(), 1, &error));
    BOOST_CHECK_EQUAL((int)reader.GetActiveHeight(), 40);
    for (int h = 0; h <= 40; ++h)
    {
        BlockIndexActiveBlock ab;
        BOOST_REQUIRE(reader.LookupByHeight(h, &ab, &error));
        BOOST_CHECK_EQUAL(ab.height, h);
    }
    // After visiting 41 heights, no resident map entry was created.
    for (int h = 0; h <= 40; ++h)
        BOOST_CHECK(mapBlockIndex.find(fx.blocks[h].hash) == mapBlockIndex.end());
}

BOOST_AUTO_TEST_SUITE_END()
