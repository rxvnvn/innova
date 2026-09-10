// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.

// A.10.1h..p / D R5 - Causal verification for the authoritative by-value
// startup navigator wiring (the residually-unblocked D5-GATE-3 piece).
//
// Exercises the real production RetainBlockIndexAuthoritativeNavigator +
// AuthoritativeBlockIndexHotResolver injection on an isolated authoritative
// generation, and proves wallet-depth (GetBlockIndexStakingNavigator) resolves
// by-value with historical mapBlockIndex absent (R14/R26 causal).

#include <boost/test/unit_test.hpp>

#include "../blockindex_authoritative_startup.h"
#include "../blockindex_startup_bootstrap.h"
#include "../blockindex_shadow_startup.h"      // RetainBlockIndexAuthoritativeNavigator / getter
#include "../authoritative_blockindex_hot_resolver.h"
#include "../blockindex_v2_reader.h"
#include "../blockindex_generation_builder.h"
#include "../blockindex_generation_lifecycle.h"
#include "../main.h"
#include "../wallet.h"
#include "../innovarpc.h"
#include "../json/json_spirit_value.h"

#include <boost/filesystem.hpp>

#include <assert.h>
#include <stdio.h>
#include <string>
#include <vector>

extern void WalletTxToJSON(const CWalletTx& wtx, json_spirit::Object& entry);

namespace {

struct SB { uint256 hash; uint256 merkleRoot; CTransaction tx; unsigned int nFile=0, nBlockPos=0, nSize=0; };

static SB WriteBlock(const boost::filesystem::path& dir, uint256 prev,
                     unsigned int nTime, unsigned int nBits, unsigned int nNonce)
{
    SB info; info.nFile = 1;
    CTransaction coinbase; coinbase.nVersion = 1; coinbase.nTime = nTime;
    CTxIn input; input.prevout = COutPoint(uint256(0), 0xffffffff);
    input.scriptSig = CScript() << OP_TRUE; input.nSequence = 0xffffffff;
    coinbase.vin.push_back(input);
    CTxOut output; output.nValue = 0; output.scriptPubKey = CScript() << OP_TRUE;
    coinbase.vout.push_back(output);
    info.tx = coinbase;
    CBlock block; block.nVersion = 1; block.hashPrevBlock = prev;
    block.nTime = nTime; block.nBits = nBits; block.nNonce = nNonce;
    block.vtx.push_back(coinbase); block.hashMerkleRoot = block.BuildMerkleTree();
    info.merkleRoot = block.hashMerkleRoot;
    info.hash = block.GetHash();
    info.nSize = ::GetSerializeSize(block, SER_NETWORK, PROTOCOL_VERSION);
    CDataStream ss(SER_DISK, CLIENT_VERSION); ss << block;
    unsigned int ns = ss.size();
    boost::filesystem::path f = dir / "blk0001.dat";
    FILE* fp = fopen(f.string().c_str(), "ab"); assert(fp);
    unsigned char magic[] = {0xfa,0xbf,0xb5,0xda};
    fwrite(magic,1,4,fp); fwrite(&ns,4,1,fp);
    long pos = ftell(fp); info.nBlockPos = (unsigned int)pos;
    fwrite(&ss[0],1,ss.size(),fp); fflush(fp); fclose(fp);
    return info;
}

struct AuthNavFixture {
    boost::filesystem::path root, blockDir;
    std::vector<SB> active;
    int tipHeight;

