// BPMR/ADR-0022 rev2 — standalone checks for the single transition executor.
// Covers T1 (extension), T2 (density/parent rejects), T3 (concurrent submitters),
// T6 (shutdown drain), T8 (fail-closed commit), T9 (backpressure).
// Build: g++ -O1 -std=c++17 bpmr_transition_executor.cpp bpmr_executor_tests.cpp -pthread

#include "bpmr_transition_executor.h"

#include <map>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <cstdio>
#include <thread>
#include <vector>

using namespace bpmr;

static int g_failures = 0;
#define CHECK(cond, name)                                                     \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::printf("  FAIL %s\n", name);                                 \
            ++g_failures;                                                     \
        } else {                                                              \
            std::printf("  ok   %s\n", name);                                 \
        }                                                                     \
    } while (0)

// ADR-0024 (T6) variant A: shutdown cancels requests that have NOT started. Tests
// that assert on executed transitions therefore wait until the worker has drained
// them BEFORE calling Shutdown(); otherwise they would assert on cancellations.
static void WaitForDrain(TransitionExecutor& ex, size_t nExpected, int nTimeoutMs = 15000)
{
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(nTimeoutMs);
    while (std::chrono::steady_clock::now() < deadline)
    {
        const uint64_t n = ex.CommittedCount() + ex.SideCommittedCount() +
                           ex.RejectedCount() + ex.FailedCount() +
                           ex.PublishFailedCount() + ex.CancelledAtShutdownCount();
        if (n >= (uint64_t)nExpected) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

static void TestExtension()
{
    std::printf("T1 extension\n");
    std::atomic<uint64_t> nCommitted{0};
    TransitionExecutor ex(64,
        [&](const TransitionRequest& r, std::string*) {
            ++nCommitted;
            return true;
        },
        [](const TransitionRequest&, std::string*) { return true; });
    for (uint64_t h = 1; h <= 50; ++h)
    {
        TransitionRequest r;
        r.nHeight = h;
        r.nParentId = (h == 1) ? 0 : h - 1;
        r.nSelfId = h;
        r.fPublish = true;
        CHECK(ex.Submit(r, 1000), "submit accepted");
    }
    WaitForDrain(ex, 50);
    auto results = ex.Shutdown();
    CHECK(results.size() == 50, "50 results recorded");
    CHECK(ex.CommittedCount() == 50, "50 committed");
    CHECK(ex.DurableTipHeight() == 50, "durable tip height 50");
    CHECK(ex.RejectedCount() == 0, "no rejects");
}

static void TestRejects()
{
    std::printf("T2 density / parent rejects\n");
    TransitionExecutor ex(16,
        [](const TransitionRequest&, std::string*) { return true; },
        [](const TransitionRequest&, std::string*) { return true; });
    TransitionRequest r1; r1.nHeight = 1; r1.nParentId = 0; r1.nSelfId = 1; r1.fPublish = true;
    ex.Submit(r1, 1000);

    TransitionRequest bad; bad.nHeight = 5; bad.nParentId = 1; bad.nSelfId = 5; bad.fPublish = true;
    ex.Submit(bad, 1000);  // not prev+1

    TransitionRequest bad2; bad2.nHeight = 2; bad2.nParentId = 999; bad2.nSelfId = 2; bad2.fPublish = true;
    ex.Submit(bad2, 1000);  // wrong parent

    WaitForDrain(ex, 2);
    auto results = ex.Shutdown();
    int nDensity = 0, nParent = 0;
    for (const auto& r : results)
    {
        if (r.eOutcome == TR_REJECTED_DENSITY) ++nDensity;
        if (r.eOutcome == TR_REJECTED_PARENT) ++nParent;
    }
    CHECK(nDensity == 1, "density violation rejected");
    CHECK(nParent == 1, "parent mismatch rejected");
    CHECK(ex.DurableTipHeight() == 1, "durable tip unchanged by rejects");
}

static void TestFailClosedCommit()
{
    std::printf("T8 fail-closed commit\n");
    TransitionExecutor ex(16,
        [](const TransitionRequest& r, std::string*) {
            return r.nHeight != 2;  // height 2 fails to commit
        },
        [](const TransitionRequest&, std::string*) { return true; });
    TransitionRequest a; a.nHeight = 1; a.nParentId = 0; a.nSelfId = 1; a.fPublish = true;
    TransitionRequest b; b.nHeight = 2; b.nParentId = 1; b.nSelfId = 2; b.fPublish = true;
    ex.Submit(a, 1000);
    ex.Submit(b, 1000);
    WaitForDrain(ex, 1);
    auto results = ex.Shutdown();
    CHECK(ex.FailedCount() == 1, "one commit failure recorded");
    CHECK(ex.DurableTipHeight() == 1, "durable tip not advanced past failure");
}

static void TestBackpressure()
{
    std::printf("T9 backpressure\n");
    std::atomic<int> nSeen{0};
    TransitionExecutor ex(2,
        [&](const TransitionRequest&, std::string*) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            ++nSeen;
            return true;
        },
        [](const TransitionRequest&, std::string*) { return true; });
    int nAccepted = 0, nRefused = 0;
    for (uint64_t i = 0; i < 40; ++i)
    {
        TransitionRequest r;
        r.nHeight = i + 1;
        r.nParentId = i;
        r.nSelfId = i + 1;
        r.fPublish = false;  // side records: no density constraint
        if (ex.TrySubmit(r)) ++nAccepted; else ++nRefused;
    }
    ex.Shutdown();
    CHECK(nRefused > 0, "queue overflow refused instead of unbounded growth");
    CHECK(nAccepted + nRefused == 40, "every submit got a definite answer");
}

static void TestConcurrentSubmitters()
{
    std::printf("T3 concurrent submitters\n");
    TransitionExecutor ex(256,
        [](const TransitionRequest&, std::string*) { return true; },
        [](const TransitionRequest&, std::string*) { return true; });
    std::atomic<uint64_t> nAccepted{0};
    std::vector<std::thread> producers;
    for (int t = 0; t < 8; ++t)
    {
        producers.emplace_back([&, t] {
            for (uint64_t i = 0; i < 200; ++i)
            {
                TransitionRequest r;
                r.nHeight = i + 1;
                r.nParentId = i;
                r.nSelfId = 1000 * (t + 1) + i;
                r.fPublish = false;  // side records from many threads
                if (ex.Submit(r, 2000)) ++nAccepted;
            }
        });
    }
    for (auto& th : producers) th.join();
    WaitForDrain(ex, 1600);
    auto results = ex.Shutdown();
    CHECK(nAccepted == 1600, "all 1600 concurrent submits accepted");
    CHECK(results.size() == 1600, "1600 results, none lost");
    CHECK(ex.CommittedCount() == 1600, "1600 committed");
}

static void TestShutdownDrain()
{
    // ADR-0024 (T6) variant A REPLACES the old "drain everything" contract: a
    // request that has not STARTED is cancelled at shutdown (nothing committed),
    // and only the request already in flight completes. Each ticket still gets a
    // definite result, so nothing is silent.
    std::printf("T6 shutdown: pending cancelled, in-flight completes\n");
    std::atomic<int> nCommitted{0};
    std::mutex m; std::condition_variable cv;
    bool fRelease = false, fInFirst = false;
    TransitionExecutor ex(1024,
        [&](const TransitionRequest&, std::string*) {
            {
                std::unique_lock<std::mutex> lk(m);
                if (!fInFirst) { fInFirst = true; cv.notify_all(); }
                // hold the FIRST transition until the test releases it
                cv.wait(lk, [&] { return fRelease; });
            }
            ++nCommitted;
            return true;
        },
        [](const TransitionRequest&, std::string*) { return true; });
    for (uint64_t i = 0; i < 200; ++i)
    {
        TransitionRequest r;
        r.nHeight = i + 1;
        r.nParentId = i;
        r.nSelfId = i + 1;
        r.fPublish = false;
        ex.Submit(r, 1000);
    }
    { std::unique_lock<std::mutex> lk(m); cv.wait(lk, [&] { return fInFirst; }); }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));  // let the rest queue
    std::thread releaser([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        { std::lock_guard<std::mutex> lk(m); fRelease = true; }
        cv.notify_all();
    });
    auto results = ex.Shutdown();   // cancels queued, waits for the in-flight one
    releaser.join();
    int nCancelled = 0, nCommittedRes = 0;
    for (size_t i = 0; i < results.size(); ++i)
    {
        if (results[i].eOutcome == TR_CANCELLED_SHUTDOWN) ++nCancelled;
        if (results[i].eOutcome == TR_COMMITTED) ++nCommittedRes;
    }
    CHECK(results.size() == 200, "every ticket has a definitive result");
    CHECK(nCommittedRes == 1, "exactly the in-flight transition committed");
    CHECK(nCancelled == 199, "all queued-but-not-started requests cancelled");
    CHECK(nCommitted == 1, "durable commit called exactly once (no double commit)");
    CHECK(ex.CancelledAtShutdownCount() == 199, "cancelled counter matches");
}

