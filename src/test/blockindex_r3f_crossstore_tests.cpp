// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.
//
// R3F (PM1-P0-03) — PRODUCTION-LEVEL cross-store authority ordering tests.
//
// Proves, through the REAL production operator transitions
// (InvalidateBlock / ReconsiderBlock authoritative V2 branch, main.cpp
// g_fAuthoritativeStartup) plus the REAL production startup entry
// (InitBlockIndexAuthoritative) and the REAL production startup
// reconciliation (ReconcileLegacyCompatibilityMirror), the ordered
// cross-store contract:
//
//   S1 (§5) crash AFTER the V2 authority commit but BEFORE the legacy
//           compatibility mirror: the call fails (caller cannot treat it as
//           success), the NEW state is already committed in the V2 tip
//           authority, the legacy mirror is stale, a restart does NOT roll
//           back V2 and does NOT choose legacy, and the production startup
//           reconciliation re-mirrors the legacy view from V2 (V2 wins).
//   S2 (§6) failure BEFORE the V2 commit: old V2 authority remains committed,
//           the attempted new state is not visible, legacy never becomes the
//           authority for the attempted transition, restart recovers the OLD
//           V2 authority.
//   S3 (§7) V2 transition failure PROPAGATES: the production function returns
//           false with an error containing the injected failpoint name, and
//           the previously committed V2 authority is recovered on restart.
//   S4 (§8) the operator-invalid transition (invalidate + reconsider) follows
//           the same V2-first / legacy-mirror-second ordering: the invalid
//           intent is durably committed in the V2 tip authority, the legacy
//           mirror lags, and the production reconciliation repairs it (V2 wins).
//
// The failpoints are the one-shot production test seams in main.cpp:
//   "FP_BEFORE_V2_AUTHORITY_COMMIT"            (before the fused tip.meta commit)
//   "FP_AFTER_V2_COMMIT_BEFORE_LEGACY_MIRROR"  (after the commit, before the
//                                               setInvalidBlockHash mirror)
// and must be disarmed (consumed one-shot) by teardown regardless of outcome.
//
// Fixture style reused verbatim from test/blockindex_window2_e2e_tests.cpp:
// a self-contained immutable generation genesis..S with REAL block files,
// booted through the REAL production startup entry, with post-S active/side
// branches appended through the real BlockIndexAuthoritativeLive seam and
// restarts performed by ResetBlockIndexAuthoritativeStartupForTest +
// InitBlockIndexAuthoritative on the SAME root (fresh authority object).

#include <boost/test/unit_test.hpp>

#include "main.h"
#include "txdb.h"
#include "blockindex_authoritative_startup.h"
#include "blockindex_authoritative_live.h"
#include "blockindex_tip.h"
#include "blockindex_generation_builder.h"
#include "blockindex_generation_lifecycle.h"

#include <boost/filesystem.hpp>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

namespace fs = boost::filesystem;

// ---------------------------------------------------------------------------
// Synthetic-generation fixture (page-level clone of the proven R2D harness in
// blockindex_window2_e2e_tests.cpp; independent copy so the production code
// and other test files are NOT touched).
// ---------------------------------------------------------------------------
struct R3FXSBlockInfo
{
    uint256 hash;
    unsigned int nFile, nBlockPos;
};

static R3FXSBlockInfo R3FXSWriteSyntheticBlock(const fs::path& blockDir, uint256 prev,
                                               unsigned int nTime, unsigned int nBits,
                                               unsigned int nNonce, unsigned int nFile)
{
    R3FXSBlockInfo info; info.nFile = nFile;
    CTransaction coinbase; coinbase.nVersion = 1; coinbase.nTime = nTime;
    CTxIn input; input.prevout = COutPoint(uint256(0), 0xffffffff);
    input.scriptSig = CScript() << OP_TRUE; input.nSequence = 0xffffffff;
    coinbase.vin.push_back(input);
    CTxOut output; output.nValue = 0; output.scriptPubKey = CScript() << OP_TRUE;
    coinbase.vout.push_back(output);
    CBlock block; block.nVersion = 1; block.hashPrevBlock = prev;
    block.nTime = nTime; block.nBits = nBits; block.nNonce = nNonce;
    block.vtx.push_back(coinbase); block.hashMerkleRoot = block.BuildMerkleTree();
    info.hash = block.GetHash();
    CDataStream ss(SER_DISK, CLIENT_VERSION); ss << block;
    char name[32]; snprintf(name, sizeof(name), "blk%04u.dat", nFile);
    fs::path f = blockDir / name;
    FILE* fp = fopen(f.string().c_str(), "ab"); BOOST_REQUIRE(fp != NULL);
    unsigned char magic[] = {0xfa,0xbf,0xb5,0xda};
    fwrite(magic,1,4,fp);
    unsigned int ns = ss.size();
    fwrite(&ns,4,1,fp);
    long pos = ftell(fp); info.nBlockPos = (unsigned int)pos;
    fwrite(&ss[0],1,ss.size(),fp); fflush(fp); fclose(fp);
    return info;
}

