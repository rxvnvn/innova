#include <boost/test/unit_test.hpp>
#include "../blockindex_navigation.h"
#include "../cold_hot_seam.h"
#include "../blockindex_generation_builder.h"
#include "../blockindex_generation_lifecycle.h"
#include "../kernel.h"
#include "../main.h"
#include <boost/filesystem.hpp>

BOOST_AUTO_TEST_SUITE(stable_navigation_identity_tests)

BOOST_AUTO_TEST_CASE(same_numeric_local_ids_are_not_a_navigation_identity)
{
    const BlockIndexLogicalId hotLogical(uint256(1001));
    const BlockIndexLogicalId coldLogical(uint256(2001));
    const BlockIndexNavigationRef hot = BlockIndexNavigationRef::Hot(hotLogical);
    const BlockIndexNavigationRef cold = BlockIndexNavigationRef::Cold(coldLogical, 17, 1);
    BOOST_CHECK(hot.IsValid()); BOOST_CHECK(cold.IsValid());
    BOOST_CHECK(hot.IsHot()); BOOST_CHECK(cold.IsCold());
    BOOST_CHECK(hot.logical != cold.logical);
    BOOST_CHECK(hot.recordId == BLOCK_INDEX_ID_INVALID);
    BOOST_CHECK_EQUAL(cold.recordId, 1U); BOOST_CHECK_EQUAL(cold.generation, 17U);
}

BOOST_AUTO_TEST_CASE(same_block_preserves_logical_identity_across_representations)
{
    const BlockIndexLogicalId logical(uint256(3001));
    const BlockIndexNavigationRef hot = BlockIndexNavigationRef::Hot(logical);
    const BlockIndexNavigationRef cold = BlockIndexNavigationRef::Cold(logical, 19, 1);
    BOOST_CHECK(hot.logical == cold.logical);
    BOOST_CHECK(hot != cold);
}

BOOST_AUTO_TEST_CASE(invalid_and_generation_bound_cold_refs_fail_closed)
{
    const BlockIndexLogicalId logical(uint256(4001));
    const BlockIndexNavigationRef invalid;
    const BlockIndexNavigationRef badRecord = BlockIndexNavigationRef::Cold(logical, 21, 0);
    BOOST_CHECK(!invalid.IsValid()); BOOST_CHECK(!badRecord.IsValid());
    BOOST_CHECK(!BlockIndexNavigationRef::Cold(logical, 21, 1).MatchesGeneration(22));
    BOOST_CHECK(BlockIndexNavigationRef::Cold(logical, 21, 1).MatchesGeneration(21));
}

namespace {
static boost::filesystem::path UniqueRoot()
{
    boost::filesystem::path root = boost::filesystem::temp_directory_path() /
        boost::filesystem::unique_path("innova-stable-nav-%%%%-%%%%");
    boost::filesystem::create_directories(root);
    return root;
}

static BlockIndexRecord Record(uint64_t hashValue, int height, const uint256& prev)
{
    BlockIndexRecord r;
    r.hash = uint256(hashValue); r.hashPrev = prev; r.height = height;
    r.nVersion = 1; r.nTime = 1000 + height; r.nBits = 0x1d00ffff;
    r.nFlags = CBlockIndex::BLOCK_STAKE_MODIFIER;
    r.nStakeModifier = 100 + height; r.hashProof = uint256(hashValue + 1000);
    return r;
}

static void BuildGeneration(const boost::filesystem::path& root, uint64_t generation, bool select)
{
    BlockIndexGenerationSource source;
    uint256 prev(0);
    for (int h = 0; h < 3; ++h) {
        BlockIndexRecord r = Record(100 + h, h, prev);
        BlockIndexGenerationSourceRecord q; q.hash = r.hash; q.record = r;
        source.records.push_back(q); prev = r.hash;
    }
    source.hashBestChain = prev; source.foundBestChain = true;
    BlockIndexGenerationBuilder builder; BlockIndexGenerationStats stats; std::string error;
    BOOST_REQUIRE_MESSAGE(builder.Build(source, (root / BlockIndexGenerationManager::GenerationName(generation)).string(), generation, &stats, &error), error);
    builder.Close();
    if (select)
        BOOST_REQUIRE_MESSAGE(BlockIndexGenerationManager::SelectGeneration(root.string(), generation, &error) == BLOCK_INDEX_LIFECYCLE_OK, error);
}

static CBlockIndex* MakeHot(uint64_t hashValue, int height, CBlockIndex* prev)
{
    CBlockIndex* p = new CBlockIndex();
    p->phashBlock = new uint256(hashValue); p->nHeight = height; p->pprev = prev;
    p->nTime = 1000 + height; p->nFlags = CBlockIndex::BLOCK_STAKE_MODIFIER;
    p->nStakeModifier = 100 + height; p->hashProof = uint256(hashValue + 1000);
    if (prev) prev->pnext = p;
    return p;
}

struct HotFixture
{
    std::map<uint256, CBlockIndex*> savedMap;
    CBlockIndex* savedBest; CBlockIndex* savedGenesis;
    uint256 savedHashBest; uint256 savedTrust; int savedHeight;
    std::vector<CBlockIndex*> created;