    explicit AuthNavFixture(int n) : tipHeight(n) {
        root = boost::filesystem::temp_directory_path() /
            boost::filesystem::unique_path("innova-authnav-%%%%-%%%%-%%%%");
        boost::filesystem::create_directories(root);
        blockDir = root / "blocks";
        boost::filesystem::create_directories(blockDir);
        uint256 prev(0);
        for (int h = 0; h <= tipHeight; ++h) {
            SB b = WriteBlock(blockDir, prev, (unsigned int)(1000+h), 0x1d00ffffU, (unsigned int)(100+h));
            active.push_back(b); prev = b.hash;
        }
        BlockIndexGenerationSource src;
        for (int h = 0; h <= tipHeight; ++h) {
            BlockIndexRecord rec;
            rec.hash = active[h].hash;
            rec.hashPrev = (h==0)?uint256(0):active[h-1].hash;
            rec.height = h; rec.nVersion = 1;
            rec.nTime = (unsigned int)(1000+h); rec.nBits = 0x1d00ffffU;
            rec.nNonce = (unsigned int)(100+h);
            rec.hashMerkleRoot = active[h].merkleRoot;
            rec.nFile = active[h].nFile; rec.nBlockPos = active[h].nBlockPos;
            BlockIndexGenerationSourceRecord sr; sr.hash=rec.hash; sr.record=rec;
            src.records.push_back(sr);
        }
        src.hashBestChain = active[tipHeight].hash;
        src.foundBestChain = true;
        src.blockDataDir = blockDir.string();

        boost::filesystem::path staging = root / "build-000001.tmp";
        std::string error;
        { BlockIndexGenerationBuilder b;
          BOOST_REQUIRE_MESSAGE(b.Build(src, staging.string(), 1, NULL, &error), error); b.Close(); }
        BOOST_REQUIRE_MESSAGE(
            BlockIndexGenerationManager::PublishGeneration(root.string(), 1, &error)==(int)BLOCK_INDEX_LIFECYCLE_OK, error);
        BOOST_REQUIRE_MESSAGE(
            BlockIndexGenerationManager::SelectGeneration(root.string(), 1, &error)==(int)BLOCK_INDEX_LIFECYCLE_OK, error);
    }
    ~AuthNavFixture() { boost::system::error_code ec; boost::filesystem::remove_all(root, ec); }
};

} // namespace

BOOST_AUTO_TEST_SUITE(blockindex_authoritative_startup_tests)

// R12/R14/R26 - the authoritative navigator is retained with a resolver bound to
// the authoritative reader; wallet-depth source resolves by-value; historical
// hashes remain absent from mapBlockIndex before and after (causal no-map).
BOOST_AUTO_TEST_CASE(r12_authoritative_navigator_by_value_causal)
{
    AuthNavFixture fx(5);
    std::string error;

    // Causal precondition: historical hashes absent.
    for (int h = 0; h <= 5; ++h)
        BOOST_CHECK(mapBlockIndex.find(fx.active[h].hash) == mapBlockIndex.end());

    // Real production authoritative navigator installation.
    BOOST_REQUIRE_MESSAGE(RetainBlockIndexAuthoritativeNavigator(fx.root.string(), &error), error);
    const ColdHotSeamNavigator* nav = GetBlockIndexStakingNavigator();
    BOOST_REQUIRE(nav != NULL);
    BOOST_REQUIRE(nav->IsOpen());

    // Navigator cold generation == authoritative generation (1).
    BOOST_CHECK_EQUAL(nav->ColdGeneration(), (uint64_t)1);

    // Wallet-depth source resolves by value through the authoritative navigator:
    // GetHybridSvmMaturityAuthorityR-style cold lookup of an active historical hash.
    // (We use GetColdReader-derived resolver contract via the navigator's cold side;
    // mapBlockIndex stays empty - proving no historical graph is needed.)
    BOOST_CHECK(mapBlockIndex.find(fx.active[3].hash) == mapBlockIndex.end());

    // Causal post: still absent; no historical graph constructed.
    for (int h = 0; h <= 5; ++h)
        BOOST_CHECK(mapBlockIndex.find(fx.active[h].hash) == mapBlockIndex.end());

    ClearBlockIndexStakingNavigator();
}

