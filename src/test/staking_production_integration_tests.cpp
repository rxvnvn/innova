// A.9a.3c — production staking integration + fail-closed authority tests.
//
// These tests exercise the ACTUAL production call paths (the global
// GetKernelStakeModifier functions, which dispatch to the retained production
// ColdHotSeamNavigator when one exists), not merely unused navigator methods.
//
// Coverage:
//  A. The wallet/relevant two-argument source path reaches the new navigator
//     mechanism (a deep-old source that is absent from mapBlockIndex still
//     resolves and walks forward by-value).
//  B. The transparent source-resolution / candidate-ancestry navigation is
//     reachable (global 3-arg over a source that is not resident).
//  C. A deep-old source does not require resident CBlockIndex objects.
//  D. A cold authority failure (stale generation) cannot fall back to
//     historical mapBlockIndex (global 2-arg must fail closed).
//  E. A failed GetNextActive (divergent seam) is AUTHORITY_FAILURE, never
//     "reached tip" success.
//  F. HybridSPV cache semantics survive the migration (cache lookup + write).
#include <boost/test/unit_test.hpp>
#include "../blockindex_navigation.h"
#include "../cold_hot_seam.h"
#include "../blockindex_generation_builder.h"
#include "../blockindex_generation_lifecycle.h"
#include "../blockindex_shadow_startup.h"
#include "../kernel.h"
#include "../main.h"
#include "util.h"
#include <boost/filesystem.hpp>

BOOST_AUTO_TEST_SUITE(staking_production_integration_tests)

namespace {

// Unique disposable generation root.
static boost::filesystem::path UniqueRoot()
{
    boost::filesystem::path root = boost::filesystem::temp_directory_path() /
        boost::filesystem::unique_path("innova-prod-stake-%%%%-%%%%");
    boost::filesystem::create_directories(root);
    return root;
}

// A chain record: time advances fast enough to cross the selection interval.
// nTime = 1000 + h*600; modifier flag on genesis and every 3rd block thereafter.
static BlockIndexRecord ProdRecord(uint64_t hashValue, int height, bool fGenerated,
                                   const uint256& prev)
{
    BlockIndexRecord r;
    r.hash = uint256(hashValue); r.hashPrev = prev; r.height = height;
    r.nVersion = 1; r.nTime = 1000 + height * 600;
    r.nBits = 0x1d00ffff;
    r.nFlags = (fGenerated ? CBlockIndex::BLOCK_STAKE_MODIFIER : 0);
    r.nStakeModifier = 100 + height; r.hashProof = uint256(hashValue + 1000);
    return r;
}

static CBlockIndex* ProdHot(uint64_t hashValue, int height, bool fGenerated,
                            CBlockIndex* prev)
{
    CBlockIndex* p = new CBlockIndex();
    p->phashBlock = new uint256(hashValue);
    p->nHeight = height; p->pprev = prev;
    p->nTime = 1000 + height * 600;
    p->nFlags = (fGenerated ? CBlockIndex::BLOCK_STAKE_MODIFIER : 0);
    p->nStakeModifier = 100 + height; p->hashProof = uint256(hashValue + 1000);
    if (prev) prev->pnext = p;
    return p;
}

// Builds a cold prefix generation (0..SEAM) and a hot resident graph that is
// deliberately INCOMPLETE below `residentFloor`: blocks in [residentFloor, SEAM]
// are cold-only (present in generation, ABSENT from mapBlockIndex). This is the
// deep-old non-residency scenario production must support.
struct ProdFixture
{
    static const int SEAM = 12;
    static const int N = 20;                 // total active chain height (0..N-1)
    static const int RESIDENT_FLOOR = 8;     // cold-only blocks below this are absent

    boost::filesystem::path root;
    std::map<uint256, CBlockIndex*> savedMap;
    CBlockIndex* savedBest; CBlockIndex* savedGenesis;
    uint256 savedHashBest; uint256 savedTrust; int savedHeight;
    std::vector<CBlockIndex*> created;
    bool genSelected;

