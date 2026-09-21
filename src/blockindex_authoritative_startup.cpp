// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.

#include "blockindex_authoritative_startup.h"

#include "blockindex_authoritative_live.h"
#include "blockindex_residency_counters.h"
#include "blockindex_startup_bootstrap.h"
#include "blockindex_stake_seen_builder.h"
#include "blockindex_candidate_startup_builder.h"
#include "blockindex_shadow_startup.h"   // RetainBlockIndexAuthoritativeNavigator
#include "dag.h"                         // g_dagManager (attribution count)
#include "blockindex_v2_reader.h"
#include "candidate_frontier.h"
#include "main.h"
#include "txdb.h"
#include "dag_tip_overlay_runtime.h"
#include "dag_tips_delta.h"
#include "dag_tip_frontier_metadata.h"
#include "dag_tip_frontier.h"
#include "fixed_blockindex_store.h"
#include "finality.h"

#include <memory>
#include <string>

extern bool g_dagSourceUnhealthy;

namespace {

// Test-only registration-boundary hook (NULL in production = no-op). Declared
// here so InitBlockIndexAuthoritative (which consults it) compiles; only a test
// installs it via SetDagObserverBoundaryHookForTest. See the header comment.
static DagObserverBoundaryHook g_testDagObserverBoundaryHook = NULL;

// Process-lifetime owner of the authoritative startup context. The bootstrap's
// HotOwner owns the best-tip/genesis CBlockIndex objects that pindexBest /
// pindexGenesisBlock point to; it MUST outlive every consumer touching those
// anchors, so it is retained for the daemon lifetime (not a stack object).
struct AuthoritativeStartupContext
{
    BlockIndexStartupBootstrap bootstrap;
    std::string v2Root;
    bool ok;

    // G1: production live-authority seam retained process-lifetime so the live
    // block path can resolve parents by value + persist post-S blocks with
    // bounded residency. Bound to the single process-open base reader (via the
    // navigator's cold reader) + the mutable tip under <v2Root>/blockindex_tip.
    std::unique_ptr<BlockIndexAuthoritativeLive> live;
    // Declared after bootstrap/live: destruction is reverse order, so runtime
    // closes first while its startup dependencies are still retained.
    std::unique_ptr<dag_tip_frontier::DagTipOverlayRuntime> dagTipRuntime;
    // The delta seam has exactly one process-global observer slot. This context
    // owns its registration and must clear it before destroying the runtime.
    bool dagTipObserverRegistered;

    AuthoritativeStartupContext() : ok(false), dagTipObserverRegistered(false) {}
};

// Single process-lifetime instance (constructed in authoritative mode).
AuthoritativeStartupContext* g_authoritativeContext = NULL;

static bool ReadAuthoritativeDagSourceState(uint256* out, void*)
{
    if (!out) return false;
    CTxDB db("r");
    const bool ok = db.ReadDAGSourceStateId(*out);
    db.Close(); // recovery subsequently opens daglinks directly; never hold txleveldb LOCK.
    return ok;
}
static bool HealthyAuthoritativeDagSource(void*)
{
    return !g_dagSourceUnhealthy;
}

// The d1 seam invokes this synchronously only after its source mutation has
// committed. It performs derived-overlay I/O only; source checkpoint validation
// remains inside the runtime-owned consumer at END.
static void DeliverCommittedDagTipDeltaToRuntime(
    const DagTipCommittedDeltaEvent& event, void* context)
{
    dag_tip_frontier::DagTipOverlayRuntime* runtime =
        static_cast<dag_tip_frontier::DagTipOverlayRuntime*>(context);
    if (!runtime) return;
    std::string ignored;
    runtime->ConsumeCommittedDelta(event, &ignored);
}

} // namespace

// Set once authoritative mode is selected (guards init.cpp continuation).
bool g_fAuthoritativeStartup = false;

// Publish startup globals from the bootstrap anchors + by-value authority.
bool PublishStartupGlobals(AuthoritativeStartupContext& ctx, std::string* error)
{
    CBlockIndex* best = ctx.bootstrap.BestTipObject();
    CBlockIndex* genesis = ctx.bootstrap.GenesisObject();
    if (!best || !genesis)
    {
        if (error) *error = "authoritative startup: best/genesis anchor absent";
        return false;
    }

    // pindexBest / pindexGenesisBlock -> bootstrap-owned permanent anchors.
    pindexBest = best;
    pindexGenesisBlock = genesis;
    nBestHeight = best->nHeight;
    hashBestChain = best->GetBlockHash();
    nBestChainTrust = best->nChainTrust;
    // nBestInvalidTrust is a diagnostic scalar; load from DB (default 0).
    {
        CBigNum bn = 0;
        CTxDB txdb("r");
        if (txdb.ReadBestInvalidTrust(bn))
            nBestInvalidTrust = bn.getuint256();
        else
            nBestInvalidTrust = uint256(0);
    }

    if (error) error->clear();
    return true;
}

