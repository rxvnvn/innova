#include <boost/test/unit_test.hpp>

#include "blockindex_tip.h"
#include "util.h"

#include <boost/filesystem.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <openssl/sha.h>
#include <string>
#include <vector>

namespace fs = boost::filesystem;

// =====================================================================
// P1 — blockindex_tip: dedicated persistent mutable tip authority.
//
// Causal gates:
//   T1  create empty + reopen -> empty, base-binding correct
//   T2  append one active block -> tip advances; reopen recovers exact state
//   T3  sequential chain append + reopen -> records/derived/active recovered
//   T4  idempotent re-append (replay) -> no duplicate, tip unchanged
//   T5  non-dense active append -> rejected CORRUPT
//   T6  base-generation binding fail-closed (wrong gen -> Open fails)
//   T7  TruncateActiveTo reorg: tip shrinks, fence bumps, records kept, reopen
//   T8  crash-mid-append: stores ahead of tip.meta -> truncated to committed tip
//   T9  side branch (activeHeight=-1) -> recorded but not active
//   T10 lookups (GetTip/byHash/byHeight/parent/next)
// =====================================================================

static std::string MakeTempDir()
{
    char tmpl[] = "/tmp/innova-tiptest-XXXXXX";
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

// Append nActive contiguous ACTIVE blocks, starting at global height
// baseTip+1 with given firstPrev (base tip hash). Each block hash is unique.
static int32_t BuildChain(BlockIndexTipAuthority& tip, uint32_t n,
                          uint256* outFinalHash, uint256 firstPrev, int baseTip)
{
    uint256 prev = firstPrev;
    for (uint32_t i = 0; i < n; ++i)
    {
        uint256 h = uint256(0x1111UL + i);
        BlockIndexRecord r = MakeRecord(h, prev, baseTip + 1 + (int)i,
                                        (i % 2) == 0);
        BlockIndexDerivedEntry d = MakeDerived(uint256(i + 1), 1000 + i);
        BlockIndexTipAppend a; a.record = r; a.derived = d;
        std::string err;
        BlockIndexTipStatus st = tip.Append(a, baseTip + 1 + (int)i, &err);
        BOOST_REQUIRE(st == BLOCK_INDEX_TIP_OK);
        prev = h;
    }
    if (outFinalHash)
        *outFinalHash = prev;
    return (int32_t)n;
}

BOOST_AUTO_TEST_SUITE(blockindex_tip_tests)

BOOST_AUTO_TEST_CASE(t1_create_empty_reopen)
{
    const std::string dir = MakeTempDir();
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 7, 1000, 999, &tip, NULL));
        BOOST_REQUIRE(tip.IsOpen());
        BOOST_REQUIRE(tip.IsEmpty());
        BOOST_REQUIRE_EQUAL(tip.BaseGeneration(), 7u);
        BOOST_REQUIRE_EQUAL(tip.BaseRecordCount(), 1000u);
        BOOST_REQUIRE_EQUAL(tip.TipHeight(), 999); // empty tip == base tip S
        BlockIndexTipRead r = tip.GetTip();
        BOOST_REQUIRE(r.status == BLOCK_INDEX_TIP_NOT_FOUND);
        printf("T1 PASS create+empty\n");
    }
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE(BlockIndexTipAuthority::Open(dir, 7, &tip, NULL));
        BOOST_REQUIRE(tip.IsOpen());
        BOOST_REQUIRE(tip.IsEmpty());
        BOOST_REQUIRE_EQUAL(tip.BaseGeneration(), 7u);
        printf("T1 PASS reopen\n");
    }
}

BOOST_AUTO_TEST_CASE(t2_append_single_reopen)
{
    const std::string dir = MakeTempDir();
    const int baseTip = 500;
    const uint256 basePrev = uint256(0x1234UL);
    uint256 tipHash;
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 3, 700, baseTip, &tip, NULL));
        BuildChain(tip, 1, &tipHash, basePrev, baseTip);
        BOOST_REQUIRE_EQUAL(tip.TipHeight(), baseTip + 1);
        BOOST_REQUIRE_EQUAL(tip.TipRecordCount(), 1u);
        BOOST_REQUIRE(!tip.IsEmpty());
        printf("T2 PASS append single: tip=%d\n", (int)tip.TipHeight());
    }
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE(BlockIndexTipAuthority::Open(dir, 3, &tip, NULL));
        BOOST_REQUIRE_EQUAL(tip.TipHeight(), baseTip + 1);
        BOOST_REQUIRE_EQUAL(tip.TipRecordCount(), 1u);
        BOOST_REQUIRE(tip.TipHash() == tipHash);
        printf("T2 PASS reopen: tip hash recovered\n");
    }
}

BOOST_AUTO_TEST_CASE(t3_chain_reopen)
{
    const std::string dir = MakeTempDir();
    const int baseTip = 499;
    const uint256 basePrev = uint256(0x7777UL);
    uint256 tipHash;
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 5, 900, baseTip, &tip, NULL));
        int32_t n = BuildChain(tip, 5, &tipHash, basePrev, baseTip);
        BOOST_REQUIRE_EQUAL(n, 5);
        BOOST_REQUIRE_EQUAL(tip.TipHeight(), baseTip + 5);
        BOOST_REQUIRE_EQUAL(tip.TipRecordCount(), 5u);
        printf("T3 PASS chain append: tip=%d\n", (int)tip.TipHeight());
    }
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE(BlockIndexTipAuthority::Open(dir, 5, &tip, NULL));
        BOOST_REQUIRE_EQUAL(tip.TipHeight(), baseTip + 5);
        BOOST_REQUIRE_EQUAL(tip.TipRecordCount(), 5u);
        BOOST_REQUIRE(tip.TipHash() == tipHash);
        printf("T3 PASS reopen: chain recovered to height %d\n", (int)tip.TipHeight());
    }
}

BOOST_AUTO_TEST_CASE(t4_idempotent_reappend)
{
    const std::string dir = MakeTempDir();
    const int baseTip = 10;
    const uint256 basePrev = uint256(0x55UL);
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 1, 30, baseTip, &tip, NULL));
        uint256 finalHash;
        BuildChain(tip, 3, &finalHash, basePrev, baseTip); // heights 11,12,13
        BOOST_REQUIRE_EQUAL(tip.TipHeight(), 13);
        BOOST_REQUIRE_EQUAL(tip.TipRecordCount(), 3u);
        // replay exact last block (height 13, hash 0x1113) -> idempotent no-op
        {
            BlockIndexRecord r = MakeRecord(uint256(0x1113UL), uint256(0x1112UL), 13, true);
            BlockIndexDerivedEntry d = MakeDerived(uint256(3), 1002);
            BlockIndexTipAppend a; a.record = r; a.derived = d;
            std::string err;
            BOOST_REQUIRE(tip.Append(a, 13, &err) == BLOCK_INDEX_TIP_OK);
            BOOST_REQUIRE_EQUAL(tip.TipRecordCount(), 3u);
            BOOST_REQUIRE_EQUAL(tip.TipHeight(), 13);
        }
        printf("T4 PASS idempotent re-append\n");
    }
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE(BlockIndexTipAuthority::Open(dir, 1, &tip, NULL));
        BOOST_REQUIRE_EQUAL(tip.TipRecordCount(), 3u);
        printf("T4 PASS reopen after idempotent\n");
    }
}

BOOST_AUTO_TEST_CASE(t5_nondense_active_rejected)
{
    const std::string dir = MakeTempDir();
    const int baseTip = 5;
    BlockIndexTipAuthority tip;
    BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 2, 30, baseTip, &tip, NULL));
    uint256 prev = uint256(0x11UL);
    BlockIndexRecord r = MakeRecord(uint256(0x44UL), prev, 21, false);
    BlockIndexDerivedEntry d = MakeDerived(uint256(1), 1);
    BlockIndexTipAppend a; a.record = r; a.derived = d;
    std::string err;
    // activeIds empty, next expected = baseTip+1 = 6; we pass 21 -> CORRUPT
    BlockIndexTipStatus st = tip.Append(a, 21, &err);
    BOOST_REQUIRE(st == BLOCK_INDEX_TIP_CORRUPT);
    printf("T5 PASS non-dense active rejected\n");
}

BOOST_AUTO_TEST_CASE(t6_base_binding_fail_closed)
{
    const std::string dir = MakeTempDir();
    const int baseTip = -1; // base chain empty (genesis-height base)
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 9, 30, baseTip, &tip, NULL));
        // genesis tip block at height 0 (hashPrev must be null per ValidateRecord)
        BlockIndexRecord r = MakeRecord(uint256(0x66UL), uint256(0), 0, false);
        BlockIndexDerivedEntry d = MakeDerived(uint256(1), 5);
        BlockIndexTipAppend a; a.record = r; a.derived = d;
        std::string err;
        BOOST_REQUIRE(tip.Append(a, 0, &err) == BLOCK_INDEX_TIP_OK);
    }
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE(!BlockIndexTipAuthority::Open(dir, 8, &tip, NULL));
        printf("T6 PASS base-binding fail-closed (wrong gen rejected)\n");
    }
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE(BlockIndexTipAuthority::Open(dir, 9, &tip, NULL));
        BOOST_REQUIRE_EQUAL(tip.TipHeight(), 0);
        printf("T6 PASS correct gen reopens\n");
    }
}

