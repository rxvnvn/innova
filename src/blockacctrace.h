// Opt-in block-acceptance diagnostics (-blockaccepttrace, default OFF).
//
// Purpose: localise a long block-acceptance stall WITHOUT perf/gdb on a live
// node. Two questions it must answer from the node's own log:
//   1. which stage of the acceptance path held cs_main (check / accept / index
//      append / set-best-chain / store publish / wallet callback);
//   2. how much record-store I/O that stage performed (ops, bytes, slowest
//      single read), so a "records.dat storm" is attributed to a stage instead
//      of guessed.
//
// Cost contract: when the flag is off, every hook is a single relaxed atomic
// load, no allocation, no formatting, no lock. When on, one line is emitted per
// block that exceeds the threshold (and one per completed block if verbose).
//
// Header-only on purpose: no new translation unit, no CMake change, and the
// function-local statics give exactly one instance across TUs (C++11).
#pragma once

#include <stdint.h>
#include <atomic>
#include <string>
#include <mutex>
#include <ctime>
#include <thread>
#include <stdio.h>

namespace blockacctrace {

// Monotonic microseconds without pulling util.h into this header.
inline uint64_t NowUs()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)(ts.tv_nsec / 1000);
}

inline std::mutex& HashLock() { static std::mutex m; return m; }
inline std::string& CurrentHash() { static std::string h; return h; }

inline std::atomic<bool>& Flag()
{
    static std::atomic<bool> f(false);
    return f;
}
inline void SetEnabled(bool v) { Flag().store(v, std::memory_order_relaxed); }
inline bool Enabled() { return Flag().load(std::memory_order_relaxed); }

struct Counters
{
    std::atomic<uint64_t> readOps;
    std::atomic<uint64_t> readBytes;
    std::atomic<uint64_t> slowReadUs;
    Counters() : readOps(0), readBytes(0), slowReadUs(0) {}
};

inline Counters& Reads()
{
    static Counters c;
    return c;
}

struct ReadSnapshot
{
    uint64_t ops;
    uint64_t bytes;
    uint64_t slowUs;
};

inline ReadSnapshot Snapshot()
{
    ReadSnapshot s;
    Counters& c = Reads();
    s.ops = c.readOps.load(std::memory_order_relaxed);
    s.bytes = c.readBytes.load(std::memory_order_relaxed);
    s.slowUs = c.slowReadUs.load(std::memory_order_relaxed);
    return s;
}

// Called from the record-store read paths only when the flag is on.
inline void NoteRead(uint64_t bytes, uint64_t us)
{
    Counters& c = Reads();
    c.readOps.fetch_add(1, std::memory_order_relaxed);
    c.readBytes.fetch_add(bytes, std::memory_order_relaxed);
    uint64_t prev = c.slowReadUs.load(std::memory_order_relaxed);
    while (us > prev && !c.slowReadUs.compare_exchange_weak(prev, us, std::memory_order_relaxed)) {
    }
}

// Per-block stage marker. Cheap enough to set unconditionally at the few call
// sites (a relaxed store), so a stall that kills the process still leaves the
// last stage visible in any later trace.
// Single source of truth for the stage: the in-flight record (declared below as
// InFlight::stage). Stage() is kept as an accessor so call sites stay unchanged.
inline std::atomic<int>& Stage();
inline void SetStage(int s);
inline const char* StageName(int s)
{
    switch (s) {
    case 0: return "none";
    case 1: return "checkblock";
    case 2: return "acceptblock";
    case 3: return "addtoblockindex";
    case 4: return "setbestchain";
    case 5: return "reorganize";
    case 6: return "store3a_publish";
    case 7: return "wallet_callback";
    default: return "?";
    }
}