static void TestShutdownSubmitAndWaitNoHang()
{
    // A caller blocked in SubmitAndWait when shutdown starts must return with an
    // unambiguous outcome (executed result or cancelled), never hang and never
    // leave the caller able to re-run a transition that actually committed.
    std::printf("T6 shutdown: SubmitAndWait returns without hanging\n");
    TransitionExecutor ex(8,
        [](const TransitionRequest&, std::string*) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            return true;
        },
        [](const TransitionRequest&, std::string*) { return true; });
    std::atomic<int> nReturned{0};
    std::atomic<int> nCancelledSeen{0};
    std::atomic<int> nExecuted{0};
    std::vector<std::thread> ts;
    for (int i = 0; i < 6; ++i)
    {
        ts.emplace_back([&, i] {
            TransitionRequest r;
            r.nHeight = (uint64_t)i + 1; r.nParentId = (uint64_t)i; r.nSelfId = (uint64_t)i + 1;
            TransitionResult out;
            const bool ok = ex.SubmitAndWait(r, 5000, &out);
            if (ok)
            {
                if (out.eOutcome == TR_CANCELLED_SHUTDOWN) ++nCancelledSeen;
                else ++nExecuted;
            }
            ++nReturned;
        });
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    std::thread stopper([&] { ex.Shutdown(); });
    for (size_t i = 0; i < ts.size(); ++i) ts[i].join();
    stopper.join();
    CHECK(nReturned.load() == 6, "all waiters returned (no hang)");
    CHECK(nExecuted.load() + nCancelledSeen.load() == 6, "each waiter got a definite outcome");
}

