// Test suite for operator block invalidation (invalidateblock / reconsiderblock).
//
// Semantics under test (all in regtest mode):
//   * setInvalidBlockHash stores ONLY explicitly invalidated hashes; descendants
//     are treated as invalid via ancestor walks (IsBlockOperatorInvalid).
//   * InvalidateBlock persists the set BEFORE rolling back the active chain, and
//     activates the best eligible alternative chain.
//   * ReconsiderBlock removes exactly one explicit hash and re-activates the best
//     eligible chain (which may reconnect the reconsidered branch).
//   * The invalidation set survives "restart" (DB round-trip and a real
//     LoadBlockIndex reload), and a crash between persisting the set and rolling
//     back the active chain is healed on startup by RecoverFromInvalidatedBestChain.
//
// The test suite shares one global TestingSetup (mock BDB, regtest, genesis-only
// chain when these tests run).  Blocks are mined at the regtest PoW limit
// (~50% of hashes valid), so a nonce search is trivial.  All mining stays below
// the regtest finality (10) and DAG (11) fork heights.
//
// Side-chain blocks cannot be delivered through ProcessBlock in this client: the
// sync-checkpoint weak-work gate (main.cpp ProcessBlock) rejects any block whose
// parent is not the current best chain when the chain has passed its last
// checkpoint.  Side chains are therefore added to the index directly via
// WriteToDisk + AddToBlockIndex (the same storage path ProcessBlock uses), and
// the operator-invalidation gates in AcceptBlock are exercised directly.

#include <boost/test/unit_test.hpp>

#include "main.h"
#include "miner.h"
#include "hooks.h"
#include "txdb.h"
#include "wallet.h"
#include "zkproof.h"

#include <algorithm>

extern CWallet* pwalletMain;

namespace invalidate_reconsider {

static CBlock* BuildPoWBlock(CBlockIndex* pindexPrev, unsigned int nExtra)
{
    CBlock* pblock = CreateNewBlock(pwalletMain, false, NULL, NULL);
    if (pblock == NULL)
        return NULL;

    pblock->nVersion = 1;
    pblock->nTime = std::max((unsigned int)GetTime(),
                             (unsigned int)(pindexPrev->GetMedianTimePast() + 1));
    pblock->hashPrevBlock = *pindexPrev->phashBlock;
    pblock->vtx[0].vin[0].scriptSig = CScript() << (pindexPrev->nHeight + 1) << nExtra;
    pblock->hashMerkleRoot = pblock->BuildMerkleTree();

    uint256 hashTarget = CBigNum().SetCompact(pblock->nBits).getuint256();
    while (pblock->GetHash() > hashTarget && pblock->nNonce < 0xffffffff)
        ++pblock->nNonce;
    return pblock;
}

// Mine a block that extends the current best chain through the full ProcessBlock
// pipeline (the positive control: valid best-chain extension).
static CBlockIndex* MinePoWBlock(CBlockIndex* pindexPrev, unsigned int nExtra)
{
    CBlock* pblock = BuildPoWBlock(pindexPrev, nExtra);
    CBlockIndex* pindex = NULL;
    bool fOk = false;
    {
        LOCK(cs_main);
        uint256 hash = pblock->GetHash();
        bool fProcessed = pblock->CheckBlock(true, true, true) && ProcessBlock(NULL, pblock);
        if (fProcessed)
        {
            fOk = true;
            pindex = mapBlockIndex[hash];
        }
    }
    delete pblock;
    BOOST_REQUIRE(fOk);
    BOOST_REQUIRE(pindex != NULL);
    return pindex;
}

// Add a side-chain block directly to the block index (WriteToDisk +
// AddToBlockIndex).  Used because ProcessBlock's sync-checkpoint weak-work gate
// rejects side-chain blocks at the minimum regtest difficulty.
static CBlockIndex* AddSidePoWBlock(CBlockIndex* pindexPrev, unsigned int nExtra)
{
    CBlock* pblock = BuildPoWBlock(pindexPrev, nExtra);
    unsigned int nFile, nBlockPos;
    bool fOk = false;
    CBlockIndex* pindex = NULL;
    {
        LOCK(cs_main);
        unsigned int nFile, nBlockPos;
        bool fWrote = pblock->WriteToDisk(nFile, nBlockPos);
        bool fAdded = fWrote && pblock->AddToBlockIndex(nFile, nBlockPos, pblock->GetHash());
        if (fAdded)
        {
            fOk = true;
            pindex = mapBlockIndex[pblock->GetHash()];
        }
    }
    delete pblock;
    BOOST_REQUIRE(fOk);
    BOOST_REQUIRE(pindex != NULL);
    return pindex;
}

// Attempt to extend pindexPrev via AcceptBlock directly, bypassing ProcessBlock's
// checkpoint gate so the operator-invalidation gate in AcceptBlock is the only
// gate that can reject the block.
static bool TryAcceptPoWBlock(CBlockIndex* pindexPrev, unsigned int nExtra)
{
    CBlock* pblock = BuildPoWBlock(pindexPrev, nExtra);
    bool fOk = false;
    {
        LOCK(cs_main);
        unsigned int nFile, nBlockPos;
        if (pblock->WriteToDisk(nFile, nBlockPos))
            fOk = pblock->AcceptBlock();
    }
    delete pblock;
    return fOk;
}

static bool IsAncestorOfTip(const CBlockIndex* pindex)
{
    for (const CBlockIndex* p = pindexBest; p != NULL; p = p->pprev)
        if (p == pindex)
            return true;
    return false;
}

static uint256 BestChainHash()
{
    return *pindexBest->phashBlock;
}

static std::string HashStr(const CBlockIndex* pindex)
{
    return pindex ? pindex->GetBlockHash().ToString() : "<null>";
}

static std::string HashStr(const uint256& hash)
{
    return hash.ToString();
}

// The block at nHeight that is NOT an ancestor of the current best chain
// (i.e. a side-chain block at that height).
static CBlockIndex* FindSideChainBlockAtHeight(int nHeight)
{
    for (std::map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.begin();
         mi != mapBlockIndex.end(); ++mi)
    {
        CBlockIndex* pindex = mi->second;
        if (pindex->nHeight == nHeight && !IsAncestorOfTip(pindex))
            return pindex;
    }
    return NULL;
}

} // namespace invalidate_reconsider

