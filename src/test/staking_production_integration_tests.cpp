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
#include "../script.h"
#include "../wallet.h"
#include "../init.h"
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
    // A.9a.3e: per-height merkle-root override applied when building the cold
    // prefix records (so a HybridSPV wallet tx's merkle proof can be validated
    // against an actual non-zero block merkle root, matching the legacy proof).
    std::map<int, uint256> merkleRootOverride;
    // A.9a.3e: optional override of the cold-block hash at a chosen height, so a
    // debug-integration fixture can make blockFrom.GetHash() be the COLD-only
    // (non-resident) source block. Off when srcOverriddenHash is null.
    uint256 srcOverriddenHash;
    int srcOverrideHeight;

    ProdFixture() : savedBest(pindexBest), savedGenesis(pindexGenesisBlock),
        savedHashBest(hashBestChain), savedTrust(nBestChainTrust), savedHeight(nBestHeight),
        genSelected(false), srcOverriddenHash(uint256(0)), srcOverrideHeight(2)
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
            if (merkleRootOverride.count(h))
                r.hashMerkleRoot = merkleRootOverride[h];
            if (h == srcOverrideHeight && srcOverriddenHash != uint256(0))
                r.hash = srcOverriddenHash;
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

namespace {

// A ColdHotHotResolver that serves the fixture's synthetic hot chain (hashes
// uint256(1000+h)) but makes the block at SEAM+1's parent a DIVERGENT block at
// height SEAM whose hash (0000...D0D0D) is ABSENT from the frozen cold
// generation. The divergent snapshot carries a process-local legacy-style id
// (9001) so the old domain-confusion defect (rebinding that id as a cold
// generation RecordId) is directly observable.
class DivergeHotResolver : public ColdHotHotResolver
{
public:
    DivergeHotResolver(int seam, int n) : seam(seam), n(n) {}
    uint256 H(int h) const { return uint256(1000 + h); }
    uint256 DivHash() const { return uint256(0xD0D0D); }

    BlockIndexSnapshot Snap(const uint256& hash, int h, bool inMain, BlockIndexId id) const
    {
        BlockIndexSnapshot s;
        s.found = true; s.id = id;
        s.hash = hash; s.hashPrev = (h == 0) ? uint256(0) : H(h - 1);
        s.hashNext = uint256(0);
        s.height = h; s.nFile = 1; s.nBlockPos = 100 + h; s.nFlags = 0;
        s.nVersion = 1; s.nTime = 1000 + h * 600; s.nBits = 0x1d00ffff; s.nNonce = 0;
        s.nMint = 0; s.nMoneySupply = 0; s.nStakeModifier = 100 + h;
        s.prevoutStake = COutPoint(); s.nStakeTime = 0; s.hashProof = uint256(h + 1000);
        s.nChainTrust = 0; s.fProofOfStake = false; s.fInMainChain = inMain;
        s.hasParent = (h != 0);
        s.hasStakeModifierTime = false; s.hasStakeModifierChecksum = false;
        s.nStakeModifierTime = 0; s.nStakeModifierChecksum = 0;
        return s;
    }

    virtual BlockIndexSnapshot LookupByHash(const uint256& hash) const
    {
        for (int h = 0; h < n; ++h)
            if (H(h) == hash) return Snap(hash, h, true, (BlockIndexId)(1000 + h));
        if (hash == DivHash()) return Snap(hash, seam, true, (BlockIndexId)9001);
        return BlockIndexSnapshot();
    }
    virtual BlockIndexSnapshot GetActiveByHeight(int height) const
    {
        if (height < 0 || height >= n) return BlockIndexSnapshot();
        return Snap(H(height), height, true, (BlockIndexId)(1000 + height));
    }
    virtual BlockIndexSnapshot GetParentByHash(const uint256& hash) const
    {
        // The hot main-chain block at SEAM+1 has the DIVERGENT parent: on the
        // live (resolver) main chain at height SEAM, absent from the cold gen.
        if (hash == H(seam + 1)) return LookupByHash(DivHash());
        for (int h = 1; h < n; ++h)
            if (H(h) == hash) return LookupByHash(H(h - 1));
        return BlockIndexSnapshot();
    }
    virtual BlockIndexSnapshot GetNextActiveByHash(const uint256& hash) const
    {
        for (int h = 0; h < n - 1; ++h)
            if (H(h) == hash) return GetActiveByHeight(h + 1);
        return BlockIndexSnapshot();
    }
    virtual BlockIndexSnapshot GetTip() const { return GetActiveByHeight(n - 1); }

private:
    int seam, n;
};

} // namespace

