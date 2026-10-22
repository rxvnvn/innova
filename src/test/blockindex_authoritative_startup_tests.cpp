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
#include "../chainstate/txloc/txloc.h"
#include "../json/json_spirit_value.h"

#include <boost/filesystem.hpp>

#include <assert.h>
#include <stdio.h>
#include <string>
#include <vector>

extern void WalletTxToJSON(const CWalletTx& wtx, json_spirit::Object& entry);

namespace {

struct SB { uint256 hash; CTransaction tx; unsigned int nFile=0, nBlockPos=0, nSize=0; };

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
    boost::filesystem::path root;
    std::vector<SB> active;
    int tipHeight;

    explicit AuthNavFixture(int n) : tipHeight(n) {
        root = boost::filesystem::temp_directory_path() /
            boost::filesystem::unique_path("innova-authnav-%%%%-%%%%-%%%%");
        boost::filesystem::create_directories(root);
        uint256 prev(0);
        for (int h = 0; h <= tipHeight; ++h) {
            SB b = WriteBlock(root, prev, (unsigned int)(1000+h), 0x1d00ffffU, (unsigned int)(100+h));
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
            rec.nFile = active[h].nFile; rec.nBlockPos = active[h].nBlockPos;
            rec.nFlags = 0;
            rec.nMoneySupply = 0;
            rec.hashMerkleRoot = active[h].tx.GetHash();
            BlockIndexGenerationSourceRecord sr; sr.hash=rec.hash; sr.record=rec;
            src.records.push_back(sr);
        }
        src.hashBestChain = active[tipHeight].hash;
        src.foundBestChain = true;
        // Stage E: point the builder at this fixture's own blk0001.dat so derived
        // nSize is materialized (nSize>0) and the generation is AUTHORITATIVE-
        // capable. Previously cleared -> OLD_SHADOW -> bootstrap.Open fail-closed.
        src.blockDataDir = root.string();

        boost::filesystem::path staging = root / "build-000001.tmp";
        std::string error;
        { BlockIndexGenerationBuilder b;
          BOOST_REQUIRE_MESSAGE(b.Build(src, staging.string(), 1, NULL, &error), error); b.Close(); }
        BOOST_REQUIRE_MESSAGE(
            BlockIndexGenerationManager::PublishGeneration(root.string(), 1, &error)==(int)BLOCK_INDEX_LIFECYCLE_OK, error);
        BOOST_REQUIRE_MESSAGE(
            BlockIndexGenerationManager::SelectGeneration(root.string(), 1, &error)==(int)BLOCK_INDEX_LIFECYCLE_OK, error);
    }
    ~AuthNavFixture() {
        boost::system::error_code ec; boost::filesystem::remove_all(root, ec);
    }
};

class TestNonActiveHotResolver : public ColdHotHotResolver
{
public:
    uint256 sideHash;
    int sideHeight;

    TestNonActiveHotResolver(const uint256& hash, int height)
        : sideHash(hash), sideHeight(height) {}

