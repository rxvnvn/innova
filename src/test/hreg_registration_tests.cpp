// Copyright (c) 2026 The Innova developers
// Distributed under the MIT/X11 software license.

#include <boost/test/unit_test.hpp>
#include <algorithm>
#include <boost/filesystem.hpp>

#include "../hreg_registration.h"
#include "../script.h"
#include "../util.h"
#include "../blockindex_authoritative_startup.h"
#include "../blockindex_authoritative_live.h"
#include "../blockindex_generation_builder.h"
#include "../blockindex_generation_lifecycle.h"
#include "../blockindex_shadow_startup.h"
// M-port: the legacy txdb-leveldb header was removed from this tree. This suite's
// assertions run entirely through the V2 surfaces (TX-LOC seeded by the fixture,
// the authoritative BI-V2 block snapshot resolver); no legacy CTxDB call remains.
#include "../chainstate/txloc/txloc.h"

using namespace hreg;

namespace {

static const int64_t HREG_COLLATERAL = 25000LL * COIN;

CKeyID KeyId(unsigned char fill)
{
    uint160 h = 0;
    memset(h.begin(), fill, 20);
    return CKeyID(h);
}

CScript P2PKHScript(unsigned char fill)
{
    return GetScriptForDestination(KeyId(fill));
}

CTxOut MakePrevOut(int64_t value, const CScript& spk)
{
    CTxOut out;
    out.nValue = value;
    out.scriptPubKey = spk;
    return out;
}

CTransaction MakePrevTx(const std::vector<CTxOut>& vouts, bool fCoinbase=false, bool fCoinstake=false)
{
    CTransaction tx;
    tx.nVersion = CTransaction::CURRENT_VERSION;
    tx.nTime = 1000;
    if (fCoinbase) {
        tx.vin.push_back(CTxIn(COutPoint(), CScript() << OP_1));
    } else if (fCoinstake) {
        tx.vin.push_back(CTxIn(uint256(1), 0, CScript() << OP_1));
    } else {
        tx.vin.push_back(CTxIn(uint256(2), 0, CScript() << OP_1));
    }
    tx.vout = vouts;
    return tx;
}

void PutPrev(MapPrevTx& mp, const CTransaction& tx)
{
    CTxIndex idx(CDiskTxPos(0,0,0), tx.vout.size());
    mp[tx.GetHash()] = std::make_pair(idx, tx);
}

CScript MarkerScript()
{
    CScript s;
    std::vector<unsigned char> payload;
    payload.push_back('I'); payload.push_back('N'); payload.push_back('C'); payload.push_back('N');
    payload.push_back(0x01); payload.push_back(0x01);
    s << OP_RETURN << payload;
    return s;
}

CTransaction MakeRegistrationTx(const CTransaction& prev, unsigned int prevN,
                                bool marker=true,
                                bool duplicateMarker=false,
                                bool duplicateMatchingCollateral=false,
                                bool changeScript=false,
                                int64_t recreatedValue=HREG_COLLATERAL,
                                int64_t changeValue=1*COIN,
                                bool addExtraFeeInput=false,
                                const CTransaction* prevFeeTx=NULL,
                                unsigned int prevFeeN=0)
{
    CTransaction tx;
    tx.nVersion = CTransaction::CURRENT_VERSION;
    tx.nTime = 2000;
    tx.vin.push_back(CTxIn(prev.GetHash(), prevN, CScript() << OP_1));
    if (addExtraFeeInput && prevFeeTx)
        tx.vin.push_back(CTxIn(prevFeeTx->GetHash(), prevFeeN, CScript() << OP_1));
    tx.vout.push_back(CTxOut(recreatedValue, changeScript ? P2PKHScript(0x44) : prev.vout[prevN].scriptPubKey));
    if (duplicateMatchingCollateral)
        tx.vout.push_back(CTxOut(recreatedValue, changeScript ? P2PKHScript(0x44) : prev.vout[prevN].scriptPubKey));
    if (changeValue > 0)
        tx.vout.push_back(CTxOut(changeValue, P2PKHScript(0x55)));
    if (marker)
        tx.vout.push_back(CTxOut(0, MarkerScript()));
    if (duplicateMarker)
        tx.vout.push_back(CTxOut(0, MarkerScript()));
    return tx;
}


bool HasState(const std::vector<RegisteredCollateralState>& v, const COutPoint& op, RegistrationState st)
{
    for (size_t i=0;i<v.size();++i)
        if (v[i].outpoint == op && v[i].state == st) return true;
    return false;
}

// One test case == ONE isolated V2 chain context (single authoritative
// generation, one contiguous 0..N chain, the real blocks the case needs, one
// TX-LOC lifetime, one staking-navigator lifetime).
struct HRegBlockSpec
{
    int height;
    std::vector<CTransaction> txs;
};

struct RealHRegDiskFixture
{
    boost::filesystem::path root;
    CTransaction prev;
    uint256 blockHash;
    unsigned int blockFile;
    unsigned int blockPos;
    bool hadDataDir;
    std::string oldDataDir;
    bool oldAuthoritative;
    // M1: per-context authority. The reader must outlive the authority, and the
    // authority must be UNINSTALLED before either is destroyed so no global
    // pointer can reference a dead reader/materializer.
    std::unique_ptr<BlockIndexAuthoritativeLive> authority;
    BlockIndexAuthoritativeLive* savedAuthority;