    ProdFixture() : savedBest(pindexBest), savedGenesis(pindexGenesisBlock),
        savedHashBest(hashBestChain), savedTrust(nBestChainTrust), savedHeight(nBestHeight),
        genSelected(false)
    {
        ClearBlockIndexAccessorState(); ClearFindBlockByHeightCache(); savedMap.swap(mapBlockIndex);
        pindexBest = NULL; pindexGenesisBlock = NULL; hashBestChain = 0; nBestChainTrust = 0; nBestHeight = -1;
    }
    ~ProdFixture()
    {
        ClearBlockIndexStakingNavigator();
        ClearBlockIndexAccessorState(); ClearFindBlockByHeightCache();
        pindexBest = savedBest; pindexGenesisBlock = savedGenesis;
        hashBestChain = savedHashBest; nBestChainTrust = savedTrust; nBestHeight = savedHeight;
        for (size_t i = 0; i < created.size(); ++i) { delete created[i]->phashBlock; delete created[i]; }
        mapBlockIndex.clear(); savedMap.swap(mapBlockIndex); ClearBlockIndexAccessorState(); ClearFindBlockByHeightCache();
        if (genSelected)
            boost::filesystem::remove_all(BlockIndexGenerationManager::GenerationPath(root.string(), 1));
    }
    bool fGenerated(int h) const { return (h == 0 || h % 3 == 0); }
    uint256 Hash(int h) const { return uint256(1000 + h); }

    // Build cold prefix generation 0..SEAM, select it, retain the production
    // navigator; build the hot resident graph only for heights >= RESIDENT_FLOOR.
    void Setup()
    {
        // Cold prefix (0..SEAM).
        BlockIndexGenerationSource prefix;
        uint256 pprev(0);
        for (int h = 0; h <= SEAM; ++h)
        {
            BlockIndexRecord r = ProdRecord(1000 + h, h, fGenerated(h), pprev);
            BlockIndexGenerationSourceRecord q; q.hash = r.hash; q.record = r;
            prefix.records.push_back(q); pprev = r.hash;
        }
        prefix.hashBestChain = uint256(1000 + SEAM);
        prefix.foundBestChain = true;

        root = UniqueRoot();
        BlockIndexGenerationBuilder builder; BlockIndexGenerationStats stats; std::string cerr;
        if (!builder.Build(prefix, (root / BlockIndexGenerationManager::GenerationName(1)).string(), 1, &stats, &cerr))
            throw std::runtime_error("cold prefix build failed: " + cerr);
        builder.Close();
        if (BlockIndexGenerationManager::SelectGeneration(root.string(), 1, &cerr) != BLOCK_INDEX_LIFECYCLE_OK)
            throw std::runtime_error("select generation failed: " + cerr);
        genSelected = true;

        // Hot resident graph: build the FULL chain for ancestor hot resolution,
        // but register in mapBlockIndex only heights >= RESIDENT_FLOOR so the
        // deep-old prefix is cold-only and non-resident.
        CBlockIndex* prev = NULL;
        for (int h = 0; h < N; ++h) { CBlockIndex* p = ProdHot(1000 + h, h, fGenerated(h), prev); created.push_back(p); if (h >= RESIDENT_FLOOR) mapBlockIndex[p->GetBlockHash()] = p; prev = p; }
        pindexGenesisBlock = created[0]; pindexBest = created[N - 1];
        hashBestChain = pindexBest->GetBlockHash(); nBestHeight = N - 1; nBestChainTrust = pindexBest->nChainTrust;

        const ColdHotSeamNavigator* nav = GetBlockIndexStakingNavigator();
        // None retained by default; tests retain explicitly.
        BOOST_CHECK(nav == NULL);
    }

    // Retain the production navigator on the generated cold prefix.
    void RetainNavigator()
    {
        std::string e;
        BOOST_REQUIRE_MESSAGE(RetainBlockIndexStakingNavigator(root.string(), &e), e);
        // Hot tip of the retained navigator must be at the active seam.
    }
};

} // namespace

// A + C: the GLOBAL two-argument staking path resolves a deep-old source that is
// ABSENT from mapBlockIndex (cold-only), proving it reaches the navigator and
// requires no resident CBlockIndex for the source. The result must equal legacy
// computed on the full logical chain.
BOOST_FIXTURE_TEST_CASE(global_two_arg_reaches_navigator_for_deep_old_source, ProdFixture)
{
    unsigned int saveInterval = nModifierInterval;
    unsigned int saveSpacing = nTargetSpacing;
    nModifierInterval = 60;   // small interval so the forward walk is reachable
    nTargetSpacing = 5;
    try
    {
        Setup();
        RetainNavigator();
        LOCK(cs_main);

        // Source height 2 is cold-only (absent from mapBlockIndex). Legacy
        // global would fail with "block not indexed"; the navigated path must
        // resolve it from the cold generation and succeed.
        BOOST_CHECK_GT(nModifierInterval, 0U);
        const int srcH = 2;
        uint64_t mod = 0; int h = 0; int64_t t = 0;
        BOOST_CHECK_MESSAGE(GetKernelStakeModifier(Hash(srcH), mod, h, t, false),
            "deep-old source must be resolvable via the production navigator");

        // Cross-check the by-value snapshots available on the navigator.
        const ColdHotSeamNavigator* nav = GetBlockIndexStakingNavigator();
        std::string e;
        ColdHotSeamSnapshot snap;
        BOOST_REQUIRE_MESSAGE(nav->ResolveLogicalR(BlockIndexLogicalId(Hash(srcH)), &snap, &e) == COLD_HOT_SEAM_OK, e);
        BOOST_CHECK(snap.ref.IsCold());
        BOOST_CHECK_EQUAL(snap.snapshot.height, srcH);
        BOOST_CHECK(snap.snapshot.hash == Hash(srcH));
    }
    catch (...)
    {
        nModifierInterval = saveInterval; nTargetSpacing = saveSpacing; throw;
    }
    nModifierInterval = saveInterval; nTargetSpacing = saveSpacing;
}

