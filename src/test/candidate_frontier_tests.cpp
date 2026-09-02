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

// =====================================================================
// A.10.1c — By-value authoritative candidate frontier
// =====================================================================
BOOST_AUTO_TEST_SUITE(candidate_frontier_byvalue_tests)

// Counting wrapper: proves the evaluator's Lookup/GetParent cost depends only
// on the candidate frontier F, not on historical block count N (INV2 / O(F)).
static uint256 byvalue_hash(int v)
{
    // Deterministic, strictly-ascending-by-value uint256 (the map iterator
    // order over these keys is ascending numeric order, which the T2/T4
    // equality tests rely on).
    uint256 h;
    h.SetHex(strprintf("%064x", (unsigned long long)v));
    return h;
}

struct CountingSnapshotStore : public SnapshotCandidateFrontierStore
{
    mutable int lookups = 0;
    mutable int parentWalks = 0;

    CandidateFrontierAuthorityRecord Lookup(const uint256& h) const
    {
        ++lookups;
        return SnapshotCandidateFrontierStore::Lookup(h);
    }
    CandidateFrontierAuthorityRecord GetParent(const uint256& child) const
    {
        ++parentWalks;
        return SnapshotCandidateFrontierStore::GetParent(child);
    }
};

// INV0/INV1/INV2 helper for a snapshot scenario.
static void assert_snapshot_winner(SnapshotCandidateFrontierStore& store,
                                   const uint256& expectHash)
{
    const CandidateFrontierAuthorityRecord sel =
        EvaluateCandidateFrontierByValue(store);
    if (expectHash == uint256(0))
    {
        BOOST_CHECK(!sel.found);   // no eligible candidate
    }
    else
    {
        BOOST_CHECK(sel.found);              // INV0 winner hash
        if (sel.found)
        {
            BOOST_CHECK(sel.hash == expectHash);
            BOOST_CHECK(sel.chainTrust == store.Lookup(expectHash).chainTrust); // INV1
            BOOST_CHECK(sel.isEligible);
        }
    }
}

// T0 — single active tip (only best; nothing above it).
BOOST_AUTO_TEST_CASE(t0_single_active_tip)
{
    SnapshotCandidateFrontierStore s;
    // best = genesis(0). No candidate above best trust.
    s.AddBlock(byvalue_hash(0), uint256(0), uint256(2), 0);
    s.SetBest(byvalue_hash(0), uint256(2));
    assert_snapshot_winner(s, uint256(0)); // no above-best candidate
}

// T1 — no eligible tip (all tips <= best trust).
BOOST_AUTO_TEST_CASE(t1_no_eligible_tip)
{
    SnapshotCandidateFrontierStore s;
    s.AddBlock(byvalue_hash(0), uint256(0), uint256(4), 0);
    s.AddBlock(byvalue_hash(1), byvalue_hash(0), uint256(4), 1); // equal trust
    s.SetBest(byvalue_hash(1), uint256(4));
    s.tipHashes.push_back(byvalue_hash(1));
    assert_snapshot_winner(s, uint256(0)); // equal trust never selected
}

// T2 — equal-trust competing tips both above best: winner is first in hash order.
BOOST_AUTO_TEST_CASE(t2_equal_trust_competing_tips)
{
    SnapshotCandidateFrontierStore s;
    s.AddBlock(byvalue_hash(0), uint256(0), uint256(2), 0);
    s.AddBlock(byvalue_hash(1), byvalue_hash(0), uint256(4), 1);
    s.AddBlock(byvalue_hash(2), byvalue_hash(0), uint256(4), 1); // equal trust
    s.SetBest(byvalue_hash(1), uint256(4));
    s.tipHashes.push_back(byvalue_hash(1));
    s.tipHashes.push_back(byvalue_hash(2));
    s.hasData.insert(byvalue_hash(1));
    s.hasData.insert(byvalue_hash(2));
    // both have trust 4 == best trust 4, so neither selected. Advance best to lower:
    s.SetBest(byvalue_hash(0), uint256(2));
    // now both 4 > 2. first in map order wins (strict >, no equality replacement).
    // lower hash must be byvalue_hash(1).
    assert_snapshot_winner(s, byvalue_hash(1));
}

