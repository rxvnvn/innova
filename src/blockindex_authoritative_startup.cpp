// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.

#include "blockindex_authoritative_startup.h"

#include "blockindex_authoritative_live.h"
#include "blockindex_residency_counters.h"
#include "blockindex_startup_bootstrap.h"
#include "blockindex_stake_seen_builder.h"
#include "blockindex_candidate_startup_builder.h"
#include "blockindex_shadow_startup.h"   // RetainBlockIndexAuthoritativeNavigator
#include "blockindex_v2_reader.h"
#include "candidate_frontier.h"
#include "main.h"
#include "txdb.h"
#include <mutex>              // R4 AUTHORITY_READY barrier
#include <condition_variable> // R4 AUTHORITY_READY barrier
#include <chrono>             // R4 AUTHORITY_READY barrier
#include "fixed_blockindex_store.h"
#include "finality.h"

#include <memory>
#include <string>


namespace {

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
    AuthoritativeStartupContext() : ok(false) {}
};

// Single process-lifetime instance (constructed in authoritative mode).
AuthoritativeStartupContext* g_authoritativeContext = NULL;

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

// ---------------------------------------------------------------------------
// R4 — AUTHORITY_READY barrier implementation.
//
// ONE lifecycle barrier. It is published ONLY by the authoritative startup, after the six
// frozen prerequisites have been evaluated against real live state; every consensus-sensitive
// consumer waits on it. Legacy (non-authoritative) operation has no V2 authority and therefore
// no barrier requirement: the wait returns immediately there. No finality semantics are
// defined here — see the FINALITY FIREWALL note in the header.
// ---------------------------------------------------------------------------
namespace {

std::mutex g_authorityReadyMutex;
std::condition_variable g_authorityReadyCond;
bool g_authorityReadySet = false;
std::string g_authorityReadyDetail;

} // namespace

std::string AuthorityReadyPrerequisites::WhyNotReady() const
{
    if (!durableIndexLoaded)                return "V2 durable index not loaded";
    if (!immutableAuthorityAvailable)       return "immutable authority not available";
    if (!trustProjectionReconciled)         return "R2 trust projection not reconciled";
    if (!finalityEpochOwnerLifecycleReady)  return "FINALITY_EPOCH_OWNER_READY lifecycle condition not satisfied";
    return "";
}

bool AuthorityReadyMarkIfSatisfied(const AuthorityReadyPrerequisites& p, std::string* detail)
{
    const std::string why = p.WhyNotReady();
    if (!why.empty())
    {
        // A partially ready node is never published: the barrier stays unset and every
        // consumer stays gated.
        if (detail) *detail = "AUTHORITY_READY refused: " + why;
        {
            std::lock_guard<std::mutex> lock(g_authorityReadyMutex);
            g_authorityReadyDetail = why;   // refusal survives for diagnostics
        }
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(g_authorityReadyMutex);
        g_authorityReadySet = true;
        g_authorityReadyDetail = "prerequisites satisfied";
    }
    g_authorityReadyCond.notify_all();
    if (detail) *detail = "AUTHORITY_READY published";
    return true;
}

bool AuthorityReadyIsSet()
{
    std::lock_guard<std::mutex> lock(g_authorityReadyMutex);
    return g_authorityReadySet;
}

std::string AuthorityReadyStateName()
{
    std::lock_guard<std::mutex> lock(g_authorityReadyMutex);
    return g_authorityReadySet ? "READY" : "NOT_READY";
}

bool AuthorityReadyWait(uint64_t timeoutMs, std::string* error)
{
    std::unique_lock<std::mutex> lock(g_authorityReadyMutex);
    if (g_authorityReadySet) return true;
    if (!g_fAuthoritativeStartup) return true;   // legacy mode: no V2 authority to wait for
    const bool signalled = g_authorityReadyCond.wait_for(
        lock, std::chrono::milliseconds(timeoutMs), [](){ return g_authorityReadySet; });
    if (!g_authorityReadySet)
    {
        (void)signalled;
        if (error) *error = "AUTHORITY_READY not published within the consumer gate window";
        return false;
    }
    return true;
}

bool AuthorityReadyConsumerEnter(const char* consumer, std::string* error)
{
    std::string werr;
    if (!AuthorityReadyWait(60000, &werr))
    {
        if (error) *error = std::string("consumer '") + (consumer ? consumer : "?") +
                            "' cannot cross AUTHORITY_READY: " + werr;
        return false;
    }
    return true;
}

std::string AuthorityReadyRefusalDetail()
{
    std::lock_guard<std::mutex> lock(g_authorityReadyMutex);
    return g_authorityReadyDetail;
}

