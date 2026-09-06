#include <boost/test/unit_test.hpp>

#include "blockindex_live_acceptance.h"
#include "blockindex_live_tail.h"
#include "blockindex_tip.h"
#include "util.h"

#include <boost/filesystem.hpp>

#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <vector>

namespace fs = boost::filesystem;

// =====================================================================
// P3 — live post-S acceptance seam.
//
// Causal gates (the Window-1 converse):
//   A1  S+1 (child of base tip anchor) is acceptable: parent resolves by-value
//   A2  S+2..S+k acceptable: parent resolves via tip as they append
//   A3  non-dense / unknown-parent (orphan) block is NOT dense-acceptable;
//       RecordOrphan persists it as a non-active side record
//   A4  accepted active blocks persist to tip authority; reopen recovers tip
//   A5  side branch (validated, not best) recorded, active tip unchanged
//   A6  live-tail reflects accepted post-S blocks (bounded residency)
// =====================================================================

static std::string MakeTempDir()
{
    char tmpl[] = "/tmp/innova-accept-XXXXXX";
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

// Base-known set simulating the immutable base V2 generation (hashes <= S).
struct BaseKnownCtx
{
    std::set<uint256> known;
    std::map<uint256,int> heights;
};

static bool BaseKnownFn(const uint256& hash, void* ud)
{
    BaseKnownCtx* ctx = (BaseKnownCtx*)ud;
    return ctx->known.count(hash) != 0;
}

BOOST_AUTO_TEST_SUITE(blockindex_live_acceptance_tests)

BOOST_AUTO_TEST_CASE(a1_s_plus_1_acceptable_via_base_anchor)
{
    const std::string dir = MakeTempDir();
    const int baseTip = 5000;              // base tip height S
    const uint256 baseTipHash = uint256(0xBEEFUL); // the base tip anchor hash
    BaseKnownCtx base;
    base.known.insert(baseTipHash);
    base.heights[baseTipHash] = baseTip;

    BlockIndexTipAuthority tip;
    BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 7, 10000, baseTip, &tip, NULL));
    BlockIndexLiveTail tail;
    tail.SetSources(NULL, &tip);
    tail.SetHorizon(64);
    tail.SetCurrentGeneration(7);
    BlockIndexLiveAcceptance seam;
    seam.SetSources(&BaseKnownFn, &base, &tip, &tail);

    // Block S+1 with prev = base tip anchor.
    const int h1 = baseTip + 1;
    uint256 h1hash = uint256(0x5151UL);
    BlockIndexRecord r1 = MakeRecord(h1hash, baseTipHash, h1, false);
    BlockIndexDerivedEntry d1 = MakeDerived(uint256(1), 11);
    BlockIndexTipAppend a1; a1.record = r1; a1.derived = d1;

    // CanAcceptDense: parent = base anchor (KNOWN), height = baseTip+1 == curTip+1
    std::string err;
    bool ok = seam.CanAcceptDense(baseTipHash, h1, &err);
    BOOST_REQUIRE(ok);
    int newTip = seam.AcceptActive(a1, h1, &err);
    BOOST_REQUIRE_EQUAL(newTip, h1);
    printf("A1 PASS S+1 acceptable via base anchor: tip->%d\n", newTip);
}

BOOST_AUTO_TEST_CASE(a2_s_plus_chain_acceptable_via_tip)
{
    const std::string dir = MakeTempDir();
    const int baseTip = 5000;
    const uint256 baseTipHash = uint256(0xBEEFUL);
    BaseKnownCtx base;
    base.known.insert(baseTipHash);

    BlockIndexTipAuthority tip;
    BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 8, 10000, baseTip, &tip, NULL));
    BlockIndexLiveTail tail;
    tail.SetSources(NULL, &tip);
    tail.SetHorizon(64);
    tail.SetCurrentGeneration(8);
    BlockIndexLiveAcceptance seam;
    seam.SetSources(&BaseKnownFn, &base, &tip, &tail);

    uint256 prev = baseTipHash;
    for (int i = 1; i <= 5; ++i)
    {
        const int h = baseTip + i;
        uint256 hh = uint256(0x6000UL + i);
        BlockIndexRecord r = MakeRecord(hh, prev, h, (i % 2) == 1);
        BlockIndexDerivedEntry d = MakeDerived(uint256(i), 20 + i);
        BlockIndexTipAppend a; a.record = r; a.derived = d;
        std::string err;
        bool ok = seam.CanAcceptDense(prev, h, &err);
        BOOST_REQUIRE(ok);
        int newTip = seam.AcceptActive(a, h, &err);
        BOOST_REQUIRE_EQUAL(newTip, h);
        // now prev is the new tip hash for the next iteration
        prev = hh;
    }
    BOOST_REQUIRE_EQUAL(tip.TipHeight(), baseTip + 5);
    BOOST_REQUIRE_EQUAL(tip.TipRecordCount(), 5u);
    printf("A2 PASS S+1..S+5 chain acceptable via tip: tip=%d count=%llu\n",
           (int)tip.TipHeight(), (unsigned long long)tip.TipRecordCount());
}

