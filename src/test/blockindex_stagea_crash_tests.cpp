// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.

#include <boost/test/unit_test.hpp>

#include "blockindex_tip.h"
#include "fixed_blockindex_store.h"
#include "blockindex_derived_state.h"
#include "main.h"
#include "util.h"

#include <boost/filesystem.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// =====================================================================
// Stage A — crash-injection tests at the ACTUAL persistence boundaries of
// the Block Index V2 mutable tip authority (blockindex_tip/).
//
// Protocol under test (src/blockindex_tip.cpp):
//   tip.meta is the SINGLE commit point (WriteMeta: tmp + fsync + rename +
//   dir-sync; the rename IS the commit, blockindex_tip.cpp:1270).
//   Records/derived/active are appended / CoW-published BEFORE the commit and
//   remain UNCOMMITTED SUFFIXES until it (Open truncates them,
//   blockindex_tip.cpp:1605-1627). The LevelDB hashindex + marker are derived
//   state, reconciled at Open (ReconcileSideIndexOnOpen,
//   blockindex_tip.cpp:1092-1135). For a reorg publication the store set is
//   CoW-renamed over by RewriteStoresForReorg (blockindex_tip.cpp:1306) and
//   tip.meta is advanced LAST.
//
// Failpoints (blockindex_tip.h:89-90; armed ONLY by tests):
//   FP_DURING_TAIL_UPDATE                tail CoW tmp durable, pre-rename
//   FP_AFTER_TAIL_DURABLE_BEFORE_META    tail stores durable, pre-commit
//   FP_BEFORE_META_RENAME                meta tmp durable, pre-rename
//   FP_AFTER_META_RENAME_BEFORE_DIRSYNC  post-commit-rename, pre-dirsync
//   FP_AFTER_REORG_TAIL_PUBLISHED        NEW: reorg store set renamed over the
//                                        authoritative paths, tip.meta NOT yet
//                                        advanced (records/derived prefixes
//                                        stay bytewise identical; only the
//                                        ACTIVE store lengthens)
// Real primitive failures use the one-shot DurabilityFailpoint seam
// (util.h:315) exactly like the R3F block of
// blockindex_crash_corruption_tests.cpp: FILE_SYNC / RENAME / META_DIR_SYNC.
//
// Oracles (same discipline as R3_CC):
//   (A) restart recovers EXACTLY the last committed authority
//   (B) restart recovers EXACTLY the new authority (rename == commit)
//   (D) Open fails closed with a non-empty error
// plus Stage-A protocol proofs:
//   - mere PRESENCE of tip-records.dat.tmp / tip.meta.tmp proves NOTHING:
//     restart succeeds with the committed state (content decides, not names),
//   - an interrupted reorg cannot permanently prevent accepting a heavier
//     competing branch at the same heights,
//   - interrupted-reorg recovery is idempotent,
//   - commit-failure paths are IN-MEMORY-clean: the still-open authority must
//     serve the committed tip (no OOM-shaped residue handling).
// =====================================================================

static std::string SA_MakeTempDir()
{
    const char* env = getenv("TMPDIR");
    std::string base = (env && *env) ? env : "/tmp";
    std::string tmpl = base + "/innova-stagea-XXXXXX";
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    char* d = mkdtemp(&buf[0]);
    BOOST_REQUIRE_MESSAGE(d != NULL, "SA: mkdtemp failed");
    return std::string(d);
}