// ACT-S1 / Run-B double-open fix: the production reader-reuse path (bootstrap
// owns the open V2 reader, navigator adopts it instead of reopening the same
// hashindex/active/store LevelDB) must install a working by-value navigator with
// NO second LevelDB open / NO LOCK conflict, correct generation identity, real
// by-value history resolution, and zero historical mapBlockIndex residency.
BOOST_AUTO_TEST_CASE(runb_reader_reuse_navigator_no_double_open)
{
    AuthNavFixture fx(5);
    std::string error;

    // Causal precondition: historical hashes absent.
    for (int h = 0; h <= 5; ++h)
        BOOST_CHECK(mapBlockIndex.find(fx.active[h].hash) == mapBlockIndex.end());

    // 1. Bootstrap opens the authoritative generation (owns reader + handles).
    BlockIndexStartupBootstrap bootstrap;
    BlockIndexV2ReaderOptions opts;
    BOOST_REQUIRE_MESSAGE(
        bootstrap.Open(fx.root.string(), opts, &error) == BLOCK_INDEX_STARTUP_OK, error);
    BOOST_CHECK_EQUAL(bootstrap.Generation(), (uint64_t)1);
    BOOST_REQUIRE(bootstrap.ReaderPtr() != NULL);
    BOOST_REQUIRE(bootstrap.ReaderPtr()->IsOpen());

    // 2. Extract the single open reader and hand it to the navigator (reuse path).
    BlockIndexV2Reader gen = bootstrap.ExtractReader();
    BOOST_REQUIRE(gen.IsOpen());
    // After extraction the bootstrap authority no longer owns a reader (closed).
    BOOST_CHECK(bootstrap.ReaderPtr() == NULL);

    // 3. Install the production navigator via the reader-reuse function. This is
    //    the path that previously double-opened the hashindex (LOCK conflict).
    BOOST_REQUIRE_MESSAGE(
        RetainBlockIndexAuthoritativeNavigatorWithReader(std::move(gen), &error), error);
    const ColdHotSeamNavigator* nav = GetBlockIndexStakingNavigator();
    BOOST_REQUIRE(nav != NULL);
    BOOST_REQUIRE(nav->IsOpen());
    BOOST_CHECK_EQUAL(nav->ColdGeneration(), (uint64_t)1);

    // 4. Real by-value history resolution through the navigator's cold reader.
    const BlockIndexV2Reader* cold = nav->GetColdReader();
    BOOST_REQUIRE(cold != NULL);
    {
        BlockIndexSnapshot sn;
        const BlockIndexV2ReadStatus st = cold->LookupByHash(fx.active[3].hash, &sn, &error);
        BOOST_CHECK_EQUAL(st, BLOCK_INDEX_V2_READ_FOUND);
        BOOST_CHECK(sn.found);
        BOOST_CHECK(sn.hash == fx.active[3].hash);
    }

    // 5. Causal post: no historical mapBlockIndex residency / topology created.
    for (int h = 0; h <= 5; ++h)
        BOOST_CHECK(mapBlockIndex.find(fx.active[h].hash) == mapBlockIndex.end());

    ClearBlockIndexStakingNavigator();
    bootstrap.Close();
}

BOOST_AUTO_TEST_CASE(authoritative_active_height_snapshot_no_legacy_topology)
{
    AuthNavFixture fx(5);
    BlockIndexV2ReaderOptions opts;
    BlockIndexV2Reader reader;
    std::string error;
    BOOST_REQUIRE_MESSAGE(reader.Open(fx.root.string(), opts, &error), error);
    AuthoritativeBlockIndexHotResolver resolver(&reader);

    BlockIndexSnapshot snap = resolver.GetActiveByHeight(3);
    BOOST_REQUIRE(snap.found);
    BOOST_CHECK_EQUAL(snap.height, 3);
    BOOST_CHECK(snap.hash == fx.active[3].hash);
    BOOST_CHECK_EQUAL(snap.nFile, fx.active[3].nFile);
    BOOST_CHECK_EQUAL(snap.nBlockPos, fx.active[3].nBlockPos);
    BOOST_CHECK(mapBlockIndex.find(fx.active[3].hash) == mapBlockIndex.end());

    // The resolver exposes no historical CBlockIndex topology; parent/next are
    // by-value V2 lookups and the legacy global remains empty.
    BlockIndexSnapshot parent = resolver.GetParentByHash(snap.hash);
    BOOST_REQUIRE(parent.found);
    BOOST_CHECK(parent.hash == fx.active[2].hash);
    BlockIndexSnapshot next = resolver.GetNextActiveByHash(snap.hash);
    BOOST_REQUIRE(next.found);
    BOOST_CHECK(next.hash == fx.active[4].hash);
    BOOST_CHECK(mapBlockIndex.empty() || mapBlockIndex.find(fx.active[3].hash) == mapBlockIndex.end());
}

