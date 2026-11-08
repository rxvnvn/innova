// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.
//
// ===========================================================================
// V2 MS-FIX3 PHASE 1 — money-supply invariant at publication time.
// ===========================================================================
//
// The exact per-block identity threaded from ConnectBlock through the reorg
// connect loop (g_reorgConnectSupply) into the publication call that feeds
// ReorgActiveTo:
//
//     supply(h) - supply(authoritative parent at h-1) == mint(h) - fees(h) - burned(h)
//
// This file covers, at the tip-authority and live-authority level (fast,
// deterministic, no mining, no daemons):
//   * released-edge reconnect carry (the ConnectBlockResolveParentSupply
//     contract: authoritative by-value read, fail-closed on unresolvable /
//     identity-less edges, legacy-mode tolerance);
//   * promote path and append path reorgs;
//   * PoW and PoS blocks;
//   * legitimate zero mint/supply on never-connected side records NOT being
//     flagged (they stay side: exempt) and legit zero mint on an active block;
//   * burned outputs reducing supply exactly;
//   * a deliberate inconsistent ACTIVE record rejected fail-closed (RED
//     without the gate, GREEN with it).
//
// Scope note: the chain-level ConnectBlock end-to-end carry is covered by the
// reorg_314 chain regression (deep_reorg_money_supply_carry); this file pins
// the publication-time gate and the parent-supply carry contract itself.
// ============================================================================

#include <boost/test/unit_test.hpp>

#include "blockindex_tip.h"
#include "blockindex_authoritative_startup.h"
#include "blockindex_authoritative_live.h"
#include "blockindex_derived_state.h"
#include "blockindex_hashindex.h"
#include "blockindex_generation_builder.h"
#include "blockindex_generation_lifecycle.h"
#include "main.h"
#include "util.h"

#include <boost/filesystem.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace fs = boost::filesystem;

BOOST_AUTO_TEST_SUITE(supply_invariant_tests)

// ---------------------------------------------------------------------------
// helpers (SI-prefixed static; no duplicated symbols with other test files)
// ---------------------------------------------------------------------------

static std::string SIMakeTempDir()
{
    char tmpl[] = "/tmp/innova-sitip-XXXXXX";
    char* d = mkdtemp(tmpl);
    BOOST_REQUIRE(d != NULL);
    return std::string(d);
}

static BlockIndexRecord SIMakeRecord(const uint256& hash, const uint256& prev,
                                     int height, bool isPos)
{
    BlockIndexRecord r;
    r.hash = hash; r.hashPrev = prev; r.height = height;
    r.nFile = 1; r.nBlockPos = (unsigned)(height * 100);
    r.nFlags = isPos ? CBlockIndex::BLOCK_PROOF_OF_STAKE : 0;
    r.nVersion = 5; r.nTime = 1600000000u + (unsigned)height;
    r.nBits = 0x1d00ffff; r.nNonce = (unsigned)height;
    if (isPos)
    {
        r.prevoutStake = COutPoint(uint256((unsigned)height), 0);
        r.nStakeTime = 1600000000u + (unsigned)height;
    }
    return r;
}

static BlockIndexDerivedEntry SIMakeDerived(uint32_t checksum)
{
    BlockIndexDerivedEntry d;
    d.chainTrust = uint256(checksum);
    d.stakeModifierChecksum = checksum;
    d.SetHasStakeModifierTime(true);
    d.stakeModifierTime = 1600000000;
    d.SetHasBlockSize(true);
    d.nSize = 1200 + (checksum % 100);
    return d;
}

// Append one ACTIVE record with its ConnectBlock-result supply/mint.
static void SIAppendActive(BlockIndexTipAuthority& tip, const uint256& hash,
                           const uint256& prev, int height, bool isPos,
                           int64_t supply, int64_t mint)
{
    BlockIndexRecord r = SIMakeRecord(hash, prev, height, isPos);
    r.nMoneySupply = supply;
    r.nMint = mint;
    BlockIndexTipAppend a;
    a.record = r;
    a.derived = SIMakeDerived((uint32_t)(height * 7 + 1));
    std::string err;
    BOOST_REQUIRE_MESSAGE(tip.Append(a, height, &err) == BLOCK_INDEX_TIP_OK,
                          "active append h=" << height << ": " << err);
}