BOOST_AUTO_TEST_CASE(t7_truncate_reorg)
{
    const std::string dir = MakeTempDir();
    const int baseTip = 100;
    const uint256 basePrev = uint256(0x1111UL);
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 4, 200, baseTip, &tip, NULL));
        BuildChain(tip, 6, NULL, basePrev, baseTip); // heights 101..106
        BOOST_REQUIRE_EQUAL(tip.TipHeight(), 106);
        uint8_t fenceBefore = tip.ActiveFence();
        std::string err;
        BOOST_REQUIRE(tip.TruncateActiveTo(103, &err) == BLOCK_INDEX_TIP_OK);
        BOOST_REQUIRE_EQUAL(tip.TipHeight(), 103);
        BOOST_REQUIRE_EQUAL((unsigned)tip.ActiveFence() - (unsigned)fenceBefore, 1u);
        BOOST_REQUIRE_EQUAL(tip.TipRecordCount(), 6u); // records kept (side/reorg)
        printf("T7 PASS reorg truncate: tip 106->103, records kept=%llu\n",
               (unsigned long long)tip.TipRecordCount());
    }
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE(BlockIndexTipAuthority::Open(dir, 4, &tip, NULL));
        BOOST_REQUIRE_EQUAL(tip.TipHeight(), 103);
        BOOST_REQUIRE_EQUAL(tip.TipRecordCount(), 6u);
        // re-append a NEW branch block at height 104 off the truncated tip
        BlockIndexTipRead t103 = tip.LookupActiveByHeight(103, NULL);
        BOOST_REQUIRE(t103.status == BLOCK_INDEX_TIP_OK);
        uint256 newPrev = t103.record.hash;
        uint256 h104 = uint256(0xFEED01UL);
        BlockIndexRecord r = MakeRecord(h104, newPrev, 104, false);
        BlockIndexDerivedEntry d = MakeDerived(uint256(0x9999UL), 999);
        BlockIndexTipAppend a; a.record = r; a.derived = d;
        std::string err;
        BOOST_REQUIRE(tip.Append(a, 104, &err) == BLOCK_INDEX_TIP_OK);
        BOOST_REQUIRE_EQUAL(tip.TipHeight(), 104);
        printf("T7 PASS reorg re-append new branch tip=%d\n", (int)tip.TipHeight());
    }
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE(BlockIndexTipAuthority::Open(dir, 4, &tip, NULL));
        BOOST_REQUIRE_EQUAL(tip.TipHeight(), 104);
        printf("T7 PASS reopen after reorg\n");
    }
}

BOOST_AUTO_TEST_CASE(t8_crash_mid_append_recovery)
{
    const std::string dir = MakeTempDir();
    const int baseTip = 200;
    const uint256 basePrev = uint256(0x11UL);
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 6, 300, baseTip, &tip, NULL));
        BuildChain(tip, 3, NULL, basePrev, baseTip); // heights 201..203
        BOOST_REQUIRE_EQUAL(tip.TipHeight(), 203);
        BOOST_REQUIRE_EQUAL(tip.TipRecordCount(), 3u);
    }
    // Simulate crash mid-append: append one EXTRA record/derived/active entry
    // to the files WITHOUT advancing tip.meta (store ahead of commit point).
    {
        BlockIndexRecord bogus = MakeRecord(uint256(0xDEADUL), uint256(0xBEEDUL), 204, false);
        std::vector<unsigned char> enc;
        BOOST_REQUIRE(EncodeBlockIndexRecordV1(bogus, &enc, NULL));
        FILE* f = fopen((dir + "/blockindex_tip/tip-records.dat").c_str(), "ab");
        BOOST_REQUIRE(f != NULL);
        BOOST_REQUIRE(fwrite(&enc[0], 1, enc.size(), f) == enc.size());
        fclose(f);

        std::string aenc;
        BOOST_REQUIRE(EncodeBlockIndexActiveEntry(300u + 3 + 1, &aenc, NULL)); // id=304
        FILE* fa = fopen((dir + "/blockindex_tip/tip-active.dat").c_str(), "ab");
        BOOST_REQUIRE(fa != NULL);
        BOOST_REQUIRE(fwrite(aenc.data(), 1, aenc.size(), fa) == aenc.size());
        fclose(fa);

        BlockIndexDerivedEntry bogusD = MakeDerived(uint256(0xFFFFUL), 777);
        std::vector<unsigned char> denc;
        BOOST_REQUIRE(EncodeBlockIndexDerivedEntry(bogusD, &denc, NULL));
        FILE* fd = fopen((dir + "/blockindex_tip/tip-derived.dat").c_str(), "ab");
        BOOST_REQUIRE(fd != NULL);
        BOOST_REQUIRE(fwrite(&denc[0], 1, denc.size(), fd) == denc.size());
        fclose(fd);
    }
    // Reopen: must truncate the uncommitted tail back to the committed tip (203).
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE(BlockIndexTipAuthority::Open(dir, 6, &tip, NULL));
        BOOST_REQUIRE_EQUAL(tip.TipHeight(), 203);
        BOOST_REQUIRE_EQUAL(tip.TipRecordCount(), 3u);
        printf("T8 PASS crash-mid-append recovery: uncommitted tail truncated, tip=203\n");
    }
}

BOOST_AUTO_TEST_CASE(t9_side_branch)
{
    const std::string dir = MakeTempDir();
    const int baseTip = 500;
    const uint256 basePrev = uint256(0x1212UL);
    BlockIndexTipAuthority tip;
    BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 7, 800, baseTip, &tip, NULL));
    BuildChain(tip, 3, NULL, basePrev, baseTip); // active 501..503
    BOOST_REQUIRE_EQUAL(tip.TipHeight(), 503);
    // side branch block at height 501 (fork off active 500 hash), NOT active
    uint256 sideHash = uint256(0xB00BUL);
    BlockIndexRecord r = MakeRecord(sideHash, basePrev, 501, false);
    BlockIndexDerivedEntry d = MakeDerived(uint256(0xABCDUL), 314);
    BlockIndexTipAppend a; a.record = r; a.derived = d;
    std::string err;
    BOOST_REQUIRE(tip.Append(a, -1, &err) == BLOCK_INDEX_TIP_OK);
    // recorded but not active
    BlockIndexTipRead sr = tip.LookupByHash(sideHash, NULL);
    BOOST_REQUIRE(sr.status == BLOCK_INDEX_TIP_OK);
    BOOST_REQUIRE(!sr.active);
    BOOST_REQUIRE_EQUAL(tip.TipRecordCount(), 4u);
    // active tip unchanged
    BOOST_REQUIRE_EQUAL(tip.TipHeight(), 503);
    BlockIndexTipRead active = tip.LookupActiveByHeight(501, NULL);
    BOOST_REQUIRE(active.status == BLOCK_INDEX_TIP_OK);
    BOOST_REQUIRE(active.record.hash != sideHash);
    printf("T9 PASS side branch: recorded but not active; tip stays 503\n");
}

BOOST_AUTO_TEST_CASE(t10_lookups)
{
    const std::string dir = MakeTempDir();
    const int baseTip = 800;
    const uint256 basePrev = uint256(0xABABUL);
    BlockIndexTipAuthority tip;
    BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 7, 1000, baseTip, &tip, NULL));
    BuildChain(tip, 4, NULL, basePrev, baseTip); // heights 801..804, hashes 0x1111..0x1114
    // GetTip
    BlockIndexTipRead t = tip.GetTip();
    BOOST_REQUIRE(t.status == BLOCK_INDEX_TIP_OK);
    BOOST_REQUIRE_EQUAL(t.height, 804);
    // LookupByHash: 0x1112 == height 802
    BlockIndexTipRead b1 = tip.LookupByHash(uint256(0x1112UL), NULL);
    BOOST_REQUIRE(b1.status == BLOCK_INDEX_TIP_OK);
    BOOST_REQUIRE(b1.active);
    BOOST_REQUIRE_EQUAL(b1.height, 802);
    // LookupActiveByHeight(801)
    BlockIndexTipRead h801 = tip.LookupActiveByHeight(801, NULL);
    BOOST_REQUIRE(h801.status == BLOCK_INDEX_TIP_OK);
    BOOST_REQUIRE_EQUAL(h801.height, 801);
    // LookupParent of 802 == 801
    BlockIndexTipRead parent = tip.LookupParent(uint256(0x1112UL), NULL);
    BOOST_REQUIRE(parent.status == BLOCK_INDEX_TIP_OK);
    BOOST_REQUIRE_EQUAL(parent.height, 801);
    // LookupNextActive from 803 (0x1113) -> 804 (0x1114)
    BlockIndexTipRead n2 = tip.LookupNextActive(uint256(0x1113UL), NULL);
    BOOST_REQUIRE(n2.status == BLOCK_INDEX_TIP_OK);
    BOOST_REQUIRE_EQUAL(n2.height, 804);
    printf("T10 PASS lookups (getTip/byHash/byHeight/parent/next)\n");
}

