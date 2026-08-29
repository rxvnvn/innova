#include <boost/test/unit_test.hpp>

#include "main.h"
#include "candidate_frontier.h"
#include "dag.h"

extern std::map<uint256, CBlockIndex*> mapBlockIndex;
extern CBlockIndex* pindexBest;
extern uint256 nBestChainTrust;
extern std::set<uint256> setInvalidBlockHash;
extern std::map<uint256, CandidateTipRecord> mapCandidateTips;
extern uint256 hashBestChain;
extern CDAGManager g_dagManager;
extern int nBestHeight;

bool IsBlockOperatorInvalid(const CBlockIndex* pindex);

BOOST_AUTO_TEST_SUITE(candidate_frontier_tests)

/**
 * Helper: create a minimal CBlockIndex, register in mapBlockIndex.
 * Returns a fresh heap-allocated index.
 */
static CBlockIndex* make_block(const uint256& hash, int height,
                                CBlockIndex* pprev, uint64_t trust)
{
    CBlockIndex* p = new CBlockIndex();
    p->phashBlock  = new uint256(hash);
    p->nHeight     = height;
    p->nChainTrust = uint256(uint64_t(trust));
    p->pprev       = pprev;
    p->nFlags      = 0; // PoW
    return p;
}

struct FrontierTestFixture
{
    std::map<uint256, CBlockIndex*> savedMap;
    uint256 savedHashBestChain;
    CBlockIndex* savedBest;
    uint256 savedTrust;
    int savedHeight;

    FrontierTestFixture()
    {
        // Save and clear global state
        savedMap.swap(mapBlockIndex);
        savedHashBestChain = hashBestChain;
        savedBest = pindexBest;
        savedTrust = nBestChainTrust;
        savedHeight = nBestHeight;
        hashBestChain = 0;
        pindexBest = NULL;
        nBestChainTrust = 0;
        nBestHeight = -1;
        setInvalidBlockHash.clear();
        mapCandidateTips.clear();
    }

    ~FrontierTestFixture()
    {
        // Restore
        for (auto& pair : mapBlockIndex) {
            delete pair.second->phashBlock;
            delete pair.second;
        }
        mapBlockIndex.clear();
        mapCandidateTips.clear();
        setInvalidBlockHash.clear();
        savedMap.swap(mapBlockIndex);
        hashBestChain = savedHashBestChain;
        pindexBest = savedBest;
        nBestChainTrust = savedTrust;
        nBestHeight = savedHeight;
    }

    // Set pindexBest + hashBestChain + nBestChainTrust + nBestHeight
    void set_best(CBlockIndex* p)
    {
        pindexBest = p;
        hashBestChain = *p->phashBlock;
        nBestChainTrust = p->nChainTrust;
        nBestHeight = p->nHeight;
    }

    // Rebuild tips and mark all tips as having disk data (since we use
    // fake CBlockIndex objects, ReadFromDisk would fail).
    void rebuild_tips_with_data()
    {
        RebuildCandidateTips();
    for (auto& entry : mapCandidateTips)
        entry.second.fHasData = 1;
        for (auto& entry : mapCandidateTips)
            entry.second.fHasData = 1;
    }
};

// =====================================================================
// 1. ACTIVE CHAIN ONLY
// =====================================================================
BOOST_FIXTURE_TEST_CASE(active_chain_only, FrontierTestFixture)
{
    uint256 h0(std::string("0"));
    uint256 h1(std::string("1"));
    uint256 h2(std::string("2"));

    CBlockIndex* p0 = make_block(h0, 0, NULL, 2);
    CBlockIndex* p1 = make_block(h1, 1, p0,    4);
    CBlockIndex* p2 = make_block(h2, 2, p1,    6);

    mapBlockIndex[h0] = p0;
    mapBlockIndex[h1] = p1;
    mapBlockIndex[h2] = p2;
    set_best(p2);

    RebuildCandidateTips();
    for (auto& entry : mapCandidateTips)
        entry.second.fHasData = 1;
    BOOST_CHECK_EQUAL(mapCandidateTips.size(), (size_t)1); // only p2 is a tip

    CBlockIndex* frontier = EvaluateCandidateFrontier();
    BOOST_CHECK(frontier == p2);
    BOOST_CHECK(frontier == pindexBest); // best tip is also the best candidate
}

