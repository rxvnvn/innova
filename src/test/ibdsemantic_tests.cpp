// Copyright (c) 2026 The Innova developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <algorithm>
#include <string>
#include <vector>

#include <boost/test/unit_test.hpp>

#include "ibdsemantic.h"
#include "ibdmetrics.h"
#include "main.h"
#include "net.h"
#include "version.h"

// Arm-only semantic IBD health diagnostics tests.
//
// 20 semantic tests drive the pure rolling-window evaluation through
// ibdsemantic::AddSampleForTesting and assert derived metrics, predicates,
// arm candidates, and counters.  8 recovery tests drive
// CStalledSyncRecoveryState::ShouldRecover and assert that the observation
// counters classify every skip branch while the decision is preserved.

namespace {

static const int64_t SEM_TEST_TIME = 1000000;

static CAddress SemTestPeerAddress(unsigned int nPeer)
{
    struct in_addr addr;
    addr.s_addr = 0x0100007f + (nPeer << 24);
    return CAddress(CService(addr, GetDefaultPort()));
}

// CNode is not copyable; configure stack-allocated peers in place.
static void SemPreparePeer(CNode& node, int nPeerHeight)
{
    node.nVersion = PROTOCOL_VERSION;
    node.nRecvVersion = PROTOCOL_VERSION;
    node.fSuccessfullyConnected = true;
    node.fClient = false;
    node.fOneShot = false;
    node.fDisconnect = false;
    node.nChainHeight = nPeerHeight;
    node.nBestKnownHeight = nPeerHeight;
}

static ibdsemantic::SemanticSample SemSample(
    int64_t nTime, int nHeight, int64_t nReceive, int64_t nOrphanNew,
    int64_t nOrphanLimit, int64_t nRejected, int64_t nPipeline,
    int nEligible, int nConfirmed, bool fConfirmed, int nPeerMax)
{
    ibdsemantic::SemanticSample s;
    s.time_us = nTime;
    s.fHeightAvailable = nHeight >= 0;
    s.nBestHeight = nHeight;
    s.block_receive_total = nReceive;
    s.block_result_orphan_new = nOrphanNew;
    s.block_result_orphan_limit_rejected = nOrphanLimit;
    s.block_result_rejected_total = nRejected;
    s.pipeline_events = nPipeline;
    s.eligible_ahead_peer_count = nEligible;
    s.confirmed_peer_height = nConfirmed;
    s.fConfirmedGapAvailable = fConfirmed;
    s.peer_height_max = nPeerMax;
    s.oldest_live_inflight_age_ms = 0;
    s.inflight_gt1s = 0;
    s.inflight_gt4s = 0;
    return s;
}

static void EnableAndReset()
{
    BOOST_REQUIRE(ibdsemantic::SetEnabled(1));
    ibdsemantic::ResetForTesting();
    ibdsemantic::SetThresholdsForTesting(0.2, 0.5, 0.2, 5000);
}

static ibdsemantic::IBDSemanticSnapshot SemSnapshot()
{
    ibdsemantic::IBDSemanticSnapshot snap;
    ibdsemantic::SnapshotAll(snap);
    return snap;
}

// Canonical pathological-but-active IBD profile: tip completely stuck while
// blocks keep arriving as orphans, the dispatch pipeline is busy, and at
// least two peers advertise a height far ahead of local.  All seven arm
// predicates hold for this profile.
static ibdsemantic::SemanticSample ArmProfile(int i)
{
    return SemSample(SEM_TEST_TIME + i, 1000, 10 + i, 9 + i, 0, 0, 5 + i,
                     2, 20000, true, 25000);
}

} // namespace

BOOST_AUTO_TEST_SUITE(ibdsemantic_tests)