    HotFixture() : savedBest(pindexBest), savedGenesis(pindexGenesisBlock),
        savedHashBest(hashBestChain), savedTrust(nBestChainTrust), savedHeight(nBestHeight)
    {
        ClearBlockIndexAccessorState(); ClearFindBlockByHeightCache(); savedMap.swap(mapBlockIndex);
        pindexBest = NULL; pindexGenesisBlock = NULL; hashBestChain = 0; nBestChainTrust = 0; nBestHeight = -1;
    }
    ~HotFixture()
    {
        ClearFindBlockByHeightCache();
        pindexBest = savedBest; pindexGenesisBlock = savedGenesis; hashBestChain = savedHashBest; nBestChainTrust = savedTrust; nBestHeight = savedHeight;
        for (size_t i = 0; i < created.size(); ++i) { delete created[i]->phashBlock; delete created[i]; }
        mapBlockIndex.clear(); savedMap.swap(mapBlockIndex); ClearBlockIndexAccessorState(); ClearFindBlockByHeightCache();
    }
    void BuildMatchingHot()
    {
        CBlockIndex* prev = NULL;
        for (int h = 0; h < 4; ++h) { CBlockIndex* p = MakeHot(100 + h, h, prev); created.push_back(p); mapBlockIndex[p->GetBlockHash()] = p; prev = p; }
        pindexGenesisBlock = created[0]; pindexBest = created[3]; hashBestChain = pindexBest->GetBlockHash(); nBestHeight = 3; nBestChainTrust = pindexBest->nChainTrust;
    }
};
}

BOOST_FIXTURE_TEST_CASE(cold_hot_seam_rejects_wrong_domain_and_crosses_by_hash, HotFixture)
{
    boost::filesystem::path root = UniqueRoot(); BuildGeneration(root, 1, true); BuildMatchingHot();
    ColdHotSeamNavigator seam; BlockIndexV2ReaderOptions options; std::string error;
    BOOST_REQUIRE_MESSAGE(seam.Open(root.string(), options, &error), error);
    LOCK(cs_main);
    BOOST_REQUIRE_MESSAGE(seam.VerifySeam(&error), error);

    ColdHotSeamSnapshot cold, hot, next, resolved;
    BOOST_REQUIRE_MESSAGE(seam.LookupCold(BlockIndexLogicalId(uint256(102)), &cold, &error), error);
    BOOST_REQUIRE_MESSAGE(seam.LookupHot(BlockIndexLogicalId(uint256(102)), &hot, &error), error);
    BOOST_CHECK(cold.ref.IsCold()); BOOST_CHECK(hot.ref.IsHot());
    BOOST_CHECK(cold.ref.logical == hot.ref.logical);
    BOOST_CHECK(cold.ref != hot.ref);
    // V1 has no serialized memo/checksum: raw snapshot values are unavailable,
    // rather than authoritative zero. The explicit derivation below is required.
    BOOST_CHECK(!cold.snapshot.hasStakeModifierTime);
    BOOST_CHECK(!cold.snapshot.hasStakeModifierChecksum);

    // The same numeric local ID (1) cannot be accepted as an unrelated cold ref:
    // resolver requires both generation and the expected logical hash.
    BlockIndexNavigationRef forged = BlockIndexNavigationRef::Cold(BlockIndexLogicalId(uint256(999999)), seam.ColdGeneration(), 1);
    BOOST_CHECK(!seam.Resolve(forged, &resolved, &error));

    // Exact cold->hot transition occurs at the frozen generation tip.
    BOOST_REQUIRE_MESSAGE(seam.GetNextActive(cold.ref, &next, &error), error);
    BOOST_CHECK(next.ref.IsHot()); BOOST_CHECK(next.snapshot.hash == uint256(103));

    BlockIndexStakingMetadata metadata;
    BOOST_REQUIRE_MESSAGE(seam.GetStakingMetadata(cold.ref, &metadata, &error), error);
    BOOST_CHECK(metadata.hasStakeModifierTime); BOOST_CHECK(metadata.hasStakeModifierChecksum);
    BOOST_CHECK_EQUAL(metadata.nStakeModifierTime, 1002);
}

