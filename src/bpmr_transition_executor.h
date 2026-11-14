// BPMR/ADR-0022 rev2 — single transition executor (variant B).
//
// Contract (see ADR-0022-REV2 §1, §7):
//   * exactly ONE thread performs durable transitions (I6);
//   * submissions are bounded, with explicit backpressure (I7);
//   * density (I1) and parent identity (I2) are checked against the DURABLE tip
//     BEFORE the commit, never against an in-memory logical tip;
//   * the commit result is explicit: OK / FAILED / REJECTED_* (I8);
//   * a reorg is ONE transition (no intermediate servable state);
//   * shutdown drains the queue and reports what was committed (T6).
//
// IF01-DUR-01: the commit callback is the REAL durable path
// (BlockIndexAuthoritativeLive::AcceptActive) and the publish callback is the REAL
// servable step (SetBestChain + Store3A apply/verify). The caller releases cs_main
// around SubmitAndWait and re-acquires it after the result (§7.3): the worker takes
// cs_main itself inside the callbacks, so holding it while waiting would deadlock.

#ifndef BPMR_TRANSITION_EXECUTOR_H
#define BPMR_TRANSITION_EXECUTOR_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace bpmr
{

// [c29 C] outcome of a durable-tip bind attempt.
enum TipBindResult
{
    TB_BOUND = 0,          // this call established the bind
    TB_ALREADY_BOUND = 1,  // the same (height,id) was already bound by someone else
    TB_CONFLICT = 2        // a DIFFERENT tip is bound: caller's view is stale -> fail closed
};

struct TransitionRequest
{
    uint64_t nHeight;
    uint64_t nParentId;      // identity (hash fingerprint) of the expected parent
    uint64_t nSelfId;        // identity (hash fingerprint) of the block committed
    bool fPublish;           // true = active extension, false = side record
    bool fReorg;             // true = reorg/cutover: one fused atomic transition
    // ADR-0022 rev2 §7.4: a publishing extension that does NOT extend the durable
    // tip (I1/I2) must not be dropped and must not be treated as invalid. When
    // this flag is set the executor commits it as a durable SIDE record instead
    // (still through the single writer, I6) and publishes nothing.
    bool fSideFallback;
    // ADR-0022 rev2 / P0.3 (I6): a fused OPERATOR transition (invalidate/reconsider).
    // I1/I2 are not applicable (like fReorg): the transition re-selects the whole
    // branch under cs_main. The durable tip is bound from the COMMIT RESULT
    // (pResultHeight/pResultId), never from a prediction, so the next I1/I2 check
    // always looks at the tip that is actually durable on disk.
    bool fOperator;
    // OUT: filled by the commit callback for fOperator requests.
    int32_t* pResultHeight;
    uint64_t* pResultId;
    uint64_t nTicket;        // filled in by Submit*/RunInline (diagnostics)
    const void* pUser;       // opaque payload for CommitFn/PublishFn

    TransitionRequest()
        : nHeight(0), nParentId(0), nSelfId(0), fPublish(false), fReorg(false),
          fSideFallback(false), fOperator(false), pResultHeight(nullptr),
          pResultId(nullptr), nTicket(0), pUser(nullptr) {}
};

enum TransitionOutcome
{
    TR_COMMITTED = 0,        // durable commit confirmed, published if requested
    TR_COMMITTED_SIDE,       // refused as an active append: durably committed as a SIDE record, no publication
    TR_REJECTED_DENSITY,     // I1 violated: height is not durable tip + 1 (and no side fallback)
    TR_REJECTED_PARENT,      // I2 violated: parent identity != durable tip (and no side fallback)
    TR_FAILED_COMMIT,        // commit callback failed -> fail-closed, nothing published
    TR_FAILED_PUBLISH,       // durable commit happened, servable switch failed -> FAIL-STOP
    TR_INLINE_BUSY,          // internal: inline attempt found the worker mid-transition
    // ADR-0024 (T6) variant A: the request was still QUEUED (never started) when
    // shutdown began. Nothing was committed and nothing was published, so the
    // caller's outcome is unambiguous: the transition did NOT happen.
    TR_CANCELLED_SHUTDOWN
};

struct TransitionResult
{
    uint64_t nTicket;
    uint64_t nSelfId;
    TransitionOutcome eOutcome;
    std::string strDetail;

    TransitionResult() : nTicket(0), nSelfId(0), eOutcome(TR_COMMITTED) {}
};

const char* TransitionOutcomeName(TransitionOutcome o);

// Commit callback: MUST be the only thing that writes durable chain state.
// Returns true only when the record is durably committed. Takes cs_main itself.
typedef std::function<bool(const TransitionRequest&, std::string*)> CommitFn;
// Publish callback: runs only after a successful commit. Makes the durable tip
// servable (SetBestChain + Store3A apply) and verifies I4. Takes cs_main itself.
typedef std::function<bool(const TransitionRequest&, std::string*)> PublishFn;

class TransitionExecutor
{
public:
    TransitionExecutor(size_t nMaxQueued, CommitFn commit, PublishFn publish);
    ~TransitionExecutor();

    // Non-blocking submit. false = queue full (backpressure, I7).
    bool TrySubmit(const TransitionRequest& req);
    // Blocking submit with a bounded wait for QUEUE SPACE only. false = timed out.
    bool Submit(const TransitionRequest& req, int nTimeoutMs);
    // Blocking submit with a bounded wait for the TRANSITION RESULT. false = timed
    // out before the worker started the request (it is then abandoned and will
    // never execute); true with *out filled = the worker executed it. A request
    // that has already started is always waited for, so a timeout can never race
    // the caller into a second execution of the same transition.
    bool SubmitAndWait(const TransitionRequest& req, int nTimeoutMs,
                       TransitionResult* out);

    // Runs one transition on the CALLING thread through the same checks and the
    // same commit/publish callbacks. Same I1/I2/I6/I8 semantics. The caller must
    // NOT hold cs_main (the callbacks take it); used after a bounded submit timed
    // out and cs_main was released again.
    TransitionOutcome RunInline(const TransitionRequest& req, TransitionResult* out);

    // Inline attempt for a caller that DOES hold cs_main: tries the transition
    // mutex without blocking. *fBusy=true means the worker is mid-transition; the
    // caller must release cs_main (so the worker can finish) and retry.
    TransitionOutcome TryRunInline(const TransitionRequest& req, TransitionResult* out,
                                   bool* fBusy);

    // ADR-0024 (T6) variant A. Sets the stop flag, CANCELS every request that has
    // not started yet (they are reported as TR_CANCELLED_SHUTDOWN and never
    // executed), then waits for the request that is already in flight to finish
    // with its own unambiguous result, joins the worker and returns everything
    // recorded. A transition that has started is never abandoned: exactly-once is
    // preserved because a cancelled request never touches durable state.
    std::vector<TransitionResult> Shutdown();

    // Durable-tip binding (I1/I2 are checked against THIS, not against pindexBest).
    void InitDurableTip(uint64_t nHeight, uint64_t nId);
    // [c29 C] same guard as A2, but the outcome is reported so a caller can fail
    // closed when its own view of the durable tip contradicts the bound one.
    TipBindResult TryInitDurableTip(uint64_t nHeight, uint64_t nId);
    bool TipInitialized() const { return fTipInitialized_.load(); }

    // Snapshot of committed state (what a reader may serve, I5).
    uint64_t DurableTipHeight() const { return nDurableTipHeight_.load(); }
    uint64_t DurableTipId() const { return nDurableTipId_.load(); }
    uint64_t CommittedCount() const { return nCommitted_.load(); }
    uint64_t SideCommittedCount() const { return nSideCommitted_.load(); }
    uint64_t RejectedCount() const { return nRejected_.load(); }
    uint64_t FailedCount() const { return nFailed_.load(); }
    uint64_t PublishFailedCount() const { return nPublishFailed_.load(); }
    uint64_t SkippedCount() const { return nSkipped_.load(); }
    int AdmittedInFlight() const { return nAdmitted_; }   // diagnostics/tests
    uint64_t CancelledAtShutdownCount() const { return nCancelled_.load(); }

private:
    TransitionOutcome Process(const TransitionRequest& req, TransitionResult* out);
    void WorkerLoop();

    std::mutex csTransition_;   // serializes commit+publish across worker and inline
    mutable std::mutex csTip_;  // [c29 A1] guards the durable-tip bind

    mutable std::mutex cs_;
    std::condition_variable cv_;
    std::deque<TransitionRequest> queue_;
    std::vector<TransitionResult> results_;
    std::map<uint64_t, TransitionResult> done_;   // ticket -> result, not yet reaped
    std::set<uint64_t> started_;                  // worker began this ticket
    std::set<uint64_t> abandoned_;                // caller gave up before start
    std::atomic<uint64_t> nNextTicket_;   // atomic ticket counter (fetch_add only)
    const size_t nMaxQueued_;
    CommitFn commit_;
    PublishFn publish_;
    std::atomic<bool> fStop_;     // admission gate: set once, never cleared
    // Admission accounting: an inline transition checks fStop_ and registers in
    // the SAME critical section (csAdmit_), so Shutdown cannot close the gate
    // between the check and the commit. Shutdown waits for nAdmitted_ == 0.
    std::mutex csAdmit_;
    std::condition_variable cvAdmit_;
    int nAdmitted_ = 0;
    std::thread worker_;

    std::atomic<bool> fTipInitialized_;
    std::atomic<uint64_t> nDurableTipHeight_;
    std::atomic<uint64_t> nDurableTipId_;
    std::atomic<uint64_t> nCommitted_;
    std::atomic<uint64_t> nSideCommitted_;
    std::atomic<uint64_t> nRejected_;
    std::atomic<uint64_t> nFailed_;
    std::atomic<uint64_t> nPublishFailed_;
    std::atomic<uint64_t> nSkipped_;
    std::atomic<uint64_t> nCancelled_;
};

} // namespace bpmr

#endif // BPMR_TRANSITION_EXECUTOR_H
