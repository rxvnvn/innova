#include <boost/test/unit_test.hpp>

#include "blockindex_tip.h"
#include "blockindex_v2_reader.h"
#include "blockindex_derived_state.h"
#include "blockindex_derived_replay.h"
#include "blockindex_generation_builder.h"
#include "blockindex_generation_lifecycle.h"
#include "main.h"
#include "util.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

// =====================================================================
// V2-R3 — crash-durability & corruption fail-closed contracts for the
// mutable Block Index tip authority (blockindex_tip/).
//
// Every injected failure is judged by a CRASH ORACLE: after the failure,
// Close()+re-Open() must recover EXACTLY one of
//   (A) the last fully-committed authority  (crash-before-commit)
//   (B) the new fully-committed authority   (the rename IS the commit)
//   (C) deterministic rebuild
//   (D) explicit fail-closed (Open returns false with a non-empty error)
// Forbidden: hybrid/ambiguous state, silent empty/zero state, or Open
// succeeding with state matching neither A nor B.
//
// Oracle A/B assert explicit tip-hash/height/count equality (never just
// a boolean). NOTE: PM1-P0-06 (bounded residency) is deliberately NOT
// closed by this suite; the reopen assertions (C13) only prove the tip
// state is recovered by-value.
// =====================================================================

static std::string R3_CC_MakeTempDir()
{
    const char* env = getenv("TMPDIR");
    std::string base = (env && *env) ? env : "/tmp";
    std::string tmpl = base + "/innova-r3cc-XXXXXX";
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    char* d = mkdtemp(&buf[0]);
    BOOST_REQUIRE_MESSAGE(d != NULL, "R3_CC: mkdtemp under TMPDIR failed");
    return std::string(d);
}

static BlockIndexRecord R3_CC_MakeRecord(uint256 hash, uint256 hashPrev,
                                         int height, bool isPos)
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

static BlockIndexDerivedEntry R3_CC_MakeDerived(uint256 trust, uint32_t checksum)
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

// Append nActive contiguous ACTIVE blocks off firstPrev; writes the final
// (tip) hash into outFinalHash.
static void R3_CC_BuildChain(BlockIndexTipAuthority& tip, uint32_t n,
                             uint256 firstPrev, int baseTip,
                             uint256* outFinalHash)
{
    uint256 prev = firstPrev;
    for (uint32_t i = 0; i < n; ++i)
    {
        uint256 h = uint256(0x2222UL + i);
        BlockIndexTipAppend a;
        a.record = R3_CC_MakeRecord(h, prev, baseTip + 1 + (int)i, (i % 2) == 0);
        a.derived = R3_CC_MakeDerived(uint256(i + 1), 2000 + i);
        std::string err;
        BlockIndexTipStatus st = tip.Append(a, baseTip + 1 + (int)i, &err);
        BOOST_REQUIRE_MESSAGE(st == BLOCK_INDEX_TIP_OK,
                              "R3_CC: setup append failed: " + err);
        prev = h;
    }
    if (outFinalHash)
        *outFinalHash = prev;
}

static std::string R3_CC_MetaPath(const std::string& dir)
{
    return dir + "/blockindex_tip/tip.meta";
}
static std::string R3_CC_RecordsPath(const std::string& dir)
{
    return dir + "/blockindex_tip/tip-records.dat";
}
static std::string R3_CC_InvalidPath(const std::string& dir)
{
    return dir + "/blockindex_tip/tip-invalid.dat";
}

static std::vector<unsigned char> R3_CC_ReadFile(const std::string& path)
{
    std::vector<unsigned char> v;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f)
        return v;
    unsigned char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        v.insert(v.end(), buf, buf + n);
    fclose(f);
    return v;
}

static void R3_CC_WriteFile(const std::string& path,
                            const std::vector<unsigned char>& v)
{
    FILE* f = fopen(path.c_str(), "wb");
    BOOST_REQUIRE_MESSAGE(f != NULL, "R3_CC: cannot open for write: " + path);
    if (!v.empty())
        BOOST_REQUIRE(fwrite(&v[0], 1, v.size(), f) == v.size());
    fclose(f);
}

static bool R3_CC_TruncateFileTo(const std::string& path, size_t newLen)
{
    std::vector<unsigned char> buf = R3_CC_ReadFile(path);
    if (newLen > buf.size())
        return false;
    buf.resize(newLen);
    R3_CC_WriteFile(path, buf);
    return true;
}

static bool R3_CC_FlipByte(const std::string& path, size_t offset)
{
    FILE* f = fopen(path.c_str(), "r+b");
    if (!f)
        return false;
    unsigned char b = 0;
    if (fseek(f, (long)offset, SEEK_SET) != 0 || fread(&b, 1, 1, f) != 1)
    {
        fclose(f);
        return false;
    }
    b ^= 0xFF;
    if (fseek(f, (long)offset, SEEK_SET) != 0)
    {
        fclose(f);
        return false;
    }
    fwrite(&b, 1, 1, f);
    fclose(f);
    return true;
}

// Copy-on-write durability consequences of each failpoint:
//   FP_DURING_TAIL_UPDATE                 + Append fails (IO error), tail
//                                         temp never renamed -> oracle A.
//   FP_AFTER_TAIL_DURABLE_BEFORE_META     + Append fails (IO error), tip.meta
//                                         not committed -> oracle A.
//   FP_BEFORE_META_RENAME                 + Append fails, meta tmp never
//                                         renamed -> oracle A.
//   FP_AFTER_META_RENAME_BEFORE_DIRSYNC   + WriteMeta fails AFTER the
//                                         rename -> the new authority IS
//                                         committed on disk -> oracle B.
static const char* R3_CC_FP_TAIL      = "FP_DURING_TAIL_UPDATE";
static const char* R3_CC_FP_PRE_META  = "FP_AFTER_TAIL_DURABLE_BEFORE_META";
static const char* R3_CC_FP_PRE_REN   = "FP_BEFORE_META_RENAME";
static const char* R3_CC_FP_POST_REN  = "FP_AFTER_META_RENAME_BEFORE_DIRSYNC";

BOOST_AUTO_TEST_SUITE(blockindex_crash_corruption_tests)

// C1: FP_DURING_TAIL_UPDATE on a 2nd Append -> oracle A (previous tip).
BOOST_AUTO_TEST_CASE(c1_crash_during_tail_update_oracle_A)
{
    const std::string dir = R3_CC_MakeTempDir();
    const int baseTip = 400;
    uint256 committedTip;
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 21, 900, baseTip, &tip, NULL));
        R3_CC_BuildChain(tip, 1, uint256(0xAAAAUL), baseTip, &committedTip);
        BOOST_REQUIRE_EQUAL(tip.TipHeight(), baseTip + 1);
        BOOST_REQUIRE_EQUAL(tip.TipRecordCount(), 1u);
        // 2nd append crashes while its tail temp file is durable but NOT renamed.
        BlockIndexTipSetFailpointForTesting(R3_CC_FP_TAIL, true);
        BlockIndexTipAppend a;
        a.record = R3_CC_MakeRecord(uint256(0x2223UL), committedTip, baseTip + 2, false);
        a.derived = R3_CC_MakeDerived(uint256(9), 3001);
        std::string err;
        BlockIndexTipStatus st = tip.Append(a, baseTip + 2, &err);
        BOOST_CHECK_MESSAGE(st != BLOCK_INDEX_TIP_OK,
                            "R3_CC: failpoint arm must abort the 2nd append");
        BOOST_CHECK_MESSAGE(!err.empty(), "R3_CC: append error must be non-empty");
        BOOST_TEST_MESSAGE("R3_CC C1 append aborted (st=" << st << " err=" << err << ")");
        BlockIndexTipSetFailpointForTesting(R3_CC_FP_TAIL, false);
        tip.Close();
    }
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE_MESSAGE(BlockIndexTipAuthority::Open(dir, 21, &tip, NULL),
                              "R3_CC C1: reopen must succeed (oracle A)");
        BOOST_TEST_MESSAGE("R3_CC C1 oracle A: recovered tip height="
                           << tip.TipHeight() << " count=" << tip.TipRecordCount());
        BOOST_REQUIRE_EQUAL(tip.TipHeight(), baseTip + 1);
        BOOST_REQUIRE_EQUAL(tip.TipRecordCount(), 1u);
        BOOST_REQUIRE(tip.TipHash() == committedTip);
        BOOST_TEST_MESSAGE("R3_CC C1 PASS oracle A (previous committed tip intact)");
    }
    // The interrupted tail update must not have destroyed the committed file.
}