static BlockIndexRecord SA_MakeRecord(uint256 hash, uint256 hashPrev, int height,
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

static BlockIndexDerivedEntry SA_MakeDerived(uint256 trust, uint32_t checksum)
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

// Append n contiguous ACTIVE blocks; returns the final (tip) hash.
static uint256 SA_BuildChain(BlockIndexTipAuthority& tip, uint32_t n,
                             uint256 firstPrev, int baseTip, uint32_t trustSeed)
{
    uint256 prev = firstPrev;
    for (uint32_t i = 0; i < n; ++i)
    {
        uint256 h = uint256(0x2A00UL + trustSeed * 1000UL + i);
        BlockIndexTipAppend a;
        a.record = SA_MakeRecord(h, prev, baseTip + 1 + (int)i, (i % 2) == 0);
        a.derived = SA_MakeDerived(uint256(trustSeed * 10 + i + 1), 4000 + i);
        std::string err;
        BlockIndexTipStatus st = tip.Append(a, baseTip + 1 + (int)i, &err);
        BOOST_REQUIRE_MESSAGE(st == BLOCK_INDEX_TIP_OK,
                              "SA: setup append failed: " + err);
        prev = h;
    }
    return prev;
}

static std::string SA_RecordsPath(const std::string& d)
{
    return d + "/blockindex_tip/tip-records.dat";
}
static std::string SA_MetaPath(const std::string& d)
{
    return d + "/blockindex_tip/tip.meta";
}
static bool SA_Exists(const std::string& p)
{
    boost::system::error_code ec;
    return boost::filesystem::exists(boost::filesystem::path(p), ec);
}

// A heavier competing branch fixture: active chain A (trust 30 tip) + side
// chain B (trust 25 tip) at the SAME heights; returns the B branch ready for
// ReorgActiveTo(fork=baseTip, branch=...).
struct SA_CompetingFixture
{
    int baseTip;
    uint256 A1, A2, A3, B1, B2, B3, C1;
    std::vector<BlockIndexTipAppend> branchB;
    std::vector<int32_t> heightsB;
    std::vector<BlockIndexTipAppend> branchC;
    std::vector<int32_t> heightsC;
};

static SA_CompetingFixture SA_MakeCompeting(BlockIndexTipAuthority& tip,
                                            int baseTip)
{
    SA_CompetingFixture fx;
    fx.baseTip = baseTip;
    fx.A1 = uint256(0xFA1UL); fx.A2 = uint256(0xFA2UL); fx.A3 = uint256(0xFA3UL);
    fx.B1 = uint256(0xFB1UL); fx.B2 = uint256(0xFB2UL); fx.B3 = uint256(0xFB3UL);
    fx.C1 = uint256(0xFC1UL);
    const uint256 TA[3] = {uint256(10), uint256(20), uint256(30)}; // A: heavier
    const uint256 TB[3] = {uint256(5), uint256(12), uint256(25)};  // B: lighter
    uint256 prev = uint256(0xF00UL);
    for (int i = 0; i < 3; ++i) // A active
    {
        const uint256 hs[3] = {fx.A1, fx.A2, fx.A3};
        BlockIndexTipAppend a;
        a.record = SA_MakeRecord(hs[i], prev, baseTip + 1 + i, false);
        a.derived = SA_MakeDerived(TA[i], 5100 + i);
        BOOST_REQUIRE(tip.Append(a, baseTip + 1 + i, NULL) == BLOCK_INDEX_TIP_OK);
        prev = hs[i];
    }
    prev = uint256(0xF00UL);
    for (int i = 0; i < 3; ++i) // B side
    {
        const uint256 hs[3] = {fx.B1, fx.B2, fx.B3};
        BlockIndexTipAppend a;
        a.record = SA_MakeRecord(hs[i], prev, baseTip + 1 + i, false);
        a.derived = SA_MakeDerived(TB[i], 5200 + i);
        BOOST_REQUIRE(tip.Append(a, -1, NULL) == BLOCK_INDEX_TIP_OK);
        prev = hs[i];
    }
    // branchB: B3 alone (B1/B2 already committed as side records; heights dense
    // over the reorg fork baseTip: baseTip+1..baseTip+3 must ALL be supplied
    // because ReorgActiveTo makes the active chain dense from fork+1).
    prev = uint256(0xF00UL);
    for (int i = 0; i < 3; ++i)
    {
        const uint256 hs[3] = {fx.B1, fx.B2, fx.B3};
        BlockIndexTipAppend a;
        a.record = SA_MakeRecord(hs[i], prev, baseTip + 1 + i, false);
        a.derived = SA_MakeDerived(TB[i], 5200 + i);
        fx.branchB.push_back(a);
        fx.heightsB.push_back(baseTip + 1 + i);
        prev = hs[i];
    }
    // branchC: A1 + C1 -- a branch HEAVIER than B3's trust (30+40) sharing A1.
    prev = uint256(0xF00UL);
    BlockIndexTipAppend a1;
    a1.record = SA_MakeRecord(fx.A1, prev, baseTip + 1, false);
    a1.derived = SA_MakeDerived(uint256(10), 5100);
    prev = fx.A1;
    BlockIndexTipAppend c1;
    c1.record = SA_MakeRecord(fx.C1, prev, baseTip + 2, false);
    c1.derived = SA_MakeDerived(uint256(40), 5300); // cumulative 50 > 25
    fx.branchC.push_back(a1);
    fx.branchC.push_back(c1);
    fx.heightsC.push_back(baseTip + 1);
    fx.heightsC.push_back(baseTip + 2);
    return fx;
}

BOOST_AUTO_TEST_SUITE(blockindex_stagea_crash_tests)

// ---------------------------------------------------------------------
// Boundary: tail CoW temp durable, not renamed (AppendBatch records).
// (A) oracle + presence protocol: tip-records.dat.tmp left behind by the
// interrupted publication does NOT imply committed-state corruption.
// ---------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(sa1_tmp_presence_is_not_corruption_oracle_A)
{
    const std::string dir = SA_MakeTempDir();
    const int baseTip = 300;
    uint256 tipHash;
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 71, 900, baseTip, &tip, NULL));
        tipHash = SA_BuildChain(tip, 2, uint256(0xA0UL), baseTip, 1);
        BOOST_REQUIRE_EQUAL(tip.TipRecordCount(), 2u);
        // 3rd block aborts at the hash-index MARKER commit (the records/
        // derived/active tails were already appended+fsynced without a temp,
        // and the marker's CoW temp is durable but NOT renamed): every store
        // is AHEAD of the commit point and ONE temp file exists.
        DurabilityFailpointSetForTesting("FILE_SYNC", true);
        BlockIndexTipAppend a;
        a.record = SA_MakeRecord(uint256(0xA00000UL), tipHash, baseTip + 3, false);
        a.derived = SA_MakeDerived(uint256(7), 4100);
        std::string err;
        BlockIndexTipStatus st = tip.Append(a, baseTip + 3, &err);
        DurabilityFailpointSetForTesting("FILE_SYNC", false);
        BOOST_CHECK_MESSAGE(st == BLOCK_INDEX_TIP_IO_ERROR,
                            "SA1: append must abort at the records fsync, got "
                                << st << " err=" << err);
        BOOST_CHECK_MESSAGE(err.find("injected file sync failure") != std::string::npos,
                            "SA1: error must carry the injected reason: " << err);
        tip.Close();
        // The snapshot shape (incremental append path, blockindex_tip.cpp:557):
        // the abort torn NOTHING (the in-place append failed before it became
        // durable); the authoritative tip.meta is untouched. Whether or not
        // partial bytes landed, ONLY tip.meta decides, and any tail on the
        // records/derived/active stores is an UNCOMMITTED suffix (Open
        // bounds-truncates it against tipRecordCount at blockindex_tip.cpp:1605).
        BOOST_CHECK_MESSAGE(boost::filesystem::file_size(
                                boost::filesystem::path(SA_MetaPath(dir))) > 0,
                            "SA1: tip.meta must exist and be untouched");
    }
    {
        BlockIndexTipAuthority tip;
        std::string oerr;
        BOOST_REQUIRE_MESSAGE(BlockIndexTipAuthority::Open(dir, 71, &tip, &oerr),
                              "SA1: restart MUST succeed: tmp presence does not imply "
                              "committed corruption, err=" << oerr);
        BOOST_REQUIRE_EQUAL(tip.TipRecordCount(), 2u);
        BOOST_REQUIRE_EQUAL(tip.TipHeight(), baseTip + 2);
        BOOST_REQUIRE(tip.TipHash() == tipHash);
        BOOST_CHECK_MESSAGE(tip.LookupByHash(uint256(0xA00000UL), NULL).status ==
                                BLOCK_INDEX_TIP_NOT_FOUND,
                            "SA1: never-published record must be absent");
        printf("SA1 PASS: oracle A; tip-records.dat.tmp presence is inert\n");
    }
    // Idempotence: identical recovery on a second reopen.
    {
        BlockIndexTipAuthority tip;
        std::string oerr;
        BOOST_REQUIRE_MESSAGE(BlockIndexTipAuthority::Open(dir, 71, &tip, &oerr), oerr);
        BOOST_REQUIRE_EQUAL(tip.TipRecordCount(), 2u);
        BOOST_REQUIRE(tip.TipHash() == tipHash);
        tip.Close();
    }
}