static void TestSideFallback()
{
    std::printf("T7.4 refused append -> durable SIDE record (no publish)\n");
    std::atomic<int> nPublishCalled{0};
    std::atomic<int> nSideCommit{0};
    std::atomic<int> nActiveCommit{0};
    TransitionExecutor ex(16,
        [&](const TransitionRequest& r, std::string*) {
            if (r.fPublish) { ++nActiveCommit; return true; }
            ++nSideCommit;
            return true;
        },
        [&](const TransitionRequest&, std::string*) { ++nPublishCalled; return true; });
    TransitionRequest a; a.nHeight = 1; a.nParentId = 0; a.nSelfId = 1; a.fPublish = true;
    ex.Submit(a, 1000);
    // The same-height sibling loses the race: it must NOT be dropped and NOT be
    // published — it becomes a durable side record.
    TransitionRequest sib; sib.nHeight = 1; sib.nParentId = 0; sib.nSelfId = 2;
    sib.fPublish = true; sib.fSideFallback = true;
    ex.Submit(sib, 1000);
    WaitForDrain(ex, 2);
    auto results = ex.Shutdown();
    int nSide = 0;
    for (const auto& r : results) if (r.eOutcome == TR_COMMITTED_SIDE) ++nSide;
    CHECK(nSide == 1, "refused append recorded as COMMITTED_SIDE");
    CHECK(nSideCommit == 1, "side commit callback invoked exactly once");
    CHECK(nActiveCommit == 1, "active commit callback invoked only for the winner");
    CHECK(nPublishCalled == 1, "publish callback NOT invoked for the side record");
    CHECK(ex.DurableTipHeight() == 1, "durable tip unchanged by the side fallback");
    CHECK(ex.RejectedCount() == 0, "no rejection recorded (block is not invalid)");

    // Without the flag the same request is refused outright.
    TransitionExecutor ex2(16,
        [](const TransitionRequest&, std::string*) { return true; },
        [](const TransitionRequest&, std::string*) { return true; });
    TransitionRequest a2; a2.nHeight = 1; a2.nParentId = 0; a2.nSelfId = 1; a2.fPublish = true;
    ex2.Submit(a2, 1000);
    TransitionRequest sib2; sib2.nHeight = 1; sib2.nParentId = 0; sib2.nSelfId = 2; sib2.fPublish = true;
    ex2.Submit(sib2, 1000);
    WaitForDrain(ex2, 2);   // T6 variant A: let the worker process both before stop
    auto r2 = ex2.Shutdown();
    int nRej = 0;
    for (const auto& r : r2) if (r.eOutcome == TR_REJECTED_DENSITY) ++nRej;
    CHECK(nRej == 1, "without the side flag the append is refused (REJECTED_DENSITY)");
}