// C2: FP_AFTER_TAIL_DURABLE_BEFORE_META -> oracle A (tail truncated to committed count).
BOOST_AUTO_TEST_CASE(c2_crash_after_tail_durable_before_meta_oracle_A)
{
    const std::string dir = R3_CC_MakeTempDir();
    const int baseTip = 500;
    uint256 committedTip;
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 22, 950, baseTip, &tip, NULL));
        R3_CC_BuildChain(tip, 2, uint256(0xBBBBUL), baseTip, &committedTip);
        BOOST_REQUIRE_EQUAL(tip.TipHeight(), baseTip + 2);
        BOOST_REQUIRE_EQUAL(tip.TipRecordCount(), 2u);
        // 3rd append: all tail stores durable, crash right before tip.meta commit.
        BlockIndexTipSetFailpointForTesting(R3_CC_FP_PRE_META, true);
        BlockIndexTipAppend a;
        a.record = R3_CC_MakeRecord(uint256(0x3333UL), committedTip, baseTip + 3, false);
        a.derived = R3_CC_MakeDerived(uint256(9), 3002);
        std::string err;
        BOOST_CHECK_MESSAGE(tip.Append(a, baseTip + 3, &err) == BLOCK_INDEX_TIP_IO_ERROR,
                            "R3_CC: expected IO_ERROR from armed failpoint, err=" + err);
        BlockIndexTipSetFailpointForTesting(R3_CC_FP_PRE_META, false);
        tip.Close();
    }
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE_MESSAGE(BlockIndexTipAuthority::Open(dir, 22, &tip, NULL),
                              "R3_CC C2: reopen must succeed (oracle A)");
        BOOST_TEST_MESSAGE("R3_CC C2 oracle A: recovered tip height="
                           << tip.TipHeight() << " count=" << tip.TipRecordCount());
        BOOST_REQUIRE_EQUAL(tip.TipHeight(), baseTip + 2);
        BOOST_REQUIRE_EQUAL(tip.TipRecordCount(), 2u);
        BOOST_REQUIRE(tip.TipHash() == committedTip);
        BOOST_TEST_MESSAGE("R3_CC C2 PASS oracle A (uncommitted tail truncated to committed count)");
    }
}

// C3: FP_BEFORE_META_RENAME -> oracle A (tip.meta tmp never renamed).
BOOST_AUTO_TEST_CASE(c3_crash_before_meta_rename_oracle_A)
{
    const std::string dir = R3_CC_MakeTempDir();
    const int baseTip = 600;
    uint256 committedTip;
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 23, 990, baseTip, &tip, NULL));
        R3_CC_BuildChain(tip, 1, uint256(0xCCCCUL), baseTip, &committedTip);
        BOOST_REQUIRE_EQUAL(tip.TipHeight(), baseTip + 1);
        BlockIndexTipSetFailpointForTesting(R3_CC_FP_PRE_REN, true);
        BlockIndexTipAppend a;
        a.record = R3_CC_MakeRecord(uint256(0x4444UL), committedTip, baseTip + 2, false);
        a.derived = R3_CC_MakeDerived(uint256(9), 3003);
        std::string err;
        BOOST_CHECK_MESSAGE(tip.Append(a, baseTip + 2, &err) != BLOCK_INDEX_TIP_OK,
                            "R3_CC: expected append abort at meta-rename boundary");
        BOOST_CHECK_MESSAGE(!err.empty(), "R3_CC: append error must be non-empty");
        BOOST_TEST_MESSAGE("R3_CC C3 append aborted (err=" << err << ")");
        BlockIndexTipSetFailpointForTesting(R3_CC_FP_PRE_REN, false);
        tip.Close();
    }
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE_MESSAGE(BlockIndexTipAuthority::Open(dir, 23, &tip, NULL),
                              "R3_CC C3: reopen must succeed (oracle A)");
        BOOST_TEST_MESSAGE("R3_CC C3 oracle A: recovered tip height="
                           << tip.TipHeight() << " count=" << tip.TipRecordCount());
        BOOST_REQUIRE_EQUAL(tip.TipHeight(), baseTip + 1);
        BOOST_REQUIRE_EQUAL(tip.TipRecordCount(), 1u);
        BOOST_REQUIRE(tip.TipHash() == committedTip);
        BOOST_TEST_MESSAGE("R3_CC C3 PASS oracle A (previous committed tip intact)");
    }
}

// C4: FP_AFTER_META_RENAME_BEFORE_DIRSYNC -> oracle B (the rename IS the commit
// point: the new authority was renamed over tip.meta before the failure).
BOOST_AUTO_TEST_CASE(c4_crash_after_meta_rename_oracle_B)
{
    const std::string dir = R3_CC_MakeTempDir();
    const int baseTip = 700;
    uint256 committedTip;
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 24, 1000, baseTip, &tip, NULL));
        R3_CC_BuildChain(tip, 2, uint256(0xDDDDUL), baseTip, &committedTip);
        BOOST_REQUIRE_EQUAL(tip.TipHeight(), baseTip + 2);
        const uint256 newHash = uint256(0x5555UL);
        BlockIndexTipSetFailpointForTesting(R3_CC_FP_POST_REN, true);
        BlockIndexTipAppend a;
        a.record = R3_CC_MakeRecord(newHash, committedTip, baseTip + 3, false);
        a.derived = R3_CC_MakeDerived(uint256(9), 3004);
        std::string err;
        BOOST_CHECK_MESSAGE(tip.Append(a, baseTip + 3, &err) != BLOCK_INDEX_TIP_OK,
                            "R3_CC: expected append abort after meta rename, err=" + err);
        BlockIndexTipSetFailpointForTesting(R3_CC_FP_POST_REN, false);
        tip.Close();
    }
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE_MESSAGE(BlockIndexTipAuthority::Open(dir, 24, &tip, NULL),
                              "R3_CC C4: reopen must succeed (oracle B)");
        BOOST_TEST_MESSAGE("R3_CC C4 oracle B: recovered tip height="
                           << tip.TipHeight() << " count=" << tip.TipRecordCount());
        BOOST_REQUIRE_EQUAL(tip.TipHeight(), baseTip + 3);
        BOOST_REQUIRE_EQUAL(tip.TipRecordCount(), 3u);
        BOOST_REQUIRE(tip.TipHash() == uint256(0x5555UL));
        BOOST_TEST_MESSAGE("R3_CC C4 PASS oracle B (rename IS the commit point; new tip recorded)");
    }
}

