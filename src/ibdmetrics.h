// Copyright (c) 2026 The Innova developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef INNOVA_IBDMETRICS_H
#define INNOVA_IBDMETRICS_H

#include <stdint.h>

#include <atomic>

// IBD deferred block-request scheduler diagnostics.
//
// Pure aggregate counters: relaxed atomics only, no new hot lock, no
// per-event strings or printf, no admission/recovery behavior change.
// Counters are reset only at process start.  They are surfaced through the
// getinfo RPC ("ibdmetrics" object) via IBDMetricsSnapshotAll.
//
// Cause counters in the deferred-budget group may overlap: a single zero
// budget call can be caused by per-peer pressure and global pressure at the
// same time, and a cs_vNodes lock failure always also reports as global
// pressure (fail-closed value MAX_DEFERRED_INV_ACTIVE_GLOBAL + 1).

struct IBDMetricsSnapshot
{
    int64_t start_time_unix;
    int64_t now_unix;
    int64_t uptime_seconds;

    // 1. Deferred-budget calls (full pressure path only, i.e. IBD + peer)
    int64_t deferred_budget_calls;
    int64_t deferred_budget_positive;
    int64_t deferred_budget_zero;
    int64_t deferred_budget_zero_peer_pressure;
    int64_t deferred_budget_zero_global_pressure;
    int64_t deferred_budget_zero_vnodes_lock_failed;

    // 2. INV admission
    int64_t block_inv_unknown_total;
    int64_t block_inv_admitted;
    int64_t block_inv_deferred;
    int64_t block_inv_deferred_no_budget;
    int64_t block_inv_deferred_overflow;
    int64_t frontier_exemption_admitted;

    // 3. Global/peer pressure high-water marks
    int64_t global_active_current;
    int64_t global_active_max;
    int64_t peer_pressure_max;
    int64_t orphan_pressure_max;
    int64_t peers_at_zero_budget_current;
    int64_t peers_at_zero_budget_max;

    // 4. Deferred refill
    int64_t refill_calls;
    int64_t refill_calls_zero_budget;
    int64_t refill_items_examined;
    int64_t refill_items_admitted;
    int64_t refill_items_already_have;
    int64_t refill_items_active_owner;
    int64_t refill_work_limit_hit;
    int64_t refill_txdb_opens;
    int64_t refill_alreadyhave_checks;

    // 5. Pipeline usefulness
    int64_t block_receive_total;
    int64_t block_result_accepted_active;
    int64_t block_result_orphan_new;
    int64_t block_result_orphan_limit_rejected;
    int64_t block_result_rejected_total;
    int64_t setbestchain_commits;
    int64_t stalled_recovery_attempts;

    int64_t active_decrement_receive_clear_inflight;
    int64_t active_decrement_inflight_timeout;
    int64_t active_decrement_askfor_sent_transition;
    int64_t active_decrement_askfor_removed_already_have;
    int64_t active_decrement_askfor_removed_owner_conflict;
    int64_t active_decrement_clear_askfor;
    int64_t active_decrement_disconnect_cleanup;
    int64_t active_decrement_other;

    int64_t global_active_zero_transitions;
    int64_t zero_with_total_deferred_nonempty;
    int64_t zero_with_total_deferred_empty;
    int64_t zero_with_eligible_ahead_peer;
    int64_t zero_without_eligible_ahead_peer;
    int64_t zero_duration_total_ms;
    int64_t zero_duration_max_ms;
    int64_t zero_duration_current_ms;

    int64_t refill_opportunity_slot_freed;
    int64_t refill_sendmessages_passes;
    int64_t refill_skipped_cs_main_trylock_failed;
    int64_t refill_called_deferred_empty;
    int64_t refill_called_positive_budget_nonempty;
    int64_t refill_positive_budget_admitted_zero;
    int64_t refill_positive_budget_admitted_count;

    int64_t pipeline_drained_checks;
    int64_t pipeline_drained_getblocks_queued;
    int64_t pipeline_drained_skip_not_ahead;
    int64_t pipeline_drained_skip_getblocks_10s_cooldown;
    int64_t pipeline_drained_skip_other_condition;
    // Same event as getblocks_dedup_skips; retained for continuity with the
    // earlier PushGetBlocks-specific counter name.
    int64_t pushgetblocks_dedup_5s_skips;