bool InitBlockIndexAuthoritative(const std::string& v2Root, std::string* error)
{
    if (g_authoritativeContext)
    {
        if (error) *error = "authoritative startup: already initialized";
        return false;
    }

    std::unique_ptr<AuthoritativeStartupContext> ctx(new AuthoritativeStartupContext());
    ctx->v2Root = v2Root;

    // RAII restore guard: on FAILURE reset every startup global that
    // PublishStartupGlobals can publish back to the authoritative-empty state
    // (no dangling/non-null authoritative anchors after a failed owner
    // publication). Authoritative startup is all-or-nothing (init.cpp:1483-1490
    // bypasses legacy LoadBlockIndex; failure is fatal with NO legacy fallback),
    // so the empty/reset state is the correct failed-startup contract and matches
    // ResetBlockIndexAuthoritativeStartupForTest. Declared AFTER ctx so its
    // destructor runs BEFORE ctx's: the globals are reset away from the
    // ctx-owned (about-to-be-freed) anchors first. On success, Disarm() is
    // called just before the context is released.
    struct AuthoritativeStartupGlobalsGuard
    {
        bool disarmed;
        AuthoritativeStartupGlobalsGuard() : disarmed(false) {}
        ~AuthoritativeStartupGlobalsGuard()
        {
            if (disarmed) return;
            pindexBest = NULL;
            pindexGenesisBlock = NULL;
            nBestHeight = -1;
            hashBestChain = uint256(0);
            nBestChainTrust = uint256(0);
            nBestInvalidTrust = uint256(0);
        }
        void Disarm() { disarmed = true; }
    } globalsGuard;

    // 1. Bootstrap (open + validate authoritative generation, pin best-tip +
    //    genesis permanent anchors; fail-closed).
    BlockIndexV2ReaderOptions opts;
    {
        const BlockIndexStartupStatus st = ctx->bootstrap.Open(v2Root, opts, error);
        if (st != BLOCK_INDEX_STARTUP_OK)
            return false; // fail closed; error already set
    }

    // 2. Publish startup globals from the anchors + by-value authority.
    if (!PublishStartupGlobals(*ctx, error))
        return false;

    // 3. setStakeSeen via A.10.1o builder (fail-closed).
    if (error) error->clear();
    {
        const BlockIndexV2Reader* rd = ctx->bootstrap.ReaderPtr();
        std::set<std::pair<COutPoint, unsigned int> > ss;
        BlockIndexStakeSeenBuilder sb;
        std::string serr;
        if (!sb.Build(*rd, &ss, &serr))
        {
            if (error) *error = "authoritative startup: setStakeSeen: " + serr;
            return false;
        }
        setStakeSeen = ss; // A.10.1o builder reproduces legacy exactly
    }

    // 4. (moved after candidate build — see below) Build the candidate frontier
    //    by value (A.10.1m) from the authoritative reader + derived store, and
    //    populate the legacy mapCandidateTips so candidate selection
    //    (EvaluateCandidateFrontierByValue) uses the SAME by-value result.
    //    (RebuildCandidateTips is skipped in authoritative mode.)
    if (error) error->clear();
    {
        SnapshotCandidateFrontierStore store;
        BlockIndexCandidateStartupBuilder cb;
        std::string cberr;
        if (!cb.Build(*ctx->bootstrap.ReaderPtr(),
                      *ctx->bootstrap.DerivedStorePtr(),
                      GetForkHeightDAG(), &store, &cberr))
        {
            if (error) *error = "authoritative startup: candidate: " + cberr;
            return false;
        }
        // Populate the legacy tip map from the by-value store (authoritative
        // membership; frontier evaluator reads this same logical content).
        mapCandidateTips.clear();
        for (size_t i = 0; i < store.tipHashes.size(); ++i)
        {
            const uint256& h = store.tipHashes[i];
            CandidateFrontierAuthorityRecord rec = store.Lookup(h);
            if (!rec.found) continue;
            mapCandidateTips[h] = CandidateTipRecord(
                h, rec.chainTrust, uint256(0), rec.height,
                store.HasBlockData(h) ? 1 : 0,
                store.IsOperatorHash(h) ? 0 : 1,
                nCandidateTipGeneration);
        }
        nCandidateTipGeneration++;
        printf("BLOCKINDEX_V2_AUTHORITATIVE candidate_tips=%zu generation=%llu\n",
               mapCandidateTips.size(),
               (unsigned long long)nCandidateTipGeneration);
    }

    // 5. Capture immutable selected-generation metadata BEFORE moving the single
    // open reader to the navigator. Never reread CURRENT after this point.
    dag_tip_frontier::DagTipOverlayRuntimeConfig dagRuntimeConfig;
    DagTipFrontierCapability dagFrontierCapability = DAG_TIP_FRONTIER_CAPABILITY_LEGACY_UNAVAILABLE;
    {
        const BlockIndexV2Reader* selected = ctx->bootstrap.ReaderPtr();
        if (!selected || !selected->IsOpen())
        {
            if (error) *error = "authoritative startup: selected reader unavailable for DAG runtime";
            return false;
        }
        const FixedBlockIndexManifest& manifest = selected->Manifest();
        dagRuntimeConfig.generation = selected->Generation();
        dagRuntimeConfig.artifactPath = selected->GenerationPath() + "/" + BLOCK_INDEX_DAG_TIP_FRONTIER_FILE_NAME;
        dagRuntimeConfig.overlayDbDir = selected->GenerationPath() + "/dag-tip-overlay";
        dagRuntimeConfig.dagLinksDir = (GetDataDir() / "txleveldb").string();
        dagRuntimeConfig.cacheCapacity = 256;
        memcpy(dagRuntimeConfig.dagInputDigest, manifest.dagInputDigest, 32);
        std::string ferr;
        dagFrontierCapability = QueryDagTipFrontierCapability(selected->GenerationPath(), dagRuntimeConfig.generation,
            manifest.capability, dagRuntimeConfig.dagInputDigest, &ferr);
        if (dagFrontierCapability == DAG_TIP_FRONTIER_CAPABILITY_CORRUPT ||
            dagFrontierCapability == DAG_TIP_FRONTIER_CAPABILITY_AUTHORITY_FAILURE)
        {
            if (error) *error = "authoritative startup: DAG frontier immutable authority: " + ferr;
            return false;
        }
    }

    // 5. Install the authoritative by-value staking navigator (A.10.1p) so
    //    wallet-depth never falls back to LegacyBlockIndexAccessor. Reuses the
    //    bootstrap's SINGLE already-open generation reader (moved out here) so
    //    no second hashindex/active/store LevelDB handle is opened (A.10.1q
    //    Stage1 double-open fix). After this the bootstrap authority no longer
    //    owns the reader; the navigator retains it for the process lifetime,
    //    and the bootstrap context (anchors) stays alive in g_authoritativeContext.
    {
        BlockIndexV2Reader genReader = ctx->bootstrap.ExtractReader();
        if (!genReader.IsOpen())
        {
            if (error) *error = "authoritative startup: bootstrap reader unavailable";
            return false;
        }
        std::string nerr;
        if (!RetainBlockIndexAuthoritativeNavigatorWithReader(std::move(genReader), &nerr))
        {
            if (error) *error = "authoritative startup: navigator: " + nerr;
            return false;
        }
    }

    // 5b. G1: retain the production live-authority seam bound to the SAME single
    //     process-open base reader (the navigator's cold reader, which survives
    //     process-lifetime) + the mutable tip under <v2Root>/blockindex_tip.
    //     This is what lets the live block path resolve a parent by value and
    //     persist post-S blocks without rebuilding historical mapBlockIndex.
    {
        const ColdHotSeamNavigator* nav = GetBlockIndexStakingNavigator();
        const BlockIndexV2Reader* cold = nav ? nav->GetColdReader() : NULL;
        if (!cold || !cold->IsOpen())
        {
            if (error) *error = "authoritative startup: live-authority base reader unavailable";
            return false;
        }
        const int livetail = GetArg("-blockindexlivetail", 2048);
        std::unique_ptr<BlockIndexAuthoritativeLive> live(new BlockIndexAuthoritativeLive());
        std::string lerr;
        if (!live->Open(v2Root, cold, livetail, &lerr))
        {
            if (error) *error = "authoritative startup: live authority: " + lerr;
            return false; // fail closed
        }
        ctx->live = std::move(live);
        printf("BLOCKINDEX_V2_AUTHORITATIVE live_authority=retained horizon=%d base_gen=%llu tip_height=%d\n",
               livetail,
               (unsigned long long)ctx->live->BaseGeneration(),
               (int)ctx->live->TipAuthorityMutable()->TipHeight());
    }

    // 5c. R2c.1d2: authoritative startup bypasses legacy LoadBlockIndex(), so
    // establish the zero-delta source-state identity here before this context
    // can expose a live DAG writer. This never claims overlay health.
    {
        CTxDB txdb;
        std::string derr;
        if (!txdb.BootstrapDAGSourceStateId(&derr))
        {
            if (error) *error = "authoritative startup: DAG source-state bootstrap: " + derr;
            return false;
        }
        if (!txdb.EnsureDAGChildCountIndex(&derr))
        {
            if (error) *error = "authoritative startup: DAG child-count index: " + derr;
            return false;
        }
    }

    // 5d. A PRESENT_VALID selected immutable frontier gets exactly one derived
    // runtime owner. Legacy-unavailable generations deliberately get no owner.
    if (dagFrontierCapability == DAG_TIP_FRONTIER_CAPABILITY_PRESENT_VALID)
    {
        dagRuntimeConfig.sourceReader = &ReadAuthoritativeDagSourceState;
        dagRuntimeConfig.sourceHealthy = &HealthyAuthoritativeDagSource;
        std::unique_ptr<dag_tip_frontier::DagTipOverlayRuntime> runtime(new dag_tip_frontier::DagTipOverlayRuntime());
        std::string rerr;
        if (!runtime->Start(dagRuntimeConfig, &rerr))
        {
            if (error) *error = "authoritative startup: DAG tip overlay runtime: " + rerr;
            return false;
        }
        ctx->dagTipRuntime = std::move(runtime);
        // Re-check source certification at the registration boundary, not only
        // at startup repair. Revoked/missing/stale authority cannot register.
        // Test-only: a hook may make the authority transition unhealthy at this
        // exact boundary so the re-check's refusal is proven (NULL = no-op).
        if (g_testDagObserverBoundaryHook) g_testDagObserverBoundaryHook();
        {
            CTxDB source("r");
            std::string healthError;
            const bool healthy = source.IsDAGChildCountIndexHealthy(&healthError);
            source.Close();
            if (!healthy) {
                if (error) *error = "authoritative startup: observer source unhealthy: " + healthError;
                return false;
            }
        }
        // Registration comes only after Start established a healthy runtime.
        // Context teardown clears this global slot before runtime destruction.
        SetDagTipCommittedDeltaObserver(&DeliverCommittedDagTipDeltaToRuntime,
                                         ctx->dagTipRuntime.get());
        ctx->dagTipObserverRegistered = true;
    }

    // HReg + wallet rescan are driven by init.cpp AFTER this returns, using
    // the by-value active-chain reader + by-value paths (ca7c7e1).

    // Persist the context for the process lifetime (anchors must survive).
    globalsGuard.Disarm();
    g_authoritativeContext = ctx.release();
    ::g_fAuthoritativeStartup = true;
    if (error) error->clear();
    printf("BLOCKINDEX_V2_AUTHORITATIVE startup=bootstrap best_height=%d best_chaintrust_hex=%s\n",
           nBestHeight, nBestChainTrust.GetHex().c_str());
    return true;
}

