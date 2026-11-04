// Copyright (c) 2026 The Innova developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// -p2pstallwatch decision contract.
//
// The 2026-11-04 incident had peers whose TCP connection stayed up while no
// header or block was delivered (blocksinflight=0, bestknownheight ahead of us,
// ping unanswered). The watchdog must key on PROGRESS, retry a bounded number of
// times with a growing backoff, and finally drop the silent peer so the addrman
// can supply another one. These cases pin exactly that contract; no sockets are
// involved.

#include <boost/test/unit_test.hpp>

#include "../net.h"

BOOST_AUTO_TEST_SUITE(p2p_stall_watch_tests)

BOOST_AUTO_TEST_CASE(no_action_when_peer_advertises_nothing_above_us)
{
    const pp2p::StallWatchDecision d =
        pp2p::DecideStallAction(1000, 0, 0, 0, /*peer*/ 8200000, /*our*/ 8200000);
    BOOST_CHECK(!d.requestHeaders);
    BOOST_CHECK(!d.disconnect);
    // one block is the normal race window, not a stall
    const pp2p::StallWatchDecision d2 =
        pp2p::DecideStallAction(1000, 0, 0, 0, 8200001, 8200000);
    BOOST_CHECK(!d2.requestHeaders);
}

BOOST_AUTO_TEST_CASE(no_action_while_the_peer_still_delivers)
{
    // progress 10 s ago, peer far ahead -> leave it alone
    const pp2p::StallWatchDecision d =
        pp2p::DecideStallAction(1000, 990, 0, 0, 8200100, 8200000);
    BOOST_CHECK(!d.requestHeaders);
    BOOST_CHECK(!d.disconnect);
}

BOOST_AUTO_TEST_CASE(stalled_peer_gets_a_bounded_retry_with_backoff)
{
    const int64_t now = 1000;
    const int64_t stale = now - pp2p::STALL_PROGRESS_SEC - 1;
    pp2p::StallWatchDecision d = pp2p::DecideStallAction(now, stale, 0, 0, 8200100, 8200000);
    BOOST_CHECK(d.requestHeaders);
    BOOST_CHECK(!d.disconnect);
    BOOST_CHECK_EQUAL(d.backoffSec, 60);

    // inside the first backoff window: no second request
    pp2p::StallWatchDecision d2 = pp2p::DecideStallAction(now + 10, stale, now, 1, 8200100, 8200000);
    BOOST_CHECK(!d2.requestHeaders);

    // after the window: retry with the doubled backoff
    pp2p::StallWatchDecision d3 = pp2p::DecideStallAction(now + 61, stale, now, 1, 8200100, 8200000);
    BOOST_CHECK(d3.requestHeaders);
    BOOST_CHECK_EQUAL(d3.backoffSec, 120);
}

BOOST_AUTO_TEST_CASE(retries_are_bounded_then_the_peer_is_dropped)
{
    const int64_t now = 5000;
    const int64_t stale = now - 10 * pp2p::STALL_PROGRESS_SEC;
    const pp2p::StallWatchDecision d =
        pp2p::DecideStallAction(now, stale, now - 1000, pp2p::STALL_MAX_ATTEMPTS, 8200100, 8200000);
    BOOST_CHECK(d.disconnect);
    BOOST_CHECK(!d.requestHeaders);
}

BOOST_AUTO_TEST_CASE(progress_age_boundary_is_exact)
{
    const int64_t now = 2000;
    // age == STALL_PROGRESS_SEC - 1 -> still healthy
    BOOST_CHECK(!pp2p::DecideStallAction(now, now - (pp2p::STALL_PROGRESS_SEC - 1), 0, 0, 8200100, 8200000)
                     .requestHeaders);
    // age == STALL_PROGRESS_SEC -> stalled
    BOOST_CHECK(pp2p::DecideStallAction(now, now - pp2p::STALL_PROGRESS_SEC, 0, 0, 8200100, 8200000)
                    .requestHeaders);
}

BOOST_AUTO_TEST_SUITE_END()
