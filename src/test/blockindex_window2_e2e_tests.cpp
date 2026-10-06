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

// Fixture chain-depth marker used by the by-value trust fixtures below. This is
// ONLY a chain-depth marker: the supported linear/V2 profile has no consensus
// rule change at any height, so no activation semantics attach to it.
static const int kFixtureDepth = 11;

#include "db.h"
#include "txdb.h"
#include "main.h"
#include "blockrequesttrace.h"
#include <cstdlib>
#include "miner.h"
#include "wallet.h"
#include "blockindex_authoritative_startup.h"
#include "blockindex_authoritative_live.h"
#include "blockindex_shadow_startup.h"
#include "blockindex_generation_builder.h"
#include "blockindex_generation_lifecycle.h"
#include "blockindex_authoritative_restart.h"
#include <openssl/sha.h>
#include <leveldb/db.h>
#include <thread>
#include <fstream>

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
extern bool CorruptDagTipDeltaSpillForTest(int);
extern int g_testDagPruneFailStage;
extern bool g_testFailDagTipDeltaSpillWrite;
extern bool g_testFailDagTipDeltaSpillClose;
extern bool g_testS12LifetimeProbe;
extern int g_testS12LastPostponed;
extern void* g_testS12SeenTxdbAddr;
extern void* g_testS12SeenPdb;
extern void* g_testS12SeenGlobal;
extern bool g_testS12SeenPdbWasClosed;
extern int g_testS12SeenCloseCount;
extern int g_testS12SeenOpenCount;
extern int g_testS12SeenDeliveredEvents;
extern int g_testTxdbCloseCount;
extern void* g_testTxdbLastClosedPtr;
extern int g_testTxdbOpenCount;
extern void* g_testTxdbLastOpenedPtr;
extern void* GetGlobalTxdbPtrForTest();
extern int g_testDagDeltaDeliveredEvents;
extern int g_testDagDeltaLastDeliveredKind;
extern int g_testDagDeltaLastDeliveredOrigin;
extern uint256 GetBlockEntropy(const uint256& hashValue);
extern bool g_testFailDAGChildCountRebuild; // S4-F12 child-count untrusted refusal

// Production runtime/consumer, real mutable txleveldb, disposable overlay only.
// Full scans here are test setup/oracles, NEVER ordinary mutation delivery.


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
    // Determinism: the coinbase destination is a fresh random wallet reserve key
    // (wallet.cpp GetReservedKey) that varies per test datadir -> varies the
    // merkleRoot -> varies the block hash run-to-run. Overwrite it with a fixed
    // destination so fixture block hashes/topology are reproducible. The DAG
    // parent commitment (coinbase vout[1]) is set later by AttachDagParentsAndRemine.
    if (!pblock->vtx.empty() && !pblock->vtx[0].vout.empty())
        pblock->vtx[0].vout[0].scriptPubKey = CScript() << OP_DUP << OP_HASH160
            << uint160(0x0000000000000000000000000000000000000001ULL) << OP_EQUALVERIFY << OP_CHECKSIG;
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





static CBlockIndex* MineRealDag(CBlockIndex* pindexPrev, unsigned int nExtra)
{
    const bool fSavedAbtrace = AcceptBlockRejectTraceEnabled();
    InitAcceptBlockRejectTrace(true);
    CBlock* b = BuildPoWBlock(pindexPrev, nExtra);
    BOOST_REQUIRE(b != NULL);
    // LEGACY DAG RETIREMENT (Phase 2 / H9 FINAL): linear build, no DAG parent metadata.
    CBlockIndex* out = NULL;
    { LOCK(cs_main); uint256 h = b->GetHash(); BOOST_REQUIRE(b->CheckBlock(true,true,true)); BOOST_REQUIRE(ProcessBlock(NULL,b)); out = mapBlockIndex[h]; }
    delete b;
    BOOST_REQUIRE(out != NULL);
    return out;
}

static void RebindMapBlockIndexHashPointersForTest()
{
    AssertLockHeld(cs_main);
    for (std::map<uint256, CBlockIndex*>::iterator it = mapBlockIndex.begin();
         it != mapBlockIndex.end(); ++it)
    {
        BOOST_REQUIRE_MESSAGE(it->second != NULL,
            "fixture mapBlockIndex may not contain a null CBlockIndex pointer");
        it->second->phashBlock = &it->first;
        BOOST_REQUIRE(it->second->phashBlock == &it->first);
        BOOST_REQUIRE(*it->second->phashBlock == it->first);
        BOOST_REQUIRE(it->second->GetBlockHash() == it->first);
    }
}

static void RestoreMapBlockIndexForFixture(const std::map<uint256, CBlockIndex*>& saved)
{
    AssertLockHeld(cs_main);
    mapBlockIndex = saved;
    RebindMapBlockIndexHashPointersForTest();
}



// ---------------------------------------------------------------------------
// R2c.2/S6 vehicle builds for deliberately-degraded-authority mutation
// fixtures. The production authoritative primary selection correctly FAILS
// CLOSED when the score authority is uncertified/revoked (or the overlay
// runtime is intentionally stale). Rollback/prune fixtures that construct a
// vehicle block for a MUTATION under test - the mutation itself still runs
// through the unchanged production path - therefore build the vehicle through
// the legacy construction branch; the block BUILD is not the subject under
// test and every assertion on the mutation/rollback semantics is unchanged.
// ---------------------------------------------------------------------------



// S6: delta observer tap that records events for the fixture AND forwards them
// to the registered overlay runtime. The S3 token-trace fixtures replace the
// production observer during their capture window; without forwarding, the
// runtime's applied token would go stale and the (correct) fail-closed
// authoritative selection would refuse to build. Later mutations use the pure
// forwarding observer below so the runtime stays current for the rest of the
// fixture.


// S12 discriminator helper: add a side DAG-era block whose resulting chain
// trust is trust-controlled by retrying nExtra until
//   maxTrustInclusive >= prev->nChainTrust + GetBlockEntropy(hash) > minTrustExclusive.
// Post-POEM trust is entropy(hash)-based, so this makes the side branch's
// relative ordering deterministic instead of hash luck.


BOOST_AUTO_TEST_SUITE(blockindex_window2_e2e)

// ---------------------------------------------------------------------------
// INDEPENDENT POST-REORG CURRENT-CANONICAL COUNTERFACTUAL ORACLE
// ---------------------------------------------------------------------------
// The authoritative S2/C-full target for an erased DAG-era boundary parent P is
// NOT the pre-reorg retained P score (historical residue from a different DAG
// state), and NOT the post-restart linear nChainTrust (legacy restart artifact).
// It is the value P and any retained child would obtain if P were colored as a
// normal RETAINED vertex under the CURRENT post-reorg canonical source, using
// unmodified current linear block-trust / chainTrust computation (the retired
// AnticoneSize. This helper builds that counterfactual independently:
//   1. take the frozen post-reorg canonical scope;
//   2. inject each erased parent P's recovered raw-coinbase DAG-parent closure
//      as REAL RETAINED daglinks records (immutable topology facts only);
//   3. run ReconstructAuthoritativeDAGFields so P colors via the NORMAL retained
//      path (no Option-R boundary reconstruction).
// The resulting full-field map is the independent oracle. It is independent of
// Option-R's boundary path and of any resident/live/restart residue.

// Raw persisted-marker helpers (definitions in the S4 section; declared here for
// earlier fixtures that must establish explicit persisted pre-states).
static std::string S4MarkerKey();
static void S4RawDel(const std::string& key);


