// REGRESSION — authoritative by-HEIGHT active-chain resolution must consult the
// mutable live tail, not only the immutable generation.
//
// Defect (fresh-IBD, v5.0.3): `AuthoritativeGetActiveSnapshotByHeight(height,...)`
// read ONLY the immutable cold generation (which on a fresh node holds just
// genesis), so `getblockhash <n>` failed with "Block height not found in
// authoritative active chain" for EVERY n>=1 while getblockcount/getbestblockhash
// (runtime scalars) were correct.
//
// Repair: the by-height path resolves from the immutable generation first, then
// falls back to the mutable authoritative live tail (BlockIndexTipAuthority::
// LookupActiveByHeight, ACTIVE members only), exactly mirroring the by-hash
// resolver's immutable-then-tail model; fail closed if neither source resolves.
//
// This test drives a real connectable regtest chain + production authoritative
// startup (create real blocks through ProcessBlock) and asserts:
//   1. an immutable/base height resolves;
//   2. a post-generation (live-tail) height resolves;
//   3. nonexistent/out-of-range heights fail closed;
//   4. the returned snapshot is the authoritative ACTIVE-chain record;
//   5. behavior is correct across the immutable -> live-tail boundary;
//   6. a SIDE record sharing a height is never selected in place of the active one.
#include <boost/test/unit_test.hpp>
#include <boost/filesystem.hpp>
#include <cstdio>
#include <string>

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

static CBlock* AHRBuildPoW(CBlockIndex* prev, unsigned int extra)
{
    CBlock* b = CreateNewBlock(pwalletMain, false, NULL, NULL);
    if (!b || b->vtx.empty() || b->vtx[0].vout.empty()) return NULL;
    b->nVersion = 1;
    b->nTime = std::max((unsigned int)GetTime(), (unsigned int)(prev->GetMedianTimePast() + 1));
    b->hashPrevBlock = *prev->phashBlock;
    b->vtx[0].vin[0].scriptSig = CScript() << (prev->nHeight + 1) << extra;
    b->vtx[0].vout[0].scriptPubKey = CScript() << OP_TRUE;
    b->hashMerkleRoot = b->BuildMerkleTree();
    uint256 target = CBigNum().SetCompact(b->nBits).getuint256();
    while (b->GetHash() > target && b->nNonce < 0xffffffff)
        ++b->nNonce;
    return b;
}

// Mine/accept a block whose parent is `prev` (may be a side/fork child). Returns
// the accepted index, or NULL if ProcessBlock rejected it.
static CBlockIndex* AHRMine(CBlockIndex* prev, unsigned int extra, bool mustAccept)
{
    CBlock* b = AHRBuildPoW(prev, extra);
    BOOST_REQUIRE(b != NULL);
    CBlockIndex* idx = NULL;
    bool ok = false;
    {
        LOCK(cs_main);
        const uint256 h = b->GetHash();
        const bool c = b->CheckBlock(true, true, true);
        const bool p = c && ProcessBlock(NULL, b);
        if (p) { ok = true; idx = mapBlockIndex[h]; }
    }
    delete b;
    if (mustAccept) { BOOST_REQUIRE_MESSAGE(ok, "block not accepted by ProcessBlock"); BOOST_REQUIRE(idx != NULL); }
    return ok ? idx : NULL;
}

static void AHRBuildRoot(const fs::path& root, std::string* error)
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

BOOST_AUTO_TEST_SUITE(blockindex_activeheight_resolver_tests)

