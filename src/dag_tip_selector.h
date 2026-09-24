// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.
//
// R2c.2/S6 — authoritative primary DAG tip selector (value-semantic).
//
// ONE selection algorithm over TWO read contexts:
//   - internal mutation-scoped synchronous consumers: the accepted S5
//     DagMutationPreview (committed-prefix transaction view);
//   - external CLEAN runtime consumers: the published healthy overlay runtime
//     plus the authoritative by-value live authority.
//
// The selector returns VALUES (hash / height / score / status); it never
// returns a borrowed resident pointer and never retains authority state. Winner
// materialization is a separate bounded step owned by the consumer
// (BlockIndexAuthoritativeLive::MaterializeParentChain + BlockIndexHotHandle).
//
// Authoritative-mode contract (g_fAuthoritativeStartup == true):
//   - UNAVAILABLE is fail-closed. It is NEVER silently replaced by the legacy
//     resident selector, pindexBest, setDAGTips, mapDAGData, or mapBlockIndex.
//   - VALID_NO_ELIGIBLE means the authoritative state is valid and complete and
//     no eligible frontier candidate exists; the semantic fallback is the
//     validated authoritative active head (tip authority GetTip() or the base
//     generation committed tip), never resident pindexBest.
//   - The selection algorithm is identical to the legacy decision rule:
//       frontier membership, post-DAG PoS exclusion, active-chain eligibility,
//       maximize persisted canonical nDAGScore, tie => numerically smaller
//       hash, zero score eligible, no eligible winner => semantic fallback.
//   - LEGACY is returned only when authoritative mode is off; callers then run
//     the unchanged historical resident selector (CDAGManager::SelectBestDAGTip).
//
// Direct-consumer classification (S6 Phase 1 inventory, durable report):
//   internal (A): RebuildDAGOrder, RebuildDAGOrderIncremental, ComputeEpochState
//                 -> SelectDagTipForInternalConsumer(preview, ...)
//   external (B): CreateNewBlock, CaptureCurrentCPUMiningWorkIdentity,
//                 IsCPUMiningCollateralStateReady, ProduceFinalityVote
//                 -> SelectDagTipForExternalConsumer(...)
//   legacy-only (C): RebuildDAGOrder/RebuildDAGOrderIncremental from the legacy
//                 startup path (no preview, authoritative mode off) -> LEGACY.

#ifndef INNOVA_DAG_TIP_SELECTOR_H
#define INNOVA_DAG_TIP_SELECTOR_H

#include "uint256.h"

#include <stdint.h>
#include <string>

class DagMutationPreview;

namespace dag_tip_frontier
{
class DagTipOverlayRuntime;
}

// ---------------------------------------------------------------------------
// Value-semantic result model
// ---------------------------------------------------------------------------

enum DagTipSelectionStatus
{
    DAG_TIP_SELECTION_LEGACY = 0,            // authoritative mode off; caller runs the legacy selector
    DAG_TIP_SELECTION_SELECTED = 1,          // eligible frontier winner found
    DAG_TIP_SELECTION_VALID_NO_ELIGIBLE = 2, // valid authoritative state; fallback = authoritative active head
    DAG_TIP_SELECTION_UNAVAILABLE = 3        // fail closed; reason/diagnostic describe why
};

