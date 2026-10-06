// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.

// PM1-P0-06 A1 — operator-invalid by-value parity suite.
//
// Proves the authoritative (BY_VALUE_AUTHORITATIVE) operator-invalid verdict
// (IsBlockOperatorInvalidTyped) against a COMPLETE raw-pointer chain oracle
// (the legacy IsBlockOperatorInvalid raw pprev walk), including ancestry FAR
// BELOW the 2048 live horizon, and proves fail-closed on authority
// unavailability / unresolvable required records.

#include <boost/test/unit_test.hpp>

#include "blockindex_authoritative_live.h"
#include "blockindex_authoritative_startup.h"
#include "blockindex_v2_reader.h"
#include "blockindex_generation_builder.h"
#include "blockindex_generation_lifecycle.h"
#include "blockindex_tip.h"
#include "main.h"

#include <boost/filesystem.hpp>

#include <stdio.h>
#include <set>
#include <string>
#include <vector>

namespace fs = boost::filesystem;

static std::string MakeTempDir()
{
    char tmpl[] = "/tmp/innova-p06a1-XXXXXX";
    char* d = mkdtemp(tmpl);
    BOOST_REQUIRE(d != NULL);
    return std::string(d);
}

struct P06Fixture
{
    fs::path root;
    uint64_t baseGen;
    int baseTip;
    std::vector<uint256> baseActive;
    std::string rootStr;

    explicit P06Fixture(int s)
        : root(MakeTempDir()), baseGen(1), baseTip(s)
    {
        BlockIndexGenerationSource src;
        for (int h = 0; h <= baseTip; ++h)
        {
            uint256 hv = uint256(0xD0000000UL + h);
            uint256 hp = (h == 0) ? uint256(0) : baseActive[h - 1];
            BlockIndexRecord rec;
            rec.hash = hv;
            rec.hashPrev = hp;
            rec.hashMerkleRoot = uint256(0x1111UL + h);
            rec.height = h;
            rec.nFile = 1;
            rec.nBlockPos = 100u + (unsigned)h;
            rec.nFlags = 0;
            rec.nVersion = 7;
            rec.nTime = 1700000000u + (unsigned)h;
            rec.nBits = 0x1d00ffff;
            rec.nNonce = (unsigned)h;
            rec.nMint = 100;
            rec.nMoneySupply = 500;
            baseActive.push_back(hv);
            BlockIndexGenerationSourceRecord sr; sr.hash = rec.hash; sr.record = rec;
            src.records.push_back(sr);
        }
        src.hashBestChain = baseActive[baseTip];
        src.foundBestChain = true;
        BlockIndexGenerationBuilder b;
        BlockIndexGenerationStats stats;
        std::string error;
        fs::path tmp = root / "blockindex-build-000001.tmp";
        BOOST_REQUIRE_MESSAGE(b.Build(src, tmp.string(), baseGen, &stats, &error), error);
        b.Close();
        std::string perr;
        BOOST_REQUIRE_MESSAGE(
            BlockIndexGenerationManager::PublishGeneration(root.string(), baseGen, &perr) ==
                (int)BLOCK_INDEX_LIFECYCLE_OK, perr);
        BOOST_REQUIRE_MESSAGE(
            BlockIndexGenerationManager::SelectGeneration(root.string(), baseGen, &perr) ==
                (int)BLOCK_INDEX_LIFECYCLE_OK, perr);
        rootStr = root.string();
    }
    ~P06Fixture() { boost::system::error_code ec; fs::remove_all(root, ec); }
};

BOOST_AUTO_TEST_SUITE(blockindex_p06_operator_invalid_byvalue_tests)