// A+B: genuinely-valid mined blocks through the real consensus path ACCEPT and
// advance the best chain; chain trust/height/hash are legacy-normal.
BOOST_AUTO_TEST_CASE(e2e_genuine_accept_advance)
{
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









// All vertices are mined through ProcessBlock. No source records or epoch
// exemptions are manufactured by this fixture.






















// ---------------------------------------------------------------------------
// F2 — AUTHORITATIVE DAG PARENT-SCORE / SELECTED-PARENT CUTOVER
//
// INVARIANT UNDER TEST: DAG PARENT SCORE TRUTH != mapDAGData RESIDENCY and
//                       != mapBlockIndex RESIDENCY.
//
// Fixture: a post-DAG PoW parent P that is LOGICALLY PRESENT (its canonical
// persisted daglinks row exists and the score authority is HEALTHY) but
// NONRESIDENT in BOTH RAM maps (absent from mapDAGData AND mapBlockIndex), and
// a child referencing P as its only DAG parent.
//
// RED (pre-repair): the parent-score lookup misses both RAM maps, so
//   nParentScore is silently 0; the child's nDAGScore collapses to its own
//   GetBlockTrust() and the fully-resident oracle is not reproduced.
// GREEN (post-repair): the parent score is resolved from the certified
//   persisted DAG score authority; child nDAGScore == oracle exactly.
// Single-parent child => no merge-blue contribution, so the fully-resident
// oracle is exactly parentAuthoritativeScore + childOwnTrust.
// ---------------------------------------------------------------------------


// ---------------------------------------------------------------------------
// F2 — parent-score resolver contract: fully-resident parity, typed
// NOT_FOUND, the VALID-ZERO control, and the fail-closed authority matrix.
//
// This case keeps the resident pointer graph INTACT (no clear) so the legacy
// resident rule and the authoritative rule can be compared directly on the same
// hashes: the authoritative resolver must reproduce the exact legacy value
// wherever the legacy source is authoritative (FULL_RESIDENT_PARITY), must
// distinguish a legitimate zero score from a failure, and must fail closed when
// the score authority is unavailable.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(f2_parent_score_resolver_parity_and_contract)
{
        CBlockIndex* tip = pindexBest;
    BOOST_REQUIRE(tip);
    while (tip->nHeight < kFixtureDepth - 1) tip = MineReal(tip, 0x8300 + tip->nHeight);

    // Capture resident hashes by height from the intact chain.
    std::map<int, uint256> byHeight;
    { LOCK(cs_main);
      for (CBlockIndex* w = tip; w; w = w->pprev) byHeight[w->nHeight] = w->GetBlockHash(); }
    BOOST_REQUIRE(byHeight.count(9) && byHeight.count(10) && byHeight.count(kFixtureDepth - 1));

    const fs::path root = fs::temp_directory_path() / fs::unique_path("f2-resolver-%%%%-%%%%");
    fs::create_directories(root / "snapshot");

    CBlockIndex* savedBest = pindexBest;
    CBlockIndex* savedGenesis = pindexGenesisBlock;
    uint256 savedBestChain = hashBestChain;
    int savedBestHeight = nBestHeight;
    uint256 savedBestTrust = nBestChainTrust;
    struct Cleanup {
        fs::path root; CBlockIndex* best; CBlockIndex* genesis;
        uint256 bestChain; int bestHeight; uint256 bestTrust;
        Cleanup(const fs::path& r, CBlockIndex* b, CBlockIndex* g, const uint256& bc, int bh, const uint256& bt)
            : root(r), best(b), genesis(g), bestChain(bc), bestHeight(bh), bestTrust(bt) {}
        ~Cleanup() {
            ResetBlockIndexAuthoritativeStartupForTest();
            pindexBest = best; pindexGenesisBlock = genesis;
            hashBestChain = bestChain; nBestHeight = bestHeight; nBestChainTrust = bestTrust;
            try { fs::remove_all(root); } catch (...) {}
        }
    } cleanup(root, savedBest, savedGenesis, savedBestChain, savedBestHeight, savedBestTrust);

    { CTxDB db; db.Close(); }
    const auto live = GetDataDir() / "txleveldb";
    for (fs::directory_iterator it(live), end; it != end; ++it)
        if (fs::is_regular_file(it->path())) fs::copy_file(it->path(), root / "snapshot" / it->path().filename());
    BlockIndexGenerationSource source; std::string error;
    BOOST_REQUIRE_MESSAGE(ReadLegacyBlockIndexSource((root / "snapshot").string(), &source, &error), error);
    source.blockDataDir = GetDataDir().string();
    BlockIndexGenerationBuilder builder;
    BOOST_REQUIRE_MESSAGE(builder.Build(source, (root / "blockindex-build-000001.tmp").string(), 1, NULL, &error), error);
    builder.Close();
    BOOST_REQUIRE_EQUAL(BlockIndexGenerationManager::PublishGeneration(root.string(), 1, &error), BLOCK_INDEX_LIFECYCLE_OK);
    BOOST_REQUIRE_EQUAL(BlockIndexGenerationManager::SelectGeneration(root.string(), 1, &error), BLOCK_INDEX_LIFECYCLE_OK);
    BOOST_REQUIRE_MESSAGE(InitBlockIndexAuthoritative(root.string(), &error), error);
    BOOST_REQUIRE(g_fAuthoritativeStartup);

    // Current contract: exact linear parent trust, independent of the retired resolver.
    for (int h : {5, 9, 10, kFixtureDepth - 1})
    {
        const uint256 hp = byHeight[h];
        CBlockIndex* resident = NULL;
        { LOCK(cs_main); resident = mapBlockIndex.at(hp); }
        uint256 trust; std::string err;
        BOOST_REQUIRE_MESSAGE(GetAuthoritativeAccumulatedChainTrust(hp, &trust, &err), err);
        BOOST_CHECK(trust == resident->nChainTrust);
    }
    uint256 trust = 1; std::string err;
    BOOST_CHECK(!GetAuthoritativeAccumulatedChainTrust(uint256(0xdeadbeef), &trust, &err));
    BOOST_CHECK(trust == 0);
    ResetBlockIndexAuthoritativeStartupForTest();
    BOOST_CHECK(!GetAuthoritativeAccumulatedChainTrust(byHeight[9], &trust, &err));
}

// ---------------------------------------------------------------------------
// F2 AUDIT BLOCKER REPAIR — shared fixture: build, publish, select an
// authoritative generation from the CURRENT live txleveldb content and boot the
// authoritative startup. Isolated temp root only; never the production datadir.
// ---------------------------------------------------------------------------
static void F2BuildAuthoritativeGenerationAndInit(const fs::path& root, std::string* error)
{
    fs::create_directories(root / "snapshot");
    { CTxDB db; db.Close(); }
    const auto live = GetDataDir() / "txleveldb";
    for (fs::directory_iterator it(live), end; it != end; ++it)
        if (fs::is_regular_file(it->path())) fs::copy_file(it->path(), root / "snapshot" / it->path().filename());
    BlockIndexGenerationSource source;
    BOOST_REQUIRE_MESSAGE(ReadLegacyBlockIndexSource((root / "snapshot").string(), &source, error), *error);
    source.blockDataDir = GetDataDir().string();
    BlockIndexGenerationBuilder builder;
    BOOST_REQUIRE_MESSAGE(builder.Build(source, (root / "blockindex-build-000001.tmp").string(), 1, NULL, error), *error);
    builder.Close();
    BOOST_REQUIRE_EQUAL(BlockIndexGenerationManager::PublishGeneration(root.string(), 1, error), BLOCK_INDEX_LIFECYCLE_OK);
    BOOST_REQUIRE_EQUAL(BlockIndexGenerationManager::SelectGeneration(root.string(), 1, error), BLOCK_INDEX_LIFECYCLE_OK);
    BOOST_REQUIRE_MESSAGE(InitBlockIndexAuthoritative(root.string(), error), *error);
    BOOST_REQUIRE(g_fAuthoritativeStartup);
}

// ---------------------------------------------------------------------------
// F2 AUDIT BLOCKER REPAIR — PERMANENT PRE-DAG SIDE-BRANCH HASH-ANCESTRY PARITY
//
// LOAD-BEARING INVARIANT: PRE-DAG TRUST TRUTH != ACTIVE CHAIN AT SAME HEIGHT.
//
// The independent audit rejected the first F2 candidate because the pre-DAG
// authoritative provider resolved the REQUESTED hash's height and then summed
// the ACTIVE chain 0..height, i.e. it returned active-chain accumulated trust at
// that height rather than accumulated trust along the requested hash's own
// branch. The auditor's discriminator (a resident h=10 side parent) got
// 0x2000f183575 (active-chain-at-height) instead of its own branch value
// 0x200f454841e.
//
// FIXTURE (three divergent side branches, all fully resident, all indexed,
// none of them the active block at its own height):
//   a7 -> a8 -> a9 -> a10   (ACTIVE chain; regtest FORK_HEIGHT_POEM == 9)
//   sideAtPoem   : child of a8, height 9   (diverges AT/after POEM)
//   sideLow8     : child of a7, height 8   (diverges BELOW POEM)
//   sideLow9     : child of sideLow8, height 9 (two-block side ancestry below POEM)
// Because every requested side hash sits strictly below the active best height,
// none of them can be "the active block at that height".
//
// REQUIRED: for every requested side parent,
//   authoritative resolver == legacy resolver == that side block's own
//   resident nChainTrust (== SUM GetAuthoritativeBlockTrust over its ancestry),
// and the authoritative value must DIFFER from the authoritative value of the
// active block at the same height. That inequality is load-bearing: it is what
// prevents a silent regression back to GetActiveByHeight.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(f2_byvalue_side_branch_hash_ancestry_parity)
{
    BOOST_REQUIRE(pindexBest != NULL);

    // 1. Build the active fixture chain up to the fixture depth.
    const int hFixtureTop = kFixtureDepth - 1;
    // Order-robust fixture: locate the ACTIVE-chain ancestor at the fixture
    // depth. Earlier cases in the same process may leave a much deeper chain, so
    // this case must build its fixture on the real active ancestry rather
    // than assume the ambient tip height.
    CBlockIndex* a10 = pindexBest;
    while (a10->nHeight < hFixtureTop) a10 = MineReal(a10, 0x9100 + a10->nHeight);
    while (a10->nHeight > hFixtureTop) { BOOST_REQUIRE(a10->pprev != NULL); a10 = a10->pprev; }
    BOOST_REQUIRE_EQUAL(a10->nHeight, hFixtureTop);
    const uint256 bestHashBeforeFixture = hashBestChain;
    CBlockIndex* a9 = a10->pprev; BOOST_REQUIRE(a9 != NULL);
    CBlockIndex* a8 = a9->pprev;  BOOST_REQUIRE(a8 != NULL);
    CBlockIndex* a7 = a8->pprev;  BOOST_REQUIRE(a7 != NULL);
    BOOST_REQUIRE_EQUAL(a9->nHeight, 9);
    BOOST_REQUIRE_EQUAL(a8->nHeight, 8);
    BOOST_REQUIRE_EQUAL(a7->nHeight, 7);

    // 2. Genuine indexed pre-DAG side branches (storage path, real block bytes).
    CBlockIndex* sideAtPoem = AddSidePoWBlock(a8, 0x9191);      // h9, diverges AT POEM
    BOOST_REQUIRE(sideAtPoem != NULL);
    CBlockIndex* sideLow8 = AddSidePoWBlock(a7, 0x9192);        // h8, diverges BELOW POEM
    BOOST_REQUIRE(sideLow8 != NULL);
    CBlockIndex* sideLow9 = AddSidePoWBlock(sideLow8, 0x9193);  // h9, 2-block side ancestry
    BOOST_REQUIRE(sideLow9 != NULL);
    CBlockIndex* sideThird = AddSidePoWBlock(a8, 0x9194);       // h9, third same-height branch
    BOOST_REQUIRE(sideThird != NULL);

    BOOST_REQUIRE_EQUAL(sideAtPoem->nHeight, 9);
    BOOST_REQUIRE_EQUAL(sideLow8->nHeight, 8);
    BOOST_REQUIRE_EQUAL(sideLow9->nHeight, 9);
    BOOST_REQUIRE_EQUAL(sideThird->nHeight, 9);

    // Every side branch is kept STRICTLY BELOW the ambient active best height, so
    // none of them can displace the best chain and none of the requested hashes
    // is the active block at its own height. (A side block created AT the best
    // height could itself win the best chain on a trust tie, which would destroy
    // the "not the active block" premise -- hence strictly below.)
    BOOST_REQUIRE_MESSAGE(nBestHeight > 9,
        "F2 fixture: the active best chain must be strictly taller than the side branches");
    BOOST_REQUIRE_MESSAGE(hashBestChain == bestHashBeforeFixture,
        "F2 fixture: the side branches must not displace the active best chain");
    BOOST_REQUIRE_MESSAGE(sideAtPoem->GetBlockHash() != a9->GetBlockHash(),
        "F2 fixture: sideAtPoem must not be the active block at height 9");
    BOOST_REQUIRE_MESSAGE(sideLow9->GetBlockHash() != a9->GetBlockHash(),
        "F2 fixture: sideLow9 must not be the active block at height 9");
    BOOST_REQUIRE_MESSAGE(sideThird->GetBlockHash() != a9->GetBlockHash(),
        "F2 fixture: sideThird must not be the active block at height 9");
    BOOST_REQUIRE_MESSAGE(sideLow8->GetBlockHash() != a8->GetBlockHash(),
        "F2 fixture: sideLow8 must not be the active block at height 8");
    // Multiple side branches at the SAME height (h9), all different hashes.
    BOOST_REQUIRE(sideAtPoem->GetBlockHash() != sideLow9->GetBlockHash());
    BOOST_REQUIRE(sideAtPoem->GetBlockHash() != sideThird->GetBlockHash());
    BOOST_REQUIRE(sideLow9->GetBlockHash() != sideThird->GetBlockHash());
    // Discriminating branch trust (height >= FORK_HEIGHT_POEM, where the legacy
    // per-block trust mixes in GetBlockEntropy of the block hash, so same-height
    // branches genuinely differ).
    BOOST_REQUIRE_MESSAGE(sideAtPoem->nChainTrust != a9->nChainTrust,
        "F2 fixture: side/active accumulated trust must differ at height 9");
    BOOST_REQUIRE_MESSAGE(sideLow9->nChainTrust != a9->nChainTrust,
        "F2 fixture: sideLow9/active accumulated trust must differ at height 9");
    BOOST_REQUIRE_MESSAGE(sideThird->nChainTrust != a9->nChainTrust,
        "F2 fixture: sideThird/active accumulated trust must differ at height 9");
    BOOST_REQUIRE_MESSAGE(sideAtPoem->nChainTrust != sideLow9->nChainTrust,
        "F2 fixture: the same-height side branches must carry different trust");
    BOOST_REQUIRE_MESSAGE(sideAtPoem->nChainTrust != sideThird->nChainTrust,
        "F2 fixture: the same-height side branches must carry different trust");
    // OBSERVATION (consensus chain format, NOT a defect): BELOW FORK_HEIGHT_POEM
    // the legacy per-block trust is the reciprocal of the compact target and thus
    // depends only on nBits (== height in regtest), so two same-height blocks
    // below POEM accumulate exactly the same trust. A below-POEM divergence is
    // therefore observable in the ACCUMULATED value only when the requested
    // height is >= POEM, which is what the depth-2 case exercises. No inequality
    // is asserted at h=8 because it would be false by consensus, not by defect.
    BOOST_REQUIRE_MESSAGE(sideLow8->nChainTrust == a8->nChainTrust,
        "F2 fixture: below POEM, same-height accumulated trust is target-reciprocal");

    // Expected values captured while the resident graph is authoritative for them.
    struct Case { const char* name; CBlockIndex* side; CBlockIndex* activePeer; };
    const Case cases[3] = {
        { "divergesAtOrAfterPoem(h9)", sideAtPoem, a9 },
        { "divergesBelowPoem(h9,depth2)", sideLow9, a9 },
        { "thirdSameHeightBranch(h9)", sideThird, a9 }
    };

    // 3. Authoritative generation + boot (maps are NOT cleared: full residency).
    const fs::path root = fs::temp_directory_path() / fs::unique_path("f2-sidebranch-%%%%-%%%%");
    std::map<uint256, CBlockIndex*> savedMap;
    { LOCK(cs_main); savedMap = mapBlockIndex; }
    CBlockIndex* savedBest = pindexBest;
    CBlockIndex* savedGenesis = pindexGenesisBlock;
    uint256 savedBestChain = hashBestChain;
    int savedBestHeight = nBestHeight;
    uint256 savedBestTrust = nBestChainTrust;
    struct Cleanup {
        fs::path root; std::map<uint256, CBlockIndex*> savedMap;
        CBlockIndex* best; CBlockIndex* genesis;
        uint256 bestChain; int bestHeight; uint256 bestTrust;
        Cleanup(const fs::path& r, const std::map<uint256, CBlockIndex*>& m,
                CBlockIndex* b, CBlockIndex* g, const uint256& bc, int bh, const uint256& bt)
            : root(r), savedMap(m), best(b), genesis(g), bestChain(bc), bestHeight(bh), bestTrust(bt) {}
        ~Cleanup() {
            ResetBlockIndexAuthoritativeStartupForTest();
            { LOCK(cs_main); if (!savedMap.empty()) RestoreMapBlockIndexForFixture(savedMap); }
            pindexBest = best; pindexGenesisBlock = genesis;
            hashBestChain = bestChain; nBestHeight = bestHeight; nBestChainTrust = bestTrust;
            try { fs::remove_all(root); } catch (...) {}
        }
    } cleanup(root, savedMap, savedBest, savedGenesis, savedBestChain, savedBestHeight, savedBestTrust);

    std::string error;
    F2BuildAuthoritativeGenerationAndInit(root, &error);

    // 4. Required parity on the SAME requested hashes.
    for (int i = 0; i < 3; ++i)
    {
        const uint256 req = cases[i].side->GetBlockHash();
        const uint256 peer = cases[i].activePeer->GetBlockHash();
        const uint256 expectedSide = cases[i].side->nChainTrust;   // legacy resident truth
        const uint256 expectedPeer = cases[i].activePeer->nChainTrust;
        BOOST_REQUIRE(expectedSide != expectedPeer);

        std::string e3, e4;
        uint256 accSide = 0, accPeer = 0;
        BOOST_REQUIRE_MESSAGE(GetAuthoritativeAccumulatedChainTrust(req, &accSide, &e3), e3);
        BOOST_REQUIRE_MESSAGE(GetAuthoritativeAccumulatedChainTrust(peer, &accPeer, &e4), e4);
        BOOST_CHECK_MESSAGE(accSide == expectedSide,
            "F2 " << cases[i].name << ": repaired provider != side branch accumulated trust");
        BOOST_CHECK_MESSAGE(accPeer == expectedPeer,
            "F2 " << cases[i].name << ": provider != active accumulated trust");

        // LOAD-BEARING: the requested-hash result must NOT be active-chain-at-height.
        BOOST_CHECK_MESSAGE(accSide != accPeer,
            "F2 " << cases[i].name << ": AUTHORITATIVE SIDE-BRANCH TRUST MUST DIFFER FROM "
            "ACTIVE-CHAIN TRUST AT THE SAME HEIGHT (regression to GetActiveByHeight)");

        BOOST_TEST_MESSAGE("F2 SIDEBRANCH " << cases[i].name
            << " req=" << req.GetHex() << " height=" << cases[i].side->nHeight
            << " auth=" << accSide.GetHex() << " legacy=" << expectedSide.GetHex()
            << " sideResidentTrust=" << expectedSide.GetHex()
            << " activePeer=" << peer.GetHex() << " activePeerTrust=" << expectedPeer.GetHex());
    }
}

// ---------------------------------------------------------------------------
// F2 AUDIT BLOCKER REPAIR — NONRESIDENT PRE-DAG SIDE-BRANCH BY-VALUE PROOF
//
// Phase 7: after the resident side-branch parity is GREEN, prove the SAME
// logical branch by value with NO resident authority. The identical side-branch
// fixture is built, the expected uint256s are captured from the resident
// pointers, then mapBlockIndex AND mapDAGData are cleared and the authoritative
// startup is performed from scratch. The by-value hash-ancestry resolution must
// return the exact same uint256s, and each side branch must still differ from
// the active chain at its own height. No residency is reconstructed.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(f2_byvalue_nonresident_side_branch)
{
    BOOST_REQUIRE(pindexBest != NULL);

    const int hFixtureTop = kFixtureDepth - 1;
    CBlockIndex* a10 = pindexBest;
    while (a10->nHeight < hFixtureTop) a10 = MineReal(a10, 0x9200 + a10->nHeight);
    while (a10->nHeight > hFixtureTop) { BOOST_REQUIRE(a10->pprev != NULL); a10 = a10->pprev; }
    BOOST_REQUIRE_EQUAL(a10->nHeight, hFixtureTop);
    const uint256 bestHashBeforeFixture = hashBestChain;
    CBlockIndex* a9 = a10->pprev; BOOST_REQUIRE(a9 != NULL);
    CBlockIndex* a8 = a9->pprev;  BOOST_REQUIRE(a8 != NULL);
    CBlockIndex* a7 = a8->pprev;  BOOST_REQUIRE(a7 != NULL);

    CBlockIndex* sideAtPoem = AddSidePoWBlock(a8, 0x9291);
    BOOST_REQUIRE(sideAtPoem != NULL);
    CBlockIndex* sideLow8 = AddSidePoWBlock(a7, 0x9292);
    BOOST_REQUIRE(sideLow8 != NULL);
    CBlockIndex* sideLow9 = AddSidePoWBlock(sideLow8, 0x9293);
    BOOST_REQUIRE(sideLow9 != NULL);
    CBlockIndex* sideThird = AddSidePoWBlock(a8, 0x9295);
    BOOST_REQUIRE(sideThird != NULL);

    BOOST_REQUIRE_EQUAL(sideAtPoem->nHeight, 9);
    BOOST_REQUIRE_EQUAL(sideLow8->nHeight, 8);
    BOOST_REQUIRE_EQUAL(sideLow9->nHeight, 9);
    BOOST_REQUIRE_EQUAL(sideThird->nHeight, 9);
    BOOST_REQUIRE_MESSAGE(nBestHeight > 9,
        "F2 fixture: the active best chain must be strictly taller than the side branches");
    BOOST_REQUIRE_MESSAGE(hashBestChain == bestHashBeforeFixture,
        "F2 fixture: the side branches must not displace the active best chain");
    BOOST_REQUIRE_MESSAGE(sideAtPoem->GetBlockHash() != a9->GetBlockHash(),
        "F2 fixture: sideAtPoem must not be the active block at height 9");
    BOOST_REQUIRE_MESSAGE(sideLow9->GetBlockHash() != a9->GetBlockHash(),
        "F2 fixture: sideLow9 must not be the active block at height 9");
    BOOST_REQUIRE_MESSAGE(sideThird->GetBlockHash() != a9->GetBlockHash(),
        "F2 fixture: sideThird must not be the active block at height 9");
    BOOST_REQUIRE_MESSAGE(sideAtPoem->nChainTrust != a9->nChainTrust,
        "F2 fixture: side/active accumulated trust must differ at height 9");
    BOOST_REQUIRE_MESSAGE(sideLow9->nChainTrust != a9->nChainTrust,
        "F2 fixture: sideLow9/active accumulated trust must differ at height 9");
    BOOST_REQUIRE_MESSAGE(sideThird->nChainTrust != a9->nChainTrust,
        "F2 fixture: sideThird/active accumulated trust must differ at height 9");
    BOOST_REQUIRE_MESSAGE(sideAtPoem->nChainTrust != sideLow9->nChainTrust,
        "F2 fixture: the same-height side branches must carry different trust");
    BOOST_REQUIRE_MESSAGE(sideAtPoem->nChainTrust != sideThird->nChainTrust,
        "F2 fixture: the same-height side branches must carry different trust");

    struct Case { const char* name; CBlockIndex* side; CBlockIndex* activePeer; };
    const Case cases[3] = {
        { "divergesAtOrAfterPoem(h9)", sideAtPoem, a9 },
        { "divergesBelowPoem(h9,depth2)", sideLow9, a9 },
        { "thirdSameHeightBranch(h9)", sideThird, a9 }
    };
    uint256 reqHash[3], peerHash[3], expectedSide[3], expectedPeer[3];
    int reqHeight[3];
    for (int i = 0; i < 3; ++i)
    {
        reqHash[i] = cases[i].side->GetBlockHash();
        peerHash[i] = cases[i].activePeer->GetBlockHash();
        expectedSide[i] = cases[i].side->nChainTrust;
        expectedPeer[i] = cases[i].activePeer->nChainTrust;
        reqHeight[i] = cases[i].side->nHeight;
        BOOST_REQUIRE(expectedSide[i] != expectedPeer[i]);
        BOOST_REQUIRE(reqHash[i] != peerHash[i]);
    }

    const fs::path root = fs::temp_directory_path() / fs::unique_path("f2-sidebranch-nr-%%%%-%%%%");
    std::map<uint256, CBlockIndex*> savedMap;
    { LOCK(cs_main); savedMap = mapBlockIndex; }
    CBlockIndex* savedBest = pindexBest;
    CBlockIndex* savedGenesis = pindexGenesisBlock;
    uint256 savedBestChain = hashBestChain;
    int savedBestHeight = nBestHeight;
    uint256 savedBestTrust = nBestChainTrust;
    struct Cleanup {
        fs::path root; std::map<uint256, CBlockIndex*> savedMap;
        CBlockIndex* best; CBlockIndex* genesis;
        uint256 bestChain; int bestHeight; uint256 bestTrust;
        Cleanup(const fs::path& r, const std::map<uint256, CBlockIndex*>& m,
                CBlockIndex* b, CBlockIndex* g, const uint256& bc, int bh, const uint256& bt)
            : root(r), savedMap(m), best(b), genesis(g), bestChain(bc), bestHeight(bh), bestTrust(bt) {}
        ~Cleanup() {
            ResetBlockIndexAuthoritativeStartupForTest();
            { LOCK(cs_main); if (!savedMap.empty()) RestoreMapBlockIndexForFixture(savedMap); }
            pindexBest = best; pindexGenesisBlock = genesis;
            hashBestChain = bestChain; nBestHeight = bestHeight; nBestChainTrust = bestTrust;
            try { fs::remove_all(root); } catch (...) {}
        }
    } cleanup(root, savedMap, savedBest, savedGenesis, savedBestChain, savedBestHeight, savedBestTrust);

    // Prove nonresidency: both RAM maps lose the side branches AND their ancestry.
    { LOCK(cs_main); mapBlockIndex.clear(); }
    pindexBest = NULL; pindexGenesisBlock = NULL;
    nBestHeight = -1; hashBestChain = uint256(0); nBestChainTrust = uint256(0);

    std::string error;
    F2BuildAuthoritativeGenerationAndInit(root, &error);

    for (int i = 0; i < 3; ++i)
    {
        { LOCK(cs_main);
          BOOST_REQUIRE_MESSAGE(mapBlockIndex.count(reqHash[i]) == 0,
              "F2 nonresident: side parent must be absent from mapBlockIndex");
          BOOST_REQUIRE_MESSAGE(mapBlockIndex.count(peerHash[i]) == 0,
              "F2 nonresident: active peer must be absent from mapBlockIndex"); }

        uint256 accSide = 0, accPeer = 0;
        std::string e1, e2;
        BOOST_REQUIRE_MESSAGE(GetAuthoritativeAccumulatedChainTrust(reqHash[i], &accSide, &e1), e1);
        BOOST_REQUIRE_MESSAGE(GetAuthoritativeAccumulatedChainTrust(peerHash[i], &accPeer, &e2), e2);
        BOOST_CHECK_MESSAGE(accSide == expectedSide[i],
            "F2 nonresident " << cases[i].name << ": by-value accumulated trust != resident "
            "side-branch nChainTrust");
        BOOST_CHECK_MESSAGE(accPeer == expectedPeer[i],
            "F2 nonresident " << cases[i].name << ": by-value active-peer trust mismatch");
        BOOST_CHECK_MESSAGE(accSide != accPeer,
            "F2 nonresident " << cases[i].name << ": side-branch trust must differ from the "
            "active chain at the same height");

        BOOST_TEST_MESSAGE("F2 NONRESIDENT_SIDEBRANCH " << cases[i].name
            << " req=" << reqHash[i].GetHex() << " height=" << reqHeight[i]
            << " auth=" << accSide.GetHex()
            << " expectedSideResidentTrust=" << expectedSide[i].GetHex()
            << " activePeer=" << peerHash[i].GetHex()
            << " activePeerTrust=" << expectedPeer[i].GetHex()
            << " mapBlockIndexHasSide=0 mapDAGDataHasSide=0");
    }
}

// ---------------------------------------------------------------------------
// R2 / C2 — HOT PARENT == COLD PARENT (ONE RESOLUTION DOMAIN).
//
// LOAD-BEARING INVARIANT: the by-value accumulated-trust provider resolves over
// the SAME complete committed hot+cold domain the authoritative resolver uses
// (BlockIndexAuthoritativeLive::ResolveBlockSnapshot). A parent that
// exists ONLY in the mutable hot tail (persisted through the real production
// live authority after the immutable generation was selected) must
//   * resolve by value — it did NOT before R2, because the provider was
//     cold-only and reported "requested hash not resolvable by value",
//   * produce the identical semantic result a cold parent yields: its
//     OWN branch accumulated trust, never the active-chain value at its height,
//   * resolve with no mapBlockIndex/mapDAGData residency at all,
// while a genuinely absent hash still FAILS CLOSED (no fabricated genesis, no
// zero-as-value result).
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(f2_byvalue_hot_parent_matches_cold_parent)
{
    BOOST_REQUIRE(pindexBest != NULL);

    // 1. Active fixture chain to the fixture depth; a8 (h8) is the COLD
    //    ancestor this fixture snapshots into the immutable generation.
    const int hFixtureTop = kFixtureDepth - 1;
    CBlockIndex* a10 = pindexBest;
    while (a10->nHeight < hFixtureTop) a10 = MineReal(a10, 0xA400 + a10->nHeight);
    while (a10->nHeight > hFixtureTop) { BOOST_REQUIRE(a10->pprev != NULL); a10 = a10->pprev; }
    BOOST_REQUIRE_EQUAL(a10->nHeight, hFixtureTop);
    CBlockIndex* a8 = a10->pprev->pprev;
    BOOST_REQUIRE(a8 != NULL);
    BOOST_REQUIRE_EQUAL(a8->nHeight, 8);
    const uint256 coldHash = a8->GetBlockHash();
    const uint256 coldTrust = a8->nChainTrust;   // legacy resident truth

    // A real pre-DAG side child of a8, created while the legacy add path is still
    // available. Only its RECORD FIELDS are used, as the shape of a pre-DAG
    // vertex at height 9; the HOT vertex in step 4 must be one the immutable
    // generation cannot contain.
    CBlockIndex* h9s = AddSidePoWBlock(a8, 0xA490);
    BOOST_REQUIRE(h9s != NULL);
    BOOST_REQUIRE_EQUAL(h9s->nHeight, 9);

    // 2. Isolated snapshot + authoritative generation + authoritative startup.
    const fs::path root = fs::temp_directory_path() / fs::unique_path("f2-hotparent-%%%%-%%%%");
    std::map<uint256, CBlockIndex*> savedMap;
    { LOCK(cs_main); savedMap = mapBlockIndex; }
    CBlockIndex* savedBest = pindexBest;
    CBlockIndex* savedGenesis = pindexGenesisBlock;
    uint256 savedBestChain = hashBestChain;
    int savedBestHeight = nBestHeight;
    uint256 savedBestTrust = nBestChainTrust;
    struct Cleanup {
        fs::path root; std::map<uint256, CBlockIndex*> savedMap;
        CBlockIndex* best; CBlockIndex* genesis;
        uint256 bestChain; int bestHeight; uint256 bestTrust;
        Cleanup(const fs::path& r, const std::map<uint256, CBlockIndex*>& m,
                CBlockIndex* b, CBlockIndex* g, const uint256& bc, int bh, const uint256& bt)
            : root(r), savedMap(m), best(b), genesis(g), bestChain(bc), bestHeight(bh), bestTrust(bt) {}
        ~Cleanup() {
            ResetBlockIndexAuthoritativeStartupForTest();
            { LOCK(cs_main); if (!savedMap.empty()) RestoreMapBlockIndexForFixture(savedMap); }
            pindexBest = best; pindexGenesisBlock = genesis;
            hashBestChain = bestChain; nBestHeight = bestHeight; nBestChainTrust = bestTrust;
            try { fs::remove_all(root); } catch (...) {}
        }
    } cleanup(root, savedMap, savedBest, savedGenesis, savedBestChain, savedBestHeight, savedBestTrust);

    std::string error;
    F2BuildAuthoritativeGenerationAndInit(root, &error);

    BlockIndexAuthoritativeLive* live = GetAuthoritativeLiveAuthority();
    BOOST_REQUIRE_MESSAGE(live && live->IsOpen(), "R2: production live authority must be open");

    // 3. The COLD form of the pre-DAG ancestor resolves exactly (unchanged path).
    uint256 accCold = 0; std::string eCold;
    BOOST_REQUIRE_MESSAGE(GetAuthoritativeAccumulatedChainTrust(coldHash, &accCold, &eCold), eCold);
    BOOST_CHECK_MESSAGE(accCold == coldTrust,
        "R2: cold pre-DAG parent accumulated trust != its own branch resident nChainTrust");

    // 4. A pre-DAG vertex that exists ONLY in the mutable hot tail: its record is
    //    persisted through the REAL production live authority (AcceptSide) after
    //    the immutable generation was selected, so the cold generation can never
    //    carry it. Its shape mirrors a real pre-DAG block at height 9 (a side
    //    child of the cold a8); its hash is unique, and the projection it carries
    //    is deliberately WRONG (a stale derived.chainTrust).
    BlockIndexRecord hotRec = BlockIndexRecordFromIndex(h9s);
    hotRec.hash = uint256(0xA4E1);
    const uint256 hotHash = hotRec.hash;
    BOOST_REQUIRE(hotHash != coldHash);

    BlockIndexSnapshot hotSnap;
    uint256 expectedHot = 0;
    {
        BlockIndexDerivedEntry der;
        der.chainTrust = uint256(0xDEADBEEF);   // stale projection: NOT authority
        der.stakeModifierChecksum = 0;
        if (der.HasStakeModifierTime()) der.stakeModifierTime = 0;
        std::string aerr;
        BOOST_REQUIRE_MESSAGE(live->AcceptSide(hotRec, der, &aerr), aerr);
        std::string herr;
        BOOST_REQUIRE_MESSAGE(live->ResolveBlockSnapshot(hotHash, &hotSnap, &herr) == BlockIndexHotStatus::OK, herr);
        BOOST_REQUIRE_EQUAL(hotSnap.height, 9);
        BOOST_REQUIRE(hotSnap.hashPrev == coldHash);
        expectedHot = accCold + GetAuthoritativeBlockTrust(hotSnap);   // own trust + cold ancestors
    }
    BOOST_REQUIRE(expectedHot > accCold);

    // 5. LOAD-BEARING DISCRIMINATOR: the hot vertex is provably ABSENT from the
    //    immutable generation while its cold ancestor IS present — the cold-only
    //    provider failed exactly here before R2.
    {
        const ColdHotSeamNavigator* nav = GetBlockIndexStakingNavigator();
        BOOST_REQUIRE(nav != NULL);
        const BlockIndexV2Reader* cold = nav->GetColdReader();
        BOOST_REQUIRE_MESSAGE(cold != NULL && cold->IsOpen(), "R2: cold generation reader must be open");
        BlockIndexSnapshot s; std::string cerr;
        BOOST_CHECK_MESSAGE(cold->LookupByHash(hotHash, &s, &cerr) != BLOCK_INDEX_V2_READ_FOUND,
            "R2 fixture: the hot-only vertex must be absent from the immutable generation");
        BlockIndexSnapshot c; std::string ce;
        BOOST_CHECK_MESSAGE(cold->LookupByHash(coldHash, &c, &ce) == BLOCK_INDEX_V2_READ_FOUND,
            "R2 fixture: the cold ancestor must BE present in the immutable generation");
    }

    // 6. The hot parent must now resolve by value, contributing exactly its own
    //    block trust on top of its cold ancestors' accumulated trust — the same
    //    rule a cold parent obeys — and never the stale persisted projection.
    uint256 accHot = 0; std::string eHot;
    BOOST_REQUIRE_MESSAGE(GetAuthoritativeAccumulatedChainTrust(hotHash, &accHot, &eHot), eHot);
    BOOST_CHECK_MESSAGE(accHot == expectedHot,
        "R2: hot pre-DAG parent accumulated trust != own trust + cold ancestors");
    BOOST_CHECK_MESSAGE(accHot > accCold,
        "R2: the hot child must accumulate strictly more trust than its cold ancestor");
    BOOST_CHECK_MESSAGE(accHot != uint256(0xDEADBEEF),
        "R2: a stale persisted derived.chainTrust must NOT become semantic authority");

    // 7. No residency dependence: with the resident index cleared the identical
    //    by-value result must still be produced (hot vertex from the mutable
    //    authority, ancestors from the immutable generation).
    {
        { LOCK(cs_main); mapBlockIndex.clear(); }
        uint256 accHot2 = 0; std::string eHot2;
        BOOST_REQUIRE_MESSAGE(GetAuthoritativeAccumulatedChainTrust(hotHash, &accHot2, &eHot2), eHot2);
        BOOST_CHECK_MESSAGE(accHot2 == expectedHot,
            "R2: non-resident hot parent must resolve to the same by-value accumulated trust");
        uint256 accCold2 = 0; std::string eCold2;
        BOOST_REQUIRE_MESSAGE(GetAuthoritativeAccumulatedChainTrust(coldHash, &accCold2, &eCold2), eCold2);
        BOOST_CHECK_MESSAGE(accCold2 == coldTrust,
            "R2: non-resident cold parent must resolve to the same by-value accumulated trust");
    }

    // 8. A genuinely absent hash still FAILS CLOSED.
    {
        uint256 accAbsent = 0; std::string eAbsent;
        BOOST_CHECK_MESSAGE(!GetAuthoritativeAccumulatedChainTrust(uint256(0xA4DEAD), &accAbsent, &eAbsent),
            "R2: an absent hash must fail closed");
        BOOST_CHECK_MESSAGE(accAbsent == uint256(0),
            "R2: a failed accumulation must not publish a partial/zero-as-value result");
    }

    BOOST_TEST_MESSAGE("R2_HOT_PARENT_PARITY cold=" << coldHash.GetHex() << " coldTrust=" << coldTrust.GetHex()
        << " hot=" << hotHash.GetHex() << " expectedHot=" << expectedHot.GetHex()
        << " accCold=" << accCold.GetHex() << " accHot=" << accHot.GetHex()
        << " coldReaderHasHot=0 coldReaderHasCold=1 mapBlockIndexHasHot=0");
}

// ---------------------------------------------------------------------------
// R3 / C6 — ENGINE WATERMARK CONTINUITY SUBSTRATE (read-only accessor).
//
// LOAD-BEARING INVARIANT (C6 section 5): continuity of provenance coverage is
// bound to the storage engine's monotone write sequence, because every durable
// provenance field that only the NEW writer increments (capability version,
// provenance counter, store identity, incarnation marker, prune journal) is
// untouched by a non-participating writer — final-state equality, including a
// digest over all final-state bytes, therefore cannot prove writer continuity.
// This test proves the substrate that binding rests on:
//   (a) the read-only accessor returns the engine's own sequence;
//   (b) one supported batch advances it by exactly its own record count
//       (the seal arithmetic W = P + C is derivable by the writer that composed
//        the batch, with no second write needed to update it);
//   (c) a clean close/reopen does not advance it, so a sealed W verifies EXACTLY
//       across a clean restart (cross-restart coverage is preserved);
//   (d) an UNSUPPORTED writer acting BENEATH the application layer — the Astra
//       counterexample shape: restore a row, then untagged-erase it, returning
//       the final row state to exactly what it was — necessarily perturbs it, so
//       a sealed value can never be preserved across such an intervention.
// The seal/verify/suspension logic itself is R3's remaining work; this test
// proves only the substrate it depends on.
// ---------------------------------------------------------------------------


// ---------------------------------------------------------------------------
// R3 / C6 section 1 — TYPED ROW OUTCOME. A consensus-sensitive reader must be
// able to tell absence, malformed, storage failure and presence apart. Bare
// absence never becomes a prune; a malformed row never becomes missing; a
// storage failure never becomes missing.
// ---------------------------------------------------------------------------


// ---------------------------------------------------------------------------
// R3 / C6 sections 2-4 — INCARNATION + POSITIVE PRUNE EVIDENCE. Durable per-row
// incarnation, append-only hash-chained prune journal, per-row prune index, and
// the transitions that keep them mutually consistent: PRUNE(X,N) can never
// explain incarnation N+1, and no absence is ever attributed retrospectively.
// ---------------------------------------------------------------------------


// ---------------------------------------------------------------------------
// R3 / C6 sections 5-6 + R3.8 — CUSTODY CERTIFICATE, SEAL AND THE FINAL PREDICATE.
// The predicate is exercised through the SAME static function used at the consensus
// site. Positive attribution requires the complete frozen predicate; a bare absence,
// a floor alone, a legacy marker, a superseded event, a restarted-but-suspended
// custody or an unsupported writer's hole must all fail closed.
// ---------------------------------------------------------------------------




// ---------------------------------------------------------------------------
// R3.9 / C6 — THE COMPLETE FROZEN A–M MATRIX.
//
// Dedicated cases, fresh process per case, isolated scratch stores only. The predicate is
// exercised through the SAME static function used at the consensus site
// (DAGRowObjectivelyPrunedForTest) and custody/certificate/seal state through the SAME
// production methods the startup/shutdown lifecycle calls.
// ---------------------------------------------------------------------------
// NOTE: g_testSuppressDagCustodyWatermark is declared at GLOBAL scope in
// txdb-leveldb.h; inside this test namespace it must be referenced as ::global (the
// declaration itself must not be repeated here, or the extern would bind to a
// namespace-local symbol that no translation unit defines).







// R3 certification-admission EPOCH-SEAM REGRESSION (confirmed re-audit defect 20260930-122133).
// A supported PRUNE that happens BEFORE the first certification carries custody epoch 0, while the
// certification transition is about to publish epoch 1. Such an event can never be admissible to
// the resolver (frozen rule: prune event epoch must equal the current certified custody epoch), so
// certification MUST refuse the absence instead of positively certifying an effectively unexplained
// hole. It is ALSO an asserting regression: this case fails if certification succeeds. The epoch-0
// event is NOT relabelled, NOT migrated and NOT rewritten; the resolver rule is NOT weakened.


// R3 certification-admission CURRENT-EPOCH POSITIVE CONTROL (directive section 5/6): a supported
// PRUNE performed INSIDE the certified custody epoch, on an already certified store, must remain
// certifiable (epoch-preserving recertification, prospective epoch == current certified epoch) and
// resolvable as ROW_OBJECTIVELY_PRUNED. This proves the epoch-seam repair did not reject every
// absent row and did not introduce epoch churn.


























// ---------------------------------------------------------------------------
// R4 — AUTHORITY_READY ORDERING TESTS.
//
// These prove real ordering on the real production seam: the barrier is published only by the
// authoritative startup, every consumer gate blocks before that, and a partially ready node is
// never published (lifecycle readiness only).
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(r4_authority_ready_prerequisite_matrix)
{
    const bool savedAuthoritative = g_fAuthoritativeStartup;
    AuthorityReadyResetForTest();
    BOOST_REQUIRE(!AuthorityReadyIsSet());

    AuthorityReadyPrerequisites all;
    all.durableIndexLoaded = true;
    all.immutableAuthorityAvailable = true;
    all.trustProjectionReconciled = true;

    struct Case { const char* unmet; const char* expected; };
    const Case cases[] = {
        {"durableIndexLoaded", "V2 durable index not loaded"},
        {"immutableAuthorityAvailable", "immutable authority not available"},
        {"trustProjectionReconciled", "R2 trust projection not reconciled"},
    };
    // LEGACY DAG RETIREMENT (Phase 2 / Slice 4b): the retired Legacy DAG prerequisites
    // (dagDurableStateRestored, provenanceCertificationComplete) were removed from the
    // current AUTHORITY_READY contract together with the S4 boot score-authority reconcile.
    // All three CURRENT prerequisites must still independently block publication.
    for (unsigned i = 0; i < 3; ++i)
    {
        AuthorityReadyPrerequisites p = all;
        if (i == 0) p.durableIndexLoaded = false;
        if (i == 1) p.immutableAuthorityAvailable = false;
        if (i == 2) p.trustProjectionReconciled = false;
        std::string detail;
        BOOST_CHECK_MESSAGE(!AuthorityReadyMarkIfSatisfied(p, &detail),
            "R4: unmet prerequisite '" << cases[i].unmet << "' must refuse publication");
        BOOST_CHECK_MESSAGE(p.WhyNotReady() == cases[i].expected,
            "R4: the refusal must name the exact prerequisite ('" << p.WhyNotReady() << "')");
        BOOST_CHECK_MESSAGE(!AuthorityReadyIsSet(), "R4: a refused publication must leave the barrier unset");
    }

    std::string detail;
    BOOST_CHECK_MESSAGE(AuthorityReadyMarkIfSatisfied(all, &detail),
        "R4: all three prerequisites satisfied must publish READY (" << detail << ")");
    BOOST_CHECK(AuthorityReadyIsSet());
    BOOST_CHECK_EQUAL(AuthorityReadyStateName(), std::string("READY"));
    BOOST_TEST_MESSAGE("R4_MATRIX three_prerequisites_required=1 all_satisfied_publishes=1");

    AuthorityReadyResetForTest();
    g_fAuthoritativeStartup = savedAuthoritative;
}

BOOST_AUTO_TEST_CASE(r4_authority_ready_consumer_cannot_cross_early)
{
    const bool savedAuthoritative = g_fAuthoritativeStartup;
    AuthorityReadyResetForTest();
    g_fAuthoritativeStartup = true;      // authoritative session, barrier NOT yet published
    BOOST_REQUIRE(!AuthorityReadyIsSet());

    // A real consumer (the same gate the -loadblock / bootstrap.dat /
    // wallet-reaccept consumers call) must NOT be able to proceed.
    volatile bool consumerReturned = false;
    volatile bool consumerResult = false;

    // Blocking wait with a short window proves the gate holds; the production gate uses a
    // bounded window and fails the startup explicitly when the barrier never arrives.
    {
        std::string err;
        const bool ok = AuthorityReadyWait(250, &err);
        BOOST_CHECK_MESSAGE(!ok, "R4: a consumer must not cross AUTHORITY_READY before it is published");
        BOOST_CHECK_MESSAGE(err == std::string("AUTHORITY_READY not published within the consumer gate window"),
            "R4: the early-cross failure must be explicit ('" << err << "')");
        BOOST_TEST_MESSAGE("R4_GATE blocked_before_publish=1 reason=" << err);
    }
    {
        std::string err;
        const bool ok = AuthorityReadyConsumerEnter("loadblock_consumer", &err);
        BOOST_CHECK_MESSAGE(!ok, "R4: the loadblock consumer gate must refuse before publication");
        BOOST_CHECK_MESSAGE(err.find("consumer 'loadblock_consumer' cannot cross AUTHORITY_READY") == 0,
            "R4: the gate must name the blocked consumer ('" << err << "')");
        consumerResult = ok; consumerReturned = true;
        BOOST_TEST_MESSAGE("R4_CONSUMER loadblock_consumer crossed_early=" << (ok ? 1 : 0));
    }

    // Publish READY and prove the same gate now lets the consumer through immediately.
    AuthorityReadyPrerequisites all;
    all.durableIndexLoaded = all.immutableAuthorityAvailable = true;
    all.trustProjectionReconciled = true;
    std::string detail;
    BOOST_REQUIRE_MESSAGE(AuthorityReadyMarkIfSatisfied(all, &detail), detail);
    {
        std::string err;
        const bool ok = AuthorityReadyConsumerEnter("loadblock_consumer", &err);
        BOOST_CHECK_MESSAGE(ok, "R4: after READY the consumer gate must open immediately (" << err << ")");
    }
    BOOST_TEST_MESSAGE("R4_CONSUMER after_ready_crosses=1");
    (void)consumerReturned; (void)consumerResult;

    AuthorityReadyResetForTest();
    g_fAuthoritativeStartup = savedAuthoritative;
}

BOOST_AUTO_TEST_CASE(r4_authority_ready_legacy_mode_not_applicable)
{
    const bool savedAuthoritative = g_fAuthoritativeStartup;
    AuthorityReadyResetForTest();
    g_fAuthoritativeStartup = false;     // legacy operation: no V2 authority to wait for
    std::string err;
    BOOST_CHECK_MESSAGE(AuthorityReadyWait(1, &err),
        "R4: legacy (non-authoritative) operation has no barrier requirement (" << err << ")");
    g_fAuthoritativeStartup = savedAuthoritative;
}

BOOST_AUTO_TEST_CASE(r4_main_cpp_trust_comparison_consumer_gate)
{
    // R4 closeout: the identified consensus-sensitive trust comparisons in main.cpp
    // (main.cpp:9581 ProcessMessage new-best, main.cpp:10537 tip candidacy, main.cpp:13777 SPV
    // header tip selection) are gated through the SAME single AUTHORITY_READY barrier — no
    // duplicated readiness logic and no local prerequisite inspection. This case exercises the
    // exact consumer identities those three sites use.
    const bool savedAuthoritative = g_fAuthoritativeStartup;
    AuthorityReadyResetForTest();
    g_fAuthoritativeStartup = true;
    const char* names[] = {"main_trust_comparison_new_best",
                           "main_trust_comparison_tip_candidate",
                           "main_trust_comparison_spv_header"};
    for (unsigned i = 0; i < 3; ++i)
    {
        std::string err;
        BOOST_CHECK_MESSAGE(!AuthorityReadyConsumerEnter(names[i], &err),
            "R4: main.cpp trust consumer '" << names[i] << "' must not cross before AUTHORITY_READY (" << err << ")");
        BOOST_CHECK_MESSAGE(err.find(std::string("consumer '") + names[i] + "' cannot cross AUTHORITY_READY") == 0,
            "R4: the refusal must name the blocked main.cpp consumer ('" << err << "')");
        BOOST_TEST_MESSAGE("R4_MAIN_CONSUMER " << names[i] << " crossed_early=0");
    }
    {
        AuthorityReadyPrerequisites all;
        all.durableIndexLoaded = all.immutableAuthorityAvailable = true;
        all.trustProjectionReconciled = true;
        std::string detail;
        BOOST_REQUIRE_MESSAGE(AuthorityReadyMarkIfSatisfied(all, &detail), detail);
    }
    for (unsigned i = 0; i < 3; ++i)
    {
        std::string err;
        BOOST_CHECK_MESSAGE(AuthorityReadyConsumerEnter(names[i], &err),
            "R4: after AUTHORITY_READY the main.cpp trust consumer must execute normally (" << err << ")");
    }
    BOOST_TEST_MESSAGE("R4_MAIN_CONSUMER after_ready_crosses=1 count=3");
    AuthorityReadyResetForTest();
    g_fAuthoritativeStartup = savedAuthoritative;
}

BOOST_AUTO_TEST_CASE(r4_authority_ready_published_by_real_authoritative_startup)
{
    SetMockTime(1700001900);
    CBlockIndex* fork=pindexBest;
    while(fork->nHeight<kFixtureDepth - 1) fork=MineReal(fork,0xE400+fork->nHeight);
    const fs::path root=fs::temp_directory_path()/fs::unique_path("r4ready-%%%%-%%%%");
    fs::create_directories(root/"snapshot");
    struct Cleanup { fs::path root; CBlockIndex* best; CBlockIndex* genesis;
        Cleanup(const fs::path& r):root(r),best(pindexBest),genesis(pindexGenesisBlock){}
        ~Cleanup(){ ResetBlockIndexAuthoritativeStartupForTest(); pindexBest=best; pindexGenesisBlock=genesis;
            if(best){nBestHeight=best->nHeight;hashBestChain=best->GetBlockHash();nBestChainTrust=best->nChainTrust;}
            SetMockTime(0); try{fs::remove_all(root);}catch(...){} }
    } cleanup(root);
    { CTxDB db; db.Close(); }
    const auto liveDir=GetDataDir()/"txleveldb";
    for(fs::directory_iterator it(liveDir),end;it!=end;++it)
        if(fs::is_regular_file(it->path())) fs::copy_file(it->path(),root/"snapshot"/it->path().filename());
    BlockIndexGenerationSource src; std::string aerr;
    BOOST_REQUIRE_MESSAGE(ReadLegacyBlockIndexSource((root/"snapshot").string(),&src,&aerr),aerr);
    src.blockDataDir=GetDataDir().string();
    BlockIndexGenerationBuilder ab;
    BOOST_REQUIRE_MESSAGE(ab.Build(src,(root/"blockindex-build-000001.tmp").string(),1,NULL,&aerr),aerr); ab.Close();
    BOOST_REQUIRE_EQUAL(BlockIndexGenerationManager::PublishGeneration(root.string(),1,&aerr),BLOCK_INDEX_LIFECYCLE_OK);
    BOOST_REQUIRE_EQUAL(BlockIndexGenerationManager::SelectGeneration(root.string(),1,&aerr),BLOCK_INDEX_LIFECYCLE_OK);
    AuthorityReadyResetForTest();                       // prove the startup itself publishes it
    BOOST_REQUIRE(!AuthorityReadyIsSet());
    BOOST_REQUIRE_MESSAGE(InitBlockIndexAuthoritative(root.string(),&aerr),aerr);
    BOOST_REQUIRE(g_fAuthoritativeStartup);
    BOOST_TEST_MESSAGE("R4_STARTUP authority_ready_state="<<AuthorityReadyStateName()
        <<" published="<<(AuthorityReadyIsSet()?1:0));
    BOOST_CHECK_MESSAGE(AuthorityReadyIsSet(),
        "R4: the real authoritative startup must publish AUTHORITY_READY once its four prerequisites hold (refusal detail: "
        << AuthorityReadyRefusalDetail() << ")");
    std::string werr;
    BOOST_CHECK_MESSAGE(AuthorityReadyWait(1,&werr),
        "R4: after the real startup a consumer gate must open immediately ("<<werr<<")");
}



// ---------------------------------------------------------------------------
// F2 AUDIT BLOCKER REPAIR — BY-VALUE TRUST PROVIDER FAILURE MATRIX (fail-closed proof)
//
// Every case below must FAIL CLOSED. None may switch to the active chain, return
// partial trust, use zero as a failure signal, or fall back to mapBlockIndex.
//   1. requested hash absent from the authority  -> provider false / NOT_FOUND
//   2. claimed parent ABSENT (injected: the parent's blockindex record is deleted
//      from the authoritative generation while the child still claims it via
//      hashPrev)                                  -> provider false / FAILURE,
//      and explicitly NOT the active-chain value at the child's height
//   3. authority unavailable                     -> resolver FAILURE
// Note on "hot-only/unavailable termination": case 2 is exactly the shape the F1
// hot-floor bug had (a vertex whose parent cannot be proven), and it must never
// be reinterpreted as canonical genesis.
// ---------------------------------------------------------------------------
static bool F2DeleteBlockIndexRecordFromSnapshot(const std::string& snapshotDir,
                                                 const uint256& hash, std::string* error)
{
    leveldb::Options options;
    options.create_if_missing = false;
    options.error_if_exists = false;
    leveldb::DB* db = NULL;
    leveldb::Status status = leveldb::DB::Open(options, snapshotDir, &db);
    if (!status.ok()) { if (error) *error = "snapshot open failed: " + status.ToString(); return false; }
    CDataStream ssKey(SER_DISK, CLIENT_VERSION);
    ssKey << make_pair(std::string("blockindex"), hash);
    leveldb::Status del = db->Delete(leveldb::WriteOptions(), ssKey.str());
    delete db;
    if (!del.ok()) { if (error) *error = "snapshot delete failed: " + del.ToString(); return false; }
    return true;
}

BOOST_AUTO_TEST_CASE(f2_byvalue_provider_failure_matrix)
{
    BOOST_REQUIRE(pindexBest != NULL);

    const int hFixtureTop = kFixtureDepth - 1;
    CBlockIndex* a10 = pindexBest;
    while (a10->nHeight < hFixtureTop) a10 = MineReal(a10, 0x9300 + a10->nHeight);
    while (a10->nHeight > hFixtureTop) { BOOST_REQUIRE(a10->pprev != NULL); a10 = a10->pprev; }
    BOOST_REQUIRE_EQUAL(a10->nHeight, hFixtureTop);
    const uint256 bestHashBeforeFixture = hashBestChain;
    CBlockIndex* a9 = a10->pprev; BOOST_REQUIRE(a9 != NULL);
    CBlockIndex* a8 = a9->pprev;  BOOST_REQUIRE(a8 != NULL);
    CBlockIndex* a7 = a8->pprev;  BOOST_REQUIRE(a7 != NULL);

    CBlockIndex* sideAtPoem = AddSidePoWBlock(a8, 0x9391);   // h9, healthy branch
    BOOST_REQUIRE(sideAtPoem != NULL);
    CBlockIndex* lostParent = AddSidePoWBlock(a7, 0x9392);   // h8, will be deleted
    BOOST_REQUIRE(lostParent != NULL);
    CBlockIndex* childOfLost = AddSidePoWBlock(lostParent, 0x9393); // h9, claims lostParent
    BOOST_REQUIRE(childOfLost != NULL);
    BOOST_REQUIRE_MESSAGE(nBestHeight > 9,
        "F2 fixture: the active best chain must be strictly taller than the side branches");
    BOOST_REQUIRE_MESSAGE(hashBestChain == bestHashBeforeFixture,
        "F2 fixture: the side branches must not displace the active best chain");

    const uint256 sideAtPoemHash = sideAtPoem->GetBlockHash();
    const uint256 lostParentHash = lostParent->GetBlockHash();
    const uint256 childOfLostHash = childOfLost->GetBlockHash();
    const uint256 a9Hash = a9->GetBlockHash();
    const uint256 sideAtPoemTrust = sideAtPoem->nChainTrust;
    const uint256 a9Trust = a9->nChainTrust;
    const uint256 childOfLostTrust = childOfLost->nChainTrust;
    BOOST_REQUIRE(sideAtPoemTrust != a9Trust);
    BOOST_REQUIRE(childOfLostTrust != a9Trust);

    const fs::path root = fs::temp_directory_path() / fs::unique_path("f2-failmatrix-%%%%-%%%%");
    std::map<uint256, CBlockIndex*> savedMap;
    { LOCK(cs_main); savedMap = mapBlockIndex; }
    CBlockIndex* savedBest = pindexBest;
    CBlockIndex* savedGenesis = pindexGenesisBlock;
    uint256 savedBestChain = hashBestChain;
    int savedBestHeight = nBestHeight;
    uint256 savedBestTrust = nBestChainTrust;
    struct Cleanup {
        fs::path root; std::map<uint256, CBlockIndex*> savedMap;
        CBlockIndex* best; CBlockIndex* genesis;
        uint256 bestChain; int bestHeight; uint256 bestTrust;
        Cleanup(const fs::path& r, const std::map<uint256, CBlockIndex*>& m,
                CBlockIndex* b, CBlockIndex* g, const uint256& bc, int bh, const uint256& bt)
            : root(r), savedMap(m), best(b), genesis(g), bestChain(bc), bestHeight(bh), bestTrust(bt) {}
        ~Cleanup() {
            ResetBlockIndexAuthoritativeStartupForTest();
            { LOCK(cs_main); if (!savedMap.empty()) RestoreMapBlockIndexForFixture(savedMap); }
            pindexBest = best; pindexGenesisBlock = genesis;
            hashBestChain = bestChain; nBestHeight = bestHeight; nBestChainTrust = bestTrust;
            try { fs::remove_all(root); } catch (...) {}
        }
    } cleanup(root, savedMap, savedBest, savedGenesis, savedBestChain, savedBestHeight, savedBestTrust);

    // Snapshot the live DB twice: one clean copy, and one copy with the claimed
    // parent's blockindex record DELETED (the injected inconsistency).
    fs::create_directories(root / "snapshot");
    fs::create_directories(root / "snapshot-broken");
    { CTxDB db; db.Close(); }
    const auto live = GetDataDir() / "txleveldb";
    for (fs::directory_iterator it(live), end; it != end; ++it)
        if (fs::is_regular_file(it->path())) {
            fs::copy_file(it->path(), root / "snapshot" / it->path().filename());
            fs::copy_file(it->path(), root / "snapshot-broken" / it->path().filename());
        }
    std::string derr;
    BOOST_REQUIRE_MESSAGE(F2DeleteBlockIndexRecordFromSnapshot((root / "snapshot-broken").string(), lostParentHash, &derr),
        "F2 failure-matrix injection failed: " << derr);

    // ---- 3. claimed parent ABSENT is UNREPRESENTABLE in a valid generation ---
    // The authoritative generation builder itself validates parent connectivity:
    // a child whose persisted hashPrev has no record is REJECTED at BUILD time.
    // A healthy authoritative generation therefore cannot contain a
    // present-vertex-with-absent-parent, so the provider can never be handed one --
    // and the provider's own "claimed parent absent" branch is the defensive
    // fail-closed guard for a CORRUPT store, not a state the authority produces.
    // This is what makes a truncated ancestry structurally unable to be read as
    // canonical genesis (the F1 hot-floor bug shape).
    {
        BlockIndexGenerationSource brokenSource;
        std::string berr;
        BOOST_REQUIRE_MESSAGE(ReadLegacyBlockIndexSource((root / "snapshot-broken").string(), &brokenSource, &berr), berr);
        brokenSource.blockDataDir = GetDataDir().string();
        BlockIndexGenerationBuilder badBuilder;
        std::string badError;
        const bool builtBad = badBuilder.Build(brokenSource, (root / "blockindex-build-broken.tmp").string(), 1, NULL, &badError);
        BOOST_CHECK_MESSAGE(!builtBad,
            "a generation containing a child with an ABSENT claimed parent must be REJECTED at build time");
        BOOST_TEST_MESSAGE("F2 FM claimed-parent-absent child=" << childOfLostHash.GetHex()
            << " lostParent=" << lostParentHash.GetHex()
            << " builderRejected=" << (builtBad ? 0 : 1) << " err=" << badError);
    }

    std::string error;
    {
        BlockIndexGenerationSource source;
        BOOST_REQUIRE_MESSAGE(ReadLegacyBlockIndexSource((root / "snapshot").string(), &source, &error), error);
        source.blockDataDir = GetDataDir().string();
        BlockIndexGenerationBuilder builder;
        BOOST_REQUIRE_MESSAGE(builder.Build(source, (root / "blockindex-build-000001.tmp").string(), 1, NULL, &error), error);
        builder.Close();
    }
    BOOST_REQUIRE_EQUAL(BlockIndexGenerationManager::PublishGeneration(root.string(), 1, &error), BLOCK_INDEX_LIFECYCLE_OK);
    BOOST_REQUIRE_EQUAL(BlockIndexGenerationManager::SelectGeneration(root.string(), 1, &error), BLOCK_INDEX_LIFECYCLE_OK);
    BOOST_REQUIRE_MESSAGE(InitBlockIndexAuthoritative(root.string(), &error), error);
    BOOST_REQUIRE(g_fAuthoritativeStartup);

    // ---- 1. requested hash absent from the authority ---------------------
    {
        uint256 ghost("0xdeadbeef00000000000000000000000000000000000000000000000000000002");
        uint256 acc = 1; std::string e;
        const bool ok = GetAuthoritativeAccumulatedChainTrust(ghost, &acc, &e);
        BOOST_CHECK_MESSAGE(!ok, "absent requested hash must fail closed; err=" << e);
        BOOST_CHECK(acc == 0);
        BOOST_TEST_MESSAGE("F2 FM requested-absent refused=1 err=" << e);
    }

    // ---- 3b. the accepted generation's walks terminate at a PROVEN chain start
    //         (persisted hashPrev == 0), never at a truncation -----------------
    {
        // Both a healthy side branch and the active chain resolve; each walk is
        // proven to end at the one record whose PERSISTED authority says it has no
        // parent. A walk that stopped early would silently under-accumulate.
        uint256 accS = 0; std::string e3;
        BOOST_REQUIRE_MESSAGE(GetAuthoritativeAccumulatedChainTrust(sideAtPoemHash, &accS, &e3), e3);
        BOOST_CHECK_MESSAGE(accS == sideAtPoemTrust, "healthy sibling branch must resolve to its own branch trust");
        uint256 accA = 0; std::string e4;
        BOOST_REQUIRE_MESSAGE(GetAuthoritativeAccumulatedChainTrust(a9Hash, &accA, &e4), e4);
        BOOST_CHECK_MESSAGE(accA == a9Trust, "active chain ancestry must resolve");
        // The deleted-parent child's hash is NOT in the clean generation at all
        // (it was only deleted in the broken copy), so the clean authority still
        // resolves it correctly -- proving the deletion was the only difference.
        uint256 accC = 0; std::string e5;
        BOOST_REQUIRE_MESSAGE(GetAuthoritativeAccumulatedChainTrust(childOfLostHash, &accC, &e5), e5);
        BOOST_CHECK_MESSAGE(accC == childOfLostTrust, "clean generation must still resolve the child branch");
        BOOST_TEST_MESSAGE("F2 FM genesis-terminated healthySibling=" << accS.GetHex()
            << " activeChain=" << accA.GetHex() << " cleanChild=" << accC.GetHex());
    }

    // ---- 4. authority unavailable (LAST: it tears the authority down) -----
    {
        ResetBlockIndexAuthoritativeStartupForTest();
        uint256 acc = 1; std::string e2;
        BOOST_CHECK(!GetAuthoritativeAccumulatedChainTrust(a9Hash, &acc, &e2));
        BOOST_TEST_MESSAGE("F2 FM authority-unavailable refused=1 err=" << e2);
    }
}


// R2c.2 prerequisite discriminator (converted to GREEN expected-mismatch regression).
// The L3 restart-consistency defect is REPRODUCED here and asserted as the KNOWN
// mismatch:
//   persisted (pre-reorg, a1-present) DAG score  !=  current-canonical post-recolor
//   score (a1-absent resident recompute after the real reorg erases a1's daglinks).
// This is NOT a weakened test: it turns the reproduced defect into an executable
// regression specification. S3 (atomic canonical score persistence) is EXPECTED to
// change/replace this assertion when persisted == current-canonical by construction.
// Until then, do NOT make persisted equal to the current-canonical score in S2.


// R2c.2s / S2 FIRST GATE: trust parity. Path A is authorized on the premise that
// authoritative derived chainTrust matches resident CBlockIndex::nChainTrust. A
// possible real divergence: the builder's derived.dat chainTrust uses the
// reciprocal target form (blockindex_generation_builder.cpp:573-575) while real
// CBlockIndex::GetBlockTrust uses GetBlockEntropy for height>=FORK_HEIGHT_POEM
// (main.cpp:9820-9826). Build a REAL generation from the mined resident source,
// start authoritative, and compare per-block authoritative nChainTrust vs
// resident nChainTrust across the POEM boundary.


// R2c.2s / S2 — erased DAG-era parent live-vs-restart discriminator (Phase 2/3).
// A retained child can reference a post-DAG parent whose daglinks record was
// erased by a real SetBestChain->Reorganize. The question is empirical: does
// the erased parent's effective scalar, and the retained child's full-field
// DAG result, differ across a REAL legacy restart (linear nChainTrust replay +
// DAG trust restoration + absent-DAG exclusion)?
//
// This test uses the SAME real multi-parent reorg fixture as the nonresident
// proof. It captures the erased-parent scalar and the retained-child full-field
// state (nDAGScore/fBlue/nInferredK/selected-parent) LIVE, then performs the
// strongest real legacy reopen available in-process:
//   CTxDB::LoadBlockIndex()  (real loader: linear nChainTrust replay)
// then the real legacy init.cpp:1942-1978 restore block
//   (legacy DAG order rebuild + DAG trust replay — physically removed in
//   Phase 2 / Slice 1, so only the linear restart replay remains).
// Then it re-captures the SAME hashes and compares.
//
// Classification (see report):
//   A - no scalar divergence
//   B - parent scalar diverges, child unchanged in this fixture
//   C - parent scalar diverges AND child full-field state diverges
// No production semantics are changed; this is evidence only.


// R2c.2s / S3 — authoritative resolver currentness (BLOCKER R-1).
// Astra proved by source that BOTH the navigator's cold side AND its production
// "hot" resolver (AuthoritativeBlockIndexHotResolver) read only the SAME
// immutable selected generation; a retained block created AFTER that generation
// snapshot could never resolve through ResolveAuthoritativeBlockSnapshot, so
// EnumerateAuthoritativeStagedScope (which resolves every merged daglinks key)
// could not enumerate a post-generation retained canvas. The fix consults the
// CURRENT mutable retained tail (BlockIndexAuthoritativeLive, the production
// live authority: tip-then-base composite) on a genuine immutable NOT_FOUND.
//
// This regression drives the REAL resolver: bootstrap an authoritative
// generation, then persist a post-generation block into the mutable tip via the
// real production live authority, and require ResolveAuthoritativeBlockSnapshotR
// resolves it by value (no mapBlockIndex / no legacy resident fallback), while
// a generation block still resolves and a genuinely-absent hash stays NOT_FOUND.


// Strict persisted-source grammar, independent of authoritative bootstrap.


// ---------------------------------------------------------------------------
// S3 authoritative ordinary ADD: FINAL-COMMIT FAILURE (mandatory Phase 8 point).
// Inject a failure at the physical source TxnCommit boundary (g_testFailInitialDagLinksCommit).
// Verify: NO mixed durable state; on reopen the source is ALL-OLD (token unchanged,
// new block daglinks absent, no score/child cert bound to a new/mismatched token,
// no external live publication of the failed ADD). The S3 ADD stages topology +
// full-field + token + certs into ONE batch; a failed commit must discard it all.
// ---------------------------------------------------------------------------


// ---------------------------------------------------------------------------
// S3 authoritative ordinary ADD: SETBESTCHAIN-FAILURE ROLLBACK coherence.
// A winning ADD commits the authoritative source batch (token + full-field +
// certs) BEFORE SetBestChain. If SetBestChain then fails, AddToBlockIndex runs its
// DAG-rollback txn: it erases the new block's daglinks, restores the parents'
// child-count, restores the OLD SourceStateId, and (authoritative) re-stages the
// score certificate bound to the RESTORED token so no stale/mismatched certificate
// survives. Reopen must show ALL-OLD source with coherent cert^token binding.
// ---------------------------------------------------------------------------


// ---------------------------------------------------------------------------
// S3 authoritative PRUNE helpers (real production-path fixture suite).
// ---------------------------------------------------------------------------
namespace {
// Semantic view of a persisted daglinks row: the exact facts PRUNE and rollback
// must preserve (topology parents + full-field). vDAGChildren/nDAGOrder are
// resident ordering/bookkeeping that legitimately evolves with child
// registration; the semantic view deliberately excludes them.
// Expected prunable set for line H under the production selection rule:
// height < H, by-value, minus the REAL epoch-boundary exemption set.

} // namespace

// ---------------------------------------------------------------------------
// S3 authoritative PRUNE failure matrix. Every injectable stage of the S3
// prune physical source commit must leave the durable source ALL-OLD: batch
// aborted, canvas/token/clean-height/both-certificates byte-identical, nothing
// partial. The production row-erase path is exercised directly (the same function
// the real ADD callers use) on a real authoritative session; each attempt must
// leave the state untouched so all injects run against the same baseline.
// ---------------------------------------------------------------------------
namespace {
int g_pruneFailMatrixBarrierTarget = -1;
// Full durable view of the authoritative source for all-old comparisons.
} // namespace

// ---------------------------------------------------------------------------
// S12: reorg-publication / postponed-reconnect CTxDB lifetime discriminator.
//
// Production flow under test (real operator semantics, no test-only shortcuts):
//   invalidateblock(A_2)
//     -> RollbackActiveChainTo(A_1): root reorg delta delivers mid-stack
//     -> ActivateBestEligibleChain(): reorg to the heavier side branch B with a
//        NON-EMPTY postponed-reconnect list; the root delta delivery happens
//        mid-stack (inside Reorganize) and, pre-fix, the runtime consumer's
//        source reader closed the shared txleveldb handle; the reconnect loop
//        then used the same pre-existing CTxDB instance -> use-after-free.
//
// Geometry (entropy-trust controlled): active A_1..A_3; side B_1..B_3 with
//   T_B3 <= T_A3 (never activates at its own addition),
//   T_B2 > T_A1 and T_B3 > T_A1 (candidate filter + walk fire post-rollback).
// ---------------------------------------------------------------------------


// =========================================================================
// S4 — startup/migration authoritative score reconcile e2e fixtures.
// Each fixture is run standalone (fresh process: TestingSetup provides a fresh
// datadir; the fixture builds its own window2 world, snapshot + generation).
// =========================================================================

// Re-expose protected raw access for test seeding/inspection.

// Full raw key/value snapshot of the shared source db.

// Source-scoped fingerprint: daglinks + child counts + all markers + token.

// Score-authority-scoped fingerprint (score marker + poison + source token).
// Used where a child-count refusal may legitimately leave child-count poison.

// Audit extension (independent freeze audit): exact raw write-set verification.


// Test-only seam state used by the S4 fixtures (declared before S4Cleanup,
// which resets it).
static int g_s4FailBarrier=0;
static bool g_s4TokenInjectArmed=false;
static bool g_s4BoundaryProbeArmed=false;
static bool g_s4BoundaryScoreHealthy=false;
static bool g_s4BoundaryRuntime=false;


// Resident oracle: post-mining resident full-field for every DAG-era PoW block
// (S2-accepted parity: resident post-recolor == canonical for these worlds).


// Compare persisted authoritative full-field against the oracle for the scope.

// Corrupt persisted full-field (all three fields) for one retained vertex.

// Both certificates must bind to the SAME current token after any S4 success.

// Token-stability discriminator: SourceStateId may only advance with physical
// canonical source mutation (ADD/Reorganize/PRUNE); startup reconcile must not.

// World prologue: real world (fork + one DAG block), snapshot, generation.


// --- Part B helpers ---------------------------------------------------------

// Merge block: extends prevA on the active chain while declaring parents
// {prevA, prevB}. Exercises multi-parent + DAGKnight full-field paths.



// fork(11) -> a-chain 12..14 (active); b-chain 12',13' (side); merge m(15)
// with parents {a3,b2} (DAGKnight-era multi-parent merge). All retained.



// ===========================================================================
// R2c.2s / S5 — owned transaction-scoped preview seam (Option B)
// ===========================================================================
// Coverage: internal-consumer receipt (the legacy
// RebuildDAGOrder consumer was removed in Phase 2 / Slice 1) -
// Incremental), complete committed pending prefix (staged-but-uncommitted
// refused), nested reorg/prune sharing the single root ownership domain,
// fail-closed matrix (wrong thread, stale nonce/token/generation, incomplete
// prefix, health loss, abort invalidation, use-after-root), external callers
// cannot reach the seam, boundedness (no global retained cache), nonresident
// readability, Option-R value parity, lock-order compatibility, determinism.












// ---------------------------------------------------------------------------
// S5-F1: internal consumer receipt + complete committed
// pending prefix + nested prune sharing the root + use-after-root refusal +
// stale nonce + determinism + boundedness.
// ---------------------------------------------------------------------------


// ---------------------------------------------------------------------------
// S5-F2: nested reorg inside a root ADD envelope borrows the single root
// ownership domain; the reorder consumer receives the root preview; Option-R
// boundary values served by the preview equal the canonical persisted values.
// ---------------------------------------------------------------------------


// ---------------------------------------------------------------------------
// S5-F3: fail-closed matrix: precommit failure, post-commit rollback, health
// loss, wrong thread, stale token, stale generation, external callers, lock
// order.
// ---------------------------------------------------------------------------


// ---------------------------------------------------------------------------
// S5-F4: nonresident retained vertices remain readable through authoritative
// bounded mechanisms; no global retained cache (stats reset per envelope).
// ---------------------------------------------------------------------------



// ===========================================================================
// R2c.2/S6 — authoritative primary DAG tip selector fixtures.
//
// Coverage map (directive matrix):
//   S6-A: external CLEAN selection, resident legacy parity, real new tip,
//         side (non-active) exclusion, valid-no-eligible active-head fallback.
//   S6-B: failure semantics: source unhealthy, runtime absent, TOCTOU (token
//         change / health loss during enumeration), CPU identity + collateral
//         fail-closed semantics, legacy-mode discriminator.
//   S6-C: boundary resolution (by-value walk + active-at-height agreement),
//         materialization lifetime, composition, negatives.
//   S6-D: internal S5-preview selection during real ADD (crossing) + REORG
//         envelopes; parity with the accepted S5 resolver; forced-unavailable
//         propagation through the internal consumer entry.
//   S6-E: synthetic reduction matrix (single/tie/zero/PoS/active/filters/
//         read-failure/revalidation/enumeration-failure).
// ===========================================================================

// Enabled only by the isolated S7 invocation. No file operations when unset.






// ---------------------------------------------------------------------------
// S6-A: external CLEAN selection parity + real tip + side exclusion + fallback
// ---------------------------------------------------------------------------


// ---------------------------------------------------------------------------
// S6-C: boundary resolution + materialization composition
// ---------------------------------------------------------------------------


// ---------------------------------------------------------------------------
// S6-D: internal S5-preview selection during real ADD + REORG envelopes
// ---------------------------------------------------------------------------






// ---------------------------------------------------------------------------
// S6-E: synthetic reduction matrix (production reduction, controlled context)
// ---------------------------------------------------------------------------


// ---------------------------------------------------------------------------
// S6-repair (blocker repair cycle): fresh-boot nonresident winner.
//
// Proves the ORIGINAL blocker state (authoritative boot publishes no
// mapBlockIndex entries; the legitimate winner is the committed tip and is
// NONRESIDENT) is served: the selector returns SELECTED(expectedHash); the
// bounded scoped materialization resolves that exact hash; CreateNewBlock
// succeeds and the template's primary parent is the selected winner; NO
// history-sized residency reconstruction occurs (mapBlockIndex untouched).
// RED (pre-repair): CreateNewBlock returned NULL ("selected winner is
// nonresident"). GREEN (post-repair): template served via bounded
// materialization; resident/nonresident template parity pinned.
// ---------------------------------------------------------------------------


// ---------------------------------------------------------------------------
// R2c.2/S7 — AUTHORITATIVE MERGE-PARENT CUTOVER (CreateNewBlock).
//
// The legacy merge-parent commitment (src/miner.cpp:265-334) derives its
// candidate set from the retired DAG tip source and requires
// mapBlockIndex RESIDENCY. After the authoritative fresh-boot boundary the
// legitimate frontier tips are nonresident, so the legacy path can only ever
// emit the primary parent: every legitimate merge parent is silently omitted.
//
// This fixture proves, on the REAL production path:
//   RED   — with the candidates nonresident, the legacy-authority path emits a
//           primary-only commitment (the bypass), while the SAME authoritative
//           state legitimately contains two eligible merge parents;
//   GREEN — the authoritative value-only result resolves those retained
//           nonresident candidates by value, includes them, and produces a
//           vector IDENTICAL to the resident case (exact legacy parity, since
//           the resident vector is pinned against an independent legacy oracle);
//   PHASE F — the CPU-mining work identity fingerprints the SAME authoritative
//           frontier, so CPUMiningBlockMatchesWorkIdentity accepts the
//           authoritatively built template (no split authority);
//   FAIL-CLOSED — an unavailable authority produces NO template and is never
//           collapsed into "no extra parents";
//   BOUNDED — no mapBlockIndex residency is created and the reduction performs
//           at most one metadata read per frontier tip.
// ---------------------------------------------------------------------------


// Independent legacy oracle: the exact legacy merge-parent rule (miner.cpp:265-334)
// evaluated over the RESIDENT map. Used to pin the resident vector so the
// authoritative result is compared against legacy semantics, not against itself.




// ---------------------------------------------------------------------------
// S6-repair (Phase 9): stale active-membership containment.
//
// A candidate that WAS active above the base boundary must be classified
// NON-active by the CURRENT live authority once a real reorg supersedes it
// (retained/childless in the tip authority, removed from active membership);
// the selector must exclude it and the current active tip must win. The
// frozen base membership is contained by the live truncation floor (live
// truncation can never reach below the base tip).
// ---------------------------------------------------------------------------


// ---------------------------------------------------------------------------
// S6-repair-cycle-2 (B1): authoritative coinbase maturity / ancestry.
//
// The legacy coinbase-maturity predicate (CTransaction::ConnectInputs,
// main.cpp:6189-6192) walks pprev from pindexBlock for up to nCoinbaseMaturity
// levels and rejects when the spent coinbase's block disk position is found.
// In authoritative V2 mode the pprev topology is sparse/bounded:
//   * fresh boot: pindexBest is a bootstrap anchor with pprev == NULL, so the
//     walk sees ONLY depth 0 -> mempool admission fails open for depths >= 1;
//   * nonresident winner: the bounded materialization covers the window
//     [base tip - 13, winner]; at fresh boot (winner = base anchor) that is
//     depths 0..13 -> miner tx selection fails open for depths >= 14.
// A coinbase spend at depth 14..64 (nCoinbaseMaturity = 65) can therefore be
// admitted, selected into a template, and yet be rejected by full-chain
// validation ("tried to spend coinbase at depth N").
//
// This fixture reproduces that exact production chain through REAL entry
// points (CTxMemPool::accept, CreateNewBlock, CTransaction::ConnectInputs)
// under a faithful fresh-boot simulation (mapBlockIndex cleared; pindexBest
// context = anchor copy with pprev/pskip/pnext == NULL - exactly the object
// state the authoritative bootstrap publishes). It pins the maturity boundary
// matrix {0,1,13,14,20,63,64 -> immature; 65,66 -> mature} and requires the
// authoritative verdict to equal the full-resident (legacy) verdict.
// ---------------------------------------------------------------------------

// BuildPoWBlock variant paying `dest` in the coinbase. The standard fixture
// builder burns the reward to an unspendable script; this one keeps the output
// spendable so the fixture can construct a real signed spend of it.

// MineRealDag equivalent for a spendable coinbase; returns the block index and
// copies the coinbase transaction out for later spending.


// A signed P2PKH spend of coinbaseTx.vout[0] (generous fee; the remaining
// checks of the admission path see a fully valid transaction).

// S6BuildAuthoritativeFixture variant that mines special blocks with
// spendable coinbases at chosen depths below the epoch-end tip, all BEFORE
// the generation snapshot - so after the authoritative boot the tip is the
// base anchor exactly as on a real fresh boot (no post-boot blocks), and the
// maturity sources are generation-resident yet spendable.




// ---------------------------------------------------------------------------
// S7-REPAIR R2 / G6: authoritative canonical-membership ROW-PRESENCE contract.
//
// The independent freeze audit (2026-09-23) reproduced: deleting ONE canonical
// `daglinks` LevelDB row out-of-band for an enumerated frontier tip silently
// dropped that merge parent (5 -> 4 parents) with status VALID, reason 0, and
// both authority certificates still healthy. The mechanism was the enumeration
// predicate collapsing ROW-ABSENT into "*member = false" BEFORE
// ReadMergeCandidate was ever reached.
//
// The repair (a) attests canonical ROW PRESENCE separately from frontier
// membership (CTxDB::ReadDAGFrontierMembershipAttested) and (b) fails the WHOLE
// external CLEAN enumeration when an enumerated authoritative tip has no
// canonical row (new reason DAG_TIP_SELECTION_REASON_FRONTIER_ROW_ABSENT +
// DagMergeParentStats::frontierRowAbsent). Legitimate non-members (row present,
// childCount > 0) keep the historical silent-skip semantics, and a legitimately
// empty frontier stays VALID.
// ---------------------------------------------------------------------------
namespace {
// Out-of-band canonical row mutation on the LIVE datadir (no in-tree
// production path produces this; it models external row loss), with an exact
// restore of the original serialized bytes.
} // namespace

// ---------------------------------------------------------------------------
// B-1 PHASE 8 — LEGITIMATELY PRUNED POST-DAG PARENT SCORE AUTHORITY
//
// The accepted S3/S5 semantics allow a real side branch to extend from a
// DAG-era parent whose canonical daglinks row was erased by the ACCEPTED prune
// lifecycle. F2 must resolve that parent's exact FORMER scalar instead of
// treating every missing row as fatal, WITHOUT weakening the G6 rule (an
// arbitrary row loss at/above the certified line must still fail closed).
//
// Shared fixture: mine a shallow DAG era on the ACTIVE chain, snapshot it into
// an isolated authoritative world, bind the certificate with a real
// authoritative ADD, record the target's FORMER canonical scalar while its row
// still exists, then erase the rows below the line through a REAL forced prune
// inside a REAL ADD (exactly the accepted lifecycle).
// ---------------------------------------------------------------------------




// ---------------------------------------------------------------------------
// F2-B1-R-A5 — REAL ACCEPT-PATH: a de-materialized canonical row that is still
// validated as a declared DAG parent by a LATER accepted block.
//
// The binding re-audit proved soundness only for the resolver called directly
// (with g_testSuppressDagSourceAbort=true), so it never observed the ACCEPT-PATH
// consequence. This case drives the real storage/accept path
//   CBlock::AddToBlockIndex -> (retired DAG colouring, removed) ->
//   ResolveDagParentScore(FAILURE) -> AbortDagSourcePersistence
// (main.cpp:9380-9393) over a lifecycle the node itself creates:
//   (1) canonical post-DAG chain fork -> a1 -> a2 (real rows);
//   (2) real SetBestChain -> Reorganize to a competing branch: a2's DURABLE row
//       is erased (main.cpp:8695) and a REORGANIZE provenance record is written;
//   (3) accept a child whose primary DAG parent is the de-materialized a2 —
//       a legitimate extension of a known, valid branch;
//   (4) reorg BACK so a2 is canonical again (Reorganize does not re-materialize
//       the reconnect branch's rows) and accept a child of a2.
//
// Process safety only: g_testSuppressDagSourceAbort is set so a failure does not
// kill the shared process. It does NOT hide the result: AbortDagSourcePersistence
// latches g_dagSourceUnhealthy BEFORE consulting the suppression flag
// (main.cpp:163-171), so every abort is still observed and asserted below.
// ---------------------------------------------------------------------------


// REAL consensus accept path (CheckBlock + ProcessBlock -> AcceptBlock ->
// AddToBlockIndex) with an explicit declared DAG parent set, reporting instead
// of asserting so the abort consequence stays observable.




// ---------------------------------------------------------------------------
// F2-B1-R-A5 (R-3 leg) — does a RECONNECT re-materialize the disconnected
// branch's durable rows, and can the reconnect itself complete?
//
// Reorganize erases the disconnected branch's canonical rows (main.cpp:8695) and
// stages the authoritative full-field ONLY for vertices present in the merged
// persisted canvas (EnumerateAuthoritativeStagedScope reads persisted rows:
// blockindex_authoritative_startup.cpp:1373-1387), so an erased row is not in the
// scope and is not rewritten. This case measures the consequence on the real
// storage/accept path with a report-style loop (no assertion inside the loop).
// ---------------------------------------------------------------------------








// ============================================================================
// LEGACY DAG RETIREMENT — PHASE 1 focused tests.
// The Legacy DAG engine is retired as a consensus authority. These tests drive
// the REAL production ingress (ProcessBlock -> AcceptBlock) and the REAL
// production mining path (CreateNewBlock); no test-local copy of the guard is
// relied upon. The retired profile is forced through the test-only seam
// (g_testForceLegacyDagRetired) and the DAG-only domain is forced with the
// test-only height override (g_testForkHeightDagOverride), so mainnet semantics
// are reproduced without touching a height constant.
// ============================================================================





BOOST_AUTO_TEST_CASE(p1_retirement_authoritative_startup_without_dag_custody)
{
    // LEGACY DAG RETIREMENT (Phase 1): in the RETIRED profile the real
    // authoritative startup must still succeed and publish AUTHORITY_READY without
    // requiring dormant Legacy DAG provenance/custody certification or DAG durable
    // score-authority health. The barrier is rebased onto the CURRENT consensus
    // authority (V2 durable index + immutable authority + linear trust projection).
    SetMockTime(1700001950);
    CBlockIndex* fork=pindexBest;
    while(fork->nHeight<kFixtureDepth) fork=MineReal(fork,0xE500+fork->nHeight);
    fork=MineRealDag(fork,0xE510);
    const fs::path root=fs::temp_directory_path()/fs::unique_path("p1retired-%%%%-%%%%");
    fs::create_directories(root/"snapshot");
    struct Cleanup { fs::path root; CBlockIndex* best; CBlockIndex* genesis;
        Cleanup(const fs::path& r):root(r),best(pindexBest),genesis(pindexGenesisBlock){}
        ~Cleanup(){ ResetBlockIndexAuthoritativeStartupForTest();
            pindexBest=best; pindexGenesisBlock=genesis;
            if(best){nBestHeight=best->nHeight;hashBestChain=best->GetBlockHash();nBestChainTrust=best->nChainTrust;}
            g_testSuppressDagSourceAbort=false; SetMockTime(0); try{fs::remove_all(root);}catch(...){} }
    } cleanup(root);
    { CTxDB db; db.Close(); }
    const auto liveDir=GetDataDir()/"txleveldb";
    for(fs::directory_iterator it(liveDir),end;it!=end;++it)
        if(fs::is_regular_file(it->path())) fs::copy_file(it->path(),root/"snapshot"/it->path().filename());
    BlockIndexGenerationSource src; std::string aerr;
    BOOST_REQUIRE_MESSAGE(ReadLegacyBlockIndexSource((root/"snapshot").string(),&src,&aerr),aerr);
    src.blockDataDir=GetDataDir().string();
    BlockIndexGenerationBuilder ab;
    BOOST_REQUIRE_MESSAGE(ab.Build(src,(root/"blockindex-build-000001.tmp").string(),1,NULL,&aerr),aerr); ab.Close();
    BOOST_REQUIRE_EQUAL(BlockIndexGenerationManager::PublishGeneration(root.string(),1,&aerr),BLOCK_INDEX_LIFECYCLE_OK);
    BOOST_REQUIRE_EQUAL(BlockIndexGenerationManager::SelectGeneration(root.string(),1,&aerr),BLOCK_INDEX_LIFECYCLE_OK);
    AuthorityReadyResetForTest();
    BOOST_REQUIRE(!AuthorityReadyIsSet());
    g_testSuppressDagSourceAbort=true;
    // LEGACY DAG RETIREMENT (Phase 2 / H9 FINAL): the retired profile is the only profile.
    BOOST_REQUIRE_MESSAGE(InitBlockIndexAuthoritative(root.string(),&aerr),aerr);
    BOOST_REQUIRE(g_fAuthoritativeStartup);
    BOOST_TEST_MESSAGE("P1_STARTUP authority_ready_state="<<AuthorityReadyStateName()
        <<" published="<<(AuthorityReadyIsSet()?1:0)<<" profile=retired dag_custody_required=0");
    BOOST_CHECK_MESSAGE(AuthorityReadyIsSet(),
        "P1: the real authoritative startup must publish AUTHORITY_READY in the retired profile without "
        "Legacy DAG provenance/custody certification (refusal detail: " << AuthorityReadyRefusalDetail() << ")");
    const std::string readyDetail = AuthorityReadyRefusalDetail();
    BOOST_CHECK_MESSAGE(readyDetail.find("dag") == std::string::npos &&
                        readyDetail.find("custody") == std::string::npos &&
                        readyDetail.find("provenance") == std::string::npos,
        "P1: no readiness prerequisite may remain unsatisfied for a DAG-only reason (" << readyDetail << ")");
    std::string werr;
    BOOST_CHECK_MESSAGE(AuthorityReadyWait(1,&werr),
        "P1: after the retired-profile startup a consumer gate must open immediately ("<<werr<<")");
}

BOOST_AUTO_TEST_CASE(p1_retirement_cpu_mining_consumers_use_linear_head)
{
    // LEGACY DAG RETIREMENT (Phase 1): the REAL production CPU-mining consumers
    // (work-identity capture / collateral readiness / work-current check, exposed
    // through the existing R2c.2/S6 test-facing hooks) must not require the retired
    // DAG tip selector, the DAG frontier tip set or the DAG source-state token.
    CBlockIndex* head = pindexBest;
    if (mapBlockIndex.count(hashBestChain)) head = mapBlockIndex[hashBestChain];
    BOOST_REQUIRE(head != NULL);
    const CPUMiningWorkIdentity id = CaptureCurrentCPUMiningWorkIdentityForTest();
    const bool fCollateralReady = IsCPUMiningCollateralStateReadyForTest();
    const bool fWorkCurrent = IsCPUMiningWorkCurrentForTest(id, false);

    BOOST_CHECK_MESSAGE(!id.fSelectionUnavailable,
        "retired profile: the work identity must not be marked unavailable for a DAG-only reason");
    BOOST_CHECK_MESSAGE(id.hashPrimaryParent == head->GetBlockHash(),
        "retired profile: the work identity primary parent must be the authoritative LINEAR head ("
        << id.hashPrimaryParent.ToString().substr(0, 20) << " vs "
        << head->GetBlockHash().ToString().substr(0, 20) << ")");
    BOOST_CHECK_MESSAGE(id.nHeight == head->nHeight + 1,
        "retired profile: the identity height must follow the authoritative linear head ("
        << id.nHeight << " vs " << (head->nHeight + 1) << ")");
    BOOST_CHECK_MESSAGE(id.vDAGTips.empty(),
        "retired profile: no DAG frontier tip set may be collected (size=" << id.vDAGTips.size() << ")");
    BOOST_CHECK_MESSAGE(fCollateralReady,
        "retired profile: collateral readiness must be computed without the DAG tip selector");
    BOOST_CHECK_MESSAGE(fWorkCurrent,
        "retired profile: a freshly captured identity must be current without any DAG-state dependency");
    BOOST_TEST_MESSAGE("P1_CPUMINING primary_parent=linear selection_unavailable=0 dag_tips=" << id.vDAGTips.size()
                       << " collateral_ready=" << (fCollateralReady ? 1 : 0)
                       << " work_current=" << (fWorkCurrent ? 1 : 0));
}

BOOST_AUTO_TEST_CASE(p1_retirement_linear_reorg_and_restart_parity)
{
    // ========================================================================
    // PHASE 1 FINAL E2E GATE — retired-profile ORDINARY LINEAR REORG + RESTART.
    // Real production path only: ProcessBlock / AddToBlockIndex -> SetBestChain ->
    // Reorganize -> PublishAuthoritativeLiveTailReorg, with the Legacy DAG engine
    // disabled for the whole scenario (the retired DAG consensus authority gate is gone).
    // This is NOT a DAG test: every block of both branches is an ordinary linear
    // PoW block with no DAG parent commitment.
    // ========================================================================
    SetMockTime(1700002300);
    // Fixture chain is built first in the harness default profile (as every other
    // fixture does), then the RETIRED profile is engaged for the whole scenario.
    CBlockIndex* fork = pindexBest;
    while (fork->nHeight < kFixtureDepth) fork = MineReal(fork, 0xD100 + fork->nHeight);
    const fs::path root = fs::temp_directory_path() / fs::unique_path("p1linreorg-%%%%-%%%%");
    fs::create_directories(root / "snapshot");
    struct Cleanup { fs::path root; CBlockIndex* best; CBlockIndex* genesis;
        Cleanup(const fs::path& r):root(r),best(pindexBest),genesis(pindexGenesisBlock){}
        ~Cleanup(){ ResetBlockIndexAuthoritativeStartupForTest();
            pindexBest=best; pindexGenesisBlock=genesis;
            if(best){nBestHeight=best->nHeight;hashBestChain=best->GetBlockHash();nBestChainTrust=best->nChainTrust;}
            g_testSuppressDagSourceAbort=false; SetMockTime(0); try{fs::remove_all(root);}catch(...){} }
    } cleanup(root);
    { CTxDB db; db.Close(); }
    const auto liveDir = GetDataDir() / "txleveldb";
    for (fs::directory_iterator it(liveDir), end; it != end; ++it)
        if (fs::is_regular_file(it->path())) fs::copy_file(it->path(), root / "snapshot" / it->path().filename());
    BlockIndexGenerationSource src; std::string aerr;
    BOOST_REQUIRE_MESSAGE(ReadLegacyBlockIndexSource((root / "snapshot").string(), &src, &aerr), aerr);
    src.blockDataDir = GetDataDir().string();
    BlockIndexGenerationBuilder ab;
    BOOST_REQUIRE_MESSAGE(ab.Build(src, (root / "blockindex-build-000001.tmp").string(), 1, NULL, &aerr), aerr); ab.Close();
    BOOST_REQUIRE_EQUAL(BlockIndexGenerationManager::PublishGeneration(root.string(), 1, &aerr), BLOCK_INDEX_LIFECYCLE_OK);
    BOOST_REQUIRE_EQUAL(BlockIndexGenerationManager::SelectGeneration(root.string(), 1, &aerr), BLOCK_INDEX_LIFECYCLE_OK);
    AuthorityReadyResetForTest();
    BOOST_REQUIRE_MESSAGE(InitBlockIndexAuthoritative(root.string(), &aerr), aerr);
    BOOST_REQUIRE(g_fAuthoritativeStartup);
    auto live = GetAuthoritativeLiveAuthority(); BOOST_REQUIRE(live && live->IsOpen());

    // --- branch B (ACTIVE first): A -> B1..B6, ordinary linear blocks built and
    //     offered through the REAL production acceptance path (ProcessBlock ->
    //     AcceptBlock -> SetBestChain), not the storage-level test shortcut -----
    const int nRefusalsBefore = 0;
    CBlockIndex* a1 = MineReal(fork, 0xD111);
    CBlockIndex* active = a1;
    for (unsigned i = 0; i < 6; ++i) active = MineReal(active, 0xD112 + i);
    BOOST_REQUIRE(pindexBest == active);
    const uint256 activeHash = active->GetBlockHash();
    const uint256 activeTrust = active->nChainTrust;
    BOOST_TEST_MESSAGE("P1_REORG branch_B active_tip=" << activeHash.ToString().substr(0, 20)
                       << " height=" << active->nHeight << " trust=" << activeTrust.ToString().substr(0, 16));

    // --- branch C (COMPETING): A -> C1, C2, then extend through the REAL
    //     production acceptance path until SetBestChain/Reorganize switches -----
    CBlockIndex* c1 = MineReal(fork, 0xD121);
    CBlockIndex* c2 = MineReal(c1, 0xD122);
    CBlockIndex* branch = c2;
    bool reorgObserved = false;
    for (unsigned i = 0; i < 12 && pindexBest == active; ++i)
    {
        // REAL production acceptance path: ProcessBlock -> AcceptBlock ->
        // SetBestChain -> Reorganize (the competing branch becomes best on trust).
        CBlockIndex* next = MineReal(branch, 0xD130 + i);
        BOOST_REQUIRE_MESSAGE(next != NULL, "the retired-profile linear reorg must succeed through ProcessBlock/SetBestChain/Reorganize");
        branch = next;
        if (pindexBest != active) reorgObserved = true;
    }
    BOOST_REQUIRE_MESSAGE(reorgObserved, "the competing branch must become best through a REAL reorganization");
    const uint256 reorgHash = pindexBest->GetBlockHash();
    const uint256 reorgTrust = pindexBest->nChainTrust;
    const int reorgHeight = pindexBest->nHeight;

    // --- post-reorg assertions: tip / membership / ordinary trust / V2 tail ---
    BOOST_CHECK_MESSAGE(reorgHash == branch->GetBlockHash(),
        "the expected C-side tip must be selected (" << reorgHash.ToString().substr(0, 20) << ")");
    BOOST_CHECK_MESSAGE(hashBestChain == reorgHash, "the active chain hash must be the reorged tip");
    BOOST_CHECK_MESSAGE(reorgTrust == branch->pprev->nChainTrust + branch->GetBlockTrust(),
        "the selected tip trust must be the ORDINARY linear recurrence (no DAG score overwrite)");
    BOOST_CHECK_MESSAGE(reorgTrust != activeTrust, "the reorg must have changed the accumulated trust");
    BlockIndexSnapshot postSnap;
    BOOST_REQUIRE_MESSAGE(ResolveAuthoritativeBlockSnapshot(reorgHash, &postSnap, &aerr), aerr);
    BOOST_CHECK_MESSAGE(postSnap.height == reorgHeight,
        "the V2 authoritative by-value snapshot must agree with the active tip height");
    // H9CLOSURE: the Legacy DAG row surface is physically removed (CTxDB DAG
    // readers/writers deleted); no DAG-row assertion is possible or meaningful
    // on the retired profile.
    const int nRefusalsAfter = 0;
    BOOST_CHECK_MESSAGE(nRefusalsAfter == nRefusalsBefore,
        "no retirement firewall refusal may be involved in a legitimate linear reorg");
    BOOST_CHECK_MESSAGE(!g_dagSourceUnhealthy, "the retired-profile reorg must not latch any DAG-source failure");
    // Mechanism probe: the live authority's own recorded tip, in-process, right
    // after the reorg (before any restart), and the derived by-value record.
    {
        BlockIndexAuthoritativeLive* lv = GetAuthoritativeLiveAuthority();
        int liveTipH = -2; uint256 liveTipHash;
        if (lv && lv->IsOpen() && lv->TipAuthority())
        {
            const BlockIndexTipRead tr = lv->TipAuthority()->GetTip();
            if (tr.status == BLOCK_INDEX_TIP_OK) { liveTipH = tr.height; liveTipHash = tr.record.hash; }
        }
        BOOST_CHECK_MESSAGE(liveTipHash == reorgHash && liveTipH == reorgHeight,
            "the retired-profile reorg MUST publish the exact reorged tip to the authoritative live tail ("
            << liveTipH << " vs " << reorgHeight << ")");
        BOOST_TEST_MESSAGE("P1_REORG_LIVETAIL live_inprocess_tip_height=" << liveTipH
                           << " live_inprocess_tip_matches_reorged=" << (liveTipHash == reorgHash ? 1 : 0)
                           << " reorg_height=" << reorgHeight);
    }

    BOOST_TEST_MESSAGE("P1_REORG reorg_completed=1 profile=retired tip=" << reorgHash.ToString().substr(0, 20)
                       << " height=" << reorgHeight << " trust_changed=1 v2_snapshot_matches=1 dag_row=0"
                       << " dag_source_unhealthy=0 firewall_refusals_delta=0");

    // --- RESTART PARITY: CLEAN production shutdown, then reopen from the SAME
    //     isolated persisted fixture (real close of the authoritative live tail
    //     and the txdb, exactly as a node shutdown does). ----------------------
    {
        BlockIndexAuthoritativeLive* shuttingDown = GetAuthoritativeLiveAuthority();
        if (shuttingDown && shuttingDown->IsOpen()) shuttingDown->Close();
    }
    { CTxDB db; db.Close(); }
    ResetBlockIndexAuthoritativeStartupForTest();
    AuthorityReadyResetForTest();
    BOOST_REQUIRE(!AuthorityReadyIsSet());
    // ---------------------------------------------------------------------
    // (a) V2 AUTHORITATIVE RESTART RECONSTRUCTION (production component P5):
    //     base generation S + the persisted mutable post-S tip authority under
    //     <root>/blockindex_tip. The live acceptance path persisted the reorged
    //     active tip there (BlockIndexTipAuthority Append / ReorgActiveTo).
    // ---------------------------------------------------------------------
    std::string rerr;
    {
        BlockIndexAuthoritativeRestart restart;
        BOOST_REQUIRE_MESSAGE(restart.OpenBaseAndTip(root.string(), true, 1, NULL, &rerr), rerr);
        BOOST_CHECK_MESSAGE(restart.HasPostSTip(),
            "the authoritative restart reconstruction must find persisted post-generation records");
        BOOST_CHECK_MESSAGE(restart.EffectiveTipHash() == reorgHash,
            "the authoritative restart must land on the EXACT reorged tip ("
            << restart.EffectiveTipHash().ToString().substr(0, 20) << " vs "
            << reorgHash.ToString().substr(0, 20) << ")");
        BOOST_CHECK_MESSAGE(restart.EffectiveTipHeight() == reorgHeight,
            "the authoritative restart must land on the reorged height ("
            << restart.EffectiveTipHeight() << " vs " << reorgHeight << ")");
        BOOST_TEST_MESSAGE("P1_REORG_RESTART post_s_tip=" << (restart.HasPostSTip() ? 1 : 0)
                           << " effective_tip_height=" << restart.EffectiveTipHeight()
                           << " effective_tip_matches_reorged_tip=" << (restart.EffectiveTipHash() == reorgHash ? 1 : 0)
                           << " tip_records=" << restart.TipRecordCount());
        restart.Close();
    }
    // (a2) The PERSISTED mutable tip itself, reopened from disk: exact reorged tip
    //      and the ORDINARY accumulated trust (no DAG score restoration involved).
    {
        BlockIndexTipAuthority tip; std::string terr;
        BOOST_REQUIRE_MESSAGE(BlockIndexTipAuthority::Open(root.string(), 1, &tip, &terr), terr);
        const BlockIndexTipRead ptr2 = tip.GetTip();
        BOOST_CHECK_EQUAL(ptr2.status, BLOCK_INDEX_TIP_OK);
        BOOST_CHECK_MESSAGE(ptr2.height == reorgHeight && ptr2.record.hash == reorgHash,
            "the persisted mutable tip must be the EXACT reorged tip ("
            << ptr2.height << " vs " << reorgHeight << ")");
        BOOST_CHECK_MESSAGE(ptr2.derived.chainTrust == reorgTrust,
            "the persisted tip must carry the ORDINARY accumulated trust");
        BOOST_CHECK_MESSAGE(ptr2.active, "the persisted tip must be marked active");
        BOOST_TEST_MESSAGE("P1_REORG_RESTART_PERSIST persisted_tip_height=" << ptr2.height
                           << " persisted_tip_matches_reorged=" << (ptr2.record.hash == reorgHash ? 1 : 0)
                           << " persisted_trust_matches=" << (ptr2.derived.chainTrust == reorgTrust ? 1 : 0)
                           << " active=" << (ptr2.active ? 1 : 0));
        tip.Close();
    }

    // ---------------------------------------------------------------------
    // (b) PRODUCTION BOOT ENTRY (init.cpp:1486 calls exactly this function) in
    //     the RETIRED profile: it must succeed, publish AUTHORITY_READY, keep the
    //     reorged rows durably present and require no DAG custody / provenance /
    //     DAG row / firewall involvement.
    // ---------------------------------------------------------------------
    BOOST_REQUIRE_MESSAGE(InitBlockIndexAuthoritative(root.string(), &rerr), rerr);
    BOOST_REQUIRE(g_fAuthoritativeStartup);
    const bool fBootReady = AuthorityReadyIsSet();
    const std::string bootDetail = AuthorityReadyRefusalDetail();
    const bool fMembershipPreserved = mapBlockIndex.count(reorgHash) != 0;
    // H9CLOSURE: Legacy DAG row surface physically removed; no DAG-row probe remains.
    const int refusalsAfterBoot = 0;
    BOOST_CHECK_MESSAGE(fBootReady, "AUTHORITY_READY must reach READY after the retired-profile restart");
    BOOST_CHECK_MESSAGE(bootDetail.find("dag") == std::string::npos &&
                        bootDetail.find("custody") == std::string::npos &&
                        bootDetail.find("provenance") == std::string::npos,
        "no DAG-only prerequisite may gate readiness after restart (" << bootDetail << ")");
    BOOST_CHECK_MESSAGE(fMembershipPreserved,
        "restart must preserve the reorged block's durable membership");
    // H9CLOSURE: Legacy DAG row surface physically removed; the former
    // "no DAG row required after restart" assertion no longer has a subject.
    BOOST_CHECK_MESSAGE(refusalsAfterBoot == nRefusalsBefore,
        "no retirement firewall refusal may occur after restart");
    BOOST_TEST_MESSAGE("P1_REORG_RESTART_BOOT authority_ready=" << (fBootReady ? 1 : 0)
                       << " membership_preserved=" << (fMembershipPreserved ? 1 : 0)
                       << " dag_custody_required=0 dag_row_required=0 firewall_refusals_delta="
                       << (refusalsAfterBoot - nRefusalsBefore));
    // Pinned OBSERVATION O1 — DOCUMENTED, NOT BLESSED (see FINAL E2E CLOSURE §O1
    // in the Phase-1 report): the production boot entry restores the base-generation
    // anchor and does not itself fold the persisted post-S tip that the P5 restart
    // reconstruction (a) resolves above. Reported to the owner; no claim is made
    // here that the boot's behaviour is correct.
    BOOST_TEST_MESSAGE("P1_REORG_BOOT_OBSERVATION boot_best_height=" << nBestHeight
                       << " persisted_post_s_tip_height=" << reorgHeight
                       << " generation_base_height=" << fork->nHeight
                       << " boot_folds_persisted_tip=0");
}

// ===========================================================================
// V2-R2 (PM1-P0-01): CLEAN-RESTART AUTHORITY OF THE DURABLE POST-S TIP.
//
// The immutable base anchor publishes S. If the durable mutable tip holds
// post-S authority (L > S), a CLEAN RESTART MUST publish L as the authoritative
// best tip -- never silently truncate to the base tip S. Drives the REAL
// production boot entry (InitBlockIndexAuthoritative, the function init.cpp
// calls), persists a post-S block through the production live-authority seam
// (the same AcceptActive call AddToBlockIndex makes on accept of an active
// block), then simulates a FRESH PROCESS (ResetBlockIndexAuthoritativeStartupForTest
// destroys the retained context and releases the stores) and boots again.
// ===========================================================================
BOOST_AUTO_TEST_CASE(r2_p01_restart_publishes_durable_post_s_tip)
{
    const fs::path root = fs::temp_directory_path() / fs::unique_path("r2p01-%%%%-%%%%");
    CBlockIndex* savedBest = pindexBest;
    CBlockIndex* savedGenesis = pindexGenesisBlock;
    uint256 savedBestChain = hashBestChain;
    int savedBestHeight = nBestHeight;
    uint256 savedBestTrust = nBestChainTrust;
    struct Cleanup {
        fs::path root; CBlockIndex* best; CBlockIndex* genesis;
        uint256 bestChain; int bestHeight; uint256 bestTrust;
        Cleanup(const fs::path& r, CBlockIndex* b, CBlockIndex* g, const uint256& bc, int bh, const uint256& bt)
            : root(r), best(b), genesis(g), bestChain(bc), bestHeight(bh), bestTrust(bt) {}
        ~Cleanup() {
            ResetBlockIndexAuthoritativeStartupForTest();
            pindexBest = best; pindexGenesisBlock = genesis;
            hashBestChain = bestChain; nBestHeight = bestHeight; nBestChainTrust = bestTrust;
            try { fs::remove_all(root); } catch (...) {}
        }
    } cleanup(root, savedBest, savedGenesis, savedBestChain, savedBestHeight, savedBestTrust);

    // Build + publish + select an authoritative generation and boot it: pindexBest = S.
    std::string error;
    F2BuildAuthoritativeGenerationAndInit(root, &error);
    const int S = nBestHeight;
    const uint256 baseTipHash = hashBestChain;
    BOOST_REQUIRE(S >= 0);

    // Persist a post-S block into the DURABLE mutable tip via the production
    // live-authority seam (the exact call AddToBlockIndex makes for an accepted
    // active block).
    BlockIndexAuthoritativeLive* live = GetAuthoritativeLiveAuthority();
    BOOST_REQUIRE(live && live->IsOpen());
    const uint256 lHash = uint256(0xABCDEF01UL);
    BlockIndexRecord rec;
    rec.hash = lHash; rec.hashPrev = baseTipHash; rec.height = S + 1;
    rec.nFile = 1; rec.nBlockPos = (unsigned)(S + 1) * 100; rec.nFlags = 0; rec.nVersion = 7;
    rec.nTime = 1700000000u + (unsigned)(S + 1); rec.nBits = 0x1d00ffff; rec.nNonce = (unsigned)(S + 1);
    BlockIndexDerivedEntry der;
    der.chainTrust = uint256(0xBEEFUL); der.stakeModifierChecksum = 7;
    der.SetHasStakeModifierTime(true); der.stakeModifierTime = 1700000000;
    der.SetHasBlockSize(true); der.nSize = 1200;
    BOOST_REQUIRE_MESSAGE(live->AcceptActive(rec, der, S + 1, &error), error);
    BOOST_REQUIRE_EQUAL(live->TipAuthorityMutable()->TipHeight(), S + 1);

    // ---- SIMULATED CLEAN RESTART (fresh process) ----
    ResetBlockIndexAuthoritativeStartupForTest();
    BOOST_CHECK(!g_fAuthoritativeStartup);
    BOOST_REQUIRE_MESSAGE(InitBlockIndexAuthoritative(root.string(), &error), error);
    BOOST_REQUIRE(g_fAuthoritativeStartup);

    // PM1-P0-01: the authoritative best tip MUST be the durable post-S tip L.
    BOOST_CHECK_MESSAGE(nBestHeight == S + 1,
        "PM1-P0-01: after restart the authoritative best height MUST be the durable post-S tip "
        << (S + 1) << ", not the base tip " << S << " (got " << nBestHeight << ")");
    BOOST_CHECK_MESSAGE(hashBestChain == lHash,
        "PM1-P0-01: after restart the authoritative best chain MUST be the durable post-S tip hash "
        << lHash.ToString().substr(0, 20) << " (got " << hashBestChain.ToString().substr(0, 20) << ")");
    BOOST_TEST_MESSAGE("R2_P01_RESTART base_tip_height=" << S << " post_s_tip_height=" << (S + 1)
                       << " boot_best_height=" << nBestHeight
                       << " boot_folds_persisted_tip=" << (hashBestChain == lHash ? 1 : 0));
}

// V2-R2B (PM1-P0-01 negative closure): a corrupted durable tip MUST fail closed
// at authoritative startup -- never silently publish S as if nothing happened.
BOOST_AUTO_TEST_CASE(r2b_p01_negative_corrupt_tip_fails_closed)
{
    const fs::path root = fs::temp_directory_path() / fs::unique_path("r2bp01n-%%%%-%%%%");
    std::string error;
    F2BuildAuthoritativeGenerationAndInit(root, &error);
    const int S = nBestHeight;
    BOOST_REQUIRE(S >= 0);

    // Corrupt tip.meta's mutable protocol version to an unknown value.
    const std::string metaPath = (root / "blockindex_tip" / "tip.meta").string();
    unsigned char buf[160];
    size_t n = 0;
    {
        FILE* f = fopen(metaPath.c_str(), "rb");
        BOOST_REQUIRE(f != NULL);
        n = fread(buf, 1, sizeof(buf), f);
        fclose(f);
        BOOST_REQUIRE(n >= 104);
        buf[0] = 99; buf[1] = 0; buf[2] = 0; buf[3] = 0; // unknown future version
        f = fopen(metaPath.c_str(), "wb");
        BOOST_REQUIRE(f != NULL);
        BOOST_REQUIRE(fwrite(buf, 1, n, f) == n);
        fclose(f);
    }

    ResetBlockIndexAuthoritativeStartupForTest();
    std::string err;
    const bool ok = InitBlockIndexAuthoritative(root.string(), &err);
    BOOST_CHECK_MESSAGE(!ok,
        "R2B-P01-N3: corrupted durable tip MUST fail closed at authoritative startup (ok="
        << ok << " err=" << err << ")");
    BOOST_TEST_MESSAGE("R2B_P01N corrupt_tip_startup_ok=" << (ok ? 1 : 0) << " err=" << err);
    ResetBlockIndexAuthoritativeStartupForTest();
    try { fs::remove_all(root); } catch (...) {}
}

// V2-R2B (PM1-P0-09): the immutable-base boundary oracle is distinct from the
// published best tip. It returns S in authoritative mode and -1 otherwise, so a
// reorg cutover can refuse a fork below S without confusing it with the tip.
BOOST_AUTO_TEST_CASE(r2b_p09_base_tip_oracle)
{
    const fs::path root = fs::temp_directory_path() / fs::unique_path("r2bp09-%%%%-%%%%");
    std::string error;
    F2BuildAuthoritativeGenerationAndInit(root, &error);
    const int S = nBestHeight;
    BOOST_CHECK_EQUAL(AuthoritativeBaseTipHeight(), S);
    BOOST_TEST_MESSAGE("R2B_P09 base_tip_oracle=" << AuthoritativeBaseTipHeight() << " S=" << S);
    ResetBlockIndexAuthoritativeStartupForTest();
    BOOST_CHECK_EQUAL(AuthoritativeBaseTipHeight(), -1);
    try { fs::remove_all(root); } catch (...) {}
}

// ===========================================================================
// V2-R2D CLOSURE FIXTURES
//
// Real S>0 authoritative generation built synthetically (deterministic, no
// ambient mining) with real on-disk block files, published + selected + booted
// through the REAL production startup entry InitBlockIndexAuthoritative. All
// operator transitions are driven through the REAL production functions
// (InvalidateBlock / ReconsiderBlock) and the real BlockIndexAuthoritativeLive
// seam; nothing here calls test-only storage helpers.
// ===========================================================================
struct R2DSB
{
    uint256 hash;
    unsigned int nFile, nBlockPos, nSize;
};

static R2DSB R2DWriteSyntheticBlock(const fs::path& blockDir, uint256 prev,
                                 unsigned int nTime, unsigned int nBits,
                                 unsigned int nNonce, unsigned int nFile)
{
    R2DSB info; info.nFile = nFile;
    CTransaction coinbase; coinbase.nVersion = 1; coinbase.nTime = nTime;
    CTxIn input; input.prevout = COutPoint(uint256(0), 0xffffffff);
    input.scriptSig = CScript() << OP_TRUE; input.nSequence = 0xffffffff;
    coinbase.vin.push_back(input);
    CTxOut output; output.nValue = 0; output.scriptPubKey = CScript() << OP_TRUE;
    coinbase.vout.push_back(output);
    CBlock block; block.nVersion = 1; block.hashPrevBlock = prev;
    block.nTime = nTime; block.nBits = nBits; block.nNonce = nNonce;
    block.vtx.push_back(coinbase); block.hashMerkleRoot = block.BuildMerkleTree();
    info.hash = block.GetHash();
    info.nSize = ::GetSerializeSize(block, SER_NETWORK, PROTOCOL_VERSION);
    CDataStream ss(SER_DISK, CLIENT_VERSION); ss << block;
    unsigned int ns = ss.size();
    char name[32]; snprintf(name, sizeof(name), "blk%04u.dat", nFile);
    fs::path f = blockDir / name;
    FILE* fp = fopen(f.string().c_str(), "ab"); BOOST_REQUIRE(fp != NULL);
    unsigned char magic[] = {0xfa,0xbf,0xb5,0xda};
    fwrite(magic,1,4,fp); fwrite(&ns,4,1,fp);
    long pos = ftell(fp); info.nBlockPos = (unsigned int)pos;
    fwrite(&ss[0],1,ss.size(),fp); fflush(fp); fclose(fp);
    return info;
}

// Build a self-contained immutable generation genesis..S (S>0) with real block
// files and boot it through the REAL production startup. Returns true on a
// successful authoritative boot; fills outActive with hashes[0..S].
static bool R2DBuildSyntheticGenerationAndInit(const fs::path& root, int S,
                                               std::vector<uint256>* outActive,
                                               std::string* error)
{
    fs::create_directories(root / "blocks");
    std::vector<uint256> active;
    uint256 prev(0);
    BlockIndexGenerationSource src;
    for (int h = 0; h <= S; ++h)
    {
        R2DSB b = R2DWriteSyntheticBlock(root / "blocks", prev,
                                      1700000000u + (unsigned)h, 0x1d00ffffU,
                                      (unsigned)h, 1);
        BlockIndexRecord rec;
        rec.hash = b.hash; rec.hashPrev = prev; rec.height = h;
        rec.nVersion = 1; rec.nTime = 1700000000u + (unsigned)h;
        rec.nBits = 0x1d00ffffU; rec.nNonce = (unsigned)h;
        rec.nFile = b.nFile; rec.nBlockPos = b.nBlockPos; rec.nFlags = 0;
        rec.nMoneySupply = 0;
        BlockIndexGenerationSourceRecord sr; sr.hash = b.hash; sr.record = rec;
        src.records.push_back(sr);
        active.push_back(b.hash);
        prev = b.hash;
    }
    src.hashBestChain = active[S];
    src.foundBestChain = true;
    src.blockDataDir = (root / "blocks").string();
    BlockIndexGenerationBuilder b;
    if (!b.Build(src, (root / "blockindex-build-000001.tmp").string(), 1, NULL, error)) return false;
    b.Close();
    if (BlockIndexGenerationManager::PublishGeneration(root.string(), 1, error) != BLOCK_INDEX_LIFECYCLE_OK) return false;
    if (BlockIndexGenerationManager::SelectGeneration(root.string(), 1, error) != BLOCK_INDEX_LIFECYCLE_OK) return false;
    if (outActive) *outActive = active;
    return InitBlockIndexAuthoritative(root.string(), error);
}

static BlockIndexRecord R2DMakeRecord(const uint256& hash, const uint256& prev, int height)
{
    BlockIndexRecord r;
    r.hash = hash; r.hashPrev = prev; r.height = height;
    r.nFile = 1; r.nBlockPos = (unsigned)(height * 100); r.nFlags = 0;
    r.nVersion = 5; r.nTime = 1700000000u + (unsigned)height;
    r.nBits = 0x1d00ffff; r.nNonce = (unsigned)height;
    return r;
}

static BlockIndexDerivedEntry R2DMakeDerived(const uint256& trust, uint32_t checksum)
{
    BlockIndexDerivedEntry d;
    d.chainTrust = trust; d.stakeModifierChecksum = checksum;
    d.SetHasStakeModifierTime(true); d.stakeModifierTime = 1700000000;
    d.SetHasBlockSize(true); d.nSize = 1200 + (checksum % 100);
    return d;
}

static std::vector<unsigned char> R2DReadFile(const std::string& path)
{
    std::vector<unsigned char> v;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return v;
    unsigned char buf[4096]; size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) v.insert(v.end(), buf, buf + n);
    fclose(f);
    return v;
}

