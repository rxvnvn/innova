// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.
//
// R2c.2s/S5 — owned transaction-scoped preview seam implementation.
// See dag_mutation_preview.h for the full contract.

#include "dag_mutation_preview.h"

#include "main.h"
#include "txdb-leveldb.h"
#include "dag_tips_delta.h"
#include "dag_tip_overlay_runtime.h"
#include "blockindex_authoritative_startup.h"
#include "blockindex_authoritative_live.h"

#include <functional>

// ---------------------------------------------------------------------------
// File-scope ownership record. This is ownership bookkeeping only (mirrors the
// accepted delta-journal singleton pattern): it holds no authority data, no
// view data, and no retained collections. Every read resolves against the
// existing live sources.
// ---------------------------------------------------------------------------
namespace {

struct PreviewState
{
    bool rootActive;
    uint64_t boundSerial; // journal rootSerial this preview state is bound to
    uint64_t nonce;
    std::thread::id owner;
    uint256 originToken;
    uint256 committedToken;
    uint64_t generation;
    uint64_t recordsAtMark;
    bool invalidLatch;
    bool genOverrideArmed;
    uint64_t genOverride;
    bool tokenOverrideArmed;
    uint256 tokenOverride;
    DagMutationPreview::Stats stats;
    PreviewState()
        : rootActive(false), boundSerial(0), nonce(0), owner(), originToken(0), committedToken(0),
          generation(0), recordsAtMark(0), invalidLatch(false),
          genOverrideArmed(false), genOverride(0),
          tokenOverrideArmed(false), tokenOverride(0) {}
};

PreviewState g_s5;
DagMutationPreview g_s5Preview;
dag_tip_frontier::DagTipOverlayRuntime* g_s5Runtime = NULL;
DagMutationPreviewPhaseHook g_s5PhaseHook = NULL;

bool S5ReadDurableToken(uint256* out)
{
    CTxDB db("r");
    return db.ReadDAGSourceStateId(*out);
}

// Streaming context shared by the base and pending passes.
struct StreamCtx
{
    CTxDB* db;
    DagMutationPreview::TipVisitor fn;
    void* userCtx;
    bool aborted;   // visitor requested stop
    bool failed;    // internal read failure
    std::string error;
    uint64_t visits;
    uint64_t emits;
    StreamCtx() : db(NULL), fn(NULL), userCtx(NULL), aborted(false), failed(false), visits(0), emits(0) {}
};

bool S5EmitCandidate(StreamCtx* s, const uint256& hash)
{
    ++s->visits;
    bool member = false;
    if (!s->db->ReadDAGFrontierMembership(hash, &member))
    {
        s->failed = true;
        s->error = "S5 preview: frontier predicate read failed";
        return false;
    }
    if (!member) return true;
    ++s->emits;
    if (!s->fn(hash, s->userCtx))
    {
        s->aborted = true;
        return false;
    }
    return true;
}

bool S5BaseVisitor(const uint256& hash, void* vctx)
{
    return S5EmitCandidate((StreamCtx*)vctx, hash);
}

bool S5PendingVisitor(const DagTipDeltaRecord& rec, void* vctx)
{
    return S5EmitCandidate((StreamCtx*)vctx, rec.hash);
}

// Candidate metadata read without re-validating (caller must have validated).
DagMutationPreviewStatus S5ReadCandidateInternal(const uint256& hash,
                                                 DagMutationCandidateView* out,
                                                 std::string* error)
{
    if (!out)
    {
        if (error) *error = "S5 preview: null candidate view";
        return DAG_MUTATION_PREVIEW_INTERNAL_FAILURE;
    }
    *out = DagMutationCandidateView();
    out->hash = hash;

    CTxDB db("r");
    bool member = false;
    if (!db.ReadDAGFrontierMembership(hash, &member))
    {
        // Distinguishes corrupt/undecodable (fail closed) from ordinary
        // absence (member=false with success).
        if (error) *error = "S5 preview: candidate predicate read failed";
        return DAG_MUTATION_PREVIEW_SOURCE_UNAVAILABLE;
    }
    out->childless = member;

    CBlockDAGData data;
    if (db.ReadDAGLinks(hash, data))
    {
        out->retained = true;
        out->hasFullField = true;
        out->nDAGScore = data.nDAGScore;
        out->fBlue = data.fBlue;
        out->nInferredK = data.nInferredK;
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
            // Not yet published to the live authority (e.g. the block being
            // added before SetBestChain/publication): remains unresolved and
            // therefore ineligible for active-chain selection, exactly like
            // the legacy selector which skips blocks above the current best.
            out->height = -1;
            out->active = false;
        }
        else
        {
            if (error) *error = "S5 preview: candidate authority resolution failed";
            return DAG_MUTATION_PREVIEW_SOURCE_UNAVAILABLE;
        }
    }
    return DAG_MUTATION_PREVIEW_OK;
}