// C5: truncated tip.meta -> fail closed.
BOOST_AUTO_TEST_CASE(c5_truncated_meta_fails_closed)
{
    const std::string dir = R3_CC_MakeTempDir();
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 25, 900, 333, &tip, NULL));
    }
    std::vector<unsigned char> meta = R3_CC_ReadFile(R3_CC_MetaPath(dir));
    BOOST_REQUIRE(meta.size() > 16);
    BOOST_REQUIRE(R3_CC_TruncateFileTo(R3_CC_MetaPath(dir), meta.size() / 2));
    BlockIndexTipAuthority tip;
    std::string err;
    const bool ok = BlockIndexTipAuthority::Open(dir, 25, &tip, &err);
    BOOST_CHECK_MESSAGE(!ok, "R3_CC C5: truncated tip.meta MUST fail closed");
    BOOST_CHECK_MESSAGE(!err.empty(), "R3_CC C5: error string must be non-empty");
    BOOST_TEST_MESSAGE("R3_CC C5 PASS truncated tip.meta fail-closed err=" << err);
}

// C6: corrupted tip.meta (bit-flip deep in the digest region + garbage
// rewrite variant) -> fail closed.
BOOST_AUTO_TEST_CASE(c6_corrupt_meta_fails_closed)
{
    const std::string dir = R3_CC_MakeTempDir();
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 26, 900, 444, &tip, NULL));
    }
    std::vector<unsigned char> meta = R3_CC_ReadFile(R3_CC_MetaPath(dir));
    BOOST_REQUIRE(meta.size() > 40);
    // tip.meta digest field covers [72,104); flip inside it -> decode
    // re-reads a corrupt digest that cannot match any committed store set.
    meta[80] ^= 0xFF;
    BOOST_REQUIRE(meta.size() == 140);
    R3_CC_WriteFile(R3_CC_MetaPath(dir), meta);
    {
        BlockIndexTipAuthority tip;
        std::string err;
        const bool ok = BlockIndexTipAuthority::Open(dir, 26, &tip, &err);
        BOOST_CHECK_MESSAGE(!ok, "R3_CC C6: corrupt tip.meta MUST fail closed");
        BOOST_CHECK_MESSAGE(!err.empty(), "R3_CC C6: error string must be non-empty");
        BOOST_TEST_MESSAGE("R3_CC C6a PASS bit-flip fail-closed err=" << err);
    }
    // Garbage rewrite of tip.meta must also fail closed.
    std::vector<unsigned char> garbage(140, 0xAB);
    R3_CC_WriteFile(R3_CC_MetaPath(dir), garbage);
    {
        BlockIndexTipAuthority tip;
        std::string err;
        const bool ok = BlockIndexTipAuthority::Open(dir, 26, &tip, &err);
        BOOST_CHECK_MESSAGE(!ok, "R3_CC C6: garbage tip.meta MUST fail closed");
        BOOST_CHECK_MESSAGE(!err.empty(), "R3_CC C6: error string must be non-empty");
        BOOST_TEST_MESSAGE("R3_CC C6b PASS garbage tip.meta fail-closed err=" << err);
    }
}

// C7: missing required tip-records.dat -> fail closed.
BOOST_AUTO_TEST_CASE(c7_missing_records_fails_closed)
{
    const std::string dir = R3_CC_MakeTempDir();
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 27, 900, 555, &tip, NULL));
    }
    BOOST_REQUIRE(std::remove(R3_CC_RecordsPath(dir).c_str()) == 0);
    BlockIndexTipAuthority tip;
    std::string err;
    const bool ok = BlockIndexTipAuthority::Open(dir, 27, &tip, &err);
    BOOST_CHECK_MESSAGE(!ok, "R3_CC C7: missing tip-records.dat MUST fail closed");
    BOOST_CHECK_MESSAGE(!err.empty(), "R3_CC C7: error string must be non-empty");
    BOOST_TEST_MESSAGE("R3_CC C7 PASS missing tip-records.dat fail-closed err=" << err);
}

// C8: truncated committed tip-records.dat (shorter than committed count) -> fail closed.
BOOST_AUTO_TEST_CASE(c8_truncated_records_fails_closed)
{
    const std::string dir = R3_CC_MakeTempDir();
    const int baseTip = 800;
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 28, 900, baseTip, &tip, NULL));
        R3_CC_BuildChain(tip, 3, uint256(0xEEEEUL), baseTip, NULL);
        BOOST_REQUIRE_EQUAL(tip.TipRecordCount(), 3u);
        tip.Close();
    }
    std::vector<unsigned char> recs = R3_CC_ReadFile(R3_CC_RecordsPath(dir));
    BOOST_REQUIRE(recs.size() > 0);
    // Truncate to a partial record region strictly shorter than 3 committed records.
    size_t partial = recs.size() / 3;
    BOOST_REQUIRE(partial < recs.size());
    BOOST_REQUIRE(R3_CC_TruncateFileTo(R3_CC_RecordsPath(dir), partial));
    BlockIndexTipAuthority tip;
    std::string err;
    const bool ok = BlockIndexTipAuthority::Open(dir, 28, &tip, &err);
    BOOST_CHECK_MESSAGE(!ok, "R3_CC C8: truncated committed tip-records.dat MUST fail closed");
    BOOST_CHECK_MESSAGE(!err.empty(), "R3_CC C8: error string must be non-empty");
    BOOST_TEST_MESSAGE("R3_CC C8 PASS truncated committed records fail-closed err=" << err);
}

// C9: bit-flip inside the committed tip-records.dat region -> fail closed
// (digest/validation mismatch).
BOOST_AUTO_TEST_CASE(c9_bitflip_records_fails_closed)
{
    const std::string dir = R3_CC_MakeTempDir();
    const int baseTip = 900;
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 29, 900, baseTip, &tip, NULL));
        R3_CC_BuildChain(tip, 2, uint256(0xFFFFUL), baseTip, NULL);
        BOOST_REQUIRE_EQUAL(tip.TipRecordCount(), 2u);
        tip.Close();
    }
    BOOST_REQUIRE_MESSAGE(R3_CC_FlipByte(R3_CC_RecordsPath(dir), 48 + 130),
                          "R3_CC C9: cannot flip byte in records file");
    BlockIndexTipAuthority tip;
    std::string err;
    const bool ok = BlockIndexTipAuthority::Open(dir, 29, &tip, &err);
    BOOST_CHECK_MESSAGE(!ok, "R3_CC C9: bit-flip in committed tip-records.dat MUST fail closed");
    BOOST_CHECK_MESSAGE(!err.empty(), "R3_CC C9: error string must be non-empty");
    BOOST_TEST_MESSAGE("R3_CC C9 PASS bit-flip committed records fail-closed err=" << err);
}