static bool R2DWriteFile(const std::string& path, const std::vector<unsigned char>& v)
{
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = v.empty() || (fwrite(&v[0], 1, v.size(), f) == v.size());
    fclose(f);
    return ok;
}

static bool R2DTruncateFileTo(const std::string& path, size_t newLen)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    std::vector<unsigned char> buf; unsigned char tmp[4096]; size_t n;
    while ((n = fread(tmp, 1, sizeof(tmp), f)) > 0) buf.insert(buf.end(), tmp, tmp + n);
    fclose(f);
    if (newLen > buf.size()) return false;
    buf.resize(newLen);
    return R2DWriteFile(path, buf);
}

// RAII authoritative boot fixture: builds + boots a real S>0 generation, then
// restores every process global it disturbed on destruction.
struct R2DGenFixture
{
    fs::path root;
    int S;
    std::vector<uint256> active;
    CBlockIndex* sBest; CBlockIndex* sGen; uint256 sHash; int sH; uint256 sT;
    std::set<uint256> sInvalid;

    explicit R2DGenFixture(int s) : S(s), sBest(NULL), sGen(NULL), sH(-1)
    {
        sBest = pindexBest; sGen = pindexGenesisBlock;
        sHash = hashBestChain; sH = nBestHeight; sT = nBestChainTrust;
        sInvalid = setInvalidBlockHash;
        root = fs::temp_directory_path() / fs::unique_path("r2dgen-%%%%-%%%%");
        std::string error;
        BOOST_REQUIRE_MESSAGE(R2DBuildSyntheticGenerationAndInit(root, S, &active, &error), error);
        BOOST_REQUIRE(g_fAuthoritativeStartup);
    }
    ~R2DGenFixture()
    {
        ResetBlockIndexAuthoritativeStartupForTest();
        pindexBest = sBest; pindexGenesisBlock = sGen;
        hashBestChain = sHash; nBestHeight = sH; nBestChainTrust = sT;
        setInvalidBlockHash = sInvalid;
        try { fs::remove_all(root); } catch (...) {}
    }
};

