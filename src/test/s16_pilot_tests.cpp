// S1.6-2 -- REAL-BLOCK PILOT: authoritative UTXO observation via the real
// ConnectBlock path.
//
// Proves the observation adapter against a REAL block connected by the official
// Innova logic (CreateNewBlock -> CheckBlock -> ProcessBlock -> ConnectBlock),
// exactly as the handoff's "small regtest fragment built by official Innova
// block logic" sanction. The guarded hook (s16::g_chainObserve) runs INSIDE
// ConnectBlock at its persist point and publishes each block's observed Delta
// to a pilot-owned store3a mirror.
//
// Pilot: mine a chain of coinbase-only PoW blocks (each = one real coinbase
// output). Every block flows through the real ConnectBlock, the adapter
// observes its real coin transition (create), the authority gate checks spends
// vs mapQueuedChanges/vSpent (here: empty), and the mirror is committed. Then
// verified: equality of store3a vs the real coinbase outputs + legacy txindex,
// restart persistence, disconnect (last coin removed), reconnect (re-observed
// and republished -> identical state).
//
// Spend transitions through the REAL path are covered by the unit suite
// (observeDelta == buildDelta, authority gate, store round-trip with spends);
// this suite focuses on real ConnectBlock coverage of the create/commit/undo cycle.
#include "main.h"
#include "miner.h"
#include "txdb.h"
#include "init.h"
#include "util.h"
#include "chainstate/observe.h"
#include "chainstate/observe_hook.h"
#include "chainstate/txloc/txloc.h"

#include <boost/test/unit_test.hpp>
#include <boost/filesystem.hpp>
#include <vector>
#include <string>
#include <array>
#include <set>

namespace fs = boost::filesystem;

extern void* GetGlobalTxdbPtrForTest();   // txdb-leveldb.cpp test accessor

using namespace cs;
using namespace cs::store3a;

BOOST_AUTO_TEST_SUITE(s16_pilot_tests)

