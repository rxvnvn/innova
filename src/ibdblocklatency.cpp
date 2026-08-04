// Copyright (c) 2026 The Innova developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "ibdblocklatency.h"

#include "ibdmetrics.h"
#include "sync.h"
#include "util.h"

#include <algorithm>
#include <cstdio>
#include <map>

namespace ibdblocklatency {

namespace {

bool g_enabled = false;
std::string g_csvPath;

// Last IBD_BLOCKLAT_1S emission time (wall microseconds).
int64_t g_lastEmitUs = 0;

// Streaming CSV: opened once in SetEnabled (AppInit2) with fully buffered
// stdio, one row written per terminal outcome, no per-row fflush, closed at
// Shutdown (Dump).  NULL when disabled or when no path was configured.  Only
// ever accessed under cs.
FILE* g_csvFile = NULL;
static const size_t CSV_BUFFER_BYTES = 1 << 20;  // 1 MiB stdio buffer

// Bounded in-memory state.  g_samples is the connected-active aggregation
// window (fixed size, feeds IBD_BLOCKLAT_1S and the shutdown summary); it is
// NOT the CSV source, so it imposes no row-count limit.  g_records holds only
// incomplete (non-terminal) lifecycles.  Lifetime outcome counters and
// interval totals are exact and never bounded.
static const size_t SAMPLE_CAPACITY = 16384;
static const size_t RECORD_CAPACITY = 16384;
static const int64_t RECORD_STALE_US = 300 * 1000000;

std::vector<BlockLatencySample> g_samples;   // reserve(SAMPLE_CAPACITY)
size_t g_samplesUsed = 0;
size_t g_samplesNext = 0;
int64_t g_totalUs[BLOCKLAT_NUM_INTERVALS] = {0};
int64_t g_totalCount[BLOCKLAT_NUM_INTERVALS] = {0};

// Funnel counters: how many received blocks reach each stage, plus the
// lifetime terminal-outcome tallies (exact, never evicted).
int64_t g_receivedTotal = 0;
int64_t g_receivedUnsolicited = 0;
int64_t g_processedTotal = 0;
int64_t g_acceptedTotal = 0;
int64_t g_orphanedTotal = 0;
int64_t g_incompleteDropped = 0;
int64_t g_outcomeCount[OUTCOME_COUNT] = {0};

struct LatencyRecord
{
    bool fActive;
    bool fOrphaned;
    int requestPeer;
    int receivePeer;
    int64_t blockSize;
    int64_t pingMs;
    int64_t requestPeerPressure;
    int64_t globalInflight;
    int64_t globalQueued;
    int64_t globalDeferred;
    int64_t t0, t1, t2, t3, t4, t5, t6, t7;
    int64_t tLast;