// C10: uncommitted garbage appended after the committed region of
// tip-records.dat -> Open SUCCEEDS and recovers exactly the committed tip
// (garbage tail truncated/ignored, oracle A by-value).
BOOST_AUTO_TEST_CASE(c10_uncommitted_garbage_truncated)
{
    const std::string dir = R3_CC_MakeTempDir();
    const int baseTip = 1000;
    uint256 committedTip;
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 30, 900, baseTip, &tip, NULL));
        R3_CC_BuildChain(tip, 2, uint256(0x7777UL), baseTip, &committedTip);
        BOOST_REQUIRE_EQUAL(tip.TipRecordCount(), 2u);
        tip.Close();
    }
    // Append a well-formed-but-uncommitted extra record (crash-mid-append shape)
    // directly after the committed region, WITHOUT touching tip.meta.
    BlockIndexRecord bogus = R3_CC_MakeRecord(uint256(0xDEADUL), committedTip, baseTip + 3, false);
    std::vector<unsigned char> enc;
    BOOST_REQUIRE(EncodeBlockIndexRecordV1(bogus, &enc, NULL));
    {
        FILE* f = fopen(R3_CC_RecordsPath(dir).c_str(), "ab");
        BOOST_REQUIRE(f != NULL);
        BOOST_REQUIRE(fwrite(&enc[0], 1, enc.size(), f) == enc.size());
        fclose(f);
    }
    // And raw garbage nibble after that (byte-length corruption of the very tail).
    std::vector<unsigned char> recs2 = R3_CC_ReadFile(R3_CC_RecordsPath(dir));
    std::vector<unsigned char> withGarbage = recs2;
    withGarbage.push_back(0x5A);
    BOOST_REQUIRE_MESSAGE(withGarbage.size() == recs2.size() + 1,
                          "R3_CC C10: garbage append sizing");
    R3_CC_WriteFile(R3_CC_RecordsPath(dir), withGarbage);
    BOOST_TEST_MESSAGE("R3_CC C10 setup: records file extended by uncommitted tail, "
                       << withGarbage.size() << " bytes total");
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE_MESSAGE(BlockIndexTipAuthority::Open(dir, 30, &tip, NULL),
                              "R3_CC C10: uncommitted garbage tail must not fail Open");
        BOOST_REQUIRE_EQUAL(tip.TipHeight(), baseTip + 2);
        BOOST_REQUIRE_EQUAL(tip.TipRecordCount(), 2u);
        BOOST_REQUIRE(tip.TipHash() == committedTip);
        BOOST_TEST_MESSAGE("R3_CC C10 PASS oracle A: garbage truncated, committed tip recovered");
    }
}

// C11: truncated tip-invalid.dat shorter than the committed invalidLogCount ->
// fail closed (committed authority cannot be validated).
BOOST_AUTO_TEST_CASE(c11_truncated_invalid_log_fails_closed)
{
    const std::string dir = R3_CC_MakeTempDir();
    const uint256 hA(0xA1UL);
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 31, 900, 444, &tip, NULL));
        BOOST_REQUIRE(tip.SetOperatorInvalid(hA, true, NULL) == BLOCK_INDEX_TIP_OK);
        BOOST_REQUIRE_EQUAL(tip.InvalidLogCount(), 1u);
        tip.Close();
    }
    // Header (12) + one committed entry (33) expected; cut into the entry.
    std::vector<unsigned char> inv = R3_CC_ReadFile(R3_CC_InvalidPath(dir));
    BOOST_REQUIRE_MESSAGE(inv.size() >= 12 + 33,
                          "R3_CC C11: unexpected invalid-log layout");
    BOOST_REQUIRE(R3_CC_TruncateFileTo(R3_CC_InvalidPath(dir), 12 + 10));
    BlockIndexTipAuthority tip;
    std::string err;
    const bool ok = BlockIndexTipAuthority::Open(dir, 31, &tip, &err);
    BOOST_CHECK_MESSAGE(!ok, "R3_CC C11: invalid log shorter than committed count MUST fail closed");
    BOOST_CHECK_MESSAGE(!err.empty(), "R3_CC C11: error string must be non-empty");
    BOOST_TEST_MESSAGE("R3_CC C11 PASS truncated invalid-log fail-closed err=" << err);
}

// C12: SetOperatorInvalid(invalidate=true) then reopen -> the operator-invalid
// set is recovered EXACTLY (round-trip sanity for the invalid authority).
BOOST_AUTO_TEST_CASE(c12_invalid_set_roundtrip_reopen)
{
    const std::string dir = R3_CC_MakeTempDir();
    const uint256 hA(0xA1UL), hB(0xB2UL);
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 32, 900, 700, &tip, NULL));
        BOOST_REQUIRE(!tip.IsOperatorInvalid(hA));
        BOOST_REQUIRE(tip.SetOperatorInvalid(hA, true, NULL) == BLOCK_INDEX_TIP_OK);
        BOOST_REQUIRE(tip.SetOperatorInvalid(hB, true, NULL) == BLOCK_INDEX_TIP_OK);
        BOOST_REQUIRE_EQUAL(tip.InvalidLogCount(), 2u);
        tip.Close();
    }
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE_MESSAGE(BlockIndexTipAuthority::Open(dir, 32, &tip, NULL),
                              "R3_CC C12: reopen must succeed");
        std::set<uint256> s = tip.OperatorInvalidSet();
        BOOST_REQUIRE_EQUAL(s.size(), 2u);
        BOOST_REQUIRE_MESSAGE(tip.IsOperatorInvalid(hA), "R3_CC C12: hA must be recovered");
        BOOST_REQUIRE_MESSAGE(tip.IsOperatorInvalid(hB), "R3_CC C12: hB must be recovered");
        BOOST_REQUIRE_EQUAL(tip.InvalidLogCount(), 2u);
        BOOST_TEST_MESSAGE("R3_CC C12 PASS invalid set recovered exactly (size=" << s.size() << ")");
    }
}

// C13: non-residency / no-static-state regression assertion: after reopen the
// recovered tip is by-value and identical (hash/height/count). PM1-P0-06
// (bounded residency) is deliberately NOT closed by this check.
BOOST_AUTO_TEST_CASE(c13_reopen_byvalue_identical_no_static_state)
{
    const std::string dir = R3_CC_MakeTempDir();
    const int baseTip = 1100;
    uint256 committedTip;
    const int expectedHeight = baseTip + 4;
    const uint64_t expectedCount = 4;
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 33, 900, baseTip, &tip, NULL));
        R3_CC_BuildChain(tip, 4, uint256(0x8888UL), baseTip, &committedTip);
        tip.Close();
    }
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE_MESSAGE(BlockIndexTipAuthority::Open(dir, 33, &tip, NULL),
                              "R3_CC C13: reopen must succeed");
        // by-value identity after reopen (recovered state, not a pointer copy)
        BOOST_REQUIRE(tip.TipHash() == committedTip);
        BOOST_REQUIRE_EQUAL(tip.TipHeight(), expectedHeight);
        BOOST_REQUIRE_EQUAL(tip.TipRecordCount(), expectedCount);
        BlockIndexTipRead t = tip.GetTip();
        BOOST_REQUIRE_MESSAGE(t.status == BLOCK_INDEX_TIP_OK,
                              "R3_CC C13: GetTip must resolve after reopen");
        BOOST_REQUIRE(t.record.hash == committedTip);
        BOOST_REQUIRE_EQUAL(t.height, expectedHeight);
        BOOST_TEST_MESSAGE("R3_CC C13 PASS by-value tip recovered identically after reopen "
                           "(PM1-P0-06 deliberately NOT closed)");
    }
}

// =====================================================================
// R3F — durability FAILURE INJECTION on the checked primitives (PM1-P1-02).
// These inject a real fsync/dir-fsync/rename failure (not a crash failpoint)
// into the checked durability primitive and prove: the failure PROPAGATES,
// the commit is not silently acknowledged, and the crash oracle still
// resolves to exactly one of {previous committed (A), new committed (B),
// fail-closed (D)} — never a hybrid.
// =====================================================================