// A.10.1q regression: LookupByHash resolves the record slot by DIRECT arithmetic
// (id - baseRecordCount - 1) instead of the former O(records) scan (+O(activeIds)
// scan). This case proves the slot-arithmetic lookup is result-identical over
// EVERY slot: each hash returns its own record/derived/height, ACTIVE records stay
// active, a side record over an already-active height stays inactive, and an
// unknown hash still fails closed NOT_FOUND.
BOOST_AUTO_TEST_CASE(t10b_hash_lookup_slot_equiv)
{
    const std::string dir = MakeTempDir();
    const int baseTip = 800;
    const uint256 basePrev = uint256(0xABABUL);
    BlockIndexTipAuthority tip;
    BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 7, 1000, baseTip, &tip, NULL));
    BuildChain(tip, 8, NULL, basePrev, baseTip); // 801..808, hashes 0x1111..0x1118

    // Every slot (first .. last) must resolve to exactly its own record.
    for (uint32_t i = 0; i < 8; ++i)
    {
        const uint256 h = uint256(0x1111UL + i);
        BlockIndexTipRead rd = tip.LookupByHash(h, NULL);
        BOOST_REQUIRE_MESSAGE(rd.status == BLOCK_INDEX_TIP_OK, "slot " << i << " must resolve");
        BOOST_REQUIRE(rd.record.hash == h);
        BOOST_REQUIRE(rd.record.hashPrev == (i == 0 ? basePrev : uint256(0x1110UL + i)));
        BOOST_REQUIRE_EQUAL(rd.height, baseTip + 1 + (int)i);
        BOOST_REQUIRE(rd.active); // BuildChain appends ACTIVE records
        BOOST_REQUIRE(rd.derived.chainTrust == uint256(i + 1));
        BOOST_REQUIRE_EQUAL(rd.derived.stakeModifierChecksum, 1000u + i);
        BOOST_REQUIRE_EQUAL(rd.derived.nSize, 1200u + ((1000u + i) % 100u));
    }

    // Side record over the LAST active height (808): recorded, never active.
    const uint256 sideHash = uint256(0x5151UL);
    BlockIndexTipAppend a;
    a.record = MakeRecord(sideHash, uint256(0x1117UL), baseTip + 8, false);
    a.derived = MakeDerived(uint256(0xF00DUL), 777);
    std::string err;
    BOOST_REQUIRE(tip.Append(a, -1, &err) == BLOCK_INDEX_TIP_OK);
    BlockIndexTipRead side = tip.LookupByHash(sideHash, NULL);
    BOOST_REQUIRE(side.status == BLOCK_INDEX_TIP_OK);
    BOOST_CHECK_MESSAGE(!side.active, "a side record over an active height must not be active");
    BOOST_REQUIRE(side.derived.chainTrust == uint256(0xF00DUL));
    // The active member at that height is still the ACTIVE record, not the side one.
    BlockIndexTipRead still = tip.LookupByHash(uint256(0x1118UL), NULL);
    BOOST_REQUIRE(still.status == BLOCK_INDEX_TIP_OK && still.active);

    // Unknown hash: fail closed, unchanged.
    BOOST_REQUIRE(tip.LookupByHash(uint256(0xDEADUL), NULL).status == BLOCK_INDEX_TIP_NOT_FOUND);
    printf("T10b PASS LookupByHash slot arithmetic: all slots + side + not-found\n");
}

// t10c (R5): LookupActiveByHeight must resolve every active height to its exact
// record by slot arithmetic (no linear scan), matching record order.
BOOST_AUTO_TEST_CASE(t10c_active_by_height_slot_equiv)
{
    const std::string dir = MakeTempDir();
    const int baseTip = 1500;
    BlockIndexTipAuthority tip;
    BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 11, 1500, baseTip, &tip, NULL));
    const size_t n = 40;
    std::vector<uint256> hs(n);
    uint256 prev = uint256(0x1a1aUL);
    for (size_t i = 0; i < n; ++i)
    {
        BlockIndexTipAppend a;
        a.record = MakeRecord(uint256(0x5000UL + i + 1), prev, baseTip + (int)i + 1, true);
        a.derived = MakeDerived(uint256(3), 500 + (uint64_t)i);
        std::string err;
        BOOST_REQUIRE(tip.Append(a, baseTip + (int)i + 1, &err) == BLOCK_INDEX_TIP_OK);
        hs[i] = a.record.hash;
        prev = a.record.hash;
    }
    for (size_t i = 0; i < n; ++i)
    {
        BlockIndexTipRead r = tip.LookupActiveByHeight(baseTip + (int)i + 1, NULL);
        BOOST_REQUIRE(r.status == BLOCK_INDEX_TIP_OK);
        BOOST_CHECK(r.record.hash == hs[i]);
        BOOST_CHECK(r.active);
        BOOST_CHECK_EQUAL(r.height, baseTip + (int)i + 1);
    }
    BOOST_CHECK(tip.LookupActiveByHeight(baseTip, NULL).status == BLOCK_INDEX_TIP_NOT_FOUND);
    BOOST_CHECK(tip.LookupActiveByHeight(baseTip + (int)n + 1, NULL).status == BLOCK_INDEX_TIP_NOT_FOUND);
    printf("t10c PASS LookupActiveByHeight slot-equivalent for %zu heights\n", n);
    tip.Close();
}

// =====================================================================
// R4 — incremental (append-only) tail persistence.
// =====================================================================

// R4-1: a normal append must write O(new) bytes: the tail-store growth per
// append is exactly the encoded entry sizes and does NOT scale with the
// retained tail size.
BOOST_AUTO_TEST_CASE(r4_incremental_append_writes_constant_bytes)
{
    const uint64_t REC = 228, DER = 56, ACT = 8;      // entry sizes
    const uint64_t HREC = 48, HDER = 72, HACT = 44;   // header sizes
    const size_t tails[3] = {16, 256, 2048};
    for (int t = 0; t < 3; ++t)
    {
        const std::string dir = MakeTempDir();
        const int baseTip = 1000 + t * 10;
        const size_t n = tails[t];
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 7, 1000, baseTip, &tip, NULL));
        uint256 tipHash;
        BuildChain(tip, (uint32_t)n, &tipHash, uint256(0x7000UL + (unsigned long)(t + 1)), baseTip);
        BOOST_REQUIRE_EQUAL(tip.TipRecordCount(), n);
        const fs::path rp = fs::path(dir) / "blockindex_tip" / "tip-records.dat";
        const fs::path dp = fs::path(dir) / "blockindex_tip" / "tip-derived.dat";
        const fs::path ap = fs::path(dir) / "blockindex_tip" / "tip-active.dat";
        const uint64_t r0 = fs::file_size(rp), d0 = fs::file_size(dp), a0 = fs::file_size(ap);
        BlockIndexTipAppend a;
        a.record = MakeRecord(uint256(0x8000UL + (unsigned long)(t + 1)), tipHash, baseTip + (int)n + 1, true);
        a.derived = MakeDerived(uint256(5), 500);
        std::string err;
        BOOST_REQUIRE(tip.Append(a, baseTip + (int)n + 1, &err) == BLOCK_INDEX_TIP_OK);
        const uint64_t r1 = fs::file_size(rp), d1 = fs::file_size(dp), a1 = fs::file_size(ap);
        BOOST_CHECK_MESSAGE(r1 - r0 == REC,
            "records growth must be exactly one entry at tail " << n << " (got " << (r1 - r0) << ")");
        BOOST_CHECK_MESSAGE(d1 - d0 == DER,
            "derived growth must be exactly one entry at tail " << n << " (got " << (d1 - d0) << ")");
        BOOST_CHECK_MESSAGE(a1 - a0 == ACT,
            "active growth must be exactly one entry at tail " << n << " (got " << (a1 - a0) << ")");
        // Absolute file size == header + committed entries: no tail re-encoding.
        BOOST_CHECK_EQUAL(r1, HREC + (n + 1) * REC);
        BOOST_CHECK_EQUAL(d1, HDER + (n + 1) * DER);
        BOOST_CHECK_EQUAL(a1, HACT + (n + 1) * ACT);
        printf("R4-1 tail=%zu bytes/append: rec=%llu der=%llu act=%llu (O(new), not O(tail))\n",
               n, (unsigned long long)(r1 - r0), (unsigned long long)(d1 - d0),
               (unsigned long long)(a1 - a0));
        tip.Close();
    }
}