using namespace invalidate_reconsider;

BOOST_AUTO_TEST_SUITE(invalidate_reconsider_tests)

// Invalidate a block on an inactive side chain: the active chain is untouched,
// the set is persisted, descendants become (implicitly) invalid, and
// reconsidering the block restores eligibility without forcing an activation.
BOOST_AUTO_TEST_CASE(inactive_side_chain_invalidation)
{
    BOOST_REQUIRE(CZKContext::Initialize());
    if (hooks == NULL)
        hooks = InitHook();  // needed by ConnectBlock at height >= RELEASE_HEIGHT (0 in regtest)
    {
        // Build the fork.  P2 is the fork parent; A wins by trust (4 blocks), B is
    // a side chain (3 blocks).
    CBlockIndex* pP1 = MinePoWBlock(pindexBest, 0x101);
    CBlockIndex* pP2 = MinePoWBlock(pP1, 0x102);
    CBlockIndex* pA1 = MinePoWBlock(pP2, 0x201);
    CBlockIndex* pA2 = MinePoWBlock(pA1, 0x202);
    CBlockIndex* pA3 = MinePoWBlock(pA2, 0x203);
    CBlockIndex* pA4 = MinePoWBlock(pA3, 0x204);
    CBlockIndex* pB1 = AddSidePoWBlock(pP2, 0x301);
    CBlockIndex* pB2 = AddSidePoWBlock(pB1, 0x302);
    CBlockIndex* pB3 = AddSidePoWBlock(pB2, 0x303);

    BOOST_CHECK_EQUAL(pA4->nHeight, pB3->nHeight + 1);
    BOOST_CHECK_EQUAL(HashStr(BestChainHash()), HashStr(pA4));

    std::string strError;
    BOOST_CHECK(InvalidateBlock(*pB1->phashBlock, strError));
    BOOST_CHECK_EQUAL(setInvalidBlockHash.count(*pB1->phashBlock), (size_t)1);
    BOOST_CHECK(IsBlockOperatorInvalid(pB3));  // implicit via B1
    BOOST_CHECK(!IsBlockOperatorInvalid(pA4));
    BOOST_CHECK_EQUAL(HashStr(BestChainHash()), HashStr(pA4));  // active chain unchanged

    // A new block built on the invalidated side chain is rejected by the
    // operator-invalidation gate in AcceptBlock (not added to the index).
    BOOST_CHECK(!TryAcceptPoWBlock(pB3, 0x401));

    // Reconsidering restores eligibility; the lower-trust side chain stays side.
    BOOST_CHECK(ReconsiderBlock(*pB1->phashBlock, strError));
    BOOST_CHECK(setInvalidBlockHash.empty());
    BOOST_CHECK(!IsBlockOperatorInvalid(pB3));
    BOOST_CHECK_EQUAL(HashStr(BestChainHash()), HashStr(pA4));
    }
}