// ---------------------------------------------------------------------------
// Two-truth helper: build an active chain (fork prefix + kept A chain), then
// run ReorgActiveTo with an explicit connect trail. Returns via out params.
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Two-truth helper: build an active chain (fork prefix + kept A chain), then
// run ReorgActiveTo with an explicit connect trail (see the individual cases).
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(si_append_path_pow_records_invariant_ok)
{
    const std::string dir = SIMakeTempDir();
    BlockIndexTipAuthority tip;
    BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 1, 0, 0, &tip, NULL));

    // Active chain A1..A3 (heights 1..3, PoW records), supplies 100/200/300
    // with mint 100 per block (fees/burned 0 on these coinbase-only blocks).
    std::vector<uint256> ashs; ashs.push_back(uint256(0));
    for (int h = 1; h <= 3; ++h)
    {
        const int64_t sup = 100 * h;
        SIAppendActive(tip, uint256(0xA000UL + h), ashs.back(), h, false, sup, 100);
        ashs.push_back(uint256(0xA000UL + h));
    }

    // BRANCH B (append path): B1@h2..B2@h3 on top of A1 (fork at height 1,
    // committed anchor = A1 supply = 100).
    const uint256 b1(0xB001UL), b2(0xB002UL);
    BlockIndexRecord rb1; rb1.hash = b1; rb1.hashPrev = ashs[1]; rb1.height = 2;
    rb1.nMint = 80; rb1.nMoneySupply = 170;   // 100 + 80 - 10 - 0
    BlockIndexDerivedEntry db1 = SIMakeDerived(2001);
    BlockIndexRecord rb2; rb2.hash = b2; rb2.hashPrev = b1; rb2.height = 3;
    rb2.nMint = 80; rb2.nMoneySupply = 240;   // 170 + 80 - 10 - 0
    BlockIndexDerivedEntry db2 = SIMakeDerived(2002);

    std::map<int32_t, BlockIndexConnectSupplyEntry> trail;
    BlockIndexConnectSupplyEntry anchor; anchor.supply = 100;
    trail[1] = anchor;   // committed fork record A1 (the committed store is authoritative)
    BlockIndexConnectSupplyEntry e1; e1.supply = 170; e1.mint = 80; e1.fees = 10; e1.burned = 0;
    trail[2] = e1;
    BlockIndexConnectSupplyEntry e2; e2.supply = 240; e2.mint = 80; e2.fees = 10; e2.burned = 0;
    trail[3] = e2;

    std::vector<BlockIndexTipAppend> branch; std::vector<int32_t> heights;
    BlockIndexTipAppend ab1; ab1.record = rb1; ab1.derived = db1;
    BlockIndexTipAppend ab2; ab2.record = rb2; ab2.derived = db2;
    branch.push_back(ab1); branch.push_back(ab2);
    heights.push_back(2); heights.push_back(3);

    std::string err;
    const BlockIndexTipStatus st = tip.ReorgActiveTo(1, branch, heights, &err, &trail);
    BOOST_CHECK_MESSAGE(st == BLOCK_INDEX_TIP_OK,
                        "append path (PoW): consistent active suffix must publish; err=" << err);
    BOOST_CHECK_MESSAGE(tip.GetTip().record.hash == b2,
                        "append path (PoW): the branch tip must become the published tip");
    BOOST_CHECK_EQUAL(tip.GetTip().record.nMoneySupply, 240LL);
}

BOOST_AUTO_TEST_CASE(si_append_path_pos_records_invariant_ok)
{
    const std::string dir = SIMakeTempDir();
    BlockIndexTipAuthority tip;
    BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 1, 0, 0, &tip, NULL));

    // Active chain A1..A2 (heights 1..2, PoS records), supplies 1000/2000,
    // mint 1000 per block.
    std::vector<uint256> ashs; ashs.push_back(uint256(0));
    for (int h = 1; h <= 2; ++h)
    {
        SIAppendActive(tip, uint256(0xC000UL + h), ashs.back(), h, true, 1000 * h, 1000);
        ashs.push_back(uint256(0xC000UL + h));
    }

    // BRANCH B: B1@h2 on top of A1 (PoS record).
    const uint256 b1(0xD001UL);
    BlockIndexRecord rb1; rb1.hash = b1; rb1.hashPrev = ashs[1]; rb1.height = 2;
    rb1.nFlags = CBlockIndex::BLOCK_PROOF_OF_STAKE;
    rb1.prevoutStake = COutPoint(uint256(9), 1); rb1.nStakeTime = 1600000010u;
    rb1.nMint = 1000; rb1.nMoneySupply = 3000;  // 2000 + 1000 - 0 - 0
    BlockIndexDerivedEntry db1 = SIMakeDerived(4001);

    std::map<int32_t, BlockIndexConnectSupplyEntry> trail;
    BlockIndexConnectSupplyEntry anchor; anchor.supply = 2000;
    trail[1] = anchor;
    BlockIndexConnectSupplyEntry e1; e1.supply = 3000; e1.mint = 1000; e1.fees = 0; e1.burned = 0;
    trail[2] = e1;

    std::vector<BlockIndexTipAppend> branch; std::vector<int32_t> heights;
    BlockIndexTipAppend ab1; ab1.record = rb1; ab1.derived = db1;
    branch.push_back(ab1); heights.push_back(2);

    std::string err;
    const BlockIndexTipStatus st = tip.ReorgActiveTo(1, branch, heights, &err, &trail);
    BOOST_CHECK_MESSAGE(st == BLOCK_INDEX_TIP_OK,
                        "append path (PoS): consistent active suffix must publish; err=" << err);
    BOOST_CHECK_EQUAL(tip.GetTip().record.nMoneySupply, 3000LL);
}