// R4-2: crash-before-meta leaves an uncommitted suffix on disk. The NEXT append
// (no reopen) must truncate that residue so the new record lands exactly at the
// committed boundary, and a restart must recover the exact committed state.
BOOST_AUTO_TEST_CASE(r4_uncommitted_suffix_then_append_is_exact)
{
    const std::string dir = MakeTempDir();
    const int baseTip = 2000;
    uint256 tipHash;
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 31, 700, baseTip, &tip, NULL));
        BuildChain(tip, 2, &tipHash, uint256(0x2000UL), baseTip); // 2001, 2002
        BOOST_REQUIRE_EQUAL(tip.TipRecordCount(), 2u);
        // Append #3 aborts after the stores are durable but before tip.meta.
        BlockIndexTipSetFailpointForTesting("FP_AFTER_TAIL_DURABLE_BEFORE_META", true);
        BlockIndexTipAppend a3;
        a3.record = MakeRecord(uint256(0x3333UL), tipHash, baseTip + 3, true);
        a3.derived = MakeDerived(uint256(9), 3003);
        std::string err;
        BOOST_REQUIRE(tip.Append(a3, baseTip + 3, &err) == BLOCK_INDEX_TIP_IO_ERROR);
        BlockIndexTipSetFailpointForTesting("FP_AFTER_TAIL_DURABLE_BEFORE_META", false);
        // In-memory authority must NOT be ahead of the committed tip.meta.
        BOOST_REQUIRE_EQUAL(tip.TipRecordCount(), 2u);
        BOOST_REQUIRE_EQUAL(tip.TipHeight(), baseTip + 2);
        // Next append must discard the residue and commit exactly one record.
        BlockIndexTipAppend a4;
        a4.record = MakeRecord(uint256(0x4444UL), tipHash, baseTip + 3, true);
        a4.derived = MakeDerived(uint256(10), 4004);
        BOOST_REQUIRE(tip.Append(a4, baseTip + 3, &err) == BLOCK_INDEX_TIP_OK);
        BOOST_REQUIRE_EQUAL(tip.TipRecordCount(), 3u);
        BOOST_REQUIRE_EQUAL(tip.TipHeight(), baseTip + 3);
        BOOST_REQUIRE(tip.TipHash() == uint256(0x4444UL));
        BOOST_REQUIRE(tip.LookupByHash(uint256(0x3333UL), NULL).status == BLOCK_INDEX_TIP_NOT_FOUND);
        BOOST_REQUIRE(tip.LookupByHash(uint256(0x4444UL), NULL).record.hash == uint256(0x4444UL));
        tip.Close();
    }
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE(BlockIndexTipAuthority::Open(dir, 31, &tip, NULL));
        BOOST_REQUIRE_EQUAL(tip.TipRecordCount(), 3u);
        BOOST_REQUIRE_EQUAL(tip.TipHeight(), baseTip + 3);
        BOOST_REQUIRE(tip.TipHash() == uint256(0x4444UL));
        BOOST_REQUIRE(tip.LookupByHash(uint256(0x3333UL), NULL).status == BLOCK_INDEX_TIP_NOT_FOUND);
        BlockIndexTipRead h = tip.LookupActiveByHeight(baseTip + 3, NULL);
        BOOST_REQUIRE(h.status == BLOCK_INDEX_TIP_OK);
        BOOST_REQUIRE(h.record.hash == uint256(0x4444UL));
        printf("R4-2 PASS uncommitted suffix truncated by next append; restart exact\n");
    }
}

// R4-3: a mixed AppendBatch (2 chained ACTIVE + 1 side fork) round-trips and is
// recovered exactly after restart.
BOOST_AUTO_TEST_CASE(r4_batch_mixed_records_roundtrip)
{
    const std::string dir = MakeTempDir();
    const int baseTip = 3000;
    for (int pass = 0; pass < 2; ++pass)
    {
        BlockIndexTipAuthority tip;
        if (pass == 0)
            BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 32, 700, baseTip, &tip, NULL));
        else
            BOOST_REQUIRE(BlockIndexTipAuthority::Open(dir, 32, &tip, NULL));
        if (pass == 0)
        {
            std::vector<BlockIndexTipAppend> blocks;
            std::vector<int32_t> hs;
            BlockIndexTipAppend b1;
            b1.record = MakeRecord(uint256(0x4001UL), uint256(0x3000UL), baseTip + 1, true);
            b1.derived = MakeDerived(uint256(1), 7001);
            BlockIndexTipAppend b2; // side fork off 0x4001 at height baseTip+2
            b2.record = MakeRecord(uint256(0x4002UL), uint256(0x4001UL), baseTip + 2, false);
            b2.derived = MakeDerived(uint256(2), 7002);
            BlockIndexTipAppend b3; // second side fork off 0x4001
            b3.record = MakeRecord(uint256(0x4003UL), uint256(0x4001UL), baseTip + 2, false);
            b3.derived = MakeDerived(uint256(3), 7003);
            // One ACTIVE record per batch (the density rule ties the next active
            // height to the committed tip); the side records ride along.
            blocks.push_back(b1); hs.push_back(baseTip + 1);
            blocks.push_back(b2); hs.push_back(-1);
            blocks.push_back(b3); hs.push_back(-1);
            std::string err;
            BOOST_REQUIRE(tip.AppendBatch(blocks, hs, &err) == BLOCK_INDEX_TIP_OK);
            tip.Close();
            continue;
        }
        BOOST_REQUIRE_EQUAL(tip.TipRecordCount(), 3u);
        BOOST_REQUIRE_EQUAL(tip.TipHeight(), baseTip + 1);
        BOOST_REQUIRE(tip.TipHash() == uint256(0x4001UL));
        BOOST_REQUIRE(tip.LookupByHash(uint256(0x4001UL), NULL).active);
        BlockIndexTipRead s2 = tip.LookupByHash(uint256(0x4002UL), NULL);
        BOOST_REQUIRE(s2.status == BLOCK_INDEX_TIP_OK);
        BOOST_REQUIRE(!s2.active);
        BlockIndexTipRead s3 = tip.LookupByHash(uint256(0x4003UL), NULL);
        BOOST_REQUIRE(s3.status == BLOCK_INDEX_TIP_OK);
        BOOST_REQUIRE(!s3.active);
        BlockIndexTipRead h = tip.LookupActiveByHeight(baseTip + 1, NULL);
        BOOST_REQUIRE(h.status == BLOCK_INDEX_TIP_OK);
        BOOST_REQUIRE(h.record.hash == uint256(0x4001UL));
        printf("R4-3 PASS mixed batch round-trip (pass=%d)\n", pass);
        tip.Close();
    }
}

// =====================================================================
// R2B — durable operator-invalid authority (v2 mutable protocol).
//   T11 invalidate persists across reopen
//   T12 reconsider clears + persists
//   T13 idempotent duplicate intent is a no-op; multi-hash set
//   T14 v1 tip.meta backward compatibility -> EMPTY invalid set
//   T15 corrupt invalid log (digest mismatch) -> FAIL CLOSED
//   T16 unknown mutable protocol version -> FAIL CLOSED
//   T17 committed invalid set missing -> FAIL CLOSED
//   T18 uncommitted log tail (crash mid-commit) -> truncated, set unchanged
// =====================================================================

static std::vector<unsigned char> ReadRawFile(const std::string& path)
{
    std::vector<unsigned char> v;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return v;
    unsigned char buf[4096];
    size_t r;
    while ((r = fread(buf, 1, sizeof(buf), f)) > 0)
        v.insert(v.end(), buf, buf + r);
    fclose(f);
    return v;
}

static void WriteRawFile(const std::string& path, const std::vector<unsigned char>& v)
{
    FILE* f = fopen(path.c_str(), "wb");
    BOOST_REQUIRE(f != NULL);
    if (!v.empty())
        BOOST_REQUIRE(fwrite(&v[0], 1, v.size(), f) == v.size());
    fclose(f);
}

static std::string TipInvalidPath(const std::string& dir)
{
    return dir + "/blockindex_tip/tip-invalid.dat";
}

BOOST_AUTO_TEST_CASE(t11_invalidate_persists_reopen)
{
    const std::string dir = MakeTempDir();
    const uint256 hA(0xA1UL);
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 5, 900, 800, &tip, NULL));
        BOOST_REQUIRE(!tip.IsOperatorInvalid(hA));
        BOOST_REQUIRE(tip.SetOperatorInvalid(hA, true, NULL) == BLOCK_INDEX_TIP_OK);
        BOOST_REQUIRE(tip.IsOperatorInvalid(hA));
        BOOST_REQUIRE_EQUAL(tip.InvalidLogCount(), 1u);
    }
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE(BlockIndexTipAuthority::Open(dir, 5, &tip, NULL));
        BOOST_REQUIRE_MESSAGE(tip.IsOperatorInvalid(hA),
                              "R2B: operator-invalid state MUST survive restart");
        BOOST_REQUIRE_EQUAL(tip.InvalidLogCount(), 1u);
        printf("T11 PASS invalidate persisted across reopen (log=%u)\n", tip.InvalidLogCount());
    }
}

BOOST_AUTO_TEST_CASE(t12_reconsider_clears_persists)
{
    const std::string dir = MakeTempDir();
    const uint256 hA(0xA1UL);
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 5, 900, 800, &tip, NULL));
        BOOST_REQUIRE(tip.SetOperatorInvalid(hA, true, NULL) == BLOCK_INDEX_TIP_OK);
        BOOST_REQUIRE(tip.SetOperatorInvalid(hA, false, NULL) == BLOCK_INDEX_TIP_OK);
        BOOST_REQUIRE(!tip.IsOperatorInvalid(hA));
        BOOST_REQUIRE_EQUAL(tip.InvalidLogCount(), 2u); // two intents, append-only
    }
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE(BlockIndexTipAuthority::Open(dir, 5, &tip, NULL));
        BOOST_REQUIRE(!tip.IsOperatorInvalid(hA));
        printf("T12 PASS reconsider cleared + persisted\n");
    }
}

