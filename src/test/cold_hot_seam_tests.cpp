#include <boost/test/unit_test.hpp>

#include "main.h"
#include "blockindex_accessor.h"

extern std::map<uint256, CBlockIndex*> mapBlockIndex;
extern CBlockIndex* pindexBest;
extern uint256 nBestChainTrust;
extern uint256 hashBestChain;
extern int nBestHeight;

BOOST_AUTO_TEST_SUITE(cold_hot_seam_tests)

static CBlockIndex* make_block(const uint256& hash, int height,
                                CBlockIndex* pprev, uint64_t trust,
                                CBlockIndex* pnext = NULL)
{
    CBlockIndex* p = new CBlockIndex();
    p->phashBlock  = new uint256(hash);
    p->nHeight     = height;
    p->nChainTrust = uint256(uint64_t(trust));
    p->pprev       = pprev;
    p->pnext       = pnext;
    p->nFlags      = 0;
    p->nStakeModifier = 0;
    p->nStakeModifierTime = 0;
    p->nStakeModifierChecksum = 0;
    p->nTime = 1296688602 + height * 100;
    return p;
}

struct SeamTestFixture
{
    LegacyBlockIndexAccessor accessor;
    std::map<uint256, CBlockIndex*> savedMap;

    SeamTestFixture()
    {
        ClearBlockIndexAccessorState();
        ClearFindBlockByHeightCache();
        savedMap.swap(mapBlockIndex);
    }

    ~SeamTestFixture()
    {
        for (auto& pair : mapBlockIndex) {
            delete pair.second->phashBlock;
            delete pair.second;
        }
        mapBlockIndex.clear();
        savedMap.swap(mapBlockIndex);
    }

    void set_best(CBlockIndex* p)
    {
        pindexBest = p;
        hashBestChain = *p->phashBlock;
        nBestChainTrust = p->nChainTrust;
        nBestHeight = p->nHeight;
    }
};

BOOST_FIXTURE_TEST_CASE(seam_navigation, SeamTestFixture)
{
    std::vector<CBlockIndex*> blocks;
    for (int i = 0; i < 10; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "seam-nav-%d", i);
        uint256 h = uint256(std::string(buf));
        CBlockIndex* p = make_block(h, i, i > 0 ? blocks.back() : NULL, uint64_t(2 + i * 2));
        if (i > 0) blocks.back()->pnext = p;
        blocks.push_back(p);
        mapBlockIndex[h] = p;
    }
    set_best(blocks[9]);

    LOCK(cs_main);

    // Debug: verify pindexBest is set
    if (pindexBest) {
        printf("DEBUG: pindexBest height=%d hash=%s\n", pindexBest->nHeight,
               pindexBest->GetBlockHash().ToString().substr(0,16).c_str());
    }

    // GetActiveByHeight on best chain
    BlockIndexSnapshot s = accessor.GetActiveByHeight(0);
    BOOST_CHECK(s.found);
    BOOST_CHECK(s.height == 0);
    BOOST_CHECK(s.fInMainChain);

    s = accessor.GetActiveByHeight(5);
    BOOST_CHECK(s.found);
    BOOST_CHECK(s.height == 5);
    BOOST_CHECK(s.fInMainChain);

    // Beyond tip
    s = accessor.GetActiveByHeight(10);
    BOOST_CHECK(!s.found);

    // Forward walk via GetActiveByHeight
    s = accessor.GetActiveByHeight(3);
    BOOST_REQUIRE(s.found);
    BOOST_CHECK(s.height == 3);

    s = accessor.GetActiveByHeight(s.height + 1);
    BOOST_REQUIRE(s.found);
    BOOST_CHECK(s.height == 4);

    s = accessor.GetActiveByHeight(s.height + 1);
    BOOST_REQUIRE(s.found);
    BOOST_CHECK(s.height == 5);

    // Backward walk via GetParent
    BlockIndexSnapshot tip = accessor.GetTip();
    BOOST_CHECK(tip.found);
    BOOST_CHECK(tip.height == 9);

    BlockIndexSnapshot current = tip;
    while (current.hasParent) {
        current = accessor.GetParent(current.id);
        BOOST_REQUIRE(current.found);
    }
    BOOST_CHECK(current.height == 0);

    // GetAncestor
    BlockIndexSnapshot anc = accessor.GetAncestor(tip.id, 3);
    BOOST_CHECK(anc.found);
    BOOST_CHECK(anc.height == 3);
}

BOOST_AUTO_TEST_SUITE_END()