BOOST_FIXTURE_TEST_CASE(stale_cold_generation_fails_closed, HotFixture)
{
    boost::filesystem::path root = UniqueRoot(); BuildGeneration(root, 1, true); BuildMatchingHot();
    ColdHotSeamNavigator seam; BlockIndexV2ReaderOptions options; std::string error;
    BOOST_REQUIRE(seam.Open(root.string(), options, &error));
    LOCK(cs_main);
    ColdHotSeamSnapshot cold, resolved;
    BOOST_REQUIRE(seam.LookupCold(BlockIndexLogicalId(uint256(101)), &cold, &error));
    // A real CURRENT replacement invalidates every reference bound to the
    // pinned generation; resolver must refuse rather than auto-switch.
    BuildGeneration(root, 2, true);
    BOOST_CHECK(!seam.Resolve(cold.ref, &resolved, &error));
}

BOOST_FIXTURE_TEST_CASE(divergent_hot_topology_fails_closed, HotFixture)
{
    boost::filesystem::path root = UniqueRoot(); BuildGeneration(root, 1, true); BuildMatchingHot();
    ColdHotSeamNavigator seam; BlockIndexV2ReaderOptions options; std::string error;
    BOOST_REQUIRE(seam.Open(root.string(), options, &error));
    LOCK(cs_main);
    BOOST_REQUIRE(seam.VerifySeam(&error));
    // Simulate a reorg that reaches the frozen seam: height 2 no longer has
    // the generation's hash. VerifySeam must reject rather than splice.
    delete created[2]->phashBlock;
    created[2]->phashBlock = new uint256(777777);
    BOOST_CHECK(!seam.VerifySeam(&error));
}

// Restart: close and reopen the SAME pinned generation. The cold reader is
// re-resolved from CURRENT and navigation must remain authoritative.
BOOST_FIXTURE_TEST_CASE(restart_reopen_same_generation_preserves_authority, HotFixture)
{
    boost::filesystem::path root = UniqueRoot(); BuildGeneration(root, 1, true); BuildMatchingHot();
    ColdHotSeamNavigator seam; BlockIndexV2ReaderOptions options; std::string error;
    BOOST_REQUIRE(seam.Open(root.string(), options, &error));
    {
        LOCK(cs_main);
        BOOST_REQUIRE(seam.VerifySeam(&error));
        ColdHotSeamSnapshot cold;
        BOOST_REQUIRE(seam.LookupCold(BlockIndexLogicalId(uint256(102)), &cold, &error));
        BOOST_CHECK(cold.snapshot.hash == uint256(102));
    }
    seam.Close();
    BOOST_CHECK(!seam.IsOpen());
    BOOST_REQUIRE(seam.Open(root.string(), options, &error));
    {
        LOCK(cs_main);
        BOOST_REQUIRE(seam.VerifySeam(&error));
        ColdHotSeamSnapshot cold;
        BOOST_REQUIRE(seam.LookupCold(BlockIndexLogicalId(uint256(102)), &cold, &error));
        BOOST_CHECK(cold.snapshot.hash == uint256(102));
        BOOST_CHECK_EQUAL(seam.ColdGeneration(), 1U);
    }
}

// Reorg entirely ABOVE the seam: the cold frozen prefix stays intact while the
// live chain extends/differs beyond it. The seam remains valid; hot authority
// governs only the post-seam tail.
BOOST_FIXTURE_TEST_CASE(reorg_above_seam_is_hot_authoritative, HotFixture)
{
    boost::filesystem::path root = UniqueRoot(); BuildGeneration(root, 1, true); BuildMatchingHot();
    ColdHotSeamNavigator seam; BlockIndexV2ReaderOptions options; std::string error;
    BOOST_REQUIRE(seam.Open(root.string(), options, &error));
    LOCK(cs_main);
    // Cold tip is height 2 (generation 0..2). Live hot tip is height 3.
    BOOST_REQUIRE(seam.VerifySeam(&error));
    // Reorganize the live chain entirely above the cold tip: replace height 3's
    // hash (side extension). Cold prefix (0..2) is untouched, so the seam must
    // STILL verify.
    delete created[3]->phashBlock;
    created[3]->phashBlock = new uint256(888888);
    BOOST_CHECK(seam.VerifySeam(&error));
    ColdHotSeamSnapshot coldTip;
    BOOST_REQUIRE(seam.LookupCold(BlockIndexLogicalId(uint256(102)), &coldTip, &error));
    ColdHotSeamSnapshot crossing;
    // Crossing the seam resolves the (reorganized) hot successor by height.
    BOOST_REQUIRE(seam.GetNextActive(coldTip.ref, &crossing, &error));
    BOOST_CHECK(crossing.ref.IsHot());
}