BOOST_AUTO_TEST_CASE(disabled_is_inert)
{
    BOOST_REQUIRE(ibdsemantic::SetEnabled(0));
    ibdsemantic::ResetForTesting();
    for (int i = 0; i < 40; ++i)
        ibdsemantic::AddSampleForTesting(ArmProfile(i));

    const ibdsemantic::IBDSemanticSnapshot snap = SemSnapshot();
    BOOST_CHECK_EQUAL(snap.nEnabled, 0);
    BOOST_CHECK_EQUAL(snap.nSampleCount, 0);
    BOOST_CHECK_EQUAL(snap.semantic_checks_total, 0);
    BOOST_CHECK_EQUAL(snap.semantic_arm_candidate_total, 0);
    BOOST_CHECK(!snap.fWindowReady);
    BOOST_CHECK(!snap.arm_candidate);
}

BOOST_AUTO_TEST_CASE(window_empty_at_startup)
{
    EnableAndReset();
    const ibdsemantic::IBDSemanticSnapshot snap = SemSnapshot();
    BOOST_CHECK_EQUAL(snap.nSampleCount, 0);
    BOOST_CHECK(!snap.fWindowReady);
    BOOST_CHECK(!snap.a1_low_progress);
    BOOST_CHECK(!snap.arm_candidate);
    BOOST_CHECK_EQUAL(snap.semantic_checks_total, 0);
}

BOOST_AUTO_TEST_CASE(partial_startup_below_min_arm_samples)
{
    EnableAndReset();
    for (int i = 0; i < 29; ++i)
        ibdsemantic::AddSampleForTesting(ArmProfile(i));

    const ibdsemantic::IBDSemanticSnapshot snap = SemSnapshot();
    BOOST_CHECK_EQUAL(snap.nSampleCount, 29);
    BOOST_CHECK(!snap.fWindowReady);
    BOOST_CHECK(!snap.a1_low_progress);
    BOOST_CHECK(!snap.arm_candidate);
}

BOOST_AUTO_TEST_CASE(tip_rate_healthy_blocks_arm)
{
    EnableAndReset();
    for (int i = 0; i < 40; ++i)
    {
        ibdsemantic::SemanticSample s = ArmProfile(i);
        s.nBestHeight = 1000 + i;
        s.fHeightAvailable = true;
        ibdsemantic::AddSampleForTesting(s);
    }
    const ibdsemantic::IBDSemanticSnapshot snap = SemSnapshot();
    BOOST_CHECK(snap.progress_rate_5m >= 0.2);
    BOOST_CHECK(!snap.a1_low_progress);
    BOOST_CHECK(!snap.arm_candidate);
}

BOOST_AUTO_TEST_CASE(tip_rate_slow_below_threshold_arms)
{
    EnableAndReset();
    for (int i = 0; i < 40; ++i)
        ibdsemantic::AddSampleForTesting(ArmProfile(i));

    const ibdsemantic::IBDSemanticSnapshot snap = SemSnapshot();
    BOOST_CHECK(snap.fWindowReady);
    BOOST_CHECK(snap.a1_low_progress);
    BOOST_CHECK(snap.arm_candidate);
    // Arm counting starts once the 30-sample minimum is reached: the last
    // 40 - 29 = 11 evaluations are window-ready and arm.
    BOOST_CHECK_EQUAL(snap.semantic_arm_candidate_total, 11);
}

BOOST_AUTO_TEST_CASE(bursty_median_zero_not_armed_by_median_alone)
{
    EnableAndReset();
    for (int i = 0; i < 40; ++i)
    {
        ibdsemantic::SemanticSample s = ArmProfile(i);
        s.nBestHeight = (i == 39) ? 1000 : 0;
        s.fHeightAvailable = true;
        ibdsemantic::AddSampleForTesting(s);
    }
    const ibdsemantic::IBDSemanticSnapshot snap = SemSnapshot();
    // A single burst yields a zero median but a high mean rate: a median-only
    // criterion would arm a bursty-but-healthy IBD.  The mean rate must veto.
    BOOST_CHECK_EQUAL(snap.progress_median_5m, 0.0);
    BOOST_CHECK(snap.progress_rate_5m >= 0.2);
    BOOST_CHECK(!snap.a1_low_progress);
    BOOST_CHECK(!snap.arm_candidate);
}