bool AuthoritativeGetActiveSnapshotByHeight(int height, BlockIndexSnapshot* out)
{
    if (!out)
        return false;
    const BlockIndexV2Reader* reader = GetAuthoritativeNavigatorReader();
    if (!reader)
    {
        const ColdHotSeamNavigator* nav = GetBlockIndexStakingNavigator();
        reader = nav ? nav->GetColdReader() : NULL;
    }
    if (!reader || !reader->IsOpen())
        return false;
    std::string error;
    return reader->GetActiveByHeight(height, out, &error) == BLOCK_INDEX_V2_READ_FOUND;
}

AuthoritativeBlockResolutionResult ResolveAuthoritativeBlockSnapshotR(
    const uint256& hash, BlockIndexSnapshot* out, std::string* error)
{
    if (error) error->clear();
    if (!out)
        return AUTHORITATIVE_BLOCK_AUTHORITY_FAILURE;
    const ColdHotSeamNavigator* nav = GetBlockIndexStakingNavigator();
    if (!nav)
    {
        if (error) *error = "authoritative block resolver: navigator unavailable";
        return AUTHORITATIVE_BLOCK_AUTHORITY_FAILURE;
    }
    ColdHotSeamSnapshot snap;
    std::string err;
    const ColdHotSeamResult r = nav->ResolveLogicalR(
        BlockIndexLogicalId(hash), &snap, &err);
    if (r == COLD_HOT_SEAM_NOT_FOUND)
    {
        // Genuine immutable-generation miss. A post-generation retained block
        // (present only in the CURRENT mutable retained tail, absent from the
        // selected immutable generation) must still be resolvable by value.
        // Consult the production live authority (BlockIndexAuthoritativeLive =
        // current mutable retained tail; tip-then-base composite) as the
        // post-generation source. This closes the Astra freeze Blocker R-1: the
        // "hot" resolver previously read only the same immutable generation, so
        // a hash created after generation could never resolve. Immutable
        // historical generation remains the FIRST source; the current tail is
        // consulted only on a genuine immutable NOT_FOUND (currentness-relevant
        // for retained blocks, no competing authority). No mapBlockIndex / no
        // legacy resident fallback.
        BlockIndexAuthoritativeLive* live = GetAuthoritativeLiveAuthority();
        if (live && live->IsOpen())
        {
            BlockIndexSnapshot tailSnap;
            std::string terr;
            const BlockIndexHotStatus st = live->ResolveBlockSnapshot(hash, &tailSnap, &terr);
            if (st == BlockIndexHotStatus::OK)
            {
                *out = tailSnap;
                if (!tailSnap.fInMainChain)
                {
                    if (error) *error = "authoritative block resolver: block is not active";
                    return AUTHORITATIVE_BLOCK_NOT_ACTIVE;
                }
                if (error) error->clear();
                return AUTHORITATIVE_BLOCK_FOUND;
            }
            if (st == BlockIndexHotStatus::CORRUPT_METADATA ||
                st == BlockIndexHotStatus::MATERIALIZATION_UNAVAILABLE)
            {
                if (error) *error = terr.empty()
                    ? "authoritative block resolver: current-tail authority failure"
                    : terr;
                return AUTHORITATIVE_BLOCK_AUTHORITY_FAILURE;
            }
            // else AUTHORITY_MISSING / NOT_RESIDENT: genuinely absent -> NOT_FOUND.
        }
        if (error) *error = err.empty() ? "authoritative block resolver: block not found" : err;
        return AUTHORITATIVE_BLOCK_NOT_FOUND;
    }
    if (r == COLD_HOT_SEAM_NOT_ACTIVE)
    {
        if (error) *error = err.empty() ? "authoritative block resolver: block is not active" : err;
        return AUTHORITATIVE_BLOCK_NOT_ACTIVE;
    }
    if (r != COLD_HOT_SEAM_OK || !snap.snapshot.found)
    {
        if (error) *error = err.empty() ? "authoritative block resolver: authority failure" : err;
        return AUTHORITATIVE_BLOCK_AUTHORITY_FAILURE;
    }
    if (!snap.snapshot.fInMainChain)
    {
        const BlockIndexV2Reader* cold = nav->GetColdReader();
        if (!cold)
        {
            if (error) *error = "authoritative block resolver: active reader unavailable";
            return AUTHORITATIVE_BLOCK_AUTHORITY_FAILURE;
        }
        BlockIndexSnapshot active;
        std::string activeError;
        const BlockIndexV2ReadStatus activeStatus =
            cold->GetActiveByHeight(snap.snapshot.height, &active, &activeError);
        if (activeStatus == BLOCK_INDEX_V2_READ_CORRUPT ||
            activeStatus == BLOCK_INDEX_V2_READ_IO_ERROR ||
            activeStatus == BLOCK_INDEX_V2_READ_NOT_OPEN)
        {
            if (error) *error = activeError.empty()
                ? "authoritative block resolver: active membership authority failure"
                : activeError;
            return AUTHORITATIVE_BLOCK_AUTHORITY_FAILURE;
        }
        if (activeStatus == BLOCK_INDEX_V2_READ_FOUND &&
            active.hash == snap.snapshot.hash)
        {
            snap.snapshot = active;
        }
        else
        {
            if (error) *error = "authoritative block resolver: block is not active";
            *out = snap.snapshot;
            return AUTHORITATIVE_BLOCK_NOT_ACTIVE;
        }
    }
    *out = snap.snapshot;
    return AUTHORITATIVE_BLOCK_FOUND;
}

bool ResolveAuthoritativeBlockSnapshot(const uint256& hash,
                                       BlockIndexSnapshot* out,
                                       std::string* error)
{
    const AuthoritativeBlockResolutionResult result =
        ResolveAuthoritativeBlockSnapshotR(hash, out, error);
    return result == AUTHORITATIVE_BLOCK_FOUND ||
           result == AUTHORITATIVE_BLOCK_NOT_ACTIVE;
}

bool ResolveAuthoritativeActiveBlock(const uint256& hash,
                                     BlockIndexSnapshot* out,
                                     std::string* error)
{
    return ResolveAuthoritativeBlockSnapshotR(hash, out, error) ==
           AUTHORITATIVE_BLOCK_FOUND;
}

// A.10.1q: expose the retained authoritative generation root + generation for
// building a BlockIndexActiveChainReader against the SAME selected generation.
std::string AuthoritativeRootPath()
{
    if (!g_authoritativeContext) return std::string();
    return g_authoritativeContext->v2Root;
}

uint64_t AuthoritativeGeneration()
{
    if (!g_authoritativeContext) return 0;
    return g_authoritativeContext->bootstrap.Generation();
}

// G1: production live-authority accessor (NULL when not authoritative mode).
// Prefers the G1 test-only handle while set so a causal test can arm the real
// ProcessBlock path against an isolated authoritative generation.
static BlockIndexAuthoritativeLive* g_testLiveAuthority = NULL;
BlockIndexAuthoritativeLive* GetAuthoritativeLiveAuthority()
{
    if (g_testLiveAuthority)
        return g_testLiveAuthority;
    if (!g_authoritativeContext) return NULL;
    return g_authoritativeContext->live.get();
}

// G1 test-only arms (see header). Pair set/clear; inert when unset.
void SetAuthoritativeLiveForTesting(BlockIndexAuthoritativeLive* live)
{
    g_testLiveAuthority = live;
}
void ClearAuthoritativeLiveForTesting()
{
    g_testLiveAuthority = NULL;
}

// Test-only registration-boundary hook setter. The static is declared at the
// top of this namespace so InitBlockIndexAuthoritative can consult it.
void SetDagObserverBoundaryHookForTest(DagObserverBoundaryHook hook)
{
    g_testDagObserverBoundaryHook = hook;
}

