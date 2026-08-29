#include <boost/test/unit_test.hpp>

#include "main.h"
#include "dag.h"

BOOST_AUTO_TEST_SUITE(dag_trust_restart_tests)

/**
 * RED test: prove restart nChainTrust != nDAGScore before repair.
 *
 * Creates: genesis (h=0) -> A (h=10, pre-DAG) -> B (h=11, post-DAG PoW)
 *
 * Simulates restart: B's nChainTrust = linear prefix (~40),
 * mapDAGData[B].nDAGScore = canonical DAG score (100).
 */
BOOST_AUTO_TEST_CASE(red_dag_trust_divergence_on_restart)
{
    // --- deterministic dummy hashes ---
    // Use uint256(uint64_t(..)) for simple numeric values; use
    // uint256(std::string("hex")) only where explicit hex is needed.
    uint256 hashGenesis = uint256(std::string("0000000000000000000000000000000000000000000000000000000000000001"));
    uint256 hashA       = uint256(std::string("000000000000000000000000000000000000000000000000000000000000000a"));
    uint256 hashB       = uint256(std::string("000000000000000000000000000000000000000000000000000000000000000b"));
    uint256 hashPreDAG  = uint256(std::string("0000000000000000000000000000000000000000000000000000000000000005"));
    uint256 hashPoS     = uint256(std::string("00000000000000000000000000000000000000000000000000000000000000cc"));

    // ---- save existing mapBlockIndex state, restore after test ---
    // The global TestingSetup creates genesis block; our test adds
    // its own heap blocks and must not leak or double-free.
    std::map<uint256, CBlockIndex*> savedMap;
    savedMap.swap(mapBlockIndex);

    // --- create minimal CBlockIndex objects ---
    CBlockIndex* pGenesis = new CBlockIndex();
    pGenesis->phashBlock  = new uint256(hashGenesis);
    pGenesis->nHeight     = 0;
    pGenesis->nChainTrust = uint256(uint64_t(2));
    pGenesis->nFlags      = 0; // PoW

    CBlockIndex* pA = new CBlockIndex();
    pA->phashBlock  = new uint256(hashA);
    pA->nHeight     = 10;
    pA->nChainTrust = uint256(uint64_t(20));
    pA->pprev       = pGenesis;
    pA->nFlags      = 0; // PoW

    CBlockIndex* pB = new CBlockIndex();
    pB->phashBlock  = new uint256(hashB);
    pB->nHeight     = 11; // >= FORK_HEIGHT_DAG (11) in regtest
    pB->nChainTrust = uint256(uint64_t(40)); // linear prefix: 20 + ~20
    pB->pprev       = pA;
    pB->nFlags      = 0; // PoW

    CBlockIndex* pPreDAG = new CBlockIndex();
    pPreDAG->phashBlock  = new uint256(hashPreDAG);
    pPreDAG->nHeight     = 8;  // pre-DAG
    pPreDAG->nChainTrust = uint256(uint64_t(16));
    pPreDAG->pprev       = pGenesis;
    pPreDAG->nFlags      = 0; // PoW

    CBlockIndex* pPostDAGPoS = new CBlockIndex();
    pPostDAGPoS->phashBlock  = new uint256(hashPoS);
    pPostDAGPoS->nHeight     = 12;
    pPostDAGPoS->nChainTrust = uint256(uint64_t(200));
    pPostDAGPoS->pprev       = pB;
    pPostDAGPoS->nFlags      = BLOCK_PROOF_OF_STAKE;

    // --- register in global mapBlockIndex ---
    mapBlockIndex[hashGenesis] = pGenesis;
    mapBlockIndex[hashA]       = pA;
    mapBlockIndex[hashB]       = pB;
    mapBlockIndex[hashPreDAG]  = pPreDAG;
    mapBlockIndex[hashPoS]     = pPostDAGPoS;

    // --- setup DAG data as RebuildDAGOrder would produce ---
    CBlockDAGData dGen;
    dGen.vDAGParents = {};
    dGen.fBlue       = true;
    dGen.nDAGScore   = uint256(uint64_t(2));
    g_dagManager.SetDAGDataForTest(hashGenesis, dGen);

    CBlockDAGData dA;
    dA.vDAGParents = {hashGenesis};
    dA.fBlue       = true;
    dA.nDAGScore   = uint256(uint64_t(20));
    g_dagManager.SetDAGDataForTest(hashA, dA);

    // B's canonical DAG score = 100 (e.g. multi-parent merge adds extra trust)
    CBlockDAGData dB;
    dB.vDAGParents = {hashA};
    dB.fBlue       = true;
    dB.nDAGScore   = uint256(uint64_t(100));
    g_dagManager.SetDAGDataForTest(hashB, dB);

    // ---- PRE-FIX: nChainTrust != nDAGScore (the bug) ----
    BOOST_CHECK(pB->nChainTrust != dB.nDAGScore);
    BOOST_CHECK(pB->nChainTrust == uint256(uint64_t(40)));

    // ---- APPLY REPAIR ----
    g_dagManager.RestoreDAGTrustIntoChainTrust();

    // ---- POST-FIX: nChainTrust == nDAGScore ----
    BOOST_CHECK(pB->nChainTrust == dB.nDAGScore);
    BOOST_CHECK(pB->nChainTrust == uint256(uint64_t(100)));

    // ---- PRE-DAG blocks unchanged ----
    BOOST_CHECK(pGenesis->nChainTrust == uint256(uint64_t(2)));
    BOOST_CHECK(pA->nChainTrust == uint256(uint64_t(20)));

    // ---- Pre-DAG block (no mapDAGData entry) unchanged ----
    BOOST_CHECK(pPreDAG->nChainTrust == uint256(uint64_t(16)));

    // ---- PoS block (no mapDAGData entry, PoS flag) unchanged ----
    BOOST_CHECK(pPostDAGPoS->nChainTrust == uint256(uint64_t(200)));

    // ---- Idempotent: calling again does nothing ----
    g_dagManager.RestoreDAGTrustIntoChainTrust();
    BOOST_CHECK(pB->nChainTrust == uint256(uint64_t(100)));

    // ---- CLEANUP: free our heap objects, restore mapBlockIndex ----
    for (auto& pair : mapBlockIndex) {
        delete pair.second->phashBlock;
        delete pair.second;
    }
    mapBlockIndex.clear();
    g_dagManager.ClearDAGDataForTest();
    // Restore the original map (with TestingSetup's genesis block)
    savedMap.swap(mapBlockIndex);
}

BOOST_AUTO_TEST_SUITE_END()