// ---------------------------------------------------------------------------
// Deliberate violations (RED before the gate lands, GREEN with it).
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(si_missing_trail_entry_fail_closed)
{
    const std::string dir = SIMakeTempDir();
    BlockIndexTipAuthority tip;
    BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 1, 0, 0, &tip, NULL));
    std::vector<uint256> ashs; ashs.push_back(uint256(0));
    for (int h = 1; h <= 2; ++h)
    {
        SIAppendActive(tip, uint256(0xE000UL + h), ashs.back(), h, false, 100 * h, 100);
        ashs.push_back(uint256(0xE000UL + h));
    }
    const uint256 b1(0xF001UL);
    BlockIndexRecord rb1; rb1.hash = b1; rb1.hashPrev = ashs[1]; rb1.height = 2;
    rb1.nMint = 100; rb1.nMoneySupply = 300;
    BlockIndexDerivedEntry db1 = SIMakeDerived(6001);

    std::map<int32_t, BlockIndexConnectSupplyEntry> trail;
    BlockIndexConnectSupplyEntry anchor; anchor.supply = 200;  // committed A2
    trail[1] = anchor;   // ONLY the anchor: the branch record has NO trail entry
                         // => a to-be-active height without connected-trail
                         //    accounting is a violation (fail closed).

    std::vector<BlockIndexTipAppend> branch; std::vector<int32_t> heights;
    BlockIndexTipAppend ab1; ab1.record = rb1; ab1.derived = db1;
    branch.push_back(ab1); heights.push_back(2);

    std::string err;
    const BlockIndexTipStatus st = tip.ReorgActiveTo(1, branch, heights, &err, &trail);
    BOOST_CHECK_MESSAGE(st == BLOCK_INDEX_TIP_CORRUPT,
                        "missing connect-trail entry for a to-be-active height must be refused (fail closed); got status "
                        << st << " err=" << err);
    BOOST_CHECK_MESSAGE(tip.GetTip().record.hash == ashs[2],
                        "violation: nothing published; the committed tip must still be A2");
}

BOOST_AUTO_TEST_CASE(si_supply_mismatch_fail_closed)
{
    const std::string dir = SIMakeTempDir();
    BlockIndexTipAuthority tip;
    BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 1, 0, 0, &tip, NULL));
    std::vector<uint256> ashs; ashs.push_back(uint256(0));
    for (int h = 1; h <= 2; ++h)
    {
        SIAppendActive(tip, uint256(0x11000UL + h), ashs.back(), h, false, 100 * h, 100);
        ashs.push_back(uint256(0x11000UL + h));
    }
    const uint256 b1(0x12001UL);
    BlockIndexRecord rb1; rb1.hash = b1; rb1.hashPrev = ashs[1]; rb1.height = 2;
    rb1.nMint = 100;
    rb1.nMoneySupply = 1999;  // INCONSISTENT: trail says 200 + 100 = 300
    BlockIndexDerivedEntry db1 = SIMakeDerived(8001);

    std::map<int32_t, BlockIndexConnectSupplyEntry> trail;
    BlockIndexConnectSupplyEntry anchor; anchor.supply = 200;
    trail[1] = anchor;
    BlockIndexConnectSupplyEntry e1; e1.supply = 300; e1.mint = 100; e1.fees = 0; e1.burned = 0;
    trail[2] = e1;

    std::vector<BlockIndexTipAppend> branch; std::vector<int32_t> heights;
    BlockIndexTipAppend ab1; ab1.record = rb1; ab1.derived = db1;
    branch.push_back(ab1); heights.push_back(2);

    std::string err;
    const BlockIndexTipStatus st = tip.ReorgActiveTo(1, branch, heights, &err, &trail);
    BOOST_CHECK_MESSAGE(st == BLOCK_INDEX_TIP_CORRUPT,
                        "inconsistent active supply (record != parent + mint - fees - burned) must be refused (fail closed); got status "
                        << st << " err=" << err);
    BOOST_CHECK_MESSAGE(err.find("SUPPLY-INVARIANT") != std::string::npos,
                        "violation message must name the supply invariant: " << err);
    BOOST_CHECK_MESSAGE(tip.GetTip().record.hash == ashs[2],
                        "violation: nothing published; the committed tip must still be A2");
}

