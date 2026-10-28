#include "p2pprop.h"

#include <cstdio>
#include <deque>
#include <string>

#ifdef P2PPROP_SELFTEST
// Isolated self-test harness: stub the util logging/clock so this module can
// be compiled+run WITHOUT the full daemon object graph. The production build
// never defines P2PPROP_SELFTEST and uses the real util.h path below.
#include <stdint.h>
static std::string g_sink;
static int64_t g_clock = 1234567890123456LL;
#define P2PPROP_OUT(fmt, ...) do { char _b[2048]; std::snprintf(_b, sizeof(_b), fmt, __VA_ARGS__); g_sink += _b; } while (0)
static inline int64_t GetTimeMicros() { return g_clock; }
#else
#include "util.h" // OutputDebugStringF (unconditional debug.log sink), GetTimeMicros
// Route telemetry through OutputDebugStringF (the same unconditional-to-debug.log
// vector the core uses) instead of LogPrintf/LogPrintStr, because LogPrintStr only
// flushes to the file when -debug is enabled; our nodes run with -debug off.
#define P2PPROP_OUT OutputDebugStringF
#endif

namespace p2pprop {

// Disabled by default; only turned on by an explicit -p2pprop=1.
static bool g_enabled = false;

// Bounded ring of the most recent emissions (empty when disabled). Kept so the
// telemetry can be verified deterministically in isolation and inspected live;
// allocation only occurs after SetEnabled(true).
static const size_t EMISSION_CAP = 4096;
static std::deque<std::string> g_emissions;

void SetEnabled(bool fEnabled)
{
    g_enabled = fEnabled;
    if (fEnabled)
        g_emissions.clear();
    P2PPROP_OUT("[P2PPROP] SetEnabled(enabled=%d)\n", (int)fEnabled);
}

bool Enabled()
{
    return g_enabled;
}

size_t EmissionCount()
{
    return g_emissions.size();
}

std::string EmissionAt(size_t i)
{
    if (i >= g_emissions.size())
        return std::string();
    return g_emissions[i];
}

void ClearEmissionsForTest()
{
    g_emissions.clear();
}

// Form the line and (if enabled) record it + LogPrintf it. Early-returns
// entirely (no allocation, no clock read) when telemetry is off.
static void Emit(const char* stage, const uint256& hash, int peer,
                 const char* extra)
{
    if (!g_enabled)
        return;
    char line[512];
    std::snprintf(line, sizeof(line),
                  "[P2PPROP] stage=%s hash=%s peer=%d wall_us=%lld %s",
                  stage, hash.ToString().c_str(), peer,
                  (long long)GetTimeMicros(), extra);
    g_emissions.push_back(std::string(line));
    if (g_emissions.size() > EMISSION_CAP)
        g_emissions.pop_front();
    P2PPROP_OUT("%s\n", line);
}

void RecordInvReceived(const uint256& hash, int peer)
{
    Emit("inv_received", hash, peer, "");
}

void RecordGetDataSent(const uint256& hash, int peer, bool fRetry)
{
    char buf[24];
    std::snprintf(buf, sizeof(buf), "retry=%d", (int)(fRetry ? 1 : 0));
    Emit("getdata_sent", hash, peer, buf);
}

void RecordBlockReceived(const uint256& hash, int peer, uint64_t bytes)
{
    char buf[48];
    std::snprintf(buf, sizeof(buf), "bytes=%llu",
                  (unsigned long long)bytes);
    Emit("block_received", hash, peer, buf);
}

void RecordBlockAccepted(const uint256& hash, int64_t height,
                         int64_t writeDiskMs, int64_t addIndexMs)
{
    char buf[96];
    std::snprintf(buf, sizeof(buf),
                  "height=%lld write_disk_ms=%lld add_index_ms=%lld",
                  (long long)height, (long long)writeDiskMs,
                  (long long)addIndexMs);
    Emit("block_accepted", hash, -1, buf);
}

void RecordAnnounce(const uint256& hash, int peer)
{
    Emit("announce_sent", hash, peer, "");
}

void RecordGetDataWire(const uint256& hash, int peer)
{
    Emit("getdata_wire", hash, peer, "wire_evt=first_byte");
}

void RecordGetDataReceived(const uint256& hash, int peer)
{
    Emit("getdata_received", hash, peer, "");
}

void RecordBlockServed(const uint256& hash, int peer)
{
    Emit("block_served", hash, peer, "");
}

} // namespace p2pprop