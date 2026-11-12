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
    }
    return "UNKNOWN";
}

TransitionExecutor::TransitionExecutor(size_t nMaxQueued, CommitFn commit,
                                       PublishFn publish)
    : nNextTicket_(0), nMaxQueued_(nMaxQueued), commit_(commit), publish_(publish),
      fStop_(false), fTipInitialized_(false), nDurableTipHeight_(0),
      nDurableTipId_(0), nCommitted_(0), nSideCommitted_(0), nRejected_(0),
      nFailed_(0), nPublishFailed_(0), nSkipped_(0)
{
    worker_ = std::thread(&TransitionExecutor::WorkerLoop, this);
}

TransitionExecutor::~TransitionExecutor()
{
    Shutdown();
}

void TransitionExecutor::InitDurableTip(uint64_t nHeight, uint64_t nId)
{
    nDurableTipHeight_.store(nHeight);
    nDurableTipId_.store(nId);
    fTipInitialized_.store(true);
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
        if (fStop_)
            return false;
        if (cv_.wait_until(lock, deadline) == std::cv_status::timeout)
        {
            // A request that has already STARTED is always waited for: abandoning
            // it here would let the synchronous fallback execute the same
            // transition a second time. Only a not-yet-started request may be
            // abandoned (the worker then skips it without touching any state).
            if (started_.count(req.nTicket) != 0)
            {
                cv_.wait(lock, [this, &req] {
                    return done_.count(req.nTicket) != 0 || fStop_;
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
            nDurableTipHeight_.store((uint64_t)nResHeight);
            nDurableTipId_.store(nResId);
            fTipInitialized_.store(true);
        }
        else
        {
            nDurableTipHeight_.store(req.nHeight);
            nDurableTipId_.store(req.nSelfId);
            fTipInitialized_.store(true);
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
    std::lock_guard<std::mutex> lock(csTransition_);
    const TransitionOutcome oc = Process(req, out);
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
    std::unique_lock<std::mutex> lock(csTransition_, std::try_to_lock);
    if (!lock.owns_lock())
    {
        if (fBusy) *fBusy = true;
        return TR_INLINE_BUSY;
    }
    TransitionRequest req = reqIn;
    req.nTicket = nNextTicket_.fetch_add(1) + 1;
    const TransitionOutcome oc = Process(req, out);
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
    }
    cv_.notify_all();
    if (worker_.joinable())
        worker_.join();
    std::lock_guard<std::mutex> lock(cs_);
    return results_;
}

} // namespace bpmr