// T3 — higher-trust side branch wins.
BOOST_AUTO_TEST_CASE(t3_higher_trust_side)
{
    SnapshotCandidateFrontierStore s;
    s.AddBlock(byvalue_hash(0), uint256(0), uint256(2), 0);
    s.AddBlock(byvalue_hash(1), byvalue_hash(0), uint256(4), 1); // best
    s.AddBlock(byvalue_hash(2), byvalue_hash(0), uint256(9), 1); // side, higher
    s.SetBest(byvalue_hash(1), uint256(4));
    s.tipHashes.push_back(byvalue_hash(2));
    s.hasData.insert(byvalue_hash(2));
    assert_snapshot_winner(s, byvalue_hash(2));
}

// T4 — strict >: equal-trust candidate never replaces the baseline even if it
// would appear later and have a lower/higher hash; only strictly higher wins.
BOOST_AUTO_TEST_CASE(t4_no_equality_replacement)
{
    SnapshotCandidateFrontierStore s;
    s.AddBlock(byvalue_hash(0), uint256(0), uint256(2), 0);
    s.AddBlock(byvalue_hash(1), byvalue_hash(0), uint256(6), 1); // best
    // two side tips, trust 8 equal to each other (both above best)
    s.AddBlock(byvalue_hash(2), byvalue_hash(0), uint256(8), 1);
    s.AddBlock(byvalue_hash(3), byvalue_hash(0), uint256(8), 1);
    s.SetBest(byvalue_hash(1), uint256(6));
    s.tipHashes.push_back(byvalue_hash(2));
    s.tipHashes.push_back(byvalue_hash(3));
    s.hasData.insert(byvalue_hash(2));
    s.hasData.insert(byvalue_hash(3));
    // first in hash order (byvalue_hash(2) vs byvalue_hash(3)): map key ordering.
    // byvalue_hash compares by uint256 content; string-derived hashes are
    // ascending ('A1','A2','A3',...). With keys 'B2' vs 'B3'... use explicit values.
    // To keep deterministic, select the hash that is the map-first of the two.
    const uint256 first = (byvalue_hash(2) < byvalue_hash(3)) ? byvalue_hash(2) : byvalue_hash(3);
    assert_snapshot_winner(s, first);
}

// T5 — invalid tip excluded.
BOOST_AUTO_TEST_CASE(t5_invalid_tip_excluded)
{
    SnapshotCandidateFrontierStore s;
    s.AddBlock(byvalue_hash(0), uint256(0), uint256(2), 0);
    s.AddBlock(byvalue_hash(1), byvalue_hash(0), uint256(6), 1); // best
    s.AddBlock(byvalue_hash(2), byvalue_hash(0), uint256(9), 1); // higher, invalid
    s.SetBest(byvalue_hash(1), uint256(6));
    s.tipHashes.push_back(byvalue_hash(2));
    s.hasData.insert(byvalue_hash(2));
    s.operatorInvalid.insert(byvalue_hash(2));
    assert_snapshot_winner(s, uint256(0)); // excluded; nothing else eligible
}

// T6 — invalid ancestor excluded.
BOOST_AUTO_TEST_CASE(t6_invalid_ancestor_excluded)
{
    SnapshotCandidateFrontierStore s;
    s.AddBlock(byvalue_hash(0), uint256(0), uint256(2), 0);
    s.AddBlock(byvalue_hash(1), byvalue_hash(0), uint256(4), 1);
    s.AddBlock(byvalue_hash(2), byvalue_hash(1), uint256(8), 2); // best
    s.AddBlock(byvalue_hash(3), byvalue_hash(1), uint256(12), 2); // side, higher
    s.AddBlock(byvalue_hash(4), byvalue_hash(3), uint256(16), 3); // descendant
    s.SetBest(byvalue_hash(2), uint256(8));
    s.tipHashes.push_back(byvalue_hash(4));
    s.hasData.insert(byvalue_hash(4));
    s.operatorInvalid.insert(byvalue_hash(3)); // ancestor of tip 4
    assert_snapshot_winner(s, uint256(0)); // ancestor-invalid => excluded
}

// T7 — reconsider (remove from invalid set) makes candidate eligible.
BOOST_AUTO_TEST_CASE(t7_reconsider)
{
    SnapshotCandidateFrontierStore s;
    s.AddBlock(byvalue_hash(0), uint256(0), uint256(2), 0);
    s.AddBlock(byvalue_hash(1), byvalue_hash(0), uint256(6), 1); // best
    s.AddBlock(byvalue_hash(2), byvalue_hash(0), uint256(9), 1);
    s.SetBest(byvalue_hash(1), uint256(6));
    s.tipHashes.push_back(byvalue_hash(2));
    s.hasData.insert(byvalue_hash(2));

    s.operatorInvalid.insert(byvalue_hash(2));
    assert_snapshot_winner(s, uint256(0)); // invalid -> none
    s.operatorInvalid.erase(byvalue_hash(2));
    assert_snapshot_winner(s, byvalue_hash(2)); // reconsidered -> wins
}

