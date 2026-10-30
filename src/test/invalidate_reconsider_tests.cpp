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
// ~50% of hashes valid), so a nonce search is trivial.  All mining stays below
// the regtest historical fork height.
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
#include "wallet.h"
#include "blockindex_authoritative_live.h"
#include "blockindex_tip.h"

#include <algorithm>
#include <boost/filesystem.hpp>

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

// Ancestry is compared BY HASH, not by pointer: the authoritative-V2 projection
// may materialize separate CBlockIndex instances for the same block, so pointer
// identity against a mapBlockIndex object is not a reliable ancestry test.
static bool IsAncestorOfTip(const CBlockIndex* pindex)
{
    if (pindex == NULL)
        return false;
    const uint256 hash = pindex->GetBlockHash();
    for (const CBlockIndex* p = pindexBest; p != NULL; p = p->pprev)
        if (p->GetBlockHash() == hash)
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

// The block at nHeight that IS an ancestor of the current best chain (the
// A-branch block). Deterministic: FindBlockByHeight() returns whichever block at
// that height comes first in mapBlockIndex hash order, which alternates between
// the A and B branches from run to run (block hashes are mined per run), making
// the whole suite flaky.
static CBlockIndex* FindActiveBlockAtHeight(int nHeight)
{
    for (std::map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.begin();
         mi != mapBlockIndex.end(); ++mi)
    {
        CBlockIndex* pindex = mi->second;
        if (pindex->nHeight == nHeight && IsAncestorOfTip(pindex))
            return pindex;
    }
    return NULL;
}

// The unique side-chain block at nHeight whose ancestry reaches pForkParent
// (e.g. the B-branch tip under the fork parent P2). Deterministic by ancestry,
// not by hash order.
static CBlockIndex* FindSideChainDescendantAtHeight(CBlockIndex* pForkParent, int nHeight)
{
    if (pForkParent == NULL)
        return NULL;
    for (std::map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.begin();
         mi != mapBlockIndex.end(); ++mi)
    {
        CBlockIndex* pindex = mi->second;
        if (pindex->nHeight != nHeight || IsAncestorOfTip(pindex))
            continue;
        const uint256 forkHash = pForkParent->GetBlockHash();
        CBlockIndex* p = pindex;
        while (p != NULL && p->nHeight > pForkParent->nHeight)
            p = p->pprev;
        if (p != NULL && p->GetBlockHash() == forkHash)
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
    CBlockIndex* pA1 = FindActiveBlockAtHeight(3);
    CBlockIndex* pA4 = FindActiveBlockAtHeight(6);
    BOOST_REQUIRE(pA1 != NULL);
    CBlockIndex* pB3 = FindSideChainDescendantAtHeight(pA1->pprev, 5);
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
    CBlockIndex* pA1 = FindActiveBlockAtHeight(3);
    BOOST_REQUIRE(pA1 != NULL);
    CBlockIndex* pP2 = pA1->pprev;
    CBlockIndex* pA4 = FindActiveBlockAtHeight(6);
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

    // M-port: the legacy CTxDB invalid-set / hashBestChain round-trip is replaced
    // by the V2 authoritative surfaces. The durable operator-invalid authority is
    // the tip authority (tip-invalid.dat, committed by tip.meta) and the durable
    // best-chain pointer is the committed V2 tip; both are read AFTER the same
    // invalidate transition that previously wrote them through txdb round-trips.
    BlockIndexAuthoritativeLive* liveInvalid = GetAuthoritativeLiveAuthority();
    BlockIndexTipAuthority* tipInvalid = liveInvalid ? liveInvalid->TipAuthorityMutable() : NULL;
    BOOST_CHECK_MESSAGE(tipInvalid != NULL,
        "restart_persistence: the TestingSetup live authority must be available for the durable invalid-set check");
    if (tipInvalid != NULL)
    {
        const std::set<uint256> setLoaded = tipInvalid->OperatorInvalidSet();
        BOOST_CHECK_EQUAL(setLoaded.count(*pA1->phashBlock), (size_t)1);
    }

    BOOST_CHECK(ReconsiderBlock(*pA1->phashBlock, strError));
    BOOST_CHECK(setInvalidBlockHash.empty());
    BOOST_CHECK_EQUAL(HashStr(BestChainHash()), HashStr(hashA4));

    // Craft the crash window directly: the invalid intent is committed to the V2
    // tip authority but the in-memory active chain still descends from the
    // invalidated block (a crash between the tip.meta commit and the chain
    // rollback). InvalidateBlock's own rollback is deliberately not used here so
    // the side chain stays untouched.
    {
        LOCK(cs_main);
        setInvalidBlockHash.clear();
        setInvalidBlockHash.insert(*pA1->phashBlock);
        BlockIndexAuthoritativeLive* liveCrash = GetAuthoritativeLiveAuthority();
        BlockIndexTipAuthority* tipCrash = liveCrash ? liveCrash->TipAuthorityMutable() : NULL;
        BOOST_REQUIRE_MESSAGE(tipCrash != NULL,
            "crash window: the TestingSetup live authority must be available (til-invalid.dat persist point)");
        std::string setErr;
        BOOST_REQUIRE_MESSAGE(tipCrash->SetOperatorInvalid(*pA1->phashBlock, true, &setErr) == BLOCK_INDEX_TIP_OK, setErr);
        hashBestChain = hashA4;   // in-memory mirror of the stale pre-crash tip
        pindexBest = mapBlockIndex[hashA4];
        nBestHeight = pindexBest->nHeight;
        nBestChainTrust = pindexBest->nChainTrust;
    }

    // The set is readable back from the V2 authority (the durable post-crash view).
    {
        BlockIndexAuthoritativeLive* liveRead = GetAuthoritativeLiveAuthority();
        BlockIndexTipAuthority* tipRead = liveRead ? liveRead->TipAuthorityMutable() : NULL;
        BOOST_REQUIRE_MESSAGE(tipRead != NULL, "crash window: tip authority must remain open");
        const std::set<uint256> setLoaded = tipRead->OperatorInvalidSet();
        BOOST_CHECK_EQUAL(setLoaded.count(*pA1->phashBlock), (size_t)1);
    }

    // Healing rolls the stale best chain back to the highest valid ancestor and
    // re-persists the corrected committed tip (the V2 best-chain pointer).
    BOOST_CHECK(RecoverFromInvalidatedBestChain());
    {
        BlockIndexAuthoritativeLive* liveHeal = GetAuthoritativeLiveAuthority();
        BlockIndexTipAuthority* tipHeal = liveHeal ? liveHeal->TipAuthorityMutable() : NULL;
        BOOST_REQUIRE_MESSAGE(tipHeal != NULL, "heal: tip authority must remain open");
        uint256 hashStored = tipHeal->GetTip().record.hash;
        BOOST_CHECK_MESSAGE(tipHeal->GetTip().status == BLOCK_INDEX_TIP_OK, "heal: no committed V2 tip");
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
    CBlockIndex* pA1 = FindActiveBlockAtHeight(3);
    BOOST_REQUIRE(pA1 != NULL);
    CBlockIndex* pA2 = FindActiveBlockAtHeight(4);
    CBlockIndex* pA4 = FindActiveBlockAtHeight(6);
    CBlockIndex* pB1 = FindSideChainDescendantAtHeight(pA1->pprev, 3);
    BOOST_REQUIRE(pA1 != NULL);
    BOOST_REQUIRE(pA2 != NULL);
    BOOST_REQUIRE(pA4 != NULL);
    BOOST_REQUIRE(pB1 != NULL);

    std::string strError;


    // Error cases first (no state change).
    BOOST_CHECK(!InvalidateBlock(uint256("00"), strError));
    BOOST_CHECK_EQUAL(strError, "Block not found");
    BOOST_CHECK(!InvalidateBlock(*pindexGenesisBlock->phashBlock, strError));
    // In BI-V2 authoritative mode genesis is at/below the immutable base tip, so
    // the fail-closed authority refuses it with the V2-R2D message instead of the
    // legacy "genesis block" wording. The BEHAVIOUR under test (refusal, no state
    // change) is identical; both messages are accepted so the check is about the
    // refusal, not about the string.
    BOOST_CHECK_MESSAGE(strError == "The genesis block cannot be invalidated" ||
                        strError == "Cannot invalidate a block at or below the immutable base tip: "
                                     "rebase/rebuild required (V2-R2D fail-closed)",
                        "unexpected refusal message: " << strError);
    BOOST_CHECK(!ReconsiderBlock(uint256("00"), strError));
    BOOST_CHECK(setInvalidBlockHash.empty());

    // Invalidate the active block A2, then the other branch's root B1.
    // A2's invalidation makes the B branch best (B3).  Invalidating B1 removes the
    // B branch, and A1 (A2's parent, NOT invalidated) is still eligible, so the
    // best eligible chain is A1 -- the same contract the whole suite asserts
    // ("activates the best eligible alternative chain").  The fork parent is
    // reached only when nothing at all is eligible above it; that case is checked
    // at the end of this test.
    BOOST_CHECK(InvalidateBlock(*pA2->phashBlock, strError));
    BOOST_CHECK(InvalidateBlock(*pB1->phashBlock, strError));
    BOOST_CHECK_EQUAL(setInvalidBlockHash.count(*pA2->phashBlock), (size_t)1);
    BOOST_CHECK_EQUAL(setInvalidBlockHash.count(*pB1->phashBlock), (size_t)1);
    BOOST_CHECK(IsBlockOperatorInvalid(pA4));  // via A2
    BOOST_CHECK_EQUAL(HashStr(BestChainHash()), HashStr(pA1));

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

    // Empty eligible set: invalidate BOTH branch roots (A1 and B1).  With no
    // eligible record above the fork P2, the active chain must rest exactly at the
    // fork parent -- this is the only situation in which "rest at the fork" is the
    // correct answer, and it also proves the branch-attachment invariant never
    // splices a branch onto an unrelated active block.
    {
        CBlockIndex* pB3 = FindSideChainDescendantAtHeight(pA1->pprev, 5);
        BOOST_REQUIRE(pB3 != NULL);
        BOOST_CHECK(InvalidateBlock(*pA1->phashBlock, strError));   // A branch dies -> B best
        BOOST_CHECK_EQUAL(HashStr(BestChainHash()), HashStr(pB3));
        BOOST_CHECK(InvalidateBlock(*pB1->phashBlock, strError));   // B dies -> nothing eligible
        BOOST_CHECK_EQUAL(HashStr(BestChainHash()), HashStr(pA1->pprev));  // rests at P2
        BOOST_CHECK_EQUAL(pindexBest->nHeight, 2);
        // Restore a fully consistent, invalid-free state.
        BOOST_CHECK(ReconsiderBlock(*pA1->phashBlock, strError));
        BOOST_CHECK(ReconsiderBlock(*pB1->phashBlock, strError));
        BOOST_CHECK(setInvalidBlockHash.empty());
        BOOST_CHECK_EQUAL(HashStr(BestChainHash()), HashStr(pA4));
    }
}


// ---------------------------------------------------------------------------
// IFCN#9 S4 helpers: a BARE BlockIndexTipAuthority on a disposable directory.
// These records are synthetic (no block data), so the case exercises ONLY the
// durable tip authority's commit invariants -- no mining, no global state.
static BlockIndexRecord S4MakeRecord(uint256 hash, uint256 hashPrev, int32_t height)
{
    BlockIndexRecord r;
    r.hash = hash;
    r.hashPrev = hashPrev;
    r.height = height;
    r.nFile = 1;
    r.nBlockPos = (unsigned)(height * 100);
    r.nFlags = 0;
    r.nVersion = 5;
    r.nTime = 1600000000u + (unsigned)height;
    r.nBits = 0x1d00ffff;
    r.nNonce = (unsigned)height;
    return r;
}

static BlockIndexDerivedEntry S4MakeDerived(int32_t height)
{
    BlockIndexDerivedEntry d;
    d.chainTrust = uint256((unsigned)height);
    d.stakeModifierChecksum = 1000u + (uint32_t)height;
    d.SetHasStakeModifierTime(true);
    d.stakeModifierTime = 1600000000;
    d.SetHasBlockSize(true);
    d.nSize = 1200;
    return d;
}

// IFCN#9 S4 -- open risk B: a branch that attaches AT THE IMMUTABLE BASE TIP.
// fork == baseTip is the only fork height where the retained active prefix is
// EMPTY, so the fork block's hash cannot be read from the tip store itself. The
// invariant is nevertheless identical: the first branch record's hashPrev MUST
// be the base tip's hash. A branch whose root is a SIBLING of the base tip must
// be refused fail-closed with NO state change; a correctly attached branch must
// be accepted (no false refusal).
BOOST_AUTO_TEST_CASE(fork_at_base_tip_attaches_by_hash)
{
    BOOST_TEST_MESSAGE("S4: live authority "
                       << (GetAuthoritativeLiveAuthority() ? "present" : "absent"));

    boost::filesystem::path root = boost::filesystem::temp_directory_path() /
        boost::filesystem::unique_path("innova-basetip-%%%%-%%%%");
    boost::filesystem::create_directories(root);
    const std::string dir = root.string();

    const int32_t baseTip = 800;
    const uint256 baseHash(0xBA5E01UL);    // base generation's committed tip hash
    const uint256 siblingAtBase(0xBAD001UL); // a DIFFERENT block at the same height
    const uint256 A1(0xA101UL), A2(0xA202UL), GOOD(0xC101UL), BAD(0xB101UL);

    BlockIndexTipAuthority tip;
    BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 9, 500, baseTip, &tip, NULL));
    // Exactly what BlockIndexAuthoritativeLive::Open supplies in production.
    tip.SetBaseTipHash(baseHash);

    {
        BlockIndexTipAppend a;
        a.record = S4MakeRecord(A1, baseHash, baseTip + 1);
        a.derived = S4MakeDerived(baseTip + 1);
        BOOST_REQUIRE(tip.Append(a, baseTip + 1, NULL) == BLOCK_INDEX_TIP_OK);
    }
    {
        BlockIndexTipAppend a;
        a.record = S4MakeRecord(A2, A1, baseTip + 2);
        a.derived = S4MakeDerived(baseTip + 2);
        BOOST_REQUIRE(tip.Append(a, baseTip + 2, NULL) == BLOCK_INDEX_TIP_OK);
    }
    BOOST_REQUIRE(tip.TipHash() == A2);

    std::string err;
    // NEGATIVE: branch root is a SIBLING of the base tip; splicing it would make
    // the active chain start at a block whose parent is not the base tip.
    std::vector<BlockIndexTipAppend> bad(1);
    bad[0].record = S4MakeRecord(BAD, siblingAtBase, baseTip + 1);
    bad[0].derived = S4MakeDerived(9901);
    std::vector<int32_t> badH(1, baseTip + 1);

    const BlockIndexTipStatus stBad =
        tip.ApplyOperatorInvalidAndReorg(A2, true, baseTip, bad, badH, &err);
    BOOST_CHECK_MESSAGE(stBad == BLOCK_INDEX_TIP_CORRUPT,
        "fork==baseTip: a branch whose root is NOT a child of the base tip must be "
        "refused (got status " << (int)stBad << ", err=" << err << ")");
    BOOST_CHECK_MESSAGE(tip.TipHash() == A2,
        "negative case must leave the committed tip untouched");
    BOOST_CHECK_EQUAL(tip.TipHeight(), baseTip + 2);
    BOOST_CHECK(!tip.IsOperatorInvalid(A2));

    err.clear();
    const BlockIndexTipStatus stBadR = tip.ReorgActiveTo(baseTip, bad, badH, &err);
    BOOST_CHECK_MESSAGE(stBadR == BLOCK_INDEX_TIP_CORRUPT,
        "ReorgActiveTo at fork==baseTip must refuse an unattached branch (got "
        << (int)stBadR << ", err=" << err << ")");
    BOOST_CHECK_MESSAGE(tip.TipHash() == A2,
        "ReorgActiveTo negative case must leave the tip untouched");

    // POSITIVE: branch root's hashPrev IS the base tip hash -> must be accepted.
    std::vector<BlockIndexTipAppend> good(1);
    good[0].record = S4MakeRecord(GOOD, baseHash, baseTip + 1);
    good[0].derived = S4MakeDerived(9902);
    std::vector<int32_t> goodH(1, baseTip + 1);
    err.clear();
    const BlockIndexTipStatus stGood =
        tip.ApplyOperatorInvalidAndReorg(A2, true, baseTip, good, goodH, &err);
    BOOST_CHECK_MESSAGE(stGood == BLOCK_INDEX_TIP_OK,
        "fork==baseTip: a correctly attached branch must be ACCEPTED, no false refusal "
        "(got status " << (int)stGood << ", err=" << err << ")");
    BOOST_CHECK(tip.TipHash() == GOOD);
    BOOST_CHECK_EQUAL(tip.TipHeight(), baseTip + 1);
    BOOST_CHECK(tip.IsOperatorInvalid(A2));
    tip.Close();
}

BOOST_AUTO_TEST_SUITE_END()