BOOST_AUTO_TEST_CASE(orphan_pressure_dominance_required)
{
    EnableAndReset();
    for (int i = 0; i < 40; ++i)
    {
        ibdsemantic::SemanticSample s = ArmProfile(i);
        s.block_result_orphan_new = 0;   // receives are all useful
        ibdsemantic::AddSampleForTesting(s);
    }
    const ibdsemantic::IBDSemanticSnapshot snap = SemSnapshot();
    BOOST_CHECK(snap.orphan_pressure_1m <= 0.5);
    BOOST_CHECK(!snap.a3_orphan_dominant);
    BOOST_CHECK(!snap.arm_candidate);
}

BOOST_AUTO_TEST_CASE(reject_futility_veto_blocks_arm)
{
    EnableAndReset();
    for (int i = 0; i < 40; ++i)
    {
        ibdsemantic::SemanticSample s = ArmProfile(i);
        s.block_result_rejected_total = 5 + i;  // 5 of 10 receives rejected
        ibdsemantic::AddSampleForTesting(s);
    }
    const ibdsemantic::IBDSemanticSnapshot snap = SemSnapshot();
    BOOST_CHECK(snap.fReceiveActive);
    BOOST_CHECK(snap.reject_futility_1m >= 0.2);
    BOOST_CHECK(!snap.a3b_not_futile);
    BOOST_CHECK(!snap.arm_candidate);
}

BOOST_AUTO_TEST_CASE(receive_silence_blocks_arm)
{
    EnableAndReset();
    for (int i = 0; i < 40; ++i)
    {
        ibdsemantic::SemanticSample s = ArmProfile(i);
        s.block_receive_total = 0;       // no blocks received at all
        s.block_result_orphan_new = 0;
        s.block_result_rejected_total = 0;
        ibdsemantic::AddSampleForTesting(s);
    }
    const ibdsemantic::IBDSemanticSnapshot snap = SemSnapshot();
    BOOST_CHECK(!snap.fReceiveActive);
    BOOST_CHECK_EQUAL(snap.received_1m, 0);
    BOOST_CHECK(!snap.a2_receiving);
    BOOST_CHECK(!snap.arm_candidate);
}

BOOST_AUTO_TEST_CASE(peer_height_requires_two_ahead_peers)
{
    EnableAndReset();
    CNode peer1(INVALID_SOCKET, SemTestPeerAddress(1), "single-ahead-peer", true);
    SemPreparePeer(peer1, nBestHeight + 10000);
    std::vector<CNode*> peers;
    peers.push_back(&peer1);

    const ibdsemantic::PeerHeightScan scan =
        ibdsemantic::ScanEligibleAheadPeers(peers, nBestHeight);
    BOOST_CHECK_EQUAL(scan.eligible_count, 1);
    BOOST_CHECK(!scan.fConfirmedAvailable);

    ibdsemantic::SemanticSample s = ArmProfile(0);
    s.eligible_ahead_peer_count = 1;
    s.fConfirmedGapAvailable = false;
    s.confirmed_peer_height = 0;
    s.peer_height_max = nBestHeight + 10000;
    ibdsemantic::AddSampleForTesting(s);
    const ibdsemantic::IBDSemanticSnapshot snap = SemSnapshot();
    BOOST_CHECK(!snap.fConfirmedGapAvailable);
    BOOST_CHECK(!snap.a5_large_confirmed_gap);
    BOOST_CHECK(!snap.a6_multiple_ahead_peers);
    BOOST_CHECK(!snap.arm_candidate);
}