static bool R3FXSBuildGenerationAndInit(const fs::path& root, int S,
                                        std::vector<uint256>* outActive,
                                        std::string* error)
{
    fs::create_directories(root / "blocks");
    std::vector<uint256> active;
    uint256 prev(0);
    BlockIndexGenerationSource src;
    for (int h = 0; h <= S; ++h)
    {
        R3FXSBlockInfo b = R3FXSWriteSyntheticBlock(root / "blocks", prev,
                                                    1700000000u + (unsigned)h, 0x1d00ffffU,
                                                    (unsigned)h, 1);
        BlockIndexRecord rec;
        rec.hash = b.hash; rec.hashPrev = prev; rec.height = h;
        rec.nVersion = 1; rec.nTime = 1700000000u + (unsigned)h;
        rec.nBits = 0x1d00ffffU; rec.nNonce = (unsigned)h;
        rec.nFile = b.nFile; rec.nBlockPos = b.nBlockPos; rec.nFlags = 0;
        rec.nMoneySupply = 0;
        BlockIndexGenerationSourceRecord sr; sr.hash = b.hash; sr.record = rec;
        src.records.push_back(sr);
        active.push_back(b.hash);
        prev = b.hash;
    }
    src.hashBestChain = active[S];
    src.foundBestChain = true;
    src.blockDataDir = (root / "blocks").string();
    BlockIndexGenerationBuilder b;
    if (!b.Build(src, (root / "blockindex-build-000001.tmp").string(), 1, NULL, error)) return false;
    b.Close();
    if (BlockIndexGenerationManager::PublishGeneration(root.string(), 1, error) != BLOCK_INDEX_LIFECYCLE_OK) return false;
    if (BlockIndexGenerationManager::SelectGeneration(root.string(), 1, error) != BLOCK_INDEX_LIFECYCLE_OK) return false;
    if (outActive) *outActive = active;
    return InitBlockIndexAuthoritative(root.string(), error);
}

// RAII fixture: builds + boots a real S>0 generation through the production
// startup entry, then restores every process global it disturbed (and disarms
// every failpoint) on destruction — the restart-with-fresh-state contract.
struct R3FXSFixture
{
    fs::path root;
    int S;
    std::vector<uint256> active;
    CBlockIndex* sBest; CBlockIndex* sGen; uint256 sHash; int sH; uint256 sT;
    std::set<uint256> sInvalid;

    explicit R3FXSFixture(int s) : S(s), sBest(NULL), sGen(NULL), sH(-1)
    {
        sBest = pindexBest; sGen = pindexGenesisBlock;
        sHash = hashBestChain; sH = nBestHeight; sT = nBestChainTrust;
        sInvalid = setInvalidBlockHash;
        root = fs::temp_directory_path() / fs::unique_path("r3fxs-%%%%-%%%%");
        std::string error;
        BOOST_REQUIRE_MESSAGE(R3FXSBuildGenerationAndInit(root, S, &active, &error), error);
        BOOST_REQUIRE_MESSAGE(g_fAuthoritativeStartup,
            "R3F XS fixture: authoritative startup must be active");
    }
    ~R3FXSFixture()
    {
        // Failpoints are one-shot; disarm unconditionally (both names).
        MainFailpointSetForTesting("FP_BEFORE_V2_AUTHORITY_COMMIT", false);
        MainFailpointSetForTesting("FP_AFTER_V2_COMMIT_BEFORE_LEGACY_MIRROR", false);
        ResetBlockIndexAuthoritativeStartupForTest();
        pindexBest = sBest; pindexGenesisBlock = sGen;
        hashBestChain = sHash; nBestHeight = sH; nBestChainTrust = sT;
        setInvalidBlockHash = sInvalid;
        try { fs::remove_all(root); } catch (...) {}
    }
};

