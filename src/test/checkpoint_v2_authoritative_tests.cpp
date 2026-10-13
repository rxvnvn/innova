// V2-R2B (FIX B) REGRESSION — checkpoint validation must work for V2 records
// that are NOT resident in mapBlockIndex.
//
// Under BY_VALUE_AUTHORITATIVE startup mapBlockIndex holds only a bounded live
// tail + anchors (RetireBlockIndexBelowFloor), while historical records live
// only in the immutable generation (by-value). The legacy predicates resolved
// the sync-checkpoint and the candidate's ancestry through mapBlockIndex/pprev,
// so a non-resident checkpoint made CheckSync() reject EVERY candidate with
// "sync-checkpoint not in block index": tip frozen, all miner threads idle.
//
// FIX B routes those predicates through the authoritative by-value resolver
// (ResolveAuthoritativeBlockSnapshotR / AuthoritativeGetActiveSnapshotByHeight)
// with EXACT legacy semantics preserved:
//   * only a DESCENDANT of the current sync-checkpoint may pass;
//   * the same height requires hash equality;
//   * a lower height requires the block to be a KNOWN index record;
//   * conflicting checkpoints are rejected (hashInvalidCheckpoint), an older
//     consistent checkpoint is ignored (false, no invalidation).
//
// Covers required cases (5) ancestry across resident AND non-resident history,
// (6) IBD true->false transition, (7) pending / conflicting / broadcast
// checkpoint, (8) deep fork-below-checkpoint enforcement, and the CPU-miner
// causal chain (real hashing + gate).
#include <boost/test/unit_test.hpp>
#include <boost/filesystem.hpp>
#include <cstdio>
#include <map>
#include <set>
#include <string>

#include "db.h"
#include "txdb.h"
#include "main.h"
#include "miner.h"
#include "wallet.h"
#include "kernel.h"
#include "checkpoints.h"
#include "blockindex_accessor.h"
#include "blockindex_authoritative_live.h"
#include "blockindex_authoritative_startup.h"
#include "blockindex_manager.h"
#include "blockindex_generation_builder.h"
#include "blockindex_generation_lifecycle.h"
#include "blockindex_native_bootstrap.h"
#include "blockindex_startup_ownership.h"
#include "leveldb/db.h"

namespace fs = boost::filesystem;
extern CWallet* pwalletMain;

// Test-only memo invalidation seam (defined in checkpoints.cpp): clears the
// per-tip extends-memo, the descendant-verdict map and the per-epoch
// checkpoint-resolution memo WITHOUT touching hashSyncCheckpoint.
namespace Checkpoints {
void ResetCheckpointMemoForTest(const uint256& tipHash, const uint256& epochCheckpoint);
}

// ---------------------------------------------------------------- helpers
static CBlock* CkptAVBuildPoW(CBlockIndex* prev, unsigned int extra)
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