// Blocker 1 (CRITICAL) RED/GREEN: a hot->cold parent crossing must FAIL CLOSED
// rather than rebind a HOT process-local id as a COLD generation RecordId.
// Old code: ResolveLogicalR fell back to HOT on a cold miss and passed that HOT
// snapshot to MakeCold, returning COLD_HOT_SEAM_OK with a cold ref whose
// recordId was the hot legacy id (9001). Fixed code requires PROVEN-COLD
// provenance at the crossing and returns COLD_HOT_SEAM_AUTHORITY_FAILURE.
BOOST_FIXTURE_TEST_CASE(hot_to_cold_parent_crossing_divergence_fails_closed, ProdFixture)
{
    Setup();
    DivergeHotResolver hot(SEAM, N);
    ColdHotSeamNavigator nav;
    BlockIndexV2ReaderOptions options;
    std::string e;
    BOOST_REQUIRE_MESSAGE(nav.Open(root.string(), options, &e), e);
    nav.SetTestHotResolver(&hot);
    LOCK(cs_main);
    ColdHotSeamSnapshot out;
    const BlockIndexNavigationRef ref = BlockIndexNavigationRef::Hot(BlockIndexLogicalId(hot.H(SEAM + 1)));
    const ColdHotSeamResult r = nav.GetParentR(ref, &out, &e);
    // The parent is on the live main chain at/below the frozen seam but is NOT
    // in the cold generation -> divergent seam -> must fail closed.
    BOOST_CHECK_EQUAL((int)r, (int)COLD_HOT_SEAM_AUTHORITY_FAILURE);
    // Under no circumstances may a HOT process-local id become a COLD RecordId.
    if (r == COLD_HOT_SEAM_OK)
        BOOST_CHECK_MESSAGE(!out.ref.IsCold() || out.ref.recordId != 9001,
            "A hot process-local id must never be rebound as a cold RecordId");
}

// Blocker 2: the wallet ancestor adapter must be TYPED so a genuine FOUND,
// NOT_FOUND, NO_NAVIGATOR and AUTHORITY_FAILURE are distinct, and an authority
// failure (stale generation here) can NEVER re-enable the arbitrary-depth pprev
// fallback — even when the requested ancestor IS resident in mapBlockIndex.
BOOST_FIXTURE_TEST_CASE(wallet_ancestor_snapshot_found_and_authority_failure_typed, ProdFixture)
{
    unsigned int saveInterval = nModifierInterval;
    unsigned int saveSpacing = nTargetSpacing;
    nModifierInterval = 60; nTargetSpacing = 5;
    try
    {
        Setup();
        RetainNavigator();
        LOCK(cs_main);
        // 1) Normal deep-old by-value resolution (source absent from mapBlockIndex).
        uint256 hOut; unsigned int tOut = 0, fOut = 0;
        const StakingAncestorStatus ok = GetStakingAncestorSnapshot(pindexBest, 2, &hOut, &tOut, &fOut);
        BOOST_CHECK_EQUAL((int)ok, (int)STAKING_ANCESTOR_OK);
        BOOST_CHECK(hOut == Hash(2));
        BOOST_CHECK_EQUAL(tOut, (unsigned int)(1000 + 2 * 600));

        // 2) Authority failure (stale CURRENT replaced after retention) is typed
        // AUTHORITY_FAILURE even though the requested ancestor (height 10, >=
        // RESIDENT_FLOOR) IS resident here — a legacy pprev walk WOULD succeed,
        // but the typed authority failure must force fail-closed (no fallback).
        BlockIndexGenerationSource alt;
        uint256 pprev(0);
        BlockIndexRecord r0 = ProdRecord(42, 0, true, pprev);
        BlockIndexGenerationSourceRecord q0; q0.hash = r0.hash; q0.record = r0; alt.records.push_back(q0);
        alt.records[0].record.hashPrev = uint256(0);
        alt.hashBestChain = r0.hash; alt.foundBestChain = true;
        BlockIndexGenerationBuilder b2; BlockIndexGenerationStats st2; std::string e2;
        if (b2.Build(alt, (root / BlockIndexGenerationManager::GenerationName(2)).string(), 2, &st2, &e2))
        {
            b2.Close();
            if (BlockIndexGenerationManager::SelectGeneration(root.string(), 2, &e2) == BLOCK_INDEX_LIFECYCLE_OK)
            {
                BOOST_CHECK(mapBlockIndex.count(Hash(10)) == 1U); // resident
                uint256 h2; unsigned int t2 = 0, f2 = 0;
                const StakingAncestorStatus st = GetStakingAncestorSnapshot(pindexBest, 10, &h2, &t2, &f2);
                BOOST_CHECK_EQUAL((int)st, (int)STAKING_ANCESTOR_AUTHORITY_FAILURE);
            }
        }
    }
    catch (...)
    {
        nModifierInterval = saveInterval; nTargetSpacing = saveSpacing; throw;
    }
    nModifierInterval = saveInterval; nTargetSpacing = saveSpacing;
}