// ---------------------------------------------------------------------------
// A.9a.3 by-value staking navigation. Build a >seam chain where modifier
// generation flags vary, then compare the navigator's by-value
// GetLastStakeModifier / GetKernelStakeModifier against the legacy kernel.cpp
// pointer functions on the same logical blocks.
// ---------------------------------------------------------------------------
namespace {
static CBlockIndex* MakeHotFlagged(uint64_t hashValue, int height, bool fGenerated,
                                   CBlockIndex* prev)
{
    CBlockIndex* p = new CBlockIndex();
    p->phashBlock = new uint256(hashValue);
    p->nHeight = height;
    p->pprev = prev;
    p->nTime = 1000 + height * 50;          // 50s per block -> reaches selection interval
    p->nFlags = (fGenerated ? CBlockIndex::BLOCK_STAKE_MODIFIER : 0);
    p->nStakeModifier = 100 + height;       // distinct per block for exact equality checks
    p->hashProof = uint256(hashValue + 1000);
    if (prev) prev->pnext = p;
    return p;
}

static BlockIndexRecord RecordFlagged(uint64_t hashValue, int height, bool fGenerated,
                                      const uint256& prev)
{
    BlockIndexRecord r;
    r.hash = uint256(hashValue);
    r.hashPrev = prev;
    r.height = height;
    r.nVersion = 1;
    r.nTime = 1000 + height * 50;
    r.nBits = 0x1d00ffff;
    r.nFlags = (fGenerated ? CBlockIndex::BLOCK_STAKE_MODIFIER : 0);
    r.nStakeModifier = 100 + height;
    r.hashProof = uint256(hashValue + 1000);
    return r;
}

// Builds both a hot resident graph (mapBlockIndex + pindexBest/genesis) and
// a cold prefix generation from the same logical chain, for a chain of N blocks
// where block `i` generates a modifier iff flags[i].
struct StakingChainFixture : HotFixture
{
    static const int N = 8;
    std::vector<bool> flags;
    boost::filesystem::path root;
    ColdHotSeamNavigator seam;
    bool opened;

    StakingChainFixture() : opened(false)
    {
        for (int i = 0; i < N; ++i)
            flags.push_back((i == 0 || i == 3 || i == 6)); // genesis, some mid, some top
        // hot resident chain 0..N-1
        CBlockIndex* prev = NULL;
        for (int h = 0; h < N; ++h)
        {
            CBlockIndex* p = MakeHotFlagged(100 + h, h, flags[h], prev);
            created.push_back(p);
            mapBlockIndex[p->GetBlockHash()] = p;
            prev = p;
        }
        pindexGenesisBlock = created[0];
        pindexBest = created[N - 1];
        hashBestChain = pindexBest->GetBlockHash();
        nBestHeight = N - 1;
        nBestChainTrust = pindexBest->nChainTrust;

        // cold prefix generation from heights 0..4 (seam at 4, hot tail 5..7)
        BlockIndexGenerationSource prefix;
        uint256 pprev(0);
        for (int h = 0; h < 5; ++h)
        {
            BlockIndexRecord r = RecordFlagged(100 + h, h, flags[h], pprev);
            BlockIndexGenerationSourceRecord q; q.hash = r.hash; q.record = r;
            prefix.records.push_back(q);
            pprev = r.hash;
        }
        prefix.hashBestChain = uint256(104);
        prefix.foundBestChain = true;

        root = UniqueRoot();
        BlockIndexGenerationBuilder builder;
        BlockIndexGenerationStats stats;
        std::string cerr;
        BOOST_REQUIRE_MESSAGE(builder.Build(prefix, (root / BlockIndexGenerationManager::GenerationName(1)).string(),
                                            1, &stats, &cerr), cerr);
        builder.Close();
        BOOST_REQUIRE_MESSAGE(BlockIndexGenerationManager::SelectGeneration(root.string(), 1, &cerr) == BLOCK_INDEX_LIFECYCLE_OK, cerr);

        BlockIndexV2ReaderOptions options;
        BOOST_REQUIRE_MESSAGE(seam.Open(root.string(), options, &cerr), cerr);
        opened = true;
    }
    uint256 Hash(int h) const { return uint256(100 + h); }
};
} // namespace

