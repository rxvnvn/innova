#include <boost/test/unit_test.hpp>

#include "blockindex_tip.h"
#include "main.h"
#include "blockindex_authoritative_startup.h"
#include "util.h"

#include <boost/filesystem.hpp>

#include <cstdio>
#include <cstdlib>
#include <set>
#include <cstring>
#include <openssl/sha.h>
#include <boost/filesystem.hpp>
#include <string>
#include <vector>

#include "blockindex_tip.h"
#include "blockindex_tip_bounded_window.h"
#include "blockindex_hashindex.h"

namespace fs = boost::filesystem;

// =====================================================================
// P1 — tip: dedicated persistent mutable tip authority.
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

BOOST_AUTO_TEST_SUITE(tip_tests)

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
        FILE* f = fopen((dir + "/tip/tip-records.dat").c_str(), "ab");
        BOOST_REQUIRE(f != NULL);
        BOOST_REQUIRE(fwrite(&enc[0], 1, enc.size(), f) == enc.size());
        fclose(f);

        std::string aenc;
        BOOST_REQUIRE(EncodeBlockIndexActiveEntry(300u + 3 + 1, &aenc, NULL)); // id=304
        FILE* fa = fopen((dir + "/tip/tip-active.dat").c_str(), "ab");
        BOOST_REQUIRE(fa != NULL);
        BOOST_REQUIRE(fwrite(aenc.data(), 1, aenc.size(), fa) == aenc.size());
        fclose(fa);

        BlockIndexDerivedEntry bogusD = MakeDerived(uint256(0xFFFFUL), 777);
        std::vector<unsigned char> denc;
        BOOST_REQUIRE(EncodeBlockIndexDerivedEntry(bogusD, &denc, NULL));
        FILE* fd = fopen((dir + "/tip/tip-derived.dat").c_str(), "ab");
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

// IFCN#9 — PENDING-WINDOW CONSENSUS CONTRACT.
// While -tipcommitbatch>1 the authority holds an in-memory append window that is
// NOT yet durably published (state A of the h=50488 reproducer). The durable
// height lookup must stay blind to it, and the LOGICAL one must see it: the
// stake-kernel modifier walk resolves the active successor by height, so a
// durable-only view stopped it at the durable tip and the kernel input then
// depended on the publication state.
BOOST_AUTO_TEST_CASE(t10d_logical_active_by_height_sees_pending_window)
{
    const std::string dir = MakeTempDir();
    const int baseTip = 4000;
    // Enable the IBD append window exactly as -tipcommitbatch does in production.
    const bool hadBatch = mapArgs.count("-tipcommitbatch") != 0;
    const bool hadAlways = mapArgs.count("-tipcommitbatchalways") != 0;
    const std::string oldBatch = hadBatch ? mapArgs["-tipcommitbatch"] : std::string();
    const std::string oldAlways = hadAlways ? mapArgs["-tipcommitbatchalways"] : std::string();
    mapArgs["-tipcommitbatch"] = "16";
    mapArgs["-tipcommitbatchalways"] = "1";

    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 41, 900, baseTip, &tip, NULL));

        const size_t n = 3;   // < batch, so the window stays UNPUBLISHED
        std::vector<uint256> hsh(n);
        uint256 prev = uint256(0x7a1aUL);
        for (size_t i = 0; i < n; ++i)
        {
            BlockIndexTipAppend a;
            a.record = MakeRecord(uint256(0x6100UL + i), prev, baseTip + (int)i + 1, true);
            a.derived = MakeDerived(uint256(9), 4001 + i);
            std::string err;
            BOOST_REQUIRE(tip.Append(a, baseTip + (int)i + 1, &err) == BLOCK_INDEX_TIP_OK);
            hsh[i] = a.record.hash;
            prev = a.record.hash;
        }
        const uint64_t pend = tip.PendingCount();
        printf("T10D pending_window=%llu logical_tip=%d durable_tip=%d\n",
               (unsigned long long)pend, tip.TipHeight(), tip.GetTip().height);
        BOOST_REQUIRE_EQUAL(pend, (uint64_t)n);
        BOOST_REQUIRE_EQUAL(tip.TipHeight(), baseTip + (int)n);      // logical
        BOOST_REQUIRE(tip.GetTip().status == BLOCK_INDEX_TIP_NOT_FOUND); // durable: nothing published

        // DURABLE contract: the unpublished window is invisible.
        for (size_t i = 0; i < n; ++i)
        {
            const BlockIndexTipRead d = tip.LookupActiveByHeight(baseTip + (int)i + 1, NULL);
            BOOST_CHECK_MESSAGE(d.status == BLOCK_INDEX_TIP_NOT_FOUND,
                                "durable lookup leaked an unpublished active height "
                                    << baseTip + (int)i + 1);
        }
        // LOGICAL contract (the fix): same heights resolve, active, exact identity.
        for (size_t i = 0; i < n; ++i)
        {
            std::string lerr;
            const BlockIndexTipRead l = tip.LookupActiveByHeightLogical(baseTip + (int)i + 1, &lerr);
            BOOST_REQUIRE_MESSAGE(l.status == BLOCK_INDEX_TIP_OK,
                                  "logical lookup missed buffered height "
                                      << baseTip + (int)i + 1 << ": " << lerr);
            BOOST_CHECK(l.active);
            BOOST_CHECK_EQUAL(l.height, baseTip + (int)i + 1);
            BOOST_CHECK(l.record.hash == hsh[i]);
        }
        BOOST_CHECK(tip.LookupActiveByHeightLogical(baseTip, NULL).status == BLOCK_INDEX_TIP_NOT_FOUND);
        BOOST_CHECK(tip.LookupActiveByHeightLogical(baseTip + (int)n + 1, NULL).status == BLOCK_INDEX_TIP_NOT_FOUND);

        // Publication: both views converge on the same active chain.
        std::string ferr;
        BOOST_REQUIRE(tip.FlushPendingAppends(&ferr) == BLOCK_INDEX_TIP_OK);
        BOOST_REQUIRE_EQUAL(tip.PendingCount(), (uint64_t)0);
        for (size_t i = 0; i < n; ++i)
        {
            const BlockIndexTipRead d = tip.LookupActiveByHeight(baseTip + (int)i + 1, NULL);
            const BlockIndexTipRead l = tip.LookupActiveByHeightLogical(baseTip + (int)i + 1, NULL);
            BOOST_REQUIRE(d.status == BLOCK_INDEX_TIP_OK);
            BOOST_REQUIRE(l.status == BLOCK_INDEX_TIP_OK);
            BOOST_CHECK(d.record.hash == l.record.hash);
            BOOST_CHECK(d.record.hash == hsh[i]);
        }
        printf("T10D PASS logical-height-lookup crosses the pending window\n");
        tip.Close();
    }

    if (hadBatch) mapArgs["-tipcommitbatch"] = oldBatch; else mapArgs.erase("-tipcommitbatch");
    if (hadAlways) mapArgs["-tipcommitbatchalways"] = oldAlways; else mapArgs.erase("-tipcommitbatchalways");
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
        const fs::path rp = fs::path(dir) / "tip" / "tip-records.dat";
        const fs::path dp = fs::path(dir) / "tip" / "tip-derived.dat";
        const fs::path ap = fs::path(dir) / "tip" / "tip-active.dat";
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
    return dir + "/tip/tip-invalid.dat";
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
    std::vector<unsigned char> meta = ReadRawFile(dir + "/tip/tip.meta");
    BOOST_REQUIRE(meta.size() >= 104);
    meta.resize(104);
    meta[0] = 1; meta[1] = 0; meta[2] = 0; meta[3] = 0; // version = 1 (LE32)
    WriteRawFile(dir + "/tip/tip.meta", meta);
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
    std::vector<unsigned char> meta = ReadRawFile(dir + "/tip/tip.meta");
    BOOST_REQUIRE(meta.size() >= 104);
    meta[0] = 99; meta[1] = 0; meta[2] = 0; meta[3] = 0; // unknown future version
    WriteRawFile(dir + "/tip/tip.meta", meta);
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
    return dir + "/tip/tip-records.dat";
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
// SAME committed vectors with OpenSSL, entirely outside tip.cpp, and
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
    std::vector<unsigned char> m = ReadRawFile(dir + "/tip/tip.meta");
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
    std::vector<unsigned char> m = ReadRawFile(dir + "/tip/tip.meta");
    BOOST_REQUIRE(m.size() >= 140);
    m.resize(104); // keep version=3 header but drop the v2/v3 tail -> must fail closed
    WriteRawFile(dir + "/tip/tip.meta", m);
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

// =====================================================================
// Repair #3 — bounded tip-authority memory regressions.
// =====================================================================
static bool R3DecU64(const unsigned char* d, size_t n, uint64_t* out, std::string* e)
{
    if (n < 8) { if (e) *e = "short"; return false; }
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= (uint64_t)d[i] << (8 * i);
    *out = v;
    return true;
}