    explicit RealHRegDiskFixture(const std::vector<HRegBlockSpec>& specs =
                                     std::vector<HRegBlockSpec>())
        : blockFile(0), blockPos(0),
          hadDataDir(mapArgs.count("-datadir") != 0),
          oldDataDir(hadDataDir ? mapArgs["-datadir"] : std::string()),
          oldAuthoritative(g_fAuthoritativeStartup),
          savedAuthority(NULL)
    {
        root = boost::filesystem::temp_directory_path() /
            boost::filesystem::unique_path("innova-hreg-real-%%%%-%%%%");
        boost::filesystem::create_directories(root);
        mapArgs["-datadir"] = root.string();
        // GetDataDir() caches its path; a bare mapArgs write does NOT invalidate the
        // cache, so later cases would keep writing/reading the FIRST case's datadir.
        // The explicit test override makes both GetDataDir variants resolve to THIS
        // context for the case's lifetime.
        SetDataDirForTest(root);
        // The block-file append handle is process-global and CACHED (FCloseBlockFile()
        // exists precisely for this). Without invalidating it, the first WriteToDisk of
        // this context appends into the handle opened for the PREVIOUS datadir (the test
        // framework's regtest dir, or a previous case's root), so the block lands in
        // another directory while TX-LOC/readers resolve inside this one -> every
        // GetTransaction() round-trip fails. Drop it before building any block.
        FCloseBlockFile();

        prev = MakePrevTx(std::vector<CTxOut>(1,
            MakePrevOut(HREG_COLLATERAL, P2PKHScript(0x11))));

        std::vector<HRegBlockSpec> chain = specs;
        if (chain.empty()) { HRegBlockSpec s0; s0.height = 0; s0.txs.push_back(prev); chain.push_back(s0); }
        std::sort(chain.begin(), chain.end(),
                  [](const HRegBlockSpec& a, const HRegBlockSpec& b) { return a.height < b.height; });

        struct RealRec { uint256 hash; unsigned int file; unsigned int pos;
                         int32_t nTime; int32_t nVersion; uint32_t nBits; uint32_t nNonce; };
        std::map<int, RealRec> reals;

        { txloc::ChainError txE; bool txlocAbsent = false;
          BOOST_REQUIRE_MESSAGE(txloc::g_txloc.openDir((root / "txloc").string(), &txlocAbsent, &txE),
                                "hreg txloc open: " << txE.str()); }

        uint256 lastHash(0);
        for (size_t i = 0; i < chain.size(); ++i)
        {
            const HRegBlockSpec& spec = chain[i];
            CTransaction coinbase = MakePrevTx(
                std::vector<CTxOut>(1, MakePrevOut(0, CScript() << OP_TRUE)), true);
            if (spec.height > 0)
            {
                coinbase.nTime = 1000 + spec.height;
                coinbase.vin[0].scriptSig = CScript() << (spec.height + 1);
            }

            CBlock block;
            block.nVersion = 1;
            block.nTime = spec.height > 0 ? (1000 + spec.height) : 1000;
            block.nBits = 0x207fffffU;
            block.nNonce = spec.height > 0 ? (100 + spec.height) : 100;
            block.hashPrevBlock = lastHash;
            block.vtx.push_back(coinbase);
            for (size_t t = 0; t < spec.txs.size(); ++t)
                block.vtx.push_back(spec.txs[t]);
            block.hashMerkleRoot = block.BuildMerkleTree();

            unsigned int file = 0, pos = 0;
            BOOST_REQUIRE(block.WriteToDisk(file, pos));
            const unsigned int firstTxPos = pos +
                ::GetSerializeSize(CBlock(), SER_DISK, CLIENT_VERSION) -
                (2 * GetSizeOfCompactSize(0)) +
                GetSizeOfCompactSize(block.vtx.size());
            unsigned int txPos = firstTxPos;
            for (size_t t = 0; t < block.vtx.size(); ++t)
            {
                if (t > 0)
                {
                    txloc::ChainError txE; txloc::Loc loc;
                    loc.nFile = file; loc.nBlockPos = pos; loc.nTxPos = txPos; loc.nHeight = spec.height;
                    BOOST_REQUIRE_MESSAGE(txloc::g_txloc.add(block.vtx[t].GetHash().begin(), loc, &txE),
                                          "hreg txloc add: " << txE.str());
                }
                txPos += ::GetSerializeSize(block.vtx[t], SER_DISK, CLIENT_VERSION);
            }
            txloc::ChainError se;
            BOOST_REQUIRE_MESSAGE(txloc::g_txloc.sync(&se), "hreg txloc sync: " << se.str());

            RealRec rr; rr.hash = block.GetHash(); rr.file = file; rr.pos = pos;
            rr.nTime = block.nTime; rr.nVersion = block.nVersion; rr.nBits = block.nBits; rr.nNonce = block.nNonce;
            reals[spec.height] = rr;
            lastHash = rr.hash;
            if (i + 1 == chain.size()) { blockHash = rr.hash; blockFile = file; blockPos = pos; }
        }

        const int maxHeight = chain.back().height;
        BlockIndexGenerationSource source;
        uint256 hashPrev(0);
        for (int h = 0; h <= maxHeight; ++h)
        {
            BlockIndexRecord record;
            std::map<int, RealRec>::const_iterator ri = reals.find(h);
            const bool isReal = (ri != reals.end());
            record.hash = isReal ? ri->second.hash : uint256(0xC0DE0000ULL + (unsigned)h);
            record.hashPrev = hashPrev;
            record.height = h;
            record.nVersion = isReal ? ri->second.nVersion : 1;
            record.nTime = isReal ? (uint32_t)ri->second.nTime : (uint32_t)(1000 + h);
            record.nBits = isReal ? ri->second.nBits : 0x207fffffU;
            record.nNonce = isReal ? ri->second.nNonce : 0;
            record.nFile = isReal ? ri->second.file : 0;
            record.nBlockPos = isReal ? ri->second.pos : 0;
            record.nFlags = 0;
            record.nMoneySupply = 0;
            BlockIndexGenerationSourceRecord sr;
            sr.hash = record.hash;
            sr.record = record;
            source.records.push_back(sr);
            hashPrev = record.hash;
        }
        source.hashBestChain = lastHash;
        source.foundBestChain = true;
        source.blockDataDir.clear();

        std::string error;
        boost::filesystem::path staging = root / "build-000001.tmp";
        { BlockIndexGenerationBuilder builder;
          BOOST_REQUIRE_MESSAGE(builder.Build(source, staging.string(), 1, NULL, &error), error);
          builder.Close(); }
        BOOST_REQUIRE_MESSAGE(
            BlockIndexGenerationManager::PublishGeneration(root.string(), 1, &error) ==
                (int)BLOCK_INDEX_LIFECYCLE_OK, error);
        BOOST_REQUIRE_MESSAGE(
            BlockIndexGenerationManager::SelectGeneration(root.string(), 1, &error) ==
                (int)BLOCK_INDEX_LIFECYCLE_OK, error);

    }