BOOST_AUTO_TEST_CASE(staking_navigation_by_value_matches_legacy)
{
    // Shrink the modifier interval so a forward selection-interval walk is
    // reachable within this short synthetic chain (mirrors stakemodifieropt_tests).
    unsigned int saveInterval = nModifierInterval;
    unsigned int saveSpacing = nTargetSpacing;
    nModifierInterval = 60;      // 60s interval
    nTargetSpacing = 5;          // ~12 candidate blocks per interval

    try
    {
        StakingChainFixture fx;
        LOCK(cs_main);
        std::string error;
        BOOST_REQUIRE_MESSAGE(fx.seam.VerifySeam(&error), error);

        // GetLastStakeModifier: from each block, last generated-modifier ancestor.
        for (int h = 0; h < StakingChainFixture::N; ++h)
        {
            // expected: walk back to the nearest generated flag at-or-below h
            int gen = h;
            while (gen > 0 && !fx.flags[gen]) --gen;
            BOOST_REQUIRE(fx.flags[gen]);

            uint64_t mod = 0; int64_t t = 0;
            std::string e;
            BOOST_REQUIRE_MESSAGE(fx.seam.GetLastStakeModifier(BlockIndexLogicalId(fx.Hash(h)), &mod, &t, &e), e);
            BOOST_CHECK_EQUAL(mod, (uint64_t)(100 + gen));
            BOOST_CHECK_EQUAL(t, (int64_t)(1000 + gen * 50));
        }

        // GetKernelStakeModifier (forward selection-interval walk). Compare against
        // the legacy pointer function on the SAME resident hot graph for sources
        // both cold (below seam) and hot (above seam). Sources where the walk
        // reaches the tip and the interval is unsatisfied hit the "reached best
        // block" branch in BOTH implementations; the comparison still asserts
        // identical success/failure and identical selected modifier.
        for (int h = 0; h < StakingChainFixture::N; ++h)
        {
            uint64_t legacyMod = 0; int legacyH = 0; int64_t legacyT = 0;
            uint64_t newMod = 0;   int newH = 0;     int64_t newT = 0;
            const bool legacyOk = GetKernelStakeModifier(fx.Hash(h), legacyMod, legacyH, legacyT, false);
            std::string e;
            const bool newOk = fx.seam.GetKernelStakeModifier(BlockIndexLogicalId(fx.Hash(h)), &newMod, &newH, &newT, false, &e);
            BOOST_CHECK_MESSAGE(newOk == legacyOk, "source h=" << h << " legacy=" << legacyOk << " new=" << newOk << " err=" << e);
            if (newOk && legacyOk)
            {
                BOOST_CHECK_MESSAGE(newMod == legacyMod, "mod h=" << h << " legacy=" << legacyMod << " new=" << newMod);
                BOOST_CHECK_EQUAL(newH, legacyH);
                BOOST_CHECK_EQUAL(newT, legacyT);
            }
        }

        // GetKernelStakeModifier (3-arg, backward branch-ancestry). Every source
        // is an ancestor of the candidate tip (height N-1), so the legacy 3-arg
        // must succeed; the by-value 3-arg must match it exactly.
        {
            const int tipH = StakingChainFixture::N - 1;
            for (int h = 0; h <= tipH; ++h)
            {
                uint64_t legacyMod = 0; int legacyH = 0; int64_t legacyT = 0;
                uint64_t newMod = 0;   int newH = 0;     int64_t newT = 0;
                CBlockIndex* pTip = pindexBest;
                const bool legacyOk = GetKernelStakeModifier(fx.Hash(h),
                                                            pTip /* candidate prev */,
                                                            legacyMod, legacyH, legacyT, false);
                std::string e;
                const bool newOk = fx.seam.GetKernelStakeModifier(
                    BlockIndexLogicalId(fx.Hash(h)), BlockIndexLogicalId(fx.Hash(tipH)),
                    &newMod, &newH, &newT, false, &e);
                BOOST_CHECK_MESSAGE(newOk == legacyOk, "3arg source h=" << h << " legacy=" << legacyOk << " new=" << newOk << " err=" << e);
                if (newOk && legacyOk)
                {
                    BOOST_CHECK_MESSAGE(newMod == legacyMod, "3arg mod h=" << h << " legacy=" << legacyMod << " new=" << newMod);
                    BOOST_CHECK_EQUAL(newH, legacyH);
                    BOOST_CHECK_EQUAL(newT, legacyT);
                }
            }
        }
    }
    catch (...)
    {
        nModifierInterval = saveInterval;
        nTargetSpacing = saveSpacing;
        throw;
    }
    nModifierInterval = saveInterval;
    nTargetSpacing = saveSpacing;
}

BOOST_AUTO_TEST_SUITE_END()
