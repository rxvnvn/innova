// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.

// A.10.1o - Causal verification for the by-value setStakeSeen builder.

#include <boost/test/unit_test.hpp>

#include "../blockindex_stake_seen_builder.h"
#include "../blockindex_v2_reader.h"
#include "../blockindex_generation_builder.h"
#include "../blockindex_generation_lifecycle.h"
#include "../main.h"

#include <boost/filesystem.hpp>

#include <assert.h>
#include <stdio.h>
#include <string>
#include <vector>
#include <set>

namespace {

struct SB { uint256 hash; unsigned int nFile=0, nBlockPos=0, nSize=0; };

static SB WriteBlock(const boost::filesystem::path& dir, uint256 prev,
                     unsigned int nTime, unsigned int nBits, unsigned int nNonce)
{
    SB info; info.nFile = 1;
    CTransaction coinbase; coinbase.nVersion = 1; coinbase.nTime = nTime;
    CTxIn input; input.prevout = COutPoint(uint256(0), 0xffffffff);
    input.scriptSig = CScript() << OP_TRUE; input.nSequence = 0xffffffff;
    coinbase.vin.push_back(input);
    CTxOut output; output.nValue = 0; output.scriptPubKey = CScript() << OP_TRUE;
    coinbase.vout.push_back(output);
    CBlock block; block.nVersion = 1; block.hashPrevBlock = prev;
    block.nTime = nTime; block.nBits = nBits; block.nNonce = nNonce;
    block.vtx.push_back(coinbase); block.hashMerkleRoot = block.BuildMerkleTree();
    info.hash = block.GetHash();
    info.nSize = ::GetSerializeSize(block, SER_NETWORK, PROTOCOL_VERSION);
    CDataStream ss(SER_DISK, CLIENT_VERSION); ss << block;
    unsigned int ns = ss.size();
    boost::filesystem::path f = dir / "blk0001.dat";
    FILE* fp = fopen(f.string().c_str(), "ab"); assert(fp);
    unsigned char magic[] = {0xfa,0xbf,0xb5,0xda};
    fwrite(magic,1,4,fp); fwrite(&ns,4,1,fp);
    long pos = ftell(fp); info.nBlockPos = (unsigned int)pos;
    fwrite(&ss[0],1,ss.size(),fp); fflush(fp); fclose(fp);
    return info;
}

// Authoritative generation: active genesis(PoW), h1(PoW), h2(active PoS with
// prevoutStake/nStakeTime), plus a SIDE PoS record (fork off genesis). Both PoS
// records must appear in setStakeSeen; PoW excluded.
struct StakeFixture {
    boost::filesystem::path root, blockDir;
    std::vector<SB> active;
    SB sidePos;
    BlockIndexV2Reader reader;
    COutPoint posPrevout, sidePrevout;
    unsigned int posStakeTime, sideStakeTime;
    uint256 activePosHash;

