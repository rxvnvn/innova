#include <boost/test/unit_test.hpp>
#include "../blockindex_accessor.h"
#include "../main.h"
#include "../miner.h"
#include "../hooks.h"
#include "../wallet.h"
#include "../zkproof.h"

#include <algorithm>
#include <type_traits>
#include <vector>

extern CWallet* pwalletMain;

namespace {

static_assert(std::is_same<BlockIndexId, uint64_t>::value, "BlockIndexId must be uint64_t");
static_assert(BLOCK_INDEX_ID_INVALID == 0, "invalid BlockIndexId should be 0");

static void ExpectSnapshotMatchesIndex(const BlockIndexSnapshot& snap,
                                       const CBlockIndex* pindex,
                                       bool fExpectParent)
{
    BOOST_REQUIRE(pindex != NULL);
    BOOST_CHECK_EQUAL(snap.hash.ToString(), pindex->GetBlockHash().ToString());
    BOOST_CHECK_EQUAL(snap.hashPrev.ToString(), pindex->pprev ? pindex->pprev->GetBlockHash().ToString() : uint256(0).ToString());
    BOOST_CHECK_EQUAL(snap.hashNext.ToString(), pindex->pnext ? pindex->pnext->GetBlockHash().ToString() : uint256(0).ToString());
    BOOST_CHECK_EQUAL(snap.height, pindex->nHeight);
    BOOST_CHECK_EQUAL(snap.nFile, pindex->nFile);
    BOOST_CHECK_EQUAL(snap.nBlockPos, pindex->nBlockPos);
    BOOST_CHECK_EQUAL(snap.nFlags, pindex->nFlags);
    BOOST_CHECK_EQUAL(snap.nVersion, pindex->nVersion);
    BOOST_CHECK_EQUAL(snap.nTime, pindex->nTime);
    BOOST_CHECK_EQUAL(snap.nBits, pindex->nBits);
    BOOST_CHECK_EQUAL(snap.nNonce, pindex->nNonce);
    BOOST_CHECK_EQUAL(snap.nMint, pindex->nMint);
    BOOST_CHECK_EQUAL(snap.nMoneySupply, pindex->nMoneySupply);
    BOOST_CHECK_EQUAL(snap.nStakeModifier, pindex->nStakeModifier);
    BOOST_CHECK_EQUAL(snap.prevoutStake.ToString(), pindex->prevoutStake.ToString());
    BOOST_CHECK_EQUAL(snap.nStakeTime, pindex->nStakeTime);
    BOOST_CHECK_EQUAL(snap.hashProof.ToString(), pindex->hashProof.ToString());
    BOOST_CHECK_EQUAL(snap.nChainTrust.ToString(), pindex->nChainTrust.ToString());
    BOOST_CHECK_EQUAL(snap.fProofOfStake, pindex->IsProofOfStake());
    BOOST_CHECK_EQUAL(snap.fInMainChain, pindex->IsInMainChain());
    BOOST_CHECK_EQUAL(snap.hasParent, fExpectParent);
}

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

static CBlockIndex* MinePoWBlock(CBlockIndex* pindexPrev, unsigned int nExtra)
{
    CBlock* pblock = BuildPoWBlock(pindexPrev, nExtra);
    CBlockIndex* pindex = NULL;
    bool fOk = false;
    {
        LOCK(cs_main);
        const uint256 hash = pblock->GetHash();
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

static CBlockIndex* AddSidePoWBlock(CBlockIndex* pindexPrev, unsigned int nExtra)
{
    CBlock* pblock = BuildPoWBlock(pindexPrev, nExtra);
    CBlockIndex* pindex = NULL;
    bool fOk = false;
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

} // namespace

BOOST_AUTO_TEST_SUITE(blockindex_accessor_tests)

BOOST_AUTO_TEST_CASE(invalid_id_and_missing_hash_fail_cleanly)
{
    LegacyBlockIndexAccessor accessor;
    BlockIndexSnapshot snap;
    LOCK(cs_main);
    snap = accessor.ReadSnapshot(BLOCK_INDEX_ID_INVALID);
    BOOST_CHECK(!snap.found);
    snap = accessor.GetParent(BLOCK_INDEX_ID_INVALID);
    BOOST_CHECK(!snap.found);
    snap = accessor.GetAncestor(BLOCK_INDEX_ID_INVALID, 0);
    BOOST_CHECK(!snap.found);
    snap = accessor.FindFork(BLOCK_INDEX_ID_INVALID, BLOCK_INDEX_ID_INVALID);
    BOOST_CHECK(!snap.found);
    snap = accessor.LookupByHash(uint256(42));
    BOOST_CHECK(!snap.found);
    BOOST_CHECK(!accessor.Contains(uint256(42)));
}

BOOST_AUTO_TEST_CASE(real_chain_lookup_parent_ancestor_tip_and_height_match_direct_legacy)
{
    LOCK(cs_main);
    BOOST_REQUIRE(pindexBest != NULL);
    LegacyBlockIndexAccessor accessor;
    BlockIndexSnapshot tip = accessor.GetTip();
    BOOST_REQUIRE(tip.found);
    ExpectSnapshotMatchesIndex(tip, pindexBest, pindexBest->pprev != NULL);

    BlockIndexSnapshot genesis = accessor.GetActiveByHeight(0);
    BOOST_REQUIRE(genesis.found);
    ExpectSnapshotMatchesIndex(genesis, pindexGenesisBlock, false);

    std::vector<int> heights;
    heights.push_back(0);
    heights.push_back(std::min(1, pindexBest->nHeight));
    heights.push_back(std::max(0, pindexBest->nHeight / 2));
    heights.push_back(std::max(0, pindexBest->nHeight - 1));
    heights.push_back(pindexBest->nHeight);
    std::sort(heights.begin(), heights.end());
    heights.erase(std::unique(heights.begin(), heights.end()), heights.end());

    for (size_t i = 0; i < heights.size(); ++i)
    {
        CBlockIndex* pindex = FindBlockByHeight(heights[i]);
        BOOST_REQUIRE(pindex != NULL);
        BlockIndexSnapshot byHeight = accessor.GetActiveByHeight(heights[i]);
        BOOST_REQUIRE(byHeight.found);
        ExpectSnapshotMatchesIndex(byHeight, pindex, pindex->pprev != NULL);

        BlockIndexSnapshot byHash = accessor.LookupByHash(pindex->GetBlockHash());
        BOOST_REQUIRE(byHash.found);
        BOOST_CHECK_EQUAL(byHash.id, byHeight.id);
        ExpectSnapshotMatchesIndex(byHash, pindex, pindex->pprev != NULL);

        BlockIndexSnapshot reread = accessor.ReadSnapshot(byHash.id);
        BOOST_REQUIRE(reread.found);
        BOOST_CHECK_EQUAL(reread.hash.ToString(), byHash.hash.ToString());

        LegacyBlockIndexAccessor accessor2;
        BlockIndexSnapshot byHashAgain = accessor2.LookupByHash(pindex->GetBlockHash());
        BOOST_REQUIRE(byHashAgain.found);
        BOOST_CHECK_EQUAL(byHashAgain.id, byHash.id);

        if (pindex->pprev)
        {
            BlockIndexSnapshot parent = accessor.GetParent(byHash.id);
            BOOST_REQUIRE(parent.found);
            ExpectSnapshotMatchesIndex(parent, pindex->pprev, pindex->pprev->pprev != NULL);

            BlockIndexSnapshot sameHeight = accessor.GetAncestor(byHash.id, pindex->nHeight);
            BOOST_REQUIRE(sameHeight.found);
            BOOST_CHECK_EQUAL(sameHeight.hash.ToString(), pindex->GetBlockHash().ToString());

            BlockIndexSnapshot oldHeight = accessor.GetAncestor(byHash.id, std::max(0, pindex->nHeight / 2));
            BOOST_REQUIRE(oldHeight.found);
            BOOST_CHECK_EQUAL(oldHeight.hash.ToString(), pindex->GetAncestor(std::max(0, pindex->nHeight / 2))->GetBlockHash().ToString());
        }
    }
}

BOOST_AUTO_TEST_CASE(fork_point_matches_direct_legacy_on_synthetic_side_branch)
{
    BOOST_REQUIRE(CZKContext::Initialize());
    if (hooks == NULL)
        hooks = InitHook();

    CBlockIndex *pP1, *pP2, *pA3, *pA4, *pB1, *pB3;
    {
        LOCK(cs_main);
        pP1 = pP2 = pA3 = pA4 = pB1 = pB3 = NULL;
    }
    pP1 = MinePoWBlock(pindexBest, 0x511);
    pP2 = MinePoWBlock(pP1, 0x512);
    CBlockIndex* pA1 = MinePoWBlock(pP2, 0x521);
    CBlockIndex* pA2 = MinePoWBlock(pA1, 0x522);
    pA3 = MinePoWBlock(pA2, 0x523);
    pA4 = MinePoWBlock(pA3, 0x524);
    pB1 = AddSidePoWBlock(pP2, 0x531);
    CBlockIndex* pB2 = AddSidePoWBlock(pB1, 0x532);
    pB3 = AddSidePoWBlock(pB2, 0x533);

    LOCK(cs_main);
    LegacyBlockIndexAccessor accessor;
    BlockIndexSnapshot a4 = accessor.LookupByHash(pA4->GetBlockHash());
    BOOST_REQUIRE(a4.found);
    BlockIndexSnapshot b3 = accessor.LookupByHash(pB3->GetBlockHash());
    BOOST_REQUIRE(b3.found);
    BlockIndexSnapshot fork = accessor.FindFork(a4.id, b3.id);
    BOOST_REQUIRE(fork.found);
    BOOST_CHECK_EQUAL(fork.hash.ToString(), pP2->GetBlockHash().ToString());
    BOOST_CHECK_EQUAL(fork.height, pP2->nHeight);
    ExpectSnapshotMatchesIndex(fork, pP2, pP2->pprev != NULL);
}

BOOST_AUTO_TEST_SUITE_END()