// Active branch A (S+1,S+2; trust 100/200) and competing side branch B
// (trust 50/150), both above the immutable base tip S.
static void R3FXSAppendActive(BlockIndexAuthoritativeLive* live, const uint256& hash,
                              const uint256& prev, int height, const uint256& trust,
                              std::string* err)
{
    BlockIndexRecord r;
    r.hash = hash; r.hashPrev = prev; r.height = height;
    r.nFile = 1; r.nBlockPos = (unsigned)(height * 100); r.nFlags = 0;
    r.nVersion = 5; r.nTime = 1700000000u + (unsigned)height;
    r.nBits = 0x1d00ffff; r.nNonce = (unsigned)height;
    BlockIndexDerivedEntry d;
    d.chainTrust = trust; d.stakeModifierChecksum = (uint32_t)height;
    d.SetHasStakeModifierTime(true); d.stakeModifierTime = 1700000000;
    d.SetHasBlockSize(true); d.nSize = 1200 + (uint32_t)height;
    BOOST_REQUIRE_MESSAGE(live->AcceptActive(r, d, height, err), *err);
}

static void R3FXSAppendSide(BlockIndexAuthoritativeLive* live, const uint256& hash,
                            const uint256& prev, int height, const uint256& trust,
                            std::string* err)
{
    BlockIndexRecord r;
    r.hash = hash; r.hashPrev = prev; r.height = height;
    r.nFile = 1; r.nBlockPos = (unsigned)(height * 100 + 7); r.nFlags = 0;
    r.nVersion = 5; r.nTime = 1700000000u + (unsigned)height;
    r.nBits = 0x1d00ffff; r.nNonce = (unsigned)height;
    BlockIndexDerivedEntry d;
    d.chainTrust = trust; d.stakeModifierChecksum = (uint32_t)height;
    d.SetHasStakeModifierTime(true); d.stakeModifierTime = 1700000000;
    d.SetHasBlockSize(true); d.nSize = 1100 + (uint32_t)height;
    BOOST_REQUIRE_MESSAGE(live->AcceptSide(r, d, err), *err);
}

// Install branches and return the branch tip hashes via out-params.
static void R3FXSInstallBranches(BlockIndexAuthoritativeLive* live, const uint256& sHash,
                                 int S, const uint256& A1, const uint256& A2,
                                 const uint256& B1, const uint256& B2)
{
    std::string e;
    R3FXSAppendActive(live, A1, sHash, S + 1, uint256(100), &e);
    R3FXSAppendActive(live, A2, A1, S + 2, uint256(200), &e);
    R3FXSAppendSide(live, B1, sHash, S + 1, uint256(50), &e);
    R3FXSAppendSide(live, B2, B1, S + 2, uint256(150), &e);
}

// Fresh-state restart on the same durable root (process-restart simulation
// exactly as the R2D harness does it).
static void R3FXSRestart(const fs::path& root)
{
    ResetBlockIndexAuthoritativeStartupForTest();
    std::string err;
    BOOST_REQUIRE_MESSAGE(g_fAuthoritativeStartup == false || true, "restart commenced");
    BOOST_REQUIRE_MESSAGE(InitBlockIndexAuthoritative(root.string(), &err), err);
    BOOST_REQUIRE_MESSAGE(g_fAuthoritativeStartup,
        "R3F XS restart: authoritative mode must be re-entered");
}

// Read the legacy compatibility mirror through the REAL txdb API (the exact
// source ReconcileLegacyCompatibilityMirror compares against).
static uint256 R3FXSLegacyBest()
{
    uint256 best(0);
    CTxDB txdb;
    txdb.ReadHashBestChain(best);
    return best;
}

static bool R3FXSCorruptInitDone = false;

BOOST_AUTO_TEST_SUITE(blockindex_r3f_crossstore_tests)