    int64_t askfor_skip_orphan_limit_cooldown;
    int64_t orphan_limit_cooldown_recorded;
    int64_t orphan_limit_cross_peer_admitted;
    int64_t orphan_limit_frontier_retry_queued;
    int64_t orphan_limit_frontier_retry_pending;
    int64_t askfor_skip_mapalreadyasked_cap;
    int64_t askfor_skip_other_peer_owner;
    int64_t askfor_skip_already_queued;
    int64_t askfor_skip_inflight;

    int64_t total_deferred_current;
    int64_t total_deferred_max;
    int64_t total_queued_current;
    int64_t total_queued_max;
    int64_t total_inflight_current;
    int64_t total_inflight_max;
    int64_t eligible_ahead_peers_current;
    int64_t eligible_ahead_peers_max;

    int64_t getblocks_decision_attempts_other;
    int64_t getblocks_decision_attempts_initial;
    int64_t getblocks_decision_attempts_continuation;
    int64_t getblocks_decision_attempts_recovery;
    int64_t getblocks_decision_attempts_prefetch;
    int64_t getblocks_decision_attempts_inv_continuation;
    int64_t getblocks_decision_attempts_version;
    int64_t getblocks_decision_attempts_headers;
    int64_t getblocks_decision_attempts_checkpoint;
    int64_t getblocks_decision_attempts_wallet_rescan;
    int64_t getblocks_decision_attempts_orphan_limit;
    int64_t getblocks_decision_attempts_empty_pipeline_wake;
    int64_t getblocks_queue_success_other;
    int64_t getblocks_queue_success_initial;
    int64_t getblocks_queue_success_continuation;
    int64_t getblocks_queue_success_recovery;
    int64_t getblocks_queue_success_prefetch;
    int64_t getblocks_queue_success_inv_continuation;
    int64_t getblocks_queue_success_version;
    int64_t getblocks_queue_success_headers;
    int64_t getblocks_queue_success_checkpoint;
    int64_t getblocks_queue_success_wallet_rescan;
    int64_t getblocks_queue_success_orphan_limit;
    int64_t getblocks_queue_success_empty_pipeline_wake;
    int64_t getblocks_wire_sent_other;
    int64_t getblocks_wire_sent_initial;
    int64_t getblocks_wire_sent_continuation;
    int64_t getblocks_wire_sent_recovery;
    int64_t getblocks_wire_sent_prefetch;
    int64_t getblocks_wire_sent_inv_continuation;
    int64_t getblocks_wire_sent_version;
    int64_t getblocks_wire_sent_headers;
    int64_t getblocks_wire_sent_checkpoint;
    int64_t getblocks_wire_sent_wallet_rescan;
    int64_t getblocks_wire_sent_orphan_limit;
    int64_t getblocks_wire_sent_empty_pipeline_wake;
    int64_t getblocks_dedup_skips;
    int64_t getblocks_identical_to_last_sent;
    int64_t getblocks_response_inv_messages;
    int64_t getblocks_response_block_inv_count;
    int64_t getblocks_response_unknown_count;
    // FIFO/time-order attributed INV observed with zero unknown block hashes.
    // getblocks_response_zero_unknown is the legacy name; the _inv_ spelling
    // is the precise interpretation exposed for new dashboards.
    int64_t getblocks_response_zero_unknown;
    int64_t getblocks_response_inv_zero_unknown;
    int64_t recovery_outcome_useful;
    int64_t recovery_outcome_known_only;
    int64_t recovery_outcome_no_response;
    int64_t zero_to_first_getblocks_wire_send_ms_total;
    int64_t zero_to_first_getblocks_wire_send_ms_max;
    int64_t zero_to_first_inv_ms_total;
    int64_t zero_to_first_inv_ms_max;
    int64_t zero_to_first_unknown_inv_ms_total;
    int64_t zero_to_first_unknown_inv_ms_max;
    int64_t zero_to_first_askfor_ms_total;
    int64_t zero_to_first_askfor_ms_max;
    int64_t zero_to_active_nonzero_ms_total;
    int64_t zero_to_active_nonzero_ms_max;
    int64_t inv_unknown_during_zero_global;
    int64_t sync_peer_change_while_pipeline_empty;
    int64_t getblocks_outstanding_current;
    int64_t getblocks_outstanding_max;
    int64_t getblocks_outstanding_timeout;
    int64_t getblocks_no_response_disconnect_cleanup;
    // Pending queue coalescing outcomes (PushGetBlocks bound of one pending).
    int64_t getblocks_pending_coalesce;
    int64_t getblocks_pending_replaced;
    int64_t getblocks_pending_drop;
    // Reconciliation: nonzero when the global outstanding gauge diverges from
    // the sum of per-peer active single-flight cycles.
    int64_t outstanding_gauge_mismatch;
    int64_t getblocks_queued_unsent_cleanup;
    int64_t peers_with_queued_getblocks_current;
    int64_t peers_with_queued_getblocks_max;
    int64_t total_getblocks_queued_requests_current;
    int64_t total_getblocks_queued_requests_max;
    int64_t pipeline_active_due_to_getblocks_current;
    int64_t pipeline_active_due_to_getblocks_max;
    int64_t frontier_response_armed;
    int64_t frontier_response_consumed;
    int64_t frontier_response_pending_current;
    int64_t frontier_response_pending_max;
    int64_t frontier_reject_locator_stale;
    int64_t frontier_reject_slot_busy;
    int64_t frontier_reject_already_admitted;
    int64_t frontier_reject_other;
    int64_t ibd_state_current;
    int64_t ibd_state_transitions;