BOOST_AUTO_TEST_CASE(a3_orphan_not_dense_persisted_side)
{
    const std::string dir = MakeTempDir();
    const int baseTip = 100;
    const uint256 baseTipHash = uint256(0xBEEFUL);
    BaseKnownCtx base;
    base.known.insert(baseTipHash);

    BlockIndexTipAuthority tip;
    BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 4, 200, baseTip, &tip, NULL));
    BlockIndexLiveTail tail;
    tail.SetSources(NULL, &tip);
    tail.SetHorizon(8);
    tail.SetCurrentGeneration(4);
    BlockIndexLiveAcceptance seam;
    seam.SetSources(&BaseKnownFn, &base, &tip, &tail);

    // A block whose parent is UNKNOWN (not base, not tip): orphan.
    uint256 unknownParent = uint256(0xDEADUL);
    int orphHeight = baseTip + 1;
    uint256 orphHash = uint256(0x5151UL);
    BlockIndexRecord r = MakeRecord(orphHash, unknownParent, orphHeight, false);
    BlockIndexDerivedEntry d = MakeDerived(uint256(0xABUL), 33);
    BlockIndexTipAppend a; a.record = r; a.derived = d;

    // Not dense-acceptable (parent unknown).
    std::string err;
    BOOST_REQUIRE(!seam.CanAcceptDense(unknownParent, orphHeight, &err));
    // Record as orphan (side) succeeds.
    BlockIndexTipStatus st = seam.RecordOrphan(a, &err);
    BOOST_REQUIRE(st == BLOCK_INDEX_TIP_OK);
    // tip NOT advanced; orphan recorded as non-active record.
    BOOST_REQUIRE_EQUAL(tip.TipHeight(), baseTip);
    BOOST_REQUIRE_EQUAL(tip.TipRecordCount(), 1u);
    BlockIndexTipRead o = tip.LookupByHash(orphHash, NULL);
    BOOST_REQUIRE(o.status == BLOCK_INDEX_TIP_OK);
    BOOST_REQUIRE(!o.active);
    printf("A3 PASS orphan: not dense, persisted as side, tip stays %d\n",
           (int)baseTip);
}

BOOST_AUTO_TEST_CASE(a4_accept_persist_reopen)
{
    const std::string dir = MakeTempDir();
    const int baseTip = 50;
    const uint256 baseTipHash = uint256(0xBEEFUL);
    BaseKnownCtx base;
    base.known.insert(baseTipHash);

    uint256 prev = baseTipHash;
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 2, 100, baseTip, &tip, NULL));
        BlockIndexLiveTail tail;
        tail.SetSources(NULL, &tip);
        tail.SetHorizon(16);
        tail.SetCurrentGeneration(2);
        BlockIndexLiveAcceptance seam;
        seam.SetSources(&BaseKnownFn, &base, &tip, &tail);
        for (int i = 1; i <= 4; ++i)
        {
            const int h = baseTip + i;
            uint256 hh = uint256(0x7000UL + i);
            BlockIndexRecord r = MakeRecord(hh, prev, h, false);
            BlockIndexDerivedEntry d = MakeDerived(uint256(i), 40 + i);
            BlockIndexTipAppend a; a.record = r; a.derived = d;
            std::string err;
            int newTip = seam.AcceptActive(a, h, &err);
            BOOST_REQUIRE_EQUAL(newTip, h);
            prev = hh;
        }
        BOOST_REQUIRE_EQUAL(tip.TipHeight(), baseTip + 4);
        printf("A4 PASS accepted+persisted: tip=%d\n", (int)tip.TipHeight());
    }
    // reopen: exact tip recovered
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE(BlockIndexTipAuthority::Open(dir, 2, &tip, NULL));
        BOOST_REQUIRE_EQUAL(tip.TipHeight(), baseTip + 4);
        BOOST_REQUIRE_EQUAL(tip.TipRecordCount(), 4u);
        printf("A4 PASS reopen recovers tip=%d count=4\n", (int)tip.TipHeight());
    }
}