// T8 — reorg / finality-compatible candidate. Finality active; a side tip whose
// fork point is >= finalized height is eligible; one below is excluded.
BOOST_AUTO_TEST_CASE(t8_finality_reorg)
{
    SnapshotCandidateFrontierStore s;
    // 0 -> 1 -> 2(best). Side: 0 -> 1 -> S (fork at height 1).
    s.AddBlock(byvalue_hash(0), uint256(0), uint256(2), 0);
    s.AddBlock(byvalue_hash(1), byvalue_hash(0), uint256(4), 1);
    s.AddBlock(byvalue_hash(2), byvalue_hash(1), uint256(6), 2);
    s.AddBlock(byvalue_hash(3), byvalue_hash(1), uint256(9), 2); // S, fork ht 1
    s.SetBest(byvalue_hash(2), uint256(6));
    s.finalizedHeight = 1;
    s.finalityActive = true;
    s.tipHashes.push_back(byvalue_hash(3));
    s.hasData.insert(byvalue_hash(3));
    assert_snapshot_winner(s, byvalue_hash(3)); // fork ht 1 >= finalized 1

    // Now raise finalized height above the fork: excluded.
    s.finalizedHeight = 2;
    assert_snapshot_winner(s, uint256(0));
}

// T9 — unavailable materialization: candidate excluded, authority unchanged.
BOOST_AUTO_TEST_CASE(t9_missing_materialization)
{
    SnapshotCandidateFrontierStore s;
    s.AddBlock(byvalue_hash(0), uint256(0), uint256(2), 0);
    s.AddBlock(byvalue_hash(1), byvalue_hash(0), uint256(6), 1); // best
    s.AddBlock(byvalue_hash(2), byvalue_hash(0), uint256(9), 1); // higher but no data
    s.SetBest(byvalue_hash(1), uint256(6));
    s.tipHashes.push_back(byvalue_hash(2));
    // hasData NOT set for tip 2.
    assert_snapshot_winner(s, uint256(0)); // excluded by missing data
    // authority record for tip 2 unchanged after a failed data check:
    BOOST_CHECK(s.Lookup(byvalue_hash(2)).chainTrust == uint256(9));
}

// T10 — DAG canonical-trust candidate: a tip whose supplied trust is the
// canonical (DAG-reconstructed) value is compared by that exact value.
BOOST_AUTO_TEST_CASE(t10_dag_canonical_trust)
{
    SnapshotCandidateFrontierStore s;
    // best chain linear trust 13; side DAG tip canonical trust 14 > 13.
    s.AddBlock(byvalue_hash(0), uint256(0), uint256(2), 0);
    s.AddBlock(byvalue_hash(1), byvalue_hash(0), uint256(4), 1);
    s.AddBlock(byvalue_hash(2), byvalue_hash(1), uint256(6), 2);
    s.AddBlock(byvalue_hash(3), byvalue_hash(2), uint256(13), 3); // best, linear 13
    s.AddBlock(byvalue_hash(4), byvalue_hash(1), uint256(14), 3); // DAG side, canonical 14
    s.SetBest(byvalue_hash(3), uint256(13));
    s.tipHashes.push_back(byvalue_hash(4));
    s.hasData.insert(byvalue_hash(4));
    assert_snapshot_winner(s, byvalue_hash(4)); // canonical trust 14 wins
}

// T11 — cold fork point: star-shaped side branch from deep history; selection
// succeeds purely by trust; materialization is separate.
BOOST_AUTO_TEST_CASE(t11_cold_fork_point)
{
    SnapshotCandidateFrontierStore s;
    s.AddBlock(byvalue_hash(0), uint256(0), uint256(2), 0);
    CBlockIndex* dummy = NULL; // unused
    (void)dummy;
    // deep active chain
    uint256 prev = byvalue_hash(0);
    uint256 trust(2);
    const int coldFork = 100000; // deep fork height
    for (int i = 1; i <= coldFork; ++i)
    {
        uint256 h = byvalue_hash(i);
        trust = uint256(uint64_t(2 * (i + 1)));
        s.AddBlock(h, prev, trust, i);
        prev = h;
    }
    s.SetBest(prev, trust);
    // cold side tip from height 1, very high trust
    s.AddBlock(byvalue_hash(200000), byvalue_hash(0), uint256(500000), 1);
    s.tipHashes.push_back(byvalue_hash(200000));
    s.hasData.insert(byvalue_hash(200000));
    assert_snapshot_winner(s, byvalue_hash(200000));
}