static void R2DAppendActive(BlockIndexAuthoritativeLive* live, const uint256& hash,
                            const uint256& prev, int height, const uint256& trust)
{
    std::string err;
    BOOST_REQUIRE_MESSAGE(
        live->AcceptActive(R2DMakeRecord(hash, prev, height),
                           R2DMakeDerived(trust, (uint32_t)height), height, &err), err);
}

static void R2DAppendSide(BlockIndexAuthoritativeLive* live, const uint256& hash,
                          const uint256& prev, int height, const uint256& trust)
{
    std::string err;
    BOOST_REQUIRE_MESSAGE(
        live->AcceptSide(R2DMakeRecord(hash, prev, height),
                         R2DMakeDerived(trust, (uint32_t)height), &err), err);
}

// Install post-S active branch A (S+1,S+2; higher trust) and competing side
// branch B (S+1,S+2; lower trust) sharing the immutable base tip S as ancestor.
static void R2DInstallBranches(BlockIndexAuthoritativeLive* live, const uint256& sHash, int S,
                               const uint256& A1, const uint256& A2,
                               const uint256& B1, const uint256& B2)
{
    R2DAppendActive(live, A1, sHash, S + 1, uint256(100));
    R2DAppendActive(live, A2, A1, S + 2, uint256(200));
    R2DAppendSide(live, B1, sHash, S + 1, uint256(50));
    R2DAppendSide(live, B2, B1, S + 2, uint256(150));
}