BOOST_AUTO_TEST_CASE(si_burned_reduces_supply_exactly)
{
    const std::string dir = SIMakeTempDir();
    BlockIndexTipAuthority tip;
    BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 1, 0, 0, &tip, NULL));
    std::vector<uint256> ashs; ashs.push_back(uint256(0));
    for (int h = 1; h <= 2; ++h)
    {
        SIAppendActive(tip, uint256(0x13000UL + h), ashs.back(), h, false, 500 * h, 500);
        ashs.push_back(uint256(0x13000UL + h));
    }
    // Consistent burned case: parent supply 1000, mint 500, fees 50, burned 120
    //   -> supply = 1000 + 500 - 50 - 120 = 1330.
    const uint256 b1(0x14001UL);
    BlockIndexRecord rb1; rb1.hash = b1; rb1.hashPrev = ashs[1]; rb1.height = 2;
    rb1.nMint = 500; rb1.nMoneySupply = 1330;
    BlockIndexDerivedEntry db1 = SIMakeDerived(10001);

    std::map<int32_t, BlockIndexConnectSupplyEntry> trail;
    BlockIndexConnectSupplyEntry anchor; anchor.supply = 1000;
    trail[1] = anchor;
    BlockIndexConnectSupplyEntry e1; e1.supply = 1330; e1.mint = 500; e1.fees = 50; e1.burned = 120;
    trail[2] = e1;

    std::vector<BlockIndexTipAppend> branch; std::vector<int32_t> heights;
    BlockIndexTipAppend ab1; ab1.record = rb1; ab1.derived = db1;
    branch.push_back(ab1); heights.push_back(2);

    std::string err;
    const BlockIndexTipStatus st = tip.ReorgActiveTo(1, branch, heights, &err, &trail);
    BOOST_CHECK_MESSAGE(st == BLOCK_INDEX_TIP_OK,
                        "burned outputs must reduce supply exactly (parent + mint - fees - burned); err=" << err);
    BOOST_CHECK_EQUAL(tip.GetTip().record.nMoneySupply, 1330LL);
}

BOOST_AUTO_TEST_CASE(si_burned_not_honored_fail_closed)
{
    const std::string dir = SIMakeTempDir();
    BlockIndexTipAuthority tip;
    BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 1, 0, 0, &tip, NULL));
    std::vector<uint256> ashs; ashs.push_back(uint256(0));
    for (int h = 1; h <= 2; ++h)
    {
        SIAppendActive(tip, uint256(0x15000UL + h), ashs.back(), h, false, 500 * h, 500);
        ashs.push_back(uint256(0x15000UL + h));
    }
    // Record (and trail supply) claim the UN-burned value 1450 while the trail
    // says burned = 120 => identity broken -> violation, nothing published.
    const uint256 b1(0x16001UL);
    BlockIndexRecord rb1; rb1.hash = b1; rb1.hashPrev = ashs[1]; rb1.height = 2;
    rb1.nMint = 500; rb1.nMoneySupply = 1450;
    BlockIndexDerivedEntry db1 = SIMakeDerived(15001);

    std::map<int32_t, BlockIndexConnectSupplyEntry> trail;
    BlockIndexConnectSupplyEntry anchor; anchor.supply = 1000;
    trail[1] = anchor;
    BlockIndexConnectSupplyEntry e1; e1.supply = 1450; e1.mint = 500; e1.fees = 50; e1.burned = 120;
    trail[2] = e1;

    std::vector<BlockIndexTipAppend> branch; std::vector<int32_t> heights;
    BlockIndexTipAppend ab1; ab1.record = rb1; ab1.derived = db1;
    branch.push_back(ab1); heights.push_back(2);

    std::string err;
    const BlockIndexTipStatus st = tip.ReorgActiveTo(1, branch, heights, &err, &trail);
    BOOST_CHECK_MESSAGE(st == BLOCK_INDEX_TIP_CORRUPT,
                        "un-honored burned in the ACTIVE suffix must be refused; got " << st << " err=" << err);
}