// P0.3 (I6): a fused OPERATOR transition (invalidate / reconsider) must
//   * skip I1/I2 (it re-selects the branch itself, exactly like a reorg),
//   * bind the durable tip from the COMMIT RESULT, never from nHeight/nSelfId,
//   * fail closed (no publication) when the commit does not report a tip.
static void TestOperatorTransition()
{
    std::printf("P0.3 operator transition (fused, tip bound from the commit result)\n");
    int32_t nResultHeight = 7;
    uint64_t nResultId = 0xABCD;
    int nCommit = 0, nPublish = 0;
    TransitionExecutor ex(16,
        [&](const TransitionRequest& r, std::string*) {
            ++nCommit;
            // Linear appends carry no result pointers; only operator requests do.
            if (r.pResultHeight) *r.pResultHeight = nResultHeight;
            if (r.pResultId) *r.pResultId = nResultId;
            return true;
        },
        [&](const TransitionRequest&, std::string*) { ++nPublish; return true; });
    ex.InitDurableTip(10, 0x1234);   // tip before the operator transition

    TransitionRequest op;
    op.fPublish = true;
    op.fReorg = true;
    op.fOperator = true;
    op.nHeight = 999;        // deliberately wrong: must NOT become the durable tip
    op.nParentId = 0xDEAD;   // deliberately wrong: I2 must not apply
    op.nSelfId = 0xBEEF;
    op.pResultHeight = &nResultHeight;
    op.pResultId = &nResultId;

    TransitionResult tr;
    CHECK(ex.RunInline(op, &tr) == TR_COMMITTED, "operator transition commits (I1/I2 exempt)");
    CHECK(nCommit == 1 && nPublish == 1, "commit and publish both ran");
    CHECK(ex.DurableTipHeight() == 7, "durable tip comes from the commit RESULT (7), not nHeight");
    CHECK(ex.DurableTipId() == 0xABCD, "durable tip identity comes from the commit result");

    // The NEXT linear append must be checked against the operator's result tip.
    TransitionRequest okNext;
    okNext.nHeight = 8; okNext.nParentId = 0xABCD; okNext.nSelfId = 8; okNext.fPublish = true;
    CHECK(ex.RunInline(okNext, &tr) == TR_COMMITTED, "next append at result_tip+1 is accepted");
    TransitionRequest badNext;
    badNext.nHeight = 12; badNext.nParentId = 8; badNext.nSelfId = 12; badNext.fPublish = true;
    CHECK(ex.RunInline(badNext, &tr) == TR_REJECTED_DENSITY, "I1 still enforced after an operator transition");

    // A commit that never reports a tip is fail-closed: nothing is published.
    int nPublish2 = 0;
    TransitionExecutor ex2(16,
        [](const TransitionRequest&, std::string*) { return true; },   // writes no result
        [&](const TransitionRequest&, std::string*) { ++nPublish2; return true; });
    ex2.InitDurableTip(3, 3);
    int32_t nH = -1;
    uint64_t nI = 0;
    TransitionRequest op2;
    op2.fPublish = true; op2.fReorg = true; op2.fOperator = true;
    op2.pResultHeight = &nH; op2.pResultId = &nI;
    TransitionResult tr2;
    CHECK(ex2.RunInline(op2, &tr2) == TR_FAILED_COMMIT, "operator commit without a tip fails closed");
    CHECK(nPublish2 == 0, "nothing published on a failed operator commit");
    CHECK(ex2.DurableTipHeight() == 3, "durable tip unchanged on failure");

    // A failing operator commit is explicit and publishes nothing.
    int nPublish3 = 0;
    TransitionExecutor ex3(16,
        [](const TransitionRequest&, std::string* e) { *e = "store unwind failed"; return false; },
        [&](const TransitionRequest&, std::string*) { ++nPublish3; return true; });
    ex3.InitDurableTip(3, 3);
    TransitionRequest op3;
    op3.fPublish = true; op3.fReorg = true; op3.fOperator = true;
    op3.pResultHeight = &nH; op3.pResultId = &nI;
    TransitionResult tr3;
    CHECK(ex3.RunInline(op3, &tr3) == TR_FAILED_COMMIT, "failing operator commit -> FAILED_COMMIT");
    CHECK(tr3.strDetail == "store unwind failed", "failure detail is propagated verbatim");
    CHECK(nPublish3 == 0, "no publication after a failed operator commit");
}