BOOST_AUTO_TEST_CASE(confirmed_height_is_second_highest)
{
    EnableAndReset();
    CNode peer1(INVALID_SOCKET, SemTestPeerAddress(1), "ahead-low", true);
    CNode peer2(INVALID_SOCKET, SemTestPeerAddress(2), "ahead-mid", true);
    CNode peer3(INVALID_SOCKET, SemTestPeerAddress(3), "ahead-high", true);
    CNode peer4(INVALID_SOCKET, SemTestPeerAddress(4), "not-ahead", true);
    SemPreparePeer(peer1, nBestHeight + 100);
    SemPreparePeer(peer2, nBestHeight + 300);
    SemPreparePeer(peer3, nBestHeight + 500);
    SemPreparePeer(peer4, nBestHeight - 50);
    std::vector<CNode*> peers;
    peers.push_back(&peer1);
    peers.push_back(&peer2);
    peers.push_back(&peer3);
    peers.push_back(&peer4);

    const ibdsemantic::PeerHeightScan scan =
        ibdsemantic::ScanEligibleAheadPeers(peers, nBestHeight);
    BOOST_CHECK_EQUAL(scan.eligible_count, 3);
    BOOST_CHECK_EQUAL(scan.peer_height_max, nBestHeight + 500);
    BOOST_CHECK_EQUAL(scan.confirmed_peer_height, nBestHeight + 300);
    BOOST_CHECK(scan.fConfirmedAvailable);
}

BOOST_AUTO_TEST_CASE(peer_gap_shrinking_during_catchup)
{
    EnableAndReset();
    for (int i = 0; i < 40; ++i)
    {
        ibdsemantic::SemanticSample s = ArmProfile(i);
        s.nBestHeight = 1000 + i;               // local catching up
        s.fHeightAvailable = true;
        s.peer_height_max = 20000;              // peer tip static
        ibdsemantic::AddSampleForTesting(s);
    }
    const ibdsemantic::IBDSemanticSnapshot snap = SemSnapshot();
    BOOST_CHECK(snap.peer_gap_change_1m < 0);
    BOOST_CHECK(snap.peer_gap_max > 0);
}

BOOST_AUTO_TEST_CASE(peer_gap_static_when_synced_gap)
{
    EnableAndReset();
    for (int i = 0; i < 40; ++i)
    {
        ibdsemantic::SemanticSample s = ArmProfile(i);
        s.nBestHeight = 1000;
        s.fHeightAvailable = true;
        s.peer_height_max = 20000;
        ibdsemantic::AddSampleForTesting(s);
    }
    const ibdsemantic::IBDSemanticSnapshot snap = SemSnapshot();
    BOOST_CHECK_EQUAL(snap.peer_gap_max, 19000);
    BOOST_CHECK_EQUAL(snap.peer_gap_change_1m, 0);
}

BOOST_AUTO_TEST_CASE(peer_gap_growing_when_peer_pulls_ahead)
{
    EnableAndReset();
    for (int i = 0; i < 40; ++i)
    {
        ibdsemantic::SemanticSample s = ArmProfile(i);
        s.nBestHeight = 1000;
        s.fHeightAvailable = true;
        s.peer_height_max = 20000 + 2 * i;      // peer advancing, local stuck
        ibdsemantic::AddSampleForTesting(s);
    }
    const ibdsemantic::IBDSemanticSnapshot snap = SemSnapshot();
    BOOST_CHECK(snap.peer_gap_change_1m > 0);
}

BOOST_AUTO_TEST_CASE(pipeline_activity_required_for_arm)
{
    EnableAndReset();
    for (int i = 0; i < 40; ++i)
    {
        ibdsemantic::SemanticSample s = ArmProfile(i);
        s.pipeline_events = 0;                  // no dispatch activity
        ibdsemantic::AddSampleForTesting(s);
    }
    const ibdsemantic::IBDSemanticSnapshot snap = SemSnapshot();
    BOOST_CHECK_EQUAL(snap.pipeline_events_1m, 0);
    BOOST_CHECK(!snap.a4_pipeline_busy);
    BOOST_CHECK(!snap.arm_candidate);
}