// R3-A: the bounded window itself — ring strictly bounded by its cap, pread
// fallback for slots below the ring, byte-LRU budget, boundaries and
// out-of-range. Proves tip-authority RAM is bounded INDEPENDENT of the number
// of committed entries.
BOOST_AUTO_TEST_CASE(r3a_window_bounded_pread_lru)
{
    const std::string dir = MakeTempDir();
    const std::string path = dir + "/w.dat";
    const uint64_t N = 100;
    std::vector<unsigned char> raw;
    for (uint64_t i = 0; i < N; ++i)
        for (int k = 0; k < 8; ++k) raw.push_back((unsigned char)((i >> (8 * k)) & 0xff));
    WriteRawFile(path, raw);
    TipStoreWindow<uint64_t> w(path, 0, 8, 4 /*ringCap*/, 32 /*lruBytes = 4 entries*/, R3DecU64);
    std::string e;
    BOOST_REQUIRE(w.InitializeCount(N, &e));
    BOOST_CHECK_EQUAL((uint64_t)w.Stats().ringResident, 4u);   // ring bounded by cap
    uint64_t v = 0;
    BOOST_REQUIRE(w.Get(0, &v, &e));  BOOST_CHECK_EQUAL(v, 0u);   // pread (below ring)
    BOOST_REQUIRE(w.Get(50, &v, &e)); BOOST_CHECK_EQUAL(v, 50u);  // pread/LRU
    BOOST_REQUIRE(w.Get(99, &v, &e)); BOOST_CHECK_EQUAL(v, 99u);  // ring
    BOOST_CHECK(!w.Get(100, &v, &e));                             // out of committed range
    uint64_t sum = 0;
    BOOST_REQUIRE(w.StreamVisit(0, N, [&](uint64_t, const uint64_t& x) { sum += x; return true; }, &e));
    BOOST_CHECK_EQUAL(sum, (N * (N - 1)) / 2);
    BOOST_CHECK(w.Stats().ringResident <= 4u);
    BOOST_CHECK(w.Stats().lruBytes <= w.Stats().lruBytesCap);
    printf("R3A PASS bounded window (ring<=cap, pread, LRU, boundaries)\n");
}

// R3-B: by-hash / by-height lookups, parent + next-active navigation, active vs
// side-branch membership, out-of-range NOT_FOUND, and restart equivalence.
BOOST_AUTO_TEST_CASE(r3b_lookup_and_membership)
{
    const std::string dir = MakeTempDir(); const int baseTip = 700; const uint64_t baseRec = 300;
    const uint256 hA[4] = {uint256(0xA1UL), uint256(0xA2UL), uint256(0xA3UL), uint256(0xA4UL)};
    const uint256 side(0x51DEUL);
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 31, baseRec, baseTip, &tip, NULL));
        uint256 prev = tip.TipHash();
        for (int i = 0; i < 4; ++i)
        {
            BlockIndexRecord r = MakeRecord(hA[i], prev, baseTip + 1 + i, false);
            BlockIndexTipAppend a; a.record = r; a.derived = MakeDerived(uint256(i + 1), 100 + i);
            BOOST_REQUIRE(tip.Append(a, baseTip + 1 + i, NULL) == BLOCK_INDEX_TIP_OK);
            prev = hA[i];
        }
        {   // side record at height baseTip+3 (competes with A3), not active
            BlockIndexRecord r = MakeRecord(side, hA[1], baseTip + 3, false);
            BlockIndexTipAppend a; a.record = r; a.derived = MakeDerived(uint256(4), 200);
            BOOST_REQUIRE(tip.Append(a, -1, NULL) == BLOCK_INDEX_TIP_OK);
        }
        BlockIndexTipRead g = tip.LookupByHash(hA[2], NULL);
        BOOST_CHECK(g.status == BLOCK_INDEX_TIP_OK); BOOST_CHECK(g.active);
        BOOST_CHECK_EQUAL(g.height, baseTip + 3);
        BlockIndexTipRead s = tip.LookupByHash(side, NULL);
        BOOST_CHECK(s.status == BLOCK_INDEX_TIP_OK); BOOST_CHECK(!s.active);
        BlockIndexTipRead hh = tip.LookupActiveByHeight(baseTip + 1, NULL);
        BOOST_CHECK(hh.status == BLOCK_INDEX_TIP_OK); BOOST_CHECK(hh.record.hash == hA[0]);
        BlockIndexTipRead p = tip.LookupParent(hA[1], NULL);
        BOOST_CHECK(p.status == BLOCK_INDEX_TIP_OK); BOOST_CHECK(p.record.hash == hA[0]);
        BlockIndexTipRead n = tip.LookupNextActive(hA[0], NULL);
        BOOST_CHECK(n.status == BLOCK_INDEX_TIP_OK); BOOST_CHECK(n.record.hash == hA[1]);
        BOOST_CHECK(tip.LookupActiveByHeight(baseTip + 40, NULL).status == BLOCK_INDEX_TIP_NOT_FOUND);
        BOOST_CHECK(tip.LookupByHash(uint256(0xDEADUL), NULL).status == BLOCK_INDEX_TIP_NOT_FOUND);
        tip.Close();
    }
    BlockIndexTipAuthority re;
    BOOST_REQUIRE(BlockIndexTipAuthority::Open(dir, 31, &re, NULL));
    BOOST_CHECK(re.LookupByHash(hA[2], NULL).active);
    BOOST_CHECK(!re.LookupByHash(side, NULL).active);
    BOOST_CHECK(re.LookupActiveByHeight(baseTip + 4, NULL).record.hash == hA[3]);
    printf("R3B PASS lookups + active/side + restart\n");
}

// R3-D: the durable hash index is DERIVED. Deleting the index dir + marker must
// NOT change the authoritative chain: Open reconciles it from the committed
// records and lookups stay correct.
BOOST_AUTO_TEST_CASE(r3d_index_reconcile_missing)
{
    const std::string dir = MakeTempDir(); const int baseTip = 900;
    const uint256 hA(0xAA01UL), hB(0xAA02UL);
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 33, 77, baseTip, &tip, NULL));
        BlockIndexRecord r1 = MakeRecord(hA, tip.TipHash(), baseTip + 1, false);
        BlockIndexTipAppend a1; a1.record = r1; a1.derived = MakeDerived(uint256(1), 1);
        BOOST_REQUIRE(tip.Append(a1, baseTip + 1, NULL) == BLOCK_INDEX_TIP_OK);
        BlockIndexRecord r2 = MakeRecord(hB, hA, baseTip + 2, false);
        BlockIndexTipAppend a2; a2.record = r2; a2.derived = MakeDerived(uint256(2), 2);
        BOOST_REQUIRE(tip.Append(a2, baseTip + 2, NULL) == BLOCK_INDEX_TIP_OK);
        tip.Close();
    }
    boost::system::error_code ec;
    boost::filesystem::remove_all((dir + "/tip/tip-hashindex").c_str(), ec);
    boost::filesystem::remove((dir + "/tip/tip-hashindex.marker").c_str(), ec);
    BlockIndexTipAuthority re; std::string err;
    BOOST_REQUIRE_MESSAGE(BlockIndexTipAuthority::Open(dir, 33, &re, &err),
                          "R3D: reconcile after index deletion failed: " << err);
    BOOST_CHECK_EQUAL(re.TipHeight(), baseTip + 2);   // tip.meta authority unchanged
    BlockIndexTipRead g = re.LookupByHash(hA, NULL);
    BOOST_CHECK(g.status == BLOCK_INDEX_TIP_OK); BOOST_CHECK_EQUAL(g.height, baseTip + 1);
    BOOST_CHECK(re.LookupByHash(hB, NULL).status == BLOCK_INDEX_TIP_OK);
    printf("R3D PASS derived index rebuild/reconcile\n");
}

// R3-E: a corrupted committed record must fail closed on Open — no silent
// acceptance and no alternative chain.
BOOST_AUTO_TEST_CASE(r3e_corrupt_records_fail_closed)
{
    const std::string dir = MakeTempDir(); const int baseTip = 1100;
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 34, 5, baseTip, &tip, NULL));
        uint256 fin; BuildChain(tip, 4, &fin, tip.TipHash(), baseTip);
        tip.Close();
    }
    BOOST_REQUIRE(FlipByteInFile(TipRecordsPath(dir), 64));
    BlockIndexTipAuthority re; std::string err;
    BOOST_CHECK_MESSAGE(!BlockIndexTipAuthority::Open(dir, 34, &re, &err),
                        "R3E: corrupted committed record MUST fail closed");
    printf("R3E PASS corrupt records fail-closed\n");
}

// =====================================================================
// Repair #3 FIX-2 — derived hash-index durability / reconcile survival.
//
// The LevelDB hash->RecordId side index is DERIVED state, never authority.
// These tests drive the REAL tipDir/hashindex dir + tip-hashindex.marker,
// then corrupt the derived state in ways a crash may leave behind, and
// REQUIRE Open to reconcile (truncate / forward-repair / full rebuild)
// so every committed-range hash lookup resolves to the CORRECT RecordId.
// The single durable commit point (tip.meta) is proven unchanged each time.
// =====================================================================
static std::string TipHashIndexPath(const std::string& dir)
{
    // The BlockIndexHashIndex wrapper takes the PARENT dir and appends
    // "hashindex" itself; this returns that parent (the tip dir).
    return dir + "/tip";
}
static std::string TipHashIndexMarkerPath(const std::string& dir)
{
    return dir + "/tip/tip-hashindex.marker";
}

// Repair #3 marker helper: rewrite ONLY the indexedCount field of an existing
// marker file (magic/version preserved) — simulates an interrupted reconcile
// that durably advanced the marker before finishing the LevelDB writes.
static void PatchHashIndexMarkerCount(const std::string& dir, uint64_t count)
{
    std::vector<unsigned char> b = ReadRawFile(TipHashIndexMarkerPath(dir));
    BOOST_REQUIRE_MESSAGE(b.size() == 16,
                          "marker must be 16 bytes before patch; got " << b.size());
    for (int i = 0; i < 8; ++i)
        b[8 + i] = (unsigned char)((count >> (8 * i)) & 0xff);
    WriteRawFile(TipHashIndexMarkerPath(dir), b);
}