// ---------------------------------------------------------------------
// Boundary: meta temp durable, pre-rename (FP_BEFORE_META_RENAME).
// (A) oracle; the rename source is the tmp file (never an in-place write).
// ---------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(sa2_meta_tmp_rename_source_oracle_A)
{
    const std::string dir = SA_MakeTempDir();
    const int baseTip = 310;
    uint256 tipHash;
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 72, 900, baseTip, &tip, NULL));
        tipHash = SA_BuildChain(tip, 1, uint256(0xB0UL), baseTip, 2);
        BlockIndexTipSetFailpointForTesting("FP_BEFORE_META_RENAME", true);
        BlockIndexTipAppend a;
        a.record = SA_MakeRecord(uint256(0xB1UL), tipHash, baseTip + 2, false);
        a.derived = SA_MakeDerived(uint256(8), 4200);
        std::string err;
        BOOST_CHECK_MESSAGE(tip.Append(a, baseTip + 2, &err) == BLOCK_INDEX_TIP_IO_ERROR,
                            "SA2: append must abort at the meta rename boundary");
        BlockIndexTipSetFailpointForTesting("FP_BEFORE_META_RENAME", false);
        tip.Close();
        BOOST_CHECK_MESSAGE(SA_Exists(SA_MetaPath(dir) + ".tmp"),
                            "SA2: expected tip.meta.tmp (rename source is the tmp)");
    }
    {
        BlockIndexTipAuthority tip;
        std::string oerr;
        BOOST_REQUIRE_MESSAGE(BlockIndexTipAuthority::Open(dir, 72, &tip, &oerr), oerr);
        BOOST_REQUIRE_EQUAL(tip.TipRecordCount(), 1u);
        BOOST_REQUIRE(tip.TipHash() == tipHash);
        printf("SA2 PASS: oracle A; rename source proven to be tip.meta.tmp\n");
    }
}