// ---- In-flight block (stall attribution) -----------------------------------
//
// A stall that never completes produces no completion line, so the tracer also
// carries WHO is currently in flight: ProcessBlock publishes (hash,height,start)
// on entry and clears it on exit, while a low-frequency ticker emits one line per
// stall window. That is what names a 20-minute acceptance hold while it happens,
// without perf/gdb and without touching the block path's semantics.
struct InFlight
{
    std::atomic<bool> active;
    std::atomic<uint64_t> startUs;
    std::atomic<uint64_t> lastReportUs;
    std::atomic<uint64_t> startOps;
    std::atomic<uint64_t> startBytes;
    std::atomic<int> height;
    std::atomic<int> stage;
    InFlight() : active(false), startUs(0), lastReportUs(0), startOps(0), startBytes(0), height(-1), stage(0) {}
};

inline InFlight& Current()
{
    static InFlight c;
    return c;
}

inline void BeginBlock(const std::string& hash, int height)
{
    InFlight& c = Current();
    const ReadSnapshot s = Snapshot();
    c.startOps.store(s.ops, std::memory_order_relaxed);
    c.startBytes.store(s.bytes, std::memory_order_relaxed);
    c.height.store(height, std::memory_order_relaxed);
    c.startUs.store(NowUs(), std::memory_order_relaxed);
    c.lastReportUs.store(0, std::memory_order_relaxed);
    c.stage.store(1, std::memory_order_relaxed);
    { std::lock_guard<std::mutex> g(HashLock()); CurrentHash() = hash; }
    c.active.store(true, std::memory_order_relaxed);
}

inline void EndBlock()
{
    Current().active.store(false, std::memory_order_relaxed);
}

inline std::atomic<int>& Stage() { return Current().stage; }
inline void SetStage(int s) { Stage().store(s, std::memory_order_relaxed); }

// ---- Stall ticker ----------------------------------------------------------
//
// One detached thread, started only when -blockaccepttrace is on. Every
// intervalMs it looks at the in-flight block; if it has been in flight longer
// than stallThresholdMs it emits ONE line per repeatMs window naming the stage
// and the record-store reads accumulated so far. This is the piece that makes a
// 24-minute hold self-reporting instead of an inference.
inline void StartTicker(unsigned intervalMs = 2000, unsigned stallThresholdMs = 5000,
                        unsigned repeatMs = 30000)
{
    static std::atomic<bool> started(false);
    bool expected = false;
    if (!started.compare_exchange_strong(expected, true))
        return;
    std::thread th([intervalMs, stallThresholdMs, repeatMs]() {
        for (;;) {
            struct timespec req;
            req.tv_sec = intervalMs / 1000;
            req.tv_nsec = (long)(intervalMs % 1000) * 1000000L;
            nanosleep(&req, NULL);
            if (!Enabled()) continue;
            InFlight& c = Current();
            if (!c.active.load(std::memory_order_relaxed)) continue;
            const uint64_t now = NowUs();
            const uint64_t start = c.startUs.load(std::memory_order_relaxed);
            uint64_t last = c.lastReportUs.load(std::memory_order_relaxed);
            if (now - start < (uint64_t)stallThresholdMs * 1000ULL) continue;
            if (last != 0 && now - last < (uint64_t)repeatMs * 1000ULL) continue;
            if (!c.lastReportUs.compare_exchange_strong(last, now)) continue;
            const ReadSnapshot s = Snapshot();
            std::string h;
            { std::lock_guard<std::mutex> g(HashLock()); h = CurrentHash(); }
            printf("BLOCKACCTRACE_STALL hash=%s height=%d in_flight_us=%llu stage=%s "
                   "reads_ops=%llu reads_bytes=%llu slow_read_us=%llu\n",
                   h.c_str(), c.height.load(std::memory_order_relaxed),
                   (unsigned long long)(now - start),
                   StageName(c.stage.load(std::memory_order_relaxed)),
                   (unsigned long long)(s.ops - c.startOps.load(std::memory_order_relaxed)),
                   (unsigned long long)(s.bytes - c.startBytes.load(std::memory_order_relaxed)),
                   (unsigned long long)s.slowUs);
            fflush(stdout);
        }
    });
    th.detach();
}

} // namespace blockacctrace
