//
// Unit tests for block-chain checkpoints
//
#include <boost/foreach.hpp>
#include <boost/test/unit_test.hpp>

#include <vector>

#include "../checkpoints.h"
#include "../kernel.h"
#include "../main.h"
#include "../util.h"

using namespace std;

BOOST_AUTO_TEST_SUITE(Checkpoints_tests)

BOOST_AUTO_TEST_CASE(regtest_ignores_mainnet_hardened_checkpoint)
{
    // Regression test for the regtest checkpoint bug: the mainnet hardened
    // checkpoint at height 2000 was being applied during regtest because
    // CheckHardened() did not exclude fRegTest, so a regtest chain could not
    // grow past height 1999 (mining loop rejected every block at height 2000).
    const uint256 wrong("0x0000000000000000000000000000000000000000000000000000000000000001");

    // Sanity: 2000 IS a mainnet hardened checkpoint, so a wrong hash must be
    // rejected on mainnet. This also pins mainnet semantics against regress.
    {
        bool fRegSaved = fRegTest;
        bool fTestSaved = fTestNet;
        fRegTest = false;
        fTestNet = false;
        BOOST_CHECK(!Checkpoints::CheckHardened(2000, wrong));
        fRegTest = fRegSaved;
        fTestNet = fTestSaved;
    }

    // The fix: regtest must bypass hardened checkpoints entirely.
    {
        bool fRegSaved = fRegTest;
        bool fTestSaved = fTestNet;
        fRegTest = true;
        fTestNet = false;
        BOOST_CHECK(Checkpoints::CheckHardened(2000, wrong));
        BOOST_CHECK(Checkpoints::CheckHardened(5000, wrong));
        fRegTest = fRegSaved;
        fTestNet = fTestSaved;
    }
}

BOOST_AUTO_TEST_CASE(mainnet_hardened_checkpoint_semantics_unchanged)
{
    // Verify mainnet hardened-checkpoint behavior is untouched by the regtest
    // fix: the stored checkpoint hash at a known height must pass and any
    // other hash must fail.
    const uint256 h2000 = Checkpoints::mapCheckpoints[2000];
    const uint256 wrong("0x0000000000000000000000000000000000000000000000000000000000000001");
    BOOST_CHECK(h2000 != 0);
    BOOST_CHECK(h2000 != wrong);

    bool fRegSaved = fRegTest;
    bool fTestSaved = fTestNet;
    fRegTest = false;
    fTestNet = false;

    BOOST_CHECK(Checkpoints::CheckHardened(2000, h2000));
    BOOST_CHECK(!Checkpoints::CheckHardened(2000, wrong));
    // A height that is not a checkpoint must accept any hash.
    BOOST_CHECK(Checkpoints::CheckHardened(2001, wrong));

    fRegTest = fRegSaved;
    fTestNet = fTestSaved;
}

BOOST_AUTO_TEST_CASE(testnet_uses_own_checkpoint_map)
{
    // Testnet has no checkpoints beyond genesis; any non-genesis height must
    // accept any hash regardless of the mainnet table.
    const uint256 h0 = Checkpoints::mapCheckpointsTestnet[0];
    const uint256 wrong("0x0000000000000000000000000000000000000000000000000000000000000001");
    BOOST_CHECK(h0 != 0);

    bool fRegSaved = fRegTest;
    bool fTestSaved = fTestNet;
    fRegTest = false;
    fTestNet = true;

    BOOST_CHECK(Checkpoints::CheckHardened(0, h0));
    BOOST_CHECK(!Checkpoints::CheckHardened(0, wrong));
    BOOST_CHECK(Checkpoints::CheckHardened(2000, wrong));

    fRegTest = fRegSaved;
    fTestNet = fTestSaved;
}

BOOST_AUTO_TEST_CASE(regtest_ignores_stake_modifier_checkpoint)
{
    const unsigned int wrong = 0x12345678;
    bool fRegSaved = fRegTest;
    bool fTestSaved = fTestNet;

    fRegTest = false;
    fTestNet = false;
    BOOST_CHECK(!CheckStakeModifierCheckpoints(100000, wrong));

    fRegTest = true;
    fTestNet = false;
    BOOST_CHECK(CheckStakeModifierCheckpoints(100000, wrong));

    fRegTest = fRegSaved;
    fTestNet = fTestSaved;
}