    int64_t pipeline_wake_signals;
    int64_t pipeline_wake_signal_clear_inflight;
    int64_t pipeline_wake_signal_inflight_timeout;
    int64_t pipeline_wake_signal_askfor_already_have;
    int64_t pipeline_wake_signal_askfor_owner_conflict;
    int64_t pipeline_wake_signal_queue_removal;
    int64_t pipeline_wake_signal_clear_askfor;
    int64_t pipeline_wake_signal_disconnect_cleanup;
    int64_t pipeline_wake_signal_getblocks_outstanding_cleared;
    int64_t pipeline_wake_signal_getblocks_outstanding_timeout;
    int64_t pipeline_wake_signal_other;
    int64_t pipeline_wake_coalesced;
    int64_t pipeline_wake_handler_runs;
    int64_t pipeline_wake_transient_cs_main_trylock_failed;
    int64_t pipeline_wake_transient_cs_vnodes_trylock_failed;
    int64_t pipeline_wake_transient_cooldown_active;
    int64_t pipeline_wake_transient_dedup_all;
    int64_t pipeline_wake_transient_incomplete_peer_scan;
    int64_t pipeline_wake_transient_shutdown;
    int64_t pipeline_wake_terminal_not_ibd;
    int64_t pipeline_wake_terminal_pipeline_not_empty;
    int64_t pipeline_wake_terminal_deferred_refill_created_work;
    int64_t pipeline_wake_terminal_getblocks_queued;
    int64_t pipeline_wake_terminal_no_eligible_ahead_peer;
    int64_t pipeline_wake_terminal_existing_queued_getblocks;
    int64_t pipeline_wake_terminal_outstanding_getblocks_present;
    int64_t pipeline_wake_refill_attempts;
    int64_t pipeline_wake_refill_admitted;
    int64_t pipeline_wake_getblocks_attempted;
    int64_t pipeline_wake_getblocks_queued;
    int64_t pipeline_wake_getblocks_dedup;
    int64_t pipeline_wake_active_restored;
    int64_t pipeline_wake_signal_to_active_ms_total;
    int64_t pipeline_wake_signal_to_active_ms_max;

    // Experiment A (future-supply diversification) counters.
    int64_t diversify_candidates;
    int64_t diversify_picked_other_lane;
    int64_t diversify_picked_announcer;
    int64_t diversify_snapshot_skip_lock;
    int64_t diversify_no_other_lane;
    int64_t diversify_other_lane_timeout;
};

