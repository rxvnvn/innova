#include <boost/test/unit_test.hpp>
#include "../main.h"
#include <vector>

void ResetBlockIndexSkipStats();
uint64_t GetBlockIndexSkipStatsCalls();
uint64_t GetBlockIndexSkipStatsEdges();

BOOST_AUTO_TEST_SUITE(cblocklocator_tests)

namespace {

struct ExposedLocator : public CBlockLocator
{
    explicit ExposedLocator(const CBlockIndex* pindex) : CBlockLocator(pindex) {}
    const std::vector<uint256>& Hashes() const { return vHave; }
};

struct Chain
{
    std::vector<uint256> hashes;
    std::vector<CBlockIndex*> nodes;
    ~Chain() { for (size_t i = 0; i < nodes.size(); ++i) delete nodes[i]; }
};

Chain BuildLinear(int n)
{
    Chain c;
    c.hashes.reserve(n);
    c.nodes.reserve(n);
    CBlockIndex* prev = NULL;
    for (int i = 0; i < n; ++i)
    {
        c.hashes.push_back(uint256((uint64_t)(0x100000 + i)));
        CBlockIndex* p = new CBlockIndex();
        p->phashBlock = &c.hashes.back();
        p->pprev = prev;
        p->nHeight = i;
        p->BuildSkip();
        if (prev)
            prev->pnext = p;
        c.nodes.push_back(p);
        prev = p;
    }
    return c;
}

struct ForkTree
{
    std::vector<uint256> hashes;
    std::vector<CBlockIndex*> all;
    std::vector<CBlockIndex*> active;
    std::vector<CBlockIndex*> side;
    ~ForkTree() { for (size_t i = 0; i < all.size(); ++i) delete all[i]; }
};

ForkTree BuildForkTree(int activeLen, int forkHeight, int sideLen)
{
    ForkTree t;
    t.hashes.reserve(activeLen + sideLen + 8);
    t.active.reserve(activeLen);
    CBlockIndex* prev = NULL;
    for (int i = 0; i < activeLen; ++i)
    {
        t.hashes.push_back(uint256((uint64_t)(0x200000 + i)));
        CBlockIndex* p = new CBlockIndex();
        p->phashBlock = &t.hashes.back();
        p->pprev = prev;
        p->nHeight = i;
        p->BuildSkip();
        if (prev)
            prev->pnext = p;
        t.active.push_back(p);
        t.all.push_back(p);
        prev = p;
    }
    CBlockIndex* sidePrev = t.active[forkHeight];
    for (int i = 0; i < sideLen; ++i)
    {
        t.hashes.push_back(uint256((uint64_t)(0x300000 + i)));
        CBlockIndex* p = new CBlockIndex();
        p->phashBlock = &t.hashes.back();
        p->pprev = sidePrev;
        p->nHeight = sidePrev->nHeight + 1;
        p->BuildSkip();
        t.side.push_back(p);
        t.all.push_back(p);
        sidePrev = p;
    }
    return t;
}

// Authoritative retained-window topology (src/blockindex_authoritative_live.cpp
// ResolveAndRetainFullParent / MaterializeParentChainInto): a CONTIGUOUS chain
// whose LOWEST node - the residency floor - has no materialized ancestor
// (floor.pprev == NULL). Entries below the floor are never resident; ancestry
// there is served by value, not by pprev traversal (WALK = nMedianTimeSpan + 2
// = 13, floor = max(0, baseTipHeight - WALK)).
//
// pskip is deliberately NULL here (a legal conservative value: the walk then
// degenerates to exact pprev stepping) so that the GetAncestor walk contract is
// tested independently of the materializer's skip construction, which is covered
// by blockindex_authoritative_live_tests.
Chain BuildPartialWindow(int floorHeight, int count)
{
    Chain c;
    c.hashes.reserve(count);
    c.nodes.reserve(count);
    CBlockIndex* prev = NULL;
    for (int i = 0; i < count; ++i)
    {
        c.hashes.push_back(uint256((uint64_t)(0x400000 + i)));
        CBlockIndex* p = new CBlockIndex();
        p->phashBlock = &c.hashes.back();
        p->pprev = prev;
        p->nHeight = floorHeight + i;
        p->pnext = NULL;
        p->pskip = NULL;
        if (prev)
            prev->pnext = p;
        c.nodes.push_back(p);
        prev = p;
    }
    return c;
}

std::vector<uint256> LegacyLocatorReference(const CBlockIndex* pindex, uint64_t& hops)
{
    std::vector<uint256> out;
    hops = 0;
    int step = 1;
    while (pindex)
    {
        out.push_back(pindex->GetBlockHash());
        for (int i = 0; pindex && i < step; ++i)
        {
            pindex = pindex->pprev;
            ++hops;
        }
        if (out.size() > 10)
            step *= 2;
    }
    out.push_back(GetGenesisBlockHash());
    return out;
}

void CheckExact(const CBlockIndex* pindex)
{
    uint64_t legacyHops = 0;
    std::vector<uint256> legacy = LegacyLocatorReference(pindex, legacyHops);
    ExposedLocator loc(pindex);
    BOOST_CHECK(loc.Hashes() == legacy);
}

} // namespace