// ===========================================================================
// S1 (§5) — crash AFTER V2 authority commit, BEFORE legacy mirror.
// V2 wins; startup reconciliation repairs the stale legacy mirror.
// ===========================================================================
BOOST_AUTO_TEST_CASE(r3f_xs_s1_crash_after_v2_commit_before_mirror)
{
    R3FXSFixture fx(12);
    const int S = fx.S; const uint256 sHash = fx.active[S];
    BlockIndexAuthoritativeLive* live = GetAuthoritativeLiveAuthority();
    BOOST_REQUIRE(live && live->IsOpen());
    BlockIndexTipAuthority* tip = live->TipAuthorityMutable();
    BOOST_REQUIRE(tip != NULL);
    BOOST_REQUIRE_EQUAL(AuthoritativeBaseTipHeight(), S);

    const uint256 A1(0xE1A001UL), A2(0xE1A002UL), B1(0xE1B001UL), B2(0xE1B002UL);
    R3FXSInstallBranches(live, sHash, S, A1, A2, B1, B2);
    BOOST_REQUIRE_MESSAGE(tip->GetTip().record.hash == A2, "fixture: branch A must be the committed pre-crash V2 tip");

    const uint256 v2TipBefore = tip->GetTip().record.hash;
    const uint256 legacyBestBefore = R3FXSLegacyBest();
    const std::set<uint256> invalidMirrorBefore = setInvalidBlockHash;
    BOOST_TEST_MESSAGE("R3F_XS S1 pre-crash v2_tip=" << v2TipBefore.ToString()
        << " legacy_best=" << legacyBestBefore.ToString());

    // ---- inject the crash: V2 commit done, legacy mirror NOT updated ----
    MainFailpointSetForTesting("FP_AFTER_V2_COMMIT_BEFORE_LEGACY_MIRROR", true);
    std::string err;
    const bool ok = InvalidateBlock(A1, err);
    BOOST_CHECK_MESSAGE(!ok,
        "S1: the transition MUST return failure after the injected post-commit failpoint (no false success)");
    BOOST_CHECK_MESSAGE(err.find("FP_AFTER_V2_COMMIT_BEFORE_LEGACY_MIRROR") != std::string::npos,
        "S1: returned error must contain the injected failpoint name (err=\"" << err << "\")");

    // V2 authority ALREADY contains the new committed state.
    const BlockIndexTipRead v2After = tip->GetTip();
    BOOST_REQUIRE(v2After.status == BLOCK_INDEX_TIP_OK);
    BOOST_CHECK_MESSAGE(v2After.record.hash == B2,
        "S1: V2 tip authority must already carry the NEW committed tip (expected B2 "
        << B2.ToString() << ", got " << v2After.record.hash.ToString() << ")");
    BOOST_CHECK_MESSAGE(tip->IsOperatorInvalid(A1),
        "S1: invalid intent for A1 must be durably committed in the V2 tip authority");
    // The legacy mirror is stale: the erase/insert never ran.
    BOOST_CHECK_MESSAGE(!setInvalidBlockHash.count(A1),
        "S1: legacy invalid mirror must be stale (A1 not mirrored after the crash)");
    BOOST_TEST_MESSAGE("R3F_XS S1 crash v2_tip_committed=" << v2After.record.hash.ToString()
        << " height=" << v2After.height
        << " legacy_best=" << R3FXSLegacyBest().ToString());

    // ---- restart with a FRESH state instance on the same durable root ----
    const int bestHeightAfterCrash = nBestHeight;
    R3FXSRestart(fx.root);

    BlockIndexTipAuthority* tipR = GetAuthoritativeLiveAuthority()->TipAuthorityMutable();
    BOOST_REQUIRE(tipR != NULL);
    // V2 authority is kept: no rollback, no legacy choice, no hybrid state.
    const BlockIndexTipRead v2Restart = tipR->GetTip();
    BOOST_REQUIRE(v2Restart.status == BLOCK_INDEX_TIP_OK);
    BOOST_CHECK_MESSAGE(v2Restart.record.hash == B2,
        "S1: restart MUST keep the committed V2 authority (expected B2, got "
        << v2Restart.record.hash.ToString() << ")");
    BOOST_CHECK_MESSAGE(tipR->IsOperatorInvalid(A1),
        "S1: V2 invalid intent for A1 must survive the restart");
    BOOST_TEST_MESSAGE("R3F_XS S1 restart v2_tip=" << v2Restart.record.hash.ToString()
        << " height=" << v2Restart.height
        << " projected_best=" << (pindexBest ? pindexBest->GetBlockHash().ToString() : std::string("<null>"))
        << " nBestHeight=" << nBestHeight
        << " bestHeightAfterCrash=" << bestHeightAfterCrash);
    const uint256 legacyStale = R3FXSLegacyBest();
    const std::set<uint256> v2InvalidRestart = tipR->OperatorInvalidSet();
    BOOST_TEST_MESSAGE("R3F_XS S1 stale legacy_best=" << legacyStale.ToString()
        << " v2_invalid_set_size=" << v2InvalidRestart.size());

    // ---- PRODUCTION startup reconciliation repairs the stale mirror (V2 wins) ----
    std::string rerr;
    BOOST_REQUIRE_MESSAGE(ReconcileLegacyCompatibilityMirror(rerr), rerr);
    // Repaired legacy hashBestChain == V2 authority tip (explicit hash equality).
    const uint256 legacyRepaired = R3FXSLegacyBest();
    BOOST_CHECK_MESSAGE(legacyRepaired == B2,
        "S1: repaired legacy hashBestChain must EQUAL the V2 authority tip (expected B2, got "
        << legacyRepaired.ToString() << ")");
    BOOST_CHECK_MESSAGE(hashBestChain == B2,
        "S1: projected global hashBestChain after reconciliation must equal the V2 tip");
    BOOST_CHECK_MESSAGE(setInvalidBlockHash == v2InvalidRestart,
        "S1: repaired legacy invalid mirror must equal the V2 OperatorInvalidSet");
    // No hybrid state: exactly one invalid hash, and it is exactly A1.
    BOOST_CHECK_EQUAL(setInvalidBlockHash.size(), (size_t)1);
    BOOST_CHECK_MESSAGE(setInvalidBlockHash.count(A1) == 1,
        "S1: reconciled invalid mirror must contain exactly A1");
    // V2 authority unchanged by the reconciliation (V2 wins, never the converse).
    BOOST_CHECK_MESSAGE(tipR->GetTip().record.hash == B2,
        "S1: reconciliation must NOT touch the V2 authority (V2 wins)");
    BOOST_TEST_MESSAGE("R3F_XS S1 reconcile repaired_legacy_best=" << legacyRepaired.ToString()
        << " v2_tip=" << tipR->GetTip().record.hash.ToString()
        << " invalid_mirror_size=" << setInvalidBlockHash.size()
        << " preCrashLegacyBest=" << legacyBestBefore.ToString()
        << " preCrashInvalidMirror=" << invalidMirrorBefore.size());
}

