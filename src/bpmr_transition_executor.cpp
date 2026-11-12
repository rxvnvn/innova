#include "bpmr_transition_executor.h"

#include <chrono>

namespace bpmr
{

TransitionExecutor::TransitionExecutor(size_t nMaxQueued, CommitFn commit,
                                       PublishFn publish)
    : nMaxQueued_(nMaxQueued), commit_(commit), publish_(publish), fStop_(false),
      nDurableTipHeight_(0), nDurableTipId_(0), nCommitted_(0), nRejected_(0),
      nFailed_(0)
{
    worker_ = std::thread(&TransitionExecutor::WorkerLoop, this);
}

TransitionExecutor::~TransitionExecutor()
{
    Shutdown();
}

bool TransitionExecutor::TrySubmit(const TransitionRequest& req)
{
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

bool TransitionExecutor::Submit(const TransitionRequest& req, int nTimeoutMs)
{
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

TransitionOutcome TransitionExecutor::RunInline(const TransitionRequest& req,
                                              TransitionResult* out)
{
    TransitionResult res;
    res.nSelfId = req.nSelfId;

    const uint64_t nTipHeight = nDurableTipHeight_.load();
    const uint64_t nTipId = nDurableTipId_.load();
    if (req.fPublish && nTipHeight != 0 && req.nHeight != nTipHeight + 1)
    {
        res.eOutcome = TR_REJECTED_DENSITY;
        res.strDetail = "non-dense active append";
        nRejected_.fetch_add(1);
        if (out) *out = res;
        return res.eOutcome;
    }
    if (!req.fCallerValidatedIdentity && req.fPublish && nTipHeight != 0 &&
        req.nParentId != nTipId)
    {
        res.eOutcome = TR_REJECTED_PARENT;
        res.strDetail = "active append parent mismatch";
        nRejected_.fetch_add(1);
        if (out) *out = res;
        return res.eOutcome;
    }

    std::string err;
    if (!commit_(req, &err))
    {
        res.eOutcome = TR_FAILED_COMMIT;
        res.strDetail = err.empty() ? "commit failed" : err;
        nFailed_.fetch_add(1);
        if (out) *out = res;
        return res.eOutcome;
    }
    if (req.fPublish)
    {
        nDurableTipHeight_.store(req.nHeight);
        nDurableTipId_.store(req.nSelfId);
        if (publish_)
            publish_(req);
    }
    res.eOutcome = TR_COMMITTED;
    nCommitted_.fetch_add(1);
    if (out) *out = res;
    return res.eOutcome;
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
        }
        cv_.notify_all();  // a blocked Submit may proceed

        TransitionResult res;
        res.nSelfId = req.nSelfId;

        // I1: density — a publishing request must extend the durable tip.
        const uint64_t nTipHeight = nDurableTipHeight_.load();
        const uint64_t nTipId = nDurableTipId_.load();
        if (req.fPublish && nTipHeight != 0 && req.nHeight != nTipHeight + 1)
        {
            res.eOutcome = TR_REJECTED_DENSITY;
            res.strDetail = "non-dense active append";
            nRejected_.fetch_add(1);
            std::lock_guard<std::mutex> lock(cs_);
            results_.push_back(res);
            continue;
        }
        // I2: parent identity — publishing requests must name the durable tip.
        if (!req.fCallerValidatedIdentity && req.fPublish && nTipHeight != 0 &&
            req.nParentId != nTipId)
        {
            res.eOutcome = TR_REJECTED_PARENT;
            res.strDetail = "active append parent mismatch";
            nRejected_.fetch_add(1);
            std::lock_guard<std::mutex> lock(cs_);
            results_.push_back(res);
            continue;
        }

        std::string err;
        if (!commit_(req, &err))
        {
            // I8: ambiguous/failed commit -> nothing is published, fail-closed.
            res.eOutcome = TR_FAILED_COMMIT;
            res.strDetail = err.empty() ? "commit failed" : err;
            nFailed_.fetch_add(1);
            std::lock_guard<std::mutex> lock(cs_);
            results_.push_back(res);
            continue;
        }

        if (req.fPublish)
        {
            nDurableTipHeight_.store(req.nHeight);
            nDurableTipId_.store(req.nSelfId);
            if (publish_)
                publish_(req);
        }
        res.eOutcome = TR_COMMITTED;
        nCommitted_.fetch_add(1);
        {
            std::lock_guard<std::mutex> lock(cs_);
            results_.push_back(res);
        }
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