// ---------------------------------------------------------------------------
// Legitimate ZERO values (never a violation by themselves).
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(si_zero_mint_supply_not_flagged)
{
    const std::string dir = SIMakeTempDir();
    BlockIndexTipAuthority tip;
    BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 1, 0, 0, &tip, NULL));
    std::vector<uint256> ashs; ashs.push_back(uint256(0));
    for (int h = 1; h <= 3; ++h)
    {
        SIAppendActive(tip, uint256(0x17000UL + h), ashs.back(), h, false, 100 * h, 100);
        ashs.push_back(uint256(0x17000UL + h));
    }

    // (1) never-connected side records carry mint == supply == 0 BY DESIGN;
    // they STAY SIDE here, so the gate must never flag them.
    const uint256 z1(0x18001UL);
    BlockIndexRecord rz1; rz1.hash = z1; rz1.hashPrev = ashs[1]; rz1.height = 2;
    rz1.nMint = 0; rz1.nMoneySupply = 0;
    BlockIndexDerivedEntry dz1 = SIMakeDerived(13001);
    {
        BlockIndexTipAppend az; az.record = rz1; az.derived = dz1;
        std::string errS;
        BOOST_REQUIRE_MESSAGE(tip.Append(az, -1, &errS) == BLOCK_INDEX_TIP_OK, errS);
    }

    // (2) a to-be-ACTIVE branch block with legitimate ZERO mint (fees=0,
    // burned=0, nothing minted): identity supply == parent supply holds.
    const uint256 b1(0x19001UL);
    BlockIndexRecord rb1; rb1.hash = b1; rb1.hashPrev = ashs[1]; rb1.height = 2;
    rb1.nMint = 0; rb1.nMoneySupply = 300;   // exactly the parent supply
    BlockIndexDerivedEntry db1 = SIMakeDerived(21001);

    std::map<int32_t, BlockIndexConnectSupplyEntry> trail;
    BlockIndexConnectSupplyEntry anchor; anchor.supply = 300;  // committed A3
    trail[1] = anchor;
    BlockIndexConnectSupplyEntry e1; e1.supply = 300; e1.mint = 0; e1.fees = 0; e1.burned = 0;
    trail[2] = e1;

    std::vector<BlockIndexTipAppend> branch; std::vector<int32_t> heights;
    BlockIndexTipAppend ab1; ab1.record = rb1; ab1.derived = db1;
    branch.push_back(ab1); heights.push_back(2);

    std::string err;
    const BlockIndexTipStatus st = tip.ReorgActiveTo(1, branch, heights, &err, &trail);
    BOOST_CHECK_MESSAGE(st == BLOCK_INDEX_TIP_OK,
                        "legitimate zero mint/supply (side Z1 untouched, active B1 with zero mint) must NOT be flagged; err=" << err);
    BOOST_CHECK_EQUAL(tip.GetTip().record.nMoneySupply, 300LL);

    std::string errZ;
    BlockIndexTipRead z = tip.LookupByHash(z1, &errZ);
    BOOST_REQUIRE(z.status == BLOCK_INDEX_TIP_OK);
    BOOST_CHECK(!z.active);
    BOOST_CHECK_EQUAL(z.record.nMoneySupply, 0LL);
}

// ---------------------------------------------------------------------------
// PROMOTE path: the committed side branch (zero mint/supply, never connected)
// becomes active and must be validated against the corrected connect trail.
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(si_promote_path_invariant_ok)
{
    const std::string dir = SIMakeTempDir();
    BlockIndexTipAuthority tip;
    BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 1, 0, 0, &tip, NULL));
    std::vector<uint256> ashs; ashs.push_back(uint256(0));
    for (int h = 1; h <= 2; ++h)
    {
        SIAppendActive(tip, uint256(0x1A000UL + h), ashs.back(), h, false, 1000 * h, 1000);
        ashs.push_back(uint256(0x1A000UL + h));
    }

    // Competing branch, recorded as SIDE with the STALE zero supply/mint
    // (never connected — exactly the condition the MS-FIX override heals).
    const uint256 b1(0x1B001UL), b2(0x1B002UL);
    BlockIndexRecord sb1 = SIMakeRecord(b1, ashs[1], 2, false);
    sb1.nMint = 0; sb1.nMoneySupply = 0;
    BlockIndexDerivedEntry db1 = SIMakeDerived(15001);
    BlockIndexRecord sb2 = SIMakeRecord(b2, b1, 3, false);
    sb2.nMint = 0; sb2.nMoneySupply = 0;
    BlockIndexDerivedEntry db2 = SIMakeDerived(15002);
    {
        BlockIndexTipAppend a; a.record = sb1; a.derived = db1;
        std::string errS; BOOST_REQUIRE_MESSAGE(tip.Append(a, -1, &errS) == BLOCK_INDEX_TIP_OK, errS);
    }
    {
        BlockIndexTipAppend a; a.record = sb2; a.derived = db2;
        std::string errS; BOOST_REQUIRE_MESSAGE(tip.Append(a, -1, &errS) == BLOCK_INDEX_TIP_OK, errS);
    }

    // The reorg connect produced the authoritative active-chain cumulative for
    // these SAME heights (the branch records arriving at ReorgActiveTo carry
    // the corrected values, per the cutover override):
    //   supply(B1) = committed A2 (2000) + 300 - 40 - 0      = 2260
    //   supply(B2) = 2260 + 150 - 50 - 30                    = 2330
    BlockIndexRecord nb1 = sb1; nb1.nMint = 300; nb1.nMoneySupply = 2260;
    BlockIndexRecord nb2 = sb2; nb2.nMint = 150; nb2.nMoneySupply = 2330;

    std::map<int32_t, BlockIndexConnectSupplyEntry> trail;
    BlockIndexConnectSupplyEntry anchor; anchor.supply = 2000;  // committed fork A2
    trail[1] = anchor;
    BlockIndexConnectSupplyEntry e1; e1.supply = 2260; e1.mint = 300; e1.fees = 40; e1.burned = 0;
    trail[2] = e1;
    BlockIndexConnectSupplyEntry e2; e2.supply = 2330; e2.mint = 150; e2.fees = 50; e2.burned = 30;
    trail[3] = e2;

    std::vector<BlockIndexTipAppend> branch; std::vector<int32_t> heights;
    BlockIndexTipAppend ab1; ab1.record = nb1; ab1.derived = db1;
    BlockIndexTipAppend ab2; ab2.record = nb2; ab2.derived = db2;
    branch.push_back(ab1); branch.push_back(ab2);
    heights.push_back(2); heights.push_back(3);

    std::string err;
    const BlockIndexTipStatus st = tip.ReorgActiveTo(1, branch, heights, &err, &trail);
    BOOST_CHECK_MESSAGE(st == BLOCK_INDEX_TIP_OK,
                        "promote path: reorg-supplied corrected supply must satisfy the identity against the committed fork anchor; err=" << err);
    BOOST_CHECK_MESSAGE(tip.GetTip().record.hash == b2,
                        "promote path: the promoted branch must become the published tip");
    BOOST_CHECK_EQUAL(tip.GetTip().record.nMoneySupply, 2330LL);
}