// R3-F (scenario: index entry ABOVE the committed tip.meta record count —
// index Put was durable but tip.meta publication crashed). Reopen must serve
// lookups over the COMMITTED range exactly (the stale entry must never win),
// and the committed tip must be unchanged.
BOOST_AUTO_TEST_CASE(r3f_index_entry_above_committed_count_is_ignored)
{
    const std::string dir = MakeTempDir(); const int baseTip = 1500;
    uint256 committedTip0;
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 41, 10, baseTip, &tip, NULL));
        uint256 fin; BuildChain(tip, 2, &fin, uint256(0xF1UL), baseTip);
        committedTip0 = fin;
        tip.Close();
    }
    // Simulate: one MORE index entry was durable than the committed records
    // (index Put succeeded for the next block, tip.meta commit did not).
    BlockIndexId phantomId = 10 + 2 + 1; // baseRecordCount + slot(2) + 1
    uint256 phantomHash((unsigned long)0x999999UL);
    {
        // tip_dir / "hashindex": TipSideIndex passes tipDir, the wrapper
        // appends PROJECT/BLOCK_INDEX_HASHINDEX_DIR_NAME itself.
        BlockIndexHashIndex side;
        std::string oerr;
        BOOST_REQUIRE_MESSAGE(side.OpenInternal(TipHashIndexPath(dir), 0, false, false, &oerr),
                              "R3-F: writable side-index open failed: " << oerr);
        BOOST_REQUIRE(side.Put(phantomHash, phantomId, &oerr));
        side.Close();
    }
    BlockIndexTipAuthority re; std::string err;
    BOOST_REQUIRE_MESSAGE(BlockIndexTipAuthority::Open(dir, 41, &re, &err),
                          "R3-F: reconcile above-committed-count failed: " << err);
    // The stale (uncommitted) entry must NOT answer lookups.
    BlockIndexTipRead p = re.LookupByHash(phantomHash, NULL);
    BOOST_CHECK_MESSAGE(p.status == BLOCK_INDEX_TIP_NOT_FOUND,
                        "R3-F: stale index entry above the committed count must be truncated, got status "
                        << (int)p.status);
    // Committed-range lookups must still be exact.
    BlockIndexTipRead h1 = re.LookupByHash(uint256(0x1111UL), NULL);
    BOOST_CHECK_MESSAGE(h1.status == BLOCK_INDEX_TIP_OK && h1.height == baseTip + 1,
                        "R3-F: committed slot 0 broke");
    BlockIndexTipRead h2 = re.LookupByHash(uint256(0x1112UL), NULL);
    BOOST_CHECK_MESSAGE(h2.status == BLOCK_INDEX_TIP_OK && h2.height == baseTip + 2,
                        "R3-F: committed slot 1 broke");
    BOOST_CHECK_MESSAGE(re.TipHash() == committedTip0,
                        "R3-F: committed tip must be unchanged");
}

// R3-G (scenario: PARTIAL index publication — a MIDDLE entry missing while the
// marker claims the full count). Reconcile must detect the hole and REBUILD,
// and the middle hash must resolve to the CORRECT RecordId afterward.
// This is the direct test of the middle-entry hole the O(1) first/last spot
// check historically missed (see also r3j-middle-corruption repair gate).
BOOST_AUTO_TEST_CASE(r3g_partial_index_publication_middle_missing_rebuilt)
{
    const std::string dir = MakeTempDir(); const int baseTip = 2100;
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 42, 10, baseTip, &tip, NULL));
        uint256 fin; BuildChain(tip, 4, &fin, uint256(0xF2UL), baseTip); // slots 0..3
        BOOST_REQUIRE_EQUAL(tip.TipRecordCount(), 4u);
        tip.Close();
    }
    // surgically DELETE the middle hash entry (slot 1 = hash 0x1112), keeping
    // the marker at the full count so neither boundary probe detects the hole.
    const uint256 hMiddle(0x1112UL);
    {
        BlockIndexHashIndex side;
        std::string oerr;
        BOOST_REQUIRE_MESSAGE(side.OpenInternal(TipHashIndexPath(dir), 0, false, false, &oerr),
                              "R3-G: writable side-index open failed: " << oerr);
        std::string derr;
        BOOST_REQUIRE_MESSAGE(side.Delete(hMiddle, 10 + 1 + 1 /* true id of slot 1 */, &derr),
                              "R3-G: delete failed: " << derr);
        side.Close();
    }
    BlockIndexTipAuthority re; std::string err;
    BOOST_REQUIRE_MESSAGE(BlockIndexTipAuthority::Open(dir, 42, &re, &err),
                          "R3-G: reconcile with a middle hole failed: " << err);
    // After reconcile the middle hash MUST resolve to its OWN record (slot 1,
    // height baseTip+2) — never CORRUPT / never NOT_FOUND.
    BlockIndexTipRead m = re.LookupByHash(hMiddle, NULL);
    BOOST_CHECK_MESSAGE(m.status == BLOCK_INDEX_TIP_OK, "R3-G: missing middle entry not rebuilt, status "
                        << (int)m.status);
    BOOST_CHECK_MESSAGE(m.record.hash == hMiddle, "R3-G: middle entry resolved to the wrong record");
    BOOST_CHECK_MESSAGE(m.height == baseTip + 2, "R3-G: middle entry resolved to the wrong height");
    BlockIndexTipRead g = re.LookupByHash(uint256(0x1111UL), NULL);
    BOOST_CHECK_MESSAGE(g.status == BLOCK_INDEX_TIP_OK && g.height == baseTip + 1,
                        "R3-G: slot 0 broke after reconcile");
    BlockIndexTipRead l = re.LookupByHash(uint256(0x1114UL), NULL);
    BOOST_CHECK_MESSAGE(l.status == BLOCK_INDEX_TIP_OK && l.height == baseTip + 4,
                        "R3-G: slot 3 (last) broke after reconcile");
}

// R3-H (scenario: a MIDDLE index entry was corrupted to point at the WRONG
// RecordId, with count AND both boundary probes intact). LookupByHash must
// never silently return the wrong record. NOTE: the current reconcile only
// spot-checks the FIRST and LAST slots, so this test EXPECTS Open to succeed
// but the launch/gate for the narrow middle-verification fix — asserted below.
BOOST_AUTO_TEST_CASE(r3h_middle_entry_wrong_id_detected)
{
    const std::string dir = MakeTempDir(); const int baseTip = 2500;
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 43, 10, baseTip, &tip, NULL));
        uint256 fin; BuildChain(tip, 4, &fin, uint256(0xF3UL), baseTip); // slots 0..3
        tip.Close();
    }
    // Corrupt slot 1 -> wrong id (slot 2's id), count unchanged, boundaries intact.
    const uint256 hMiddle(0x1112UL);           // baseRecordCount(10)+1 -> slot 1
    const uint256 hWrong(0x1111UL);            // baseRecordCount(10)+0 -> slot 0
    BlockIndexId wrongId = 10 + 0 + 1;          // the WRONG committed id (slot 0)
    {
        BlockIndexHashIndex side;
        std::string oerr;
        BOOST_REQUIRE_MESSAGE(side.OpenInternal(TipHashIndexPath(dir), 0, false, false, &oerr),
                              "R3-H: writable side-index open failed: " << oerr);
        std::string derr;
        BOOST_REQUIRE_MESSAGE(side.Delete(hMiddle, 10 + 1 + 1 /* true id of slot 1 */, &derr),
                              "R3-H: delete failed: " << derr);
        BOOST_REQUIRE_MESSAGE(side.Put(hMiddle, wrongId, &derr), "R3-H: put failed: " << derr);
        side.Close();
    }
    {
        BlockIndexTipAuthority re; std::string err;
        BOOST_REQUIRE_MESSAGE(BlockIndexTipAuthority::Open(dir, 43, &re, &err),
                              "R3-H: reconcile with a wrong middle id failed: " << err);
        // The WRONG mapping must NOT win: querying 0x1112 must return the true
        // slot-1 record. If reconcile caught the wrong id, it rebuilds and Put
        // the correct mapping over it (LevelDB overwrite = same key path).
        BlockIndexTipRead m = re.LookupByHash(hMiddle, NULL);
        BOOST_CHECK_MESSAGE(m.status == BLOCK_INDEX_TIP_OK, "R3-H: middle lookup failed after reconcile, status "
                            << (int)m.status);
        BOOST_CHECK_MESSAGE(m.record.hash == hMiddle, "R3-H: MIDDLE ENTRY WRONG-ID NOT CAUGHT — the corrupt index "
                            "mapped " << hMiddle.ToString() << " to another record (silent wrong result)");
        BOOST_CHECK_MESSAGE(m.height == baseTip + 2, "R3-H: middle entry resolved to the wrong height after corrupt index");
    }
}