// D1: injected FILE fsync failure -> Append fails, tip.meta not committed -> oracle A.
BOOST_AUTO_TEST_CASE(d1_file_sync_failure_propagates_oracle_A)
{
    const std::string dir = R3_CC_MakeTempDir();
    const int baseTip = 1200;
    uint256 committedTip;
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 41, 900, baseTip, &tip, NULL));
        R3_CC_BuildChain(tip, 1, uint256(0xB1B1UL), baseTip, &committedTip);
        DurabilityFailpointSetForTesting("FILE_SYNC", true);
        BlockIndexTipAppend a;
        a.record = R3_CC_MakeRecord(uint256(0xB1B2UL), committedTip, baseTip + 2, false);
        a.derived = R3_CC_MakeDerived(uint256(11), 4102);
        std::string err;
        BlockIndexTipStatus st = tip.Append(a, baseTip + 2, &err);
        DurabilityFailpointSetForTesting("FILE_SYNC", false);
        BOOST_CHECK_MESSAGE(st != BLOCK_INDEX_TIP_OK,
                            "R3_CC D1: injected file-sync failure must abort the append");
        BOOST_CHECK_MESSAGE(err.find("injected file sync failure") != std::string::npos,
                            "R3_CC D1: error must carry the injected reason: " << err);
        BOOST_TEST_MESSAGE("R3_CC D1 append aborted (st=" << st << " err=" << err << ")");
        // In-memory authority must NOT be ahead of the last committed tip.meta.
        BOOST_CHECK_EQUAL(tip.TipHeight(), baseTip + 1);
        BOOST_CHECK_EQUAL(tip.TipRecordCount(), 1u);
        BOOST_CHECK(tip.TipHash() == committedTip);
        tip.Close();
    }
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE_MESSAGE(BlockIndexTipAuthority::Open(dir, 41, &tip, NULL),
                              "R3_CC D1: reopen must succeed (oracle A)");
        BOOST_REQUIRE_EQUAL(tip.TipHeight(), baseTip + 1);
        BOOST_REQUIRE_EQUAL(tip.TipRecordCount(), 1u);
        BOOST_REQUIRE(tip.TipHash() == committedTip);
        BOOST_TEST_MESSAGE("R3_CC D1 PASS oracle A (file-sync failure propagated, previous tip intact)");
    }
}

// D2: injected DIRECTORY fsync failure on the first tail publication -> oracle A.
BOOST_AUTO_TEST_CASE(d2_dir_sync_failure_propagates_oracle_A)
{
    const std::string dir = R3_CC_MakeTempDir();
    const int baseTip = 1300;
    uint256 committedTip;
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 42, 900, baseTip, &tip, NULL));
        R3_CC_BuildChain(tip, 1, uint256(0xC2C2UL), baseTip, &committedTip);
        DurabilityFailpointSetForTesting("DIR_SYNC", true);
        BlockIndexTipAppend a;
        a.record = R3_CC_MakeRecord(uint256(0xC2C3UL), committedTip, baseTip + 2, false);
        a.derived = R3_CC_MakeDerived(uint256(12), 4202);
        std::string err;
        BlockIndexTipStatus st = tip.Append(a, baseTip + 2, &err);
        DurabilityFailpointSetForTesting("DIR_SYNC", false);
        BOOST_CHECK_MESSAGE(st != BLOCK_INDEX_TIP_OK,
                            "R3_CC D2: injected dir-sync failure must abort the append");
        BOOST_CHECK_MESSAGE(err.find("injected directory sync failure") != std::string::npos,
                            "R3_CC D2: error must carry the injected reason: " << err);
        BOOST_CHECK_EQUAL(tip.TipHeight(), baseTip + 1);
        tip.Close();
    }
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE_MESSAGE(BlockIndexTipAuthority::Open(dir, 42, &tip, NULL),
                              "R3_CC D2: reopen must succeed (oracle A)");
        BOOST_REQUIRE_EQUAL(tip.TipHeight(), baseTip + 1);
        BOOST_REQUIRE(tip.TipHash() == committedTip);
        BOOST_TEST_MESSAGE("R3_CC D2 PASS oracle A (dir-sync failure propagated; uncommitted tail discarded)");
    }
}

// D3: injected RENAME failure -> previous committed file intact -> oracle A.
BOOST_AUTO_TEST_CASE(d3_rename_failure_propagates_oracle_A)
{
    const std::string dir = R3_CC_MakeTempDir();
    const int baseTip = 1400;
    uint256 committedTip;
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 43, 900, baseTip, &tip, NULL));
        R3_CC_BuildChain(tip, 1, uint256(0xD3D3UL), baseTip, &committedTip);
        DurabilityFailpointSetForTesting("RENAME", true);
        BlockIndexTipAppend a;
        a.record = R3_CC_MakeRecord(uint256(0xD3D4UL), committedTip, baseTip + 2, false);
        a.derived = R3_CC_MakeDerived(uint256(13), 4302);
        std::string err;
        BlockIndexTipStatus st = tip.Append(a, baseTip + 2, &err);
        DurabilityFailpointSetForTesting("RENAME", false);
        BOOST_CHECK_MESSAGE(st != BLOCK_INDEX_TIP_OK,
                            "R3_CC D3: injected rename failure must abort the append");
        BOOST_CHECK_MESSAGE(err.find("injected rename failure") != std::string::npos,
                            "R3_CC D3: error must carry the injected reason: " << err);
        tip.Close();
    }
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE_MESSAGE(BlockIndexTipAuthority::Open(dir, 43, &tip, NULL),
                              "R3_CC D3: reopen must succeed (oracle A)");
        BOOST_REQUIRE_EQUAL(tip.TipHeight(), baseTip + 1);
        BOOST_REQUIRE(tip.TipHash() == committedTip);
        BOOST_TEST_MESSAGE("R3_CC D3 PASS oracle A (rename failure propagated, committed file intact)");
    }
}

// D4: injected directory-sync failure AFTER the tip.meta rename (the explicit
// uncertain-durability point): the rename is already visible, so the outcome must
// be oracle B (new committed authority) — NOT a hybrid and NOT a silent success.
BOOST_AUTO_TEST_CASE(d4_meta_dir_sync_failure_after_rename_oracle_B)
{
    const std::string dir = R3_CC_MakeTempDir();
    const int baseTip = 1500;
    uint256 committedTip;
    uint256 newTip;
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 44, 900, baseTip, &tip, NULL));
        R3_CC_BuildChain(tip, 1, uint256(0xE4E4UL), baseTip, &committedTip);
        newTip = uint256(0xE4E5UL);
        DurabilityFailpointSetForTesting("META_DIR_SYNC", true);
        BlockIndexTipAppend a;
        a.record = R3_CC_MakeRecord(newTip, committedTip, baseTip + 2, false);
        a.derived = R3_CC_MakeDerived(uint256(14), 4402);
        std::string err;
        BlockIndexTipStatus st = tip.Append(a, baseTip + 2, &err);
        DurabilityFailpointSetForTesting("META_DIR_SYNC", false);
        BOOST_CHECK_MESSAGE(st != BLOCK_INDEX_TIP_OK,
                            "R3_CC D4: dir-sync failure after rename must be reported, not acknowledged");
        BOOST_CHECK_MESSAGE(err.find("injected meta directory sync failure") != std::string::npos,
                            "R3_CC D4: error must carry the injected reason: " << err);
        // The protocol must NOT pretend the commit failed on disk: the rename is
        // visible; the in-memory rollback is only a local view.
        BOOST_TEST_MESSAGE("R3_CC D4 append reported failure (st=" << st << " err=" << err << ")");
        tip.Close();
    }
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE_MESSAGE(BlockIndexTipAuthority::Open(dir, 44, &tip, NULL),
                              "R3_CC D4: reopen must succeed (oracle B: rename was the commit)");
        BOOST_REQUIRE_EQUAL(tip.TipHeight(), baseTip + 2);
        BOOST_REQUIRE_EQUAL(tip.TipRecordCount(), 2u);
        BOOST_REQUIRE(tip.TipHash() == newTip);
        BOOST_TEST_MESSAGE("R3_CC D4 PASS oracle B (new authority visible; failure was still reported, no hybrid)");
    }
}

