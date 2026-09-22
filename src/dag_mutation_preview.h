// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.
//
// R2c.2s/S5 — owned transaction-scoped preview seam (Option B) for internal
// synchronous consumers.
//
// During an authoritative mutation envelope the external CLEAN overlay is
// intentionally stale until the mutation is sealed (delivery happens at the
// root Commit). Internal synchronous consumers (ComputeEpochState,
// RebuildDAGOrder, RebuildDAGOrderIncremental) execute inside the same
// envelope and must be able to observe the CURRENT authoritative
// transaction-scoped view: the certified base at the origin token overlaid by
// the complete committed pending prefix, filtered through the stable canonical
// frontier predicate, with candidate metadata resolved by value from the
// authoritative source and both authority certificates bound to the committed
// prefix token.
//
// Contract (accepted Option-B model):
//   - The preview is a READ capability, never an authority and never a cache.
//     It owns no retained collections; every read goes to the existing live
//     sources (overlay base, delta journal, keyed LevelDB reads, live
//     authority). AUTHORITY != MATERIALIZATION != RESIDENCY LIFETIME.
//   - Ownership is explicit: the ROOT mutation envelope (the one whose
//     BeginDagTipDeltaTransaction returned true) creates the preview; nested
//     reorg/prune scopes borrow the SAME root preview; there is exactly one
//     root mutation ownership domain. External callers (miner/finality) never
//     receive it — the seam is unreachable without the explicit pointer.
//   - Fail closed: no active root, invalidated root, wrong thread, stale
//     generation, stale token, incomplete (staged-but-uncommitted) pending
//     prefix, or lost source health all refuse the read. No fallback to
//     legacy resident objects is permitted.
//   - The committed-prefix contract: the view never includes uncommitted
//     staged source changes (keyed reads bypass open batches), every pending
//     journal record must be backed by a completed physical source commit
//     (records-since-last-mark == 0), and the durable token must equal the
//     recorded committed prefix.
//   - Lock order cs_main -> cs_dag is preserved: the seam acquires no cs_main
//     and no new lock; reads are keyed/streaming only.
//   - S5 scope: this seam does NOT cut over selection. Consumers receive the
//     preview by explicit parameter and validate it (fail closed); their
//     selection output remains the accepted legacy behavior until the
//     separately authorized selector cutover.

#ifndef INNOVA_DAG_MUTATION_PREVIEW_H
#define INNOVA_DAG_MUTATION_PREVIEW_H

#include "uint256.h"

#include <stdint.h>
#include <string>
#include <thread>

namespace dag_tip_frontier { class DagTipOverlayRuntime; }

enum DagMutationPreviewStatus
{
    DAG_MUTATION_PREVIEW_OK = 0,
    DAG_MUTATION_PREVIEW_NO_ACTIVE_ROOT,
    DAG_MUTATION_PREVIEW_INVALIDATED,
    DAG_MUTATION_PREVIEW_WRONG_THREAD,
    DAG_MUTATION_PREVIEW_GENERATION_MISMATCH,
    DAG_MUTATION_PREVIEW_STALE_TOKEN,
    DAG_MUTATION_PREVIEW_INCOMPLETE_PREFIX,
    DAG_MUTATION_PREVIEW_SOURCE_UNAVAILABLE,
    DAG_MUTATION_PREVIEW_SOURCE_UNHEALTHY,
    DAG_MUTATION_PREVIEW_BASE_UNAVAILABLE,
    DAG_MUTATION_PREVIEW_INTERNAL_FAILURE
};

const char* DagMutationPreviewStatusName(DagMutationPreviewStatus status);

// Consumer identities for the explicit internal threading (diagnostics/tests).
enum DagMutationPreviewConsumer
{
    DAG_MUTATION_PREVIEW_CONSUMER_EPOCH = 1,    // ComputeEpochState
    DAG_MUTATION_PREVIEW_CONSUMER_REORDER = 2,  // RebuildDAGOrderIncremental
    DAG_MUTATION_PREVIEW_CONSUMER_ORDER = 3     // RebuildDAGOrder
};

// By-value candidate metadata at the committed prefix token. `retained` and
// `childless` come from the canonical predicate (daglinks presence + keyed
// child count); `hasFullField` means a persisted canonical full-field record
// exists (score authority health is enforced by the read's validation);
// height/proofOfStake/active come from the authoritative by-value live
// authority (height -1 = not resolved).
struct DagMutationCandidateView
{
    uint256 hash;
    bool retained;
    bool childless;
    bool hasFullField;
    uint256 nDAGScore;
    bool fBlue;
    int nInferredK;
    int height;
    bool proofOfStake;
    bool active;
    DagMutationCandidateView()
        : hash(0), retained(false), childless(false), hasFullField(false),
          nDAGScore(0), fBlue(false), nInferredK(-1), height(-1),
          proofOfStake(false), active(false) {}
};

class DagMutationPreview
{
public:
    DagMutationPreview();
    ~DagMutationPreview();
    DagMutationPreview(const DagMutationPreview&) = delete;
    DagMutationPreview& operator=(const DagMutationPreview&) = delete;