namespace ibdmetrics {

enum ActiveDecrementCause
{
    ACTIVE_DECREMENT_RECEIVE_CLEAR_INFLIGHT = 0,
    ACTIVE_DECREMENT_INFLIGHT_TIMEOUT,
    ACTIVE_DECREMENT_ASKFOR_SENT_TRANSITION,
    ACTIVE_DECREMENT_ASKFOR_REMOVED_ALREADY_HAVE,
    ACTIVE_DECREMENT_ASKFOR_REMOVED_OWNER_CONFLICT,
    ACTIVE_DECREMENT_CLEAR_ASKFOR,
    ACTIVE_DECREMENT_DISCONNECT_CLEANUP,
    ACTIVE_DECREMENT_OTHER
};

enum GetBlocksSource
{
    GETBLOCKS_SOURCE_OTHER = 0,
    GETBLOCKS_SOURCE_INITIAL,
    GETBLOCKS_SOURCE_CONTINUATION,
    GETBLOCKS_SOURCE_RECOVERY,
    GETBLOCKS_SOURCE_PREFETCH,
    GETBLOCKS_SOURCE_INV_CONTINUATION,
    GETBLOCKS_SOURCE_VERSION,
    GETBLOCKS_SOURCE_HEADERS,
    GETBLOCKS_SOURCE_CHECKPOINT,
    GETBLOCKS_SOURCE_WALLET_RESCAN,
    GETBLOCKS_SOURCE_ORPHAN_LIMIT,
    GETBLOCKS_SOURCE_EMPTY_PIPELINE_WAKE
};

enum ZeroLatencyEvent
{
    ZERO_LATENCY_GETBLOCKS_WIRE_SEND = 0,
    ZERO_LATENCY_INV,
    ZERO_LATENCY_UNKNOWN_INV,
    ZERO_LATENCY_ASKFOR,
    ZERO_LATENCY_ACTIVE_NONZERO
};

struct Counters
{
    std::atomic<int64_t> deferred_budget_calls;
    std::atomic<int64_t> deferred_budget_positive;
    std::atomic<int64_t> deferred_budget_zero;
    std::atomic<int64_t> deferred_budget_zero_peer_pressure;
    std::atomic<int64_t> deferred_budget_zero_global_pressure;
    std::atomic<int64_t> deferred_budget_zero_vnodes_lock_failed;

    std::atomic<int64_t> block_inv_unknown_total;
    std::atomic<int64_t> block_inv_admitted;
    std::atomic<int64_t> block_inv_deferred;
    std::atomic<int64_t> block_inv_deferred_no_budget;
    std::atomic<int64_t> block_inv_deferred_overflow;
    std::atomic<int64_t> frontier_exemption_admitted;

    std::atomic<int64_t> global_active_current;
    std::atomic<int64_t> global_active_max;
    std::atomic<int64_t> peer_pressure_max;
    std::atomic<int64_t> orphan_pressure_max;
    std::atomic<int64_t> peers_at_zero_budget_current;
    std::atomic<int64_t> peers_at_zero_budget_max;

    std::atomic<int64_t> refill_calls;
    std::atomic<int64_t> refill_calls_zero_budget;
    std::atomic<int64_t> refill_items_examined;
    std::atomic<int64_t> refill_items_admitted;
    std::atomic<int64_t> refill_items_already_have;
    std::atomic<int64_t> refill_items_active_owner;
    std::atomic<int64_t> refill_work_limit_hit;
    std::atomic<int64_t> refill_txdb_opens;
    std::atomic<int64_t> refill_alreadyhave_checks;

    std::atomic<int64_t> block_receive_total;
    std::atomic<int64_t> block_result_accepted_active;
    std::atomic<int64_t> block_result_orphan_new;
    std::atomic<int64_t> block_result_orphan_limit_rejected;
    std::atomic<int64_t> block_result_rejected_total;
    std::atomic<int64_t> setbestchain_commits;
    std::atomic<int64_t> stalled_recovery_attempts;

    std::atomic<int64_t> active_decrement_receive_clear_inflight;
    std::atomic<int64_t> active_decrement_inflight_timeout;
    std::atomic<int64_t> active_decrement_askfor_sent_transition;
    std::atomic<int64_t> active_decrement_askfor_removed_already_have;
    std::atomic<int64_t> active_decrement_askfor_removed_owner_conflict;
    std::atomic<int64_t> active_decrement_clear_askfor;
    std::atomic<int64_t> active_decrement_disconnect_cleanup;
    std::atomic<int64_t> active_decrement_other;

    std::atomic<int64_t> global_active_zero_transitions;
    std::atomic<int64_t> zero_with_total_deferred_nonempty;
    std::atomic<int64_t> zero_with_total_deferred_empty;
    std::atomic<int64_t> zero_with_eligible_ahead_peer;
    std::atomic<int64_t> zero_without_eligible_ahead_peer;
    std::atomic<int64_t> zero_duration_total_ms;
    std::atomic<int64_t> zero_duration_max_ms;
    std::atomic<int64_t> zero_start_time_ms;

