#include "bpmr_transition_executor.h"

#include <chrono>
#include <cstdio>

namespace bpmr
{

const char* TransitionOutcomeName(TransitionOutcome o)
{
    switch (o)
    {
    case TR_COMMITTED: return "COMMITTED";
    case TR_COMMITTED_SIDE: return "COMMITTED_SIDE";
    case TR_REJECTED_DENSITY: return "REJECTED_DENSITY";
    case TR_REJECTED_PARENT: return "REJECTED_PARENT";
    case TR_FAILED_COMMIT: return "FAILED_COMMIT";
    case TR_FAILED_PUBLISH: return "FAILED_PUBLISH";
    case TR_INLINE_BUSY: return "INLINE_BUSY";
    case TR_CANCELLED_SHUTDOWN: return "CANCELLED_SHUTDOWN";
    }
    return "UNKNOWN";
}

TransitionExecutor::TransitionExecutor(size_t nMaxQueued, CommitFn commit,
                                       PublishFn publish)
    : nNextTicket_(0), nMaxQueued_(nMaxQueued), commit_(commit), publish_(publish),
      fStop_(false), fTipInitialized_(false), nDurableTipHeight_(0),
      nDurableTipId_(0), nCommitted_(0), nSideCommitted_(0), nRejected_(0),
      nFailed_(0), nPublishFailed_(0), nSkipped_(0), nCancelled_(0)
{
    worker_ = std::thread(&TransitionExecutor::WorkerLoop, this);
}

TransitionExecutor::~TransitionExecutor()
{
    Shutdown();
}

void TransitionExecutor::InitDurableTip(uint64_t nHeight, uint64_t nId)
{
    TryInitDurableTip(nHeight, nId);
}

// [c29 C] A1/A2 guard plus an explicit verdict: a contradiction is reported, never
// silently dropped, and never applied.
TipBindResult TransitionExecutor::TryInitDurableTip(uint64_t nHeight, uint64_t nId)
{
    std::lock_guard<std::mutex> tipLock(csTip_);
    // ADR-0030 A: after shutdown the durable store is flushed and closed, so a
    // late first-bind must not write anything. Refused, never applied.
    if (fStop_)
    {
        printf("BPM_DURABLE_TIP_BIND %s height=%llu requested_height=%llu "
               "(refused: admission closed at shutdown)\n",
               "TB_STOPPED", (unsigned long long)nDurableTipHeight_.load(),
               (unsigned long long)nHeight);
        fflush(stdout);
        return TB_STOPPED;
    }
    if (fTipInitialized_.load())
    {
        const bool fSame = (nDurableTipHeight_.load() == nHeight &&
                            nDurableTipId_.load() == nId);
        if (fSame)
            return TB_ALREADY_BOUND;
        printf("BPM_DURABLE_TIP_BIND_CONFLICT bound_height=%llu bound_id=%016llx "
               "requested_height=%llu requested_id=%016llx (refused, fail-closed)\n",
               (unsigned long long)nDurableTipHeight_.load(),
               (unsigned long long)nDurableTipId_.load(),
               (unsigned long long)nHeight, (unsigned long long)nId);
        fflush(stdout);
        return TB_CONFLICT;
    }
    nDurableTipHeight_.store(nHeight);
    nDurableTipId_.store(nId);
    fTipInitialized_.store(true);
    return TB_BOUND;
}

bool TransitionExecutor::TrySubmit(const TransitionRequest& reqIn)
{
    TransitionRequest req = reqIn;
    req.nTicket = nNextTicket_.fetch_add(1) + 1;
    {
        std::lock_guard<std::mutex> lock(cs_);
        if (fStop_)
            return false;
        if (queue_.size() >= nMaxQueued_)
            return false;  // backpressure: the caller must slow down (I7)
        queue_.push_back(req);
    }
    cv_.notify_one();
    return true;
}

bool TransitionExecutor::Submit(const TransitionRequest& reqIn, int nTimeoutMs)
{
    TransitionRequest req = reqIn;
    req.nTicket = nNextTicket_.fetch_add(1) + 1;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(nTimeoutMs);
    std::unique_lock<std::mutex> lock(cs_);
    while (true)
    {
        if (fStop_)
            return false;
        if (queue_.size() < nMaxQueued_)
        {
            queue_.push_back(req);
            lock.unlock();
            cv_.notify_one();
            return true;
        }
        if (cv_.wait_until(lock, deadline) == std::cv_status::timeout)
            return false;
    }
}

bool TransitionExecutor::SubmitAndWait(const TransitionRequest& reqIn,
                                       int nTimeoutMs, TransitionResult* out)
{
    TransitionRequest req = reqIn;
    req.nTicket = nNextTicket_.fetch_add(1) + 1;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(nTimeoutMs);

    {
        std::unique_lock<std::mutex> lock(cs_);
        while (true)
        {
            if (fStop_)
                return false;
            if (queue_.size() < nMaxQueued_)
            {
                queue_.push_back(req);
                break;
            }
            if (cv_.wait_until(lock, deadline) == std::cv_status::timeout)
                return false;
        }
    }
    cv_.notify_one();

    std::unique_lock<std::mutex> lock(cs_);
    while (true)
    {
        std::map<uint64_t, TransitionResult>::iterator it = done_.find(req.nTicket);
        if (it != done_.end())
        {
            if (out) *out = it->second;
            done_.erase(it);
            return true;
        }
        // ADR-0024 (T6): during shutdown every ticket gets a definitive result -
        // cancelled (never executed) or executed by the worker. Returning false
        // here just because fStop_ became true would be ambiguous for a request
        // that had already STARTED (the caller could re-run it -> double commit),
        // so the wait continues until done_ holds this ticket. Shutdown() always
        // populates done_ for both cases, therefore this cannot hang.
        // false from SubmitAndWait means ONE thing only: this ticket was never
        // enqueued, so the caller may run the same transition inline (exactly
        // once). Reaching fStop_ here is impossible while the ticket is not in
        // done_: Shutdown() sets fStop_ and fills done_ for every queued ticket
        // inside a single cs_ critical section, and done_ (line above) is checked
        // before fStop_. So this branch cannot return for a ticket the executor
        // owns; it is kept as a fail-closed guard, not as an inline permission:
        // an enqueued ticket must always be resolved through done_.
        if (fStop_ && started_.count(req.nTicket) == 0)
        {
            // Fail-closed: an enqueued ticket is always resolved through done_.
            cv_.wait(lock, [this, &req] { return done_.count(req.nTicket) != 0; });
            continue;
        }
        if (cv_.wait_until(lock, deadline) == std::cv_status::timeout)
        {
            // A request that has already STARTED is always waited for: abandoning
            // it here would let the synchronous fallback execute the same
            // transition a second time. Only a not-yet-started request may be
            // abandoned (the worker then skips it without touching any state).
            if (started_.count(req.nTicket) != 0)
            {
                cv_.wait(lock, [this, &req] {
                    return done_.count(req.nTicket) != 0;
                });
                std::map<uint64_t, TransitionResult>::iterator it2 =
                    done_.find(req.nTicket);
                if (it2 != done_.end())
                {
                    if (out) *out = it2->second;
                    done_.erase(it2);
                    return true;
                }
                return false;
            }
            abandoned_.insert(req.nTicket);
            return false;
        }
    }
}

TransitionOutcome TransitionExecutor::Process(const TransitionRequest& req,
                                              TransitionResult* out)
{
    TransitionResult res;
    res.nTicket = req.nTicket;
    res.nSelfId = req.nSelfId;

    const uint64_t nTipHeight = nDurableTipHeight_.load();
    const uint64_t nTipId = nDurableTipId_.load();
    const bool fHaveTip = fTipInitialized_.load();

    // I1: density — a publishing EXTENSION must extend the durable tip. A reorg is
    // one fused atomic transition and is exempt by construction (its own commit
    // validates the fork point and the whole branch). An OPERATOR transition is
    // exempt for the same reason: it re-selects the branch itself, and its durable
    // tip comes from the commit result, not from a prediction.
    const bool fLinear = req.fPublish && !req.fReorg && !req.fOperator;
    const bool fNonDense = (fLinear && fHaveTip && req.nHeight != nTipHeight + 1);
    // I2: parent identity — publishing extensions must name the durable tip.
    const bool fParentMismatch = (fLinear && fHaveTip && req.nParentId != nTipId);
    if (fNonDense || fParentMismatch)
    {
        // ADR-0022 rev2 §7.4: a refused publication is NOT a consensus reject and
        // NOT a reason to drop a validated block. With fSideFallback the same
        // prepared record is committed as a durable SIDE record by the same single
        // writer (I6), and nothing is published. Without it the request is refused
        // outright (caller keeps the block and decides).
        if (req.fSideFallback)
        {
            TransitionRequest side = req;
            side.fPublish = false;
            side.fReorg = false;
            std::string serr;
            if (commit_(side, &serr))
            {
                res.eOutcome = TR_COMMITTED_SIDE;
                res.strDetail = fNonDense ? "non-dense active append: committed as SIDE"
                                          : "parent mismatch: committed as SIDE";
                nSideCommitted_.fetch_add(1);
                if (out) *out = res;
                return res.eOutcome;
            }
            res.eOutcome = TR_FAILED_COMMIT;
            res.strDetail = serr.empty() ? "side fallback commit failed" : serr;
            nFailed_.fetch_add(1);
            if (out) *out = res;
            return res.eOutcome;
        }
        res.eOutcome = fNonDense ? TR_REJECTED_DENSITY : TR_REJECTED_PARENT;
        res.strDetail = fNonDense ? "non-dense active append (publish refused; block not invalid)"
                                  : "active append parent mismatch (publish refused; block not invalid)";
        nRejected_.fetch_add(1);
        if (out) *out = res;
        return res.eOutcome;
    }

    std::string err;
    if (!commit_(req, &err))
    {
        // I8: ambiguous/failed commit -> nothing is published, fail-closed.
        res.eOutcome = TR_FAILED_COMMIT;
        res.strDetail = err.empty() ? "commit failed" : err;
        nFailed_.fetch_add(1);
        if (out) *out = res;
        return res.eOutcome;
    }

    if (req.fPublish)
    {
        if (req.fOperator)
        {
            // The operator commit is fused: the blockindex tip, the operator-invalid
            // intent and the resulting active tip were published in ONE commit. The
            // durable tip is therefore the RESULT the commit reported, not nHeight/
            // nSelfId (which a caller cannot predict for a re-selected branch).
            const int32_t nResHeight = req.pResultHeight ? *req.pResultHeight : -1;
            const uint64_t nResId = req.pResultId ? *req.pResultId : 0;
            if (nResHeight < 0)
            {
                res.eOutcome = TR_FAILED_COMMIT;
                res.strDetail = "operator commit did not report a durable tip";
                nFailed_.fetch_add(1);
                if (out) *out = res;
                return res.eOutcome;
            }
            {
                // [c29 A2] a commit's tip update shares the bind mutex, so a late
                // first bind cannot interleave between these three stores.
                std::lock_guard<std::mutex> tipLock(csTip_);
                nDurableTipHeight_.store((uint64_t)nResHeight);
                nDurableTipId_.store(nResId);
                fTipInitialized_.store(true);
            }
        }
        else
        {
            {
                // [c29 A2] see above: one mutex for every durable-tip write.
                std::lock_guard<std::mutex> tipLock(csTip_);
                nDurableTipHeight_.store(req.nHeight);
                nDurableTipId_.store(req.nSelfId);
                fTipInitialized_.store(true);
            }
        }
        std::string perr;
        if (publish_ && !publish_(req, &perr))
        {
            // The durable commit is already on disk; the servable switch failed.
            // Fail-stop: never report success, never retry silently (I4/I8).
            res.eOutcome = TR_FAILED_PUBLISH;
            res.strDetail = perr.empty() ? "publish failed" : perr;
            nPublishFailed_.fetch_add(1);
            printf("BPM_TRANSITION ticket=%llu height=%llu outcome=FAILED_PUBLISH detail=%s\n",
                   (unsigned long long)req.nTicket, (unsigned long long)req.nHeight,
                   res.strDetail.c_str());
            fflush(stdout);
            if (out) *out = res;
            return res.eOutcome;
        }
    }
    res.eOutcome = TR_COMMITTED;
    nCommitted_.fetch_add(1);
    if (out) *out = res;
    return res.eOutcome;
}

TransitionOutcome TransitionExecutor::RunInline(const TransitionRequest& reqIn,
                                                TransitionResult* out)
{
    TransitionRequest req = reqIn;
    req.nTicket = nNextTicket_.fetch_add(1) + 1;
    // ADMISSION GATE (P0-1): the check and the registration happen in one
    // critical section, so shutdown can never close the gate between "the
    // executor is open" and the durable commit below. Once Shutdown() has set
    // fStop_, no inline transition is admitted: it returns fail-closed instead
    // of committing into a store that is being flushed and closed.
    {
        std::lock_guard<std::mutex> admit(csAdmit_);
        if (fStop_)
        {
            if (out)
            {
                out->nTicket = req.nTicket;
                out->nSelfId = req.nSelfId;
                out->eOutcome = TR_FAILED_COMMIT;
                out->strDetail = "stopped: admission closed at shutdown (inline refused)";
            }
            printf("BPM_TRANSITION ticket=%llu outcome=REFUSED_STOPPED mode=inline\n",
                   (unsigned long long)req.nTicket);
            fflush(stdout);
            return TR_FAILED_COMMIT;
        }
        ++nAdmitted_;
    }
    TransitionOutcome oc;
    {
        std::lock_guard<std::mutex> lock(csTransition_);
        oc = Process(req, out);
    }
    {
        std::lock_guard<std::mutex> admit(csAdmit_);
        --nAdmitted_;
    }
    cvAdmit_.notify_all();
    printf("BPM_TRANSITION ticket=%llu height=%llu self=%016llx outcome=%s mode=inline reorg=%d publish=%d\n",
           (unsigned long long)req.nTicket, (unsigned long long)req.nHeight,
           (unsigned long long)req.nSelfId, TransitionOutcomeName(oc),
           req.fReorg ? 1 : 0, req.fPublish ? 1 : 0);
    fflush(stdout);
    return oc;
}

TransitionOutcome TransitionExecutor::TryRunInline(const TransitionRequest& reqIn,
                                                   TransitionResult* out, bool* fBusy)
{
    if (fBusy) *fBusy = false;
    // ADMISSION GATE (P0-1), identical contract to RunInline: a refused admission
    // is NOT reported as "busy" (the caller would retry and could then commit),
    // it is a definitive fail-closed result.
    {
        std::lock_guard<std::mutex> admit(csAdmit_);
        if (fStop_)
        {
            if (out)
            {
                out->eOutcome = TR_FAILED_COMMIT;
                out->strDetail = "stopped: admission closed at shutdown (inline refused)";
            }
            printf("BPM_TRANSITION outcome=REFUSED_STOPPED mode=inline_fallback\n");
            fflush(stdout);
            return TR_FAILED_COMMIT;
        }
        ++nAdmitted_;
    }
    std::unique_lock<std::mutex> lock(csTransition_, std::try_to_lock);
    if (!lock.owns_lock())
    {
        {
            std::lock_guard<std::mutex> admit(csAdmit_);
            --nAdmitted_;
        }
        cvAdmit_.notify_all();
        if (fBusy) *fBusy = true;
        return TR_INLINE_BUSY;
    }
    TransitionRequest req = reqIn;
    req.nTicket = nNextTicket_.fetch_add(1) + 1;
    const TransitionOutcome oc = Process(req, out);
    lock.unlock();
    {
        std::lock_guard<std::mutex> admit(csAdmit_);
        --nAdmitted_;
    }
    cvAdmit_.notify_all();
    printf("BPM_TRANSITION ticket=%llu height=%llu self=%016llx outcome=%s mode=inline_fallback reorg=%d publish=%d\n",
           (unsigned long long)req.nTicket, (unsigned long long)req.nHeight,
           (unsigned long long)req.nSelfId, TransitionOutcomeName(oc),
           req.fReorg ? 1 : 0, req.fPublish ? 1 : 0);
    fflush(stdout);
    return oc;
}

void TransitionExecutor::WorkerLoop()
{
    while (true)
    {
        TransitionRequest req;
        {
            std::unique_lock<std::mutex> lock(cs_);
            cv_.wait(lock, [this] { return fStop_ || !queue_.empty(); });
            if (queue_.empty())
            {
                if (fStop_)
                    return;  // drained and stopping (T6)
                continue;
            }
            req = queue_.front();
            queue_.pop_front();
            if (abandoned_.erase(req.nTicket) != 0)
            {
                // The submitter timed out before we started: never execute it.
                nSkipped_.fetch_add(1);
                continue;
            }
            started_.insert(req.nTicket);
        }
        cv_.notify_all();  // a blocked Submit may proceed

        TransitionResult res;
        TransitionOutcome oc;
        {
            std::lock_guard<std::mutex> transLock(csTransition_);
            oc = Process(req, &res);
        }
        printf("BPM_TRANSITION ticket=%llu height=%llu self=%016llx outcome=%s mode=worker reorg=%d publish=%d\n",
               (unsigned long long)req.nTicket, (unsigned long long)req.nHeight,
               (unsigned long long)req.nSelfId, TransitionOutcomeName(oc),
               req.fReorg ? 1 : 0, req.fPublish ? 1 : 0);
        fflush(stdout);
        {
            std::lock_guard<std::mutex> lock(cs_);
            results_.push_back(res);
            done_[req.nTicket] = res;
            started_.erase(req.nTicket);
        }
        cv_.notify_all();
    }
}

std::vector<TransitionResult> TransitionExecutor::Shutdown()
{
    {
        std::lock_guard<std::mutex> lock(cs_);
        if (fStop_)
            return results_;
        fStop_ = true;
        // ADR-0024 (T6) variant A: everything still QUEUED has not started, so it
        // is cancelled here - never executed, nothing committed, nothing published.
        // Each cancelled ticket gets a definitive result so a SubmitAndWait waiter
        // cannot hang and cannot see an ambiguous outcome. A request the worker has
        // already popped is NOT in queue_ and therefore finishes on its own.
        while (!queue_.empty())
        {
            TransitionRequest req = queue_.front();
            queue_.pop_front();
            TransitionResult res;
            res.nTicket = req.nTicket;
            res.nSelfId = req.nSelfId;
            res.eOutcome = TR_CANCELLED_SHUTDOWN;
            res.strDetail = "cancelled at shutdown before start (nothing committed)";
            results_.push_back(res);
            done_[req.nTicket] = res;
            abandoned_.erase(req.nTicket);
            nCancelled_.fetch_add(1);
            printf("BPM_TRANSITION ticket=%llu height=%llu outcome=%s mode=cancel\n",
                   (unsigned long long)req.nTicket, (unsigned long long)req.nHeight,
                   TransitionOutcomeName(TR_CANCELLED_SHUTDOWN));
            fflush(stdout);
        }
    }
    // Admission is closed (fStop_ = true): no NEW inline transition can start.
    // Wait for the ones already admitted so the durable store is not flushed and
    // closed underneath a commit that had passed the gate.
    {
        std::unique_lock<std::mutex> admit(csAdmit_);
        cvAdmit_.wait(admit, [this] { return nAdmitted_ == 0; });
    }
    cv_.notify_all();
    if (worker_.joinable())
        worker_.join();   // the in-flight transition finishes with its own result
    std::lock_guard<std::mutex> lock(cs_);
    return results_;
}

} // namespace bpmr