    void OpenAuthority()
    {
        std::string error;
        BOOST_REQUIRE_MESSAGE(
            RetainBlockIndexStakingNavigator(root.string(), &error), error);
        // M1: the authority's base reader is the navigator's cold reader (the
        // navigator already opened this generation; opening a second reader on the
        // same LevelDB would deadlock on its LOCK file). The authority owns the
        // live-tail materializer over that reader; both live exactly as long as
        // this context.
        const BlockIndexV2Reader* cold = GetAuthoritativeNavigatorReader();
        BOOST_REQUIRE_MESSAGE(cold != NULL, "authoritative navigator has no cold reader");
        authority.reset(new BlockIndexAuthoritativeLive());
        BOOST_REQUIRE_MESSAGE(authority->Open(root.string(), cold, 2048, &error), error);
        savedAuthority = GetAuthoritativeLiveAuthority();
        SetAuthoritativeLiveForTesting(authority.get());
        g_fAuthoritativeStartup = true;
    }

    ~RealHRegDiskFixture()
    {
        // Order is dictated by the ownership graph, not by a guessed sequence:
        //   (0) uninstall the global pointer  -> nothing can reach the objects below
        //   (1) authority (owns the live-tail materializer over the reader)
        //   (2) reader  (authority's base reader; materializer must be gone first)
        //   (3) staking navigator
        //   (4) TX-LOC
        SetAuthoritativeLiveForTesting(savedAuthority);
        authority.reset();                    // materializer over the navigator reader
        ClearBlockIndexStakingNavigator();    // navigator + its cold reader
        txloc::g_txloc.close();
        // Release the cached append handle while this datadir still exists; otherwise it
        // keeps pointing at a deleted inode and the next context would append into it.
        FCloseBlockFile();
        g_fAuthoritativeStartup = oldAuthoritative;
        ClearDataDirForTest();
        if (hadDataDir) mapArgs["-datadir"] = oldDataDir;
        else mapArgs.erase("-datadir");
        boost::system::error_code ec;
        boost::filesystem::remove_all(root, ec);
    }
};

static std::unique_ptr<RealHRegDiskFixture> g_hregCtx;
static size_t g_hregCtxSig = 0;

static size_t HRegSig(const std::vector<HRegBlockSpec>& specs)
{
    size_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < specs.size(); ++i)
    {
        h = (h ^ (size_t)specs[i].height) * 1099511628211ULL;
        for (size_t t = 0; t < specs[i].txs.size(); ++t)
        {
            const uint256& txid = specs[i].txs[t].GetHash();
            for (int b = 0; b < 32; ++b) h = (h ^ (size_t)txid.begin()[b]) * 1099511628211ULL;
        }
    }
    return h;
}

// One context per case, and ONLY one. The context is built on the case's first
// HRegCtx() call and destroyed by HRegCtxScope at the case boundary (RAII), so the
// lifetime order is always "teardown previous -> build next" and two contexts can
// never own the process-global TX-LOC / staking navigator / authority at once.
// Rebuilding a DIFFERENT chain while a context is alive is a test bug: it would
// require destroying the live context mid-case (the previous lazy-recreate path did
// exactly that, which is why case N+1's globals were torn down inside case N+1 and
// could interleave with another fixture that opened the same globals).
static RealHRegDiskFixture& HRegCtx(const std::vector<HRegBlockSpec>& specs)
{
    if (g_hregCtx == NULL)
    {
        g_hregCtx.reset(new RealHRegDiskFixture(specs));
        g_hregCtxSig = HRegSig(specs);
        g_hregCtx->OpenAuthority();
    }
    else
    {
        BOOST_REQUIRE_MESSAGE(g_hregCtxSig == HRegSig(specs),
            "HRegCtx: a different chain was requested while the case's context is "
            "alive; declare the whole chain in the case's FIRST HRegCtx() call");
    }
    return *g_hregCtx;
}

// Case-boundary owner: construct as the FIRST statement of every case, destroyed as
// the LAST. Guarantees "teardown old context -> build new context" instead of a lazy
// teardown that happens inside the next case's first HRegCtx() call.
struct HRegCtxScope
{
    HRegCtxScope() { g_hregCtx.reset(); g_hregCtxSig = 0; }
    ~HRegCtxScope() { g_hregCtx.reset(); g_hregCtxSig = 0; }
};

MapPrevTx InputsFor(const CTransaction& regTx, const CTransaction& prevCollateral, int nPrevHeight = 80)
{
    HRegBlockSpec spec; spec.height = nPrevHeight; spec.txs.push_back(prevCollateral);
    HRegCtx(std::vector<HRegBlockSpec>(1, spec));
    MapPrevTx m;
    PutPrev(m, prevCollateral);
    return m;
}

} // namespace

// Deterministic teardown BEFORE static destruction: the context must be closed
// while the process-global TX-LOC / navigator / logging objects are still alive.
// Relying on the file-static unique_ptr's destruction order caused a
// "recursive_mutex::lock()" assertion at process exit (locked after destroy).
struct HRegGlobalTeardown
{
    HRegGlobalTeardown() {}
    ~HRegGlobalTeardown() { g_hregCtx.reset(); }
};
BOOST_GLOBAL_FIXTURE(HRegGlobalTeardown);

