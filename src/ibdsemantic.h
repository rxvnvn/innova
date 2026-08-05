// Copyright (c) 2026 The Innova developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef INNOVA_IBDSEMANTIC_H
#define INNOVA_IBDSEMANTIC_H

#include <stdint.h>

#include <vector>

class CNode;

// Arm-only semantic IBD health diagnostics.
//
// Disabled by default (-ibdsemrecover=0).  Enabled mode (1) is observation
// only: it samples tip height, block-result counters, pipeline activity, and
// peer heights once per second, rolls them into a bounded 300-sample window,
// and evaluates the A1..A6 arm predicates plus diagnostic log/recovery
// counters.  It never changes IBD behavior, limits, timeouts, scheduling,
// peer selection, request ownership, or recovery decisions (ShouldRecover()
// return values and side effects are preserved exactly).
//
// Sampling rules (audited):
//  - Tip height is sampled only under TRY_LOCK(cs_main); on lock failure the
//    sample is marked height-unavailable and no artificial zero delta is
//    fabricated from a stale value.
//  - Height deltas are computed only between consecutive successfully locked
//    samples; gaps never synthesize progress.
//  - Both the mean per-second rate and the median per-second delta are
//    exported.  The median alone is not used as an arm criterion (a bursty
//    healthy IBD may legitimately have median=0).

namespace ibdsemantic {

enum
{
    // Bounded rolling window in samples (one per second).
    SEMANTIC_WINDOW_SIZE = 300,
    // Minimum samples before the window can arm predicates.
    SEMANTIC_MIN_ARM_SAMPLES = 30,
    // One-minute and five-minute sub-window lengths.
    SEMANTIC_WINDOW_1M = 60,
    SEMANTIC_WINDOW_5M = 300
};

// Raw per-second observation gathered by the 1s emit hook.
struct SemanticSample
{
    int64_t time_us;
    bool fHeightAvailable;
    int nBestHeight;
    int64_t block_receive_total;
    int64_t block_result_orphan_new;
    int64_t block_result_orphan_limit_rejected;
    int64_t block_result_rejected_total;
    // Monotonic pipeline event counter (active_decrement_askfor_sent_transition).
    int64_t pipeline_events;
    int eligible_ahead_peer_count;
    int confirmed_peer_height;    // second-highest eligible-ahead height; 0 if not available
    bool fConfirmedGapAvailable;  // true only when >= 2 eligible-ahead peers
    int peer_height_max;
    int64_t oldest_live_inflight_age_ms;
    int64_t inflight_gt1s;
    int64_t inflight_gt4s;
};

// Peer height scan result (mirrors the MaybeQueueStalledSyncRecovery filter).
struct PeerHeightScan
{
    int eligible_count;
    int peer_height_max;
    int confirmed_peer_height;
    bool fConfirmedAvailable;
};

// Full observable snapshot for getinfo/ibdmetrics export.
struct IBDSemanticSnapshot
{
    int nEnabled;
    int nSampleCount;
    bool fWindowReady;
    int nLocalHeight;
    int64_t time_us;

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

    int eligible_ahead_peers;
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

    int64_t semantic_checks_total;
    int64_t semantic_arm_candidate_total;
    int64_t semantic_a1_true_total;
    int64_t semantic_a2_true_total;
    int64_t semantic_a3_true_total;
    int64_t semantic_a3b_true_total;
    int64_t semantic_a4_true_total;
    int64_t semantic_a5_true_total;
    int64_t semantic_a6_true_total;
    int64_t semantic_all_but_a1_total;
    int64_t semantic_all_but_a2_total;
    int64_t semantic_all_but_a3_total;
    int64_t semantic_all_but_a3b_total;
    int64_t semantic_all_but_a4_total;
    int64_t semantic_all_but_a5_total;
    int64_t semantic_all_but_a6_total;

    int64_t recovery_checks_total;
    int64_t recovery_skip_not_armed;
    int64_t recovery_skip_height_changed;
    int64_t recovery_skip_peer_not_ahead;
    int64_t recovery_skip_pipeline_active;
    int64_t recovery_skip_pipeline_active_after_timeout;
    int64_t recovery_skip_timeout_not_reached;
    int64_t recovery_skip_cooldown;
    int64_t recovery_triggered;
    int64_t recovery_armed;
    int64_t recovery_last_observed_height;
    int64_t recovery_stall_age_seconds;
    int64_t recovery_longest_stall_seconds;
    int64_t recovery_max_peer_height;
    int64_t recovery_peer_gap;
    int64_t recovery_pipeline_active;
};

// Enablement.  Only 0 (inert) and 1 (arm-only diagnostics) are accepted;
// any other value returns false so init can reject it explicitly.  Also
// (re)loads the threshold configuration from mapArgs.
bool SetEnabled(int nMode);
int GetEnabled();

// Peer scan used by the 1s hook and by unit tests.
PeerHeightScan ScanEligibleAheadPeers(const std::vector<CNode*>& vNodesCopy,
                                      int nLocalHeight);

// Append a raw sample and re-evaluate predicates/counters.  Used by the 1s
// hook; exported for deterministic unit tests.
void AddSampleForTesting(const SemanticSample& sample);

// One aggregated IBD_SEMANTIC_HEALTH_1S line per second (LogPrintf).  Called
// from the message-handler thread with the ref-counted peer list; scans peers
// only here, never per block.
void EmitIBDSemanticHealth1s(const std::vector<CNode*>& vNodesCopy);

// Snapshot for getinfo/ibdmetrics export.
void SnapshotAll(IBDSemanticSnapshot& out);

// Recovery evaluation observation hooks (net.cpp CStalledSyncRecoveryState::
// ShouldRecover).  These never alter the decision or side effects; they only
// count the branch taken and update gauges.
void RecordRecoveryCheck(int64_t nLocalHeight, int64_t nPeerHeight,
                         bool fPipelineActive, bool fSyncRequestSent,
                         int64_t nLastObservedHeight,
                         int64_t nLastProgressTime, int64_t nNow);
void RecordRecoverySkipNotArmed();
void RecordRecoverySkipHeightChanged();
void RecordRecoverySkipPeerNotAhead();
void RecordRecoverySkipPipelineActive();
void RecordRecoverySkipPipelineActiveAfterTimeout();
void RecordRecoverySkipTimeoutNotReached();
void RecordRecoverySkipCooldown();
void RecordRecoveryTriggered();

// Test helpers.
void ResetForTesting();
void SetThresholdsForTesting(double dMinProgressRate, double dOrphanDom,
                             double dFutileReject, int64_t nGapMin);
int SampleCountForTesting();

} // namespace ibdsemantic

#endif // INNOVA_IBDSEMANTIC_H
