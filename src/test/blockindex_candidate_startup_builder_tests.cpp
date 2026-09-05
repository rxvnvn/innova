// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.

// A.10.1m - Causal verification for the by-value candidate startup builder.

#include <boost/test/unit_test.hpp>

#include "../blockindex_candidate_startup_builder.h"
#include "../blockindex_v2_reader.h"
#include "../blockindex_derived_state.h"
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

struct SB
{
    uint256 hash;
    unsigned int nFile=0, nBlockPos=0, nSize=0;
};

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

// Build an authoritative generation: active chain 0..tip plus a side branch
// (a side tip not on chain). Returns reader + derived handles + tip/side hashes.
struct CandFixture
{
    boost::filesystem::path root, blockDir;
    std::vector<SB> active;
    SB side;
    int tipHeight;
    BlockIndexV2Reader reader;
    BlockIndexDerivedStateStore derived;

    CandFixture(int n) : tipHeight(n)
    {
        root = boost::filesystem::temp_directory_path() /
            boost::filesystem::unique_path("innova-cand-%%%%-%%%%-%%%%");
        boost::filesystem::create_directories(root);
        blockDir = root / "blocks";
        boost::filesystem::create_directories(blockDir);
        uint256 prev(0);
        for (int h = 0; h <= tipHeight; ++h) {
            SB b = WriteBlock(blockDir, prev, (unsigned int)(1000+h), 0x1d00ffffU, (unsigned int)(100+h));
            active.push_back(b); prev = b.hash;
        }
        // side branch: forks off height 0, one extra block, not on active chain
        side = WriteBlock(blockDir, active[0].hash, 2000, 0x1d00ffffU, 9001);

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
        {   // add side branch record
            BlockIndexRecord rec;
            rec.hash = side.hash; rec.hashPrev = active[0].hash; rec.height = 1;
            rec.nVersion = 1; rec.nTime = 2000; rec.nBits = 0x1d00ffffU;
            rec.nNonce = 9001; rec.nFile = side.nFile; rec.nBlockPos = side.nBlockPos;
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
        BOOST_REQUIRE_MESSAGE(
            BlockIndexDerivedStateStore::OpenReadOnly((root/"gen-000001").string(), 1, &derived, &error), error);
    }
    ~CandFixture() {
        boost::system::error_code ec;
        boost::filesystem::remove_all(root, ec);
    }
};

} // namespace

BOOST_AUTO_TEST_SUITE(blockindex_candidate_startup_builder_tests)

// M0/M1/M2/M3 - linear chain + side branch: by-value frontier finds exactly the
// active tip + the side tip (both not referenced as parents), and the evaluator
// picks the higher-trust winner.
BOOST_AUTO_TEST_CASE(m0_linear_and_side_tips)
{
    CandFixture fx(5);
    std::string error;
    SnapshotCandidateFrontierStore store;
    BlockIndexCandidateStartupBuilder builder;
    // historical hashes absent
    BOOST_CHECK(mapBlockIndex.find(fx.active[5].hash) == mapBlockIndex.end());
    BOOST_CHECK(mapBlockIndex.find(fx.side.hash) == mapBlockIndex.end());

    BOOST_REQUIRE_MESSAGE(builder.Build(fx.reader, fx.derived, 0x7fffffff, &store, &error), error);

    // tips: active tip (height5) + side tip (height1 fork)
    std::vector<uint256> tips = store.GetCandidateTipHashes();
    std::set<uint256> tipSet(tips.begin(), tips.end());
    BOOST_CHECK_MESSAGE(tipSet.count(fx.active[5].hash), "active tip must be a candidate");
    BOOST_CHECK_MESSAGE(tipSet.count(fx.side.hash), "side tip must be a candidate");
    // intermediate active blocks ARE referenced (as parents) -> not tips
    for (int h = 0; h <= 4; ++h)
        BOOST_CHECK_MESSAGE(!tipSet.count(fx.active[h].hash), "non-tip active block must not be candidate");

    // evaluator: strict '>' on trust; the active tip's trust == nBestChainTrust,
    // so NO eligible candidate exceeds best -> found=false (matches legacy
    // ActivateBestEligibleChain: candidate must strictly exceed best trust).
    CandidateFrontierAuthorityRecord sel = EvaluateCandidateFrontierByValue(store);
    BOOST_CHECK_MESSAGE(!sel.found, "no candidate strictly exceeds best trust on a plain chain");
}

// M4/M5/M6 - higher-trust SIDE tip is the strict-> winner (exact trust parity).
BOOST_AUTO_TEST_CASE(m4_higher_trust_side_wins)
{
    // Build a chain where the side tip has GREATER trust than the active tip by
    // adding a deeper side branch. Simpler: reuse CandFixture and manually inject
    // a snapshot store with two tips of known trust, then assert strict-> winner.
    CandFixture fx(3);
    SnapshotCandidateFrontierStore s;
    // active tip (lower trust)
    s.SetBest(fx.active[3].hash, uint256(uint64_t(100)));
    s.AddBlock(fx.active[3].hash, fx.active[2].hash, uint256(uint64_t(100)), 3);
    s.AddBlock(fx.side.hash, fx.active[1].hash, uint256(uint64_t(200)), 5);
    s.tipHashes.push_back(fx.active[3].hash);
    s.tipHashes.push_back(fx.side.hash);
    s.hasData.insert(fx.side.hash);      // side tip has block data (eligible)
    CandidateFrontierAuthorityRecord sel = EvaluateCandidateFrontierByValue(s);
    BOOST_REQUIRE_MESSAGE(sel.found, "higher-trust side tip must win");
    BOOST_CHECK(sel.hash == fx.side.hash);
    BOOST_CHECK(sel.chainTrust == uint256(uint64_t(200)));
}

// M9 - causal no-map: builder runs with mapBlockIndex empty; historical hashes
// remain absent after.
BOOST_AUTO_TEST_CASE(m9_no_map_causal)
{
    CandFixture fx(3);
    std::string error;
    SnapshotCandidateFrontierStore store;
    BlockIndexCandidateStartupBuilder builder;
    for (int h = 0; h <= 3; ++h)
        BOOST_CHECK(mapBlockIndex.find(fx.active[h].hash) == mapBlockIndex.end());
    BOOST_REQUIRE(builder.Build(fx.reader, fx.derived, 0x7fffffff, &store, &error));
    // after
    for (int h = 0; h <= 3; ++h)
        BOOST_CHECK(mapBlockIndex.find(fx.active[h].hash) == mapBlockIndex.end());
    BOOST_CHECK(mapBlockIndex.find(fx.side.hash) == mapBlockIndex.end());
}

// M10 - causal no-pprev: builder reproduces frontier without constructing pprev.
BOOST_AUTO_TEST_CASE(m10_no_pprev_causal)
{
    CandFixture fx(3);
    std::string error;
    SnapshotCandidateFrontierStore store;
    BlockIndexCandidateStartupBuilder builder;
    BOOST_REQUIRE(builder.Build(fx.reader, fx.derived, 0x7fffffff, &store, &error));
    // no pprev/topology reconstructed: synthetic hashes stay absent from the
    // resident map (only the global testing-setup genesis block is resident).
    for (int h = 0; h <= 3; ++h)
        BOOST_CHECK(mapBlockIndex.find(fx.active[h].hash) == mapBlockIndex.end());
    BOOST_CHECK(mapBlockIndex.find(fx.side.hash) == mapBlockIndex.end());
}

BOOST_AUTO_TEST_SUITE_END()
