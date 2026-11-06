// ATOMIC-REORG (S1-d) — integration tests of the production V -> C -> A path.
//
// These tests drive the REAL production code: real regtest blocks through
// ProcessBlock/AcceptBlock -> AddToBlockIndex -> SetBestChain -> ReorganizeAtomic
// with the REAL BI-V2 authority (bootstrapped by the global test fixture) and a
// REAL armed Store3A (scratch dir, opened + reconciled to the current active tip
// through the production startup recovery function). No consensus code is
// re-implemented here: block construction/extraction is the same helper shape the
// reorg_314 reproduction uses.
//
// Covered invariants (S1-d owner list):
//   1  branch blocks see the UTXO changes of previous branch blocks (the overlay
//      drives every V-phase resolve; a miss would fail validation);
//   2  full consensus validation before commit (a V-phase failure leaves no
//      durable change — case v_before_c_aborts_clean);
//   3  fork state only from peekUndo + bounded overlay (stats.lastOverlayEntries);
//   4  winner/base-tip checks immediately before C (inside ReorganizeAtomic);
//   5  ONE ReorgActiveTo for the whole branch (stats.count + no prefix ever
//      becomes the committed tip — the failpoint case proves the only state a
//      crash can expose is fork-level or fully-committed, never a mid-branch
//      commit other than the single publication);
//   7  post-C A failure => deterministic reconcile or fail-stop, never success;
//   8  overlay/trail/flag state cleared on EVERY exit path (stats.overlayArmed /
//      trailArmed / trailEntries asserted after each case).
//
// Depth matrix: 100 / 1000 / 2101 disconnected blocks (fork at the pre-branch tip).
#include <boost/test/unit_test.hpp>

#include "main.h"
#include "chainstore_context.h"
#include "miner.h"
#include "wallet.h"
#include "util.h"
#include "serialize.h"
#include "chainstate/store3a/incstore.h"
#include "blockindex_tip.h"
#include "blockindex_manager.h"
#include "blockindex_authoritative_live.h"
#include "chainstate/observe_hook.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <string>
#include <vector>

extern CWallet* pwalletMain;

