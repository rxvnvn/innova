// S1 ACCEPTANCE — REAL SIGKILL AT THE ATOMIC C->A BOUNDARY (test-only).
//
// Two-phase harness (deliberately out-of-process; NOT another in-process failpoint):
//
//   phase 1  (child): boots on a FIXED datadir (RX_TEST_DATADIR), arms the real
//            V2 authority + Store3A, mines branch A, pre-builds competing branch
//            B, then delivers B so that the atomic reorg commits the V2 tip.
//            With RX_REORG_HOLD_AFTER_C_MS set the production code prints
//            REORG-HOLD-AFTER-C and blocks BEFORE the Store3A apply. The external
//            supervisor SIGKILLs this process there: V2 tip committed on disk,
//            Store3A still on the old branch.
//
//   phase 2  (fresh process): boots on the SAME datadir (production restart
//            path), asserts the committed V2 tip survived, opens Store3A (still
//            on the old branch -> proves the crash window), runs the production
//            ReconcileStore3AToChain, then checks:
//              * Store3A tip == V2 tip == nBestHeight/hashBestChain == manager tip
//              * supply consistency (no double accrual)
//              * reconcile idempotence (no duplicate records / re-application)
//              * a further block can be accepted after recovery
//
// Both cases refuse to run without RX_SIGKILL_WORKDIR, so the default suite is
// unaffected. Nothing here changes consensus code paths.
#include <boost/test/unit_test.hpp>

#include "main.h"
#include "chainstore_context.h"
#include "miner.h"
#include "wallet.h"
#include "util.h"
#include "blockindex_authoritative_live.h"
#include "blockindex_manager.h"
#include "chainstate/observe_hook.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <sys/stat.h>

extern CWallet* pwalletMain;

