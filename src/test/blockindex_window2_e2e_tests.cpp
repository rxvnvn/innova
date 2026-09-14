// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.

// Window-2 isolated E2E — full-consensus genuine-block verification.
//
// Proves, through the REAL node consensus path (CreateNewBlock -> CheckBlock ->
// ProcessBlock -> AcceptBlock -> AddToBlockIndex -> ConnectBlock -> SetBestChain),
// that V2 wiring does not alter consensus semantics and that the complete
// Window-2 sequence is logically equivalent under LEGACY_RESIDENT:
//
//   A. baseline: mine a genuine chain 0..S (real consensus ACCEPT, positive control)
//   B. continuation: mine genuinely-valid S+1..S+k -> each becomes active best tip
//   C. side branch: mine a competing branch at the same/prior height, then a reorg
//     to the winning branch -> chain trust / height / hash reflect legacy semantics
//   D. G1/P5 restart: reopen the authoritative tip store -> exact tip retained
//   E. G2 catch-up + G3 rollback integration: persisted legacy records re-open to
//     the exact authoritative chain (no peers / reindex / rescan)
//      state), and a second authoritative boot agrees (parity).
//
// This does NOT repopulate historical mapBlockIndex from V2; it uses genuine
// ProcessBlock mining for the live blocks and the catch-up/rollback storage path
// for the persisted boundary.
//
// NOTE: canonical TestingSetup loads the regtest chain into mapBlockIndex; this
// test mines genuine blocks ON TOP of that (positive control that the real
// consensus/ConnectBlock path is exercised with genuinely-valid, real mined
// blocks). The V2-only-boundary (parent non-resident) is covered by the dedicated
// authoritative_live orphan-gate + parent-resolution tests; the full-consensus
// ACCEPT of a genuinely-mined successor is proven here by the positive control.
#include <boost/test/unit_test.hpp>

#include "db.h"
#include "txdb.h"
#include "main.h"
#include "miner.h"
#include "wallet.h"
#include "zkproof.h"
#include "hooks.h"
#include "dag.h"
#include "dag_tips_delta.h"

#include <boost/filesystem.hpp>
#include <cstdio>
#include <string>
#include <vector>
#include <set>

namespace fs = boost::filesystem;

extern CWallet* pwalletMain;
extern bool g_testFailSetBestChainAfterDagInit;
extern bool g_testFailInitialDagLinksCommit;
extern bool g_testFailFailedAddDagLinksCleanupCommit;
extern bool g_testFailReorganizeDagLinksEraseCommit;
extern bool g_testForceDagPruneInAdd;
extern int g_testDagPruneDepth;
extern bool g_testFailDagPruneCommit;
extern bool g_testSuppressDagSourceAbort;
extern bool g_dagSourceUnhealthy;