enum DagTipSelectionReason
{
    DAG_TIP_SELECTION_REASON_NONE = 0,
    DAG_TIP_SELECTION_REASON_SOURCE_UNHEALTHY,          // DAG source persistence health flag down
    DAG_TIP_SELECTION_REASON_SCORE_AUTHORITY_UNHEALTHY, // score certificate absent/stale/revoked
    DAG_TIP_SELECTION_REASON_CHILD_COUNT_UNHEALTHY,     // child-count certificate absent/stale/revoked
    DAG_TIP_SELECTION_REASON_GENERATION_MISMATCH,       // authoritative generation changed
    DAG_TIP_SELECTION_REASON_TOKEN_MISMATCH,            // source-state token changed / stale
    DAG_TIP_SELECTION_REASON_PREVIEW_INCOMPLETE,        // staged-but-uncommitted pending prefix present
    DAG_TIP_SELECTION_REASON_PREVIEW_INVALID,           // preview permit invalid (root/thread/invalidated/missing)
    DAG_TIP_SELECTION_REASON_RUNTIME_ABSENT,            // no runtime registered while authoritative mode is on
    DAG_TIP_SELECTION_REASON_RUNTIME_UNAVAILABLE,       // runtime not healthy CLEAN (token/checkpoint/binding)
    DAG_TIP_SELECTION_REASON_LIVE_AUTHORITY_MISSING,    // by-value live authority absent/unopened
    DAG_TIP_SELECTION_REASON_FRONTIER_UNAVAILABLE,      // frontier enumeration failed
    DAG_TIP_SELECTION_REASON_METADATA_UNAVAILABLE,      // candidate metadata read failed
    DAG_TIP_SELECTION_REASON_IO_FAILURE,                // keyed persisted read failed
    DAG_TIP_SELECTION_REASON_ACTIVE_HEAD_UNAVAILABLE,   // semantic fallback head unresolvable
    DAG_TIP_SELECTION_REASON_TRAVERSAL_UNAVAILABLE,     // selected-parent / active-at-height traversal failed
    DAG_TIP_SELECTION_REASON_REVALIDATION_FAILED,       // snapshot identity changed while reading (TOCTOU)
    DAG_TIP_SELECTION_REASON_INTERNAL_FAILURE,          // defensive: internal invariant violation
    // G6: appended AT THE END (existing values, including INTERNAL_FAILURE == 17,
    // are frozen). The authoritative frontier enumeration offered a tip whose
    // canonical daglinks row does not exist: the canonical source is INCOMPLETE,
    // so the whole enumeration fails closed instead of silently dropping the
    // merge parent and publishing a reduced VALID vector.
    DAG_TIP_SELECTION_REASON_FRONTIER_ROW_ABSENT        // enumerated frontier tip with no canonical row
};

struct DagTipSelectionResult
{
    DagTipSelectionStatus status;
    DagTipSelectionReason reason;
    uint256 hash;          // SELECTED: winner hash; VALID_NO_ELIGIBLE: authoritative active head
    uint256 score;         // SELECTED: winner nDAGScore; else 0
    int height;            // winner/head height when resolvable; -1 otherwise
    bool fromActiveHeadFallback;
    std::string diagnostic;

    DagTipSelectionResult()
        : status(DAG_TIP_SELECTION_LEGACY), reason(DAG_TIP_SELECTION_REASON_NONE),
          hash(0), score(0), height(-1), fromActiveHeadFallback(false) {}

    bool IsUsable() const
    {
        return status == DAG_TIP_SELECTION_SELECTED ||
               status == DAG_TIP_SELECTION_VALID_NO_ELIGIBLE;
    }
};

// ---------------------------------------------------------------------------
// R2c.2/S7 — authoritative MERGE-PARENT result (value-only).
//
// This is a SEPARATE algorithm from the primary selector, NOT a reuse of it.
// The milestone Phase-A source freeze proved the legacy merge-parent rule
// differs from CDAGManager::SelectBestDAGTip in five material ways; the
// reduction below reproduces the MERGE-PARENT rule exactly:
//   - frontier membership (authoritative tip stream), primary exclusion,
//   - accumulated-chainTrust threshold against the current best chain
//     (`cand != bestChain && cand.nChainTrust > nBestChainTrust`),
//   - persisted canonical nDAGScore ordering (score DESC, hash ASC),
//   - DAG_MERGE_DEPTH window [primaryHeight - DAG_MERGE_DEPTH, primaryHeight],
//   - MAX_DAG_PARENTS cap counted INCLUDING the primary.
// It deliberately does NOT apply the primary selector's `retained` /
// post-DAG-PoS / active-chain-membership filters, because legacy does not.
//
// The result is VALUE-ONLY: an ordered vector of parent hashes. It never
// returns a borrowed resident CBlockIndex*, never materializes an all-frontier
// graph, and never consults mapBlockIndex / mapDAGData / setDAGTips as
// authority.
//
// VALID with parents.size() == 1 (primary only) is a legitimate
// "no extra merge parents" result and is DISTINCT from UNAVAILABLE.
// ---------------------------------------------------------------------------
enum DagMergeParentStatus
{
    DAG_MERGE_PARENT_LEGACY = 0,      // authoritative mode off; caller runs the legacy path
    DAG_MERGE_PARENT_VALID = 1,       // valid authoritative result (see above)
    DAG_MERGE_PARENT_UNAVAILABLE = 2  // fail closed; reason/diagnostic describe why
};

struct DagMergeParentResult
{
    DagMergeParentStatus status;
    DagTipSelectionReason reason;
    std::vector<uint256> parents;  // ordered: [0] == primary, then merge parents
    uint256 primaryHash;           // echo of the caller-bound primary
    int primaryHeight;             // echo of the caller-bound primary height (-1 if unknown)
    std::string diagnostic;