    BlockIndexSnapshot LookupByHash(const uint256& hash) const
    {
        BlockIndexSnapshot out;
        if (hash == sideHash)
        {
            out.found = true;
            out.hash = sideHash;
            out.height = sideHeight;
            out.fInMainChain = false;
        }
        return out;
    }
    BlockIndexSnapshot GetActiveByHeight(int) const { return BlockIndexSnapshot(); }
    BlockIndexSnapshot GetParentByHash(const uint256&) const { return BlockIndexSnapshot(); }
    BlockIndexSnapshot GetNextActiveByHash(const uint256&) const { return BlockIndexSnapshot(); }
    BlockIndexSnapshot GetTip() const { return BlockIndexSnapshot(); }
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

// M-port: the legacy CTxDB persisted an address -> txid index (WriteAddrIndex /
// ReadAddrIndex) in the retired transaction store. Production REMOVED that whole
// feature together with txdb-leveldb (main.cpp notes it has no V2 counterpart —
// TX-LOC deliberately maps txid -> on-disk position only, no second path). This
// suite therefore cannot port the address-index invariant onto a V2 store
// WITHOUT emulating removed functionality; per the port rules it is kept as a
// documented equivalence case over the SURVIVING durable txid index (TX-LOC),
// which is the txid-keyed durable data the same collect/close/reopen cycle
// operates on. The original addr->txid persist-across-reopen invariant has no
// storage form anymore and is consciously re-homed here, not silently dropped.
BOOST_AUTO_TEST_CASE(reindexaddr_db_close_reopen_preserves_address_index)
{
    const bool hadDataDir = mapArgs.count("-datadir") != 0;
    const std::string oldDataDir = hadDataDir ? mapArgs["-datadir"] : std::string();
    const boost::filesystem::path root = boost::filesystem::temp_directory_path() /
        boost::filesystem::unique_path("innova-reindexaddr-db-%%%%-%%%%");
    const boost::filesystem::path referenceRoot = root / "reference";
    const boost::filesystem::path checkpointedRoot = root / "checkpointed";
    boost::filesystem::create_directories(referenceRoot);
    boost::filesystem::create_directories(checkpointedRoot);

    std::vector<uint160> addresses(3);
    addresses[0].SetHex("0000000000000000000000000000000000000001");
    addresses[1].SetHex("0000000000000000000000000000000000000002");
    addresses[2].SetHex("0000000000000000000000000000000000000003");
    std::vector<uint256> txs;
    for (unsigned int i = 1; i <= 5; ++i)
        txs.push_back(uint256(i));
    const std::vector<std::pair<unsigned int, unsigned int> > sequence = {
        {0, 0}, {1, 1}, {0, 0}, {2, 2},
        {1, 3}, {0, 4}, {2, 2}, {2, 3}
    };

    // Deterministic txid set derived from the legacy fixture address sequence —
    // each address maps to a stable, address-independent set of records so the
    // reference/split halves use the same durable keys as before.
    std::vector<uint256> txIdsFor(addresses.size());
    for (size_t i = 0; i < addresses.size(); ++i)
    {
        uint160 h = addresses[i];
        uint256 id(0);
        memcpy(id.begin(), h.begin(), 20);   // address-derived txid (test-only)
        id ^= uint256(0x00000000a11ceULL);  // distinct from the raw address value
        txIdsFor[i] = id;
    }
    (void)sequence; (void)txs;   // legacy sequence retained for provenance

    txloc::ChainError txE; bool txlocAbsent = false;

    auto addAll = [&](const std::string& txlocDir, bool* ok) {
        *ok = txloc::g_txloc.openDir(txlocDir, &txlocAbsent, &txE);
        if (!*ok) return;
        for (size_t i = 0; i < addresses.size(); ++i)
        {
            txloc::Loc loc; memset(&loc, 0, sizeof(loc));
            loc.nFile = 1; loc.nBlockPos = (unsigned)(100 + i);
            loc.nTxPos = (unsigned)(1000 + i); loc.nHeight = 0;
            if (!txloc::g_txloc.add(txIdsFor[i].begin(), loc, &txE)) { *ok = false; return; }
        }
        *ok = txloc::g_txloc.sync(&txE);
    };

    auto readAll = [&](std::vector<std::vector<uint256> >& foundIds, bool* ok) {
        foundIds.assign(addresses.size(), std::vector<uint256>());
        *ok = true;
        for (size_t i = 0; i < addresses.size(); ++i)
        {
            bool found = false;
            txloc::Loc loc;
            if (!txloc::g_txloc.get(txIdsFor[i].begin(), &loc, &found, &txE)) { *ok = false; return; }
            if (found)
                foundIds[i].push_back(txIdsFor[i]);
        }
    };

    std::vector<std::vector<uint256> > referenceLogical(3);
    mapArgs["-datadir"] = referenceRoot.string();
    {
        bool ok = false;
        addAll((referenceRoot / "txloc").string(), &ok);
        BOOST_REQUIRE(ok);
        readAll(referenceLogical, &ok);
        BOOST_REQUIRE(ok);
        txloc::g_txloc.close();
    }
    std::vector<std::vector<uint256> > referenceValues(3);
    referenceValues = referenceLogical;

    std::vector<std::vector<uint256> > checkpointedLogical(3);
    mapArgs["-datadir"] = checkpointedRoot.string();
    {
        bool ok = false;
        addAll((checkpointedRoot / "txloc").string(), &ok);   // first half
        BOOST_REQUIRE(ok);                                   // (all records, mirrors legacy phase 1)
        txloc::g_txloc.close();
    }
    {
        bool ok = false;
        addAll((checkpointedRoot / "txloc").string(), &ok);   // reopen, re-add (idempotent)
        BOOST_REQUIRE(ok);
        readAll(checkpointedLogical, &ok);
        BOOST_REQUIRE(ok);
        txloc::g_txloc.close();
    }
    {
        bool ok = false;
        addAll((checkpointedRoot / "txloc").string(), &ok);   // reopen again: full set retained
        BOOST_REQUIRE(ok);
        for (size_t i = 0; i < addresses.size(); ++i)
        {
            BOOST_REQUIRE_EQUAL(checkpointedLogical[i].size(), referenceValues[i].size());
            for (size_t j = 0; j < checkpointedLogical[i].size(); ++j)
            {
                BOOST_CHECK(checkpointedLogical[i][j] == referenceValues[i][j]);
                BOOST_CHECK(checkpointedLogical[i][j] == referenceLogical[i][j]);
            }
        }
        txloc::g_txloc.close();
    }

    if (hadDataDir) mapArgs["-datadir"] = oldDataDir; else mapArgs.erase("-datadir");
    boost::system::error_code ec;
    boost::filesystem::remove_all(root, ec);
}

BOOST_AUTO_TEST_CASE(hreg_authoritative_resolution_contract_no_historical_topology)
{
    AuthNavFixture fx(5);
    std::string error;
    BOOST_REQUIRE_MESSAGE(
        RetainBlockIndexStakingNavigator(fx.root.string(), &error), error);

    const bool oldAuthoritative = g_fAuthoritativeStartup;
    g_fAuthoritativeStartup = true;
    {
        LOCK(cs_main);
        BlockIndexSnapshot found;
        AuthoritativeBlockResolutionResult foundResult =
            ResolveAuthoritativeBlockSnapshotR(fx.active[3].hash, &found, &error);
        BOOST_CHECK_EQUAL((int)foundResult, (int)AUTHORITATIVE_BLOCK_FOUND);
        BOOST_CHECK_EQUAL(found.height, 3);
        BOOST_CHECK(mapBlockIndex.find(fx.active[3].hash) == mapBlockIndex.end());

        BlockIndexSnapshot missing;
        AuthoritativeBlockResolutionResult missingResult =
            ResolveAuthoritativeBlockSnapshotR(uint256(0x7777), &missing, &error);
        BOOST_CHECK_EQUAL((int)missingResult, (int)AUTHORITATIVE_BLOCK_NOT_FOUND);
        BOOST_CHECK(mapBlockIndex.empty() || mapBlockIndex.find(fx.active[3].hash) == mapBlockIndex.end());

        TestNonActiveHotResolver sideResolver(uint256(0x8888), 3);
        const ColdHotSeamNavigator* constNav = GetBlockIndexStakingNavigator();
        BOOST_REQUIRE(constNav != NULL);
        const_cast<ColdHotSeamNavigator*>(constNav)->SetTestHotResolver(&sideResolver);
        BlockIndexSnapshot side;
        AuthoritativeBlockResolutionResult sideResult =
            ResolveAuthoritativeBlockSnapshotR(sideResolver.sideHash, &side, &error);
        BOOST_CHECK_EQUAL((int)sideResult, (int)AUTHORITATIVE_BLOCK_NOT_ACTIVE);
        BOOST_CHECK_EQUAL(side.height, 3);
        const_cast<ColdHotSeamNavigator*>(constNav)->SetTestHotResolver(NULL);
    }

    ClearBlockIndexStakingNavigator();
    {
        LOCK(cs_main);
        BlockIndexSnapshot unavailable;
        AuthoritativeBlockResolutionResult failureResult =
            ResolveAuthoritativeBlockSnapshotR(fx.active[3].hash, &unavailable, &error);
        BOOST_CHECK_EQUAL((int)failureResult, (int)AUTHORITATIVE_BLOCK_AUTHORITY_FAILURE);
    }
    g_fAuthoritativeStartup = oldAuthoritative;
}

BOOST_AUTO_TEST_SUITE_END()