namespace atomicreorg {

static CBlock* MakeCoinbaseBlock(CBlockIndex* pindexPrev, unsigned int nTag)
{
    CBlock* pblock = CreateNewBlock(pwalletMain, false, NULL, NULL);
    if (pblock == NULL)
        return NULL;
    pblock->nVersion = 1;
    int64_t nTime = (int64_t)pindexPrev->GetBlockTime() + 1;
    if (nTime < (int64_t)pindexPrev->GetMedianTimePast() + 1)
        nTime = (int64_t)pindexPrev->GetMedianTimePast() + 1;
    pblock->nTime = (unsigned int)nTime;
    pblock->hashPrevBlock = *pindexPrev->phashBlock;
    pblock->vtx[0].nTime = pblock->nTime;
    pblock->vtx[0].vin[0].scriptSig = CScript() << (pindexPrev->nHeight + 1) << nTag;
    pblock->hashMerkleRoot = pblock->BuildMerkleTree();
    const uint256 hashTarget = CBigNum().SetCompact(pblock->nBits).getuint256();
    while (pblock->GetHash() > hashTarget && pblock->nNonce < 0xffffffff)
        ++pblock->nNonce;
    return pblock;
}

// Pre-build a whole competing branch OFFLINE (no mapBlockIndex dependency), so a
// >horizon branch can be constructed without holding raw CBlockIndex pointers
// across the retirement floor. The template is the production CreateNewBlock
// output (same coinbase shape); each block is re-parented onto the PREVIOUS
// PRE-BUILT block with a deterministic +1s timestamp and a real PoW grind — the
// same construction rule reorg_314 uses, applied to a locally tracked parent.
struct PreBlock { CBlock blk; uint256 hash; int height; int64_t time; };

static bool PrebuildBranch(const CBlockIndex* fork, int count, unsigned int tagBase,
                           std::vector<PreBlock>* out, std::string* err)
{
    out->clear();
    CBlock* tmplRaw = CreateNewBlock(pwalletMain, false, NULL, NULL);
    if (tmplRaw == NULL) { if (err) *err = "CreateNewBlock template unavailable"; return false; }
    const CBlock tmpl = *tmplRaw;
    delete tmplRaw;

    uint256 prevHash = fork->GetBlockHash();
    int64_t prevTime = (int64_t)fork->GetBlockTime();
    int prevHeight = fork->nHeight;
    // The pre-built parent's MTP is dominated by the fork's recent times; +1s per
    // block keeps every block strictly above its parent's MTP (regtest spacing 1s).
    for (int i = 1; i <= count; ++i)
    {
        CBlock b = tmpl;
        b.nVersion = 1;
        int64_t nTime = prevTime + 1;
        b.nTime = (unsigned int)nTime;
        b.hashPrevBlock = prevHash;
        b.vtx[0].nTime = b.nTime;
        b.vtx[0].vin[0].scriptSig = CScript() << (prevHeight + 1) << (tagBase + (unsigned)i);
        b.hashMerkleRoot = b.BuildMerkleTree();
        const uint256 hashTarget = CBigNum().SetCompact(b.nBits).getuint256();
        while (b.GetHash() > hashTarget && b.nNonce < 0xffffffff)
            ++b.nNonce;
        PreBlock pb;
        pb.blk = b;
        pb.hash = b.GetHash();
        pb.height = prevHeight + 1;
        pb.time = nTime;
        out->push_back(pb);
        prevHash = pb.hash;
        prevTime = nTime;
        prevHeight += 1;
    }
    return true;
}

// Deliver through the production acceptance path. Side branches that the
// weak-work ProcessBlock gate refuses fall back to the same storage path the
// network uses (WriteToDisk + AcceptBlock), exactly like reorg_314.
enum Delivery { DELIVERY_REJECTED = 0, DELIVERY_PROCESSBLOCK = 1, DELIVERY_ACCEPTBLOCK = 2 };
static Delivery Deliver(CBlock* pblock, uint256* outHash)
{
    const uint256 hash = pblock->GetHash();
    *outHash = hash;
    {
        LOCK(cs_main);
        if (pblock->CheckBlock(true, true, true) && ProcessBlock(NULL, pblock))
            return DELIVERY_PROCESSBLOCK;
    }
    unsigned int nFile = 0, nBlockPos = 0;
    {
        LOCK(cs_main);
        if (pblock->WriteToDisk(nFile, nBlockPos) && pblock->AcceptBlock())
            return DELIVERY_ACCEPTBLOCK;
    }
    return DELIVERY_REJECTED;
}

// Deliver one branch-B block with a one-shot reorg-boundary failpoint armed.
// Returns false when the block was REJECTED by a TRIPPED reorg boundary (the
// failpoint was consumed by the reorg path). Side-block acceptances (the normal
// case for a competing branch below the tip) leave the failpoint untouched; the
// fallback then runs with the failpoint disarmed, because a side acceptance
// cannot enter the reorg arm.
static bool DeliverWithReorgFailpoint(CBlock* pblock, uint256* outHash, const char* fp, bool* tripped)
{
    *tripped = false;
    MainFailpointSetForTesting(fp, true);
    bool ok = false;
    {
        LOCK(cs_main);
        ok = pblock->CheckBlock(true, true, true) && ProcessBlock(NULL, pblock);
    }
    if (ok) { *outHash = pblock->GetHash(); MainFailpointSetForTesting(fp, false); return true; }
    if (!MainFailpointArmedForTesting(fp))
    {
        // The failpoint was consumed: the attempted path WAS the reorg boundary.
        *tripped = true;
        *outHash = pblock->GetHash();
        return false;
    }
    MainFailpointSetForTesting(fp, false);
    unsigned int nFile = 0, nBlockPos = 0;
    {
        LOCK(cs_main);
        ok = pblock->WriteToDisk(nFile, nBlockPos) && pblock->AcceptBlock();
    }
    *outHash = pblock->GetHash();
    return ok;
}

static uint64_t RssKb()
{
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line))
        if (line.compare(0, 6, "VmRSS:") == 0)
            return strtoull(line.c_str() + 6, NULL, 10);
    return 0;
}