// R3-I: bundled derived-index durability matrix — each scenario corrupts the
// REAL <tipDir>/hashindex LevelDB (or the sidecar marker) in a way a crash
// between the derived index Put and the tip.meta commit can leave behind, then
// REQUIRES a reopen to reconcile so every committed-range lookup stays exact
// and the committed tip (tip.meta, the single commit point) never moves.
//   s1 stale entries after reorganization/truncation (TipRecordCount N, marker N)
//   s2 same-count-but-different-content (all four committed entries are WRONG)
//   s3 malformed marker (garbage bytes) forces the deterministic rebuild
//   s4 interrupted reconcile (marker faithfully advanced, LevelDB torn)
//   s5 repeated restart A/B: correct BEFORE corruption == identical AFTER
BOOST_AUTO_TEST_CASE(r3i_index_stale_samecount_marker_rebuild_matrix)
{
    const std::string dir = MakeTempDir(); const int baseTip = 3100;
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 44, 10, baseTip, &tip, NULL));
        uint256 fin; BuildChain(tip, 4, &fin, uint256(0xF4UL), baseTip); // slots 0..3
        tip.Close();
    }
    const BlockIndexId base = 10;
    // -- s1: 4 stale entries for blocks the reorg dropped (same count N=4).
    //    Their lookups would be a silent fork of history unless truncated.
    {
        BlockIndexHashIndex side; std::string oerr, werr;
        BOOST_REQUIRE_MESSAGE(side.OpenInternal(TipHashIndexPath(dir), 0, false, false, &oerr),
                              "R3-I s1: open failed: " << oerr);
        for (int i = 0; i < 4; ++i)
        {
            uint256 ph((unsigned long)(0xFFF000UL + i));
            BOOST_REQUIRE_MESSAGE(side.Put(ph, base + 4 + 1 + (BlockIndexId)i, &werr),
                                  "R3-I s1 put: " << werr);
        }
        side.Close();
    }
    {
        BlockIndexTipAuthority re; std::string oerr;
        BOOST_REQUIRE_MESSAGE(BlockIndexTipAuthority::Open(dir, 44, &re, &oerr),
                              "R3-I s1: reopen with stale truncation entries failed: " << oerr);
        for (int i = 0; i < 4; ++i)
        {
            uint256 ph((unsigned long)(0xFFF000UL + i));
            BOOST_CHECK_MESSAGE(re.LookupByHash(ph, NULL).status == BLOCK_INDEX_TIP_NOT_FOUND,
                                "R3-I s1: stale reorged-out hash " << ph.ToString()
                                << " must be truncated out of the derived index");
        }
        BOOST_CHECK_MESSAGE(re.TipHash() == uint256(0x1114UL), "R3-I s1: committed tip moved");
        re.Close();
    }
    // -- s2: same-count-but-different-content: EVERY index entry re-points at
    //    a wrong (still-committed) id; boundaries included so no probe helps.
    const uint256 hs[4] = {uint256(0x1111UL), uint256(0x1112UL), uint256(0x1113UL), uint256(0x1114UL)};
    {
        BlockIndexHashIndex side; std::string oerr, werr;
        BOOST_REQUIRE_MESSAGE(side.OpenInternal(TipHashIndexPath(dir), 0, false, false, &oerr),
                              "R3-I s2: open failed: " << oerr);
        for (int i = 0; i < 4; ++i)
        {
            BOOST_REQUIRE_MESSAGE(side.Delete(hs[i], base + (BlockIndexId)i + 1, &werr),
                                  "R3-I s2 delete: " << werr);
            BOOST_REQUIRE_MESSAGE(side.Put(hs[i], base + (BlockIndexId)((i + 1) % 4) + 1, &werr),
                                  "R3-I s2 put: " << werr);  // next slot's id
        }
        side.Close();
    }
    {
        BlockIndexTipAuthority re; std::string oerr;
        BOOST_REQUIRE_MESSAGE(BlockIndexTipAuthority::Open(dir, 44, &re, &oerr),
                              "R3-I s2: reopen with same-count-different-content failed: " << oerr);
        for (int i = 0; i < 4; ++i)
        {
            BlockIndexTipRead rd = re.LookupByHash(hs[i], NULL);
            BOOST_CHECK_MESSAGE(rd.status == BLOCK_INDEX_TIP_OK && rd.record.hash == hs[i] &&
                                rd.height == baseTip + 1 + i,
                                "R3-I s2: committed entry " << hs[i].ToString()
                                << " resolved to the wrong record/height (count agree, boundaries wrong)" );
        }
        re.Close();
    }
    // -- s3: malformed marker (garbage) must force the deterministic rebuild.
    {
        std::vector<unsigned char> junk;
        for (int i = 0; i < 16; ++i) junk.push_back((unsigned char)(0xA0 + i));
        WriteRawFile(TipHashIndexMarkerPath(dir), junk);
    }
    {
        BlockIndexTipAuthority re; std::string oerr;
        BOOST_REQUIRE_MESSAGE(BlockIndexTipAuthority::Open(dir, 44, &re, &oerr),
                              "R3-I s3: reopen with a malformed marker failed: " << oerr);
        BlockIndexTipRead m = re.LookupByHash(uint256(0x1112UL), NULL);
        BOOST_CHECK_MESSAGE(m.status == BLOCK_INDEX_TIP_OK && m.record.hash == uint256(0x1112UL) &&
                            m.height == baseTip + 2,
                            "R3-I s3: rebuild after a malformed marker broke the middle lookup");
        re.Close();
    }
    // -- s4: interrupted reconcile — marker faithfully says N (=4), LevelDB torn
    //    (this is s2's corrupt state, but the marker now MATCHES the claim).
    PatchHashIndexMarkerCount(dir, 4);
    {
        BlockIndexTipAuthority re; std::string oerr;
        BOOST_REQUIRE_MESSAGE(BlockIndexTipAuthority::Open(dir, 44, &re, &oerr),
                              "R3-I s4: reopen with a torn index under a matching marker failed: " << oerr);
        for (int i = 0; i < 4; ++i)
        {
            const uint256 h((unsigned long)(0x1111UL + i));
            BlockIndexTipRead rd = re.LookupByHash(h, NULL);
            BOOST_CHECK_MESSAGE(rd.status == BLOCK_INDEX_TIP_OK && rd.record.hash == h,
                                "R3-I s4: torn index under an advanced marker must be rebuilt");
        }
        re.Close();
    }
    // -- s5: AFTER-corruption restart must equal the BEFORE-corruption restart.
    {
        BlockIndexTipAuthority a; std::string oerr;
        BOOST_REQUIRE_MESSAGE(BlockIndexTipAuthority::Open(dir, 44, &a, &oerr),
                              "R3-I s5: reopen-after-corruption failed: " << oerr);
        BlockIndexTipRead t = a.LookupByHash(uint256(0x1113UL), NULL);
        BOOST_CHECK_MESSAGE(t.status == BLOCK_INDEX_TIP_OK && t.record.hash == uint256(0x1113UL) &&
                            t.height == baseTip + 3,
                            "R3-I s5: post-corruption restart must be identical to the pre-corruption result");
        a.Close();
    }
    printf("R3-I PASS reconcile matrix (stale/same-count/malformed-marker/torn/restart-equivalence)\n");
}


// =====================================================================
// IFCN#9 perf — IBD durable-commit batching (-tipcommitbatch)
//
// P1  appends inside the window advance the LOGICAL tip and are resolvable
//     (LookupByHash) while the DURABLE tip.meta stays at the last boundary
// P2  Close() publishes a residual window (clean shutdown loses nothing)
// P3  idempotent replay inside the window does not duplicate
// P4  a crash image taken mid-window reopens at the last committed tip
//     (nothing torn: the window is RAM-only until the boundary)
// =====================================================================
namespace {
struct TipBatchArgs
{
    TipBatchArgs(const char* n, const char* always)
    {
        mapArgs["-tipcommitbatch"] = n;
        if (always) mapArgs["-tipcommitbatchalways"] = always;
    }
    ~TipBatchArgs()
    {
        mapArgs.erase("-tipcommitbatch");
        mapArgs.erase("-tipcommitbatchalways");
    }
};
static void AppendExplicit(BlockIndexTipAuthority& tip, uint256 h, uint256 prev,
                           int height, bool pos)
{
    BlockIndexRecord r = MakeRecord(h, prev, height, pos);
    BlockIndexDerivedEntry d = MakeDerived(h, 2000 + height);
    BlockIndexTipAppend a; a.record = r; a.derived = d;
    std::string err;
    BOOST_REQUIRE(tip.Append(a, height, &err) == BLOCK_INDEX_TIP_OK);
}
} // namespace

BOOST_AUTO_TEST_CASE(p_tipbatch_buffers_then_publishes_at_boundary)
{
    const std::string dir = MakeTempDir();
    const std::string crashImage = dir + "-crashimg";
    const int baseTip = 499;
    TipBatchArgs args("4", "1");
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 9, 900, baseTip, &tip, NULL));
        uint256 prev = uint256(0xAA01UL);
        AppendExplicit(tip, uint256(0xAA02UL), prev, baseTip + 1, true);
        AppendExplicit(tip, uint256(0xAA03UL), uint256(0xAA02UL), baseTip + 2, false);
        AppendExplicit(tip, uint256(0xAA04UL), uint256(0xAA03UL), baseTip + 3, true);
        // logical authority already sees all three...
        BOOST_REQUIRE_EQUAL(tip.TipHeight(), baseTip + 3);
        BOOST_REQUIRE_EQUAL(tip.TipRecordCount(), 3u);
        BlockIndexTipRead rd = tip.LookupByHash(uint256(0xAA04UL), NULL);
        BOOST_REQUIRE(rd.status == BLOCK_INDEX_TIP_OK);
        BOOST_REQUIRE_EQUAL(rd.height, baseTip + 3);
        // ...while the durable commit point has not moved: copy the datadir as a crash image
        fs::remove_all(crashImage);
        fs::copy(dir, crashImage, fs::copy_options::recursive);
        // the 4th append reaches the boundary and publishes ONE durable batch
        AppendExplicit(tip, uint256(0xAA05UL), uint256(0xAA04UL), baseTip + 4, false);
        BOOST_REQUIRE_EQUAL(tip.TipHeight(), baseTip + 4);
    }
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE(BlockIndexTipAuthority::Open(dir, 9, &tip, NULL));
        BOOST_REQUIRE_EQUAL(tip.TipHeight(), baseTip + 4);
        BOOST_REQUIRE_EQUAL(tip.TipRecordCount(), 4u);
        BOOST_REQUIRE(tip.TipHash() == uint256(0xAA05UL));
    }
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE(BlockIndexTipAuthority::Open(crashImage, 9, &tip, NULL));
        BOOST_REQUIRE_EQUAL(tip.TipHeight(), baseTip);   // last committed boundary only
        BOOST_REQUIRE_EQUAL(tip.TipRecordCount(), 0u);
        BOOST_REQUIRE(tip.LookupByHash(uint256(0xAA04UL), NULL).status ==
                      BLOCK_INDEX_TIP_NOT_FOUND);
    }
    printf("P1 PASS batched window: logical tip advances, durable boundary moves once per N\n");
}