    std::atomic<int64_t> refill_opportunity_slot_freed;
    std::atomic<int64_t> refill_sendmessages_passes;
    std::atomic<int64_t> refill_skipped_cs_main_trylock_failed;
    std::atomic<int64_t> refill_called_deferred_empty;
    std::atomic<int64_t> refill_called_positive_budget_nonempty;
    std::atomic<int64_t> refill_positive_budget_admitted_zero;
    std::atomic<int64_t> refill_positive_budget_admitted_count;

    std::atomic<int64_t> pipeline_drained_checks;
    std::atomic<int64_t> pipeline_drained_getblocks_queued;
    std::atomic<int64_t> pipeline_drained_skip_not_ahead;
    std::atomic<int64_t> pipeline_drained_skip_getblocks_10s_cooldown;
    std::atomic<int64_t> pipeline_drained_skip_other_condition;
    // Same event as getblocks_dedup_skips; retained for continuity with the
    // earlier PushGetBlocks-specific counter name.
    std::atomic<int64_t> pushgetblocks_dedup_5s_skips;

    std::atomic<int64_t> askfor_skip_orphan_limit_cooldown;
    std::atomic<int64_t> orphan_limit_cooldown_recorded;
    std::atomic<int64_t> orphan_limit_cross_peer_admitted;
    std::atomic<int64_t> orphan_limit_frontier_retry_queued;
    std::atomic<int64_t> orphan_limit_frontier_retry_pending;
    std::atomic<int64_t> askfor_skip_mapalreadyasked_cap;
    std::atomic<int64_t> askfor_skip_other_peer_owner;
    std::atomic<int64_t> askfor_skip_already_queued;
    std::atomic<int64_t> askfor_skip_inflight;

    std::atomic<int64_t> total_deferred_current;
    std::atomic<int64_t> total_deferred_max;
    std::atomic<int64_t> total_queued_current;
    std::atomic<int64_t> total_queued_max;
    std::atomic<int64_t> total_inflight_current;
    std::atomic<int64_t> total_inflight_max;
    std::atomic<int64_t> eligible_ahead_peers_current;
    std::atomic<int64_t> eligible_ahead_peers_max;