BOOST_AUTO_TEST_CASE(arm_all_predicates_true)
{
    EnableAndReset();
    for (int i = 0; i < 40; ++i)
        ibdsemantic::AddSampleForTesting(ArmProfile(i));

    const ibdsemantic::IBDSemanticSnapshot snap = SemSnapshot();
    BOOST_CHECK(snap.a1_low_progress);
    BOOST_CHECK(snap.a2_receiving);
    BOOST_CHECK(snap.a3_orphan_dominant);
    BOOST_CHECK(snap.a3b_not_futile);
    BOOST_CHECK(snap.a4_pipeline_busy);
    BOOST_CHECK(snap.a5_large_confirmed_gap);
    BOOST_CHECK(snap.a6_multiple_ahead_peers);
    BOOST_CHECK(snap.arm_candidate);
    BOOST_CHECK_EQUAL(snap.semantic_checks_total, 40);
    BOOST_CHECK_EQUAL(snap.semantic_arm_candidate_total, 11);
    BOOST_CHECK_EQUAL(snap.semantic_a1_true_total, 11);
    BOOST_CHECK_EQUAL(snap.semantic_all_but_a4_total, 0);
}

BOOST_AUTO_TEST_CASE(all_but_a4_counter_tracks_pipeline_failure)
{
    EnableAndReset();
    for (int i = 0; i < 40; ++i)
    {
        ibdsemantic::SemanticSample s = ArmProfile(i);
        s.pipeline_events = 0;                  // only a4 fails
        ibdsemantic::AddSampleForTesting(s);
    }
    const ibdsemantic::IBDSemanticSnapshot snap = SemSnapshot();
    BOOST_CHECK(!snap.a4_pipeline_busy);
    BOOST_CHECK(snap.a1_low_progress);
    BOOST_CHECK(snap.a2_receiving);
    BOOST_CHECK(snap.a3_orphan_dominant);
    BOOST_CHECK(snap.a3b_not_futile);
    BOOST_CHECK(snap.a5_large_confirmed_gap);
    BOOST_CHECK(snap.a6_multiple_ahead_peers);
    BOOST_CHECK(!snap.arm_candidate);
    // Only the 11 window-ready evaluations satisfy all-but-a4.
    BOOST_CHECK_EQUAL(snap.semantic_all_but_a4_total, 11);
    BOOST_CHECK_EQUAL(snap.semantic_all_but_a1_total, 0);
}

BOOST_AUTO_TEST_CASE(windows_bounded_at_300_samples)
{
    EnableAndReset();
    for (int i = 0; i < 350; ++i)
        ibdsemantic::AddSampleForTesting(ArmProfile(i));
    BOOST_CHECK_EQUAL(ibdsemantic::SampleCountForTesting(),
                      ibdsemantic::SEMANTIC_WINDOW_SIZE);
    const ibdsemantic::IBDSemanticSnapshot snap = SemSnapshot();
    BOOST_CHECK_EQUAL(snap.nSampleCount, ibdsemantic::SEMANTIC_WINDOW_SIZE);
}

BOOST_AUTO_TEST_CASE(reset_clears_window_and_counters)
{
    EnableAndReset();
    for (int i = 0; i < 40; ++i)
        ibdsemantic::AddSampleForTesting(ArmProfile(i));
    BOOST_CHECK_EQUAL(ibdsemantic::SampleCountForTesting(), 40);

    ibdsemantic::ResetForTesting();
    const ibdsemantic::IBDSemanticSnapshot snap = SemSnapshot();
    BOOST_CHECK_EQUAL(snap.nSampleCount, 0);
    BOOST_CHECK_EQUAL(snap.semantic_checks_total, 0);
    BOOST_CHECK_EQUAL(snap.semantic_arm_candidate_total, 0);
    BOOST_CHECK_EQUAL(snap.recovery_checks_total, 0);
    BOOST_CHECK(!snap.fWindowReady);
}