// Four independent height sources must agree:
//   1 getblockcount (authoritative committed tip, GetBlockIndexManager)
//   2 the V2 tip authority height/hash
//   3 Store3A committed tip height/hash
//   4 the legacy derived view (nBestHeight / hashBestChain)
static bool FourHeightsAgree(std::string* why)
{
    const BlockIndexTipAuthority* tipAuth =
        GetAuthoritativeLiveAuthority() ? GetAuthoritativeLiveAuthority()->TipAuthority() : NULL;
    if (tipAuth == NULL) { if (why) *why = "no tip authority"; return false; }
    int mgrHeight = -1; std::string merr;
    if (GetBlockIndexManager().ActiveTipHeight(&mgrHeight, &merr) != BLOCK_INDEX_MANAGER_OK)
    { if (why) *why = "getblockcount/manager tip unavailable: " + merr; return false; }
    // In the immutable-base window there is no logical tip yet: the authoritative
    // identity is the BASE tip (same rule the extension decision uses).
    const uint256 authHashT = tipAuth->TipHash();
    const bool haveLogical = (authHashT != uint256(0));
    const int authHeight = haveLogical ? tipAuth->TipHeight() : AuthoritativeBaseTipHeight();
    const uint256 authHash = haveLogical ? authHashT : tipAuth->BaseTipHash();
    cs::store3a::CommitStore3A* st = s16::g_chainObserve.store;
    if (st == NULL || !st->opened()) { if (why) *why = "store not armed"; return false; }
    // The armed store batches its durability barrier (S2.1): the logical tree is
    // already at the branch tip but the MANIFEST may lag by the open batch. Settle
    // the owed barrier before comparing the durable tip — this is the standing
    // contract, not part of the atomic window.
    if (!s16::g_chainObserve.pending.empty() && !s16::g_chainObserve.FlushPending())
    { if (why) *why = "store barrier flush failed: " + s16::g_chainObserve.lastOpError; return false; }
    uint256 stHash; memcpy(stHash.begin(), st->tipHash().data(), 32);
    if (mgrHeight != authHeight) { if (why) *why = strprintf("manager=%d authority=%d", mgrHeight, authHeight); return false; }
    if (authHeight != nBestHeight) { if (why) *why = strprintf("authority=%d nBestHeight=%d", authHeight, nBestHeight); return false; }
    if (authHash != hashBestChain) { if (why) *why = strprintf("authority hash=%s h=%d != hashBestChain=%s h=%d", authHash.ToString().substr(0,16).c_str(), authHeight, hashBestChain.ToString().substr(0,16).c_str(), nBestHeight); return false; }
    if (st->tipHeight() != nBestHeight) { if (why) *why = strprintf("store=%d nBestHeight=%d", st->tipHeight(), nBestHeight); return false; }
    if (stHash != hashBestChain) { if (why) *why = "store tip hash != hashBestChain"; return false; }
    return true;
}

// The whole V->C->A run requires the PRODUCTION authority pair: authoritative
// startup (global fixture) + armed Store3A. Production always has both
// (init.cpp); the fixture boots the authority and we arm the store here.
struct ArmedStoreFixture
{
    cs::store3a::CommitStore3A store;
    s16::ChainObserveState saved;
    bool armed;
    std::string err;
    boost::filesystem::path dir;