// A.10.1q / Stage1: emit residency for the retained authoritative context,
// reading the bootstrap HotOwner live metrics.
void ResetBlockIndexAuthoritativeStartupForTest()
{
    // Test-only: quiesce the global observer before normal context destruction;
    // it holds a raw pointer to the runtime-owned consumer.
    if (g_authoritativeContext && g_authoritativeContext->dagTipObserverRegistered)
    {
        SetDagTipCommittedDeltaObserver(NULL, NULL);
        g_authoritativeContext->dagTipObserverRegistered = false;
    }
    // Then destroy the context, navigator, and only the globals published by
    // InitBlockIndexAuthoritative.
    delete g_authoritativeContext;
    g_authoritativeContext = NULL;
    ClearBlockIndexStakingNavigator();
    g_fAuthoritativeStartup = false;
    pindexBest = NULL;
    pindexGenesisBlock = NULL;
    nBestHeight = -1;
    hashBestChain = uint256(0);
    nBestChainTrust = uint256(0);
    nBestInvalidTrust = uint256(0);
}

dag_tip_frontier::DagTipOverlayRuntime* GetDagTipOverlayRuntimeForTest()
{
    return g_authoritativeContext ? g_authoritativeContext->dagTipRuntime.get() : NULL;
}

bool HasDagTipOverlayRuntimeForTest()
{
    return g_authoritativeContext && g_authoritativeContext->dagTipRuntime.get() != NULL;
}

uint64_t DagTipOverlayRuntimeGenerationForTest()
{
    if (!HasDagTipOverlayRuntimeForTest()) return 0;
    return g_authoritativeContext->dagTipRuntime->Overlay()
        ? g_authoritativeContext->dagTipRuntime->Overlay()->Generation() : 0;
}

void PrintAuthoritativeResidency(const char* tag)
{
    int64_t hc = 0, hp = 0, pc = 0, pp = 0;
    if (g_authoritativeContext)
    {
        const BlockIndexHotMetrics m = g_authoritativeContext->bootstrap.Owner().Metrics();
        hc = m.residentCount; hp = m.peakResidentCount;
        pc = m.pinnedCount;   pp = m.pinnedCount; // pin peak == current pinned count at T (stable)
    }
    PrintBlockIndexResidency("BY_VALUE_AUTHORITATIVE",
                            (int64_t)AuthoritativeGeneration(), tag, hc, hp, pc, pp);
    printf("BLOCKINDEX_ATTRIBUTION %s stake_seen=%llu candidate_tips=%llu dag_entries=%d\n",
           tag, (unsigned long long)setStakeSeen.size(),
           (unsigned long long)mapCandidateTips.size(),
           (int)g_dagManager.GetDAGEntryCount());
    fflush(stdout);
}

// R2c.2s/S2: authoritative by-value trust provider. Reproduces EXACT legacy
// CBlockIndex::GetBlockTrust semantics (main.cpp:9812-9827).
//   1. target = SetCompact(nBits); if target <= 0 -> 0
//   2. if nHeight >= FORK_HEIGHT_DAG && IsProofOfStake() -> 0
//   3. if nHeight >= FORK_HEIGHT_POEM
//        -> GetBlockEntropy( (IsProofOfStake() && nHeight < FORK_HEIGHT_DAG)
//                              ? hashProof : blockHash )
//   4. else -> reciprocal ( (1<<256) / (target+1) )
//
// S2 isolated retained recolor. Legacy coloring runs on a local manager and
// explicit local index only; no save/swap/restore of process-global state.
bool AuthoritativeDAGRecolorSource::Block(const uint256& hash,
    BlockIndexSnapshot* out, std::string* error) const
{
    if (!out) { if (error) *error="recolor: null block output"; return false; }
    if (error) error->clear();
    // Mutation-scoped pending overlay FIRST: during a live Reorganize the winning
    // post-generation block is part of THIS logical mutation (its by-value
    // metadata already validated) but is not yet published to the external live
    // authority, so the authoritative resolver alone cannot see it. Consult the
    // pending by-value snapshot first (pending mutation-owned snapshot > current
    // certified live retained tail > immutable generation), then fall through to
    // the global authoritative resolver. No borrowed pointer; the snapshot is a
    // by-value copy owned by the enclosing mutation.
    if (!pendingSnapshots_.empty())
    {
        std::map<uint256,BlockIndexSnapshot>::const_iterator pit = pendingSnapshots_.find(hash);
        if (pit != pendingSnapshots_.end())
        {
            if (!pit->second.found || pit->second.hash != hash)
            { if (error) *error="recolor: pending snapshot identity mismatch: "+hash.GetHex(); return false; }
            *out = pit->second;
            return true;
        }
    }
    const AuthoritativeBlockResolutionResult r = ResolveAuthoritativeBlockSnapshotR(hash,out,error);
    if ((r==AUTHORITATIVE_BLOCK_FOUND || r==AUTHORITATIVE_BLOCK_NOT_ACTIVE) &&
        out->found && out->hash==hash) return true;
    // A non-active cold record can be unavailable through the active navigator.
    // Resolve metadata by hash, never substitute active-at-same-height metadata.
    if (r!=AUTHORITATIVE_BLOCK_AUTHORITY_FAILURE) {
        const BlockIndexV2Reader* reader=GetAuthoritativeNavigatorReader();
        if (reader && reader->LookupByHash(hash,out,error)==BLOCK_INDEX_V2_READ_FOUND &&
            out->found && out->hash==hash) return true;
    }
    if(error) *error="recolor: required block metadata unavailable: "+hash.GetHex();
    return false;
}
bool AuthoritativeDAGRecolorSource::Parents(const uint256& hash,
    std::vector<uint256>* out, std::string* error) const
{
    CBlockDAGData data;
    if (!db.ReadDAGLinks(hash,data)) {
        if(error) *error="recolor: required daglinks unreadable: "+hash.GetHex();
        return false;
    }
    *out=data.vDAGParents;
    return true;
}
bool AuthoritativeDAGRecolorSource::PreDAGTrust(const uint256& hash,
    uint256* out, std::string* error) const
{
    return GetAuthoritativeAccumulatedChainTrust(hash,out,error);
}

// ---------------------------------------------------------------------------
// Option-R: exact reconstruction of a referenced non-retained DAG-era parent's
// former DAG-overwritten scalar.
//
// A retained child may reference a DAG-era parent whose daglinks record was
// erased (reorg/prune). Legacy live coloring consumed mapBlockIndex[parent]->
// nChainTrust (the former DAG score); after restart the linear replay leaves it
// at a lower value -> the legacy L3 restart-consistency defect. Option-R
// reconstructs the parent's score deterministically from canonical persisted
// inputs (raw-block coinbase parents + current daglinks topology + pre-DAG
// accumulated trust), so the isolated recolor is independent of incidental
// live/restart residue.
//
// Semantics: build a bounded closure of the target's ancestors (retained DAG
// via source.Parents, erased DAG via raw-block coinbase, pre-DAG via trust),
// color the whole closure in height order with the SAME ColorBlock /
// ColorBlockDAGKnight on a stack-local CDAGManager canvas, and read the
// target's nDAGScore. No global mapBlockIndex / g_dagManager mutation; no
// borrowed pointers escape. Fail closed on any missing/malformed raw metadata.
// ---------------------------------------------------------------------------
namespace {
struct BoundaryClosureBuilder
{
    const CanonicalDAGRecolorSource& source;
    std::map<uint256,int32_t> heightOf;          // hash -> height (discovered)
    std::map<uint256,std::vector<uint256>> parentsOf; // hash -> resolved parents
    std::map<uint256,uint256> preDAGTrust;       // pre-DAG boundary trust
    std::vector<std::pair<int32_t,uint256>> closure; // height-sorted closure
    std::string err;
    bool ok;

    explicit BoundaryClosureBuilder(const CanonicalDAGRecolorSource& src)
        : source(src), ok(true) {}