BOOST_AUTO_TEST_CASE(p06_a1_parity_and_failclosed)
{
    // Chain height S = 2500 > 2048 so the deep-ancestry cases cross FAR below
    // the future live horizon.
    P06Fixture fx(2500);
    BlockIndexV2Reader reader;
    BlockIndexV2ReaderOptions opts;
    std::string error;
    BOOST_REQUIRE_MESSAGE(reader.Open(fx.rootStr, opts, &error), error);

    BlockIndexAuthoritativeLive live;
    BOOST_REQUIRE_MESSAGE(live.Open(fx.rootStr, &reader, 2048, &error), error);
    BOOST_REQUIRE(live.IsOpen());

    const int S = fx.baseTip;                       // 2500
    const uint256 hInv = fx.baseActive[100];        // invalid ancestor, deep
    const uint256 hInv2 = fx.baseActive[200];       // second invalid ancestor

    // ---- ORACLE: complete raw-pointer chain (heights 0..S) ----
    std::vector<CBlockIndex> oracleChain(S + 1);
    std::vector<uint256> oracleHash(S + 1);
    for (int h = 0; h <= S; ++h)
    {
        oracleHash[h] = fx.baseActive[h];
        oracleChain[h].phashBlock = &oracleHash[h];
        oracleChain[h].nHeight = h;
        oracleChain[h].pprev = (h > 0) ? &oracleChain[h - 1] : NULL;
    }
    const std::set<uint256> savedInvalid = setInvalidBlockHash;
    setInvalidBlockHash.clear();
    setInvalidBlockHash.insert(hInv);
    setInvalidBlockHash.insert(hInv2);

    const bool oldDesc = IsBlockOperatorInvalid(&oracleChain[2400]); // deep descendant of hInv
    const bool oldSelf = IsBlockOperatorInvalid(&oracleChain[100]);  // == hInv
    const bool oldAbove = IsBlockOperatorInvalid(&oracleChain[50]);  // above hInv, no invalid
    const bool oldGenesis = IsBlockOperatorInvalid(&oracleChain[0]); // genesis, not invalid
    setInvalidBlockHash = savedInvalid;

    // ---- arm authoritative mode ----
    const bool savedAuth = g_fAuthoritativeStartup;
    SetAuthoritativeLiveForTesting(&live);
    ::g_fAuthoritativeStartup = true;

    std::string serr;
    BOOST_REQUIRE_EQUAL((int)live.TipAuthorityMutable()->SetOperatorInvalid(hInv, true, &serr),
                        (int)BLOCK_INDEX_TIP_OK);
    BOOST_REQUIRE_EQUAL((int)live.TipAuthorityMutable()->SetOperatorInvalid(hInv2, true, &serr),
                        (int)BLOCK_INDEX_TIP_OK);

    // candidate CBlockIndex objects: only phashBlock/nHeight matter to the verdict
    CBlockIndex cDesc; uint256 hDesc = fx.baseActive[2400]; cDesc.phashBlock = &hDesc; cDesc.nHeight = 2400;
    CBlockIndex cSelf; uint256 hSelf = hInv; cSelf.phashBlock = &hSelf; cSelf.nHeight = 100;
    CBlockIndex cAbove; uint256 hAbove = fx.baseActive[50]; cAbove.phashBlock = &hAbove; cAbove.nHeight = 50;
    CBlockIndex cGen; uint256 hGen = fx.baseActive[0]; cGen.phashBlock = &hGen; cGen.nHeight = 0;

    // ---- PARITY: new by-value == old complete-chain oracle ----
    BOOST_CHECK_EQUAL(IsBlockOperatorInvalidTyped(&cDesc) == BLOCK_INDEX_OPERATOR_INVALID, oldDesc);
    BOOST_CHECK_EQUAL(IsBlockOperatorInvalidTyped(&cSelf) == BLOCK_INDEX_OPERATOR_INVALID, oldSelf);
    BOOST_CHECK_EQUAL(IsBlockOperatorInvalidTyped(&cAbove) == BLOCK_INDEX_OPERATOR_INVALID, oldAbove);
    BOOST_CHECK_EQUAL(IsBlockOperatorInvalidTyped(&cGen) == BLOCK_INDEX_OPERATOR_INVALID, oldGenesis);

    // ---- explicit expected verdicts (deep below horizon, 2300 blocks) ----
    BOOST_CHECK_EQUAL((int)IsBlockOperatorInvalidTyped(&cDesc), (int)BLOCK_INDEX_OPERATOR_INVALID);
    BOOST_CHECK_EQUAL((int)IsBlockOperatorInvalidTyped(&cSelf), (int)BLOCK_INDEX_OPERATOR_INVALID);
    BOOST_CHECK_EQUAL((int)IsBlockOperatorInvalidTyped(&cAbove), (int)BLOCK_INDEX_OPERATOR_VALID);
    BOOST_CHECK_EQUAL((int)IsBlockOperatorInvalidTyped(&cGen), (int)BLOCK_INDEX_OPERATOR_VALID);

    // ---- fail-closed: authority closed ----
    SetAuthoritativeLiveForTesting(NULL);
    BOOST_CHECK_EQUAL((int)IsBlockOperatorInvalidTyped(&cDesc), (int)BLOCK_INDEX_OPERATOR_UNAVAILABLE);

    // ---- fail-closed: required authoritative record unresolvable ----
    SetAuthoritativeLiveForTesting(&live);
    const uint256 hGhost = uint256(0x0BADF00DUL);
    BOOST_REQUIRE_EQUAL((int)live.TipAuthorityMutable()->SetOperatorInvalid(hGhost, true, &serr),
                        (int)BLOCK_INDEX_TIP_OK);
    BOOST_CHECK_EQUAL((int)IsBlockOperatorInvalidTyped(&cDesc), (int)BLOCK_INDEX_OPERATOR_UNAVAILABLE);

    // ---- empty invalid set -> VALID (O(1) fast path) ----
    BOOST_REQUIRE_EQUAL((int)live.TipAuthorityMutable()->SetOperatorInvalid(hGhost, false, &serr),
                        (int)BLOCK_INDEX_TIP_OK);
    BOOST_REQUIRE_EQUAL((int)live.TipAuthorityMutable()->SetOperatorInvalid(hInv, false, &serr),
                        (int)BLOCK_INDEX_TIP_OK);
    BOOST_REQUIRE_EQUAL((int)live.TipAuthorityMutable()->SetOperatorInvalid(hInv2, false, &serr),
                        (int)BLOCK_INDEX_TIP_OK);
    BOOST_CHECK_EQUAL((int)IsBlockOperatorInvalidTyped(&cDesc), (int)BLOCK_INDEX_OPERATOR_VALID);

    ::g_fAuthoritativeStartup = savedAuth;
    ClearAuthoritativeLiveForTesting();
    live.Close();
    reader.Close();

    printf("P06-A1 PASS: operator-invalid by-value parity vs complete raw chain "
           "(deep ancestor 2300 blocks below horizon), fail-closed on closed "
           "authority / unresolvable invalid id, empty-set fast path VALID.\n");
}