// Invalidate a block on the ACTIVE chain: the chain is rolled back to the
// block's parent and the best eligible alternative (the previously inactive
// side chain) is activated.  Reconsidering re-activates the longer branch.
BOOST_AUTO_TEST_CASE(active_chain_rollback_and_best_alternative)
{
    // Reuse the fork built by inactive_side_chain_invalidation: best = A4,
    // side chain B1..B3 (lower trust) still present.
    CBlockIndex* pA1 = FindBlockByHeight(3);
    CBlockIndex* pA4 = FindBlockByHeight(6);
    CBlockIndex* pB3 = FindSideChainBlockAtHeight(5);
    BOOST_REQUIRE(pA1 != NULL);
    BOOST_REQUIRE(pA4 != NULL);
    BOOST_REQUIRE(pB3 != NULL);
    BOOST_CHECK(IsAncestorOfTip(pA1));

    std::string strError;
    BOOST_CHECK(InvalidateBlock(*pA1->phashBlock, strError));
    BOOST_CHECK_EQUAL(setInvalidBlockHash.count(*pA1->phashBlock), (size_t)1);
    BOOST_CHECK(IsBlockOperatorInvalid(pA4));
    // The best eligible alternative is now the B chain.
    BOOST_CHECK_EQUAL(HashStr(BestChainHash()), HashStr(pB3));
    BOOST_CHECK(!IsAncestorOfTip(pA1));

    // While A1 is invalid, extending A (an ancestor-invalidated chain) is rejected.
    BOOST_CHECK(!TryAcceptPoWBlock(pA4, 0x402));

    // Reconsidering A1 makes the longer A branch best eligible again.
    BOOST_CHECK(ReconsiderBlock(*pA1->phashBlock, strError));
    BOOST_CHECK(setInvalidBlockHash.empty());
    BOOST_CHECK_EQUAL(HashStr(BestChainHash()), HashStr(pA4));
    BOOST_CHECK(IsAncestorOfTip(pA1));
}

BOOST_AUTO_TEST_CASE(restart_persistence_and_crash_window_heal)
{
    // State: best = A4, side chain B1..B3.  Invalidate A1 -> consistent rollback.
    CBlockIndex* pA1 = FindBlockByHeight(3);
    CBlockIndex* pP2 = pA1->pprev;
    CBlockIndex* pA4 = FindBlockByHeight(6);
    uint256 hashA4 = *pA4->phashBlock;
    BOOST_REQUIRE(pA1 != NULL);
    BOOST_REQUIRE(pP2 != NULL);
    BOOST_REQUIRE(pA4 != NULL);

    std::string strError;

    // InvalidateBlock persists the set and rolls the best chain back, then
    // activates the best eligible alternative (the side chain).  A subsequent
    // ReconsiderBlock re-activates the longer branch, cleanly disconnecting the
    // side chain again.
    BOOST_CHECK(InvalidateBlock(*pA1->phashBlock, strError));
    BOOST_CHECK_EQUAL(setInvalidBlockHash.count(*pA1->phashBlock), (size_t)1);

    // Persistence round-trip through a fresh DB handle (simulated restart).
    {
        CTxDB txdb;
        std::set<uint256> setLoaded;
        BOOST_CHECK(txdb.ReadInvalidBlockSet(setLoaded));
        BOOST_CHECK_EQUAL(setLoaded.count(*pA1->phashBlock), (size_t)1);
    }

    BOOST_CHECK(ReconsiderBlock(*pA1->phashBlock, strError));
    BOOST_CHECK(setInvalidBlockHash.empty());
    BOOST_CHECK_EQUAL(HashStr(BestChainHash()), HashStr(hashA4));

    // Craft the crash window directly: the invalid set is persisted but the
    // active chain still descends from the invalidated block (a crash between
    // persisting the set and rolling back the chain).  InvalidateBlock's own
    // rollback is deliberately not used here so the side chain stays untouched.
    {
        LOCK(cs_main);
        setInvalidBlockHash.clear();
        setInvalidBlockHash.insert(*pA1->phashBlock);
        CTxDB txdb;
        txdb.WriteInvalidBlockSet(setInvalidBlockHash);
        txdb.WriteHashBestChain(hashA4);
        pindexBest = mapBlockIndex[hashA4];
        nBestHeight = pindexBest->nHeight;
        nBestChainTrust = pindexBest->nChainTrust;
    }

    // The set is readable from a fresh handle.
    {
        CTxDB txdb;
        std::set<uint256> setLoaded;
        BOOST_CHECK(txdb.ReadInvalidBlockSet(setLoaded));
        BOOST_CHECK_EQUAL(setLoaded.count(*pA1->phashBlock), (size_t)1);
    }

    // Healing rolls the stale best chain back to the highest valid ancestor and
    // re-persists the corrected best-chain pointer.
    BOOST_CHECK(RecoverFromInvalidatedBestChain());
    {
        CTxDB txdb;
        uint256 hashStored;
        BOOST_CHECK(txdb.ReadHashBestChain(hashStored));
        BOOST_CHECK_EQUAL(HashStr(hashStored), HashStr(pP2));
    }
    BOOST_CHECK_EQUAL(HashStr(BestChainHash()), HashStr(pP2));
    BOOST_CHECK_EQUAL(setInvalidBlockHash.count(*pA1->phashBlock), (size_t)1);

    // Restore a fully consistent, invalid-free state for the rest of the suite.
    BOOST_CHECK(ReconsiderBlock(*pA1->phashBlock, strError));
    BOOST_CHECK(setInvalidBlockHash.empty());
    BOOST_CHECK_EQUAL(HashStr(BestChainHash()), HashStr(hashA4));
}

