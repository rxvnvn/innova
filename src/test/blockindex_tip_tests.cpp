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

BOOST_AUTO_TEST_SUITE_END()