    bool Resolve(const uint256& hash, int32_t depth)
    {
        if (!ok) return false;
        if (heightOf.count(hash)) return true; // already resolved / memoized
        if (depth > static_cast<int32_t>(DAG_PRUNE_DEPTH))
        { err="option-r: closure exceeds work budget"; return ok=false; }
        BlockIndexSnapshot snap;
        if (!source.Block(hash,&snap,&err)) return ok=false;
        if (!snap.found || snap.hash!=hash || snap.height<0)
        { err="option-r: inconsistent boundary metadata: "+hash.GetHex(); return ok=false; }
        heightOf[hash]=snap.height;
        if (snap.height < GetForkHeightDAG())
        {
            uint256 trust;
            if (!source.PreDAGTrust(hash,&trust,&err)) return ok=false;
            preDAGTrust[hash]=trust;
            closure.push_back(std::make_pair(snap.height,hash));
            return true;
        }
        if (snap.fProofOfStake)
        { err="option-r: post-DAG PoS boundary not representable: "+hash.GetHex(); return ok=false; }
        // Parents: prefer canonical daglinks if retained, else raw coinbase.
        std::vector<uint256> parents;
        bool retained=false;
        if (source.Parents(hash,&parents,&err))
        {
            retained=true; // retained DAG parent: parents come from canonical daglinks
        }
        else
        {
            // erased: recover from raw block coinbase
            err.clear();
            CBlock raw;
            if (!raw.ReadFromDisk(snap.nFile, snap.nBlockPos, true))
            { err="option-r: raw block unavailable: "+hash.GetHex(); return ok=false; }
            if (raw.GetHash()!=hash)
            { err="option-r: raw block hash mismatch: "+hash.GetHex(); return ok=false; }
            if (raw.hashPrevBlock!=snap.hashPrev)
            { err="option-r: raw block hashPrev mismatch: "+hash.GetHex(); return ok=false; }
            parents.clear();
            for (const auto& out : raw.vtx[0].vout)
            {
                std::vector<uint256> extracted = ExtractDAGParents(out.scriptPubKey);
                if (extracted.empty()) continue;
                if (parents.empty()) parents = extracted;
                else parents.insert(parents.end(), extracted.begin(), extracted.end());
            }
            if (parents.empty())
            { err="option-r: malformed/missing DAG coinbase for: "+hash.GetHex(); return ok=false; }
            std::set<uint256> seen;
            for (const auto& p : parents)
                if (p==uint256(0) || !seen.insert(p).second)
                { err="option-r: invalid/duplicate parent for: "+hash.GetHex(); return ok=false; }
        }
        if (parents.size()>MAX_DAG_PARENTS)
        { err="option-r: excessive parent count: "+hash.GetHex(); return ok=false; }
        parentsOf[hash]=parents;
        closure.push_back(std::make_pair(snap.height,hash));
        for (const auto& p : parents)
            if (!Resolve(p, depth+1)) return false;
        return true;
    }
};
} // namespace

bool AuthoritativeDAGRecolorSource::ReconstructBoundaryScore(
    const uint256& hash, BoundaryScoreResult* out, std::string* error) const
{
    if (!out) { if(error) *error="option-r: null output"; return false; }
    *out=BoundaryScoreResult();
    if (error) error->clear();
    try {
        BoundaryClosureBuilder builder(*this);
        if (!builder.Resolve(hash,0)) { if(error) *error=builder.err; return false; }
        std::sort(builder.closure.begin(), builder.closure.end());
        // Detect ancestor-height inversion / cycles.
        for (const auto& entry : builder.closure) {
            auto pit=builder.parentsOf.find(entry.second);
            if (pit==builder.parentsOf.end()) continue;
            for (const auto& p : pit->second)
                if (builder.heightOf.count(p) && builder.heightOf.at(p)>=entry.first)
                { if(error) *error="option-r: non-ancestral/cycle in boundary closure"; return false; }
        }
        // Materialize by-value objects for the whole closure on a stack-local canvas.
        std::map<uint256,std::unique_ptr<CBlockIndex>> owned;
        std::map<uint256,CBlockIndex*> localIndex;
        std::map<uint256,CBlockDAGData> records;
        for (const auto& entry : builder.closure) {
            const uint256& h=entry.second;
            BlockIndexSnapshot snap;
            if (!Block(h,&snap,error)) return false;
            std::unique_ptr<CBlockIndex> pi(new CBlockIndex());
            pi->phashBlock=&h;
            pi->nHeight=snap.height; pi->nBits=snap.nBits; pi->nTime=snap.nTime;
            pi->nVersion=snap.nVersion; pi->nFlags=snap.nFlags; pi->hashProof=snap.hashProof;
            if(snap.fProofOfStake) pi->nFlags |= BLOCK_PROOF_OF_STAKE;
            if(snap.height<GetForkHeightDAG()) {
                if(!builder.preDAGTrust.count(h)) { if(error) *error="option-r: missing pre-DAG trust"; return false; }
                pi->nChainTrust=builder.preDAGTrust.at(h);
            } else if(builder.parentsOf.count(h)) {
                CBlockDAGData rec;
                rec.vDAGParents=builder.parentsOf.at(h);
                rec.fBlue=false;
                records[h]=rec;
            }
            localIndex[h]=pi.get();
            owned.emplace(h,std::move(pi));
        }
        CDAGManager canvas(localIndex);
        canvas.LoadRecolorCanvas(records);
        for (const auto& entry : builder.closure) {
            CBlockIndex* pi=localIndex.at(entry.second);
            if(pi->nHeight>=GetForkHeightDAGKnight()) canvas.ColorBlockDAGKnight(pi);
            else canvas.ColorBlock(pi);
        }
        CBlockDAGData target;
        if(!canvas.GetDAGData(hash,target)) { if(error) *error="option-r: incomplete reconstruction"; return false; }
        out->hash=hash;
        out->height=builder.heightOf.at(hash);
        out->score=target.nDAGScore;
        out->sourceGeneration=AuthoritativeGeneration();
        out->valid=true;
        return true;
    } catch(const std::exception& ex) {
        if(error) *error=std::string("option-r: ")+ex.what();
        return false;
    }
}

bool AuthoritativeDAGRecolorSource::ReconstructBoundaryClosure(
    const uint256& hash,
    std::map<uint256,BlockIndexSnapshot>* metadata,
    std::map<uint256,uint256>* preDAGTrust,
    std::map<uint256,CBlockDAGData>* records,
    std::vector<std::pair<int32_t,uint256>>* ordered,
    std::string* error) const
{
    if (!metadata || !preDAGTrust || !records || !ordered) { if(error) *error="option-r: null closure output"; return false; }
    metadata->clear(); preDAGTrust->clear(); records->clear(); ordered->clear();
    if (error) error->clear();
    try {
        BoundaryClosureBuilder builder(*this);
        if (!builder.Resolve(hash,0)) { if(error) *error=builder.err; return false; }
        std::sort(builder.closure.begin(), builder.closure.end());
        // Detect ancestor-height inversion / cycles.
        for (const auto& entry : builder.closure) {
            auto pit=builder.parentsOf.find(entry.second);
            if (pit==builder.parentsOf.end()) continue;
            for (const auto& p : pit->second)
                if (builder.heightOf.count(p) && builder.heightOf.at(p)>=entry.first)
                { if(error) *error="option-r: non-ancestral/cycle in boundary closure"; return false; }
        }
        // Emit by-value metadata + pre-DAG trust for every closure vertex, and a
        // REAL CBlockDAGData record (recovered vDAGParents) for every DAG-era one.
        for (const auto& entry : builder.closure) {
            const uint256& h=entry.second;
            BlockIndexSnapshot snap;
            if (!Block(h,&snap,error)) return false;
            if (!snap.found || snap.hash!=h || snap.height!=entry.first)
            { if(error) *error="option-r: inconsistent closure metadata: "+h.GetHex(); return false; }
            (*metadata)[h]=snap;
            if (snap.height < GetForkHeightDAG()) {
                if(!builder.preDAGTrust.count(h)) { if(error) *error="option-r: missing pre-DAG trust: "+h.GetHex(); return false; }
                (*preDAGTrust)[h]=builder.preDAGTrust.at(h);
            } else if (builder.parentsOf.count(h)) {
                CBlockDAGData rec;
                rec.vDAGParents=builder.parentsOf.at(h);
                rec.fBlue=false;
                (*records)[h]=rec;
            }
            ordered->push_back(entry);
        }
        if (error) error->clear();
        return true;
    } catch(const std::exception& ex) {
        if(error) *error=std::string("option-r: ")+ex.what();
        return false;
    }
}

