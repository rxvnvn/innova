// Copyright (c) 2026 The Innova developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "ibdsemantic.h"

#include <stdlib.h>

#include <algorithm>
#include <deque>
#include <iomanip>
#include <sstream>
#include <vector>

#include "ibdactivepath.h"
#include "ibdmetrics.h"
#include "main.h"
#include "net.h"
#include "sync.h"
#include "util.h"
#include "version.h"

// Arm-only semantic IBD health diagnostics.
//
// See the header comment.  Everything here is observation only: nothing in
// this translation unit queues requests, mutates request ownership, schedules
// recovery, or changes IBD/recovery behavior.

namespace ibdsemantic {

namespace {

// ---------------------------------------------------------------------------
// Configuration (loaded from mapArgs by SetEnabled; overridable in tests).
// ---------------------------------------------------------------------------

static int g_nMode = 0;
static double g_dMinProgressRate = 0.2;
static double g_dOrphanDom = 0.5;
static double g_dFutileReject = 0.2;
static int64_t g_nGapMin = 5000;

static bool ParseDoubleConfig(const std::string& str, double& dOut)
{
    if (str.empty())
        return false;
    char* pEnd = NULL;
    dOut = strtod(str.c_str(), &pEnd);
    if (pEnd == NULL || *pEnd != '\0')
        return false;
    return true;
}

static double LoadDouble(const char* pszArg, double dDefault, double dMin,
                         double dMax)
{
    double d = dDefault;
    if (mapArgs.count(pszArg) &&
        ParseDoubleConfig(mapArgs[pszArg], d))
    {
        d = std::max(dMin, std::min(dMax, d));
    }
    return d;
}

// ---------------------------------------------------------------------------
// Cumulative counters and gauges (relaxed atomics; no new hot lock).
// ---------------------------------------------------------------------------

struct Counters
{
    std::atomic<int64_t> semantic_checks_total;
    std::atomic<int64_t> semantic_arm_candidate_total;
    std::atomic<int64_t> semantic_a1_true_total;
    std::atomic<int64_t> semantic_a2_true_total;
    std::atomic<int64_t> semantic_a3_true_total;
    std::atomic<int64_t> semantic_a3b_true_total;
    std::atomic<int64_t> semantic_a4_true_total;
    std::atomic<int64_t> semantic_a5_true_total;
    std::atomic<int64_t> semantic_a6_true_total;
    std::atomic<int64_t> semantic_all_but_a1_total;
    std::atomic<int64_t> semantic_all_but_a2_total;
    std::atomic<int64_t> semantic_all_but_a3_total;
    std::atomic<int64_t> semantic_all_but_a3b_total;
    std::atomic<int64_t> semantic_all_but_a4_total;
    std::atomic<int64_t> semantic_all_but_a5_total;
    std::atomic<int64_t> semantic_all_but_a6_total;

    std::atomic<int64_t> recovery_checks_total;
    std::atomic<int64_t> recovery_skip_not_armed;
    std::atomic<int64_t> recovery_skip_height_changed;
    std::atomic<int64_t> recovery_skip_peer_not_ahead;
    std::atomic<int64_t> recovery_skip_pipeline_active;
    std::atomic<int64_t> recovery_skip_pipeline_active_after_timeout;
    std::atomic<int64_t> recovery_skip_timeout_not_reached;
    std::atomic<int64_t> recovery_skip_cooldown;
    std::atomic<int64_t> recovery_triggered;

    std::atomic<int64_t> recovery_armed;
    std::atomic<int64_t> recovery_last_observed_height;
    std::atomic<int64_t> recovery_stall_age_seconds;
    std::atomic<int64_t> recovery_longest_stall_seconds;
    std::atomic<int64_t> recovery_max_peer_height;
    std::atomic<int64_t> recovery_peer_gap;
    std::atomic<int64_t> recovery_pipeline_active;

    Counters()
        : semantic_checks_total(0),
          semantic_arm_candidate_total(0),
          semantic_a1_true_total(0),
          semantic_a2_true_total(0),
          semantic_a3_true_total(0),
          semantic_a3b_true_total(0),
          semantic_a4_true_total(0),
          semantic_a5_true_total(0),
          semantic_a6_true_total(0),
          semantic_all_but_a1_total(0),
          semantic_all_but_a2_total(0),
          semantic_all_but_a3_total(0),
          semantic_all_but_a3b_total(0),
          semantic_all_but_a4_total(0),
          semantic_all_but_a5_total(0),
          semantic_all_but_a6_total(0),
          recovery_checks_total(0),
          recovery_skip_not_armed(0),
          recovery_skip_height_changed(0),
          recovery_skip_peer_not_ahead(0),
          recovery_skip_pipeline_active(0),
          recovery_skip_pipeline_active_after_timeout(0),
          recovery_skip_timeout_not_reached(0),
          recovery_skip_cooldown(0),
          recovery_triggered(0),
          recovery_armed(0),
          recovery_last_observed_height(0),
          recovery_stall_age_seconds(0),
          recovery_longest_stall_seconds(0),
          recovery_max_peer_height(0),
          recovery_peer_gap(0),
          recovery_pipeline_active(0)
    {
    }

