// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.
//
// R2c.2/S6 — authoritative primary DAG tip selector implementation.
// See dag_tip_selector.h for the full contract.
//
// Design notes (audit-facing):
//   - ONE algorithm (RunUnifiedSelection) over a SelectionSource abstraction.
//     The internal source reads the accepted S5 DagMutationPreview; the
//     external source reads the published healthy overlay runtime + the
//     authoritative by-value live authority. The candidate predicate, filters,
//     argmax and tie-break are character-equivalent to the legacy
//     CDAGManager::SelectBestDAGTip decision rule and to the accepted S5
//     DagMutationPreview::ResolveBestTip reduction.
//   - No locks are taken here. External callers hold cs_main (the accepted
//     cs_main -> cs_dag order is preserved by the callers; the selector itself
//     acquires neither). Internal callers run inside the mutation envelope on
//     the owner thread.
//   - No retained state: bounded winner-only reduction; the frontier is
//     streamed. The boundary walk keeps only a visited-set bounded by the walk
//     length (mirrors the legacy walk's guard).
//   - Fail closed everywhere: any enumeration/read failure aborts the run with
//     UNAVAILABLE; no partial winner is ever returned; the tentative result is
//     revalidated against the snapshot identity before release.

#include "dag_tip_selector.h"

#include "main.h"
#include "txdb-leveldb.h"
#include "dag_mutation_preview.h"
#include "dag_tip_overlay_runtime.h"
#include "dag_tip_live_overlay.h"
#include "blockindex_authoritative_startup.h"
#include "blockindex_authoritative_live.h"
#include "blockindex_shadow_startup.h"
#include "blockindex_tip.h"
#include "blockindex_v2_reader.h"

#include <algorithm>
#include <set>

// Defined in main.cpp; the overlay runtime's health predicate reads the same
// global source-health flag.
extern bool g_dagSourceUnhealthy;

namespace {

// ---------------------------------------------------------------------------
// File-scope registration + test seams (mirrors the accepted S5 pattern).
// Ownership bookkeeping only: no authority data, no retained collections.
// ---------------------------------------------------------------------------
dag_tip_frontier::DagTipOverlayRuntime* g_s6Runtime = NULL;
DagTipSelectorStats g_s6Stats;
DagTipSelectorEnumerationHook g_s6EnumHook = NULL;
void* g_s6EnumHookCtx = NULL;
bool g_s6ForceUnavailable = false;
DagTipSelectionReason g_s6ForceReason = DAG_TIP_SELECTION_REASON_NONE;

// R2c.2/S7 — merge-parent reduction state (ownership bookkeeping + counters only;
// no authority data, no retained collections).
DagMergeParentStats g_s7MergeStats;
bool g_s7MergeForceUnavailable = false;
DagTipSelectionReason g_s7MergeForceReason = DAG_TIP_SELECTION_REASON_NONE;

void FailResult(DagTipSelectionResult* res, DagTipSelectionReason reason,
                const std::string& diagnostic)
{
    res->status = DAG_TIP_SELECTION_UNAVAILABLE;
    res->reason = reason;
    res->hash = 0;
    res->score = 0;
    res->height = -1;
    res->fromActiveHeadFallback = false;
    res->diagnostic = diagnostic;
    ++g_s6Stats.unavailable;
}

// ---------------------------------------------------------------------------
// Candidate view (mirror of the accepted S5 DagMutationCandidateView fields
// consumed by the selector reduction).
// ---------------------------------------------------------------------------
struct CandidateView
{
    uint256 hash;
    bool retained;
    bool childless;
    bool hasFullField;
    uint256 nDAGScore;
    int height;
    bool proofOfStake;
    bool active;
    CandidateView()
        : hash(0), retained(false), childless(false), hasFullField(false),
          nDAGScore(0), height(-1), proofOfStake(false), active(false) {}
};

// ---------------------------------------------------------------------------
// R2c.2/S7 — merge-parent candidate view. SUPERSET of CandidateView: the
// merge-parent reduction additionally needs the candidate's accumulated
// chainTrust (the legacy `nChainTrust > nBestChainTrust` threshold) and whether
// it is resolvable by value at all (the legacy `mapBlockIndex` presence gate).
// ---------------------------------------------------------------------------
struct MergeCandidateView
{
    uint256 hash;
    bool resolvable;      // false => legacy residency-miss parity (silent skip)
    uint256 nChainTrust;  // accumulated chain trust (best-chain threshold input)
    uint256 nDAGScore;    // persisted canonical score (ComputeDAGScore parity)
    int height;
    bool proofOfStake;
    MergeCandidateView()
        : hash(0), resolvable(false), nChainTrust(0), nDAGScore(0), height(-1),
          proofOfStake(false) {}
};

// ---------------------------------------------------------------------------
// Source abstraction: the read context. Both implementations below produce the
// same candidate view; only the read source differs.
// ---------------------------------------------------------------------------
struct SelectionSource
{
    virtual ~SelectionSource() {}
    // Snapshot the source identity (token/generation) and validate the context.
    virtual bool Begin(uint256* token, DagTipSelectionReason* reason, std::string* error) = 0;
    // Stream the current authoritative frontier through `fn`.
    virtual bool ForEachFrontier(bool (*fn)(const uint256&, void*), void* ctx,
                                 DagTipSelectionReason* reason, std::string* error) = 0;
    // Resolve one candidate's by-value metadata at the snapshot.
    virtual bool ReadCandidate(const uint256& hash, CandidateView* out,
                               DagTipSelectionReason* reason, std::string* error) = 0;
    // Resolve the validated authoritative active head (semantic fallback).
    virtual bool ResolveActiveHead(uint256* hash, int* height,
                                   DagTipSelectionReason* reason, std::string* error) = 0;
    // Revalidate the snapshot identity before a result may be released.
    virtual bool Revalidate(DagTipSelectionReason* reason, std::string* error) = 0;