    std::atomic<int64_t> getblocks_decision_attempts_other;
    std::atomic<int64_t> getblocks_decision_attempts_initial;
    std::atomic<int64_t> getblocks_decision_attempts_continuation;
    std::atomic<int64_t> getblocks_decision_attempts_recovery;
    std::atomic<int64_t> getblocks_decision_attempts_prefetch;
    std::atomic<int64_t> getblocks_decision_attempts_inv_continuation;
    std::atomic<int64_t> getblocks_decision_attempts_version;
    std::atomic<int64_t> getblocks_decision_attempts_headers;
    std::atomic<int64_t> getblocks_decision_attempts_checkpoint;
    std::atomic<int64_t> getblocks_decision_attempts_wallet_rescan;
    std::atomic<int64_t> getblocks_decision_attempts_orphan_limit;
    std::atomic<int64_t> getblocks_decision_attempts_empty_pipeline_wake;
    std::atomic<int64_t> getblocks_queue_success_other;
    std::atomic<int64_t> getblocks_queue_success_initial;
    std::atomic<int64_t> getblocks_queue_success_continuation;
    std::atomic<int64_t> getblocks_queue_success_recovery;
    std::atomic<int64_t> getblocks_queue_success_prefetch;
    std::atomic<int64_t> getblocks_queue_success_inv_continuation;
    std::atomic<int64_t> getblocks_queue_success_version;
    std::atomic<int64_t> getblocks_queue_success_headers;
    std::atomic<int64_t> getblocks_queue_success_checkpoint;
    std::atomic<int64_t> getblocks_queue_success_wallet_rescan;
    std::atomic<int64_t> getblocks_queue_success_orphan_limit;
    std::atomic<int64_t> getblocks_queue_success_empty_pipeline_wake;
    std::atomic<int64_t> getblocks_wire_sent_other;
    std::atomic<int64_t> getblocks_wire_sent_initial;
    std::atomic<int64_t> getblocks_wire_sent_continuation;
    std::atomic<int64_t> getblocks_wire_sent_recovery;
    std::atomic<int64_t> getblocks_wire_sent_prefetch;
    std::atomic<int64_t> getblocks_wire_sent_inv_continuation;
    std::atomic<int64_t> getblocks_wire_sent_version;
    std::atomic<int64_t> getblocks_wire_sent_headers;
    std::atomic<int64_t> getblocks_wire_sent_checkpoint;
    std::atomic<int64_t> getblocks_wire_sent_wallet_rescan;
    std::atomic<int64_t> getblocks_wire_sent_orphan_limit;
    std::atomic<int64_t> getblocks_wire_sent_empty_pipeline_wake;
    std::atomic<int64_t> getblocks_dedup_skips;
    std::atomic<int64_t> getblocks_identical_to_last_sent;
    std::atomic<int64_t> getblocks_response_inv_messages;
    std::atomic<int64_t> getblocks_response_block_inv_count;
    std::atomic<int64_t> getblocks_response_unknown_count;
    // FIFO/time-order attributed INV observed with zero unknown block hashes.
    // getblocks_response_zero_unknown is the legacy name; the _inv_ spelling
    // is the precise interpretation exposed for new dashboards.
    std::atomic<int64_t> getblocks_response_zero_unknown;
    std::atomic<int64_t> getblocks_response_inv_zero_unknown;
    std::atomic<int64_t> recovery_outcome_useful;
    std::atomic<int64_t> recovery_outcome_known_only;
    std::atomic<int64_t> recovery_outcome_no_response;
    std::atomic<int64_t> zero_latency_recorded_mask;
    std::atomic<int64_t> zero_to_first_getblocks_wire_send_ms_total;
    std::atomic<int64_t> zero_to_first_getblocks_wire_send_ms_max;
    std::atomic<int64_t> zero_to_first_inv_ms_total;
    std::atomic<int64_t> zero_to_first_inv_ms_max;
    std::atomic<int64_t> zero_to_first_unknown_inv_ms_total;
    std::atomic<int64_t> zero_to_first_unknown_inv_ms_max;
    std::atomic<int64_t> zero_to_first_askfor_ms_total;
    std::atomic<int64_t> zero_to_first_askfor_ms_max;
    std::atomic<int64_t> zero_to_active_nonzero_ms_total;
    std::atomic<int64_t> zero_to_active_nonzero_ms_max;
    std::atomic<int64_t> inv_unknown_during_zero_global;
    std::atomic<int64_t> sync_peer_change_while_pipeline_empty;
    std::atomic<int64_t> getblocks_outstanding_current;
    std::atomic<int64_t> getblocks_outstanding_max;
    std::atomic<int64_t> getblocks_outstanding_timeout;
    std::atomic<int64_t> getblocks_no_response_disconnect_cleanup;
    std::atomic<int64_t> getblocks_pending_coalesce;
    std::atomic<int64_t> getblocks_pending_replaced;
    std::atomic<int64_t> getblocks_pending_drop;
    std::atomic<int64_t> outstanding_gauge_mismatch;
    std::atomic<int64_t> getblocks_queued_unsent_cleanup;
    std::atomic<int64_t> peers_with_queued_getblocks_current;
    std::atomic<int64_t> peers_with_queued_getblocks_max;
    std::atomic<int64_t> total_getblocks_queued_requests_current;
    std::atomic<int64_t> total_getblocks_queued_requests_max;
    std::atomic<int64_t> pipeline_active_due_to_getblocks_current;
    std::atomic<int64_t> pipeline_active_due_to_getblocks_max;
    std::atomic<int64_t> frontier_response_armed;
    std::atomic<int64_t> frontier_response_consumed;
    std::atomic<int64_t> frontier_response_pending_current;
    std::atomic<int64_t> frontier_response_pending_max;
    std::atomic<int64_t> frontier_reject_locator_stale;
    std::atomic<int64_t> frontier_reject_slot_busy;
    std::atomic<int64_t> frontier_reject_already_admitted;
    std::atomic<int64_t> frontier_reject_other;
    std::atomic<int64_t> ibd_state_current;
    std::atomic<int64_t> ibd_state_transitions;