namespace atomicsigkill {

static std::string WorkDir() { const char* d = getenv("RX_SIGKILL_WORKDIR"); return d ? d : ""; }
static std::string StoreDir() { return WorkDir() + "/store"; }
static std::string ManifestPath() { return WorkDir() + "/manifest.txt"; }

static bool Mkdir(const std::string& d)
{
    struct stat st;
    if (stat(d.c_str(), &st) == 0) return true;
    return mkdir(d.c_str(), 0700) == 0;
}

static bool WriteManifest(const std::string& body)
{
    std::ofstream f(ManifestPath().c_str(), std::ios::out | std::ios::trunc);
    if (!f) return false;
    f << body;
    f.flush();
    return (bool)f;
}

static std::string ReadManifestKey(const std::string& key)
{
    std::ifstream f(ManifestPath().c_str());
    std::string line;
    while (std::getline(f, line))
    {
        const std::string prefix = key + "=";
        if (line.compare(0, prefix.size(), prefix) == 0)
            return line.substr(prefix.size());
    }
    return "";
}

// ---------------------------------------------------------------- block build
static CBlock* MakeCoinbaseBlock(CBlockIndex* pindexPrev, unsigned int nTag)
{
    CBlock* pblock = CreateNewBlock(pwalletMain, false, NULL, NULL);
    if (pblock == NULL) return NULL;
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

static bool MineBlock(CBlockIndex* pindexPrev, unsigned int nTag, CBlockIndex** out)
{
    CBlock* b = MakeCoinbaseBlock(pindexPrev, nTag);
    if (b == NULL) return false;
    bool ok = false;
    {
        LOCK(cs_main);
        const uint256 h = b->GetHash();
        ok = b->CheckBlock(true, true, true) && ProcessBlock(NULL, b);
        if (ok) *out = mapBlockIndex[h];
    }
    delete b;
    return ok && *out != NULL;
}

static bool MineChain(CBlockIndex* parent, int depth, unsigned int tagBase, CBlockIndex** out)
{
    CBlockIndex* p = parent;
    for (int i = 1; i <= depth; ++i)
    {
        CBlockIndex* n = NULL;
        if (!MineBlock(p, tagBase + (unsigned)i, &n)) return false;
        p = n;
    }
    *out = p;
    return true;
}

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
    for (int i = 1; i <= count; ++i)
    {
        CBlock b = tmpl;
        b.nVersion = 1;
        const int64_t nTime = prevTime + 1;
        b.nTime = (unsigned int)nTime;
        b.hashPrevBlock = prevHash;
        b.vtx[0].nTime = b.nTime;
        b.vtx[0].vin[0].scriptSig = CScript() << (prevHeight + 1) << (tagBase + (unsigned)i);
        b.hashMerkleRoot = b.BuildMerkleTree();
        const uint256 hashTarget = CBigNum().SetCompact(b.nBits).getuint256();
        while (b.GetHash() > hashTarget && b.nNonce < 0xffffffff)
            ++b.nNonce;
        PreBlock pb;
        pb.blk = b; pb.hash = b.GetHash(); pb.height = prevHeight + 1; pb.time = nTime;
        out->push_back(pb);
        prevHash = pb.hash; prevTime = nTime; prevHeight += 1;
    }
    return true;
}

// Production acceptance path; side blocks fall back to the same storage path the
// network uses (WriteToDisk + AcceptBlock).
static bool Deliver(CBlock* pblock)
{
    {
        LOCK(cs_main);
        if (pblock->CheckBlock(true, true, true) && ProcessBlock(NULL, pblock))
            return true;
    }
    unsigned int nFile = 0, nBlockPos = 0;
    {
        LOCK(cs_main);
        if (pblock->WriteToDisk(nFile, nBlockPos) && pblock->AcceptBlock())
            return true;
    }
    return false;
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

static const BlockIndexTipAuthority* TipAuth()
{
    return GetAuthoritativeLiveAuthority() ? GetAuthoritativeLiveAuthority()->TipAuthority() : NULL;
}

// ------------------------------------------------------------------ fixtures
// Arms the REAL in-process V2 authority pair exactly as the production startup
// does, on a FIXED store dir so a second process can reopen it.
struct ArmedPersistentStore
{
    cs::store3a::CommitStore3A store;
    s16::ChainObserveState saved;
    bool armed;
    std::string err;

    ArmedPersistentStore() : armed(false)
    {
        if (!Mkdir(WorkDir())) { err = "workdir create failed: " + WorkDir(); return; }
        Mkdir(StoreDir());
        cs::ChainError oe;
        if (!store.open(StoreDir(), &oe)) { err = "store open: " + oe.str(); return; }
        if (store.tipHeight() < 0)
        {
            if (pindexGenesisBlock == NULL) { err = "no genesis"; return; }
            cs::Delta birth;
            cs::Hash gh; memcpy(gh.data(), pindexGenesisBlock->GetBlockHash().begin(), 32);
            cs::ChainError be;
            if (!store.publish(birth, gh, 0, &be)) { err = "genesis born-commit: " + be.str(); return; }
        }
        std::string recErr;
        const bool rok = s16::ReconcileStore3AToChain(&store, &recErr);
        if (!rok) { err = recErr; }
        if (!rok) return;

        saved = s16::g_chainObserve;
        s16::g_chainObserve.store = &store;
        s16::g_chainObserve.enabled = true;
        s16::g_chainObserve.checkAuthority = true;
        s16::g_chainObserve.hardFail = true;
        s16::g_chainObserve.batchSize = 1000;
        s16::g_chainObserve.batchChunk = 0;
        s16::g_chainObserve.pending.clear();
        s16::g_chainObserve.needsRecovery = false;
        armed = true;
    }
    ~ArmedPersistentStore()
    {
        if (armed) s16::g_chainObserve = saved;
        if (store.opened()) store.close();
    }
};

static bool FourSourcesAgree(const char* when, std::string* why)
{
    const BlockIndexTipAuthority* ta = TipAuth();
    if (ta == NULL) { *why = std::string(when) + ": no tip authority"; return false; }
    int mgrH = -1; std::string merr;
    if (GetBlockIndexManager().ActiveTipHeight(&mgrH, &merr) != BLOCK_INDEX_MANAGER_OK)
    { *why = std::string(when) + ": manager tip unavailable: " + merr; return false; }
    const uint256 th = ta->TipHash();
    const bool haveLogical = (th != uint256(0));
    const int authH = haveLogical ? ta->TipHeight() : AuthoritativeBaseTipHeight();
    const uint256 authHash = haveLogical ? th : ta->BaseTipHash();
    cs::store3a::CommitStore3A* st = s16::g_chainObserve.store;
    if (st == NULL || !st->opened()) { *why = std::string(when) + ": store not armed"; return false; }
    if (!s16::g_chainObserve.pending.empty() && !s16::g_chainObserve.FlushPending())
    { *why = std::string(when) + ": barrier flush: " + s16::g_chainObserve.lastOpError; return false; }
    uint256 stHash; memcpy(stHash.begin(), st->tipHash().data(), 32);
    if (mgrH != authH) { *why = strprintf("%s: manager=%d authority=%d", when, mgrH, authH); return false; }
    if (authH != nBestHeight) { *why = strprintf("%s: authority=%d nBestHeight=%d", when, authH, nBestHeight); return false; }
    if (authHash != hashBestChain) { *why = strprintf("%s: authority hash != hashBestChain", when); return false; }
    if (st->tipHeight() != nBestHeight) { *why = strprintf("%s: store=%d nBestHeight=%d", when, (int)st->tipHeight(), nBestHeight); return false; }
    if (stHash != hashBestChain) { *why = std::string(when) + ": store tip hash != hashBestChain"; return false; }
    return true;
}

BOOST_AUTO_TEST_SUITE(atomic_sigkill_tests)

// ------------------------------------------------------------------- phase 1
BOOST_AUTO_TEST_CASE(sigkill_phase1_commit_then_die)
{
    chaintest::StoreOwnershipGuard ownerGuard;
    BOOST_REQUIRE_MESSAGE(ownerGuard.owned, "chain store ownership: " << ownerGuard.err);
    const std::string wd = WorkDir();
    if (wd.empty())
    {
        BOOST_TEST_MESSAGE("sigkill_phase1: RX_SIGKILL_WORKDIR not set — skipped");
        return;
    }
    const char* holdEnv = getenv("RX_REORG_HOLD_AFTER_C_MS");
    BOOST_REQUIRE_MESSAGE(holdEnv != NULL && atol(holdEnv) > 0,
        "phase1 must run with RX_REORG_HOLD_AFTER_C_MS so the supervisor can SIGKILL at C->A");

    BOOST_REQUIRE_MESSAGE(g_fAuthoritativeStartup,
        "phase1 requires the authoritative startup mode (fixture boots it)");
    ArmedPersistentStore arm;
    BOOST_REQUIRE_MESSAGE(arm.armed, "armed persistent store: " << arm.err);

    // Branch A: 12 blocks on top of genesis through the production path.
    CBlockIndex* aTip = NULL;
    BOOST_REQUIRE(MineChain(pindexBest, 12, 0x5000, &aTip));
    BOOST_REQUIRE_EQUAL(aTip->nHeight, 12);
    if (!s16::g_chainObserve.pending.empty())
        BOOST_REQUIRE(s16::g_chainObserve.FlushPending());
    BOOST_REQUIRE_EQUAL(s16::g_chainObserve.store->tipHeight(), aTip->nHeight);

    // Branch B: 14 pre-built blocks off genesis (a strict superset of A's depth).
    std::vector<PreBlock> branch;
    std::string berr;
    BOOST_REQUIRE_MESSAGE(PrebuildBranch(pindexGenesisBlock, 14, 0x6000, &branch, &berr), berr);

    // Durable manifest for the restart process: what state MUST survive the kill.
    std::ostringstream m;
    m << "fork_height=0\n"
      << "fork_hash=" << pindexGenesisBlock->GetBlockHash().ToString() << "\n"
      << "a_tip_height=" << aTip->nHeight << "\n"
      << "a_tip_hash=" << aTip->GetBlockHash().ToString() << "\n"
      << "branch_tip_height=" << branch.back().height << "\n"
      << "branch_tip_hash=" << branch.back().hash.ToString() << "\n"
      << "store_height_before_recovery=" << s16::g_chainObserve.store->tipHeight() << "\n";
    BOOST_REQUIRE_MESSAGE(WriteManifest(m.str()), "manifest write failed");

    fprintf(stderr, "SIGKILL-PHASE1 ready: a_tip=%d/%s branch_tip=%d/%s store=%d rss=%lluKB\n",
           aTip->nHeight, aTip->GetBlockHash().ToString().substr(0,16).c_str(),
           branch.back().height, branch.back().hash.ToString().substr(0,16).c_str(),
           (int)s16::g_chainObserve.store->tipHeight(), (unsigned long long)RssKb());
    fflush(stdout);

    // Deliver B until the winning block enters the atomic reorg. The production
    // code will print REORG-HOLD-AFTER-C and block; the supervisor kills us there.
    for (size_t i = 0; i < branch.size(); ++i)
    {
        const bool ok = Deliver(&branch[i].blk);
        fprintf(stderr, "SIGKILL-PHASE1 deliver h=%d ok=%d best=%d/%s trust=%s\n",
                branch[i].height, ok ? 1 : 0, nBestHeight,
                hashBestChain.ToString().substr(0,12).c_str(),
                nBestChainTrust.ToString().c_str());
        BOOST_REQUIRE_MESSAGE(ok, "branch B block " << branch[i].height << " rejected");
    }
    // If control returns here the supervisor did NOT kill us: make that explicit
    // rather than letting an "uninterrupted run" masquerade as a crash test.
    if (!s16::g_chainObserve.pending.empty())
        s16::g_chainObserve.FlushPending();
    ReorgAtomicStats rst; memset(&rst, 0, sizeof(rst));
    GetReorgAtomicStatsForTesting(&rst);
    fprintf(stderr, "SIGKILL-PHASE1 no-hold: store_h=%d best_h=%d best_trust=%s atomicCount=%lld\n",
            (int)s16::g_chainObserve.store->tipHeight(), nBestHeight,
            nBestChainTrust.ToString().c_str(), (long long)rst.count);
    BOOST_FAIL("phase1 completed WITHOUT SIGKILL — the supervisor must kill the process at REORG-HOLD-AFTER-C");
}

// ------------------------------------------------------------------- phase 2
BOOST_AUTO_TEST_CASE(sigkill_phase2_restart_and_recover)
{
    chaintest::StoreOwnershipGuard ownerGuard;
    BOOST_REQUIRE_MESSAGE(ownerGuard.owned, "chain store ownership: " << ownerGuard.err);
    const std::string wd = WorkDir();
    if (wd.empty())
    {
        BOOST_TEST_MESSAGE("sigkill_phase2: RX_SIGKILL_WORKDIR not set — skipped");
        return;
    }
    const std::string aHash = ReadManifestKey("a_tip_hash");
    const int aH = atoi(ReadManifestKey("a_tip_height").c_str());
    // Exact durable commit point, written by the production code inside the hold
    // window (i.e. AFTER the durable ReorgActiveTo, BEFORE any Store3A apply).
    const std::string markerPath = WorkDir() + "/commit-marker.txt";
    std::ifstream sm(markerPath.c_str());
    std::string line, bHash;
    int bH = -1;
    while (std::getline(sm, line))
    {
        if (line.compare(0, 14, "commit_height=") == 0) bH = atoi(line.substr(14).c_str());
        if (line.compare(0, 12, "commit_hash=") == 0) bHash = line.substr(12);
    }
    BOOST_REQUIRE_MESSAGE(!aHash.empty(), "manifest unreadable: " + ManifestPath());
    // A branch replacement can land at the SAME height as the old tip (higher
    // trust at equal depth), so require >= and a DIFFERENT hash: that is exactly
    // the durable C evidence we need.
    BOOST_REQUIRE_MESSAGE(bH >= aH && !bHash.empty() && bHash != aHash,
        "commit marker unreadable (no durable C evidence): " + markerPath);

    // 1) The committed V2 tip MUST survive the SIGKILL (production restart path
    //    already ran in TestingSetup: LoadBlockIndex opened the persisted V2).
    const BlockIndexTipAuthority* ta = TipAuth();
    BOOST_REQUIRE(ta != NULL);
    BOOST_TEST_MESSAGE("after restart: authority tip h=" << ta->TipHeight() << " hash=" << ta->TipHash().ToString().substr(0,16));
    BOOST_REQUIRE_EQUAL(ta->TipHeight(), bH);
    BOOST_CHECK_EQUAL(ta->TipHash().ToString(), bHash);

    // 2) Open Store3A from the crashed directory: it MUST still be on the OLD
    //    branch — that proves the kill landed between C and A (no partial apply).
    cs::store3a::CommitStore3A store;
    cs::ChainError oe;
    BOOST_REQUIRE_MESSAGE(store.open(StoreDir(), &oe), "store open: " << oe.str());
    fprintf(stderr, "SIGKILL-PHASE2 crash window: store_h=%d (expected %d) chain_h=%d (expected %d)\n",
           (int)store.tipHeight(), aH, nBestHeight, bH);
    BOOST_REQUIRE_EQUAL((int)store.tipHeight(), aH);
    {
        uint256 stHash; memcpy(stHash.begin(), store.tipHash().data(), 32);
        BOOST_CHECK_EQUAL(stHash.ToString(), aHash);
    }

    // 3) Production recovery: reconcile Store3A to the committed V2 chain.
    s16::ChainObserveState saved = s16::g_chainObserve;
    s16::g_chainObserve.store = &store;
    s16::g_chainObserve.enabled = true;
    s16::g_chainObserve.checkAuthority = true;
    s16::g_chainObserve.hardFail = true;
    s16::g_chainObserve.batchSize = 1000;
    s16::g_chainObserve.pending.clear();
    std::string rerr;
    const uint64_t pubBefore = s16::g_chainObserve.published;
    const uint64_t blkBefore = s16::g_chainObserve.blocks;
    const bool rok = s16::ReconcileStore3AToChain(&store, &rerr);
    BOOST_REQUIRE_MESSAGE(rok, "reconcile after SIGKILL failed: " << rerr);
    const uint64_t pubAfter = s16::g_chainObserve.published;
    const uint64_t blkAfter = s16::g_chainObserve.blocks;
    fprintf(stderr, "SIGKILL-PHASE2 replay: published %llu->%llu blocks %llu->%llu\n",
            (unsigned long long)pubBefore, (unsigned long long)pubAfter,
            (unsigned long long)blkBefore, (unsigned long long)blkAfter);
    // NOTE: the reconcile path does not increment the observe-hook counters, so
    // those are informational only; the real proof of application is the store
    // tip advancing below plus the presence of the committed tip's coinbase
    // output in Store3A (checked after recovery).
    (void)pubAfter; (void)blkAfter;
    if (!s16::g_chainObserve.pending.empty())
        BOOST_REQUIRE(s16::g_chainObserve.FlushPending());

    // 4) Four independent heights + supply consistency (no double accrual).
    std::string why;
    BOOST_REQUIRE_MESSAGE(FourSourcesAgree("after recovery", &why), why);
    BOOST_CHECK_EQUAL((int)store.tipHeight(), bH);
    {
        uint256 stHash; memcpy(stHash.begin(), store.tipHash().data(), 32);
        BOOST_CHECK_EQUAL(stHash.ToString(), bHash);
    }
    // TX-LOC/by-value replay proof: the committed tip block's coinbase output
    // MUST exist in Store3A after recovery. This can only be true if the block
    // was read back by value and applied to the store.
    {
        CBlock tipBlk;
        BOOST_REQUIRE_MESSAGE(tipBlk.ReadFromDisk(pindexBest), "cannot read the recovered tip block from disk");
        cs::OutPoint ck;
        memcpy(ck.txid.data(), tipBlk.vtx[0].GetHash().begin(), 32);
        ck.n = 0;
        cs::Coin c;
        bool found = false;
        cs::ChainError ce;
        BOOST_REQUIRE_MESSAGE(store.get(ck, &c, &found, &ce), "store.get failed: " << ce.str());
        BOOST_CHECK_MESSAGE(found, "the committed tip block's coinbase MUST be in Store3A after recovery");
        if (found)
        {
            BOOST_CHECK_EQUAL(c.height, bH);
            BOOST_CHECK_MESSAGE(c.fCoinBase, "the tip coinbase coin must carry the coinbase flag");
        }
    }

    BlockIndexSnapshot snap; std::string serr;
    BOOST_REQUIRE_EQUAL((int)GetAuthoritativeLiveAuthority()->ResolveBlockSnapshot(hashBestChain, &snap, &serr),
                        (int)BlockIndexHotStatus::OK);
    BOOST_CHECK_EQUAL(snap.height, bH);
    BOOST_CHECK_EQUAL(snap.hash.ToString(), bHash);
    // Supply: the authoritative tip snapshot and the legacy derived view must
    // agree exactly — a duplicated apply would show up as an inflated supply.
    BOOST_CHECK_EQUAL(snap.nMoneySupply, pindexBest->nMoneySupply);
    fprintf(stderr, "SIGKILL-PHASE2 recovered: tip=%d/%s supply=%lld store_h=%d\n",
           (int)store.tipHeight(), bHash.substr(0,16).c_str(),
           (long long)snap.nMoneySupply, (int)store.tipHeight());

    // 5) Idempotence: a second reconcile must be a NO-OP (no duplicate records,
    //    no re-application).
    const int h2 = (int)store.tipHeight();
    std::string rerr2;
    BOOST_REQUIRE_MESSAGE(s16::ReconcileStore3AToChain(&store, &rerr2),
                          "second reconcile failed: " << rerr2);
    if (!s16::g_chainObserve.pending.empty())
        BOOST_REQUIRE(s16::g_chainObserve.FlushPending());
    BOOST_CHECK_EQUAL((int)store.tipHeight(), h2);
    {
        uint256 stHash; memcpy(stHash.begin(), store.tipHash().data(), 32);
        BOOST_CHECK_EQUAL(stHash.ToString(), bHash);
    }

    // 6) The recovered node must be able to ACCEPT the next block.
    CBlockIndex* next = NULL;
    const bool mined = MineBlock(pindexBest, 0x7777, &next);
    BOOST_REQUIRE_MESSAGE(mined, "accepting the next block after recovery failed");
    BOOST_REQUIRE_EQUAL(next->nHeight, bH + 1);
    if (!s16::g_chainObserve.pending.empty())
        BOOST_REQUIRE(s16::g_chainObserve.FlushPending());
    BOOST_REQUIRE_MESSAGE(FourSourcesAgree("after next block", &why), why);
    BOOST_CHECK_EQUAL((int)store.tipHeight(), bH + 1);
    s16::g_chainObserve = saved;
    store.close();
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace atomicsigkill