bool ReconstructAuthoritativeDAGFields(
    const std::vector<std::pair<int32_t,uint256>>& scope,
    const CanonicalDAGRecolorSource& source,
    std::vector<CanonicalDAGRecolorRecord>* result,
    CanonicalDAGRecolorStats* stats, std::string* error)
{
    if (!result) { if(error) *error="recolor: null output"; return false; }
    result->clear();
    if (stats) *stats=CanonicalDAGRecolorStats();
    if (error) error->clear();
    const auto fail=[&](const std::string& why) { if(error) *error=why; return false; };
    // Hard retained-work budget, independent of total historical height.
    if (scope.size()>static_cast<size_t>(DAG_PRUNE_DEPTH))
        return fail("recolor: retained canvas exceeds work budget");
    try {
        std::vector<std::pair<int32_t,uint256>> ordered=scope;
        std::sort(ordered.begin(),ordered.end());
        std::map<uint256,BlockIndexSnapshot> metadata;
        std::map<uint256,CBlockDAGData> records;
        for (const auto& entry:ordered) {
            if (records.count(entry.second)) return fail("recolor: duplicate retained identity");
            BlockIndexSnapshot snap;
            if (!source.Block(entry.second,&snap,error)) return false;
            if (!snap.found || snap.hash!=entry.second || snap.height!=entry.first ||
                snap.height<GetForkHeightDAG() || snap.fProofOfStake)
                return fail("recolor: inconsistent retained metadata");
            CBlockDAGData data;
            if (!source.Parents(entry.second,&data.vDAGParents,error)) return false;
            if (data.vDAGParents.size()>MAX_DAG_PARENTS)
                return fail("recolor: excessive parent count");
            std::set<uint256> parents;
            for(const uint256& parent:data.vDAGParents)
                if(parent==uint256(0) || !parents.insert(parent).second)
                    return fail("recolor: invalid/duplicate parent identity");
            data.fBlue=false;
            metadata[entry.second]=snap;
            records.emplace(entry.second,data);
        }
        // Resolve every direct dependency before any coloring. Transitive DAG
        // dependencies are covered by the complete retained scope. Outside it,
        // traversal stops at an explicit pre-DAG/pruned boundary.
        for(const auto& record:records) {
            for(const uint256& parent:record.second.vDAGParents) {
                if(!metadata.count(parent)) {
                    BlockIndexSnapshot snap;
                    if(!source.Block(parent,&snap,error)) return false;
                    if(!snap.found || snap.hash!=parent || snap.height<0)
                        return fail("recolor: inconsistent parent metadata");
                    metadata[parent]=snap;
                }
                if(metadata.at(parent).height>=metadata.at(record.first).height)
                    return fail("recolor: non-ancestral parent height");
            }
        }
        std::map<uint256,std::unique_ptr<CBlockIndex>> owned;
        std::map<uint256,CBlockIndex*> localIndex;
        std::map<uint256,uint256> preDAGTrust; // pre-DAG trust for boundary closure leaves
        CanonicalDAGRecolorStats measured;
        measured.retainedVertices=records.size();
        // Merge the erased-selected-parent DAG-context closures into the canvas as
        // REAL DAG records (so GetBlueSet(P)/InferLocalK traverse genuine topology),
        // not just a scalar nChainTrust. This is the Option-R boundary-context
        // materialization; these vertices are temporary, never retained/frontier.
        std::vector<std::pair<int32_t,uint256>> colorOrder=ordered; // retained + closure
        measured.colorOrderEntries=colorOrder.size();
        for(const auto& item:metadata) {
            if (records.count(item.first)) continue;      // retained -> real record already
            if (item.second.height < GetForkHeightDAG()) continue; // pre-DAG -> trust below
            if (item.second.fProofOfStake)
                return fail("recolor: post-DAG PoS boundary not representable: "+item.first.GetHex());
            // Non-retained DAG-era parent: reconstruct its full bounded closure and
            // merge the closure records/metadata into the shared canvas.
            std::map<uint256,BlockIndexSnapshot> bMeta;
            std::map<uint256,uint256> bPre;
            std::map<uint256,CBlockDAGData> bRec;
            std::vector<std::pair<int32_t,uint256>> bOrd;
            if(!source.ReconstructBoundaryClosure(item.first,&bMeta,&bPre,&bRec,&bOrd,error)) return false;
            for(const auto& be:bMeta) if(!metadata.count(be.first)) metadata[be.first]=be.second;
            for(const auto& bp:bPre)  preDAGTrust[bp.first]=bp.second;
            for(const auto& br:bRec)  if(!records.count(br.first)) records[br.first]=br.second;
            for(const auto& bo:bOrd)  colorOrder.push_back(bo);
            ++measured.boundaryVertices;
            measured.boundaryClosureVertices += bMeta.size();
            measured.boundaryClosureDagRecords += bRec.size();
            // Raw CBlock reads are sequential during recursive closure resolution
            // (one CBlock in flight per call depth); peak ~ depth*(sizeof(CBlock)+vtx).
            measured.rawBlockBytes += 1 + (bMeta.size()<64?bMeta.size():64);
        }
        measured.metadataSnapshots=metadata.size();
        measured.preDAGTrustEntries=preDAGTrust.size();
        measured.parentsMemoEntries=records.size();
        // Re-sort the coloring order (retained + boundary closures) by height.
        std::sort(colorOrder.begin(),colorOrder.end());
        colorOrder.erase(std::unique(colorOrder.begin(),colorOrder.end()),colorOrder.end());
        measured.colorOrderEntries=colorOrder.size();
        for(const auto& item:metadata) {
            const BlockIndexSnapshot& snap=item.second;
            std::unique_ptr<CBlockIndex> pi(new CBlockIndex());
            pi->phashBlock=&item.first;
            pi->nHeight=snap.height; pi->nBits=snap.nBits; pi->nTime=snap.nTime;
            pi->nVersion=snap.nVersion; pi->nFlags=snap.nFlags; pi->hashProof=snap.hashProof;
            if(snap.fProofOfStake) pi->nFlags |= BLOCK_PROOF_OF_STAKE;
            if(snap.height<GetForkHeightDAG()) {
                if(preDAGTrust.count(item.first)) pi->nChainTrust=preDAGTrust.at(item.first);
                else if(!source.PreDAGTrust(item.first,&pi->nChainTrust,error)) return false;
                ++measured.preDAGBaseVertices;
            } else if(!records.count(item.first)) {
                // A boundary parent whose closure could not produce a real record
                // is a genuine fail-closed condition.
                return fail("recolor: boundary closure incomplete: "+item.first.GetHex());
            }
            localIndex[item.first]=pi.get();
            owned.emplace(item.first,std::move(pi));
        }
        measured.localIndexEntries=localIndex.size();
        measured.materializedObjects=owned.size();
        measured.objectBytes=owned.size()*sizeof(CBlockIndex);
        // Approximate peak temporary bytes (upper-bound per retained + closure vertex).
        measured.estimatedTemporaryBytes =
            measured.metadataSnapshots*sizeof(BlockIndexSnapshot)
          + measured.parentsMemoEntries*(sizeof(CBlockDAGData)+sizeof(std::vector<uint256>)+32)
          + measured.preDAGTrustEntries*(sizeof(uint256)*2)
          + measured.localIndexEntries*(sizeof(CBlockIndex)+sizeof(uint256)+sizeof(void*))
          + measured.boundaryClosureDagRecords*sizeof(CBlockDAGData)
          + measured.objectBytes
          + measured.rawBlockBytes;
        CDAGManager canvas(localIndex);
        canvas.LoadRecolorCanvas(records);
        for(const auto& entry:colorOrder) {
            if(!localIndex.count(entry.second)) continue;
            CBlockIndex* pi=localIndex.at(entry.second);
            if(pi->nHeight>=GetForkHeightDAGKnight()) canvas.ColorBlockDAGKnight(pi);
            else canvas.ColorBlock(pi);
        }
        std::vector<CanonicalDAGRecolorRecord> complete;
        for(const auto& entry:ordered) {
            CBlockDAGData data;
            if(!canvas.GetDAGData(entry.second,data)) return fail("recolor: incomplete result");
            complete.push_back({entry.second,entry.first,data.nDAGScore,data.fBlue,data.nInferredK});
        }
        result->swap(complete);
        if(stats) *stats=measured;
        return true;
    } catch(const std::exception& ex) {
        return fail(std::string("recolor: ")+ex.what());
    }
}