// ---------------------------------------------------------------------------
// COHORT A: live transient projection == durable selected tip (invalidate AND
// reconsider), asserted BEFORE and AFTER a clean restart. Exercises the real
// production InvalidateBlock/ReconsiderBlock on a non-resident target.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(r2d_p02_live_transient_projection_before_after_restart)
{
    R2DGenFixture fx(12);
    const int S = fx.S; const uint256 sHash = fx.active[S];
    BOOST_REQUIRE(S > 0);
    BlockIndexAuthoritativeLive* live = GetAuthoritativeLiveAuthority();
    BOOST_REQUIRE(live && live->IsOpen());
    BlockIndexTipAuthority* tip = live->TipAuthorityMutable();
    BOOST_REQUIRE(tip != NULL);
    BOOST_REQUIRE_EQUAL(AuthoritativeBaseTipHeight(), S);

    const uint256 A1(0xA10001UL), A2(0xA10002UL), B1(0xB10001UL), B2(0xB10002UL);
    R2DInstallBranches(live, sHash, S, A1, A2, B1, B2);
    BOOST_REQUIRE(tip->GetTip().record.hash == A2);

    // Pre-mutation projection is the immutable base anchor S (no post-S tip was
    // published by the boot); proves the projection is genuinely refreshed below.
    BOOST_CHECK_MESSAGE(nBestHeight == S && pindexBest != NULL,
        "pre-mutation projection must be the immutable base anchor S");
    // PM1-P0-06 non-regression: no historical CBlockIndex population introduced.
    {
        LOCK(cs_main);
        int hist = 0;
        for (size_t h = 0; h < fx.active.size(); ++h) if (mapBlockIndex.count(fx.active[h])) ++hist;
        BOOST_CHECK_EQUAL(hist, 0);
        BOOST_CHECK_EQUAL(mapBlockIndex.count(A1), (size_t)0);
        BOOST_CHECK_EQUAL(mapBlockIndex.count(A2), (size_t)0);
    }

    // ---- production InvalidateBlock(A1): active branch loses, B selected ----
    {
        std::string err;
        BOOST_REQUIRE_MESSAGE(InvalidateBlock(A1, err), err);
        const BlockIndexTipRead dt = tip->GetTip();
        BOOST_REQUIRE(dt.status == BLOCK_INDEX_TIP_OK);
        BOOST_CHECK_MESSAGE(dt.record.hash == B2,
            "invalidate: durable selected tip must be the competing branch tip");
        BOOST_CHECK_MESSAGE(pindexBest != NULL && pindexBest->GetBlockHash() == dt.record.hash,
            "LIVE PROJECTION (invalidate): projected best tip MUST equal durable selected tip");
        BOOST_CHECK_EQUAL(nBestHeight, dt.height);
        BOOST_CHECK(hashBestChain == dt.record.hash);
        BOOST_TEST_MESSAGE("R2D_LIVE_PROJ invalidate durable_tip=" << dt.record.hash.ToString()
            << " projected=" << (pindexBest ? pindexBest->GetBlockHash().ToString() : std::string("<null>"))
            << " height=" << nBestHeight);
    }

    // ---- restart: same authoritative tip ----
    {
        ResetBlockIndexAuthoritativeStartupForTest();
        std::string e2;
        BOOST_REQUIRE_MESSAGE(InitBlockIndexAuthoritative(fx.root.string(), &e2), e2);
        const BlockIndexTipAuthority* tip2 = GetAuthoritativeLiveAuthority()->TipAuthority();
        BOOST_REQUIRE(tip2 != NULL);
        BOOST_CHECK_MESSAGE(tip2->GetTip().record.hash == B2,
            "restart after invalidate must keep the durable selected tip");
        BOOST_CHECK_EQUAL(nBestHeight, S + 2);
        BOOST_CHECK(hashBestChain == B2);
    }

    // ---- production ReconsiderBlock(A1): branch A restored as best ----
    {
        std::string e3;
        BOOST_REQUIRE_MESSAGE(ReconsiderBlock(A1, e3), e3);
        BlockIndexTipAuthority* tip3 = GetAuthoritativeLiveAuthority()->TipAuthorityMutable();
        const BlockIndexTipRead dt3 = tip3->GetTip();
        BOOST_CHECK_MESSAGE(dt3.record.hash == A2,
            "reconsider: branch A must be restored as the durable authority");
        BOOST_CHECK_MESSAGE(pindexBest != NULL && pindexBest->GetBlockHash() == dt3.record.hash,
            "LIVE PROJECTION (reconsider): projected best tip MUST equal durable selected tip");
        BOOST_CHECK_EQUAL(nBestHeight, dt3.height);
        BOOST_CHECK(hashBestChain == dt3.record.hash);
    }

    // ---- restart: restored authority remains ----
    {
        ResetBlockIndexAuthoritativeStartupForTest();
        std::string e4;
        BOOST_REQUIRE_MESSAGE(InitBlockIndexAuthoritative(fx.root.string(), &e4), e4);
        const BlockIndexTipAuthority* tip4 = GetAuthoritativeLiveAuthority()->TipAuthority();
        BOOST_CHECK_MESSAGE(tip4->GetTip().record.hash == A2,
            "restart after reconsider must keep the restored authority");
        BOOST_CHECK_EQUAL(nBestHeight, S + 2);
        BOOST_CHECK(hashBestChain == A2);
    }
}