// ---------------------------------------------------------------------------
// Released-edge reconnect carry (the exact ConnectBlock contract, unit level).
// ---------------------------------------------------------------------------

// Real S>0 authoritative generation built synthetically (deterministic, no
// ambient mining), booted through the REAL production startup. Mirrors the
// window2 fixture family; all symbols here are SI-prefixed and file-static.
struct SIGenFixture
{
    fs::path root;
    int S;
    std::vector<uint256> active;
    CBlockIndex* sBest; CBlockIndex* sGen; uint256 sHash; int sH; uint256 sT;
    std::set<uint256> sInvalid;

    explicit SIGenFixture(int s)
        : S(s), sBest(NULL), sGen(NULL), sH(-1)
    {
        // Test-harness isolation (V2 MS-FIX3): the process-wide global fixture
        // (TestingSetup, test_innova.cpp) already booted a REAL authoritative
        // context through LoadBlockIndex() -> InitBlockIndexAuthoritative(). A
        // second init refuses with "authoritative startup: already
        // initialized", so tear the global down first and let THIS case own an
        // isolated context on its own temp root. The saved globals are
        // re-captured AFTER the reset so the destructor restores the clean
        // empty state (never pointers owned by an already-freed context).
        ResetBlockIndexAuthoritativeStartupForTest();
        sBest = pindexBest; sGen = pindexGenesisBlock;
        sHash = hashBestChain; sH = nBestHeight; sT = nBestChainTrust;
        sInvalid = setInvalidBlockHash;
        root = fs::temp_directory_path() / fs::unique_path("sigf-%%%%-%%%%");
        std::string error;
        BOOST_REQUIRE_MESSAGE(SIBuildAndInit(root, S, &active, &error), error);
        BOOST_REQUIRE(g_fAuthoritativeStartup);
    }
    ~SIGenFixture()
    {
        ResetBlockIndexAuthoritativeStartupForTest();
        pindexBest = sBest; pindexGenesisBlock = sGen;
        hashBestChain = sHash; nBestHeight = sH; nBestChainTrust = sT;
        setInvalidBlockHash = sInvalid;
        try { fs::remove_all(root); } catch (...) {}
    }

private:
    static bool SIBuildAndInit(const fs::path& root, int S,
                               std::vector<uint256>* outActive, std::string* error)
    {
        fs::create_directories(root / "blocks");
        std::vector<uint256> active;
        uint256 prev(0);
        BlockIndexGenerationSource src;
        for (int h = 0; h <= S; ++h)
        {
            // minimal real PoW block + file entry (same shape as the window2
            // synthetic-generation fixture; deterministic, no ambient mining).
            CTransaction coinbase; coinbase.nVersion = 1; coinbase.nTime = 1700000000u + (unsigned)h;
            CTxIn input; input.prevout = COutPoint(uint256(0), 0xffffffff);
            input.scriptSig = CScript() << OP_TRUE; input.nSequence = 0xffffffff;
            coinbase.vin.push_back(input);
            CTxOut output; output.nValue = 0; output.scriptPubKey = CScript() << OP_TRUE;
            coinbase.vout.push_back(output);
            CBlock block; block.nVersion = 1; block.hashPrevBlock = prev;
            block.nTime = 1700000000u + (unsigned)h; block.nBits = 0x1d00ffffU;
            block.nNonce = (unsigned)h;
            block.vtx.push_back(coinbase); block.hashMerkleRoot = block.BuildMerkleTree();
            const uint256 bh = block.GetHash();
            CDataStream ss(SER_DISK, CLIENT_VERSION); ss << block;
            const unsigned int ns = ss.size();
            FILE* fp = fopen((root / "blocks" / "blk0001.dat").string().c_str(), "ab");
            BOOST_REQUIRE(fp != NULL);
            unsigned char magic[] = {0xfa,0xbf,0xb5,0xda};
            fwrite(magic,1,4,fp);
            fwrite(&ns,4,1,fp);
            const long bi = ftell(fp);
            fwrite(&ss[0],1,ss.size(),fp);
            fflush(fp); fclose(fp);

            BlockIndexRecord rec;
            rec.hash = bh; rec.hashPrev = prev; rec.height = h;
            rec.nVersion = 1; rec.nTime = 1700000000u + (unsigned)h;
            rec.nBits = 0x1d00ffffU; rec.nNonce = (unsigned)h;
            rec.nFile = 1; rec.nBlockPos = (unsigned)bi;
            rec.nFlags = 0;
            rec.nMoneySupply = 777000;   // non-zero cumulative, stamped once
            rec.nMint = 0;
            BlockIndexGenerationSourceRecord sr; sr.hash = rec.hash; sr.record = rec;
            src.records.push_back(sr);
            active.push_back(rec.hash);
            prev = rec.hash;
        }
        src.hashBestChain = active[S];
        src.foundBestChain = true;
        src.blockDataDir = (root / "blocks").string();
        BlockIndexGenerationBuilder b;
        if (!b.Build(src, (root / "build-000001.tmp").string(), 1, NULL, error)) return false;
        b.Close();
        if (BlockIndexGenerationManager::PublishGeneration(root.string(), 1, error) != BLOCK_INDEX_LIFECYCLE_OK) return false;
        if (BlockIndexGenerationManager::SelectGeneration(root.string(), 1, error) != BLOCK_INDEX_LIFECYCLE_OK) return false;
        if (outActive) *outActive = active;
        return InitBlockIndexAuthoritative(root.string(), error);
    }
};