bool ReconstructAuthoritativeDAGScore(
    const std::vector<std::pair<int32_t,uint256>>& heightSorted,
    const std::map<uint256,std::vector<uint256>>& dagLinks,
    std::map<uint256,uint256>* canonicalScores, std::string* error)
{
    if(!canonicalScores) { if(error) *error="recolor: null score output"; return false; }
    canonicalScores->clear();
    CTxDB db("r");
    class SuppliedLinksSource : public AuthoritativeDAGRecolorSource {
        const std::map<uint256,std::vector<uint256>>& links;
    public:
        SuppliedLinksSource(CTxDB& db,const std::map<uint256,std::vector<uint256>>& l)
            :AuthoritativeDAGRecolorSource(db),links(l){}
        bool Parents(const uint256& h,std::vector<uint256>* out,std::string* error) const override {
            auto it=links.find(h);
            if(it==links.end()) { if(error) *error="recolor: missing supplied links"; return false; }
            *out=it->second; return true;
        }
    } source(db,dagLinks);
    std::vector<std::pair<int32_t,uint256>> scope;
    for(const auto& item:heightSorted) if(dagLinks.count(item.second)) scope.push_back(item);
    if(scope.size()!=dagLinks.size()) { if(error) *error="recolor: incomplete supplied scope"; return false; }
    std::vector<CanonicalDAGRecolorRecord> fields;
    if(!ReconstructAuthoritativeDAGFields(scope,source,&fields,NULL,error)) return false;
    for(const auto& record:fields) canonicalScores->emplace(record.hash,record.nDAGScore);
    return true;
}

uint256 GetAuthoritativeBlockTrust(const BlockIndexSnapshot& snap)
{
    CBigNum bnTarget;
    bnTarget.SetCompact(snap.nBits);
    if (bnTarget <= 0)
        return 0;
    if (snap.height >= GetForkHeightDAG() && snap.fProofOfStake)
        return 0;
    if (snap.height >= GetForkHeightPoem())
    {
        const uint256& entropyInput = (snap.fProofOfStake && snap.height < GetForkHeightDAG())
            ? snap.hashProof : snap.hash;
        return GetBlockEntropy(entropyInput);
    }
    return ((CBigNum(1) << 256) / (bnTarget + 1)).getuint256();
}

// Bounded authoritative accumulated chainTrust = chainTrust(parent) + blockTrust.
// Walks the ACTIVE chain from genesis to `snap` (via the same navigator cold
// reader used by ResolveAuthoritativeBlockSnapshot), accumulating
// GetAuthoritativeBlockTrust. The walk is bounded by the fixed pre-DAG active
// ancestry depth, never all history; no resident mapBlockIndex.
bool GetAuthoritativeAccumulatedChainTrust(const uint256& hash,
                                           uint256* out, std::string* error)
{
    if (!out) { if (error) *error = "trust accumulator: null output"; return false; }
    *out = 0;
    const BlockIndexV2Reader* reader = GetAuthoritativeNavigatorReader();
    if (!reader)
    {
        const ColdHotSeamNavigator* nav = GetBlockIndexStakingNavigator();
        reader = nav ? nav->GetColdReader() : NULL;
    }
    if (!reader || !reader->IsOpen()) { if (error) *error = "trust accumulator: reader unavailable"; return false; }

    BlockIndexSnapshot target;
    std::string terr;
    const BlockIndexV2ReadStatus st = reader->LookupByHash(hash, &target, &terr);
    if (st != BLOCK_INDEX_V2_READ_FOUND) { if (error) *error = "trust accumulator: hash not resolvable: " + terr; return false; }
    if (target.height < 0) { if (error) *error = "trust accumulator: negative height"; return false; }

    // Active-chain walk from genesis to target. Bounded by the active height
    // (= pre-DAG boundary for this consumer); designed for startup/reorg
    // maintenance, not a hot per-tip selector path.
    for (int h = 0; h <= target.height; ++h)
    {
        BlockIndexSnapshot cur;
        std::string cerr;
        const BlockIndexV2ReadStatus cst = reader->GetActiveByHeight(h, &cur, &cerr);
        if (cst != BLOCK_INDEX_V2_READ_FOUND) { if (error) *error = "trust accumulator: active height gap at " + std::to_string(h) + ": " + cerr; return false; }
        *out = *out + GetAuthoritativeBlockTrust(cur);
    }
    if (error) error->clear();
    return true;
}

// ---------------------------------------------------------------------------
// S3 staged-view enumeration: merged retained scope =
//   (persisted daglinks keys + staged writes - staged tombstones), height-sorted.
// The staged writes/tombstones come from the active WriteBatch. Each retained
// vertex's authoritative height is resolved by-value (never resident authority).
// Fail closed on any enumeration / resolution error.
bool EnumerateAuthoritativeStagedScope(
    CTxDB& db,
    std::vector<std::pair<int32_t,uint256>>* scope,
    const std::map<uint256,BlockIndexSnapshot>* chainedPending,
    std::string* error)
{
    if (!scope) { if (error) *error="S3 staged scope: null output"; return false; }
    scope->clear();
    if (error) error->clear();
    std::map<uint256,CBlockDAGData> stagedWrites;
    std::set<uint256> stagedTombstones;
    bool batchOpen=false;
    if (!db.ScanBatchDAGLinks(&stagedWrites,&stagedTombstones,&batchOpen,error)) return false;
    std::map<uint256,CBlockDAGData> persisted;
    // Authoritative source construction MUST fail closed on any malformed
    // persisted daglinks record: an incomplete/malformed retained canvas must
    // never be silently certified. The lenient IterateDAGLinks skips malformed
    // records (legacy/recovery), which would let a corrupted canvas receive
    // healthy score/child-count certificates. Use the strict iterator here so
    // any malformed record fails the whole authoritative staged-scope build.
    {
        std::string serr;
        if (!db.IterateDAGLinksStrict(persisted, &serr)) { if (error) *error="S3 staged scope: malformed persisted daglinks: "+serr; return false; }
    }
    // Merge: start with persisted, apply staged tombstones, then staged writes.
    std::map<uint256,CBlockDAGData> merged = persisted;
    for (const uint256& t : stagedTombstones) merged.erase(t);
    for (const auto& w : stagedWrites) merged[w.first] = w.second;
    // Resolve authoritative heights (by-value) for every retained vertex.
    // A mutation-owned PENDING block (chainedPending) — a post-generation block
    // accepted in THIS logical mutation but not yet published to the external
    // live authority (SetBestChain has not returned) — is resolved by value
    // from the pending overlay with precedence over current certified live tail
    // and immutable generation. No borrowed pointer; snapshot is by value.
    for (const auto& entry : merged) {
        BlockIndexSnapshot snap;
        std::string e2;
        bool resolved=false;
        if (chainedPending)
        {
            std::map<uint256,BlockIndexSnapshot>::const_iterator pit = chainedPending->find(entry.first);
            if (pit != chainedPending->end())
            {
                if (!pit->second.found || pit->second.hash != entry.first)
                { if (error) *error="S3 staged scope: pending snapshot identity mismatch for "+entry.first.GetHex(); return false; }
                snap = pit->second;
                resolved = true;
            }
        }
        if (!resolved)
        {
            if (!ResolveAuthoritativeBlockSnapshot(entry.first,&snap,&e2)) { if (error) *error="S3 staged scope: authoritative metadata unavailable for "+entry.first.GetHex()+": "+e2; return false; }
        }
        if (!snap.found || snap.hash!=entry.first) { if (error) *error="S3 staged scope: identity mismatch for "+entry.first.GetHex(); return false; }
        scope->push_back(std::make_pair((int32_t)snap.height, entry.first));
    }
    std::sort(scope->begin(), scope->end());
    return true;
}

