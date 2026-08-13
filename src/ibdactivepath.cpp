// Copyright (c) 2026 The Innova developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "ibdactivepath.h"

#include "checkpoints.h"
#include "ibdmetrics.h"
#include "main.h"
#include "net.h"
#include "sync.h"
#include "util.h"

#include <inttypes.h>
#include <stdio.h>

#include <map>

#ifndef WIN32
#include <time.h>
#endif

namespace ibdactivepath {

namespace {

bool g_IBDActivePathEnabled = false;

// Monotonic epoch for the first enabled interval.
int64_t g_nTraceEpochMicros = 0;
// Last IBD_ACTIVE_1S emission time.
std::atomic<int64_t> g_nLastEmitMicros(0);
// Last IBD_SLOW_BLOCK emission time.
std::atomic<int64_t> g_nLastSlowBlockMicros(0);
// Last IBD_STATE_TRACE emission time.
int64_t g_nLastStateTraceMicros = 0;
int64_t g_nLastStateTraceIBD = -1;
int g_nLastStateTraceReason = -1;

// End of the last getblocks response that contained at least one unknown
// block inv (monotonic).  Zero = none yet.
std::atomic<int64_t> g_last_useful_response_end_us(0);

// Monotonic AskFor-enqueue → getdata-export tracking.  Keyed by block hash
// (ownership is first-wins, so at most one peer holds a block request at a
// time).  The map is instrumentation-only: a leaf lock that is never held
// while another lock is acquired, and bounded by age pruning at insert time.
CCriticalSection cs_askForWire;
std::map<uint256, int64_t> g_askForWireEnqueuedMonotonic;

// Previous-interval totals for per-second rate deltas.  Owned exclusively by
// the message-handler thread (the only thread that emits).
int64_t g_prevBlocksAccepted = 0;
int64_t g_prevGetBlocksSent = 0;
int64_t g_prevGetDataSent = 0;
int64_t g_prevZeroPassDueCapacity = 0;
int64_t g_prevGetBlocksResponseInv = 0;

ActivePathCounters g_counters;

int64_t SlowBlockThresholdMicros()
{
    static const int64_t nThresholdUs =
        std::max<int64_t>(1000,
            GetArg("-ibdactiveslowthresholdms", 50)) * 1000;
    return nThresholdUs;
}

void AtomicMax(std::atomic<int64_t>& target, int64_t value)
{
    int64_t current = target.load(std::memory_order_relaxed);
    while (current < value &&
           !target.compare_exchange_weak(current, value,
                                         std::memory_order_relaxed))
    {
    }
}

int64_t Avg(int64_t nTotal, int64_t nCount)
{
    return nCount > 0 ? nTotal / nCount : 0;
}

} // namespace

int64_t MonotonicMicros()
{
#ifndef WIN32
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
        return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
#endif
    return GetTimeMicros();
}

bool InitIBDActivePathTrace(bool fEnabled)
{
    g_IBDActivePathEnabled = fEnabled;
    if (fEnabled)
    {
        g_nTraceEpochMicros = MonotonicMicros();
        // Re-arming forces a fresh 1-second emission cadence.
        g_nLastEmitMicros.store(0, std::memory_order_relaxed);
        printf("IBD_ACTIVE_1S time_us=%lld event=START enabled=1\n",
               (long long)g_nTraceEpochMicros);
    }
    return true;
}

bool IBDActivePathTraceEnabled()
{
    return g_IBDActivePathEnabled;
}

ActivePathCounters::ActivePathCounters()
    : getblocks_wire_sent(0),
      getblocks_response_inv(0),
      getblocks_response_block_inv(0),
      getblocks_response_unknown(0),
      response_latency_us_total(0),
      response_latency_us_max(0),
      response_latency_count(0),
      useful_gap_us_total(0),
      useful_gap_us_max(0),
      useful_gap_count(0),
      getdata_sent_total(0),
      getdata_pass_sent_count(0),
      getdata_pass_sent_max(0),
      getdata_zero_pass_due_capacity(0),
      askfor_stop_empty(0),
      askfor_stop_no_due(0),
      askfor_stop_inflight_cap(0),
      askfor_stop_other(0),
      askfor_to_getdata_us_total(0),
      askfor_to_getdata_us_max(0),
      askfor_to_getdata_count(0),
      msghand_pass_interval_us_total(0),
      msghand_pass_interval_us_max(0),
      msghand_pass_interval_count(0),
      msghand_sleep_us_total(0),
      msghand_sleep_count(0),
      block_dispatch_delay_us_total(0),
      block_dispatch_delay_us_max(0),
      block_dispatch_delay_count(0),
      complete_block_waiting_sum(0),
      complete_block_waiting_max(0),
      complete_block_waiting_count(0),
      cs_main_wait_us_total(0),
      cs_main_wait_us_max(0),
      cs_main_wait_count(0),
      processblock_us_total(0),
      processblock_us_max(0),
      processblock_count(0),
      acceptblock_us_total(0),
      acceptblock_us_max(0),
      acceptblock_count(0),
      addtoblockindex_us_total(0),
      addtoblockindex_us_max(0),
      addtoblockindex_count(0),
      setbestchain_us_total(0),
      setbestchain_us_max(0),
      setbestchain_count(0),
      connectblock_us_total(0),
      connectblock_us_max(0),
      connectblock_count(0),
      raw_block_write_us_total(0),
      raw_block_write_us_max(0),
      raw_block_write_count(0),
      filecommit_us_total(0),
      filecommit_us_max(0),
      filecommit_count(0),
      blockindex_commit_us_total(0),
      blockindex_commit_us_max(0),
      blockindex_commit_count(0),
      chainstate_commit_us_total(0),
      chainstate_commit_us_max(0),
      chainstate_commit_count(0),
      dag_epoch_commit_us_total(0),
      dag_epoch_commit_us_max(0),
      dag_epoch_commit_count(0),
      wallet_callback_us_total(0),
      wallet_callback_us_max(0),
      wallet_callback_count(0),
      peers_inflight_gt0(0),
      peers_queued_gt0(0),
      inflight_peer_max(0),
      queued_peer_max(0),
      dominant_peer_inflight_share_pct(0),
      global_free_active_slots(0),
      global_free_slots_with_deferred(-1),
      samples_single_peer_over_75pct(0),
      samples_global_below_half_with_deferred(0),
      block_request_wire_latency_us_total(0),
      block_request_wire_latency_us_max(0),
      block_request_wire_latency_count(0)
{
}

ActivePathCounters& GetCounters()
{
    return g_counters;
}

ActivePathTimer::ActivePathTimer(std::atomic<int64_t>& total,
                                 std::atomic<int64_t>& max,
                                 std::atomic<int64_t>& count,
                                 const char* pszPhaseIn,
                                 int nHeightIn)
    : nStart(IBDActivePathTraceEnabled() ? MonotonicMicros() : 0),
      pszPhase(pszPhaseIn),
      nHeight(nHeightIn),
      pTotal(&total),
      pMax(&max),
      pCount(&count)
{
}

ActivePathTimer::~ActivePathTimer()
{
    if (nStart == 0)
        return;
    const int64_t nUs = MonotonicMicros() - nStart;
    if (nUs < 0)
        return;
    pTotal->fetch_add(nUs, std::memory_order_relaxed);
    AtomicMax(*pMax, nUs);
    pCount->fetch_add(1, std::memory_order_relaxed);

    const int64_t nSlowUs = SlowBlockThresholdMicros();
    if (nUs >= nSlowUs)
    {
        const int64_t nNow = MonotonicMicros();
        int64_t nLast = g_nLastSlowBlockMicros.load(std::memory_order_relaxed);
        if (nNow - nLast >= 5000000 &&
            g_nLastSlowBlockMicros.compare_exchange_weak(
                nLast, nNow, std::memory_order_relaxed))
        {
            printf("IBD_SLOW_BLOCK time_us=%lld phase=%s us=%lld height=%d threshold_us=%lld\n",
                   (long long)nNow, pszPhase, (long long)nUs,
                   nHeight, (long long)nSlowUs);
        }
    }
}

void RecordGetBlocksWireSent()
{
    if (!g_IBDActivePathEnabled)
        return;
    ActivePathCounters& c = GetCounters();
    c.getblocks_wire_sent.fetch_add(1, std::memory_order_relaxed);

    // Gap between the end of the last useful response and this getblocks.
    const int64_t nEnd = g_last_useful_response_end_us.exchange(
                             0, std::memory_order_relaxed);
    if (nEnd > 0)
    {
        const int64_t nGap = std::max<int64_t>(
            0, MonotonicMicros() - nEnd);
        c.useful_gap_us_total.fetch_add(nGap, std::memory_order_relaxed);
        AtomicMax(c.useful_gap_us_max, nGap);
        c.useful_gap_count.fetch_add(1, std::memory_order_relaxed);
    }
}

void RecordGetBlocksResponse(int64_t nBlockInv, int64_t nUnknown)
{
    if (!g_IBDActivePathEnabled)
        return;
    ActivePathCounters& c = GetCounters();
    c.getblocks_response_inv.fetch_add(1, std::memory_order_relaxed);
    c.getblocks_response_block_inv.fetch_add(nBlockInv, std::memory_order_relaxed);
    c.getblocks_response_unknown.fetch_add(nUnknown, std::memory_order_relaxed);
}

void RecordUsefulResponseEnd()
{
    if (!g_IBDActivePathEnabled)
        return;
    g_last_useful_response_end_us.store(
        MonotonicMicros(), std::memory_order_relaxed);
}

void RecordGetDataPass(int64_t nSent, bool fDueRequests,
                       bool fFreeCapacity, int nStopReason)
{
    if (!g_IBDActivePathEnabled)
        return;
    ActivePathCounters& c = GetCounters();
    c.getdata_sent_total.fetch_add(nSent, std::memory_order_relaxed);
    if (nSent > 0)
    {
        c.getdata_pass_sent_count.fetch_add(1, std::memory_order_relaxed);
        AtomicMax(c.getdata_pass_sent_max, nSent);
    }
    else if (fDueRequests && fFreeCapacity)
    {
        c.getdata_zero_pass_due_capacity.fetch_add(1, std::memory_order_relaxed);
    }
    switch (nStopReason)
    {
    case GETDATA_STOP_EMPTY:
        c.askfor_stop_empty.fetch_add(1, std::memory_order_relaxed);
        break;
    case GETDATA_STOP_NO_DUE:
        c.askfor_stop_no_due.fetch_add(1, std::memory_order_relaxed);
        break;
    case GETDATA_STOP_INFLIGHT_CAP:
        c.askfor_stop_inflight_cap.fetch_add(1, std::memory_order_relaxed);
        break;
    default:
        c.askfor_stop_other.fetch_add(1, std::memory_order_relaxed);
        break;
    }
}

void RecordAskForToGetData(int64_t nWaitUs)
{
    if (!g_IBDActivePathEnabled)
        return;
    ActivePathCounters& c = GetCounters();
    c.askfor_to_getdata_us_total.fetch_add(nWaitUs, std::memory_order_relaxed);
    AtomicMax(c.askfor_to_getdata_us_max, nWaitUs);
    c.askfor_to_getdata_count.fetch_add(1, std::memory_order_relaxed);
}

void RecordBlockRequestEnqueued(const uint256& hash)
{
    if (!g_IBDActivePathEnabled)
        return;
    LOCK(cs_askForWire);
    const int64_t nNow = MonotonicMicros();
    if (g_askForWireEnqueuedMonotonic.size() >= 8192)
    {
        // Bound the instrumentation map: purge any record older than 60s.
        const int64_t nCutoff = nNow - 60 * 1000000;
        for (std::map<uint256, int64_t>::iterator it =
                 g_askForWireEnqueuedMonotonic.begin();
             it != g_askForWireEnqueuedMonotonic.end();)
        {
            if (it->second < nCutoff)
                it = g_askForWireEnqueuedMonotonic.erase(it);
            else
                ++it;
        }
    }
    g_askForWireEnqueuedMonotonic[hash] = nNow;
}

void RecordBlockRequestSent(const uint256& hash)
{
    if (!g_IBDActivePathEnabled)
        return;
    int64_t nWaitUs = -1;
    {
        LOCK(cs_askForWire);
        std::map<uint256, int64_t>::iterator it =
            g_askForWireEnqueuedMonotonic.find(hash);
        if (it == g_askForWireEnqueuedMonotonic.end())
            return;
        nWaitUs = std::max<int64_t>(0, MonotonicMicros() - it->second);
        g_askForWireEnqueuedMonotonic.erase(it);
    }
    ActivePathCounters& c = GetCounters();
    c.block_request_wire_latency_us_total.fetch_add(
        nWaitUs, std::memory_order_relaxed);
    AtomicMax(c.block_request_wire_latency_us_max, nWaitUs);
    c.block_request_wire_latency_count.fetch_add(1, std::memory_order_relaxed);
}

void RecordMessageHandlerPassInterval(int64_t nUs)
{
    if (!g_IBDActivePathEnabled)
        return;
    ActivePathCounters& c = GetCounters();
    c.msghand_pass_interval_us_total.fetch_add(nUs, std::memory_order_relaxed);
    AtomicMax(c.msghand_pass_interval_us_max, nUs);
    c.msghand_pass_interval_count.fetch_add(1, std::memory_order_relaxed);
}

void RecordMessageHandlerSleep(int64_t nUs)
{
    if (!g_IBDActivePathEnabled)
        return;
    ActivePathCounters& c = GetCounters();
    c.msghand_sleep_us_total.fetch_add(nUs, std::memory_order_relaxed);
    c.msghand_sleep_count.fetch_add(1, std::memory_order_relaxed);
}

void RecordBlockDispatchDelay(int64_t nUs, int64_t nCompleteWaiting)
{
    if (!g_IBDActivePathEnabled)
        return;
    ActivePathCounters& c = GetCounters();
    c.block_dispatch_delay_us_total.fetch_add(nUs, std::memory_order_relaxed);
    AtomicMax(c.block_dispatch_delay_us_max, nUs);
    c.block_dispatch_delay_count.fetch_add(1, std::memory_order_relaxed);
    c.complete_block_waiting_sum.fetch_add(nCompleteWaiting, std::memory_order_relaxed);
    AtomicMax(c.complete_block_waiting_max, nCompleteWaiting);
    c.complete_block_waiting_count.fetch_add(1, std::memory_order_relaxed);
}

void RecordCSMainWait(int64_t nUs)
{
    if (!g_IBDActivePathEnabled)
        return;
    ActivePathCounters& c = GetCounters();
    c.cs_main_wait_us_total.fetch_add(nUs, std::memory_order_relaxed);
    AtomicMax(c.cs_main_wait_us_max, nUs);
    c.cs_main_wait_count.fetch_add(1, std::memory_order_relaxed);
}

static const char* IBDStateReasonName(int nReason)
{
    switch (nReason)
    {
    case IBD_REASON_REGTEST: return "regtest";
    case IBD_REASON_IMPORT_REINDEX_NULL: return "import-reindex-null";
    case IBD_REASON_BELOW_ESTIMATE: return "below-estimate";
    case IBD_REASON_PEER_AHEAD_LAG: return "peer-ahead-lag";
    case IBD_REASON_ACTIVE_CATCHUP: return "active-catchup";
    case IBD_REASON_STALE_RECV: return "stale-recv";
    case IBD_REASON_PEER_SNAPSHOT_UNAVAILABLE: return "peer-snapshot-unavailable";
    case IBD_REASON_NOT_IBD: return "not-ibd";
    }
    return "unknown";
}

void RecordIBDStateTrace(bool fIBD, int nReason, int nLocalHeight,
                         int nFreshPeerHeight, int64_t nNow,
                         int64_t nFreshPeerLastBlockRecv,
                         int64_t nLocalLastBlockRecv)
{
    if (!g_IBDActivePathEnabled)
        return;

    const int64_t nEmitNow = MonotonicMicros();
    const bool fChanged =
        g_nLastStateTraceIBD != (fIBD ? 1 : 0) ||
        g_nLastStateTraceReason != nReason;
    const bool fInterval = nEmitNow - g_nLastStateTraceMicros >= 5000000;
    if (!fChanged && !fInterval)
        return;

    g_nLastStateTraceIBD = fIBD ? 1 : 0;
    g_nLastStateTraceReason = nReason;
    g_nLastStateTraceMicros = nEmitNow;

    const int64_t nEstimate = Checkpoints::GetTotalBlocksEstimate();
    const int nPeerMedian = GetNumBlocksOfPeers();
    const int64_t nEstimateAhead = std::max<int64_t>(
        0, nEstimate - (nLocalHeight >= 0 ? nLocalHeight : 0));
    const int64_t nFreshHeightAge = nFreshPeerLastBlockRecv > 0
        ? std::max<int64_t>(0, nNow - nFreshPeerLastBlockRecv) : -1;
    const int64_t nLocalRecvAge = nLocalLastBlockRecv > 0
        ? std::max<int64_t>(0, nNow - nLocalLastBlockRecv) : -1;

    ibdmetrics::Counters& mc = ibdmetrics::Get();
    const int64_t nDeferred = mc.total_deferred_current.load(std::memory_order_relaxed);
    const int64_t nQueued = mc.total_queued_current.load(std::memory_order_relaxed);
    const int64_t nInflight = mc.total_inflight_current.load(std::memory_order_relaxed);
    const int64_t nGetBlocksOut = mc.getblocks_outstanding_current.load(std::memory_order_relaxed);
    const int64_t nEligibleAhead = mc.eligible_ahead_peers_current.load(std::memory_order_relaxed);

    const bool fFalseWhileAhead = (!fIBD) && nEstimateAhead >= 1000;

    printf("IBD_STATE_TRACE time_us=%lld ibd=%d reason=%s local_height=%d fresh_peer_height=%d peer_median=%d gui_estimate=%lld estimate_ahead=%lld fresh_height_age_s=%lld local_last_recv_age_s=%lld deferred=%lld queued=%lld inflight=%lld getblocks_outstanding=%lld eligible_ahead_peers=%lld ibd_false_while_estimate_ahead=%d\n",
           (long long)nEmitNow, fIBD ? 1 : 0,
           IBDStateReasonName(nReason),
           nLocalHeight, nFreshPeerHeight, nPeerMedian,
           (long long)nEstimate, (long long)nEstimateAhead,
           (long long)nFreshHeightAge, (long long)nLocalRecvAge,
           (long long)nDeferred, (long long)nQueued, (long long)nInflight,
           (long long)nGetBlocksOut, (long long)nEligibleAhead,
           fFalseWhileAhead ? 1 : 0);
}

void EmitIBDActive1s(const std::vector<CNode*>& vNodesCopy)
{
    if (!g_IBDActivePathEnabled)
        return;

    const int64_t nNow = MonotonicMicros();
    int64_t nLast = g_nLastEmitMicros.load(std::memory_order_relaxed);
    if (nNow - nLast < 1000000)
        return;
    if (!g_nLastEmitMicros.compare_exchange_weak(
            nLast, nNow, std::memory_order_relaxed))
        return;

    ActivePathCounters& c = GetCounters();

    // Peer gauges (scan only here, once per second).
    int64_t nAskForDepth = 0;
    int64_t nAskForDue = 0;
    int64_t nDeferredPeer = 0;
    int64_t nInflightPeer = 0;
    int64_t nFreeCapacityPeer = 0;
    int64_t nGetBlocksQueuedPeer = 0;
    int64_t nGetBlocksOutstandingPeer = 0;
    int64_t nPeers = 0;
    int64_t nPeersInflightGT0 = 0;
    int64_t nPeersQueuedGT0 = 0;
    int64_t nInflightPeerMax = 0;
    int64_t nQueuedPeerMax = 0;
    const int64_t nNowKey = GetTime() * 1000000;
    const int64_t nCap = GetMaxActiveBlockRequestsPerPeer();
    for (const CNode* pnode : vNodesCopy)
    {
        if (!pnode || pnode->fDisconnect || pnode->nVersion == 0)
            continue;
        ++nPeers;
        nAskForDepth += (int64_t)pnode->mapAskFor.size();
        nDeferredPeer += (int64_t)pnode->deferredBlockInv.size();
        const int64_t nInflightThis = (int64_t)pnode->setBlocksInFlight.size();
        const int64_t nQueuedThis = (int64_t)pnode->setAskForBlocks.size();
        nInflightPeer += nInflightThis;
        if (nInflightThis > 0)
            ++nPeersInflightGT0;
        if (nQueuedThis > 0)
            ++nPeersQueuedGT0;
        nInflightPeerMax = std::max(nInflightPeerMax, nInflightThis);
        nQueuedPeerMax = std::max(nQueuedPeerMax, nQueuedThis);
        nGetBlocksQueuedPeer += (int64_t)pnode->getBlocksIndex.size();
        nGetBlocksOutstandingPeer +=
            pnode->HasOutstandingGetBlocks() ? 1 : 0;
        nFreeCapacityPeer += std::max<int64_t>(
            0, nCap - nInflightThis);
        for (std::multimap<int64_t, CInv>::const_iterator it =
                 pnode->mapAskFor.begin();
             it != pnode->mapAskFor.end() && it->first <= nNowKey; ++it)
        {
            ++nAskForDue;
        }
    }

    // IBD active-window distribution gauges.  Dominant share is the largest
    // single-peer inflight as a percentage of total peer inflight.  Global
    // free slots are 512 - global active (queued+inflight), clamped to
    // [0, 512].  Sample counters increment at most once per 1s emission.
    ibdmetrics::Counters& mc = ibdmetrics::Get();
    const int64_t nGlobalActiveCurrent =
        mc.total_queued_current.load(std::memory_order_relaxed) +
        mc.total_inflight_current.load(std::memory_order_relaxed);
    const int64_t nGlobalFreeActiveSlots = std::max<int64_t>(
        0, (int64_t)MAX_DEFERRED_INV_ACTIVE_GLOBAL - nGlobalActiveCurrent);
    const int64_t nTotalDeferredCurrent =
        mc.total_deferred_current.load(std::memory_order_relaxed);
    int64_t nDominantSharePct = 0;
    if (nInflightPeer > 0)
        nDominantSharePct = (100 * nInflightPeerMax) / nInflightPeer;
    if (nInflightPeer > 0 &&
        100 * nInflightPeerMax >= 75 * nInflightPeer)
    {
        c.samples_single_peer_over_75pct.fetch_add(1, std::memory_order_relaxed);
    }
    if (nTotalDeferredCurrent > 0 &&
        nGlobalFreeActiveSlots > MAX_DEFERRED_INV_ACTIVE_GLOBAL / 2)
    {
        c.samples_global_below_half_with_deferred.fetch_add(
            1, std::memory_order_relaxed);
    }
    c.peers_inflight_gt0.store(nPeersInflightGT0, std::memory_order_relaxed);
    c.peers_queued_gt0.store(nPeersQueuedGT0, std::memory_order_relaxed);
    c.inflight_peer_max.store(nInflightPeerMax, std::memory_order_relaxed);
    c.queued_peer_max.store(nQueuedPeerMax, std::memory_order_relaxed);
    c.dominant_peer_inflight_share_pct.store(
        nDominantSharePct, std::memory_order_relaxed);
    c.global_free_active_slots.store(
        nGlobalFreeActiveSlots, std::memory_order_relaxed);
    c.global_free_slots_with_deferred.store(
        nTotalDeferredCurrent > 0 ? nGlobalFreeActiveSlots : -1,
        std::memory_order_relaxed);

    // Per-second rates from totals delta.
    const int64_t nBlocksAccepted = c.acceptblock_count.load(std::memory_order_relaxed);
    const int64_t nGetBlocksSent = c.getblocks_wire_sent.load(std::memory_order_relaxed);
    const int64_t nGetDataSent = c.getdata_sent_total.load(std::memory_order_relaxed);
    const int64_t nZeroPass = c.getdata_zero_pass_due_capacity.load(std::memory_order_relaxed);
    const int64_t nInvMsgs = c.getblocks_response_inv.load(std::memory_order_relaxed);
    const int64_t dBlocks = nBlocksAccepted - g_prevBlocksAccepted;
    const int64_t dGetBlocks = nGetBlocksSent - g_prevGetBlocksSent;
    const int64_t dGetData = nGetDataSent - g_prevGetDataSent;
    const int64_t dZeroPass = nZeroPass - g_prevZeroPassDueCapacity;
    const int64_t dInv = nInvMsgs - g_prevGetBlocksResponseInv;
    g_prevBlocksAccepted = nBlocksAccepted;
    g_prevGetBlocksSent = nGetBlocksSent;
    g_prevGetDataSent = nGetDataSent;
    g_prevZeroPassDueCapacity = nZeroPass;
    g_prevGetBlocksResponseInv = nInvMsgs;

    printf("IBD_ACTIVE_1S time_us=%lld uptime_s=%lld local_height=%d peers=%lld "
           "blocks_1s=%lld getblocks_sent_1s=%lld getdata_sent_1s=%lld zero_pass_due_cap_1s=%lld inv_msgs_1s=%lld "
           "askfor_depth=%lld askfor_due=%lld deferred_peer=%lld inflight_peer=%lld free_capacity_peer=%lld "
           "getblocks_queued_peer=%lld getblocks_outstanding_peer=%lld "
           "passes_sent=%lld stop_empty=%lld stop_no_due=%lld stop_inflight_cap=%lld "
           "askfor_to_getdata_avg_us=%lld askfor_to_getdata_max_us=%lld "
           "pass_interval_avg_us=%lld pass_interval_max_us=%lld sleep_us=%lld "
           "dispatch_delay_avg_us=%lld dispatch_delay_max_us=%lld cs_main_wait_avg_us=%lld cs_main_wait_max_us=%lld "
           "processblock_avg_us=%lld acceptblock_avg_us=%lld addtoblockindex_avg_us=%lld setbestchain_avg_us=%lld connectblock_avg_us=%lld "
           "raw_write_avg_us=%lld filecommit_avg_us=%lld blockindex_commit_avg_us=%lld chainstate_commit_avg_us=%lld dag_commit_avg_us=%lld wallet_cb_avg_us=%lld "
           "peers_inflight_gt0=%lld peers_queued_gt0=%lld inflight_peer_max=%lld queued_peer_max=%lld "
           "dominant_peer_inflight_share_pct=%lld global_free_active_slots=%lld global_free_slots_with_deferred=%lld "
           "global_active_current=%lld global_inflight_current=%lld global_queued_current=%lld global_deferred_current=%lld "
           "samples_single_peer_over_75pct=%lld samples_global_below_half_with_deferred=%lld "
           "wire_latency_avg_us=%lld wire_latency_max_us=%lld\n",
           (long long)nNow,
           (long long)((nNow - g_nTraceEpochMicros) / 1000000),
           nBestHeight, (long long)nPeers,
           (long long)dBlocks, (long long)dGetBlocks,
           (long long)dGetData, (long long)dZeroPass, (long long)dInv,
           (long long)nAskForDepth, (long long)nAskForDue,
           (long long)nDeferredPeer, (long long)nInflightPeer,
           (long long)nFreeCapacityPeer,
           (long long)nGetBlocksQueuedPeer, (long long)nGetBlocksOutstandingPeer,
           (long long)c.getdata_pass_sent_count.load(std::memory_order_relaxed),
           (long long)c.askfor_stop_empty.load(std::memory_order_relaxed),
           (long long)c.askfor_stop_no_due.load(std::memory_order_relaxed),
           (long long)c.askfor_stop_inflight_cap.load(std::memory_order_relaxed),
           (long long)Avg(c.askfor_to_getdata_us_total.load(std::memory_order_relaxed),
                          c.askfor_to_getdata_count.load(std::memory_order_relaxed)),
           (long long)c.askfor_to_getdata_us_max.load(std::memory_order_relaxed),
           (long long)Avg(c.msghand_pass_interval_us_total.load(std::memory_order_relaxed),
                          c.msghand_pass_interval_count.load(std::memory_order_relaxed)),
           (long long)c.msghand_pass_interval_us_max.load(std::memory_order_relaxed),
           (long long)c.msghand_sleep_us_total.load(std::memory_order_relaxed),
           (long long)Avg(c.block_dispatch_delay_us_total.load(std::memory_order_relaxed),
                          c.block_dispatch_delay_count.load(std::memory_order_relaxed)),
           (long long)c.block_dispatch_delay_us_max.load(std::memory_order_relaxed),
           (long long)Avg(c.cs_main_wait_us_total.load(std::memory_order_relaxed),
                          c.cs_main_wait_count.load(std::memory_order_relaxed)),
           (long long)c.cs_main_wait_us_max.load(std::memory_order_relaxed),
           (long long)Avg(c.processblock_us_total.load(std::memory_order_relaxed),
                          c.processblock_count.load(std::memory_order_relaxed)),
           (long long)Avg(c.acceptblock_us_total.load(std::memory_order_relaxed),
                          c.acceptblock_count.load(std::memory_order_relaxed)),
           (long long)Avg(c.addtoblockindex_us_total.load(std::memory_order_relaxed),
                          c.addtoblockindex_count.load(std::memory_order_relaxed)),
           (long long)Avg(c.setbestchain_us_total.load(std::memory_order_relaxed),
                          c.setbestchain_count.load(std::memory_order_relaxed)),
           (long long)Avg(c.connectblock_us_total.load(std::memory_order_relaxed),
                          c.connectblock_count.load(std::memory_order_relaxed)),
           (long long)Avg(c.raw_block_write_us_total.load(std::memory_order_relaxed),
                          c.raw_block_write_count.load(std::memory_order_relaxed)),
           (long long)Avg(c.filecommit_us_total.load(std::memory_order_relaxed),
                          c.filecommit_count.load(std::memory_order_relaxed)),
           (long long)Avg(c.blockindex_commit_us_total.load(std::memory_order_relaxed),
                          c.blockindex_commit_count.load(std::memory_order_relaxed)),
           (long long)Avg(c.chainstate_commit_us_total.load(std::memory_order_relaxed),
                          c.chainstate_commit_count.load(std::memory_order_relaxed)),
            (long long)Avg(c.dag_epoch_commit_us_total.load(std::memory_order_relaxed),
                           c.dag_epoch_commit_count.load(std::memory_order_relaxed)),
            (long long)Avg(c.wallet_callback_us_total.load(std::memory_order_relaxed),
                           c.wallet_callback_count.load(std::memory_order_relaxed)),
            (long long)c.peers_inflight_gt0.load(std::memory_order_relaxed),
            (long long)c.peers_queued_gt0.load(std::memory_order_relaxed),
            (long long)c.inflight_peer_max.load(std::memory_order_relaxed),
            (long long)c.queued_peer_max.load(std::memory_order_relaxed),
            (long long)c.dominant_peer_inflight_share_pct.load(std::memory_order_relaxed),
            (long long)c.global_free_active_slots.load(std::memory_order_relaxed),
            (long long)c.global_free_slots_with_deferred.load(std::memory_order_relaxed),
            (long long)nGlobalActiveCurrent,
            (long long)mc.total_inflight_current.load(std::memory_order_relaxed),
            (long long)mc.total_queued_current.load(std::memory_order_relaxed),
            (long long)nTotalDeferredCurrent,
            (long long)c.samples_single_peer_over_75pct.load(std::memory_order_relaxed),
            (long long)c.samples_global_below_half_with_deferred.load(std::memory_order_relaxed),
            (long long)Avg(c.block_request_wire_latency_us_total.load(std::memory_order_relaxed),
                           c.block_request_wire_latency_count.load(std::memory_order_relaxed)),
            (long long)c.block_request_wire_latency_us_max.load(std::memory_order_relaxed));
}

} // namespace ibdactivepath
