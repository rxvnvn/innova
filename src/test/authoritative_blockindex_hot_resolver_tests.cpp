// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.

// A.10.1p - Causal verification for the by-value navigator hot resolver.

#include <boost/test/unit_test.hpp>

#include "../authoritative_blockindex_hot_resolver.h"
#include "../blockindex_v2_reader.h"
#include "../blockindex_generation_builder.h"
#include "../blockindex_generation_lifecycle.h"
#include "../main.h"

#include <boost/filesystem.hpp>

#include <assert.h>
#include <stdio.h>
#include <string>
#include <vector>

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

// Authoritative active chain 0..tip; no side record needed for hot resolver core.
struct HotFixture {
    boost::filesystem::path root, blockDir;
    std::vector<SB> active;
    int tipHeight;
    BlockIndexV2Reader reader;

    explicit HotFixture(int n) : tipHeight(n) {
        root = boost::filesystem::temp_directory_path() /
            boost::filesystem::unique_path("innova-hotres-%%%%-%%%%-%%%%");
        boost::filesystem::create_directories(root);
        blockDir = root / "blocks";
        boost::filesystem::create_directories(blockDir);
        uint256 prev(0);
        for (int h = 0; h <= tipHeight; ++h) {
            SB b = WriteBlock(blockDir, prev, (unsigned int)(1000+h), 0x1d00ffffU, (unsigned int)(100+h));
            active.push_back(b); prev = b.hash;
        }
        BlockIndexGenerationSource src;
        for (int h = 0; h <= tipHeight; ++h) {
            BlockIndexRecord rec;
            rec.hash = active[h].hash;
            rec.hashPrev = (h==0)?uint256(0):active[h-1].hash;
            rec.height = h; rec.nVersion = 1;
            rec.nTime = (unsigned int)(1000+h); rec.nBits = 0x1d00ffffU;
            rec.nNonce = (unsigned int)(100+h);
            rec.nFile = active[h].nFile; rec.nBlockPos = active[h].nBlockPos;
            BlockIndexGenerationSourceRecord sr; sr.hash=rec.hash; sr.record=rec;
            src.records.push_back(sr);
        }
        src.hashBestChain = active[tipHeight].hash;
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
    ~HotFixture() { boost::system::error_code ec; boost::filesystem::remove_all(root, ec); }
};

} // namespace

BOOST_AUTO_TEST_SUITE(authoritative_blockindex_hot_resolver_tests)

// P0/P3/P6/P7 - by-value hot resolver core ops: Lookup, GetParent, GetActiveByHeight,
// GetNextActive, GetTip - all resolve against authoritative reader, no mapBlockIndex.
BOOST_AUTO_TEST_CASE(p0_hot_resolver_core_resolves_by_value)
{
    HotFixture fx(5);
    std::string error;
    AuthoritativeBlockIndexHotResolver res(&fx.reader);

    // P15 causal pre: historical hashes absent
    for (int h = 0; h <= 5; ++h)
        BOOST_CHECK(mapBlockIndex.find(fx.active[h].hash) == mapBlockIndex.end());

    // P6 GetActiveByHeight
    BlockIndexSnapshot s2 = res.GetActiveByHeight(2);
    BOOST_REQUIRE(s2.found);
    BOOST_CHECK_EQUAL(s2.height, 2);
    BOOST_CHECK(s2.hash == fx.active[2].hash);

    // P0 LookupByHash
    BlockIndexSnapshot s5 = res.LookupByHash(fx.active[5].hash);
    BOOST_REQUIRE(s5.found);
    BOOST_CHECK_EQUAL(s5.height, 5);
    BOOST_CHECK(s5.fInMainChain);

    // unknown hash -> found=false
    BlockIndexSnapshot sU = res.LookupByHash(uint256(0xdead));
    BOOST_CHECK(!sU.found);

    // P3 GetParentByHash
    BlockIndexSnapshot par = res.GetParentByHash(fx.active[3].hash);
    BOOST_REQUIRE(par.found);
    BOOST_CHECK(par.hash == fx.active[2].hash);
    BOOST_CHECK_EQUAL(par.height, 2);

    // genesis parent -> found=false
    BlockIndexSnapshot gpar = res.GetParentByHash(fx.active[0].hash);
    BOOST_CHECK(!gpar.found);

    // P7 GetNextActiveByHash middle
    BlockIndexSnapshot nxt = res.GetNextActiveByHash(fx.active[2].hash);
    BOOST_REQUIRE(nxt.found);
    BOOST_CHECK(nxt.hash == fx.active[3].hash);

    // P8 GetNextActive tip -> found=false
    BlockIndexSnapshot tipNxt = res.GetNextActiveByHash(fx.active[5].hash);
    BOOST_CHECK(!tipNxt.found);

    // GetTip
    BlockIndexSnapshot tip = res.GetTip();
    BOOST_REQUIRE(tip.found);
    BOOST_CHECK(tip.hash == fx.active[5].hash);

    // P15 causal post: still absent
    for (int h = 0; h <= 5; ++h)
        BOOST_CHECK(mapBlockIndex.find(fx.active[h].hash) == mapBlockIndex.end());
}

// P13/P14 - STALE generation / mismatched reader fails closed (resolver holds a
// closed/other-generation reader -> all ops return found=false, never a legacy
// fallback). Simulated: resolver over a non-open reader.
BOOST_AUTO_TEST_CASE(p14_stale_generation_fails_closed)
{
    BlockIndexV2Reader closed; // not open
    AuthoritativeBlockIndexHotResolver res(&closed);
    BlockIndexSnapshot s = res.LookupByHash(uint256(0x1));
    BOOST_CHECK(!s.found);
    s = res.GetActiveByHeight(0);
    BOOST_CHECK(!s.found);
    s = res.GetTip();
    BOOST_CHECK(!s.found);
}

BOOST_AUTO_TEST_SUITE_END()
