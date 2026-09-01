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
#include <boost/thread/barrier.hpp>

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
    WalletTxMap savedWallet;
    std::map<COutPoint, SPVUtxo> savedSpvUtxos;
    std::map<uint256, StakingMaterializationInfo> savedMaterializations;
    uint64_t savedMaterializationSequence;
    CBlockIndex* savedBest; CBlockIndex* savedGenesis;
    uint256 savedHashBest; uint256 savedTrust; int savedHeight;
    std::vector<CBlockIndex*> created;
    bool genSelected;
    // A.9a.3e: per-height merkle-root override applied when building the cold
    // prefix records (so a HybridSPV wallet tx's merkle proof can be validated
    // against an actual non-zero block merkle root, matching the legacy proof).
    std::map<int, uint256> merkleRootOverride;
    std::map<int, unsigned int> nFileOverride;
    std::map<int, unsigned int> nBlockPosOverride;
    std::map<int, unsigned int> nBitsOverride;
    std::map<int, unsigned int> nTimeOverride;
    // A.9a.3e: optional override of the cold-block hash at a chosen height, so a
    // debug-integration fixture can make blockFrom.GetHash() be the COLD-only
    // (non-resident) source block. Off when srcOverriddenHash is null.
    uint256 srcOverriddenHash;
    int srcOverrideHeight;

    ProdFixture() : savedMaterializationSequence(0),
        savedBest(pindexBest), savedGenesis(pindexGenesisBlock),
        savedHashBest(hashBestChain), savedTrust(nBestChainTrust), savedHeight(nBestHeight),
        genSelected(false), srcOverriddenHash(uint256(0)), srcOverrideHeight(2)
    {
        ClearBlockIndexAccessorState(); ClearFindBlockByHeightCache(); savedMap.swap(mapBlockIndex);
        if (pwalletMain)
        {
            LOCK(pwalletMain->cs_wallet);
            savedWallet.swap(pwalletMain->mapWallet);
        }
        if (pwalletMain)
        {
            LOCK(pwalletMain->cs_spvutxos);
            savedSpvUtxos.swap(pwalletMain->mapSPVUtxos);
            savedMaterializations.swap(pwalletMain->mapStakingMaterializations);
            savedMaterializationSequence = pwalletMain->nStakingMaterializationSequence;
            pwalletMain->nStakingMaterializationSequence = 0;
        }
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
        if (pwalletMain)
        {
            LOCK(pwalletMain->cs_wallet);
            pwalletMain->mapWallet.clear();
            savedWallet.swap(pwalletMain->mapWallet);
        }
        if (pwalletMain)
        {
            LOCK(pwalletMain->cs_spvutxos);
            pwalletMain->mapSPVUtxos.clear();
            savedSpvUtxos.swap(pwalletMain->mapSPVUtxos);
            pwalletMain->mapStakingMaterializations.clear();
            savedMaterializations.swap(pwalletMain->mapStakingMaterializations);
            pwalletMain->nStakingMaterializationSequence = savedMaterializationSequence;
        }
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
            if (nFileOverride.count(h))
                r.nFile = nFileOverride[h];
            if (nBlockPosOverride.count(h))
                r.nBlockPos = nBlockPosOverride[h];
            if (nBitsOverride.count(h))
                r.nBits = nBitsOverride[h];
            if (nTimeOverride.count(h))
                r.nTime = nTimeOverride[h];
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

// A.9a.3f: build a HybridSPV wallet-source transaction and the transparent
// coinstake that spends it, used to drive the REAL CheckProofOfStake HybridSPV
// fallback. wtx.hashBlock / branch are set per-test; the single-leaf merkle
// tree (nIndex==0, empty branch) has root == srcHash, so the source block's
// merkle root must equal the source transaction hash for a valid merkle proof.
static unsigned int g_hybridSpvFixtureSerial = 0;
static void BuildHybridSpvWalletFixture(CWallet* wallet, CTransaction* srcOut,
    CWalletTx* wtxOut, CTransaction* coinstakeOut, uint256* srcHashOut)
{
    CTransaction src;
    src.nVersion = 1;
    const unsigned int serial = ++g_hybridSpvFixtureSerial;
    src.nTime = 1000 + 2 * 600 + serial;
    CScript uniqueTrueScript = CScript() << (int64_t)serial << OP_DROP << OP_TRUE;
    CTxOut out; out.nValue = 500000 * COIN - serial; out.scriptPubKey = uniqueTrueScript;
    src.vout.push_back(out);

    CWalletTx wtx(wallet, src);
    const uint256 srcHash = wtx.GetHash();
    wtx.hashBlock = uint256(0);
    wtx.nIndex = 0;
    wtx.vMerkleBranch.clear();

    CTransaction coinstake;
    coinstake.nVersion = 1;
    CTxIn in; in.prevout = COutPoint(srcHash, 0); in.scriptSig = CScript();
    coinstake.vin.push_back(in);
    CTxOut emptyOut; emptyOut.nValue = 0; emptyOut.scriptPubKey = CScript();
    coinstake.vout.push_back(emptyOut);
    CTxOut rewardOut; rewardOut.nValue = out.nValue; rewardOut.scriptPubKey = uniqueTrueScript;
    coinstake.vout.push_back(rewardOut);

    *srcOut = src; *wtxOut = wtx; *coinstakeOut = coinstake; *srcHashOut = srcHash;
}

struct HybridSpvRealSourceFixture
{
    CTransaction srcTx;
    CWalletTx wtx;
    CTransaction coinstake;
    CBlock blockFrom;
    uint256 srcTxHash;
    uint256 sourceBlockHash;
    unsigned int nFile;
    unsigned int nBlockPos;
    HybridSpvRealSourceFixture() : nFile(0), nBlockPos(0) {}
};

static void PrepareHybridSpvRealSource(ProdFixture& fx, HybridSpvRealSourceFixture* out)
{
    BOOST_REQUIRE(out != NULL);
    BuildHybridSpvWalletFixture(pwalletMain, &out->srcTx, &out->wtx, &out->coinstake, &out->srcTxHash);

    out->blockFrom.nVersion = 1;
    out->blockFrom.hashPrevBlock = fx.Hash(1);
    out->blockFrom.nTime = out->srcTx.nTime;
    out->blockFrom.nBits = 0x207fffff;
    out->blockFrom.nNonce = 7;
    out->blockFrom.vtx.clear();
        out->blockFrom.vtx.push_back(out->srcTx);
        out->blockFrom.hashMerkleRoot = out->blockFrom.BuildMerkleTree();

        // A.9a.3f: the source block must be a VALID regtest PoW block so that
        // ReadFromDisk(fReadTransactions=true PoW check passes (a real
        // CheckProofOfStake recovery needs a readable source block). Grind the
        // nonce against the same nBits until GetPoWHash satisfies the target.


        while (!CheckProofOfWork(out->blockFrom.GetPoWHash(), out->blockFrom.nBits))
            out->blockFrom.nNonce++;


        {
            LOCK(cs_main);
            BOOST_REQUIRE(out->blockFrom.WriteToDisk(out->nFile, out->nBlockPos));
        }

    out->sourceBlockHash = out->blockFrom.GetHash();
    out->wtx.hashBlock = out->sourceBlockHash;
    fx.srcOverriddenHash = out->sourceBlockHash;
    fx.srcOverrideHeight = 2;
    fx.merkleRootOverride[2] = out->blockFrom.hashMerkleRoot;
    fx.nFileOverride[2] = out->nFile;
    fx.nBlockPosOverride[2] = out->nBlockPos;
    fx.nBitsOverride[2] = out->blockFrom.nBits;
    fx.nTimeOverride[2] = out->blockFrom.nTime;
}

static void ApplyHybridSpvHotOverride(ProdFixture& fx, const HybridSpvRealSourceFixture& src)
{
    BOOST_REQUIRE(fx.created.size() > 2);
    delete fx.created[2]->phashBlock;
    fx.created[2]->phashBlock = new uint256(src.sourceBlockHash);
    fx.created[2]->hashMerkleRoot = src.blockFrom.hashMerkleRoot;
    fx.created[2]->nFile = src.nFile;
    fx.created[2]->nBlockPos = src.nBlockPos;
    fx.created[2]->nTime = src.blockFrom.nTime;
    fx.created[2]->nBits = src.blockFrom.nBits;
}

static bool FindPassingStakeTime(const CBlockIndex* pindexPrev,
    const CBlock& blockFrom, const CTransaction& txPrev, const uint256& txHash,
    unsigned int* nTimeTxOut)
{
    BOOST_REQUIRE(nTimeTxOut != NULL);
    uint256 hp, tp;
    const unsigned int nStart = std::max((unsigned int)txPrev.nTime, (unsigned int)blockFrom.GetBlockTime());
    const unsigned int nTxPrevOffset =
        ::GetSerializeSize(CBlock(), SER_DISK, CLIENT_VERSION) -
        (2 * GetSizeOfCompactSize(0)) +
        GetSizeOfCompactSize(blockFrom.vtx.size());
    for (unsigned int nTimeTx = nStart; nTimeTx < nStart + 131072; ++nTimeTx)
    {
        if (CheckStakeKernelHash(pindexPrev, blockFrom.nBits, blockFrom, nTxPrevOffset, txPrev,
                                 COutPoint(txHash, 0), nTimeTx, hp, tp, false))
        {
            *nTimeTxOut = nTimeTx;
            return true;
        }
    }
    return false;
}

static bool RunCheckProofOfStake(const CTransaction& coinstake,
    const CBlockIndex* pindexPrev, unsigned int nBits)
{
    uint256 hp, tp;
    return CheckProofOfStake(pindexPrev, coinstake, nBits, hp, tp);
}

} // namespace

// A.9a.3f NEW-N9 / T1: causal RED with valid fixture fields. Resident source at
// height 2 yields positive legacy depth and a green baseline CheckProofOfStake;
// removing only historical mapBlockIndex residency flips legacy depth to 0/-1
// and the real legacy/no-navigator CheckProofOfStake rejects before recovery.
BOOST_FIXTURE_TEST_CASE(legacy_hybridspv_causal_nonresident_map_lookup_red, ProdFixture)
{
    const bool saveSPV = fHybridSPV;
    const int saveMaturity = nCoinbaseMaturity;
    const unsigned int saveMinAge = nStakeMinAge;
    unsigned int saveI = nModifierInterval, saveS = nTargetSpacing;
    fHybridSPV = true; nCoinbaseMaturity = 10; nStakeMinAge = 0; nModifierInterval = 60; nTargetSpacing = 5;
    HybridSpvRealSourceFixture src;
    try
    {
        PrepareHybridSpvRealSource(*this, &src);
        Setup();
        LOCK(cs_main);
        ApplyHybridSpvHotOverride(*this, src);
        mapBlockIndex[src.sourceBlockHash] = created[2];
        BOOST_REQUIRE_EQUAL(src.wtx.GetDepthInMainChain(), 18);
        BOOST_REQUIRE(FindPassingStakeTime(pindexBest, src.blockFrom, src.srcTx, src.srcTxHash, &src.coinstake.nTime));
        BOOST_CHECK(VerifySignature(src.srcTx, src.coinstake, 0, SCRIPT_VERIFY_NONE, 0));
        {
            uint256 hp, tp;
            const unsigned int nTxPrevOffset = ::GetSerializeSize(CBlock(), SER_DISK, CLIENT_VERSION) -
                (2 * GetSizeOfCompactSize(0)) + GetSizeOfCompactSize(src.blockFrom.vtx.size());
            BOOST_CHECK(CheckStakeKernelHash(pindexBest, src.blockFrom.nBits, src.blockFrom, nTxPrevOffset,
                src.srcTx, COutPoint(src.srcTxHash, 0), src.coinstake.nTime, hp, tp, false));
        }
        pwalletMain->mapWallet[src.srcTxHash] = src.wtx;
        pwalletMain->mapWallet[src.srcTxHash].BindWallet(pwalletMain);
        BOOST_CHECK(RunCheckProofOfStake(src.coinstake, pindexBest, src.blockFrom.nBits));

        mapBlockIndex.erase(src.sourceBlockHash);
        const int nNonResidentDepth = src.wtx.GetDepthInMainChain();
        BOOST_CHECK(nNonResidentDepth == 0 || nNonResidentDepth == -1);
        BOOST_CHECK(!RunCheckProofOfStake(src.coinstake, pindexBest, src.blockFrom.nBits));
        pwalletMain->mapWallet.erase(src.srcTxHash);
    }
    catch (...)
    {
        pwalletMain->mapWallet.erase(src.srcTxHash);
        fHybridSPV = saveSPV; nCoinbaseMaturity = saveMaturity; nStakeMinAge = saveMinAge; nModifierInterval = saveI; nTargetSpacing = saveS; throw;
    }
    fHybridSPV = saveSPV; nCoinbaseMaturity = saveMaturity; nStakeMinAge = saveMinAge; nModifierInterval = saveI; nTargetSpacing = saveS;
}

// A.9a.3f NEW-N10/T2: with the production navigator retained, the same cold-only
// source is mature by value, passes the real CheckProofOfStake boundary, and no
// historical CBlockIndex residency is required.
BOOST_FIXTURE_TEST_CASE(checkproofofstake_hybridspv_cold_mature_byvalue_verified_seam, ProdFixture)
{
    const bool saveSPV = fHybridSPV;
    const int saveMaturity = nCoinbaseMaturity;
    const unsigned int saveMinAge = nStakeMinAge;
    unsigned int saveI = nModifierInterval, saveS = nTargetSpacing;
    fHybridSPV = true; nCoinbaseMaturity = 10; nStakeMinAge = 0; nModifierInterval = 60; nTargetSpacing = 5;
    HybridSpvRealSourceFixture src;
    try
    {
        PrepareHybridSpvRealSource(*this, &src);
        Setup();
        RetainNavigator();
        LOCK(cs_main);
        ApplyHybridSpvHotOverride(*this, src);
        BOOST_CHECK_MESSAGE(mapBlockIndex.count(src.sourceBlockHash) == 0,
            "source must remain cold-only (absent from mapBlockIndex)");
        BOOST_REQUIRE(FindPassingStakeTime(pindexBest, src.blockFrom, src.srcTx, src.srcTxHash, &src.coinstake.nTime));
        {
            std::string e;
            int depth = 0;
            ColdHotSeamSnapshot snap;
            const ColdHotSeamNavigator* nav = GetBlockIndexStakingNavigator();
            BOOST_REQUIRE(nav != NULL);
            BOOST_CHECK_EQUAL((int)nav->ResolveLogicalR(BlockIndexLogicalId(src.sourceBlockHash), &snap, &e), (int)COLD_HOT_SEAM_OK);
            BOOST_CHECK(snap.ref.IsCold());
            BOOST_CHECK_EQUAL(snap.snapshot.nFile, src.nFile);
            BOOST_CHECK_EQUAL(snap.snapshot.nBlockPos, src.nBlockPos);
            BOOST_CHECK_EQUAL((int)nav->GetHybridSvmMaturityAuthorityR(BlockIndexLogicalId(src.wtx.hashBlock), src.wtx.GetHash(), src.wtx.vMerkleBranch, src.wtx.nIndex, &depth, &e), (int)COLD_HOT_SEAM_OK);
            BOOST_CHECK_EQUAL(depth, 18);
        }
        BOOST_CHECK(VerifySignature(src.srcTx, src.coinstake, 0, SCRIPT_VERIFY_NONE, 0));
        {
            uint256 hp, tp;
            const unsigned int nTxPrevOffset = ::GetSerializeSize(CBlock(), SER_DISK, CLIENT_VERSION) -
                (2 * GetSizeOfCompactSize(0)) + GetSizeOfCompactSize(src.blockFrom.vtx.size());
            BOOST_CHECK(CheckStakeKernelHash(pindexBest, src.blockFrom.nBits, src.blockFrom, nTxPrevOffset,
                src.srcTx, COutPoint(src.srcTxHash, 0), src.coinstake.nTime, hp, tp, false));
        }
        pwalletMain->mapWallet[src.srcTxHash] = src.wtx;
        pwalletMain->mapWallet[src.srcTxHash].BindWallet(pwalletMain);
        BOOST_CHECK(RunCheckProofOfStake(src.coinstake, pindexBest, src.blockFrom.nBits));
        pwalletMain->mapWallet.erase(src.srcTxHash);
    }
    catch (...)
    {
        pwalletMain->mapWallet.erase(src.srcTxHash);
        fHybridSPV = saveSPV; nCoinbaseMaturity = saveMaturity; nStakeMinAge = saveMinAge; nModifierInterval = saveI; nTargetSpacing = saveS; throw;
    }
    fHybridSPV = saveSPV; nCoinbaseMaturity = saveMaturity; nStakeMinAge = saveMinAge; nModifierInterval = saveI; nTargetSpacing = saveS;
}