BOOST_AUTO_TEST_CASE(p_tipbatch_close_flushes_window)
{
    const std::string dir = MakeTempDir();
    const int baseTip = 20;
    {
        TipBatchArgs args("1000", "1");
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 3, 40, baseTip, &tip, NULL));
        AppendExplicit(tip, uint256(0xBB01UL), uint256(0xBB00UL), baseTip + 1, true);
        AppendExplicit(tip, uint256(0xBB02UL), uint256(0xBB01UL), baseTip + 2, false);
        BOOST_REQUIRE_EQUAL(tip.TipHeight(), baseTip + 2);
        // window still open: nothing durable yet; Close() must publish it
        tip.Close();
    }
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE(BlockIndexTipAuthority::Open(dir, 3, &tip, NULL));
        BOOST_REQUIRE_EQUAL(tip.TipHeight(), baseTip + 2);   // Close() flushed it
        BOOST_REQUIRE_EQUAL(tip.TipRecordCount(), 2u);
    }
    printf("P2 PASS close publishes the residual window\n");
}

BOOST_AUTO_TEST_CASE(p_tipbatch_idempotent_replay_inside_window)
{
    const std::string dir = MakeTempDir();
    const int baseTip = 60;
    TipBatchArgs args("8", "1");
    BlockIndexTipAuthority tip;
    BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 4, 80, baseTip, &tip, NULL));
    AppendExplicit(tip, uint256(0xCC01UL), uint256(0xCC00UL), baseTip + 1, true);
    AppendExplicit(tip, uint256(0xCC01UL), uint256(0xCC00UL), baseTip + 1, true); // replay
    BOOST_REQUIRE_EQUAL(tip.TipRecordCount(), 1u);
    BOOST_REQUIRE_EQUAL(tip.TipHeight(), baseTip + 1);
    printf("P3 PASS replay inside the window is a no-op\n");
}

BOOST_AUTO_TEST_CASE(p_tipbatch_rejects_nondense_inside_window)
{
    const std::string dir = MakeTempDir();
    const int baseTip = 80;
    TipBatchArgs args("8", "1");
    BlockIndexTipAuthority tip;
    BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 4, 100, baseTip, &tip, NULL));
    AppendExplicit(tip, uint256(0xDD01UL), uint256(0xDD00UL), baseTip + 1, true);
    // a gap must be refused exactly like the per-block path
    BlockIndexRecord r = MakeRecord(uint256(0xDD03UL), uint256(0xDD02UL), baseTip + 3, false);
    BlockIndexDerivedEntry d = MakeDerived(uint256(0xDD03UL), 1);
    BlockIndexTipAppend a; a.record = r; a.derived = d;
    std::string err;
    BOOST_REQUIRE(tip.Append(a, baseTip + 3, &err) == BLOCK_INDEX_TIP_CORRUPT);
    BOOST_REQUIRE_EQUAL(tip.TipHeight(), baseTip + 1);
    printf("P4 PASS non-dense append rejected inside the window\n");
}


// =====================================================================
// IFCN#9 perf — CUTOVER PUBLICATION CONTRACT (T1–T6)
//
// Observed production defect (real -cutstate trace, heights 500–503):
//   h=500  DECIDE durStatus=NOT_FOUND durH=-1  logH=499 pend=499 ext=1  -> append, boundary commit -> dur=500
//   h=501  DECIDE durH=500 logH=500 pend=0 ext=1                        -> append (pend=1)
//   h=502  DECIDE durH=500 logH=501 pend=1 ext=0  <-- CUTOVER against the DURABLE tip
//          CUT-ENTER oldPubH=500 / CUT-FORK fork=500 / REORGACT branch=2 (firstH=501)
//   h=503  DECIDE durH=502 ...
// The cutover branch starts at a block that is already logically active, so the
// publication is an O(tip) truncate+re-attach for a plain extension.
//
// Contract pinned here:
//   T1 progress    a published cutover/extension advances the DURABLE tip
//   T2 convergence logical == durable ONLY at a confirmed durable boundary
//   T3 publication the durable tip survives Close()/Open()
//   T4 no duplicate the new block resolves to exactly one record id
//   T5 continuation the next height is still accepted afterwards
//   T6 detector    the "publish the branch with AppendBatch" substitution must
//                  fail the T1/T5 contract instead of silently stalling
// =====================================================================
namespace {
static bool DurableTipIs(const BlockIndexTipAuthority& tip, int32_t h, const uint256& hash)
{
    const BlockIndexTipRead d = tip.GetTip();
    return d.status == BLOCK_INDEX_TIP_OK && d.height == h && d.record.hash == hash;
}
// T2: convergence is mandatory ONLY when the window is empty (confirmed durable
// boundary). Mid-window divergence is the designed batching contract.
static bool DurableBoundaryConverged(const BlockIndexTipAuthority& tip, std::string* why)
{
    if (tip.PendingCount() != 0)
    {
        *why = "not a durable boundary (pending window non-empty)";
        return false;
    }
    const BlockIndexTipRead d = tip.GetTip();
    if (d.status != BLOCK_INDEX_TIP_OK) { *why = "no durable tip"; return false; }
    if (d.height != tip.TipHeight())
    {
        *why = "height divergence at durable boundary";
        return false;
    }
    if (d.record.hash != tip.TipHash())
    {
        *why = "hash divergence at durable boundary";
        return false;
    }
    return true;
}
// Mirror of the PRODUCTION caller's expected-parent rule (main.cpp): the logical
// tip when the authority holds one, else the immutable base tip S.
static uint256 ExpectedParentForTest(const BlockIndexTipAuthority& tip)
{
    const uint256 logical = tip.TipHash();
    return logical != uint256(0) ? logical : tip.BaseTipHash();
}
static bool ExtensionDecisionForTest(const BlockIndexTipAuthority& tip, const uint256& parent)
{
    return IsAuthoritativeActiveExtension(parent, ExpectedParentForTest(tip));
}
static void AppendAt(BlockIndexTipAuthority& tip, uint256 h, uint256 prev, int height)
{
    BlockIndexRecord r = MakeRecord(h, prev, height, false);
    BlockIndexDerivedEntry d = MakeDerived(h, 3000 + height);
    BlockIndexTipAppend a; a.record = r; a.derived = d;
    std::string err;
    BOOST_REQUIRE_MESSAGE(tip.Append(a, height, &err) == BLOCK_INDEX_TIP_OK,
                          "append h=" << height << " failed: " << err);
}
} // namespace

BOOST_AUTO_TEST_CASE(p_cutover_contract_extension_publish_t1_t5)
{
    const std::string dir = MakeTempDir();
    const int32_t baseTip = 499;
    TipBatchArgs args("500", "1");
    const uint256 h0(0xE000UL);   // base tip hash
    const uint256 h500(0xE500UL), h501(0xE501UL), h502(0xE502UL), h503(0xE503UL);
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 11, 120, baseTip, &tip, NULL));
        tip.SetBaseTipHash(h0);

        // durable boundary at 500 (the real batch boundary)
        AppendAt(tip, h500, h0, 500);
        std::string err;
        BOOST_REQUIRE(tip.FlushPendingAppends(&err) == BLOCK_INDEX_TIP_OK);
        BOOST_REQUIRE(DurableTipIs(tip, 500, h500));

        // 501 buffered: this is exactly the observed pre-cutover state
        AppendAt(tip, h501, h500, 501);
        BOOST_REQUIRE_EQUAL(tip.TipHeight(), 501);
        BOOST_REQUIRE_EQUAL(tip.PendingCount(), 1u);
        BOOST_REQUIRE_MESSAGE(DurableTipIs(tip, 500, h500),
                              "durable tip must still be 500 while 501 is buffered");

        // DECISION for 502: parent IS the logical tip -> EXTENSION, not a cutover
        const BlockIndexTipRead d = tip.GetTip();
        BOOST_REQUIRE_MESSAGE(ExtensionDecisionForTest(tip, h501),
                              "T-decision: 502 must publish as an extension of the logical tip");
        // the old durable-based rule would have said CUTOVER:
        BOOST_CHECK_MESSAGE(!IsAuthoritativeActiveExtension(h501, d.record.hash),
                            "T6: the durable-tip reference must reproduce the misrouting");

        AppendAt(tip, h502, h501, 502);
        BOOST_REQUIRE_EQUAL(tip.TipHeight(), 502);
        BOOST_REQUIRE_MESSAGE(DurableTipIs(tip, 500, h500),
                              "mid-window divergence is allowed (batch contract)");

        std::string why;
        BOOST_REQUIRE_MESSAGE(tip.FlushPendingAppends(&err) == BLOCK_INDEX_TIP_OK, "flush: " << err);
        BOOST_REQUIRE_MESSAGE(DurableTipIs(tip, 502, h502), "T1: durable progress to 502");
        BOOST_REQUIRE_MESSAGE(DurableBoundaryConverged(tip, &why), "T2: " << why);
        BOOST_REQUIRE_EQUAL(tip.TipRecordCount(), 3u);                 // T4: 500,501,502 only
        BOOST_REQUIRE(tip.LookupByHash(h501, NULL).status == BLOCK_INDEX_TIP_OK);
        BOOST_REQUIRE(tip.LookupByHash(h502, NULL).status == BLOCK_INDEX_TIP_OK);

        // T5 continuation
        AppendAt(tip, h503, h502, 503);
        BOOST_REQUIRE(tip.FlushPendingAppends(&err) == BLOCK_INDEX_TIP_OK);
        BOOST_REQUIRE_MESSAGE(DurableTipIs(tip, 503, h503), "T5: 503 accepted after 501+502");
        BOOST_REQUIRE_MESSAGE(DurableBoundaryConverged(tip, &why), "T2(503): " << why);
        tip.Close();
    }
    {
        // T3 publication survives restart
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE(BlockIndexTipAuthority::Open(dir, 11, &tip, NULL));
        BOOST_REQUIRE_MESSAGE(DurableTipIs(tip, 503, h503), "T3: restart returns the published tip");
        BOOST_REQUIRE_EQUAL(tip.TipRecordCount(), 4u);
        tip.Close();
    }
    printf("CUTOVER-T1..T5 PASS extension publish: no cutover, durable boundary at 502/503, restart exact\n");
}