    std::atomic<int64_t> pipeline_wake_signals;
    std::atomic<int64_t> pipeline_wake_signal_clear_inflight;
    std::atomic<int64_t> pipeline_wake_signal_inflight_timeout;
    std::atomic<int64_t> pipeline_wake_signal_askfor_already_have;
    std::atomic<int64_t> pipeline_wake_signal_askfor_owner_conflict;
    std::atomic<int64_t> pipeline_wake_signal_queue_removal;
    std::atomic<int64_t> pipeline_wake_signal_clear_askfor;
    std::atomic<int64_t> pipeline_wake_signal_disconnect_cleanup;
    std::atomic<int64_t> pipeline_wake_signal_getblocks_outstanding_cleared;
    std::atomic<int64_t> pipeline_wake_signal_getblocks_outstanding_timeout;
    std::atomic<int64_t> pipeline_wake_signal_other;
    std::atomic<int64_t> pipeline_wake_coalesced;
    std::atomic<int64_t> pipeline_wake_handler_runs;
    std::atomic<int64_t> pipeline_wake_transient_cs_main_trylock_failed;
    std::atomic<int64_t> pipeline_wake_transient_cs_vnodes_trylock_failed;
    std::atomic<int64_t> pipeline_wake_transient_cooldown_active;
    std::atomic<int64_t> pipeline_wake_transient_dedup_all;
    std::atomic<int64_t> pipeline_wake_transient_incomplete_peer_scan;
    std::atomic<int64_t> pipeline_wake_transient_shutdown;
    std::atomic<int64_t> pipeline_wake_terminal_not_ibd;
    std::atomic<int64_t> pipeline_wake_terminal_pipeline_not_empty;
    std::atomic<int64_t> pipeline_wake_terminal_deferred_refill_created_work;
    std::atomic<int64_t> pipeline_wake_terminal_getblocks_queued;
    std::atomic<int64_t> pipeline_wake_terminal_no_eligible_ahead_peer;
    std::atomic<int64_t> pipeline_wake_terminal_existing_queued_getblocks;
    std::atomic<int64_t> pipeline_wake_terminal_outstanding_getblocks_present;
    std::atomic<int64_t> pipeline_wake_refill_attempts;
    std::atomic<int64_t> pipeline_wake_refill_admitted;
    std::atomic<int64_t> pipeline_wake_getblocks_attempted;
    std::atomic<int64_t> pipeline_wake_getblocks_queued;
    std::atomic<int64_t> pipeline_wake_getblocks_dedup;
    std::atomic<int64_t> pipeline_wake_active_restored;
    std::atomic<int64_t> pipeline_wake_signal_start_ms;
    std::atomic<int64_t> pipeline_wake_signal_to_active_ms_total;
    std::atomic<int64_t> pipeline_wake_signal_to_active_ms_max;

    // Experiment A (future-supply diversification) counters.
    std::atomic<int64_t> diversify_candidates;
    std::atomic<int64_t> diversify_picked_other_lane;
    std::atomic<int64_t> diversify_picked_announcer;
    std::atomic<int64_t> diversify_snapshot_skip_lock;
    std::atomic<int64_t> diversify_no_other_lane;
    std::atomic<int64_t> diversify_other_lane_timeout;

    Counters();
};

Counters& Get();

inline void AtomicMax(std::atomic<int64_t>& target, int64_t value)
{
    int64_t current = target.load(std::memory_order_relaxed);
    while (current < value &&
           !target.compare_exchange_weak(current, value,
                                         std::memory_order_relaxed))
    {
    }
}

// Mirror update for the global active-request pressure (sum over peers of
// queued + in-flight block requests).  Callers pass +1/-1 to reflect set
// insertions/removals.  Also tracks the running high-water mark.
void GlobalActiveAdd(int64_t delta,
                     ActiveDecrementCause cause = ACTIVE_DECREMENT_OTHER);
void DeferredAdd(int64_t delta);
void QueuedAdd(int64_t delta);
void InflightAdd(int64_t delta);
void SetEligibleAheadPeers(int64_t count);
void RecordGetBlocksDecision(GetBlocksSource source);
void RecordGetBlocksQueueSuccess(GetBlocksSource source);
void RecordGetBlocksWireSent(GetBlocksSource source);
void GetBlocksQueuedAdd(int64_t delta, bool peerTransition);
void GetBlocksOutstandingAdd(int64_t delta);
void FrontierResponsePendingAdd(int64_t delta);
void RecordIBDState(bool fInitialBlockDownload);
void RecordZeroLatency(ZeroLatencyEvent event);

// Per-peer deferred-budget zero-state transition bookkeeping.  Callers pass
// the previous (0/1) and new (0/1) zero-budget state for a peer.
void PeerZeroStateChange(int nOldZero, int nNewZero);

// Read a snapshot of all counters (relaxed loads; not a consistent read).
void SnapshotAll(IBDMetricsSnapshot& out);

// Test-only: zero the pipeline-wake counters so unit tests can assert exact
// deltas.  Production call sites never use it.
void ResetPipelineWakeMetricsForTesting();

} // namespace ibdmetrics

#endif // INNOVA_IBDMETRICS_H