BOOST_AUTO_TEST_SUITE(hreg_registration_tests)

BOOST_AUTO_TEST_CASE(marker_exact_match)
{
    HRegCtxScope scope;   // teardown at the case boundary, before the next case builds
    BOOST_CHECK(IsExactHRegMarkerScript(MarkerScript()));
    CScript wrong; std::vector<unsigned char> p; p.push_back('I'); p.push_back('N'); p.push_back('C'); p.push_back('N'); p.push_back(0x01); p.push_back(0x02); wrong << OP_RETURN << p;
    BOOST_CHECK(!IsExactHRegMarkerScript(wrong));
}

BOOST_AUTO_TEST_CASE(plain_25k_without_marker_remains_unregistered)
{
    HRegCtxScope scope;   // teardown at the case boundary, before the next case builds
    ClearHRegStateForTesting();
    SetHRegActivationOverrideForTesting(1);
    CTransaction prev = MakePrevTx(std::vector<CTxOut>(1, MakePrevOut(HREG_COLLATERAL, P2PKHScript(0x11))));
    CTransaction tx = MakeRegistrationTx(prev, 0, false);
    MapPrevTx mp = InputsFor(tx, prev);
    std::string err;
    BOOST_CHECK(ApplyHRegConnectedTx(tx, mp, 100, err));
    BOOST_CHECK(GetHRegStateSnapshotForTesting().empty());
    ClearHRegActivationOverrideForTesting();
}

BOOST_AUTO_TEST_CASE(valid_registration_creates_pending)
{
    HRegCtxScope scope;   // teardown at the case boundary, before the next case builds
    ClearHRegStateForTesting();
    SetHRegActivationOverrideForTesting(1);
    CTransaction prev = MakePrevTx(std::vector<CTxOut>(1, MakePrevOut(HREG_COLLATERAL, P2PKHScript(0x11))));
    CTransaction tx = MakeRegistrationTx(prev, 0);
    MapPrevTx mp = InputsFor(tx, prev);
    std::string err;
    RegistrationParseResult parsed = EvaluateHRegRegistrationTx(tx, mp, 100);
    BOOST_CHECK_EQUAL((int)parsed.action, (int)RegistrationParseResult::ACTION_APPLY);
    BOOST_CHECK(ApplyHRegConnectedTx(tx, mp, 100, err));
    std::vector<RegisteredCollateralState> snap = GetHRegStateSnapshotForTesting();
    BOOST_REQUIRE_EQUAL(snap.size(), 1U);
    BOOST_CHECK_EQUAL(snap[0].state, HREG_STATE_PENDING);
    BOOST_CHECK_EQUAL(snap[0].registrationHeight, 100);
    ClearHRegActivationOverrideForTesting();
}


BOOST_AUTO_TEST_CASE(valid_registration_with_extra_fee_input)
{
    HRegCtxScope scope;   // teardown at the case boundary, before the next case builds
    ClearHRegStateForTesting();
    SetHRegActivationOverrideForTesting(1);
    CTransaction prev = MakePrevTx(std::vector<CTxOut>(1, MakePrevOut(HREG_COLLATERAL, P2PKHScript(0x11))));
    CTransaction feeTx = MakePrevTx(std::vector<CTxOut>(1, MakePrevOut(2*COIN, P2PKHScript(0x22))));
    CTransaction tx = MakeRegistrationTx(prev, 0, true, false, false, false, HREG_COLLATERAL, 1*COIN, true, &feeTx, 0);
    // The case needs a disk context like every other applying case: without one there
    // is no TX-LOC and the collateral height cannot be resolved, so the tx would be
    // classified as non-registration.
    { HRegBlockSpec a; a.height = 80; a.txs.push_back(prev);
      HRegBlockSpec b2; b2.height = 85; b2.txs.push_back(feeTx);
      std::vector<HRegBlockSpec> sp; sp.push_back(a); sp.push_back(b2); HRegCtx(sp); }
    MapPrevTx mp; PutPrev(mp, prev); PutPrev(mp, feeTx);
    std::string err;
    BOOST_CHECK(ApplyHRegConnectedTx(tx, mp, 100, err));
    BOOST_REQUIRE_EQUAL(GetHRegStateSnapshotForTesting().size(), 1U);
    ClearHRegActivationOverrideForTesting();
}

BOOST_AUTO_TEST_CASE(wrong_value_non_registration)
{
    HRegCtxScope scope;   // teardown at the case boundary, before the next case builds
    ClearHRegStateForTesting();
    SetHRegActivationOverrideForTesting(1);
    CTransaction prev = MakePrevTx(std::vector<CTxOut>(1, MakePrevOut(24000LL*COIN, P2PKHScript(0x11))));
    CTransaction tx = MakeRegistrationTx(prev, 0);
    MapPrevTx mp = InputsFor(tx, prev, 90);
    std::string err;
    BOOST_CHECK(ApplyHRegConnectedTx(tx, mp, 100, err));
    BOOST_CHECK(GetHRegStateSnapshotForTesting().empty());
    ClearHRegActivationOverrideForTesting();
}

BOOST_AUTO_TEST_CASE(immature_collateral_non_registration)
{
    HRegCtxScope scope;   // teardown at the case boundary, before the next case builds
    ClearHRegStateForTesting();
    SetHRegActivationOverrideForTesting(1);
    CTransaction prev = MakePrevTx(std::vector<CTxOut>(1, MakePrevOut(HREG_COLLATERAL, P2PKHScript(0x11))));
    CTransaction tx = MakeRegistrationTx(prev, 0);
    MapPrevTx mp = InputsFor(tx, prev, 90);
    std::string err;
    BOOST_CHECK(ApplyHRegConnectedTx(tx, mp, 100, err));
    BOOST_CHECK(GetHRegStateSnapshotForTesting().empty());
    ClearHRegActivationOverrideForTesting();
}