    // ---- ownership / permit surface (cheap, no DB reads) ----
    bool IsActive() const;
    uint64_t Nonce() const;
    bool IsOwnerThread() const;
    // Nonce identity check for stale-permit detection: a permit captured from a
    // previous root must not be considered current.
    bool ValidatePermitNonce(uint64_t nonce, std::string* error) const;
    bool GetOriginToken(uint256* out) const;
    bool GetCommittedPrefixToken(uint256* out) const;
    bool GetGeneration(uint64_t* out) const;
    // Owner thread identity hashed for diagnostics.
    bool GetOwnerThreadHash(uint64_t* out) const;

    // ---- full fail-closed validation (pure check; no side effects) ----
    DagMutationPreviewStatus Validate(std::string* error) const;

    // ---- view reads (each validates first; all read-only, streaming) ----
    // Current committed source token; equals the recorded committed prefix.
    DagMutationPreviewStatus CurrentSourceStateId(uint256* out, std::string* error) const;
    // Streaming current frontier: certified base (CLEAN at the origin token)
    // overlaid with the complete committed pending prefix, both filtered by the
    // stable canonical predicate at the committed prefix. Duplicate visits are
    // possible (base hash also present in the pending prefix) and are harmless
    // only for idempotent reductions; unique-enumeration consumers need their
    // own bounded deduplication.
    typedef bool (*TipVisitor)(const uint256& tip, void* ctx);
    DagMutationPreviewStatus ForEachCurrentTip(TipVisitor fn, void* ctx, std::string* error) const;
    // Keyed authoritative candidate metadata at the committed prefix.
    DagMutationPreviewStatus ReadCandidate(const uint256& hash, DagMutationCandidateView* out, std::string* error) const;
    // Bounded authoritative best-tip resolution over the current frontier with
    // the historical selector semantics: exclude post-DAG PoS, require active
    // eligibility, maximize persisted nDAGScore, smaller-hash tie, zero score
    // allowed. No eligible winner => found=false (the caller owns fallback
    // policy; this resolver never falls back to legacy residency). Retains
    // bounded winner-only state.
    DagMutationPreviewStatus ResolveBestTip(bool* found, uint256* bestHash, uint256* bestScore, std::string* error) const;

    struct Stats
    {
        uint64_t validateCalls, tipVisits, tipEmits, candidateReads, resolveCalls, borrows;
        Stats() : validateCalls(0), tipVisits(0), tipEmits(0), candidateReads(0), resolveCalls(0), borrows(0) {}
    };
    Stats GetStats() const;

private:
    // No per-object data: the seam state is the single root ownership record
    // owned by this module (mirrors the accepted delta-journal singleton
    // pattern; ownership bookkeeping only, no authority, no retained data).
};

// ---- root lifecycle (called only by the mutation envelopes) ----
// Root creation. Called immediately after BeginDagTipDeltaTransaction returned
// true. Reads the origin token (durable token at envelope start) and the
// authoritative generation itself. Returns NULL (fail closed) when no root
// journal is active or the origin token cannot be read.
DagMutationPreview* BeginDagMutationPreviewRoot(std::string* error);
// Nested borrow. Returns the root preview while a root is active, NULL
// otherwise. No independent nested preview can exist.
DagMutationPreview* BorrowDagMutationPreview();
// Record a completed physical source commit of the envelope: re-reads the
// durable token and the journal record count. No-op when no root is active.
// A failed read latches the preview invalid (fail closed).
void MarkDagMutationCommittedPrefix();
// Active-root accessor for the OWNING envelope (explicit threading only).
// External consumers never call this.
DagMutationPreview* GetActiveDagMutationPreview();
bool HasActiveDagMutationPreview();

// Consumer-side explicit validation: returns OK when preview==NULL (legacy
// path, no seam involvement); otherwise validates and records the observation
// for tests. On failure the caller must fail closed.
DagMutationPreviewStatus ValidateDagMutationPreviewForConsumer(
    const DagMutationPreview* preview, int consumerId, std::string* error);

// Production runtime registration (mirrors the committed-delta observer
// registration). The runtime pointer is the certified base source; unset means
// base reads fail closed (BASE_UNAVAILABLE). Cleared by context teardown.
void SetDagMutationPreviewRuntime(dag_tip_frontier::DagTipOverlayRuntime* runtime);
void ClearDagMutationPreviewRuntime();

// ---- test seams (inert in production; default NULL/off) ----
typedef void (*DagMutationPreviewPhaseHook)(const char* phase);
void SetDagMutationPreviewPhaseHookForTest(DagMutationPreviewPhaseHook hook);
// Called at the envelope phase points; no-op when unset.
void CallDagMutationPreviewPhaseHook(const char* phase);
// Injected staleness (armed only while a root is active; cleared at begin).
void SetDagMutationPreviewGenerationForTest(uint64_t generation);
void SetDagMutationPreviewCommittedTokenForTest(const uint256& token);
// Context-teardown reset: drops root ownership state and the phase hook.
void ResetDagMutationPreviewForTest();

// Test observation counters (file scope, not anonymous namespace).
extern int g_testS5ConsumerValidationsEpoch;
extern int g_testS5ConsumerValidationsReorder;
extern int g_testS5ConsumerValidationsOrder;
extern int g_testS5LastConsumerValidationStatus;
extern uint64_t g_testS5LastConsumerPreviewNonce;
extern const void* g_testS5LastConsumerPreviewPtr;
extern int g_testS5NestedBorrowCalls;

#endif // INNOVA_DAG_MUTATION_PREVIEW_H