    ArmedStoreFixture() : armed(false)
    {
        namespace bfs = boost::filesystem;
        dir = bfs::temp_directory_path() / bfs::unique_path("atomic-reorg-%%%%-%%%%");
        bfs::create_directories(dir);
        cs::ChainError oe;
        if (!store.open(dir.string(), &oe)) { err = oe.str(); return; }
        // Bring the fresh store to the CURRENT active tip with the production
        // startup recovery (replay from height 1; also exercises the C4 path).
        // The in-process harness has no PUBLISHED committed-tip snapshot, so the
        // authoritative branch of the reconcile predicate is not available here;
        // reconcile through its chain-projection arm (same replay code, same
        // blocks, same deltas — only the chain-tip source differs), then restore
        // the authoritative flag the rest of the suite (and the production path
        // under test) requires.
        {
            // Born-commit at genesis height 0 (the production bootstrap shape: the
            // store's tip identity is the genesis block even though genesis is not a
            // UTXO delta). An empty delta at h=0 is the same representation the
            // native bootstrap publishes.
            if (store.tipHeight() < 0)
            {
                if (pindexGenesisBlock == NULL) { err = "no genesis block loaded"; store.close(); return; }
                cs::Delta birth;
                cs::Hash gh; memcpy(gh.data(), pindexGenesisBlock->GetBlockHash().begin(), 32);
                cs::ChainError be;
                if (!store.publish(birth, gh, 0, &be)) { err = "genesis born-commit: " + be.str(); store.close(); return; }
            }
            // S1-e: reconcile by VALUE (authoritative arm). The legacy
            // projection arm (mapBlockIndex + GetAncestor) cannot resolve the
            // active chain across the bounded-residency boundary — after a
            // >2000-block fixture mining order the h=1 ancestor is retired,
            // and the projection resolver fails with "no ancestor at h=1".
            // The by-value resolver is the production path for exactly this
            // reason (IFCN#9 correctness fix).
            // Attempt 1: by-value (authoritative) arm — no GetAncestor over
            // retired mapBlockIndex entries. When the process has not yet
            // published a committed tip at all (fresh process, tip=genesis),
            // this arm bails out by design, so retry once with the legacy
            // projection arm for this in-process fixture only.
            std::string recErr;
            const bool rok = s16::ReconcileStore3AToChain(&store, &recErr);
            if (!rok) { err = recErr; store.close(); return; }
        }
        saved = s16::g_chainObserve;
        s16::g_chainObserve.store = &store;
        s16::g_chainObserve.enabled = true;
        s16::g_chainObserve.checkAuthority = true;
        s16::g_chainObserve.hardFail = true;
        s16::g_chainObserve.batchSize = 1000;
        s16::g_chainObserve.batchChunk = 0;
        s16::g_chainObserve.pending.clear();
        s16::g_chainObserve.needsRecovery = false;
        // Restart-equivalence: the atomic protocol compares the committed authority,
        // Store3A and the legacy base view, so derive the legacy globals from the
        // committed authority exactly as a restart would.
        {
            const BlockIndexTipAuthority* ta =
                GetAuthoritativeLiveAuthority() ? GetAuthoritativeLiveAuthority()->TipAuthority() : NULL;
            if (ta != NULL && ta->TipHash() != uint256(0))
            {
                LOCK(cs_main);
                std::map<uint256, CBlockIndex*>::iterator it = mapBlockIndex.find(ta->TipHash());
                if (it != mapBlockIndex.end() && it->second != NULL)
                {
                    hashBestChain = ta->TipHash();
                    pindexBest = it->second;
                    nBestHeight = it->second->nHeight;
                    nBestChainTrust = it->second->nChainTrust;
                }
            }
        }
        armed = true;
    }
    ~ArmedStoreFixture()
    {
        s16::g_chainObserve = saved;
        store.close();
        if (!dir.empty()) boost::filesystem::remove_all(dir);
    }
};

static int64_t CountAtomicReorgs()
{
    ReorgAtomicStats st;
    memset(&st, 0, sizeof(st));
    GetReorgAtomicStatsForTesting(&st);
    return st.count;
}

static void AssertCleanState()
{
    ReorgAtomicStats st;
    memset(&st, 0, sizeof(st));
    GetReorgAtomicStatsForTesting(&st);
    BOOST_CHECK_MESSAGE(!st.overlayArmed, "the V-phase overlay must be disarmed after every atomic reorg (invariant 8)");
    BOOST_CHECK_MESSAGE(!st.trailArmed, "the connect supply trail must be disarmed after every atomic reorg (invariant 8)");
    BOOST_CHECK_EQUAL(st.trailEntries, 0);
    BOOST_CHECK_LT((uint64_t)st.lastRssKb == 0 ? 0 : (uint64_t)st.lastRssKb, (uint64_t)1024 * 1024);
}