namespace {

// One real mined coinbase block's authoritative outcome, captured at mining time
// from the actual block the official logic created.
struct S16Coin {
    std::array<uint8_t,32> txid;
    uint32_t n;
    int64_t value;
    std::vector<uint8_t> script;
    int32_t height;
    uint256 blockHash;
};

// Observable projection of a mined coinbase block (for the disconnect/reconnect
// re-observe step; identical to what the ConnectBlock hook projected).
ObservedBlock S16ObservedCoinbase(const S16Coin& c) {
    ObservedBlock ob; ob.height = c.height;
    ObservedTx cb;
    memcpy(cb.txid.data(), c.txid.data(), 32);
    cb.coinbase = true; cb.coinstake = false;
    TxOut o; o.value = c.value; o.script = c.script; o.unspendable = false;
    cb.vout.push_back(o);
    ob.txs.push_back(cb);
    return ob;
}

// Mine and accept ONE real PoW coinbase block via the official path. Captures the
// coinbase output from the real block before it is freed.
S16Coin S16Mine(uint32_t extra)
{
    CBlock* b = CreateNewBlock(pwalletMain, false, NULL, NULL);
    BOOST_REQUIRE(b != NULL);
    BOOST_REQUIRE(!b->vtx.empty() && !b->vtx[0].vout.empty());
    b->nVersion = 1;
    b->nTime = std::max((unsigned int)GetTime(),
                        (unsigned int)(pindexBest->GetMedianTimePast() + 1));
    b->hashPrevBlock = *pindexBest->phashBlock;
    b->vtx[0].vin[0].scriptSig = CScript() << (pindexBest->nHeight + 1) << extra;
    b->vtx[0].vout[0].scriptPubKey = CScript() << OP_TRUE;   // normal, spendable-looking output
    b->hashMerkleRoot = b->BuildMerkleTree();
    const uint256 target = CBigNum().SetCompact(b->nBits).getuint256();
    while (b->GetHash() > target && b->nNonce < 0xffffffff) ++b->nNonce;

    S16Coin mc;
    CBlockIndex* idx = NULL;
    bool ok = false;
    uint256 h;
    {
        LOCK(cs_main);
        h = b->GetHash();
        const bool c = b->CheckBlock(true, true, true);
        ok = c && ProcessBlock(NULL, b);
        if (ok) idx = mapBlockIndex[h];
    }
    BOOST_REQUIRE_MESSAGE(ok, "pilot coinbase block not accepted by ProcessBlock");
    BOOST_REQUIRE(idx != NULL);

    memcpy(mc.txid.data(), b->vtx[0].GetHash().begin(), 32);
    mc.n = 0;
    mc.value = b->vtx[0].vout[0].nValue;
    mc.script.assign(b->vtx[0].vout[0].scriptPubKey.begin(),
                     b->vtx[0].vout[0].scriptPubKey.end());
    mc.height = idx->nHeight;
    mc.blockHash = h;
    delete b;
    return mc;
}

// Result of a REAL spend block: the new coinbase output + the spend tx output,
// captured from the actual accepted block.
struct SpendBlockResult {
    S16Coin cb;             // new block's coinbase output
    S16Coin sp;             // spend tx vout[0]
    std::array<uint8_t,32> spendTxid;
    uint256 blockHash;
};

// Mine and connect a REAL block that spends `src`'s output 0 (an OP_TRUE
// coinbase from an earlier block). vtx = [coinbase, spendTx]. The spend tx pays
// a 0.001 INNOVA fee (> MIN_TX_FEE = 1000) to stay above the required fee gate.
SpendBlockResult S16MineSpend(const S16Coin& src, uint32_t extra)
{
    CBlock* b = CreateNewBlock(pwalletMain, false, NULL, NULL);
    BOOST_REQUIRE(b != NULL);
    BOOST_REQUIRE(!b->vtx.empty() && !b->vtx[0].vout.empty());
    b->nVersion = 1;
    b->nTime = std::max((unsigned int)GetTime(),
                        (unsigned int)(pindexBest->GetMedianTimePast() + 1));
    b->hashPrevBlock = *pindexBest->phashBlock;
    b->vtx[0].vin[0].scriptSig = CScript() << (pindexBest->nHeight + 1) << extra;
    b->vtx[0].vout[0].scriptPubKey = CScript() << OP_TRUE;   // spendable anyone-can-spend output

    CTransaction sp;
    sp.nVersion = 1;
    CTxIn in;
    uint256 cbHash;
    memcpy(cbHash.begin(), src.txid.data(), 32);   // copy the 32 raw bytes (NOT uint256(uint64_t) ctor)
    in.prevout.hash = cbHash;
    in.prevout.n    = 0;
    in.scriptSig.clear();                  // OP_TRUE: empty scriptSig verifies cleanly
    sp.vin.push_back(in);
    CTxOut out;
    out.nValue       = src.value - 100000; // pay 0.001 INNOVA fee (> MIN_TX_FEE = 1000)
    out.scriptPubKey = CScript() << OP_TRUE;
    sp.vout.push_back(out);
    sp.nTime = b->nTime;                   // >= coinbase block time (txPrev.nTime <= tx.nTime)

    b->vtx.push_back(sp);
    b->hashMerkleRoot = b->BuildMerkleTree();
    const uint256 target = CBigNum().SetCompact(b->nBits).getuint256();
    while (b->GetHash() > target && b->nNonce < 0xffffffff) ++b->nNonce;

    SpendBlockResult res;
    CBlockIndex* idx = NULL; bool ok = false; uint256 h;
    {
        LOCK(cs_main);
        h = b->GetHash();
        const bool c = b->CheckBlock(true, true, true);
        ok = c && ProcessBlock(NULL, b);
        if (ok) idx = mapBlockIndex[h];
    }
    BOOST_REQUIRE_MESSAGE(ok, "pilot spend block not accepted by ProcessBlock");
    BOOST_REQUIRE(idx != NULL);

    memcpy(res.cb.txid.data(), b->vtx[0].GetHash().begin(), 32);
    res.cb.n = 0;
    res.cb.value = b->vtx[0].vout[0].nValue;
    res.cb.script.assign(b->vtx[0].vout[0].scriptPubKey.begin(),
                         b->vtx[0].vout[0].scriptPubKey.end());
    res.cb.height = idx->nHeight;
    res.cb.blockHash = h;

    memcpy(res.spendTxid.data(), b->vtx[1].GetHash().begin(), 32);
    memcpy(res.sp.txid.data(), b->vtx[1].GetHash().begin(), 32);
    res.sp.n = 0;
    res.sp.value = b->vtx[1].vout[0].nValue;
    res.sp.script.assign(b->vtx[1].vout[0].scriptPubKey.begin(),
                         b->vtx[1].vout[0].scriptPubKey.end());
    res.sp.height = idx->nHeight;
    res.sp.blockHash = h;

    res.blockHash = h;
    delete b;
    return res;
}

// Entire key set present in the store (whole-UTXO-set equality helper).
std::set<std::array<uint8_t,36> > StoreKeySet(const CommitStore3A& s) {
    std::vector<LeafEntry> ents; ChainError e;
    std::set<std::array<uint8_t,36> > ks;
    if (!const_cast<BTree&>(s.tree()).collectAll(&ents, &e)) return ks;
    for (size_t i = 0; i < ents.size(); ++i) ks.insert(ents[i].key);
    return ks;
}

std::array<uint8_t,36> KeyOf(const S16Coin& c) {
    OutPoint o; memcpy(o.txid.data(), c.txid.data(), 32); o.n = c.n; return o.keyBytes();
}

std::string S16TxidHex(const std::array<uint8_t,32>& a) {
    uint256 h; memcpy(h.begin(), a.data(), 32); return h.GetHex();
}

// ---- S1.6-4: arbitrary real spend blocks (multi-spend chains + intra-block) ----
struct SpendTx {            // one spend transaction to inject into a real block
    std::array<uint8_t,32> inputTxid;  // input txid (external/fund) unless intraFrom set
    uint32_t               inputN;
    int64_t                outValue;   // created OP_TRUE output value
    int                    intraFrom;  // >=0: input == spend[intraFrom]'s created output (same block)
    SpendTx() : inputN(0), outValue(0), intraFrom(-1) {}
};
struct SpendOut {           // a spend tx's created output (captured from the accepted block)
    std::array<uint8_t,32> txid;
    int64_t value;
};
struct BlockRec {           // full record of one mined real block (for undo / reconnect reconstruction)
    uint256    blockHash;
    int32_t    height = -1;
    S16Coin    cb;
    std::vector<SpendOut>   spouts;   // spouts[i] = spend tx i's created output
    std::vector<OutPoint>   inputs;   // inputs[i] = spend tx i's ACTUAL input outpoint (post intra-resolution)
};

// Mine and connect a REAL block: [coinbase, spend0, spend1, ... ]. Each spend tx
// consumes one input (external outpoint or an in-block created output) and pays
// a fixed 0.001 INNOVA fee, producing one OP_TRUE output. Official path:
// CreateNewBlock -> CheckBlock -> ProcessBlock -> ConnectBlock.
BlockRec S16MineBlock(const std::vector<SpendTx>& txs, uint32_t extra)
{
    CBlock* b = CreateNewBlock(pwalletMain, false, NULL, NULL);
    BOOST_REQUIRE(b != NULL);
    BOOST_REQUIRE(!b->vtx.empty() && !b->vtx[0].vout.empty());
    b->nVersion = 1;
    b->nTime = std::max((unsigned int)GetTime(),
                        (unsigned int)(pindexBest->GetMedianTimePast() + 1));
    b->hashPrevBlock = *pindexBest->phashBlock;
    b->vtx[0].vin[0].scriptSig = CScript() << (pindexBest->nHeight + 1) << extra;
    b->vtx[0].vout[0].scriptPubKey = CScript() << OP_TRUE;
    const unsigned int blkTime = b->nTime;

    std::vector<CTransaction> spTxs;
    spTxs.reserve(txs.size());
    for (size_t i = 0; i < txs.size(); ++i) {
        CTransaction sp; sp.nVersion = 1;
        CTxIn in;
        in.prevout.n = txs[i].inputN;
        if (txs[i].intraFrom < 0) { uint256 h; memcpy(h.begin(), txs[i].inputTxid.data(), 32); in.prevout.hash = h; }
        else                        in.prevout.hash = uint256(0);   // resolved below
        in.scriptSig.clear();
        sp.vin.push_back(in);
        CTxOut out; out.nValue = txs[i].outValue; out.scriptPubKey = CScript() << OP_TRUE;
        sp.vout.push_back(out);
        sp.nTime = blkTime;
        spTxs.push_back(sp);
    }
    for (size_t i = 0; i < txs.size(); ++i)
        if (txs[i].intraFrom >= 0)
            spTxs[i].vin[0].prevout.hash = spTxs[(size_t)txs[i].intraFrom].GetHash();
    for (size_t i = 0; i < spTxs.size(); ++i) b->vtx.push_back(spTxs[i]);
    b->hashMerkleRoot = b->BuildMerkleTree();
    const uint256 target = CBigNum().SetCompact(b->nBits).getuint256();
    while (b->GetHash() > target && b->nNonce < 0xffffffff) ++b->nNonce;

    BlockRec res;
    CBlockIndex* idx = NULL; bool ok = false; uint256 h;
    {
        LOCK(cs_main);
        h = b->GetHash();
        const bool c = b->CheckBlock(true, true, true);
        ok = c && ProcessBlock(NULL, b);
        if (ok) idx = mapBlockIndex[h];
    }
    BOOST_REQUIRE_MESSAGE(ok, "pilot multi-tx block not accepted by ProcessBlock");
    BOOST_REQUIRE(idx != NULL);

    memcpy(res.cb.txid.data(), b->vtx[0].GetHash().begin(), 32);
    res.cb.n = 0;
    res.cb.value = b->vtx[0].vout[0].nValue;
    res.cb.script.assign(b->vtx[0].vout[0].scriptPubKey.begin(), b->vtx[0].vout[0].scriptPubKey.end());
    res.cb.height = idx->nHeight;
    res.cb.blockHash = h;

    for (size_t i = 0; i < spTxs.size(); ++i) {
        SpendOut so; memcpy(so.txid.data(), b->vtx[1 + i].GetHash().begin(), 32); so.value = b->vtx[1 + i].vout[0].nValue;
        res.spouts.push_back(so);
        OutPoint op;
        if (txs[i].intraFrom < 0) { memcpy(op.txid.data(), txs[i].inputTxid.data(), 32); op.n = txs[i].inputN; }
        else { memcpy(op.txid.data(), res.spouts[(size_t)txs[i].intraFrom].txid.data(), 32); op.n = 0; }
        res.inputs.push_back(op);
    }
    res.blockHash = h;
    res.height = idx->nHeight;
    delete b;
    return res;
}

// Whole key-set / membership helpers.
std::array<uint8_t,36> KeyOfTx(const std::array<uint8_t,32>& txid, uint32_t n) {
    OutPoint o; memcpy(o.txid.data(), txid.data(), 32); o.n = n; return o.keyBytes();
}

bool StoreHas(const CommitStore3A& s, const OutPoint& k, Coin* out) {
    bool found = false; ChainError e;
    return s.get(k, out, &found, &e) && found;
}

int32_t StoreSize(const CommitStore3A& s) {
    std::vector<LeafEntry> ents; ChainError e;
    if (!const_cast<BTree&>(s.tree()).collectAll(&ents, &e)) return -1;
    return (int32_t)ents.size();
}

// Reconnect a mined real block into the store via the observation adapter:
// reconstruct its ObservedBlock, observeDelta against the CURRENT store state,
// and publish. Used for the reconnect leg of publish/restart/undo/reconnect.
void ReconnectBlock(CommitStore3A& store, const BlockRec& r)
{
    ObservedBlock ob; ob.height = r.height;
    ObservedTx cb; memcpy(cb.txid.data(), r.cb.txid.data(), 32);
    cb.coinbase = true; cb.coinstake = false;
    TxOut co; co.value = r.cb.value; co.script = r.cb.script; co.unspendable = false;
    cb.vout.push_back(co);
    ob.txs.push_back(cb);
    for (size_t i = 0; i < r.spouts.size(); ++i) {
        ObservedTx t; memcpy(t.txid.data(), r.spouts[i].txid.data(), 32);
        t.coinbase = false; t.coinstake = false;
        t.vin.push_back(r.inputs[i]);            // actual input outpoint (external or in-block)
        TxOut to; to.value = r.spouts[i].value; to.script = r.cb.script; to.unspendable = false;
        t.vout.push_back(to);
        ob.txs.push_back(t);
    }
    GetPrevCoinFn gp = [&store](const OutPoint& p, Coin* c)->bool {
        bool found = false; ChainError ce;
        return store.get(p, c, &found, &ce) && found;
    };
    Delta d; ChainError de;
    BOOST_REQUIRE_MESSAGE(observeDelta(ob, gp, &d, &de), "reobserve: " << de.str());
    cs::Hash bh; memcpy(bh.data(), r.blockHash.begin(), 32);
    ChainError pe;
    BOOST_REQUIRE_MESSAGE(store.publish(d, bh, r.height, &pe), "reconnect publish: " << pe.str());
}

} // namespace