BOOST_AUTO_TEST_CASE(p06_a1d_tip_ancestor_peer_parity_and_failclosed)
{
    P06Fixture fx(2500);
    BlockIndexV2Reader reader;
    BlockIndexV2ReaderOptions opts;
    std::string error;
    BOOST_REQUIRE_MESSAGE(reader.Open(fx.rootStr, opts, &error), error);
    BlockIndexAuthoritativeLive live;
    BOOST_REQUIRE_MESSAGE(live.Open(fx.rootStr, &reader, 2048, &error), error);
    BOOST_REQUIRE(live.IsOpen());

    const int savedBest = nBestHeight;
    const uint256 savedBestHash = hashBestChain;
    const bool savedAuth = g_fAuthoritativeStartup;

    nBestHeight = 100;
    hashBestChain = fx.baseActive[100];

    SetAuthoritativeLiveForTesting(&live);
    ::g_fAuthoritativeStartup = true;

    // (1) peer best == local tip -> 1
    BOOST_CHECK_EQUAL(TipAncestorOfPeerBestKnown(fx.baseActive[100]), 1);
    // (2) peer best ancestor inside hot horizon -> 1
    BOOST_CHECK_EQUAL(TipAncestorOfPeerBestKnown(fx.baseActive[105]), 1);
    // (3) peer best ancestor FAR BELOW 2048 (depth 2300) -> 1
    BOOST_CHECK_EQUAL(TipAncestorOfPeerBestKnown(fx.baseActive[2400]), 1);
    // (4) sibling/fork: ancestor at nBestHeight != hashBestChain -> 0
    hashBestChain = fx.baseActive[101];
    BOOST_CHECK_EQUAL(TipAncestorOfPeerBestKnown(fx.baseActive[2400]), 0);
    hashBestChain = fx.baseActive[100];
    // (5) peer best below local best -> 0
    BOOST_CHECK_EQUAL(TipAncestorOfPeerBestKnown(fx.baseActive[50]), 0);
    // (6) unknown peer best -> -1 ; zero -> -1
    BOOST_CHECK_EQUAL(TipAncestorOfPeerBestKnown(uint256(0x1234UL)), -1);
    BOOST_CHECK_EQUAL(TipAncestorOfPeerBestKnown(uint256(0)), -1);
    // (7) fail-closed: closed authority -> -1 (unknown), never a false answer
    SetAuthoritativeLiveForTesting(NULL);
    BOOST_CHECK_EQUAL(TipAncestorOfPeerBestKnown(fx.baseActive[2400]), -1);
    SetAuthoritativeLiveForTesting(&live);

    // ORACLE parity on case (3): legacy raw-chain answer == authoritative answer.
    ::g_fAuthoritativeStartup = false;
    std::vector<CBlockIndex> chain(2401);
    std::vector<uint256> hh(2401);
    for (int h = 0; h <= 2400; ++h)
    {
        hh[h] = fx.baseActive[h];
        chain[h].phashBlock = &hh[h];
        chain[h].nHeight = h;
        chain[h].pprev = (h > 0) ? &chain[h - 1] : NULL;
    }
    mapBlockIndex[fx.baseActive[2400]] = &chain[2400];
    const int legacyRes = TipAncestorOfPeerBestKnown(fx.baseActive[2400]);
    mapBlockIndex.erase(fx.baseActive[2400]);
    ::g_fAuthoritativeStartup = true;
    const int authRes = TipAncestorOfPeerBestKnown(fx.baseActive[2400]);
    BOOST_CHECK_EQUAL(legacyRes, 1);
    BOOST_CHECK_EQUAL(authRes, legacyRes); // parity

    ::g_fAuthoritativeStartup = savedAuth;
    nBestHeight = savedBest;
    hashBestChain = savedBestHash;
    ClearAuthoritativeLiveForTesting();
    live.Close();
    reader.Close();

    printf("P06-A1D PASS: TipAncestorOfPeerBestKnown by-value parity vs complete "
           "raw chain (deep ancestor 2300 below horizon), sibling -> 0, unknown "
           "-> -1, closed authority -> -1 (unknown).\n");
}

BOOST_AUTO_TEST_SUITE_END()