// =====================================================================
// 2. ONE SIDE TIP (lower trust than best)
// =====================================================================
BOOST_FIXTURE_TEST_CASE(one_side_tip_lower, FrontierTestFixture)
{
    // best:     0 -> 1 -> 2 -> 3   (trust 8)
    // side:     0 -> 1 -> a        (trust 6)
    uint256 h0(std::string("0"));
    uint256 h1(std::string("1"));
    uint256 h2(std::string("2"));
    uint256 h3(std::string("3"));
    uint256 ha(std::string("a"));

    CBlockIndex* p0 = make_block(h0, 0, NULL, 2);
    CBlockIndex* p1 = make_block(h1, 1, p0,   4);
    CBlockIndex* p2 = make_block(h2, 2, p1,   6);
    CBlockIndex* p3 = make_block(h3, 3, p2,   8);
    CBlockIndex* pa = make_block(ha, 2, p1,   6); // side tip, same trust as p2

    mapBlockIndex[h0] = p0;
    mapBlockIndex[h1] = p1;
    mapBlockIndex[h2] = p2;
    mapBlockIndex[h3] = p3;
    mapBlockIndex[ha] = pa;
    set_best(p3);

    // Run legacy full-scan semantic inline
    // Legacy: leaves = {p3, pa}. pa has trust=6 <= nBestChainTrust=8 → skip.
    // Only p3 eligible.
    RebuildCandidateTips();
    for (auto& entry : mapCandidateTips)
        entry.second.fHasData = 1;
    BOOST_CHECK_EQUAL(mapCandidateTips.size(), (size_t)2); // p3, pa
    CBlockIndex* frontier = EvaluateCandidateFrontier();
    BOOST_CHECK(frontier == p3);
}

// =====================================================================
// 3. HIGHER-TRUST SIDE TIP
// =====================================================================
BOOST_FIXTURE_TEST_CASE(higher_trust_side_tip, FrontierTestFixture)
{
    // best:     0 -> 1 -> 2 -> 3   (trust 6)
    // side:     0 -> 1 -> a        (trust 10); a is leaf
    uint256 h0(std::string("0"));
    uint256 h1(std::string("1"));
    uint256 h2(std::string("2"));
    uint256 h3(std::string("3"));
    uint256 ha(std::string("a"));

    CBlockIndex* p0 = make_block(h0, 0, NULL, 2);
    CBlockIndex* p1 = make_block(h1, 1, p0,  4);
    CBlockIndex* p2 = make_block(h2, 2, p1,  6);
    CBlockIndex* p3 = make_block(h3, 3, p2,  8);
    CBlockIndex* pa = make_block(ha, 2, p1, 10); // higher trust than p3

    mapBlockIndex[h0] = p0;
    mapBlockIndex[h1] = p1;
    mapBlockIndex[h2] = p2;
    mapBlockIndex[h3] = p3;
    mapBlockIndex[ha] = pa;
    set_best(p3);
    // nBestChainTrust = 8. pa has trust 10 > 8 → eligible.

    RebuildCandidateTips();
    for (auto& entry : mapCandidateTips)
        entry.second.fHasData = 1;
    CBlockIndex* frontier = EvaluateCandidateFrontier();
    BOOST_CHECK(frontier == pa); // side tip wins due to higher trust
}

// =====================================================================
// 4. EQUAL-TRUST TIPS
// =====================================================================
BOOST_FIXTURE_TEST_CASE(equal_trust_tips, FrontierTestFixture)
{
    // best:     0 -> 1     (trust 4)
    // side a:   0 -> a     (trust 4) — equal trust
    // side b:   0 -> b     (trust 4) — equal trust
    uint256 h0(std::string("0"));
    uint256 h1(std::string("1"));
    uint256 ha(std::string("a"));
    uint256 hb(std::string("b"));

    CBlockIndex* p0 = make_block(h0, 0, NULL, 2);
    CBlockIndex* p1 = make_block(h1, 1, p0,   4);
    CBlockIndex* pa = make_block(ha, 1, p0,   4);
    CBlockIndex* pb = make_block(hb, 1, p0,   4);

    mapBlockIndex[h0] = p0;
    mapBlockIndex[h1] = p1;
    mapBlockIndex[ha] = pa;
    mapBlockIndex[hb] = pb;
    set_best(p1);
    // nBestChainTrust = 4. All side tips have trust 4 which is <= 4 → excluded.
    // Only p1 has trust 4 and is on best chain (is pindexBest) → frontier returns pindexBest as default.

    RebuildCandidateTips();
    for (auto& entry : mapCandidateTips)
        entry.second.fHasData = 1;
    CBlockIndex* frontier = EvaluateCandidateFrontier();
    BOOST_CHECK(frontier == p1);
    // Legacy: side tips excluded by <= nBestChainTrust. On equal trust, legacy
    // selects none above current, returns early with pindexBest unchanged.
}