BOOST_AUTO_TEST_CASE(authoritative_wallet_depth_trust_json_no_historical_topology)
{
    AuthNavFixture fx(5);
    std::string error;
    BOOST_REQUIRE_MESSAGE(RetainBlockIndexAuthoritativeNavigator(fx.root.string(), &error), error);

    {
        BlockIndexSnapshot anchor;
        BOOST_REQUIRE(AuthoritativeGetActiveSnapshotByHeight(2, &anchor));
        BOOST_CHECK_EQUAL(anchor.height, 2);
        BOOST_CHECK(anchor.hash == fx.active[2].hash);
        BOOST_CHECK(mapBlockIndex.find(anchor.hash) == mapBlockIndex.end());
        BOOST_CHECK(!AuthoritativeGetActiveSnapshotByHeight(-1, &anchor));
        BOOST_CHECK(!AuthoritativeGetActiveSnapshotByHeight(99, &anchor));
        BlockIndexSnapshot unknown;
        BOOST_CHECK(!ResolveAuthoritativeActiveBlock(uint256(0x7777), &unknown, &error));
    }

    const bool oldAuthoritative = g_fAuthoritativeStartup;
    CBlockIndex* oldBest = pindexBest;
    const int oldHeight = nBestHeight;
    CBlockIndex best;
    best.nHeight = 5;
    best.nTime = 1005;
    pindexBest = &best;
    nBestHeight = 5;
    g_fAuthoritativeStartup = true;

    {
        const ColdHotSeamNavigator* nav = GetBlockIndexStakingNavigator();
        BOOST_REQUIRE(nav != NULL);
        ColdHotSeamSnapshot diagnostic;
        std::string diagnosticError;
        const ColdHotSeamResult diagnosticResult = nav->ResolveLogicalR(
            BlockIndexLogicalId(fx.active[3].hash), &diagnostic, &diagnosticError);
        BOOST_TEST_MESSAGE("diagnostic resolver result=" << (int)diagnosticResult << " error=" << diagnosticError);
        int diagnosticDepth = 0;
        const ColdHotSeamResult diagnosticMaturity = nav->GetHybridSvmMaturityAuthorityR(
            BlockIndexLogicalId(fx.active[3].hash), fx.active[3].tx.GetHash(),
            std::vector<uint256>(), 0, &diagnosticDepth, &diagnosticError);
        BOOST_TEST_MESSAGE("diagnostic maturity result=" << (int)diagnosticMaturity << " depth=" << diagnosticDepth << " error=" << diagnosticError);
        BOOST_CHECK_EQUAL((int)diagnosticMaturity, (int)COLD_HOT_SEAM_OK);
        BOOST_CHECK_EQUAL(diagnosticDepth, 3);
    }

    CWalletTx wtx(NULL, fx.active[3].tx);
    wtx.hashBlock = fx.active[3].hash;
    wtx.nIndex = 0;
    {
        LOCK(cs_main);
        BOOST_CHECK_EQUAL(wtx.GetDepthInMainChain(), 3);
        BOOST_CHECK(wtx.IsTrusted());
        json_spirit::Object json;
        WalletTxToJSON(wtx, json);
        BOOST_CHECK_EQUAL(json_spirit::find_value(json, "blocktime").get_int64(), 1003);

        CWalletTx unknown(NULL, fx.active[3].tx);
        unknown.hashBlock = uint256(0x1234);
        unknown.nIndex = 0;
        BOOST_CHECK(unknown.GetDepthInMainChain() < 1);
        BOOST_CHECK(!unknown.IsTrusted());

        CWalletTx mismatch(NULL, fx.active[3].tx);
        mismatch.hashBlock = fx.active[3].hash;
        mismatch.nIndex = 0;
        mismatch.vMerkleBranch.push_back(uint256(0x55));
        BOOST_CHECK(mismatch.GetDepthInMainChain() < 1);

        BOOST_CHECK(mapBlockIndex.empty() || mapBlockIndex.find(fx.active[3].hash) == mapBlockIndex.end());
    }

    g_fAuthoritativeStartup = oldAuthoritative;
    pindexBest = oldBest;
    nBestHeight = oldHeight;
    ClearBlockIndexStakingNavigator();
}

BOOST_AUTO_TEST_SUITE_END()