BOOST_AUTO_TEST_CASE(t13_idempotent_and_multiset)
{
    const std::string dir = MakeTempDir();
    const uint256 hA(0xA1UL), hB(0xB2UL);
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 5, 900, 800, &tip, NULL));
        BOOST_REQUIRE(tip.SetOperatorInvalid(hA, true, NULL) == BLOCK_INDEX_TIP_OK);
        BOOST_REQUIRE(tip.SetOperatorInvalid(hB, true, NULL) == BLOCK_INDEX_TIP_OK);
        BOOST_REQUIRE(tip.SetOperatorInvalid(hA, true, NULL) == BLOCK_INDEX_TIP_OK);
        BOOST_REQUIRE_EQUAL(tip.InvalidLogCount(), 2u); // duplicate is a no-op
        BOOST_REQUIRE(tip.IsOperatorInvalid(hA));
        BOOST_REQUIRE(tip.IsOperatorInvalid(hB));
    }
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE(BlockIndexTipAuthority::Open(dir, 5, &tip, NULL));
        std::set<uint256> s = tip.OperatorInvalidSet();
        BOOST_REQUIRE_EQUAL(s.size(), 2u);
        printf("T13 PASS idempotent + multi-hash set (size=%u)\n", (unsigned)s.size());
    }
}

BOOST_AUTO_TEST_CASE(t14_v1_meta_backward_compatible_empty)
{
    const std::string dir = MakeTempDir();
    const uint256 hA(0xA1UL);
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 5, 900, 800, &tip, NULL));
        BOOST_REQUIRE(tip.SetOperatorInvalid(hA, true, NULL) == BLOCK_INDEX_TIP_OK);
    }
    // Rewrite tip.meta as the previous supported (v1) 104-byte layout.
    std::vector<unsigned char> meta = ReadRawFile(dir + "/blockindex_tip/tip.meta");
    BOOST_REQUIRE(meta.size() >= 104);
    meta.resize(104);
    meta[0] = 1; meta[1] = 0; meta[2] = 0; meta[3] = 0; // version = 1 (LE32)
    WriteRawFile(dir + "/blockindex_tip/tip.meta", meta);
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE_MESSAGE(BlockIndexTipAuthority::Open(dir, 5, &tip, NULL),
                              "R2B: a v1 tip.meta MUST open (backward compatible)");
        BOOST_REQUIRE_EQUAL(tip.InvalidLogCount(), 0u);
        BOOST_REQUIRE(!tip.IsOperatorInvalid(hA)); // v1 => EMPTY set
        BOOST_REQUIRE_EQUAL(tip.TipHeight(), 800); // effective tip preserved
        // first legitimate new commit upgrades to v2 deterministically.
        BOOST_REQUIRE(tip.SetOperatorInvalid(hA, true, NULL) == BLOCK_INDEX_TIP_OK);
        BOOST_REQUIRE(tip.IsOperatorInvalid(hA));
    }
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE(BlockIndexTipAuthority::Open(dir, 5, &tip, NULL));
        BOOST_REQUIRE(tip.IsOperatorInvalid(hA));
        printf("T14 PASS v1 meta opens (empty) and upgrades on commit\n");
    }
}

BOOST_AUTO_TEST_CASE(t15_corrupt_invalid_log_fail_closed)
{
    const std::string dir = MakeTempDir();
    const uint256 hA(0xA1UL);
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 5, 900, 800, &tip, NULL));
        BOOST_REQUIRE(tip.SetOperatorInvalid(hA, true, NULL) == BLOCK_INDEX_TIP_OK);
    }
    std::vector<unsigned char> inv = ReadRawFile(TipInvalidPath(dir));
    BOOST_REQUIRE(inv.size() >= 12 + 33);
    inv[12 + 5] ^= 0xFF; // flip a byte inside the committed entry -> digest mismatch
    WriteRawFile(TipInvalidPath(dir), inv);
    {
        BlockIndexTipAuthority tip;
        std::string err;
        BOOST_REQUIRE_MESSAGE(!BlockIndexTipAuthority::Open(dir, 5, &tip, &err),
                              "R2B: corrupt invalid log MUST fail closed");
        printf("T15 PASS corrupt invalid log fail-closed: %s\n", err.c_str());
    }
}

BOOST_AUTO_TEST_CASE(t16_unknown_version_fail_closed)
{
    const std::string dir = MakeTempDir();
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 5, 900, 800, &tip, NULL));
    }
    std::vector<unsigned char> meta = ReadRawFile(dir + "/blockindex_tip/tip.meta");
    BOOST_REQUIRE(meta.size() >= 104);
    meta[0] = 99; meta[1] = 0; meta[2] = 0; meta[3] = 0; // unknown future version
    WriteRawFile(dir + "/blockindex_tip/tip.meta", meta);
    {
        BlockIndexTipAuthority tip;
        std::string err;
        BOOST_REQUIRE_MESSAGE(!BlockIndexTipAuthority::Open(dir, 5, &tip, &err),
                              "R2B: unknown mutable protocol version MUST fail closed");
        printf("T16 PASS unknown version fail-closed: %s\n", err.c_str());
    }
}

BOOST_AUTO_TEST_CASE(t17_missing_committed_invalid_set_fail_closed)
{
    const std::string dir = MakeTempDir();
    const uint256 hA(0xA1UL);
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 5, 900, 800, &tip, NULL));
        BOOST_REQUIRE(tip.SetOperatorInvalid(hA, true, NULL) == BLOCK_INDEX_TIP_OK);
    }
    BOOST_REQUIRE(std::remove(TipInvalidPath(dir).c_str()) == 0);
    {
        BlockIndexTipAuthority tip;
        std::string err;
        BOOST_REQUIRE_MESSAGE(!BlockIndexTipAuthority::Open(dir, 5, &tip, &err),
                              "R2B: committed invalid set missing MUST fail closed");
        printf("T17 PASS missing committed invalid set fail-closed: %s\n", err.c_str());
    }
}

BOOST_AUTO_TEST_CASE(t18_uncommitted_tail_truncated)
{
    const std::string dir = MakeTempDir();
    const uint256 hA(0xA1UL), hX(0xCCUL);
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 5, 900, 800, &tip, NULL));
        BOOST_REQUIRE(tip.SetOperatorInvalid(hA, true, NULL) == BLOCK_INDEX_TIP_OK);
    }
    // Extend the log with an uncommitted entry: header count 2 + two entries,
    // while tip.meta still commits only the first (invalidLogCount == 1).
    std::vector<unsigned char> inv = ReadRawFile(TipInvalidPath(dir));
    BOOST_REQUIRE(inv.size() == 12 + 33);
    inv[8] = 2; inv[9] = 0; inv[10] = 0; inv[11] = 0; // header count = 2
    std::vector<unsigned char> extra(hX.begin(), hX.end());
    extra.push_back(1);
    inv.insert(inv.end(), extra.begin(), extra.end());
    WriteRawFile(TipInvalidPath(dir), inv);
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE_MESSAGE(BlockIndexTipAuthority::Open(dir, 5, &tip, NULL),
                              "R2B: an uncommitted log tail must be truncated, not fatal");
        BOOST_REQUIRE(tip.IsOperatorInvalid(hA));
        BOOST_REQUIRE(!tip.IsOperatorInvalid(hX)); // uncommitted tail dropped
        BOOST_REQUIRE_EQUAL(tip.InvalidLogCount(), 1u);
        printf("T18 PASS uncommitted tail truncated to committed set\n");
    }
}

