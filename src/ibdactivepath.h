// Copyright (c) 2026 The Innova developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef INNOVA_IBDACTIVEPATH_H
#define INNOVA_IBDACTIVEPATH_H

#include <stdint.h>

#include <atomic>
#include <vector>

#include "uint256.h"

class CNode;

// Runtime-only IBD active-path throughput instrumentation.
//
// Disabled by default (-ibdactivepathtrace=0).  When enabled it proves where
// the time is lost between useful blocks without changing any P2P/IBD
// behavior, limits, sleeps, timeouts, scheduling, peer selection, request
// ownership, DB behavior, or IsInitialBlockDownload().
//
// All event counters are relaxed atomics (no new hot lock, no per-event
// strings or printf).  The only formatted output is the aggregated
// IBD_ACTIVE_1S line emitted once per second from the message-handler thread,
// rate-limited IBD_SLOW_BLOCK events, and IBD_STATE_TRACE transition lines.

namespace ibdactivepath {

// Monotonic microsecond clock (CLOCK_MONOTONIC where available).
int64_t MonotonicMicros();

bool InitIBDActivePathTrace(bool fEnabled);
bool IBDActivePathTraceEnabled();

enum GetDataStopReason
{
    GETDATA_STOP_EMPTY = 0,
    GETDATA_STOP_NO_DUE,
    GETDATA_STOP_INFLIGHT_CAP,
    GETDATA_STOP_OTHER
};

enum IBDStateReason
{
    IBD_REASON_REGTEST = 0,
    IBD_REASON_IMPORT_REINDEX_NULL,
    IBD_REASON_BELOW_ESTIMATE,
    IBD_REASON_PEER_AHEAD_LAG,
    IBD_REASON_ACTIVE_CATCHUP,
    IBD_REASON_STALE_RECV,
    IBD_REASON_PEER_SNAPSHOT_UNAVAILABLE,
    IBD_REASON_NOT_IBD
};

struct ActivePathCounters
{
    // 1. Inventory supply
    std::atomic<int64_t> getblocks_wire_sent;
    std::atomic<int64_t> getblocks_response_inv;
    std::atomic<int64_t> getblocks_response_block_inv;
    std::atomic<int64_t> getblocks_response_unknown;
    std::atomic<int64_t> response_latency_us_total;
    std::atomic<int64_t> response_latency_us_max;
    std::atomic<int64_t> response_latency_count;
    std::atomic<int64_t> useful_gap_us_total;
    std::atomic<int64_t> useful_gap_us_max;
    std::atomic<int64_t> useful_gap_count;

    // 2. getdata scheduling
    std::atomic<int64_t> getdata_sent_total;
    std::atomic<int64_t> getdata_pass_sent_count;
    std::atomic<int64_t> getdata_pass_sent_max;
    std::atomic<int64_t> getdata_zero_pass_due_capacity;
    std::atomic<int64_t> askfor_stop_empty;
    std::atomic<int64_t> askfor_stop_no_due;
    std::atomic<int64_t> askfor_stop_inflight_cap;
    std::atomic<int64_t> askfor_stop_other;
    std::atomic<int64_t> askfor_to_getdata_us_total;
    std::atomic<int64_t> askfor_to_getdata_us_max;
    std::atomic<int64_t> askfor_to_getdata_count;
    std::atomic<int64_t> msghand_pass_interval_us_total;
    std::atomic<int64_t> msghand_pass_interval_us_max;
    std::atomic<int64_t> msghand_pass_interval_count;
    std::atomic<int64_t> msghand_sleep_us_total;
    std::atomic<int64_t> msghand_sleep_count;

    // 3. Validation
    std::atomic<int64_t> block_dispatch_delay_us_total;
    std::atomic<int64_t> block_dispatch_delay_us_max;
    std::atomic<int64_t> block_dispatch_delay_count;
    std::atomic<int64_t> complete_block_waiting_sum;
    std::atomic<int64_t> complete_block_waiting_max;
    std::atomic<int64_t> complete_block_waiting_count;
    std::atomic<int64_t> cs_main_wait_us_total;
    std::atomic<int64_t> cs_main_wait_us_max;
    std::atomic<int64_t> cs_main_wait_count;
    std::atomic<int64_t> processblock_us_total;
    std::atomic<int64_t> processblock_us_max;
    std::atomic<int64_t> processblock_count;
    std::atomic<int64_t> acceptblock_us_total;
    std::atomic<int64_t> acceptblock_us_max;
    std::atomic<int64_t> acceptblock_count;
    std::atomic<int64_t> addtoblockindex_us_total;
    std::atomic<int64_t> addtoblockindex_us_max;
    std::atomic<int64_t> addtoblockindex_count;
    std::atomic<int64_t> setbestchain_us_total;
    std::atomic<int64_t> setbestchain_us_max;
    std::atomic<int64_t> setbestchain_count;
    std::atomic<int64_t> connectblock_us_total;
    std::atomic<int64_t> connectblock_us_max;
    std::atomic<int64_t> connectblock_count;