    StakeFixture() {
        root = boost::filesystem::temp_directory_path() /
            boost::filesystem::unique_path("innova-stakeseen-%%%%-%%%%-%%%%");
        boost::filesystem::create_directories(root);
        blockDir = root / "blocks";
        boost::filesystem::create_directories(blockDir);
        uint256 prev(0);
        active.push_back(WriteBlock(blockDir, prev, 1000, 0x1d00ffffU, 101)); prev = active[0].hash; // gen PoW
        active.push_back(WriteBlock(blockDir, prev, 1001, 0x1d00ffffU, 102)); prev = active[1].hash; // h1 PoW
        active.push_back(WriteBlock(blockDir, prev, 1002, 0x1d00ffffU, 103)); prev = active[2].hash; // h2 active PoS
        activePosHash = active[2].hash;
        sidePos = WriteBlock(blockDir, active[0].hash, 2000, 0x1d00ffffU, 9001); // side PoS

        posPrevout = COutPoint(uint256(0xabc), 1); posStakeTime = 1002;
        sidePrevout = COutPoint(uint256(0xdef), 2); sideStakeTime = 2000;

        BlockIndexGenerationSource src;
        // genesis PoW
        { BlockIndexRecord rec;
          rec.hash=active[0].hash; rec.hashPrev=uint256(0); rec.height=0; rec.nVersion=1;
          rec.nTime=1000; rec.nBits=0x1d00ffffU; rec.nNonce=101;
          rec.nFile=active[0].nFile; rec.nBlockPos=active[0].nBlockPos; rec.nFlags=0;
          BlockIndexGenerationSourceRecord sr; sr.hash=rec.hash; sr.record=rec;
          src.records.push_back(sr); }
        // h1 PoW
        { BlockIndexRecord rec;
          rec.hash=active[1].hash; rec.hashPrev=active[0].hash; rec.height=1; rec.nVersion=1;
          rec.nTime=1001; rec.nBits=0x1d00ffffU; rec.nNonce=102;
          rec.nFile=active[1].nFile; rec.nBlockPos=active[1].nBlockPos; rec.nFlags=0;
          BlockIndexGenerationSourceRecord sr; sr.hash=rec.hash; sr.record=rec;
          src.records.push_back(sr); }
        // h2 active PoS
        { BlockIndexRecord rec;
          rec.hash=active[2].hash; rec.hashPrev=active[1].hash; rec.height=2; rec.nVersion=1;
          rec.nTime=1002; rec.nBits=0x1d00ffffU; rec.nNonce=103;
          rec.nFile=active[2].nFile; rec.nBlockPos=active[2].nBlockPos;
          rec.nFlags=CBlockIndex::BLOCK_PROOF_OF_STAKE;
          rec.prevoutStake=posPrevout; rec.nStakeTime=1002;
          BlockIndexGenerationSourceRecord sr; sr.hash=rec.hash; sr.record=rec;
          src.records.push_back(sr); }
        // side PoS (fork off genesis, height 1)
        { BlockIndexRecord rec;
          rec.hash=sidePos.hash; rec.hashPrev=active[0].hash; rec.height=1; rec.nVersion=1;
          rec.nTime=2000; rec.nBits=0x1d00ffffU; rec.nNonce=9001;
          rec.nFile=sidePos.nFile; rec.nBlockPos=sidePos.nBlockPos;
          rec.nFlags=CBlockIndex::BLOCK_PROOF_OF_STAKE;
          rec.prevoutStake=sidePrevout; rec.nStakeTime=2000;
          BlockIndexGenerationSourceRecord sr; sr.hash=rec.hash; sr.record=rec;
          src.records.push_back(sr); }
        src.hashBestChain = active[2].hash;
        src.foundBestChain = true;
        src.blockDataDir = blockDir.string();

        boost::filesystem::path staging = root / "build-000001.tmp";
        std::string error;
        { BlockIndexGenerationBuilder b;
          BOOST_REQUIRE_MESSAGE(b.Build(src, staging.string(), 1, NULL, &error), error); b.Close(); }
        BOOST_REQUIRE_MESSAGE(
            BlockIndexGenerationManager::PublishGeneration(root.string(), 1, &error)==(int)BLOCK_INDEX_LIFECYCLE_OK, error);
        BOOST_REQUIRE_MESSAGE(
            BlockIndexGenerationManager::SelectGeneration(root.string(), 1, &error)==(int)BLOCK_INDEX_LIFECYCLE_OK, error);
        BlockIndexV2ReaderOptions o;
        BOOST_REQUIRE_MESSAGE(reader.Open(root.string(), o, &error), error);
    }
    ~StakeFixture() { boost::system::error_code ec; boost::filesystem::remove_all(root, ec); }
};

} // namespace

BOOST_AUTO_TEST_SUITE(blockindex_stake_seen_builder_tests)

// O1/O2/O4/O5 - exact setStakeSeen parity: exactly the two PoS records (active h2
// + side), PoW/genesis excluded, side included, correct keys.
BOOST_AUTO_TEST_CASE(o1_exact_stake_seen_parity)
{
    StakeFixture fx;
    std::string error;
    BlockIndexStakeSeenBuilder b;
    std::set<std::pair<COutPoint,unsigned int> > ss;
    BOOST_REQUIRE_MESSAGE(b.Build(fx.reader, &ss, &error), error);
    BOOST_CHECK_EQUAL(ss.size(), (size_t)2);
    BOOST_CHECK(ss.count(std::make_pair(fx.posPrevout, fx.posStakeTime)) == 1);
    BOOST_CHECK(ss.count(std::make_pair(fx.sidePrevout, fx.sideStakeTime)) == 1);
}

// O6 CAUSAL no-map - builder runs with historical hashes absent; they stay absent.
BOOST_AUTO_TEST_CASE(o6_no_map_causal)
{
    StakeFixture fx;
    std::string error;
    for (size_t i = 0; i < fx.active.size(); ++i)
        BOOST_CHECK(mapBlockIndex.find(fx.active[i].hash) == mapBlockIndex.end());
    BOOST_CHECK(mapBlockIndex.find(fx.sidePos.hash) == mapBlockIndex.end());
    BlockIndexStakeSeenBuilder b;
    std::set<std::pair<COutPoint,unsigned int> > ss;
    BOOST_REQUIRE(b.Build(fx.reader, &ss, &error));
    for (size_t i = 0; i < fx.active.size(); ++i)
        BOOST_CHECK(mapBlockIndex.find(fx.active[i].hash) == mapBlockIndex.end());
    BOOST_CHECK(mapBlockIndex.find(fx.sidePos.hash) == mapBlockIndex.end());
}

BOOST_AUTO_TEST_SUITE_END()