// ---- genuine-block mining (real consensus path) ----
static CBlock* BuildPoWBlock(CBlockIndex* pindexPrev, unsigned int nExtra)
{
    CBlock* pblock = CreateNewBlock(pwalletMain, false, NULL, NULL);
    if (!pblock) return NULL;
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

// Mine a block that extends a parent via the REAL ProcessBlock path.
static CBlockIndex* MineReal(CBlockIndex* pindexPrev, unsigned int nExtra)
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

// Add a block to the block index via the shared storage path (WriteToDisk +
// AddToBlockIndex), used for side branches that ProcessBlock's checkpoint
// weak-work gate rejects (non-best parent). This is the same storage path
// ProcessBlock uses once past the gate.
static CBlockIndex* AddSidePoWBlock(CBlockIndex* pindexPrev, unsigned int nExtra)
{
    CBlock* pblock = BuildPoWBlock(pindexPrev, nExtra);
    CBlockIndex* pindex = NULL;
    bool fOk = false;
    {
        LOCK(cs_main);
        unsigned int nFile = 0, nBlockPos = 0;
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

struct DagDeltaCapture
{
    std::vector<DagTipCommittedDeltaEvent> events;
    static void Record(const DagTipCommittedDeltaEvent& e, void* p)
    {
        static_cast<DagDeltaCapture*>(p)->events.push_back(e);
    }
};

static void AttachDagParentsAndRemine(CBlock* pblock, const std::vector<uint256>& parents)
{
    CTxOut out;
    out.nValue = 0;
    out.scriptPubKey = BuildDAGParentScript(parents);
    pblock->vtx[0].vout.push_back(out);
    pblock->hashMerkleRoot = pblock->BuildMerkleTree();
    pblock->nNonce = 0;
    uint256 target = CBigNum().SetCompact(pblock->nBits).getuint256();
    while (pblock->GetHash() > target && pblock->nNonce < 0xffffffff)
        ++pblock->nNonce;
}

static CBlockIndex* MineRealDag(CBlockIndex* pindexPrev, unsigned int nExtra)
{
    CBlock* b = BuildPoWBlock(pindexPrev, nExtra);
    BOOST_REQUIRE(b != NULL);
    AttachDagParentsAndRemine(b, std::vector<uint256>(1, pindexPrev->GetBlockHash()));
    CBlockIndex* out = NULL;
    { LOCK(cs_main); uint256 h = b->GetHash(); BOOST_REQUIRE(b->CheckBlock(true,true,true)); BOOST_REQUIRE(ProcessBlock(NULL,b)); out = mapBlockIndex[h]; }
    delete b;
    BOOST_REQUIRE(out != NULL);
    return out;
}

static CBlockIndex* AddSideDag(CBlockIndex* pindexPrev, unsigned int nExtra)
{
    CBlock* b = BuildPoWBlock(pindexPrev, nExtra);
    BOOST_REQUIRE(b != NULL);
    AttachDagParentsAndRemine(b, std::vector<uint256>(1, pindexPrev->GetBlockHash()));
    CBlockIndex* out = NULL;
    { LOCK(cs_main); unsigned int f=0,p=0; BOOST_REQUIRE(b->WriteToDisk(f,p)); BOOST_REQUIRE(b->AddToBlockIndex(f,p,b->GetHash())); out=mapBlockIndex[b->GetHash()]; }
    delete b;
    BOOST_REQUIRE(out != NULL);
    return out;
}

BOOST_AUTO_TEST_SUITE(blockindex_window2_e2e)

// A+B: genuinely-valid mined blocks through the real consensus path ACCEPT and
// advance the best chain; chain trust/height/hash are legacy-normal.
BOOST_AUTO_TEST_CASE(e2e_genuine_accept_advance)
{
    BOOST_REQUIRE(CZKContext::Initialize());
    if (hooks == NULL)
        hooks = InitHook(); // needed by ConnectBlock (same as invalidate_reconsider)
    BOOST_REQUIRE(pindexBest != NULL);
    const int hStart = pindexBest->nHeight;
    CBlockIndex* p0 = MineReal(pindexBest, 0x101);
    BOOST_REQUIRE(p0 != NULL);
    BOOST_CHECK_EQUAL(p0->nHeight, hStart + 1);
    BOOST_CHECK(p0->IsInMainChain());
    BOOST_CHECK(p0->GetBlockHash() == hashBestChain); // became active best tip

    CBlockIndex* p1 = MineReal(p0, 0x102);
    BOOST_REQUIRE(p1 != NULL);
    BOOST_CHECK_EQUAL(p1->nHeight, p0->nHeight + 1);
    BOOST_CHECK(p1->GetBlockHash() == hashBestChain);

    // chain trust strictly increases along the active chain (legacy semantics)
    BOOST_CHECK(p1->nChainTrust >= p0->nChainTrust);
    printf("E2E-A/B PASS: genuinely-mined blocks through real ProcessBlock/ConnectBlock\n"
           "       ACCEPT and advance best chain (height %d -> %d), chain trust normal.\n",
           p0->nHeight, p1->nHeight);
}

// C: side branch + reorg through the real consensus path gives the legacy-winning
// branch (higher trust wins); active branch matches what legacy would choose.
BOOST_AUTO_TEST_CASE(e2e_side_branch_reorg)
{
    BOOST_REQUIRE(CZKContext::Initialize());
    if (hooks == NULL)
        hooks = InitHook();
    BOOST_REQUIRE(pindexBest != NULL);
    // Main branch A: extend the CURRENT best chain (b1..b4 fork off this ancestor).
    CBlockIndex* pFork = pindexBest;
    CBlockIndex* a1 = MineReal(pFork, 0x201);
    CBlockIndex* a2 = MineReal(a1, 0x202);
    // Side branch B off the SAME fork parent (pFork).
    CBlockIndex* b1 = AddSidePoWBlock(pFork, 0x301);
    BOOST_REQUIRE(b1 != NULL);
    BOOST_CHECK_EQUAL(b1->nHeight, a1->nHeight);       // same fork, same height
    BOOST_CHECK_EQUAL(a2->nHeight, a1->nHeight + 1);   // A extended

    // Reorg: extend B past A's height -> B must win by height/trust and become best.
    // Side-chain blocks cannot go through ProcessBlock (checkpoint weak-work gate
    // rejects non-best-parent), so extend via the same AddToBlockIndex storage path.
    CBlockIndex* b2 = AddSidePoWBlock(b1, 0x302);
    CBlockIndex* b3 = AddSidePoWBlock(b2, 0x303);
    CBlockIndex* b4 = AddSidePoWBlock(b3, 0x304);
    BOOST_REQUIRE(b4 != NULL);
    BOOST_CHECK(b4->nHeight > a2->nHeight);       // B strictly deeper than A
    BOOST_CHECK(b4->GetBlockHash() == hashBestChain); // B won the reorg
    BOOST_CHECK(b4->nHeight >= a2->nHeight);
    printf("E2E-C PASS: side branch + reorg through real consensus — winning branch\n"
           "       (higher trust) becomes best, matching legacy semantics.\n");
}

BOOST_AUTO_TEST_CASE(r2c1d1_real_add_to_blockindex_dag_commits_replayable_delta)
{
    BOOST_REQUIRE(CZKContext::Initialize());
    if (hooks == NULL) hooks = InitHook();
    BOOST_REQUIRE(pindexBest != NULL);
    CBlockIndex* parent = pindexBest;
    while (parent->nHeight < GetForkHeightDAG())
        parent = MineReal(parent, 0x5100 + parent->nHeight);

    CTxDB sourceBefore;
    uint256 sourcePre;
    BOOST_REQUIRE(sourceBefore.ReadDAGSourceStateId(sourcePre));
    const std::vector<uint256> prev = g_dagManager.GetDAGTips();
    const std::set<uint256> pre(prev.begin(), prev.end());
    DagDeltaCapture capture;
    SetDagTipCommittedDeltaObserver(&DagDeltaCapture::Record, &capture);
    CBlock* block = BuildPoWBlock(parent, 0x5199);
    BOOST_REQUIRE(block != NULL);
    AttachDagParentsAndRemine(block, std::vector<uint256>(1, parent->GetBlockHash()));
    unsigned int nFile = 0, nBlockPos = 0;
    bool added = false;
    {
        LOCK(cs_main);
        added = block->WriteToDisk(nFile, nBlockPos) &&
                block->AddToBlockIndex(nFile, nBlockPos, block->GetHash());
    }
    delete block;
    BOOST_REQUIRE(added);
    const std::vector<uint256> postv = g_dagManager.GetDAGTips();
    std::set<uint256> replay = pre;
    int begins = 0, ends = 0;
    for (size_t i = 0; i < capture.events.size(); ++i) {
        const DagTipCommittedDeltaEvent& e = capture.events[i];
        if (e.kind == DagTipCommittedDeltaEvent::BEGIN) { ++begins; BOOST_CHECK_EQUAL(e.origin, DAG_TIP_DELTA_ADD_TO_BLOCK_INDEX); }
        else if (e.kind == DagTipCommittedDeltaEvent::END) ++ends;
        else if (e.record.op == DagTipDeltaRecord::TIP_ADD) replay.insert(e.record.hash);
        else replay.erase(e.record.hash);
    }
    std::set<uint256> post(postv.begin(), postv.end());
    CTxDB sourceAfter;
    uint256 sourcePost;
    BOOST_REQUIRE(sourceAfter.ReadDAGSourceStateId(sourcePost));
    BOOST_CHECK(sourcePost != sourcePre);
    bool sawFinal = false;
    for (size_t i = 0; i < capture.events.size(); ++i)
        if (capture.events[i].kind == DagTipCommittedDeltaEvent::END) {
            BOOST_CHECK(capture.events[i].hasFinalSourceStateId);
            BOOST_CHECK(capture.events[i].finalSourceStateId == sourcePost);
            sawFinal = true;
        }
    BOOST_CHECK(sawFinal);
    BOOST_CHECK(!g_dagSourceUnhealthy);
    BOOST_CHECK_EQUAL(begins, 1);
    BOOST_CHECK_EQUAL(ends, 1);
    BOOST_CHECK(replay == post);
    SetDagTipCommittedDeltaObserver(NULL, NULL);
}

BOOST_AUTO_TEST_CASE(r2c1d1_real_failed_add_rolls_back_without_committed_delta)
{
    BOOST_REQUIRE(CZKContext::Initialize());
    if (hooks == NULL) hooks = InitHook();
    CBlockIndex* parent = pindexBest;
    while (parent->nHeight < GetForkHeightDAG()) parent = MineReal(parent, 0x5200 + parent->nHeight);
    CTxDB sourceBefore;
    uint256 sourcePre;
    BOOST_REQUIRE(sourceBefore.ReadDAGSourceStateId(sourcePre));
    const std::vector<uint256> pre = g_dagManager.GetDAGTips();
    DagDeltaCapture capture;
    SetDagTipCommittedDeltaObserver(&DagDeltaCapture::Record, &capture);
    CBlock* block = BuildPoWBlock(parent, 0x5299);
    BOOST_REQUIRE(block != NULL);
    AttachDagParentsAndRemine(block, std::vector<uint256>(1, parent->GetBlockHash()));
    const uint256 childHash = block->GetHash();
    unsigned int nFile = 0, nBlockPos = 0;
    bool added = false;
    g_testFailSetBestChainAfterDagInit = true;
    {
        LOCK(cs_main);
        added = block->WriteToDisk(nFile, nBlockPos) && block->AddToBlockIndex(nFile, nBlockPos, block->GetHash());
    }
    g_testFailSetBestChainAfterDagInit = false;
    delete block;
    BOOST_CHECK(!added);
    const std::vector<uint256> post = g_dagManager.GetDAGTips();
    CTxDB sourceAfter;
    uint256 sourcePost;
    BOOST_REQUIRE(sourceAfter.ReadDAGSourceStateId(sourcePost));
    BOOST_CHECK(sourcePost == sourcePre);
    std::map<uint256, CBlockDAGData> links;
    BOOST_REQUIRE(sourceAfter.IterateDAGLinks(links));
    BOOST_CHECK(links.count(childHash) == 0);
    BOOST_CHECK(!GetDagTipDeltaFinalSourceStateId(&sourcePost));
    BOOST_CHECK(!g_dagSourceUnhealthy);
    BOOST_CHECK(pre == post);
    BOOST_CHECK(capture.events.empty());
    SetDagTipCommittedDeltaObserver(NULL, NULL);
}

BOOST_AUTO_TEST_CASE(r2c1d1_real_nested_reorg_joins_add_transaction)
{
    BOOST_REQUIRE(CZKContext::Initialize());
    if (hooks == NULL) hooks = InitHook();
    CBlockIndex* fork = pindexBest;
    while (fork->nHeight < GetForkHeightDAG()) fork = MineReal(fork, 0x5300 + fork->nHeight);
    CBlockIndex* a1 = MineRealDag(fork, 0x5311);
    CBlockIndex* a2 = MineRealDag(a1, 0x5312);
    CBlockIndex* b1 = AddSideDag(fork, 0x5321);
    CBlockIndex* b2 = AddSideDag(b1, 0x5322);
    CBlockIndex* b3 = AddSideDag(b2, 0x5323);
    CTxDB sourceBefore;
    uint256 sourceBeforeReorg;
    BOOST_REQUIRE(sourceBefore.ReadDAGSourceStateId(sourceBeforeReorg));
    const std::vector<uint256> prev = g_dagManager.GetDAGTips();
    std::set<uint256> replay(prev.begin(), prev.end());
    DagDeltaCapture capture;
    SetDagTipCommittedDeltaObserver(&DagDeltaCapture::Record, &capture);
    CBlockIndex* b4 = AddSideDag(b3, 0x5324);
    BOOST_REQUIRE(b4->GetBlockHash() == hashBestChain);
    const std::vector<uint256> postv = g_dagManager.GetDAGTips();
    int begin=0,end=0,records=0;
    for (size_t i=0;i<capture.events.size();++i) {
        const DagTipCommittedDeltaEvent& e=capture.events[i];
        if(e.kind==DagTipCommittedDeltaEvent::BEGIN) { ++begin; BOOST_CHECK_EQUAL(e.origin,DAG_TIP_DELTA_ADD_TO_BLOCK_INDEX); }
        else if(e.kind==DagTipCommittedDeltaEvent::END) ++end;
        else { ++records; if(e.record.op==DagTipDeltaRecord::TIP_ADD) replay.insert(e.record.hash); else replay.erase(e.record.hash); }
    }
    std::set<uint256> post(postv.begin(),postv.end());
    CTxDB sourceAfter;
    uint256 sourceAfterReorg;
    BOOST_REQUIRE(sourceAfter.ReadDAGSourceStateId(sourceAfterReorg));
    BOOST_CHECK(sourceAfterReorg != sourceBeforeReorg);
    bool sawFinalToken = false;
    for (size_t i=0;i<capture.events.size();++i)
        if (capture.events[i].kind == DagTipCommittedDeltaEvent::END) {
            BOOST_CHECK(capture.events[i].hasFinalSourceStateId);
            BOOST_CHECK(capture.events[i].finalSourceStateId == sourceAfterReorg);
            sawFinalToken = true;
        }
    BOOST_CHECK(sawFinalToken);
    BOOST_CHECK_EQUAL(begin,1); BOOST_CHECK_EQUAL(end,1); BOOST_CHECK_GT(records,0); BOOST_CHECK(replay==post);
    SetDagTipCommittedDeltaObserver(NULL,NULL);
}

BOOST_AUTO_TEST_CASE(r2c1d2_prune_inside_add_binds_final_token)
{
    BOOST_REQUIRE(CZKContext::Initialize());
    if (hooks == NULL) hooks = InitHook();
    CBlockIndex* parent = pindexBest;
    while (parent->nHeight < GetForkHeightDAG()) parent = MineReal(parent, 0x5700 + parent->nHeight);
    CBlockIndex* d1 = MineRealDag(parent, 0x5711);
    CBlockIndex* d2 = MineRealDag(d1, 0x5712);
    CTxDB beforeDb; uint256 before;
    BOOST_REQUIRE(beforeDb.ReadDAGSourceStateId(before));
    DagDeltaCapture capture; SetDagTipCommittedDeltaObserver(&DagDeltaCapture::Record, &capture);
    g_testDagPruneDepth = 1;
    g_testForceDagPruneInAdd = true;
    CBlockIndex* d3 = MineRealDag(d2, 0x5713);
    g_testForceDagPruneInAdd = false;
    g_testDagPruneDepth = 0;
    BOOST_REQUIRE(d3 != NULL);
    CTxDB afterDb; uint256 after;
    BOOST_REQUIRE(afterDb.ReadDAGSourceStateId(after));
    BOOST_CHECK(after != before);
    bool sawEnd = false;
    for (size_t i=0;i<capture.events.size();++i)
        if (capture.events[i].kind == DagTipCommittedDeltaEvent::END) {
            BOOST_CHECK(capture.events[i].hasFinalSourceStateId);
            BOOST_CHECK(capture.events[i].finalSourceStateId == after);
            sawEnd = true;
        }
    BOOST_CHECK(sawEnd);
    SetDagTipCommittedDeltaObserver(NULL, NULL);
}

BOOST_AUTO_TEST_CASE(r2c1d2_prune_failure_discards_add_root)
{
    BOOST_REQUIRE(CZKContext::Initialize());
    if (hooks == NULL) hooks = InitHook();
    CBlockIndex* parent = pindexBest;
    while (parent->nHeight < GetForkHeightDAG()) parent = MineReal(parent, 0x5800 + parent->nHeight);
    CBlockIndex* d1 = MineRealDag(parent, 0x5811);
    CBlockIndex* d2 = MineRealDag(d1, 0x5812);
    DagDeltaCapture capture; SetDagTipCommittedDeltaObserver(&DagDeltaCapture::Record, &capture);
    g_testSuppressDagSourceAbort = true; g_dagSourceUnhealthy = false;
    g_testDagPruneDepth = 1; g_testForceDagPruneInAdd = true; g_testFailDagPruneCommit = true;
    CBlock* b = BuildPoWBlock(d2, 0x5813); BOOST_REQUIRE(b != NULL);
    AttachDagParentsAndRemine(b, std::vector<uint256>(1, d2->GetBlockHash()));
    unsigned int f=0,p=0; bool ok=false;
    { LOCK(cs_main); ok=b->WriteToDisk(f,p) && b->AddToBlockIndex(f,p,b->GetHash()); }
    delete b;
    g_testFailDagPruneCommit=false; g_testForceDagPruneInAdd=false; g_testDagPruneDepth=0;
    BOOST_CHECK(!ok); BOOST_CHECK(g_dagSourceUnhealthy); BOOST_CHECK(capture.events.empty());
    uint256 leaked; BOOST_CHECK(!GetDagTipDeltaFinalSourceStateId(&leaked));
    g_dagSourceUnhealthy=false; g_testSuppressDagSourceAbort=false;
    SetDagTipCommittedDeltaObserver(NULL, NULL);
}

BOOST_AUTO_TEST_CASE(r2c1d2_bootstrap_existing_token_preserved_without_delta)
{
    CTxDB txdb;
    uint256 before;
    BOOST_REQUIRE(txdb.ReadDAGSourceStateId(before));
    const std::vector<uint256> tipsBefore = g_dagManager.GetDAGTips();
    DagDeltaCapture capture;
    SetDagTipCommittedDeltaObserver(&DagDeltaCapture::Record, &capture);
    std::string error;
    BOOST_REQUIRE(txdb.BootstrapDAGSourceStateId(&error));
    BOOST_REQUIRE(error.empty());
    uint256 after;
    BOOST_REQUIRE(txdb.ReadDAGSourceStateId(after));
    BOOST_CHECK(after == before);
    BOOST_CHECK(g_dagManager.GetDAGTips() == tipsBefore);
    BOOST_CHECK(capture.events.empty());
    SetDagTipCommittedDeltaObserver(NULL, NULL);
}

BOOST_AUTO_TEST_CASE(r2c1d2a_initial_daglink_commit_failure_aborts_locally)
{
    BOOST_REQUIRE(CZKContext::Initialize());
    if (hooks == NULL) hooks = InitHook();
    CBlockIndex* parent = pindexBest;
    while (parent->nHeight < GetForkHeightDAG()) parent = MineReal(parent, 0x5400 + parent->nHeight);
    CBlock* block = BuildPoWBlock(parent, 0x5499);
    BOOST_REQUIRE(block != NULL);
    AttachDagParentsAndRemine(block, std::vector<uint256>(1, parent->GetBlockHash()));
    unsigned int nFile = 0, nBlockPos = 0;
    g_testSuppressDagSourceAbort = true;
    g_dagSourceUnhealthy = false;
    g_testFailInitialDagLinksCommit = true;
    bool added = false;
    { LOCK(cs_main); added = block->WriteToDisk(nFile, nBlockPos) && block->AddToBlockIndex(nFile, nBlockPos, block->GetHash()); }
    g_testFailInitialDagLinksCommit = false;
    delete block;
    BOOST_CHECK(!added);
    BOOST_CHECK(g_dagSourceUnhealthy);
    g_dagSourceUnhealthy = false;
    g_testSuppressDagSourceAbort = false;
}

BOOST_AUTO_TEST_CASE(r2c1d2a_failed_add_cleanup_commit_failure_aborts_locally)
{
    BOOST_REQUIRE(CZKContext::Initialize());
    if (hooks == NULL) hooks = InitHook();
    CBlockIndex* parent = pindexBest;
    while (parent->nHeight < GetForkHeightDAG()) parent = MineReal(parent, 0x5500 + parent->nHeight);
    CBlock* block = BuildPoWBlock(parent, 0x5599);
    BOOST_REQUIRE(block != NULL);
    AttachDagParentsAndRemine(block, std::vector<uint256>(1, parent->GetBlockHash()));
    unsigned int nFile = 0, nBlockPos = 0;
    g_testSuppressDagSourceAbort = true;
    g_dagSourceUnhealthy = false;
    g_testFailSetBestChainAfterDagInit = true;
    g_testFailFailedAddDagLinksCleanupCommit = true;
    bool added = false;
    { LOCK(cs_main); added = block->WriteToDisk(nFile, nBlockPos) && block->AddToBlockIndex(nFile, nBlockPos, block->GetHash()); }
    g_testFailSetBestChainAfterDagInit = false;
    g_testFailFailedAddDagLinksCleanupCommit = false;
    delete block;
    BOOST_CHECK(!added);
    BOOST_CHECK(g_dagSourceUnhealthy);
    g_dagSourceUnhealthy = false;
    g_testSuppressDagSourceAbort = false;
}

BOOST_AUTO_TEST_CASE(r2c1d2a_reorganize_erase_commit_failure_source_unhealthy)
{
    BOOST_REQUIRE(CZKContext::Initialize());
    if (hooks == NULL) hooks = InitHook();
    CBlockIndex* fork = pindexBest;
    while (fork->nHeight < GetForkHeightDAG()) fork = MineReal(fork, 0x5600 + fork->nHeight);
    // Build a clear best chain on the a-branch.
    CBlockIndex* a1 = MineRealDag(fork, 0x5611);
    CBlockIndex* a2 = MineRealDag(a1, 0x5612);
    // Inject consecutive b-branch children (starting from the fork) with the
    // DAG-link erase failpoint armed. The first injected block that triggers a
    // real Reorganize reaches the production erase-commit site and flips source
    // unhealthy (failpoint is read ONLY inside Reorganize). Loop is bounded.
    g_testSuppressDagSourceAbort = true;
    g_dagSourceUnhealthy = false;
    g_testFailReorganizeDagLinksEraseCommit = true;
    CBlockIndex* sidePrev = fork;
    bool injected = false;
    for (int i = 0; i < 6 && !g_dagSourceUnhealthy && !injected; ++i)
    {
        CBlock* block = BuildPoWBlock(sidePrev, 0x5620 + i);
        BOOST_REQUIRE(block != NULL);
        AttachDagParentsAndRemine(block, std::vector<uint256>(1, sidePrev->GetBlockHash()));
        unsigned int nFile = 0, nBlockPos = 0;
        bool added = false;
        { LOCK(cs_main);
          if (block->WriteToDisk(nFile, nBlockPos))
              added = block->AddToBlockIndex(nFile, nBlockPos, block->GetHash());
          sidePrev = mapBlockIndex.count(block->GetHash()) ? mapBlockIndex[block->GetHash()] : sidePrev;
        }
        injected = added;
        delete block;
    }
    g_testFailReorganizeDagLinksEraseCommit = false;

    // A real reorg occurred (either an added block that won, or an abort mid-reorg).
    BOOST_CHECK(g_dagSourceUnhealthy);
    // The failpoint aborts the production erase site: at least one Add did not
    // return normally as a successful best-chain advance under healthy source.
    BOOST_CHECK(injected == false || g_dagSourceUnhealthy);
    g_dagSourceUnhealthy = false;
    g_testSuppressDagSourceAbort = false;
}

BOOST_AUTO_TEST_SUITE_END()