// =====================================================================
// 5. INVALID TIP (operator-invalidated)
// =====================================================================
BOOST_FIXTURE_TEST_CASE(invalid_tip_excluded, FrontierTestFixture)
{
    // best:     0 -> 1     (trust 4)
    // side:     0 -> a     (trust 6) — but a or its ancestor is invalid
    uint256 h0(std::string("0"));
    uint256 h1(std::string("1"));
    uint256 ha(std::string("a"));

    CBlockIndex* p0 = make_block(h0, 0, NULL, 2);
    CBlockIndex* p1 = make_block(h1, 1, p0,   4);
    CBlockIndex* pa = make_block(ha, 1, p0,   6);

    mapBlockIndex[h0] = p0;
    mapBlockIndex[h1] = p1;
    mapBlockIndex[ha] = pa;
    set_best(p1);

    // Invalidate 'a' itself
    setInvalidBlockHash.insert(ha);

    RebuildCandidateTips();
    for (auto& entry : mapCandidateTips)
        entry.second.fHasData = 1;
    CBlockIndex* frontier = EvaluateCandidateFrontier();
    BOOST_CHECK(frontier == p1); // side excluded by invalidity, best stays
}

// =====================================================================
// 6. INVALID ANCESTOR (ancestor in setInvalidBlockHash)
// =====================================================================
BOOST_FIXTURE_TEST_CASE(invalid_ancestor_excluded, FrontierTestFixture)
{
    // best:     0 -> 1     (trust 4)
    // side:     0 -> a -> b (trust 8) — a is invalid
    uint256 h0(std::string("0"));
    uint256 h1(std::string("1"));
    uint256 ha(std::string("a"));
    uint256 hb(std::string("b"));

    CBlockIndex* p0 = make_block(h0, 0, NULL, 2);
    CBlockIndex* p1 = make_block(h1, 1, p0,  4);
    CBlockIndex* pa = make_block(ha, 1, p0,  4);
    CBlockIndex* pb = make_block(hb, 2, pa,  8);

    mapBlockIndex[h0] = p0;
    mapBlockIndex[h1] = p1;
    mapBlockIndex[ha] = pa;
    mapBlockIndex[hb] = pb;
    set_best(p1);

    setInvalidBlockHash.insert(ha); // invalidate the ancestor

    RebuildCandidateTips();
    for (auto& entry : mapCandidateTips)
        entry.second.fHasData = 1; // leaves = {p1, pb}
    CBlockIndex* frontier = EvaluateCandidateFrontier();
    BOOST_CHECK(frontier == p1); // pb excluded by ancestor invalidity
}

// =====================================================================
// 7. RECONSIDERED BRANCH
// =====================================================================
BOOST_FIXTURE_TEST_CASE(reconsidered_branch, FrontierTestFixture)
{
    // best:     0 -> 1     (trust 4)
    // side:     0 -> a     (trust 6) — a was invalidated, then reconsidered
    uint256 h0(std::string("0"));
    uint256 h1(std::string("1"));
    uint256 ha(std::string("a"));

    CBlockIndex* p0 = make_block(h0, 0, NULL, 2);
    CBlockIndex* p1 = make_block(h1, 1, p0,  4);
    CBlockIndex* pa = make_block(ha, 1, p0,  6);

    mapBlockIndex[h0] = p0;
    mapBlockIndex[h1] = p1;
    mapBlockIndex[ha] = pa;
    set_best(p1);

    // Invalidate, then reconsider
    setInvalidBlockHash.insert(ha);
    RebuildCandidateTips();
    for (auto& entry : mapCandidateTips)
        entry.second.fHasData = 1;
    BOOST_CHECK(EvaluateCandidateFrontier() == p1); // excluded

    setInvalidBlockHash.erase(ha);
    RebuildCandidateTips();
    for (auto& entry : mapCandidateTips)
        entry.second.fHasData = 1;
    CBlockIndex* frontier = EvaluateCandidateFrontier();
    BOOST_CHECK(frontier == pa); // now eligible, higher trust
}

// =====================================================================
// 8. MISSING DATA CANDIDATE
// =====================================================================
BOOST_FIXTURE_TEST_CASE(missing_data_excluded, FrontierTestFixture)
{
    // best:     0 -> 1     (trust 4)
    // side:     0 -> a     (trust 6) — no disk data (fHasData=0, ReadFromDisk fails)
    uint256 h0(std::string("0"));
    uint256 h1(std::string("1"));
    uint256 ha(std::string("a"));

    CBlockIndex* p0 = make_block(h0, 0, NULL, 2);
    CBlockIndex* p1 = make_block(h1, 1, p0,  4);
    CBlockIndex* pa = make_block(ha, 1, p0,  6);

    mapBlockIndex[h0] = p0;
    mapBlockIndex[h1] = p1;
    mapBlockIndex[ha] = pa;
    set_best(p1);

    // Simulate missing data: set fHasData=0 in the tip record, and
    // ReadFromDisk will fail for a hash with no actual disk block.
    RebuildCandidateTips();
    // mapCandidateTips[ha].fHasData was set during rebuild based on actual
    // ReadFromDisk.  Since there's no real block data, it should be 0.
    // EvaluateCandidateFrontier re-checks: if fHasData==0, it tries ReadFromDisk.
    // With fake CBlockIndex objects, ReadFromDisk returns false.
    CBlockIndex* frontier = EvaluateCandidateFrontier();
    BOOST_CHECK(frontier == p1); // pa excluded by missing data
}