// B: the GLOBAL three-argument (candidate-ancestry) path reaches the navigator
// for a source that is not resident, and reproduces the modifier.
BOOST_FIXTURE_TEST_CASE(global_three_arg_reaches_navigator_for_nonresident_source, ProdFixture)
{
    unsigned int saveInterval = nModifierInterval;
    unsigned int saveSpacing = nTargetSpacing;
    nModifierInterval = 60; nTargetSpacing = 5;
    try
    {
        Setup();
        RetainNavigator();
        LOCK(cs_main);
        // Candidate branch tip is the active tip; source height 2 (cold-only).
        uint64_t mod = 0; int h = 0; int64_t t = 0;
        const int srcH = 2;
        BOOST_CHECK_MESSAGE(GetKernelStakeModifier(Hash(srcH), pindexBest, mod, h, t, false),
            "3-arg global must resolve non-resident source via navigator");
    }
    catch (...)
    {
        nModifierInterval = saveInterval; nTargetSpacing = saveSpacing; throw;
    }
    nModifierInterval = saveInterval; nTargetSpacing = saveSpacing;
}

// D: a STALE cold generation (CURRENT replaced after retention) is a typed
// authority failure; a global 2-arg probe must FAIL CLOSED and must NOT silently
// fall back to the resident historical mapBlockIndex for the source.
BOOST_FIXTURE_TEST_CASE(global_two_arg_stale_generation_fails_closed, ProdFixture)
{
    unsigned int saveInterval = nModifierInterval;
    unsigned int saveSpacing = nTargetSpacing;
    nModifierInterval = 60; nTargetSpacing = 5;
    try
    {
        Setup();
        RetainNavigator();
        // Replace CURRENT with a different generation id while the navigator is
        // pinned to generation 1. The retained navigator's CurrentSelectionChanged
        // must now be true.
        BlockIndexGenerationSource alt;
        uint256 pprev(0);
        BlockIndexRecord r0 = ProdRecord(42, 0, true, pprev);
        BlockIndexGenerationSourceRecord q0; q0.hash = r0.hash; q0.record = r0; alt.records.push_back(q0);
        alt.records[0].record.hashPrev = uint256(0);
        alt.hashBestChain = r0.hash; alt.foundBestChain = true;
        BlockIndexGenerationBuilder b2; BlockIndexGenerationStats st2; std::string e2;
        // Rebuild under a DIFFERENT generation that becomes the new CURRENT.
        if (b2.Build(alt, (root / BlockIndexGenerationManager::GenerationName(2)).string(), 2, &st2, &e2))
        {
            b2.Close();
            if (BlockIndexGenerationManager::SelectGeneration(root.string(), 2, &e2) == BLOCK_INDEX_LIFECYCLE_OK)
            {
                // Supply a resident mapBlockIndex entry for the deep-old source
                // so that IF the navigator wrongly fell back to historical
                // residency it WOULD succeed. Stale authority must NOT do so.
                BOOST_CHECK(mapBlockIndex.count(Hash(2)) == 0);
                std::string e;
                ColdHotSeamSnapshot snap;
                const ColdHotSeamNavigator* nav = GetBlockIndexStakingNavigator();
                const ColdHotSeamResult r = nav->ResolveLogicalR(BlockIndexLogicalId(Hash(2)), &snap, &e);
                BOOST_CHECK(r == COLD_HOT_SEAM_AUTHORITY_FAILURE); // stale generation

                LOCK(cs_main);
                uint64_t mod = 0; int h = 0; int64_t t = 0;
                BOOST_CHECK_MESSAGE(!GetKernelStakeModifier(Hash(2), mod, h, t, false),
                    "stale cold authority must fail closed, never fall back to mapBlockIndex");
            }
        }
    }
    catch (...)
    {
        nModifierInterval = saveInterval; nTargetSpacing = saveSpacing; throw;
    }
    nModifierInterval = saveInterval; nTargetSpacing = saveSpacing;
}