    void Reset()
    {
        semantic_checks_total.store(0, std::memory_order_relaxed);
        semantic_arm_candidate_total.store(0, std::memory_order_relaxed);
        semantic_a1_true_total.store(0, std::memory_order_relaxed);
        semantic_a2_true_total.store(0, std::memory_order_relaxed);
        semantic_a3_true_total.store(0, std::memory_order_relaxed);
        semantic_a3b_true_total.store(0, std::memory_order_relaxed);
        semantic_a4_true_total.store(0, std::memory_order_relaxed);
        semantic_a5_true_total.store(0, std::memory_order_relaxed);
        semantic_a6_true_total.store(0, std::memory_order_relaxed);
        semantic_all_but_a1_total.store(0, std::memory_order_relaxed);
        semantic_all_but_a2_total.store(0, std::memory_order_relaxed);
        semantic_all_but_a3_total.store(0, std::memory_order_relaxed);
        semantic_all_but_a3b_total.store(0, std::memory_order_relaxed);
        semantic_all_but_a4_total.store(0, std::memory_order_relaxed);
        semantic_all_but_a5_total.store(0, std::memory_order_relaxed);
        semantic_all_but_a6_total.store(0, std::memory_order_relaxed);
        recovery_checks_total.store(0, std::memory_order_relaxed);
        recovery_skip_not_armed.store(0, std::memory_order_relaxed);
        recovery_skip_height_changed.store(0, std::memory_order_relaxed);
        recovery_skip_peer_not_ahead.store(0, std::memory_order_relaxed);
        recovery_skip_pipeline_active.store(0, std::memory_order_relaxed);
        recovery_skip_pipeline_active_after_timeout.store(
            0, std::memory_order_relaxed);
        recovery_skip_timeout_not_reached.store(0, std::memory_order_relaxed);
        recovery_skip_cooldown.store(0, std::memory_order_relaxed);
        recovery_triggered.store(0, std::memory_order_relaxed);
        recovery_armed.store(0, std::memory_order_relaxed);
        recovery_last_observed_height.store(0, std::memory_order_relaxed);
        recovery_stall_age_seconds.store(0, std::memory_order_relaxed);
        recovery_longest_stall_seconds.store(0, std::memory_order_relaxed);
        recovery_max_peer_height.store(0, std::memory_order_relaxed);
        recovery_peer_gap.store(0, std::memory_order_relaxed);
        recovery_pipeline_active.store(0, std::memory_order_relaxed);
    }
};

static Counters g_counters;

// Rolling window and its guard.  Written from the message-handler thread
// (1s emit) and from unit tests; read by getinfo export.  cs_main is never
// held across this guard.
static std::deque<SemanticSample> g_samples;
static CCriticalSection cs_semantic;

// 1s cadence gate (same CAS pattern as ibdactivepath).
static std::atomic<int64_t> g_nLastEmitMicros(0);

// ---------------------------------------------------------------------------
// Pure window evaluation (no side effects; shared by emit, snapshot, tests).
// ---------------------------------------------------------------------------

struct HeightStats
{
    bool fTipValid;
    int64_t tip_delta;
    bool fRateValid;
    double rate;   // mean per-second delta
    bool fMedianValid;
    double median;
};

static HeightStats ComputeHeightStats(const std::deque<SemanticSample>& samples,
                                      size_t nWindow)
{
    HeightStats st;
    st.fTipValid = false;
    st.tip_delta = 0;
    st.fRateValid = false;
    st.rate = 0.0;
    st.fMedianValid = false;
    st.median = 0.0;

    const size_t n = samples.size();
    if (n == 0)
        return st;
    const size_t iStart = n > nWindow ? n - nWindow : 0;

    bool fFirst = false;
    int64_t hFirst = 0;
    int64_t hLast = 0;
    for (size_t i = iStart; i < n; ++i)
    {
        if (samples[i].fHeightAvailable)
        {
            if (!fFirst)
            {
                fFirst = true;
                hFirst = samples[i].nBestHeight;
            }
            hLast = samples[i].nBestHeight;
        }
    }
    if (fFirst)
    {
        st.fTipValid = true;
        st.tip_delta = hLast - hFirst;
    }

    std::vector<int64_t> vDeltas;
    for (size_t i = iStart + 1; i < n; ++i)
    {
        const SemanticSample& prev = samples[i - 1];
        const SemanticSample& cur = samples[i];
        if (prev.fHeightAvailable && cur.fHeightAvailable)
            vDeltas.push_back((int64_t)cur.nBestHeight - (int64_t)prev.nBestHeight);
    }
    if (!vDeltas.empty())
    {
        st.fRateValid = true;
        int64_t nSum = 0;
        for (size_t i = 0; i < vDeltas.size(); ++i)
            nSum += vDeltas[i];
        st.rate = (double)nSum / (double)vDeltas.size();
        std::sort(vDeltas.begin(), vDeltas.end());
        st.fMedianValid = true;
        const size_t k = vDeltas.size();
        st.median = (k % 2 == 1)
            ? (double)vDeltas[k / 2]
            : ((double)vDeltas[k / 2 - 1] + (double)vDeltas[k / 2]) / 2.0;
    }
    return st;
}

static int64_t DeltaCounter(const std::deque<SemanticSample>& samples,
                            size_t nWindow, int64_t SemanticSample::*pField)
{
    const size_t n = samples.size();
    if (n < 2)
        return 0;
    const size_t iStart = n > nWindow ? n - nWindow : 0;
    return samples.back().*pField - samples[iStart].*pField;
}

static int64_t PeerGap(const SemanticSample& s)
{
    if (!s.fHeightAvailable)
        return 0;
    return std::max<int64_t>(
        0, (int64_t)s.peer_height_max - (int64_t)s.nBestHeight);
}

struct EvaluationResult
{
    int nSampleCount;
    bool fWindowReady;
    int nLocalHeight;
    int64_t tip_delta_1m;
    int64_t tip_delta_5m;
    bool fTip1mValid;
    bool fTip5mValid;
    double progress_rate_1m;
    double progress_rate_5m;
    double progress_median_5m;
    bool fReceiveActive;
    int64_t received_1m;
    double orphan_pressure_1m;
    double reject_futility_1m;
    int64_t pipeline_events_1m;
    int eligible_ahead_peer_count;
    int peer_height_max;
    int peer_height_confirmed;
    bool fConfirmedGapAvailable;
    int64_t peer_gap_max;
    int64_t peer_gap_confirmed;
    int64_t peer_gap_change_1m;
    int64_t peer_gap_change_5m;
    int64_t oldest_live_inflight_age_ms;
    int64_t inflight_gt1s;
    int64_t inflight_gt4s;
    bool a1_low_progress;
    bool a2_receiving;
    bool a3_orphan_dominant;
    bool a3b_not_futile;
    bool a4_pipeline_busy;
    bool a5_large_confirmed_gap;
    bool a6_multiple_ahead_peers;
    bool arm_candidate;
};

static EvaluationResult Evaluate(const std::deque<SemanticSample>& samples)
{
    EvaluationResult r;
    r.nSampleCount = (int)samples.size();
    r.fWindowReady = r.nSampleCount >= SEMANTIC_MIN_ARM_SAMPLES;
    r.nLocalHeight = -1;
    r.tip_delta_1m = 0;
    r.tip_delta_5m = 0;
    r.fTip1mValid = false;
    r.fTip5mValid = false;
    r.progress_rate_1m = 0.0;
    r.progress_rate_5m = 0.0;
    r.progress_median_5m = 0.0;
    r.fReceiveActive = false;
    r.received_1m = 0;
    r.orphan_pressure_1m = 0.0;
    r.reject_futility_1m = 0.0;
    r.pipeline_events_1m = 0;
    r.eligible_ahead_peer_count = 0;
    r.peer_height_max = -1;
    r.peer_height_confirmed = 0;
    r.fConfirmedGapAvailable = false;
    r.peer_gap_max = 0;
    r.peer_gap_confirmed = 0;
    r.peer_gap_change_1m = 0;
    r.peer_gap_change_5m = 0;
    r.oldest_live_inflight_age_ms = 0;
    r.inflight_gt1s = 0;
    r.inflight_gt4s = 0;
    r.a1_low_progress = false;
    r.a2_receiving = false;
    r.a3_orphan_dominant = false;
    r.a3b_not_futile = false;
    r.a4_pipeline_busy = false;
    r.a5_large_confirmed_gap = false;
    r.a6_multiple_ahead_peers = false;
    r.arm_candidate = false;

    if (samples.empty())
        return r;

    const SemanticSample& last = samples.back();
    r.nLocalHeight = last.fHeightAvailable ? last.nBestHeight : -1;

    const HeightStats hs1 = ComputeHeightStats(samples, SEMANTIC_WINDOW_1M);
    const HeightStats hs5 = ComputeHeightStats(samples, SEMANTIC_WINDOW_5M);
    r.fTip1mValid = hs1.fTipValid;
    r.fTip5mValid = hs5.fTipValid;
    r.tip_delta_1m = hs1.tip_delta;
    r.tip_delta_5m = hs5.tip_delta;
    r.progress_rate_1m = hs1.rate;
    r.progress_rate_5m = hs5.rate;
    r.progress_median_5m = hs5.median;

    const int64_t nOrphan1m =
        DeltaCounter(samples, SEMANTIC_WINDOW_1M,
                     &SemanticSample::block_result_orphan_new) +
        DeltaCounter(samples, SEMANTIC_WINDOW_1M,
                     &SemanticSample::block_result_orphan_limit_rejected);
    const int64_t nRejected1m =
        DeltaCounter(samples, SEMANTIC_WINDOW_1M,
                     &SemanticSample::block_result_rejected_total);
    r.received_1m = DeltaCounter(samples, SEMANTIC_WINDOW_1M,
                                 &SemanticSample::block_receive_total);
    if (r.received_1m > 0)
    {
        r.fReceiveActive = true;
        r.orphan_pressure_1m = (double)nOrphan1m / (double)r.received_1m;
        r.reject_futility_1m = (double)nRejected1m / (double)r.received_1m;
    }
    r.pipeline_events_1m = DeltaCounter(samples, SEMANTIC_WINDOW_1M,
                                        &SemanticSample::pipeline_events);

    r.eligible_ahead_peer_count = last.eligible_ahead_peer_count;
    r.peer_height_max = last.peer_height_max;
    r.peer_height_confirmed = last.confirmed_peer_height;
    r.fConfirmedGapAvailable = last.fConfirmedGapAvailable;
    r.peer_gap_max = PeerGap(last);
    r.peer_gap_confirmed =
        (last.fConfirmedGapAvailable && last.fHeightAvailable)
            ? std::max<int64_t>(0, (int64_t)last.confirmed_peer_height -
                                       (int64_t)last.nBestHeight)
            : 0;
    {
        const size_t n = samples.size();
        const size_t i1m = n > SEMANTIC_WINDOW_1M
                               ? n - SEMANTIC_WINDOW_1M : 0;
        const size_t i5m = n > SEMANTIC_WINDOW_5M
                               ? n - SEMANTIC_WINDOW_5M : 0;
        r.peer_gap_change_1m = PeerGap(last) - PeerGap(samples[i1m]);
        r.peer_gap_change_5m = PeerGap(last) - PeerGap(samples[i5m]);
    }
    r.oldest_live_inflight_age_ms = last.oldest_live_inflight_age_ms;
    r.inflight_gt1s = last.inflight_gt1s;
    r.inflight_gt4s = last.inflight_gt4s;

    // Arm predicates.  A1 requires the 5m tip delta, the mean per-second
    // rate, and the median per-second delta to all be below the minimum
    // progress rate: a bursty-but-healthy IBD may legitimately have
    // median=0, so the median alone is never the deciding criterion.
    const bool a1 =
        r.fWindowReady && hs5.fTipValid && hs5.fRateValid &&
        hs5.fMedianValid &&
        hs5.tip_delta <
            (int64_t)(g_dMinProgressRate * (double)SEMANTIC_WINDOW_5M) &&
        hs5.rate < g_dMinProgressRate &&
        hs5.median < g_dMinProgressRate;
    const bool a2 = r.fReceiveActive && r.received_1m >= 1;
    const bool a3 = r.fReceiveActive &&
                    r.orphan_pressure_1m > g_dOrphanDom;
    const bool a3b = r.fReceiveActive &&
                     r.reject_futility_1m < g_dFutileReject;
    const bool a4 = r.pipeline_events_1m > 0;
    const bool a5 = r.fConfirmedGapAvailable && last.fHeightAvailable &&
                    r.peer_gap_confirmed > g_nGapMin;
    const bool a6 = r.eligible_ahead_peer_count >= 2;

    r.a1_low_progress = a1;
    r.a2_receiving = a2;
    r.a3_orphan_dominant = a3;
    r.a3b_not_futile = a3b;
    r.a4_pipeline_busy = a4;
    r.a5_large_confirmed_gap = a5;
    r.a6_multiple_ahead_peers = a6;
    r.arm_candidate = a1 && a2 && a3 && a3b && a4 && a5 && a6;

    return r;
}

// Append + evaluate + update cumulative counters.  Inert while disabled.
static EvaluationResult AddSampleAndEvaluate(const SemanticSample& sample)
{
    EvaluationResult r;
    if (g_nMode == 0)
    {
        r.nSampleCount = 0;
        r.fWindowReady = false;
        return r;
    }
    {
        LOCK(cs_semantic);
        g_samples.push_back(sample);
        while ((int)g_samples.size() > SEMANTIC_WINDOW_SIZE)
            g_samples.pop_front();
        r = Evaluate(g_samples);
    }
    g_counters.semantic_checks_total.fetch_add(1, std::memory_order_relaxed);
    if (r.arm_candidate)
        g_counters.semantic_arm_candidate_total.fetch_add(
            1, std::memory_order_relaxed);
    if (r.a1_low_progress)
        g_counters.semantic_a1_true_total.fetch_add(1, std::memory_order_relaxed);
    if (r.a2_receiving)
        g_counters.semantic_a2_true_total.fetch_add(1, std::memory_order_relaxed);
    if (r.a3_orphan_dominant)
        g_counters.semantic_a3_true_total.fetch_add(1, std::memory_order_relaxed);
    if (r.a3b_not_futile)
        g_counters.semantic_a3b_true_total.fetch_add(1, std::memory_order_relaxed);
    if (r.a4_pipeline_busy)
        g_counters.semantic_a4_true_total.fetch_add(1, std::memory_order_relaxed);
    if (r.a5_large_confirmed_gap)
        g_counters.semantic_a5_true_total.fetch_add(1, std::memory_order_relaxed);
    if (r.a6_multiple_ahead_peers)
        g_counters.semantic_a6_true_total.fetch_add(1, std::memory_order_relaxed);
    // "All except aN" counters track evaluations where every predicate other
    // than aN holds while aN itself fails (i.e. aN is the sole blocker); this
    // is independent of arm_candidate and isolates each threshold's effect.
    if (!r.a1_low_progress && r.a2_receiving && r.a3_orphan_dominant &&
        r.a3b_not_futile && r.a4_pipeline_busy && r.a5_large_confirmed_gap &&
        r.a6_multiple_ahead_peers)
        g_counters.semantic_all_but_a1_total.fetch_add(
            1, std::memory_order_relaxed);
    if (r.a1_low_progress && !r.a2_receiving && r.a3_orphan_dominant &&
        r.a3b_not_futile && r.a4_pipeline_busy && r.a5_large_confirmed_gap &&
        r.a6_multiple_ahead_peers)
        g_counters.semantic_all_but_a2_total.fetch_add(
            1, std::memory_order_relaxed);
    if (r.a1_low_progress && r.a2_receiving && !r.a3_orphan_dominant &&
        r.a3b_not_futile && r.a4_pipeline_busy && r.a5_large_confirmed_gap &&
        r.a6_multiple_ahead_peers)
        g_counters.semantic_all_but_a3_total.fetch_add(
            1, std::memory_order_relaxed);
    if (r.a1_low_progress && r.a2_receiving && r.a3_orphan_dominant &&
        !r.a3b_not_futile && r.a4_pipeline_busy && r.a5_large_confirmed_gap &&
        r.a6_multiple_ahead_peers)
        g_counters.semantic_all_but_a3b_total.fetch_add(
            1, std::memory_order_relaxed);
    if (r.a1_low_progress && r.a2_receiving && r.a3_orphan_dominant &&
        r.a3b_not_futile && !r.a4_pipeline_busy && r.a5_large_confirmed_gap &&
        r.a6_multiple_ahead_peers)
        g_counters.semantic_all_but_a4_total.fetch_add(
            1, std::memory_order_relaxed);
    if (r.a1_low_progress && r.a2_receiving && r.a3_orphan_dominant &&
        r.a3b_not_futile && r.a4_pipeline_busy && !r.a5_large_confirmed_gap &&
        r.a6_multiple_ahead_peers)
        g_counters.semantic_all_but_a5_total.fetch_add(
            1, std::memory_order_relaxed);
    if (r.a1_low_progress && r.a2_receiving && r.a3_orphan_dominant &&
        r.a3b_not_futile && r.a4_pipeline_busy && r.a5_large_confirmed_gap &&
        !r.a6_multiple_ahead_peers)
        g_counters.semantic_all_but_a6_total.fetch_add(
            1, std::memory_order_relaxed);
    return r;
}

// ---------------------------------------------------------------------------
// Peer and inflight scans (called once per second from the emit hook).
// ---------------------------------------------------------------------------

static void ComputeInflightDiagnostics(const std::vector<CNode*>& vNodesCopy,
                                       int64_t nNow,
                                       int64_t& nOldestMs,
                                       int64_t& nGt1s,
                                       int64_t& nGt4s)
{
    nOldestMs = 0;
    nGt1s = 0;
    nGt4s = 0;
    BOOST_FOREACH (const CNode* pnode, vNodesCopy)
    {
        if (pnode == NULL)
            continue;
        for (std::map<uint256, int64_t>::const_iterator it =
                 pnode->mapBlockInFlightSince.begin();
             it != pnode->mapBlockInFlightSince.end(); ++it)
        {
            const int64_t nAgeMs = (nNow - it->second) * 1000;
            nOldestMs = std::max(nOldestMs, nAgeMs);
            if (nAgeMs > 1000)
                ++nGt1s;
            if (nAgeMs > 4000)
                ++nGt4s;
        }
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Public API.
// ---------------------------------------------------------------------------

bool SetEnabled(int nMode)
{
    if (nMode != 0 && nMode != 1)
        return false;
    g_nMode = nMode;
    g_dMinProgressRate =
        LoadDouble("-ibdminprogressrate", 0.2, 0.0, 100.0);
    g_dOrphanDom = LoadDouble("-ibdorphandom", 0.5, 0.0, 1.0);
    g_dFutileReject = LoadDouble("-ibdfutilereject", 0.2, 0.0, 1.0);
    {
        int64_t nRaw = 5000;
        if (mapArgs.count("-ibdgapmin") &&
            !ParseInt64(mapArgs["-ibdgapmin"], &nRaw))
        {
            nRaw = 5000;
        }
        g_nGapMin = std::max<int64_t>(0, nRaw);
    }
    return true;
}

int GetEnabled()
{
    return g_nMode;
}

PeerHeightScan ScanEligibleAheadPeers(const std::vector<CNode*>& vNodesCopy,
                                      int nLocalHeight)
{
    PeerHeightScan out;
    out.eligible_count = 0;
    out.peer_height_max = -1;
    out.confirmed_peer_height = 0;
    out.fConfirmedAvailable = false;

    std::vector<int64_t> vAheadHeights;
    BOOST_FOREACH (const CNode* pnode, vNodesCopy)
    {
        if (pnode == NULL || pnode->fDisconnect ||
            !pnode->fSuccessfullyConnected || pnode->fClient ||
            pnode->fOneShot || !IsBlockSyncPeerVersion(pnode->nVersion))
        {
            continue;
        }
        const int64_t nPeerHeight =
            std::max((int64_t)pnode->nBestKnownHeight,
                     (int64_t)pnode->nChainHeight);
        out.peer_height_max =
            std::max<int64_t>(out.peer_height_max, nPeerHeight);
        if (nPeerHeight > nLocalHeight)
        {
            ++out.eligible_count;
            vAheadHeights.push_back(nPeerHeight);
        }
    }
    if (vAheadHeights.size() >= 2)
    {
        std::sort(vAheadHeights.begin(), vAheadHeights.end());
        out.confirmed_peer_height = (int)vAheadHeights[vAheadHeights.size() - 2];
        out.fConfirmedAvailable = true;
    }
    return out;
}

void AddSampleForTesting(const SemanticSample& sample)
{
    AddSampleAndEvaluate(sample);
}

void EmitIBDSemanticHealth1s(const std::vector<CNode*>& vNodesCopy)
{
    if (g_nMode == 0)
        return;

    const int64_t nNow = ibdactivepath::MonotonicMicros();
    int64_t nLast = g_nLastEmitMicros.load(std::memory_order_relaxed);
    if (nNow - nLast < 1000000)
        return;
    if (!g_nLastEmitMicros.compare_exchange_weak(
            nLast, nNow, std::memory_order_relaxed))
        return;

    int nLocalHeight = -1;
    bool fHeightAvailable = false;
    {
        TRY_LOCK(cs_main, lockMain);
        if (lockMain)
        {
            nLocalHeight = nBestHeight;
            fHeightAvailable = true;
        }
    }

    SemanticSample s;
    s.time_us = nNow;
    s.fHeightAvailable = fHeightAvailable;
    s.nBestHeight = fHeightAvailable ? nLocalHeight : -1;
    const ibdmetrics::Counters& mc = ibdmetrics::Get();
    s.block_receive_total =
        mc.block_receive_total.load(std::memory_order_relaxed);
    s.block_result_orphan_new =
        mc.block_result_orphan_new.load(std::memory_order_relaxed);
    s.block_result_orphan_limit_rejected =
        mc.block_result_orphan_limit_rejected.load(std::memory_order_relaxed);
    s.block_result_rejected_total =
        mc.block_result_rejected_total.load(std::memory_order_relaxed);
    s.pipeline_events =
        mc.active_decrement_askfor_sent_transition.load(
            std::memory_order_relaxed);

    if (fHeightAvailable)
    {
        const PeerHeightScan scan =
            ScanEligibleAheadPeers(vNodesCopy, nLocalHeight);
        s.eligible_ahead_peer_count = scan.eligible_count;
        s.confirmed_peer_height = scan.confirmed_peer_height;
        s.fConfirmedGapAvailable = scan.fConfirmedAvailable;
        s.peer_height_max = scan.peer_height_max;
    }
    else
    {
        s.eligible_ahead_peer_count = 0;
        s.confirmed_peer_height = 0;
        s.fConfirmedGapAvailable = false;
        s.peer_height_max = -1;
    }
    ComputeInflightDiagnostics(vNodesCopy, GetTime(),
                               s.oldest_live_inflight_age_ms,
                               s.inflight_gt1s, s.inflight_gt4s);

    const EvaluationResult r = AddSampleAndEvaluate(s);

    std::ostringstream oss;
    oss << "IBD_SEMANTIC_HEALTH_1S time_us=" << (long long)nNow
        << " local_height=" << (int)r.nLocalHeight
        << " samples=" << (int)r.nSampleCount
        << " window_ready=" << (r.fWindowReady ? 1 : 0)
        << " tip_delta_1m=" << (long long)r.tip_delta_1m
        << " tip_delta_5m=" << (long long)r.tip_delta_5m
        << " progress_rate_1m=" << std::fixed << std::setprecision(6)
        << r.progress_rate_1m
        << " progress_rate_5m=" << r.progress_rate_5m
        << " progress_median_5m=" << r.progress_median_5m
        << std::defaultfloat
        << " received_1m=" << (long long)r.received_1m
        << " orphan_pressure_1m=" << std::setprecision(6)
        << r.orphan_pressure_1m
        << " reject_futility_1m=" << r.reject_futility_1m
        << std::defaultfloat
        << " pipeline_events_1m=" << (long long)r.pipeline_events_1m
        << " eligible_ahead_peers=" << (int)r.eligible_ahead_peer_count
        << " peer_height_max=" << (long long)r.peer_height_max
        << " peer_height_confirmed=" << (long long)r.peer_height_confirmed
        << " peer_gap_max=" << (long long)r.peer_gap_max
        << " peer_gap_confirmed=" << (long long)r.peer_gap_confirmed
        << " peer_gap_change_1m=" << (long long)r.peer_gap_change_1m
        << " peer_gap_change_5m=" << (long long)r.peer_gap_change_5m
        << " oldest_live_inflight_age_ms="
        << (long long)r.oldest_live_inflight_age_ms
        << " inflight_gt1s=" << (long long)r.inflight_gt1s
        << " inflight_gt4s=" << (long long)r.inflight_gt4s
        << " a1=" << (r.a1_low_progress ? 1 : 0)
        << " a2=" << (r.a2_receiving ? 1 : 0)
        << " a3=" << (r.a3_orphan_dominant ? 1 : 0)
        << " a3b=" << (r.a3b_not_futile ? 1 : 0)
        << " a4=" << (r.a4_pipeline_busy ? 1 : 0)
        << " a5=" << (r.a5_large_confirmed_gap ? 1 : 0)
        << " a6=" << (r.a6_multiple_ahead_peers ? 1 : 0)
        << " arm_candidate=" << (r.arm_candidate ? 1 : 0)
        << " recovery_armed="
        << (long long)g_counters.recovery_armed.load(std::memory_order_relaxed)
        << " recovery_last_observed_height="
        << (long long)g_counters.recovery_last_observed_height.load(
               std::memory_order_relaxed)
        << " recovery_stall_age_seconds="
        << (long long)g_counters.recovery_stall_age_seconds.load(
               std::memory_order_relaxed)
        << " recovery_longest_stall_seconds="
        << (long long)g_counters.recovery_longest_stall_seconds.load(
               std::memory_order_relaxed)
        << " recovery_max_peer_height="
        << (long long)g_counters.recovery_max_peer_height.load(
               std::memory_order_relaxed)
        << " recovery_peer_gap="
        << (long long)g_counters.recovery_peer_gap.load(
               std::memory_order_relaxed)
        << " recovery_pipeline_active="
        << (long long)g_counters.recovery_pipeline_active.load(
               std::memory_order_relaxed)
        << " recovery_checks_total="
        << (long long)g_counters.recovery_checks_total.load(
               std::memory_order_relaxed)
        << "\n";
    LogPrintf("%s", oss.str().c_str());
}

void SnapshotAll(IBDSemanticSnapshot& out)
{
    EvaluationResult r;
    {
        LOCK(cs_semantic);
        r = Evaluate(g_samples);
    }
    out.nEnabled = g_nMode;
    out.nSampleCount = r.nSampleCount;
    out.fWindowReady = r.fWindowReady;
    out.nLocalHeight = r.nLocalHeight;
    out.time_us = ibdactivepath::MonotonicMicros();
    out.tip_delta_1m = r.tip_delta_1m;
    out.tip_delta_5m = r.tip_delta_5m;
    out.fTip1mValid = r.fTip1mValid;
    out.fTip5mValid = r.fTip5mValid;
    out.progress_rate_1m = r.progress_rate_1m;
    out.progress_rate_5m = r.progress_rate_5m;
    out.progress_median_5m = r.progress_median_5m;
    out.fReceiveActive = r.fReceiveActive;
    out.received_1m = r.received_1m;
    out.orphan_pressure_1m = r.orphan_pressure_1m;
    out.reject_futility_1m = r.reject_futility_1m;
    out.pipeline_events_1m = r.pipeline_events_1m;
    out.eligible_ahead_peers = r.eligible_ahead_peer_count;
    out.peer_height_max = r.peer_height_max;
    out.peer_height_confirmed = r.peer_height_confirmed;
    out.fConfirmedGapAvailable = r.fConfirmedGapAvailable;
    out.peer_gap_max = r.peer_gap_max;
    out.peer_gap_confirmed = r.peer_gap_confirmed;
    out.peer_gap_change_1m = r.peer_gap_change_1m;
    out.peer_gap_change_5m = r.peer_gap_change_5m;
    out.oldest_live_inflight_age_ms = r.oldest_live_inflight_age_ms;
    out.inflight_gt1s = r.inflight_gt1s;
    out.inflight_gt4s = r.inflight_gt4s;
    out.a1_low_progress = r.a1_low_progress;
    out.a2_receiving = r.a2_receiving;
    out.a3_orphan_dominant = r.a3_orphan_dominant;
    out.a3b_not_futile = r.a3b_not_futile;
    out.a4_pipeline_busy = r.a4_pipeline_busy;
    out.a5_large_confirmed_gap = r.a5_large_confirmed_gap;
    out.a6_multiple_ahead_peers = r.a6_multiple_ahead_peers;
    out.arm_candidate = r.arm_candidate;
    out.semantic_checks_total =
        g_counters.semantic_checks_total.load(std::memory_order_relaxed);
    out.semantic_arm_candidate_total =
        g_counters.semantic_arm_candidate_total.load(std::memory_order_relaxed);
    out.semantic_a1_true_total =
        g_counters.semantic_a1_true_total.load(std::memory_order_relaxed);
    out.semantic_a2_true_total =
        g_counters.semantic_a2_true_total.load(std::memory_order_relaxed);
    out.semantic_a3_true_total =
        g_counters.semantic_a3_true_total.load(std::memory_order_relaxed);
    out.semantic_a3b_true_total =
        g_counters.semantic_a3b_true_total.load(std::memory_order_relaxed);
    out.semantic_a4_true_total =
        g_counters.semantic_a4_true_total.load(std::memory_order_relaxed);
    out.semantic_a5_true_total =
        g_counters.semantic_a5_true_total.load(std::memory_order_relaxed);
    out.semantic_a6_true_total =
        g_counters.semantic_a6_true_total.load(std::memory_order_relaxed);
    out.semantic_all_but_a1_total =
        g_counters.semantic_all_but_a1_total.load(std::memory_order_relaxed);
    out.semantic_all_but_a2_total =
        g_counters.semantic_all_but_a2_total.load(std::memory_order_relaxed);
    out.semantic_all_but_a3_total =
        g_counters.semantic_all_but_a3_total.load(std::memory_order_relaxed);
    out.semantic_all_but_a3b_total =
        g_counters.semantic_all_but_a3b_total.load(std::memory_order_relaxed);
    out.semantic_all_but_a4_total =
        g_counters.semantic_all_but_a4_total.load(std::memory_order_relaxed);
    out.semantic_all_but_a5_total =
        g_counters.semantic_all_but_a5_total.load(std::memory_order_relaxed);
    out.semantic_all_but_a6_total =
        g_counters.semantic_all_but_a6_total.load(std::memory_order_relaxed);
    out.recovery_checks_total =
        g_counters.recovery_checks_total.load(std::memory_order_relaxed);
    out.recovery_skip_not_armed =
        g_counters.recovery_skip_not_armed.load(std::memory_order_relaxed);
    out.recovery_skip_height_changed =
        g_counters.recovery_skip_height_changed.load(std::memory_order_relaxed);
    out.recovery_skip_peer_not_ahead =
        g_counters.recovery_skip_peer_not_ahead.load(std::memory_order_relaxed);
    out.recovery_skip_pipeline_active =
        g_counters.recovery_skip_pipeline_active.load(std::memory_order_relaxed);
    out.recovery_skip_pipeline_active_after_timeout =
        g_counters.recovery_skip_pipeline_active_after_timeout.load(
            std::memory_order_relaxed);
    out.recovery_skip_timeout_not_reached =
        g_counters.recovery_skip_timeout_not_reached.load(
            std::memory_order_relaxed);
    out.recovery_skip_cooldown =
        g_counters.recovery_skip_cooldown.load(std::memory_order_relaxed);
    out.recovery_triggered =
        g_counters.recovery_triggered.load(std::memory_order_relaxed);
    out.recovery_armed =
        g_counters.recovery_armed.load(std::memory_order_relaxed);
    out.recovery_last_observed_height =
        g_counters.recovery_last_observed_height.load(std::memory_order_relaxed);
    out.recovery_stall_age_seconds =
        g_counters.recovery_stall_age_seconds.load(std::memory_order_relaxed);
    out.recovery_longest_stall_seconds =
        g_counters.recovery_longest_stall_seconds.load(std::memory_order_relaxed);
    out.recovery_max_peer_height =
        g_counters.recovery_max_peer_height.load(std::memory_order_relaxed);
    out.recovery_peer_gap =
        g_counters.recovery_peer_gap.load(std::memory_order_relaxed);
    out.recovery_pipeline_active =
        g_counters.recovery_pipeline_active.load(std::memory_order_relaxed);
}

void RecordRecoveryCheck(int64_t nLocalHeight, int64_t nPeerHeight,
                         bool fPipelineActive, bool fSyncRequestSent,
                         int64_t nLastObservedHeight,
                         int64_t nLastProgressTime, int64_t nNow)
{
    g_counters.recovery_checks_total.fetch_add(1, std::memory_order_relaxed);
    const int64_t nStallAge = nLastProgressTime == 0
        ? -1 : std::max<int64_t>(0, nNow - nLastProgressTime);
    g_counters.recovery_armed.store(fSyncRequestSent ? 1 : 0,
                                    std::memory_order_relaxed);
    g_counters.recovery_last_observed_height.store(
        nLastObservedHeight, std::memory_order_relaxed);
    g_counters.recovery_stall_age_seconds.store(
        nStallAge, std::memory_order_relaxed);
    int64_t nLongest =
        g_counters.recovery_longest_stall_seconds.load(std::memory_order_relaxed);
    while (nStallAge > nLongest &&
           !g_counters.recovery_longest_stall_seconds.compare_exchange_weak(
               nLongest, nStallAge, std::memory_order_relaxed))
    {
    }
    g_counters.recovery_max_peer_height.store(
        nPeerHeight, std::memory_order_relaxed);
    g_counters.recovery_peer_gap.store(
        std::max<int64_t>(0, nPeerHeight - nLocalHeight),
        std::memory_order_relaxed);
    g_counters.recovery_pipeline_active.store(
        fPipelineActive ? 1 : 0, std::memory_order_relaxed);
}

void RecordRecoverySkipNotArmed()
{
    g_counters.recovery_skip_not_armed.fetch_add(1, std::memory_order_relaxed);
}

void RecordRecoverySkipHeightChanged()
{
    g_counters.recovery_skip_height_changed.fetch_add(
        1, std::memory_order_relaxed);
}

void RecordRecoverySkipPeerNotAhead()
{
    g_counters.recovery_skip_peer_not_ahead.fetch_add(
        1, std::memory_order_relaxed);
}

void RecordRecoverySkipPipelineActive()
{
    g_counters.recovery_skip_pipeline_active.fetch_add(
        1, std::memory_order_relaxed);
}

void RecordRecoverySkipPipelineActiveAfterTimeout()
{
    g_counters.recovery_skip_pipeline_active_after_timeout.fetch_add(
        1, std::memory_order_relaxed);
}

void RecordRecoverySkipTimeoutNotReached()
{
    g_counters.recovery_skip_timeout_not_reached.fetch_add(
        1, std::memory_order_relaxed);
}

void RecordRecoverySkipCooldown()
{
    g_counters.recovery_skip_cooldown.fetch_add(1, std::memory_order_relaxed);
}

void RecordRecoveryTriggered()
{
    g_counters.recovery_triggered.fetch_add(1, std::memory_order_relaxed);
}

void ResetForTesting()
{
    LOCK(cs_semantic);
    g_samples.clear();
    g_nLastEmitMicros = 0;
    g_counters.Reset();
}

void SetThresholdsForTesting(double dMinProgressRate, double dOrphanDom,
                             double dFutileReject, int64_t nGapMin)
{
    g_dMinProgressRate = dMinProgressRate;
    g_dOrphanDom = dOrphanDom;
    g_dFutileReject = dFutileReject;
    g_nGapMin = nGapMin;
}

int SampleCountForTesting()
{
    LOCK(cs_semantic);
    return (int)g_samples.size();
}

} // namespace ibdsemantic
