//
// Unit tests for block-chain checkpoints
//
#include <boost/assign/list_of.hpp> // for 'map_list_of()'
#include <boost/test/unit_test.hpp>
#include <boost/foreach.hpp>

#include <vector>

#include "../checkpoints.h"
#include "../main.h"
#include "../net.h"
#include "../util.h"

using namespace std;

BOOST_AUTO_TEST_SUITE(Checkpoints_tests)

BOOST_AUTO_TEST_CASE(sanity)
{
    uint256 p11111 = uint256("0x0000000069e244f73d78e8fd29ba2fd2ed618bd6fa2ee92559f542fdb26e7c1d");
    uint256 p134444 = uint256("0x00000000000005b12ffd4cd315cd34ffd4a594f430ac814c91184a0d42d2b0fe");
    BOOST_CHECK(Checkpoints::CheckHardened(11111, p11111));
    BOOST_CHECK(Checkpoints::CheckHardened(134444, p134444));

    // Wrong hashes at checkpoints should fail:
    BOOST_CHECK(!Checkpoints::CheckHardened(11111, p134444));
    BOOST_CHECK(!Checkpoints::CheckHardened(134444, p11111));

    // ... but any hash not at a checkpoint should succeed:
    BOOST_CHECK(Checkpoints::CheckHardened(11111+1, p134444));
    BOOST_CHECK(Checkpoints::CheckHardened(134444+1, p11111));

    BOOST_CHECK(Checkpoints::GetTotalBlocksEstimate() >= 134444);
}

BOOST_AUTO_TEST_CASE(ibd_check_sync_accepts_higher_trust_competing_branch)
{
    const bool fRegSaved = fRegTest;
    const bool fTestSaved = fTestNet;
    const int nBestHeightSaved = nBestHeight;
    CBlockIndex* const pindexBestSaved = pindexBest;
    const uint256 hashBestChainSaved = hashBestChain;

    const int TIP = 1000;
    const int FORK_AT = 50;
    const int M_CAND_HEIGHT = 800;

    std::vector<CBlockIndex*> vIndex;
    std::vector<uint256*> vHashes;
    vIndex.reserve(TIP + M_CAND_HEIGHT + 100);
    vHashes.reserve(TIP + M_CAND_HEIGHT + 100);

    std::vector<CBlockIndex*> vF(TIP + 1);
    CBlockIndex* pindexPrev = NULL;
    for (int h = 0; h <= TIP; h++)
    {
        uint256* ph = new uint256((uint64_t)(h + 1));
        CBlockIndex* p = new CBlockIndex();
        p->phashBlock = ph;
        p->nHeight = h;
        p->nTime = 1200000000U + (unsigned int)h * 1000000U;
        p->pprev = pindexPrev;
        p->nChainTrust = uint256((uint64_t)(h + 1));
        vF[h] = p;
        vIndex.push_back(p);
        vHashes.push_back(ph);
        pindexPrev = p;
    }

    pindexBest = vF[TIP];
    nBestHeight = TIP;
    hashBestChain = *vF[TIP]->phashBlock;

    CBlockIndex* pindexM = vF[FORK_AT];
    for (int h = FORK_AT + 1; h <= M_CAND_HEIGHT - 1; h++)
    {
        uint256* ph = new uint256((uint64_t)(1000000 + h));
        CBlockIndex* p = new CBlockIndex();
        p->phashBlock = ph;
        p->nHeight = h;
        p->nTime = 1200000000U + (unsigned int)h * 1000000U;
        p->pprev = pindexM;
        p->nChainTrust = pindexM->nChainTrust + uint256((uint64_t)1);
        mapBlockIndex[*ph] = p;
        pindexM = p;
        vIndex.push_back(p);
        vHashes.push_back(ph);
    }
    const uint256 hashMCand((uint64_t)(1000000 + M_CAND_HEIGHT));

    fRegTest = false;
    fTestNet = false;

    const CBlockIndex* pindexSync = Checkpoints::AutoSelectSyncCheckpoint();
    BOOST_REQUIRE(pindexSync != NULL);
    BOOST_CHECK(pindexSync->nHeight >= M_CAND_HEIGHT);

    BOOST_CHECK(Checkpoints::CheckSync(hashMCand, pindexM));

    fRegTest = fRegSaved;
    fTestNet = fTestSaved;
    nBestHeight = nBestHeightSaved;
    pindexBest = pindexBestSaved;
    hashBestChain = hashBestChainSaved;
    for (size_t i = 0; i < vHashes.size(); i++)
        mapBlockIndex.erase(*vHashes[i]);
    for (size_t i = 0; i < vHashes.size(); i++)
        delete vHashes[i];
    for (size_t i = 0; i < vIndex.size(); i++)
        delete vIndex[i];
}

BOOST_AUTO_TEST_CASE(ibd_known_ahead_peer_height_is_conservative)
{
    BOOST_REQUIRE(pindexBest != NULL);
    const bool regSaved = fRegTest;
    const bool testnetSaved = fTestNet;
    const bool importingSaved = fImporting;
    const bool reindexSaved = fReindex;
    const int heightSaved = nBestHeight;
    std::vector<CNode*> nodesSaved;
    {
        LOCK(cs_vNodes);
        nodesSaved = vNodes;
        vNodes.clear();
    }

    CNode peer(INVALID_SOCKET, CAddress(CService("127.0.0.1", 0), NODE_NETWORK), "ibd-stale-height", true);
    fRegTest = false;
    fTestNet = false;
    fImporting = false;
    fReindex = false;
    nBestHeight = Checkpoints::GetTotalBlocksEstimate();
    peer.fClient = false;
    peer.nBestKnownHeight = nBestHeight + 1000;
    peer.nChainHeight = peer.nBestKnownHeight;
    peer.nLastHeightUpdate = GetTime() - 121;
    peer.nLastBlockRecv = GetTime();
    {
        LOCK(cs_vNodes);
        vNodes.push_back(&peer);
    }

    BOOST_CHECK(IsInitialBlockDownload());

    nBestHeight = peer.nBestKnownHeight;
    BOOST_CHECK(!IsInitialBlockDownload());

    {
        LOCK(cs_vNodes);
        vNodes = nodesSaved;
    }
    nBestHeight = heightSaved;
    fReindex = reindexSaved;
    fImporting = importingSaved;
    fTestNet = testnetSaved;
    fRegTest = regSaved;
}

BOOST_AUTO_TEST_SUITE_END()