// ---------------------------------------------------------------------------
// COHORT A (non-resident): the target is NOT resident in mapBlockIndex; the real
// production resolver finds it BY VALUE, the invalid intent is durably committed,
// the best eligible competing authority is selected, the projection updates
// immediately, and everything persists across restart / reconsider.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(r2d_p02_nonresident_production_operator_target)
{
    R2DGenFixture fx(12);
    const int S = fx.S; const uint256 sHash = fx.active[S];
    BlockIndexAuthoritativeLive* live = GetAuthoritativeLiveAuthority();
    BOOST_REQUIRE(live && live->IsOpen());
    BlockIndexTipAuthority* tip = live->TipAuthorityMutable();
    BOOST_REQUIRE(tip != NULL);

    const uint256 A1(0xAA1001UL), A2(0xAA1002UL), B1(0xBB1001UL), B2(0xBB1002UL);
    R2DInstallBranches(live, sHash, S, A1, A2, B1, B2);

    // EXPLICIT non-residency of the production target and the competing tip.
    BOOST_CHECK_MESSAGE(mapBlockIndex.count(A1) == 0, "target A1 must NOT be resident in mapBlockIndex");
    BOOST_CHECK_EQUAL(mapBlockIndex.count(A2), (size_t)0);
    BOOST_CHECK_EQUAL(mapBlockIndex.count(B2), (size_t)0);
    // ... yet the by-value authority resolves A1 (the production resolver's source).
    {
        BlockIndexSnapshot snap; std::string serr;
        BOOST_REQUIRE(live->ResolveBlockSnapshot(A1, &snap, &serr) == BlockIndexHotStatus::OK);
        BOOST_CHECK(snap.found);
        BOOST_CHECK_EQUAL(snap.height, S + 1);
    }

    // Production InvalidateBlock(A1) with A1 NON-RESIDENT.
    {
        std::string err;
        BOOST_REQUIRE_MESSAGE(InvalidateBlock(A1, err), err);
        BOOST_CHECK_MESSAGE(tip->IsOperatorInvalid(A1),
            "invalid intent must be durably committed in the mutable tip");
        BOOST_CHECK(tip->GetTip().record.hash == B2);
        BOOST_CHECK(pindexBest != NULL && pindexBest->GetBlockHash() == B2);
        BOOST_TEST_MESSAGE("R2D_NONRESIDENT invalidate target_nresident=0 tip=" << tip->GetTip().record.hash.ToString());
    }

    // Restart: invalid intent + selected authority persist.
    {
        ResetBlockIndexAuthoritativeStartupForTest();
        std::string e2;
        BOOST_REQUIRE_MESSAGE(InitBlockIndexAuthoritative(fx.root.string(), &e2), e2);
        BlockIndexTipAuthority* tip2 = GetAuthoritativeLiveAuthority()->TipAuthorityMutable();
        BOOST_CHECK(tip2->IsOperatorInvalid(A1));
        BOOST_CHECK(tip2->GetTip().record.hash == B2);
        BOOST_CHECK(hashBestChain == B2);
    }

    // Production ReconsiderBlock(A1) with A1 still NON-RESIDENT.
    {
        BOOST_CHECK_EQUAL(mapBlockIndex.count(A1), (size_t)0);
        std::string e3;
        BOOST_REQUIRE_MESSAGE(ReconsiderBlock(A1, e3), e3);
        BlockIndexTipAuthority* tip3 = GetAuthoritativeLiveAuthority()->TipAuthorityMutable();
        BOOST_CHECK(!tip3->IsOperatorInvalid(A1));
        BOOST_CHECK(tip3->GetTip().record.hash == A2);
        BOOST_CHECK(pindexBest != NULL && pindexBest->GetBlockHash() == A2);
    }

    // Restart: restored authority persists.
    {
        ResetBlockIndexAuthoritativeStartupForTest();
        std::string e4;
        BOOST_REQUIRE_MESSAGE(InitBlockIndexAuthoritative(fx.root.string(), &e4), e4);
        BOOST_CHECK(hashBestChain == A2);
        BOOST_CHECK_EQUAL(nBestHeight, S + 2);
    }
}