// Fixtures for the R3G block (below): mainnet fork-gate inertness (POEM/DAG
// inert at any usable test height) + block identity derivation.
static bool& R3G_FRegTest() { return ::fRegTest; }
static bool& R3G_FTestNet() { return ::fTestNet; }

// =====================================================================
// R3G (PM1-P0-05) — deterministic derived-dat rebuild on the V2 reader.
//
// The reader's Open previously FAILED CLOSED on any PRESENT-BUT-CORRUPT
// derived.dat. R3G proves the new contract: when the AUTHORITATIVE
// reconstruction source (generation records/active + blk dir) is valid,
// Open DETERMINISTICALLY REBUILDS the derived state in memory (generation
// immutability preserved — no file inside the published generation is
// rewritten) with EXACT semantic parity, and still FAILS CLOSED when the
// authoritative source itself is invalid (never a converted failing reader).
//
// Fixture: a REAL generation builder run (identical derived semantics to the
// reader's replay primitive by construction), >= 5 active records with
// DIFFERING per-block trust so the cumulative chainTrust sequence matters.
// =====================================================================

// ---- fixture --------------------------------------------------------------
struct R3G_GenFixture
{
    boost::filesystem::path root;
    std::vector<uint256> hashes;          // ascending active heights 0..tip
    std::vector<BlockIndexDerivedEntry> derived; // ORIGINAL builder values, ascending heights
    int tip;
    std::string derivedPath;              // gen-000001/derived.dat
    std::vector<unsigned char> derivedBytes; // pre-corruption file copy
    bool frSaved, ftSaved;                // network flag restore (RAII)

    explicit R3G_GenFixture(int t) : tip(t)
    {
        frSaved = R3G_FRegTest(); ftSaved = R3G_FTestNet();
        R3G_FRegTest() = false; R3G_FTestNet() = false; // mainnet: POEM/DAG inert
        root = boost::filesystem::temp_directory_path() /
               boost::filesystem::unique_path("r3g-gen-%%%%-%%%%");
        boost::filesystem::create_directories(root / "blocks");

        // Non-trivial chain: POEM-inert per-block trust varies through the
        // reciprocal formula ((1<<256)/(target+1)) because nTime/nNonce make
        // each block hash differ — the cumulative chainTrust sequence is
        // strictly increasing and cannot collapse to zero or sameness.
        std::vector<BlockIndexRecord> recs;
        uint256 prev(0);
        for (int h = 0; h <= tip; ++h)
        {
            // Real serialized block (identity-verified nSize materialization).
            CBlock blk; blk.nVersion = 1; blk.hashPrevBlock = prev;
            blk.nTime = 1700000000u + 37u * (unsigned)h + 11u;
            blk.nBits = 0x1d00ffffU; blk.nNonce = (unsigned)h;
            CTransaction coin; coin.nVersion = 1; coin.nTime = blk.nTime;
            CTxIn in; in.prevout = COutPoint(uint256(0), 0xffffffff);
            in.scriptSig = CScript() << OP_TRUE; in.nSequence = 0xffffffff;
            coin.vin.push_back(in);
            CTxOut xo; xo.nValue = 0; xo.scriptPubKey = CScript() << OP_TRUE;
            coin.vout.push_back(xo);
            blk.vtx.push_back(coin); blk.hashMerkleRoot = blk.BuildMerkleTree();
            CDataStream ss(SER_DISK, CLIENT_VERSION); ss << blk;
            const std::string blkPath = (root / "blocks" / "blk0001.dat").string();
            FILE* bf = fopen(blkPath.c_str(), "ab");
            BOOST_REQUIRE(bf != NULL);
            unsigned char magic[] = {0xfa,0xbf,0xb5,0xda};
            fwrite(magic,1,4,bf);
            unsigned ns = ss.size();
            fwrite(&ns,4,1,bf);
            long pos = ftell(bf);
            fwrite(&ss[0],1,ss.size(),bf); fflush(bf); fclose(bf);

            BlockIndexRecord rec;
            rec.hash = blk.GetHash(); rec.hashPrev = prev; rec.height = h;
            rec.nVersion = 1; rec.nTime = blk.nTime;
            rec.nBits = 0x1d00ffffU; rec.nNonce = (unsigned)h;
            rec.nFile = 1; rec.nBlockPos = (unsigned)pos; rec.nFlags = 0;
            rec.nMoneySupply = 0;
            recs.push_back(rec);
            hashes.push_back(rec.hash);
            prev = rec.hash;
        }

        BlockIndexGenerationSource src;
        for (int h = 0; h <= tip; ++h)
        {
            BlockIndexGenerationSourceRecord sr;
            sr.hash = recs[h].hash; sr.record = recs[h];
            src.records.push_back(sr);
        }
        src.hashBestChain = hashes[tip];
        src.foundBestChain = true;
        src.blockDataDir = (root / "blocks").string();

        BlockIndexGenerationBuilder b;
        BlockIndexGenerationStats st;
        std::string error;
        BOOST_REQUIRE_MESSAGE(b.Build(src, (root / "blockindex-build-000001.tmp").string(), 1, &st, &error), error);
        b.Close();
        BOOST_REQUIRE_MESSAGE(
            BlockIndexGenerationManager::PublishGeneration(root.string(), 1, &error) ==
                (int)BLOCK_INDEX_LIFECYCLE_OK, error);
        BOOST_REQUIRE_MESSAGE(
            BlockIndexGenerationManager::SelectGeneration(root.string(), 1, &error) ==
                (int)BLOCK_INDEX_LIFECYCLE_OK, error);

        // Capture the ORIGINAL builder-derived entries (ascending heights) and
        // the original derived.dat bytes BEFORE any corruption.
        boost::filesystem::path gd = root / BlockIndexGenerationManager::GenerationName(1);
        derivedPath = (gd / BLOCK_INDEX_DERIVED_FILE_NAME).string();
        derivedBytes = R3_CC_ReadFile(derivedPath);
        BOOST_REQUIRE(derivedBytes.size() >= BLOCK_INDEX_DERIVED_HEADER_SIZE_V2);
        BlockIndexDerivedStateStore dstore;
        BOOST_REQUIRE_MESSAGE(BlockIndexDerivedStateStore::OpenReadOnly(gd.string(), 1, &dstore, &error), error);
        for (int h = 0; h <= tip; ++h)
        {
            // RecordIds are hash-sorted at build; resolve via the reader.
            BlockIndexV2Reader probe;
            BOOST_REQUIRE_MESSAGE(probe.Open(root.string(), BlockIndexV2ReaderOptions(), &error), error);
            BlockIndexSnapshot snap;
            BOOST_REQUIRE_MESSAGE(probe.LookupByHash(hashes[h], &snap, &error) == BLOCK_INDEX_V2_READ_FOUND, error);
            BlockIndexDerivedEntry de;
            BOOST_REQUIRE(dstore.Read(snap.id, &de, &error) == BLOCK_INDEX_DERIVED_LOOKUP_FOUND);
            derived.push_back(de);
            probe.Close();
        }
        BOOST_REQUIRE(!(derived[tip].chainTrust == uint256(0))); // cumulative trust is nondegenerate
        BOOST_REQUIRE(!(derived[0].chainTrust == derived[1].chainTrust)); // differing per-block trust
    }