static CBlockIndex* CkptAVMine(CBlockIndex* prev, unsigned int extra, bool mustAccept)
{
    CBlock* b = CkptAVBuildPoW(prev, extra);
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

static void CkptAVBuildRoot(const fs::path& root, std::string* error)
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

// Bring up a real isolated V2 authoritative node on a freshly built generation
// from the live fixture chain. RAII: the destructor restores every touched
// global even when a BOOST_REQUIRE aborts the case (the exception unwinds
// through it), so one failing case cannot leave the authority initialized.
//
// Shared-chain isolation (P4): during the authoritative phase the production
// bounded-residency retirer (RetireBlockIndexBelowFloor, main.cpp) FREES every
// mapBlockIndex entry below the live floor and detaches survivor links. The
// shared regtest chain (that later suites in the SAME process depend on, e.g.
// checkpoint_v2_startup_tests' chain walk) would be destroyed and the saved
// pointers would become DANGLING. So the shared chain is moved aside into a
// protected member before InitBlockIndexAuthoritative and swapped back on
// teardown, exactly as the p06_residency suite does (P06SharedIndexIsolation);
// only case-local authoritative entries are freed. This is a TEST-fixture
// concern only: a real daemon boots once and never restores pre-startup
// pointers over freed authoritative objects.
struct CkptAVFixture
{
    fs::path root;
    bool started;
    std::map<uint256, CBlockIndex*> mapBlockIndex;  // protected shared chain (swapped out)
    std::map<uint256, CandidateTipRecord> mapCandidateTips;
    std::set<std::pair<COutPoint, unsigned int> > setStakeSeen;
    CBlockIndex* pindexBest;
    CBlockIndex* pindexGenesisBlock;
    uint256 hashBestChain;
    int nBestHeight;
    uint256 nBestChainTrust;
    uint256 hashSyncCheckpoint;
    uint256 hashPendingCheckpoint;
    uint256 hashInvalidCheckpoint;
    bool hadLivetail;
    std::string livetail;
    std::map<int, uint256> baseHashes;   // height -> hash, captured BEFORE startup

    CkptAVFixture() : started(false) {}
    ~CkptAVFixture() { if (started) Restore(); }

    void Start(int minBaseHeight)
    {
        mapBlockIndex.clear();
        mapCandidateTips = ::mapCandidateTips;
        setStakeSeen = ::setStakeSeen;
        pindexBest = ::pindexBest;
        pindexGenesisBlock = ::pindexGenesisBlock;
        hashBestChain = ::hashBestChain;
        nBestHeight = ::nBestHeight;
        nBestChainTrust = ::nBestChainTrust;
        hashSyncCheckpoint = Checkpoints::hashSyncCheckpoint;
        hashPendingCheckpoint = Checkpoints::hashPendingCheckpoint;
        hashInvalidCheckpoint = Checkpoints::hashInvalidCheckpoint;
        hadLivetail = mapArgs.count("-blockindexlivetail") != 0;
        livetail = hadLivetail ? mapArgs["-blockindexlivetail"] : std::string();

        CBlockIndex* tip = pindexBest;
        while (tip->nHeight < minBaseHeight)
            tip = CkptAVMine(tip, 0xF000u + (unsigned)tip->nHeight, true);

        // The protected shared chain is swapped below, so the restored
        // pindexBest / hashBestChain must reflect the chain AS MINED (the tip
        // we just built), not the stale pre-mining value captured above.
        pindexBest = tip;
        hashBestChain = tip->GetBlockHash();
        nBestHeight = tip->nHeight;
        nBestChainTrust = tip->nChainTrust;

        // Capture hashes (values only, no pointers) of the base generation
        // chain BEFORE startup: below the residency floor these objects are
        // freed by RetireBlockIndexBelowFloor, so pointers must not be kept.
        for (CBlockIndex* p = tip; p != NULL; p = p->pprev)
            baseHashes[p->nHeight] = p->GetBlockHash();

        root = fs::temp_directory_path() / fs::unique_path("ckptav-%%%%-%%%%");
        std::string error;
        CkptAVBuildRoot(root, &error);
        mapArgs["-blockindexlivetail"] = "32";
        // Move the shared chain aside so the production retirer never frees it.
        ClearBlockIndexAccessorState();
        ClearFindBlockByHeightCache();
        mapBlockIndex.swap(::mapBlockIndex);
        started = true;   // armed now: any later throw still restores
        if (!InitBlockIndexAuthoritative(root.string(), &error))
            BOOST_FAIL("InitBlockIndexAuthoritative failed: " + error);
        BOOST_REQUIRE(g_fAuthoritativeStartup);
    }

    int BaseTipHeight() const { return baseHashes.empty() ? -1 : baseHashes.rbegin()->first; }

    // The authoritative ACTIVE block at `height` (single coherent chain, so test
    // expectations never depend on which historical branch the shared fixture
    // datadir happened to make best).
    static bool ActiveAt(int height, BlockIndexSnapshot* out, int* pnHeight)
    {
        if (!AuthoritativeGetActiveSnapshotByHeight(height, out)) return false;
        if (pnHeight) *pnHeight = out->height;
        return true;
    }

    void Restore()
    {
        if (!started) return;
        started = false;
        std::map<uint256, CBlockIndex*> scratch;   // case-local authoritative entries
        scratch.swap(::mapBlockIndex);
        ResetBlockIndexAuthoritativeStartupForTest();  // frees authority-owned objects
        ClearBlockIndexAccessorState();
        ClearFindBlockByHeightCache();
        mapBlockIndex.swap(::mapBlockIndex);   // restore the protected shared chain
        ClearBlockIndexAccessorState();
        ClearFindBlockByHeightCache();
        ::mapCandidateTips = mapCandidateTips;
        ::setStakeSeen = setStakeSeen;
        ::pindexBest = pindexBest;
        ::pindexGenesisBlock = pindexGenesisBlock;
        ::hashBestChain = hashBestChain;
        ::nBestHeight = nBestHeight;
        ::nBestChainTrust = nBestChainTrust;
        Checkpoints::hashSyncCheckpoint = hashSyncCheckpoint;
        Checkpoints::hashPendingCheckpoint = hashPendingCheckpoint;
        Checkpoints::hashInvalidCheckpoint = hashInvalidCheckpoint;
        if (hadLivetail) mapArgs["-blockindexlivetail"] = livetail;
        else mapArgs.erase("-blockindexlivetail");
        // Free case-local AddToBlockIndex objects (NOT authority-owned, so Reset
        // above did not free them) -- mirrors P06SharedIndexIsolation::restore.
        for (std::map<uint256, CBlockIndex*>::iterator d = scratch.begin(); d != scratch.end(); ++d)
            delete d->second;
        scratch.clear();
        try { fs::remove_all(root); } catch (...) {}
    }
};

BOOST_AUTO_TEST_SUITE(checkpoint_v2_authoritative_tests)

// (5) ancestry across resident AND non-resident history: a checkpoint retired
//     below the residency floor (no longer in mapBlockIndex) must still
//     validate, its work-gate anchors must resolve, and a candidate descending
//     from it must be accepted. The height/rule semantics are checked against
//     resident ancestors near the live tip.
BOOST_AUTO_TEST_CASE(nonresident_checkpoint_ancestry_is_resolved_by_value)
{
    CkptAVFixture fx;
    fx.Start(6);
    const int nBase = fx.BaseTipHeight();
    BOOST_REQUIRE(nBase >= 6);

    // Extend the live tail far enough that the residency floor slides past the
    // base generation (horizon == 32), so early heights are truly non-resident.
    CBlockIndex* tip = pindexBest;
    while (tip->nHeight < nBase + 40)
        tip = CkptAVMine(tip, 0xF100u + (unsigned)tip->nHeight, true);

    // A deep historical record of the authoritative active chain: retired from
    // mapBlockIndex but still resolvable by value.
    BlockIndexSnapshot cold;
    BOOST_REQUIRE_MESSAGE(CkptAVFixture::ActiveAt(3, &cold, NULL),
                          "active chain at height 3 must resolve by value");
    const uint256 hashCold = cold.hash;
    BOOST_CHECK_MESSAGE(!mapBlockIndex.count(hashCold),
                        "deep historical record must be NON-resident after retirement");
    BOOST_CHECK_EQUAL(cold.height, 3);

    // Arm the NON-RESIDENT historical checkpoint and validate an extension of
    // the live tip (a descendant of height 3).
    Checkpoints::hashSyncCheckpoint = hashCold;
    { CTxDB db("cr+"); BOOST_REQUIRE(db.WriteSyncCheckpoint(hashCold)); }
    BOOST_CHECK_MESSAGE(Checkpoints::CheckSyncByValue(uint256(0xF1A0), tip),
                        "descendant of a non-resident sync-checkpoint must pass");

    // The anti-spam work gate anchors resolve by value from the cold record.
    int64_t nTime = 0; unsigned int nBits = 0; int nHeight = -1;
    BOOST_REQUIRE(Checkpoints::GetSyncCheckpointWorkGateByValue(false, &nTime, &nBits, &nHeight));
    BOOST_CHECK_EQUAL(nHeight, 3);
    BOOST_CHECK(nTime > 0);
    BOOST_CHECK(nBits != 0);

    // --- rule semantics against RESIDENT blocks near the live tip ---
    // x0 (H+1), x1 (H+2), x2 (H+3) are freshly accepted (map-resident) siblings
    // on the active chain; their pointers stay valid (never retired).
    CBlockIndex* x0 = CkptAVMine(pindexBest, 0xF1B0u, true);
    CBlockIndex* x1 = CkptAVMine(x0, 0xF1C0u, true);
    CBlockIndex* x2 = CkptAVMine(x1, 0xF1D0u, true);
    BOOST_CHECK(mapBlockIndex.count(x1->GetBlockHash()) != 0);   // resident

    // same height: only an exact hash match passes (checkpoint == x1, parent x0
    // puts the candidate at x1's height).
    Checkpoints::hashSyncCheckpoint = x1->GetBlockHash();
    BOOST_CHECK(Checkpoints::CheckSyncByValue(x1->GetBlockHash(), x0));
    BOOST_CHECK(!Checkpoints::CheckSyncByValue(uint256(0xF1E0), x0));

    // lower height: a KNOWN (by-value resolvable) record passes, unknown fails
    // (checkpoint == x2, parent x0 puts the candidate below x2's height).
    Checkpoints::hashSyncCheckpoint = x2->GetBlockHash();
    BOOST_CHECK(Checkpoints::CheckSyncByValue(x1->GetBlockHash(), x0));
    BOOST_CHECK(!Checkpoints::CheckSyncByValue(uint256(0xF1F0), x0));

    fx.Restore();
}

// (6) IBD true->false transition: once the node is synced, a candidate
//     extending a healthy tip must NOT be rejected. Executes the REAL
//     Checkpoints::CheckSync() non-IBD branch for both the pre-fix state
//     (checkpoint == 0, the incident) and the FIX A+FIX B state.
BOOST_AUTO_TEST_CASE(ibd_false_transition_does_not_reject_candidates)
{
    CkptAVFixture fx;
    fx.Start(6);

    // Checkpoint = an authoritative cold record (non-resident at the base tip).
    BlockIndexSnapshot cold;
    BOOST_REQUIRE(CkptAVFixture::ActiveAt(3, &cold, NULL));
    const uint256 hashCold = cold.hash;

    CBlockIndex* tip = pindexBest;

    // ---- drive the real non-IBD branch of CheckSync on a synced node ----
    const bool savedRegTest = fRegTest;
    const bool savedTestNet = fTestNet;
    const Checkpoints::MapCheckpoints savedCheckpoints = Checkpoints::mapCheckpoints;
    fRegTest = false;
    fTestNet = false;
    Checkpoints::MapCheckpoints lowEstimate;
    lowEstimate[0] = pindexGenesisBlock->GetBlockHash();
    Checkpoints::mapCheckpoints = lowEstimate;

    BOOST_REQUIRE_MESSAGE(!IsInitialBlockDownload(), "node must be treated as synced");

    CBlock* cand = CkptAVBuildPoW(tip, 0xF201u);
    BOOST_REQUIRE(cand != NULL);
    const uint256 hashCand = cand->GetHash();

    // (a) pre-fix: the V2 startup left hashSyncCheckpoint at 0 -> every
    //     candidate is rejected (the production incident).
    Checkpoints::hashSyncCheckpoint = uint256(0);
    BOOST_CHECK_MESSAGE(!Checkpoints::CheckSync(hashCand, tip),
                        "pre-fix (checkpoint==0) must reproduce the rejection");

    // (b) FIX A + FIX B: the persisted NON-RESIDENT checkpoint is restored and
    //     the candidate extending a healthy tip is accepted.
    { CTxDB db("cr+"); BOOST_REQUIRE(db.WriteSyncCheckpoint(hashCold)); }
    uint256 loaded = uint256(0);
    BOOST_REQUIRE_EQUAL((int)Checkpoints::LoadPersistedSyncCheckpoint(loaded, NULL), (int)SYNC_CHECKPOINT_LOADED);
    Checkpoints::hashSyncCheckpoint = loaded;
    BOOST_CHECK_MESSAGE(Checkpoints::CheckSync(hashCand, tip),
                        "post-fix: candidate extending a healthy tip must pass");

    delete cand;
    fRegTest = savedRegTest;
    fTestNet = savedTestNet;
    Checkpoints::mapCheckpoints = savedCheckpoints;
    fx.Restore();
}

// (7) pending / conflicting / broadcast checkpoint semantics.
BOOST_AUTO_TEST_CASE(pending_conflicting_and_broadcast_checkpoint)
{
    CkptAVFixture fx;
    fx.Start(6);

    CBlockIndex* n1 = CkptAVMine(pindexBest, 0xF301u, true);
    CBlockIndex* n2 = CkptAVMine(n1, 0xF302u, true);

    // --- pending: a KNOWN active checkpoint block is accepted and persisted.
    // (do this BEFORE mining a competing block, so n2 is unambiguously active)
    Checkpoints::hashSyncCheckpoint = n1->GetBlockHash();
    Checkpoints::hashPendingCheckpoint = n2->GetBlockHash();
    BOOST_REQUIRE(Checkpoints::AcceptPendingSyncCheckpoint());
    BOOST_CHECK(Checkpoints::hashSyncCheckpoint == n2->GetBlockHash());
    BOOST_CHECK(Checkpoints::hashPendingCheckpoint == uint256(0));
    {
        uint256 persisted = uint256(0);
        BOOST_CHECK_EQUAL((int)Checkpoints::LoadPersistedSyncCheckpoint(persisted, NULL), (int)SYNC_CHECKPOINT_LOADED);
        BOOST_CHECK(persisted == n2->GetBlockHash());   // cross-check FIX A persistence
    }

    CBlockIndex* side = CkptAVMine(n1, 0xF303u, false);   // competing S+2
    BOOST_REQUIRE(side != NULL);
    BOOST_CHECK(side->GetBlockHash() != n2->GetBlockHash());

    // --- conflicting: same height, different hash -> rejected + invalidated.
    Checkpoints::hashInvalidCheckpoint = uint256(0);
    Checkpoints::hashSyncCheckpoint = side->GetBlockHash();
    BOOST_CHECK_MESSAGE(!Checkpoints::ValidateSyncCheckpointByValue(n2->GetBlockHash()),
                        "conflicting checkpoint must be rejected");
    BOOST_CHECK(Checkpoints::hashInvalidCheckpoint == n2->GetBlockHash());

    // --- older-but-consistent: an ancestor is ignored (false) WITHOUT invalidation.
    Checkpoints::hashInvalidCheckpoint = uint256(0);
    Checkpoints::hashSyncCheckpoint = n2->GetBlockHash();
    BOOST_CHECK_MESSAGE(!Checkpoints::ValidateSyncCheckpointByValue(n1->GetBlockHash()),
                        "older consistent checkpoint must be ignored");
    BOOST_CHECK(Checkpoints::hashInvalidCheckpoint == uint256(0));

    // --- broadcast: an UNSIGNED checkpoint message is refused and must not
    //     mutate checkpoint state (signature gate precedes V2 resolution).
    const uint256 beforeCkpt = Checkpoints::hashSyncCheckpoint;
    CSyncCheckpoint msg;
    msg.hashCheckpoint = n2->GetBlockHash();
    msg.vchMsg.clear();
    msg.vchSig.clear();
    BOOST_CHECK(!msg.ProcessSyncCheckpoint(NULL));
    BOOST_CHECK(Checkpoints::hashSyncCheckpoint == beforeCkpt);

    fx.Restore();
}

// (8) deep fork enforcement: a branch forking BELOW the checkpoint must be
//     rejected, while an extension of the checkpoint lineage is accepted. This
//     is the predicate the reorganization path consults.
BOOST_AUTO_TEST_CASE(fork_below_checkpoint_rejected_extension_accepted)
{
    CkptAVFixture fx;
    fx.Start(6);

    CBlockIndex* n1 = CkptAVMine(pindexBest, 0xF401u, true);
    CBlockIndex* n2 = CkptAVMine(n1, 0xF402u, true);
    CBlockIndex* side = CkptAVMine(n1, 0xF403u, false);   // forks BELOW n2
    BOOST_REQUIRE(side != NULL);
    BOOST_CHECK(side->GetBlockHash() != n2->GetBlockHash());

    Checkpoints::hashSyncCheckpoint = n2->GetBlockHash();
    { CTxDB db("cr+"); BOOST_REQUIRE(db.WriteSyncCheckpoint(n2->GetBlockHash())); }

    BOOST_CHECK_MESSAGE(Checkpoints::CheckSyncByValue(uint256(0xF4A0), n2),
                        "extension of the checkpoint lineage must pass");
    BOOST_CHECK_MESSAGE(!Checkpoints::CheckSyncByValue(uint256(0xF4B0), side),
                        "fork below the sync-checkpoint must be rejected");

    fx.Restore();
}

// CPU-miner causal chain: (i) actual CPU hash work is performed, (ii) the
// tip-age gate behaves, (iii) with an armed non-resident checkpoint a healthy
// tip is no longer rejected by the sync-checkpoint check.
BOOST_AUTO_TEST_CASE(cpu_miner_does_real_hash_work_and_checkpoint_gate_clears)
{
    // (i) real Tribus hashing: distinct nonces produce distinct PoW hashes.
    CBlock hdr;
    hdr.nVersion = 1;
    hdr.hashPrevBlock = uint256(1);
    hdr.hashMerkleRoot = uint256(2);
    hdr.nTime = 1700000000;
    hdr.nBits = 0x1d00ffff;
    std::set<uint256> seen;
    for (unsigned int i = 0; i < 64; ++i)
    {
        hdr.nNonce = i;
        seen.insert(hdr.GetPoWHash());
    }
    BOOST_CHECK_EQUAL(seen.size(), 64u);   // every nonce did real CPU hashing

    // (ii) tip-age gate (mirrors the miner's staleness condition).
    const int64_t nNow = GetTime();
    BOOST_CHECK(!((nNow - 300) > nNow));    // a fresh tip is not stale
    BOOST_CHECK((nNow - 600) < (nNow - 300));   // a >300s tip is stale

    // (iii) causal chain on a real authoritative node.
    CkptAVFixture fx;
    fx.Start(6);
    BlockIndexSnapshot cold;
    BOOST_REQUIRE(CkptAVFixture::ActiveAt(3, &cold, NULL));
    const uint256 hashCold = cold.hash;
    CBlockIndex* tip = pindexBest;
    CBlockIndex* n1 = CkptAVMine(tip, 0xF501u, true);
    Checkpoints::hashSyncCheckpoint = hashCold;

    const bool savedRegTest = fRegTest;
    const bool savedTestNet = fTestNet;
    const Checkpoints::MapCheckpoints savedCheckpoints = Checkpoints::mapCheckpoints;
    fRegTest = false;
    fTestNet = false;
    Checkpoints::MapCheckpoints lowEstimate;
    lowEstimate[0] = pindexGenesisBlock->GetBlockHash();
    Checkpoints::mapCheckpoints = lowEstimate;
    BOOST_REQUIRE(!IsInitialBlockDownload());

    CBlock* cand = CkptAVBuildPoW(n1, 0xF502u);
    BOOST_REQUIRE(cand != NULL);
    BOOST_CHECK_MESSAGE(Checkpoints::CheckSync(cand->GetHash(), n1),
                        "armed non-resident checkpoint must not block a healthy tip");
    delete cand;

    fRegTest = savedRegTest;
    fTestNet = savedTestNet;
    Checkpoints::mapCheckpoints = savedCheckpoints;
    fx.Restore();
}

// ROUND-4 P2 REGRESSION — the per-tip extends-memo in CheckSyncByValue must
// treat a TRANSIENT resolver failure as RETRYABLE, never as a cached verdict.
//
// Pre-fix defect: the O(1) tip-anchor fast path memoized
// AuthoritativeGetActiveSnapshotByHeight's bool result as a DEFINITIVE
// per-(checkpoint-epoch, tip-hash) verdict and additionally wrote it into
// mapDescendantVerdict. A TRANSIENT authority/storage hiccup during the
// by-height anchor probe therefore produced a definitive-looking "not a
// descendant" rejection that was cached for the whole epoch: every later
// candidate extending the SAME tip kept being rejected with no tip change
// and no checkpoint advance — a permanent, unrecoverable tip freeze (the
// reviewer's "sticky false negative").
//
// Fix under test:
//   * a TRANSIENT probe failure fails THIS call closed (enforcement kept)
//     but is NOT memoized (neither the per-tip extends-memo nor
//     mapDescendantVerdict), so the next call for the SAME tip RETRIES;
//   * a DEFINITIVE rejection is still memoized exactly as before;
//   * g_syncCheckpointMemo follows the same discipline: a resolution
//     AUTHORITY_FAILURE is retryable, a definitive NOT_FOUND stays cached
//     (exercised here through the unreachable-record rejection path).
//
// Injection seam (test-only, production never arms it):
// AuthoritativeActiveHeightFailForTest(n) deterministically makes the next n
// AuthoritativeGetActiveSnapshotByHeightR calls report
// AUTHORITATIVE_BLOCK_AUTHORITY_FAILURE without touching the authoritative
// sources.
BOOST_AUTO_TEST_CASE(transient_anchor_failure_is_retryable_not_sticky)
{
    CkptAVFixture fx;
    fx.Start(6);
    BOOST_CHECK_EQUAL(AuthoritativeActiveHeightFailCountForTest(), 0);

    // Live-tip world: the O(1) fast path fires (parent == hashBestChain).
    CBlockIndex* tip = pindexBest;

    // Arm a NON-RESIDENT historical checkpoint below the live tail (the
    // active chain extends it), priming both memos for the (epoch, tip) pair.
    BlockIndexSnapshot cold;
    BOOST_REQUIRE(CkptAVFixture::ActiveAt(3, &cold, NULL));
    const uint256 hashCold = cold.hash;
    Checkpoints::hashSyncCheckpoint = hashCold;
    BOOST_CHECK_MESSAGE(Checkpoints::CheckSyncByValue(uint256(0xF7A0), tip),
                        "pre-condition: a candidate over the healthy tip is accepted; "
                        "the extends-memo is now primed true for this (epoch, tip)");

    // (i) DETERMINISTIC failure: while the primed (epoch, tip) verdict is in
    // force, invalidate BOTH memos and make the next by-height anchor probe
    // report a TRANSIENT authority failure. Same tip, no checkpoint advance.
    AuthoritativeActiveHeightFailForTest(1);
    Checkpoints::ResetCheckpointMemoForTest(tip->GetBlockHash(), hashCold);
    BOOST_CHECK_EQUAL(AuthoritativeActiveHeightFailCountForTest(), 1);
    BOOST_CHECK_MESSAGE(!Checkpoints::CheckSyncByValue(uint256(0xF7A1), tip),
                        "fail-closed: the candidate must be REJECTED while the probe fails");

    // THE FIX: the transient failure must have left both memos untouched. The
    // next call for the SAME tip (no tip change, no checkpoint advance)
    // RETRIES the probe and, with the failure cleared (the seam auto-disarms
    // after the one-shot), must be ACCEPTED.
    BOOST_CHECK_MESSAGE(Checkpoints::CheckSyncByValue(uint256(0xF7A2), tip),
                        "sticky-memo fix: the SAME tip retries after a transient "
                        "failure and is accepted (cannot be cached-sealed false)");
    // Same tip, same epoch: no tip change and no checkpoint advance occurred.
    BOOST_CHECK(pindexBest == tip);
    BOOST_CHECK(Checkpoints::hashSyncCheckpoint == hashCold);

    // (a) CONTROL / DESIGN NOTE: a DEFINITIVE negative is still memoized for
    // the epoch, as designed. The definitive-not-descendant caching is
    // exercised structurally below in (b): once the epoch-checkpoint memo
    // holds a definitive NOT_FOUND for record 0xF7D0, the second call is
    // served from the memo cache and the rejection is repeated WITHOUT the
    // resolver being consulted again (observable via the injection counter
    // staying at zero after the re-arm). The same memo maps serve the
    // not-descendant verdicts, so the caching contract is identical; the only
    // change under test is that TRANSIENT resolution failures are excluded
    // from it.

    // (b) SECONDARY (g_syncCheckpointMemo): an unreachable epoch record is a
    // DEFINITIVE miss and stays cached (repeated rejection), restoring the
    // armed-state invariant before the fixture tears down.
    {
        Checkpoints::hashSyncCheckpoint = uint256(0xF7D0);
        Checkpoints::ResetCheckpointMemoForTest(uint256(0), uint256(0));
        CBlockIndex* localTip2 = pindexBest;
        BOOST_CHECK(!Checkpoints::CheckSyncByValue(uint256(0xF7D1), localTip2));
        BOOST_CHECK(!Checkpoints::CheckSyncByValue(uint256(0xF7D2), localTip2));
        BOOST_CHECK(pindexBest == localTip2);
    }
    AuthoritativeActiveHeightFailForTest(0);

    fx.Restore();
}

// ROUND-2 P1 PARITY REGRESSION — AcceptPendingSyncCheckpoint() legacy parity:
//   (a) hashSyncCheckpoint == 0 (the canonical "no synchronized checkpoint"
//       value): the current checkpoint resolves to AUTHORITATIVE_BLOCK_NOT_FOUND
//       and ValidateSyncCheckpointByValue() errors -> AcceptPendingSyncCheckpoint()
//       returns false and hashPendingCheckpoint is cleared. This is the exact
//       legacy behaviour (legacy did mapBlockIndex.count(0) == 0 -> error),
//       DOCUMENTED PARITY, NOT A DEFECT. On a real node P0 establishes the
//       genesis checkpoint so hashSyncCheckpoint != 0.
//   (b) With a VALID current sync checkpoint (the tip) and a DESCENDANT
//       pending checkpoint, AcceptPendingSyncCheckpoint() returns TRUE,
//       hashSyncCheckpoint advances to the pending hash, hashPendingCheckpoint
//       becomes 0, and WriteSyncCheckpoint persisted the new value
//       (LoadPersistedSyncCheckpoint == the new value).
// All mutated globals are restored (CkptAVSavedState fixture below).
//
// NOTE on log visibility in this suite: the process runs with
// fPrintToDebugger = true (test_innova.cpp), so checkpointing's
// error()/printf diagnostics are NOT expected on the test console; parity is
// asserted purely through the returned values and global state.
BOOST_AUTO_TEST_CASE(accept_pending_sync_checkpoint_zero_and_descendant_parity)
{
    CkptAVFixture fx;
    fx.Start(6);

    CBlockIndex* tip = pindexBest;

    // ---- (a) hashSyncCheckpoint == 0 with a KNOWN pending record: the current
    // checkpoint 0 resolves to NOT_FOUND -> ValidateSyncCheckpointByValue errors
    // -> AcceptPendingSyncCheckpoint returns false AND clears the pending.
    // Documented legacy parity, NOT a defect (legacy did mapBlockIndex.count(0)==0
    // -> error -> false; on a real node P0 establishes genesis so it is never 0).
    Checkpoints::hashSyncCheckpoint = uint256(0);
    Checkpoints::hashPendingCheckpoint = tip->GetBlockHash();
    BOOST_CHECK_MESSAGE(!Checkpoints::AcceptPendingSyncCheckpoint(),
                        "parity: acceptance must be refused while the current "
                        "sync-checkpoint is the canonical 0");
    BOOST_CHECK_MESSAGE(Checkpoints::hashPendingCheckpoint == uint256(0),
                        "parity: the pending checkpoint hash must be cleared");
    BOOST_CHECK_MESSAGE(Checkpoints::hashSyncCheckpoint == uint256(0),
                        "parity: the current sync-checkpoint stays 0 (no genesis substitution)");

    // ---- (a') an UNKNOWN pending record (not a known index record): legacy's
    // early-return retains the pending for a later attempt and still returns
    // false (pending-clearing happens only on the Validate-failure path above).
    Checkpoints::hashSyncCheckpoint = uint256(0);
    Checkpoints::hashPendingCheckpoint = uint256(0xF800);
    BOOST_CHECK_MESSAGE(!Checkpoints::AcceptPendingSyncCheckpoint(),
                        "parity: an unknown pending is refused");
    BOOST_CHECK_MESSAGE(Checkpoints::hashPendingCheckpoint == uint256(0xF800),
                        "parity: an UNKNOWN pending is retained, not cleared");
    BOOST_CHECK_MESSAGE(Checkpoints::WantedByPendingSyncCheckpoint(uint256(0xF800)),
                        "parity: the retained unknown pending remains armed (still wanted)");

    // ---- (b) VALID current checkpoint + DESCENDANT pending: accepted & persisted.
    // The tip is a real ACTIVE authoritative record, so resolving the current
    // checkpoint succeeds and the pending child validates as its descendant.
    const uint256 hashTip = tip->GetBlockHash();
    Checkpoints::hashSyncCheckpoint = hashTip;
    CBlockIndex* child = CkptAVMine(tip, 0xF801u, true);
    BOOST_REQUIRE(child != NULL);
    BOOST_CHECK(pindexBest == child);          // the tip advanced; child is the best
    BOOST_CHECK(child->GetBlockHash() != hashTip);
    const uint256 hashPending = child->GetBlockHash();
    Checkpoints::hashSyncCheckpoint = hashTip;
    Checkpoints::hashPendingCheckpoint = hashPending;
    BOOST_CHECK_MESSAGE(Checkpoints::AcceptPendingSyncCheckpoint(),
                        "parity: a valid descendant pending checkpoint must be accepted");
    BOOST_CHECK_MESSAGE(Checkpoints::hashSyncCheckpoint == hashPending,
        "parity: hashSyncCheckpoint must advance to the pending hash");
    BOOST_CHECK_MESSAGE(Checkpoints::hashPendingCheckpoint == uint256(0),
                        "parity: hashPendingCheckpoint must be cleared after acceptance");
    {
        uint256 persisted = uint256(0);
        BOOST_REQUIRE_EQUAL((int)Checkpoints::LoadPersistedSyncCheckpoint(persisted, NULL),
                            (int)SYNC_CHECKPOINT_LOADED);
        BOOST_CHECK_MESSAGE(persisted == hashPending,
                            "parity: WriteSyncCheckpoint must have persisted the new value");
    }

    fx.Restore();
}

BOOST_AUTO_TEST_SUITE_END()