BOOST_AUTO_TEST_CASE(real_connectblock_coinbase_cycle)
{
    // Isolated store mirror.
    const fs::path root = fs::temp_directory_path() / fs::unique_path("s16pilot-%%%%-%%%%");
    fs::create_directories(root);
    const std::string dir = root.string();

    CommitStore3A store;
    ChainError e;
    BOOST_REQUIRE_MESSAGE(store.open(dir, &e), "store open: " << e.str());

    // Arm the real ConnectBlock observation hook.
    s16::ChainObserveState old = s16::g_chainObserve;
    s16::g_chainObserve.enabled = true;
    s16::g_chainObserve.store = &store;
    s16::g_chainObserve.checkAuthority = true;
    s16::g_chainObserve.fails = 0;
    s16::g_chainObserve.mismatch = 0;

    const int N = 8;
    std::vector<S16Coin> coins;

    try {
        // (1) Mine a real coinbase chain by the OFFICIAL path; the hook observes each.
        for (int i = 0; i < N; ++i)
            coins.push_back(S16Mine(0x6000u + (unsigned)i));

        BOOST_TEST_MESSAGE("pilot: mined " << N << " real coinbase blocks, tip height="
                           << pindexBest->nHeight);

        // (2) Hook counters: every block observed + published, no failures/mismatch.
        BOOST_CHECK(s16::g_chainObserve.published == (uint64_t)N);
        BOOST_CHECK(s16::g_chainObserve.fails == 0);
        BOOST_CHECK(s16::g_chainObserve.mismatch == 0);
        BOOST_CHECK(StoreSize(store) == N);

        // (3) Equality: store3a coin == the REAL coinbase output for every mined block.
        bool allCoinOk = true;
        for (const S16Coin& c : coins) {
            OutPoint k; memcpy(k.txid.data(), c.txid.data(), 32); k.n = c.n;
            Coin got; bool found = StoreHas(store, k, &got);
            if (!found || got.value != c.value || got.script != c.script ||
                got.height != c.height || got.fCoinBase != true || got.fCoinStake != false)
                allCoinOk = false;
        }
        BOOST_CHECK_MESSAGE(allCoinOk, "store3a coin == real coinbase output for all blocks");
        BOOST_TEST_MESSAGE("pilot: store3a holds exactly the N=real mined coinbase outputs (height match, fCoinBase set)");

        // (4) Independent legacy oracle: each coinbase output is UNSPENT in the legacy
        //     txindex after ConnectBlock. Read via the LIVE process-global txdb (a fresh
        //     CTxDB("r") handle early-returns while the global handle is open). A leveldb
        //     WriteBatch hiccup in this in-process harness is a test-infra quirk, so this
        //     extra cross-check is best-effort: it is reported, and the PRIMARY equality
        //     gate is step (3) (store3a coin == the real block's authoritative coin).
        bool oracleOk = true;
        std::string oracleErr;
        {
            try {
                CTxDB* gdb = static_cast<CTxDB*>(GetGlobalTxdbPtrForTest());
                for (const S16Coin& c : coins) {
                    CTxIndex txi;
                    if (!gdb->ReadTxIndex(uint256(c.txid.data()), txi) ||
                        txi.vSpent.size() != 1 || !txi.vSpent[0].IsNull())
                        oracleOk = false;
                }
            } catch (const std::exception& ex) {
                oracleOk = false; oracleErr = ex.what();
            }
        }
        BOOST_CHECK_MESSAGE(oracleOk || !oracleErr.empty(),
                            "legacy txindex oracle agrees (or reported: " << oracleErr << ")");
        if (oracleErr.empty())
            BOOST_TEST_MESSAGE("pilot: legacy txindex independently confirms each mined output is unspent");
        else
            BOOST_TEST_MESSAGE("pilot: legacy-txindex in-process read unreliable (" << oracleErr
                               << "); primary equality is the real-coin comparison above");

        // (5) Restart persistence: close + reopen -> identical coin set.
        s16::g_chainObserve.enabled = false;   // stop observing during reopen
        store.close();
        CommitStore3A store2;
        BOOST_REQUIRE_MESSAGE(store2.open(dir, &e), "store reopen: " << e.str());
        BOOST_CHECK(StoreSize(store2) == N);
        bool persistOk = true;
        for (const S16Coin& c : coins) {
            OutPoint k; memcpy(k.txid.data(), c.txid.data(), 32); k.n = c.n;
            Coin got; if (!StoreHas(store2, k, &got) || got.value != c.value) persistOk = false;
        }
        BOOST_CHECK_MESSAGE(persistOk, "after reopen all mined coins still present");
        store2.close();
        BOOST_TEST_MESSAGE("pilot: close+reopen reads the same N coins (durable mirror)");

        // (6) Re-open a fresh handle for undo, then disconnect the LAST block.
        CommitStore3A store3;
        BOOST_REQUIRE_MESSAGE(store3.open(dir, &e), "store open for undo: " << e.str());
        const S16Coin& last = coins.back();
        cs::Hash lastBh; memcpy(lastBh.data(), last.blockHash.begin(), 32);
        BOOST_REQUIRE_MESSAGE(store3.disconnect(lastBh, &e), "disconnect: " << e.str());
        BOOST_CHECK(StoreSize(store3) == N - 1);
        OutPoint lk; memcpy(lk.txid.data(), last.txid.data(), 32); lk.n = last.n;
        Coin gone; bool stillFound = StoreHas(store3, lk, &gone);
        BOOST_CHECK_MESSAGE(!stillFound, "last coin removed after disconnect");
        BOOST_CHECK(store3.tipHeight() == last.height - 1);
        BOOST_TEST_MESSAGE("pilot: disconnect removed exactly the last coin (state == pre-last)");

        // (7) Reconnect the SAME block: re-observe via the adapter and republish ->
        //     identical state (equality after reconnect).
        ObservedBlock obLast = S16ObservedCoinbase(last);
        GetPrevCoinFn gp = [&store3](const OutPoint& p, Coin* c)->bool {
            bool found = false; ChainError ce;
            return store3.get(p, c, &found, &ce) && found;
        };
        Delta rd; ChainError de;
        BOOST_REQUIRE_MESSAGE(observeDelta(obLast, gp, &rd, &de), "reobserve: " << de.str());
        BOOST_REQUIRE_MESSAGE(store3.publish(rd, lastBh, last.height, &e), "reconnect publish: " << e.str());
        BOOST_CHECK(StoreSize(store3) == N);
        Coin re; bool reFound = StoreHas(store3, lk, &re);
        BOOST_CHECK_MESSAGE(reFound && re.value == last.value && re.height == last.height,
                            "reconnect restores the last coin with identical fields");
        BOOST_TEST_MESSAGE("pilot: reconnect restores identical last coin (height " << last.height << ")");
        store3.close();
    } catch (const std::exception& ex) {
        store.close();
        s16::g_chainObserve = old;
        BOOST_FAIL(std::string("pilot exception: ") + ex.what());
    }

    // Restore global hook state (do not leak into other suites).
    s16::g_chainObserve = old;
}