BOOST_AUTO_TEST_CASE(height_lock_failure_no_fabricated_zero_delta)
{
    EnableAndReset();
    // One available sample, then a long cs_main lock-failure gap, then one
    // available sample.  The gap must never synthesize zero-height samples:
    // the per-second rate stays invalid (no consecutive available pairs) and
    // the tip delta is computed only between the real available heights.
    for (int i = 0; i < 34; ++i)
    {
        ibdsemantic::SemanticSample s = ArmProfile(i);
        s.fHeightAvailable = (i == 0 || i == 34 - 1);
        s.nBestHeight = (i == 0) ? 10 : ((i == 34 - 1) ? 30 : -1);
        ibdsemantic::AddSampleForTesting(s);
    }
    const ibdsemantic::IBDSemanticSnapshot snap = SemSnapshot();
    BOOST_CHECK_EQUAL(snap.nSampleCount, 34);
    BOOST_CHECK_EQUAL(snap.tip_delta_5m, 20);
    BOOST_CHECK(!snap.a1_low_progress);
    BOOST_CHECK(!snap.arm_candidate);
}

BOOST_AUTO_TEST_CASE(recovery_not_armed_skip)
{
    EnableAndReset();
    CStalledSyncRecoveryState state;
    BOOST_CHECK(!state.ShouldRecover(
        nBestHeight, nBestHeight + 10, false, SEM_TEST_TIME, 15, 30));
    const ibdsemantic::IBDSemanticSnapshot snap = SemSnapshot();
    BOOST_CHECK_EQUAL(snap.recovery_checks_total, 1);
    BOOST_CHECK_EQUAL(snap.recovery_skip_not_armed, 1);
    BOOST_CHECK_EQUAL(snap.recovery_triggered, 0);
}

BOOST_AUTO_TEST_CASE(recovery_height_changed_skip)
{
    EnableAndReset();
    CStalledSyncRecoveryState state;
    state.MarkSyncRequestSent(SEM_TEST_TIME);
    BOOST_CHECK(!state.ShouldRecover(
        nBestHeight + 1, nBestHeight + 10, false, SEM_TEST_TIME + 1, 15, 30));
    const ibdsemantic::IBDSemanticSnapshot snap = SemSnapshot();
    BOOST_CHECK_EQUAL(snap.recovery_skip_height_changed, 1);
    BOOST_CHECK_EQUAL(snap.recovery_triggered, 0);
}

BOOST_AUTO_TEST_CASE(recovery_peer_not_ahead_skip)
{
    EnableAndReset();
    CStalledSyncRecoveryState state;
    state.MarkSyncRequestSent(SEM_TEST_TIME);
    BOOST_CHECK(!state.ShouldRecover(
        nBestHeight, nBestHeight, false, SEM_TEST_TIME, 15, 30));
    const ibdsemantic::IBDSemanticSnapshot snap = SemSnapshot();
    BOOST_CHECK_EQUAL(snap.recovery_skip_peer_not_ahead, 1);
    BOOST_CHECK_EQUAL(snap.recovery_skip_pipeline_active, 0);
    BOOST_CHECK_EQUAL(snap.recovery_triggered, 0);
}

BOOST_AUTO_TEST_CASE(recovery_pipeline_active_skip)
{
    EnableAndReset();
    CStalledSyncRecoveryState state;
    state.MarkSyncRequestSent(SEM_TEST_TIME);
    // Stall age 0 < timeout, pipeline busy -> plain pipeline-active skip.
    BOOST_CHECK(!state.ShouldRecover(
        nBestHeight, nBestHeight + 10, true, SEM_TEST_TIME, 15, 30));
    const ibdsemantic::IBDSemanticSnapshot snap = SemSnapshot();
    BOOST_CHECK_EQUAL(snap.recovery_skip_pipeline_active, 1);
    BOOST_CHECK_EQUAL(snap.recovery_skip_pipeline_active_after_timeout, 0);
    BOOST_CHECK_EQUAL(snap.recovery_triggered, 0);
}

