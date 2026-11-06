// ATOMIC-REORG (S1-e): shared production-path test fixture.
//
// The atomic reorg protocol (ReorganizeAtomic) requires the PRODUCTION authority
// pair: the authoritative BI-V2 startup (installed by the global TestingSetup
// fixture) AND an armed Store3A that agrees with the committed active tip. This
// header arms exactly that pair for the duration of one test case and restores
// the previous global observe state afterwards, so non-reorg behavior of other
// suites in the same process is untouched.
//
// Blocks are delivered through the production acceptance path (ProcessBlock,
// with the same WriteToDisk+AcceptBlock fallback reorg_314 uses).
#pragma once

#include "main.h"
#include "miner.h"
#include "wallet.h"
#include "util.h"
#include "serialize.h"
#include "blockindex_tip.h"
#include "blockindex_manager.h"
#include "blockindex_authoritative_live.h"
#include "chainstate/store3a/incstore.h"
#include "chainstate/reorg_overlay.h"
#include "chainstate/observe_hook.h"

#include <boost/test/unit_test.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <string>
#include <vector>

extern CWallet* pwalletMain;

namespace atomictest {

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

// Four independent height sources must agree:
//   1 getblockcount (authoritative committed tip via GetBlockIndexManager)
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
    { if (why) *why = "manager tip unavailable: " + merr; return false; }
    const uint256 authHashT = tipAuth->TipHash();
    const bool haveLogical = (authHashT != uint256(0));
    const int authHeight = haveLogical ? tipAuth->TipHeight() : AuthoritativeBaseTipHeight();
    const uint256 authHash = haveLogical ? authHashT : tipAuth->BaseTipHash();
    cs::store3a::CommitStore3A* st = s16::g_chainObserve.store;
    if (st == NULL || !st->opened()) { if (why) *why = "store not armed"; return false; }
    if (!s16::g_chainObserve.pending.empty() && !s16::g_chainObserve.FlushPending())
    { if (why) *why = "store barrier flush failed: " + s16::g_chainObserve.lastOpError; return false; }
    uint256 stHash; memcpy(stHash.begin(), st->tipHash().data(), 32);
    if (mgrHeight != authHeight) { if (why) *why = strprintf("manager=%d authority=%d", mgrHeight, authHeight); return false; }
    if (authHeight != nBestHeight) { if (why) *why = strprintf("authority=%d nBestHeight=%d", authHeight, nBestHeight); return false; }
    if (authHash != hashBestChain) { if (why) *why = strprintf("authority hash=%s != hashBestChain=%s", authHash.ToString().substr(0,16).c_str(), hashBestChain.ToString().substr(0,16).c_str()); return false; }
    if (st->tipHeight() != nBestHeight) { if (why) *why = strprintf("store=%d nBestHeight=%d", st->tipHeight(), nBestHeight); return false; }
    if (stHash != hashBestChain) { if (why) *why = "store tip hash != hashBestChain"; return false; }
    return true;
}

// Arms a scratch Store3A for the CURRENT active tip (production reconciliation),
// with hardFail authoritative semantics and the standing 1000-block batch size.
struct ArmedStoreFixture
{
    cs::store3a::CommitStore3A store;
    s16::ChainObserveState saved;
    boost::filesystem::path dir;
    bool armed;
    std::string err;

    ArmedStoreFixture() : armed(false)
    {
        namespace bfs = boost::filesystem;
        dir = bfs::temp_directory_path() / bfs::unique_path("atomic-armed-%%%%-%%%%");
        bfs::create_directories(dir);
        cs::ChainError oe;
        if (!store.open(dir.string(), &oe)) { err = oe.str(); return; }
        // Born-commit at genesis BEFORE reconcile when the store is empty (the
        // native bootstrap publishes this same representation: the store tip
        // identity is the genesis block even though genesis is not a UTXO
        // delta). Without it the reconcile's final check sees store h=-1 vs
        // chain h=0 and fails closed.
        if (store.tipHeight() < 0)
        {
            if (pindexGenesisBlock == NULL) { err = "no genesis block loaded"; store.close(); return; }
            cs::Delta birth;
            cs::Hash gh; memcpy(gh.data(), pindexGenesisBlock->GetBlockHash().begin(), 32);
            cs::ChainError be;
            if (!store.publish(birth, gh, 0, &be)) { err = "genesis born-commit: " + be.str(); store.close(); return; }
        }
        // Reconcile through the production startup recovery. The in-process
        // harness has no published committed-tip snapshot, so reconcile through
        // its chain-projection arm (identical replay code; only the chain-tip
        // source differs), then restore the authoritative flag.
        {
            const bool savedAuth = g_fAuthoritativeStartup;
            g_fAuthoritativeStartup = false;
            const bool rok = s16::ReconcileStore3AToChain(&store, &err);
            g_fAuthoritativeStartup = savedAuth;
            if (!rok)
            {
                LOCK(cs_main);
                fprintf(stderr, "ARMDBG: reconcile failed: %s | chainH=%d chainHash=%s genesisInMap=%d storeTipH=%d storeTipHash=%s\n",
                        err.c_str(),
                        pindexBest ? (int)pindexBest->nHeight : -2,
                        pindexBest ? pindexBest->GetBlockHash().ToString().substr(0,16).c_str() : "NULL",
                        pindexGenesisBlock ? (int)mapBlockIndex.count(pindexGenesisBlock->GetBlockHash()) : -1,
                        (int)store.tipHeight(),
                        store.tipHash().data()[0] ? "nonzero" : "zero");
                store.close();
                return;
            }
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
        // Restart-equivalence: the atomic protocol compares the committed
        // authority, Store3A and the legacy base view, so derive the legacy
        // globals from the committed authority exactly as a restart would.
        {
            const BlockIndexTipAuthority* ta =
                GetAuthoritativeLiveAuthority() ? GetAuthoritativeLiveAuthority()->TipAuthority() : NULL;
            if (ta != NULL)
            {
                LOCK(cs_main);
                const uint256 authHashT = ta->TipHash();
                const uint256 authHash = (authHashT != uint256(0)) ? authHashT : ta->BaseTipHash();
                std::map<uint256, CBlockIndex*>::iterator it = mapBlockIndex.find(authHash);
                if (it != mapBlockIndex.end() && it->second != NULL)
                {
                    hashBestChain = authHash;
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

// Optional failpoint-aware delivery (single-instance only).
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

// ReorgAtomicStats accessors (S1-d2 seam).
static void GetStats(ReorgAtomicStats* out)
{
    if (out == NULL) return;
    memset(out, 0, sizeof(*out));
    GetReorgAtomicStatsForTesting(out);
}

static void AssertCleanState()
{
    ReorgAtomicStats st;
    memset(&st, 0, sizeof(st));
    GetReorgAtomicStatsForTesting(&st);
    BOOST_CHECK_MESSAGE(!st.overlayArmed, "the V-phase overlay must be disarmed after every atomic reorg (invariant 8)");
    BOOST_CHECK_MESSAGE(!st.trailArmed, "the connect supply trail must be disarmed after every atomic reorg (invariant 8)");
    BOOST_CHECK_EQUAL(st.trailEntries, 0);
}

} // namespace atomictest
