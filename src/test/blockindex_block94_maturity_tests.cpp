// REGRESSION — fresh-IBD block-94 (coinbase-spend) acceptance under V2 authority.
//
// Production blocker (v5.0.3, first fresh mainnet self-sync): a zero-datadir node
// connected blocks 1..93, then permanently rejected canonical block 94 -
// the first mainnet block whose transaction spends a coinbase. Root cause:
// ConnectInputs anchored the by-value maturity walk at the SPENDER's own hash
// (pindexBlock->GetBlockHash()), but AddToBlockIndex publishes the accepted block
// into the mutable tip authority only AFTER SetBestChain/ConnectBlock returns, so
// the depth-0 lookup failed -> BLOCK_INDEX_MATURITY_UNAVAILABLE -> fail-closed
// reject of every coinbase/coinstake spend.
//
// Minimal repair (main.cpp ConnectInputs authoritative branch): reproduce depth 0
// from the resident spender, then walk depths 1..nCoinbaseMaturity-1 by value from
// the already-published parent (pindexBlock->pprev). Semantics identical to the
// legacy loop.
//
// This test drives the REAL production accept path (CreateNewBlock -> CheckBlock ->
// ProcessBlock -> AcceptBlock -> AddToBlockIndex -> SetBestChain -> ConnectBlock ->
// ConnectInputs) in authoritative mode and asserts a MATURE coinbase spend is
// ACCEPTED (pre-fix: rejected) while an IMMATURE spend is still REJECTED.
//
// Coinbases are paid to an anyone-can-spend (OP_TRUE) output so the spend needs no
// signature - the maturity predicate is the only consensus check under test.
#include <boost/test/unit_test.hpp>
#include <boost/filesystem.hpp>
#include <cstdio>
#include <string>
#include <vector>

#include "db.h"
#include "txdb.h"
#include "main.h"
#include "miner.h"
#include "wallet.h"
#include "kernel.h"
#include "blockindex_authoritative_live.h"
#include "blockindex_authoritative_startup.h"
#include "blockindex_generation_builder.h"
#include "blockindex_generation_lifecycle.h"

namespace fs = boost::filesystem;
extern CWallet* pwalletMain;

// Build a PoW block whose coinbase pays a SPENDABLE (OP_TRUE) output; optionally
// append one extra (already-built) transaction.
static CBlock* B94BuildPoW(CBlockIndex* prev, unsigned int extra, const CTransaction* extraTx)
{
    CBlock* b = CreateNewBlock(pwalletMain, false, NULL, NULL);
    if (!b || b->vtx.empty() || b->vtx[0].vout.empty()) return NULL;
    b->nVersion = 1;
    b->nTime = std::max((unsigned int)GetTime(), (unsigned int)(prev->GetMedianTimePast() + 1));
    b->hashPrevBlock = *prev->phashBlock;
    b->vtx[0].vin[0].scriptSig = CScript() << (prev->nHeight + 1) << extra;
    b->vtx[0].vout[0].scriptPubKey = CScript() << OP_TRUE; // anyone-can-spend
    if (extraTx)
    {
        CTransaction t = *extraTx;
        t.nTime = b->nTime;
        b->vtx.push_back(t);
    }
    b->hashMerkleRoot = b->BuildMerkleTree();
    uint256 target = CBigNum().SetCompact(b->nBits).getuint256();
    while (b->GetHash() > target && b->nNonce < 0xffffffff)
        ++b->nNonce;
    return b;
}

static CBlockIndex* B94Mine(CBlockIndex* prev, unsigned int extra)
{
    CBlock* b = B94BuildPoW(prev, extra, NULL);
    BOOST_REQUIRE(b != NULL);
    CBlockIndex* idx = NULL;
    bool ok = false;
    {
        LOCK(cs_main);
        const uint256 h = b->GetHash();
        const bool c = b->CheckBlock(true, true, true);
        const bool p = c && ProcessBlock(NULL, b);
        if (p) { ok = true; idx = mapBlockIndex[h]; }
        else fprintf(stderr, "B94Mine FAIL prevH=%d check=%d process=%d orphan=%d\n",
                     prev->nHeight, (int)c, (int)p, (int)(mapOrphanBlocks.count(h) != 0));
    }
    delete b;
    BOOST_REQUIRE_MESSAGE(ok, "B94Mine: mined block not accepted by ProcessBlock");
    BOOST_REQUIRE(idx != NULL);
    return idx;
}