BOOST_AUTO_TEST_CASE(recovery_pipeline_active_after_timeout_skip)
{
    EnableAndReset();
    CStalledSyncRecoveryState state;
    state.MarkSyncRequestSent(SEM_TEST_TIME);
    // Stall age 20 >= timeout while the pipeline stays busy.
    BOOST_CHECK(!state.ShouldRecover(
        nBestHeight, nBestHeight + 10, true, SEM_TEST_TIME + 20, 15, 30));
    const ibdsemantic::IBDSemanticSnapshot snap = SemSnapshot();
    BOOST_CHECK_EQUAL(snap.recovery_skip_pipeline_active_after_timeout, 1);
    BOOST_CHECK_EQUAL(snap.recovery_skip_pipeline_active, 0);
    BOOST_CHECK_EQUAL(snap.recovery_triggered, 0);
}

BOOST_AUTO_TEST_CASE(recovery_timeout_not_reached_skip)
{
    EnableAndReset();
    CStalledSyncRecoveryState state;
    state.MarkSyncRequestSent(SEM_TEST_TIME);
    BOOST_CHECK(!state.ShouldRecover(
        nBestHeight, nBestHeight + 10, false, SEM_TEST_TIME + 5, 15, 30));
    const ibdsemantic::IBDSemanticSnapshot snap = SemSnapshot();
    BOOST_CHECK_EQUAL(snap.recovery_skip_timeout_not_reached, 1);
    BOOST_CHECK_EQUAL(snap.recovery_triggered, 0);
}

BOOST_AUTO_TEST_CASE(recovery_cooldown_skip)
{
    EnableAndReset();
    CStalledSyncRecoveryState state;
    state.MarkSyncRequestSent(SEM_TEST_TIME);
    // First stall recovery triggers once the timeout is reached...
    BOOST_CHECK(state.ShouldRecover(
        nBestHeight, nBestHeight + 10, false, SEM_TEST_TIME + 20, 15, 30));
    // ...and the immediate re-evaluation is held by the exponential cooldown.
    BOOST_CHECK(!state.ShouldRecover(
        nBestHeight, nBestHeight + 10, false, SEM_TEST_TIME + 21, 15, 30));
    const ibdsemantic::IBDSemanticSnapshot snap = SemSnapshot();
    BOOST_CHECK_EQUAL(snap.recovery_triggered, 1);
    BOOST_CHECK_EQUAL(snap.recovery_skip_cooldown, 1);
}

BOOST_AUTO_TEST_CASE(recovery_trigger_preserves_decision)
{
    EnableAndReset();
    CStalledSyncRecoveryState state;
    state.MarkSyncRequestSent(SEM_TEST_TIME);
    // stalled_recovery_attempts is cumulative across the suite; assert the
    // delta caused by this trigger instead of an absolute value.
    const int64_t nAttemptsBefore =
        ibdmetrics::Get().stalled_recovery_attempts.load(
            std::memory_order_relaxed);
    // First stall recovery triggers once the timeout is reached...
    BOOST_CHECK(state.ShouldRecover(
        nBestHeight, nBestHeight + 10, false, SEM_TEST_TIME + 20, 15, 30));
    // ...the cooldown starts immediately after a trigger, so the same-instant
    // re-evaluation (and the next second) is held, exactly as before
    // instrumentation.
    BOOST_CHECK(!state.ShouldRecover(
        nBestHeight, nBestHeight + 10, false, SEM_TEST_TIME + 20, 15, 30));
    BOOST_CHECK(!state.ShouldRecover(
        nBestHeight, nBestHeight + 10, false, SEM_TEST_TIME + 21, 15, 30));
    const ibdsemantic::IBDSemanticSnapshot snap = SemSnapshot();
    BOOST_CHECK_EQUAL(snap.recovery_triggered, 1);
    BOOST_CHECK_EQUAL(snap.recovery_skip_cooldown, 2);
    // The instrumented trigger still performs the original side effects.
    BOOST_CHECK_EQUAL(state.RecoveryAttempts(), 1);
    BOOST_CHECK_EQUAL(
        ibdmetrics::Get().stalled_recovery_attempts.load(
            std::memory_order_relaxed),
        nAttemptsBefore + 1);
}

BOOST_AUTO_TEST_SUITE_END()