BOOST_AUTO_TEST_CASE(non_p2pkh_non_registration)
{
    HRegCtxScope scope;   // teardown at the case boundary, before the next case builds
    ClearHRegStateForTesting();
    SetHRegActivationOverrideForTesting(1);
    CScript bare; bare << std::vector<unsigned char>(33, 2) << OP_CHECKSIG;
    CTransaction prev = MakePrevTx(std::vector<CTxOut>(1, MakePrevOut(HREG_COLLATERAL, bare)));
    CTransaction tx = MakeRegistrationTx(prev, 0);
    MapPrevTx mp = InputsFor(tx, prev);
    std::string err;
    BOOST_CHECK(ApplyHRegConnectedTx(tx, mp, 100, err));
    BOOST_CHECK(GetHRegStateSnapshotForTesting().empty());
    ClearHRegActivationOverrideForTesting();
}

BOOST_AUTO_TEST_CASE(changed_recreated_script_non_registration)
{
    HRegCtxScope scope;   // teardown at the case boundary, before the next case builds
    ClearHRegStateForTesting();
    SetHRegActivationOverrideForTesting(1);
    CTransaction prev = MakePrevTx(std::vector<CTxOut>(1, MakePrevOut(HREG_COLLATERAL, P2PKHScript(0x11))));
    CTransaction tx = MakeRegistrationTx(prev, 0, true, false, false, true);
    MapPrevTx mp = InputsFor(tx, prev);
    std::string err;
    BOOST_CHECK(ApplyHRegConnectedTx(tx, mp, 100, err));
    BOOST_CHECK(GetHRegStateSnapshotForTesting().empty());
    ClearHRegActivationOverrideForTesting();
}

BOOST_AUTO_TEST_CASE(duplicate_matching_collateral_reject)
{
    HRegCtxScope scope;   // teardown at the case boundary, before the next case builds
    ClearHRegStateForTesting();
    SetHRegActivationOverrideForTesting(1);
    CTransaction prev = MakePrevTx(std::vector<CTxOut>(1, MakePrevOut(HREG_COLLATERAL, P2PKHScript(0x11))));
    CTransaction tx = MakeRegistrationTx(prev, 0, true, false, true);
    MapPrevTx mp = InputsFor(tx, prev);
    std::string err;
    BOOST_CHECK(!ApplyHRegConnectedTx(tx, mp, 100, err));
    BOOST_CHECK(!err.empty());
    ClearHRegActivationOverrideForTesting();
}

BOOST_AUTO_TEST_CASE(multiple_markers_reject)
{
    HRegCtxScope scope;   // teardown at the case boundary, before the next case builds
    ClearHRegStateForTesting();
    SetHRegActivationOverrideForTesting(1);
    CTransaction prev = MakePrevTx(std::vector<CTxOut>(1, MakePrevOut(HREG_COLLATERAL, P2PKHScript(0x11))));
    CTransaction tx = MakeRegistrationTx(prev, 0, true, true);
    MapPrevTx mp = InputsFor(tx, prev);
    std::string err;
    BOOST_CHECK(!ApplyHRegConnectedTx(tx, mp, 100, err));
    ClearHRegActivationOverrideForTesting();
}

BOOST_AUTO_TEST_CASE(malformed_marker_is_non_registration)
{
    HRegCtxScope scope;   // teardown at the case boundary, before the next case builds
    ClearHRegStateForTesting();
    SetHRegActivationOverrideForTesting(1);
    CTransaction prev = MakePrevTx(std::vector<CTxOut>(1, MakePrevOut(HREG_COLLATERAL, P2PKHScript(0x11))));
    CTransaction tx = MakeRegistrationTx(prev, 0, false);
    tx.vout.push_back(CTxOut(0, CScript() << OP_RETURN << std::vector<unsigned char>(1, 'X')));
    MapPrevTx mp = InputsFor(tx, prev);
    std::string err;
    BOOST_CHECK(ApplyHRegConnectedTx(tx, mp, 100, err));
    BOOST_CHECK(GetHRegStateSnapshotForTesting().empty());
    ClearHRegActivationOverrideForTesting();
}


BOOST_AUTO_TEST_CASE(unknown_marker_version_is_non_registration)
{
    HRegCtxScope scope;   // teardown at the case boundary, before the next case builds
    ClearHRegStateForTesting();
    SetHRegActivationOverrideForTesting(1);
    CTransaction prev = MakePrevTx(std::vector<CTxOut>(1, MakePrevOut(HREG_COLLATERAL, P2PKHScript(0x11))));
    CTransaction tx = MakeRegistrationTx(prev, 0, false);
    std::vector<unsigned char> pld; pld.push_back('I'); pld.push_back('N'); pld.push_back('C'); pld.push_back('N'); pld.push_back(0x02); pld.push_back(0x01);
    tx.vout.push_back(CTxOut(0, CScript() << OP_RETURN << pld));
    MapPrevTx mp = InputsFor(tx, prev);
    std::string err;
    BOOST_CHECK(ApplyHRegConnectedTx(tx, mp, 100, err));
    BOOST_CHECK(GetHRegStateSnapshotForTesting().empty());
    ClearHRegActivationOverrideForTesting();
}

BOOST_AUTO_TEST_CASE(coinbase_like_tx_no_registration_effect)
{
    HRegCtxScope scope;   // teardown at the case boundary, before the next case builds
    ClearHRegStateForTesting();
    SetHRegActivationOverrideForTesting(1);
    CTransaction prev = MakePrevTx(std::vector<CTxOut>(1, MakePrevOut(HREG_COLLATERAL, P2PKHScript(0x11))));
    CTransaction tx = MakeRegistrationTx(prev, 0);
    tx.vin.clear();
    tx.vin.push_back(CTxIn(COutPoint(), CScript() << OP_1));
    MapPrevTx mp; PutPrev(mp, prev);
    std::string err;
    BOOST_CHECK(ApplyHRegConnectedTx(tx, mp, 100, err));
    BOOST_CHECK(GetHRegStateSnapshotForTesting().empty());
    ClearHRegActivationOverrideForTesting();
}