BOOST_AUTO_TEST_CASE(genesis_exact_equality_and_duplicate_genesis)
{
    Chain c = BuildLinear(1);
    uint64_t hops = 0;
    std::vector<uint256> legacy = LegacyLocatorReference(c.nodes[0], hops);
    ExposedLocator loc(c.nodes[0]);
    BOOST_CHECK_EQUAL(legacy.size(), (size_t)2);
    BOOST_CHECK(loc.Hashes() == legacy);
    BOOST_CHECK(legacy[0] == c.nodes[0]->GetBlockHash());
    BOOST_CHECK(legacy[1] == GetGenesisBlockHash());
}

BOOST_AUTO_TEST_CASE(active_chain_threshold_cases_exact)
{
    CheckExact(BuildLinear(2).nodes.back());
    CheckExact(BuildLinear(10).nodes.back());
    CheckExact(BuildLinear(11).nodes.back());
    CheckExact(BuildLinear(12).nodes.back());
    CheckExact(BuildLinear(32).nodes.back());
    CheckExact(BuildLinear(257).nodes.back());
}

BOOST_AUTO_TEST_CASE(intermediate_and_tip_exact)
{
    Chain c = BuildLinear(200);
    CheckExact(c.nodes[37]);
    CheckExact(c.nodes[100]);
    CheckExact(c.nodes[101]);
    CheckExact(c.nodes[102]);
    CheckExact(c.nodes[199]);
}

BOOST_AUTO_TEST_CASE(side_branch_same_height_and_reorg_style_exact)
{
    ForkTree t = BuildForkTree(40, 20, 12);
    CheckExact(t.active[20]);
    CheckExact(t.side[0]);
    CheckExact(t.side[5]);
    CheckExact(t.side.back());
    BOOST_CHECK_EQUAL(t.side[5]->nHeight, t.active[26]->nHeight);
    CheckExact(t.active.back());
}

BOOST_AUTO_TEST_CASE(large_chain_exact_and_sublinear_ancestor_work)
{
    Chain c = BuildLinear(100000);
    uint64_t legacyHops = 0;
    std::vector<uint256> legacy = LegacyLocatorReference(c.nodes.back(), legacyHops);
    ResetBlockIndexSkipStats();
    ExposedLocator loc(c.nodes.back());
    BOOST_CHECK(loc.Hashes() == legacy);
    BOOST_CHECK(GetBlockIndexSkipStatsCalls() > 0);
    BOOST_TEST_MESSAGE("legacyHops=" << legacyHops << " skipEdges=" << GetBlockIndexSkipStatsEdges() << " skipCalls=" << GetBlockIndexSkipStatsCalls());
    BOOST_CHECK(GetBlockIndexSkipStatsEdges() < legacyHops / 4);
}