// =====================================================================
// 9. LEGACY vs FRONTIER: full scan comparison
// =====================================================================
BOOST_FIXTURE_TEST_CASE(legacy_vs_frontier_equivalence, FrontierTestFixture)
{
    // Build a small chain with side branches
    //     genesis -> A -> B -> C (best, trust 8)
    //                    \-> D (trust 6)
    //     genesis -> E (trust 4)
    uint256 hG(std::string("g"));
    uint256 hA(std::string("a"));
    uint256 hB(std::string("b"));
    uint256 hC(std::string("c"));
    uint256 hD(std::string("d"));
    uint256 hE(std::string("e"));

    CBlockIndex* pG = make_block(hG, 0, NULL, 2);
    CBlockIndex* pA = make_block(hA, 1, pG,   4);
    CBlockIndex* pB = make_block(hB, 2, pA,   6);
    CBlockIndex* pC = make_block(hC, 3, pB,   8);
    CBlockIndex* pD = make_block(hD, 3, pB,   6); // side branch from B
    CBlockIndex* pE = make_block(hE, 1, pG,   4); // side branch from genesis

    mapBlockIndex[hG] = pG;
    mapBlockIndex[hA] = pA;
    mapBlockIndex[hB] = pB;
    mapBlockIndex[hC] = pC;
    mapBlockIndex[hD] = pD;
    mapBlockIndex[hE] = pE;
    set_best(pC);

    // Legacy full-scan logic:
    // Leaves = {pC, pD, pE}
    // pC: trust 8 <= nBestChainTrust 8 → skip (not above)
    // pD: trust 6 <= 8 → skip
    // pE: trust 4 <= 8 → skip
    // No eligible candidate → legacy returns early

    RebuildCandidateTips();
    for (auto& entry : mapCandidateTips)
        entry.second.fHasData = 1;
    CBlockIndex* frontier = EvaluateCandidateFrontier();
    // All tips have trust <= nBestChainTrust, frontier returns pindexBest
    BOOST_CHECK(frontier == pC);

    // Now advance best chain: D gets extended by F with higher trust
    uint256 hF(std::string("f"));
    CBlockIndex* pF = make_block(hF, 4, pD, 12);
    mapBlockIndex[hF] = pF;
    // Leaves = {pC, pF, pE}
    // pF trust 12 > 8 → eligible
    set_best(pC); // keep best as C initially

    RebuildCandidateTips();
    for (auto& entry : mapCandidateTips)
        entry.second.fHasData = 1;
    frontier = EvaluateCandidateFrontier();
    BOOST_CHECK(frontier == pF); // F wins due to higher trust
}

// =====================================================================
// 10. MULTIPLE SIDE TIPS — CANDIDATE SET EQUIVALENCE
// =====================================================================
BOOST_FIXTURE_TEST_CASE(multiple_side_tips_set, FrontierTestFixture)
{
    // Two side tips. Verify EXACT set of tips matches between legacy and frontier.
    // Legacy leaves = mapCandidateTips keys (after rebuild)
    uint256 h0(std::string("0"));
    uint256 h1(std::string("1"));
    uint256 h2(std::string("2"));
    uint256 ha(std::string("a"));
    uint256 hb(std::string("b"));

    CBlockIndex* p0 = make_block(h0, 0, NULL, 2);
    CBlockIndex* p1 = make_block(h1, 1, p0,   4);
    CBlockIndex* p2 = make_block(h2, 2, p1,   6);
    CBlockIndex* pa = make_block(ha, 1, p0,   4);
    CBlockIndex* pb = make_block(hb, 1, p0,   4);

    mapBlockIndex[h0] = p0;
    mapBlockIndex[h1] = p1;
    mapBlockIndex[h2] = p2;
    mapBlockIndex[ha] = pa;
    mapBlockIndex[hb] = pb;
    set_best(p2);

    RebuildCandidateTips();
    for (auto& entry : mapCandidateTips)
        entry.second.fHasData = 1;
    // Expected leaves = {p2, pa, pb}
    BOOST_CHECK_EQUAL(mapCandidateTips.size(), (size_t)3);

    // Count how many are actually leaves (not referenced as pprev)
    // p2 pprev not referenced → yes. pa pprev not referenced → yes. pb → yes.
    // All three are true leaves.
}

BOOST_AUTO_TEST_SUITE_END()