// R2D — fused mutable authority transition: ONE operator-invalid intent AND the
// resulting active-tip reorg committed as a SINGLE tip.meta publication.
BOOST_AUTO_TEST_CASE(t19_fused_invalid_and_reorg_single_commit)
{
    const std::string dir = MakeTempDir();
    const int baseTip = 800;
    const uint256 basePrev(0x9999UL);
    const uint256 h1(0x1111UL), h2(0x2222UL), h2b(0x3333UL);
    uint8_t f0 = 0;
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 9, 500, baseTip, &tip, NULL));
        BlockIndexTipAppend a1;
        a1.record = MakeRecord(h1, basePrev, baseTip + 1, false);
        a1.derived = MakeDerived(uint256(1), 1000);
        BOOST_REQUIRE(tip.Append(a1, baseTip + 1, NULL) == BLOCK_INDEX_TIP_OK);
        BlockIndexTipAppend a2;
        a2.record = MakeRecord(h2, h1, baseTip + 2, false);
        a2.derived = MakeDerived(uint256(2), 1001);
        BOOST_REQUIRE(tip.Append(a2, baseTip + 2, NULL) == BLOCK_INDEX_TIP_OK);
        BOOST_REQUIRE(tip.TipHash() == h2);
        f0 = tip.ActiveFence();

        // Fused: invalidate h2 AND reorg the active chain back to S+1 -> [h2b],
        // as ONE logical authority transition.
        std::vector<BlockIndexTipAppend> branch(1);
        branch[0].record = MakeRecord(h2b, h1, baseTip + 2, false);
        branch[0].derived = MakeDerived(uint256(3), 1002);
        std::vector<int32_t> bh(1, baseTip + 2);
        std::string err;
        BOOST_REQUIRE_MESSAGE(
            tip.ApplyOperatorInvalidAndReorg(h2, true, baseTip + 1, branch, bh, &err) == BLOCK_INDEX_TIP_OK,
            err);
        BOOST_REQUIRE(tip.IsOperatorInvalid(h2));
        BOOST_REQUIRE(!tip.IsOperatorInvalid(h2b));
        BOOST_REQUIRE_EQUAL(tip.TipHeight(), baseTip + 2);
        BOOST_REQUIRE(tip.TipHash() == h2b);
        BOOST_REQUIRE_EQUAL(tip.InvalidLogCount(), 1u);
        BOOST_CHECK_EQUAL((int)tip.ActiveFence(), (int)f0 + 1); // one fence bump, one commit
        printf("t19 fused commit applied: tip=%.12s invalid(h2)=1 invalid(h2b)=0 fence+1\n",
               tip.TipHash().ToString().substr(0, 12).c_str());
    }
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE(BlockIndexTipAuthority::Open(dir, 9, &tip, NULL));
        BOOST_REQUIRE_MESSAGE(tip.IsOperatorInvalid(h2),
                              "R2D: committed invalid intent must survive reopen");
        BOOST_REQUIRE_MESSAGE(tip.TipHash() == h2b,
                              "R2D: committed active tip must survive reopen (same logical state)");
        BOOST_REQUIRE_EQUAL(tip.TipHeight(), baseTip + 2);
        BOOST_REQUIRE_EQUAL(tip.InvalidLogCount(), 1u);
        printf("t19 PASS fused invalid+reorg persisted coherently across reopen\n");
    }
}

// V2-R2D selector: competing branches + same-height tips + fused invalidate/reconsider.
BOOST_AUTO_TEST_CASE(t20_selector_competing_branch)
{
    const std::string dir = MakeTempDir();
    const int baseTip = 500;
    const uint256 basePrev = uint256(0xB45EUL);
    const uint256 A1(0xA1UL), A2(0xA2UL), A3(0xA3UL);
    const uint256 B1(0xB1UL), B2(0xB2UL), B3(0xB3UL);
    BlockIndexTipAuthority tip;
    BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 9, 2000, baseTip, &tip, NULL));

    {   // Branch A (ACTIVE): canonical trust 10,20,30 -> best tip trust 30.
        uint256 prev = basePrev;
        const uint256 hs[3] = {A1, A2, A3};
        const uint256 ts[3] = {uint256(10), uint256(20), uint256(30)};
        for (int i = 0; i < 3; ++i) {
            BlockIndexTipAppend a;
            a.record = MakeRecord(hs[i], prev, baseTip + 1 + i, false);
            a.derived = MakeDerived(ts[i], 200 + i);
            BOOST_REQUIRE(tip.Append(a, baseTip + 1 + i, NULL) == BLOCK_INDEX_TIP_OK);
            prev = hs[i];
        }
    }
    {   // Branch B (SIDE): SAME height (tip 503), LOWER trust 5,12,25.
        uint256 prev = basePrev;
        const uint256 hs[3] = {B1, B2, B3};
        const uint256 ts[3] = {uint256(5), uint256(12), uint256(25)};
        for (int i = 0; i < 3; ++i) {
            BlockIndexTipAppend a;
            a.record = MakeRecord(hs[i], prev, baseTip + 1 + i, false);
            a.derived = MakeDerived(ts[i], 300 + i);
            BOOST_REQUIRE(tip.Append(a, -1, NULL) == BLOCK_INDEX_TIP_OK);
            prev = hs[i];
        }
    }
    BOOST_CHECK(tip.TipHash() == A3);

    std::vector<BlockIndexTipAppend> br; std::vector<int32_t> bh; uint256 best; int32_t bestH = -1;
    // Same-height competing tips: selector picks the HIGHER canonical trust (A3).
    BOOST_REQUIRE(tip.SelectBestEligibleBranch(baseTip, uint256(0), false, &br, &bh, &best, &bestH, NULL) == BLOCK_INDEX_TIP_OK);
    BOOST_CHECK(best == A3); BOOST_CHECK_EQUAL(bestH, baseTip + 3); BOOST_CHECK_EQUAL(br.size(), 3u);

    // Invalidate A2 (mid-branch): A subtree ineligible -> B selected.
    br.clear(); bh.clear(); best = 0; bestH = -1;
    BOOST_REQUIRE(tip.SelectBestEligibleBranch(baseTip, A2, true, &br, &bh, &best, &bestH, NULL) == BLOCK_INDEX_TIP_OK);
    BOOST_CHECK(best == B3); BOOST_CHECK_EQUAL(bestH, baseTip + 3);
    BOOST_REQUIRE(tip.ApplyOperatorInvalidAndReorg(A2, true, baseTip, br, bh, NULL) == BLOCK_INDEX_TIP_OK);
    BOOST_CHECK(tip.TipHash() == B3);
    BOOST_CHECK(tip.IsOperatorInvalid(A2));
    BOOST_CHECK(!tip.IsOperatorInvalid(A1));

    // Reconsider A2: A eligible again (30 > 25) -> A re-selected.
    br.clear(); bh.clear(); best = 0; bestH = -1;
    BOOST_REQUIRE(tip.SelectBestEligibleBranch(baseTip, A2, false, &br, &bh, &best, &bestH, NULL) == BLOCK_INDEX_TIP_OK);
    BOOST_CHECK(best == A3);
    BOOST_REQUIRE(tip.ApplyOperatorInvalidAndReorg(A2, false, baseTip, br, bh, NULL) == BLOCK_INDEX_TIP_OK);
    BOOST_CHECK(tip.TipHash() == A3);
    BOOST_CHECK(!tip.IsOperatorInvalid(A2));
    tip.Close();

    {   // Restart: durable authority recovered exactly (tip A3, no invalid intent).
        BlockIndexTipAuthority re;
        BOOST_REQUIRE(BlockIndexTipAuthority::Open(dir, 9, &re, NULL));
        BOOST_CHECK(re.TipHash() == A3);
        BOOST_CHECK_EQUAL(re.TipHeight(), baseTip + 3);
        BOOST_CHECK(!re.IsOperatorInvalid(A2));
        re.Close();
    }
    printf("T20 PASS selector competing-branch + same-height + fused + restart\n");
}

BOOST_AUTO_TEST_CASE(t21_operator_idempotence)
{
    const std::string dir = MakeTempDir();
    const int baseTip = 10;
    BlockIndexTipAuthority tip;
    BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 3, 50, baseTip, &tip, NULL));
    const uint256 X(0xDEADUL), Y(0xBEEFUL);
    std::vector<BlockIndexTipAppend> br; std::vector<int32_t> bh;
    BOOST_REQUIRE(tip.ApplyOperatorInvalidAndReorg(X, true, baseTip, br, bh, NULL) == BLOCK_INDEX_TIP_OK);
    const uint32_t c1 = tip.InvalidLogCount();
    BOOST_REQUIRE(tip.ApplyOperatorInvalidAndReorg(X, true, baseTip, br, bh, NULL) == BLOCK_INDEX_TIP_OK);
    BOOST_CHECK_EQUAL(tip.InvalidLogCount(), c1);      // repeated invalidate: no new entry
    BOOST_CHECK(tip.IsOperatorInvalid(X));
    BOOST_REQUIRE(tip.ApplyOperatorInvalidAndReorg(Y, false, baseTip, br, bh, NULL) == BLOCK_INDEX_TIP_OK);
    BOOST_CHECK_EQUAL(tip.InvalidLogCount(), c1);      // reconsider non-invalid: no-op
    BOOST_REQUIRE(tip.ApplyOperatorInvalidAndReorg(X, false, baseTip, br, bh, NULL) == BLOCK_INDEX_TIP_OK);
    const uint32_t c2 = tip.InvalidLogCount();
    BOOST_REQUIRE(tip.ApplyOperatorInvalidAndReorg(X, false, baseTip, br, bh, NULL) == BLOCK_INDEX_TIP_OK);
    BOOST_CHECK_EQUAL(tip.InvalidLogCount(), c2);      // repeated reconsider: no-op
    BOOST_CHECK(!tip.IsOperatorInvalid(X));
    tip.Close();
    printf("T21 PASS operator idempotence/negatives\n");
}

// ---- V2-R2D P0-01 N1/N2: authoritative tip corruption -> FAIL CLOSED ----
static std::string TipRecordsPath(const std::string& dir)
{
    return dir + "/blockindex_tip/tip-records.dat";
}