void AuthorityReadyResetForTest()
{
    std::lock_guard<std::mutex> lock(g_authorityReadyMutex);
    g_authorityReadySet = false;
    g_authorityReadyDetail.clear();
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

    // Readiness binds to the selected generation, not a DAG capability.
    const uint64_t selectedGeneration = ctx->bootstrap.ReaderPtr()->Generation();

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

    // R3 certification-stage result, consumed by the R4 AUTHORITY_READY prerequisite
    // 'R3 provenance/projection certification complete' at the true exit of this function.
    // VERIFIED / SUSPENDED / UNAVAILABLE are all COMPLETED determinations: a store that
    // cannot be positively certified must stay uncertified and fail closed (frozen R3
    // contract) and must NOT be prevented from booting. Only an I/O failure (probe false)
    // leaves the stage incomplete.
    bool custodyStageComplete = false;

    // R3 section 2 (production custody wiring) — establish / verify provenance custody at
    // the frozen lifecycle point: every durable prerequisite above is loaded and validated
    // and nothing has been published yet. This uses the SAME certification implementation
    // as R3.6 (there is deliberately no startup-only interpretation).
    {
        int custodyState = 2;
        uint64_t custodyEpoch = 0;
        std::string custodyDetail;
        // LEGACY DAG RETIREMENT (Phase 1, updated Phase 2 / Slice 3): the dormant Legacy DAG
        // engine owns no durable authority, so provenance/custody certification is not a
        // prerequisite of current consensus startup. Its startup-time establishment was the
        // retired engine's executable implementation and has been physically removed; the state
        // is REPORTED (never normalized to VERIFIED) and gates nothing.
        custodyState = 3;
        custodyStageComplete = true;
        custodyDetail = "Legacy DAG retired: provenance/custody certification not required";
        std::string custodyMsg = std::string("BLOCKINDEX_V2_AUTHORITATIVE custody_state=") +
                                 std::to_string(custodyState) + " custody_epoch=" +
                                 std::to_string((unsigned long long)custodyEpoch);
        if (!custodyDetail.empty()) custodyMsg += " detail=" + custodyDetail;
        printf("%s\n", custodyMsg.c_str());
        fflush(stdout);
        // Any state other than VERIFIED is REPORTED, never normalized: coverage is
        // unavailable/suspended for this session, availability loss stays explicit, and
        // provenance-dependent reconstruction fails closed through the final predicate.
        // A false-positive prune attribution remains impossible either way.

    }

    // HReg + wallet rescan are driven by init.cpp AFTER this returns, using
    // the by-value active-chain reader + by-value paths (ca7c7e1).

    // Persist the context for the process lifetime (anchors must survive).
    globalsGuard.Disarm();
    g_authoritativeContext = ctx.release();
    ::g_fAuthoritativeStartup = true;

    // R4 — AUTHORITY_READY: evaluated at the TRUE successful exit of the authoritative startup,
    // after the immutable authority has actually opened (`g_authoritativeContext` registered above
    // and the live authority open) and after every other frozen prerequisite is satisfied. The
    // barrier is published only when all four hold; no consumer may publish readiness itself.
    {
        AuthorityReadyPrerequisites pre;
        pre.durableIndexLoaded = (selectedGeneration != 0);
        {
            auto readyAuthority = GetAuthoritativeLiveAuthority();
            pre.immutableAuthorityAvailable = (readyAuthority != NULL && readyAuthority->IsOpen());
        }
        {
            // R2 trust projection reconciliation, evaluated over the frozen R2 domain.
            // The composite/pre-DAG accumulated-trust provider is defined on the PRE-DAG domain
            // and legitimately refuses a post-DAG hash; so the condition is: the authoritative
            // domain must resolve the current best chain, and it must additionally reproduce the
            // accumulated trust through the R2 composite provider whenever the best chain is
            // pre-DAG. Nothing is fabricated for a post-DAG chain (the DAG trust path owns it).
            BlockIndexSnapshot bestSnap;
            std::string bestSnapErr;
            const bool bestResolved =
                ResolveAuthoritativeBlockSnapshot(hashBestChain, &bestSnap, &bestSnapErr);
            if (bestResolved && bestSnap.height < GetForkHeightDAG())
            {
                std::string trustErr;
                uint256 projectedTrust;
                pre.trustProjectionReconciled =
                    GetAuthoritativeAccumulatedChainTrust(hashBestChain, &projectedTrust, &trustErr);
            }
            else
            {
                pre.trustProjectionReconciled = bestResolved;
            }
        }
        // Lifecycle readiness belongs to the current immutable/live authority.
        // No DAG runtime, custody or certificate is a current prerequisite.
        pre.finalityEpochOwnerLifecycleReady = pre.immutableAuthorityAvailable;
        std::string readyDetail;
        const bool authorityReady = AuthorityReadyMarkIfSatisfied(pre, &readyDetail);
        printf("BLOCKINDEX_V2_AUTHORITATIVE authority_ready=%d detail=%s\n",
               authorityReady ? 1 : 0, readyDetail.c_str());
        fflush(stdout);
    }
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

// A.10.1q / Stage1: emit residency for the retained authoritative context,
// reading the bootstrap HotOwner live metrics.
void ResetBlockIndexAuthoritativeStartupForTest()
{
    // Destroy only current authority owners and their published globals.
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
    printf("BLOCKINDEX_ATTRIBUTION %s stake_seen=%llu candidate_tips=%llu\n",
           tag, (unsigned long long)setStakeSeen.size(),
           (unsigned long long)mapCandidateTips.size());
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

// PRE-DAG AUTHORITATIVE ACCUMULATED TRUST.
//
// CONTRACT (the load-bearing invariant of this provider):
//
//   AuthoritativeAccumulatedTrust(H) == SUM GetAuthoritativeBlockTrust(X)
//
// for every block X on the REQUESTED HASH'S OWN ancestry genesis -> ... -> H,
// resolved through the exact persisted hashPrev links of the authoritative
// store. It is expressly NOT "the active-chain block at height(H)": a pre-DAG
// side branch at height h has its own accumulated trust, and resolving by
// target height alone returns the wrong uint256 for it.
//
// NAVIGATION (hash-driven, by value, no residency):
//   requested hash -> exact snapshot (identity-checked)
//                  -> exact logical parent (BlockIndexV2Reader::GetParent, which
//                     itself validates the child record and the parent hash)
//                  -> ... -> the ONE record whose PERSISTED authority proves it
//                     has no parent (snapshot.hasParent == false, i.e. the
//                     authoritative record's hashPrev is zero).
// This is the same cold authoritative reader ResolveAuthoritativeBlockSnapshot
// uses. It works for side branches (LookupByHash resolves any indexed record,
// not only active ones), touches no resident CBlockIndex, no mapBlockIndex, and
// introduces no cache. In particular it does NOT read derived.dat chainTrust as
// accumulated-trust authority.
//
// PROVENANCE / END-OF-CHAIN (never repeat the F1 hot-floor bug): the walk stops
// ONLY on a persisted `hashPrev == 0`. A child whose persisted authority claims
// a parent (hashPrev != 0) that the store cannot resolve, a hot/resident
// truncation, a missing local pointer, or any unavailable authority is an
// INCONSISTENT authority and FAILS CLOSED. Absence is never reinterpreted as
// canonical genesis.
//
// FAIL CLOSED on: reader unavailable/not open; requested hash absent or
// identity-mismatched; requested hash not pre-DAG (this provider's contract);
// any non-FOUND reader status (CORRUPT / IO_ERROR / NOT_OPEN); an unresolvable
// claimed parent; a parent hash/height that contradicts the child
// (parent.height == child.height - 1 required).
//
// BOUNDEDNESS: O(depth) time, O(1) temporary memory (one snapshot at a time, no
// vector), no object-graph reconstruction, no cache, no mapBlockIndex /
// mapDAGData growth. Depth is bounded by the requested height, and the requested
// height is bounded by FORK_HEIGHT_DAG by the check below.
bool GetAuthoritativeAccumulatedChainTrust(const uint256& hash,
                                           uint256* out, std::string* error)
{
    if (!out) { if (error) *error = "trust accumulator: null output"; return false; }
    *out = 0;
    // R2 / C2 — ONE RESOLUTION DOMAIN, ONE AUTHORITY.
    // The pre-DAG accumulated-trust authority resolves over the SAME complete
    // committed hot+cold domain the authoritative resolver uses
    // (BlockIndexAuthoritativeLive::ResolveBlockSnapshot — the single current-tail
    // snapshot seam consumed by ResolveParentScoreAuthoritative). A pre-DAG parent
    // retained only in the MUTABLE HOT TAIL (accepted after the immutable
    // generation was selected) therefore resolves exactly like a cold parent;
    // residency of the parent CBlockIndex is never consulted and never decides
    // whether the ancestor exists. A hot parent and a cold parent produce the
    // identical semantic result.
    //
    // The cold immutable reader is used ONLY when no composite authority is open
    // at all (non-authoritative / graded contexts). That is a strictly narrower
    // domain, so it can only FAIL CLOSED — it can never substitute a value, and a
    // hot-only parent can never be silently reported as absent genesis.
    BlockIndexAuthoritativeLive* live = GetAuthoritativeLiveAuthority();
    const bool fComposite = (live != NULL && live->IsOpen());
    const BlockIndexV2Reader* reader = NULL;
    if (!fComposite)
    {
        reader = GetAuthoritativeNavigatorReader();
        if (!reader)
        {
            const ColdHotSeamNavigator* nav = GetBlockIndexStakingNavigator();
            reader = nav ? nav->GetColdReader() : NULL;
        }
        if (!reader || !reader->IsOpen()) { if (error) *error = "trust accumulator: reader unavailable"; return false; }
    }

    BlockIndexSnapshot cur;
    std::string rerr;
    if (fComposite)
    {
        const BlockIndexHotStatus hst = live->ResolveBlockSnapshot(hash, &cur, &rerr);
        if (hst != BlockIndexHotStatus::OK)
        {
            if (error) *error = "trust accumulator: requested hash not resolvable by value in the "
                                "authoritative hot+cold domain: "
                                + (rerr.empty() ? std::string("(no detail)") : rerr);
            return false;
        }
    }
    else
    {
        const BlockIndexV2ReadStatus rst = reader->LookupByHash(hash, &cur, &rerr);
        if (rst != BLOCK_INDEX_V2_READ_FOUND)
        {
            if (error) *error = "trust accumulator: requested hash not resolvable by value: "
                                + (rerr.empty() ? std::string("(no detail)") : rerr);
            return false;
        }
    }
    if (!cur.found || cur.hash != hash)
    {
        if (error) *error = "trust accumulator: requested hash identity mismatch for " + hash.GetHex();
        return false;
    }
    if (cur.height < 0) { if (error) *error = "trust accumulator: negative height"; return false; }
    if (cur.height >= GetForkHeightDAG())
    {
        // This provider is the PRE-DAG accumulated-trust authority. A post-DAG
        // route reaching it is inconsistent metadata, not a pre-DAG parent.
        if (error) *error = "trust accumulator: hash " + hash.GetHex()
                            + " is not pre-DAG (height " + std::to_string(cur.height) + ")";
        return false;
    }

    uint256 acc = 0;
    for (;;)
    {
        acc = acc + GetAuthoritativeBlockTrust(cur);
        if (!cur.hasParent)
        {
            // The persisted authoritative record itself proves this vertex is a
            // chain start (authoritative hashPrev == 0). Not an inferred
            // termination and not a resident/hot truncation.
            *out = acc;
            if (error) error->clear();
            return true;
        }

        BlockIndexSnapshot parent;
        std::string perr;
        if (fComposite)
        {
            // Same composite domain for the ancestry walk: the parent is resolved
            // by its claimed hashPrev within the complete committed hot+cold
            // domain, so a hot-only ancestor is walked exactly like a cold one.
            const BlockIndexHotStatus phst = live->ResolveBlockSnapshot(cur.hashPrev, &parent, &perr);
            if (phst == BlockIndexHotStatus::AUTHORITY_MISSING)
            {
                // The child's persisted authority claims a parent (hashPrev != 0)
                // but the complete committed hot+cold domain does not carry it.
                // Incomplete authority is NEVER read as canonical genesis.
                if (error) *error = "trust accumulator: claimed parent " + cur.hashPrev.GetHex()
                                    + " of " + cur.hash.GetHex()
                                    + " is absent from the authoritative hot+cold domain";
                return false;
            }
            if (phst != BlockIndexHotStatus::OK)
            {
                if (error) *error = "trust accumulator: parent authority failure at " + cur.hash.GetHex() + ": "
                                    + (perr.empty() ? std::string("(no detail)") : perr);
                return false;
            }
        }
        else
        {
            const BlockIndexV2ReadStatus pst = reader->GetParent(cur.id, &parent, &perr);
            if (pst == BLOCK_INDEX_V2_READ_NOT_FOUND)
            {
                // The child's persisted authority claims a parent (hashPrev != 0)
                // but no authoritative record carries that hash. Incomplete
                // authority is NEVER read as canonical genesis.
                if (error) *error = "trust accumulator: claimed parent " + cur.hashPrev.GetHex()
                                    + " of " + cur.hash.GetHex() + " is absent from the authoritative store";
                return false;
            }
            if (pst != BLOCK_INDEX_V2_READ_FOUND)
            {
                if (error) *error = "trust accumulator: parent authority failure at " + cur.hash.GetHex() + ": "
                                    + (perr.empty() ? std::string("(no detail)") : perr);
                return false;
            }
        }
        if (!parent.found || parent.hash != cur.hashPrev)
        {
            if (error) *error = "trust accumulator: parent identity mismatch for " + cur.hash.GetHex();
            return false;
        }
        if (parent.height != cur.height - 1)
        {
            if (error) *error = "trust accumulator: parent height inconsistency at " + cur.hash.GetHex()
                                + " (child " + std::to_string(cur.height)
                                + ", parent " + std::to_string(parent.height) + ")";
            return false;
        }
        cur = parent;
    }
}