BOOST_AUTO_TEST_CASE(si_carry_resident_parent_uses_precomputed_supply)
{
    // (a) resident pprev path (the common connect case).
    CBlockIndex parent; parent.nMoneySupply = 770077;
    CBlockIndex child; child.pprev = &parent; child.nHeight = 42;
    int64_t supply = 0; std::string err;
    BOOST_CHECK(ResolveConnectParentSupplyForTesting(&child, supply, err));
    BOOST_CHECK_EQUAL(supply, 770077LL);

    // (b) legacy mode (not armed): a released edge without identity is
    // tolerated exactly as the pre-fix semantics were.
    //
    // Test-harness isolation (V2 MS-FIX3): the process-wide global fixture boots
    // the REAL authoritative context, which would otherwise route this
    // identity-less released edge into the armed-V2 refusal. The LEGACY
    // tolerance branch is the contract under test, so the armed flag is
    // suspended for the scope below and restored on exit (harness state
    // isolation only; no production check is touched).
    struct SILegacyAuthScope
    {
        const bool saved;
        SILegacyAuthScope() : saved(g_fAuthoritativeStartup) {}
        ~SILegacyAuthScope() { g_fAuthoritativeStartup = saved; }
    } legacyModeScope;
    g_fAuthoritativeStartup = false;
    CBlockIndex childB; childB.pprev = NULL; childB.nHeight = 7;
    BOOST_CHECK(ResolveConnectParentSupplyForTesting(&childB, supply, err));
    BOOST_CHECK_EQUAL(supply, 0LL);
}

BOOST_AUTO_TEST_CASE(si_carry_released_edge_resolves_from_authoritative_store)
{
    const int S = 6;
    SIGenFixture fx(S);
    // Released-edge reconnect contract: pprev == NULL, armed-V2, the parent is
    // a published base-generation block whose record carries 777000.
    CBlockIndex child; child.pprev = NULL; child.pskip = NULL;
    child.nHeight = S + 1;
    child.phashBlock = new uint256(0x52039001UL);
    child.hashPrevStable = fx.active[S];
    int64_t supply = 0; std::string err;
    const bool ok = ResolveConnectParentSupplyForTesting(&child, supply, err);
    BOOST_CHECK_MESSAGE(ok,
        "released-edge parent must resolve from the authoritative live store; err=" << err);
    BOOST_CHECK_MESSAGE(supply == 777000LL,
        "the resolved parent supply must be the authoritative record's cumulative (777000); got " << supply);
    delete child.phashBlock;
}

BOOST_AUTO_TEST_CASE(si_carry_released_edge_unresolvable_fails_closed)
{
    const int S = 6;
    SIGenFixture fx(S);
    CBlockIndex child; child.pprev = NULL; child.pskip = NULL;
    child.nHeight = S + 7;
    child.phashBlock = new uint256(0x5203A001UL);
    child.hashPrevStable = uint256(0xDEADBEEFUL); // published NOWHERE
    int64_t supply = -1; std::string err;
    const bool ok = ResolveConnectParentSupplyForTesting(&child, supply, err);
    BOOST_CHECK_MESSAGE(!ok, "an unresolvable released-edge ancestor must fail closed");
    BOOST_CHECK_MESSAGE(!err.empty(), "the failure must carry a precise message");
    BOOST_CHECK_EQUAL(supply, 0LL);
    delete child.phashBlock;
}

