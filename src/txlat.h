// txlat.h - opt-in wallet transaction-creation latency tracing.
// OFF by default. Emits [TXLAT] lines via OutputDebugStringF (unconditional
// sink, util.h:?49) only when enabled. Does not change any transaction
// behavior, coin selection, fees, serialization or consensus.
// Uses a monotonic clock for intervals within one send attempt.
#ifndef INNOVA_TXLAT_H
#define INNOVA_TXLAT_H
#include <cstdint>
namespace txlat {
// Thread-safe control. OFF by default.
void SetEnabled(bool on);
bool Enabled();
uint64_t MonotonicMs();            // steady_clock, ms since process start
// Timestamped phase mark. name is a short ascii label; a/b optional counters.
// Logs: [TXLAT] phase=<name> ms=<monotonic_ms> rpc_ms=<since Begin> del_ms=<since last Mark> a=<a> b=<b>
void Mark(const char* name, int64_t a = -1, int64_t b = -1);
// Start anchor for a send attempt (resets rpc_ms baseline). Reentrant-safe:
// stores per-thread start.
void Begin(const char* name);
} // namespace txlat
#endif // INNOVA_TXLAT_H