    void CorruptDerived()
    {
        // Byte-level payload corruption (entry body, not header): makes the
        // companion unreadable while every authoritative byte stays intact.
        std::vector<unsigned char> bad = derivedBytes;
        BOOST_REQUIRE(bad.size() > BLOCK_INDEX_DERIVED_HEADER_SIZE_V2 + 10);
        bad[BLOCK_INDEX_DERIVED_HEADER_SIZE_V2 + 5] ^= 0xFF; // first entry chainTrust byte
        BOOST_REQUIRE(bad.size() == derivedBytes.size());
        R3_CC_WriteFile(derivedPath, bad);
    }
    void RestoreDerived()
    {
        R3_CC_WriteFile(derivedPath, derivedBytes);
    }
    ~R3G_GenFixture()
    {
        R3G_FRegTest() = frSaved; R3G_FTestNet() = ftSaved;
    }
};

// Vote a derived contract through the reader: Open succeeds exactly when the
// oracle says so; on success every snapshot must still expose the exact
// cumulative chainTrust of the original derived values.
static void R3G_ReadAllTrustsViaReader(const R3G_GenFixture& fx,
                                       std::vector<uint256>* outTrusts,
                                       std::string* openError)
{
    outTrusts->clear();
    BlockIndexV2Reader reader;
    if (!reader.Open(fx.root.string(), BlockIndexV2ReaderOptions(), openError))
        return;
    for (int h = 0; h <= fx.tip; ++h)
    {
        BlockIndexSnapshot snap;
        std::string err;
        if (reader.LookupByHash(fx.hashes[h], &snap, &err) != BLOCK_INDEX_V2_READ_FOUND)
            return; // leave the rest empty (must not happen after a successful Open)
        outTrusts->push_back(snap.nChainTrust);
    }
    reader.Close();
}

// R3G-1: corrupt derived + valid authoritative source -> rebuild PASS
// (Open succeeds and every served chainTrust equals the original value).
BOOST_AUTO_TEST_CASE(r3g1_corrupt_derived_rebuild_pass)
{
    R3G_GenFixture fx(6); // 7 active records, differing trusts

    // Phase 1 — intact baseline: Open owns the sealed companion.
    std::vector<uint256> baseline;
    std::string err;
    R3G_ReadAllTrustsViaReader(fx, &baseline, &err);
    BOOST_REQUIRE_MESSAGE(baseline.size() == (size_t)(fx.tip + 1),
                          "R3G1: intact read-through baseline " << err);
    BOOST_TEST_MESSAGE("R3G1 baseline: records=" << fx.derived.size()
                       << " tipTrust(b0)=" << baseline[fx.tip].GetHex());

    // Phase 2 — corrupt the companion; rebuild must still PASS.
    fx.CorruptDerived();
    std::vector<uint256> rebuilt;
    R3G_ReadAllTrustsViaReader(fx, &rebuilt, &err);
    BOOST_REQUIRE_MESSAGE(rebuilt.size() == (size_t)(fx.tip + 1),
                          "R3G1: rebuild must PASS (got " << rebuilt.size() << ", err=" << err << ")");
    for (int h = 0; h <= fx.tip; ++h)
        BOOST_REQUIRE_MESSAGE(rebuilt[h] == baseline[h],
                              "R3G1: rebuilt trust differs at h=" << h);
    BOOST_TEST_MESSAGE("R3G1 PASS: corrupt derived.dat rebuilt deterministically; "
                       "recordCount=" << fx.derived.size()
                       << " tipChainTrust(orig)=" << baseline[fx.tip].GetHex()
                       << " tipChainTrust(rebuilt)=" << rebuilt[fx.tip].GetHex());
}

static std::string& R3G_E() // unique per-case error sink identifier
{
    static std::string sink;
    return sink;
}

// R3G-2: exact semantic parity — the rebuilt map equals the ORIGINAL builder
// entries field-for-field (chainTrust EXACT equality on the cumulative
// sequence, checksum value, and modifier-memo availability).
BOOST_AUTO_TEST_CASE(r3g2_exact_semantic_parity)
{
    R3G_GenFixture fx(6);
    const std::string genDir = (fx.root / BlockIndexGenerationManager::GenerationName(1)).string();

    // Direct comparison of the shared primitive's rebuilt map (the exact
    // object the reader installs) against the ORIGINAL builder entries.
    FixedBlockIndexOpenOptions sopts;
    FixedBlockIndexStore store;
    BOOST_REQUIRE_MESSAGE(FixedBlockIndexStore::OpenReadOnly(genDir, sopts, &store, &R3G_E()), "store open");
    const FixedBlockIndexManifest manifest = store.GetManifest();
    BlockIndexActiveIndex active;
    BOOST_REQUIRE_MESSAGE(BlockIndexActiveIndex::Open(genDir, 1, &active, &R3G_E()), "active open");
    std::map<BlockIndexId, BlockIndexDerivedEntry> rebuilt;
    BlockIndexDerivedReplayStatus rst;
    std::string rerr;
    BOOST_REQUIRE_MESSAGE(RebuildAllDerivedFromAuthoritative(store, active, manifest,
                                                             (fx.root / "blocks").string(),
                                                             &rebuilt, &rst, &rerr), rerr);
    BOOST_REQUIRE_EQUAL(rebuilt.size(), (size_t)(fx.tip + 1));

    BlockIndexV2Reader idMap;
    BOOST_REQUIRE_MESSAGE(idMap.Open(fx.root.string(), BlockIndexV2ReaderOptions(), &R3G_E()), "id map open");
    for (int h = 0; h <= fx.tip; ++h)
    {
        BlockIndexSnapshot snap;
        BOOST_REQUIRE_MESSAGE(idMap.LookupByHash(fx.hashes[h], &snap, &R3G_E()) == BLOCK_INDEX_V2_READ_FOUND, "id map");
        const BlockIndexDerivedEntry& orig = fx.derived[h];
        const BlockIndexDerivedEntry& rebuiltEntry = rebuilt[snap.id];
        BOOST_REQUIRE(rebuiltEntry.chainTrust == orig.chainTrust);
        BOOST_REQUIRE_EQUAL(rebuiltEntry.stakeModifierChecksum, orig.stakeModifierChecksum);
        BOOST_REQUIRE_EQUAL(rebuiltEntry.HasStakeModifierTime(), orig.HasStakeModifierTime());
        if (orig.HasStakeModifierTime())
            BOOST_REQUIRE_EQUAL((long long)rebuiltEntry.stakeModifierTime, (long long)orig.stakeModifierTime);
        BOOST_TEST_MESSAGE("R3G2 h=" << h << " id=" << snap.id
                           << " chainTrust(orig)=" << orig.chainTrust.GetHex()
                           << " chainTrust(rebuilt)=" << rebuiltEntry.chainTrust.GetHex()
                           << " checksum(orig)=" << orig.stakeModifierChecksum
                           << " checks=" << rebuiltEntry.stakeModifierChecksum
                           << " replayed=1");
    }
    BOOST_TEST_MESSAGE("R3G2 PASS: EXACT parity on all " << (fx.tip + 1)
                       << " entries; cumulative tipChainTrust="
                       << fx.derived[fx.tip].chainTrust.GetHex()
                       << " replayedCount=" << rebuilt.size());
}