BOOST_AUTO_TEST_CASE(coinstake_like_tx_no_registration_effect)
{
    HRegCtxScope scope;   // teardown at the case boundary, before the next case builds
    ClearHRegStateForTesting();
    SetHRegActivationOverrideForTesting(1);
    CTransaction prev = MakePrevTx(std::vector<CTxOut>(1, MakePrevOut(HREG_COLLATERAL, P2PKHScript(0x11))));
    CTransaction tx = MakeRegistrationTx(prev, 0);
    tx.vin.clear();
    tx.vin.push_back(CTxIn(uint256(1), 0, CScript() << OP_1));
    tx.vout[0].SetEmpty();
    MapPrevTx mp; PutPrev(mp, prev);
    std::string err;
    BOOST_CHECK(ApplyHRegConnectedTx(tx, mp, 100, err));
    BOOST_CHECK(GetHRegStateSnapshotForTesting().empty());
    ClearHRegActivationOverrideForTesting();
}

BOOST_AUTO_TEST_CASE(spend_then_attempted_registration_no_state)
{
    HRegCtxScope scope;   // teardown at the case boundary, before the next case builds
    ClearHRegStateForTesting();
    SetHRegActivationOverrideForTesting(1);
    CTransaction prev = MakePrevTx(std::vector<CTxOut>(1, MakePrevOut(HREG_COLLATERAL, P2PKHScript(0x11))));
    CTransaction spend;
    spend.nVersion = CTransaction::CURRENT_VERSION;
    spend.nTime = 1500;
    spend.vin.push_back(CTxIn(prev.GetHash(), 0, CScript() << OP_1));
    spend.vout.push_back(CTxOut(5*COIN, P2PKHScript(0x33)));
    MapPrevTx mp; PutPrev(mp, prev);
    std::string err;
    BOOST_CHECK(ApplyHRegConnectedTx(spend, mp, 100, err));
    mp[prev.GetHash()].first.vSpent[0] = CDiskTxPos(9,9,9);
    CTransaction reg = MakeRegistrationTx(prev, 0);
    BOOST_CHECK(ApplyHRegConnectedTx(reg, mp, 100, err));
    BOOST_CHECK(GetHRegStateSnapshotForTesting().empty());
    ClearHRegActivationOverrideForTesting();
}

BOOST_AUTO_TEST_CASE(maturity_boundary)
{
    HRegCtxScope scope;   // teardown at the case boundary, before the next case builds
    ClearHRegStateForTesting();
    SetHRegActivationOverrideForTesting(1);
    CTransaction prev = MakePrevTx(std::vector<CTxOut>(1, MakePrevOut(HREG_COLLATERAL, P2PKHScript(0x11))));
    CTransaction tx = MakeRegistrationTx(prev, 0);
    MapPrevTx mp = InputsFor(tx, prev);
    std::string err;
    BOOST_CHECK(ApplyHRegConnectedTx(tx, mp, 100, err));
    BOOST_CHECK_EQUAL(ComputePreSpendState(100, 115), HREG_STATE_PENDING);
    BOOST_CHECK_EQUAL(ComputePreSpendState(100, 116), HREG_STATE_ELIGIBLE);
    ClearHRegActivationOverrideForTesting();
}

BOOST_AUTO_TEST_CASE(spend_registered_without_marker_revokes)
{
    HRegCtxScope scope;   // teardown at the case boundary, before the next case builds
    ClearHRegStateForTesting();
    SetHRegActivationOverrideForTesting(1);
    CTransaction prev = MakePrevTx(std::vector<CTxOut>(1, MakePrevOut(HREG_COLLATERAL, P2PKHScript(0x11))));
    CTransaction reg = MakeRegistrationTx(prev, 0);
    { HRegBlockSpec a; a.height = 80; a.txs.push_back(prev);
      HRegBlockSpec b2; b2.height = 100; b2.txs.push_back(reg);
      std::vector<HRegBlockSpec> sp; sp.push_back(a); sp.push_back(b2); HRegCtx(sp); }
    MapPrevTx mp; PutPrev(mp, prev);
    std::string err;
    BOOST_CHECK(ApplyHRegConnectedTx(reg, mp, 100, err));
    CTransaction spend;
    spend.nVersion = CTransaction::CURRENT_VERSION;
    spend.nTime = 2001;
    spend.vin.push_back(CTxIn(reg.GetHash(), 0, CScript() << OP_1));
    spend.vout.push_back(CTxOut(10*COIN, P2PKHScript(0x77)));
    MapPrevTx mp2; PutPrev(mp2, reg);
    BOOST_CHECK(ApplyHRegConnectedTx(spend, mp2, 101, err));
    std::vector<RegisteredCollateralState> snap = GetHRegStateSnapshotForTesting();
    BOOST_CHECK(HasState(snap, COutPoint(reg.GetHash(),0), HREG_STATE_SPENT));
    ClearHRegActivationOverrideForTesting();
}

BOOST_AUTO_TEST_CASE(rollover_revokes_old_new_pending)
{
    HRegCtxScope scope;   // teardown at the case boundary, before the next case builds
    ClearHRegStateForTesting();
    SetHRegActivationOverrideForTesting(1);
    CTransaction prev = MakePrevTx(std::vector<CTxOut>(1, MakePrevOut(HREG_COLLATERAL, P2PKHScript(0x11))));
    CTransaction reg = MakeRegistrationTx(prev, 0);
    { HRegBlockSpec a; a.height = 80; a.txs.push_back(prev);
      HRegBlockSpec b2; b2.height = 100; b2.txs.push_back(reg);
      std::vector<HRegBlockSpec> sp; sp.push_back(a); sp.push_back(b2); HRegCtx(sp); }
    MapPrevTx mp; PutPrev(mp, prev);
    std::string err;
    BOOST_CHECK(ApplyHRegConnectedTx(reg, mp, 100, err));
    CTransaction rollover = MakeRegistrationTx(reg, 0);
    MapPrevTx mp2; PutPrev(mp2, reg);
    BOOST_CHECK(ApplyHRegConnectedTx(rollover, mp2, 120, err));
    std::vector<RegisteredCollateralState> snap = GetHRegStateSnapshotForTesting();
    BOOST_CHECK(HasState(snap, COutPoint(reg.GetHash(),0), HREG_STATE_SPENT));
    BOOST_CHECK(HasState(snap, COutPoint(rollover.GetHash(),0), HREG_STATE_PENDING));
    ClearHRegActivationOverrideForTesting();
}