    // R2c.2/S7 — merge-parent metadata read (accumulated chainTrust + by-value
    // resolvability). NON-PURE with a FAIL-CLOSED default: a read context that
    // cannot supply the merge-parent contract (e.g. the internal S5 preview,
    // whose candidate view carries no chainTrust) refuses rather than silently
    // returning a wrong threshold input. Only the external CLEAN source
    // implements it, and only external CLEAN consumers may call the
    // merge-parent entry point.
    virtual bool ReadMergeCandidate(const uint256& hash, MergeCandidateView* out,
                                    DagTipSelectionReason* reason, std::string* error)
    {
        (void)hash; (void)out;
        if (reason) *reason = DAG_TIP_SELECTION_REASON_INTERNAL_FAILURE;
        if (error) *error = "selector: read context does not support merge-parent metadata";
        return false;
    }
};

// ---------------------------------------------------------------------------
// Shared by-value reads (mirror of the accepted S5 S5ReadCandidateInternal).
// ---------------------------------------------------------------------------
bool ReadCandidateKeyed(const uint256& hash, CandidateView* out,
                        DagTipSelectionReason* reason, std::string* error)
{
    *out = CandidateView();
    out->hash = hash;

    CTxDB db("r");
    bool member = false;
    if (!db.ReadDAGFrontierMembership(hash, &member))
    {
        if (reason) *reason = DAG_TIP_SELECTION_REASON_IO_FAILURE;
        if (error) *error = "selector: candidate predicate read failed";
        return false;
    }
    out->childless = member;

    CBlockDAGData data;
    if (db.ReadDAGLinks(hash, data))
    {
        out->retained = true;
        out->hasFullField = true;
        out->nDAGScore = data.nDAGScore;
    }
    else
    {
        out->retained = false;
    }

    BlockIndexAuthoritativeLive* live = GetAuthoritativeLiveAuthority();
    if (live && live->IsOpen())
    {
        BlockIndexAuthoritativeParentInfo info;
        std::string e;
        BlockIndexAuthoritativeParentStatus st = live->ResolveParentInfo(hash, &info, &e);
        if (st == BLOCK_INDEX_AUTHORITATIVE_PARENT_FOUND)
        {
            out->height = info.height;
            out->proofOfStake = info.proofOfStake;
            out->active = info.active;
        }
        else if (st == BLOCK_INDEX_AUTHORITATIVE_PARENT_NOT_FOUND)
        {
            // Not yet published (e.g. the block being added before
            // publication): unresolved and therefore ineligible, exactly like
            // the legacy selector which skips blocks above the current best.
            out->height = -1;
            out->active = false;
        }
        else
        {
            if (reason) *reason = DAG_TIP_SELECTION_REASON_METADATA_UNAVAILABLE;
            if (error) *error = "selector: candidate authority resolution failed";
            return false;
        }
    }
    else
    {
        if (reason) *reason = DAG_TIP_SELECTION_REASON_LIVE_AUTHORITY_MISSING;
        if (error) *error = "selector: live authority unavailable during candidate read";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Authoritative active head (semantic fallback source): tip authority current
// tip first, then the base generation committed tip. By value; bounded.
// ---------------------------------------------------------------------------
bool ResolveAuthoritativeActiveHeadInternal(uint256* hash, int* height,
                                            DagTipSelectionReason* reason,
                                            std::string* error)
{
    *hash = 0;
    *height = -1;
    BlockIndexAuthoritativeLive* live = GetAuthoritativeLiveAuthority();
    if (!live || !live->IsOpen())
    {
        if (reason) *reason = DAG_TIP_SELECTION_REASON_LIVE_AUTHORITY_MISSING;
        if (error) *error = "selector: live authority absent";
        return false;
    }
    const BlockIndexTipAuthority* tip = live->TipAuthority();
    if (tip && tip->IsOpen())
    {
        std::string e;
        BlockIndexTipRead tr = tip->GetTip();
        if (tr.status == BLOCK_INDEX_TIP_OK)
        {
            if (tr.record.hash == uint256(0) || tr.height < 0)
            {
                if (reason) *reason = DAG_TIP_SELECTION_REASON_ACTIVE_HEAD_UNAVAILABLE;
                if (error) *error = "selector: tip head identity invalid";
                return false;
            }
            *hash = tr.record.hash;
            *height = tr.height;
            return true;
        }
        if (tr.status != BLOCK_INDEX_TIP_NOT_FOUND)
        {
            if (reason) *reason = DAG_TIP_SELECTION_REASON_ACTIVE_HEAD_UNAVAILABLE;
            if (error) *error = "selector: tip head read failed";
            return false;
        }
        // Empty tip store: fall through to the base committed tip.
    }
    const BlockIndexV2Reader* reader = GetAuthoritativeNavigatorReader();
    if (!reader || !reader->IsOpen())
    {
        if (reason) *reason = DAG_TIP_SELECTION_REASON_LIVE_AUTHORITY_MISSING;
        if (error) *error = "selector: base reader absent";
        return false;
    }
    BlockIndexSnapshot s = reader->GetTip();
    if (!s.found || s.hash == uint256(0) || s.height < 0)
    {
        if (reason) *reason = DAG_TIP_SELECTION_REASON_ACTIVE_HEAD_UNAVAILABLE;
        if (error) *error = "selector: base committed tip unavailable";
        return false;
    }
    *hash = s.hash;
    *height = s.height;
    return true;
}

// ---------------------------------------------------------------------------
// Internal source: the accepted S5 transaction-scoped preview.
// ---------------------------------------------------------------------------
DagTipSelectionReason MapPreviewStatusToReason(DagMutationPreviewStatus st,
                                               const std::string& text)
{
    switch (st)
    {
    case DAG_MUTATION_PREVIEW_NO_ACTIVE_ROOT:
    case DAG_MUTATION_PREVIEW_INVALIDATED:
    case DAG_MUTATION_PREVIEW_WRONG_THREAD:
        return DAG_TIP_SELECTION_REASON_PREVIEW_INVALID;
    case DAG_MUTATION_PREVIEW_GENERATION_MISMATCH:
        return DAG_TIP_SELECTION_REASON_GENERATION_MISMATCH;
    case DAG_MUTATION_PREVIEW_STALE_TOKEN:
        return DAG_TIP_SELECTION_REASON_TOKEN_MISMATCH;
    case DAG_MUTATION_PREVIEW_INCOMPLETE_PREFIX:
        return DAG_TIP_SELECTION_REASON_PREVIEW_INCOMPLETE;
    case DAG_MUTATION_PREVIEW_SOURCE_UNAVAILABLE:
        if (text.find("child-count") != std::string::npos)
            return DAG_TIP_SELECTION_REASON_CHILD_COUNT_UNHEALTHY;
        if (text.find("score authority") != std::string::npos)
            return DAG_TIP_SELECTION_REASON_SCORE_AUTHORITY_UNHEALTHY;
        return DAG_TIP_SELECTION_REASON_SOURCE_UNHEALTHY;
    default:
        return DAG_TIP_SELECTION_REASON_INTERNAL_FAILURE;
    }
}

struct PreviewSelectionSource : SelectionSource
{
    const DagMutationPreview* preview;
    uint256 token0;

    explicit PreviewSelectionSource(const DagMutationPreview* p) : preview(p), token0(0) {}

    bool Begin(uint256* token, DagTipSelectionReason* reason, std::string* error) override
    {
        if (!preview)
        {
            if (reason) *reason = DAG_TIP_SELECTION_REASON_PREVIEW_INVALID;
            if (error) *error = "selector: internal consumer without preview in authoritative mode";
            return false;
        }
        std::string e;
        DagMutationPreviewStatus st = preview->Validate(&e);
        if (st != DAG_MUTATION_PREVIEW_OK)
        {
            if (reason) *reason = MapPreviewStatusToReason(st, e);
            if (error) *error = e.empty() ? "selector: preview validation failed" : e;
            return false;
        }
        st = preview->CurrentSourceStateId(&token0, &e);
        if (st != DAG_MUTATION_PREVIEW_OK)
        {
            if (reason) *reason = MapPreviewStatusToReason(st, e);
            if (error) *error = e.empty() ? "selector: preview token unavailable" : e;
            return false;
        }
        *token = token0;
        return true;
    }

    bool ForEachFrontier(bool (*fn)(const uint256&, void*), void* ctx,
                         DagTipSelectionReason* reason, std::string* error) override
    {
        std::string e;
        DagMutationPreviewStatus st = preview->ForEachCurrentTip(fn, ctx, &e);
        if (st != DAG_MUTATION_PREVIEW_OK)
        {
            if (reason) *reason = MapPreviewStatusToReason(st, e);
            if (error) *error = e.empty() ? "selector: preview frontier enumeration failed" : e;
            return false;
        }
        return true;
    }

    bool ReadCandidate(const uint256& hash, CandidateView* out,
                       DagTipSelectionReason* reason, std::string* error) override
    {
        DagMutationCandidateView view;
        std::string e;
        DagMutationPreviewStatus st = preview->ReadCandidate(hash, &view, &e);
        if (st != DAG_MUTATION_PREVIEW_OK)
        {
            if (reason) *reason = MapPreviewStatusToReason(st, e);
            if (error) *error = e.empty() ? "selector: preview candidate read failed" : e;
            return false;
        }
        out->hash = view.hash;
        out->retained = view.retained;
        out->childless = view.childless;
        out->hasFullField = view.hasFullField;
        out->nDAGScore = view.nDAGScore;
        out->height = view.height;
        out->proofOfStake = view.proofOfStake;
        out->active = view.active;
        return true;
    }

    bool ResolveActiveHead(uint256* hash, int* height,
                           DagTipSelectionReason* reason, std::string* error) override
    {
        return ResolveAuthoritativeActiveHeadInternal(hash, height, reason, error);
    }

    bool Revalidate(DagTipSelectionReason* reason, std::string* error) override
    {
        std::string e;
        DagMutationPreviewStatus st = preview->Validate(&e);
        if (st != DAG_MUTATION_PREVIEW_OK)
        {
            if (reason) *reason = MapPreviewStatusToReason(st, e);
            if (error) *error = "selector: preview revalidation failed: " + e;
            return false;
        }
        uint256 now;
        st = preview->CurrentSourceStateId(&now, &e);
        if (st != DAG_MUTATION_PREVIEW_OK || now != token0)
        {
            if (reason) *reason = DAG_TIP_SELECTION_REASON_REVALIDATION_FAILED;
            if (error) *error = "selector: preview token changed during enumeration";
            return false;
        }
        ++g_s6Stats.revalidations;
        return true;
    }
};

// ---------------------------------------------------------------------------
// External source: published healthy overlay runtime + by-value live authority.
// ---------------------------------------------------------------------------
struct CleanStreamCtx
{
    CTxDB* db;
    bool (*fn)(const uint256&, void*);
    void* userCtx;
    bool failed;
    DagTipSelectionReason failReason;
    std::string error;
};

bool CleanEmitVisitor(const uint256& hash, void* vctx)
{
    CleanStreamCtx* s = (CleanStreamCtx*)vctx;
    ++g_s6Stats.frontierVisits;
    ++g_s7MergeStats.frontierVisits;
    bool member = false;
    bool rowPresent = false;
    // G6: the membership predicate must attest canonical ROW PRESENCE, not just
    // membership. Enumerated authority (immutable seed + applied TIP_ADD deltas)
    // can only offer hashes that HAD a canonical daglinks row; an enumerated tip
    // whose row is absent means the canonical source is INCOMPLETE (e.g. a row
    // deleted out-of-band). Collapsing that into *member == false would silently
    // drop the merge parent and publish a REDUCED VALID vector, so fail the whole
    // enumeration instead (the caller returns UNAVAILABLE, never a partial vector).
    if (!s->db->ReadDAGFrontierMembershipAttested(hash, &member, &rowPresent))
    {
        s->failed = true;
        s->failReason = DAG_TIP_SELECTION_REASON_IO_FAILURE;
        s->error = "selector: frontier predicate read failed";
        return false;
    }
    if (!rowPresent)
    {
        ++g_s7MergeStats.frontierRowAbsent;
        s->failed = true;
        s->failReason = DAG_TIP_SELECTION_REASON_FRONTIER_ROW_ABSENT;
        s->error = "selector: canonical frontier row absent for enumerated authoritative tip";
        return false;
    }
    // Legitimate non-tip (row present, childCount > 0) keeps the historical
    // silent-skip semantics.
    if (!member) return true;
    ++g_s6Stats.frontierEmits;
    ++g_s7MergeStats.frontierEmits;
    return s->fn(hash, s->userCtx);
}

struct CleanSelectionSource : SelectionSource
{
    dag_tip_frontier::DagTipOverlayRuntime* runtime;
    dag_tip_frontier::LiveTipFrontierOverlay* overlay;
    uint256 token0;
    uint64_t gen0;

    CleanSelectionSource() : runtime(NULL), overlay(NULL), token0(0), gen0(0) {}

    bool Begin(uint256* token, DagTipSelectionReason* reason, std::string* error) override
    {
        runtime = g_s6Runtime;
        if (!runtime)
        {
            if (reason) *reason = DAG_TIP_SELECTION_REASON_RUNTIME_ABSENT;
            if (error) *error = "selector: no runtime registered in authoritative mode";
            return false;
        }
        if (!runtime->Available())
        {
            // Detailed fail-closed diagnostic: report which CLEAN gate failed.
            std::string why = "runtime not healthy CLEAN";
            if (runtime->Status() != dag_tip_frontier::DAG_TIP_OVERLAY_RUNTIME_AVAILABLE)
                why += " (runtime status unavailable)";
            else if (runtime->Consumer() && !runtime->Consumer()->Available())
                why += " (overlay consumer unavailable or applying)";
            else if (g_dagSourceUnhealthy)
                why += " (source health flag unhealthy)";
            else
            {
                dag_tip_frontier::LiveTipFrontierOverlay* ov = runtime->Overlay();
                dag_tip_frontier::LiveTipOverlayCheckpoint cp;
                std::string ce;
                if (ov && ov->ReadCheckpoint(&cp, &ce))
                {
                    why += std::string(" (checkpoint phase=") + std::to_string((int)cp.phase) +
                           " binding=" + (ov->IsImmutableBindingValid(cp) ? "1" : "0") + ")";
                    uint256 tok;
                    CTxDB tdb("r");
                    if (tdb.ReadDAGSourceStateId(tok))
                        why += (tok == cp.appliedSourceStateId) ? " (token match)" : " (token MISMATCH)";
                }
                else
                    why += " (checkpoint unreadable)";
            }
            if (reason) *reason = DAG_TIP_SELECTION_REASON_RUNTIME_UNAVAILABLE;
            if (error) *error = "selector: " + why;
            return false;
        }
        overlay = runtime->Overlay();
        if (!overlay || !overlay->IsOpen())
        {
            if (reason) *reason = DAG_TIP_SELECTION_REASON_RUNTIME_UNAVAILABLE;
            if (error) *error = "selector: runtime overlay not open";
            return false;
        }
        BlockIndexAuthoritativeLive* live = GetAuthoritativeLiveAuthority();
        if (!live || !live->IsOpen())
        {
            if (reason) *reason = DAG_TIP_SELECTION_REASON_LIVE_AUTHORITY_MISSING;
            if (error) *error = "selector: live authority absent";
            return false;
        }
        CTxDB db("r");
        if (!db.ReadDAGSourceStateId(token0))
        {
            if (reason) *reason = DAG_TIP_SELECTION_REASON_IO_FAILURE;
            if (error) *error = "selector: source-state token unavailable";
            return false;
        }
        // Trusting persisted canonical scores/child counts requires the same
        // authority-health gates the accepted S5 preview Validate enforces.
        {
            std::string herr;
            if (!db.IsDAGChildCountIndexHealthy(&herr))
            {
                if (reason) *reason = DAG_TIP_SELECTION_REASON_CHILD_COUNT_UNHEALTHY;
                if (error) *error = "selector: " + herr;
                return false;
            }
            if (!db.IsDAGScoreAuthorityHealthy(&herr))
            {
                if (reason) *reason = DAG_TIP_SELECTION_REASON_SCORE_AUTHORITY_UNHEALTHY;
                if (error) *error = "selector: " + herr;
                return false;
            }
        }
        gen0 = overlay->Generation();
        *token = token0;
        return true;
    }

    bool ForEachFrontier(bool (*fn)(const uint256&, void*), void* ctx,
                         DagTipSelectionReason* reason, std::string* error) override
    {
        CTxDB db("r");
        CleanStreamCtx s;
        s.db = &db;
        s.fn = fn;
        s.userCtx = ctx;
        s.failed = false;
        s.failReason = DAG_TIP_SELECTION_REASON_NONE;
        std::string e;
        if (!overlay->ForEachTip(&CleanEmitVisitor, &s, &e))
        {
            if (reason) *reason = DAG_TIP_SELECTION_REASON_FRONTIER_UNAVAILABLE;
            if (error) *error = e.empty() ? "selector: frontier enumeration failed" : e;
            return false;
        }
        if (s.failed)
        {
            // G6: the visitor distinguishes an IO failure from an enumerated
            // authoritative tip whose canonical row is absent; both fail the whole
            // enumeration (never a partial vector).
            if (reason) *reason = s.failReason == DAG_TIP_SELECTION_REASON_NONE
                                          ? DAG_TIP_SELECTION_REASON_IO_FAILURE
                                          : s.failReason;
            if (error) *error = s.error;
            return false;
        }
        return true;
    }

    bool ReadCandidate(const uint256& hash, CandidateView* out,
                       DagTipSelectionReason* reason, std::string* error) override
    {
        return ReadCandidateKeyed(hash, out, reason, error);
    }

    // R2c.2/S7 — merge-parent metadata by value. Reads the persisted canonical
    // score plus the authoritative snapshot (tip-then-base). No mapBlockIndex,
    // no mapDAGData, no residency requirement.
    bool ReadMergeCandidate(const uint256& hash, MergeCandidateView* out,
                            DagTipSelectionReason* reason, std::string* error) override
    {
        *out = MergeCandidateView();
        out->hash = hash;

        // Persisted canonical score (the legacy ComputeDAGScore source). An
        // absent record is NOT an error: legacy falls back to chainTrust.
        bool hasScoreRecord = false;
        uint256 persistedScore = 0;
        {
            CTxDB db("r");
            CBlockDAGData data;
            if (db.ReadDAGLinks(hash, data))
            {
                hasScoreRecord = true;
                persistedScore = data.nDAGScore;
            }
        }

        BlockIndexAuthoritativeLive* live = GetAuthoritativeLiveAuthority();
        if (!live || !live->IsOpen())
        {
            if (reason) *reason = DAG_TIP_SELECTION_REASON_LIVE_AUTHORITY_MISSING;
            if (error) *error = "merge-parent: live authority unavailable during candidate read";
            return false;
        }
        BlockIndexSnapshot snap;
        std::string e;
        const BlockIndexHotStatus st = live->ResolveBlockSnapshot(hash, &snap, &e);
        if (st == BlockIndexHotStatus::AUTHORITY_MISSING)
        {
            // Legacy parity: a candidate that cannot be resolved is silently
            // skipped, exactly like the legacy `mapBlockIndex.find == end`
            // branch. It is NOT an authority failure.
            out->resolvable = false;
            return true;
        }
        if (st != BlockIndexHotStatus::OK)
        {
            // Fail closed: an authority/IO/corruption failure must never be
            // collapsed into a silent omission that changes the result.
            if (reason) *reason = DAG_TIP_SELECTION_REASON_METADATA_UNAVAILABLE;
            if (error) *error = e.empty() ? "merge-parent: candidate metadata read failed" : e;
            return false;
        }
        out->resolvable = true;
        out->height = snap.height;
        out->proofOfStake = snap.fProofOfStake;
        out->nChainTrust = snap.nChainTrust;
        // ComputeDAGScore parity (dag.cpp:540-556): post-DAG PoS scores 0; a
        // persisted canonical record wins; otherwise the accumulated
        // chainTrust fallback for a block with no DAG record.
        if (snap.height >= FORK_HEIGHT_DAG && snap.fProofOfStake)
            out->nDAGScore = 0;
        else if (hasScoreRecord)
            out->nDAGScore = persistedScore;
        else
            out->nDAGScore = snap.nChainTrust;
        return true;
    }

    bool ResolveActiveHead(uint256* hash, int* height,
                           DagTipSelectionReason* reason, std::string* error) override
    {
        return ResolveAuthoritativeActiveHeadInternal(hash, height, reason, error);
    }

    bool Revalidate(DagTipSelectionReason* reason, std::string* error) override
    {
        if (!runtime->Available())
        {
            if (reason) *reason = DAG_TIP_SELECTION_REASON_REVALIDATION_FAILED;
            if (error) *error = "selector: runtime left CLEAN during enumeration";
            return false;
        }
        CTxDB db("r");
        uint256 now;
        if (!db.ReadDAGSourceStateId(now) || now != token0)
        {
            if (reason) *reason = DAG_TIP_SELECTION_REASON_REVALIDATION_FAILED;
            if (error) *error = "selector: source token changed during enumeration";
            return false;
        }
        {
            std::string herr;
            if (!db.IsDAGChildCountIndexHealthy(&herr) || !db.IsDAGScoreAuthorityHealthy(&herr))
            {
                if (reason) *reason = DAG_TIP_SELECTION_REASON_REVALIDATION_FAILED;
                if (error) *error = "selector: authority health lost during enumeration (" + herr + ")";
                return false;
            }
        }
        if (!overlay || overlay->Generation() != gen0)
        {
            if (reason) *reason = DAG_TIP_SELECTION_REASON_REVALIDATION_FAILED;
            if (error) *error = "selector: overlay generation changed during enumeration";
            return false;
        }
        ++g_s6Stats.revalidations;
        return true;
    }
};

// ---------------------------------------------------------------------------
// The reduction (mirror of the accepted S5 S5ResolveVisitor + legacy rule).
// ---------------------------------------------------------------------------
struct ResolveCtx
{
    SelectionSource* src;
    bool found;
    uint256 bestHash;
    uint256 bestScore;
    int bestHeight;
    bool failed;
    DagTipSelectionReason reason;
    std::string error;
    ResolveCtx()
        : src(NULL), found(false), bestHash(0), bestScore(0), bestHeight(-1),
          failed(false), reason(DAG_TIP_SELECTION_REASON_NONE) {}
};

bool S6ResolveVisitor(const uint256& tip, void* vctx)
{
    ResolveCtx* r = (ResolveCtx*)vctx;
    CandidateView view;
    DagTipSelectionReason reason = DAG_TIP_SELECTION_REASON_NONE;
    std::string e;
    ++g_s6Stats.candidateReads;
    if (!r->src->ReadCandidate(tip, &view, &reason, &e))
    {
        r->failed = true;
        r->reason = reason;
        r->error = e;
        return false;
    }
    if (!view.retained || !view.childless) return true;
    if (view.height >= FORK_HEIGHT_DAG && view.proofOfStake) return true; // exclude post-DAG PoS
    if (!view.active) return true;                                        // active-chain eligibility
    if (!r->found || view.nDAGScore > r->bestScore ||
        (view.nDAGScore == r->bestScore && tip < r->bestHash))
    {
        r->found = true;
        r->bestScore = view.nDAGScore;
        r->bestHash = tip;
        r->bestHeight = view.height;
    }
    return true;
}

// The single algorithm, identical for both read contexts.
DagTipSelectionResult RunUnifiedSelection(SelectionSource* src, bool internal)
{
    DagTipSelectionResult res;
    if (internal) ++g_s6Stats.internalCalls;
    else ++g_s6Stats.externalCalls;

    if (g_s6ForceUnavailable)
    {
        FailResult(&res, g_s6ForceReason == DAG_TIP_SELECTION_REASON_NONE
                             ? DAG_TIP_SELECTION_REASON_INTERNAL_FAILURE
                             : g_s6ForceReason,
                   "selector: forced unavailable (test)");
        return res;
    }

    uint256 token0;
    DagTipSelectionReason reason = DAG_TIP_SELECTION_REASON_NONE;
    std::string err;
    if (!src->Begin(&token0, &reason, &err))
    {
        FailResult(&res, reason, err);
        return res;
    }

    ResolveCtx ctx;
    ctx.src = src;
    reason = DAG_TIP_SELECTION_REASON_NONE;
    err.clear();
    if (!src->ForEachFrontier(&S6ResolveVisitor, &ctx, &reason, &err))
    {
        // A visitor abort (e.g. candidate read failure) carries the precise
        // reason; a source-level enumeration failure carries its own.
        if (ctx.failed)
            FailResult(&res, ctx.reason, ctx.error);
        else
            FailResult(&res, reason, err);
        return res;
    }
    if (ctx.failed)
    {
        FailResult(&res, ctx.reason, ctx.error);
        return res;
    }

    // Test seam: state may be mutated here to prove the revalidation gate.
    if (g_s6EnumHook) g_s6EnumHook(g_s6EnumHookCtx);

    if (ctx.found)
    {
        reason = DAG_TIP_SELECTION_REASON_NONE;
        err.clear();
        if (!src->Revalidate(&reason, &err))
        {
            FailResult(&res, reason == DAG_TIP_SELECTION_REASON_NONE
                                 ? DAG_TIP_SELECTION_REASON_REVALIDATION_FAILED
                                 : reason,
                       err);
            return res;
        }
        res.status = DAG_TIP_SELECTION_SELECTED;
        res.hash = ctx.bestHash;
        res.score = ctx.bestScore;
        res.height = ctx.bestHeight;
        ++g_s6Stats.selected;
        return res;
    }

    // Valid state, no eligible frontier candidate: semantic fallback to the
    // validated authoritative active head (never resident pindexBest).
    uint256 head;
    int headHeight = -1;
    reason = DAG_TIP_SELECTION_REASON_NONE;
    err.clear();
    if (!src->ResolveActiveHead(&head, &headHeight, &reason, &err))
    {
        FailResult(&res, reason == DAG_TIP_SELECTION_REASON_NONE
                             ? DAG_TIP_SELECTION_REASON_ACTIVE_HEAD_UNAVAILABLE
                             : reason,
                   err);
        return res;
    }
    reason = DAG_TIP_SELECTION_REASON_NONE;
    err.clear();
    if (!src->Revalidate(&reason, &err))
    {
        FailResult(&res, reason == DAG_TIP_SELECTION_REASON_NONE
                             ? DAG_TIP_SELECTION_REASON_REVALIDATION_FAILED
                             : reason,
                   err);
        return res;
    }
    res.status = DAG_TIP_SELECTION_VALID_NO_ELIGIBLE;
    res.hash = head;
    res.height = headHeight;
    res.score = 0;
    res.fromActiveHeadFallback = true;
    ++g_s6Stats.validNoEligible;
    ++g_s6Stats.activeHeadFallbacks;
    return res;
}

// ---------------------------------------------------------------------------
// Synthetic reduction fixture: pins the reduction semantics (filters, argmax,
// tie-break, zero-score eligibility, no-partial-winner) over the SAME
// production algorithm with a fully controlled read context. The real read
// contexts (S5 preview / CLEAN runtime) are pinned separately by the
// production-path fixtures.
// ---------------------------------------------------------------------------
struct SyntheticSelectionSource : SelectionSource
{
    const std::vector<DagTipSyntheticCandidate>* candidates;
    uint256 headHash;
    int headHeight;
    bool enumerationFails;
    bool revalidationFails;
    bool headUnavailable;

    SyntheticSelectionSource()
        : candidates(NULL), headHash(0), headHeight(-1), enumerationFails(false),
          revalidationFails(false), headUnavailable(false) {}

    bool Begin(uint256* token, DagTipSelectionReason*, std::string*) override
    {
        *token = uint256(0x56);
        return true;
    }
    bool ForEachFrontier(bool (*fn)(const uint256&, void*), void* ctx,
                         DagTipSelectionReason* reason, std::string* error) override
    {
        if (enumerationFails)
        {
            if (reason) *reason = DAG_TIP_SELECTION_REASON_FRONTIER_UNAVAILABLE;
            if (error) *error = "synthetic: enumeration failure";
            return false;
        }
        for (size_t i = 0; i < candidates->size(); ++i)
            if (!fn((*candidates)[i].hash, ctx))
                return false;
        return true;
    }
    bool ReadCandidate(const uint256& hash, CandidateView* out,
                       DagTipSelectionReason* reason, std::string* error) override
    {
        for (size_t i = 0; i < candidates->size(); ++i)
        {
            const DagTipSyntheticCandidate& c = (*candidates)[i];
            if (c.hash != hash) continue;
            if (c.readFails)
            {
                if (reason) *reason = DAG_TIP_SELECTION_REASON_METADATA_UNAVAILABLE;
                if (error) *error = "synthetic: candidate read failure";
                return false;
            }
            out->hash = c.hash;
            out->retained = c.retained;
            out->childless = c.childless;
            out->hasFullField = true;
            out->nDAGScore = c.score;
            out->height = c.height;
            out->proofOfStake = c.proofOfStake;
            out->active = c.active;
            return true;
        }
        if (reason) *reason = DAG_TIP_SELECTION_REASON_METADATA_UNAVAILABLE;
        if (error) *error = "synthetic: unknown candidate";
        return false;
    }
    bool ResolveActiveHead(uint256* hash, int* height,
                           DagTipSelectionReason* reason, std::string* error) override
    {
        if (headUnavailable || headHash == 0)
        {
            if (reason) *reason = DAG_TIP_SELECTION_REASON_ACTIVE_HEAD_UNAVAILABLE;
            if (error) *error = "synthetic: no active head";
            return false;
        }
        *hash = headHash;
        *height = headHeight;
        return true;
    }
    bool Revalidate(DagTipSelectionReason* reason, std::string* error) override
    {
        if (revalidationFails)
        {
            if (reason) *reason = DAG_TIP_SELECTION_REASON_REVALIDATION_FAILED;
            if (error) *error = "synthetic: revalidation failure";
            return false;
        }
        return true;
    }
};

DagTipSelectionResult RunDagTipSelectionFromSyntheticSourceForTest(
    SyntheticSelectionSource& src)
{
    return RunUnifiedSelection(&src, true);
}

// ---------------------------------------------------------------------------
// Authoritative traversal helpers (by value; no resident walks).
// ---------------------------------------------------------------------------

// Selected parent of `hash` by value: persisted daglinks parents + persisted
// parent scores; pre-DAG parents use the accepted authoritative accumulated
// chain trust; exact legacy argmax + tie-break. Returns false when the parent
// is unresolvable (pre-DAG vertex without persisted links, empty parent list)
// — the caller then uses the active-at-height fallback, exactly mirroring the
// legacy walk-break -> FindBlockByHeight fallback shape.
bool ResolveSelectedParentByValue(CTxDB& db, const uint256& hash, uint256* out,
                                  std::string* error)
{
    *out = 0;
    CBlockDAGData data;
    if (!db.ReadDAGLinks(hash, data) || data.vDAGParents.empty())
    {
        if (error) *error = "selected-parent: no persisted DAG links";
        return false;
    }
    uint256 hashBest = 0;
    uint256 nBestScore = 0;
    for (const uint256& hashParent : data.vDAGParents)
    {
        uint256 nParentScore = 0;
        CBlockDAGData pdata;
        if (db.ReadDAGLinks(hashParent, pdata))
        {
            nParentScore = pdata.nDAGScore;
        }
        else
        {
            // Pre-DAG parent: authoritative accumulated chain trust, guarded by
            // the exact legacy post-DAG PoS exclusion.
            BlockIndexAuthoritativeLive* live = GetAuthoritativeLiveAuthority();
            BlockIndexAuthoritativeParentInfo info;
            std::string pe;
            BlockIndexAuthoritativeParentStatus st =
                (live && live->IsOpen())
                    ? live->ResolveParentInfo(hashParent, &info, &pe)
                    : BLOCK_INDEX_AUTHORITATIVE_PARENT_FAILURE;
            if (st == BLOCK_INDEX_AUTHORITATIVE_PARENT_FOUND)
            {
                if (!(info.height >= FORK_HEIGHT_DAG && info.proofOfStake))
                {
                    uint256 trust;
                    std::string te;
                    if (!GetAuthoritativeAccumulatedChainTrust(hashParent, &trust, &te))
                    {
                        if (error) *error = "selected-parent: pre-DAG trust unavailable: " + te;
                        return false;
                    }
                    nParentScore = trust;
                }
            }
            else if (st == BLOCK_INDEX_AUTHORITATIVE_PARENT_NOT_FOUND)
            {
                // Legacy mirror: a parent absent from the resident graph scores 0.
                nParentScore = 0;
            }
            else
            {
                if (error) *error = "selected-parent: parent resolution failed";
                return false;
            }
        }
        if (nParentScore > nBestScore ||
            (nParentScore == nBestScore && (hashBest == 0 || hashParent < hashBest)))
        {
            nBestScore = nParentScore;
            hashBest = hashParent;
        }
    }
    *out = hashBest;
    return hashBest != 0;
}

bool ResolveActiveAtHeightInternal(int height, uint256* out, std::string* error)
{
    *out = 0;
    BlockIndexAuthoritativeLive* live = GetAuthoritativeLiveAuthority();
    if (!live || !live->IsOpen())
    {
        if (error) *error = "active-at-height: live authority absent";
        return false;
    }
    const BlockIndexTipAuthority* tip = live->TipAuthority();
    if (tip && tip->IsOpen())
    {
        std::string e;
        BlockIndexTipRead tr = tip->LookupActiveByHeight(height, &e);
        if (tr.status == BLOCK_INDEX_TIP_OK)
        {
            if (tr.record.hash == uint256(0))
            {
                if (error) *error = "active-at-height: tip identity invalid";
                return false;
            }
            *out = tr.record.hash;
            return true;
        }
        if (tr.status != BLOCK_INDEX_TIP_NOT_FOUND)
        {
            if (error) *error = "active-at-height: tip lookup failed";
            return false;
        }
    }
    const BlockIndexV2Reader* reader = GetAuthoritativeNavigatorReader();
    if (!reader || !reader->IsOpen())
    {
        if (error) *error = "active-at-height: base reader absent";
        return false;
    }
    BlockIndexSnapshot snap;
    std::string e;
    BlockIndexV2ReadStatus st = reader->GetActiveByHeight(height, &snap, &e);
    if (st == BLOCK_INDEX_V2_READ_FOUND && snap.found && snap.hash != uint256(0))
    {
        *out = snap.hash;
        return true;
    }
    if (error) *error = "active-at-height: not resolvable at height " + std::to_string(height);
    return false;
}

// ---------------------------------------------------------------------------
// R2c.2/S7 — the MERGE-PARENT reduction. A SEPARATE algorithm from the primary
// selector's argmax: it shares only the read context (SelectionSource), never
// the reduction. Character-equivalent to the legacy CreateNewBlock merge-parent
// loop (src/miner.cpp:265-334) with the residency gate replaced by the
// authoritative by-value resolvability gate.
// ---------------------------------------------------------------------------
struct MergeCand
{
    uint256 score;
    uint256 hash;
    int height;
};

struct MergeResolveCtx
{
    SelectionSource* src;
    uint256 primaryHash;
    uint256 bestHash;
    uint256 bestTrust;
    std::vector<MergeCand> cands;
    bool failed;
    DagTipSelectionReason reason;
    std::string error;
    MergeResolveCtx()
        : src(NULL), primaryHash(0), bestHash(0), bestTrust(0), failed(false),
          reason(DAG_TIP_SELECTION_REASON_NONE) {}
};

bool S7MergeResolveVisitor(const uint256& tip, void* vctx)
{
    MergeResolveCtx* r = (MergeResolveCtx*)vctx;
    MergeCandidateView view;
    DagTipSelectionReason reason = DAG_TIP_SELECTION_REASON_NONE;
    std::string e;
    ++g_s7MergeStats.candidateReads;
    if (!r->src->ReadMergeCandidate(tip, &view, &reason, &e))
    {
        r->failed = true;
        r->reason = reason;
        r->error = e;
        return false;
    }
    if (!view.resolvable)
    {
        // Legacy `mapBlockIndex.find == end` parity: silently skipped.
        ++g_s7MergeStats.unresolvableSkipped;
        return true;
    }
    if (tip == r->primaryHash)
    {
        ++g_s7MergeStats.primaryExcluded;
        return true;
    }
    if (tip != r->bestHash && view.nChainTrust > r->bestTrust)
    {
        ++g_s7MergeStats.trustExcluded;
        return true;
    }
    MergeCand c;
    c.score = view.nDAGScore;
    c.hash = tip;
    c.height = view.height;
    r->cands.push_back(c);
    return true;
}

// Exact legacy ordering (miner.cpp:293-298): score DESC, then hash ASC.
bool MergeCandGreater(const MergeCand& a, const MergeCand& b)
{
    if (a.score != b.score) return a.score > b.score;
    return a.hash < b.hash;
}

struct FrontierCollectCtx
{
    std::vector<uint256>* tips;
};

bool S7FrontierCollectVisitor(const uint256& tip, void* vctx)
{
    static_cast<FrontierCollectCtx*>(vctx)->tips->push_back(tip);
    return true;
}

void FailMergeResult(DagMergeParentResult* res, DagTipSelectionReason reason,
                     const std::string& diagnostic, const uint256& primaryHash,
                     int primaryHeight)
{
    res->status = DAG_MERGE_PARENT_UNAVAILABLE;
    res->reason = reason;
    res->parents.clear();
    res->primaryHash = primaryHash;
    res->primaryHeight = primaryHeight;
    res->diagnostic = diagnostic;
    ++g_s7MergeStats.unavailable;
}

void FailFrontierResult(DagFrontierTipsResult* res, DagTipSelectionReason reason,
                        const std::string& diagnostic)
{
    res->status = DAG_MERGE_PARENT_UNAVAILABLE;
    res->reason = reason;
    res->tips.clear();
    res->diagnostic = diagnostic;
    ++g_s7MergeStats.unavailable;
}

} // namespace

// ---------------------------------------------------------------------------
// Public entry points
// ---------------------------------------------------------------------------

DagTipSelectionResult SelectDagTipFromSyntheticFrontierForTest(
    const std::vector<DagTipSyntheticCandidate>& candidates,
    const uint256& activeHeadHash, int activeHeadHeight,
    bool enumerationFails, bool revalidationFails, bool headUnavailable)
{
    SyntheticSelectionSource src;
    src.candidates = &candidates;
    src.headHash = activeHeadHash;
    src.headHeight = activeHeadHeight;
    src.enumerationFails = enumerationFails;
    src.revalidationFails = revalidationFails;
    src.headUnavailable = headUnavailable;
    return RunDagTipSelectionFromSyntheticSourceForTest(src);
}

DagTipSelectionResult SelectDagTipForInternalConsumer(const DagMutationPreview* preview,
                                                      std::string* error)
{
    DagTipSelectionResult res;
    if (!preview)
    {
        if (g_fAuthoritativeStartup)
        {
            // Contract violation: authoritative mutation context without the
            // explicit transaction-scoped preview. Fail closed; never fall
            // back to the external CLEAN path or the legacy selector.
            FailResult(&res, DAG_TIP_SELECTION_REASON_PREVIEW_INVALID,
                       "selector: authoritative internal consumer requires the S5 preview");
            if (error) *error = res.diagnostic;
            return res;
        }
        res.status = DAG_TIP_SELECTION_LEGACY;
        if (error) error->clear();
        return res;
    }
    PreviewSelectionSource src(preview);
    res = RunUnifiedSelection(&src, true);
    if (error) *error = res.diagnostic;
    return res;
}

DagTipSelectionResult SelectDagTipForExternalConsumer(std::string* error)
{
    DagTipSelectionResult res;
    if (!g_fAuthoritativeStartup)
    {
        res.status = DAG_TIP_SELECTION_LEGACY;
        if (error) error->clear();
        return res;
    }
    CleanSelectionSource src;
    res = RunUnifiedSelection(&src, false);
    if (error) *error = res.diagnostic;
    return res;
}

// ---------------------------------------------------------------------------
// R2c.2/S7 — external CLEAN entry points (no mutation permit by construction).
// ---------------------------------------------------------------------------
DagMergeParentResult SelectMergeParentsForExternalConsumer(const uint256& primaryHash,
                                                           int primaryHeight,
                                                           std::string* error)
{
    DagMergeParentResult res;
    res.primaryHash = primaryHash;
    res.primaryHeight = primaryHeight;
    if (error) error->clear();

    if (!g_fAuthoritativeStartup)
    {
        res.status = DAG_MERGE_PARENT_LEGACY;
        return res;
    }
    ++g_s7MergeStats.calls;

    if (g_s7MergeForceUnavailable)
    {
        FailMergeResult(&res, g_s7MergeForceReason == DAG_TIP_SELECTION_REASON_NONE
                                  ? DAG_TIP_SELECTION_REASON_INTERNAL_FAILURE
                                  : g_s7MergeForceReason,
                        "merge-parent: forced unavailable (test)", primaryHash, primaryHeight);
        if (error) *error = res.diagnostic;
        return res;
    }
    if (primaryHash == uint256(0) || primaryHeight < 0)
    {
        FailMergeResult(&res, DAG_TIP_SELECTION_REASON_INTERNAL_FAILURE,
                        "merge-parent: caller-bound primary is not usable",
                        primaryHash, primaryHeight);
        if (error) *error = res.diagnostic;
        return res;
    }

    CleanSelectionSource src;
    uint256 token0;
    DagTipSelectionReason reason = DAG_TIP_SELECTION_REASON_NONE;
    std::string err;
    if (!src.Begin(&token0, &reason, &err))
    {
        FailMergeResult(&res, reason, err, primaryHash, primaryHeight);
        if (error) *error = res.diagnostic;
        return res;
    }

    MergeResolveCtx ctx;
    ctx.src = &src;
    ctx.primaryHash = primaryHash;
    // The trust threshold reads the SAME value-only scalars the legacy loop
    // reads (main.cpp:107 nBestChainTrust, main.cpp:121 hashBestChain); they are
    // maintained in authoritative mode by CBlock::SetBestChain (main.cpp:8839,
    // :8990-8994) which AddToBlockIndex drives at :9475-9478. No mapBlockIndex.
    ctx.bestHash = hashBestChain;
    ctx.bestTrust = nBestChainTrust;

    reason = DAG_TIP_SELECTION_REASON_NONE;
    err.clear();
    if (!src.ForEachFrontier(&S7MergeResolveVisitor, &ctx, &reason, &err))
    {
        FailMergeResult(&res, ctx.failed ? ctx.reason : reason,
                        ctx.failed ? ctx.error : err, primaryHash, primaryHeight);
        if (error) *error = res.diagnostic;
        return res;
    }
    if (ctx.failed)
    {
        FailMergeResult(&res, ctx.reason, ctx.error, primaryHash, primaryHeight);
        if (error) *error = res.diagnostic;
        return res;
    }

    std::sort(ctx.cands.begin(), ctx.cands.end(), MergeCandGreater);

    std::vector<uint256> parents;
    parents.push_back(primaryHash); // index 0, exactly like legacy miner.cpp:271-272

    for (size_t i = 0; i < ctx.cands.size(); ++i)
    {
        // Exact legacy cap semantics (miner.cpp:302-303): the cap counts the
        // primary, so at most MAX_DAG_PARENTS - 1 extras are appended.
        if (parents.size() >= (size_t)MAX_DAG_PARENTS)
        {
            ++g_s7MergeStats.capped;
            break;
        }
        if (ctx.cands[i].height < primaryHeight - DAG_MERGE_DEPTH)
        {
            ++g_s7MergeStats.depthExcluded;
            continue;
        }
        if (ctx.cands[i].height >= primaryHeight + 1)
        {
            ++g_s7MergeStats.heightExcluded;
            continue;
        }
        parents.push_back(ctx.cands[i].hash);
    }

    // Never release a result built from a source that moved under us.
    reason = DAG_TIP_SELECTION_REASON_NONE;
    err.clear();
    if (!src.Revalidate(&reason, &err))
    {
        FailMergeResult(&res, reason == DAG_TIP_SELECTION_REASON_NONE
                                  ? DAG_TIP_SELECTION_REASON_REVALIDATION_FAILED
                                  : reason,
                        err, primaryHash, primaryHeight);
        if (error) *error = res.diagnostic;
        return res;
    }

    res.status = DAG_MERGE_PARENT_VALID;
    res.reason = DAG_TIP_SELECTION_REASON_NONE;
    res.parents = parents;
    ++g_s7MergeStats.published;
    return res;
}

DagFrontierTipsResult SelectFrontierTipsForExternalConsumer(std::string* error)
{
    DagFrontierTipsResult res;
    if (error) error->clear();

    if (!g_fAuthoritativeStartup)
    {
        res.status = DAG_MERGE_PARENT_LEGACY;
        return res;
    }

    if (g_s7MergeForceUnavailable)
    {
        FailFrontierResult(&res, g_s7MergeForceReason == DAG_TIP_SELECTION_REASON_NONE
                                    ? DAG_TIP_SELECTION_REASON_INTERNAL_FAILURE
                                    : g_s7MergeForceReason,
                           "merge-parent: forced unavailable (test)");
        if (error) *error = res.diagnostic;
        return res;
    }

    CleanSelectionSource src;
    uint256 token0;
    DagTipSelectionReason reason = DAG_TIP_SELECTION_REASON_NONE;
    std::string err;
    if (!src.Begin(&token0, &reason, &err))
    {
        FailFrontierResult(&res, reason, err);
        if (error) *error = res.diagnostic;
        return res;
    }

    FrontierCollectCtx ctx;
    ctx.tips = &res.tips;
    reason = DAG_TIP_SELECTION_REASON_NONE;
    err.clear();
    if (!src.ForEachFrontier(&S7FrontierCollectVisitor, &ctx, &reason, &err))
    {
        FailFrontierResult(&res, reason, err);
        if (error) *error = res.diagnostic;
        return res;
    }
    std::sort(res.tips.begin(), res.tips.end());

    reason = DAG_TIP_SELECTION_REASON_NONE;
    err.clear();
    if (!src.Revalidate(&reason, &err))
    {
        FailFrontierResult(&res, reason == DAG_TIP_SELECTION_REASON_NONE
                                    ? DAG_TIP_SELECTION_REASON_REVALIDATION_FAILED
                                    : reason,
                           err);
        if (error) *error = res.diagnostic;
        return res;
    }

    res.status = DAG_MERGE_PARENT_VALID;
    res.reason = DAG_TIP_SELECTION_REASON_NONE;
    return res;
}

DagMergeParentStats GetDagMergeParentStats()
{
    return g_s7MergeStats;
}

void ResetDagMergeParentStatsForTest()
{
    g_s7MergeStats = DagMergeParentStats();
}

void SetMergeParentForceUnavailableForTest(bool armed, DagTipSelectionReason reason)
{
    g_s7MergeForceUnavailable = armed;
    g_s7MergeForceReason = reason;
}

DagTipSelectionResult ResolveAuthoritativeBoundaryAtHeight(const uint256& fromHash,
                                                           int targetHeight,
                                                           std::string* error)
{
    DagTipSelectionResult res;
    if (fromHash == uint256(0) || targetHeight < 0)
    {
        FailResult(&res, DAG_TIP_SELECTION_REASON_TRAVERSAL_UNAVAILABLE,
                   "boundary: invalid arguments");
        if (error) *error = res.diagnostic;
        return res;
    }
    BlockIndexAuthoritativeLive* live = GetAuthoritativeLiveAuthority();
    if (!live || !live->IsOpen())
    {
        FailResult(&res, DAG_TIP_SELECTION_REASON_LIVE_AUTHORITY_MISSING,
                   "boundary: live authority absent");
        if (error) *error = res.diagnostic;
        return res;
    }

    BlockIndexAuthoritativeParentInfo info;
    std::string e;
    BlockIndexAuthoritativeParentStatus st = live->ResolveParentInfo(fromHash, &info, &e);
    if (st == BLOCK_INDEX_AUTHORITATIVE_PARENT_NOT_FOUND)
    {
        FailResult(&res, DAG_TIP_SELECTION_REASON_TRAVERSAL_UNAVAILABLE,
                   "boundary: start hash not resolvable");
        if (error) *error = res.diagnostic;
        return res;
    }
    if (st != BLOCK_INDEX_AUTHORITATIVE_PARENT_FOUND)
    {
        FailResult(&res, DAG_TIP_SELECTION_REASON_METADATA_UNAVAILABLE,
                   "boundary: start resolution failed");
        if (error) *error = res.diagnostic;
        return res;
    }

    int curHeight = info.height;
    if (curHeight < targetHeight)
    {
        FailResult(&res, DAG_TIP_SELECTION_REASON_TRAVERSAL_UNAVAILABLE,
                   "boundary: target above start height");
        if (error) *error = res.diagnostic;
        return res;
    }

    // Pass 1: by-value selected-parent walk (exact legacy walk shape).
    uint256 cur = fromHash;
    bool walkOk = true;
    std::string walkError;
    {
        CTxDB db("r");
        std::set<uint256> visited;
        while (curHeight > targetHeight)
        {
            if (!visited.insert(cur).second)
            {
                walkOk = false;
                walkError = "cycle guard tripped";
                break;
            }
            uint256 parent;
            std::string we;
            if (!ResolveSelectedParentByValue(db, cur, &parent, &we) || parent == uint256(0))
            {
                walkOk = false;
                walkError = we.empty() ? "selected parent unresolvable" : we;
                break;
            }
            BlockIndexAuthoritativeParentInfo pinfo;
            std::string pe;
            BlockIndexAuthoritativeParentStatus pst = live->ResolveParentInfo(parent, &pinfo, &pe);
            if (pst != BLOCK_INDEX_AUTHORITATIVE_PARENT_FOUND)
            {
                walkOk = false;
                walkError = "walk parent not resolvable";
                break;
            }
            if (pinfo.height >= curHeight)
            {
                walkOk = false;
                walkError = "walk did not descend";
                break;
            }
            cur = parent;
            curHeight = pinfo.height;
        }
    }
    if (walkOk && curHeight == targetHeight)
    {
        ++g_s6Stats.boundaryWalks;
        res.status = DAG_TIP_SELECTION_SELECTED;
        res.hash = cur;
        res.height = curHeight;
        res.diagnostic = "boundary: selected-parent walk";
        if (error) error->clear();
        return res;
    }

    // Pass 2: authoritative active-at-height fallback (never a resident walk).
    uint256 fallback;
    std::string fe;
    if (ResolveActiveAtHeightInternal(targetHeight, &fallback, &fe))
    {
        ++g_s6Stats.boundaryFallbacks;
        res.status = DAG_TIP_SELECTION_SELECTED;
        res.hash = fallback;
        res.height = targetHeight;
        res.diagnostic = "boundary: active-at-height fallback";
        if (error) error->clear();
        return res;
    }

    FailResult(&res, DAG_TIP_SELECTION_REASON_TRAVERSAL_UNAVAILABLE,
               "boundary: walk failed (" + walkError + "); fallback failed (" + fe + ")");
    if (error) *error = res.diagnostic;
    return res;
}

DagTipSelectionResult ResolveAuthoritativeActiveAtHeight(int height, std::string* error)
{
    DagTipSelectionResult res;
    uint256 hash;
    std::string e;
    if (height < 0 || !ResolveActiveAtHeightInternal(height, &hash, &e))
    {
        FailResult(&res, DAG_TIP_SELECTION_REASON_TRAVERSAL_UNAVAILABLE,
                   e.empty() ? "active-at-height: invalid height" : e);
        if (error) *error = res.diagnostic;
        return res;
    }
    res.status = DAG_TIP_SELECTION_SELECTED;
    res.hash = hash;
    res.height = height;
    res.diagnostic = "active-at-height";
    if (error) error->clear();
    return res;
}

// ---------------------------------------------------------------------------
// Registration + test seams
// ---------------------------------------------------------------------------
void SetDagTipSelectorRuntime(dag_tip_frontier::DagTipOverlayRuntime* runtime)
{
    g_s6Runtime = runtime;
}

void ClearDagTipSelectorRuntime()
{
    g_s6Runtime = NULL;
}

bool IsDagTipSelectorRuntimeRegistered()
{
    return g_s6Runtime != NULL;
}

DagTipSelectorStats GetDagTipSelectorStats()
{
    return g_s6Stats;
}

void ResetDagTipSelectorStatsForTest()
{
    g_s6Stats = DagTipSelectorStats();
}

void SetDagTipSelectorEnumerationHookForTest(DagTipSelectorEnumerationHook hook, void* ctx)
{
    g_s6EnumHook = hook;
    g_s6EnumHookCtx = ctx;
}

void SetDagTipSelectorForceUnavailableForTest(bool armed, DagTipSelectionReason reason)
{
    g_s6ForceUnavailable = armed;
    g_s6ForceReason = reason;
}

const char* DagTipSelectionStatusName(DagTipSelectionStatus status)
{
    switch (status)
    {
    case DAG_TIP_SELECTION_LEGACY: return "LEGACY";
    case DAG_TIP_SELECTION_SELECTED: return "SELECTED";
    case DAG_TIP_SELECTION_VALID_NO_ELIGIBLE: return "VALID_NO_ELIGIBLE";
    case DAG_TIP_SELECTION_UNAVAILABLE: return "UNAVAILABLE";
    default: return "UNKNOWN";
    }
}

const char* DagTipSelectionReasonName(DagTipSelectionReason reason)
{
    switch (reason)
    {
    case DAG_TIP_SELECTION_REASON_NONE: return "NONE";
    case DAG_TIP_SELECTION_REASON_SOURCE_UNHEALTHY: return "SOURCE_UNHEALTHY";
    case DAG_TIP_SELECTION_REASON_SCORE_AUTHORITY_UNHEALTHY: return "SCORE_AUTHORITY_UNHEALTHY";
    case DAG_TIP_SELECTION_REASON_CHILD_COUNT_UNHEALTHY: return "CHILD_COUNT_UNHEALTHY";
    case DAG_TIP_SELECTION_REASON_GENERATION_MISMATCH: return "GENERATION_MISMATCH";
    case DAG_TIP_SELECTION_REASON_TOKEN_MISMATCH: return "TOKEN_MISMATCH";
    case DAG_TIP_SELECTION_REASON_PREVIEW_INCOMPLETE: return "PREVIEW_INCOMPLETE";
    case DAG_TIP_SELECTION_REASON_PREVIEW_INVALID: return "PREVIEW_INVALID";
    case DAG_TIP_SELECTION_REASON_RUNTIME_ABSENT: return "RUNTIME_ABSENT";
    case DAG_TIP_SELECTION_REASON_RUNTIME_UNAVAILABLE: return "RUNTIME_UNAVAILABLE";
    case DAG_TIP_SELECTION_REASON_LIVE_AUTHORITY_MISSING: return "LIVE_AUTHORITY_MISSING";
    case DAG_TIP_SELECTION_REASON_FRONTIER_UNAVAILABLE: return "FRONTIER_UNAVAILABLE";
    case DAG_TIP_SELECTION_REASON_METADATA_UNAVAILABLE: return "METADATA_UNAVAILABLE";
    case DAG_TIP_SELECTION_REASON_IO_FAILURE: return "IO_FAILURE";
    case DAG_TIP_SELECTION_REASON_ACTIVE_HEAD_UNAVAILABLE: return "ACTIVE_HEAD_UNAVAILABLE";
    case DAG_TIP_SELECTION_REASON_TRAVERSAL_UNAVAILABLE: return "TRAVERSAL_UNAVAILABLE";
    case DAG_TIP_SELECTION_REASON_REVALIDATION_FAILED: return "REVALIDATION_FAILED";
    case DAG_TIP_SELECTION_REASON_INTERNAL_FAILURE: return "INTERNAL_FAILURE";
    case DAG_TIP_SELECTION_REASON_FRONTIER_ROW_ABSENT: return "FRONTIER_ROW_ABSENT";
    default: return "UNKNOWN";
    }
}