// ---------------------------------------------------------------------------
// S3 full-field diff staging (shared by the authoritative stage and the
// rollback reconciliation). With diffOnlyWrites set, `chainedPending` keys are
// mutation-owned vertices that are ALWAYS written (the new block of an ADD,
// guaranteeing its authoritative values regardless of any earlier
// topology-staged resident residue); every OTHER vertex is written only when
// its layered full-field (staged write > tombstone > persisted DB) differs from
// the canonical recolor record. With diffOnlyWrites clear every record is
// staged (Reorganize/legacy semantics of the current engine). Returns false on
// any read/write failure; callers fail closed and abort the batch.
// ---------------------------------------------------------------------------
static bool StageDAGFullFieldRecords(CTxDB& db,
    const std::vector<CanonicalDAGRecolorRecord>& fields,
    const std::map<uint256,BlockIndexSnapshot>* chainedPending,
    bool diffOnlyWrites,
    AuthoritativeDAGStageResult* result, std::string* error)
{
    for (const auto& rec : fields) {
        const bool force = diffOnlyWrites && chainedPending &&
                           chainedPending->count(rec.hash) != 0;
        if (diffOnlyWrites && !force)
        {
            CBlockDAGData layered;
            if (!db.ReadDAGLinks(rec.hash, layered))
            { if (error) *error="S3 stage: diff full-field read failed for "+rec.hash.GetHex(); return false; }
            if (layered.fBlue == rec.fBlue &&
                layered.nDAGScore == rec.nDAGScore &&
                layered.nInferredK == rec.nInferredK)
                continue; // unchanged retained vertex -> not rewritten (minimal writeset)
        }
        CBlockDAGData data;
        if (!db.ReadDAGLinks(rec.hash, data)) { if (error) *error="S3 stage: full-field read failed for "+rec.hash.GetHex(); return false; }
        // Preserve the canonical topology (vDAGParents/vDAGChildren/order) and
        // replace the derived full-field with the authoritative current-canonical values.
        data.fBlue = rec.fBlue;
        data.nDAGScore = rec.nDAGScore;
        data.nInferredK = rec.nInferredK;
        if (!db.WriteDAGLinks(rec.hash, data)) { if (error) *error="S3 stage: full-field stage failed for "+rec.hash.GetHex(); return false; }
        result->fullFields.push_back(rec);
        result->affectedHashes.push_back(rec.hash);
        ++result->stagedFullFieldRecords;
    }
    return true;
}

// ---------------------------------------------------------------------------
// S3 atomic authoritative full-field staging (batch-only path).
// Runs the accepted isolated C-full/Option-R recolor against the staged source
// view (persisted daglinks + active-batch staged writes - staged tombstones),
// then stages, into the SAME WriteBatch: the authoritative full-field records for
// every affected retained canonical vertex, the new SourceStateId, the child-count
// certificate, and the DAG-score certificate. No global mapBlockIndex/g_dagManager
// mutation. On failure returns false (empty output); caller must TxnAbort.
bool StageAuthoritativeDAGScoreState(
    CTxDB& db,
    const std::vector<std::pair<int32_t,uint256>>& stagedScope,
    const uint256& newSourceToken,
    const std::map<uint256,BlockIndexSnapshot>* chainedPending,
    AuthoritativeDAGStageResult* result,
    std::string* error,
    bool diffOnlyWrites)
{
    if (!result) { if (error) *error="S3 stage: null output"; return false; }
    result->fullFields.clear(); result->affectedHashes.clear();
    if (error) error->clear();
    // Batch-only contract: enforce an active authoritative source transaction
    // BEFORE any write. Without this guard, an empty staged scope would fall
    // straight through to WriteDAGSourceStateId below, which (with no active
    // batch) writes the token durably to disk and only then fails in the
    // batch-only certificate helper - leaving a durable mutation despite
    // returning false. Reject first so zero durable mutation occurs on reject.
    if (!db.HasActiveBatch()) { if (error) *error="S3 stage: no active transaction (batch-only API)"; return false; }
    if (newSourceToken == uint256(0)) { if (error) *error="S3 stage: zero new source token"; return false; }
    // The recolor source reads parents via ReadDAGLinks, which (through the
    // generic Read + ScanBatch) already resolves the active-batch staged writes
    // and tombstones. Blocks/trust/boundaries come from by-value authoritative
    // metadata, so this is the accepted isolated C-full/Option-R path. When a
    // mutation-owned pending overlay is provided (post-gen winning block of THIS
    // Reorganize, not yet externally published), it is threaded into the recolor
    // source so every required vertex resolves by value during the mutation.
    AuthoritativeDAGRecolorSource source(db);
    if (chainedPending) source.SetPendingSnapshots(*chainedPending);
    std::vector<CanonicalDAGRecolorRecord> fields;
    CanonicalDAGRecolorStats stats;
    std::string rerr;
    if (!ReconstructAuthoritativeDAGFields(stagedScope, source, &fields, &stats, &rerr)) {
        if (error) *error="S3 stage: canonical recolor failed: "+rerr;
        return false;
    }
    // Stage authoritative full-field for every affected retained vertex back into
    // the SAME WriteBatch. WriteDAGLinks persists the full CBlockDAGData (parents +
    // fBlue + nDAGScore + nInferredK) and updates the child-count projection.
    // diffOnlyWrites (used by ordinary incremental ADD): force-write canonical
    // full-field for every mutation-owned pending vertex (chainedPending key; for
    // ADD that is exactly the newly-added block, guaranteeing its authoritative
    // values regardless of any earlier topology-staged resident residue), and for
    // every OTHER retained vertex stage canonical full-field ONLY if it differs
    // from the currently layered value (staged write > tombstone > persisted DB).
    // This yields the exact incremental writeset ({new block} for single-parent
    // ADD; any retained vertex whose full-field genuinely changes for a merge ADD)
    // while still running the full canonical recolor for correct leaf heritage.
    // Stage authoritative full-field for every affected retained vertex back into
    // the SAME WriteBatch (shared diff-only loop; see StageDAGFullFieldRecords).
    if (!StageDAGFullFieldRecords(db, fields, chainedPending, diffOnlyWrites, result, error)) return false;
    // Stage the new SourceStateId (advances the token; also binds the child-count
    // marker to it via WriteDAGSourceStateId), then both certificates.
    if (!db.WriteDAGSourceStateId(newSourceToken)) { if (error) *error="S3 stage: source token write failed"; return false; }
    if (!db.StageDAGScoreCertificateInBatch(newSourceToken, error)) return false;
    if (!db.StageDAGChildCountCertificateInBatch(newSourceToken, error)) return false;
    // Rough write-batch byte estimate: daglinks records (full-field) + markers + token.
    size_t approx = (sizeof(uint256)+sizeof(CBlockDAGData)+32) * fields.size();
    approx += 128; // markers + token
    result->writeBatchBytes = approx;
    result->stagedTopologyEntries = stats.retainedVertices;
    result->stats = stats;
    return true;
}

bool ReconcileAuthoritativeDAGScoreInBatch(
    CTxDB& db,
    const std::vector<std::pair<int32_t,uint256>>& stagedScope,
    AuthoritativeDAGStageResult* result,
    std::string* error)
{
    if (!result) { if (error) *error="S3 reconcile: null output"; return false; }
    result->fullFields.clear(); result->affectedHashes.clear();
    if (error) error->clear();
    // Batch-only contract, same as the authoritative stage.
    if (!db.HasActiveBatch()) { if (error) *error="S3 reconcile: no active transaction (batch-only API)"; return false; }
    // Recolor against the CURRENT merged staged view (the rollback batch already
    // holds the restored parents and the tombstone for the erased new block), so
    // every residue the failed mutation left on retained vertices is compared
    // against the canonical state OF THE RESTORED CANVAS and written back where
    // it differs. No pending overlay: the restored canvas has no un-published
    // vertices. No token/certificate staging: the caller restores those exactly.
    AuthoritativeDAGRecolorSource source(db);
    std::vector<CanonicalDAGRecolorRecord> fields;
    CanonicalDAGRecolorStats stats;
    std::string rerr;
    if (!ReconstructAuthoritativeDAGFields(stagedScope, source, &fields, &stats, &rerr)) {
        if (error) *error="S3 reconcile: canonical recolor failed: "+rerr;
        return false;
    }
    if (!StageDAGFullFieldRecords(db, fields, NULL, true, result, error)) return false;
    size_t approx = (sizeof(uint256)+sizeof(CBlockDAGData)+32) * result->stagedFullFieldRecords;
    result->writeBatchBytes = approx;
    result->stagedTopologyEntries = stats.retainedVertices;
    result->stats = stats;
    return true;
}