static void TestFalseMeansNeverEnqueued()
{
    // After shutdown no ticket can be enqueued, so SubmitAndWait must answer
    // false - and false is the ONLY reason the caller may run the transition
    // inline. Nothing may be committed by the executor in this state.
    std::printf("P0-1: false after shutdown means never enqueued\n");
    TransitionExecutor ex(64,
        [](const TransitionRequest&, std::string*) { return true; },
        [](const TransitionRequest&, std::string*) { return true; });
    ex.Shutdown();
    TransitionRequest r;
    r.nHeight = 1; r.nParentId = 0; r.nSelfId = 1; r.fPublish = false;
    TransitionResult out;
    const bool ok = ex.SubmitAndWait(r, 200, &out);
    CHECK(!ok, "SubmitAndWait false once stopped");
    CHECK(ex.CommittedCount() == 0, "no commit from a refused submit");
    CHECK(ex.SideCommittedCount() == 0, "no side commit from a refused submit");
    CHECK(ex.DurableTipHeight() == 0, "durable tip untouched");
}

static void TestTimeoutAbandonedSingleCommit()
{
    // The worker is busy with an earlier transition; the second one waits in the
    // queue and times out. Its ticket must be abandoned (never executed), so the
    // synchronous fallback performs the ONLY commit for that height.
    std::printf("P0-1: timeout of a queued ticket -> abandoned, one commit\n");
    std::mutex m; std::condition_variable cv;
    bool fInFirst = false, fRelease = false;
    std::map<uint64_t, int> commits;   // height -> number of commits
    std::mutex cm;
    TransitionExecutor ex(64,
        [&](const TransitionRequest& req, std::string*) {
            {
                std::unique_lock<std::mutex> lk(m);
                if (!fInFirst) { fInFirst = true; cv.notify_all(); }
                cv.wait(lk, [&] { return fRelease; });
            }
            std::lock_guard<std::mutex> g(cm);
            commits[req.nHeight] += 1;
            return true;
        },
        [](const TransitionRequest&, std::string*) { return true; });

    TransitionRequest r1; r1.nHeight = 1; r1.nParentId = 0; r1.nSelfId = 1; r1.fPublish = false;
    CHECK(ex.Submit(r1, 0), "first submit accepted");
    { std::unique_lock<std::mutex> lk(m); cv.wait(lk, [&] { return fInFirst; }); }

    TransitionRequest r2; r2.nHeight = 2; r2.nParentId = 1; r2.nSelfId = 2; r2.fPublish = false;
    TransitionResult out2;
    const bool ok2 = ex.SubmitAndWait(r2, 30, &out2);   // times out while queued
    CHECK(!ok2, "queued ticket abandoned at timeout (false)");

    { std::lock_guard<std::mutex> lk(m); fRelease = true; }
    cv.notify_all();
    ex.Shutdown();
    std::lock_guard<std::mutex> g(cm);
    CHECK(commits[1] == 1, "first height committed exactly once");
    CHECK(commits.find(2) == commits.end() || commits[2] == 0,
          "abandoned ticket was NOT executed by the worker");
    // The abandoned ticket is resolved without ever executing: either the worker
    // pops it and sees abandoned_ (skipped) or shutdown cancels it in the queue.
    CHECK(ex.SkippedCount() + ex.CancelledAtShutdownCount() == 1,
          "abandoned ticket resolved without execution");
    // The executor committed height 1 only; the abandoned height was never its
    // work - the caller may commit it inline, which is exactly one execution.
    CHECK(ex.CommittedCount() == 1, "executor committed only the in-flight height");
}