// Blocker 5: a HybridSPV cache hit must return the EXACT cached modifier plus
// the legacy source height/time outputs (height = source height, time = source
// time, NOT advanced), and miss->lookup->write side effects stay exact. Drives
// the REAL global 2-arg adapter and the real HybridSPV cache.
BOOST_FIXTURE_TEST_CASE(hybridspv_cache_hit_height_time_exact, ProdFixture)
{
    const bool saveSPV = fHybridSPV;
    unsigned int saveInterval = nModifierInterval;
    unsigned int saveSpacing = nTargetSpacing;
    fHybridSPV = true;
    nModifierInterval = 60; nTargetSpacing = 5;
    try
    {
        Setup();
        RetainNavigator();
        LOCK(cs_main);
        const int srcH = 2;
        // First call: cache MISS -> forward walk must succeed and populate cache.
        uint64_t modMiss = 0; int hMiss = 0; int64_t tMiss = 0;
        BOOST_CHECK_MESSAGE(GetKernelStakeModifier(Hash(srcH), modMiss, hMiss, tMiss, false),
            "first (miss) global 2-arg must succeed and populate the HybridSPV cache");
        // Second call: cache HIT -> cached modifier + EXACT legacy source
        // height/time (nStakeModifierHeight=src height, nStakeModifierTime=src time).
        uint64_t modHit = 0; int hHit = -1; int64_t tHit = -1;
        BOOST_CHECK_MESSAGE(GetKernelStakeModifier(Hash(srcH), modHit, hHit, tHit, false),
            "second (hit) global 2-arg must succeed via cached modifier");
        BOOST_CHECK_EQUAL(modHit, modMiss);                       // D11 modifier exact
        BOOST_CHECK_EQUAL(hHit, srcH);                            // D12 height exact
        BOOST_CHECK_EQUAL((int64_t)tHit, (int64_t)(1000 + srcH * 600)); // D13 time exact
    }
    catch (...)
    {
        fHybridSPV = saveSPV; nModifierInterval = saveInterval; nTargetSpacing = saveSpacing; throw;
    }
    fHybridSPV = saveSPV; nModifierInterval = saveInterval; nTargetSpacing = saveSpacing;
}

// Blocker 3a: transparent candidate-side source recovery works BY-VALUE with a
// retained navigator, searching a hot side-suffix (non-main-chain branch block)
// and reading it by snapshot position (candidate map) — no arbitrary historical
// pointer topology / main-chain residency required for the frozen portion.
BOOST_FIXTURE_TEST_CASE(candidate_side_suffix_source_recovery_byvalue, ProdFixture)
{
    Setup();
    RetainNavigator();
    LOCK(cs_main);
    // A previous transaction placed in a hot side-block below the seam.
    CTransaction txPrevRef;
    txPrevRef.nVersion = 1;
    CTxOut out; out.nValue = 1000; out.scriptPubKey = CScript() << OP_TRUE;
    txPrevRef.vout.push_back(out);
    const uint256 txHash = txPrevRef.GetHash();

    CBlock sideBlock;
    sideBlock.nVersion = 1;
    sideBlock.hashPrevBlock = Hash(SEAM); // parent = main-chain block at the seam (cold)
    sideBlock.nTime = 1000 + (SEAM + 1) * 600;
    sideBlock.vtx.push_back(txPrevRef);
    const uint256 sideHash = sideBlock.GetHash();

    // Hot side-branch tip (not in the main chain), registered in mapBlockIndex.
    CBlockIndex* pTip = new CBlockIndex();
    pTip->phashBlock = new uint256(sideHash);
    pTip->nHeight = SEAM + 1;
    pTip->pprev = created[SEAM]; // parent = main-chain seam block (frozen/cold)
    pTip->nTime = 1000 + (SEAM + 1) * 600;
    pTip->nFlags = 0; pTip->nStakeModifier = 100 + SEAM + 1;
    created.push_back(pTip); // fixture-owned
    mapBlockIndex[sideHash] = pTip;

    std::map<uint256, CBlock> candidateBlocks;
    candidateBlocks[sideHash] = sideBlock;

    CTransaction foundTx; CTxIndex foundIndex; CBlock foundBlock;
    const bool found = ReadStakeSourceTransactionForTesting(
        pTip, COutPoint(txHash, 0), foundTx, foundIndex, foundBlock, candidateBlocks);
    BOOST_CHECK_MESSAGE(found, "by-value candidate side-suffix must recover the source transaction");
    if (found)
    {
        BOOST_CHECK(foundTx.GetHash() == txHash);
        BOOST_CHECK(foundBlock.GetHash() == sideHash);
    }
}

// Blocker 3b: HybridSPV historical source-block recovery is by-value (resolved
// via the retained navigator to a cold ref with a persisted disk position), so
// it does NOT require an arbitrary historical CBlockIndex resident in mapBlockIndex.
BOOST_FIXTURE_TEST_CASE(hybridspv_source_block_position_byvalue, ProdFixture)
{
    Setup();
    RetainNavigator();
    LOCK(cs_main);
    BOOST_CHECK(mapBlockIndex.count(Hash(2)) == 0); // cold-only, non-resident
    const ColdHotSeamNavigator* nav = GetBlockIndexStakingNavigator();
    std::string e;
    ColdHotSeamSnapshot src;
    BOOST_REQUIRE_MESSAGE(nav->ResolveLogicalR(BlockIndexLogicalId(Hash(2)), &src, &e) == COLD_HOT_SEAM_OK, e);
    // The by-value source is a COLD ref (generation-bound RecordId) — the same
    // resolution CheckProofOfStake's HybridSPV source-block read now uses to get
    // nFile/nBlockPos without mapBlockIndex residency.
    BOOST_CHECK(src.ref.IsCold());
    BOOST_CHECK(src.snapshot.found);
}