static bool TruncateFileTo(const std::string& path, size_t newLen)
{
    std::vector<char> buf;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    char tmp[4096]; size_t n;
    while ((n = fread(tmp, 1, sizeof(tmp), f)) > 0) buf.insert(buf.end(), tmp, tmp + n);
    fclose(f);
    if (newLen > buf.size()) return false;
    f = fopen(path.c_str(), "wb");
    if (!f) return false;
    if (newLen > 0) fwrite(&buf[0], 1, newLen, f);
    fclose(f);
    return true;
}

static bool FlipByteInFile(const std::string& path, size_t offset)
{
    FILE* f = fopen(path.c_str(), "r+b");
    if (!f) return false;
    unsigned char b = 0;
    if (fseek(f, (long)offset, SEEK_SET) != 0) { fclose(f); return false; }
    if (fread(&b, 1, 1, f) != 1) { fclose(f); return false; }
    b ^= 0xFF;
    if (fseek(f, (long)offset, SEEK_SET) != 0) { fclose(f); return false; }
    fwrite(&b, 1, 1, f);
    fclose(f);
    return true;
}

// N1: durable tip claims L>S but the committed record for L is missing/unresolvable
// (tip-records.dat truncated below the tip.meta-committed record count). Startup/Open
// must FAIL CLOSED and must NOT silently fall back to S.
BOOST_AUTO_TEST_CASE(n1_missing_committed_tip_record_fails_closed)
{
    const std::string dir = MakeTempDir();
    const int baseTip = 500;
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 11, 300, baseTip, &tip, NULL));
        uint256 fin; BuildChain(tip, 3, &fin, uint256(0xC0DEUL), baseTip);
        BOOST_CHECK_EQUAL(tip.TipHeight(), baseTip + 3);
        tip.Close();
    }
    const std::string rp = TipRecordsPath(dir);
    // Remove the committed tail records: the durable tip still CLAIMS height S+3.
    BOOST_REQUIRE(TruncateFileTo(rp, 1));
    BlockIndexTipAuthority re;
    std::string err;
    const bool ok = BlockIndexTipAuthority::Open(dir, 11, &re, &err);
    BOOST_CHECK_MESSAGE(!ok, "N1: Open must FAIL CLOSED when the committed tip record is missing");
    BOOST_CHECK(!err.empty());
    printf("N1 PASS missing-committed-tip-record -> fail closed (err=%s)\n", err.substr(0, 48).c_str());
}

// N2: a committed post-S record is inconsistent with tip.meta (content digest no longer
// matches the committed region). Startup/Open must FAIL CLOSED and publish NO partial
// authority (no fallback to S).
BOOST_AUTO_TEST_CASE(n2_inconsistent_committed_record_fails_closed)
{
    const std::string dir = MakeTempDir();
    const int baseTip = 700;
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 13, 400, baseTip, &tip, NULL));
        uint256 fin; BuildChain(tip, 4, &fin, uint256(0xF00DUL), baseTip);
        BOOST_CHECK_EQUAL(tip.TipHeight(), baseTip + 4);
        tip.Close();
    }
    const std::string rp = TipRecordsPath(dir);
    // Flip a byte inside the committed record region -> committed bytes no longer match
    // the tip.meta content digest (post-S record inconsistent with the committed tip).
    BOOST_REQUIRE(FlipByteInFile(rp, 64));
    BlockIndexTipAuthority re;
    std::string err;
    const bool ok = BlockIndexTipAuthority::Open(dir, 13, &re, &err);
    BOOST_CHECK_MESSAGE(!ok, "N2: Open must FAIL CLOSED on an inconsistent committed post-S record");
    BOOST_CHECK(!err.empty());
    printf("N2 PASS inconsistent-committed-record -> fail closed (err=%s)\n", err.substr(0, 48).c_str());
}

// =====================================================================
// Repair #2 (v3 chained digest) — Gate A regressions.
// Independent ORACLE: recompute digest_v3 = SHA256(Rch||Dch||Ach||fence) over the
// SAME committed vectors with OpenSSL, entirely outside blockindex_tip.cpp, and
// compare against the PERSISTED tip.meta contentDigest. This proves live
// incremental == independent rebuild, not merely that Open accepts its own value.
// =====================================================================
static void R2PutLE32(std::vector<unsigned char>& b, uint32_t v)
{ for (int i = 0; i < 4; ++i) b.push_back((unsigned char)((v >> (8 * i)) & 0xff)); }
static void R2PutLE64(std::vector<unsigned char>& b, uint64_t v)
{ for (int i = 0; i < 8; ++i) b.push_back((unsigned char)((v >> (8 * i)) & 0xff)); }
static void R2Sha(const std::vector<unsigned char>& in, unsigned char out[32])
{ SHA256_CTX c; SHA256_Init(&c); if (!in.empty()) SHA256_Update(&c, &in[0], in.size()); SHA256_Final(out, &c); }
static void R2ChainInit(unsigned char out[32], const char* dom)
{ std::vector<unsigned char> b(dom, dom + strlen(dom)); R2Sha(b, out); }
static void R2ChainExtend(unsigned char st[32], const std::vector<unsigned char>& e)
{ std::vector<unsigned char> in(st, st + 32); in.insert(in.end(), e.begin(), e.end()); R2Sha(in, st); }

static void OracleDigestV3(const std::vector<BlockIndexRecord>& recs,
                           const std::vector<BlockIndexDerivedEntry>& ders,
                           const std::vector<BlockIndexId>& acts,
                           uint8_t fence, unsigned char out[32])
{
    unsigned char R[32], D[32], A[32];
    R2ChainInit(R, "INNOVA-TIP-DIGEST-V3/R");
    for (size_t i = 0; i < recs.size(); ++i)
    {
        std::vector<unsigned char> e(recs[i].hash.begin(), recs[i].hash.end());
        e.insert(e.end(), recs[i].hashPrev.begin(), recs[i].hashPrev.end());
        R2PutLE32(e, (uint32_t)recs[i].height);
        R2ChainExtend(R, e);
    }
    R2ChainInit(D, "INNOVA-TIP-DIGEST-V3/D");
    for (size_t i = 0; i < ders.size(); ++i)
    {
        std::vector<unsigned char> e(ders[i].chainTrust.begin(), ders[i].chainTrust.end());
        R2PutLE32(e, ders[i].stakeModifierChecksum);
        R2ChainExtend(D, e);
    }
    R2ChainInit(A, "INNOVA-TIP-DIGEST-V3/A");
    for (size_t i = 0; i < acts.size(); ++i)
    { std::vector<unsigned char> e; R2PutLE64(e, acts[i]); R2ChainExtend(A, e); }
    std::vector<unsigned char> fin(R, R + 32);
    fin.insert(fin.end(), D, D + 32);
    fin.insert(fin.end(), A, A + 32);
    fin.push_back(fence);
    R2Sha(fin, out);
}

static void ReadMetaDigest(const std::string& dir, unsigned char out[32], uint32_t* ver)
{
    std::vector<unsigned char> m = ReadRawFile(dir + "/blockindex_tip/tip.meta");
    BOOST_REQUIRE(m.size() >= 104);
    if (ver) *ver = (uint32_t)m[0] | ((uint32_t)m[1] << 8) | ((uint32_t)m[2] << 16) | ((uint32_t)m[3] << 24);
    memcpy(out, &m[72], 32);
}

// R2-A: ORACLE equivalence. Live incremental v3 digest == independent rebuild,
// and the store reopens (Open rebuilds v3 independently and must validate).
BOOST_AUTO_TEST_CASE(r2a_v3_oracle_matches_incremental_and_reopen)
{
    const std::string dir = MakeTempDir();
    const int baseTip = 400; const uint64_t baseRec = 900; const uint32_t N = 7;
    std::vector<BlockIndexRecord> recs; std::vector<BlockIndexDerivedEntry> ders;
    std::vector<BlockIndexId> acts; uint256 prev(0xBEEFUL);
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 21, baseRec, baseTip, &tip, NULL));
        for (uint32_t i = 0; i < N; ++i)
        {
            uint256 h = uint256(0x1111UL + i);
            BlockIndexRecord r = MakeRecord(h, prev, baseTip + 1 + (int)i, (i % 2) == 0);
            BlockIndexDerivedEntry d = MakeDerived(uint256(i + 1), 1000 + i);
            BlockIndexTipAppend a; a.record = r; a.derived = d;
            BOOST_REQUIRE(tip.Append(a, baseTip + 1 + (int)i, NULL) == BLOCK_INDEX_TIP_OK);
            recs.push_back(r); ders.push_back(d); acts.push_back(baseRec + i + 1);
            prev = h;
        }
    }
    unsigned char got[32]; uint32_t ver = 0; ReadMetaDigest(dir, got, &ver);
    BOOST_REQUIRE_EQUAL(ver, 3u);
    unsigned char want[32]; OracleDigestV3(recs, ders, acts, 0, want);
    BOOST_CHECK_MESSAGE(memcmp(got, want, 32) == 0,
                        "R2A: live incremental v3 digest MUST equal the independent oracle");
    BlockIndexTipAuthority re;
    BOOST_REQUIRE_MESSAGE(BlockIndexTipAuthority::Open(dir, 21, &re, NULL),
                          "R2A: v3 store MUST reopen and validate");
    BOOST_CHECK_EQUAL(re.TipHeight(), baseTip + (int)N);
    printf("R2A PASS v3 oracle == incremental == reopen\n");
}

