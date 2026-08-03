// Copyright (c) 2026 The Innova developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "ibdmetrics.h"

#include "util.h"

#include <algorithm>

namespace ibdmetrics {

namespace {

Counters g_counters;
// Process-start timestamp.  File-scope dynamic initialization runs at
// program startup, before any IBD activity.
const int64_t g_nStartTime = GetTime();

} // namespace

Counters::Counters()
    : deferred_budget_calls(0),
      deferred_budget_positive(0),
      deferred_budget_zero(0),
      deferred_budget_zero_peer_pressure(0),
      deferred_budget_zero_global_pressure(0),
      deferred_budget_zero_vnodes_lock_failed(0),
      block_inv_unknown_total(0),
      block_inv_admitted(0),
      block_inv_deferred(0),
      block_inv_deferred_no_budget(0),
      block_inv_deferred_overflow(0),
      frontier_exemption_admitted(0),
      global_active_current(0),
      global_active_max(0),
      peer_pressure_max(0),
      orphan_pressure_max(0),
      peers_at_zero_budget_current(0),
      peers_at_zero_budget_max(0),
      refill_calls(0),
      refill_calls_zero_budget(0),
      refill_items_examined(0),
      refill_items_admitted(0),
      refill_items_already_have(0),
      refill_items_active_owner(0),
      refill_work_limit_hit(0),
      refill_txdb_opens(0),
      refill_alreadyhave_checks(0),
      block_receive_total(0),
      block_result_accepted_active(0),
      block_result_orphan_new(0),
      setbestchain_commits(0),
      stalled_recovery_attempts(0),
      active_decrement_receive_clear_inflight(0),
      active_decrement_inflight_timeout(0),
      active_decrement_askfor_sent_transition(0),
      active_decrement_askfor_removed_already_have(0),
      active_decrement_askfor_removed_owner_conflict(0),
      active_decrement_clear_askfor(0),
      active_decrement_disconnect_cleanup(0),
      active_decrement_other(0),
      global_active_zero_transitions(0),
      zero_with_total_deferred_nonempty(0),
      zero_with_total_deferred_empty(0),
      zero_with_eligible_ahead_peer(0),
      zero_without_eligible_ahead_peer(0),
      zero_duration_total_ms(0),
      zero_duration_max_ms(0),
      zero_start_time_ms(0),
      refill_opportunity_slot_freed(0),
      refill_sendmessages_passes(0),
      refill_skipped_cs_main_trylock_failed(0),
      refill_called_deferred_empty(0),
      refill_called_positive_budget_nonempty(0),
      refill_positive_budget_admitted_zero(0),
      refill_positive_budget_admitted_count(0),
      pipeline_drained_checks(0),
      pipeline_drained_getblocks_queued(0),
      pipeline_drained_skip_not_ahead(0),
      pipeline_drained_skip_getblocks_10s_cooldown(0),
      pipeline_drained_skip_other_condition(0),
      pushgetblocks_dedup_5s_skips(0),
      askfor_skip_orphan_limit_cooldown(0),
      orphan_limit_cooldown_recorded(0),
      orphan_limit_cross_peer_admitted(0),
      orphan_limit_frontier_retry_queued(0),
      orphan_limit_frontier_retry_pending(0),
      askfor_skip_mapalreadyasked_cap(0),
      askfor_skip_other_peer_owner(0),
      askfor_skip_already_queued(0),
      askfor_skip_inflight(0),
      total_deferred_current(0),
      total_deferred_max(0),
      total_queued_current(0),
      total_queued_max(0),
      total_inflight_current(0),
      total_inflight_max(0),
      eligible_ahead_peers_current(0),
      eligible_ahead_peers_max(0),
      getblocks_decision_attempts_other(0),
      getblocks_decision_attempts_initial(0),
      getblocks_decision_attempts_continuation(0),
      getblocks_decision_attempts_recovery(0),
      getblocks_decision_attempts_prefetch(0),
      getblocks_decision_attempts_inv_continuation(0),
      getblocks_decision_attempts_version(0),
      getblocks_decision_attempts_headers(0),
      getblocks_decision_attempts_checkpoint(0),
      getblocks_decision_attempts_wallet_rescan(0),
      getblocks_decision_attempts_orphan_limit(0),
      getblocks_decision_attempts_empty_pipeline_wake(0),
      getblocks_queue_success_other(0),
      getblocks_queue_success_initial(0),
      getblocks_queue_success_continuation(0),
      getblocks_queue_success_recovery(0),
      getblocks_queue_success_prefetch(0),
      getblocks_queue_success_inv_continuation(0),
      getblocks_queue_success_version(0),
      getblocks_queue_success_headers(0),
      getblocks_queue_success_checkpoint(0),
      getblocks_queue_success_wallet_rescan(0),
      getblocks_queue_success_orphan_limit(0),
      getblocks_queue_success_empty_pipeline_wake(0),
      getblocks_wire_sent_other(0),
      getblocks_wire_sent_initial(0),
      getblocks_wire_sent_continuation(0),
      getblocks_wire_sent_recovery(0),
      getblocks_wire_sent_prefetch(0),
      getblocks_wire_sent_inv_continuation(0),
      getblocks_wire_sent_version(0),
      getblocks_wire_sent_headers(0),
      getblocks_wire_sent_checkpoint(0),
      getblocks_wire_sent_wallet_rescan(0),
      getblocks_wire_sent_orphan_limit(0),
      getblocks_wire_sent_empty_pipeline_wake(0),
      getblocks_dedup_skips(0),
      getblocks_identical_to_last_sent(0),
      getblocks_response_inv_messages(0),
      getblocks_response_block_inv_count(0),
      getblocks_response_unknown_count(0),
      getblocks_response_zero_unknown(0),
      getblocks_response_inv_zero_unknown(0),
      recovery_outcome_useful(0),
      recovery_outcome_known_only(0),
      recovery_outcome_no_response(0),
      zero_latency_recorded_mask(0),
      zero_to_first_getblocks_wire_send_ms_total(0),
      zero_to_first_getblocks_wire_send_ms_max(0),
      zero_to_first_inv_ms_total(0),
      zero_to_first_inv_ms_max(0),
      zero_to_first_unknown_inv_ms_total(0),
      zero_to_first_unknown_inv_ms_max(0),
      zero_to_first_askfor_ms_total(0),
      zero_to_first_askfor_ms_max(0),
      zero_to_active_nonzero_ms_total(0),
      zero_to_active_nonzero_ms_max(0),
      inv_unknown_during_zero_global(0),
      sync_peer_change_while_pipeline_empty(0),
      getblocks_outstanding_current(0),
      getblocks_outstanding_max(0),
      getblocks_no_response_disconnect_cleanup(0),
      getblocks_queued_unsent_cleanup(0),
      peers_with_queued_getblocks_current(0),
      peers_with_queued_getblocks_max(0),
      total_getblocks_queued_requests_current(0),
      total_getblocks_queued_requests_max(0),
      pipeline_active_due_to_getblocks_current(0),
      pipeline_active_due_to_getblocks_max(0),
      frontier_response_armed(0),
      frontier_response_consumed(0),
      frontier_response_pending_current(0),
      frontier_response_pending_max(0),
      frontier_reject_locator_stale(0),
      frontier_reject_slot_busy(0),
      frontier_reject_already_admitted(0),
      frontier_reject_other(0),
      ibd_state_current(-1),
      ibd_state_transitions(0),
      pipeline_wake_signals(0),
      pipeline_wake_signal_clear_inflight(0),
      pipeline_wake_signal_inflight_timeout(0),
      pipeline_wake_signal_askfor_already_have(0),
      pipeline_wake_signal_askfor_owner_conflict(0),
      pipeline_wake_signal_queue_removal(0),
      pipeline_wake_signal_clear_askfor(0),
      pipeline_wake_signal_disconnect_cleanup(0),
      pipeline_wake_signal_getblocks_outstanding_cleared(0),
      pipeline_wake_signal_other(0),
      pipeline_wake_coalesced(0),
      pipeline_wake_handler_runs(0),
      pipeline_wake_transient_cs_main_trylock_failed(0),
      pipeline_wake_transient_cs_vnodes_trylock_failed(0),
      pipeline_wake_transient_cooldown_active(0),
      pipeline_wake_transient_dedup_all(0),
      pipeline_wake_transient_incomplete_peer_scan(0),
      pipeline_wake_transient_shutdown(0),
      pipeline_wake_terminal_not_ibd(0),
      pipeline_wake_terminal_pipeline_not_empty(0),
      pipeline_wake_terminal_deferred_refill_created_work(0),
      pipeline_wake_terminal_getblocks_queued(0),
      pipeline_wake_terminal_no_eligible_ahead_peer(0),
      pipeline_wake_terminal_existing_queued_getblocks(0),
      pipeline_wake_terminal_outstanding_getblocks_present(0),
      pipeline_wake_refill_attempts(0),
      pipeline_wake_refill_admitted(0),
      pipeline_wake_getblocks_attempted(0),
      pipeline_wake_getblocks_queued(0),
      pipeline_wake_getblocks_dedup(0),
      pipeline_wake_active_restored(0),
      pipeline_wake_signal_start_ms(0),
      pipeline_wake_signal_to_active_ms_total(0),
      pipeline_wake_signal_to_active_ms_max(0),
      diversify_candidates(0),
      diversify_picked_other_lane(0),
      diversify_picked_announcer(0),
      diversify_snapshot_skip_lock(0),
      diversify_no_other_lane(0),
      diversify_other_lane_timeout(0)
{
}

Counters& Get()
{
    return g_counters;
}

static void IncrementDecrementCause(Counters& c, ActiveDecrementCause cause,
                                    int64_t units)
{
    switch (cause)
    {
    case ACTIVE_DECREMENT_RECEIVE_CLEAR_INFLIGHT:
        c.active_decrement_receive_clear_inflight.fetch_add(units, std::memory_order_relaxed);
        break;
    case ACTIVE_DECREMENT_INFLIGHT_TIMEOUT:
        c.active_decrement_inflight_timeout.fetch_add(units, std::memory_order_relaxed);
        break;
    case ACTIVE_DECREMENT_ASKFOR_SENT_TRANSITION:
        c.active_decrement_askfor_sent_transition.fetch_add(units, std::memory_order_relaxed);
        break;
    case ACTIVE_DECREMENT_ASKFOR_REMOVED_ALREADY_HAVE:
        c.active_decrement_askfor_removed_already_have.fetch_add(units, std::memory_order_relaxed);
        break;
    case ACTIVE_DECREMENT_ASKFOR_REMOVED_OWNER_CONFLICT:
        c.active_decrement_askfor_removed_owner_conflict.fetch_add(units, std::memory_order_relaxed);
        break;
    case ACTIVE_DECREMENT_CLEAR_ASKFOR:
        c.active_decrement_clear_askfor.fetch_add(units, std::memory_order_relaxed);
        break;
    case ACTIVE_DECREMENT_DISCONNECT_CLEANUP:
        c.active_decrement_disconnect_cleanup.fetch_add(units, std::memory_order_relaxed);
        break;
    case ACTIVE_DECREMENT_OTHER:
        c.active_decrement_other.fetch_add(units, std::memory_order_relaxed);
        break;
    }
}

void GlobalActiveAdd(int64_t delta, ActiveDecrementCause cause)
{
    Counters& c = Get();
    const int64_t nOld = c.global_active_current.fetch_add(
                             delta, std::memory_order_relaxed);
    const int64_t nNew = nOld + delta;
    if (delta < 0)
    {
        const int64_t nUnits = -delta;
        IncrementDecrementCause(c, cause, nUnits);
        c.refill_opportunity_slot_freed.fetch_add(nUnits, std::memory_order_relaxed);
    }
    if (nNew > 0)
        AtomicMax(c.global_active_max, nNew);
    if (nOld > 0 && nNew <= 0)
    {
        c.global_active_zero_transitions.fetch_add(1, std::memory_order_relaxed);
        if (c.total_deferred_current.load(std::memory_order_relaxed) > 0)
            c.zero_with_total_deferred_nonempty.fetch_add(1, std::memory_order_relaxed);
        else
            c.zero_with_total_deferred_empty.fetch_add(1, std::memory_order_relaxed);
        if (c.eligible_ahead_peers_current.load(std::memory_order_relaxed) > 0)
            c.zero_with_eligible_ahead_peer.fetch_add(1, std::memory_order_relaxed);
        else
            c.zero_without_eligible_ahead_peer.fetch_add(1, std::memory_order_relaxed);
        c.zero_latency_recorded_mask.store(0, std::memory_order_relaxed);
        c.zero_start_time_ms.store(GetTimeMillis(), std::memory_order_relaxed);
    }
    else if (nOld <= 0 && nNew > 0)
    {
        const int64_t nStart = c.zero_start_time_ms.exchange(0, std::memory_order_relaxed);
        if (nStart > 0)
        {
            const int64_t nDuration = std::max<int64_t>(0, GetTimeMillis() - nStart);
            c.zero_duration_total_ms.fetch_add(nDuration, std::memory_order_relaxed);
            AtomicMax(c.zero_duration_max_ms, nDuration);
            c.zero_to_active_nonzero_ms_total.fetch_add(nDuration, std::memory_order_relaxed);
            AtomicMax(c.zero_to_active_nonzero_ms_max, nDuration);
            const int64_t nWakeStart = c.pipeline_wake_signal_start_ms.exchange(0, std::memory_order_relaxed);
            if (nWakeStart > 0)
            {
                const int64_t nWakeDuration = std::max<int64_t>(0, GetTimeMillis() - nWakeStart);
                c.pipeline_wake_active_restored.fetch_add(1, std::memory_order_relaxed);
                c.pipeline_wake_signal_to_active_ms_total.fetch_add(nWakeDuration, std::memory_order_relaxed);
                AtomicMax(c.pipeline_wake_signal_to_active_ms_max, nWakeDuration);
            }
        }
    }
}

static void GaugeAdd(std::atomic<int64_t>& current, std::atomic<int64_t>& max,
                     int64_t delta)
{
    const int64_t nNew = current.fetch_add(delta, std::memory_order_relaxed) + delta;
    if (nNew > 0)
        AtomicMax(max, nNew);
}

void DeferredAdd(int64_t delta)
{
    Counters& c = Get();
    GaugeAdd(c.total_deferred_current, c.total_deferred_max, delta);
}

void QueuedAdd(int64_t delta)
{
    Counters& c = Get();
    GaugeAdd(c.total_queued_current, c.total_queued_max, delta);
}

void InflightAdd(int64_t delta)
{
    Counters& c = Get();
    GaugeAdd(c.total_inflight_current, c.total_inflight_max, delta);
}

void SetEligibleAheadPeers(int64_t count)
{
    Counters& c = Get();
    c.eligible_ahead_peers_current.store(count, std::memory_order_relaxed);
    AtomicMax(c.eligible_ahead_peers_max, count);
}

static std::atomic<int64_t>* GetBlocksDecisionCounter(Counters& c,
                                                       GetBlocksSource source)
{
    switch (source)
    {
    case GETBLOCKS_SOURCE_OTHER: return &c.getblocks_decision_attempts_other;
    case GETBLOCKS_SOURCE_INITIAL: return &c.getblocks_decision_attempts_initial;
    case GETBLOCKS_SOURCE_CONTINUATION: return &c.getblocks_decision_attempts_continuation;
    case GETBLOCKS_SOURCE_RECOVERY: return &c.getblocks_decision_attempts_recovery;
    case GETBLOCKS_SOURCE_PREFETCH: return &c.getblocks_decision_attempts_prefetch;
    case GETBLOCKS_SOURCE_INV_CONTINUATION: return &c.getblocks_decision_attempts_inv_continuation;
    case GETBLOCKS_SOURCE_VERSION: return &c.getblocks_decision_attempts_version;
    case GETBLOCKS_SOURCE_HEADERS: return &c.getblocks_decision_attempts_headers;
    case GETBLOCKS_SOURCE_CHECKPOINT: return &c.getblocks_decision_attempts_checkpoint;
    case GETBLOCKS_SOURCE_WALLET_RESCAN: return &c.getblocks_decision_attempts_wallet_rescan;
    case GETBLOCKS_SOURCE_ORPHAN_LIMIT: return &c.getblocks_decision_attempts_orphan_limit;
    case GETBLOCKS_SOURCE_EMPTY_PIPELINE_WAKE: return &c.getblocks_decision_attempts_empty_pipeline_wake;
    }
    return NULL;
}

static std::atomic<int64_t>* GetBlocksQueueCounter(Counters& c,
                                                    GetBlocksSource source)
{
    switch (source)
    {
    case GETBLOCKS_SOURCE_OTHER: return &c.getblocks_queue_success_other;
    case GETBLOCKS_SOURCE_INITIAL: return &c.getblocks_queue_success_initial;
    case GETBLOCKS_SOURCE_CONTINUATION: return &c.getblocks_queue_success_continuation;
    case GETBLOCKS_SOURCE_RECOVERY: return &c.getblocks_queue_success_recovery;
    case GETBLOCKS_SOURCE_PREFETCH: return &c.getblocks_queue_success_prefetch;
    case GETBLOCKS_SOURCE_INV_CONTINUATION: return &c.getblocks_queue_success_inv_continuation;
    case GETBLOCKS_SOURCE_VERSION: return &c.getblocks_queue_success_version;
    case GETBLOCKS_SOURCE_HEADERS: return &c.getblocks_queue_success_headers;
    case GETBLOCKS_SOURCE_CHECKPOINT: return &c.getblocks_queue_success_checkpoint;
    case GETBLOCKS_SOURCE_WALLET_RESCAN: return &c.getblocks_queue_success_wallet_rescan;
    case GETBLOCKS_SOURCE_ORPHAN_LIMIT: return &c.getblocks_queue_success_orphan_limit;
    case GETBLOCKS_SOURCE_EMPTY_PIPELINE_WAKE: return &c.getblocks_queue_success_empty_pipeline_wake;
    }
    return NULL;
}

static std::atomic<int64_t>* GetBlocksWireCounter(Counters& c,
                                                   GetBlocksSource source)
{
    switch (source)
    {
    case GETBLOCKS_SOURCE_OTHER: return &c.getblocks_wire_sent_other;
    case GETBLOCKS_SOURCE_INITIAL: return &c.getblocks_wire_sent_initial;
    case GETBLOCKS_SOURCE_CONTINUATION: return &c.getblocks_wire_sent_continuation;
    case GETBLOCKS_SOURCE_RECOVERY: return &c.getblocks_wire_sent_recovery;
    case GETBLOCKS_SOURCE_PREFETCH: return &c.getblocks_wire_sent_prefetch;
    case GETBLOCKS_SOURCE_INV_CONTINUATION: return &c.getblocks_wire_sent_inv_continuation;
    case GETBLOCKS_SOURCE_VERSION: return &c.getblocks_wire_sent_version;
    case GETBLOCKS_SOURCE_HEADERS: return &c.getblocks_wire_sent_headers;
    case GETBLOCKS_SOURCE_CHECKPOINT: return &c.getblocks_wire_sent_checkpoint;
    case GETBLOCKS_SOURCE_WALLET_RESCAN: return &c.getblocks_wire_sent_wallet_rescan;
    case GETBLOCKS_SOURCE_ORPHAN_LIMIT: return &c.getblocks_wire_sent_orphan_limit;
    case GETBLOCKS_SOURCE_EMPTY_PIPELINE_WAKE: return &c.getblocks_wire_sent_empty_pipeline_wake;
    }
    return NULL;
}

void RecordGetBlocksDecision(GetBlocksSource source)
{
    std::atomic<int64_t>* counter = GetBlocksDecisionCounter(Get(), source);
    if (counter)
        counter->fetch_add(1, std::memory_order_relaxed);
}

void RecordGetBlocksQueueSuccess(GetBlocksSource source)
{
    std::atomic<int64_t>* counter = GetBlocksQueueCounter(Get(), source);
    if (counter)
        counter->fetch_add(1, std::memory_order_relaxed);
}

void RecordGetBlocksWireSent(GetBlocksSource source)
{
    std::atomic<int64_t>* counter = GetBlocksWireCounter(Get(), source);
    if (counter)
        counter->fetch_add(1, std::memory_order_relaxed);
    RecordZeroLatency(ZERO_LATENCY_GETBLOCKS_WIRE_SEND);
}

void GetBlocksQueuedAdd(int64_t delta, bool peerTransition)
{
    Counters& c = Get();
    const int64_t nQueued = c.total_getblocks_queued_requests_current.fetch_add(
                               delta, std::memory_order_relaxed) + delta;
    c.pipeline_active_due_to_getblocks_current.store(
        nQueued, std::memory_order_relaxed);
    if (nQueued > 0)
    {
        AtomicMax(c.total_getblocks_queued_requests_max, nQueued);
        AtomicMax(c.pipeline_active_due_to_getblocks_max, nQueued);
    }
    if (peerTransition)
    {
        const int64_t nPeers = c.peers_with_queued_getblocks_current.fetch_add(
                                  delta > 0 ? 1 : -1,
                                  std::memory_order_relaxed) +
                              (delta > 0 ? 1 : -1);
        if (nPeers > 0)
            AtomicMax(c.peers_with_queued_getblocks_max, nPeers);
    }
}

void GetBlocksOutstandingAdd(int64_t delta)
{
    Counters& c = Get();
    const int64_t nOutstanding = c.getblocks_outstanding_current.fetch_add(
                                    delta, std::memory_order_relaxed) + delta;
    if (nOutstanding > 0)
        AtomicMax(c.getblocks_outstanding_max, nOutstanding);
}

void FrontierResponsePendingAdd(int64_t delta)
{
    Counters& c = Get();
    const int64_t nPending = c.frontier_response_pending_current.fetch_add(
                                delta, std::memory_order_relaxed) + delta;
    if (nPending > 0)
        AtomicMax(c.frontier_response_pending_max, nPending);
}

void RecordIBDState(bool fInitialBlockDownload)
{
    Counters& c = Get();
    const int64_t nNew = fInitialBlockDownload ? 1 : 0;
    const int64_t nOld = c.ibd_state_current.exchange(
                            nNew, std::memory_order_relaxed);
    if (nOld >= 0 && nOld != nNew)
        c.ibd_state_transitions.fetch_add(1, std::memory_order_relaxed);
}

void RecordZeroLatency(ZeroLatencyEvent event)
{
    Counters& c = Get();
    const int64_t nStart = c.zero_start_time_ms.load(std::memory_order_relaxed);
    if (nStart <= 0)
        return;
    const int64_t nBit = 1LL << (int)event;
    int64_t nMask = c.zero_latency_recorded_mask.load(std::memory_order_relaxed);
    while ((nMask & nBit) == 0)
    {
        if (c.zero_latency_recorded_mask.compare_exchange_weak(
                nMask, nMask | nBit, std::memory_order_relaxed))
        {
            const int64_t nDuration =
                std::max<int64_t>(0, GetTimeMillis() - nStart);
            switch (event)
            {
            case ZERO_LATENCY_GETBLOCKS_WIRE_SEND:
                c.zero_to_first_getblocks_wire_send_ms_total.fetch_add(nDuration, std::memory_order_relaxed);
                AtomicMax(c.zero_to_first_getblocks_wire_send_ms_max, nDuration);
                break;
            case ZERO_LATENCY_INV:
                c.zero_to_first_inv_ms_total.fetch_add(nDuration, std::memory_order_relaxed);
                AtomicMax(c.zero_to_first_inv_ms_max, nDuration);
                break;
            case ZERO_LATENCY_UNKNOWN_INV:
                c.zero_to_first_unknown_inv_ms_total.fetch_add(nDuration, std::memory_order_relaxed);
                AtomicMax(c.zero_to_first_unknown_inv_ms_max, nDuration);
                break;
            case ZERO_LATENCY_ASKFOR:
                c.zero_to_first_askfor_ms_total.fetch_add(nDuration, std::memory_order_relaxed);
                AtomicMax(c.zero_to_first_askfor_ms_max, nDuration);
                break;
            case ZERO_LATENCY_ACTIVE_NONZERO:
                c.zero_to_active_nonzero_ms_total.fetch_add(nDuration, std::memory_order_relaxed);
                AtomicMax(c.zero_to_active_nonzero_ms_max, nDuration);
                break;
            }
            return;
        }
    }
}

void PeerZeroStateChange(int nOldZero, int nNewZero)
{
    if (nOldZero == nNewZero)
        return;
    Counters& c = Get();
    if (nNewZero)
    {
        const int64_t nNew = c.peers_at_zero_budget_current.fetch_add(
                                 1, std::memory_order_relaxed) +
                             1;
        AtomicMax(c.peers_at_zero_budget_max, nNew);
    }
    else
    {
        c.peers_at_zero_budget_current.fetch_sub(1,
                                                 std::memory_order_relaxed);
    }
}

void SnapshotAll(IBDMetricsSnapshot& out)
{
    const int64_t nNow = GetTime();
    Counters& c = Get();
    out.start_time_unix = g_nStartTime;
    out.now_unix = nNow;
    out.uptime_seconds = std::max<int64_t>(0, nNow - g_nStartTime);

    out.deferred_budget_calls =
        c.deferred_budget_calls.load(std::memory_order_relaxed);
    out.deferred_budget_positive =
        c.deferred_budget_positive.load(std::memory_order_relaxed);
    out.deferred_budget_zero =
        c.deferred_budget_zero.load(std::memory_order_relaxed);
    out.deferred_budget_zero_peer_pressure =
        c.deferred_budget_zero_peer_pressure.load(std::memory_order_relaxed);
    out.deferred_budget_zero_global_pressure =
        c.deferred_budget_zero_global_pressure.load(std::memory_order_relaxed);
    out.deferred_budget_zero_vnodes_lock_failed =
        c.deferred_budget_zero_vnodes_lock_failed.load(
            std::memory_order_relaxed);

    out.block_inv_unknown_total =
        c.block_inv_unknown_total.load(std::memory_order_relaxed);
    out.block_inv_admitted =
        c.block_inv_admitted.load(std::memory_order_relaxed);
    out.block_inv_deferred =
        c.block_inv_deferred.load(std::memory_order_relaxed);
    out.block_inv_deferred_no_budget =
        c.block_inv_deferred_no_budget.load(std::memory_order_relaxed);
    out.block_inv_deferred_overflow =
        c.block_inv_deferred_overflow.load(std::memory_order_relaxed);
    out.frontier_exemption_admitted =
        c.frontier_exemption_admitted.load(std::memory_order_relaxed);

    out.global_active_current =
        c.global_active_current.load(std::memory_order_relaxed);
    out.global_active_max =
        c.global_active_max.load(std::memory_order_relaxed);
    out.peer_pressure_max =
        c.peer_pressure_max.load(std::memory_order_relaxed);
    out.orphan_pressure_max =
        c.orphan_pressure_max.load(std::memory_order_relaxed);
    out.peers_at_zero_budget_current =
        c.peers_at_zero_budget_current.load(std::memory_order_relaxed);
    out.peers_at_zero_budget_max =
        c.peers_at_zero_budget_max.load(std::memory_order_relaxed);

    out.refill_calls = c.refill_calls.load(std::memory_order_relaxed);
    out.refill_calls_zero_budget =
        c.refill_calls_zero_budget.load(std::memory_order_relaxed);
    out.refill_items_examined =
        c.refill_items_examined.load(std::memory_order_relaxed);
    out.refill_items_admitted =
        c.refill_items_admitted.load(std::memory_order_relaxed);
    out.refill_items_already_have =
        c.refill_items_already_have.load(std::memory_order_relaxed);
    out.refill_items_active_owner =
        c.refill_items_active_owner.load(std::memory_order_relaxed);
    out.refill_work_limit_hit =
        c.refill_work_limit_hit.load(std::memory_order_relaxed);
    out.refill_txdb_opens =
        c.refill_txdb_opens.load(std::memory_order_relaxed);
    out.refill_alreadyhave_checks =
        c.refill_alreadyhave_checks.load(std::memory_order_relaxed);

    out.block_receive_total =
        c.block_receive_total.load(std::memory_order_relaxed);
    out.block_result_accepted_active =
        c.block_result_accepted_active.load(std::memory_order_relaxed);
    out.block_result_orphan_new =
        c.block_result_orphan_new.load(std::memory_order_relaxed);
    out.setbestchain_commits =
        c.setbestchain_commits.load(std::memory_order_relaxed);
    out.stalled_recovery_attempts =
        c.stalled_recovery_attempts.load(std::memory_order_relaxed);

    out.active_decrement_receive_clear_inflight =
        c.active_decrement_receive_clear_inflight.load(std::memory_order_relaxed);
    out.active_decrement_inflight_timeout =
        c.active_decrement_inflight_timeout.load(std::memory_order_relaxed);
    out.active_decrement_askfor_sent_transition =
        c.active_decrement_askfor_sent_transition.load(std::memory_order_relaxed);
    out.active_decrement_askfor_removed_already_have =
        c.active_decrement_askfor_removed_already_have.load(std::memory_order_relaxed);
    out.active_decrement_askfor_removed_owner_conflict =
        c.active_decrement_askfor_removed_owner_conflict.load(std::memory_order_relaxed);
    out.active_decrement_clear_askfor =
        c.active_decrement_clear_askfor.load(std::memory_order_relaxed);
    out.active_decrement_disconnect_cleanup =
        c.active_decrement_disconnect_cleanup.load(std::memory_order_relaxed);
    out.active_decrement_other =
        c.active_decrement_other.load(std::memory_order_relaxed);

    out.global_active_zero_transitions =
        c.global_active_zero_transitions.load(std::memory_order_relaxed);
    out.zero_with_total_deferred_nonempty =
        c.zero_with_total_deferred_nonempty.load(std::memory_order_relaxed);
    out.zero_with_total_deferred_empty =
        c.zero_with_total_deferred_empty.load(std::memory_order_relaxed);
    out.zero_with_eligible_ahead_peer =
        c.zero_with_eligible_ahead_peer.load(std::memory_order_relaxed);
    out.zero_without_eligible_ahead_peer =
        c.zero_without_eligible_ahead_peer.load(std::memory_order_relaxed);
    out.zero_duration_total_ms =
        c.zero_duration_total_ms.load(std::memory_order_relaxed);
    out.zero_duration_max_ms =
        c.zero_duration_max_ms.load(std::memory_order_relaxed);
    const int64_t nZeroStart =
        c.zero_start_time_ms.load(std::memory_order_relaxed);
    out.zero_duration_current_ms = nZeroStart > 0
        ? std::max<int64_t>(0, GetTimeMillis() - nZeroStart) : 0;

    out.refill_opportunity_slot_freed =
        c.refill_opportunity_slot_freed.load(std::memory_order_relaxed);
    out.refill_sendmessages_passes =
        c.refill_sendmessages_passes.load(std::memory_order_relaxed);
    out.refill_skipped_cs_main_trylock_failed =
        c.refill_skipped_cs_main_trylock_failed.load(std::memory_order_relaxed);
    out.refill_called_deferred_empty =
        c.refill_called_deferred_empty.load(std::memory_order_relaxed);
    out.refill_called_positive_budget_nonempty =
        c.refill_called_positive_budget_nonempty.load(std::memory_order_relaxed);
    out.refill_positive_budget_admitted_zero =
        c.refill_positive_budget_admitted_zero.load(std::memory_order_relaxed);
    out.refill_positive_budget_admitted_count =
        c.refill_positive_budget_admitted_count.load(std::memory_order_relaxed);

    out.pipeline_drained_checks =
        c.pipeline_drained_checks.load(std::memory_order_relaxed);
    out.pipeline_drained_getblocks_queued =
        c.pipeline_drained_getblocks_queued.load(std::memory_order_relaxed);
    out.pipeline_drained_skip_not_ahead =
        c.pipeline_drained_skip_not_ahead.load(std::memory_order_relaxed);
    out.pipeline_drained_skip_getblocks_10s_cooldown =
        c.pipeline_drained_skip_getblocks_10s_cooldown.load(std::memory_order_relaxed);
    out.pipeline_drained_skip_other_condition =
        c.pipeline_drained_skip_other_condition.load(std::memory_order_relaxed);
    out.pushgetblocks_dedup_5s_skips =
        c.pushgetblocks_dedup_5s_skips.load(std::memory_order_relaxed);

    out.askfor_skip_orphan_limit_cooldown =
        c.askfor_skip_orphan_limit_cooldown.load(std::memory_order_relaxed);
    out.orphan_limit_cooldown_recorded =
        c.orphan_limit_cooldown_recorded.load(std::memory_order_relaxed);
    out.orphan_limit_cross_peer_admitted =
        c.orphan_limit_cross_peer_admitted.load(std::memory_order_relaxed);
    out.orphan_limit_frontier_retry_queued =
        c.orphan_limit_frontier_retry_queued.load(std::memory_order_relaxed);
    out.orphan_limit_frontier_retry_pending =
        c.orphan_limit_frontier_retry_pending.load(std::memory_order_relaxed);
    out.askfor_skip_mapalreadyasked_cap =
        c.askfor_skip_mapalreadyasked_cap.load(std::memory_order_relaxed);
    out.askfor_skip_other_peer_owner =
        c.askfor_skip_other_peer_owner.load(std::memory_order_relaxed);
    out.askfor_skip_already_queued =
        c.askfor_skip_already_queued.load(std::memory_order_relaxed);
    out.askfor_skip_inflight =
        c.askfor_skip_inflight.load(std::memory_order_relaxed);

    out.total_deferred_current =
        c.total_deferred_current.load(std::memory_order_relaxed);
    out.total_deferred_max =
        c.total_deferred_max.load(std::memory_order_relaxed);
    out.total_queued_current =
        c.total_queued_current.load(std::memory_order_relaxed);
    out.total_queued_max =
        c.total_queued_max.load(std::memory_order_relaxed);
    out.total_inflight_current =
        c.total_inflight_current.load(std::memory_order_relaxed);
    out.total_inflight_max =
        c.total_inflight_max.load(std::memory_order_relaxed);
    out.eligible_ahead_peers_current =
        c.eligible_ahead_peers_current.load(std::memory_order_relaxed);
    out.eligible_ahead_peers_max =
        c.eligible_ahead_peers_max.load(std::memory_order_relaxed);

    out.getblocks_decision_attempts_other =
        c.getblocks_decision_attempts_other.load(std::memory_order_relaxed);
    out.getblocks_decision_attempts_initial =
        c.getblocks_decision_attempts_initial.load(std::memory_order_relaxed);
    out.getblocks_decision_attempts_continuation =
        c.getblocks_decision_attempts_continuation.load(std::memory_order_relaxed);
    out.getblocks_decision_attempts_recovery =
        c.getblocks_decision_attempts_recovery.load(std::memory_order_relaxed);
    out.getblocks_decision_attempts_prefetch =
        c.getblocks_decision_attempts_prefetch.load(std::memory_order_relaxed);
    out.getblocks_decision_attempts_inv_continuation =
        c.getblocks_decision_attempts_inv_continuation.load(std::memory_order_relaxed);
    out.getblocks_decision_attempts_version =
        c.getblocks_decision_attempts_version.load(std::memory_order_relaxed);
    out.getblocks_decision_attempts_headers =
        c.getblocks_decision_attempts_headers.load(std::memory_order_relaxed);
    out.getblocks_decision_attempts_checkpoint =
        c.getblocks_decision_attempts_checkpoint.load(std::memory_order_relaxed);
    out.getblocks_decision_attempts_wallet_rescan =
        c.getblocks_decision_attempts_wallet_rescan.load(std::memory_order_relaxed);
    out.getblocks_decision_attempts_orphan_limit =
        c.getblocks_decision_attempts_orphan_limit.load(std::memory_order_relaxed);
    out.getblocks_decision_attempts_empty_pipeline_wake =
        c.getblocks_decision_attempts_empty_pipeline_wake.load(std::memory_order_relaxed);
    out.getblocks_queue_success_other =
        c.getblocks_queue_success_other.load(std::memory_order_relaxed);
    out.getblocks_queue_success_initial =
        c.getblocks_queue_success_initial.load(std::memory_order_relaxed);
    out.getblocks_queue_success_continuation =
        c.getblocks_queue_success_continuation.load(std::memory_order_relaxed);
    out.getblocks_queue_success_recovery =
        c.getblocks_queue_success_recovery.load(std::memory_order_relaxed);
    out.getblocks_queue_success_prefetch =
        c.getblocks_queue_success_prefetch.load(std::memory_order_relaxed);
    out.getblocks_queue_success_inv_continuation =
        c.getblocks_queue_success_inv_continuation.load(std::memory_order_relaxed);
    out.getblocks_queue_success_version =
        c.getblocks_queue_success_version.load(std::memory_order_relaxed);
    out.getblocks_queue_success_headers =
        c.getblocks_queue_success_headers.load(std::memory_order_relaxed);
    out.getblocks_queue_success_checkpoint =
        c.getblocks_queue_success_checkpoint.load(std::memory_order_relaxed);
    out.getblocks_queue_success_wallet_rescan =
        c.getblocks_queue_success_wallet_rescan.load(std::memory_order_relaxed);
    out.getblocks_queue_success_orphan_limit =
        c.getblocks_queue_success_orphan_limit.load(std::memory_order_relaxed);
    out.getblocks_queue_success_empty_pipeline_wake =
        c.getblocks_queue_success_empty_pipeline_wake.load(std::memory_order_relaxed);
    out.getblocks_wire_sent_other =
        c.getblocks_wire_sent_other.load(std::memory_order_relaxed);
    out.getblocks_wire_sent_initial =
        c.getblocks_wire_sent_initial.load(std::memory_order_relaxed);
    out.getblocks_wire_sent_continuation =
        c.getblocks_wire_sent_continuation.load(std::memory_order_relaxed);
    out.getblocks_wire_sent_recovery =
        c.getblocks_wire_sent_recovery.load(std::memory_order_relaxed);
    out.getblocks_wire_sent_prefetch =
        c.getblocks_wire_sent_prefetch.load(std::memory_order_relaxed);
    out.getblocks_wire_sent_inv_continuation =
        c.getblocks_wire_sent_inv_continuation.load(std::memory_order_relaxed);
    out.getblocks_wire_sent_version =
        c.getblocks_wire_sent_version.load(std::memory_order_relaxed);
    out.getblocks_wire_sent_headers =
        c.getblocks_wire_sent_headers.load(std::memory_order_relaxed);
    out.getblocks_wire_sent_checkpoint =
        c.getblocks_wire_sent_checkpoint.load(std::memory_order_relaxed);
    out.getblocks_wire_sent_wallet_rescan =
        c.getblocks_wire_sent_wallet_rescan.load(std::memory_order_relaxed);
    out.getblocks_wire_sent_orphan_limit =
        c.getblocks_wire_sent_orphan_limit.load(std::memory_order_relaxed);
    out.getblocks_wire_sent_empty_pipeline_wake =
        c.getblocks_wire_sent_empty_pipeline_wake.load(std::memory_order_relaxed);
    out.getblocks_dedup_skips =
        c.getblocks_dedup_skips.load(std::memory_order_relaxed);
    out.getblocks_identical_to_last_sent =
        c.getblocks_identical_to_last_sent.load(std::memory_order_relaxed);
    out.getblocks_response_inv_messages =
        c.getblocks_response_inv_messages.load(std::memory_order_relaxed);
    out.getblocks_response_block_inv_count =
        c.getblocks_response_block_inv_count.load(std::memory_order_relaxed);
    out.getblocks_response_unknown_count =
        c.getblocks_response_unknown_count.load(std::memory_order_relaxed);
    out.getblocks_response_zero_unknown =
        c.getblocks_response_zero_unknown.load(std::memory_order_relaxed);
    out.getblocks_response_inv_zero_unknown =
        c.getblocks_response_inv_zero_unknown.load(std::memory_order_relaxed);
    out.recovery_outcome_useful =
        c.recovery_outcome_useful.load(std::memory_order_relaxed);
    out.recovery_outcome_known_only =
        c.recovery_outcome_known_only.load(std::memory_order_relaxed);
    out.recovery_outcome_no_response =
        c.recovery_outcome_no_response.load(std::memory_order_relaxed);
    out.zero_to_first_getblocks_wire_send_ms_total =
        c.zero_to_first_getblocks_wire_send_ms_total.load(std::memory_order_relaxed);
    out.zero_to_first_getblocks_wire_send_ms_max =
        c.zero_to_first_getblocks_wire_send_ms_max.load(std::memory_order_relaxed);
    out.zero_to_first_inv_ms_total =
        c.zero_to_first_inv_ms_total.load(std::memory_order_relaxed);
    out.zero_to_first_inv_ms_max =
        c.zero_to_first_inv_ms_max.load(std::memory_order_relaxed);
    out.zero_to_first_unknown_inv_ms_total =
        c.zero_to_first_unknown_inv_ms_total.load(std::memory_order_relaxed);
    out.zero_to_first_unknown_inv_ms_max =
        c.zero_to_first_unknown_inv_ms_max.load(std::memory_order_relaxed);
    out.zero_to_first_askfor_ms_total =
        c.zero_to_first_askfor_ms_total.load(std::memory_order_relaxed);
    out.zero_to_first_askfor_ms_max =
        c.zero_to_first_askfor_ms_max.load(std::memory_order_relaxed);
    out.zero_to_active_nonzero_ms_total =
        c.zero_to_active_nonzero_ms_total.load(std::memory_order_relaxed);
    out.zero_to_active_nonzero_ms_max =
        c.zero_to_active_nonzero_ms_max.load(std::memory_order_relaxed);
    out.inv_unknown_during_zero_global =
        c.inv_unknown_during_zero_global.load(std::memory_order_relaxed);
    out.sync_peer_change_while_pipeline_empty =
        c.sync_peer_change_while_pipeline_empty.load(std::memory_order_relaxed);
    out.getblocks_outstanding_current =
        c.getblocks_outstanding_current.load(std::memory_order_relaxed);
    out.getblocks_outstanding_max =
        c.getblocks_outstanding_max.load(std::memory_order_relaxed);
    out.getblocks_no_response_disconnect_cleanup =
        c.getblocks_no_response_disconnect_cleanup.load(std::memory_order_relaxed);
    out.getblocks_queued_unsent_cleanup =
        c.getblocks_queued_unsent_cleanup.load(std::memory_order_relaxed);
    out.peers_with_queued_getblocks_current =
        c.peers_with_queued_getblocks_current.load(std::memory_order_relaxed);
    out.peers_with_queued_getblocks_max =
        c.peers_with_queued_getblocks_max.load(std::memory_order_relaxed);
    out.total_getblocks_queued_requests_current =
        c.total_getblocks_queued_requests_current.load(std::memory_order_relaxed);
    out.total_getblocks_queued_requests_max =
        c.total_getblocks_queued_requests_max.load(std::memory_order_relaxed);
    out.pipeline_active_due_to_getblocks_current =
        c.pipeline_active_due_to_getblocks_current.load(std::memory_order_relaxed);
    out.pipeline_active_due_to_getblocks_max =
        c.pipeline_active_due_to_getblocks_max.load(std::memory_order_relaxed);
    out.frontier_response_armed =
        c.frontier_response_armed.load(std::memory_order_relaxed);
    out.frontier_response_consumed =
        c.frontier_response_consumed.load(std::memory_order_relaxed);
    out.frontier_response_pending_current =
        c.frontier_response_pending_current.load(std::memory_order_relaxed);
    out.frontier_response_pending_max =
        c.frontier_response_pending_max.load(std::memory_order_relaxed);
    out.frontier_reject_locator_stale =
        c.frontier_reject_locator_stale.load(std::memory_order_relaxed);
    out.frontier_reject_slot_busy =
        c.frontier_reject_slot_busy.load(std::memory_order_relaxed);
    out.frontier_reject_already_admitted =
        c.frontier_reject_already_admitted.load(std::memory_order_relaxed);
    out.frontier_reject_other =
        c.frontier_reject_other.load(std::memory_order_relaxed);
    out.ibd_state_current =
        c.ibd_state_current.load(std::memory_order_relaxed);
    out.ibd_state_transitions =
        c.ibd_state_transitions.load(std::memory_order_relaxed);

    out.pipeline_wake_signals =
        c.pipeline_wake_signals.load(std::memory_order_relaxed);
    out.pipeline_wake_signal_clear_inflight =
        c.pipeline_wake_signal_clear_inflight.load(std::memory_order_relaxed);
    out.pipeline_wake_signal_inflight_timeout =
        c.pipeline_wake_signal_inflight_timeout.load(std::memory_order_relaxed);
    out.pipeline_wake_signal_askfor_already_have =
        c.pipeline_wake_signal_askfor_already_have.load(std::memory_order_relaxed);
    out.pipeline_wake_signal_askfor_owner_conflict =
        c.pipeline_wake_signal_askfor_owner_conflict.load(std::memory_order_relaxed);
    out.pipeline_wake_signal_queue_removal =
        c.pipeline_wake_signal_queue_removal.load(std::memory_order_relaxed);
    out.pipeline_wake_signal_clear_askfor =
        c.pipeline_wake_signal_clear_askfor.load(std::memory_order_relaxed);
    out.pipeline_wake_signal_disconnect_cleanup =
        c.pipeline_wake_signal_disconnect_cleanup.load(std::memory_order_relaxed);
    out.pipeline_wake_signal_getblocks_outstanding_cleared =
        c.pipeline_wake_signal_getblocks_outstanding_cleared.load(std::memory_order_relaxed);
    out.pipeline_wake_signal_other =
        c.pipeline_wake_signal_other.load(std::memory_order_relaxed);
    out.pipeline_wake_coalesced =
        c.pipeline_wake_coalesced.load(std::memory_order_relaxed);
    out.pipeline_wake_handler_runs =
        c.pipeline_wake_handler_runs.load(std::memory_order_relaxed);
    out.pipeline_wake_transient_cs_main_trylock_failed =
        c.pipeline_wake_transient_cs_main_trylock_failed.load(std::memory_order_relaxed);
    out.pipeline_wake_transient_cs_vnodes_trylock_failed =
        c.pipeline_wake_transient_cs_vnodes_trylock_failed.load(std::memory_order_relaxed);
    out.pipeline_wake_transient_cooldown_active =
        c.pipeline_wake_transient_cooldown_active.load(std::memory_order_relaxed);
    out.pipeline_wake_transient_dedup_all =
        c.pipeline_wake_transient_dedup_all.load(std::memory_order_relaxed);
    out.pipeline_wake_transient_incomplete_peer_scan =
        c.pipeline_wake_transient_incomplete_peer_scan.load(std::memory_order_relaxed);
    out.pipeline_wake_transient_shutdown =
        c.pipeline_wake_transient_shutdown.load(std::memory_order_relaxed);
    out.pipeline_wake_terminal_not_ibd =
        c.pipeline_wake_terminal_not_ibd.load(std::memory_order_relaxed);
    out.pipeline_wake_terminal_pipeline_not_empty =
        c.pipeline_wake_terminal_pipeline_not_empty.load(std::memory_order_relaxed);
    out.pipeline_wake_terminal_deferred_refill_created_work =
        c.pipeline_wake_terminal_deferred_refill_created_work.load(std::memory_order_relaxed);
    out.pipeline_wake_terminal_getblocks_queued =
        c.pipeline_wake_terminal_getblocks_queued.load(std::memory_order_relaxed);
    out.pipeline_wake_terminal_no_eligible_ahead_peer =
        c.pipeline_wake_terminal_no_eligible_ahead_peer.load(std::memory_order_relaxed);
    out.pipeline_wake_terminal_existing_queued_getblocks =
        c.pipeline_wake_terminal_existing_queued_getblocks.load(std::memory_order_relaxed);
    out.pipeline_wake_terminal_outstanding_getblocks_present =
        c.pipeline_wake_terminal_outstanding_getblocks_present.load(std::memory_order_relaxed);
    out.pipeline_wake_refill_attempts =
        c.pipeline_wake_refill_attempts.load(std::memory_order_relaxed);
    out.pipeline_wake_refill_admitted =
        c.pipeline_wake_refill_admitted.load(std::memory_order_relaxed);
    out.pipeline_wake_getblocks_attempted =
        c.pipeline_wake_getblocks_attempted.load(std::memory_order_relaxed);
    out.pipeline_wake_getblocks_queued =
        c.pipeline_wake_getblocks_queued.load(std::memory_order_relaxed);
    out.pipeline_wake_getblocks_dedup =
        c.pipeline_wake_getblocks_dedup.load(std::memory_order_relaxed);
    out.pipeline_wake_active_restored =
        c.pipeline_wake_active_restored.load(std::memory_order_relaxed);
    out.pipeline_wake_signal_to_active_ms_total =
        c.pipeline_wake_signal_to_active_ms_total.load(std::memory_order_relaxed);
    out.pipeline_wake_signal_to_active_ms_max =
        c.pipeline_wake_signal_to_active_ms_max.load(std::memory_order_relaxed);

    out.diversify_candidates =
        c.diversify_candidates.load(std::memory_order_relaxed);
    out.diversify_picked_other_lane =
        c.diversify_picked_other_lane.load(std::memory_order_relaxed);
    out.diversify_picked_announcer =
        c.diversify_picked_announcer.load(std::memory_order_relaxed);
    out.diversify_snapshot_skip_lock =
        c.diversify_snapshot_skip_lock.load(std::memory_order_relaxed);
    out.diversify_no_other_lane =
        c.diversify_no_other_lane.load(std::memory_order_relaxed);
    out.diversify_other_lane_timeout =
        c.diversify_other_lane_timeout.load(std::memory_order_relaxed);
}

void ResetPipelineWakeMetricsForTesting()
{
    Counters& c = Get();
    std::atomic<int64_t>* apszCounters[] = {
        &c.global_active_current,
        &c.total_queued_current,
        &c.total_inflight_current,
        &c.total_deferred_current,
        &c.getblocks_outstanding_current,
        &c.total_getblocks_queued_requests_current,
        &c.peers_with_queued_getblocks_current,
        &c.pipeline_active_due_to_getblocks_current,
        &c.pipeline_wake_signals,
        &c.pipeline_wake_signal_clear_inflight,
        &c.pipeline_wake_signal_inflight_timeout,
        &c.pipeline_wake_signal_askfor_already_have,
        &c.pipeline_wake_signal_askfor_owner_conflict,
        &c.pipeline_wake_signal_queue_removal,
        &c.pipeline_wake_signal_clear_askfor,
        &c.pipeline_wake_signal_disconnect_cleanup,
        &c.pipeline_wake_signal_getblocks_outstanding_cleared,
        &c.pipeline_wake_signal_other,
        &c.pipeline_wake_coalesced,
        &c.pipeline_wake_handler_runs,
        &c.pipeline_wake_transient_cs_main_trylock_failed,
        &c.pipeline_wake_transient_cs_vnodes_trylock_failed,
        &c.pipeline_wake_transient_cooldown_active,
        &c.pipeline_wake_transient_dedup_all,
        &c.pipeline_wake_transient_incomplete_peer_scan,
        &c.pipeline_wake_transient_shutdown,
        &c.pipeline_wake_terminal_not_ibd,
        &c.pipeline_wake_terminal_pipeline_not_empty,
        &c.pipeline_wake_terminal_deferred_refill_created_work,
        &c.pipeline_wake_terminal_getblocks_queued,
        &c.pipeline_wake_terminal_no_eligible_ahead_peer,
        &c.pipeline_wake_terminal_existing_queued_getblocks,
        &c.pipeline_wake_terminal_outstanding_getblocks_present,
        &c.pipeline_wake_refill_attempts,
        &c.pipeline_wake_refill_admitted,
        &c.pipeline_wake_getblocks_attempted,
        &c.pipeline_wake_getblocks_queued,
        &c.pipeline_wake_getblocks_dedup,
        &c.pipeline_wake_active_restored,
        &c.pipeline_wake_signal_to_active_ms_total,
        &c.pipeline_wake_signal_to_active_ms_max,
        &c.pipeline_wake_signal_start_ms
    };
    for (size_t i = 0; i < sizeof(apszCounters) / sizeof(apszCounters[0]); ++i)
        apszCounters[i]->store(0, std::memory_order_relaxed);
}

} // namespace ibdmetrics