BOOST_AUTO_TEST_CASE(disconnect_registration_restores_empty)
{
    HRegCtxScope scope;   // teardown at the case boundary, before the next case builds
    ClearHRegStateForTesting();
    SetHRegActivationOverrideForTesting(1);
    CTransaction prev = MakePrevTx(std::vector<CTxOut>(1, MakePrevOut(HREG_COLLATERAL, P2PKHScript(0x11))));
    CTransaction reg = MakeRegistrationTx(prev, 0);
    MapPrevTx mp = InputsFor(reg, prev);
    std::string err;
    BOOST_CHECK(ApplyHRegConnectedTx(reg, mp, 100, err));
    BOOST_CHECK(DisconnectHRegTx(reg, mp, 100, err));
    BOOST_CHECK(GetHRegStateSnapshotForTesting().empty());
    ClearHRegActivationOverrideForTesting();
}

BOOST_AUTO_TEST_CASE(registration_then_same_block_spend_no_phantom)
{
    HRegCtxScope scope;   // teardown at the case boundary, before the next case builds
    ClearHRegStateForTesting();
    SetHRegActivationOverrideForTesting(1);
    CTransaction prev = MakePrevTx(std::vector<CTxOut>(1, MakePrevOut(HREG_COLLATERAL, P2PKHScript(0x11))));
    CTransaction reg = MakeRegistrationTx(prev, 0);
    { HRegBlockSpec a; a.height = 80; a.txs.push_back(prev);
      HRegBlockSpec b2; b2.height = 100; b2.txs.push_back(reg);
      std::vector<HRegBlockSpec> sp; sp.push_back(a); sp.push_back(b2); HRegCtx(sp); }
    MapPrevTx mp; PutPrev(mp, prev);
    std::string err;
    BOOST_CHECK(ApplyHRegConnectedTx(reg, mp, 100, err));
    CTransaction spend;
    spend.nVersion = CTransaction::CURRENT_VERSION;
    spend.nTime = 2001;
    spend.vin.push_back(CTxIn(reg.GetHash(), 0, CScript() << OP_1));
    spend.vout.push_back(CTxOut(5*COIN, P2PKHScript(0x77)));
    MapPrevTx mp2; PutPrev(mp2, reg);
    BOOST_CHECK(ApplyHRegConnectedTx(spend, mp2, 100, err));
    std::vector<RegisteredCollateralState> snap = GetHRegStateSnapshotForTesting();
    BOOST_CHECK(HasState(snap, COutPoint(reg.GetHash(),0), HREG_STATE_SPENT));
    BOOST_CHECK(DisconnectHRegTx(spend, mp2, 100, err));
    BOOST_CHECK(DisconnectHRegTx(reg, mp, 100, err));
    BOOST_CHECK(GetHRegStateSnapshotForTesting().empty());
    ClearHRegActivationOverrideForTesting();
}

BOOST_AUTO_TEST_CASE(rebuild_equals_replay_for_simple_history)
{
    HRegCtxScope scope;   // teardown at the case boundary, before the next case builds
    ClearHRegStateForTesting();
    SetHRegActivationOverrideForTesting(1);
    CTransaction prev = MakePrevTx(std::vector<CTxOut>(1, MakePrevOut(HREG_COLLATERAL, P2PKHScript(0x11))));
    CTransaction reg = MakeRegistrationTx(prev, 0);
    // Original semantics: the collateral prev-tx lives at height 80; the
    // registration is applied at height 100. `reg` itself need not be on-chain.
    MapPrevTx mp = InputsFor(reg, prev);
    std::string err;
    BOOST_REQUIRE_MESSAGE(ApplyHRegConnectedTx(reg, mp, 100, err), "live registration: " << err);
    std::vector<RegisteredCollateralState> live = GetHRegStateSnapshotForTesting();
    ClearHRegStateForTesting();
    SetHRegActivationOverrideForTesting(1);
    BOOST_CHECK(ApplyHRegConnectedTx(reg, mp, 100, err));
    std::vector<RegisteredCollateralState> replay = GetHRegStateSnapshotForTesting();
    BOOST_REQUIRE_MESSAGE(live.size() == 1U, "live registration failed: " << err);
    BOOST_REQUIRE_MESSAGE(replay.size() == 1U, "replay registration failed: " << err);
    BOOST_CHECK_EQUAL(live[0].outpoint.ToString(), replay[0].outpoint.ToString());
    BOOST_CHECK_EQUAL((int)live[0].state, (int)replay[0].state);
    BOOST_CHECK_EQUAL(live[0].registrationHeight, replay[0].registrationHeight);
    ClearHRegActivationOverrideForTesting();
}


BOOST_AUTO_TEST_CASE(compat_valid_registration_old_tx_valid_new_state_apply)
{
    HRegCtxScope scope;   // teardown at the case boundary, before the next case builds
    ClearHRegStateForTesting();
    SetHRegActivationOverrideForTesting(1);
    CTransaction prev = MakePrevTx(std::vector<CTxOut>(1, MakePrevOut(HREG_COLLATERAL, P2PKHScript(0x11))));
    CTransaction tx = MakeRegistrationTx(prev, 0);
    MapPrevTx mp = InputsFor(tx, prev);
    std::string reason, err;
    BOOST_CHECK(tx.CheckTransaction());
    BOOST_CHECK(IsStandardTx(tx, reason));
    BOOST_CHECK(ApplyHRegConnectedTx(tx, mp, 100, err));
    ClearHRegActivationOverrideForTesting();
}

