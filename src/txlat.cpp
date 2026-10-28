// txlat.cpp - see txlat.h
#include "txlat.h"
#include <chrono>
#include <atomic>
#include <cstdio>
#include <cinttypes>
#ifdef TXLAT_SELFTEST
#include <cstdarg>
static void TxlSink(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); vfprintf(stdout, fmt, ap); va_end(ap);
}
#define TXL_OUT TxlSink
#else
#include "util.h"
#define TXL_OUT OutputDebugStringF
#endif

namespace txlat {
namespace {
std::atomic<bool> g_enabled(false);
thread_local uint64_t g_t0 = 0;
thread_local uint64_t g_last = 0;
} // namespace

void SetEnabled(bool on) {
    g_enabled.store(on);
    TXL_OUT("[TXLAT] SetEnabled(enabled=%d)\n", on ? 1 : 0);
}
bool Enabled() { return g_enabled.load(); }

uint64_t MonotonicMs() {
    auto now = std::chrono::steady_clock::now();
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
               now.time_since_epoch()).count();
}

void Begin(const char* name) {
    if (!g_enabled.load()) return;
    g_t0 = MonotonicMs();
    g_last = g_t0;
    TXL_OUT("[TXLAT] phase=begin name=%s ms=%" PRIu64 "\n", name, g_t0);
}

void Mark(const char* name, int64_t a, int64_t b) {
    if (!g_enabled.load()) return;
    uint64_t now = MonotonicMs();
    if (g_t0 == 0) g_t0 = now;
    TXL_OUT("[TXLAT] phase=%s ms=%" PRIu64 " rpc_ms=%" PRIu64 " del_ms=%" PRIu64
            " a=%" PRId64 " b=%" PRId64 "\n",
            name, now, now - g_t0, now - g_last, a, b);
    g_last = now;
}
} // namespace txlat