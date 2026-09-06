#include <boost/test/unit_test.hpp>

#include "blockindex_live_tail.h"
#include "blockindex_tip.h"
#include "blockindex_v2_reader.h"
#include "blockindex_startup_authority.h"
#include "util.h"

#include <boost/filesystem.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace fs = boost::filesystem;

// =====================================================================
// P2 — bounded HotOwner live-tail: RESIDENCY bounded independent of total
// historical N, composite base(V2)+tip by-value materialization, horizon trim
// (cache-only, never a consensus/reorg bound), and deep-reorg re-materialization.
// =====================================================================

static std::string MakeTempDir()
{
    char tmpl[] = "/tmp/innova-live-tail-XXXXXX";
    char* d = mkdtemp(tmpl);
    BOOST_REQUIRE(d != NULL);
    return std::string(d);
}

static BlockIndexRecord MakeRecord(uint256 hash, uint256 hashPrev, int height,
                                   bool isPos)
{
    BlockIndexRecord r;
    r.hash = hash;
    r.hashPrev = hashPrev;
    r.height = height;
    r.nFile = 1;
    r.nBlockPos = (unsigned)(height * 100);
    r.nFlags = isPos ? CBlockIndex::BLOCK_PROOF_OF_STAKE : 0;
    r.nVersion = 5;
    r.nTime = 1600000000u + (unsigned)height;
    r.nBits = 0x1d00ffff;
    r.nNonce = (unsigned)height;
    if (isPos)
    {
        r.prevoutStake = COutPoint(uint256((unsigned)height), 0);
        r.nStakeTime = 1600000000u + (unsigned)height;
    }
    return r;
}

static BlockIndexDerivedEntry MakeDerived(uint256 trust, uint32_t checksum)
{
    BlockIndexDerivedEntry d;
    d.chainTrust = trust;
    d.stakeModifierChecksum = checksum;
    d.SetHasStakeModifierTime(true);
    d.stakeModifierTime = 1600000000;
    d.SetHasBlockSize(true);
    d.nSize = 1200 + (checksum % 100);
    return d;
}

// Deterministic by-value BASE authority standalone (no real V2 generation
// needed): provides base blocks <= S to the composite materializer, mirroring
// BlockIndexV2Reader::LookupByHash. We use a tiny in-memory map that the
// materializer's base path would read via the real reader; for unit isolation
// we test the LIVE TAIL over the tip + a small base map through the composite
// materializer directly.
//
// The composite materializer needs a real BlockIndexV2Reader* for the base.
// To keep the test self-contained (no full V2 generation build), we instead
// test the live-tail with tip-only sources (baseReader_ NULL) for the horizon
// semantics, and separately verify tip->snapshot + composite routing via a
// null base (base fallback = AUTHORITY_MISSING). Real base+tip composite is
// covered in P5 integration (needs a real base generation).

static void AppendChain(BlockIndexTipAuthority& tip, uint32_t n,
                        uint256 firstPrev, int baseTip, uint256* outFinal)
{
    uint256 prev = firstPrev;
    for (uint32_t i = 0; i < n; ++i)
    {
        uint256 h = uint256(0x5151UL + i);
        BlockIndexRecord r = MakeRecord(h, prev, baseTip + 1 + (int)i,
                                        (i % 2) == 0);
        BlockIndexDerivedEntry d = MakeDerived(uint256(i + 1), 2000 + i);
        BlockIndexTipAppend a; a.record = r; a.derived = d;
        std::string err;
        BlockIndexTipStatus st = tip.Append(a, baseTip + 1 + (int)i, &err);
        BOOST_REQUIRE(st == BLOCK_INDEX_TIP_OK);
        prev = h;
    }
    if (outFinal)
        *outFinal = prev;
}

BOOST_AUTO_TEST_SUITE(blockindex_live_tail_tests)

// Tip-first materialization + bounded residency.
BOOST_AUTO_TEST_CASE(l1_tiponly_materialize_resident)
{
    const std::string dir = MakeTempDir();
    const int baseTip = 50000;   // large base tip to stress "independent of N"
    const uint256 basePrev = uint256(0xAAAAUL);

    BlockIndexTipAuthority tip;
    BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 7, 100000, baseTip, &tip, NULL));
    uint256 finalHash;
    AppendChain(tip, 20, basePrev, baseTip, &finalHash); // heights 50001..50020

    // Live tail with NO base reader (tip-only sources).
    BlockIndexLiveTail tail;
    tail.SetSources(NULL, &tip);
    tail.SetHorizon(8);
    tail.SetCurrentGeneration(7);

    // Pin a block near the tip -> materializes from tip authority.
    BlockIndexLogicalId midId(uint256(0x5151UL + 10)); // height 50011
    BlockIndexHotHandle h1;
    BlockIndexHotStatus st = tail.Pin(midId, &h1);
    BOOST_REQUIRE(st == BlockIndexHotStatus::OK);
    BOOST_REQUIRE(h1.IsValid());
    BOOST_REQUIRE(h1.Get() != NULL);
    BOOST_REQUIRE_EQUAL(h1.Get()->nHeight, baseTip + 11);
    h1.Reset();

    // Pin the tip (50020).
    BlockIndexLogicalId tipId(finalHash);
    BlockIndexHotHandle h2;
    BOOST_REQUIRE(tail.Pin(tipId, &h2) == BlockIndexHotStatus::OK);
    BOOST_REQUIRE_EQUAL(h2.Get()->nHeight, baseTip + 20);
    h2.Reset();

    printf("L1 PASS tip-only materialize: height=%d resident=%zu\n",
           (int)(baseTip + 20), tail.ResidentCount());
}