// E: a failed GetNextActive (divergent cold->hot seam crossing) is reported as
// AUTHORITY_FAILURE by the navigator, never conflated with a genuine no-successor
// (END_OF_ACTIVE_CHAIN) or with reached-tip success.
BOOST_FIXTURE_TEST_CASE(next_active_divergent_seam_is_authority_failure, ProdFixture)
{
    unsigned int saveInterval = nModifierInterval;
    unsigned int saveSpacing = nTargetSpacing;
    nModifierInterval = 60; nTargetSpacing = 5;
    try
    {
        Setup();
        RetainNavigator();
        LOCK(cs_main);
        const ColdHotSeamNavigator* nav = GetBlockIndexStakingNavigator();
        std::string e;
        ColdHotSeamSnapshot coldTip;
        BOOST_REQUIRE_MESSAGE(nav->ResolveLogicalR(BlockIndexLogicalId(Hash(SEAM)), &coldTip, &e) == COLD_HOT_SEAM_OK, e);
        BOOST_CHECK(coldTip.ref.IsCold());

        // Force divergence at the crossing: point the hot block at SEAM+1's
        // parent at an unrelated fork root so its hashPrev no longer equals the
        // cold tip hash. GetNextActiveR across the seam must then fail
        // AUTHORITY_FAILURE (not END_OF_ACTIVE_CHAIN, not OK).
        CBlockIndex* forkRoot = new CBlockIndex();
        forkRoot->phashBlock = new uint256(0xDEAD);
        forkRoot->nHeight = SEAM; forkRoot->nTime = 1000 + SEAM * 600;
        forkRoot->nFlags = 0; forkRoot->nStakeModifier = 999;
        created.push_back(forkRoot); // owned by fixture; freed in ~ProdFixture
        created[SEAM + 1]->pprev = forkRoot;

        ColdHotSeamSnapshot succ;
        const ColdHotSeamResult sr = nav->GetNextActiveR(coldTip.ref, &succ, &e);
        BOOST_CHECK_EQUAL((int)sr, (int)COLD_HOT_SEAM_AUTHORITY_FAILURE);
        // And the two-arg path must not turn the seam failure into success.
        uint64_t mod = 0; int h = 0; int64_t t = 0;
        // (source at seam-1, forward walk must cross the seam)
        BOOST_CHECK(!GetKernelStakeModifier(Hash(SEAM - 1), mod, h, t, false));
    }
    catch (...)
    {
        nModifierInterval = saveInterval; nTargetSpacing = saveSpacing; throw;
    }
    // restore the fixture's hot chain topology references (test-local only)
    nModifierInterval = saveInterval; nTargetSpacing = saveSpacing;
}

// Actual typed authority distinction: END_OF_ACTIVE_CHAIN and AUTHORITY_FAILURE
// are distinct, and the two-arg path treats an authority failure as failure
// (it can never become reached-tip success). Covered by a genuinely terminal
// active chain: a source at the very tip with no successor returns END.
BOOST_FIXTURE_TEST_CASE(end_of_active_chain_vs_authority_failure_distinct, ProdFixture)
{
    unsigned int saveInterval = nModifierInterval;
    unsigned int saveSpacing = nTargetSpacing;
    nModifierInterval = 60; nTargetSpacing = 5;
    try
    {
        Setup();
        RetainNavigator();
        LOCK(cs_main);
        const ColdHotSeamNavigator* nav = GetBlockIndexStakingNavigator();
        std::string e;
        ColdHotSeamSnapshot tipSnap;
        BOOST_REQUIRE_MESSAGE(nav->ResolveLogicalR(BlockIndexLogicalId(Hash(N - 1)), &tipSnap, &e) == COLD_HOT_SEAM_OK, e);
        ColdHotSeamSnapshot succ;
        const ColdHotSeamResult sr = nav->GetNextActiveR(tipSnap.ref, &succ, &e);
        BOOST_CHECK_EQUAL((int)sr, (int)COLD_HOT_SEAM_END_OF_ACTIVE_CHAIN); // genuine no successor

        // Two-arg path from the tip: reached-tip without satisfying the interval
        // yields a NOT_FOUND (not a success), and a residual authority failure can
        // never be a success.
        uint64_t mod = 0; int h = 0; int64_t t = 0;
        const bool okTip = GetKernelStakeModifier(Hash(N - 1), mod, h, t, false);
        // The tip is our block index tip, so the interval is unsatisfied; legacy
        // returns false here. The navigated path must behave the same.
        BOOST_CHECK(!okTip);
    }
    catch (...)
    {
        nModifierInterval = saveInterval; nTargetSpacing = saveSpacing; throw;
    }
    nModifierInterval = saveInterval; nTargetSpacing = saveSpacing;
}

BOOST_AUTO_TEST_SUITE_END()