BOOST_AUTO_TEST_CASE(compat_duplicate_marker_old_valid_new_reject)
{
    HRegCtxScope scope;   // teardown at the case boundary, before the next case builds
    ClearHRegStateForTesting();
    SetHRegActivationOverrideForTesting(1);
    CTransaction prev = MakePrevTx(std::vector<CTxOut>(1, MakePrevOut(HREG_COLLATERAL, P2PKHScript(0x11))));
    CTransaction tx = MakeRegistrationTx(prev, 0, true, true);
    MapPrevTx mp = InputsFor(tx, prev);
    std::string reason, err;
    BOOST_CHECK(tx.CheckTransaction());
    BOOST_CHECK(IsStandardTx(tx, reason));
    BOOST_CHECK(!ApplyHRegConnectedTx(tx, mp, 100, err));
    ClearHRegActivationOverrideForTesting();
}

BOOST_AUTO_TEST_CASE(compat_malformed_marker_old_valid_new_nonregistration)
{
    HRegCtxScope scope;   // teardown at the case boundary, before the next case builds
    ClearHRegStateForTesting();
    SetHRegActivationOverrideForTesting(1);
    CTransaction prev = MakePrevTx(std::vector<CTxOut>(1, MakePrevOut(HREG_COLLATERAL, P2PKHScript(0x11))));
    CTransaction tx = MakeRegistrationTx(prev, 0, false);
    tx.vout.push_back(CTxOut(0, CScript() << OP_RETURN << std::vector<unsigned char>(6, 'Z')));
    MapPrevTx mp = InputsFor(tx, prev);
    std::string reason, err;
    BOOST_CHECK(tx.CheckTransaction());
    BOOST_CHECK(IsStandardTx(tx, reason));
    BOOST_CHECK(ApplyHRegConnectedTx(tx, mp, 100, err));
    BOOST_CHECK(GetHRegStateSnapshotForTesting().empty());
    ClearHRegActivationOverrideForTesting();
}

BOOST_AUTO_TEST_CASE(hreg_inactive_before_activation)
{
    HRegCtxScope scope;   // teardown at the case boundary, before the next case builds
    ClearHRegStateForTesting();
    SetHRegActivationOverrideForTesting(500);
    CTransaction prev = MakePrevTx(std::vector<CTxOut>(1, MakePrevOut(HREG_COLLATERAL, P2PKHScript(0x11))));
    CTransaction reg = MakeRegistrationTx(prev, 0);
    MapPrevTx mp = InputsFor(reg, prev);
    std::string err;
    BOOST_CHECK(ApplyHRegConnectedTx(reg, mp, 100, err));
    BOOST_CHECK(GetHRegStateSnapshotForTesting().empty());
    ClearHRegActivationOverrideForTesting();
}

BOOST_AUTO_TEST_CASE(hreg_authoritative_real_disk_activation_readiness)
{
    HRegCtxScope scope;   // teardown at the case boundary, before the next case builds
    ClearHRegStateForTesting();
    RealHRegDiskFixture fixture;

    CTransaction roundTrip;
    uint256 roundTripBlock;
    BOOST_REQUIRE(GetTransaction(fixture.prev.GetHash(), roundTrip, roundTripBlock));
    BOOST_CHECK(roundTrip.GetHash() == fixture.prev.GetHash());
    BOOST_CHECK(roundTripBlock == fixture.blockHash);

    std::string error;
    fixture.OpenAuthority();
    {
        LOCK(cs_main);
        BlockIndexSnapshot snapshot;
        BOOST_REQUIRE_EQUAL((int)ResolveAuthoritativeBlockSnapshotR(
            roundTripBlock, &snapshot, &error),
            (int)AUTHORITATIVE_BLOCK_FOUND);
        BOOST_CHECK(snapshot.fInMainChain);
        BOOST_CHECK_EQUAL(snapshot.height, 0);
    }
    BOOST_CHECK(mapBlockIndex.find(fixture.blockHash) == mapBlockIndex.end());

    SetHRegActivationOverrideForTesting(1); // controlled tomorrow-lowered gate
    CTransaction reg = MakeRegistrationTx(fixture.prev, 0);
    MapPrevTx inputs;
    PutPrev(inputs, fixture.prev);
    BOOST_REQUIRE_MESSAGE(ApplyHRegConnectedTx(reg, inputs, 16, error), error);
    BOOST_REQUIRE_EQUAL(GetHRegStateSnapshotForTesting().size(), 1U);
    BOOST_CHECK_EQUAL(GetHRegStateSnapshotForTesting()[0].registrationHeight, 16);
    BOOST_CHECK(mapBlockIndex.find(fixture.blockHash) == mapBlockIndex.end());

    ClearHRegStateForTesting();
    BOOST_CHECK(ApplyHRegConnectedTx(reg, inputs, 15, error));
    BOOST_CHECK(GetHRegStateSnapshotForTesting().empty());
    BOOST_CHECK(mapBlockIndex.find(fixture.blockHash) == mapBlockIndex.end());

    ClearHRegStateForTesting();
    ClearBlockIndexStakingNavigator();
    BOOST_CHECK(!ApplyHRegConnectedTx(reg, inputs, 16, error));
    BOOST_CHECK(!error.empty());
    BOOST_CHECK(error.find("navigator unavailable") != std::string::npos ||
                error.find("authority") != std::string::npos ||
                error.find("Authority") != std::string::npos);
    BOOST_CHECK(GetHRegStateSnapshotForTesting().empty());

    ClearHRegActivationOverrideForTesting();
}

BOOST_AUTO_TEST_SUITE_END()
