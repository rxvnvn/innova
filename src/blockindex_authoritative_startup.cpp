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

bool InitBlockIndexAuthoritative(const std::string& v2Root, std::string* error)
{
    if (g_authoritativeContext)
    {
        if (error) *error = "authoritative startup: already initialized";
        return false;
    }

    std::unique_ptr<AuthoritativeStartupContext> ctx(new AuthoritativeStartupContext());
    ctx->v2Root = v2Root;

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

    // HReg + wallet rescan are driven by init.cpp AFTER this returns, using
    // the by-value active-chain reader + by-value paths (ca7c7e1).

    // Persist the context for the process lifetime (anchors must survive).
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

bool ResolveAuthoritativeBlockSnapshot(const uint256& hash,
                                       BlockIndexSnapshot* out,
                                       std::string* error)
{
    if (error) error->clear();
    if (!out)
        return false;
    const ColdHotSeamNavigator* nav = GetBlockIndexStakingNavigator();
    if (!nav)
    {
        if (error) *error = "authoritative block resolver: navigator unavailable";
        return false;
    }
    ColdHotSeamSnapshot snap;
    std::string err;
    const ColdHotSeamResult r = nav->ResolveLogicalR(
        BlockIndexLogicalId(hash), &snap, &err);
    if (r != COLD_HOT_SEAM_OK || !snap.snapshot.found)
    {
        if (error) *error = err.empty() ? "authoritative block resolver: block not found" : err;
        return false;
    }
    if (!snap.snapshot.fInMainChain)
    {
        const BlockIndexV2Reader* cold = nav->GetColdReader();
        BlockIndexSnapshot active;
        std::string activeError;
        if (cold && cold->GetActiveByHeight(snap.snapshot.height, &active, &activeError) == BLOCK_INDEX_V2_READ_FOUND &&
            active.hash == snap.snapshot.hash)
            snap.snapshot = active;
    }
    *out = snap.snapshot;
    return true;
}

bool ResolveAuthoritativeActiveBlock(const uint256& hash,
                                     BlockIndexSnapshot* out,
                                     std::string* error)
{
    if (!ResolveAuthoritativeBlockSnapshot(hash, out, error))
        return false;
    if (!out->fInMainChain)
    {
        if (error) *error = "authoritative block resolver: block is not active";
        return false;
    }
    return true;
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
