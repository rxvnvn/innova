// BPMR/ADR-0022 rev2 — standalone checks for the single transition executor.
// Covers T1 (extension), T2 (density/parent rejects), T3 (concurrent submitters),
// T6 (shutdown drain), T8 (fail-closed commit), T9 (backpressure).
// Build: g++ -O1 -std=c++17 bpmr_transition_executor.cpp bpmr_executor_tests.cpp -pthread

#include "bpmr_transition_executor.h"

#include <atomic>
#include <chrono>
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

static void TestExtension()
{
    std::printf("T1 extension\n");
    std::atomic<uint64_t> nCommitted{0};
    TransitionExecutor ex(64,
        [&](const TransitionRequest& r, std::string*) {
            ++nCommitted;
            return true;
        },
        [](const TransitionRequest&) {});
    for (uint64_t h = 1; h <= 50; ++h)
    {
        TransitionRequest r;
        r.nHeight = h;
        r.nParentId = (h == 1) ? 0 : h - 1;
        r.nSelfId = h;
        r.fPublish = true;
        CHECK(ex.Submit(r, 1000), "submit accepted");
    }
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
        [](const TransitionRequest&) {});
    TransitionRequest r1; r1.nHeight = 1; r1.nParentId = 0; r1.nSelfId = 1; r1.fPublish = true;
    ex.Submit(r1, 1000);

    TransitionRequest bad; bad.nHeight = 5; bad.nParentId = 1; bad.nSelfId = 5; bad.fPublish = true;
    ex.Submit(bad, 1000);  // not prev+1

    TransitionRequest bad2; bad2.nHeight = 2; bad2.nParentId = 999; bad2.nSelfId = 2; bad2.fPublish = true;
    ex.Submit(bad2, 1000);  // wrong parent

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
        [](const TransitionRequest&) {});
    TransitionRequest a; a.nHeight = 1; a.nParentId = 0; a.nSelfId = 1; a.fPublish = true;
    TransitionRequest b; b.nHeight = 2; b.nParentId = 1; b.nSelfId = 2; b.fPublish = true;
    ex.Submit(a, 1000);
    ex.Submit(b, 1000);
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
        [](const TransitionRequest&) {});
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
        [](const TransitionRequest&) {});
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
    auto results = ex.Shutdown();
    CHECK(nAccepted == 1600, "all 1600 concurrent submits accepted");
    CHECK(results.size() == 1600, "1600 results, none lost");
    CHECK(ex.CommittedCount() == 1600, "1600 committed");
}

static void TestShutdownDrain()
{
    std::printf("T6 shutdown drain\n");
    std::atomic<int> nCommitted{0};
    TransitionExecutor ex(1024,
        [&](const TransitionRequest&, std::string*) {
            std::this_thread::sleep_for(std::chrono::microseconds(200));
            ++nCommitted;
            return true;
        },
        [](const TransitionRequest&) {});
    for (uint64_t i = 0; i < 200; ++i)
    {
        TransitionRequest r;
        r.nHeight = i + 1;
        r.nParentId = i;
        r.nSelfId = i + 1;
        r.fPublish = false;
        ex.Submit(r, 1000);
    }
    auto results = ex.Shutdown();  // must drain, not drop
    CHECK(results.size() == 200, "queued work drained on shutdown");
    CHECK(nCommitted == 200, "every queued request committed");
}

int main()
{
    TestExtension();
    TestRejects();
    TestFailClosedCommit();
    TestBackpressure();
    TestConcurrentSubmitters();
    TestShutdownDrain();
    std::printf("\n%s failures=%d\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}