    DagMergeParentResult()
        : status(DAG_MERGE_PARENT_LEGACY), reason(DAG_TIP_SELECTION_REASON_NONE),
          primaryHash(0), primaryHeight(-1) {}

    bool IsUsable() const { return status == DAG_MERGE_PARENT_VALID; }
    bool HasExtraParents() const
    {
        return status == DAG_MERGE_PARENT_VALID && parents.size() > 1;
    }
};

// Value-only snapshot of the authoritative frontier TIP SET (all tips offered
// by the same enumeration + membership predicate the merge-parent reduction
// consumes, UNFILTERED and ascending). Used by the CPU-mining work identity so
// its fingerprint watches the SAME parent universe the template is built from.
struct DagFrontierTipsResult
{
    DagMergeParentStatus status;
    DagTipSelectionReason reason;
    std::vector<uint256> tips;  // ascending; every authoritative frontier tip
    std::string diagnostic;

    DagFrontierTipsResult()
        : status(DAG_MERGE_PARENT_LEGACY), reason(DAG_TIP_SELECTION_REASON_NONE) {}

    bool IsUsable() const { return status == DAG_MERGE_PARENT_VALID; }
};

// ---------------------------------------------------------------------------
// Entry points
// ---------------------------------------------------------------------------

// Internal mutation-scoped synchronous consumers. In authoritative mode the
// explicit S5 transaction-scoped preview is REQUIRED; a NULL preview fails
// closed (never falls back to the external CLEAN path or the legacy selector).
DagTipSelectionResult SelectDagTipForInternalConsumer(const DagMutationPreview* preview,
                                                      std::string* error);

// External CLEAN runtime consumers. Uses the published healthy runtime; never
// receives a mutation permit and never runs mid-envelope.
DagTipSelectionResult SelectDagTipForExternalConsumer(std::string* error);

// R2c.2/S7 — authoritative merge-parent commitment for CreateNewBlock.
//
// External CLEAN runtime consumers ONLY. These take NO DagMutationPreview
// parameter and construct the CLEAN source internally, so a transaction-scoped
// internal preview CANNOT be handed to them: an internal pre-END caller can
// never receive preview privilege through this seam.
//
// `primaryHash` / `primaryHeight` bind the result to the SAME selected primary
// the caller is building on: parents[0] is that hash and it is excluded from the
// candidate set. A caller that cannot supply a valid primary must not call this
// (the reduction is only meaningful relative to a chosen primary).
//
// UNAVAILABLE is fail closed. VALID with parents.size() == 1 means the
// authoritative state is valid and complete and there are NO eligible extra
// merge parents — a legitimate result, never conflated with UNAVAILABLE.
DagMergeParentResult SelectMergeParentsForExternalConsumer(const uint256& primaryHash,
                                                           int primaryHeight,
                                                           std::string* error);

// R2c.2/S7 — value-only snapshot of the authoritative frontier TIP SET, from the
// SAME enumeration + membership predicate the merge-parent reduction consumes.
// Used by the CPU-mining work identity so it watches the authoritative parent
// universe rather than the legacy setDAGTips enumeration. Ascending order.
DagFrontierTipsResult SelectFrontierTipsForExternalConsumer(std::string* error);

// Authoritative boundary resolution for consumers that need the active-chain
// block at a target height (finality vote boundary, epoch boundary):
//   1. by-value selected-parent walk from `fromHash` down to `targetHeight`
//      (exact legacy walk shape, visited-guarded);
//   2. fallback: authoritative active-chain block at `targetHeight`
//      (tip authority active index, then base generation active index);
//   3. both failing => UNAVAILABLE (never a resident FindBlockByHeight walk).
// `fromHash` must be an active-eligible selected winner (SELECTED /
// VALID_NO_ELIGIBLE result). Works in both internal and external contexts: it
// reads only committed persisted state + the by-value live authority.
DagTipSelectionResult ResolveAuthoritativeBoundaryAtHeight(const uint256& fromHash,
                                                           int targetHeight,
                                                           std::string* error);

// Authoritative active-chain block at a height, by value (bounded).
DagTipSelectionResult ResolveAuthoritativeActiveAtHeight(int height, std::string* error);

// ---------------------------------------------------------------------------
// Runtime registration (startup context lifecycle; mirrors the accepted S5
// registration pattern). Registration only; the selector never takes ownership.
// ---------------------------------------------------------------------------
void SetDagTipSelectorRuntime(dag_tip_frontier::DagTipOverlayRuntime* runtime);
void ClearDagTipSelectorRuntime();
bool IsDagTipSelectorRuntimeRegistered();