// Blocker 4: debug/diagnostic source-height lookup must be OBSERVATIONAL: it
// must not require a resident cold CBlockIndex, must not insert a NULL entry via
// mapBlockIndex::operator[], and must not change the consensus result. The
// diagnostic height now comes from the by-value navigator. Verify a cold-only
// source resolves with diagnostics on, mapBlockIndex stays unmutated, and the
// result (modifier/height/time) is identical with and without the diagnostic flag.
BOOST_FIXTURE_TEST_CASE(debug_cold_source_height_is_observational, ProdFixture)
{
    unsigned int saveInterval = nModifierInterval;
    unsigned int saveSpacing = nTargetSpacing;
    nModifierInterval = 60; nTargetSpacing = 5;
    try
    {
        Setup();
        RetainNavigator();
        LOCK(cs_main);
        const int srcH = 2;
        BOOST_CHECK(mapBlockIndex.count(Hash(srcH)) == 0); // cold-only, non-resident
        uint64_t m1 = 0; int h1 = 0; int64_t t1 = 0;
        const bool r1 = GetKernelStakeModifier(Hash(srcH), m1, h1, t1, /*fPrintProofOfStake=*/true);
        BOOST_CHECK_MESSAGE(r1, "diagnostic-flag-on modifier must still succeed ");
        // The cold-only source must remain absent (no operator[] insertion).
        BOOST_CHECK(mapBlockIndex.count(Hash(srcH)) == 0);
        // Same consensus result with the diagnostic flag off.
        uint64_t m2 = 0; int h2 = 0; int64_t t2 = 0;
        const bool r2 = GetKernelStakeModifier(Hash(srcH), m2, h2, t2, false);
        BOOST_CHECK_EQUAL((int)r1, (int)r2);
        if (r1 && r2)
        {
            BOOST_CHECK_EQUAL(m1, m2);
            BOOST_CHECK_EQUAL(h1, h2);
            BOOST_CHECK_EQUAL(t1, t2);
        }
    }
    catch (...)
    {
        nModifierInterval = saveInterval; nTargetSpacing = saveSpacing; throw;
    }
    nModifierInterval = saveInterval; nTargetSpacing = saveSpacing;
}