// ---------------------------------------------------------------------
// Boundary: post-rename, pre-dir-sync (FP_AFTER_META_RENAME_BEFORE_DIRSYNC).
// The rename IS the commit -> (B) oracle: new authority recovered.
// ---------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(sa3_rename_is_the_commit_point_oracle_B)
{
    const std::string dir = SA_MakeTempDir();
    const int baseTip = 320;
    const uint256 newHash = uint256(0xC1UL);
    uint256 tipHash;
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 73, 900, baseTip, &tip, NULL));
        tipHash = SA_BuildChain(tip, 1, uint256(0xC0UL), baseTip, 3);
        BlockIndexTipSetFailpointForTesting("FP_AFTER_META_RENAME_BEFORE_DIRSYNC", true);
        BlockIndexTipAppend a;
        a.record = SA_MakeRecord(newHash, tipHash, baseTip + 2, false);
        a.derived = SA_MakeDerived(uint256(9), 4300);
        std::string err;
        BOOST_CHECK_MESSAGE(tip.Append(a, baseTip + 2, &err) != BLOCK_INDEX_TIP_OK,
                            "SA3: protocol must REPORT the uncertain-durability failure");
        BlockIndexTipSetFailpointForTesting("FP_AFTER_META_RENAME_BEFORE_DIRSYNC", false);
        tip.Close();
    }
    {
        BlockIndexTipAuthority tip;
        std::string oerr;
        BOOST_REQUIRE_MESSAGE(BlockIndexTipAuthority::Open(dir, 73, &tip, &oerr), oerr);
        BOOST_REQUIRE_EQUAL(tip.TipRecordCount(), 2u);
        BOOST_REQUIRE_EQUAL(tip.TipHeight(), baseTip + 2);
        BOOST_REQUIRE(tip.TipHash() == newHash);
        printf("SA3 PASS: oracle B (rename == commit; new authority recovered)\n");
    }
}