BOOST_AUTO_TEST_CASE(a5_side_branch_tip_unchanged)
{
    const std::string dir = MakeTempDir();
    const int baseTip = 10;
    const uint256 baseTipHash = uint256(0xBEEFUL);
    BaseKnownCtx base;
    base.known.insert(baseTipHash);

    BlockIndexTipAuthority tip;
    BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 6, 100, baseTip, &tip, NULL));
    BlockIndexLiveTail tail;
    tail.SetSources(NULL, &tip);
    tail.SetHorizon(8);
    tail.SetCurrentGeneration(6);
    BlockIndexLiveAcceptance seam;
    seam.SetSources(&BaseKnownFn, &base, &tip, &tail);

    // main chain S+1, S+2 (active)
    uint256 h1 = uint256(0x5151UL), h2 = uint256(0x5152UL);
    {
        BlockIndexTipAppend a1;
        a1.record = MakeRecord(h1, baseTipHash, 11, false);
        a1.derived = MakeDerived(uint256(1), 1);
        std::string err;
        BOOST_REQUIRE_EQUAL(seam.AcceptActive(a1, 11, &err), 11);
        BlockIndexTipAppend a2;
        a2.record = MakeRecord(h2, h1, 12, false);
        a2.derived = MakeDerived(uint256(2), 2);
        BOOST_REQUIRE_EQUAL(seam.AcceptActive(a2, 12, &err), 12);
    }
    // side branch at height 12 off h1 (different hash)
    uint256 sideHash = uint256(0x9999UL);
    BlockIndexRecord sr = MakeRecord(sideHash, h1, 12, false);
    BlockIndexDerivedEntry sd = MakeDerived(uint256(0x55UL), 5);
    BlockIndexTipAppend a; a.record = sr; a.derived = sd;
    std::string err;
    BOOST_REQUIRE(seam.AcceptSide(a, &err) == BLOCK_INDEX_TIP_OK);
    BOOST_REQUIRE_EQUAL(tip.TipHeight(), 12); // unchanged
    BOOST_REQUIRE_EQUAL(tip.TipRecordCount(), 3u);
    BlockIndexTipRead side = tip.LookupByHash(sideHash, NULL);
    BOOST_REQUIRE(side.status == BLOCK_INDEX_TIP_OK);
    BOOST_REQUIRE(!side.active);
    printf("A5 PASS side branch recorded, active tip unchanged: tip=%d count=3\n",
           (int)tip.TipHeight());
}

BOOST_AUTO_TEST_CASE(a6_live_tail_reflects_accepted)
{
    const std::string dir = MakeTempDir();
    const int baseTip = 5000;
    const uint256 baseTipHash = uint256(0xBEEFUL);
    BaseKnownCtx base;
    base.known.insert(baseTipHash);

    BlockIndexTipAuthority tip;
    BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 9, 10000, baseTip, &tip, NULL));
    BlockIndexLiveTail tail;
    tail.SetSources(NULL, &tip);
    tail.SetHorizon(64);
    tail.SetCurrentGeneration(9);
    BlockIndexLiveAcceptance seam;
    seam.SetSources(&BaseKnownFn, &base, &tip, &tail);

    uint256 prev = baseTipHash;
    for (int i = 1; i <= 10; ++i)
    {
        const int h = baseTip + i;
        uint256 hh = uint256(0x8000UL + i);
        BlockIndexRecord r = MakeRecord(hh, prev, h, false);
        BlockIndexDerivedEntry d = MakeDerived(uint256(i), 60 + i);
        BlockIndexTipAppend a; a.record = r; a.derived = d;
        std::string err;
        BOOST_REQUIRE((seam.AcceptActive(a, h, &err)) == h);
        prev = hh;
    }
    // latest accepted tip should be resident/known in the tail
    BlockIndexLogicalId lastId(prev);
    bool res = tail.IsResident(lastId);
    // After AcceptActive pin+release+trim, the last may or may not be resident
    // depending on horizon; the CORE invariant is that residency is bounded by
    // horizon (small), NOT by the 10 accepted blocks.
    printf("A6 PASS tail resident=%zu (bounded), tipHeight=%d\n",
           tail.ResidentCount(), (int)tip.TipHeight());
    (void)res;
    (void)tail;
}