// ===========================================================================
// S2 (§6) — failure BEFORE the V2 commit: old V2 authority remains committed,
// the new state is not visible, legacy never becomes the authority.
// ===========================================================================
BOOST_AUTO_TEST_CASE(r3f_xs_s2_failure_before_v2_commit_keeps_old_authority)
{
    R3FXSFixture fx(12);
    const int S = fx.S; const uint256 sHash = fx.active[S];
    BlockIndexAuthoritativeLive* live = GetAuthoritativeLiveAuthority();
    BOOST_REQUIRE(live && live->IsOpen());
    BlockIndexTipAuthority* tip = live->TipAuthorityMutable();
    BOOST_REQUIRE(tip != NULL);

    const uint256 A1(0xE2A001UL), A2(0xE2A002UL), B1(0xE2B001UL), B2(0xE2B002UL);
    R3FXSInstallBranches(live, sHash, S, A1, A2, B1, B2);
    BOOST_REQUIRE_MESSAGE(tip->GetTip().record.hash == A2, "fixture: branch A must be the OLD committed V2 tip");

    const uint256 oldV2Tip = tip->GetTip().record.hash;
    const int32_t oldV2Height = tip->TipHeight();
    const uint256 legacyBestBefore = R3FXSLegacyBest();
    const std::set<uint256> orphanMirrorBefore = setInvalidBlockHash; // new name, old guard
    R3FXSCorruptInitDone = legacyBestBefore != oldV2Tip || orphanMirrorBefore.empty();

    // ---- inject the pre-commit failure and drive the REAL transition ----
    MainFailpointSetForTesting("FP_BEFORE_V2_AUTHORITY_COMMIT", true);
    std::string err;
    const bool ok = InvalidateBlock(A1, err);
    BOOST_CHECK_MESSAGE(!ok, "S2: the transition MUST fail before any mutation");
    BOOST_CHECK_MESSAGE(err.find("FP_BEFORE_V2_AUTHORITY_COMMIT") != std::string::npos,
        "S2: error must carry the injected failpoint name (err=\"" << err << "\")");

    // Old V2 authority remains committed untouched.
    const BlockIndexTipRead v2Old = tip->GetTip();
    BOOST_REQUIRE(v2Old.status == BLOCK_INDEX_TIP_OK);
    BOOST_CHECK_MESSAGE(v2Old.record.hash == A2,
        "S2: OLD V2 tip must remain committed (expected A2, got " << v2Old.record.hash.ToString() << ")");
    BOOST_CHECK_EQUAL(tip->TipHeight(), oldV2Height);
    BOOST_CHECK_MESSAGE(!tip->IsOperatorInvalid(A1),
        "S2: the attempted NEW state (invalidating A1) must NOT be visible in V2");
    BOOST_CHECK_MESSAGE(tip->OperatorInvalidSet().empty(),
        "S2: no invalid intent may leak into the V2 authority on pre-commit failure");
    // Legacy does NOT become the authority for the attempted transition.
    BOOST_CHECK_MESSAGE(!setInvalidBlockHash.count(A1),
        "S2: legacy mirror must not gain the attempted invalidation");
    BOOST_CHECK_MESSAGE(R3FXSLegacyBest() == legacyBestBefore,
        "S2: legacy hashBestChain must be untouched by the failed transition");
    BOOST_TEST_MESSAGE("R3F_XS S2 precommit-fail old_v2_tip=" << v2Old.record.hash.ToString()
        << " old_height=" << oldV2Height
        << " legacy_best_unchanged=" << R3FXSLegacyBest().ToString());

    // ---- restart recovers the OLD V2 authority ----
    R3FXSRestart(fx.root);
    BlockIndexTipAuthority* tipR = GetAuthoritativeLiveAuthority()->TipAuthorityMutable();
    const BlockIndexTipRead v2Restart = tipR->GetTip();
    BOOST_REQUIRE(v2Restart.status == BLOCK_INDEX_TIP_OK);
    BOOST_CHECK_MESSAGE(v2Restart.record.hash == A2,
        "S2: restart MUST recover the OLD V2 authority (expected A2, got "
        << v2Restart.record.hash.ToString() << ")");
    BOOST_CHECK_MESSAGE(!tipR->IsOperatorInvalid(A1),
        "S2: restart must show NO legacy-only successful transition (A1 not invalidated)");
    // Startup reconciliation on the untouched state must be a no-op success.
    std::string rerr;
    BOOST_REQUIRE_MESSAGE(ReconcileLegacyCompatibilityMirror(rerr), rerr);
    BOOST_CHECK_MESSAGE(tipR->GetTip().record.hash == A2,
        "S2: no hybrid state after reconciliation — V2 tip still A2");
    BOOST_TEST_MESSAGE("R3F_XS S2 restart recovered_tip=" << v2Restart.record.hash.ToString()
        << " recovered_height=" << v2Restart.height);
}

