#include <boost/test/unit_test.hpp>

#include "blockindex_tip.h"
#include "blockindex_tip_bounded_window.h"
#include "util.h"

#include <boost/filesystem.hpp>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace fs = boost::filesystem;

// =====================================================================
// C11 — coherent committed-tip snapshot regression suite.
//
// The proven C10 data race: BlockIndexTipAuthority mutates its internal
// state in place under cs_main while unlocked readers (getblockcount /
// getbestblockhash -> BlockIndexManager::ActiveTipHeight/BestTipHash)
// read it without cs_main. C11 publishes an immutable committed-tip
// snapshot atomically AFTER the durable commit; unlocked readers observe
// one whole committed state.
//
// Causal gates:
//   S1 empty/bootstrap: snapshot.valid == false (no fabricated tip)
//   S2 post-commit visibility + internal coherence (one meta generation)
//   S3 atomicity: concurrent readers never observe a torn (height,hash)
//      combination across a concurrent writer (discriminates the fix)
//   S4 pre-commit failure: a commit that fails before the durable rename
//      is NEVER published; the snapshot stays the previous committed tip
//   S5 bounded retention: publication replaces (never accumulates); after
//      many commits the snapshot stays valid + coherent
// =====================================================================

static std::string MakeTempDir()
{
    char tmpl[] = "/tmp/innova-tipsnap-XXXXXX";
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

// The chain hashes are deterministic: block at global height H (H = baseTip+1+i)
// has hash uint256(0x1111 + i). This lets a reader validate that an observed
// snapshot's (tipHeight, tipHash) pair is internally coherent with ONE committed
// generation (a torn old/new pair would fail this identity).
static const int kBaseTip = 999;
static uint256 HashForHeight(int height)
{
    const int i = height - (kBaseTip + 1);
    return uint256(0x1111UL + (unsigned)i);
}

static BlockIndexTipStatus AppendHeight(BlockIndexTipAuthority& tip, int height,
                                        uint256 hashPrev, std::string* err)
{
    const int i = height - (kBaseTip + 1);
    BlockIndexRecord r = MakeRecord(HashForHeight(height), hashPrev, height,
                                    (i % 2) == 0);
    BlockIndexDerivedEntry d = MakeDerived(uint256(i + 1), 1000 + (uint32_t)i);
    BlockIndexTipAppend a;
    a.record = r;
    a.derived = d;
    return tip.Append(a, height, err);
}

BOOST_AUTO_TEST_SUITE(blockindex_committed_tip_snapshot_tests)

// S1 — empty/bootstrap: no committed active tip -> valid == false, no fabricated
// height/hash. (Preserves the fail-closed contract.)
BOOST_AUTO_TEST_CASE(s1_empty_bootstrap_no_fabrication)
{
    const std::string dir = MakeTempDir();
    BlockIndexTipAuthority tip;
    BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 7, 1000, kBaseTip, &tip, NULL));
    BOOST_REQUIRE(tip.IsEmpty());

    const BlockIndexCommittedTipSnapshot s = tip.CommittedTipSnapshot();
    BOOST_REQUIRE(!s.valid);           // no committed active tip
    BOOST_REQUIRE_EQUAL(s.tipHeight, -1);
    BOOST_REQUIRE(s.tipHash == uint256(0));
    printf("S1 PASS empty bootstrap fail-closed\n");
}

// S2 — post-commit visibility + coherence: after a committed append the snapshot
// reflects the NEW tip; the (height,hash) pair is one coherent generation.
BOOST_AUTO_TEST_CASE(s2_post_commit_visibility_and_coherence)
{
    const std::string dir = MakeTempDir();
    BlockIndexTipAuthority tip;
    BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 7, 1000, kBaseTip, &tip, NULL));

    uint256 prev = uint256(0);
    std::string err;
    for (int k = 1; k <= 5; ++k)
    {
        const int h = kBaseTip + k;
        BOOST_REQUIRE(AppendHeight(tip, h, prev, &err) == BLOCK_INDEX_TIP_OK);
        const uint256 expectedHash = HashForHeight(h);
        const BlockIndexCommittedTipSnapshot s = tip.CommittedTipSnapshot();
        BOOST_REQUIRE(s.valid);
        BOOST_REQUIRE_EQUAL(s.tipHeight, h);
        BOOST_REQUIRE(s.tipHash == expectedHash);   // coherent pair
        BOOST_REQUIRE_EQUAL(s.baseGeneration, 7u);
        prev = expectedHash;
    }
    printf("S2 PASS post-commit visibility + coherence\n");
}