BOOST_AUTO_TEST_CASE(ibd_check_sync_accepts_higher_trust_competing_branch)
{
    // Regression test for the synchronized-checkpoint fork trap.
    //
    // Scenario: the node is in IBD on competing branch F. The true main chain
    // M forks from F well below the auto-selected IBD checkpoint boundary. M
    // remains a lower-trust side chain until a candidate block makes it stronger.
    // CheckSync() must not prevent M from reaching that chain-selection point.
    //
    // Pre-fix CheckSync() rejects M@800: the AutoSelectSyncCheckpoint() boundary
    // is active tip - nCheckpointSpan (nCheckpointSpan = nCoinbaseMaturity*2).
    // The sanity check below asserts the boundary is actually at or above 800,
    // so the assertion is meaningful for any nCoinbaseMaturity in the process.

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

    // Active fork F: heights 0..TIP. Block times increase sharply with height
    // so the AutoSelectSyncCheckpoint time walk is false at every non-tip block
    // and the walk terminates at the height boundary (active tip - nCheckpointSpan).
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

    // Competing chain M forks from F@FORK_AT and extends to M_CAND_HEIGHT-1
    // while remaining no stronger than the active F tip.
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
    // Candidate M block at M_CAND_HEIGHT with parent pindexM @ M_CAND_HEIGHT-1.
    const uint256 hashMCand((uint64_t)(1000000 + M_CAND_HEIGHT));

    fRegTest = false;
    fTestNet = false;

    // Sanity: the pre-fix rejection geometry must actually reject M_CAND_HEIGHT
    // (i.e., the auto boundary must be at or above it). If this fails the test
    // would pass trivially and prove nothing.
    const CBlockIndex* pindexSync = Checkpoints::AutoSelectSyncCheckpoint();
    BOOST_CHECK(pindexSync != NULL);
    BOOST_CHECK(pindexSync->nHeight >= M_CAND_HEIGHT);

    // Old behavior rejects the first divergent M block, preventing this lower-
    // trust side chain from ever being indexed far enough to overtake F.
    const uint256 hashMFirst((uint64_t)(1000000 + FORK_AT + 1));
    BOOST_CHECK(Checkpoints::CheckSync(hashMFirst, vF[FORK_AT]));
    BOOST_CHECK(pindexM->nChainTrust <= pindexBest->nChainTrust);

    // A valid higher-trust candidate can make M stronger only if CheckSync lets
    // its ancestors and the candidate enter the block index.
    const uint256 nCandidateChainTrust = pindexM->nChainTrust + uint256((uint64_t)202);
    BOOST_CHECK(nCandidateChainTrust > pindexBest->nChainTrust);
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

BOOST_AUTO_TEST_CASE(synced_check_sync_preserves_stored_checkpoint_descendant_rule)
{
    // Pin the non-IBD (synced) CheckSync protection: the stored synchronized
    // checkpoint descendant rule must remain enforced exactly as before the
    // IBD fork-trap fix.

    const bool fRegSaved = fRegTest;
    const bool fTestSaved = fTestNet;
    const int nBestHeightSaved = nBestHeight;
    CBlockIndex* const pindexBestSaved = pindexBest;
    const uint256 hashBestChainSaved = hashBestChain;
    const uint256 hashSyncCheckpointSaved = Checkpoints::hashSyncCheckpoint;

    // Chain heights 6,749,996 .. 6,749,998 (common prefix below the checkpoint).
    std::vector<CBlockIndex*> vIndex;
    std::vector<uint256*> vHashes;
    CBlockIndex* pindexPrev = NULL;
    for (int h = 6749996; h <= 6749998; h++)
    {
        uint256* ph = new uint256((uint64_t)(2000000 + h));
        CBlockIndex* p = new CBlockIndex();
        p->phashBlock = ph;
        p->nHeight = h;
        p->nTime = 1200000000U + (unsigned int)h;
        p->pprev = pindexPrev;
        vIndex.push_back(p);
        vHashes.push_back(ph);
        pindexPrev = p;
    }
    CBlockIndex* pindexPrefix = pindexPrev; // @6,749,998

    // Stored synchronized checkpoint at 6,749,999.
    uint256* phSC = new uint256((uint64_t)(2000000 + 6749999));
    CBlockIndex* pindexSC = new CBlockIndex();
    pindexSC->phashBlock = phSC;
    pindexSC->nHeight = 6749999;
    pindexSC->nTime = 1200000000U + 6749999U;
    pindexSC->pprev = pindexPrefix;
    vIndex.push_back(pindexSC);
    vHashes.push_back(phSC);
    mapBlockIndex[*phSC] = pindexSC;

    // Descendant chain D through the checkpoint: 6,750,000 and 6,750,001.
    uint256* phD0 = new uint256((uint64_t)(2000000 + 6750000));
    CBlockIndex* pindexD0 = new CBlockIndex();
    pindexD0->phashBlock = phD0;
    pindexD0->nHeight = 6750000;
    pindexD0->nTime = 1200000000U + 6750000U;
    pindexD0->pprev = pindexSC;
    vIndex.push_back(pindexD0);
    vHashes.push_back(phD0);

    uint256* phD1 = new uint256((uint64_t)(2000000 + 6750001));
    CBlockIndex* pindexD1 = new CBlockIndex();
    pindexD1->phashBlock = phD1;
    pindexD1->nHeight = 6750001;
    pindexD1->nTime = 1200000000U + 6750001U;
    pindexD1->pprev = pindexD0;
    vIndex.push_back(pindexD1);
    vHashes.push_back(phD1);

    // Sibling chain S forking from the checkpoint's parent (not through the
    // checkpoint): 6,749,999 .. 6,750,001.
    std::vector<CBlockIndex*> vS(3);
    for (int h = 6749999; h <= 6750001; h++)
    {
        uint256* ph = new uint256((uint64_t)(3000000 + h));
        CBlockIndex* p = new CBlockIndex();
        p->phashBlock = ph;
        p->nHeight = h;
        p->nTime = 1200000000U + (unsigned int)h;
        p->pprev = (h == 6749999) ? pindexPrefix : vS[h - 6749999 - 1];
        vS[h - 6749999] = p;
        vIndex.push_back(p);
        vHashes.push_back(ph);
    }

    // Synced state: pindexBest and nBestHeight at 6,750,001 (past the last
    // hardened checkpoint), no peer ahead => IsInitialBlockDownload() false.
    pindexBest = pindexD1;
    nBestHeight = 6750001;
    hashBestChain = *pindexD1->phashBlock;
    Checkpoints::hashSyncCheckpoint = *phSC;

    fRegTest = false;
    fTestNet = false;

    BOOST_CHECK(Checkpoints::CheckSync(*phD0, pindexSC));                            // direct child passes
    BOOST_CHECK(!Checkpoints::CheckSync(uint256(88888888888ULL), pindexPrefix));     // same height, different hash fails
    BOOST_CHECK(!Checkpoints::CheckSync(*vS[1]->phashBlock, vS[0]));                 // non-descendant above fails
    BOOST_CHECK(!Checkpoints::CheckSync(*vS[2]->phashBlock, vS[1]));                 // non-descendant above (deeper) fails
    BOOST_CHECK(!Checkpoints::CheckSync(uint256(99999999999ULL), pindexPrefix->pprev)); // below height, unknown hash fails

    fRegTest = fRegSaved;
    fTestNet = fTestSaved;

    nBestHeight = nBestHeightSaved;
    pindexBest = pindexBestSaved;
    hashBestChain = hashBestChainSaved;
    Checkpoints::hashSyncCheckpoint = hashSyncCheckpointSaved;
    for (size_t i = 0; i < vHashes.size(); i++)
        mapBlockIndex.erase(*vHashes[i]);
    for (size_t i = 0; i < vHashes.size(); i++)
        delete vHashes[i];
    for (size_t i = 0; i < vIndex.size(); i++)
        delete vIndex[i];
}

BOOST_AUTO_TEST_SUITE_END()