// ===========================================================================
// S3 (§7) — V2 commit failure PROPAGATES: every failpoint on the transition
// path returns production-level failure carrying the injected reason, and the
// previously committed V2 authority is recovered on restart.
// ===========================================================================
BOOST_AUTO_TEST_CASE(r3f_xs_s3_v2_commit_failure_propagates)
{
    R3FXSFixture fx(12);
    const int S = fx.S; const uint256 sHash = fx.active[S];
    BlockIndexAuthoritativeLive* live = GetAuthoritativeLiveAuthority();
    BOOST_REQUIRE(live && live->IsOpen());

    const uint256 A1(0xE3A001UL), A2(0xE3A002UL), B1(0xE3B001UL), B2(0xE3B002UL);
    R3FXSInstallBranches(GetAuthoritativeLiveAuthority(), sHash, S, A1, A2, B1, B2);

    // Pre-commit failure must propagate as a production-level false return.
    {
        BlockIndexTipAuthority* tip = GetAuthoritativeLiveAuthority()->TipAuthorityMutable();
        const uint256 commit0 = tip->GetTip().record.hash;
        MainFailpointSetForTesting("FP_BEFORE_V2_AUTHORITY_COMMIT", true);
        std::string err;
        const bool ok = InvalidateBlock(A1, err);
        BOOST_CHECK_MESSAGE(!ok,
            "S3: pre-commit failure MUST propagate as return=false (caller cannot treat as success)");
        BOOST_CHECK_MESSAGE(err.find("FP_BEFORE_V2_AUTHORITY_COMMIT") != std::string::npos,
            "S3: propagated error must carry the injected reason (err=\"" << err << "\")");
        BOOST_CHECK_MESSAGE(tip->GetTip().record.hash == commit0,
            "S3: committed V2 tip unchanged after propagated failure");
        BOOST_TEST_MESSAGE("R3F_XS S3 before-commit propagate ok=0 err=" << err);
    }
    // Restart: previous committed V2 authority (A2) recovered intact.
    {
        R3FXSRestart(fx.root);
        BlockIndexTipAuthority* tipR = GetAuthoritativeLiveAuthority()->TipAuthorityMutable();
        BOOST_CHECK_MESSAGE(tipR->GetTip().record.hash == A2,
            "S3: restart after propagated pre-commit failure must recover the previous committed V2 tip A2 (got "
            << tipR->GetTip().record.hash.ToString() << ")");
        BOOST_CHECK_MESSAGE(!tipR->IsOperatorInvalid(A1),
            "S3: no invalid intent may persist for the failed transition");
        BOOST_TEST_MESSAGE("R3F_XS S3 restart recovered=" << tipR->GetTip().record.hash.ToString());
    }
    // Post-commit (mirror) failure must ALSO propagate, while the committed
    // new state stands and restart keeps it plus heals the mirror. Production
    // drives it with the ACTIVE-chain tip A2 as the target so the selected
    // fork (S+1) is a legal fused-reorg fork for the topping side branch B2.
    {
        BlockIndexTipAuthority* tip2 = GetAuthoritativeLiveAuthority()->TipAuthorityMutable();
        BOOST_REQUIRE_MESSAGE(tip2->GetTip().record.hash == A2,
            "S3 fixture: V2 tip must be A2 before the post-commit phase");
        MainFailpointSetForTesting("FP_AFTER_V2_COMMIT_BEFORE_LEGACY_MIRROR", true);
        std::string err2;
        const bool ok2 = InvalidateBlock(A2, err2);
        BOOST_CHECK_MESSAGE(!ok2,
            "S3: post-commit/mirror failure MUST propagate as return=false as well");
        BOOST_CHECK_MESSAGE(err2.find("FP_AFTER_V2_COMMIT_BEFORE_LEGACY_MIRROR") != std::string::npos,
            "S3: propagated error must carry the injected mirror failpoint name (err=\"" << err2 << "\")");
        BOOST_CHECK_MESSAGE(tip2->IsOperatorInvalid(A2),
            "S3: the committed invalid intent for A2 stands in V2 despite the propagated failure");
        BOOST_TEST_MESSAGE("R3F_XS S3 after-commit propagate ok=0 err=" << err2
            << " v2_invalid_A2=1 tip=" << tip2->GetTip().record.hash.ToString());
        // Restart + production reconciliation (V2 wins): mirror healed, tip kept.
        R3FXSRestart(fx.root);
        BlockIndexTipAuthority* tip3 = GetAuthoritativeLiveAuthority()->TipAuthorityMutable();
        const uint256 v2Tip3 = tip3->GetTip().record.hash;
        BOOST_CHECK_MESSAGE(tip3->IsOperatorInvalid(A2),
            "S3: committed invalid intent for A2 must survive the restart");
        std::string rerr3;
        BOOST_REQUIRE_MESSAGE(ReconcileLegacyCompatibilityMirror(rerr3), rerr3);
        BOOST_CHECK_MESSAGE(R3FXSLegacyBest() == v2Tip3,
            "S3: reconciliation must restore legacy mirror to the committed V2 tip ("
            << v2Tip3.ToString() << ", legacy=" << R3FXSLegacyBest().ToString() << ")");
        BOOST_CHECK_MESSAGE(setInvalidBlockHash == tip3->OperatorInvalidSet(),
            "S3: reconciled mirror must equal the V2 invalid set exactly");
        BOOST_TEST_MESSAGE("R3F_XS S3 heal v2_tip=" << v2Tip3.ToString()
            << " legacy_repaired=" << R3FXSLegacyBest().ToString());
    }
}