// ---------------------------------------------------------------------------
// Test seams (test-only; no production behavior unless armed)
// ---------------------------------------------------------------------------
struct DagTipSelectorStats
{
    uint64_t internalCalls;
    uint64_t externalCalls;
    uint64_t selected;
    uint64_t validNoEligible;
    uint64_t unavailable;
    uint64_t revalidations;
    uint64_t frontierVisits;      // frontier tips offered by the source
    uint64_t frontierEmits;       // frontier tips passing the membership predicate
    uint64_t candidateReads;
    uint64_t boundaryWalks;       // boundary resolved by the selected-parent walk
    uint64_t boundaryFallbacks;   // boundary resolved by active-at-height
    uint64_t activeHeadFallbacks; // VALID_NO_ELIGIBLE head resolutions
    DagTipSelectorStats()
        : internalCalls(0), externalCalls(0), selected(0), validNoEligible(0),
          unavailable(0), revalidations(0), frontierVisits(0), frontierEmits(0),
          candidateReads(0), boundaryWalks(0), boundaryFallbacks(0),
          activeHeadFallbacks(0) {}
};
DagTipSelectorStats GetDagTipSelectorStats();
void ResetDagTipSelectorStatsForTest();

// Fired once per selection run AFTER frontier enumeration and BEFORE snapshot
// revalidation; a test can mutate authoritative state mid-flight to prove the
// no-partial-winner / revalidation contract.
typedef void (*DagTipSelectorEnumerationHook)(void* ctx);
void SetDagTipSelectorEnumerationHookForTest(DagTipSelectorEnumerationHook hook, void* ctx);

// Force an UNAVAILABLE outcome (fault injection for consumer-behavior tests).
void SetDagTipSelectorForceUnavailableForTest(bool armed, DagTipSelectionReason reason);

// R2c.2/S7 — merge-parent reduction counters (boundedness evidence: how many
// frontier tips were offered, how many candidate metadata reads were performed,
// and why candidates were excluded). Test-only surface; never consulted by
// production logic.
struct DagMergeParentStats
{
    uint64_t calls;
    uint64_t frontierVisits;      // tips offered by the authoritative source
    uint64_t frontierEmits;       // tips passing the frontier membership predicate
    uint64_t candidateReads;      // by-value metadata reads performed
    uint64_t primaryExcluded;
    uint64_t trustExcluded;
    uint64_t unresolvableSkipped; // not resolvable by value (legacy residency-miss parity)
    uint64_t depthExcluded;
    uint64_t heightExcluded;
    uint64_t capped;              // dropped by the MAX_DAG_PARENTS cap
    uint64_t published;
    uint64_t unavailable;
    uint64_t frontierRowAbsent;   // G6: enumerated frontier tips whose canonical row is absent (fail-closed)
    DagMergeParentStats()
        : calls(0), frontierVisits(0), frontierEmits(0), candidateReads(0),
          primaryExcluded(0), trustExcluded(0), unresolvableSkipped(0),
          depthExcluded(0), heightExcluded(0), capped(0), published(0),
          unavailable(0), frontierRowAbsent(0) {}
};
DagMergeParentStats GetDagMergeParentStats();
void ResetDagMergeParentStatsForTest();

// Force the merge-parent reduction to fail closed (consumer-behavior tests).
void SetMergeParentForceUnavailableForTest(bool armed, DagTipSelectionReason reason);

// Synthetic reduction fixture: pins the reduction semantics (filters, argmax,
// tie-break, zero-score eligibility, no-partial-winner) over the SAME
// production algorithm with a fully controlled read context. Real read
// contexts are pinned separately by the production-path fixtures.
struct DagTipSyntheticCandidate
{
    uint256 hash;
    bool retained;
    bool childless;
    uint256 score;
    int height;
    bool proofOfStake;
    bool active;
    bool readFails;
    DagTipSyntheticCandidate()
        : retained(true), childless(true), score(0), height(0),
          proofOfStake(false), active(true), readFails(false) {}
};

DagTipSelectionResult SelectDagTipFromSyntheticFrontierForTest(
    const std::vector<DagTipSyntheticCandidate>& candidates,
    const uint256& activeHeadHash, int activeHeadHeight,
    bool enumerationFails, bool revalidationFails, bool headUnavailable);

const char* DagTipSelectionStatusName(DagTipSelectionStatus status);
const char* DagTipSelectionReasonName(DagTipSelectionReason reason);

#endif // INNOVA_DAG_TIP_SELECTOR_H
