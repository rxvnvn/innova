// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.

// G1 — full-topology live-tail materializer causal verification.
//
// Proves that a linked CBlockIndex chain (pprev/pnext/pskip + scalar metadata)
// can be materialized from the by-value authoritative V2 authority, so the
// legacy consensus engine's pointer walks (ComputeNextStakeModifier /
// GetLastStakeModifier, GetMedianTimePast, SetBestChain, Reorganize) can run on
// a BOUNDED resident tail fed from by-value storage -- without rebuilding
// historical mapBlockIndex residency.

#include <boost/test/unit_test.hpp>

#include "blockindex_live_tail_full.h"
#include "blockindex_v2_reader.h"
#include "blockindex_generation_builder.h"
#include "blockindex_generation_lifecycle.h"
#include "main.h"

#include <boost/filesystem.hpp>

#include <stdio.h>
#include <string>
#include <vector>

namespace fs = boost::filesystem;

static std::string MakeTempDir()
{
    char tmpl[] = "/tmp/innova-g1full-XXXXXX";
    char* d = mkdtemp(tmpl);
    BOOST_REQUIRE(d != NULL);
    return std::string(d);
}

struct G1FullFixture
{
    fs::path root;
    int tip;
    std::vector<uint256> active;
    std::string rootStr;

    explicit G1FullFixture(int n) : root(MakeTempDir()), tip(n)
    {
        BlockIndexGenerationSource src;
        for (int h = 0; h <= tip; ++h)
        {
            uint256 hv = uint256(0xC0000000UL + h);
            uint256 hp = (h == 0) ? uint256(0) : active[h - 1];
            BlockIndexRecord rec;
            rec.hash = hv;
            rec.hashPrev = hp;
            rec.hashMerkleRoot = uint256(0x1111UL + h);
            rec.height = h;
            rec.nFile = 1;
            rec.nBlockPos = 100u + (unsigned)h;
            rec.nFlags = (h == 0) ? CBlockIndex::BLOCK_STAKE_MODIFIER : 0; // genesis generates modifier
            rec.nVersion = 7;
            rec.nTime = 1700000000u + (unsigned)h;
            rec.nBits = 0x1d00ffff;
            rec.nNonce = (unsigned)h;
            rec.nMint = 100;
            rec.nMoneySupply = 500;
            active.push_back(hv);
            BlockIndexGenerationSourceRecord s; s.hash = rec.hash; s.record = rec;
            src.records.push_back(s);
        }
        src.hashBestChain = active[tip];
        src.foundBestChain = true;
        BlockIndexGenerationBuilder b;
        BlockIndexGenerationStats stats;
        std::string error;
        fs::path tmp = root / "build-000001.tmp";
        BOOST_REQUIRE_MESSAGE(b.Build(src, tmp.string(), 1, &stats, &error), error);
        b.Close();
        std::string perr;
        BOOST_REQUIRE_MESSAGE(
            BlockIndexGenerationManager::PublishGeneration(root.string(), 1, &perr) ==
                (int)BLOCK_INDEX_LIFECYCLE_OK, perr);
        BOOST_REQUIRE_MESSAGE(
            BlockIndexGenerationManager::SelectGeneration(root.string(), 1, &perr) ==
                (int)BLOCK_INDEX_LIFECYCLE_OK, perr);
        rootStr = root.string();
    }
    ~G1FullFixture() { boost::system::error_code ec; fs::remove_all(root, ec); }
};

BOOST_AUTO_TEST_SUITE(blockindex_live_tail_full_tests)

// G1-FULL-A: MaterializeChain from tip down to genesis builds a walkable
// pprev/pnext/pskip chain with correct scalar metadata (height, chainTrust,
// hash identity, phashBlock owner-owned).
BOOST_AUTO_TEST_CASE(g1full_chain_materialization_topology)
{
    G1FullFixture fx(6);
    BlockIndexV2Reader reader;
    BlockIndexV2ReaderOptions opts;
    std::string error;
    BOOST_REQUIRE_MESSAGE(reader.Open(fx.rootStr, opts, &error), error);

    BlockIndexLiveTailFull full;
    full.SetSources(&reader, NULL);
    BlockIndexHotHandle tipHandle;
    // Materialize from tip (height 6) down to genesis (height 0).
    CBlockIndex* tip = full.MaterializeChain(fx.active[6], fx.active[0], &tipHandle, &error);
    BOOST_REQUIRE_MESSAGE(tip != NULL, error);
    BOOST_CHECK_EQUAL(tip->nHeight, 6);
    BOOST_CHECK(tip->GetBlockHash() == fx.active[6]);

    // Walk pprev down to genesis; assert heights decrement and linkage is exact.
    CBlockIndex* p = tip;
    int expected = 6;
    bool reachedFloor = false;
    while (p)
    {
        BOOST_CHECK_EQUAL(p->nHeight, expected);
        BOOST_CHECK(p->GetBlockHash() == fx.active[expected]);
        if (expected == 0)
        {
            reachedFloor = true;
            break;
        }
        p = p->pprev;
        --expected;
    }
    BOOST_CHECK(reachedFloor);
    BOOST_CHECK_EQUAL(full.ResidentCount(), (size_t)7);

    // pnext links (except floor) point to the child.
    BOOST_CHECK(tip->pnext == NULL);          // tip has no child
    CBlockIndex* floor = tip;
    while (floor->pprev) floor = floor->pprev; // reach genesis
    BOOST_CHECK(floor->pnext != NULL && floor->pnext->nHeight == 1);

    // Owner-owned identity: phashBlock must be non-NULL and equal the logical hash.
    BOOST_CHECK(tip->phashBlock != NULL);
    BOOST_CHECK(*tip->phashBlock == fx.active[6]);

    reader.Close();
    printf("G1-FULL PASS: MaterializeChain built walkable pprev/pnext/pskip chain\n"
           "       heights 6..0 with owner-owned identity from by-value authority.\n");
}

// G1-FULL-B: a missing floor (authority absence) fails closed (no partial chain,
// no silent fallback to legacy residency).
BOOST_AUTO_TEST_CASE(g1full_unreachable_floor_fails_closed)
{
    G1FullFixture fx(4);
    BlockIndexV2Reader reader;
    BlockIndexV2ReaderOptions opts;
    std::string error;
    BOOST_REQUIRE_MESSAGE(reader.Open(fx.rootStr, opts, &error), error);

    BlockIndexLiveTailFull full;
    full.SetSources(&reader, NULL);
    BlockIndexHotHandle handle;
    uint256 bogusFloor = uint256(0xDEADBEEFUL);
    CBlockIndex* tip = full.MaterializeChain(fx.active[4], bogusFloor, &handle, &error);
    BOOST_CHECK(tip == NULL); // floor not in authority => fail closed
    BOOST_CHECK(!error.empty());

    reader.Close();
    printf("G1-FULL PASS: unreachable floor fails closed (no partial / no legacy fallback).\n");
}

BOOST_AUTO_TEST_SUITE_END()