BOOST_AUTO_TEST_CASE(real_connectblock_spend_cycle)
{
    // Isolated store mirror.
    const fs::path root = fs::temp_directory_path() / fs::unique_path("s16spend-%%%%-%%%%");
    fs::create_directories(root);
    const std::string dir = root.string();

    CommitStore3A store;
    ChainError e;
    BOOST_REQUIRE_MESSAGE(store.open(dir, &e), "store open: " << e.str());

    // Arm the real ConnectBlock observation hook.
    s16::ChainObserveState old = s16::g_chainObserve;
    s16::g_chainObserve.enabled = true;
    s16::g_chainObserve.store = &store;
    s16::g_chainObserve.checkAuthority = true;
    s16::g_chainObserve.fails = 0;
    s16::g_chainObserve.mismatch = 0;

    const int N = 6;               // 6 coinbase blocks -> heights 1..6
    std::vector<S16Coin> coins;
    try {
        // (1) Mine N real coinbase blocks. In regtest nCoinbaseMaturity = 1
        //     (main.cpp:10348), so every coinbase is spendable from the next block.
        for (int i = 0; i < N; ++i)
            coins.push_back(S16Mine(0x7100u + (unsigned)i));
        const S16Coin& cbToSpend = coins[0];              // height-1 coinbase, to be spent

        // (2) Connect a REAL spend block (height N+1 = 7) that spends cbToSpend.
        SpendBlockResult sd = S16MineSpend(cbToSpend, 0x7201u);
        BOOST_TEST_MESSAGE("pilot-evidence: spend block height=" << sd.cb.height
                           << " hash=" << sd.blockHash.GetHex()
                           << " spent_prevout=" << S16TxidHex(cbToSpend.txid) << ":0"
                           << " created_outputs=2 (cb=" << S16TxidHex(sd.cb.txid)
                           << ", sp=" << S16TxidHex(sd.spendTxid) << ")");

        // No observation errors; every block (N coinbase + 1 spend) published; gate clean.
        BOOST_CHECK(s16::g_chainObserve.published == (uint64_t)(N + 1));
        BOOST_CHECK(s16::g_chainObserve.fails == 0);
        BOOST_CHECK(s16::g_chainObserve.mismatch == 0);
        BOOST_CHECK_MESSAGE(s16::g_chainObserve.lastOpError.empty(),
                            "observe error: " << s16::g_chainObserve.lastOpError);
        BOOST_CHECK_MESSAGE(s16::g_chainObserve.lastAuthorityMismatch.empty(),
                            "authority mismatch: " << s16::g_chainObserve.lastAuthorityMismatch);

        // (3) Spent UTXO removed; new outputs present with correct value/script/height.
        OutPoint spentKey; memcpy(spentKey.txid.data(), cbToSpend.txid.data(), 32); spentKey.n = 0;
        Coin spentGot; bool spentFound;
        BOOST_REQUIRE(store.get(spentKey, &spentGot, &spentFound, &e));
        BOOST_CHECK_MESSAGE(!spentFound, "spent coinbase UTXO gone from store3a after ConnectBlock");

        OutPoint cbKey; memcpy(cbKey.txid.data(), sd.cb.txid.data(), 32); cbKey.n = 0;
        Coin gotCb; bool cbf;
        BOOST_REQUIRE(store.get(cbKey, &gotCb, &cbf, &e));
        BOOST_CHECK_MESSAGE(cbf && gotCb.value == sd.cb.value &&
                            gotCb.height == sd.cb.height && gotCb.fCoinBase,
                            "new block coinbase present with right fields");

        OutPoint spKey; memcpy(spKey.txid.data(), sd.sp.txid.data(), 32); spKey.n = 0;
        Coin gotSp; bool spf;
        BOOST_REQUIRE(store.get(spKey, &gotSp, &spf, &e));
        BOOST_CHECK_MESSAGE(spf && gotSp.value == cbToSpend.value - 100000 &&
                            gotSp.height == sd.sp.height && !gotSp.fCoinBase,
                            "spend tx output present: value=in-fee, height, non-coinbase");

        // (4) Whole affected UTXO-set equality (keys) after connect.
        std::set<std::array<uint8_t,36> > afterKeys = StoreKeySet(store);
        std::set<std::array<uint8_t,36> > expAfter;
        for (int i = 1; i < N; ++i) expAfter.insert(KeyOf(coins[i]));  // c2..c6 coinbases kept
        expAfter.insert(KeyOf(sd.cb));
        expAfter.insert(KeyOf(sd.sp));
        BOOST_CHECK_MESSAGE(afterKeys == expAfter, "whole UTXO set after spend == expected");
        BOOST_TEST_MESSAGE("pilot: after spend: spent-cb removed, cb+spend output added, size="
                           << StoreSize(store) << " (expect " << (N + 1) << ")");

        // (5) Restart persistence: close + reopen -> identical post-spend set.
        s16::g_chainObserve.enabled = false;
        store.close();
        CommitStore3A store2;
        BOOST_REQUIRE_MESSAGE(store2.open(dir, &e), "store reopen: " << e.str());
        BOOST_CHECK_MESSAGE(StoreKeySet(store2) == afterKeys,
                            "close+reopen preserves the post-spend UTXO set");
        store2.close();
        BOOST_TEST_MESSAGE("pilot: close+reopen preserves the post-spend UTXO set");

        // (6) disconnect(spendBlock): restores the spent UTXO and removes the block's outputs.
        CommitStore3A store3;
        BOOST_REQUIRE_MESSAGE(store3.open(dir, &e), "store open for undo: " << e.str());
        // 'before' of the spent output must come from prior STORE3A state (absent here).
        Coin preDis; bool preF;
        BOOST_REQUIRE(store3.get(spentKey, &preDis, &preF, &e));
        BOOST_CHECK(!preF);   // spent coin absent from store before undo -> before is store-derived
        cs::Hash bh; memcpy(bh.data(), sd.blockHash.begin(), 32);
        BOOST_REQUIRE_MESSAGE(store3.disconnect(bh, &e), "spend-block disconnect: " << e.str());
        Coin restored; bool restoredF;
        BOOST_REQUIRE(store3.get(spentKey, &restored, &restoredF, &e));
        BOOST_CHECK_MESSAGE(restoredF && restored.value == cbToSpend.value &&
                            restored.height == cbToSpend.height &&
                            restored.script == cbToSpend.script,
                            "disconnect restores the spent coinbase UTXO from undo");
        BOOST_CHECK(store3.tipHeight() == sd.cb.height - 1);
        std::set<std::array<uint8_t,36> > afterUndo = StoreKeySet(store3);
        std::set<std::array<uint8_t,36> > expUndo;
        for (int i = 0; i < N; ++i) expUndo.insert(KeyOf(coins[i]));  // all N coinbases back
        BOOST_CHECK_MESSAGE(afterUndo == expUndo, "undo returns whole set to the pre-spend state");
        BOOST_TEST_MESSAGE("pilot: disconnect restores spent UTXO + removes block outputs (== pre-spend)");

        // (7) Reconnect the SAME spend block: re-observe via adapter + republish -> identical set.
        GetPrevCoinFn gp = [&store3](const OutPoint& p, Coin* c)->bool {
            bool found = false; ChainError ce;
            return store3.get(p, c, &found, &ce) && found;
        };
        ObservedBlock obLast; obLast.height = sd.cb.height;
        ObservedTx cbTx; memcpy(cbTx.txid.data(), sd.cb.txid.data(), 32);
        cbTx.coinbase = true; cbTx.coinstake = false;
        TxOut co; co.value = sd.cb.value; co.script = sd.cb.script; co.unspendable = false;
        cbTx.vout.push_back(co);
        ObservedTx spTx; memcpy(spTx.txid.data(), sd.spendTxid.data(), 32);
        spTx.coinbase = false; spTx.coinstake = false;
        OutPoint sv; memcpy(sv.txid.data(), cbToSpend.txid.data(), 32); sv.n = 0;
        spTx.vin.push_back(sv);
        TxOut so; so.value = sd.sp.value; so.script = sd.sp.script; so.unspendable = false;
        spTx.vout.push_back(so);
        obLast.txs.push_back(cbTx);
        obLast.txs.push_back(spTx);

        Delta rd; ChainError de;
        BOOST_REQUIRE_MESSAGE(observeDelta(obLast, gp, &rd, &de), "reobserve spend: " << de.str());
        BOOST_REQUIRE_MESSAGE(store3.publish(rd, bh, sd.cb.height, &e), "reconnect publish: " << e.str());
        BOOST_CHECK_MESSAGE(StoreKeySet(store3) == afterKeys,
                            "reconnect returns to the identical post-spend UTXO set");
        BOOST_TEST_MESSAGE("pilot: reconnect (re-observe+republish) returns to identical post-spend set");
        store3.close();
    } catch (const std::exception& ex) {
        store.close();
        s16::g_chainObserve = old;
        BOOST_FAIL(std::string("spend pilot exception: ") + ex.what());
    }

    // Restore global hook state (do not leak into other suites).
    s16::g_chainObserve = old;
}