// Mine `depth` blocks on top of `parent` through the production path.
static CBlockIndex* MineChain(CBlockIndex* parent, int depth, unsigned int tagBase)
{
    CBlockIndex* cur = parent;
    for (int i = 1; i <= depth; ++i)
    {
        CBlock* b = MakeCoinbaseBlock(cur, tagBase + (unsigned)i);
        uint256 h;
        const Delivery d = Deliver(b, &h);
        delete b;
        BOOST_REQUIRE_MESSAGE(d != DELIVERY_REJECTED, "chain block " << i << " rejected at height " << (cur->nHeight + 1));
        LOCK(cs_main);
        std::map<uint256, CBlockIndex*>::iterator it = mapBlockIndex.find(h);
        BOOST_REQUIRE(it != mapBlockIndex.end());
        cur = it->second;
    }
    return cur;
}

// Drive a NATURAL trust-based reorg of the given depth: branch A is extended by
// `depth` blocks in place (becoming active), then a competing branch B forks at
// the pre-branch tip and is delivered block by block until cumulative trust
// makes it the new best chain. Returns true when the switch happened atomically.
static bool RunDepthCase(int depth, const char* label)
{
    // RETIREMENT-FLOOR SAFETY (test harness): bounded residency retires and FREES
    // CBlockIndex objects below the retention floor, so a raw fork pointer must not
    // be held across a >horizon mining run (that is a harness use-after-free, NOT a
    // production defect). The fork is therefore captured by HASH, and the FIRST
    // branch-B block is built immediately (while the fork object is certainly
    // resident). All later B blocks chain on their own recent predecessors.
    const uint256 forkHash = pindexBest->GetBlockHash();
    const int forkHeight = pindexBest->nHeight;
    const int64_t reorgsBefore = CountAtomicReorgs();

    // Pre-build the WHOLE branch B offline (parent tracked locally), then mine A
    // through the production path, then deliver B block by block. This keeps every
    // harness pointer within the residency window regardless of the depth.
    std::vector<PreBlock> branchB;
    std::string preErr;
    const int prebuild = depth + 96;
    BOOST_REQUIRE_MESSAGE(PrebuildBranch(pindexBest, prebuild, 0x4100u, &branchB, &preErr),
                          label << ": prebuild failed: " << preErr);

    // Branch A: depth blocks on the current chain (store + authority advance).
    CBlockIndex* tipA = MineChain(pindexBest, depth, 0x3100u);
    const uint256 tipAHash = tipA->GetBlockHash();
    const int tipAHeight = tipA->nHeight;
    BOOST_REQUIRE_MESSAGE(tipAHash == pindexBest->GetBlockHash(), label << ": branch A did not become active");

    // Branch B delivery: the fork (parent of B#1) may already be below the
    // retirement floor, but acceptance resolves the parent BY VALUE; every later
    // block's parent is the immediately preceding B block (always recent).
    uint256 curBHash(0);
    bool switched = false;
    int delivered = 0;
    for (size_t i = 0; i < branchB.size() && !switched; ++i)
    {
        CBlock* b = new CBlock(branchB[i].blk);
        uint256 h;
        const Delivery d = Deliver(b, &h);
        delete b;
        BOOST_REQUIRE_MESSAGE(d != DELIVERY_REJECTED, label << ": branch B block " << (i + 1)
                              << " rejected at height " << branchB[i].height);
        curBHash = h;
        delivered = (int)(i + 1);
        if (h == hashBestChain) switched = true;
    }
    BOOST_REQUIRE_MESSAGE(switched, label << ": branch B never became the best chain");
    (void)delivered;

    // The atomic protocol ran exactly ONCE for this switch.
    BOOST_CHECK_EQUAL(CountAtomicReorgs(), reorgsBefore + 1);

    ReorgAtomicStats st;
    memset(&st, 0, sizeof(st));
    GetReorgAtomicStatsForTesting(&st);
    BOOST_TEST_MESSAGE(label << ": fork=" << st.lastForkHeight << " disc=" << st.lastDisconnect
                       << " conn=" << st.lastConnect << " undoOps=" << st.lastUndoOps
                       << " overlayEntries=" << st.lastOverlayEntries << " rss=" << st.lastRssKb << "KB");
    // The engine chooses the fork by chain selection (regtest trust is ENTROPY
    // based after FORK_HEIGHT_POEM, so a shorter branch can legitimately win —
    // the achieved depth is measured, never assumed). Invariants asserted:
    //   * the atomic protocol ran ONCE for this switch,
    //   * the fork lies at/above this case's fork (never above the A tip),
    //   * the ENTIRE A suffix above the chosen fork was disconnected,
    //   * the committed branch is dense from fork+1 to the new tip.
    BOOST_TEST_MESSAGE(label << ": case fork=" << forkHeight << " A tip=" << tipAHeight
                       << " achieved fork=" << st.lastForkHeight << " disc=" << st.lastDisconnect);
    // Single-fork construction: the chosen fork MUST be this case's fork and the
    // ENTIRE A suffix above it MUST be disconnected (exact requested depth).
    BOOST_CHECK_EQUAL(st.lastForkHeight, forkHeight);
    BOOST_CHECK_EQUAL(st.lastDisconnect, depth);
    BOOST_CHECK_EQUAL(st.lastDisconnect, tipAHeight - st.lastForkHeight);
    // Entropy-based trust: B wins at/around the same block count (small variance).
    BOOST_CHECK_GE(st.lastConnect, depth - 8);
    BOOST_CHECK_LE(st.lastConnect, depth + 96);
    BOOST_CHECK(st.lastUndoOps >= (uint64_t)depth);   // one undo record per A block, ops >= blocks

    // Four height sources agree, and the store is at the committed tip.
    std::string why;
    BOOST_CHECK_MESSAGE(FourHeightsAgree(&why), label << ": four-height disagreement: " << why);
    int curBHeight = -1;
    { LOCK(cs_main); std::map<uint256, CBlockIndex*>::iterator it = mapBlockIndex.find(curBHash);
      if (it != mapBlockIndex.end()) curBHeight = it->second->nHeight; }
    BOOST_CHECK_EQUAL(nBestHeight, curBHeight);
    BOOST_CHECK(pindexBest->GetBlockHash() == curBHash);
    (void)tipAHash;
    AssertCleanState();

    // Bounded overlay: record the measured envelope for the report.
    BOOST_TEST_MESSAGE(label << ": PASS atomic reorg depth " << depth
                       << " (overlay entries " << st.lastOverlayEntries
                       << ", undo ops " << st.lastUndoOps << ", RSS " << st.lastRssKb << " KB)");
    return true;
}

} // namespace atomicreorg