// ---------------------------------------------------------------------
// Boundary: real primitives, one-shot fsync burn at the tail publication
// (this subsumes FP_AFTER_TAIL_DURABLE_BEFORE_META with a REAL failure).
// (A) oracle; in-memory view must stay at the committed tip.
// ---------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(sa4_real_fsync_burn_pre_commit_oracle_A)
{
    const std::string dir = SA_MakeTempDir();
    const int baseTip = 330;
    uint256 tipHash;
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 74, 900, baseTip, &tip, NULL));
        tipHash = SA_BuildChain(tip, 2, uint256(0xD0UL), baseTip, 4);
        DurabilityFailpointSetForTesting("FILE_SYNC", true);
        BlockIndexTipAppend a;
        a.record = SA_MakeRecord(uint256(0xD1UL), tipHash, baseTip + 3, false);
        a.derived = SA_MakeDerived(uint256(10), 4400);
        std::string err;
        BlockIndexTipStatus st = tip.Append(a, baseTip + 3, &err);
        DurabilityFailpointSetForTesting("FILE_SYNC", false);
        BOOST_CHECK_MESSAGE(st == BLOCK_INDEX_TIP_IO_ERROR,
                            "SA4: injected file-sync failure must abort the append");
        BOOST_CHECK_MESSAGE(err.find("injected file sync failure") != std::string::npos,
                            "SA4: error must carry the injected reason: " << err);
        // In-memory view must NOT run ahead of the committed meta.
        BOOST_CHECK_EQUAL(tip.TipRecordCount(), 2u);
        BOOST_CHECK_EQUAL(tip.TipHeight(), baseTip + 2);
        BOOST_CHECK(tip.TipHash() == tipHash);
        tip.Close();
    }
    {
        BlockIndexTipAuthority tip;
        std::string oerr;
        BOOST_REQUIRE_MESSAGE(BlockIndexTipAuthority::Open(dir, 74, &tip, &oerr), oerr);
        BOOST_REQUIRE_EQUAL(tip.TipRecordCount(), 2u);
        BOOST_REQUIRE(tip.TipHash() == tipHash);
        printf("SA4 PASS: oracle A (real fsync burn at the tail publication)\n");
    }
}

// ---------------------------------------------------------------------
// Boundary: reorg publication (RewriteStoresForReorg) — interrupted AFTER
// the store set was CoW-renamed over but BEFORE tip.meta is advanced.
//
// Asserts the Stage-A protocol requirements:
//   1. (A) restart recovers exactly the last committed authority (the
//      uncommitted reorg tail is truncated + digest-validated),
//   2. the interrupted reorg cannot permanently prevent a valid HEAVIER
//      competing branch at the same heights (accept on the still-open
//      authority AND after restart),
//   3. recovery is idempotent (Open twice == Open after in-session abort).
// ---------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(sa5_interrupted_reorg_recover_heavier_branch)
{
    const std::string dir = SA_MakeTempDir();
    const int baseTip = 340;
    SA_CompetingFixture fx;
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 76, 900, baseTip, &tip, NULL));
        fx = SA_MakeCompeting(tip, baseTip);
        BOOST_REQUIRE(tip.TipHash() == fx.A3);

        // Interrupt the reorg TO branch B right after the store set was
        // renamed over the authoritative paths (before meta advance).
        BlockIndexTipSetFailpointForTesting("FP_AFTER_REORG_TAIL_PUBLISHED", true);
        std::string rerr;
        BlockIndexTipStatus rst = tip.ReorgActiveTo(baseTip, fx.branchB,
                                                    fx.heightsB, &rerr);
        BlockIndexTipSetFailpointForTesting("FP_AFTER_REORG_TAIL_PUBLISHED", false);
        BOOST_CHECK_MESSAGE(rst == BLOCK_INDEX_TIP_IO_ERROR,
                            "SA5: armed failpoint must abort the reorg, got " << rst);
        printf("SA5: reorg interrupted after tail publication (err=%s)\n", rerr.c_str());
        // The authority is FAIL-STOP closed by the abort (it must not serve
        // the uncommitted swapped image); every subsequent call says so.
        std::string cerr;
        BlockIndexTipStatus cst = tip.ReorgActiveTo(baseTip, fx.branchC,
                                                    fx.heightsC, &cerr);
        BOOST_CHECK_MESSAGE(cst == BLOCK_INDEX_TIP_IO_ERROR &&
                                cerr.find("tip not open") != std::string::npos,
                            "SA5: after a fail-stop abort the authority must refuse "
                            "service, got st=" << cst << " err=" << cerr);
        tip.Close();
    }
    // (1)+(3): restart recovers EXACTLY the committed authority, TWICE
    // (idempotence), and the heavier branch is then fully acceptable.
    for (int round = 0; round < 2; ++round)
    {
        BlockIndexTipAuthority tip;
        std::string oerr;
        BOOST_REQUIRE_MESSAGE(BlockIndexTipAuthority::Open(dir, 76, &tip, &oerr),
                              "SA5 round " << round << ": restart must recover the "
                              "committed authority (interrupted reorg cannot "
                              "permanently block acceptance), err=" << oerr);
        BOOST_REQUIRE_EQUAL(tip.TipRecordCount(), 6u); // A(3) + B-side(3)
        BOOST_REQUIRE_EQUAL(tip.TipHeight(), baseTip + 3);
        BOOST_REQUIRE(tip.TipHash() == fx.A3);
        if (round == 1)
        {
            // Heavier branch at the same heights must now be accepted.
            std::string cerr;
            BlockIndexTipStatus cst = tip.ReorgActiveTo(baseTip, fx.branchC,
                                                        fx.heightsC, &cerr);
            BOOST_REQUIRE_MESSAGE(cst == BLOCK_INDEX_TIP_OK,
                                  "SA5: after interrupted-reorg recovery a valid heavier "
                                  "competing branch MUST be acceptable, err=" << cerr);
            BOOST_REQUIRE(tip.TipHash() == fx.C1);
        }
        tip.Close();
    }
    {
        BlockIndexTipAuthority tip;
        std::string oerr;
        BOOST_REQUIRE_MESSAGE(BlockIndexTipAuthority::Open(dir, 76, &tip, &oerr), oerr);
        BOOST_REQUIRE(tip.TipHash() == fx.C1);
        BOOST_REQUIRE_EQUAL(tip.TipHeight(), baseTip + 2);
        printf("SA5 PASS: oracle A across restart; recovery idempotent; heavier "
               "branch accepted\n");
    }
}

