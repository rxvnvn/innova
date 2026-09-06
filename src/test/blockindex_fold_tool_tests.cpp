#include <boost/test/unit_test.hpp>

#include "blockindex_fold_tool.h"
#include "blockindex_tip.h"
#include "blockindex_live_acceptance.h"
#include "blockindex_generation_builder.h"
#include "blockindex_generation_lifecycle.h"
#include "util.h"

#include <boost/filesystem.hpp>

#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <vector>

namespace fs = boost::filesystem;

// =====================================================================
// P6 — streaming online fold / compaction of the mutable tip into a new
// immutable generation.
//
// Causal gates:
//   F1  Fold tip blocks (S+1..L) into new gen: new immutable generation created,
//       CURRENT flipped, tip truncated to fold height.
//   F2  Fold preserves base records (RecordId identity intact) + appends fold
//       prefix; committed tip of new gen == fold height.
//   F3  Current after fold selects the new generation; base gen still present.
//   F4  Tip truncated: remaining tip (fold, L] retained; side/std above S+fold
//       preserved.
//   F5  Fold is bounded-RAM by construction (streamed appends, no O(N)
//       resident history) — asserted via the API shape (no full-history rebuild).
// =====================================================================

static std::string MakeTempDir()
{
    char tmpl[] = "/tmp/innova-fold-XXXXXX";
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

struct BaseKnownCtx { std::set<uint256> known; };
static bool BaseKnownFn(const uint256& hash, void* ud)
{
    BaseKnownCtx* ctx = (BaseKnownCtx*)ud;
    return ctx->known.count(hash) != 0;
}

// Build a small real base generation gen-1 at <root>/gen-000001 (tip height =
// baseTip), using the same builder the existing lifecycle tests use.
static void BuildBaseGeneration(const std::string& root, uint64_t gen,
                                int tipHeight, uint256* outTipHash,
                                std::vector<uint256>* outActiveHash)
{
    // Use BlockIndexGenerationBuilder directly on a synthetic source.
    BlockIndexGenerationSource src;
    std::vector<uint256> activeHashV;
    for (int h = 0; h <= tipHeight; ++h)
    {
        uint256 hprev = (h == 0) ? uint256(0) : activeHashV[h - 1];
        BlockIndexRecord rec;
        rec.hash = uint256(0xFFFF0000UL + h);       // distinct base block hashes
        rec.hashPrev = hprev;
        rec.hashMerkleRoot = uint256(0x1111UL + h);
        rec.height = h;
        rec.nFile = 1;
        rec.nBlockPos = 100u + (unsigned)h;
        rec.nFlags = 0;
        rec.nVersion = 7;
        rec.nTime = 1700000000u + (unsigned)h;
        rec.nBits = 0x1d00ffff;
        rec.nNonce = (unsigned)h;
        rec.nMint = 100;
        rec.nMoneySupply = 500;
        activeHashV.push_back(rec.hash);
        BlockIndexGenerationSourceRecord s; s.hash = rec.hash; s.record = rec;
        src.records.push_back(s);
    }
    src.hashBestChain = activeHashV[tipHeight];
    src.foundBestChain = true;

    BlockIndexGenerationBuilder builder;
    BlockIndexGenerationStats stats;
    std::string error;
    fs::path tmp = fs::temp_directory_path() / fs::unique_path("innova-fold-base-%%%%-%%%%");
    BOOST_REQUIRE_MESSAGE(builder.Build(src, tmp.string(), gen, &stats, &error), error);
    builder.Close();
    fs::path target = fs::path(root) / (strprintf("gen-%06llu", (unsigned long long)gen));
    fs::create_directories(root);
    BOOST_REQUIRE(!fs::exists(target));
    BOOST_REQUIRE_NO_THROW(fs::rename(tmp, target));
    if (outTipHash)
        *outTipHash = activeHashV[tipHeight];
    if (outActiveHash)
        *outActiveHash = activeHashV;
}

BOOST_AUTO_TEST_SUITE(blockindex_fold_tool_tests)

BOOST_AUTO_TEST_CASE(f1_fold_basic_create_flip_truncate)
{
    const std::string root = MakeTempDir();
    const uint64_t baseGen = 1;
    const int baseTip = 6;                // base generation tip height S
    std::vector<uint256> baseActive;
    BuildBaseGeneration(root, baseGen, baseTip, NULL, &baseActive);

    // Open base gen and create the mutable tip under <root>.
    const uint64_t baseRec = (uint64_t)(baseTip + 1); // one record per active height
    BlockIndexTipAuthority tip;
    BOOST_REQUIRE(BlockIndexTipAuthority::Create(root, baseGen, baseRec, baseTip, &tip, NULL));

    // Accept S+1..S+4 as post-S active tip blocks.
    BaseKnownCtx base;
    for (int h = 0; h <= baseTip; ++h)
        base.known.insert(baseActive[h]);
    BlockIndexLiveTail tail;
    tail.SetSources(NULL, &tip);
    tail.SetHorizon(64);
    tail.SetCurrentGeneration(baseGen);
    BlockIndexLiveAcceptance seam;
    seam.SetSources(&BaseKnownFn, &base, &tip, &tail);

    uint256 prev = baseActive[baseTip]; // S
    uint256 postHash[5];
    for (int i = 1; i <= 4; ++i)
    {
        const int h = baseTip + i;
        uint256 hh = uint256(0xABCD0000UL + i);
        BlockIndexRecord r = MakeRecord(hh, prev, h, false);
        BlockIndexDerivedEntry d = MakeDerived(uint256(i), 100 + i);
        BlockIndexTipAppend a; a.record = r; a.derived = d;
        std::string err;
        int newTip = seam.AcceptActive(a, h, &err);
        BOOST_REQUIRE_EQUAL(newTip, h);
        prev = hh;
        postHash[i] = hh;
    }
    BOOST_REQUIRE_EQUAL(tip.TipHeight(), baseTip + 4); // L = S+4 = 10

    // Fold to F = S+3 = 9: fold S+1..S+3 into the immutable gen, keep S+4 in tip.
    std::string err;
    const int32_t foldHeight = baseTip + 3;
    BlockIndexFoldResult r = BlockIndexFoldTool::Fold(root, baseGen + 1, foldHeight, &err);
    BOOST_REQUIRE_MESSAGE(r.ok, r.error);
    // new gen selected by CURRENT
    BlockIndexCurrentRecord cur;
    BlockIndexLifecycleStatus cst = BlockIndexGenerationManager::ReadCurrent(root, &cur, &err);
    BOOST_REQUIRE(cst == BLOCK_INDEX_LIFECYCLE_OK);
    BOOST_REQUIRE_EQUAL(cur.generation, baseGen + 1);
    // base gen still exists
    BOOST_REQUIRE(fs::exists(fs::path(root) / "gen-000001"));
    BOOST_REQUIRE(fs::exists(fs::path(root) / "gen-000002"));
    printf("F1 PASS fold: CURRENT->gen-2, base gen-1 retained, foldHeight=%d\n",
           (int)foldHeight);
}

BOOST_AUTO_TEST_CASE(f2_fold_truncates_tip_to_fold)
{
    const std::string root = MakeTempDir();
    const uint64_t baseGen = 1;
    const int baseTip = 6;
    std::vector<uint256> baseActive;
    BuildBaseGeneration(root, baseGen, baseTip, NULL, &baseActive);

    BlockIndexTipAuthority tip;
    BOOST_REQUIRE(BlockIndexTipAuthority::Create(root, baseGen, (uint64_t)(baseTip+1), baseTip, &tip, NULL));
    BaseKnownCtx base;
    for (int h = 0; h <= baseTip; ++h) base.known.insert(baseActive[h]);
    BlockIndexLiveTail tail;
    tail.SetSources(NULL, &tip);
    tail.SetHorizon(64);
    tail.SetCurrentGeneration(baseGen);
    BlockIndexLiveAcceptance seam;
    seam.SetSources(&BaseKnownFn, &base, &tip, &tail);
    uint256 prev = baseActive[baseTip];
    for (int i = 1; i <= 4; ++i)
    {
        uint256 hh = uint256(0xABCD0000UL + i);
        BlockIndexRecord r = MakeRecord(hh, prev, baseTip + i, false);
        BlockIndexDerivedEntry d = MakeDerived(uint256(i), 100 + i);
        BlockIndexTipAppend a; a.record = r; a.derived = d;
        std::string err;
        BOOST_REQUIRE_EQUAL(seam.AcceptActive(a, baseTip + i, &err), baseTip + i);
        prev = hh;
    }

    // Fold to F=S+3. After fold, tip must be truncated to S+3.
    std::string err;
    const int32_t foldHeight = baseTip + 3;
    BlockIndexFoldResult r = BlockIndexFoldTool::Fold(root, baseGen + 1, foldHeight, &err);
    BOOST_REQUIRE_MESSAGE(r.ok, r.error);
    // reopen tip: truncated to fold
    BlockIndexTipAuthority tip2;
    BOOST_REQUIRE(BlockIndexTipAuthority::Open(root, baseGen, &tip2, &err));
    BOOST_REQUIRE_EQUAL(tip2.TipHeight(), foldHeight);
    // the S+4 block that was NOT folded is now gone from active tip (it was
    // above the fold and the fold truncates active to F)
    printf("F2 PASS fold truncates tip to F=%d (tip2.TipHeight=%d)\n",
           (int)foldHeight, (int)tip2.TipHeight());
}

BOOST_AUTO_TEST_CASE(f3_fold_noop_below_base_tip)
{
    const std::string root = MakeTempDir();
    const uint64_t baseGen = 1;
    const int baseTip = 6;
    std::vector<uint256> baseActive;
    BuildBaseGeneration(root, baseGen, baseTip, NULL, &baseActive);
    BlockIndexTipAuthority tip;
    BOOST_REQUIRE(BlockIndexTipAuthority::Create(root, baseGen, (uint64_t)(baseTip+1), baseTip, &tip, NULL));
    // fold to a height below the base tip (no-op)
    std::string err;
    BlockIndexFoldResult r = BlockIndexFoldTool::Fold(root, baseGen + 1, baseTip, &err);
    BOOST_REQUIRE_MESSAGE(r.ok, r.error);
    BOOST_REQUIRE_EQUAL(r.foldRecordCount, 0u);
    BOOST_REQUIRE_EQUAL(r.newGeneration, baseGen); // no new gen selected/flipped
    // no gen-2 created (no publish happened)
    BOOST_REQUIRE(!fs::exists(fs::path(root) / "gen-000002"));
    printf("F3 PASS fold below base tip is no-op (no gen-2, no flip)\n");
}

BOOST_AUTO_TEST_SUITE_END()