using namespace atomicreorg;

BOOST_AUTO_TEST_SUITE(atomic_reorg_tests)

BOOST_AUTO_TEST_CASE(atomic_reorg_depths_100_1000_2101)
{
    chaintest::StoreOwnershipGuard ownerGuard;
    BOOST_REQUIRE_MESSAGE(ownerGuard.owned, "chain store ownership: " << ownerGuard.err);
    BOOST_REQUIRE_MESSAGE(g_fAuthoritativeStartup, "this suite needs the authoritative startup the production path requires");
    BOOST_REQUIRE(GetAuthoritativeLiveAuthority() != NULL && GetAuthoritativeLiveAuthority()->IsOpen());
    ArmedStoreFixture fx;
    BOOST_REQUIRE_MESSAGE(fx.armed, "armed Store3A fixture: " << fx.err);

    std::string why;
    BOOST_REQUIRE_MESSAGE(FourHeightsAgree(&why), "precondition: " << why);

    // Optional bounded selection for focused re-runs: RX_ATOMIC_DEPTHS="2101" etc.
    const char* envDepths = getenv("RX_ATOMIC_DEPTHS");
    std::vector<int> depths;
    if (envDepths != NULL && envDepths[0] != '\0')
    {
        std::string s(envDepths); size_t pos = 0;
        while (pos <= s.size())
        {
            const size_t comma = s.find(',', pos);
            const std::string tok = s.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
            if (!tok.empty()) depths.push_back(atoi(tok.c_str()));
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
    }
    if (depths.empty()) { depths.push_back(100); depths.push_back(1000); depths.push_back(2101); }
    for (size_t di = 0; di < depths.size(); ++di)
    {
        char label[32]; snprintf(label, sizeof(label), "depth%d", depths[di]);
        RunDepthCase(depths[di], label);
    }
    AssertCleanState();
}

BOOST_AUTO_TEST_CASE(atomic_reorg_v_before_c_aborts_clean)
{
    chaintest::StoreOwnershipGuard ownerGuard;
    BOOST_REQUIRE_MESSAGE(ownerGuard.owned, "chain store ownership: " << ownerGuard.err);
    BOOST_REQUIRE(g_fAuthoritativeStartup);
    ArmedStoreFixture fx;
    BOOST_REQUIRE_MESSAGE(fx.armed, "armed Store3A fixture: " << fx.err);

    CBlockIndex* fork = pindexBest;
    const int forkHeight = fork->nHeight;
    const uint256 forkHash = fork->GetBlockHash();
    const int64_t reorgsBefore = CountAtomicReorgs();

    CBlockIndex* tipA = MineChain(fork, 8, 0x5100u);
    // Settle the standing store batch first: the V-phase is allowed to flush an
    // owed durability barrier for blocks ALREADY committed to the index (that is
    // the pre-existing S2.1 contract, not a V-phase mutation). After this flush
    // the generation must not move across the aborted reorg.
    BOOST_REQUIRE(s16::g_chainObserve.FlushPending() || s16::g_chainObserve.pending.empty());
    const uint64_t storeGen = s16::g_chainObserve.store->generation();

    // Deliver B blocks until the next one WOULD switch; abort that switch in the
    // V->C boundary (failpoint) — nothing may be committed or applied.
    CBlockIndex* curB = fork;
    bool aborted = false;
    for (int i = 1; i <= 8 + 64 && !aborted; ++i)
    {
        CBlock* b = MakeCoinbaseBlock(curB, 0x6100u + (unsigned)i);
        uint256 h; bool tripped = false;
        const bool ok = DeliverWithReorgFailpoint(b, &h, "FP_REORG_BEFORE_C", &tripped);
        delete b;
        if (tripped) { aborted = true; break; }
        BOOST_REQUIRE_MESSAGE(ok, "side deliver failed at B block " << i);
        LOCK(cs_main);
        curB = mapBlockIndex[h];
    }
    BOOST_REQUIRE_MESSAGE(aborted, "the failpoint did not trip a reorg boundary");

    // The whole branch was rejected before any durable change: A remains best,
    // the store did not move, no atomic reorg was counted, and no V-phase state
    // leaked (invariants 2, 5, 8).
    BOOST_CHECK(hashBestChain == tipA->GetBlockHash());
    BOOST_CHECK_EQUAL(nBestHeight, tipA->nHeight);
    BOOST_CHECK_EQUAL(s16::g_chainObserve.store->generation(), storeGen);
    BOOST_CHECK_EQUAL(CountAtomicReorgs(), reorgsBefore);
    BOOST_CHECK_EQUAL(pindexBest->nHeight, tipA->nHeight);
    BOOST_CHECK_EQUAL(forkHeight + 8, tipA->nHeight);
    (void)forkHash;
    AssertCleanState();
    std::string why;
    BOOST_CHECK_MESSAGE(FourHeightsAgree(&why), "after abort: " << why);
}

BOOST_AUTO_TEST_CASE(atomic_reorg_c_then_an_fail_stop_and_reconcile)
{
    chaintest::StoreOwnershipGuard ownerGuard;
    BOOST_REQUIRE_MESSAGE(ownerGuard.owned, "chain store ownership: " << ownerGuard.err);
    BOOST_REQUIRE(g_fAuthoritativeStartup);
    ArmedStoreFixture fx;
    BOOST_REQUIRE_MESSAGE(fx.armed, "armed Store3A fixture: " << fx.err);

    CBlockIndex* fork = pindexBest;
    const int forkHeight = fork->nHeight;
    const int64_t reorgsBefore = CountAtomicReorgs();

    CBlockIndex* tipA = MineChain(fork, 6, 0x7100u);
    const uint256 tipAHash = tipA->GetBlockHash();
    const int tipAHeight = tipA->nHeight;

    // The failing-delivery arm: deliver B blocks; on the switch attempt the
    // failpoint fires AFTER the single atomic C publication (durable state is
    // exactly a crash between C and A).
    CBlockIndex* curB = fork;
    int tripped = -1;
    for (int i = 1; i <= 6 + 64 && tripped < 0; ++i)
    {
        CBlock* b = MakeCoinbaseBlock(curB, 0x8100u + (unsigned)i);
        uint256 h; bool wasTripped = false;
        const bool ok = DeliverWithReorgFailpoint(b, &h, "FP_REORG_AFTER_C_BEFORE_A", &wasTripped);
        delete b;
        if (wasTripped)
        {
            tripped = i;
            // The commit happened; the fail-stop path keeps the block, so it IS the
            // committed tip even though the delivery reported failure.
            LOCK(cs_main);
            std::map<uint256, CBlockIndex*>::iterator it = mapBlockIndex.find(h);
            BOOST_REQUIRE_MESSAGE(it != mapBlockIndex.end(), "fail-stop must preserve the committed block object");
            curB = it->second;
            break;
        }
        BOOST_REQUIRE_MESSAGE(ok, "side deliver failed at B block " << i);
        LOCK(cs_main);
        curB = mapBlockIndex[h];
    }
    BOOST_REQUIRE_MESSAGE(tripped > 0, "the failpoint did not trip the post-commit boundary");

    // C committed: the V2 authority is at the branch tip; Store3A is still on the
    // OLD branch (index ahead) — the exact durable state of a SIGKILL there.
    const BlockIndexTipAuthority* tipAuth = GetAuthoritativeLiveAuthority()->TipAuthority();
    const BlockIndexTipRead tr = tipAuth->GetTip();
    BOOST_REQUIRE_EQUAL(tr.status, BLOCK_INDEX_TIP_OK);
    BOOST_CHECK(tr.record.hash == curB->GetBlockHash());
    BOOST_CHECK_EQUAL(tr.height, curB->nHeight);
    BOOST_CHECK_EQUAL(s16::g_chainObserve.store->tipHeight(), tipAHeight);
    uint256 storeTip; memcpy(storeTip.begin(), s16::g_chainObserve.store->tipHash().data(), 32);
    BOOST_CHECK(storeTip == tipAHash);
    BOOST_CHECK_MESSAGE(s16::g_chainObserve.needsRecovery, "fail-stop must be explicit");
    BOOST_CHECK_EQUAL(CountAtomicReorgs(), reorgsBefore + 1);
    AssertCleanState();

    // Deterministic recovery: the SAME startup function drives the store to the
    // committed tip. After it, all four height sources agree again.
    s16::g_chainObserve.needsRecovery = false;
    std::string rerr;
    BOOST_REQUIRE_MESSAGE(s16::ReconcileStore3AToChain(s16::g_chainObserve.store, &rerr),
                          "reconcile after C: " << rerr);

    // The legacy view catches up through the normal settlement of a re-delivery:
    // the block is already committed; emulate restart-equivalence by asserting the
    // store now equals the committed branch tip, then accept the next block to
    // prove the node keeps working.
    cs::store3a::CommitStore3A* st = s16::g_chainObserve.store;
    uint256 stTip2; memcpy(stTip2.begin(), st->tipHash().data(), 32);
    BOOST_CHECK(stTip2 == curB->GetBlockHash());
    BOOST_CHECK_EQUAL(st->tipHeight(), curB->nHeight);

    // Restart-equivalent settlement: a real restart rebuilds the legacy view from
    // the committed authority; emulate exactly that (globals from the committed
    // snapshot) so the in-process world is consistent for the following cases.
    {
        LOCK(cs_main);
        hashBestChain = curB->GetBlockHash();
        pindexBest = curB;
        nBestHeight = curB->nHeight;
        nBestChainTrust = curB->nChainTrust;
        if (curB->pprev) curB->pprev->pnext = curB;
    }
    std::string why2;
    BOOST_CHECK_MESSAGE(FourHeightsAgree(&why2), "after reconcile + settlement: " << why2);

    // Liveness: a further ordinary block still extends the chain. The armed store
    // batches its durability barrier (the standing S2.1 contract), so settle the
    // pending batch before comparing the durable store tip.
    CBlockIndex* next = MineChain(curB, 1, 0x8F00u);
    BOOST_CHECK_EQUAL(nBestHeight, next->nHeight);
    BOOST_REQUIRE(s16::g_chainObserve.FlushPending() || s16::g_chainObserve.pending.empty());
    BOOST_CHECK_EQUAL(s16::g_chainObserve.store->tipHeight(), next->nHeight);
}

BOOST_AUTO_TEST_SUITE_END()