BOOST_AUTO_TEST_CASE(p_cutover_stall_detector_appendbatch_substitute)
{
    // T6 — DETECTOR for the reproduced silent stall (real run: -cutstate with the
    // pre-fix durable-based decision + the AppendBatch substitution reached only
    // h=502 in 151 s; see reports/innova/ifcn9-perf/logs/cutstate-defectlegacy.log).
    //
    // Measured mechanism (FACT, not inference):
    //   h=502 decision: durable=500, logical=501, pending=1, ext=0 -> CUTOVER
    //   the substituted publish calls AppendBatch([501,502]) WITHOUT the
    //   ReorgActiveTo entry flush, so the branch is committed durably
    //   (TIPCOMMIT durH=502) while the RAM window still holds {501} and
    //   pendTipHeight stays 501;
    //   h=503: Append validates against the STALE pending tip (503 != 502) and
    //   fails closed with CORRUPT -> the chain is frozen at the published 501
    //   even though the durable store advanced to 502.
    //
    // The detector asserts the contract violations. It FAILS if a future change
    // makes this defective path look healthy (e.g. AppendBatch silently flushing
    // the window), which is exactly the regression it must catch.
    const std::string dir = MakeTempDir();
    const int32_t baseTip = 499;
    TipBatchArgs args("500", "1");
    const uint256 h0(0xE000UL), h500(0xE500UL), h501(0xE501UL), h502(0xE502UL), h503(0xE503UL);
    BlockIndexTipAuthority tip;
    BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 12, 140, baseTip, &tip, NULL));
    tip.SetBaseTipHash(h0);
    AppendAt(tip, h500, h0, 500);
    std::string err;
    BOOST_REQUIRE(tip.FlushPendingAppends(&err) == BLOCK_INDEX_TIP_OK);
    AppendAt(tip, h501, h500, 501);            // buffered: durable 500, logical 501
    BOOST_REQUIRE_EQUAL(tip.PendingCount(), 1u);
    BOOST_REQUIRE(tip.CommittedTipSnapshot().tipHeight == 501);   // published tip

    // the substitution: publish the cutover branch with AppendBatch, NO flush
    std::vector<BlockIndexTipAppend> branch;
    std::vector<int32_t> heights;
    for (int32_t h = 501; h <= 502; ++h)
    {
        BlockIndexTipAppend a;
        a.record = MakeRecord(h == 501 ? h501 : h502, h == 501 ? h500 : h501, h, false);
        a.derived = MakeDerived(uint256(h), 4000 + h);
        branch.push_back(a);
        heights.push_back(h);
    }
    const BlockIndexTipStatus sub = tip.AppendBatch(branch, heights, &err);
    BOOST_REQUIRE_MESSAGE(sub == BLOCK_INDEX_TIP_OK,
                          "the substitute itself succeeds (that is why the stall is silent), got=" << (int)sub);

    // The durable side looks HEALTHY (that is why the stall is silent): the
    // commit landed and the committed snapshot advanced.
    BOOST_REQUIRE_EQUAL(tip.GetTip().height, 502);
    BOOST_REQUIRE(tip.CommittedTipSnapshot().tipHeight == 502);
    // T1 VIOLATED: the LOGICAL tip (the one every subsequent Append validates
    // against) never advanced -- it is frozen at the buffered 501.
    BOOST_CHECK_MESSAGE(tip.TipHeight() != 502,
                        "T6/T1: the defective path must leave the logical tip behind");
    BOOST_CHECK_EQUAL(tip.TipHeight(), 501);
    // T2 VIOLATED: a durable commit left the RAM window dirty, and the logical
    // tip now sits BELOW the durable tip (illegal inversion).
    BOOST_CHECK_MESSAGE(tip.PendingCount() == 1u,
                        "T6/T2: the stale window is the causal state");
    BOOST_CHECK_MESSAGE(tip.TipHeight() < tip.GetTip().height,
                        "T6/T2: logical tip must be below durable to reproduce the freeze");
    std::string why;
    BOOST_CHECK_MESSAGE(!DurableBoundaryConverged(tip, &why),
                        "T6/T2: logical/durable convergence must be violated here");
    // T5 VIOLATED: the next block can never be accepted
    BlockIndexRecord r503 = MakeRecord(h503, h502, 503, false);
    BlockIndexDerivedEntry d503 = MakeDerived(h503, 4503);
    BlockIndexTipAppend a503; a503.record = r503; a503.derived = d503;
    const BlockIndexTipStatus cont = tip.Append(a503, 503, &err);
    BOOST_CHECK_MESSAGE(cont == BLOCK_INDEX_TIP_CORRUPT,
                        "T6/T5: 503 must be refused (lost progress), got=" << (int)cont);
    printf("STALL DETECTED: published tip frozen at 501, durable=502, stale window=1, 503 rejected (%s)\n",
           err.c_str());
    tip.Close();
}

BOOST_AUTO_TEST_CASE(p_cutover_integration_501_502_503)
{
    // Integration: drive the PRODUCTION decision for 501,502,503 and require
    // that 503 is accepted after 501+502 with no lost progress.
    const std::string dir = MakeTempDir();
    const int32_t baseTip = 499;
    TipBatchArgs args("500", "1");
    const uint256 h0(0xE000UL);
    uint256 hashOf[504];
    hashOf[500] = uint256(0xE500UL);
    hashOf[501] = uint256(0xE501UL);
    hashOf[502] = uint256(0xE502UL);
    hashOf[503] = uint256(0xE503UL);
    BlockIndexTipAuthority tip;
    BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 13, 160, baseTip, &tip, NULL));
    tip.SetBaseTipHash(h0);
    AppendAt(tip, hashOf[500], h0, 500);
    std::string err;
    BOOST_REQUIRE(tip.FlushPendingAppends(&err) == BLOCK_INDEX_TIP_OK);

    for (int32_t h = 501; h <= 503; ++h)
    {
        const bool ext = ExtensionDecisionForTest(tip, hashOf[h - 1]);
        BOOST_REQUIRE_MESSAGE(ext, "h=" << h << ": linear IBD block must publish as an extension");
        AppendAt(tip, hashOf[h], hashOf[h - 1], h);
        BOOST_REQUIRE_MESSAGE(tip.TipHeight() == h, "h=" << h << ": logical tip must advance immediately");
    }
    BOOST_REQUIRE_MESSAGE(tip.FlushPendingAppends(&err) == BLOCK_INDEX_TIP_OK, "flush: " << err);
    BOOST_REQUIRE_MESSAGE(DurableTipIs(tip, 503, hashOf[503]),
                          "integration: 503 must be published and not lost after 501+502");
    std::string why;
    BOOST_REQUIRE_MESSAGE(DurableBoundaryConverged(tip, &why), "integration T2: " << why);
    tip.Close();
    {
        BlockIndexTipAuthority re;
        BOOST_REQUIRE(BlockIndexTipAuthority::Open(dir, 13, &re, NULL));
        BOOST_REQUIRE_MESSAGE(DurableTipIs(re, 503, hashOf[503]), "integration: restart exact at 503");
        re.Close();
    }
    printf("CUTOVER-INTEGRATION PASS 501->502->503 accepted, durable 503, restart exact\n");
}

BOOST_AUTO_TEST_CASE(p_cutover_decision_true_reorg_is_still_cutover)
{
    // The pending-aware rule must NOT swallow a genuine best-chain replacement:
    // a side branch block that becomes best has a parent != logical tip, so the
    // decision must stay CUTOVER and ReorgActiveTo must promote the side record.
    const std::string dir = MakeTempDir();
    const int32_t baseTip = 499;
    TipBatchArgs args("500", "1");
    const uint256 h0(0xE000UL), h500(0xE500UL);
    const uint256 s1(0xF501UL), s2(0xF502UL);
    BlockIndexTipAuthority tip;
    BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 14, 180, baseTip, &tip, NULL));
    tip.SetBaseTipHash(h0);
    AppendAt(tip, h500, h0, 500);
    AppendAt(tip, uint256(0xE501UL), h500, 501);   // durable 501: the block the reorg replaces
    std::string err;
    BOOST_REQUIRE(tip.FlushPendingAppends(&err) == BLOCK_INDEX_TIP_OK);
    BOOST_REQUIRE(DurableTipIs(tip, 501, uint256(0xE501UL)));

    // side records (activeHeight = -1): a competing branch at fork 500
    {
        BlockIndexTipAppend a;
        a.record = MakeRecord(s1, h500, 501, false);
        a.derived = MakeDerived(s1, 5001);
        BOOST_REQUIRE(tip.Append(a, -1, &err) == BLOCK_INDEX_TIP_OK);
        a.record = MakeRecord(s2, s1, 502, false);
        a.derived = MakeDerived(s2, 5002);
        BOOST_REQUIRE(tip.Append(a, -1, &err) == BLOCK_INDEX_TIP_OK);
    }
    BOOST_REQUIRE_MESSAGE(!ExtensionDecisionForTest(tip, s1),
                          "a side-branch parent is not an extension of the active tip");
    // publish the cutover exactly as the caller does (fork = durable tip)
    std::vector<BlockIndexTipAppend> branch;
    std::vector<int32_t> heights;
    for (int32_t h = 501; h <= 502; ++h)
    {
        BlockIndexTipAppend a;
        a.record = MakeRecord(h == 501 ? s1 : s2, h == 501 ? h500 : s1, h, false);
        a.derived = MakeDerived(uint256(h), 5100 + h);
        branch.push_back(a); heights.push_back(h);
    }
    BOOST_REQUIRE_MESSAGE(tip.ReorgActiveTo(500, branch, heights, &err) == BLOCK_INDEX_TIP_OK,
                          "true-reorg publish failed: " << err);
    BOOST_REQUIRE_MESSAGE(DurableTipIs(tip, 502, s2), "the promoted side branch must become the durable tip");
    BOOST_REQUIRE(tip.LookupByHash(s1, NULL).status == BLOCK_INDEX_TIP_OK);
    printf("CUTOVER-DECISION PASS true reorg still routes through ReorgActiveTo (side promotion)\n");
    tip.Close();
}