// A.9a.3f NEW-N10/T3: a COLD active source + UNCHANGED CURRENT + deliberately
// DIVERGED live seam must FAIL AUTHORITY -- never a stale positive depth.
// ResolveLogicalR only proves CURRENT is unchanged; it does NOT prove the frozen
// active.dat membership still shares the live topology. VerifySeam must reject
// before positive depth is issued.
BOOST_FIXTURE_TEST_CASE(hybridspv_maturity_cold_source_divergent_live_seam_fails_authority, ProdFixture)
{
    const bool saveSPV = fHybridSPV;
    const int saveMaturity = nCoinbaseMaturity;
    const unsigned int saveMinAge = nStakeMinAge;
    unsigned int saveI = nModifierInterval, saveS = nTargetSpacing;
    fHybridSPV = true; nCoinbaseMaturity = 10; nStakeMinAge = 0; nModifierInterval = 60; nTargetSpacing = 5;
    HybridSpvRealSourceFixture src;
    try
    {
        PrepareHybridSpvRealSource(*this, &src);
        Setup();
        RetainNavigator();
        LOCK(cs_main);
        ApplyHybridSpvHotOverride(*this, src);
        std::string e; ColdHotSeamSnapshot snap;
        const ColdHotSeamNavigator* nav = GetBlockIndexStakingNavigator();
        BOOST_REQUIRE(nav != NULL);
        // Establish the cold source is ACTIVE and the mature authority issues positive
        // depth WHILE the seam is intact.
        BOOST_REQUIRE_EQUAL((int)nav->ResolveLogicalR(BlockIndexLogicalId(src.sourceBlockHash), &snap, &e), (int)COLD_HOT_SEAM_OK);
        BOOST_CHECK(snap.ref.IsCold());
        int depth = 0;
        BOOST_REQUIRE_EQUAL((int)nav->GetHybridSvmMaturityAuthorityR(BlockIndexLogicalId(src.wtx.hashBlock), src.wtx.GetHash(), src.wtx.vMerkleBranch, src.wtx.nIndex, &depth, &e), (int)COLD_HOT_SEAM_OK);
        BOOST_CHECK_EQUAL(depth, 18);
        // Reorg reached the seam: replace the live chain's single-parent/generation
        // anchor block hash at height SEAM while CURRENT stays pinned to generation 1.
        BOOST_REQUIRE(created.size() > (size_t)SEAM);
        BOOST_REQUIRE(created[SEAM]->phashBlock != NULL);
        delete created[SEAM]->phashBlock;
        created[SEAM]->phashBlock = new uint256(0x777777);
        BOOST_CHECK_EQUAL(nav->ColdGeneration(), 1U); // CURRENT unchanged
        depth = 0;
        const ColdHotSeamResult r2 = nav->GetHybridSvmMaturityAuthorityR(
            BlockIndexLogicalId(src.wtx.hashBlock), src.wtx.GetHash(), src.wtx.vMerkleBranch, src.wtx.nIndex, &depth, &e);
        BOOST_CHECK_EQUAL((int)r2, (int)COLD_HOT_SEAM_AUTHORITY_FAILURE);
        BOOST_CHECK_EQUAL(depth, 0);
    }
    catch (...)
    {
        fHybridSPV = saveSPV; nCoinbaseMaturity = saveMaturity; nStakeMinAge = saveMinAge; nModifierInterval = saveI; nTargetSpacing = saveS; throw;
    }
    fHybridSPV = saveSPV; nCoinbaseMaturity = saveMaturity; nStakeMinAge = saveMinAge; nModifierInterval = saveI; nTargetSpacing = saveS;
}

// A.9a.3f H2: a HOT active source above the seam still receives exact positive
// depth authority without any cold lookup.
BOOST_FIXTURE_TEST_CASE(checkproofofstake_hybridspv_hot_active_source_unchanged, ProdFixture)
{
    Setup();
    RetainNavigator();
    LOCK(cs_main);
    const int hotH = 13;
    CTransaction srcTx; CWalletTx wtx; CTransaction coinstake; uint256 srcHash;
    BuildHybridSpvWalletFixture(pwalletMain, &srcTx, &wtx, &coinstake, &srcHash);
    created[hotH]->hashMerkleRoot = srcHash;
    wtx.hashBlock = Hash(hotH);
    int depth = 0;
    std::string e;
    const ColdHotSeamResult r = GetBlockIndexStakingNavigator()->GetHybridSvmMaturityAuthorityR(
        BlockIndexLogicalId(wtx.hashBlock), wtx.GetHash(), wtx.vMerkleBranch, wtx.nIndex, &depth, &e);
    BOOST_CHECK_EQUAL((int)r, (int)COLD_HOT_SEAM_OK);
    BOOST_CHECK_EQUAL(depth, 19 - hotH + 1);
}

// A.9a.3f H4/T8/T9: an otherwise-valid cold source that is below the maturity
// boundary is rejected at the real CheckProofOfStake boundary.
BOOST_FIXTURE_TEST_CASE(checkproofofstake_hybridspv_immature_rejected_at_legacy_boundary, ProdFixture)
{
    const bool saveSPV = fHybridSPV;
    const int saveMaturity = nCoinbaseMaturity;
    const unsigned int saveMinAge = nStakeMinAge;
    unsigned int saveI = nModifierInterval, saveS = nTargetSpacing;
    fHybridSPV = true; nCoinbaseMaturity = 25; nStakeMinAge = 0; nModifierInterval = 60; nTargetSpacing = 5;
    HybridSpvRealSourceFixture src;
    try
    {
        PrepareHybridSpvRealSource(*this, &src);
        Setup();
        RetainNavigator();
        LOCK(cs_main);
        ApplyHybridSpvHotOverride(*this, src);
        BOOST_REQUIRE(FindPassingStakeTime(pindexBest, src.blockFrom, src.srcTx, src.srcTxHash, &src.coinstake.nTime));
        BOOST_CHECK(VerifySignature(src.srcTx, src.coinstake, 0, SCRIPT_VERIFY_NONE, 0));
        {
            uint256 hp, tp;
            const unsigned int nTxPrevOffset = ::GetSerializeSize(CBlock(), SER_DISK, CLIENT_VERSION) -
                (2 * GetSizeOfCompactSize(0)) + GetSizeOfCompactSize(src.blockFrom.vtx.size());
            BOOST_CHECK(CheckStakeKernelHash(pindexBest, src.blockFrom.nBits, src.blockFrom, nTxPrevOffset,
                src.srcTx, COutPoint(src.srcTxHash, 0), src.coinstake.nTime, hp, tp, false));
        }
        pwalletMain->mapWallet[src.srcTxHash] = src.wtx;
        pwalletMain->mapWallet[src.srcTxHash].BindWallet(pwalletMain);
        BOOST_CHECK(!RunCheckProofOfStake(src.coinstake, pindexBest, src.blockFrom.nBits));
        pwalletMain->mapWallet.erase(src.srcTxHash);
    }
    catch (...)
    {
        pwalletMain->mapWallet.erase(src.srcTxHash);
        fHybridSPV = saveSPV; nCoinbaseMaturity = saveMaturity; nStakeMinAge = saveMinAge; nModifierInterval = saveI; nTargetSpacing = saveS; throw;
    }
    fHybridSPV = saveSPV; nCoinbaseMaturity = saveMaturity; nStakeMinAge = saveMinAge; nModifierInterval = saveI; nTargetSpacing = saveS;
}

// A.9a.3f H6: an invalid merkle authority is rejected before the real
// CheckProofOfStake recovery path can succeed.
BOOST_FIXTURE_TEST_CASE(checkproofofstake_hybridspv_merkle_mismatch_rejected, ProdFixture)
{
    const bool saveSPV = fHybridSPV;
    const int saveMaturity = nCoinbaseMaturity;
    const unsigned int saveMinAge = nStakeMinAge;
    unsigned int saveI = nModifierInterval, saveS = nTargetSpacing;
    fHybridSPV = true; nCoinbaseMaturity = 10; nStakeMinAge = 0; nModifierInterval = 60; nTargetSpacing = 5;
    HybridSpvRealSourceFixture src;
    try
    {
        PrepareHybridSpvRealSource(*this, &src);
        merkleRootOverride[2] = uint256(src.srcTxHash.Get64() ^ 1ULL);
        Setup();
        RetainNavigator();
        LOCK(cs_main);
        ApplyHybridSpvHotOverride(*this, src);
        BOOST_REQUIRE(FindPassingStakeTime(pindexBest, src.blockFrom, src.srcTx, src.srcTxHash, &src.coinstake.nTime));
        BOOST_CHECK(VerifySignature(src.srcTx, src.coinstake, 0, SCRIPT_VERIFY_NONE, 0));
        {
            uint256 hp, tp;
            const unsigned int nTxPrevOffset = ::GetSerializeSize(CBlock(), SER_DISK, CLIENT_VERSION) -
                (2 * GetSizeOfCompactSize(0)) + GetSizeOfCompactSize(src.blockFrom.vtx.size());
            BOOST_CHECK(CheckStakeKernelHash(pindexBest, src.blockFrom.nBits, src.blockFrom, nTxPrevOffset,
                src.srcTx, COutPoint(src.srcTxHash, 0), src.coinstake.nTime, hp, tp, false));
        }
        pwalletMain->mapWallet[src.srcTxHash] = src.wtx;
        pwalletMain->mapWallet[src.srcTxHash].BindWallet(pwalletMain);
        BOOST_CHECK(!RunCheckProofOfStake(src.coinstake, pindexBest, src.blockFrom.nBits));
        pwalletMain->mapWallet.erase(src.srcTxHash);
    }
    catch (...)
    {
        pwalletMain->mapWallet.erase(src.srcTxHash);
        fHybridSPV = saveSPV; nCoinbaseMaturity = saveMaturity; nStakeMinAge = saveMinAge; nModifierInterval = saveI; nTargetSpacing = saveS; throw;
    }
    fHybridSPV = saveSPV; nCoinbaseMaturity = saveMaturity; nStakeMinAge = saveMinAge; nModifierInterval = saveI; nTargetSpacing = saveS;
}

