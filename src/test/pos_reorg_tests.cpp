// S1 ACCEPTANCE — REAL PoS REORG (test-only fixture).
//
// Goal: prove that a branch whose winning block is a genuine PROOF-OF-STAKE block
// (real coinstake, real kernel, real signature) is published by the atomic
// V->C->A path with the full consensus checks, and that rollback/restart and the
// MoneySupply invariant hold afterwards.
//
// Regtest difficulty: the production regtest profile keeps the MAINNET PoS limit
// (bnProofOfStakeLimit = ~0 >> 20, main.cpp), which makes a kernel unfindable on
// this hardware. The test therefore lowers the PoS limit IN THIS TEST PROCESS
// ONLY (regtest fixture only). This is not a production path: the override is a
// local assignment in the test binary, never reachable from mainnet/testnet, does
// not touch consensus formulas, serialization or blk*.dat format, and every
// coinstake/kernel/signature/maturity check still runs unchanged.
#include <boost/test/unit_test.hpp>

#include "main.h"
#include "chainstore_context.h"
#include "miner.h"
#include "wallet.h"
#include "util.h"
#include "blockindex_authoritative_live.h"
#include "blockindex_manager.h"
#include "chainstate/observe_hook.h"
#include "kernel.h"

#include <boost/filesystem.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

extern CWallet* pwalletMain;
extern int64_t nLastCoinStakeSearchTime;

namespace posreorg {

static uint64_t RssKb()
{
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line))
        if (line.compare(0, 6, "VmRSS:") == 0) return strtoull(line.c_str() + 6, NULL, 10);
    return 0;
}