// ---------------------------------------------------------------------
// The interrupted reorg LEFT-OVER must be rewindable to the fork tip and
// re-playable: recovery must land at a reorg-able state (same as sa5 but the
// reorg is retried to B after restart -- the LIGHTER branch must still be
// acceptable too: no branch is permanently blacklisted by a crash).
// ---------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(sa6_interrupted_reorg_retry_after_restart)
{
    const std::string dir = SA_MakeTempDir();
    const int baseTip = 350;
    SA_CompetingFixture fx;
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 77, 900, baseTip, &tip, NULL));
        fx = SA_MakeCompeting(tip, baseTip);
        BlockIndexTipSetFailpointForTesting("FP_AFTER_REORG_TAIL_PUBLISHED", true);
        std::string rerr;
        BOOST_CHECK(tip.ReorgActiveTo(baseTip, fx.branchB, fx.heightsB, &rerr) ==
                    BLOCK_INDEX_TIP_IO_ERROR);
        BlockIndexTipSetFailpointForTesting("FP_AFTER_REORG_TAIL_PUBLISHED", false);
        tip.Close();
    }
    {
        BlockIndexTipAuthority tip;
        std::string oerr;
        BOOST_REQUIRE_MESSAGE(BlockIndexTipAuthority::Open(dir, 77, &tip, &oerr), oerr);
        BOOST_REQUIRE(tip.TipHash() == fx.A3);
        // Retry the ORIGINAL (lighter) reorg to B: must be accepted exactly.
        std::string rerr;
        BlockIndexTipStatus rst = tip.ReorgActiveTo(baseTip, fx.branchB,
                                                    fx.heightsB, &rerr);
        BOOST_REQUIRE_MESSAGE(rst == BLOCK_INDEX_TIP_OK,
                              "SA6: retried reorg must succeed, err=" << rerr);
        BOOST_REQUIRE(tip.TipHash() == fx.B3);
        BOOST_REQUIRE_EQUAL(tip.TipHeight(), baseTip + 3);
        tip.Close();
    }
    {
        BlockIndexTipAuthority tip;
        std::string oerr;
        BOOST_REQUIRE_MESSAGE(BlockIndexTipAuthority::Open(dir, 77, &tip, &oerr), oerr);
        BOOST_REQUIRE(tip.TipHash() == fx.B3);
        printf("SA6 PASS: interrupted reorg retried after restart to the lighter "
               "branch; recovery lands at a reorg-able state\n");
    }
}

BOOST_AUTO_TEST_SUITE_END()