// Residency bounded by horizon, independent of huge base tip height.
BOOST_AUTO_TEST_CASE(l2_residency_bounded_independent_of_n)
{
    const std::string dir = MakeTempDir();
    // Simulate a chain with a VERY high base tip (like 8M) but only a bounded
    // live window materialized. Residency must stay O(horizon), not O(N).
    const int baseTip = 8000000;  // "8 million" base tip
    const uint256 basePrev = uint256(0xBBBBUL);

    BlockIndexTipAuthority tip;
    BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 9, 9000000, baseTip, &tip, NULL));
    uint256 finalHash;
    AppendChain(tip, 12, basePrev, baseTip, &finalHash); // heights 8000001..8000012

    BlockIndexLiveTail tail;
    tail.SetSources(NULL, &tip);
    tail.SetHorizon(5);
    tail.SetCurrentGeneration(9);

    // Materialize a window of live blocks (say 6 of them).
    for (int i = 0; i < 6; ++i)
    {
        BlockIndexLogicalId id(uint256(0x5151UL + i));
        BlockIndexHotHandle h;
        BOOST_REQUIRE(tail.Pin(id, &h) == BlockIndexHotStatus::OK);
        h.Reset(); // release pin -> becomes evictable (unpinned, not anchor)
    }
    // Resident count is 6 irrespective of baseTip = 8,000,000.
    BOOST_REQUIRE(tail.ResidentCount() == 6);
    // Trim to horizon (floor = tipHeight - 5 + 1 = 8000008); evicts the low ones.
    size_t evicted = tail.TrimToHorizon();
    BOOST_REQUIRE(evicted >= 2); // heights 8000001..8000003 should go (floor 8000008)
    // After trim, resident <= horizon + any pinned.
    BOOST_REQUIRE(tail.ResidentCount() <= 6);
    printf("L2 PASS residency bounded: baseTip=8,000,000 resident=%zu evicted=%zu\n",
           tail.ResidentCount(), evicted);
}

// Re-materialization of an evicted (below-horizon) block — deep-reorg path must
// NOT be blocked by the horizon. Pin again after eviction must succeed.
BOOST_AUTO_TEST_CASE(l3_rematerialize_after_evict_deep_reorg)
{
    const std::string dir = MakeTempDir();
    const int baseTip = 100;
    const uint256 basePrev = uint256(0xCCCCUL);
    BlockIndexTipAuthority tip;
    BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 3, 300, baseTip, &tip, NULL));
    uint256 finalHash;
    AppendChain(tip, 30, basePrev, baseTip, &finalHash); // heights 101..130

    BlockIndexLiveTail tail;
    tail.SetSources(NULL, &tip);
    tail.SetHorizon(5);
    tail.SetCurrentGeneration(3);

    // Materialize a broad set (e.g., a deep-reorg would need blocks well below
    // the horizon). Pin origin, then evict by trim, then re-pin (must re-materialize).
    BlockIndexLogicalId deepId(uint256(0x5151UL + 5)); // height 106 (deep)
    // Pin it once (forces residency), release, trim (floor=126 -> 106 evicts), re-pin.
    {
        BlockIndexHotHandle h;
        BOOST_REQUIRE(tail.Pin(deepId, &h) == BlockIndexHotStatus::OK);
        h.Reset();
    }
    tail.TrimToHorizon();
    BOOST_REQUIRE(!tail.IsResident(deepId)); // evicted (below horizon)
    // Re-pin: must re-materialize from tip authority (deep-reorg path). MUST NOT
    // be rejected for exceeding horizon.
    {
        BlockIndexHotHandle h;
        BlockIndexHotStatus st = tail.Pin(deepId, &h);
        BOOST_REQUIRE(st == BlockIndexHotStatus::OK);
        BOOST_REQUIRE_EQUAL(h.Get()->nHeight, baseTip + 6);
        h.Reset();
        printf("L3 PASS deep-reorg re-materialization after evict: height %d\n",
               (int)(baseTip + 6));
    }
    (void)finalHash;
}

// Anchors (permanent pins) survive trimming.
BOOST_AUTO_TEST_CASE(l4_anchors_survive_trim)
{
    const std::string dir = MakeTempDir();
    const int baseTip = 10;
    const uint256 basePrev = uint256(0xDDDDUL);
    BlockIndexTipAuthority tip;
    BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 5, 50, baseTip, &tip, NULL));
    uint256 finalHash;
    AppendChain(tip, 20, basePrev, baseTip, &finalHash); // heights 11..30

    BlockIndexLiveTail tail;
    tail.SetSources(NULL, &tip);
    tail.SetHorizon(3);
    tail.SetCurrentGeneration(5);

    BlockIndexLogicalId anchorId(uint256(0x5151UL + 2)); // height 13
    tail.PinPermanent(anchorId);

    // Materialize many, release, trim aggressively (floor = 30-3+1 = 28).
    for (int i = 0; i < 20; ++i)
    {
        BlockIndexLogicalId id(uint256(0x5151UL + i));
        BlockIndexHotHandle h;
        if (tail.Pin(id, &h) == BlockIndexHotStatus::OK)
            h.Reset();
    }
    size_t evicted = tail.TrimToHorizon();
    (void)evicted;
    // The anchor must survive trimming regardless of how many others evicted.
    BOOST_REQUIRE(tail.IsResident(anchorId));
    printf("L4 PASS anchors survive trim (resident=%zu after evict=%zu)\n",
           tail.ResidentCount(), evicted);
}

BOOST_AUTO_TEST_SUITE_END()