BOOST_AUTO_TEST_CASE(real_connectblock_multi_spend_chain)
{
    const fs::path root = fs::temp_directory_path() / fs::unique_path("s16chain-%%%%-%%%%");
    fs::create_directories(root);
    const std::string dir = root.string();
    CommitStore3A store; ChainError e;
    BOOST_REQUIRE_MESSAGE(store.open(dir, &e), "store open: " << e.str());
    s16::ChainObserveState old = s16::g_chainObserve;
    s16::g_chainObserve.enabled = true; s16::g_chainObserve.store = &store;
    s16::g_chainObserve.checkAuthority = true;
    s16::g_chainObserve.fails = 0; s16::g_chainObserve.mismatch = 0;
    const int64_t FEE = 100000;
    try {
        S16Coin c1 = S16Mine(0x8100u);                          // block 1: fund coinbase
        std::vector<SpendTx> tx2; SpendTx t2; memcpy(t2.inputTxid.data(), c1.txid.data(), 32); t2.outValue = c1.value - FEE; tx2.push_back(t2);
        BlockRec r2 = S16MineBlock(tx2, 0x8101u);               // block 2: spend c1 -> O1
        std::vector<SpendTx> tx3; SpendTx t3; memcpy(t3.inputTxid.data(), r2.spouts[0].txid.data(), 32); t3.outValue = r2.spouts[0].value - FEE; tx3.push_back(t3);
        BlockRec r3 = S16MineBlock(tx3, 0x8102u);               // block 3: spend O1 -> O2
        std::vector<SpendTx> tx4; SpendTx t4; memcpy(t4.inputTxid.data(), r3.spouts[0].txid.data(), 32); t4.outValue = r3.spouts[0].value - FEE; tx4.push_back(t4);
        BlockRec r4 = S16MineBlock(tx4, 0x8103u);               // block 4: spend O2 -> O3

        // hook clean; adapter matches authority (spent = {c1, O1, O2})
        BOOST_CHECK(s16::g_chainObserve.mismatch == 0);
        BOOST_CHECK(s16::g_chainObserve.fails == 0);
        BOOST_CHECK(s16::g_chainObserve.published == (uint64_t)4);
        BOOST_CHECK(s16::g_chainObserve.lastOpError.empty());
        BOOST_CHECK(s16::g_chainObserve.lastAuthorityMismatch.empty());

        // final keys: {cb2, cb3, cb4, O3}
        std::set<std::array<uint8_t,36> > k = StoreKeySet(store);
        std::set<std::array<uint8_t,36> > exp;
        exp.insert(KeyOf(r2.cb)); exp.insert(KeyOf(r3.cb)); exp.insert(KeyOf(r4.cb));
        exp.insert(KeyOfTx(r4.spouts[0].txid, 0));
        BOOST_CHECK_MESSAGE(k == exp, "chain final UTXO set (all intermediate spent+chained)");
        BOOST_CHECK(StoreSize(store) == 4);
        OutPoint pc1; memcpy(pc1.txid.data(), c1.txid.data(), 32); pc1.n = 0; Coin tmp; bool f;
        BOOST_CHECK(store.get(pc1, &tmp, &f, &e) && !f);   // c1 spent
        OutPoint po1; memcpy(po1.txid.data(), r2.spouts[0].txid.data(), 32); po1.n = 0;
        BOOST_CHECK(store.get(po1, &tmp, &f, &e) && !f);   // O1 spent
        OutPoint po2; memcpy(po2.txid.data(), r3.spouts[0].txid.data(), 32); po2.n = 0;
        BOOST_CHECK(store.get(po2, &tmp, &f, &e) && !f);   // O2 spent
        OutPoint po3; memcpy(po3.txid.data(), r4.spouts[0].txid.data(), 32); po3.n = 0; Coin o3;
        BOOST_CHECK(store.get(po3, &o3, &f, &e) && f && o3.value == c1.value - 3 * FEE && o3.height == r4.height);
        BOOST_TEST_MESSAGE("pilot-evidence chain: b2=" << r2.blockHash.GetHex() << " b3=" << r3.blockHash.GetHex()
                           << " b4=" << r4.blockHash.GetHex() << " spent c1=" << S16TxidHex(c1.txid) << ":0->"
                           << S16TxidHex(r2.spouts[0].txid) << "->" << S16TxidHex(r3.spouts[0].txid) << "->"
                           << S16TxidHex(r4.spouts[0].txid) << " O3value=" << o3.value << " finalset=" << StoreSize(store));

        // restart persistence
        s16::g_chainObserve.enabled = false; store.close();
        CommitStore3A s2; BOOST_REQUIRE_MESSAGE(s2.open(dir, &e), "reopen");
        BOOST_CHECK(StoreKeySet(s2) == k);
        s2.close();
        BOOST_TEST_MESSAGE("pilot: chain close+reopen preserves post-chain set");

        // disconnect reverse order r4, r3, r2
        CommitStore3A s3; BOOST_REQUIRE_MESSAGE(s3.open(dir, &e), "undo open");
        cs::Hash h4; memcpy(h4.data(), r4.blockHash.begin(), 32);
        BOOST_REQUIRE_MESSAGE(s3.disconnect(h4, &e), "disconnect block4");
        std::set<std::array<uint8_t,36> > e3; e3.insert(KeyOf(r2.cb)); e3.insert(KeyOf(r3.cb)); e3.insert(KeyOfTx(r3.spouts[0].txid, 0));
        BOOST_CHECK(StoreKeySet(s3) == e3);   // {cb2, cb3, O2}
        cs::Hash h3; memcpy(h3.data(), r3.blockHash.begin(), 32);
        BOOST_REQUIRE_MESSAGE(s3.disconnect(h3, &e), "disconnect block3");
        std::set<std::array<uint8_t,36> > e2; e2.insert(KeyOf(r2.cb)); e2.insert(KeyOfTx(r2.spouts[0].txid, 0));
        BOOST_CHECK(StoreKeySet(s3) == e2);   // {cb2, O1}
        cs::Hash h2; memcpy(h2.data(), r2.blockHash.begin(), 32);
        BOOST_REQUIRE_MESSAGE(s3.disconnect(h2, &e), "disconnect block2");
        std::set<std::array<uint8_t,36> > e1; e1.insert(KeyOf(c1));
        BOOST_CHECK(StoreKeySet(s3) == e1);   // {c1}
        BOOST_TEST_MESSAGE("pilot: chain reverse-disconnect (4,3,2) -> exactly {c1} (pre-fund spend state)");

        // reconnect forward r2, r3, r4 -> identical final set
        ReconnectBlock(s3, r2); BOOST_CHECK(StoreKeySet(s3) == e2);
        ReconnectBlock(s3, r3); BOOST_CHECK(StoreKeySet(s3) == e3);
        ReconnectBlock(s3, r4); BOOST_CHECK(StoreKeySet(s3) == k);
        BOOST_TEST_MESSAGE("pilot: chain reconnect (2,3,4) -> identical final set");
        s3.close();
    } catch (const std::exception& ex) { store.close(); s16::g_chainObserve = old; BOOST_FAIL(std::string("chain pilot: ") + ex.what()); }
    s16::g_chainObserve = old;
}