    LatencyRecord()
        : fActive(false), fOrphaned(false), requestPeer(-1), receivePeer(-1),
          blockSize(0), pingMs(-1), requestPeerPressure(0), globalInflight(0),
          globalQueued(0), globalDeferred(0), t0(0), t1(0), t2(0), t3(0),
          t4(0), t5(0), t6(0), t7(0), tLast(0)
    {
    }
};

std::map<uint256, LatencyRecord> g_records;

// Leaf lock: never held while acquiring any other lock.  All hooks run on the
// message-handler thread (plus the socket-handler thread for the timeout and
// disconnect terminal hooks), so this is a cheap correctness guard only.
CCriticalSection cs;

int64_t Diff(int64_t a, int64_t b)
{
    if (a <= 0 || b <= 0)
        return -1;
    return std::max<int64_t>(0, a - b);
}

void PushSampleLocked(std::vector<BlockLatencySample>& v, size_t& nUsed,
                      size_t& nNext, size_t nCapacity,
                      const BlockLatencySample& s)
{
    if (nUsed < nCapacity)
    {
        v.push_back(s);
        ++nUsed;
    }
    else
    {
        v[nNext] = s;
    }
    nNext = (nNext + 1) % nCapacity;
}

// Build a sample from a record at its terminal moment.  nTerminalUs is the
// wall time of the terminal event (used for the TOTAL interval when T7 was
// never reached).  Missing stamps yield -1 intervals.
BlockLatencySample SampleFromRecordLocked(const uint256& hash,
                                          const LatencyRecord& r,
                                          int outcome, int64_t nHeight,
                                          int64_t nTerminalUs)
{
    BlockLatencySample s;
    s.hash = hash;
    s.outcome = outcome;
    s.fOrphaned = r.fOrphaned ? 1 : 0;
    s.requestPeer = r.requestPeer;
    s.receivePeer = r.receivePeer;
    s.blockSize = r.blockSize;
    s.height = nHeight;
    s.pingMs = r.pingMs;
    s.requestPeerPressure = r.requestPeerPressure;
    s.globalInflight = r.globalInflight;
    s.globalQueued = r.globalQueued;
    s.globalDeferred = r.globalDeferred;
    s.intervalUs[BLOCKLAT_INTERVAL_ASKFOR_TO_GETDATA] = Diff(r.t1, r.t0);
    s.intervalUs[BLOCKLAT_INTERVAL_GETDATA_TO_RECEIVE] = Diff(r.t2, r.t1);
    s.intervalUs[BLOCKLAT_INTERVAL_RECEIVE_TO_PROCESS] = Diff(r.t3, r.t2);
    s.intervalUs[BLOCKLAT_INTERVAL_PROCESS_TO_ACCEPT] = Diff(r.t4, r.t3);
    s.intervalUs[BLOCKLAT_INTERVAL_ACCEPT_TO_INDEX] = Diff(r.t5, r.t4);
    s.intervalUs[BLOCKLAT_INTERVAL_INDEX_TO_BEST] = Diff(r.t6, r.t5);
    s.intervalUs[BLOCKLAT_INTERVAL_BEST_TO_CONNECT] = Diff(r.t7, r.t6);
    s.intervalUs[BLOCKLAT_INTERVAL_TOTAL] = Diff(r.t7, r.t0);
    if (s.intervalUs[BLOCKLAT_INTERVAL_TOTAL] < 0)
        s.intervalUs[BLOCKLAT_INTERVAL_TOTAL] = Diff(nTerminalUs, r.t0);
    for (int i = 0; i < BLOCKLAT_NUM_INTERVALS; ++i)
    {
        if (s.intervalUs[i] >= 0)
        {
            g_totalUs[i] += s.intervalUs[i];
            ++g_totalCount[i];
        }
    }
    ++g_outcomeCount[outcome];
    return s;
}

// Stream one completed lifecycle to the buffered CSV.  Fully buffered stdio:
// no fflush here, data lands on disk only when the 1 MiB buffer fills or the
// file is closed at Shutdown.
void WriteSampleRowLocked(FILE* f, const BlockLatencySample& s)
{
    fprintf(f, "%d,%d,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%s,%d,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%s\n",
            s.requestPeer, s.receivePeer, (long long)s.height,
            (long long)s.blockSize, (long long)s.pingMs,
            (long long)s.requestPeerPressure,
            (long long)s.globalInflight, (long long)s.globalQueued,
            (long long)s.globalDeferred,
            OutcomeName(s.outcome), s.fOrphaned,
            (long long)s.intervalUs[0], (long long)s.intervalUs[1],
            (long long)s.intervalUs[2], (long long)s.intervalUs[3],
            (long long)s.intervalUs[4], (long long)s.intervalUs[5],
            (long long)s.intervalUs[6], (long long)s.intervalUs[7],
            s.hash.ToString().c_str());
}

void WriteRowLocked(const BlockLatencySample& s)
{
    if (g_csvFile == NULL)
        return;
    WriteSampleRowLocked(g_csvFile, s);
}

// Write the CSV header once, immediately after opening.
void WriteCsvHeaderLocked(FILE* f)
{
    fprintf(f,
            "#ibdblocklatency: per-block GETDATA->CONNECT decomposition\n"
            "# request_peer,receive_peer,height,size,ping_ms,req_peer_pressure,"
            "global_inflight,global_queued,global_deferred,outcome,orphaned,"
            "askfor_to_getdata_us,getdata_to_recv_us,recv_to_process_us,"
            "process_to_accept_us,accept_to_index_us,index_to_best_us,"
            "best_to_connect_us,total_us,hash\n");
}

void PruneStaleLocked(int64_t nNow)
{
    const int64_t nCutoff = nNow - RECORD_STALE_US;
    for (std::map<uint256, LatencyRecord>::iterator it = g_records.begin();
         it != g_records.end();)
    {
        if (it->second.fActive && it->second.tLast < nCutoff)
        {
            const uint256 hash = it->first;
            LatencyRecord r = it->second;
            ++g_incompleteDropped;
            it = g_records.erase(it);
            const BlockLatencySample s = SampleFromRecordLocked(
                hash, r, OUTCOME_INCOMPLETE_EVICTED, -1, nNow);
            WriteRowLocked(s);
        }
        else
        {
            ++it;
        }
    }
}

struct IntervalAgg
{
    int64_t meanUs;
    int64_t medianUs;
    int64_t p95Us;
    int64_t maxUs;
    int64_t count;
};

IntervalAgg AggregateLocked(int nInterval)
{
    IntervalAgg a;
    a.meanUs = a.medianUs = a.p95Us = a.maxUs = 0;
    a.count = 0;
    std::vector<int64_t> v;
    v.reserve(g_samplesUsed);
    for (size_t i = 0; i < g_samplesUsed; ++i)
    {
        const BlockLatencySample& s = g_samples[i];
        if (s.outcome != OUTCOME_CONNECTED_ACTIVE)
            continue;
        const int64_t x = s.intervalUs[nInterval];
        if (x >= 0)
            v.push_back(x);
    }
    if (v.empty())
        return a;
    std::sort(v.begin(), v.end());
    int64_t nSum = 0;
    for (size_t i = 0; i < v.size(); ++i)
        nSum += v[i];
    a.count = (int64_t)v.size();
    a.meanUs = nSum / (int64_t)v.size();
    a.medianUs = v[v.size() / 2];
    a.p95Us = v[(size_t)((v.size() - 1) * 0.95 + 0.5)];
    a.maxUs = v.back();
    return a;
}

} // namespace

void SetEnabled(bool fEnabled, const std::string& strCsvPath)
{
    LOCK(cs);
    if (g_csvFile != NULL)
    {
        fclose(g_csvFile);
        g_csvFile = NULL;
    }
    g_enabled = fEnabled;
    g_csvPath = strCsvPath;
    if (fEnabled)
    {
        g_samples.reserve(SAMPLE_CAPACITY);
        if (!strCsvPath.empty())
        {
            g_csvFile = fopen(strCsvPath.c_str(), "w");
            if (g_csvFile == NULL)
            {
                fprintf(stderr, "ibdblocklatency: cannot open CSV '%s'\n",
                        strCsvPath.c_str());
            }
            else
            {
                setvbuf(g_csvFile, NULL, _IOFBF, CSV_BUFFER_BYTES);
                WriteCsvHeaderLocked(g_csvFile);
            }
        }
        g_lastEmitUs = 0;
    }
}

bool Enabled()
{
    return g_enabled;
}

void ResetForTesting()
{
    LOCK(cs);
    if (g_csvFile != NULL)
    {
        fclose(g_csvFile);
        g_csvFile = NULL;
    }
    g_records.clear();
    g_samples.clear();
    g_samplesUsed = 0;
    g_samplesNext = 0;
    for (int i = 0; i < BLOCKLAT_NUM_INTERVALS; ++i)
    {
        g_totalUs[i] = 0;
        g_totalCount[i] = 0;
    }
    g_receivedTotal = 0;
    g_receivedUnsolicited = 0;
    g_processedTotal = 0;
    g_acceptedTotal = 0;
    g_orphanedTotal = 0;
    g_incompleteDropped = 0;
    for (int i = 0; i < OUTCOME_COUNT; ++i)
        g_outcomeCount[i] = 0;
    g_lastEmitUs = 0;
}

size_t SampleCountForTesting()
{
    LOCK(cs);
    return g_samplesUsed;
}

const std::vector<BlockLatencySample>& SamplesForTesting()
{
    LOCK(cs);
    return g_samples;
}

void FlushCsvForTesting()
{
    LOCK(cs);
    if (g_csvFile != NULL)
        fflush(g_csvFile);
}

int64_t ReceivedForTesting()
{
    LOCK(cs);
    return g_receivedTotal;
}

int64_t UnsolicitedForTesting()
{
    LOCK(cs);
    return g_receivedUnsolicited;
}

int64_t ProcessedForTesting()
{
    LOCK(cs);
    return g_processedTotal;
}

int64_t ConnectedForTesting()
{
    LOCK(cs);
    return g_acceptedTotal;
}

int64_t OrphanedForTesting()
{
    LOCK(cs);
    return g_orphanedTotal;
}

int64_t OutcomeCountForTesting(int outcome)
{
    LOCK(cs);
    if (outcome < 0 || outcome >= OUTCOME_COUNT)
        return 0;
    return g_outcomeCount[outcome];
}

void RecordAskForEnqueue(const uint256& hash, int peer, int64_t peerPressure)
{
    if (!g_enabled)
        return;
    const int64_t nNow = GetTimeMicros();
    LOCK(cs);
    if (g_records.size() >= RECORD_CAPACITY)
        PruneStaleLocked(nNow);
    LatencyRecord& r = g_records[hash];
    if (!r.fActive)
    {
        r.fActive = true;
        r.requestPeer = peer;
        r.requestPeerPressure = peerPressure;
    }
    r.t0 = nNow;
    r.tLast = nNow;
}

void RecordGetDataSent(const uint256& hash, int peer)
{
    if (!g_enabled)
        return;
    const int64_t nNow = GetTimeMicros();
    LOCK(cs);
    std::map<uint256, LatencyRecord>::iterator it = g_records.find(hash);
    if (it == g_records.end() || !it->second.fActive)
        return;
    it->second.t1 = nNow;
    it->second.tLast = nNow;
}

void RecordBlockReceived(const uint256& hash, int receivePeer,
                         int64_t nBlockSize, int64_t nTimeReceivedUs,
                         int64_t nPingUsec)
{
    if (!g_enabled)
        return;
    const int64_t nNow = GetTimeMicros();
    LOCK(cs);
    if (g_records.size() >= RECORD_CAPACITY)
        PruneStaleLocked(nNow);
    ++g_receivedTotal;
    std::map<uint256, LatencyRecord>::iterator it = g_records.find(hash);
    if (it == g_records.end() || !it->second.fActive)
        ++g_receivedUnsolicited;
    LatencyRecord& r = g_records[hash];
    if (!r.fActive)
    {
        r.fActive = true;
        r.requestPeer = -1;
    }
    r.receivePeer = receivePeer;
    r.blockSize = nBlockSize;
    r.pingMs = nPingUsec > 0 ? nPingUsec / 1000 : -1;
    r.t2 = nTimeReceivedUs;
    r.tLast = nNow;
    ibdmetrics::Counters& mc = ibdmetrics::Get();
    r.globalInflight = mc.total_inflight_current.load(std::memory_order_relaxed);
    r.globalQueued = mc.total_queued_current.load(std::memory_order_relaxed);
    r.globalDeferred = mc.total_deferred_current.load(std::memory_order_relaxed);
}

void RecordProcessBlockBegin(const uint256& hash)
{
    if (!g_enabled)
        return;
    const int64_t nNow = GetTimeMicros();
    LOCK(cs);
    std::map<uint256, LatencyRecord>::iterator it = g_records.find(hash);
    if (it == g_records.end() || !it->second.fActive)
        return;
    ++g_processedTotal;
    it->second.t3 = nNow;
    it->second.tLast = nNow;
}

void RecordAcceptBlockBegin(const uint256& hash)
{
    if (!g_enabled)
        return;
    const int64_t nNow = GetTimeMicros();
    LOCK(cs);
    std::map<uint256, LatencyRecord>::iterator it = g_records.find(hash);
    if (it == g_records.end() || !it->second.fActive)
        return;
    it->second.t4 = nNow;
    it->second.tLast = nNow;
}

void RecordAddToBlockIndexBegin(const uint256& hash)
{
    if (!g_enabled)
        return;
    const int64_t nNow = GetTimeMicros();
    LOCK(cs);
    std::map<uint256, LatencyRecord>::iterator it = g_records.find(hash);
    if (it == g_records.end() || !it->second.fActive)
        return;
    it->second.t5 = nNow;
    it->second.tLast = nNow;
}

void RecordSetBestChainBegin(const uint256& hash)
{
    if (!g_enabled)
        return;
    const int64_t nNow = GetTimeMicros();
    LOCK(cs);
    std::map<uint256, LatencyRecord>::iterator it = g_records.find(hash);
    if (it == g_records.end() || !it->second.fActive)
        return;
    it->second.t6 = nNow;
    it->second.tLast = nNow;
}

void RecordBlockConnected(const uint256& hash, int64_t nConnectedHeight)
{
    if (!g_enabled)
        return;
    const int64_t nNow = GetTimeMicros();
    LOCK(cs);
    std::map<uint256, LatencyRecord>::iterator it = g_records.find(hash);
    if (it == g_records.end() || !it->second.fActive)
        return;
    LatencyRecord& r = it->second;
    r.t7 = nNow;

    const BlockLatencySample s = SampleFromRecordLocked(
        hash, r, OUTCOME_CONNECTED_ACTIVE, nConnectedHeight, nNow);
    ++g_acceptedTotal;
    WriteRowLocked(s);
    PushSampleLocked(g_samples, g_samplesUsed, g_samplesNext,
                     SAMPLE_CAPACITY, s);
    g_records.erase(it);
}
void RecordBlockOrphaned(const uint256& hash)
{
    if (!g_enabled)
        return;
    const int64_t nNow = GetTimeMicros();
    LOCK(cs);
    std::map<uint256, LatencyRecord>::iterator it = g_records.find(hash);
    if (it == g_records.end() || !it->second.fActive)
        return;
    ++g_orphanedTotal;
    it->second.fOrphaned = true;
    it->second.tLast = nNow;
}

void RecordBlockAcceptedSide(const uint256& hash, int64_t nBlockHeight)
{
    if (!g_enabled)
        return;
    const int64_t nNow = GetTimeMicros();
    LOCK(cs);
    std::map<uint256, LatencyRecord>::iterator it = g_records.find(hash);
    if (it == g_records.end() || !it->second.fActive)
        return;
    LatencyRecord r = it->second;
    g_records.erase(it);
    const BlockLatencySample s = SampleFromRecordLocked(
        hash, r, OUTCOME_ACCEPTED_SIDE, nBlockHeight, nNow);
    WriteRowLocked(s);
}

void RecordBlockTerminal(const uint256& hash, int outcome, int64_t nHeight)
{
    if (!g_enabled)
        return;
    if (outcome < 0 || outcome >= OUTCOME_COUNT)
        return;
    const int64_t nNow = GetTimeMicros();
    LOCK(cs);
    std::map<uint256, LatencyRecord>::iterator it = g_records.find(hash);
    if (it == g_records.end() || !it->second.fActive)
        return;
    LatencyRecord r = it->second;
    g_records.erase(it);
    const BlockLatencySample s = SampleFromRecordLocked(
        hash, r, outcome, nHeight, nNow);
    WriteRowLocked(s);
}

const char* OutcomeName(int outcome)
{
    switch (outcome)
    {
    case OUTCOME_CONNECTED_ACTIVE: return "connected_active";
    case OUTCOME_ACCEPTED_SIDE: return "accepted_side";
    case OUTCOME_ALREADY_HAVE: return "already_have_duplicate";
    case OUTCOME_REJECTED: return "rejected";
    case OUTCOME_TIMEOUT: return "timeout";
    case OUTCOME_DISCONNECT: return "disconnect";
    case OUTCOME_INCOMPLETE_EVICTED: return "incomplete_evicted";
    }
    return "unknown";
}

static const char* IntervalName(int nInterval)
{
    switch (nInterval)
    {
    case BLOCKLAT_INTERVAL_ASKFOR_TO_GETDATA: return "askfor_to_getdata";
    case BLOCKLAT_INTERVAL_GETDATA_TO_RECEIVE: return "getdata_to_recv";
    case BLOCKLAT_INTERVAL_RECEIVE_TO_PROCESS: return "recv_to_process";
    case BLOCKLAT_INTERVAL_PROCESS_TO_ACCEPT: return "process_to_accept";
    case BLOCKLAT_INTERVAL_ACCEPT_TO_INDEX: return "accept_to_index";
    case BLOCKLAT_INTERVAL_INDEX_TO_BEST: return "index_to_best";
    case BLOCKLAT_INTERVAL_BEST_TO_CONNECT: return "best_to_connect";
    case BLOCKLAT_INTERVAL_TOTAL: return "total";
    }
    return "unknown";
}

static void PrintFunnelLineLocked(int64_t nNow)
{
    printf("IBD_BLOCKLAT_1S time_us=%lld samples=%lld lifetime=%lld "
           "funnel_received=%lld funnel_unsolicited=%lld funnel_processed=%lld "
           "funnel_connected=%lld funnel_orphaned=%lld "
           "funnel_incomplete_dropped=%lld "
           "outcome_connected_active=%lld outcome_accepted_side=%lld "
           "outcome_already_have=%lld outcome_rejected=%lld outcome_timeout=%lld "
           "outcome_disconnect=%lld outcome_incomplete_evicted=%lld ",
           (long long)nNow, (long long)g_samplesUsed,
           (long long)g_acceptedTotal, (long long)g_receivedTotal,
           (long long)g_receivedUnsolicited, (long long)g_processedTotal,
           (long long)g_acceptedTotal, (long long)g_orphanedTotal,
           (long long)g_incompleteDropped,
           (long long)g_outcomeCount[OUTCOME_CONNECTED_ACTIVE],
           (long long)g_outcomeCount[OUTCOME_ACCEPTED_SIDE],
           (long long)g_outcomeCount[OUTCOME_ALREADY_HAVE],
           (long long)g_outcomeCount[OUTCOME_REJECTED],
           (long long)g_outcomeCount[OUTCOME_TIMEOUT],
           (long long)g_outcomeCount[OUTCOME_DISCONNECT],
           (long long)g_outcomeCount[OUTCOME_INCOMPLETE_EVICTED]);
}

static void PrintAggregateLineLocked()
{
    IntervalAgg a[BLOCKLAT_NUM_INTERVALS];
    for (int i = 0; i < BLOCKLAT_NUM_INTERVALS; ++i)
        a[i] = AggregateLocked(i);
    PrintFunnelLineLocked(GetTimeMicros());
    for (int i = 0; i < BLOCKLAT_NUM_INTERVALS; ++i)
        printf("%s={mean=%lld,med=%lld,p95=%lld,max=%lld,n=%lld} ",
               IntervalName(i), (long long)a[i].meanUs,
               (long long)a[i].medianUs, (long long)a[i].p95Us,
               (long long)a[i].maxUs, (long long)a[i].count);
    printf("\n");
}

void EmitIBDBlockLatency1s()
{
    if (!g_enabled)
        return;
    const int64_t nNow = GetTimeMicros();
    if (nNow - g_lastEmitUs < 1000000)
        return;
    g_lastEmitUs = nNow;
    LOCK(cs);
    if (g_samplesUsed == 0)
    {
        PrintFunnelLineLocked(nNow);
        printf("\n");
        return;
    }
    PrintAggregateLineLocked();
}

static void WriteCsvFooterLocked(FILE* f)
{
    fprintf(f, "# OUTCOME_SUMMARY connected_active=%lld accepted_side=%lld "
               "already_have_duplicate=%lld orphaned=%lld rejected=%lld "
               "timeout=%lld disconnect=%lld incomplete_evicted=%lld "
               "unsolicited=%lld\n",
            (long long)g_outcomeCount[OUTCOME_CONNECTED_ACTIVE],
            (long long)g_outcomeCount[OUTCOME_ACCEPTED_SIDE],
            (long long)g_outcomeCount[OUTCOME_ALREADY_HAVE],
            (long long)g_orphanedTotal,
            (long long)g_outcomeCount[OUTCOME_REJECTED],
            (long long)g_outcomeCount[OUTCOME_TIMEOUT],
            (long long)g_outcomeCount[OUTCOME_DISCONNECT],
            (long long)g_outcomeCount[OUTCOME_INCOMPLETE_EVICTED],
            (long long)g_receivedUnsolicited);
}

void Dump()
{
    if (!g_enabled)
        return;
    LOCK(cs);

    // Reclaim any still-open records as incomplete_evicted so the CSV and
    // summary account for every tracked lifecycle at shutdown.  Rows stream
    // to the open file; the footer is appended and the file closed (flushing
    // the 1 MiB stdio buffer) below.
    const int64_t nNow = GetTimeMicros();
    for (std::map<uint256, LatencyRecord>::iterator it = g_records.begin();
         it != g_records.end();)
    {
        const uint256 hash = it->first;
        LatencyRecord r = it->second;
        ++g_incompleteDropped;
        it = g_records.erase(it);
        const BlockLatencySample s = SampleFromRecordLocked(
            hash, r, OUTCOME_INCOMPLETE_EVICTED, -1, nNow);
        WriteRowLocked(s);
    }

    if (g_csvFile != NULL)
    {
        WriteCsvFooterLocked(g_csvFile);
        fclose(g_csvFile);
        g_csvFile = NULL;
        printf("IBD_BLOCKLAT_DUMP wrote=%s\n", g_csvPath.c_str());
    }

    // Summary with lifetime outcome tallies, lifetime means and ring
    // percentiles for connected-active blocks.
    printf("IBD_BLOCKLAT_SUMMARY samples=%lld received=%lld unsolicited=%lld "
           "processed=%lld connected=%lld orphaned=%lld incomplete_dropped=%lld\n",
           (long long)g_samplesUsed, (long long)g_receivedTotal,
           (long long)g_receivedUnsolicited, (long long)g_processedTotal,
           (long long)g_acceptedTotal, (long long)g_orphanedTotal,
           (long long)g_incompleteDropped);
    for (int i = 0; i < OUTCOME_COUNT; ++i)
        printf("IBD_BLOCKLAT_OUTCOME %s=%lld\n", OutcomeName(i),
               (long long)g_outcomeCount[i]);
    for (int i = 0; i < BLOCKLAT_NUM_INTERVALS; ++i)
    {
        IntervalAgg a = AggregateLocked(i);
        const int64_t nLifetimeMean = g_totalCount[i] > 0
            ? g_totalUs[i] / g_totalCount[i] : 0;
        printf("IBD_BLOCKLAT_INTERVAL %s lifetime_mean_us=%lld lifetime_n=%lld "
               "ring_mean_us=%lld ring_median_us=%lld ring_p95_us=%lld "
               "ring_max_us=%lld ring_n=%lld\n",
               IntervalName(i), (long long)nLifetimeMean,
               (long long)g_totalCount[i], (long long)a.meanUs,
               (long long)a.medianUs, (long long)a.p95Us, (long long)a.maxUs,
               (long long)a.count);
    }
}

} // namespace ibdblocklatency