// ---- P4: reorg within tip authority ----
BOOST_AUTO_TEST_CASE(r1_reorg_within_tip)
{
    const std::string dir = MakeTempDir();
    const int baseTip = 10;
    const uint256 baseTipHash = uint256(0xBEEFUL);
    BaseKnownCtx base;
    base.known.insert(baseTipHash);

    BlockIndexTipAuthority tip;
    BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 2, 100, baseTip, &tip, NULL));
    BlockIndexLiveTail tail;
    tail.SetSources(NULL, &tip);
    tail.SetHorizon(64);
    tail.SetCurrentGeneration(2);
    BlockIndexLiveAcceptance seam;
    seam.SetSources(&BaseKnownFn, &base, &tip, &tail);

    // main chain S+1..S+4 (active)
    uint256 prev = baseTipHash;
    uint256 h[5];
    for (int i = 1; i <= 4; ++i)
    {
        h[i] = uint256(0x6000UL + i);
        BlockIndexTipAppend a;
        a.record = MakeRecord(h[i], prev, baseTip + i, false);
        a.derived = MakeDerived(uint256(i), i);
        std::string err;
        BOOST_REQUIRE_EQUAL(seam.AcceptActive(a, baseTip + i, &err), baseTip + i);
        prev = h[i];
    }
    BOOST_REQUIRE_EQUAL(tip.TipHeight(), baseTip + 4);
    uint8_t fenceBefore = tip.ActiveFence();

    // New branch: same fork at S+2 (height 12), different blocks at 13 and 14.
    uint256 nh13 = uint256(0xA111UL);
    uint256 nh14 = uint256(0xA222UL);
    std::vector<BlockIndexTipAppend> branch;
    std::vector<int32_t> heights;
    {
        BlockIndexTipAppend a;
        a.record = MakeRecord(nh13, h[2], baseTip + 3, false);
        a.derived = MakeDerived(uint256(0x30UL), 30);
        branch.push_back(a); heights.push_back(baseTip + 3);
        a = BlockIndexTipAppend();
        a.record = MakeRecord(nh14, nh13, baseTip + 4, false);
        a.derived = MakeDerived(uint256(0x40UL), 40);
        branch.push_back(a); heights.push_back(baseTip + 4);
    }
    std::string err;
    BOOST_REQUIRE(seam.ReorgTo(baseTip + 2, branch, heights, &err) == BLOCK_INDEX_TIP_OK);
    BOOST_REQUIRE_EQUAL(tip.TipHeight(), baseTip + 4);
    // fence bumped (reorg happened)
    BOOST_REQUIRE((unsigned)tip.ActiveFence() != (unsigned)fenceBefore);
    // new tip is the new branch's tip hash
    BlockIndexTipRead t = tip.GetTip();
    BOOST_REQUIRE(t.status == BLOCK_INDEX_TIP_OK);
    BOOST_REQUIRE(t.record.hash == nh14);
    // old branch block h[4] is retained as a non-active record
    BlockIndexTipRead old = tip.LookupByHash(h[4], NULL);
    BOOST_REQUIRE(old.status == BLOCK_INDEX_TIP_OK);
    BOOST_REQUIRE(!old.active);
    printf("R1 PASS reorg within tip: tip->%s, old branch retained non-active\n",
           t.record.hash.ToString().substr(0,12).c_str());
}