// Multiple explicit invalidations (ancestor and descendant branches together)
// behave independently: reconsidering one leaves the other in force, and
// invalidating both competing chains leaves the chain resting at the fork.
BOOST_AUTO_TEST_CASE(multi_invalidate_and_error_cases)
{
    CBlockIndex* pA1 = FindBlockByHeight(3);
    CBlockIndex* pA2 = FindBlockByHeight(4);
    CBlockIndex* pA4 = FindBlockByHeight(6);
    CBlockIndex* pB1 = FindSideChainBlockAtHeight(3);
    BOOST_REQUIRE(pA1 != NULL);
    BOOST_REQUIRE(pA2 != NULL);
    BOOST_REQUIRE(pA4 != NULL);
    BOOST_REQUIRE(pB1 != NULL);

    std::string strError;

    // Error cases first (no state change).
    BOOST_CHECK(!InvalidateBlock(uint256("00"), strError));
    BOOST_CHECK_EQUAL(strError, "Block not found");
    BOOST_CHECK(!InvalidateBlock(*pindexGenesisBlock->phashBlock, strError));
    BOOST_CHECK_EQUAL(strError, "The genesis block cannot be invalidated");
    BOOST_CHECK(!ReconsiderBlock(uint256("00"), strError));
    BOOST_CHECK(setInvalidBlockHash.empty());

    // Invalidate the active branch (A2) then the other branch (B1).  With both
    // branches invalidated the best eligible set is empty and the chain rests at
    // the fork parent (P2 -> best block at height 2).
    BOOST_CHECK(InvalidateBlock(*pA2->phashBlock, strError));
    BOOST_CHECK(InvalidateBlock(*pB1->phashBlock, strError));
    BOOST_CHECK_EQUAL(setInvalidBlockHash.count(*pA2->phashBlock), (size_t)1);
    BOOST_CHECK_EQUAL(setInvalidBlockHash.count(*pB1->phashBlock), (size_t)1);
    BOOST_CHECK(IsBlockOperatorInvalid(pA4));  // via A2
    BOOST_CHECK_EQUAL(HashStr(BestChainHash()), HashStr(pA1->pprev));

    // Reconsidering A2 alone leaves B1 invalid; A becomes best eligible again.
    BOOST_CHECK(ReconsiderBlock(*pA2->phashBlock, strError));
    BOOST_CHECK_EQUAL(setInvalidBlockHash.count(*pA2->phashBlock), (size_t)0);
    BOOST_CHECK_EQUAL(setInvalidBlockHash.count(*pB1->phashBlock), (size_t)1);
    BOOST_CHECK(!IsBlockOperatorInvalid(pA4));
    BOOST_CHECK_EQUAL(HashStr(BestChainHash()), HashStr(pA4));

    // Reconsidering B1 clears the set entirely and restores eligibility.
    BOOST_CHECK(ReconsiderBlock(*pB1->phashBlock, strError));
    BOOST_CHECK(setInvalidBlockHash.empty());
    BOOST_CHECK(!IsBlockOperatorInvalid(pB1));
    BOOST_CHECK_EQUAL(HashStr(BestChainHash()), HashStr(pA4));

    // Idempotency: reconsidering a block that is not invalidated succeeds.
    BOOST_CHECK(ReconsiderBlock(*pA4->phashBlock, strError));
    BOOST_CHECK(setInvalidBlockHash.empty());
}

BOOST_AUTO_TEST_SUITE_END()