    // 4. DB/disk
    std::atomic<int64_t> raw_block_write_us_total;
    std::atomic<int64_t> raw_block_write_us_max;
    std::atomic<int64_t> raw_block_write_count;
    std::atomic<int64_t> filecommit_us_total;
    std::atomic<int64_t> filecommit_us_max;
    std::atomic<int64_t> filecommit_count;
    std::atomic<int64_t> blockindex_commit_us_total;
    std::atomic<int64_t> blockindex_commit_us_max;
    std::atomic<int64_t> blockindex_commit_count;
    std::atomic<int64_t> chainstate_commit_us_total;
    std::atomic<int64_t> chainstate_commit_us_max;
    std::atomic<int64_t> chainstate_commit_count;
    std::atomic<int64_t> dag_epoch_commit_us_total;
    std::atomic<int64_t> dag_epoch_commit_us_max;
    std::atomic<int64_t> dag_epoch_commit_count;
    std::atomic<int64_t> wallet_callback_us_total;
    std::atomic<int64_t> wallet_callback_us_max;
    std::atomic<int64_t> wallet_callback_count;

    // 5. IBD active-window distribution (per-second sampled gauges, updated
    // once per IBD_ACTIVE_1S emission) and monotonic wire latency.
    std::atomic<int64_t> peers_inflight_gt0;
    std::atomic<int64_t> peers_queued_gt0;
    std::atomic<int64_t> inflight_peer_max;
    std::atomic<int64_t> queued_peer_max;
    std::atomic<int64_t> dominant_peer_inflight_share_pct;
    std::atomic<int64_t> global_free_active_slots;
    std::atomic<int64_t> global_free_slots_with_deferred;
    std::atomic<int64_t> samples_single_peer_over_75pct;
    std::atomic<int64_t> samples_global_below_half_with_deferred;
    std::atomic<int64_t> block_request_wire_latency_us_total;
    std::atomic<int64_t> block_request_wire_latency_us_max;
    std::atomic<int64_t> block_request_wire_latency_count;

    ActivePathCounters();
};

ActivePathCounters& GetCounters();

// RAII phase timer: records duration into a (total,max,count) bucket and
// rate-limits an IBD_SLOW_BLOCK event for genuinely slow phases.
class ActivePathTimer
{
public:
    ActivePathTimer(std::atomic<int64_t>& total,
                    std::atomic<int64_t>& max,
                    std::atomic<int64_t>& count,
                    const char* pszPhase,
                    int nHeight);
    ~ActivePathTimer();

private:
    int64_t nStart;
    const char* pszPhase;
    int nHeight;
    std::atomic<int64_t>* pTotal;
    std::atomic<int64_t>* pMax;
    std::atomic<int64_t>* pCount;
};

// Event recording hooks (no-ops when disabled).
void RecordGetBlocksWireSent();
void RecordGetBlocksResponse(int64_t nBlockInv, int64_t nUnknown);
void RecordUsefulResponseEnd();
void RecordGetDataPass(int64_t nSent, bool fDueRequests,
                       bool fFreeCapacity, int nStopReason);
void RecordAskForToGetData(int64_t nWaitUs);

// Monotonic AskFor-enqueue → getdata-export wire latency (bounded, lock
// guarded, no-op when trace disabled).  RecordBlockRequestSent() consumes the
// enqueue record and folds the wait into
// block_request_wire_latency_{total,max,count}.
void RecordBlockRequestEnqueued(const uint256& hash);
void RecordBlockRequestSent(const uint256& hash);
void RecordMessageHandlerPassInterval(int64_t nUs);
void RecordMessageHandlerSleep(int64_t nUs);
void RecordBlockDispatchDelay(int64_t nUs, int64_t nCompleteWaiting);
void RecordCSMainWait(int64_t nUs);

// One aggregated IBD_ACTIVE_1S line per second.  Called from the
// message-handler thread with the ref-counted peer list; scans peers only
// here (never per block).
void EmitIBDActive1s(const std::vector<CNode*>& vNodesCopy);

// IBD state observability.  Does not change the result of
// IsInitialBlockDownload(); emits IBD_STATE_TRACE on reason/state change or
// every 5s.
void RecordIBDStateTrace(bool fIBD, int nReason, int nLocalHeight,
                         int nFreshPeerHeight, int64_t nNow,
                         int64_t nFreshPeerLastBlockRecv,
                         int64_t nLocalLastBlockRecv);

} // namespace ibdactivepath

#endif // INNOVA_IBDACTIVEPATH_H
