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

BOOST_AUTO_TEST_SUITE_END()