// R3G-3: invalid/missing authoritative replay source -> FAIL-CLOSED
// (authoritative corruption never converts into a derived rebuild).
BOOST_AUTO_TEST_CASE(r3g3_invalid_authoritative_fails_closed)
{
    // 3a corrupted RECORDS payload deep inside an active record.
    {
        R3G_GenFixture fx(5);
        const std::string recordsPath =
            (fx.root / BlockIndexGenerationManager::GenerationName(1) / "records.dat").string();
        std::vector<unsigned char> recs = R3_CC_ReadFile(recordsPath);
        BOOST_REQUIRE(recs.size() > BLOCK_INDEX_RECORDS_HEADER_SIZE_V1 + BLOCK_INDEX_RECORD_SIZE_V1 + 4);
        recs[BLOCK_INDEX_RECORDS_HEADER_SIZE_V1 + BLOCK_INDEX_RECORD_SIZE_V1 + 3] ^= 0xFF;
        R3_CC_WriteFile(recordsPath, recs);

        std::vector<uint256> trusts;
        std::string err;
        R3G_ReadAllTrustsViaReader(fx, &trusts, &err);
        BOOST_CHECK_MESSAGE(trusts.empty() && !err.empty(),
                            "R3G3a: corrupt records.dat MUST fail closed, err=" << err);
        BOOST_TEST_MESSAGE("R3G3a PASS: open fail-closed on corrupt authoritative records; err=" << err);
    }
    // 3b missing/truncated ACTIVE REGISTRY (authoritative store damage).
    {
        R3G_GenFixture fx(5);
        const std::string activePath =
            (fx.root / BlockIndexGenerationManager::GenerationName(1) / "active.dat").string();
        std::vector<unsigned char> act = R3_CC_ReadFile(activePath);
        BOOST_REQUIRE(act.size() > BLOCK_INDEX_ACTIVE_HEADER_SIZE_V1 + 8);
        act.resize(BLOCK_INDEX_ACTIVE_HEADER_SIZE_V1 + 8); // truncate past the tip entries
        R3_CC_WriteFile(activePath, act);

        std::vector<uint256> trusts;
        std::string err;
        R3G_ReadAllTrustsViaReader(fx, &trusts, &err);
        BOOST_CHECK_MESSAGE(trusts.empty() && !err.empty(),
                            "R3G3b: truncated active.dat MUST fail closed, err=" << err);
        BOOST_TEST_MESSAGE("R3G3b PASS: open fail-closed on truncated authoritative active.dat; err=" << err);
    }
}

// R3G-4: rebuild-interruption via the one-shot durability injection ->
// FAIL-CLOSED and then a clean, deterministic retry.
BOOST_AUTO_TEST_CASE(r3g4_durability_failpoint_interrupted_rebuild_fail_closed_then_retry)
{
    R3G_GenFixture fx(6);
    BOOST_REQUIRE(!fx.derivedPath.empty());
    // Pre-read the derived bytes (fixture already did) so the corrupted file
    // never carries value into the injection phase.
    fx.CorruptDerived();

    // Arm the shared one-shot failpoint: the immutable-generation rebuild is
    // in-memory (no FileCommit/Rename/SyncDirectory to inject into), so the
    // interruption is injected at the replay boundary and the rebuild must
    // FAIL CLOSED (nothing partial served).
    DurabilityFailpointSetForTesting("DERIVED_REBUILD", true);
    std::vector<uint256> trusts;
    std::string err;
    R3G_ReadAllTrustsViaReader(fx, &trusts, &err);
    DurabilityFailpointSetForTesting("DERIVED_REBUILD", false);
    BOOST_CHECK_MESSAGE(!err.empty(),
                        "R3G4: interrupted rebuild MUST fail closed, err=" << err);
    BOOST_TEST_MESSAGE("R3G4 interrupted rebuild: fail-closed err=" << err);

    // Clean retry (failpoint disarmed above): the corruption is still present,
    // so the rebuild runs UNINTERRUPTED and must pass deterministically.
    std::vector<uint256> baseline;
    std::string err2;
    R3G_ReadAllTrustsViaReader(fx, &baseline, &err2);
    BOOST_REQUIRE_MESSAGE(baseline.size() == (size_t)(fx.tip + 1),
                          "R3G4: retry must rebuild deterministically (err=" << err2 << ")");
    BOOST_TEST_MESSAGE("R3G4 retry PASS: rebuilt tipChainTrust="
                       << baseline[fx.tip].GetHex() << " (expected " << fx.derived[fx.tip].chainTrust.GetHex() << ")");
    BOOST_REQUIRE_MESSAGE(baseline[fx.tip] == fx.derived[fx.tip].chainTrust,
                          "R3G4: retry trust parity");
    // Determinism: a second retry yields the SAME value (by-value, no drift).
    std::vector<uint256> again;
    R3G_ReadAllTrustsViaReader(fx, &again, &err2);
    BOOST_REQUIRE_MESSAGE(again.size() == baseline.size() &&
                          again[fx.tip] == baseline[fx.tip],
                          "R3G4: second retry must be identical");
}

// R3G-5: bounded rebuild — no full-history materialization.
BOOST_AUTO_TEST_CASE(r3g5_bounded_no_full_history_materialization)
{
    R3G_GenFixture fx(6);
    const std::string genDir = (fx.root / BlockIndexGenerationManager::GenerationName(1)).string();
    FixedBlockIndexOpenOptions sopts;
    FixedBlockIndexStore store;
    BOOST_REQUIRE_MESSAGE(FixedBlockIndexStore::OpenReadOnly(genDir, sopts, &store, &R3G_E()), "store open");
    const FixedBlockIndexManifest manifest = store.GetManifest();
    BlockIndexActiveIndex active;
    BOOST_REQUIRE_MESSAGE(BlockIndexActiveIndex::Open(genDir, 1, &active, &R3G_E()), "active open");

    const uint64_t recordsBefore = mapBlockIndex.size();
    std::map<BlockIndexId, BlockIndexDerivedEntry> rebuilt;
    BlockIndexDerivedReplayStatus rst;
    std::string rerr;
    BOOST_REQUIRE_MESSAGE(RebuildAllDerivedFromAuthoritative(store, active, manifest,
                                                             "", &rebuilt, &rst, &rerr), rerr);
    BOOST_REQUIRE_EQUAL(rebuilt.size(), (size_t)(fx.tip + 1));
    BOOST_CHECK_EQUAL(mapBlockIndex.size(), recordsBefore);   // no map growth
    BOOST_TEST_MESSAGE("R3G5 PASS: bounded rebuild, entries=" << rebuilt.size()
                       << " mapBlockIndexDelta=" << (mapBlockIndex.size() - recordsBefore));
}

BOOST_AUTO_TEST_SUITE_END()