BOOST_AUTO_TEST_CASE(si_carry_released_edge_no_identity_fails_closed)
{
    // THE GAP FIXED IN THIS PHASE: a non-genesis released edge in armed-V2 with
    // NO parent identity must fail closed (it used to silently assume parent 0
    // and re-base the accumulator).
    const int S = 6;
    SIGenFixture fx(S);
    CBlockIndex child; child.pprev = NULL; child.pskip = NULL;
    child.nHeight = 4;
    child.phashBlock = new uint256(0x5203B001UL);
    child.hashPrevStable = uint256(0);
    int64_t supply = -1; std::string err;
    const bool ok = ResolveConnectParentSupplyForTesting(&child, supply, err);
    BOOST_CHECK_MESSAGE(!ok, "an armed-V2 released edge without parent identity must fail closed");
    BOOST_CHECK_MESSAGE(err.find("refus") != std::string::npos,
        "the failure must say the re-base is refused: " << err);
    BOOST_CHECK_EQUAL(supply, 0LL);
    delete child.phashBlock;
}

// MS-ANCHOR (2026-11-08): the gap that allowed the observed mainnet re-base even
// WITH a resident parent object. A parent object that never went through
// ConnectBlock (a never-connected side object, a stale/duplicate twin) carries
// nMoneySupply == 0 and nSupplyAuthority == 0. It must NEVER be used as a carry
// source: the resolver has to read the authoritative record instead.
BOOST_AUTO_TEST_CASE(si_carry_unverified_resident_parent_uses_authoritative_record)
{
    const int S = 6;
    SIGenFixture fx(S);
    uint256 parentHash = fx.active[S];      // published record carries 777000
    CBlockIndex parent;                     // NEVER connected: accumulator 0, unmarked
    parent.phashBlock = &parentHash;
    parent.nHeight = S;
    parent.nMoneySupply = 0;
    BOOST_REQUIRE_EQUAL(parent.nSupplyAuthority, 0);

    CBlockIndex child;
    child.pprev = &parent;
    child.nHeight = S + 1;
    int64_t supply = -1;
    std::string err;
    const bool ok = ResolveConnectParentSupplyForTesting(&child, supply, err);
    BOOST_CHECK_MESSAGE(ok, "an unverified resident parent must resolve from the record; err=" << err);
    BOOST_CHECK_MESSAGE(supply == 777000LL,
        "the carry must be the authoritative record's cumulative (777000), never the "
        "uninitialized in-memory zero; got " << supply);
}

// A resident/twin parent that is NOT an authoritative carry source has its
// in-memory value IGNORED: the persisted record wins (that is exactly how a
// stale/duplicate object can no longer re-base the accumulator).
BOOST_AUTO_TEST_CASE(si_carry_unverified_resident_parent_value_is_ignored)
{
    const int S = 6;
    SIGenFixture fx(S);
    uint256 parentHash = fx.active[S];
    CBlockIndex parent;
    parent.phashBlock = &parentHash;
    parent.nHeight = S;
    parent.nMoneySupply = 5; // stale/duplicate object, unmarked
    CBlockIndex child;
    child.pprev = &parent;
    child.nHeight = S + 1;
    int64_t supply = -1;
    std::string err;
    BOOST_CHECK(ResolveConnectParentSupplyForTesting(&child, supply, err));
    BOOST_CHECK_MESSAGE(supply == 777000LL,
                        "the record must win over an unverified in-memory value; got " << supply);
}

// An unverified resident parent whose block is NOT in the authoritative store
// cannot be resolved at all: fail closed (never assume zero).
BOOST_AUTO_TEST_CASE(si_carry_unverified_resident_parent_unresolvable_fails_closed)
{
    const int S = 6;
    SIGenFixture fx(S);
    uint256 unknownHash(0x5203D00DUL);
    CBlockIndex parent;
    parent.phashBlock = &unknownHash;
    parent.nHeight = S;
    parent.nMoneySupply = 0;
    CBlockIndex child;
    child.pprev = &parent;
    child.nHeight = S + 1;
    int64_t supply = -1;
    std::string err;
    BOOST_CHECK(!ResolveConnectParentSupplyForTesting(&child, supply, err));
    BOOST_CHECK(!err.empty());
    BOOST_CHECK_EQUAL(supply, 0LL);
}

// The forward-sync fast path: a VERIFIED parent object (one whose accumulator
// came from the authoritative record / a connect against it) is used directly,
// with no store read and no behavioural change.
BOOST_AUTO_TEST_CASE(si_carry_verified_resident_parent_is_trusted_directly)
{
    CBlockIndex parent;
    parent.phashBlock = NULL;
    parent.nHeight = 41;
    parent.nMoneySupply = 770077;
    parent.nSupplyAuthority = 1;
    CBlockIndex child;
    child.pprev = &parent;
    child.nHeight = 42;
    int64_t supply = 0;
    std::string err;
    BOOST_CHECK(ResolveConnectParentSupplyForTesting(&child, supply, err));
    BOOST_CHECK_EQUAL(supply, 770077LL);
}

BOOST_AUTO_TEST_SUITE_END()