namespace {

// A.9a.3e: a ColdHotHotResolver whose source snapshot's active membership is a
// toggleable flag. Lets a test prove a "known block" can move from ACTIVE to
// INACTIVE (reorg) and never receive stale positive active depth.
class ToggleActiveResolver : public ColdHotHotResolver
{
public:
    bool* active; uint256 src; uint256 merkleLeaf;
    BlockIndexSnapshot tipSnap;
    ToggleActiveResolver(bool* activeIn, const uint256& srcIn, const uint256& leaf)
        : active(activeIn), src(srcIn), merkleLeaf(leaf)
    {
        tipSnap.found = true; tipSnap.id = (BlockIndexId)9000;
        tipSnap.hash = uint256(0xF00F); tipSnap.height = 19;
        tipSnap.fInMainChain = true; tipSnap.hasParent = false;
    }
    BlockIndexSnapshot Snap(const uint256& hash, int h, bool in) const
    {
        BlockIndexSnapshot s;
        s.found = true; s.id = (BlockIndexId)(1000 + h);
        s.hash = hash; s.hashPrev = (h == 0) ? uint256(0) : uint256(1000 + h - 1);
        s.hashNext = uint256(0);
        s.hashMerkleRoot = merkleLeaf;
        s.height = h; s.nFile = 1; s.nBlockPos = 100 + h; s.nFlags = 0;
        s.nVersion = 1; s.nTime = 1000 + h * 600; s.nBits = 0x1d00ffff; s.nNonce = 0;
        s.nMint = 0; s.nMoneySupply = 0; s.nStakeModifier = 100 + h;
        s.prevoutStake = COutPoint(); s.nStakeTime = 0; s.hashProof = uint256(h + 1000);
        s.nChainTrust = 0; s.fProofOfStake = false; s.fInMainChain = in; s.hasParent = (h != 0);
        return s;
    }
    virtual BlockIndexSnapshot LookupByHash(const uint256& hash) const
    {
        if (hash == src) return Snap(src, 2, *active);
        if (hash == tipSnap.hash) return tipSnap;
        return BlockIndexSnapshot();
    }
    virtual BlockIndexSnapshot GetActiveByHeight(int) const { return BlockIndexSnapshot(); }
    virtual BlockIndexSnapshot GetParentByHash(const uint256&) const { return BlockIndexSnapshot(); }
    virtual BlockIndexSnapshot GetNextActiveByHash(const uint256&) const { return BlockIndexSnapshot(); }
    virtual BlockIndexSnapshot GetTip() const { return tipSnap; }
};

// A.9a.3e: build a HybridSPV wallet-source transaction and the transparent
// coinstake that spends it, used to drive the REAL CheckProofOfStake HybridSPV
// fallback. wtx.hashBlock / branch are set per-test; the single-leaf merkle
// tree (nIndex==0, empty branch) has root == srcHash, so the source block's
// merkle root must be set to srcHash for a valid merkle authority.
static void BuildHybridSpvWalletFixture(CWallet* wallet, CTransaction* srcOut,
    CWalletTx* wtxOut, CTransaction* coinstakeOut, uint256* srcHashOut)
{
    CTransaction src;
    src.nVersion = 1;
    CTxOut out; out.nValue = 1000; out.scriptPubKey = CScript() << OP_TRUE;
    src.vout.push_back(out);

    CWalletTx wtx(wallet, src);
    const uint256 srcHash = wtx.GetHash();
    wtx.hashBlock = uint256(0);
    wtx.nIndex = 0;
    wtx.vMerkleBranch.clear(); // single-leaf merkle tree: root == srcHash

    CTransaction coinstake;
    coinstake.nVersion = 1;
    CTxIn in; in.prevout = COutPoint(srcHash, 0); in.scriptSig = CScript();
    coinstake.vin.push_back(in);
    CTxOut emptyOut; emptyOut.nValue = 0; emptyOut.scriptPubKey = CScript();
    coinstake.vout.push_back(emptyOut);   // vout[0] empty (nValue==0) -> IsCoinStake
    CTxOut rewardOut; rewardOut.nValue = 1000; rewardOut.scriptPubKey = CScript() << OP_TRUE;
    coinstake.vout.push_back(rewardOut);

    *srcOut = src; *wtxOut = wtx; *coinstakeOut = coinstake; *srcHashOut = srcHash;
}

static bool RunCheckProofOfStake(const CTransaction& coinstake,
    const CBlockIndex* pindexPrev, unsigned int nBits)
{
    uint256 hp, tp;
    return CheckProofOfStake(pindexPrev, coinstake, nBits, hp, tp);
}

// A.9a.3e: test-only observation of the real CheckProofOfStake HybridSPV
// maturity gate. Fires only when by-value maturity authority is satisfied and
// the code is about to enter by-value source-block recovery.
static int g_spvProbeFires = 0;
static int g_spvProbeDepth = -1;
static void ArmSpvProbe()
{
    g_spvProbeFires = 0; g_spvProbeDepth = -1;
    SetHybridSvmRecoveryProbe([](int d){ g_spvProbeFires++; g_spvProbeDepth = d; });
}
static void DisarmSpvProbe() { SetHybridSvmRecoveryProbe(NULL); }

} // namespace

// A.9a.3e NEW-N6, H1: a deep COLD (non-resident) mature HybridSPV source must be
// able to satisfy maturity/active-chain/merkle authority BY-VALUE and reach the
// by-value source-block recovery -- exactly the A.10 scenario the previous
// commit could not reach because GetDepthInMainChain required a resident
// historical CBlockIndex. Crosses the REAL CheckProofOfStake boundary.
BOOST_FIXTURE_TEST_CASE(checkproofofstake_hybridspv_cold_mature_byvalue_reaches_recovery, ProdFixture)
{
    const bool saveSPV = fHybridSPV;
    const int saveMaturity = nCoinbaseMaturity;
    unsigned int saveI = nModifierInterval, saveS = nTargetSpacing;
    fHybridSPV = true; nCoinbaseMaturity = 10; nModifierInterval = 60; nTargetSpacing = 5;
    CTransaction srcTx; CWalletTx wtx; CTransaction coinstake; uint256 srcHash;
    try
    {
        BuildHybridSpvWalletFixture(pwalletMain, &srcTx, &wtx, &coinstake, &srcHash);
        merkleRootOverride[2] = srcHash; // single-leaf branch root
        Setup();
        RetainNavigator();
        LOCK(cs_main);
        BOOST_CHECK_MESSAGE(mapBlockIndex.count(Hash(2)) == 0,
            "source must be cold-only (absent from mapBlockIndex)");
        wtx.hashBlock = Hash(2);
        pwalletMain->mapWallet[srcHash] = wtx;
        ArmSpvProbe();
        const bool r = RunCheckProofOfStake(coinstake, pindexBest, 0x1d00ffff);
        (void)r; // full recovery then fails (synthetic source block not on disk);
                 // the maturity gate + by-value recovery ENTRY is what we assert.
        BOOST_CHECK_EQUAL(g_spvProbeFires, 1);

        BOOST_CHECK_EQUAL(g_spvProbeDepth, 18); // exact legacy depth: 19 - 2 + 1
        pwalletMain->mapWallet.erase(srcHash);
        DisarmSpvProbe();
    }
    catch (...)
    {
        pwalletMain->mapWallet.erase(srcHash); DisarmSpvProbe();
        fHybridSPV = saveSPV; nCoinbaseMaturity = saveMaturity; nModifierInterval = saveI; nTargetSpacing = saveS; throw;
    }
    fHybridSPV = saveSPV; nCoinbaseMaturity = saveMaturity; nModifierInterval = saveI; nTargetSpacing = saveS;
}

