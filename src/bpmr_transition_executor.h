// BPMR/ADR-0022 rev2 — single transition executor (variant B skeleton).
//
// Contract (see ADR-0022-REV2 §1):
//   * exactly ONE thread performs durable transitions (I6);
//   * submissions are bounded, with explicit backpressure (I7);
//   * density (I1) and parent identity (I2) are checked BEFORE the commit;
//   * the commit result is explicit: OK / FAILED / REJECTED_* (I8);
//   * shutdown drains the queue and reports what was committed (T6).
//
// This file is deliberately dependency-free (no cs_main, no DB): it is the
// serialization skeleton that a real commit callback plugs into.

#ifndef BPMR_TRANSITION_EXECUTOR_H
#define BPMR_TRANSITION_EXECUTOR_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace bpmr
{

struct TransitionRequest
{
    uint64_t nHeight;
    uint64_t nParentId;      // identity of the expected parent (opaque id)
    uint64_t nSelfId;        // identity of the block being committed
    bool fPublish;           // true = active extension, false = side record
};

enum TransitionOutcome
{
    TR_COMMITTED = 0,        // durable commit confirmed, published if requested
    TR_REJECTED_DENSITY,     // I1 violated: height is not prev+1
    TR_REJECTED_PARENT,      // I2 violated: parent identity mismatch
    TR_FAILED_COMMIT,        // commit callback failed -> fail-closed, nothing published
};

struct TransitionResult
{
    uint64_t nSelfId;
    TransitionOutcome eOutcome;
    std::string strDetail;
};

// Commit callback: MUST be the only thing that writes durable state.
// Returns true only when the record is durably committed.
typedef std::function<bool(const TransitionRequest&, std::string*)> CommitFn;
// Publish callback: runs only after a successful commit.
typedef std::function<void(const TransitionRequest&)> PublishFn;

class TransitionExecutor
{
public:
    TransitionExecutor(size_t nMaxQueued, CommitFn commit, PublishFn publish);
    ~TransitionExecutor();

    // Non-blocking submit. false = queue full (backpressure, I7).
    bool TrySubmit(const TransitionRequest& req);
    // Blocking submit with a bounded wait. false = timed out.
    bool Submit(const TransitionRequest& req, int nTimeoutMs);

    // Drains the queue, joins the worker, returns every result recorded.
    std::vector<TransitionResult> Shutdown();

    // Snapshot of committed state (what a reader may serve, I5).
    uint64_t DurableTipHeight() const { return nDurableTipHeight_.load(); }
    uint64_t DurableTipId() const { return nDurableTipId_.load(); }
    uint64_t CommittedCount() const { return nCommitted_.load(); }
    uint64_t RejectedCount() const { return nRejected_.load(); }
    uint64_t FailedCount() const { return nFailed_.load(); }

private:
    void WorkerLoop();

    mutable std::mutex cs_;
    std::condition_variable cv_;
    std::deque<TransitionRequest> queue_;
    std::vector<TransitionResult> results_;
    const size_t nMaxQueued_;
    CommitFn commit_;
    PublishFn publish_;
    bool fStop_;
    std::thread worker_;

    std::atomic<uint64_t> nDurableTipHeight_;
    std::atomic<uint64_t> nDurableTipId_;
    std::atomic<uint64_t> nCommitted_;
    std::atomic<uint64_t> nRejected_;
    std::atomic<uint64_t> nFailed_;
};

} // namespace bpmr

#endif // BPMR_TRANSITION_EXECUTOR_H