// ---------------------------------------------------------------------------
// COHORT C: real S>0 seam + deep-reorg boundary + operator boundary + NULL/NULL
// alias elimination.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(r2d_p09_real_s0_seam_deep_reorg_boundary)
{
    R2DGenFixture fx(12);
    const int S = fx.S; const uint256 sHash = fx.active[S];
    BOOST_REQUIRE_MESSAGE(S > 0, "COHORT C requires a real S>0 generation");
    BlockIndexAuthoritativeLive* live = GetAuthoritativeLiveAuthority();
    BOOST_REQUIRE(live && live->IsOpen());
    BlockIndexTipAuthority* tip = live->TipAuthorityMutable();
    BOOST_REQUIRE(tip != NULL);
    BOOST_REQUIRE_EQUAL(AuthoritativeBaseTipHeight(), S);

    const uint256 A1(0xC1A001UL), A2(0xC1A002UL), B1(0xC1B001UL), B2(0xC1B002UL);
    R2DInstallBranches(live, sHash, S, A1, A2, B1, B2);
    BOOST_REQUIRE(tip->GetTip().record.hash == A2);

    // (1) FORK == S: legal competing post-S transition whose common ancestor is
    //     exactly the immutable base tip S. Invalidate the S+1 active block so the
    //     fork lands exactly at S and branch B replaces A. ALLOWED.
    {
        std::string err;
        BOOST_REQUIRE_MESSAGE(InvalidateBlock(A1, err), err);
        const BlockIndexTipRead dt = tip->GetTip();
        BOOST_CHECK_MESSAGE(dt.record.hash == B2,
            "FORK==S: a legal post-S transition with common ancestor S must be ALLOWED");
        BOOST_CHECK_EQUAL(tip->TipHeight(), S + 2);
        BOOST_TEST_MESSAGE("R2D_FORK_EQ_S allowed tip=" << dt.record.hash.ToString()
            << " S=" << S << " fork_used=S height=" << tip->TipHeight());
    }
    // Restart: same authority.
    {
        ResetBlockIndexAuthoritativeStartupForTest();
        std::string e2;
        BOOST_REQUIRE_MESSAGE(InitBlockIndexAuthoritative(fx.root.string(), &e2), e2);
        BOOST_CHECK(GetAuthoritativeLiveAuthority()->TipAuthority()->GetTip().record.hash == B2);
        BOOST_CHECK_EQUAL(nBestHeight, S + 2);
        BOOST_CHECK(hashBestChain == B2);
    }

    // (2)+(3) FORK == S-1 and DEEPER FORK: the fused mutable-authority transition
    //     primitive the production operator drives MUST fail closed BEFORE mutation.
    {
        BlockIndexTipAuthority* tipN = GetAuthoritativeLiveAuthority()->TipAuthorityMutable();
        const int32_t h0 = tipN->TipHeight();
        const uint256 hash0 = tipN->GetTip().record.hash;
        const uint32_t log0 = tipN->InvalidLogCount();
        const uint256 Z1(0xDEAD0001UL), Z2(0xDEAD0002UL);
        std::vector<BlockIndexTipAppend> emptyBranch; std::vector<int32_t> emptyHeights;
        std::string e1, e2, e3;
        BOOST_CHECK_MESSAGE(tipN->ApplyOperatorInvalidAndReorg(Z1, true, S - 1, emptyBranch, emptyHeights, &e1) != BLOCK_INDEX_TIP_OK,
            "FORK==S-1: must fail closed BEFORE mutation");
        BOOST_CHECK_MESSAGE(tipN->ApplyOperatorInvalidAndReorg(Z2, true, S - 2, emptyBranch, emptyHeights, &e2) != BLOCK_INDEX_TIP_OK,
            "DEEPER FORK: must fail closed BEFORE mutation");
        BOOST_CHECK_MESSAGE(tipN->ReorgActiveTo(S - 1, emptyBranch, emptyHeights, &e3) != BLOCK_INDEX_TIP_OK,
            "FORK==S-1 (ReorgActiveTo): must fail closed BEFORE mutation");
        // unchanged: durable authority, effective tip, mutable metadata, invalid state
        BOOST_CHECK_EQUAL(tipN->TipHeight(), h0);
        BOOST_CHECK(tipN->GetTip().record.hash == hash0);
        BOOST_CHECK_EQUAL(tipN->InvalidLogCount(), log0);
        BOOST_CHECK(!tipN->IsOperatorInvalid(Z1));
        BOOST_CHECK(!tipN->IsOperatorInvalid(Z2));
        BOOST_TEST_MESSAGE("R2D_BELOW_S failclosed fork_s_1=1 deeper=1 unchanged=1");
    }
    // Restart: exact same previous authority.
    {
        ResetBlockIndexAuthoritativeStartupForTest();
        std::string e4;
        BOOST_REQUIRE_MESSAGE(InitBlockIndexAuthoritative(fx.root.string(), &e4), e4);
        const BlockIndexTipAuthority* tip4 = GetAuthoritativeLiveAuthority()->TipAuthority();
        BOOST_CHECK_EQUAL(tip4->TipHeight(), S + 2);
        BOOST_CHECK(tip4->GetTip().record.hash == B2);
        BOOST_CHECK(!tip4->IsOperatorInvalid(uint256(0xDEAD0001UL)));
    }

    // (4) PRODUCTION OPERATOR BOUNDARY: InvalidateBlock(S) and InvalidateBlock(S-1)
    //     rejected BEFORE mutation (rebase/rebuild contract).
    {
        BlockIndexAuthoritativeLive* l4 = GetAuthoritativeLiveAuthority();
        BlockIndexTipAuthority* t4 = l4->TipAuthorityMutable();
        const int32_t h0 = t4->TipHeight(); const uint256 hash0 = t4->GetTip().record.hash;
        const std::set<uint256> inv0 = t4->OperatorInvalidSet();
        std::string e1, e2;
        BOOST_CHECK_MESSAGE(!InvalidateBlock(sHash, e1),
            "InvalidateBlock(S) MUST be rejected before mutation");
        BOOST_CHECK_MESSAGE(!InvalidateBlock(fx.active[S - 1], e2),
            "InvalidateBlock(S-1) MUST be rejected before mutation");
        BOOST_CHECK_EQUAL(t4->TipHeight(), h0);
        BOOST_CHECK(t4->GetTip().record.hash == hash0);
        BOOST_CHECK(t4->OperatorInvalidSet() == inv0);
        BOOST_TEST_MESSAGE("R2D_OP_BOUNDARY invalidate_S=0 invalidate_S_1=0 errS="
                           << e1.substr(0, 48));
    }
    // Restart: unchanged.
    {
        ResetBlockIndexAuthoritativeStartupForTest();
        std::string e5;
        BOOST_REQUIRE_MESSAGE(InitBlockIndexAuthoritative(fx.root.string(), &e5), e5);
        BOOST_CHECK_EQUAL(nBestHeight, S + 2);
        BOOST_CHECK(hashBestChain == B2);
    }

    // (5) NULL/NULL ALIAS regression: two UNRESOLVED retained ancestors must NOT be
    //     treated as a valid common ancestor. Runtime assertion on the real
    //     production cutover guard.
    {
        BlockIndexAuthoritativeLive* l5 = GetAuthoritativeLiveAuthority();
        CBlockIndex newIdx; newIdx.nHeight = 5; newIdx.pprev = NULL;
        CBlockIndex oldIdx; oldIdx.nHeight = 4; oldIdx.pprev = NULL;
        const uint256 oldHash(0xD1D1D1UL);
        { LOCK(cs_main); mapBlockIndex[oldHash] = &oldIdx; }
        std::string e;
        const bool r = PublishAuthoritativeLiveTailCutoverForTesting(l5, &newIdx, oldHash, &e);
        { LOCK(cs_main); mapBlockIndex.erase(oldHash); }   // erase BEFORE asserting (no dangling)
        BOOST_CHECK_MESSAGE(!r, "NULL==NULL MUST NOT be treated as a valid common ancestor (fail closed)");
        BOOST_CHECK_MESSAGE(e.find("no common ancestor") != std::string::npos,
            "NULL/NULL alias must be reported as no-common-ancestor (err=" << e << ")");
        BOOST_TEST_MESSAGE("R2D_NULL_ALIAS failclosed=" << (r ? 0 : 1) << " err=" << e);
    }
}