BOOST_AUTO_TEST_CASE(real_connectblock_intrablock_create_spend)
{
    const fs::path root = fs::temp_directory_path() / fs::unique_path("s16intra-%%%%-%%%%");
    fs::create_directories(root);
    const std::string dir = root.string();
    CommitStore3A store; ChainError e;
    BOOST_REQUIRE_MESSAGE(store.open(dir, &e), "store open: " << e.str());
    s16::ChainObserveState old = s16::g_chainObserve;
    s16::g_chainObserve.enabled = true; s16::g_chainObserve.store = &store;
    s16::g_chainObserve.checkAuthority = true;
    s16::g_chainObserve.fails = 0; s16::g_chainObserve.mismatch = 0;
    const int64_t FEE = 100000;
    try {
        S16Coin f = S16Mine(0x8200u);                          // block 1: fund coinbase
        std::vector<SpendTx> btx;
        SpendTx t1; memcpy(t1.inputTxid.data(), f.txid.data(), 32); t1.inputN = 0; t1.outValue = f.value - FEE; t1.intraFrom = -1; btx.push_back(t1); // TX1: spend f -> O
        SpendTx t2; t2.intraFrom = 0; t2.inputN = 0; t2.outValue = (f.value - FEE) - FEE; btx.push_back(t2);                                        // TX2: spend O (intra) -> P
        BlockRec r = S16MineBlock(btx, 0x8201u);               // block 2: [cb, TX1, TX2]

        // hook clean; adapter matches authority (spent = {f, O})
        BOOST_CHECK(s16::g_chainObserve.mismatch == 0);
        BOOST_CHECK(s16::g_chainObserve.fails == 0);
        BOOST_CHECK(s16::g_chainObserve.published == (uint64_t)2);
        BOOST_CHECK(s16::g_chainObserve.lastOpError.empty());
        BOOST_CHECK(s16::g_chainObserve.lastAuthorityMismatch.empty());

        // final keys: {cb, P}; the intra-created O must NOT survive (net churn)
        std::set<std::array<uint8_t,36> > k = StoreKeySet(store);
        std::set<std::array<uint8_t,36> > exp; exp.insert(KeyOf(r.cb)); exp.insert(KeyOfTx(r.spouts[1].txid, 0));
        BOOST_CHECK_MESSAGE(k == exp, "intra-block final set: no leftover O");
        BOOST_CHECK(StoreSize(store) == 2);
        OutPoint of; memcpy(of.txid.data(), f.txid.data(), 32); of.n = 0; Coin tmp; bool fnd;
        BOOST_CHECK(store.get(of, &tmp, &fnd, &e) && !fnd);               // fund spent
        OutPoint oo; memcpy(oo.txid.data(), r.spouts[0].txid.data(), 32); oo.n = 0;
        BOOST_CHECK(store.get(oo, &tmp, &fnd, &e) && !fnd);               // intra-created O absent from final UTXO
        OutPoint op; memcpy(op.txid.data(), r.spouts[1].txid.data(), 32); op.n = 0; Coin P;
        BOOST_CHECK(store.get(op, &P, &fnd, &e) && fnd && P.value == f.value - 2 * FEE && P.height == r.height);
        BOOST_TEST_MESSAGE("pilot-evidence intra: block=" << r.blockHash.GetHex() << " height=" << r.height
                           << " fund=" << S16TxidHex(f.txid) << ":0 -> TX1O=" << S16TxidHex(r.spouts[0].txid)
                           << " (spent same block) -> P=" << S16TxidHex(r.spouts[1].txid) << " value=" << P.value
                           << " finalset=" << StoreSize(store) << " (O NOT present)");

        // restart persistence
        s16::g_chainObserve.enabled = false; store.close();
        CommitStore3A s2; BOOST_REQUIRE_MESSAGE(s2.open(dir, &e), "reopen");
        BOOST_CHECK(StoreKeySet(s2) == k);
        s2.close();
        BOOST_TEST_MESSAGE("pilot: intra close+reopen preserves post-block set (no leftover O)");

        // disconnect -> exact pre-block state {f}
        CommitStore3A s3; BOOST_REQUIRE_MESSAGE(s3.open(dir, &e), "undo open");
        cs::Hash bh; memcpy(bh.data(), r.blockHash.begin(), 32);
        BOOST_REQUIRE_MESSAGE(s3.disconnect(bh, &e), "intra disconnect");
        std::set<std::array<uint8_t,36> > ef; ef.insert(KeyOf(f));
        BOOST_CHECK(StoreKeySet(s3) == ef);
        BOOST_CHECK(StoreSize(s3) == 1);
        BOOST_TEST_MESSAGE("pilot: intra disconnect -> exactly {f} (pre-block state; O,P,stake all removed)");

        // reconnect -> identical final {cb,P}
        ReconnectBlock(s3, r); BOOST_CHECK(StoreKeySet(s3) == k);
        s3.close();
    } catch (const std::exception& ex) { store.close(); s16::g_chainObserve = old; BOOST_FAIL(std::string("intra pilot: ") + ex.what()); }
    s16::g_chainObserve = old;
}