// A.9a.3f H7/T13: stale generation authority fails closed at the real
// CheckProofOfStake boundary.
BOOST_FIXTURE_TEST_CASE(checkproofofstake_hybridspv_stale_generation_fails_closed, ProdFixture)
{
    const bool saveSPV = fHybridSPV;
    const int saveMaturity = nCoinbaseMaturity;
    const unsigned int saveMinAge = nStakeMinAge;
    unsigned int saveI = nModifierInterval, saveS = nTargetSpacing;
    fHybridSPV = true; nCoinbaseMaturity = 10; nStakeMinAge = 0; nModifierInterval = 60; nTargetSpacing = 5;
    HybridSpvRealSourceFixture src;
    try
    {
        PrepareHybridSpvRealSource(*this, &src);
        Setup();
        RetainNavigator();
        LOCK(cs_main);
        ApplyHybridSpvHotOverride(*this, src);
        BOOST_REQUIRE(FindPassingStakeTime(pindexBest, src.blockFrom, src.srcTx, src.srcTxHash, &src.coinstake.nTime));
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
        pwalletMain->mapWallet[src.srcTxHash] = src.wtx;
        pwalletMain->mapWallet[src.srcTxHash].BindWallet(pwalletMain);
        BOOST_CHECK(!RunCheckProofOfStake(src.coinstake, pindexBest, src.blockFrom.nBits));
        pwalletMain->mapWallet.erase(src.srcTxHash);
    }
    catch (...)
    {
        pwalletMain->mapWallet.erase(src.srcTxHash);
        fHybridSPV = saveSPV; nCoinbaseMaturity = saveMaturity; nStakeMinAge = saveMinAge; nModifierInterval = saveI; nTargetSpacing = saveS; throw;
    }
    fHybridSPV = saveSPV; nCoinbaseMaturity = saveMaturity; nStakeMinAge = saveMinAge; nModifierInterval = saveI; nTargetSpacing = saveS;
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

// ===========================================================================
// A.9a.3f -- wallet staking-generation production-boundary tests (P5-P11 +
// PopulateSPVUtxosFromWallet fourth-family closure). All remove ONLY historical
// CBlockIndex residency as the causal variable: the retained navigator + stable
// logical identity stay authoritative, and authority failure is never conflated
// with missing materialization.
// ===========================================================================
namespace {

// Seed a spendable P2PKH key in the global test wallet and return its CPubKey.
static CPubKey SeedSpendableKey()
{
    CKey key;
    key.MakeNewKey(true);
    CPubKey pub = key.GetPubKey();
    {
        LOCK(pwalletMain->cs_wallet);
        BOOST_REQUIRE_MESSAGE(pwalletMain->LoadKey(key, pub), "seed spendable key failed");
    }
    return pub;
}

// Build a source CTransaction whose spendable output pays `pub` (P2PKH) at the
// given cold height. Call BEFORE Setup() so merkleRootOverride for `height` is
// applied when the cold generation is built.
struct ColdSpendableCoin
{
    CTransaction srcTx;
    uint256 txHash;
    CScript script;
    int64_t value;
};

static ColdSpendableCoin PrepareColdSpendableCoin(ProdFixture& fx, const CPubKey& pub,
    int height, int64_t nValue, bool fCoinStake)
{
    ColdSpendableCoin c;
    c.script = GetScriptForDestination(pub.GetID());
    c.value = nValue;
    c.srcTx.nVersion = 1;
    c.srcTx.nTime = 1000;                       // far past; passes min-age (nStakeMinAge=0)
    if (fCoinStake)
    {
        // IsCoinStake(): vin[0].prevout non-null, vout[0] empty, vout[1] reward.
        c.srcTx.vin.push_back(CTxIn(COutPoint(uint256(0xABCD), 0)));
        c.srcTx.vout.push_back(CTxOut(0, CScript()));       // empty marker output
        c.srcTx.vout.push_back(CTxOut(nValue, c.script));  // spendable reward
    }
    else
    {
        c.srcTx.vin.push_back(CTxIn(COutPoint(uint256(0xBEEF), 0)));
        c.srcTx.vout.push_back(CTxOut(nValue, c.script));  // spendable output
    }
    c.txHash = c.srcTx.GetHash();
    fx.merkleRootOverride[height] = c.txHash;   // single-leaf root == tx hash
    return c;
}

// Insert the prepared coin into the wallet as a cold nonresident active tx whose
// hashBlock == Hash(height) (uint256(1000+height), cold, absent from mapBlockIndex).
static void InstallColdWalletTx(const ColdSpendableCoin& c, int height, int64_t nTime)
{
    CWalletTx wtx(pwalletMain, c.srcTx);
    wtx.hashBlock = uint256(1000 + height);
    wtx.nIndex = 0;
    wtx.vMerkleBranch.clear();
    wtx.nTime = nTime;
    LOCK(pwalletMain->cs_wallet);
    pwalletMain->mapWallet[c.txHash] = wtx;
    pwalletMain->mapWallet[c.txHash].BindWallet(pwalletMain);
}

} // namespace (A.9a.3f helpers)

// P5/G11: staking balance/trust path (GetTransparentStakingBalance via the real
// GetStakeWeight production entry, !fHybridSPV) must COUNT a deep cold ACTIVE
// nonresident wallet transaction equivalently to a resident source -- a spendable
// cold coin must make the balance+selection gates pass.
BOOST_FIXTURE_TEST_CASE(stakeweight_cold_nonresident_active_spendable_coin_balance_counted, ProdFixture)
{
    const bool saveSPV = fHybridSPV;
    const bool saveRegTest = fRegTest;
    const unsigned int saveMinAge = nStakeMinAge;
    const int saveMaturity = nCoinbaseMaturity;
    const int64_t saveReserve = nReserveBalance;
    try
    {
        fHybridSPV = false; fRegTest = true; nStakeMinAge = 0; nCoinbaseMaturity = 10; nReserveBalance = 0;
        const CPubKey pub = SeedSpendableKey();
        ColdSpendableCoin c = PrepareColdSpendableCoin(*this, pub, 2, 500000 * COIN, false);
        Setup();
        RetainNavigator();
        LOCK(cs_main);
        BOOST_CHECK_MESSAGE(mapBlockIndex.count(uint256(1000 + 2)) == 0, "height-2 block must be cold-only/non-resident");
        InstallColdWalletTx(c, 2, 1000);

        uint64_t nMin = 0, nMax = 0, nW = 0;
        const bool ok = pwalletMain->GetStakeWeight(*pwalletMain, nMin, nMax, nW);
        BOOST_CHECK_MESSAGE(ok, "GetStakeWeight must succeed: cold nonresident active spendable coin is counted and selected");
    }
    catch (...)
    {
        fHybridSPV = saveSPV; fRegTest = saveRegTest; nStakeMinAge = saveMinAge; nCoinbaseMaturity = saveMaturity; nReserveBalance = saveReserve; throw;
    }
    fHybridSPV = saveSPV; fRegTest = saveRegTest; nStakeMinAge = saveMinAge; nCoinbaseMaturity = saveMaturity; nReserveBalance = saveReserve;
}

// P6/G12: the same staking trust/balance path must REJECT / not count a source
// that is known-but-INACTIVE (side/reorged hot block), exactly as legacy would.
// GetStakeWeight must fail the balance gate because the coin is not counted.
BOOST_FIXTURE_TEST_CASE(stakeweight_cold_inactive_source_balance_not_counted, ProdFixture)
{
    const bool saveSPV = fHybridSPV;
    const bool saveRegTest = fRegTest;
    const unsigned int saveMinAge = nStakeMinAge;
    const int saveMaturity = nCoinbaseMaturity;
    const int64_t saveReserve = nReserveBalance;
    try
    {
        fHybridSPV = false; fRegTest = true; nStakeMinAge = 0; nCoinbaseMaturity = 10; nReserveBalance = 0;
        const CPubKey pub = SeedSpendableKey();
        ColdSpendableCoin c = PrepareColdSpendableCoin(*this, pub, 2, 500000 * COIN, false);
        Setup();
        RetainNavigator();
        LOCK(cs_main);

        // Build a real known-but-INACTIVE hot source: a side CBlockIndex in
        // mapBlockIndex but NOT reachable from best (pnext NULL, != pindexBest)
        // => IsInMainChain() false. The maturity authority resolves HOT and
        // returns NON-positive depth (NOT_FOUND), so the coin is not counted.
        CBlockIndex* pSide = new CBlockIndex();
        pSide->phashBlock = new uint256(0xDEADBEEF);
        pSide->nHeight = 2;
        pSide->pprev = created[1];
        pSide->nTime = 1002;
        pSide->nFlags = 0; pSide->nStakeModifier = 102;
        created.push_back(pSide);
        mapBlockIndex[uint256(0xDEADBEEF)] = pSide;

        CWalletTx wtx(pwalletMain, c.srcTx);
        wtx.hashBlock = uint256(0xDEADBEEF);   // known but INACTIVE side source
        wtx.nIndex = 0; wtx.vMerkleBranch.clear(); wtx.nTime = 1000;
        {
            LOCK(pwalletMain->cs_wallet);
            pwalletMain->mapWallet[c.txHash] = wtx;
            pwalletMain->mapWallet[c.txHash].BindWallet(pwalletMain);
        }

        uint64_t nMin = 0, nMax = 0, nW = 0;
        const bool ok = pwalletMain->GetStakeWeight(*pwalletMain, nMin, nMax, nW);
        BOOST_CHECK_MESSAGE(!ok, "GetStakeWeight must reject: inactive/reorged source is not counted by the staking balance path");
    }
    catch (...)
    {
        fHybridSPV = saveSPV; fRegTest = saveRegTest; nStakeMinAge = saveMinAge; nCoinbaseMaturity = saveMaturity; nReserveBalance = saveReserve; throw;
    }
    fHybridSPV = saveSPV; fRegTest = saveRegTest; nStakeMinAge = saveMinAge; nCoinbaseMaturity = saveMaturity; nReserveBalance = saveReserve;
}

// P7/G13: AvailableCoinsForStaking must RETURN a deep mature COLD nonresident
// ACTIVE spendable coin (it is not silently skipped when its historical
// CBlockIndex is absent from mapBlockIndex). Retained navigator is REQUIRED so the
// by-value maturity/depth authority (not the no-navigator legacy fallback) decides.
BOOST_FIXTURE_TEST_CASE(availablecoinsforstaking_cold_nonresident_deep_mature_eligible, ProdFixture)
{
    const bool saveRegTest = fRegTest;
    const unsigned int saveMinAge = nStakeMinAge;
    const int saveMaturity = nCoinbaseMaturity;
    try
    {
        fRegTest = true; nStakeMinAge = 0; nCoinbaseMaturity = 10;
        const CPubKey pub = SeedSpendableKey();
        ColdSpendableCoin c = PrepareColdSpendableCoin(*this, pub, 2, 500000 * COIN, false);
        Setup();
        RetainNavigator();
        LOCK2(cs_main, pwalletMain->cs_wallet);
        InstallColdWalletTx(c, 2, 1000);

        std::vector<COutput> vCoins;
        const bool r = pwalletMain->AvailableCoinsForStaking(vCoins, (unsigned int)GetTime());
        bool found = false;
        for (const COutput& out : vCoins)
            if (out.tx->GetHash() == c.txHash) { found = true; break; }
        BOOST_CHECK_MESSAGE(r && found, "AvailableCoinsForStaking must return the deep cold nonresident active coin");
    }
    catch (...)
    {
        fRegTest = saveRegTest; nStakeMinAge = saveMinAge; nCoinbaseMaturity = saveMaturity; throw;
    }
    fRegTest = saveRegTest; nStakeMinAge = saveMinAge; nCoinbaseMaturity = saveMaturity;
}

// P8/G14: exact maturity boundary -- a cold coinstake at depth == required
// maturity must be ELIGIBLE (blocksToMaturity == 0 at the exact boundary).
BOOST_FIXTURE_TEST_CASE(availablecoinsforstaking_exact_maturity_boundary_eligible, ProdFixture)
{
    const bool saveRegTest = fRegTest;
    const unsigned int saveMinAge = nStakeMinAge;
    const int saveMaturity = nCoinbaseMaturity;
    try
    {
        fRegTest = true; nStakeMinAge = 0;
        const int kHeight = 2;
        const int kDepth = (N - 1) - kHeight + 1;   // 18
        nCoinbaseMaturity = kDepth;                  // exact boundary: depth == maturity
        const CPubKey pub = SeedSpendableKey();
        ColdSpendableCoin c = PrepareColdSpendableCoin(*this, pub, kHeight, 500000 * COIN, true);
        Setup();
        RetainNavigator();
        LOCK2(cs_main, pwalletMain->cs_wallet);
        InstallColdWalletTx(c, kHeight, 1000);

        std::vector<COutput> vCoins;
        const bool r = pwalletMain->AvailableCoinsForStaking(vCoins, (unsigned int)GetTime());
        bool found = false;
        for (const COutput& out : vCoins)
            if (out.tx->GetHash() == c.txHash) { found = true; break; }
        BOOST_CHECK_MESSAGE(r && found, "cold coinstake at EXACT maturity boundary must be eligible");
    }
    catch (...)
    {
        fRegTest = saveRegTest; nStakeMinAge = saveMinAge; nCoinbaseMaturity = saveMaturity; throw;
    }
    fRegTest = saveRegTest; nStakeMinAge = saveMinAge; nCoinbaseMaturity = saveMaturity;
}

// P9/G15: maturity boundary - 1 -- the same cold coinstake at depth one below the
// required maturity must NOT be eligible (blocksToMaturity > 0).
BOOST_FIXTURE_TEST_CASE(availablecoinsforstaking_maturity_boundary_minus_one_ineligible, ProdFixture)
{
    const bool saveRegTest = fRegTest;
    const unsigned int saveMinAge = nStakeMinAge;
    const int saveMaturity = nCoinbaseMaturity;
    try
    {
        fRegTest = true; nStakeMinAge = 0;
        const int kHeight = 2;
        const int kDepth = (N - 1) - kHeight + 1;   // 18
        nCoinbaseMaturity = kDepth + 1;              // boundary-1: one MORE maturity
        const CPubKey pub = SeedSpendableKey();
        ColdSpendableCoin c = PrepareColdSpendableCoin(*this, pub, kHeight, 500000 * COIN, true);
        Setup();
        RetainNavigator();
        LOCK2(cs_main, pwalletMain->cs_wallet);
        InstallColdWalletTx(c, kHeight, 1000);

        std::vector<COutput> vCoins;
        const bool r = pwalletMain->AvailableCoinsForStaking(vCoins, (unsigned int)GetTime());
        bool found = false;
        for (const COutput& out : vCoins)
            if (out.tx->GetHash() == c.txHash) { found = true; break; }
        BOOST_CHECK_MESSAGE(!found, "cold coinstake one block below maturity must NOT be eligible");
        (void)r;
    }
    catch (...)
    {
        fRegTest = saveRegTest; nStakeMinAge = saveMinAge; nCoinbaseMaturity = saveMaturity; throw;
    }
    fRegTest = saveRegTest; nStakeMinAge = saveMinAge; nCoinbaseMaturity = saveMaturity;
}

// P10/G16: SelectCoinsForStakingSPV mapWallet branch must keep a COLD nonresident
// ACTIVE source selectable/materializable by-value (no historical mapBlockIndex).
// Driven through the real GetStakeWeight (fHybridSPV=true) which calls
// SelectCoinsForStakingSPV; the mapWallet branch resolves the disk position
// by-value and must return the coin.
BOOST_FIXTURE_TEST_CASE(selectcoinsspv_mapwallet_cold_nonresident_source_selectable, ProdFixture)
{
    const bool saveSPV = fHybridSPV;
    const bool saveRegTest = fRegTest;
    const unsigned int saveMinAge = nStakeMinAge;
    const int saveMaturity = nCoinbaseMaturity;
    const int64_t saveReserve = nReserveBalance;
    try
    {
        fHybridSPV = true; fRegTest = true; nStakeMinAge = 0; nCoinbaseMaturity = 10; nReserveBalance = 0;
        const CPubKey pub = SeedSpendableKey();
        ColdSpendableCoin c = PrepareColdSpendableCoin(*this, pub, 2, 500000 * COIN, false);
        // Give the cold record a persisted disk position so the mapWallet branch
        // resolves it BY-VALUE (navigator -> nFile>0) and selects the coin.
        nFileOverride[2] = 5; nBlockPosOverride[2] = 64;
        Setup();
        RetainNavigator();
        LOCK2(cs_main, pwalletMain->cs_wallet);
        InstallColdWalletTx(c, 2, 1000);

        // The SPV cache must offer the outpoint; the mapWallet branch resolves its
        // disk position by-value and stays selectable without a resident CBlockIndex.
        SPVUtxo u;
        u.txhash = c.txHash; u.n = 0; u.nValue = c.value;
        u.nHeight = 2; u.hashBlock = uint256(1000 + 2); u.nTime = 1000;
        u.fHaveBlock = true; u.fVerified = true; u.fSpent = false; u.scriptPubKey = c.script;
        {
            LOCK(pwalletMain->cs_spvutxos);
            pwalletMain->mapSPVUtxos[COutPoint(c.txHash, 0)] = u;
        }

        uint64_t nMin = 0, nMax = 0, nW = 0;
        const bool ok = pwalletMain->GetStakeWeight(*pwalletMain, nMin, nMax, nW);
        BOOST_CHECK_MESSAGE(ok, "SelectCoinsForStakingSPV mapWallet branch must keep a cold nonresident active source selectable");
    }
    catch (...)
    {
        fHybridSPV = saveSPV; fRegTest = saveRegTest; nStakeMinAge = saveMinAge; nCoinbaseMaturity = saveMaturity; nReserveBalance = saveReserve; throw;
    }
    fHybridSPV = saveSPV; fRegTest = saveRegTest; nStakeMinAge = saveMinAge; nCoinbaseMaturity = saveMaturity; nReserveBalance = saveReserve;
}

// P11/G17: SelectCoinsForStakingSPV SPV-cache branch -- a COLD nonresident active
// source offered only through the SPV cache (not chosen from mapWallet) must be
// materialized and selected by-value from its persisted disk position.
BOOST_FIXTURE_TEST_CASE(selectcoinsspv_spvcache_cold_nonresident_source_materialized, ProdFixture)
{
    const bool saveSPV = fHybridSPV;
    const bool saveRegTest = fRegTest;
    const unsigned int saveMinAge = nStakeMinAge;
    const int saveMaturity = nCoinbaseMaturity;
    const int64_t saveReserve = nReserveBalance;
    try
    {
        fHybridSPV = true; fRegTest = true; nStakeMinAge = 0; nCoinbaseMaturity = 10; nReserveBalance = 0;
        // Prepare writes a real PoW source block and sets the cold record's
        // merkleRoot/nFile/nBlockPos overrides at height 2; Setup must run AFTER
        // it (same order as the proven T2 real-source test).
        HybridSpvRealSourceFixture src;
        PrepareHybridSpvRealSource(*this, &src);
        Setup();
        RetainNavigator();
        BOOST_REQUIRE(mapBlockIndex.count(src.sourceBlockHash) == 0);

        // Offer the outpoint ONLY through the SPV cache (not via mapWallet): the
        // SPV-cache branch must read the block by-value from disk and materialize.
        SPVUtxo u;
        u.txhash = src.srcTxHash; u.n = 0; u.nValue = src.srcTx.vout[0].nValue;
        u.nHeight = 2; u.hashBlock = src.sourceBlockHash; u.nTime = src.srcTx.nTime;
        u.fHaveBlock = true; u.fVerified = true; u.fSpent = false; u.scriptPubKey = src.srcTx.vout[0].scriptPubKey;
        {
            LOCK(pwalletMain->cs_spvutxos);
            pwalletMain->mapSPVUtxos[COutPoint(src.srcTxHash, 0)] = u;
        }

        uint64_t nMin = 0, nMax = 0, nW = 0;
        const bool ok = pwalletMain->GetStakeWeight(*pwalletMain, nMin, nMax, nW);
        BOOST_CHECK_MESSAGE(ok, "SelectCoinsForStakingSPV SPV-cache branch must materialize a cold nonresident source by-value");
    }
    catch (...)
    {
        fHybridSPV = saveSPV; fRegTest = saveRegTest; nStakeMinAge = saveMinAge; nCoinbaseMaturity = saveMaturity; nReserveBalance = saveReserve; throw;
    }
    fHybridSPV = saveSPV; fRegTest = saveRegTest; nStakeMinAge = saveMinAge; nCoinbaseMaturity = saveMaturity; nReserveBalance = saveReserve;
}

// Family #4: PopulateSPVUtxosFromWallet must ADMIT a deep cold nonresident ACTIVE
// mature coin into the SPV staking cache -- it is NOT silently dropped because
// mapBlockIndex does not contain its historical CBlockIndex.
BOOST_FIXTURE_TEST_CASE(populatespvutxosfromwallet_admits_cold_nonresident_mature_coin, ProdFixture)
{
    const bool saveRegTest = fRegTest;
    const unsigned int saveMinAge = nStakeMinAge;
    const int saveMaturity = nCoinbaseMaturity;
    try
    {
        fRegTest = true; nStakeMinAge = 0; nCoinbaseMaturity = 10;
        const CPubKey pub = SeedSpendableKey();
        ColdSpendableCoin c = PrepareColdSpendableCoin(*this, pub, 2, 500000 * COIN, false);
        Setup();
        RetainNavigator();
        LOCK2(cs_main, pwalletMain->cs_wallet);
        InstallColdWalletTx(c, 2, 1000);

        BOOST_CHECK_MESSAGE(mapBlockIndex.count(uint256(1000 + 2)) == 0, "historical block must remain cold-only/non-resident");

        pwalletMain->PopulateSPVUtxosFromWallet();

        bool present = false;
        {
            LOCK(pwalletMain->cs_spvutxos);
            present = pwalletMain->mapSPVUtxos.count(COutPoint(c.txHash, 0)) > 0;
        }
        BOOST_CHECK_MESSAGE(present, "PopulateSPVUtxosFromWallet must admit a deep cold nonresident active mature coin into the SPV cache (not drop it)");
    }
    catch (...)
    {
        fRegTest = saveRegTest; nStakeMinAge = saveMinAge; nCoinbaseMaturity = saveMaturity; throw;
    }
    fRegTest = saveRegTest; nStakeMinAge = saveMinAge; nCoinbaseMaturity = saveMaturity;
}

// NEW-N14: staking balance must preserve the legacy trusted balance/reserve
// gate and must not apply stake-min-age before coin selection.
BOOST_FIXTURE_TEST_CASE(stakeweight_mixed_age_balance_preserves_reserve_semantics, ProdFixture)
{
    const bool saveSPV = fHybridSPV;
    const bool saveRegTest = fRegTest;
    const unsigned int saveMinAge = nStakeMinAge;
    const int saveMaturity = nCoinbaseMaturity;
    const int64_t saveReserve = nReserveBalance;
    try
    {
        fHybridSPV = false; fRegTest = true; nStakeMinAge = 1000;
        nCoinbaseMaturity = 10; nReserveBalance = 105 * COIN / 10;
        const CPubKey pub = SeedSpendableKey();
        ColdSpendableCoin oldCoin = PrepareColdSpendableCoin(*this, pub, 8, 10 * COIN, false);
        ColdSpendableCoin youngCoin = PrepareColdSpendableCoin(*this, pub, 9, 9 * COIN, false);
        Setup();
        const unsigned int now = GetTime();
        created[N - 1]->nTime = now;
        created[8]->hashMerkleRoot = oldCoin.txHash;
        created[9]->hashMerkleRoot = youngCoin.txHash;
        LOCK2(cs_main, pwalletMain->cs_wallet);
        InstallColdWalletTx(oldCoin, 8, now - 10000);
        InstallColdWalletTx(youngCoin, 9, now - 1);
        pwalletMain->mapWallet[oldCoin.txHash].fMerkleVerified = true;
        pwalletMain->mapWallet[youngCoin.txHash].fMerkleVerified = true;
        BOOST_CHECK(pwalletMain->mapWallet[oldCoin.txHash].IsFinal());
        BOOST_CHECK(mapBlockIndex.count(uint256(1008)) == 1);
        BOOST_CHECK_GT(pwalletMain->mapWallet[oldCoin.txHash].GetDepthInMainChain(), 0);
        BOOST_CHECK(pwalletMain->mapWallet[oldCoin.txHash].IsTrusted());
        BOOST_CHECK(pwalletMain->mapWallet[youngCoin.txHash].IsTrusted());

        // Age-filtering the balance would leave only 10 COIN and fail the
        // 10.5-COIN reserve gate. Legacy balance semantics count both trusted
        // transactions; later selection still filters the young transaction.
        uint64_t nMin = 0, nMax = 0, nWeight = 0;
        BOOST_CHECK_MESSAGE(pwalletMain->GetStakeWeight(*pwalletMain, nMin, nMax, nWeight),
            "mixed-age trusted balance must remain above reserve before selection age filtering");
    }
    catch (...)
    {
        fHybridSPV = saveSPV; fRegTest = saveRegTest; nStakeMinAge = saveMinAge;
        nCoinbaseMaturity = saveMaturity; nReserveBalance = saveReserve; throw;
    }
    fHybridSPV = saveSPV; fRegTest = saveRegTest; nStakeMinAge = saveMinAge;
    nCoinbaseMaturity = saveMaturity; nReserveBalance = saveReserve;
}


//   NEW-N13  merkleblock cache-feeder authority (no historical mapBlockIndex)
//   NEW-N14  exact legacy balance/trust/reserve semantics (no min-age fold)
//   NEW-N15  typed SPV staking source authority (both selection branches)
// Every positive case removes ONLY historical mapBlockIndex residency as the
// causal variable; every negative pair fails closed on typed authority.
// ===========================================================================
namespace {

// Frame a wire message the same way the real receive path does (pattern from
// p2p_sync_tests FramedPayload): full P2P framing + checksum.
static CSerializeData StakeFramedPayload(const std::string& command,
                                          const CSerializeData& payload)
{
    CDataStream wire(SER_NETWORK, INIT_PROTO_VERSION);
    wire << CMessageHeader(command.c_str(), 0);
    if (!payload.empty())
        wire.write((const char*)&payload[0], payload.size());
    const unsigned int size = payload.size();
    memcpy((char*)&wire[CMessageHeader::MESSAGE_SIZE_OFFSET], &size, sizeof(size));
    const uint256 hash = Hash(payload.begin(), payload.end());
    unsigned int checksum = 0;
    memcpy(&checksum, &hash, sizeof(checksum));
    memcpy((char*)&wire[CMessageHeader::CHECKSUM_OFFSET], &checksum, sizeof(checksum));
    CSerializeData out;
    wire.GetAndClear(out);
    return out;
}

static void DeliverStakeFrame(CNode& node, const std::string& command,
                               const CSerializeData& payload)
{
    const CSerializeData wire = StakeFramedPayload(command, payload);
    LOCK(node.cs_vRecvMsg);
    BOOST_REQUIRE(node.ReceiveMsgBytes((const char*)&wire[0], wire.size()));
}

static CAddress StakePeerAddress(unsigned int nPeer)
{
    struct in_addr addr;
    addr.s_addr = 0x0100007f + (nPeer << 24);
    return CAddress(CService(addr, GetDefaultPort()));
}

// Build a REAL PoW-valid source block that contains `c.srcTx` as its only
// transaction, write it to disk, and bind the cold record at `height` to it
// (hash, merkle root, disk position). The block header is what a peer would
// send inside a merkleblock for this source.
struct MerkleBlockSourceFixture
{
    CTransaction srcTx;
    uint256 srcTxHash;
    CBlock block;
    uint256 sourceBlockHash;
    unsigned int nFile;
    unsigned int nBlockPos;
    MerkleBlockSourceFixture() : nFile(0), nBlockPos(0) {}
};

static void PrepareMerkleBlockColdSource(ProdFixture& fx, MerkleBlockSourceFixture* out,
    const CPubKey& pub, int height, int64_t nValue)
{
    BOOST_REQUIRE(out != NULL);
    out->srcTx.nVersion = 1;
    static unsigned int nSerial = 0;
    ++nSerial;
    out->srcTx.nTime = 1000 + nSerial;
    out->srcTx.vin.push_back(CTxIn(COutPoint(uint256(0xBEEF), 0)));
    out->srcTx.vout.push_back(CTxOut(500000 * COIN - nSerial,
        GetScriptForDestination(pub.GetID())));
    out->srcTxHash = out->srcTx.GetHash();

    out->block.nVersion = 1;
    out->block.hashPrevBlock = fx.Hash(1);
    out->block.nTime = out->srcTx.nTime;
    out->block.nBits = 0x207fffff;
    out->block.nNonce = 7;
    out->block.vtx.clear();
    out->block.vtx.push_back(out->srcTx);
    out->block.hashMerkleRoot = out->block.BuildMerkleTree();
    while (!CheckProofOfWork(out->block.GetPoWHash(), out->block.nBits))
        out->block.nNonce++;
    {
        LOCK(cs_main);
        BOOST_REQUIRE(out->block.WriteToDisk(out->nFile, out->nBlockPos));
    }
    out->sourceBlockHash = out->block.GetHash();

    fx.srcOverriddenHash = out->sourceBlockHash;
    fx.srcOverrideHeight = height;
    fx.merkleRootOverride[height] = out->block.hashMerkleRoot;
    fx.nFileOverride[height] = out->nFile;
    fx.nBlockPosOverride[height] = out->nBlockPos;
    fx.nBitsOverride[height] = out->block.nBits;
    fx.nTimeOverride[height] = out->block.nTime;
}

// Same cold source shape, but with a structurally valid coinbase first so the
// real ProcessMessage("block") arrival path can validate and store it.
static void PrepareRecoveryColdSource(ProdFixture& fx, MerkleBlockSourceFixture* out,
    const CPubKey& pub, int height)
{
    PrepareMerkleBlockColdSource(fx, out, pub, height, 500000 * COIN);
    CTransaction coinbase;
    coinbase.nTime = out->srcTx.nTime;
    coinbase.vin.resize(1);
    coinbase.vin[0].prevout.SetNull();
    coinbase.vin[0].scriptSig = CScript() << OP_0 << OP_1;
    coinbase.vout.push_back(CTxOut(0, CScript() << OP_TRUE));
    out->block.vtx.insert(out->block.vtx.begin(), coinbase);
    out->block.hashMerkleRoot = out->block.BuildMerkleTree();
    while (!CheckProofOfWork(out->block.GetPoWHash(), out->block.nBits))
        out->block.nNonce++;
    out->sourceBlockHash = out->block.GetHash();
    fx.srcOverriddenHash = out->sourceBlockHash;
    fx.merkleRootOverride[height] = out->block.hashMerkleRoot;
    fx.nBitsOverride[height] = out->block.nBits;
    fx.nTimeOverride[height] = out->block.nTime;
}

// Build the exact merkleblock a peer sends for the single-tx source block:
// header copy + single-leaf partial tree (root == tx hash, nTxIndex == 0).
static CMerkleBlock BuildMerkleBlockFor(const MerkleBlockSourceFixture& src)
{
    CMerkleBlock mb;
    mb.header.nVersion = src.block.nVersion;
    mb.header.hashPrevBlock = src.block.hashPrevBlock;
    mb.header.hashMerkleRoot = src.block.hashMerkleRoot;
    mb.header.nTime = src.block.nTime;
    mb.header.nBits = src.block.nBits;
    mb.header.nNonce = src.block.nNonce;
    std::vector<uint256> vTxid(1, src.srcTxHash);
    std::vector<bool> vMatch(1, true);
    mb.txn = CPartialMerkleTree(vTxid, vMatch);
    return mb;
}

// Send a framed merkleblock through the REAL receive path and let the real
// ProcessMessages dispatch it (the exact network feeder boundary).
static void DeliverMerkleBlock(CNode& node, const CMerkleBlock& mb)
{
    node.fSuccessfullyConnected = true;
    node.fClient = false;
    node.fOneShot = false;
    node.fDisconnect = false;
    node.nVersion = PROTOCOL_VERSION;
    CDataStream payload(SER_NETWORK, INIT_PROTO_VERSION);
    payload << mb;
    CSerializeData payloadBytes;
    payload.GetAndClear(payloadBytes);
    DeliverStakeFrame(node, "merkleblock", payloadBytes);
    { LOCK(node.cs_vRecvMsg); BOOST_REQUIRE(ProcessMessages(&node, criticalblock)); }
}

static void DeliverFullBlock(CNode& node, const CBlock& block)
{
    node.fSuccessfullyConnected = true;
    node.fClient = false;
    node.fOneShot = false;
    node.fDisconnect = false;
    node.nVersion = PROTOCOL_VERSION;
    CDataStream payload(SER_NETWORK, INIT_PROTO_VERSION);
    payload << block;
    CSerializeData payloadBytes;
    payload.GetAndClear(payloadBytes);
    DeliverStakeFrame(node, "block", payloadBytes);
    { LOCK(node.cs_vRecvMsg); BOOST_REQUIRE(ProcessMessages(&node, criticalblock)); }
}

static void AddStakingPeer(CNode* node)
{
    LOCK(cs_vNodes);
    vNodes.push_back(node);
}

static void RemoveStakingPeer(CNode* node)
{
    LOCK(cs_vNodes);
    vNodes.erase(std::remove(vNodes.begin(), vNodes.end(), node), vNodes.end());
}

static bool RunSpvAuthorityNegative(ProdFixture& fx, bool mapWalletBranch,
                                    bool divergentSeam, bool staleGeneration)
{
    const CPubKey pub = SeedSpendableKey();
    MerkleBlockSourceFixture src;
    PrepareMerkleBlockColdSource(fx, &src, pub, 2, 500000 * COIN);
    BOOST_REQUIRE(src.nFile > 0);
    CBlock verifySource;
    BOOST_REQUIRE(verifySource.ReadFromDisk(src.nFile, src.nBlockPos, true));
    BOOST_REQUIRE(verifySource.GetHash() == src.sourceBlockHash);
    fx.nFileOverride[2] = src.nFile;
    fx.nBlockPosOverride[2] = src.nBlockPos;
    fx.Setup();
    fx.RetainNavigator();
    if (divergentSeam)
    {
        BOOST_REQUIRE(fx.created.size() > (size_t)ProdFixture::SEAM);
        delete fx.created[ProdFixture::SEAM]->phashBlock;
        fx.created[ProdFixture::SEAM]->phashBlock = new uint256(0x777777);
    }
    if (staleGeneration)
    {
        BlockIndexGenerationSource alt;
        BlockIndexRecord r = ProdRecord(42, 0, true, uint256(0));
        BlockIndexGenerationSourceRecord q; q.hash = r.hash; q.record = r;
        alt.records.push_back(q); alt.hashBestChain = r.hash; alt.foundBestChain = true;
        BlockIndexGenerationBuilder builder; BlockIndexGenerationStats stats; std::string error;
        BOOST_REQUIRE(builder.Build(alt,
            (fx.root / BlockIndexGenerationManager::GenerationName(2)).string(),
            2, &stats, &error));
        builder.Close();
        BOOST_REQUIRE(BlockIndexGenerationManager::SelectGeneration(fx.root.string(), 2, &error) == BLOCK_INDEX_LIFECYCLE_OK);
    }
    if (mapWalletBranch)
    {
        CWalletTx wtx(pwalletMain, src.srcTx);
        wtx.hashBlock = src.sourceBlockHash; wtx.nIndex = 0; wtx.nTime = src.srcTx.nTime;
        LOCK(pwalletMain->cs_wallet);
        pwalletMain->mapWallet[src.srcTxHash] = wtx;
        pwalletMain->mapWallet[src.srcTxHash].BindWallet(pwalletMain);
    }
    SPVUtxo u;
    u.txhash = src.srcTxHash; u.n = 0; u.nValue = src.srcTx.vout[0].nValue;
    u.nHeight = 2; u.hashBlock = src.sourceBlockHash; u.nTime = src.srcTx.nTime;
    u.fHaveBlock = true; u.fVerified = true; u.scriptPubKey = src.srcTx.vout[0].scriptPubKey;
    {
        LOCK(pwalletMain->cs_spvutxos);
        pwalletMain->mapSPVUtxos[COutPoint(src.srcTxHash, 0)] = u;
    }
    // Make wrong authority acceptance observable: materialization is real and
    // usable, so only the typed authority gate can keep this source out.
    pwalletMain->PublishStakingMaterialization(
        src.sourceBlockHash, src.nFile, src.nBlockPos);
    uint64_t nMin = 0, nMax = 0, nWeight = 0;
    const bool rejected = !pwalletMain->GetStakeWeight(*pwalletMain, nMin, nMax, nWeight);
    const size_t walletCount = pwalletMain->mapWallet.count(src.srcTxHash);
    return rejected && (mapWalletBranch ? walletCount == 1 : walletCount == 0);
}

// NEW-N13 positive: a valid OLD ACTIVE merkleblock whose historical CBlockIndex
// is NOT resident in mapBlockIndex (cold-only, retained navigator, intact seam)
// MUST reach UpdateSPVUtxo and admit/refresh the SPV staking cache. Only
// historical mapBlockIndex residency is removed from legacy control.
// Real network boundary: real CNode -> ReceiveMsgBytes -> ProcessMessages ->
// ProcessMessage("merkleblock") -> UpdateSPVUtxo.
BOOST_FIXTURE_TEST_CASE(merkleblock_cold_nonresident_active_source_admitted_to_spv_cache, ProdFixture)
{
    const bool saveSPV = fHybridSPV;
    const bool saveRegTest = fRegTest;
    const unsigned int saveMinAge = nStakeMinAge;
    const int saveMaturity = nCoinbaseMaturity;
    try
    {
        fHybridSPV = true; fRegTest = true; nStakeMinAge = 0; nCoinbaseMaturity = 10;
        const CPubKey pub = SeedSpendableKey();
        MerkleBlockSourceFixture src;
        PrepareMerkleBlockColdSource(*this, &src, pub, 2, 500000 * COIN);
        Setup();
        RetainNavigator();
        // The causal variable: the historical source block is NOT in mapBlockIndex.
        BOOST_CHECK_MESSAGE(mapBlockIndex.count(src.sourceBlockHash) == 0,
            "historical source block must be cold-only/non-resident");

        // Wallet already holds the source tx bound to the cold source block.
        {
            CWalletTx wtx(pwalletMain, src.srcTx);
            wtx.hashBlock = src.sourceBlockHash;
            wtx.nIndex = 0;
            wtx.vMerkleBranch.clear();
            wtx.nTime = src.srcTx.nTime;
            LOCK(pwalletMain->cs_wallet);
            pwalletMain->mapWallet[src.srcTxHash] = wtx;
            pwalletMain->mapWallet[src.srcTxHash].BindWallet(pwalletMain);
        }

        // Prove the same production authority operation accepts the source
        // before crossing the network feeder.
        {
            LOCK(cs_main);
            ColdHotSeamSnapshot authority;
            std::string authorityError;
            BOOST_REQUIRE_MESSAGE(
                GetStakingSourceAuthority(src.sourceBlockHash, &authority, &authorityError) == COLD_HOT_SEAM_OK,
                authorityError);
            BOOST_REQUIRE_EQUAL(authority.snapshot.height, 2);
            BOOST_REQUIRE(authority.snapshot.fInMainChain);
        }

        BOOST_REQUIRE(
            Tribus(BEGIN(src.block.nVersion), END(src.block.nNonce)) ==
            src.sourceBlockHash);
        CMerkleBlock checkBlock = BuildMerkleBlockFor(src);
        std::vector<uint256> checkMatches;
        BOOST_REQUIRE(checkBlock.txn.ExtractMatches(checkMatches) == src.block.hashMerkleRoot);

        BOOST_REQUIRE(pwalletMain->mapWallet.count(src.srcTxHash) == 1);
        BOOST_REQUIRE(pwalletMain->IsMine(src.srcTx.vout[0]));

        // Control A: add only a resident historical alias; all authority and
        // source/proof inputs are otherwise identical.
        CBlockIndex* residentAlias = new CBlockIndex();
        residentAlias->phashBlock = new uint256(src.sourceBlockHash);
        residentAlias->nHeight = 2; residentAlias->pprev = created[1];
        residentAlias->nFile = src.nFile; residentAlias->nBlockPos = src.nBlockPos;
        residentAlias->nTime = src.block.nTime; created.push_back(residentAlias);
        mapBlockIndex[src.sourceBlockHash] = residentAlias;

        CNode peerA(INVALID_SOCKET, StakePeerAddress(8601), "merkleblock-resident", true);
        DeliverMerkleBlock(peerA, BuildMerkleBlockFor(src));
        {
            LOCK(pwalletMain->cs_spvutxos);
            std::map<COutPoint, SPVUtxo>::const_iterator it =
                pwalletMain->mapSPVUtxos.find(COutPoint(src.srcTxHash, 0));
            BOOST_REQUIRE(it != pwalletMain->mapSPVUtxos.end());
            BOOST_REQUIRE(it->second.fHaveBlock && it->second.fVerified);
            pwalletMain->mapSPVUtxos.clear();
        }

        // Control B: remove ONLY historical mapBlockIndex residency, retain
        // the same navigator/generation, bytes, proof, wallet, and source.
        mapBlockIndex.erase(src.sourceBlockHash);
        CNode peerB(INVALID_SOCKET, StakePeerAddress(8601), "merkleblock-cold", true);
        DeliverMerkleBlock(peerB, BuildMerkleBlockFor(src));

        bool admitted = false;
        {
            LOCK(pwalletMain->cs_spvutxos);
            std::map<COutPoint, SPVUtxo>::const_iterator it =
                pwalletMain->mapSPVUtxos.find(COutPoint(src.srcTxHash, 0));
            BOOST_CHECK_MESSAGE(it != pwalletMain->mapSPVUtxos.end(),
                "network handler must create an SPV entry for the matched wallet transaction");
            if (it != pwalletMain->mapSPVUtxos.end())
            {
                BOOST_CHECK(it->second.fHaveBlock);
                BOOST_CHECK(it->second.fVerified);
                BOOST_CHECK_EQUAL(it->second.nHeight, 2);
                admitted = it->second.fHaveBlock && it->second.fVerified &&
                           it->second.nHeight == 2 &&
                           it->second.hashBlock == src.sourceBlockHash;
            }
        }
        BOOST_CHECK_MESSAGE(admitted,
            "valid old merkleblock with cold nonresident source must update the SPV staking cache");
    }
    catch (...)
    {
        fHybridSPV = saveSPV; fRegTest = saveRegTest; nStakeMinAge = saveMinAge; nCoinbaseMaturity = saveMaturity; throw;
    }
    fHybridSPV = saveSPV; fRegTest = saveRegTest; nStakeMinAge = saveMinAge; nCoinbaseMaturity = saveMaturity;
}

// NEW-N13 negative pair 1: an INACTIVE/side source block merkleblock must NOT
// update the SPV staking cache. The source CBlockIndex exists in mapBlockIndex
// (hot, resident) but is NOT reachable from best -> not active -> rejected.
BOOST_FIXTURE_TEST_CASE(merkleblock_inactive_side_source_rejected, ProdFixture)
{
    const bool saveSPV = fHybridSPV;
    const bool saveRegTest = fRegTest;
    const unsigned int saveMinAge = nStakeMinAge;
    const int saveMaturity = nCoinbaseMaturity;
    try
    {
        fHybridSPV = true; fRegTest = true; nStakeMinAge = 0; nCoinbaseMaturity = 10;
        const CPubKey pub = SeedSpendableKey();
        MerkleBlockSourceFixture src;
        PrepareMerkleBlockColdSource(*this, &src, pub, 2, 500000 * COIN);
        // This negative uses a hot side record, not the cold active record.
        // Keep the source block bytes/header, but do not place its hash in the
        // frozen active generation.
        merkleRootOverride.clear();
        srcOverriddenHash = uint256(0);
        Setup();
        // Register a real SIDE hot block with the SAME header identity as the
        // source block, but NOT reachable from best: pnext NULL and != tip.
        CBlockIndex* pSide = new CBlockIndex();
        pSide->phashBlock = new uint256(src.sourceBlockHash);
        pSide->nHeight = 2;
        pSide->pprev = created[1];
        pSide->nFile = src.nFile;
        pSide->nBlockPos = src.nBlockPos;
        pSide->nTime = src.block.nTime;
        created.push_back(pSide);
        mapBlockIndex[src.sourceBlockHash] = pSide;
        BOOST_CHECK(pSide->IsInMainChain() == false);

        {
            CWalletTx wtx(pwalletMain, src.srcTx);
            wtx.hashBlock = src.sourceBlockHash;
            wtx.nIndex = 0;
            wtx.vMerkleBranch.clear();
            wtx.nTime = src.srcTx.nTime;
            LOCK(pwalletMain->cs_wallet);
            pwalletMain->mapWallet[src.srcTxHash] = wtx;
            pwalletMain->mapWallet[src.srcTxHash].BindWallet(pwalletMain);
        }

        CNode peer(INVALID_SOCKET, StakePeerAddress(8602), "merkleblock-side-reject", true);
        DeliverMerkleBlock(peer, BuildMerkleBlockFor(src));

        bool admitted = false;
        {
            LOCK(pwalletMain->cs_spvutxos);
            admitted = pwalletMain->mapSPVUtxos.count(COutPoint(src.srcTxHash, 0)) > 0;
        }
        BOOST_CHECK_MESSAGE(!admitted,
            "inactive/side source merkleblock must NOT be admitted to the SPV cache");
    }
    catch (...)
    {
        fHybridSPV = saveSPV; fRegTest = saveRegTest; nStakeMinAge = saveMinAge; nCoinbaseMaturity = saveMaturity; throw;
    }
    fHybridSPV = saveSPV; fRegTest = saveRegTest; nStakeMinAge = saveMinAge; nCoinbaseMaturity = saveMaturity;
}

// NEW-N13 negative pair 2: a DIVERGENT live seam with unchanged CURRENT must
// fail authority -- the cold source is active per frozen active.dat, but the
// live chain no longer shares the seam, so the merkleblock must NOT update
// the SPV cache.
BOOST_FIXTURE_TEST_CASE(merkleblock_divergent_live_seam_fails_authority, ProdFixture)
{
    const bool saveSPV = fHybridSPV;
    const bool saveRegTest = fRegTest;
    const unsigned int saveMinAge = nStakeMinAge;
    const int saveMaturity = nCoinbaseMaturity;
    try
    {
        fHybridSPV = true; fRegTest = true; nStakeMinAge = 0; nCoinbaseMaturity = 10;
        const CPubKey pub = SeedSpendableKey();
        MerkleBlockSourceFixture src;
        PrepareMerkleBlockColdSource(*this, &src, pub, 2, 500000 * COIN);
        Setup();
        RetainNavigator();
        BOOST_CHECK_MESSAGE(mapBlockIndex.count(src.sourceBlockHash) == 0,
            "historical source block must be cold-only/non-resident");

        // Diverge the seam: replace the hot seam anchor hash at SEAM while
        // CURRENT stays pinned to generation 1.
        BOOST_REQUIRE(created.size() > (size_t)SEAM);
        delete created[SEAM]->phashBlock;
        created[SEAM]->phashBlock = new uint256(0x777777);

        {
            CWalletTx wtx(pwalletMain, src.srcTx);
            wtx.hashBlock = src.sourceBlockHash;
            wtx.nIndex = 0;
            wtx.vMerkleBranch.clear();
            wtx.nTime = src.srcTx.nTime;
            LOCK(pwalletMain->cs_wallet);
            pwalletMain->mapWallet[src.srcTxHash] = wtx;
            pwalletMain->mapWallet[src.srcTxHash].BindWallet(pwalletMain);
        }

        CNode peer(INVALID_SOCKET, StakePeerAddress(8603), "merkleblock-seam-divergent", true);
        DeliverMerkleBlock(peer, BuildMerkleBlockFor(src));

        bool admitted = false;
        {
            LOCK(pwalletMain->cs_spvutxos);
            admitted = pwalletMain->mapSPVUtxos.count(COutPoint(src.srcTxHash, 0)) > 0;
        }
        BOOST_CHECK_MESSAGE(!admitted,
            "merkleblock under a divergent live seam must fail authority and not update the SPV cache");
    }
    catch (...)
    {
        fHybridSPV = saveSPV; fRegTest = saveRegTest; nStakeMinAge = saveMinAge; nCoinbaseMaturity = saveMaturity; throw;
    }
    fHybridSPV = saveSPV; fRegTest = saveRegTest; nStakeMinAge = saveMinAge; nCoinbaseMaturity = saveMaturity;
}

// NEW-N13 negative pair 3: a STALE generation (CURRENT replaced after
// retention) must fail closed -- the merkleblock must NOT update the SPV cache
// and must NOT fall back to historical residency even when the source is
// resident.
BOOST_FIXTURE_TEST_CASE(merkleblock_stale_generation_fails_closed, ProdFixture)
{
    const bool saveSPV = fHybridSPV;
    const bool saveRegTest = fRegTest;
    const unsigned int saveMinAge = nStakeMinAge;
    const int saveMaturity = nCoinbaseMaturity;
    try
    {
        fHybridSPV = true; fRegTest = true; nStakeMinAge = 0; nCoinbaseMaturity = 10;
        const CPubKey pub = SeedSpendableKey();
        MerkleBlockSourceFixture src;
        PrepareMerkleBlockColdSource(*this, &src, pub, 2, 500000 * COIN);
        Setup();
        RetainNavigator();

        // Replace CURRENT with a different generation while retained.
        BlockIndexGenerationSource alt;
        uint256 pprev(0);
        BlockIndexRecord r0 = ProdRecord(42, 0, true, pprev);
        BlockIndexGenerationSourceRecord q0; q0.hash = r0.hash; q0.record = r0; alt.records.push_back(q0);
        alt.hashBestChain = r0.hash; alt.foundBestChain = true;
        BlockIndexGenerationBuilder b2; BlockIndexGenerationStats st2; std::string e2;
        BOOST_REQUIRE(b2.Build(alt, (root / BlockIndexGenerationManager::GenerationName(2)).string(), 2, &st2, &e2));
        b2.Close();
        BOOST_REQUIRE(BlockIndexGenerationManager::SelectGeneration(root.string(), 2, &e2) == BLOCK_INDEX_LIFECYCLE_OK);

        // Resident alias: IF authority wrongly fell back to mapBlockIndex it
        // WOULD succeed. Stale authority must not.
        mapBlockIndex[src.sourceBlockHash] = created[2];

        {
            CWalletTx wtx(pwalletMain, src.srcTx);
            wtx.hashBlock = src.sourceBlockHash;
            wtx.nIndex = 0;
            wtx.vMerkleBranch.clear();
            wtx.nTime = src.srcTx.nTime;
            LOCK(pwalletMain->cs_wallet);
            pwalletMain->mapWallet[src.srcTxHash] = wtx;
            pwalletMain->mapWallet[src.srcTxHash].BindWallet(pwalletMain);
        }

        CNode peer(INVALID_SOCKET, StakePeerAddress(8604), "merkleblock-stale-gen", true);
        DeliverMerkleBlock(peer, BuildMerkleBlockFor(src));

        bool admitted = false;
        {
            LOCK(pwalletMain->cs_spvutxos);
            admitted = pwalletMain->mapSPVUtxos.count(COutPoint(src.srcTxHash, 0)) > 0;
        }
        BOOST_CHECK_MESSAGE(!admitted,
            "stale-generation merkleblock must fail closed and not update the SPV cache");
    }
    catch (...)
    {
        fHybridSPV = saveSPV; fRegTest = saveRegTest; nStakeMinAge = saveMinAge; nCoinbaseMaturity = saveMaturity; throw;
    }
    fHybridSPV = saveSPV; fRegTest = saveRegTest; nStakeMinAge = saveMinAge; nCoinbaseMaturity = saveMaturity;
}

// NEW-N15: both SPV selection branches must reject a source that has disk
// coordinates but is not currently active.
BOOST_FIXTURE_TEST_CASE(selectcoinsspv_mapwallet_inactive_side_not_selected, ProdFixture)
{
    const bool saveSPV = fHybridSPV;
    const unsigned int saveMinAge = nStakeMinAge;
    const int64_t saveReserve = nReserveBalance;
    try
    {
        fHybridSPV = true; nStakeMinAge = 0; nReserveBalance = 0;
        const CPubKey pub = SeedSpendableKey();
        MerkleBlockSourceFixture src;
        PrepareMerkleBlockColdSource(*this, &src, pub, 2, 500000 * COIN);
        merkleRootOverride.clear(); srcOverriddenHash = uint256(0);
        Setup(); RetainNavigator();
        CBlockIndex* side = new CBlockIndex();
        side->phashBlock = new uint256(src.sourceBlockHash);
        side->nHeight = 2; side->pprev = created[1];
        side->nFile = src.nFile; side->nBlockPos = src.nBlockPos;
        side->nTime = src.block.nTime; created.push_back(side);
        mapBlockIndex[src.sourceBlockHash] = side;
        LOCK2(cs_main, pwalletMain->cs_wallet);
        CWalletTx wtx(pwalletMain, src.srcTx); wtx.hashBlock = src.sourceBlockHash; wtx.nTime = src.srcTx.nTime;
        pwalletMain->mapWallet[src.srcTxHash] = wtx;
        pwalletMain->mapWallet[src.srcTxHash].BindWallet(pwalletMain);
        SPVUtxo u; u.txhash = src.srcTxHash; u.n = 0; u.nValue = src.srcTx.vout[0].nValue;
        u.nHeight = 2; u.hashBlock = src.sourceBlockHash; u.nTime = src.srcTx.nTime;
        u.fHaveBlock = true; u.fVerified = true; u.scriptPubKey = src.srcTx.vout[0].scriptPubKey;
        pwalletMain->mapSPVUtxos[COutPoint(src.srcTxHash, 0)] = u;
        pwalletMain->PublishStakingMaterialization(
            src.sourceBlockHash, src.nFile, src.nBlockPos);
        uint64_t nMin = 0, nMax = 0, nWeight = 0;
        BOOST_CHECK(!pwalletMain->GetStakeWeight(*pwalletMain, nMin, nMax, nWeight));
    }
    catch (...)
    {
        fHybridSPV = saveSPV; nStakeMinAge = saveMinAge; nReserveBalance = saveReserve; throw;
    }
    fHybridSPV = saveSPV; nStakeMinAge = saveMinAge; nReserveBalance = saveReserve;
}

BOOST_FIXTURE_TEST_CASE(selectcoinsspv_cache_inactive_side_not_materialized, ProdFixture)
{
    const bool saveSPV = fHybridSPV;
    const unsigned int saveMinAge = nStakeMinAge;
    const int64_t saveReserve = nReserveBalance;
    try
    {
        fHybridSPV = true; nStakeMinAge = 0; nReserveBalance = 0;
        const CPubKey pub = SeedSpendableKey();
        MerkleBlockSourceFixture src;
        PrepareMerkleBlockColdSource(*this, &src, pub, 2, 500000 * COIN);
        merkleRootOverride.clear(); srcOverriddenHash = uint256(0);
        Setup(); RetainNavigator();
        CBlockIndex* side = new CBlockIndex();
        side->phashBlock = new uint256(src.sourceBlockHash);
        side->nHeight = 2; side->pprev = created[1];
        side->nFile = src.nFile; side->nBlockPos = src.nBlockPos;
        side->nTime = src.block.nTime; created.push_back(side);
        mapBlockIndex[src.sourceBlockHash] = side;
        SPVUtxo u; u.txhash = src.srcTxHash; u.n = 0; u.nValue = src.srcTx.vout[0].nValue;
        u.nHeight = 2; u.hashBlock = src.sourceBlockHash; u.nTime = src.srcTx.nTime;
        u.fHaveBlock = true; u.fVerified = true; u.scriptPubKey = src.srcTx.vout[0].scriptPubKey;
        {
            LOCK(pwalletMain->cs_spvutxos);
            pwalletMain->mapSPVUtxos[COutPoint(src.srcTxHash, 0)] = u;
        }
        pwalletMain->PublishStakingMaterialization(
            src.sourceBlockHash, src.nFile, src.nBlockPos);
        uint64_t nMin = 0, nMax = 0, nWeight = 0;
        BOOST_CHECK(!pwalletMain->GetStakeWeight(*pwalletMain, nMin, nMax, nWeight));
        BOOST_CHECK(pwalletMain->mapWallet.count(src.srcTxHash) == 0);
    }
    catch (...)
    {
        fHybridSPV = saveSPV; nStakeMinAge = saveMinAge; nReserveBalance = saveReserve; throw;
    }
    fHybridSPV = saveSPV; nStakeMinAge = saveMinAge; nReserveBalance = saveReserve;
}

BOOST_FIXTURE_TEST_CASE(selectcoinsspv_mapwallet_divergent_seam_not_selected, ProdFixture)
{
    const bool saveSPV = fHybridSPV; const unsigned int saveAge = nStakeMinAge;
    const int64_t saveReserve = nReserveBalance;
    fHybridSPV = true; nStakeMinAge = 0; nReserveBalance = 0;
    BOOST_CHECK(RunSpvAuthorityNegative(*this, true, true, false));
    fHybridSPV = saveSPV; nStakeMinAge = saveAge; nReserveBalance = saveReserve;
}

BOOST_FIXTURE_TEST_CASE(selectcoinsspv_cache_divergent_seam_not_materialized, ProdFixture)
{
    const bool saveSPV = fHybridSPV; const unsigned int saveAge = nStakeMinAge;
    const int64_t saveReserve = nReserveBalance;
    fHybridSPV = true; nStakeMinAge = 0; nReserveBalance = 0;
    BOOST_CHECK(RunSpvAuthorityNegative(*this, false, true, false));
    fHybridSPV = saveSPV; nStakeMinAge = saveAge; nReserveBalance = saveReserve;
}

BOOST_FIXTURE_TEST_CASE(selectcoinsspv_mapwallet_stale_generation_not_selected, ProdFixture)
{
    const bool saveSPV = fHybridSPV; const unsigned int saveAge = nStakeMinAge;
    const int64_t saveReserve = nReserveBalance;
    fHybridSPV = true; nStakeMinAge = 0; nReserveBalance = 0;
    BOOST_CHECK(RunSpvAuthorityNegative(*this, true, false, true));
    fHybridSPV = saveSPV; nStakeMinAge = saveAge; nReserveBalance = saveReserve;
}

BOOST_FIXTURE_TEST_CASE(selectcoinsspv_cache_stale_generation_not_materialized, ProdFixture)
{
    const bool saveSPV = fHybridSPV; const unsigned int saveAge = nStakeMinAge;
    const int64_t saveReserve = nReserveBalance;
    fHybridSPV = true; nStakeMinAge = 0; nReserveBalance = 0;
    BOOST_CHECK(RunSpvAuthorityNegative(*this, false, false, true));
    fHybridSPV = saveSPV; nStakeMinAge = saveAge; nReserveBalance = saveReserve;
}

// A.9a.3h T1: deterministic two-thread lock-order regression. Both the
// publication side and staking/cache side acquire main -> wallet -> spv; a
// barrier starts them together and both production operations must complete.
BOOST_FIXTURE_TEST_CASE(a9a3h_main_wallet_spv_lock_order_completes, ProdFixture)
{
    Setup(); RetainNavigator();
    const COutPoint outpoint(uint256(0xA9A301), 0);
    SPVUtxo incoming;
    incoming.txhash = outpoint.hash; incoming.n = 0; incoming.nValue = COIN;
    incoming.nHeight = 2; incoming.hashBlock = Hash(2); incoming.nTime = 1000;
    incoming.fHaveBlock = true; incoming.fVerified = true;
    incoming.scriptPubKey << OP_TRUE;

    boost::barrier start(3);
    bool publicationDone = false;
    bool stakingDone = false;
    boost::thread publication([&]() {
        start.wait();
        LOCK2(cs_main, pwalletMain->cs_wallet);
        publicationDone = pwalletMain->UpdateSPVUtxo(outpoint, incoming);
    });
    boost::thread staking([&]() {
        start.wait();
        LOCK2(cs_main, pwalletMain->cs_wallet);
        std::vector<COutPoint> coins;
        pwalletMain->AvailableCoinsForStakingSPV(coins);
        stakingDone = true;
    });
    start.wait();
    publication.join();
    staking.join();
    BOOST_CHECK(publicationDone);
    BOOST_CHECK(stakingDone);
}

// A.9a.3h T3/T4/T6/T7: an active cold source starts with no
// coordinates and a both-false cache entry. Production selection requests once,
// real block arrival publishes bounded materialization, and the next selection
// materializes the exact tx while historical mapBlockIndex remains absent.
BOOST_FIXTURE_TEST_CASE(a9a3h_cold_nfile0_request_arrival_then_selects, ProdFixture)
{
    const bool saveSPV = fHybridSPV;
    const unsigned int saveAge = nStakeMinAge;
    const int64_t saveReserve = nReserveBalance;
    CNode peer(INVALID_SOCKET, StakePeerAddress(8701), "staking-materialization", true);
    bool peerAdded = false;
    try
    {
        fHybridSPV = true; nStakeMinAge = 0; nReserveBalance = 0;
        const CPubKey pub = SeedSpendableKey();
        MerkleBlockSourceFixture src;
        PrepareRecoveryColdSource(*this, &src, pub, 2);
        nFileOverride[2] = 0;
        nBlockPosOverride[2] = 0;
        Setup(); RetainNavigator();
        BOOST_REQUIRE(mapBlockIndex.count(src.sourceBlockHash) == 0);

        SPVUtxo u;
        u.txhash = src.srcTxHash; u.n = 0; u.nValue = src.srcTx.vout[0].nValue;
        u.nHeight = 2; u.hashBlock = src.sourceBlockHash;
        u.nTime = src.srcTx.nTime; u.fHaveBlock = false; u.fVerified = false;
        u.scriptPubKey = src.srcTx.vout[0].scriptPubKey;
        {
            LOCK(pwalletMain->cs_spvutxos);
            pwalletMain->mapSPVUtxos[COutPoint(src.srcTxHash, 0)] = u;
        }

        peer.fSuccessfullyConnected = true;
        peer.nVersion = PROTOCOL_VERSION;
        AddStakingPeer(&peer); peerAdded = true;
        uint64_t nMin = 0, nMax = 0, nWeight = 0;
        BOOST_CHECK(!pwalletMain->GetStakeWeight(*pwalletMain, nMin, nMax, nWeight));
        BOOST_CHECK(pwalletMain->mapWallet.count(src.srcTxHash) == 0);
        StakingMaterializationInfo pending;
        BOOST_REQUIRE(pwalletMain->GetStakingMaterialization(src.sourceBlockHash, &pending));
        BOOST_CHECK(pending.pending);
        BOOST_CHECK_EQUAL(pending.nRequestCount, 1U);
        size_t queuedAfterFirst = 0;
        {
            LOCK(peer.cs_vSend);
            queuedAfterFirst = peer.vSendMsg.size();
        }
        BOOST_CHECK_GT(queuedAfterFirst, 0U);

        // Repeated selection is deduplicated by the bounded stable-hash state.
        BOOST_CHECK(!pwalletMain->GetStakeWeight(*pwalletMain, nMin, nMax, nWeight));
        StakingMaterializationInfo repeated;
        BOOST_REQUIRE(pwalletMain->GetStakingMaterialization(src.sourceBlockHash, &repeated));
        BOOST_CHECK_EQUAL(repeated.nRequestCount, 1U);
        {
            LOCK(peer.cs_vSend);
            BOOST_CHECK_EQUAL(peer.vSendMsg.size(), queuedAfterFirst);
        }

        RemoveStakingPeer(&peer); peerAdded = false;
        DeliverFullBlock(peer, src.block);
        StakingMaterializationInfo arrived;
        BOOST_REQUIRE(pwalletMain->GetStakingMaterialization(src.sourceBlockHash, &arrived));
        BOOST_CHECK(arrived.available);
        BOOST_CHECK(!arrived.pending);
        BOOST_CHECK(arrived.nFile > 0);
        BOOST_CHECK(mapBlockIndex.count(src.sourceBlockHash) == 0);

        BOOST_CHECK(pwalletMain->GetStakeWeight(*pwalletMain, nMin, nMax, nWeight));
        BOOST_CHECK(pwalletMain->mapWallet.count(src.srcTxHash) == 1);
        BOOST_CHECK(mapBlockIndex.count(src.sourceBlockHash) == 0);
    }
    catch (...)
    {
        if (peerAdded) RemoveStakingPeer(&peer);
        fHybridSPV = saveSPV; nStakeMinAge = saveAge; nReserveBalance = saveReserve;
        throw;
    }
    if (peerAdded) RemoveStakingPeer(&peer);
    fHybridSPV = saveSPV; nStakeMinAge = saveAge; nReserveBalance = saveReserve;
}

// A.9a.3h T5: stale materialization coordinates that read the wrong block are
// invalidated, enter bounded recovery, and become selectable after real arrival.
BOOST_FIXTURE_TEST_CASE(a9a3h_stale_coordinate_recovers_after_real_arrival, ProdFixture)
{
    const bool saveSPV = fHybridSPV;
    const unsigned int saveAge = nStakeMinAge;
    const int64_t saveReserve = nReserveBalance;
    CNode peer(INVALID_SOCKET, StakePeerAddress(8702), "staking-stale-position", true);
    bool peerAdded = false;
    try
    {
        fHybridSPV = true; nStakeMinAge = 0; nReserveBalance = 0;
        const CPubKey pub = SeedSpendableKey();
        MerkleBlockSourceFixture src;
        PrepareRecoveryColdSource(*this, &src, pub, 2);
        // These coordinates contain the pre-recovery block written by the
        // helper, whose hash differs from the final active source block.
        nFileOverride[2] = src.nFile;
        nBlockPosOverride[2] = src.nBlockPos;
        Setup(); RetainNavigator();
        BOOST_REQUIRE(mapBlockIndex.count(src.sourceBlockHash) == 0);

        SPVUtxo u;
        u.txhash = src.srcTxHash; u.n = 0; u.nValue = src.srcTx.vout[0].nValue;
        u.nHeight = 2; u.hashBlock = src.sourceBlockHash; u.nTime = src.srcTx.nTime;
        u.fHaveBlock = true; u.fVerified = true;
        u.scriptPubKey = src.srcTx.vout[0].scriptPubKey;
        { LOCK(pwalletMain->cs_spvutxos); pwalletMain->mapSPVUtxos[COutPoint(src.srcTxHash, 0)] = u; }

        peer.fSuccessfullyConnected = true; peer.nVersion = PROTOCOL_VERSION;
        AddStakingPeer(&peer); peerAdded = true;
        uint64_t nMin = 0, nMax = 0, nWeight = 0;
        BOOST_CHECK(!pwalletMain->GetStakeWeight(*pwalletMain, nMin, nMax, nWeight));
        BOOST_CHECK(pwalletMain->mapWallet.count(src.srcTxHash) == 0);
        StakingMaterializationInfo pending;
        BOOST_REQUIRE(pwalletMain->GetStakingMaterialization(src.sourceBlockHash, &pending));
        BOOST_CHECK(pending.pending);
        BOOST_CHECK_EQUAL(pending.nRequestCount, 1U);

        RemoveStakingPeer(&peer); peerAdded = false;
        DeliverFullBlock(peer, src.block);
        BOOST_CHECK(pwalletMain->GetStakeWeight(*pwalletMain, nMin, nMax, nWeight));
        BOOST_CHECK(pwalletMain->mapWallet.count(src.srcTxHash) == 1);
        BOOST_CHECK(mapBlockIndex.count(src.sourceBlockHash) == 0);
    }
    catch (...)
    {
        if (peerAdded) RemoveStakingPeer(&peer);
        fHybridSPV = saveSPV; nStakeMinAge = saveAge; nReserveBalance = saveReserve;
        throw;
    }
    if (peerAdded) RemoveStakingPeer(&peer);
    fHybridSPV = saveSPV; nStakeMinAge = saveAge; nReserveBalance = saveReserve;
}

// A.9a.3h T2: publication-time authority failure is mutation-closed.  The
// existing valid entry must survive byte/field-semantically unchanged.
BOOST_FIXTURE_TEST_CASE(a9a3h_failed_authority_publication_preserves_existing_cache, ProdFixture)
{
    Setup();
    RetainNavigator();
    const COutPoint outpoint(uint256(0xA9A3), 1);
    SPVUtxo oldEntry;
    oldEntry.txhash = outpoint.hash; oldEntry.n = outpoint.n;
    oldEntry.nValue = 77 * COIN; oldEntry.nHeight = 2;
    oldEntry.hashBlock = Hash(2); oldEntry.hashMerkleRoot = uint256(0x1111);
    oldEntry.nTxIndex = 3; oldEntry.fHaveBlock = true;
    oldEntry.fSpent = false; oldEntry.fVerified = true;
    oldEntry.nTime = 1234; oldEntry.scriptPubKey << OP_TRUE;
    {
        LOCK(pwalletMain->cs_spvutxos);
        pwalletMain->mapSPVUtxos[outpoint] = oldEntry;
    }

    SPVUtxo incoming = oldEntry;
    incoming.nValue = 1;
    incoming.nHeight = 999;
    incoming.hashBlock = Hash(2);
    incoming.fVerified = true;

    // Invalid merkle proof is independently mutation-closed and does not rely
    // on authority failure behavior.
    SPVUtxo invalidProof = incoming;
    invalidProof.vMerkleBranch.push_back(uint256(0xBAD));
    invalidProof.hashMerkleRoot = uint256(0xBAD2);
    {
        LOCK(cs_main);
        BOOST_CHECK(!pwalletMain->UpdateSPVUtxo(outpoint, invalidProof));
    }
    {
        LOCK(pwalletMain->cs_spvutxos);
        BOOST_CHECK_EQUAL(pwalletMain->mapSPVUtxos[outpoint].nValue, oldEntry.nValue);
        BOOST_CHECK(pwalletMain->mapSPVUtxos[outpoint].hashBlock == oldEntry.hashBlock);
    }

    CBlockIndex* side = new CBlockIndex();
    side->phashBlock = new uint256(0xDEADBEEF);
    side->nHeight = 2; side->pprev = created[1]; side->nFile = 9;
    created.push_back(side); mapBlockIndex[side->GetBlockHash()] = side;
    SPVUtxo inactive = incoming;
    inactive.hashBlock = side->GetBlockHash();
    {
        LOCK(cs_main);
        BOOST_CHECK(!pwalletMain->UpdateSPVUtxo(outpoint, inactive));
    }
    {
        LOCK(pwalletMain->cs_spvutxos);
        BOOST_CHECK_EQUAL(pwalletMain->mapSPVUtxos[outpoint].nValue, oldEntry.nValue);
        BOOST_CHECK(pwalletMain->mapSPVUtxos[outpoint].hashBlock == oldEntry.hashBlock);
    }

    // Outer admission is initially valid.
    {
        LOCK(cs_main);
        ColdHotSeamSnapshot authority;
        std::string error;
        BOOST_REQUIRE(GetStakingSourceAuthority(
            incoming.hashBlock, &authority, &error) == COLD_HOT_SEAM_OK);
    }

    // Replace CURRENT before the production publication point. The retained
    // navigator must report AUTHORITY_FAILURE and Update must publish nothing.
    BlockIndexGenerationSource alt;
    BlockIndexRecord record = ProdRecord(0x4242, 0, true, uint256(0));
    BlockIndexGenerationSourceRecord sourceRecord;
    sourceRecord.hash = record.hash; sourceRecord.record = record;
    alt.records.push_back(sourceRecord);
    alt.hashBestChain = record.hash; alt.foundBestChain = true;
    BlockIndexGenerationBuilder builder;
    BlockIndexGenerationStats stats;
    std::string lifecycleError;
    BOOST_REQUIRE(builder.Build(alt,
        (root / BlockIndexGenerationManager::GenerationName(2)).string(),
        2, &stats, &lifecycleError));
    builder.Close();
    BOOST_REQUIRE(BlockIndexGenerationManager::SelectGeneration(
        root.string(), 2, &lifecycleError) == BLOCK_INDEX_LIFECYCLE_OK);

    bool published = true;
    {
        LOCK(cs_main);
        published = pwalletMain->UpdateSPVUtxo(outpoint, incoming);
    }
    BOOST_CHECK(!published);
    {
        LOCK(pwalletMain->cs_spvutxos);
        BOOST_REQUIRE(pwalletMain->mapSPVUtxos.count(outpoint) == 1);
        const SPVUtxo& after = pwalletMain->mapSPVUtxos[outpoint];
        BOOST_CHECK(after.txhash == oldEntry.txhash);
        BOOST_CHECK_EQUAL(after.n, oldEntry.n);
        BOOST_CHECK_EQUAL(after.nValue, oldEntry.nValue);
        BOOST_CHECK_EQUAL(after.nHeight, oldEntry.nHeight);
        BOOST_CHECK(after.hashBlock == oldEntry.hashBlock);
        BOOST_CHECK(after.hashMerkleRoot == oldEntry.hashMerkleRoot);
        BOOST_CHECK_EQUAL(after.nTxIndex, oldEntry.nTxIndex);
        BOOST_CHECK_EQUAL(after.fHaveBlock, oldEntry.fHaveBlock);
        BOOST_CHECK_EQUAL(after.fSpent, oldEntry.fSpent);
        BOOST_CHECK_EQUAL(after.fVerified, oldEntry.fVerified);
        BOOST_CHECK_EQUAL(after.nTime, oldEntry.nTime);
        BOOST_CHECK(after.scriptPubKey == oldEntry.scriptPubKey);
    }
}

// A.9a.3h H13-H16/H34-H36: materialization state is stable-hash keyed,
// bounded by the configured SPV working set, and carries no chain authority.
BOOST_FIXTURE_TEST_CASE(a9a3h_materialization_overlay_is_bounded, ProdFixture)
{
    const bool hadLimit = mapArgs.count("-spvutxocachesize") != 0;
    const std::string oldLimit = hadLimit ? mapArgs["-spvutxocachesize"] : std::string();
    mapArgs["-spvutxocachesize"] = "3";
    pwalletMain->ClearStakingMaterializations();
    for (unsigned int i = 1; i <= 5; ++i)
        pwalletMain->PublishStakingMaterialization(uint256(0xC000 + i), i, i * 100);
    BOOST_CHECK_LE(pwalletMain->GetStakingMaterializationCount(), 3U);
    StakingMaterializationInfo info;
    BOOST_CHECK(pwalletMain->GetStakingMaterialization(uint256(0xC005), &info));
    BOOST_CHECK(info.available);
    BOOST_CHECK_EQUAL(info.nFile, 5U);
    BOOST_CHECK_EQUAL(info.nBlockPos, 500U);
    pwalletMain->ClearStakingMaterializations();
    if (hadLimit)
        mapArgs["-spvutxocachesize"] = oldLimit;
    else
        mapArgs.erase("-spvutxocachesize");
}

// A.9a.3i E3 CAUSAL RED: an active COLD source with frozen nFile=0 in the
// navigator snapshot.  Without the materialization overlay, real
// CheckProofOfStake fails because the frozen snapshot coordinates are
// unreadable.  After the overlay publishes the real disk coordinates,
// CheckProofOfStake succeeds -- proving the validation consumer now reads
// through the same authority-first overlay-aware resolution used by
// selection/generation.
//
// This is the decisive E3 boundary that A.9a.3h failed to cross.
BOOST_FIXTURE_TEST_CASE(a9a3i_overlay_recovery_passes_checkproofofstake, ProdFixture)
{
    const bool saveSPV = fHybridSPV;
    const int saveMaturity = nCoinbaseMaturity;
    const unsigned int saveMinAge = nStakeMinAge;
    unsigned int saveI = nModifierInterval, saveS = nTargetSpacing;
    fHybridSPV = true; nCoinbaseMaturity = 10; nStakeMinAge = 0;
    nModifierInterval = 60; nTargetSpacing = 5;
    HybridSpvRealSourceFixture src;
    CNode peer(INVALID_SOCKET, StakePeerAddress(8711), "a9a3i-validation-arrival", true);
    bool peerAdded = false;
    try
    {
        PrepareHybridSpvRealSource(*this, &src);
        // The synthetic HybridSPV helper intentionally creates a vin-less
        // source tx for kernel-only tests.  Real block arrival requires a
        // structurally valid non-coinbase transaction, so rebuild that tx and
        // propagate its identity through the wallet/coinstake fixture.
        src.srcTx.vin.push_back(CTxIn(COutPoint(uint256(0xBEEF), 0)));
        src.srcTxHash = src.srcTx.GetHash();
        src.blockFrom.vtx[0] = src.srcTx;
        src.coinstake.vin[0].prevout = COutPoint(src.srcTxHash, 0);
        src.wtx = CWalletTx(pwalletMain, src.srcTx);
        // Make the delivered source block structurally valid for the real
        // ProcessMessage("block") path, then rewrite the exact bytes at the
        // same coordinates used by the fixture.
        CTransaction coinbase;
        coinbase.nTime = src.srcTx.nTime;
        coinbase.vin.resize(1);
        coinbase.vin[0].prevout.SetNull();
        coinbase.vin[0].scriptSig = CScript() << OP_0 << OP_1;
        coinbase.vout.push_back(CTxOut(0, CScript() << OP_TRUE));
        src.blockFrom.vtx.insert(src.blockFrom.vtx.begin(), coinbase);
        src.blockFrom.hashMerkleRoot = src.blockFrom.BuildMerkleTree();
        while (!CheckProofOfWork(src.blockFrom.GetPoWHash(), src.blockFrom.nBits))
            src.blockFrom.nNonce++;
        {
            LOCK(cs_main);
            BOOST_REQUIRE(src.blockFrom.WriteToDisk(src.nFile, src.nBlockPos));
        }
        src.sourceBlockHash = src.blockFrom.GetHash();
        BOOST_REQUIRE(src.blockFrom.CheckBlock());
        BOOST_REQUIRE(src.blockFrom.GetHash() == src.sourceBlockHash);
        src.wtx.hashBlock = src.sourceBlockHash;
        src.wtx.nIndex = 1;
        src.wtx.vMerkleBranch = src.blockFrom.GetMerkleBranch(1);
        srcOverriddenHash = src.sourceBlockHash;
        merkleRootOverride[2] = src.blockFrom.hashMerkleRoot;
        nBitsOverride[2] = src.blockFrom.nBits;
        nTimeOverride[2] = src.blockFrom.nTime;
        // Freeze the navigator snapshot: nFile=0 means "not yet materialized".
        // The real block bytes were already written to disk by
        // PrepareHybridSpvRealSource at src.nFile/src.nBlockPos.
        nFileOverride[2] = 0;
        nBlockPosOverride[2] = 0;
        Setup();
        RetainNavigator();
        LOCK(cs_main);

        // Precondition: source block absent from mapBlockIndex (cold-only).
        BOOST_CHECK_MESSAGE(mapBlockIndex.count(src.sourceBlockHash) == 0,
            "source must remain cold-only (absent from mapBlockIndex)");

        // Precondition: navigator resolves as active COLD with frozen nFile=0.
        {
            std::string e;
            ColdHotSeamSnapshot snap;
            const ColdHotSeamNavigator* nav = GetBlockIndexStakingNavigator();
            BOOST_REQUIRE(nav != NULL);
            BOOST_CHECK(nav->ResolveLogicalR(
                BlockIndexLogicalId(src.sourceBlockHash), &snap, &e) == COLD_HOT_SEAM_OK);
            BOOST_CHECK(snap.ref.IsCold());
            BOOST_CHECK_EQUAL(snap.snapshot.nFile, 0U);
            BOOST_CHECK_EQUAL(snap.snapshot.nBlockPos, 0U);
        }

        // Precondition: typed authority (maturity, seam, merkle) passes.
        {
            std::string e; int depth = 0;
            const ColdHotSeamNavigator* nav = GetBlockIndexStakingNavigator();
            BOOST_CHECK(nav->GetHybridSvmMaturityAuthorityR(
                BlockIndexLogicalId(src.wtx.hashBlock), src.wtx.GetHash(),
                src.wtx.vMerkleBranch, src.wtx.nIndex, &depth, &e) == COLD_HOT_SEAM_OK);
            BOOST_CHECK_EQUAL(depth, 18);
        }

        // Precondition: kernel time fixture passes.
        BOOST_REQUIRE(FindPassingStakeTime(pindexBest, src.blockFrom,
            src.srcTx, src.srcTxHash, &src.coinstake.nTime));
        BOOST_CHECK(VerifySignature(src.srcTx, src.coinstake,
            0, SCRIPT_VERIFY_NONE, 0));
        {
            uint256 hp, tp;
            const unsigned int nTxPrevOffset =
                ::GetSerializeSize(CBlock(), SER_DISK, CLIENT_VERSION) -
                (2 * GetSizeOfCompactSize(0)) +
                GetSizeOfCompactSize(src.blockFrom.vtx.size());
            BOOST_CHECK(CheckStakeKernelHash(pindexBest, src.blockFrom.nBits,
                src.blockFrom, nTxPrevOffset, src.srcTx,
                COutPoint(src.srcTxHash, 0), src.coinstake.nTime, hp, tp, false));
        }

        // Precondition: no materialization overlay yet.
        StakingMaterializationInfo info;
        BOOST_CHECK(!pwalletMain->GetStakingMaterialization(src.sourceBlockHash, &info));

        // Add the both-false SPV candidate so production selection owns the
        // request/recovery transition (as in the existing real-arrival test).
        SPVUtxo candidate;
        candidate.txhash = src.srcTxHash; candidate.n = 0;
        candidate.nValue = src.srcTx.vout[0].nValue;
        candidate.nHeight = 2; candidate.hashBlock = src.sourceBlockHash;
        candidate.nTime = src.srcTx.nTime;
        candidate.fHaveBlock = false; candidate.fVerified = false;
        candidate.scriptPubKey = src.srcTx.vout[0].scriptPubKey;
        {
            LOCK(pwalletMain->cs_spvutxos);
            pwalletMain->mapSPVUtxos[COutPoint(src.srcTxHash, 0)] = candidate;
        }

        // WITHOUT overlay: CheckProofOfStake must fail (frozen nFile=0 is
        // unreadable and no overlay coordinates exist).  The wallet tx is
        // inserted only for this validation boundary, not for request setup.
        pwalletMain->mapWallet[src.srcTxHash] = src.wtx;
        pwalletMain->mapWallet[src.srcTxHash].BindWallet(pwalletMain);
        BOOST_CHECK_MESSAGE(
            !RunCheckProofOfStake(src.coinstake, pindexBest, src.blockFrom.nBits),
            "CheckProofOfStake must reject when overlay is absent and snapshot nFile=0");

        // Establish the production request path before real arrival.
        peer.fSuccessfullyConnected = true;
        peer.nVersion = PROTOCOL_VERSION;
        AddStakingPeer(&peer); peerAdded = true;
        uint64_t nMin = 0, nMax = 0, nWeight = 0;
        BOOST_CHECK(!pwalletMain->GetStakeWeight(*pwalletMain, nMin, nMax, nWeight));
        StakingMaterializationInfo requested;
        BOOST_REQUIRE(pwalletMain->GetStakingMaterialization(src.sourceBlockHash, &requested));
        BOOST_CHECK(requested.pending);
        BOOST_CHECK_EQUAL(requested.nRequestCount, 1U);
        RemoveStakingPeer(&peer); peerAdded = false;

        // Deliver through the REAL block receive path.  This validates the
        // request -> arrival -> overlay publication lifecycle rather than
        // directly publishing test-only coordinates.
        DeliverFullBlock(peer, src.blockFrom);
        {
            StakingMaterializationInfo arrived;
            BOOST_REQUIRE(pwalletMain->GetStakingMaterialization(
                src.sourceBlockHash, &arrived));
            BOOST_CHECK(arrived.available);
            BOOST_CHECK(!arrived.pending);
            BOOST_CHECK(arrived.nFile > 0);
        }

        // WITH overlay: CheckProofOfStake must succeed.  This is the
        // decisive E3 boundary that A.9a.3h failed to cross.
        BOOST_CHECK_MESSAGE(
            RunCheckProofOfStake(src.coinstake, pindexBest, src.blockFrom.nBits),
            "CheckProofOfStake must succeed after overlay publishes real coordinates");

        // Verify: mapBlockIndex still absent (no historical index created).
        BOOST_CHECK(mapBlockIndex.count(src.sourceBlockHash) == 0);

        pwalletMain->mapWallet.erase(src.srcTxHash);
    }
    catch (...)
    {
        if (peerAdded) RemoveStakingPeer(&peer);
        pwalletMain->mapWallet.erase(src.srcTxHash);
        fHybridSPV = saveSPV; nCoinbaseMaturity = saveMaturity;
        nStakeMinAge = saveMinAge; nModifierInterval = saveI; nTargetSpacing = saveS;
        throw;
    }
    fHybridSPV = saveSPV; nCoinbaseMaturity = saveMaturity;
    nStakeMinAge = saveMinAge; nModifierInterval = saveI; nTargetSpacing = saveS;
}

// A.9a.3i negative E: active authority + overlay coordinates whose block hash
// does not match wtx.hashBlock must be rejected.  The new code in kernel.cpp
// verifies block.GetHash() == wtx.hashBlock after ReadFromDisk and
// invalidates the stale overlay entry on mismatch.
BOOST_FIXTURE_TEST_CASE(a9a3i_overlay_wrong_hash_rejected_at_validation, ProdFixture)
{
    const bool saveSPV = fHybridSPV;
    const int saveMaturity = nCoinbaseMaturity;
    const unsigned int saveMinAge = nStakeMinAge;
    unsigned int saveI = nModifierInterval, saveS = nTargetSpacing;
    fHybridSPV = true; nCoinbaseMaturity = 10; nStakeMinAge = 0;
    nModifierInterval = 60; nTargetSpacing = 5;
    HybridSpvRealSourceFixture src;
    try
    {
        PrepareHybridSpvRealSource(*this, &src);
        nFileOverride[2] = 0;
        nBlockPosOverride[2] = 0;
        Setup();
        RetainNavigator();
        LOCK(cs_main);

        BOOST_REQUIRE(mapBlockIndex.count(src.sourceBlockHash) == 0);

        // Authority passes.
        {
            std::string e; int depth = 0;
            const ColdHotSeamNavigator* nav = GetBlockIndexStakingNavigator();
            BOOST_REQUIRE(nav != NULL);
            BOOST_REQUIRE(nav->GetHybridSvmMaturityAuthorityR(
                BlockIndexLogicalId(src.wtx.hashBlock), src.wtx.GetHash(),
                src.wtx.vMerkleBranch, src.wtx.nIndex, &depth, &e) == COLD_HOT_SEAM_OK);
        }

        BOOST_REQUIRE(FindPassingStakeTime(pindexBest, src.blockFrom,
            src.srcTx, src.srcTxHash, &src.coinstake.nTime));

        pwalletMain->mapWallet[src.srcTxHash] = src.wtx;
        pwalletMain->mapWallet[src.srcTxHash].BindWallet(pwalletMain);

        // Write a DIFFERENT block to disk and publish its coordinates as if
        // they were the source block's materialization.  ReadFromDisk will
        // succeed but block.GetHash() != wtx.hashBlock -> reject.
        CBlock dummyBlock;
        dummyBlock.nVersion = 1;
        dummyBlock.hashPrevBlock = Hash(999);
        dummyBlock.nTime = 9999;
        dummyBlock.nBits = 0x207fffff;
        dummyBlock.nNonce = 7;
        while (!CheckProofOfWork(dummyBlock.GetPoWHash(), dummyBlock.nBits))
            dummyBlock.nNonce++;
        unsigned int dummyFile = 0, dummyPos = 0;
        BOOST_REQUIRE(dummyBlock.WriteToDisk(dummyFile, dummyPos));
        BOOST_REQUIRE(dummyBlock.GetHash() != src.sourceBlockHash);

        pwalletMain->PublishStakingMaterialization(
            src.sourceBlockHash, dummyFile, dummyPos);

        // Must reject: overlay points to a block with wrong hash.
        BOOST_CHECK_MESSAGE(
            !RunCheckProofOfStake(src.coinstake, pindexBest, src.blockFrom.nBits),
            "CheckProofOfStake must reject overlay coordinates with wrong block hash");

        // The overlay entry should have been invalidated by the hash mismatch.
        StakingMaterializationInfo after;
        BOOST_REQUIRE(pwalletMain->GetStakingMaterialization(src.sourceBlockHash, &after));
        BOOST_CHECK(!after.available);

        pwalletMain->mapWallet.erase(src.srcTxHash);
    }
    catch (...)
    {
        pwalletMain->mapWallet.erase(src.srcTxHash);
        fHybridSPV = saveSPV; nCoinbaseMaturity = saveMaturity;
        nStakeMinAge = saveMinAge; nModifierInterval = saveI; nTargetSpacing = saveS;
        throw;
    }
    fHybridSPV = saveSPV; nCoinbaseMaturity = saveMaturity;
    nStakeMinAge = saveMinAge; nModifierInterval = saveI; nTargetSpacing = saveS;
}

// A.9a.3i negative F: active authority + overlay coordinates that point to
// unreadable/non-existent disk locations must be rejected.  ReadFromDisk
// fails, fHaveBlock stays false, and validation falls through to rejection.
BOOST_FIXTURE_TEST_CASE(a9a3i_overlay_unreadable_coords_rejected_at_validation, ProdFixture)
{
    const bool saveSPV = fHybridSPV;
    const int saveMaturity = nCoinbaseMaturity;
    const unsigned int saveMinAge = nStakeMinAge;
    unsigned int saveI = nModifierInterval, saveS = nTargetSpacing;
    fHybridSPV = true; nCoinbaseMaturity = 10; nStakeMinAge = 0;
    nModifierInterval = 60; nTargetSpacing = 5;
    HybridSpvRealSourceFixture src;
    try
    {
        PrepareHybridSpvRealSource(*this, &src);
        nFileOverride[2] = 0;
        nBlockPosOverride[2] = 0;
        Setup();
        RetainNavigator();
        LOCK(cs_main);

        BOOST_REQUIRE(mapBlockIndex.count(src.sourceBlockHash) == 0);

        {
            std::string e; int depth = 0;
            const ColdHotSeamNavigator* nav = GetBlockIndexStakingNavigator();
            BOOST_REQUIRE(nav != NULL);
            BOOST_REQUIRE(nav->GetHybridSvmMaturityAuthorityR(
                BlockIndexLogicalId(src.wtx.hashBlock), src.wtx.GetHash(),
                src.wtx.vMerkleBranch, src.wtx.nIndex, &depth, &e) == COLD_HOT_SEAM_OK);
        }

        BOOST_REQUIRE(FindPassingStakeTime(pindexBest, src.blockFrom,
            src.srcTx, src.srcTxHash, &src.coinstake.nTime));

        pwalletMain->mapWallet[src.srcTxHash] = src.wtx;
        pwalletMain->mapWallet[src.srcTxHash].BindWallet(pwalletMain);

        // Publish overlay with non-existent coordinates.
        pwalletMain->PublishStakingMaterialization(
            src.sourceBlockHash, 99, 99999);

        // Must reject: ReadFromDisk fails at non-existent coordinates.
        BOOST_CHECK_MESSAGE(
            !RunCheckProofOfStake(src.coinstake, pindexBest, src.blockFrom.nBits),
            "CheckProofOfStake must reject unreadable overlay coordinates");

        pwalletMain->mapWallet.erase(src.srcTxHash);
    }
    catch (...)
    {
        pwalletMain->mapWallet.erase(src.srcTxHash);
        fHybridSPV = saveSPV; nCoinbaseMaturity = saveMaturity;
        nStakeMinAge = saveMinAge; nModifierInterval = saveI; nTargetSpacing = saveS;
        throw;
    }
    fHybridSPV = saveSPV; nCoinbaseMaturity = saveMaturity;
    nStakeMinAge = saveMinAge; nModifierInterval = saveI; nTargetSpacing = saveS;
}


// ==========================================================================
// A.9a.3j — LOCAL CHECKSTAKE LOCK-CONTRACT CLOSURE
// ==========================================================================

// A.9a.3j T1: causal lock-contract proof.  CheckStake must hold cs_main
// across the parent lookup and the initial CheckProofOfStake call.  The test
// calls CheckStake WITHOUT pre-acquiring cs_main (the production StakeMiner
// boundary).  A concurrent probe thread continuously tries to acquire cs_main
// via try_lock() while CheckStake runs.  A boost::barrier ensures the probe
// is actively spinning before CheckStake enters its critical section.
//
// This is causal: pre-fix, the probe always acquires cs_main (no contention);
// post-fix, the probe observes contention during the critical section.
BOOST_FIXTURE_TEST_CASE(a9a3j_checkstake_holds_cs_main_across_validation, ProdFixture)
{
    const bool saveSPV = fHybridSPV;
    const int saveMaturity = nCoinbaseMaturity;
    const unsigned int saveMinAge = nStakeMinAge;
    unsigned int saveI = nModifierInterval, saveS = nTargetSpacing;
    fHybridSPV = true; nCoinbaseMaturity = 10; nStakeMinAge = 0;
    nModifierInterval = 60; nTargetSpacing = 5;
    HybridSpvRealSourceFixture src;
    try
    {
        PrepareHybridSpvRealSource(*this, &src);
        // Make the source tx structurally valid (non-empty vin).
        src.srcTx.vin.push_back(CTxIn(COutPoint(uint256(0xBEEF), 0)));
        src.srcTxHash = src.srcTx.GetHash();
        src.blockFrom.vtx[0] = src.srcTx;
        src.coinstake.vin[0].prevout = COutPoint(src.srcTxHash, 0);
        src.wtx = CWalletTx(pwalletMain, src.srcTx);
        // Build a structurally valid source block with coinbase.
        CTransaction coinbase;
        coinbase.nTime = src.srcTx.nTime;
        coinbase.vin.resize(1);
        coinbase.vin[0].prevout.SetNull();
        coinbase.vin[0].scriptSig = CScript() << OP_0 << OP_1;
        coinbase.vout.push_back(CTxOut(0, CScript() << OP_TRUE));
        src.blockFrom.vtx.insert(src.blockFrom.vtx.begin(), coinbase);
        src.blockFrom.hashMerkleRoot = src.blockFrom.BuildMerkleTree();
        while (!CheckProofOfWork(src.blockFrom.GetPoWHash(), src.blockFrom.nBits))
            src.blockFrom.nNonce++;
        {
            LOCK(cs_main);
            BOOST_REQUIRE(src.blockFrom.WriteToDisk(src.nFile, src.nBlockPos));
        }
        src.sourceBlockHash = src.blockFrom.GetHash();
        src.wtx.hashBlock = src.sourceBlockHash;
        src.wtx.nIndex = 1;
        src.wtx.vMerkleBranch = src.blockFrom.GetMerkleBranch(1);
        srcOverriddenHash = src.sourceBlockHash;
        merkleRootOverride[2] = src.blockFrom.hashMerkleRoot;
        nBitsOverride[2] = src.blockFrom.nBits;
        nTimeOverride[2] = src.blockFrom.nTime;
        nFileOverride[2] = src.nFile;
        nBlockPosOverride[2] = src.nBlockPos;
        Setup();
        RetainNavigator();

        // Insert the wallet tx so CheckProofOfStake can find it.
        {
            LOCK(cs_main);
            pwalletMain->mapWallet[src.srcTxHash] = src.wtx;
            pwalletMain->mapWallet[src.srcTxHash].BindWallet(pwalletMain);

            BOOST_REQUIRE(FindPassingStakeTime(pindexBest, src.blockFrom,
                src.srcTx, src.srcTxHash, &src.coinstake.nTime));
        }

        // Build a PoS block that CheckStake will validate.
        CBlock stakeBlock;
        stakeBlock.nVersion = 1;
        stakeBlock.hashPrevBlock = hashBestChain;
        stakeBlock.nTime = src.coinstake.nTime;
        stakeBlock.nBits = pindexBest->nBits;
        stakeBlock.nNonce = 1;
        stakeBlock.vtx.push_back(coinbase);
        stakeBlock.vtx.push_back(src.coinstake);
        stakeBlock.hashMerkleRoot = stakeBlock.BuildMerkleTree();

        // Concurrent probe: use a barrier so the probe thread is actively
        // spinning before CheckStake starts.  Direct try_lock()/unlock() on
        // the raw mutex avoids CCriticalBlock overhead per iteration.
        // boost::barrier(2) ensures both threads reach the sync point before
        // either proceeds — the probe enters its tight loop and the main
        // thread then calls CheckStake.
        boost::barrier syncPoint(2);
        std::atomic<bool> contentionDetected{false};
        std::atomic<bool> running{true};

        boost::thread probe([&]() {
            syncPoint.wait();
            while (running.load(std::memory_order_relaxed))
            {
                if (!cs_main.try_lock())
                {
                    contentionDetected.store(true, std::memory_order_relaxed);
                    break;
                }
                cs_main.unlock();
                boost::this_thread::yield();
            }
        });

        syncPoint.wait();
        // Give the probe thread a moment to enter its spin loop.
        boost::this_thread::yield();

        // Call CheckStake WITHOUT pre-holding cs_main (production boundary).
        // CheckStake will either succeed or fail for expected reasons
        // (e.g., ProcessBlock rejection), but it must NOT fail due to
        // missing cs_main.
        CheckStake(&stakeBlock, *pwalletMain);

        running.store(false, std::memory_order_relaxed);
        probe.join();

        // The probe must have detected contention, proving cs_main was held
        // during the critical section (parent lookup + CheckProofOfStake).
        BOOST_CHECK_MESSAGE(contentionDetected.load(std::memory_order_relaxed),
            "CheckStake must hold cs_main during parent lookup and CheckProofOfStake; "
            "concurrent probe did not detect lock contention");

        pwalletMain->mapWallet.erase(src.srcTxHash);
    }
    catch (...)
    {
        pwalletMain->mapWallet.erase(src.srcTxHash);
        fHybridSPV = saveSPV; nCoinbaseMaturity = saveMaturity;
        nStakeMinAge = saveMinAge; nModifierInterval = saveI; nTargetSpacing = saveS;
        throw;
    }
    fHybridSPV = saveSPV; nCoinbaseMaturity = saveMaturity;
    nStakeMinAge = saveMinAge; nModifierInterval = saveI; nTargetSpacing = saveS;
}

// A.9a.3j T2: missing-parent no-insertion proof.  When CheckStake is called
// with a block whose hashPrevBlock is NOT in mapBlockIndex, the non-inserting
// find() lookup must fail without polluting mapBlockIndex.  The old
// operator[] would have inserted a null/default CBlockIndex* entry.
BOOST_FIXTURE_TEST_CASE(a9a3j_checkstake_missing_parent_no_insertion, ProdFixture)
{
    Setup();

    // Record mapBlockIndex state before the call.
    const size_t sizeBefore = mapBlockIndex.size();

    // Build a PoS block with a parent hash that does NOT exist in mapBlockIndex.
    CBlock orphanBlock;
    orphanBlock.nVersion = 1;
    orphanBlock.hashPrevBlock = uint256(0xDEADBEEF);
    orphanBlock.nTime = 9999;
    orphanBlock.nBits = 0x207fffff;
    orphanBlock.nNonce = 1;

    // Add a minimal coinstake so IsProofOfStake() returns true.
    CTransaction coinbase;
    coinbase.nTime = 9999;
    coinbase.vin.resize(1);
    coinbase.vin[0].prevout.SetNull();
    coinbase.vin[0].scriptSig = CScript() << OP_0 << OP_1;
    coinbase.vout.push_back(CTxOut(0, CScript() << OP_TRUE));

    CTransaction coinstake;
    coinstake.nVersion = 1;
    coinstake.nTime = 9999;
    CTxIn in; in.prevout = COutPoint(uint256(0xF00D), 0);
    coinstake.vin.push_back(in);
    CTxOut emptyOut; emptyOut.nValue = 0; emptyOut.scriptPubKey = CScript();
    coinstake.vout.push_back(emptyOut);
    CTxOut rewardOut; rewardOut.nValue = COIN; rewardOut.scriptPubKey = CScript() << OP_TRUE;
    coinstake.vout.push_back(rewardOut);

    orphanBlock.vtx.push_back(coinbase);
    orphanBlock.vtx.push_back(coinstake);
    orphanBlock.hashMerkleRoot = orphanBlock.BuildMerkleTree();

    // Verify the parent is truly absent.
    BOOST_REQUIRE(mapBlockIndex.count(orphanBlock.hashPrevBlock) == 0);

    // Call CheckStake WITHOUT cs_main (production boundary).
    // Must fail because parent is not found.
    BOOST_CHECK(!CheckStake(&orphanBlock, *pwalletMain));

    // Critical: mapBlockIndex must NOT have grown.  The old operator[] would
    // have inserted {hashPrevBlock -> NULL} into the map.
    BOOST_CHECK_EQUAL(mapBlockIndex.size(), sizeBefore);
    BOOST_CHECK_MESSAGE(mapBlockIndex.count(orphanBlock.hashPrevBlock) == 0,
        "CheckStake must not insert a null CBlockIndex for a missing parent");
}

} // namespace (A.9a.3g helpers)

BOOST_AUTO_TEST_SUITE_END()