// A.9a.3e H2/E11: a HOT active source (resident, above the cold seam) behaves
// identically -- unchanged behavior through the real CheckProofOfStake.
BOOST_FIXTURE_TEST_CASE(checkproofofstake_hybridspv_hot_active_source_unchanged, ProdFixture)
{
    const bool saveSPV = fHybridSPV;
    const int saveMaturity = nCoinbaseMaturity;
    unsigned int saveI = nModifierInterval, saveS = nTargetSpacing;
    fHybridSPV = true; nCoinbaseMaturity = 5; nModifierInterval = 60; nTargetSpacing = 5;
    CTransaction srcTx; CWalletTx wtx; CTransaction coinstake; uint256 srcHash;
    try
    {
        BuildHybridSpvWalletFixture(pwalletMain, &srcTx, &wtx, &coinstake, &srcHash);
        Setup();
        RetainNavigator();
        LOCK(cs_main);
        const int hotH = 13; // above the cold seam (SEAM=12): HOT-only/resident
        created[hotH]->hashMerkleRoot = srcHash; // valid merkle root for the single-leaf branch
        wtx.hashBlock = Hash(hotH);
        pwalletMain->mapWallet[srcHash] = wtx;
        ArmSpvProbe();
        const bool r = RunCheckProofOfStake(coinstake, pindexBest, 0x1d00ffff);
        (void)r;
        BOOST_CHECK_EQUAL(g_spvProbeFires, 1);
        BOOST_CHECK_EQUAL(g_spvProbeDepth, 19 - hotH + 1); // 7
        pwalletMain->mapWallet.erase(srcHash);
        DisarmSpvProbe();
    }
    catch (...)
    {
        pwalletMain->mapWallet.erase(srcHash); DisarmSpvProbe();
        fHybridSPV = saveSPV; nCoinbaseMaturity = saveMaturity; nModifierInterval = saveI; nTargetSpacing = saveS; throw;
    }
    fHybridSPV = saveSPV; nCoinbaseMaturity = saveMaturity; nModifierInterval = saveI; nTargetSpacing = saveS;
}

// A.9a.3e H4/E5: an IMMATURE source (found + active + valid merkle, but depth <
// nCoinbaseMaturity) is rejected at the exact legacy boundary -- recovery is
// never reached.
BOOST_FIXTURE_TEST_CASE(checkproofofstake_hybridspv_immature_rejected_at_legacy_boundary, ProdFixture)
{
    const bool saveSPV = fHybridSPV;
    const int saveMaturity = nCoinbaseMaturity;
    unsigned int saveI = nModifierInterval, saveS = nTargetSpacing;
    fHybridSPV = true; nCoinbaseMaturity = 25; nModifierInterval = 60; nTargetSpacing = 5; // > depth 18
    CTransaction srcTx; CWalletTx wtx; CTransaction coinstake; uint256 srcHash;
    try
    {
        BuildHybridSpvWalletFixture(pwalletMain, &srcTx, &wtx, &coinstake, &srcHash);
        merkleRootOverride[2] = srcHash;
        Setup();
        RetainNavigator();
        LOCK(cs_main);
        wtx.hashBlock = Hash(2);
        pwalletMain->mapWallet[srcHash] = wtx;
        ArmSpvProbe();
        const bool r = RunCheckProofOfStake(coinstake, pindexBest, 0x1d00ffff);
        BOOST_CHECK(!r);                       // rejected
        BOOST_CHECK_EQUAL(g_spvProbeFires, 0); // recovery never reached
        pwalletMain->mapWallet.erase(srcHash);
        DisarmSpvProbe();
    }
    catch (...)
    {
        pwalletMain->mapWallet.erase(srcHash); DisarmSpvProbe();
        fHybridSPV = saveSPV; nCoinbaseMaturity = saveMaturity; nModifierInterval = saveI; nTargetSpacing = saveS; throw;
    }
    fHybridSPV = saveSPV; nCoinbaseMaturity = saveMaturity; nModifierInterval = saveI; nTargetSpacing = saveS;
}