// =====================================================================
// HARDENING PASS — first pending window, flush failure, publication contract
// =====================================================================

BOOST_AUTO_TEST_CASE(p_first_window_extension_requires_parent_identity)
{
    // A fresh IBD has NO durable tip. That must NOT mean "any block is an
    // extension": the expected parent is the immutable base tip S, and after the
    // first append the logical (buffered) tip. Consensus/chainwork are untouched:
    // the authority only refuses to splice a block that does not attach.
    const std::string dir = MakeTempDir();
    const int32_t baseTip = 0;
    TipBatchArgs args("500", "1");
    const uint256 g(0xB000UL);        // base tip S hash
    const uint256 h1(0xB001UL), h2(0xB002UL), h2bad(0xB0F2UL), other(0xB0FFUL);
    BlockIndexTipAuthority tip;
    BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 21, 10, baseTip, &tip, NULL));
    tip.SetBaseTipHash(g);

    // (a) empty authority: only the child of S is an extension
    BOOST_CHECK_MESSAGE(ExtensionDecisionForTest(tip, g), "the child of the base tip IS an extension");
    BOOST_CHECK_MESSAGE(!ExtensionDecisionForTest(tip, other), "a non-child of the base tip is NOT");

    // (b) the authority itself refuses a first block that does not attach to S
    std::string err;
    {
        BlockIndexTipAppend a;
        a.record = MakeRecord(h1, other, 1, false);
        a.derived = MakeDerived(h1, 7001);
        BOOST_CHECK_MESSAGE(tip.Append(a, 1, &err) == BLOCK_INDEX_TIP_CORRUPT,
                            "misparented first active block must be refused: " << err);
        BOOST_CHECK_EQUAL(tip.TipHeight(), baseTip);
        BOOST_CHECK_EQUAL(tip.PendingCount(), 0u);
    }
    // (c) the true child is accepted and becomes the logical tip
    AppendAt(tip, h1, g, 1);
    BOOST_REQUIRE_EQUAL(tip.TipHeight(), 1);
    BOOST_REQUIRE(tip.TipHash() == h1);

    // (d) after the first append the expected parent is the LOGICAL tip, not S
    BOOST_CHECK_MESSAGE(ExtensionDecisionForTest(tip, h1), "child of the buffered tip is an extension");
    BOOST_CHECK_MESSAGE(!ExtensionDecisionForTest(tip, g), "S is no longer the tip once h1 is buffered");
    {
        BlockIndexTipAppend a;
        a.record = MakeRecord(h2bad, other, 2, false);
        a.derived = MakeDerived(h2bad, 7002);
        BOOST_CHECK_MESSAGE(tip.Append(a, 2, &err) == BLOCK_INDEX_TIP_CORRUPT,
                            "misparented continuation must be refused inside the window: " << err);
        BOOST_CHECK_EQUAL(tip.TipHeight(), 1);
        BOOST_CHECK(tip.TipHash() == h1);          // active chain identity unchanged
        BOOST_CHECK_EQUAL(tip.PendingCount(), 1u); // no phantom record buffered
    }
    // (e) the true continuation still works and survives the boundary
    AppendAt(tip, h2, h1, 2);
    BOOST_REQUIRE(tip.FlushPendingAppends(&err) == BLOCK_INDEX_TIP_OK);
    BOOST_REQUIRE(DurableTipIs(tip, 2, h2));
    printf("FIRST-WINDOW PASS extension needs parent identity (S, then logical tip); misparented refused\n");
    tip.Close();
}

BOOST_AUTO_TEST_CASE(p_first_window_competing_branch_is_reorg_not_extension)
{
    // A competing branch that becomes best BEFORE the first batch boundary must
    // be published as a real cutover (side promotion), never spliced in as an
    // extension of the active chain.
    const std::string dir = MakeTempDir();
    const int32_t baseTip = 0;
    TipBatchArgs args("500", "1");
    const uint256 g(0xB000UL), h1(0xB101UL), h2(0xB102UL);
    const uint256 s2(0xB1F2UL);       // competing block at height 2 on parent h1
    BlockIndexTipAuthority tip;
    BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 22, 10, baseTip, &tip, NULL));
    tip.SetBaseTipHash(g);
    AppendAt(tip, h1, g, 1);
    AppendAt(tip, h2, h1, 2);
    BOOST_REQUIRE_EQUAL(tip.PendingCount(), 2u);   // still inside the first window

    // the competitor's parent is h1, the logical tip is h2 -> NOT an extension
    BOOST_REQUIRE_MESSAGE(!ExtensionDecisionForTest(tip, h1),
                          "same-height competitor must not take the extension fast path");
    // it is accepted as a SIDE record (as AddToBlockIndex does for a non-best block)
    std::string err;
    {
        BlockIndexTipAppend a;
        a.record = MakeRecord(s2, h1, 2, false);
        a.derived = MakeDerived(s2, 7102);
        BOOST_REQUIRE_MESSAGE(tip.Append(a, -1, &err) == BLOCK_INDEX_TIP_OK, err);
    }
    // and when it becomes best it is published through the reorg operation:
    // fork = 1 (h1), branch = [s2]
    std::vector<BlockIndexTipAppend> branch;
    std::vector<int32_t> heights;
    {
        BlockIndexTipAppend a;
        a.record = MakeRecord(s2, h1, 2, false);
        a.derived = MakeDerived(s2, 7102);
        branch.push_back(a); heights.push_back(2);
    }
    BOOST_REQUIRE_MESSAGE(tip.ReorgActiveTo(1, branch, heights, &err) == BLOCK_INDEX_TIP_OK, err);
    BOOST_REQUIRE_EQUAL(tip.TipHeight(), 2);
    BOOST_REQUIRE(tip.TipHash() == s2);
    BOOST_REQUIRE_MESSAGE(DurableTipIs(tip, 2, s2), "reorg flushed and committed the competitor");
    BOOST_REQUIRE(tip.LookupByHash(h2, NULL).status == BLOCK_INDEX_TIP_OK); // old branch kept as record
    printf("FIRST-WINDOW-REORG PASS competitor published via ReorgActiveTo (side promotion), not spliced\n");
    tip.Close();
}

BOOST_AUTO_TEST_CASE(p_flush_failure_keeps_window_and_no_false_commit)
{
    // A failed durable publication must NOT be reported as success and must NOT
    // roll the logical tip back: the window stays answerable and a retry commits.
    const std::string dir = MakeTempDir();
    const int32_t baseTip = 499;
    TipBatchArgs args("500", "1");
    const uint256 h0(0xB000UL), h500(0xB500UL);
    BlockIndexTipAuthority tip;
    BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 23, 10, baseTip, &tip, NULL));
    tip.SetBaseTipHash(h0);
    AppendAt(tip, h500, h0, 500);
    BOOST_REQUIRE_EQUAL(tip.PendingCount(), 1u);

    BlockIndexTipSetFailpointForTesting("FP_FLUSH_PENDING_APPENDS", true);
    std::string err;
    const BlockIndexTipStatus st = tip.FlushPendingAppends(&err);
    BlockIndexTipSetFailpointForTesting("FP_FLUSH_PENDING_APPENDS", false);
    BOOST_REQUIRE_MESSAGE(st == BLOCK_INDEX_TIP_IO_ERROR, "flush must fail, got " << (int)st);
    BOOST_CHECK_MESSAGE(err.find("FP_FLUSH_PENDING_APPENDS") != std::string::npos,
                        "the failure reason must be legible: " << err);
    // no false durable confirmation
    BOOST_CHECK_MESSAGE(tip.GetTip().status != BLOCK_INDEX_TIP_OK,
                        "a failed flush must not publish a durable tip");
    // the window survives: logical progress is not silently lost
    BOOST_CHECK_EQUAL(tip.PendingCount(), 1u);
    BOOST_CHECK_EQUAL(tip.TipHeight(), 500);
    BOOST_CHECK(tip.TipHash() == h500);
    BOOST_CHECK(tip.CommittedTipSnapshot().tipHeight == 500);   // logical snapshot kept
    // retry publishes for real
    BOOST_REQUIRE(tip.FlushPendingAppends(&err) == BLOCK_INDEX_TIP_OK);
    BOOST_REQUIRE(DurableTipIs(tip, 500, h500));
    BOOST_CHECK_EQUAL(tip.PendingCount(), 0u);
    printf("FLUSH-FAILURE PASS no false commit, window preserved, retry commits\n");
    tip.Close();
}