static CBlock* MakePoWBlock(CBlockIndex* pindexPrev, unsigned int nTag)
{
    CBlock* pblock = CreateNewBlock(pwalletMain, false, NULL, NULL);
    if (pblock == NULL) return NULL;
    pblock->nVersion = 1;
    // One day per block: the stake-modifier generation interval is a TIME
    // interval (600 s), so a +1s regtest chain never crosses one and
    // GetKernelStakeModifier would fail. Realistic spacing (the same rule the
    // reorg_314 daemon fixture uses) keeps every consensus check intact.
    int64_t nTime = (int64_t)pindexPrev->GetBlockTime() + 86400;
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

static bool MinePoW(CBlockIndex* prev, unsigned int tag, CBlockIndex** out)
{
    CBlock* b = MakePoWBlock(prev, tag);
    if (b == NULL) return false;
    bool ok = false;
    {
        LOCK(cs_main);
        const uint256 h = b->GetHash();
        ok = b->CheckBlock(true, true, true) && ProcessBlock(NULL, b);
        if (ok) *out = mapBlockIndex[h];
    }
    if (ok)
    {
        // The accept path does not notify the wallet; staking needs the coinbase
        // in the wallet (SelectCoinsForStaking works off wallet txns).
        pwalletMain->AddToWalletIfInvolvingMe(b->vtx[0], b, true);
    }
    delete b;
    return ok && *out != NULL;
}

static bool MinePoWChain(CBlockIndex* parent, int n, unsigned int tagBase, CBlockIndex** out)
{
    CBlockIndex* p = parent;
    for (int i = 1; i <= n; ++i)
    {
        CBlockIndex* nx = NULL;
        if (!MinePoW(p, tagBase + (unsigned)i, &nx)) return false;
        p = nx;
    }
    *out = p;
    return true;
}

// Build a REAL proof-of-stake block on top of `pindexPrev`.
//
// The coinstake is assembled from the same consensus primitives the production
// staking path uses (CheckStakeKernelHash for the kernel + SignSignature for the
// stake input), but the coin is chosen explicitly instead of going through
// CWallet::CreateCoinStake's staking-balance seam (which is a wallet-query path,
// not a consensus shortcut). Every consensus check on the resulting block still
// runs in CheckBlock/ConnectBlock: coinstake shape, kernel hash vs target,
// signature, maturity, block time.
static bool BuildPoSBlock(CBlockIndex* pindexPrev, CBlock** out, std::string* err)
{
    // Stake source: the coinbase of the block at height 1 (matured in regtest).
    CBlockIndex* stakeIdx = pindexPrev;
    while (stakeIdx != NULL && stakeIdx->nHeight > 1) stakeIdx = stakeIdx->pprev;
    if (stakeIdx == NULL || stakeIdx->nHeight != 1) { *err = "no matured stake source block"; return false; }
    CBlock stakeBlk;
    if (!stakeBlk.ReadFromDisk(stakeIdx)) { *err = "cannot read the stake source block"; return false; }
    if (stakeBlk.vtx.empty()) { *err = "stake source block has no transactions"; return false; }
    const CTransaction txPrev = stakeBlk.vtx[0];
    const uint256 txPrevHash = txPrev.GetHash();

    CBlock* b = CreateNewBlock(pwalletMain, true, NULL, NULL);
    if (b == NULL) { *err = "PoS template unavailable"; return false; }
    b->nVersion = 1;
    b->hashPrevBlock = *pindexPrev->phashBlock;
    b->nBits = GetNextTargetRequired(pindexPrev, true);

    // Coinstake: empty first output, payout of the stake value (zero interest,
    // so the MoneySupply invariant is parent + 0 - 0).
    CTransaction cs;
    cs.vin.resize(1);
    cs.vin[0].prevout = COutPoint(txPrevHash, 0);
    cs.vout.resize(2);
    cs.vout[0].SetEmpty();
    cs.vout[1].nValue = txPrev.vout[0].nValue;
    cs.vout[1].scriptPubKey = txPrev.vout[0].scriptPubKey;

    const unsigned int nTxPrevOffset =
        ::GetSerializeSize(CBlock(), SER_DISK, CLIENT_VERSION) -
        (2 * GetSizeOfCompactSize(0)) + GetSizeOfCompactSize(stakeBlk.vtx.size());

    {
        uint64_t mod = 0; int modH = 0; int64_t modT = 0;
        const bool modOk = GetKernelStakeModifier(stakeBlk.GetHash(), pindexPrev, mod, modH, modT, false);
        fprintf(stderr, "POS-REORG kernel-prep: nBits=%08x stakeValue=%lld txPrevTime=%u modOk=%d mod=%016llx modH=%d\n",
                b->nBits, (long long)txPrev.vout[0].nValue, txPrev.nTime, modOk ? 1 : 0,
                (unsigned long long)mod, modH);
    }
    int64_t nTimeStart = std::max((int64_t)pindexPrev->GetBlockTime() + 1,
                                  (int64_t)pindexPrev->GetPastTimeLimit() + 1);
    bool found = false;
    uint256 hp, tp;
    for (int64_t nTimeTx = nTimeStart; nTimeTx < nTimeStart + 6 * 3600; ++nTimeTx)
    {
        if (CheckStakeKernelHash(pindexPrev, b->nBits, stakeBlk, nTxPrevOffset, txPrev,
                                 cs.vin[0].prevout, (unsigned int)nTimeTx, hp, tp, false))
        {
            cs.nTime = (unsigned int)nTimeTx;
            found = true;
            break;
        }
    }
    if (!found) { delete b; *err = "no PoS kernel found in the search window"; return false; }
    if (!SignSignature(*pwalletMain, txPrev, cs, 0)) { delete b; *err = "coinstake signature failed"; return false; }

    b->vtx.insert(b->vtx.begin() + 1, cs);
    b->nTime = cs.nTime;
    b->vtx[0].nTime = cs.nTime;
    b->vtx[0].vin[0].scriptSig = CScript() << (pindexPrev->nHeight + 1) << (unsigned)0x9001;
    b->hashMerkleRoot = b->BuildMerkleTree();
    if (!b->IsProofOfStake()) { delete b; *err = "assembled block is not proof-of-stake"; return false; }
    *out = b;
    return true;
}

// Build a PoW block on an explicit parent hash (side-branch capable): copies the
// production template and re-parents it. Difficulty is constant at the regtest
// limit, so the template nBits still equals GetNextTargetRequired(prev,false).
static bool BuildPoWOnTemplate(const CBlock& tmpl, const uint256& prevHash, int height,
                               int64_t nTime, unsigned int tag, CBlock* out)
{
    *out = tmpl;
    out->nVersion = 1;
    out->nTime = (unsigned int)nTime;
    out->hashPrevBlock = prevHash;
    out->vtx.resize(1);
    out->vtx[0].nTime = out->nTime;
    out->vtx[0].vin[0].scriptSig = CScript() << height << tag;
    out->hashMerkleRoot = out->BuildMerkleTree();
    const uint256 target = CBigNum().SetCompact(out->nBits).getuint256();
    while (out->GetHash() > target && out->nNonce < 0xffffffff) ++out->nNonce;
    return out->GetHash() <= target;
}

static bool Deliver(CBlock* pblock)
{
    {
        LOCK(cs_main);
        if (pblock->CheckBlock(true, true, true) && ProcessBlock(NULL, pblock)) return true;
    }
    unsigned int nFile = 0, nBlockPos = 0;
    {
        LOCK(cs_main);
        if (pblock->WriteToDisk(nFile, nBlockPos) && pblock->AcceptBlock()) return true;
    }
    return false;
}

static const BlockIndexTipAuthority* TipAuth()
{
    return GetAuthoritativeLiveAuthority() ? GetAuthoritativeLiveAuthority()->TipAuthority() : NULL;
}

static bool FourSourcesAgree(const char* when, std::string* why)
{
    const BlockIndexTipAuthority* ta = TipAuth();
    if (ta == NULL) { *why = std::string(when) + ": no tip authority"; return false; }
    int mgrH = -1; std::string merr;
    if (GetBlockIndexManager().ActiveTipHeight(&mgrH, &merr) != BLOCK_INDEX_MANAGER_OK)
    { *why = std::string(when) + ": manager tip unavailable"; return false; }
    const uint256 th = ta->TipHash();
    const bool haveLogical = (th != uint256(0));
    const int authH = haveLogical ? ta->TipHeight() : AuthoritativeBaseTipHeight();
    const uint256 authHash = haveLogical ? th : ta->BaseTipHash();
    cs::store3a::CommitStore3A* st = s16::g_chainObserve.store;
    if (st == NULL || !st->opened()) { *why = std::string(when) + ": store not armed"; return false; }
    if (!s16::g_chainObserve.pending.empty() && !s16::g_chainObserve.FlushPending())
    { *why = std::string(when) + ": barrier flush failed"; return false; }
    uint256 stHash; memcpy(stHash.begin(), st->tipHash().data(), 32);
    if (mgrH != authH) { *why = strprintf("%s: manager=%d authority=%d", when, mgrH, authH); return false; }
    if (authH != nBestHeight) { *why = strprintf("%s: authority=%d nBestHeight=%d", when, authH, nBestHeight); return false; }
    if (authHash != hashBestChain) { *why = std::string(when) + ": authority hash != hashBestChain"; return false; }
    if (st->tipHeight() != nBestHeight) { *why = strprintf("%s: store=%d nBestHeight=%d", when, (int)st->tipHeight(), nBestHeight); return false; }
    if (stHash != hashBestChain) { *why = std::string(when) + ": store tip hash != hashBestChain"; return false; }
    return true;
}

struct ArmedStore
{
    cs::store3a::CommitStore3A store;
    s16::ChainObserveState saved;
    bool armed;
    std::string err;

    ArmedStore() : armed(false)
    {
        namespace bfs = boost::filesystem;
        const bfs::path dir = bfs::temp_directory_path() / bfs::unique_path("pos-reorg-%%%%-%%%%");
        bfs::create_directories(dir);
        cs::ChainError oe;
        if (!store.open(dir.string(), &oe)) { err = "store open: " + oe.str(); return; }
        cs::Delta birth;
        cs::Hash gh; memcpy(gh.data(), pindexGenesisBlock->GetBlockHash().begin(), 32);
        cs::ChainError be;
        if (!store.publish(birth, gh, 0, &be)) { err = "genesis born-commit: " + be.str(); return; }
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
        s16::g_chainObserve.pending.clear();
        s16::g_chainObserve.needsRecovery = false;
        armed = true;
    }
    ~ArmedStore()
    {
        if (armed) s16::g_chainObserve = saved;
        if (store.opened()) store.close();
    }
};

BOOST_AUTO_TEST_SUITE(pos_reorg_tests)

BOOST_AUTO_TEST_CASE(pos_branch_reorg_atomic_with_restart_and_supply)
{
    chaintest::StoreOwnershipGuard ownerGuard;
    BOOST_REQUIRE_MESSAGE(ownerGuard.owned, "chain store ownership: " << ownerGuard.err);
    BOOST_REQUIRE_MESSAGE(fRegTest, "this fixture must run in regtest");
    // Test-only regtest PoS limit: mainnet keeps >>20 (unfindable here). The
    // hook refuses outside regtest by construction.
    const CBigNum savedPosLimit = GetProofOfStakeLimitForTesting();
    BOOST_REQUIRE_MESSAGE(SetProofOfStakeLimitForTesting(CBigNum(~uint256(0) >> 8)),
                          "the PoS limit override must be accepted in regtest only");

    ArmedStore arm;
    BOOST_REQUIRE_MESSAGE(arm.armed, "armed store: " << arm.err);

    // A: PoW chain 1..10.
    CBlockIndex* a10 = NULL;
    const int kForkDepth = 10;   // daily timestamps cross the 600 s modifier interval
    BOOST_REQUIRE(MinePoWChain(pindexBest, kForkDepth, 0x8100, &a10));
    BOOST_REQUIRE_EQUAL(a10->nHeight, kForkDepth);
    if (!s16::g_chainObserve.pending.empty())
        BOOST_REQUIRE(s16::g_chainObserve.FlushPending());
    BOOST_REQUIRE_EQUAL(s16::g_chainObserve.store->tipHeight(), kForkDepth);

    // Real PoS block on top of A10 (signed while A10 is still the active tip, so
    // CreateCoinStake resolves the kernel against the same parent the side branch
    // will use). This block is the WINNING block of the competing branch.
    {
        const int64_t nBal = pwalletMain->GetBalance();
        fprintf(stderr, "POS-REORG wallet: balance=%lld walletTx=%" PRIszu"\n",
                (long long)nBal, pwalletMain->mapWallet.size());
    }
    CBlock* posBlk = NULL;
    std::string perr;
    BOOST_REQUIRE_MESSAGE(BuildPoSBlock(a10, &posBlk, &perr), perr);
    const uint256 posHash = posBlk->GetHash();
    printf("POS-REORG built PoS block h=%d hash=%s time=%u nBits=%08x vtx=%" PRIszu"\n",
           kForkDepth + 1, posHash.ToString().substr(0,16).c_str(), posBlk->nTime, posBlk->nBits, posBlk->vtx.size());
    fflush(stdout);
    BOOST_REQUIRE_MESSAGE(posBlk->vtx.size() >= 2, "PoS block must carry a coinstake");
    BOOST_CHECK_MESSAGE(posBlk->vtx[1].IsCoinStake(), "vtx[1] must be the coinstake");
    BOOST_CHECK_MESSAGE(posBlk->IsProofOfStake(), "block must be flagged PoS");

    // A continues to 12 (PoW) -> the PoS block now competes at height 11.
    CBlockIndex* a12 = NULL;
    BOOST_REQUIRE(MinePoWChain(a10, 2, 0x8200, &a12));
    BOOST_REQUIRE_EQUAL(a12->nHeight, kForkDepth + 2);
    const int forkHeight = a10->nHeight;
    const uint256 forkHash = a10->GetBlockHash();
    if (!s16::g_chainObserve.pending.empty())
        BOOST_REQUIRE(s16::g_chainObserve.FlushPending());
    BOOST_REQUIRE_EQUAL(s16::g_chainObserve.store->tipHeight(), kForkDepth + 2);

    // Extend the PoS branch with PoW blocks until it outweighs the active tip: a
    // single PoS block carries less cumulative trust than the 12 PoW blocks, and
    // the reorg must land on a branch that CONTAINS the PoS block.
    CBlock* tmplRaw = CreateNewBlock(pwalletMain, false, NULL, NULL);
    BOOST_REQUIRE(tmplRaw != NULL);
    const CBlock tmpl = *tmplRaw;
    delete tmplRaw;
    std::vector<CBlock> branch;
    branch.push_back(*posBlk);
    uint256 prevHash = posHash;
    int64_t prevTime = (int64_t)posBlk->nTime;
    int prevHeight = kForkDepth + 1;
    for (int i = 1; i <= 14; ++i)
    {
        CBlock nb;
        BOOST_REQUIRE_MESSAGE(BuildPoWOnTemplate(tmpl, prevHash, prevHeight + 1, prevTime + 86400,
                                                 0x8300 + (unsigned)i, &nb),
                              "side-branch PoW grind failed at height " << (prevHeight + 1));
        branch.push_back(nb);
        prevHash = nb.GetHash();
        prevTime = nb.nTime;
        prevHeight += 1;
    }
    fprintf(stderr, "POS-REORG branch: pos=%d pow=%" PRIszu" tip_h=%d\n",
            1, branch.size() - 1, branch.back().GetHash() == uint256(0) ? -1 : kForkDepth + 1 + (int)branch.size() - 1);

    ReorgAtomicStats before; memset(&before, 0, sizeof(before));
    GetReorgAtomicStatsForTesting(&before);

    // Deliver the PoS block first (side), then its PoW extensions. The winning
    // delivery runs the atomic V->C->A path with FULL ConnectBlock validation of
    // every branch block (kernel, signature, maturity, supply).
    bool ok = true;
    for (size_t i = 0; i < branch.size() && ok; ++i)
        ok = Deliver(&branch[i]);
    fprintf(stderr, "POS-REORG deliver ok=%d best=%d/%s trust=%s\n", ok ? 1 : 0,
            nBestHeight, hashBestChain.ToString().substr(0,12).c_str(),
            nBestChainTrust.ToString().c_str());
    BOOST_REQUIRE_MESSAGE(ok, "the PoS branch block was rejected");
    delete posBlk;

    ReorgAtomicStats after; memset(&after, 0, sizeof(after));
    GetReorgAtomicStatsForTesting(&after);
    BOOST_REQUIRE_MESSAGE(after.count == before.count + 1, "expected exactly ONE atomic reorg");
    BOOST_CHECK_EQUAL(after.lastForkHeight, forkHeight);
    BOOST_TEST_MESSAGE("atomic stats: fork=" << after.lastForkHeight << " disc=" << after.lastDisconnect
                      << " conn=" << after.lastConnect << " undoOps=" << after.lastUndoOps);

    // The winning tip must be the last block of the PoS-containing branch.
    BOOST_REQUIRE_EQUAL(hashBestChain.ToString(), branch.back().GetHash().ToString());
    BOOST_REQUIRE_EQUAL(nBestHeight, kForkDepth + 1 + (int)branch.size() - 1);
    BlockIndexSnapshot snap; std::string serr;
    BOOST_REQUIRE_EQUAL((int)GetAuthoritativeLiveAuthority()->ResolveBlockSnapshot(hashBestChain, &snap, &serr),
                        (int)BlockIndexHotStatus::OK);
    BOOST_CHECK_EQUAL(snap.height, nBestHeight);
    // The PoS block MUST be part of the committed active chain.
    {
        BlockIndexSnapshot posSnap; std::string perr2;
        BOOST_REQUIRE_EQUAL((int)GetAuthoritativeLiveAuthority()->ResolveBlockSnapshot(posHash, &posSnap, &perr2),
                            (int)BlockIndexHotStatus::OK);
        BOOST_CHECK_MESSAGE(posSnap.fProofOfStake, "the committed chain must contain the proof-of-stake block");
        BOOST_CHECK_EQUAL(posSnap.height, kForkDepth + 1);
        BOOST_CHECK_EQUAL(posSnap.hash.ToString(), posHash.ToString());
    }

    std::string why;
    BOOST_REQUIRE_MESSAGE(FourSourcesAgree("after PoS reorg", &why), why);
    // MoneySupply: authoritative snapshot vs legacy derived view must agree (a
    // duplicated/missing coinstake mint would break this).
    BOOST_CHECK_EQUAL(snap.nMoneySupply, pindexBest->nMoneySupply);
    fprintf(stderr, "POS-REORG committed tip=%d/%s supply=%lld pos=%d store_h=%d rss=%lluKB\n",
            nBestHeight, hashBestChain.ToString().substr(0,16).c_str(),
            (long long)snap.nMoneySupply, snap.fProofOfStake ? 1 : 0,
            (int)s16::g_chainObserve.store->tipHeight(), (unsigned long long)RssKb());

    // Rollback/restart equivalence: a second reconcile must be a strict no-op.
    const int hNow = (int)s16::g_chainObserve.store->tipHeight();
    std::string r2;
    BOOST_REQUIRE_MESSAGE(s16::ReconcileStore3AToChain(s16::g_chainObserve.store, &r2),
                          "restart reconcile failed: " << r2);
    if (!s16::g_chainObserve.pending.empty())
        BOOST_REQUIRE(s16::g_chainObserve.FlushPending());
    BOOST_CHECK_EQUAL((int)s16::g_chainObserve.store->tipHeight(), hNow);
    BOOST_REQUIRE_MESSAGE(FourSourcesAgree("after restart reconcile", &why), why);

    // The coinstake's first output must exist in Store3A (the PoS block was
    // applied for real, not just published in the index).
    {
        std::map<uint256, CBlockIndex*>::iterator pit = mapBlockIndex.find(posHash);
        BOOST_REQUIRE_MESSAGE(pit != mapBlockIndex.end() && pit->second != NULL,
                              "the committed PoS block must be in the block index");
        CBlock chk;
        BOOST_REQUIRE_MESSAGE(chk.ReadFromDisk(pit->second), "cannot read the committed PoS block");
        BOOST_REQUIRE(chk.IsProofOfStake());
        const CTransaction& cs2 = chk.vtx[1];
        BOOST_REQUIRE(cs2.IsCoinStake());
        BOOST_REQUIRE(cs2.vout.size() > 1);
        cs::OutPoint ck;
        memcpy(ck.txid.data(), cs2.GetHash().begin(), 32);
        ck.n = 1;
        cs::Coin coin; bool found = false; cs::ChainError ce;
        BOOST_REQUIRE_MESSAGE(s16::g_chainObserve.store->get(ck, &coin, &found, &ce), "store.get: " << ce.str());
        BOOST_CHECK_MESSAGE(found, "the coinstake payout MUST exist in Store3A after the reorg");
    }

    BOOST_CHECK(SetProofOfStakeLimitForTesting(savedPosLimit));
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace posreorg