// Build + publish + select an isolated V2 generation from the current live txleveldb.
static void B94BuildRoot(const fs::path& root, std::string* error)
{
    fs::create_directories(root / "snapshot");
    { CTxDB db; db.Close(); }
    const auto live = GetDataDir() / "txleveldb";
    for (fs::directory_iterator it(live), end; it != end; ++it)
        if (fs::is_regular_file(it->path()))
            fs::copy_file(it->path(), root / "snapshot" / it->path().filename());
    BlockIndexGenerationSource source;
    BOOST_REQUIRE_MESSAGE(ReadLegacyBlockIndexSource((root / "snapshot").string(), &source, error), *error);
    source.blockDataDir = GetDataDir().string();
    BlockIndexGenerationBuilder builder;
    BOOST_REQUIRE_MESSAGE(builder.Build(source, (root / "blockindex-build-000001.tmp").string(), 1, NULL, error), *error);
    builder.Close();
    BOOST_REQUIRE_EQUAL(BlockIndexGenerationManager::PublishGeneration(root.string(), 1, error), BLOCK_INDEX_LIFECYCLE_OK);
    BOOST_REQUIRE_EQUAL(BlockIndexGenerationManager::SelectGeneration(root.string(), 1, error), BLOCK_INDEX_LIFECYCLE_OK);
}

// Construct a signed-optional spend of coinbase output (cbHash, 0) with an OP_TRUE
// scriptSig (valid against the OP_TRUE coinbase output above).
static CTransaction B94Spend(const uint256& cbHash, int64_t cbValue, int64_t fee)
{
    CTransaction t;
    t.nVersion = 1;
    t.vin.push_back(CTxIn(COutPoint(cbHash, 0), CScript(), 0xffffffff));
    CTxOut o;
    o.nValue = cbValue - fee;
    o.scriptPubKey = CScript() << OP_TRUE;
    t.vout.push_back(o);
    return t;
}

BOOST_AUTO_TEST_SUITE(blockindex_block94_maturity_tests)