static void TestTimeoutStartedAlwaysWaited()
{
    // Once the worker has STARTED a ticket, a timeout must not return false:
    // the caller would re-run the same transition inline and commit twice.
    std::printf("P0-1: started ticket is waited past its timeout\n");
    std::mutex m; std::condition_variable cv;
    bool fInFirst = false, fRelease = false;
    std::atomic<int> nCommits{0};
    TransitionExecutor ex(64,
        [&](const TransitionRequest&, std::string*) {
            {
                std::unique_lock<std::mutex> lk(m);
                fInFirst = true; cv.notify_all();
                cv.wait(lk, [&] { return fRelease; });
            }
            ++nCommits;
            return true;
        },
        [](const TransitionRequest&, std::string*) { return true; });

    TransitionRequest r; r.nHeight = 1; r.nParentId = 0; r.nSelfId = 1; r.fPublish = false;
    TransitionResult out;
    std::atomic<bool> fReturned{false};
    std::thread waiter([&] {
        const bool ok = ex.SubmitAndWait(r, 30, &out);   // 30 ms, work takes ~400 ms
        CHECK(ok, "SubmitAndWait true despite timeout (started ticket)");
        fReturned.store(true);
    });
    { std::unique_lock<std::mutex> lk(m); cv.wait(lk, [&] { return fInFirst; }); }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(!fReturned.load(), "still waiting past the timeout - no inline fallback");
    { std::lock_guard<std::mutex> lk(m); fRelease = true; }
    cv.notify_all();
    waiter.join();
    ex.Shutdown();
    CHECK(nCommits.load() == 1, "exactly one commit for the started ticket");
    CHECK(ex.FailedCount() == 0, "no failure recorded");
}

int main()
{
    TestExtension();
    TestRejects();
    TestFailClosedCommit();
    TestBackpressure();
    TestConcurrentSubmitters();
    TestShutdownDrain();
    TestSideFallback();
    TestOperatorTransition();
    TestShutdownSubmitAndWaitNoHang();
    TestFalseMeansNeverEnqueued();
    TestTimeoutAbandonedSingleCommit();
    TestTimeoutStartedAlwaysWaited();
    std::printf("\n%s failures=%d\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}