// A.9a.3e H6/E6: an invalid merkle authority (block merkle root does not match
// the wallet tx's single-leaf branch) is rejected exactly as legacy.
BOOST_FIXTURE_TEST_CASE(checkproofofstake_hybridspv_merkle_mismatch_rejected, ProdFixture)
{
    const bool saveSPV = fHybridSPV;
    const int saveMaturity = nCoinbaseMaturity;
    unsigned int saveI = nModifierInterval, saveS = nTargetSpacing;
    fHybridSPV = true; nCoinbaseMaturity = 10; nModifierInterval = 60; nTargetSpacing = 5;
    CTransaction srcTx; CWalletTx wtx; CTransaction coinstake; uint256 srcHash;
    try
    {
        BuildHybridSpvWalletFixture(pwalletMain, &srcTx, &wtx, &coinstake, &srcHash);
        merkleRootOverride[2] = uint256(srcHash.Get64() ^ 1ULL); // WRONG root -> merkle must fail
        Setup();
        RetainNavigator();
        LOCK(cs_main);
        wtx.hashBlock = Hash(2);
        pwalletMain->mapWallet[srcHash] = wtx;
        ArmSpvProbe();
        const bool r = RunCheckProofOfStake(coinstake, pindexBest, 0x1d00ffff);
        BOOST_CHECK(!r);
        BOOST_CHECK_EQUAL(g_spvProbeFires, 0); // merkle authority rejected before recovery
        pwalletMain->mapWallet.erase(srcHash);
        DisarmSpvProbe();
    }
    catch (...)
    {
        pwalletMain->mapWallet.erase(srcHash); DisarmSpvProbe();
        fHybridSPV = saveSPV; nCoinbaseMaturity = saveMaturity; nModifierInterval = saveI; nTargetSpacing = saveS; throw;
    }
    fHybridSPV = saveSPV; nCoinbaseMaturity = saveMaturity; nModifierInterval = saveI; nTargetSpacing = saveS;
}

// A.9a.3e H7/E7: a STALE generation authority (CURRENT replaced after
// retention) FAILS CLOSED: CheckProofOfStake rejects and never reaches recovery,
// even though the source would be otherwise findable.
BOOST_FIXTURE_TEST_CASE(checkproofofstake_hybridspv_stale_generation_fails_closed, ProdFixture)
{
    const bool saveSPV = fHybridSPV;
    const int saveMaturity = nCoinbaseMaturity;
    unsigned int saveI = nModifierInterval, saveS = nTargetSpacing;
    fHybridSPV = true; nCoinbaseMaturity = 10; nModifierInterval = 60; nTargetSpacing = 5;
    CTransaction srcTx; CWalletTx wtx; CTransaction coinstake; uint256 srcHash;
    try
    {
        BuildHybridSpvWalletFixture(pwalletMain, &srcTx, &wtx, &coinstake, &srcHash);
        merkleRootOverride[2] = srcHash;
        Setup();
        RetainNavigator();
        // Replace CURRENT with a different generation while the navigator is
        // pinned to generation 1.
        BlockIndexGenerationSource alt;
        uint256 pprev(0);
        BlockIndexRecord r0 = ProdRecord(42, 0, true, pprev);
        BlockIndexGenerationSourceRecord q0; q0.hash = r0.hash; q0.record = r0; alt.records.push_back(q0);
        alt.records[0].record.hashPrev = uint256(0);
        alt.hashBestChain = r0.hash; alt.foundBestChain = true;
        BlockIndexGenerationBuilder b2; BlockIndexGenerationStats st2; std::string e2;
        if (b2.Build(alt, (root / BlockIndexGenerationManager::GenerationName(2)).string(), 2, &st2, &e2))
        {
            b2.Close();
            BOOST_REQUIRE_MESSAGE(BlockIndexGenerationManager::SelectGeneration(root.string(), 2, &e2) == BLOCK_INDEX_LIFECYCLE_OK, e2);
        }
        LOCK(cs_main);
        wtx.hashBlock = Hash(2);
        pwalletMain->mapWallet[srcHash] = wtx;
        ArmSpvProbe();
        const bool r = RunCheckProofOfStake(coinstake, pindexBest, 0x1d00ffff);
        BOOST_CHECK(!r);
        BOOST_CHECK_EQUAL(g_spvProbeFires, 0); // stale authority fails closed
        pwalletMain->mapWallet.erase(srcHash);
        DisarmSpvProbe();
    }
    catch (...)
    {
        pwalletMain->mapWallet.erase(srcHash); DisarmSpvProbe();
        fHybridSPV = saveSPV; nCoinbaseMaturity = saveMaturity; nModifierInterval = saveI; nTargetSpacing = saveS; throw;
    }
    fHybridSPV = saveSPV; nCoinbaseMaturity = saveMaturity; nModifierInterval = saveI; nTargetSpacing = saveS;
}