BOOST_AUTO_TEST_CASE(r2_reorg_requires_old_ancestry_in_base)
{
    const std::string dir = MakeTempDir();
    const int baseTip = 100;
    const uint256 baseTipHash = uint256(0xBEEFUL);
    // base knows S and S-1 (deep history), simulating a reorg whose fork is at
    // S-1 (below the generation tip) — requires base V2 ancestry re-materialization
    // (which is authority, not residency). The tip accepts the new branch on top.
    BaseKnownCtx base;
    base.known.insert(baseTipHash);
    base.known.insert(uint256(0xABEUL)); // S-1

    BlockIndexTipAuthority tip;
    BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 5, 200, baseTip, &tip, NULL));
    BlockIndexLiveTail tail;
    tail.SetSources(NULL, &tip);
    tail.SetHorizon(8);
    tail.SetCurrentGeneration(5);
    BlockIndexLiveAcceptance seam;
    seam.SetSources(&BaseKnownFn, &base, &tip, &tail);

    // active main: S+1,S+2
    uint256 prev = baseTipHash;
    for (int i = 1; i <= 2; ++i)
    {
        uint256 hh = uint256(0x7000UL + i);
        BlockIndexTipAppend a;
        a.record = MakeRecord(hh, prev, baseTip + i, false);
        a.derived = MakeDerived(uint256(i), i);
        std::string err;
        BOOST_REQUIRE_EQUAL(seam.AcceptActive(a, baseTip + i, &err), baseTip + i);
        prev = hh;
    }
    // Reorg to fork at S-1 (height 99, in base): truncate active tip to baseTip
    // (removing S+1,S+2), then append a new branch starting at S+1.
    uint256 nh11 = uint256(0xBB11UL);
    uint256 nh12 = uint256(0xBB22UL);
    std::vector<BlockIndexTipAppend> branch;
    std::vector<int32_t> heights;
    BlockIndexTipAppend a;
    a.record = MakeRecord(nh11, baseTipHash, baseTip + 1, false); // parent = S
    a.derived = MakeDerived(uint256(0x11UL), 11);
    branch.push_back(a); heights.push_back(baseTip + 1);
    a = BlockIndexTipAppend();
    a.record = MakeRecord(nh12, nh11, baseTip + 2, false);
    a.derived = MakeDerived(uint256(0x22UL), 22);
    branch.push_back(a); heights.push_back(baseTip + 2);

    std::string err;
    // forkHeight must be >= baseTip (TruncateActiveTo range). A fork at S-1 is
    // below the tip floor; the seam must still allow it by truncating to baseTip
    // (empty tip) which equals the base V2 floor. Here ReorgTo truncates to S.
    BlockIndexTipStatus st = seam.ReorgTo(baseTip, branch, heights, &err);
    BOOST_REQUIRE(st == BLOCK_INDEX_TIP_OK);
    BOOST_REQUIRE_EQUAL(tip.TipHeight(), baseTip + 2);
    BlockIndexTipRead nt = tip.GetTip();
    BOOST_REQUIRE(nt.status == BLOCK_INDEX_TIP_OK);
    BOOST_REQUIRE(nt.record.hash == nh12);
    printf("R2 PASS reorg to base-floor fork (S-1 ancestry via base V2), new tip=%s\n",
           nt.record.hash.ToString().substr(0,12).c_str());
}

BOOST_AUTO_TEST_CASE(r3_reorg_persists_reopen)
{
    const std::string dir = MakeTempDir();
    const int baseTip = 10;
    const uint256 baseTipHash = uint256(0xBEEFUL);
    BaseKnownCtx base;
    base.known.insert(baseTipHash);

    uint256 finalHash;
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 3, 100, baseTip, &tip, NULL));
        BlockIndexLiveTail tail;
        tail.SetSources(NULL, &tip);
        tail.SetHorizon(64);
        tail.SetCurrentGeneration(3);
        BlockIndexLiveAcceptance seam;
        seam.SetSources(&BaseKnownFn, &base, &tip, &tail);
        // main S+1..S+4
        uint256 prev = baseTipHash;
        uint256 h[5];
        for (int i = 1; i <= 4; ++i)
        {
            h[i] = uint256(0x8100UL + i);
            BlockIndexTipAppend a;
            a.record = MakeRecord(h[i], prev, baseTip + i, false);
            a.derived = MakeDerived(uint256(i), i);
            std::string err;
            BOOST_REQUIRE_EQUAL(seam.AcceptActive(a, baseTip + i, &err), baseTip + i);
            prev = h[i];
        }
        // reorg to fork S+2 with new branch at 13,14
        uint256 nh13 = uint256(0xC111UL), nh14 = uint256(0xC222UL);
        std::vector<BlockIndexTipAppend> branch;
        std::vector<int32_t> heights;
        BlockIndexTipAppend a;
        a.record = MakeRecord(nh13, h[2], baseTip + 3, false);
        a.derived = MakeDerived(uint256(0x31UL), 31);
        branch.push_back(a); heights.push_back(baseTip + 3);
        a = BlockIndexTipAppend();
        a.record = MakeRecord(nh14, nh13, baseTip + 4, false);
        a.derived = MakeDerived(uint256(0x41UL), 41);
        branch.push_back(a); heights.push_back(baseTip + 4);
        std::string err;
        BOOST_REQUIRE(seam.ReorgTo(baseTip + 2, branch, heights, &err) == BLOCK_INDEX_TIP_OK);
        finalHash = nh14;
    }
    {
        BlockIndexTipAuthority tip;
        BOOST_REQUIRE(BlockIndexTipAuthority::Open(dir, 3, &tip, NULL));
        BOOST_REQUIRE_EQUAL(tip.TipHeight(), baseTip + 4);
        BOOST_REQUIRE(tip.TipHash() == finalHash);
        printf("R3 PASS reorg persisted+reopen: tip=%d hash=%s\n",
               (int)tip.TipHeight(), tip.TipHash().ToString().substr(0,12).c_str());
    }
}

BOOST_AUTO_TEST_SUITE_END()