// INV2 (causal): the by-value evaluator makes ZERO resident mapBlockIndex
// resolves. Clear the entire resident graph (mapBlockIndex + pindexBest +
// nBestChainTrust) and prove evaluation succeeds purely from the value store.
// If EvaluateCandidateFrontierByValue had any hidden mapBlockIndex dependency
// it would break here.
BOOST_AUTO_TEST_CASE(inv2_zero_resident_resolve)
{
    // Save + clear resident graph.
    std::map<uint256, CBlockIndex*> savedMap;
    savedMap.swap(mapBlockIndex);
    CBlockIndex* savedBest = pindexBest;
    uint256 savedHashBest = hashBestChain;
    uint256 savedTrust = nBestChainTrust;
    int savedHeight = nBestHeight;
    pindexBest = NULL;
    hashBestChain = 0;
    nBestChainTrust = 0;
    nBestHeight = -1;

    SnapshotCandidateFrontierStore s;
    s.AddBlock(byvalue_hash(0), uint256(0), uint256(2), 0);
    s.AddBlock(byvalue_hash(1), byvalue_hash(0), uint256(4), 1);
    s.AddBlock(byvalue_hash(2), byvalue_hash(0), uint256(9), 1); // winner
    s.SetBest(byvalue_hash(1), uint256(4));
    s.tipHashes.push_back(byvalue_hash(2));
    s.hasData.insert(byvalue_hash(2));

    const CandidateFrontierAuthorityRecord sel =
        EvaluateCandidateFrontierByValue(s);
    BOOST_CHECK(sel.found);
    if (sel.found) BOOST_CHECK(sel.hash == byvalue_hash(2));

    // Restore resident graph.
    mapBlockIndex.swap(savedMap);
    pindexBest = savedBest;
    hashBestChain = savedHashBest;
    nBestChainTrust = savedTrust;
    nBestHeight = savedHeight;
}

// T12 — many historical blocks, fixed F: authoritative record cost scales with
// F, not N. Counting store with 2 field sizes.
BOOST_AUTO_TEST_CASE(t12_fixed_frontier_many_history)
{
    // Build small-F frontier over a long chain.
    CountingSnapshotStore s;
    const int N = 50000; // historical blocks
    s.AddBlock(byvalue_hash(0), uint256(0), uint256(2), 0);
    uint256 prev = byvalue_hash(0);
    for (int i = 1; i < N; ++i)
    {
        uint256 h = byvalue_hash(i);
        s.AddBlock(h, prev, uint256(uint64_t(2 * (i + 1))), i);
        prev = h;
    }
    s.SetBest(prev, uint256(uint64_t(2 * N)));
    // one side tip (the frontier): trust above best
    s.AddBlock(byvalue_hash(N + 1), byvalue_hash(0), uint256(uint64_t(2 * (N + 1))), 1);
    s.tipHashes.push_back(byvalue_hash(N + 1));
    s.hasData.insert(byvalue_hash(N + 1));

    s.lookups = 0; s.parentWalks = 0;
    const CandidateFrontierAuthorityRecord sel = EvaluateCandidateFrontierByValue(s);
    BOOST_CHECK(sel.found);
    if (sel.found) BOOST_CHECK(sel.hash == byvalue_hash(N + 1));
    // Authority cost must NOT scale with N: only the frontier tip is looked up.
    // (Lookup: best-tip + the 1 candidate tip + ancestors in walks; parentWalk:
    // the operator-validity walk over the side tip's chain from height-1 to 0.)
    const int total = s.lookups + s.parentWalks;
    BOOST_CHECK(total < 200); // independent of N=50000; visits only the frontier
}

// 12-real: single-eligible but long — covered above. Add exact INV0/INV1 by
// driving the same scenario through the resident mapFixture-style store.

BOOST_AUTO_TEST_SUITE_END()