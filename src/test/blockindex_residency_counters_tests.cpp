// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.

// Activation Stage 1 - causal verification of the residency scalar counters.
//
// Proves the counters observe events at natural creation/call sites and do not
// invent readings:
//   - the legacy LoadBlockIndex path increments mapinserts/cblockindex_constructed
//     (the TestingSetup global fixture already loaded a genesis-based index, so we
//     verify monotonic behavior of the process-global counters rather than absolutes);
//   - a synthetic by-value authoritative generation does NOT add any historical
//     mapBlockIndex entries (map_entries stable) and does not construct CBlockIndex
//     through the legacy InsertBlockIndex path (construction counter unchanged).

#include <boost/test/unit_test.hpp>

#include "../blockindex_authoritative_startup.h"
#include "../blockindex_active_chain_reader.h"
#include "../blockindex_generation_builder.h"
#include "../blockindex_generation_lifecycle.h"
#include "../blockindex_residency_counters.h"
#include "../hreg_registration.h"
#include "../main.h"

#include <boost/filesystem.hpp>

#include <stdio.h>
#include <string>
#include <vector>

namespace {

struct CBRec { uint256 hash; unsigned int nFile=1, nBlockPos=0; };

static CBRec WriteRes(const boost::filesystem::path& blocksDir, uint256 prev,
                       unsigned int nTime, unsigned int nBits, unsigned int nNonce)
{
    CBRec b;
    CTransaction cb; cb.nVersion = 1; cb.nTime = nTime;
    CTxIn in; in.prevout = COutPoint(uint256(0), 0xffffffff);
    in.scriptSig = CScript() << OP_TRUE; in.nSequence = 0xffffffff;
    cb.vin.push_back(in);
    CTxOut out; out.nValue = 500000000LL; out.scriptPubKey = CScript() << OP_TRUE;
    cb.vout.push_back(out);
    ::CBlock block; block.nVersion = 1; block.hashPrevBlock = prev;
    block.nTime = nTime; block.nBits = nBits; block.nNonce = nNonce;
    block.vtx.push_back(cb); block.hashMerkleRoot = block.BuildMerkleTree();
    b.hash = block.GetHash();
    CDataStream ss(SER_DISK, CLIENT_VERSION); ss << block;
    boost::filesystem::path f = blocksDir / "blk0001.dat";
    FILE* fp = fopen(f.string().c_str(), "ab"); assert(fp);
    unsigned char magic[] = {0xfa,0xbf,0xb5,0xda};
    unsigned int nSize = ::GetSerializeSize(block, SER_NETWORK, PROTOCOL_VERSION);
    fwrite(magic,1,4,fp); fwrite(&nSize,4,1,fp);
    long pos = ftell(fp); b.nBlockPos = (unsigned int)pos;
    fwrite(&ss[0],1,ss.size(),fp); fflush(fp); fclose(fp);
    return b;
}

} // namespace

BOOST_AUTO_TEST_SUITE(blockindex_residency_counters_tests)

// Causal: building+publishing a by-value V2 generation and opening the
// authoritative reader adds NO historicmapBlockIndex entries and does not
// increment the legacy CBlockIndex construction / mapinsert counters. The
// counters stay at their pre-existing global baseline (from the global
// TestingSetup legacy load) rather than growing from the by-value path.
BOOST_AUTO_TEST_CASE(authoritative_byvalue_adds_no_historical_residency)
{
    // Baseline of the process-global counters BEFORE this generation build.
    const int64_t baseConstructed = g_res_cblockindex_constructed;
    const int64_t baseInserts = g_res_mapinserts;
    const int64_t baseEntries = (int64_t)mapBlockIndex.size();

    boost::filesystem::path root = boost::filesystem::temp_directory_path() /
        boost::filesystem::unique_path("innova-rescount-%%%%-%%%%-%%%%");
    boost::filesystem::path blocksDir = root / "blocks";
    boost::filesystem::create_directories(root);
    boost::filesystem::create_directories(blocksDir);
    std::vector<CBRec> blocks;
    uint256 prev(0);
    for (int h = 0; h <= 4; ++h)
    {
        CBRec b = WriteRes(blocksDir, prev, (unsigned int)(2000+h), 0x1d00ffffU, (unsigned int)(500+h));
        blocks.push_back(b); prev = b.hash;
    }
    BlockIndexGenerationSource src;
    for (int h = 0; h <= 4; ++h)
    {
        BlockIndexRecord rec;
        rec.hash = blocks[h].hash;
        rec.hashPrev = (h==0)?uint256(0):blocks[h-1].hash;
        rec.height = h; rec.nVersion = 1;
        rec.nTime = (unsigned int)(2000+h); rec.nBits = 0x1d00ffffU;
        rec.nNonce = (unsigned int)(500+h);
        rec.nFile = blocks[h].nFile; rec.nBlockPos = blocks[h].nBlockPos;
        BlockIndexGenerationSourceRecord sr; sr.hash=rec.hash; sr.record=rec;
        src.records.push_back(sr);
    }
    src.hashBestChain = blocks.back().hash;
    src.foundBestChain = true;
    src.blockDataDir = blocksDir.string();
    std::string error;
    boost::filesystem::path staging = root / "build-000001.tmp";
    { BlockIndexGenerationBuilder b;
      BOOST_REQUIRE_MESSAGE(b.Build(src, staging.string(), 1, NULL, &error), error); b.Close(); }
    BOOST_REQUIRE_MESSAGE(
        BlockIndexGenerationManager::PublishGeneration(root.string(), 1, &error)==(int)BLOCK_INDEX_LIFECYCLE_OK, error);
    BOOST_REQUIRE_MESSAGE(
        BlockIndexGenerationManager::SelectGeneration(root.string(), 1, &error)==(int)BLOCK_INDEX_LIFECYCLE_OK, error);

    // Opening the authoritative by-value active-chain reader must not touch mapBlockIndex.
    BlockIndexActiveChainReader reader;
    std::string openErr;
    BOOST_REQUIRE_MESSAGE(reader.Open(BlockIndexGenerationManager::GenerationPath(root.string(),1), 1, &openErr), openErr);
    BOOST_CHECK_EQUAL(reader.GetActiveHeight(), (int64_t)4);

    // Causal POST: no historical mapBlockIndex growth and no legacy CBlockIndex
    // construction / map insert from the by-value generation path.
    BOOST_CHECK_EQUAL((int64_t)mapBlockIndex.size(), baseEntries);
    BOOST_CHECK_EQUAL(g_res_cblockindex_constructed, baseConstructed);
    BOOST_CHECK_EQUAL(g_res_mapinserts, baseInserts);
    // The 5 fetched block hashes are NOT in mapBlockIndex.
    for (int h = 0; h <= 4; ++h)
        BOOST_CHECK(mapBlockIndex.find(blocks[h].hash) == mapBlockIndex.end());

    reader.Close();
    boost::system::error_code ec; boost::filesystem::remove_all(root, ec);
}

BOOST_AUTO_TEST_SUITE_END()