BOOST_AUTO_TEST_CASE(p_publication_contract_logical_vs_durable)
{
    // Pins the whole publication surface: logical (TipHeight/TipHash), durable
    // (GetTip = last committed active entry), the coherent snapshot, the RAM
    // window and the on-disk tip.meta.
    const std::string dir = MakeTempDir();
    const int32_t baseTip = 499;
    TipBatchArgs args("500", "1");
    const uint256 h0(0xB000UL), h500(0xB500UL), s(0xB5F0UL);
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 24, 10, baseTip, &tip, NULL));
        tip.SetBaseTipHash(h0);

        // S0: empty authority -> no durable tip, logical tip == base tip S
        BOOST_REQUIRE(tip.GetTip().status == BLOCK_INDEX_TIP_NOT_FOUND);
        BOOST_CHECK_EQUAL(tip.TipHeight(), baseTip);
        BOOST_CHECK(tip.TipHash() == uint256(0));
        BOOST_CHECK_EQUAL(tip.PendingCount(), 0u);

        // S1: buffered append -> LOGICAL advances, DURABLE does not, the snapshot
        // is logical (that is what the sync pipeline consumes)
        AppendAt(tip, h500, h0, 500);
        BOOST_CHECK_EQUAL(tip.TipHeight(), 500);
        BOOST_CHECK(tip.TipHash() == h500);
        BOOST_CHECK(tip.GetTip().status == BLOCK_INDEX_TIP_NOT_FOUND);
        BOOST_CHECK_EQUAL(tip.PendingCount(), 1u);
        const BlockIndexCommittedTipSnapshot snap1 = tip.CommittedTipSnapshot();
        BOOST_CHECK_MESSAGE(snap1.valid && snap1.tipHeight == 500 && snap1.tipHash == h500,
                            "snapshot must publish the LOGICAL tip inside the window");

        // S2: durable boundary -> everything converges
        std::string err;
        BOOST_REQUIRE(tip.FlushPendingAppends(&err) == BLOCK_INDEX_TIP_OK);
        BOOST_CHECK(DurableTipIs(tip, 500, h500));
        BOOST_CHECK_EQUAL(tip.TipHeight(), 500);
        BOOST_CHECK(tip.TipHash() == h500);
        BOOST_CHECK_EQUAL(tip.PendingCount(), 0u);
        const BlockIndexCommittedTipSnapshot snap2 = tip.CommittedTipSnapshot();
        BOOST_CHECK_MESSAGE(snap2.valid && snap2.tipHeight == 500 && snap2.tipHash == h500,
                            "snapshot converges at the durable boundary");
        tip.Close();
    }
    {
        // S3: tip.meta on disk carries exactly that state
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE(BlockIndexTipAuthority::Open(dir, 24, &tip, NULL));
        BOOST_CHECK(DurableTipIs(tip, 500, h500));
        BOOST_CHECK_EQUAL(tip.PendingCount(), 0u);
        const BlockIndexCommittedTipSnapshot snap3 = tip.CommittedTipSnapshot();
        BOOST_CHECK(snap3.valid && snap3.tipHeight == 500 && snap3.tipHash == h500);

        // S4: a reorg publishes through the same surface (fork == base tip S)
        std::string err;
        {
            BlockIndexTipAppend a;
            a.record = MakeRecord(s, h0, 500, false);
            a.derived = MakeDerived(s, 7500);
            BOOST_REQUIRE(tip.Append(a, -1, &err) == BLOCK_INDEX_TIP_OK);
        }
        std::vector<BlockIndexTipAppend> branch; std::vector<int32_t> heights;
        {
            BlockIndexTipAppend a;
            a.record = MakeRecord(s, h0, 500, false);
            a.derived = MakeDerived(s, 7500);
            branch.push_back(a); heights.push_back(500);
        }
        BOOST_REQUIRE_MESSAGE(tip.ReorgActiveTo(baseTip, branch, heights, &err) == BLOCK_INDEX_TIP_OK, err);
        BOOST_CHECK(DurableTipIs(tip, 500, s));
        BOOST_CHECK_EQUAL(tip.TipHeight(), 500);
        BOOST_CHECK(tip.TipHash() == s);
        const BlockIndexCommittedTipSnapshot snap4 = tip.CommittedTipSnapshot();
        BOOST_CHECK_MESSAGE(snap4.valid && snap4.tipHash == s,
                            "snapshot must follow a durable reorg");
        tip.Close();
    }
    printf("CONTRACT PASS logical/durable/snapshot/window/tip.meta separated and convergent at boundaries\n");
}

BOOST_AUTO_TEST_CASE(p_tipbatch_boundary_must_not_exceed_store_barrier)
{
    // Store3A commits at the barrier and the tip publishes the same height right
    // after ConnectBlock returns; a coarser tip boundary would let the store run
    // ahead of the block index, a state the startup reconcile refuses.
    BOOST_CHECK(IsTipBatchCompatibleWithStoreBarrier(1, 1000));    // default: unchanged
    BOOST_CHECK(IsTipBatchCompatibleWithStoreBarrier(500, 1000));  // our IBD setting
    BOOST_CHECK(IsTipBatchCompatibleWithStoreBarrier(1000, 1000)); // exactly aligned
    BOOST_CHECK_MESSAGE(!IsTipBatchCompatibleWithStoreBarrier(2000, 1000),
                        "a tip boundary coarser than the store barrier must be refused");
    BOOST_CHECK_MESSAGE(!IsTipBatchCompatibleWithStoreBarrier(500, 1),
                        "a single-block store barrier cannot absorb a coarse tip boundary");
    BOOST_CHECK_MESSAGE(!IsTipBatchCompatibleWithStoreBarrier(500, 0),
                        "no barrier => no bound => refuse");
    printf("BATCH-BOUNDARY PASS tip boundary must not exceed the Store3A barrier\n");
}

BOOST_AUTO_TEST_CASE(p_shutdown_flush_helper_inert_without_authority)
{
    // The shutdown hook must be inert (and claim nothing) when authoritative mode
    // is not armed -- it must never report a publication it did not perform.
    BOOST_CHECK(FlushAuthoritativeTipWindowAtShutdown());
    BlockIndexTipSetFailpointForTesting("FP_FLUSH_PENDING_APPENDS", true);
    BOOST_CHECK(FlushAuthoritativeTipWindowAtShutdown());
    BlockIndexTipSetFailpointForTesting("FP_FLUSH_PENDING_APPENDS", false);
    printf("SHUTDOWN-HELPER PASS inert without an authoritative live authority\n");
}

BOOST_AUTO_TEST_SUITE_END()

// =====================================================================
// R3-G/H — node 57.75 outage regression (2026-10-28).
//
// Symptom: after a tip-hashindex rebuild the authoritative startup walk failed
// at the NEWEST tip hash with "walk lookup failed", twice, on an unchanged
// store whose record was provably present.
//
// Cause chain (measured on the real store): the committed window contained two
// slots carrying the SAME block hash; the rebuild passed BlockIndexHashIndex::
// Put's boolean straight back to StreamVisit, the sink rejected the second
// mapping ("conflicting"), StreamVisit interpreted the callback-false as a
// caller-requested early stop and returned SUCCESS, and the marker was written
// as if all N slots had been indexed. The index then held only the slots
// before the first duplicate, so the tip hash was unindexed and the walk
// failed closed.
//
// R3-G pins the sink contract the rebuild must handle. R3-H replays a real
// store copy (set INNOVA_TIP_TEST_STORE_DIR to the directory CONTAINING tip/)
// and emulates the walk the startup could not complete.
// =====================================================================
BOOST_AUTO_TEST_CASE(r3g_duplicate_hash_sink_conflict)
{
    const std::string dir = MakeTempDir();
    BlockIndexHashIndex idx;
    std::string err;
    BOOST_REQUIRE_MESSAGE(BlockIndexHashIndex::Create(dir, 1, &idx, &err), "R3G create: " << err);
    const uint256 h(0xD00DUL);
    BOOST_REQUIRE_MESSAGE(idx.Put(h, 101, &err), "R3G first put: " << err);
    BlockIndexId got = 0;
    BOOST_CHECK(idx.Lookup(h, &got, NULL) == BLOCK_INDEX_HASH_LOOKUP_FOUND);
    BOOST_CHECK_EQUAL(got, (BlockIndexId)101);
    std::string cerr;
    BOOST_CHECK_MESSAGE(!idx.Put(h, 102, &cerr),
                        "R3G: the sink MUST reject a second id for an already mapped hash");
    BOOST_CHECK(!cerr.empty());
    BOOST_CHECK(idx.Lookup(h, &got, NULL) == BLOCK_INDEX_HASH_LOOKUP_FOUND);
    BOOST_CHECK_EQUAL(got, (BlockIndexId)101);   // first mapping survives
    BOOST_CHECK_MESSAGE(idx.Put(h, 101, NULL),
                        "R3G: re-putting the SAME mapping (duplicate slot) must be accepted");
    idx.Close();
    printf("R3G PASS duplicate hash sink conflict\n");
}

BOOST_AUTO_TEST_CASE(r3h_real_tip_store_walk_after_rebuild)
{
    const char* dirp = getenv("INNOVA_TIP_TEST_STORE_DIR");
    if (!dirp || !*dirp)
    {
        printf("R3H SKIP (INNOVA_TIP_TEST_STORE_DIR not set)\n");
        return;
    }
    const char* genp = getenv("INNOVA_TIP_TEST_GEN");
    const uint64_t gen = (genp && *genp) ? strtoull(genp, NULL, 10) : 1;
    BlockIndexTipAuthority tip;
    std::string err;
    BOOST_REQUIRE_MESSAGE(BlockIndexTipAuthority::Open(dirp, gen, &tip, &err),
                          "R3H: Open of the real store copy failed: " << err);
    const BlockIndexTipRead gotTip = tip.GetTip();
    BOOST_REQUIRE(gotTip.status == BLOCK_INDEX_TIP_OK);
    printf("R3H store: tipHeight=%d N=%llu\n", gotTip.height,
           (unsigned long long)tip.TipRecordCount());
    // The walk the authoritative startup could not complete: from the tip
    // backwards through hashPrev, resolving every hash via the index.
    uint256 cur = gotTip.record.hash;
    int steps = 0;
    std::string werr;
    while (steps < 2048)
    {
        BlockIndexTipRead rd = tip.LookupByHash(cur, &werr);
        if (rd.status != BLOCK_INDEX_TIP_OK)
        {
            BOOST_ERROR("R3H: walk lookup failed at " << cur.GetHex() << " after "
                        << steps << " step(s): status=" << (int)rd.status << " " << werr);
            break;
        }
        cur = rd.record.hashPrev;
        ++steps;
    }
    BOOST_CHECK_MESSAGE(steps > 0, "R3H: walk did not advance");
    BOOST_CHECK(tip.LookupByHash(gotTip.record.hash, NULL).status == BLOCK_INDEX_TIP_OK);
    printf("R3H PASS real store: tip hash resolvable, walked %d step(s)\n", steps);
    tip.Close();
}