// R2-B: v2 -> v3 transition. Fresh store is v2; first commit upgrades to v3.
BOOST_AUTO_TEST_CASE(r2b_v2_to_v3_transition)
{
    const std::string dir = MakeTempDir(); const int baseTip = 600;
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 22, 500, baseTip, &tip, NULL));
        unsigned char g[32]; uint32_t v = 0; ReadMetaDigest(dir, g, &v);
        BOOST_CHECK_EQUAL(v, 2u); // created store is v2 (legacy digest)
        uint256 fin; BuildChain(tip, 1, &fin, uint256(0xAAUL), baseTip);
        ReadMetaDigest(dir, g, &v);
        BOOST_CHECK_EQUAL(v, 3u); // first commit upgrades to v3
    }
    BlockIndexTipAuthority re;
    BOOST_REQUIRE_MESSAGE(BlockIndexTipAuthority::Open(dir, 22, &re, NULL),
                          "R2B: upgraded v3 store MUST reopen");
    printf("R2B PASS v2->v3 transition\n");
}

// R2-C: restart equivalence across multiple append sessions. chainTrust must
// keep INCREASING across the restart, or tip-selection treats the new blocks as
// an inferior branch and the tip legitimately does not advance.
BOOST_AUTO_TEST_CASE(r2c_v3_restart_accumulates)
{
    const std::string dir = MakeTempDir(); const int baseTip = 300;
    uint256 prev, s1tip;
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 23, 100, baseTip, &tip, NULL));
        prev = tip.TipHash();
        for (int i = 0; i < 5; ++i)
        {
            uint256 h = uint256(0x1000UL + i);
            BlockIndexRecord r = MakeRecord(h, prev, baseTip + 1 + i, (i % 2) == 0);
            BlockIndexDerivedEntry d = MakeDerived(uint256(i + 1), 1000 + i);
            BlockIndexTipAppend a; a.record = r; a.derived = d;
            BOOST_REQUIRE(tip.Append(a, baseTip + 1 + i, NULL) == BLOCK_INDEX_TIP_OK);
            prev = h;
        }
        s1tip = prev;
        BOOST_CHECK_EQUAL(tip.TipHeight(), baseTip + 5);
    }
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE(BlockIndexTipAuthority::Open(dir, 23, &tip, NULL));
        BOOST_CHECK_EQUAL(tip.TipHeight(), baseTip + 5);
        BOOST_CHECK(tip.TipHash() == s1tip);  // reopened tip identity preserved
        for (int i = 0; i < 5; ++i)
        {
            uint256 h = uint256(0x2000UL + i);
            BlockIndexRecord r = MakeRecord(h, prev, baseTip + 6 + i, (i % 2) == 0);
            BlockIndexDerivedEntry d = MakeDerived(uint256(6 + i), 2000 + i); // trust continues UP
            BlockIndexTipAppend a; a.record = r; a.derived = d;
            BOOST_REQUIRE(tip.Append(a, baseTip + 6 + i, NULL) == BLOCK_INDEX_TIP_OK);
            prev = h;
        }
        BOOST_CHECK_EQUAL(tip.TipHeight(), baseTip + 10);
    }
    { BlockIndexTipAuthority tip; BOOST_REQUIRE(BlockIndexTipAuthority::Open(dir, 23, &tip, NULL));
      BOOST_CHECK_EQUAL(tip.TipHeight(), baseTip + 10); }
    printf("R2C PASS v3 restart accumulates\n");
}

// R2-D: side branch then reorg (truncate) -> v3 rebuild validates.
BOOST_AUTO_TEST_CASE(r2d_v3_side_and_truncate)
{
    const std::string dir = MakeTempDir(); const int baseTip = 200;
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 24, 50, baseTip, &tip, NULL));
        uint256 fin; BuildChain(tip, 4, &fin, uint256(0x02UL), baseTip);
        // side record (not active)
        BlockIndexTipAppend s; s.record = MakeRecord(uint256(0x5EEDUL), fin, baseTip + 5, false);
        s.derived = MakeDerived(uint256(99), 4242);
        BOOST_REQUIRE(tip.Append(s, -1, NULL) == BLOCK_INDEX_TIP_OK);
        BOOST_REQUIRE(tip.TruncateActiveTo(baseTip + 2, NULL) == BLOCK_INDEX_TIP_OK);
    }
    BlockIndexTipAuthority re;
    BOOST_REQUIRE_MESSAGE(BlockIndexTipAuthority::Open(dir, 24, &re, NULL),
                          "R2D: v3 side+truncate store MUST reopen");
    printf("R2D PASS v3 side branch + truncate reopen\n");
}

// R2-E: v3 meta with a truncated body MUST fail closed (no guessing).
BOOST_AUTO_TEST_CASE(r2e_v3_truncated_meta_fail_closed)
{
    const std::string dir = MakeTempDir(); const int baseTip = 150;
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 25, 10, baseTip, &tip, NULL));
        uint256 fin; BuildChain(tip, 2, &fin, uint256(0x03UL), baseTip);
    }
    std::vector<unsigned char> m = ReadRawFile(dir + "/blockindex_tip/tip.meta");
    BOOST_REQUIRE(m.size() >= 140);
    m.resize(104); // keep version=3 header but drop the v2/v3 tail -> must fail closed
    WriteRawFile(dir + "/blockindex_tip/tip.meta", m);
    BlockIndexTipAuthority re; std::string err;
    BOOST_REQUIRE_MESSAGE(!BlockIndexTipAuthority::Open(dir, 25, &re, &err),
                          "R2E: truncated v3 meta MUST fail closed");
    printf("R2E PASS truncated v3 meta fail-closed\n");
}

// R2-F: operator-invalid on a v3 tip keeps v3 and persists.
BOOST_AUTO_TEST_CASE(r2f_v3_operator_invalid_keeps_v3)
{
    const std::string dir = MakeTempDir(); const int baseTip = 120; const uint256 hX(0x77UL);
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 26, 5, baseTip, &tip, NULL));
        uint256 fin; BuildChain(tip, 3, &fin, uint256(0x04UL), baseTip);
        BOOST_REQUIRE(tip.SetOperatorInvalid(hX, true, NULL) == BLOCK_INDEX_TIP_OK);
        unsigned char g[32]; uint32_t v = 0; ReadMetaDigest(dir, g, &v);
        BOOST_CHECK_EQUAL(v, 3u); // must stay v3
    }
    BlockIndexTipAuthority re;
    BOOST_REQUIRE(BlockIndexTipAuthority::Open(dir, 26, &re, NULL));
    BOOST_CHECK(re.IsOperatorInvalid(hX));
    printf("R2F PASS v3 operator-invalid persists\n");
}

// R2-G: after a reorg the persisted digest equals the oracle over the RESULTING
// committed vectors (records/derived unchanged, active truncated, fence bumped).
BOOST_AUTO_TEST_CASE(r2g_v3_oracle_after_reorg)
{
    const std::string dir = MakeTempDir();
    const int baseTip = 100; const uint64_t baseRec = 20; const uint32_t N = 6;
    std::vector<BlockIndexRecord> recs; std::vector<BlockIndexDerivedEntry> ders;
    uint256 prev(0xC0DEUL);
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 27, baseRec, baseTip, &tip, NULL));
        for (uint32_t i = 0; i < N; ++i)
        {
            uint256 h = uint256(0x2222UL + i);
            BlockIndexRecord r = MakeRecord(h, prev, baseTip + 1 + (int)i, (i % 2) == 0);
            BlockIndexDerivedEntry d = MakeDerived(uint256(i + 5), 700 + i);
            BlockIndexTipAppend a; a.record = r; a.derived = d;
            BOOST_REQUIRE(tip.Append(a, baseTip + 1 + (int)i, NULL) == BLOCK_INDEX_TIP_OK);
            recs.push_back(r); ders.push_back(d); prev = h;
        }
        BOOST_REQUIRE(tip.TruncateActiveTo(baseTip + 3, NULL) == BLOCK_INDEX_TIP_OK);
    }
    std::vector<BlockIndexId> actsAfter;
    for (int i = 0; i < 3; ++i) actsAfter.push_back(baseRec + i + 1);
    unsigned char got[32]; uint32_t ver = 0; ReadMetaDigest(dir, got, &ver);
    BOOST_REQUIRE_EQUAL(ver, 3u);
    unsigned char want[32]; OracleDigestV3(recs, ders, actsAfter, 1 /*fence bumped*/, want);
    BOOST_CHECK_MESSAGE(memcmp(got, want, 32) == 0,
                        "R2G: post-reorg v3 digest MUST equal the oracle over the resulting vectors");
    BlockIndexTipAuthority re;
    BOOST_REQUIRE(BlockIndexTipAuthority::Open(dir, 27, &re, NULL));
    BOOST_CHECK_EQUAL(re.TipHeight(), baseTip + 3);
    printf("R2G PASS v3 oracle after reorg\n");
}

BOOST_AUTO_TEST_SUITE_END()