// Resolver visitor state: bounded winner-only reduction.
struct ResolveCtx
{
    bool found;
    uint256 bestHash;
    uint256 bestScore;
    bool failed;
    std::string error;
    ResolveCtx() : found(false), bestHash(0), bestScore(0), failed(false) {}
};

bool S5ResolveVisitor(const uint256& tip, void* vctx)
{
    ResolveCtx* r = (ResolveCtx*)vctx;
    DagMutationCandidateView view;
    std::string e;
    DagMutationPreviewStatus st = S5ReadCandidateInternal(tip, &view, &e);
    if (st != DAG_MUTATION_PREVIEW_OK)
    {
        r->failed = true;
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
    }
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Test observation counters (file scope; NOT in an anonymous namespace).
// ---------------------------------------------------------------------------
int g_testS5ConsumerValidationsEpoch = 0;
int g_testS5ConsumerValidationsReorder = 0;
int g_testS5ConsumerValidationsOrder = 0;
int g_testS5LastConsumerValidationStatus = 0;
uint64_t g_testS5LastConsumerPreviewNonce = 0;
const void* g_testS5LastConsumerPreviewPtr = NULL;
int g_testS5NestedBorrowCalls = 0;

// ---------------------------------------------------------------------------
// Status names
// ---------------------------------------------------------------------------
const char* DagMutationPreviewStatusName(DagMutationPreviewStatus status)
{
    switch (status)
    {
    case DAG_MUTATION_PREVIEW_OK: return "OK";
    case DAG_MUTATION_PREVIEW_NO_ACTIVE_ROOT: return "NO_ACTIVE_ROOT";
    case DAG_MUTATION_PREVIEW_INVALIDATED: return "INVALIDATED";
    case DAG_MUTATION_PREVIEW_WRONG_THREAD: return "WRONG_THREAD";
    case DAG_MUTATION_PREVIEW_GENERATION_MISMATCH: return "GENERATION_MISMATCH";
    case DAG_MUTATION_PREVIEW_STALE_TOKEN: return "STALE_TOKEN";
    case DAG_MUTATION_PREVIEW_INCOMPLETE_PREFIX: return "INCOMPLETE_PREFIX";
    case DAG_MUTATION_PREVIEW_SOURCE_UNAVAILABLE: return "SOURCE_UNAVAILABLE";
    case DAG_MUTATION_PREVIEW_SOURCE_UNHEALTHY: return "SOURCE_UNHEALTHY";
    case DAG_MUTATION_PREVIEW_BASE_UNAVAILABLE: return "BASE_UNAVAILABLE";
    case DAG_MUTATION_PREVIEW_INTERNAL_FAILURE: return "INTERNAL_FAILURE";
    }
    return "UNKNOWN";
}

// ---------------------------------------------------------------------------
// DagMutationPreview
// ---------------------------------------------------------------------------
DagMutationPreview::DagMutationPreview() {}
DagMutationPreview::~DagMutationPreview() {}

bool DagMutationPreview::IsActive() const
{
    DagTipDeltaState js = GetDagTipDeltaState();
    return g_s5.rootActive && js.active && !g_s5.invalidLatch && !js.failureLatched;
}

uint64_t DagMutationPreview::Nonce() const { return g_s5.nonce; }

bool DagMutationPreview::IsOwnerThread() const
{
    return std::this_thread::get_id() == g_s5.owner;
}

bool DagMutationPreview::ValidatePermitNonce(uint64_t nonce, std::string* error) const
{
    if (!g_s5.rootActive || !GetDagTipDeltaState().active)
    {
        if (error) *error = "S5 preview: no active root";
        return false;
    }
    if (nonce != g_s5.nonce)
    {
        if (error) *error = "S5 preview: stale permit nonce";
        return false;
    }
    if (error) error->clear();
    return true;
}

bool DagMutationPreview::GetOriginToken(uint256* out) const
{
    if (!out || !g_s5.rootActive) return false;
    *out = g_s5.originToken;
    return true;
}

bool DagMutationPreview::GetCommittedPrefixToken(uint256* out) const
{
    if (!out || !g_s5.rootActive) return false;
    *out = g_s5.committedToken;
    return true;
}

bool DagMutationPreview::GetGeneration(uint64_t* out) const
{
    if (!out || !g_s5.rootActive) return false;
    *out = g_s5.generation;
    return true;
}

bool DagMutationPreview::GetOwnerThreadHash(uint64_t* out) const
{
    if (!out) return false;
    *out = (uint64_t)std::hash<std::thread::id>()(g_s5.owner);
    return true;
}

DagMutationPreviewStatus DagMutationPreview::Validate(std::string* error) const
{
    DagTipDeltaState js = GetDagTipDeltaState();
    if (!g_s5.rootActive || !js.active)
    {
        if (error) *error = "S5 preview: no active root mutation";
        return DAG_MUTATION_PREVIEW_NO_ACTIVE_ROOT;
    }
    if (g_s5.invalidLatch || js.failureLatched)
    {
        if (error) *error = "S5 preview: root mutation invalidated";
        return DAG_MUTATION_PREVIEW_INVALIDATED;
    }
    if (std::this_thread::get_id() != g_s5.owner)
    {
        if (error) *error = "S5 preview: read from non-owner thread";
        return DAG_MUTATION_PREVIEW_WRONG_THREAD;
    }
    ++g_s5.stats.validateCalls;
    const uint64_t gen = g_s5.genOverrideArmed ? g_s5.genOverride : AuthoritativeGeneration();
    if (gen != g_s5.generation)
    {
        if (error) *error = "S5 preview: generation mismatch";
        return DAG_MUTATION_PREVIEW_GENERATION_MISMATCH;
    }
    uint256 durable;
    if (!S5ReadDurableToken(&durable))
    {
        if (error) *error = "S5 preview: durable source token unavailable";
        return DAG_MUTATION_PREVIEW_SOURCE_UNAVAILABLE;
    }
    const uint256 expect = g_s5.tokenOverrideArmed ? g_s5.tokenOverride : g_s5.committedToken;
    if (durable != expect)
    {
        if (error) *error = "S5 preview: committed prefix token is stale";
        return DAG_MUTATION_PREVIEW_STALE_TOKEN;
    }
    if (js.logicalRecords != g_s5.recordsAtMark)
    {
        if (error) *error = "S5 preview: staged-but-uncommitted pending prefix present";
        return DAG_MUTATION_PREVIEW_INCOMPLETE_PREFIX;
    }
    {
        CTxDB db("r");
        std::string e;
        if (!db.IsDAGChildCountIndexHealthy(&e))
        {
            if (error) *error = "S5 preview: child-count authority unhealthy: " + e;
            return DAG_MUTATION_PREVIEW_SOURCE_UNHEALTHY;
        }
        if (!db.IsDAGScoreAuthorityHealthy(&e))
        {
            if (error) *error = "S5 preview: score authority unhealthy: " + e;
            return DAG_MUTATION_PREVIEW_SOURCE_UNHEALTHY;
        }
    }
    if (error) error->clear();
    return DAG_MUTATION_PREVIEW_OK;
}

DagMutationPreviewStatus DagMutationPreview::CurrentSourceStateId(uint256* out, std::string* error) const
{
    if (!out)
    {
        if (error) *error = "S5 preview: null output";
        return DAG_MUTATION_PREVIEW_INTERNAL_FAILURE;
    }
    DagMutationPreviewStatus st = Validate(error);
    if (st != DAG_MUTATION_PREVIEW_OK) return st;
    uint256 durable;
    if (!S5ReadDurableToken(&durable))
    {
        if (error) *error = "S5 preview: durable source token unavailable";
        return DAG_MUTATION_PREVIEW_SOURCE_UNAVAILABLE;
    }
    *out = durable;
    return DAG_MUTATION_PREVIEW_OK;
}

// Shared streaming pass (caller must have validated). Base first, then the
// committed pending prefix; both filtered by the canonical predicate.
static DagMutationPreviewStatus S5StreamCurrentTips(StreamCtx* s, std::string* error)
{
    // --- certified base: the overlay must be CLEAN at the origin token ---
    dag_tip_frontier::DagTipOverlayRuntime* rt = g_s5Runtime;
    if (!rt)
    {
        if (error) *error = "S5 preview: overlay runtime not registered";
        return DAG_MUTATION_PREVIEW_BASE_UNAVAILABLE;
    }
    dag_tip_frontier::LiveTipFrontierOverlay* ov = rt->Overlay();
    if (!ov)
    {
        if (error) *error = "S5 preview: overlay unavailable";
        return DAG_MUTATION_PREVIEW_BASE_UNAVAILABLE;
    }
    dag_tip_frontier::LiveTipOverlayCheckpoint cp;
    std::string e;
    if (!ov->ReadCheckpoint(&cp, &e) || cp.phase != dag_tip_frontier::LIVE_OVERLAY_PHASE_CLEAN ||
        !ov->IsImmutableBindingValid(cp) || cp.appliedSourceStateId != g_s5.originToken)
    {
        if (error) *error = "S5 preview: base not certified CLEAN at origin token";
        return DAG_MUTATION_PREVIEW_BASE_UNAVAILABLE;
    }
    if (!ov->ForEachTip(&S5BaseVisitor, s, &e))
    {
        if (s->failed)
        {
            if (error) *error = s->error;
            return DAG_MUTATION_PREVIEW_SOURCE_UNAVAILABLE;
        }
        if (!s->aborted)
        {
            if (error) *error = "S5 preview: base iteration failed";
            return DAG_MUTATION_PREVIEW_BASE_UNAVAILABLE;
        }
        return DAG_MUTATION_PREVIEW_OK; // visitor stop honored
    }
    if (s->aborted) return DAG_MUTATION_PREVIEW_OK; // visitor stop honored (base pass)

    // --- committed pending prefix: read-only journal iteration ---
    if (!ForEachDagTipDeltaRecord(&S5PendingVisitor, s, &e))
    {
        if (s->failed)
        {
            if (error) *error = s->error;
            return DAG_MUTATION_PREVIEW_SOURCE_UNAVAILABLE;
        }
        if (s->aborted) return DAG_MUTATION_PREVIEW_OK;
        DagTipDeltaState js = GetDagTipDeltaState();
        if (!js.active)
        {
            if (error) *error = "S5 preview: root mutation ended during read";
            return DAG_MUTATION_PREVIEW_NO_ACTIVE_ROOT;
        }
        if (js.failureLatched)
        {
            if (error) *error = "S5 preview: journal invalid";
            return DAG_MUTATION_PREVIEW_INVALIDATED;
        }
        if (error) *error = e.empty() ? "S5 preview: pending prefix read failed" : e;
        return DAG_MUTATION_PREVIEW_INTERNAL_FAILURE;
    }
    return DAG_MUTATION_PREVIEW_OK;
}

DagMutationPreviewStatus DagMutationPreview::ForEachCurrentTip(TipVisitor fn, void* ctx, std::string* error) const
{
    if (!fn)
    {
        if (error) *error = "S5 preview: null visitor";
        return DAG_MUTATION_PREVIEW_INTERNAL_FAILURE;
    }
    DagMutationPreviewStatus st = Validate(error);
    if (st != DAG_MUTATION_PREVIEW_OK) return st;
    CTxDB db("r");
    StreamCtx s;
    s.db = &db;
    s.fn = fn;
    s.userCtx = ctx;
    st = S5StreamCurrentTips(&s, error);
    g_s5.stats.tipVisits += s.visits;
    g_s5.stats.tipEmits += s.emits;
    return st;
}

DagMutationPreviewStatus DagMutationPreview::ReadCandidate(const uint256& hash,
                                                           DagMutationCandidateView* out,
                                                           std::string* error) const
{
    DagMutationPreviewStatus st = Validate(error);
    if (st != DAG_MUTATION_PREVIEW_OK) return st;
    ++g_s5.stats.candidateReads;
    return S5ReadCandidateInternal(hash, out, error);
}

DagMutationPreviewStatus DagMutationPreview::ResolveBestTip(bool* found, uint256* bestHash,
                                                            uint256* bestScore, std::string* error) const
{
    if (!found || !bestHash || !bestScore)
    {
        if (error) *error = "S5 preview: null resolver output";
        return DAG_MUTATION_PREVIEW_INTERNAL_FAILURE;
    }
    *found = false;
    *bestHash = uint256(0);
    *bestScore = uint256(0);
    DagMutationPreviewStatus st = Validate(error);
    if (st != DAG_MUTATION_PREVIEW_OK) return st;
    ++g_s5.stats.resolveCalls;
    ResolveCtx r;
    CTxDB db("r");
    StreamCtx s;
    s.db = &db;
    s.fn = &S5ResolveVisitor;
    s.userCtx = &r;
    st = S5StreamCurrentTips(&s, error);
    g_s5.stats.tipVisits += s.visits;
    g_s5.stats.tipEmits += s.emits;
    if (st != DAG_MUTATION_PREVIEW_OK) return st;
    if (r.failed)
    {
        if (error) *error = r.error;
        return DAG_MUTATION_PREVIEW_SOURCE_UNAVAILABLE;
    }
    if (r.found)
    {
        *found = true;
        *bestHash = r.bestHash;
        *bestScore = r.bestScore;
    }
    return DAG_MUTATION_PREVIEW_OK;
}

DagMutationPreview::Stats DagMutationPreview::GetStats() const { return g_s5.stats; }

// ---------------------------------------------------------------------------
// Root lifecycle
// ---------------------------------------------------------------------------
DagMutationPreview* BeginDagMutationPreviewRoot(std::string* error)
{
    DagTipDeltaState js = GetDagTipDeltaState();
    if (!js.active)
    {
        if (error) *error = "S5 preview: no active root journal";
        return NULL;
    }
    if (g_s5.rootActive && js.active && js.rootSerial == g_s5.boundSerial)
    {
        // A genuinely live root already owns the preview; a second root must
        // not mint a competing ownership domain. A finished root leaves
        // rootActive set but its serial is stale; a fresh root serial below
        // re-binds the preview.
        if (error) *error = "S5 preview: root preview already active";
        return NULL;
    }
    uint256 origin;
    if (!S5ReadDurableToken(&origin))
    {
        if (error) *error = "S5 preview: origin token unavailable";
        return NULL;
    }
    ++g_s5.nonce;
    g_s5.owner = std::this_thread::get_id();
    g_s5.originToken = origin;
    g_s5.committedToken = origin;
    g_s5.recordsAtMark = js.logicalRecords;
    g_s5.boundSerial = js.rootSerial;
    g_s5.generation = AuthoritativeGeneration();
    g_s5.invalidLatch = false;
    g_s5.genOverrideArmed = false;
    g_s5.tokenOverrideArmed = false;
    g_s5.stats = DagMutationPreview::Stats();
    g_s5.rootActive = true;
    if (error) error->clear();
    return &g_s5Preview;
}

DagMutationPreview* BorrowDagMutationPreview()
{
    if (!g_s5.rootActive || !GetDagTipDeltaState().active) return NULL;
    ++g_s5.stats.borrows;
    ++g_testS5NestedBorrowCalls;
    return &g_s5Preview;
}

void MarkDagMutationCommittedPrefix()
{
    if (!g_s5.rootActive) return;
    DagTipDeltaState js = GetDagTipDeltaState();
    if (!js.active) return;
    uint256 durable;
    if (!S5ReadDurableToken(&durable))
    {
        g_s5.invalidLatch = true;
        return;
    }
    g_s5.committedToken = durable;
    g_s5.recordsAtMark = js.logicalRecords;
}

DagMutationPreview* GetActiveDagMutationPreview()
{
    if (!g_s5.rootActive || !GetDagTipDeltaState().active) return NULL;
    return &g_s5Preview;
}

bool HasActiveDagMutationPreview()
{
    return GetActiveDagMutationPreview() != NULL;
}

DagMutationPreviewStatus ValidateDagMutationPreviewForConsumer(
    const DagMutationPreview* preview, int consumerId, std::string* error)
{
    if (!preview)
    {
        // Legacy path: no seam involvement.
        if (error) error->clear();
        return DAG_MUTATION_PREVIEW_OK;
    }
    std::string e;
    DagMutationPreviewStatus st = preview->Validate(&e);
    g_testS5LastConsumerValidationStatus = (int)st;
    g_testS5LastConsumerPreviewNonce = preview->Nonce();
    g_testS5LastConsumerPreviewPtr = (const void*)preview;
    if (consumerId == DAG_MUTATION_PREVIEW_CONSUMER_EPOCH) ++g_testS5ConsumerValidationsEpoch;
    else if (consumerId == DAG_MUTATION_PREVIEW_CONSUMER_REORDER) ++g_testS5ConsumerValidationsReorder;
    else if (consumerId == DAG_MUTATION_PREVIEW_CONSUMER_ORDER) ++g_testS5ConsumerValidationsOrder;
    if (st != DAG_MUTATION_PREVIEW_OK)
    {
        if (error) *error = e.empty() ? DagMutationPreviewStatusName(st) : e;
        return st;
    }
    if (error) error->clear();
    return DAG_MUTATION_PREVIEW_OK;
}

// ---------------------------------------------------------------------------
// Production runtime registration + test seams
// ---------------------------------------------------------------------------
void SetDagMutationPreviewRuntime(dag_tip_frontier::DagTipOverlayRuntime* runtime)
{
    g_s5Runtime = runtime;
}

void ClearDagMutationPreviewRuntime()
{
    g_s5Runtime = NULL;
}

void SetDagMutationPreviewPhaseHookForTest(DagMutationPreviewPhaseHook hook)
{
    g_s5PhaseHook = hook;
}

void CallDagMutationPreviewPhaseHook(const char* phase)
{
    if (g_s5PhaseHook) g_s5PhaseHook(phase ? phase : "");
}

void SetDagMutationPreviewGenerationForTest(uint64_t generation)
{
    if (!g_s5.rootActive) return;
    g_s5.genOverrideArmed = true;
    g_s5.genOverride = generation;
}

void SetDagMutationPreviewCommittedTokenForTest(const uint256& token)
{
    if (!g_s5.rootActive) return;
    g_s5.tokenOverrideArmed = true;
    g_s5.tokenOverride = token;
}

void ResetDagMutationPreviewForTest()
{
    g_s5.rootActive = false;
    g_s5.invalidLatch = false;
    g_s5.genOverrideArmed = false;
    g_s5.tokenOverrideArmed = false;
    g_s5PhaseHook = NULL;
}
