#include <boost/test/unit_test.hpp>

#include "blockindex_tip.h"
#include "util.h"

#include <boost/filesystem.hpp>

#include <cstdio>
#include <cstdlib>
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

BOOST_AUTO_TEST_SUITE_END()