// ---------------------------------------------------------------------------
// Authoritative retained-window (partial chain) coverage - F3/F4.
//
// RED before the repair: GetAncestor had no retained-floor/NULL guard, so a
// target >= 2 heights below the materialized floor dereferenced a NULL walker
// (SIGSEGV at main.cpp:4958); CBlockLocator::Set inherited the crash.
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(partial_window_getancestor_exact_and_null_below_floor)
{
    Chain w = BuildPartialWindow(45, 14); // heights 45..58, floor 45
    const CBlockIndex* tip = w.nodes.back();
    BOOST_REQUIRE_EQUAL(tip->nHeight, 58);
    BOOST_REQUIRE(w.nodes.front()->pprev == NULL); // the retained floor

    // In-window targets: the EXACT materialized ancestor.
    for (int t = 45; t <= 58; ++t)
    {
        const CBlockIndex* a = tip->GetAncestor(t);
        BOOST_REQUIRE_MESSAGE(a != NULL, "in-window target " << t << " must be materialized");
        BOOST_CHECK_EQUAL(a->nHeight, t);
        BOOST_CHECK(a->GetBlockHash() == w.nodes[t - 45]->GetBlockHash());
    }
    // floor - 1 and deeper: NULL - never a crash, never a wrong node.
    BOOST_CHECK(tip->GetAncestor(44) == NULL);
    BOOST_CHECK(tip->GetAncestor(33) == NULL);
    BOOST_CHECK(tip->GetAncestor(32) == NULL); // >= 2 below the floor: SIGSEGV before the fix
    BOOST_CHECK(tip->GetAncestor(0) == NULL);
    // Above the current height: NULL (pre-existing contract).
    BOOST_CHECK(tip->GetAncestor(59) == NULL);
}

BOOST_AUTO_TEST_CASE(partial_window_sizes_one_two_and_full13)
{
    const int sizes[] = {1, 2, 3, 13, 14};
    for (int si = 0; si < 5; ++si)
    {
        const int count = sizes[si];
        Chain w = BuildPartialWindow(45, count);
        const CBlockIndex* tip = w.nodes.back();
        for (int i = 0; i < count; ++i)
        {
            const CBlockIndex* a = tip->GetAncestor(45 + i);
            BOOST_REQUIRE_MESSAGE(a != NULL, "count=" << count << " target=" << (45 + i));
            BOOST_CHECK(a->GetBlockHash() == w.nodes[i]->GetBlockHash());
        }
        BOOST_CHECK(tip->GetAncestor(44) == NULL);
        BOOST_CHECK(tip->GetAncestor(0) == NULL);
    }
}

BOOST_AUTO_TEST_CASE(partial_window_locator_truncates_safely_at_floor)
{
    Chain w = BuildPartialWindow(45, 14); // heights 45..58
    const CBlockIndex* tip = w.nodes.back();

    ExposedLocator loc(tip); // SIGSEGV at main.cpp:4958 before the fix

    const std::vector<uint256>& v = loc.Hashes();
    BOOST_REQUIRE(!v.empty());
    // Consumer contract (main.h:2083-2108): first entry is the tip, the walk
    // terminates safely at the retained floor, genesis is appended last.
    BOOST_CHECK(v.front() == tip->GetBlockHash());
    BOOST_CHECK(v.back() == GetGenesisBlockHash());
    // Every emitted hash must be a materialized window node - never an ancestor
    // the materialization does not own (the F4 silent-wrong mode) - and heights
    // must descend strictly.
    int prevHeight = 1000000;
    for (size_t i = 0; i + 1 < v.size(); ++i)
    {
        int found = -1;
        for (int k = 0; k < 14; ++k)
            if (w.nodes[k]->GetBlockHash() == v[i]) { found = 45 + k; break; }
        BOOST_REQUIRE_MESSAGE(found >= 0, "locator emitted a non-materialized hash at index " << i);
        BOOST_CHECK(found < prevHeight);
        prevHeight = found;
    }
    BOOST_CHECK(v.size() <= 15); // window + appended genesis, never all-history
}

BOOST_AUTO_TEST_SUITE_END()