BOOST_AUTO_TEST_CASE(active_snapshot_by_height_base_plus_live_tail)
{
    // ---- 1. baseline connectable chain ----
    CBlockIndex* tip = pindexBest;
    while (tip->nHeight < 6)
        tip = AHRMine(tip, 0xA000u + (unsigned)tip->nHeight, true);
    uint256 hashAtHeight4 = uint256(0);
    {
        CBlockIndex* p = tip;
        while (p && p->nHeight > 4) p = p->pprev;
        BOOST_REQUIRE(p != NULL);
        hashAtHeight4 = p->GetBlockHash();
    }

    // ---- 2. isolated V2 root + production authoritative startup ----
    const fs::path root = fs::temp_directory_path() / fs::unique_path("ahr-%%%%-%%%%");
    std::string error;
    AHRBuildRoot(root, &error);

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

    const int S = pindexBest->nHeight;   // immutable base tip height

    BlockIndexSnapshot s;

    // (1) immutable/base height resolves, and is the ACTIVE record.
    BOOST_REQUIRE_MESSAGE(AuthoritativeGetActiveSnapshotByHeight(4, &s),
        "base-generation height 4 must resolve");
    BOOST_CHECK_MESSAGE(s.fInMainChain, "base height 4 must be the active record");
    BOOST_CHECK_MESSAGE(s.hash == hashAtHeight4,
        "base height 4 must equal the authoritative active block at height 4");

    // (5a) base tip itself resolves.
    BOOST_CHECK_MESSAGE(AuthoritativeGetActiveSnapshotByHeight(S, &s),
        "the immutable base tip height must resolve");

    // (2) post-generation (live-tail) height resolves after a real accept.
    CBlockIndex* n1 = AHRMine(pindexBest, 0xA100u, true);      // height S+1
    BOOST_REQUIRE_EQUAL(n1->nHeight, S + 1);
    BOOST_REQUIRE_MESSAGE(AuthoritativeGetActiveSnapshotByHeight(n1->nHeight, &s),
        "a live-tail height (S+1) must resolve after accept");
    BOOST_CHECK_MESSAGE(s.fInMainChain, "live-tail height must be active");
    BOOST_CHECK_MESSAGE(s.hash == n1->GetBlockHash(),
        "live-tail height must equal the newly accepted active block");

    // (5b) boundary: S and S+1 resolve; S+2 (not yet accepted) fails.
    BOOST_CHECK(AuthoritativeGetActiveSnapshotByHeight(S, &s));
    BOOST_CHECK(AuthoritativeGetActiveSnapshotByHeight(S + 1, &s));
    BOOST_CHECK_MESSAGE(!AuthoritativeGetActiveSnapshotByHeight(S + 2, &s),
        "an unconsumed height above the tip must fail closed");

    // (3) nonexistent / out-of-range heights fail closed.
    BOOST_CHECK_MESSAGE(!AuthoritativeGetActiveSnapshotByHeight(-1, &s), "negative height must fail");
    BOOST_CHECK_MESSAGE(!AuthoritativeGetActiveSnapshotByHeight(1000000, &s), "far-future height must fail");

    // (4)/(6) a SIDE block sharing a height must never be selected.
    CBlockIndex* n2 = AHRMine(n1, 0xA200u, true);              // best child, height S+2
    BOOST_REQUIRE_EQUAL(n2->nHeight, S + 2);
    BOOST_REQUIRE(n2 == pindexBest);
    CBlockIndex* side = AHRMine(n1, 0xA201u, false);           // competing child of n1, SAME height
    BOOST_REQUIRE(side != NULL);
    BOOST_REQUIRE_EQUAL(side->nHeight, n2->nHeight);
    BOOST_REQUIRE(side->GetBlockHash() != n2->GetBlockHash());
    BOOST_REQUIRE_MESSAGE(AuthoritativeGetActiveSnapshotByHeight(n2->nHeight, &s),
        "height S+2 must resolve");
    BOOST_CHECK_MESSAGE(s.fInMainChain, "height S+2 must be active");
    BOOST_CHECK_MESSAGE(s.hash == n2->GetBlockHash(),
        "by-height lookup must select the ACTIVE record, not the same-height side record");
    BOOST_CHECK_MESSAGE(s.hash != side->GetBlockHash(),
        "by-height lookup must never return the side record");

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