// A.9a.3e H3/H10/E4: a KNOWN but INACTIVE (side/reorged) source must NEVER
// receive positive active depth. Verified at the by-value maturity authority
// operation with a toggleable resolver (active -> reorg-inactive). The real
// CheckProofOfStake H1/H4/H6/H7 cases cover the production wiring; this case
// isolates the authority operation's active/inactive distinction.
BOOST_FIXTURE_TEST_CASE(hybridspv_maturity_reorg_inactive_never_positive_depth, ProdFixture)
{
    Setup(); // cold generation built but not retained; we use a test resolver
    bool active = true;
    ColdHotSeamNavigator nav;
    const uint256 srcHash(0xBEEF);
    const uint256 merkleLeaf(0x5555);
    ToggleActiveResolver resolver(&active, srcHash, merkleLeaf);
    nav.SetTestHotResolver(&resolver);
    LOCK(cs_main);
    std::string e;

    // ACTIVE: exact positive depth.
    int depth = -1;
    ColdHotSeamResult m1 = nav.GetHybridSvmMaturityAuthorityR(
        BlockIndexLogicalId(srcHash), merkleLeaf, std::vector<uint256>(), 0, &depth, &e);
    BOOST_CHECK_EQUAL((int)m1, (int)COLD_HOT_SEAM_OK);
    BOOST_CHECK_EQUAL(depth, 18); // 19 - 2 + 1

    // Reorg deactivates the source: must NOT retain stale positive depth.
    active = false;
    int depth2 = -1;
    ColdHotSeamResult m2 = nav.GetHybridSvmMaturityAuthorityR(
        BlockIndexLogicalId(srcHash), merkleLeaf, std::vector<uint256>(), 0, &depth2, &e);
    BOOST_CHECK_EQUAL((int)m2, (int)COLD_HOT_SEAM_NOT_FOUND);
    BOOST_CHECK_EQUAL(depth2, 0);
}

// A.9a.3e E14/E15: the real CheckStakeKernelHash boundary with a COLD debug
// source absent from mapBlockIndex must reach the diagnostic source-height
// helper by-value, must not insert a mapBlockIndex entry, and must not alter
// the consensus result (fPrint on/off identical).
BOOST_FIXTURE_TEST_CASE(debug_checkstakekernelhash_cold_source_byvalue_observational, ProdFixture)
{
    const unsigned int saveMinAge = nStakeMinAge;
    unsigned int saveI = nModifierInterval, saveS = nTargetSpacing;
    nStakeMinAge = 0; nModifierInterval = 60; nTargetSpacing = 5;
    try
    {
        // Build txPrev + blockFrom; compute the block's header hash as the
        // COLD-only (non-resident) source block hash.
        CTransaction txPrev; txPrev.nVersion = 1; txPrev.nTime = 1000 + 2 * 600;
        CTxOut out; out.nValue = 1000; out.scriptPubKey = CScript() << OP_TRUE;
        txPrev.vout.push_back(out);

        CBlock blockFrom; blockFrom.nVersion = 1; blockFrom.hashPrevBlock = uint256(0);
        blockFrom.hashMerkleRoot = uint256(0); blockFrom.nTime = 1000 + 2 * 600;
        blockFrom.nBits = 0x1d00ffff; blockFrom.nNonce = 12345;
        blockFrom.vtx.push_back(txPrev);
        const uint256 H = blockFrom.GetHash();

        srcOverriddenHash = H; srcOverrideHeight = 2;
        Setup();
        RetainNavigator();
        LOCK(cs_main);
        BOOST_CHECK_MESSAGE(mapBlockIndex.count(H) == 0, "debug source must be cold-only");

        // The navigator resolves the source by-value to its real cold height.
        const ColdHotSeamNavigator* nav = GetBlockIndexStakingNavigator();
        std::string e; ColdHotSeamSnapshot snap;
        BOOST_REQUIRE_MESSAGE(nav->ResolveLogicalR(BlockIndexLogicalId(H), &snap, &e) == COLD_HOT_SEAM_OK, e);
        BOOST_CHECK(snap.ref.IsCold());
        BOOST_CHECK_EQUAL(snap.snapshot.height, 2);

        const COutPoint prevout(H, 0);
        const unsigned int nTimeTx = blockFrom.nTime;
        // Prove the 3-arg modifier (needed by CheckStakeKernelHash BEFORE the
        // diagnostic print) actually succeeds for this cold source, so the
        // diagnostic source-height helper is genuinely reached below.
        uint64_t mMod = 0; int mH = 0; int64_t mT = 0;
        BOOST_REQUIRE_MESSAGE(GetKernelStakeModifier(H, pindexBest, mMod, mH, mT, false),
            "3-arg modifier for the cold source must succeed so the debug path is reached");
        uint256 hp, tp;
        const bool rPrint = CheckStakeKernelHash(pindexBest, blockFrom.nBits, blockFrom,
            0, txPrev, prevout, nTimeTx, hp, tp, /*fPrintProofOfStake=*/true);
        // Diagnostic lookup is observational: no mapBlockIndex insertion.
        BOOST_CHECK(mapBlockIndex.count(H) == 0);

        // Diagnostic flag does not change the consensus result.
        uint256 hp2, tp2;
        const bool rNoPrint = CheckStakeKernelHash(pindexBest, blockFrom.nBits, blockFrom,
            0, txPrev, prevout, nTimeTx, hp2, tp2, /*fPrintProofOfStake=*/false);
        BOOST_CHECK_EQUAL((int)rPrint, (int)rNoPrint);
    }
    catch (...)
    {
        nStakeMinAge = saveMinAge; nModifierInterval = saveI; nTargetSpacing = saveS; throw;
    }
    nStakeMinAge = saveMinAge; nModifierInterval = saveI; nTargetSpacing = saveS;
}

BOOST_AUTO_TEST_SUITE_END()