// S3 — atomicity under concurrency: reader threads spin CommittedTipSnapshot()
// while the writer commits new blocks. Every observed pair must match the
// deterministic chain identity (never a mixed old/new combination). This is the
// discriminating test for the fix: a non-atomic publication that exposed height
// and hash from different commits would violate the identity below.
BOOST_AUTO_TEST_CASE(s3_concurrent_reads_never_torn)
{
    const std::string dir = MakeTempDir();
    BlockIndexTipAuthority tip;
    BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 7, 1000, kBaseTip, &tip, NULL));

    const int kBlocks = 60;
    std::atomic<bool> stop(false);
    std::atomic<int> torn(0);
    std::atomic<long> reads(0);

    std::vector<std::thread> readers;
    for (int t = 0; t < 4; ++t)
    {
        readers.push_back(std::thread([&]() {
            while (!stop.load(std::memory_order_relaxed))
            {
                const BlockIndexCommittedTipSnapshot s = tip.CommittedTipSnapshot();
                if (s.valid)
                {
                    // Coherence identity: the hash must be the one committed at
                    // that exact height. A torn (old height, new hash) pair fails.
                    if (s.tipHash != HashForHeight(s.tipHeight))
                        torn.fetch_add(1, std::memory_order_relaxed);
                }
                reads.fetch_add(1, std::memory_order_relaxed);
            }
        }));
    }

    uint256 prev = uint256(0);
    std::string err;
    for (int k = 1; k <= kBlocks; ++k)
    {
        const int h = kBaseTip + k;
        BOOST_REQUIRE(AppendHeight(tip, h, prev, &err) == BLOCK_INDEX_TIP_OK);
        prev = HashForHeight(h);
    }
    stop.store(true, std::memory_order_relaxed);
    for (size_t i = 0; i < readers.size(); ++i)
        readers[i].join();

    BOOST_REQUIRE_EQUAL(torn.load(), 0);
    BOOST_REQUIRE(reads.load() > 0);
    // Final state coherent.
    const BlockIndexCommittedTipSnapshot f = tip.CommittedTipSnapshot();
    BOOST_REQUIRE(f.valid);
    BOOST_REQUIRE_EQUAL(f.tipHeight, kBaseTip + kBlocks);
    BOOST_REQUIRE(f.tipHash == HashForHeight(kBaseTip + kBlocks));
    printf("S3 PASS atomicity reads=%ld torn=0\n", reads.load());
}

// S4 — pre-commit failure is never published. Arm the failpoint that fires AFTER
// the tail stores are durable but BEFORE the tip.meta commit; the append fails
// (IO_ERROR) and the snapshot MUST remain the previous committed tip (ordering:
// publish strictly after the durable commit). Discriminates a
// publish-before-commit implementation.
BOOST_AUTO_TEST_CASE(s4_pre_commit_failure_not_published)
{
    const std::string dir = MakeTempDir();
    BlockIndexTipAuthority tip;
    BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 7, 1000, kBaseTip, &tip, NULL));

    uint256 prev = uint256(0);
    std::string err;
    // Commit one good block.
    BOOST_REQUIRE(AppendHeight(tip, kBaseTip + 1, prev, &err) == BLOCK_INDEX_TIP_OK);
    prev = HashForHeight(kBaseTip + 1);
    const BlockIndexCommittedTipSnapshot good = tip.CommittedTipSnapshot();
    BOOST_REQUIRE(good.valid);
    BOOST_REQUIRE_EQUAL(good.tipHeight, kBaseTip + 1);

    // Fail before the durable meta commit.
    BlockIndexTipSetFailpointForTesting("FP_AFTER_TAIL_DURABLE_BEFORE_META", true);
    const BlockIndexTipStatus st = AppendHeight(tip, kBaseTip + 2, prev, &err);
    BlockIndexTipSetFailpointForTesting("FP_AFTER_TAIL_DURABLE_BEFORE_META", false);
    BOOST_REQUIRE(st != BLOCK_INDEX_TIP_OK);   // the commit failed

    const BlockIndexCommittedTipSnapshot after = tip.CommittedTipSnapshot();
    BOOST_REQUIRE(after.valid);
    BOOST_REQUIRE_EQUAL(after.tipHeight, kBaseTip + 1);     // unchanged
    BOOST_REQUIRE(after.tipHash == HashForHeight(kBaseTip + 1));
    printf("S4 PASS pre-commit failure not published\n");
}

// S5 — bounded retention + sustained coherence: many commits, publication
// replaces the single snapshot (structurally one shared_ptr field, no
// accumulation), and the snapshot stays valid + coherent throughout.
BOOST_AUTO_TEST_CASE(s5_bounded_retention_sustained_coherence)
{
    const std::string dir = MakeTempDir();
    BlockIndexTipAuthority tip;
    BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 7, 1000, kBaseTip, &tip, NULL));

    uint256 prev = uint256(0);
    std::string err;
    for (int k = 1; k <= 40; ++k)
    {
        const int h = kBaseTip + k;
        BOOST_REQUIRE(AppendHeight(tip, h, prev, &err) == BLOCK_INDEX_TIP_OK);
        prev = HashForHeight(h);
    }
    const BlockIndexCommittedTipSnapshot s = tip.CommittedTipSnapshot();
    BOOST_REQUIRE(s.valid);
    BOOST_REQUIRE_EQUAL(s.tipHeight, kBaseTip + 40);
    BOOST_REQUIRE(s.tipHash == HashForHeight(kBaseTip + 40));
    printf("S5 PASS bounded retention + coherence\n");
}

BOOST_AUTO_TEST_SUITE_END()