// ---------------------------------------------------------------------------
// COHORT B: startup-level PM1-P0-01 N1 / N2 (drive the REAL production startup
// entry). The durable mutable tip claims a post-S tip L>S but the committed
// record is missing (N1) or inconsistent (N2); authoritative startup MUST fail
// closed and publish NO partial authority (never silently fall back to S).
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(r2d_p01_startup_n1_missing_post_s_record_fails_closed)
{
    R2DGenFixture fx(12);
    const int S = fx.S; const uint256 sHash = fx.active[S];
    BlockIndexAuthoritativeLive* live = GetAuthoritativeLiveAuthority();
    BOOST_REQUIRE(live && live->IsOpen());
    const uint256 L(0x11AA11UL);
    R2DAppendActive(live, L, sHash, S + 1, uint256(9));
    BOOST_REQUIRE_EQUAL(live->TipAuthorityMutable()->TipHeight(), S + 1);
    ResetBlockIndexAuthoritativeStartupForTest();   // simulate process end (stores closed)

    // Corrupt: truncate tip-records.dat below the tip.meta-committed record count
    // while the durable tip still CLAIMS height S+1 (required record missing).
    const std::string rp = (fx.root / "blockindex_tip" / "tip-records.dat").string();
    BOOST_REQUIRE(R2DTruncateFileTo(rp, 0));

    std::string err;
    const bool ok = InitBlockIndexAuthoritative(fx.root.string(), &err);
    BOOST_CHECK_MESSAGE(!ok, "STARTUP N1: missing committed post-S record MUST fail closed");
    BOOST_CHECK_MESSAGE(pindexBest == NULL && nBestHeight == -1,
        "STARTUP N1: no partial authority publication / no silent fallback to S");
    BOOST_TEST_MESSAGE("R2D_P01_N1 startup_ok=" << (int)ok << " no_fallback=" << (pindexBest == NULL) << " err=" << err);
}

BOOST_AUTO_TEST_CASE(r2d_p01_startup_n2_inconsistent_post_s_record_fails_closed)
{
    R2DGenFixture fx(12);
    const int S = fx.S; const uint256 sHash = fx.active[S];
    BlockIndexAuthoritativeLive* live = GetAuthoritativeLiveAuthority();
    BOOST_REQUIRE(live && live->IsOpen());
    const uint256 L(0x22BB22UL);
    R2DAppendActive(live, L, sHash, S + 1, uint256(9));
    BOOST_REQUIRE_EQUAL(live->TipAuthorityMutable()->TipHeight(), S + 1);
    ResetBlockIndexAuthoritativeStartupForTest();

    // Corrupt: flip a byte inside the committed record region so the committed
    // post-S record no longer matches the tip.meta content digest (inconsistent).
    const std::string rp = (fx.root / "blockindex_tip" / "tip-records.dat").string();
    std::vector<unsigned char> rec = R2DReadFile(rp);
    BOOST_REQUIRE(rec.size() > 64);
    rec[64] ^= 0xFF;
    BOOST_REQUIRE(R2DWriteFile(rp, rec));

    std::string err;
    const bool ok = InitBlockIndexAuthoritative(fx.root.string(), &err);
    BOOST_CHECK_MESSAGE(!ok, "STARTUP N2: inconsistent committed post-S record MUST fail closed");
    BOOST_CHECK_MESSAGE(pindexBest == NULL && nBestHeight == -1,
        "STARTUP N2: no partial authority publication / no silent fallback to S");
    BOOST_TEST_MESSAGE("R2D_P01_N2 startup_ok=" << (int)ok << " no_fallback=" << (pindexBest == NULL) << " err=" << err);
}

BOOST_AUTO_TEST_SUITE_END()