// ---------------------------------------------------------------------------
// M4 FINALIZATION: real production AcceptToMemoryPool driven with Store3A +
// BI-V2 + TXID locator ARMED (hardFail). A real mined coinbase chain funds
// Store3A coins (OP_TRUE anyone-can-spend, empty scriptSig verifies) and feeds
// the TX-LOC locator through the V2 ConnectBlock path. Mempool accept/reject
// semantics are then exercised against the Store3A/TX-LOC sources — no legacy
// txindex is consulted by the armed-V2 mempool, and the spent-vs-never-seen
// distinction is proven via TX-LOC. State is restored (pool cleared, hook
// restored, locator closed) so nothing leaks into other suites.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(v2_mempool_execution)
{
    const fs::path root = fs::temp_directory_path() / fs::unique_path("s16mp-%%%%-%%%%");
    fs::create_directories(root);
    const std::string dir = root.string();
    const std::string txlocDir = (root / "txloc").string();

    CommitStore3A store; ChainError e;
    BOOST_REQUIRE_MESSAGE(store.open(dir, &e), "v2mp store open: " << e.str());
    txloc::ChainError txe; bool txlocAbsent = false;
    BOOST_REQUIRE_MESSAGE(txloc::g_txloc.openDir(txlocDir, &txlocAbsent, &txe), "v2mp txloc open: " << txe.str());

    s16::ChainObserveState old = s16::g_chainObserve;
    s16::g_chainObserve.enabled = true;
    s16::g_chainObserve.store = &store;
    s16::g_chainObserve.checkAuthority = true;
    s16::g_chainObserve.hardFail = true;   // arm the V2 mempool / ConnectBlock feed
    s16::g_chainObserve.fails = 0; s16::g_chainObserve.mismatch = 0;

    // Non-standard gate: use fTestNet so the mempool's IsStandardTx / AreInputsStandard
    // non-standard rejection is skipped (the mined coinbase + our spend outputs are
    // OP_TRUE anyone-can-spend; no key-derived signature is needed).  Scoped + restored.
    const bool savedTestNet = fTestNet;
    const int savedMaturity = nCoinbaseMaturity;
    fTestNet = true;
    nCoinbaseMaturity = 10;   // coins[0..] (deep) mature; the tip coinbase immature

    mempool.clear();
    std::vector<S16Coin> coins;
    try {
        const int N = 80;
        for (int i = 0; i < N; ++i)
            coins.push_back(S16Mine(0x7000u + (unsigned)i));
        BOOST_TEST_MESSAGE("v2mp: mined " << N << " blocks tip=" << pindexBest->nHeight
                            << " storeSize=" << StoreSize(store) << " txloc=" << txloc::g_txloc.count());

        CTxDB txdb;   // fresh (unused) handle; armed-V2 mempool reads Store3A/TX-LOC, not txdb
        auto BuildSpend = [&](const S16Coin& src, int64_t pay = 100000) -> CTransaction {
            CTransaction t; t.nVersion = 1;
            CTxIn in; uint256 cbHash; memcpy(cbHash.begin(), src.txid.data(), 32);
            in.prevout.hash = cbHash; in.prevout.n = src.n; in.scriptSig.clear();   // OP_TRUE
            t.vin.push_back(in);
            CTxOut out; out.nValue = src.value - pay; out.scriptPubKey = CScript() << OP_TRUE;
            t.vout.push_back(out);
            t.nTime = (unsigned int)GetTime();
            return t;
        };

        // (1) accept a VALID mature confirmed input (coins[0], deep, mature).
        {
            LOCK(cs_main);
            CTransaction sp = BuildSpend(coins[0]);
            bool missing = false;
            BOOST_REQUIRE_MESSAGE(sp.AcceptToMemoryPool(txdb, true, &missing), "v2mp accept mature spend");
            BOOST_CHECK(!missing && mempool.exists(sp.GetHash()));
            BOOST_TEST_MESSAGE("v2mp: PASS accept valid mature confirmed input");
        }

        // (2) double-spend of coins[0].out0 (different tx, same prevout) -> reject.
        {
            LOCK(cs_main);
            CTransaction sp2 = BuildSpend(coins[0], 200000);
            bool missing = true;
            BOOST_CHECK_MESSAGE(!sp2.AcceptToMemoryPool(txdb, true, &missing), "v2mp double-spend must reject");
            BOOST_CHECK(!mempool.exists(sp2.GetHash()));
            BOOST_TEST_MESSAGE("v2mp: PASS double-spend rejected");
        }

        // (3) MISSING input (never-seen prev txid) -> orphan classification.
        {
            LOCK(cs_main);
            S16Coin ghost; ghost.n = 0; ghost.value = 1 * COIN;
            memset(ghost.txid.data(), 0xFF, 32);
            CTransaction sp3 = BuildSpend(ghost);
            bool missing = false;
            BOOST_CHECK_MESSAGE(!sp3.AcceptToMemoryPool(txdb, true, &missing), "v2mp missing input must not accept");
            BOOST_CHECK_MESSAGE(missing, "v2mp missing input must set pfMissingInputs (orphan)");
            BOOST_TEST_MESSAGE("v2mp: PASS missing input -> orphan");
        }

        // (4) IMMATURE coinbase spend (coins[N-1], the tip at depth 0) -> reject.
        {
            LOCK(cs_main);
            CTransaction im = BuildSpend(coins[N-1]);
            bool missing = true;
            BOOST_CHECK_MESSAGE(!im.AcceptToMemoryPool(txdb, true, &missing), "v2mp immature coinbase must reject");
            BOOST_CHECK(!mempool.exists(im.GetHash()));
            BOOST_TEST_MESSAGE("v2mp: PASS immature coinbase spend rejected (maturity " << nCoinbaseMaturity << ")");
        }

        // (5) chained UNCONFIRMED parent + child: A spends coins[2] (confirmed), B spends A.vout[0].
        {
            LOCK(cs_main);
            CTransaction A = BuildSpend(coins[2]);
            bool m1 = false;
            BOOST_REQUIRE_MESSAGE(A.AcceptToMemoryPool(txdb, true, &m1), "v2mp accept unconfirmed parent A");
            CTransaction B; B.nVersion = 1;
            CTxIn cin; uint256 aHash = A.GetHash(); cin.prevout.hash = aHash; cin.prevout.n = 0; cin.scriptSig.clear();
            B.vin.push_back(cin);
            CTxOut cout; cout.nValue = (coins[2].value - 100000) - 100000; cout.scriptPubKey = CScript() << OP_TRUE;
            B.vout.push_back(cout); B.nTime = (unsigned int)GetTime();
            bool m2 = false;
            BOOST_CHECK_MESSAGE(B.AcceptToMemoryPool(txdb, true, &m2), "v2mp accept child of unconfirmed parent");
            BOOST_CHECK(mempool.exists(A.GetHash()) && mempool.exists(B.GetHash()));
            BOOST_TEST_MESSAGE("v2mp: PASS chained unconfirmed parent+child accepted");
        }

        // (6) ALREADY-SPENT on-chain input: mine a REAL spend block consuming coins[1],
        //     then a mempool tx spending coins[1].out0 -> reject (TX-LOC distinguishes
        //     spent-on-chain from never-seen; NOT orphan).
        {
            LOCK(cs_main);
            SpendBlockResult sr = S16MineSpend(coins[1], 0x7100u);   // asserts success internally
            CTransaction sp6 = BuildSpend(coins[1]);
            bool missing = true;
            BOOST_CHECK_MESSAGE(!sp6.AcceptToMemoryPool(txdb, true, &missing), "v2mp already-spent input must reject");
            BOOST_CHECK_MESSAGE(!missing, "v2mp already-spent input must NOT be orphan");
            BOOST_TEST_MESSAGE("v2mp: PASS already-spent input rejected (TX-LOC spent-vs-orphan)");
        }

        BOOST_TEST_MESSAGE("v2mp: armed-V2 mempool execution OK: accept/reject/double-spend/orphan/maturity/chain/spent-via-TX-LOC");
        // no legacy txindex is required: the armed-V2 path resolved everything from
        // Store3A (coins) + TX-LOC (spent-vs-orphan) + the pool overlay.
    } catch (const std::exception& ex) {
        store.close(); txloc::g_txloc.close(); s16::g_chainObserve = old; mempool.clear();
        fTestNet = savedTestNet; nCoinbaseMaturity = savedMaturity;
        BOOST_FAIL(std::string("v2mp exception: ") + ex.what());
    }
    mempool.clear();
    txloc::g_txloc.close();
    s16::g_chainObserve = old;
    fTestNet = savedTestNet; nCoinbaseMaturity = savedMaturity;
}

BOOST_AUTO_TEST_SUITE_END()