BOOST_AUTO_TEST_CASE(coinbase_spend_mature_accepted_under_authority)
{
    // Only meaningful in the suite's regtest context (nCoinbaseMaturity is small).
    const int maturity = nCoinbaseMaturity;
    BOOST_REQUIRE(maturity > 0);

    // ---- 1. mine a chain tall enough that an early coinbase is mature ----
    CBlockIndex* tip = pindexBest;
    while (tip->nHeight < maturity + 4)
        tip = B94Mine(tip, 0x9000u + (unsigned)tip->nHeight);

    // source coinbase: a block >= maturity+1 below the current tip (mature).
    CBlockIndex* src = tip;
    for (int i = 0; i < maturity + 1 && src->pprev; ++i)
        src = src->pprev;
    CTransaction cb;
    {
        CBlock blk;
        BOOST_REQUIRE(blk.ReadFromDisk(src->nFile, src->nBlockPos, true));
        BOOST_REQUIRE(!blk.vtx.empty());
        cb = blk.vtx[0];
    }
    BOOST_REQUIRE(cb.IsCoinBase());
    const uint256 cbHash = cb.GetHash();
    const int64_t cbValue = cb.vout[0].nValue;

    // ---- 2. isolated V2 root + PRODUCTION authoritative startup ----
    const fs::path root = fs::temp_directory_path() / fs::unique_path("b94m-%%%%-%%%%");
    std::string error;
    B94BuildRoot(root, &error);

    CBlockIndex* savedBest = pindexBest;
    CBlockIndex* savedGenesis = pindexGenesisBlock;
    uint256 savedBestChain = hashBestChain;
    int savedBestHeight = nBestHeight;
    uint256 savedBestTrust = nBestChainTrust;
    const bool hadLivetail = mapArgs.count("-blockindexlivetail") != 0;
    const std::string savedLivetail = hadLivetail ? mapArgs["-blockindexlivetail"] : std::string();

    mapArgs["-blockindexlivetail"] = "32";
    BOOST_REQUIRE_MESSAGE(InitBlockIndexAuthoritative(root.string(), &error), error);
    BOOST_REQUIRE(g_fAuthoritativeStartup);
    BOOST_REQUIRE(GetAuthoritativeLiveAuthority() != NULL);

    // ---- 3. MATURE spend: must be ACCEPTED (pre-fix: rejected UNAVAILABLE) ----
    CBlockIndex* cur = pindexBest;
    const int spendHeight = cur->nHeight + 1;
    CTransaction matureSpend = B94Spend(cbHash, cbValue, 100000);
    CBlock* sb = B94BuildPoW(cur, 0x9911u, &matureSpend);
    BOOST_REQUIRE(sb != NULL);
    bool fChecked = false, fAccepted = false;
    {
        LOCK(cs_main);
        fChecked = sb->CheckBlock(true, true, true);
        fAccepted = fChecked && ProcessBlock(NULL, sb);
    }
    fprintf(stderr, "B94 REGRESSION: mature coinbase spend at height %d -> checked=%d accepted=%d\n",
            spendHeight, (int)fChecked, (int)fAccepted);
    delete sb;
    BOOST_CHECK_MESSAGE(fAccepted,
        "a MATURE coinbase spend must be ACCEPTED under V2 authoritative mode; "
        "pre-fix the maturity walk anchored at the un-published spender and failed closed");

    // ---- 4. IMMATURE spend: must still be REJECTED (consensus preserved) ----
    // Only meaningful when nCoinbaseMaturity > 1: with maturity == 1 the legacy
    // predicate compares depth 0 alone (the spender spending its own block's
    // coinbase), which a valid block can never do - so no immature case exists.
    if (maturity > 1)
    {
        CBlockIndex* par = pindexBest;
        CTransaction nearCb;
        {
            CBlock blk;
            BOOST_REQUIRE(blk.ReadFromDisk(par->nFile, par->nBlockPos, true));
            nearCb = blk.vtx[0];
        }
        CTransaction imm = B94Spend(nearCb.GetHash(), nearCb.vout[0].nValue, 100000);
        CBlock* ib = B94BuildPoW(par, 0x9912u, &imm);
        BOOST_REQUIRE(ib != NULL);
        bool fAcc2 = false;
        {
            LOCK(cs_main);
            fAcc2 = ib->CheckBlock(true, true, true) && ProcessBlock(NULL, ib);
        }
        delete ib;
        fprintf(stderr, "B94 REGRESSION: immature coinbase spend (depth 1, maturity %d) -> accepted=%d (expected 0)\n",
                maturity, (int)fAcc2);
        BOOST_CHECK_MESSAGE(!fAcc2, "an IMMATURE coinbase spend must be REJECTED");
    }
    else
    {
        fprintf(stderr, "B94 REGRESSION: immature case skipped (nCoinbaseMaturity==%d in this context)\n", maturity);
    }

    // ---- cleanup ----
    ResetBlockIndexAuthoritativeStartupForTest();
    pindexBest = savedBest;
    pindexGenesisBlock = savedGenesis;
    hashBestChain = savedBestChain;
    nBestHeight = savedBestHeight;
    nBestChainTrust = savedBestTrust;
    if (hadLivetail) mapArgs["-blockindexlivetail"] = savedLivetail;
    else mapArgs.erase("-blockindexlivetail");
    try { fs::remove_all(root); } catch (...) {}
}

BOOST_AUTO_TEST_SUITE_END()