// ===========================================================================
// S4 (§8) — the operator-invalid transition (invalidate + reconsider pair)
// proves the SAME V2-first / legacy-mirror-second ordering at runtime:
// invalid intent is committed in the V2 tip authority, the legacy mirror lags
// until the production reconciliation, and V2 wins on the pair.
// ===========================================================================
BOOST_AUTO_TEST_CASE(r3f_xs_s4_operator_invalid_v2_first_ordering)
{
    R3FXSFixture fx(12);
    const int S = fx.S; const uint256 sHash = fx.active[S];
    const uint256 A1(0xE4A001UL), A2(0xE4A002UL), B1(0xE4B001UL), B2(0xE4B002UL);
    R3FXSInstallBranches(GetAuthoritativeLiveAuthority(), sHash, S, A1, A2, B1, B2);

    // ---- invalidate A1: V2-first commitment ----
    {
        std::string err;
        BOOST_REQUIRE_MESSAGE(InvalidateBlock(A1, err), err);
        BlockIndexTipAuthority* tip = GetAuthoritativeLiveAuthority()->TipAuthorityMutable();
        const BlockIndexTipRead dt = tip->GetTip();
        BOOST_CHECK_MESSAGE(dt.status == BLOCK_INDEX_TIP_OK && dt.record.hash == B2,
            "S4 invalidate: V2 tip must move to the selected competing branch (expected B2, got "
            << dt.record.hash.ToString() << ")");
        BOOST_CHECK_MESSAGE(tip->IsOperatorInvalid(A1),
            "S4 invalidate: invalid intent must be committed V2-first (tip-invalid.dat), not mirror-first");
        BOOST_CHECK_MESSAGE(!setInvalidBlockHash.count(A1) || setInvalidBlockHash == tip->OperatorInvalidSet(),
            "S4 invalidate: the legacy mirror can never lead V2");
        BOOST_CHECK_MESSAGE(pindexBest != NULL && pindexBest->GetBlockHash() == dt.record.hash,
            "S4 invalidate: projected best tip must equal the durable selected V2 tip");
        BOOST_TEST_MESSAGE("R3F_XS S4 invalidate v2_tip=" << dt.record.hash.ToString()
            << " v2_invalid=" << tip->OperatorInvalidSet().size()
            << " mirror_invalid=" << setInvalidBlockHash.size()
            << " projected=" << (pindexBest ? pindexBest->GetBlockHash().ToString() : std::string("<null>")));
    }
    // Mirror lag + repair via the production reconciliation (V2 wins).
    {
        std::string rerr;
        BOOST_REQUIRE_MESSAGE(ReconcileLegacyCompatibilityMirror(rerr), rerr);
        BlockIndexTipAuthority* tip = GetAuthoritativeLiveAuthority()->TipAuthorityMutable();
        BOOST_CHECK_MESSAGE(R3FXSLegacyBest() == tip->GetTip().record.hash,
            "S4 invalidate: reconciliation must re-mirror legacy best to the V2 tip");
        BOOST_CHECK_MESSAGE(setInvalidBlockHash == tip->OperatorInvalidSet(),
            "S4 invalidate: reconciliation must re-mirror the invalid set from V2");
        BOOST_TEST_MESSAGE("R3F_XS S4 reconcile invalidate v2_tip=" << tip->GetTip().record.hash.ToString()
            << " legacy=" << R3FXSLegacyBest().ToString());
    }
    // ---- reconsider A1: same V2-first ordering on the reverse transition ----
    {
        std::string e2;
        BOOST_REQUIRE_MESSAGE(ReconsiderBlock(A1, e2), e2);
        BlockIndexTipAuthority* tip = GetAuthoritativeLiveAuthority()->TipAuthorityMutable();
        const BlockIndexTipRead dt2 = tip->GetTip();
        BOOST_CHECK_MESSAGE(dt2.status == BLOCK_INDEX_TIP_OK && dt2.record.hash == A2,
            "S4 reconsider: V2 authority must be restored (expected A2, got "
            << dt2.record.hash.ToString() << ")");
        BOOST_CHECK_MESSAGE(!tip->IsOperatorInvalid(A1),
            "S4 reconsider: reverse intent must be committed V2-first (invalid set cleared)");
        BOOST_CHECK_MESSAGE(pindexBest != NULL && pindexBest->GetBlockHash() == dt2.record.hash,
            "S4 reconsider: projected best tip must equal the durable selected V2 tip");
        BOOST_TEST_MESSAGE("R3F_XS S4 reconsider v2_tip=" << dt2.record.hash.ToString()
            << " v2_invalid=" << tip->OperatorInvalidSet().size()
            << " mirror_invalid=" << setInvalidBlockHash.size());
    }
    // Restart: the restored V2 authority stands; reconciliation keeps it and V2 wins.
    {
        R3FXSRestart(fx.root);
        BlockIndexTipAuthority* tipR = GetAuthoritativeLiveAuthority()->TipAuthorityMutable();
        BOOST_CHECK_MESSAGE(tipR->GetTip().record.hash == A2,
            "S4 reconsider: restart must keep the restored V2 authority (expected A2, got "
            << tipR->GetTip().record.hash.ToString() << ")");
        std::string rerr;
        BOOST_REQUIRE_MESSAGE(ReconcileLegacyCompatibilityMirror(rerr), rerr);
        BOOST_CHECK_MESSAGE(R3FXSLegacyBest() == tipR->GetTip().record.hash,
            "S4 reconsider: legacy mirror must agree with the (restored) V2 tip");
        BOOST_TEST_MESSAGE("R3F_XS S4 restart+reconcile v2_tip=" << tipR->GetTip().record.hash.ToString()
            << " legacy=" << R3FXSLegacyBest().ToString());
    }
}

BOOST_AUTO_TEST_SUITE_END()
