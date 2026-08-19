// Copyright (c) 2026 The Innova developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <vector>

#include <boost/test/unit_test.hpp>

#include "getblocksservedinvzero.h"
#include "ibdmetrics.h"
#include "main.h"
#include "net.h"
#include "protocol.h"
#include "util.h"
#include "version.h"

namespace {

static const int64_t USEC_PER_SEC = 1000000;
static const int64_t NOW = 1000000LL * 1000000;

// Distinct chain-tip hashes: TIP_A is "current", TIP_ADV an advance, TIP_REORG
// a fork to an unrelated tip.
static const uint256 TIP_A = uint256(5000);
static const uint256 TIP_ADV = uint256(5001);
static const uint256 TIP_REORG = uint256(4000);

static uint256 H(uint64_t value) { return uint256(value); }

static CAddress TestPeerAddress(unsigned int nPeer)
{
    struct in_addr addr;
    addr.s_addr = 0x0100007f + (nPeer << 24);
    return CAddress(CService(addr, GetDefaultPort()));
}

static CGetBlocksRequestInfo MakeRequest(const uint256& hashTip,
                                         int nResolvedHeight,
                                         unsigned int nPredictedCount)
{
    CGetBlocksRequestInfo req;
    req.hashLocatorTip = H(nResolvedHeight < 0 ? 1 : (uint64_t)nResolvedHeight);
    req.nResolvedHeight = nResolvedHeight;
    req.hashStop = uint256(0);
    req.nStopHeight = -1;
    req.hashChainTip = hashTip;
    req.hashPredictedFirst = uint256(0);
    req.hashPredictedLast = uint256(0);
    req.nPredictedResponseCount = nPredictedCount;
    req.nRequestTimeMillis = 0;
    return req;
}

// Mirror of the production getblocks reply path: each pushed inv item is
// recorded into the current window, then the served-window footprint and
// response chain tip are recorded.
static void ServeReply(CNode::CGetBlocksServedInvState& state,
                       const CGetBlocksRequestInfo& request,
                       unsigned int nItems, int nStartHeight, int64_t nNowUs)
{
    CGetBlocksResponseInfo response;
    for (unsigned int i = 0; i < nItems; ++i)
    {
        response.Add(H(100000 + i), nStartHeight + (int)i);
        GetBlocksServedInvRecordItem(state, nNowUs);
    }
    GetBlocksServedInvRecordResponse(state, response, request.hashChainTip,
                                     nNowUs);
}

// Mirror of the production handler side effects for one getblocks request:
// returns true when the reply was suppressed (nothing written).
static bool ProcessRequest(CNode::CGetBlocksServedInvState& state,
                           const CGetBlocksRequestInfo& request,
                           bool fStrictInbound, int64_t nNowUs)
{
    GetBlocksServedInvDecision d = GetBlocksServedInvEvaluate(
        state, request, fStrictInbound, nNowUs,
        request.nPredictedResponseCount);
    if (d.fSuppress)
    {
        state.nGbZeroConsumeStreak++;
        state.fGbSuppressInv = true;
        state.fGbPriorZeroConsume = true;
        ibdmetrics::Get().getblocks_suppressed_inv_replies.fetch_add(
            1, std::memory_order_relaxed);
        ibdmetrics::Get().getblocks_suppressed_inv_items.fetch_add(
            d.nItemsAvoided, std::memory_order_relaxed);
        ibdmetrics::Get().getblocks_suppressed_inv_bytes_avoided.fetch_add(
            d.nBytesAvoided, std::memory_order_relaxed);
        return true;
    }
    if (d.fQualify)
        state.nGbZeroConsumeStreak++;
    return false;
}

// A getdata for block nHash at nHeight, as recorded in the getdata-serving
// path.
static bool Consume(CNode::CGetBlocksServedInvState& state, uint64_t nHash,
                    int nHeight, int64_t nNowUs)
{
    return GetBlocksServedInvNoteGetData(state, H(nHash), nHeight, nNowUs);
}

static bool IsStrictInbound(const CNode& peer)
{
    return peer.fInbound && !peer.fWhitelisted;
}

} // namespace

BOOST_AUTO_TEST_SUITE(getblocksservedinvzero_tests)

// T1: flag OFF => byte-for-byte legacy: nothing is ever suppressed and the
//     per-peer state is never mutated.
BOOST_AUTO_TEST_CASE(t1_flag_off_legacy)
{
    InitGetBlocksServedInvZero(false);
    CNode peer(INVALID_SOCKET, TestPeerAddress(1), "gz-t1", true);
    CNode::CGetBlocksServedInvState& st = peer.getBlocksServedInv;
    CGetBlocksRequestInfo req = MakeRequest(TIP_A, 100, 1000);

    ServeReply(st, req, 1000, 101, NOW);
    ServeReply(st, req, 1000, 101, NOW + USEC_PER_SEC);

    // Ten repeats well past grace: never suppressed.
    for (int i = 0; i < 10; ++i)
        BOOST_CHECK(!ProcessRequest(st, req, IsStrictInbound(peer),
                                    NOW + (70 + i) * USEC_PER_SEC));

    // State untouched by the disabled mechanism.
    BOOST_CHECK_EQUAL(st.nGbZeroConsumeStreak, 0u);
    BOOST_CHECK(!st.fGbSuppressInv);
    BOOST_CHECK(!st.fGbPriorZeroConsume);
    BOOST_CHECK_EQUAL(st.nGbServedInvItems, 0ULL);
    BOOST_CHECK_EQUAL(st.nGbServedInvBytes, 0ULL);
    BOOST_CHECK(st.vGbServedWindows.empty());
}

// T2: pure zero-consumption repeat: served for the first two qualifying
//     requests, suppressed on the third (INITIAL_STREAK = 3).
BOOST_AUTO_TEST_CASE(t2_zero_consumption_repeat_suppressed)
{
    InitGetBlocksServedInvZero(true);
    CNode peer(INVALID_SOCKET, TestPeerAddress(2), "gz-t2", true);
    CNode::CGetBlocksServedInvState& st = peer.getBlocksServedInv;
    CGetBlocksRequestInfo req = MakeRequest(TIP_A, 100, 1000);

    const int64_t nSuppressBefore =
        ibdmetrics::Get().getblocks_suppressed_inv_replies.load(
            std::memory_order_relaxed);

    ServeReply(st, req, 1000, 101, NOW);
    ServeReply(st, req, 1000, 101, NOW + USEC_PER_SEC);
    BOOST_CHECK_EQUAL(st.nGbServedInvItems, 2000ULL);
    BOOST_CHECK_EQUAL(st.nGbServedInvBytes, 2000ULL * 36);

    BOOST_CHECK(!ProcessRequest(st, req, IsStrictInbound(peer),
                                NOW + 70 * USEC_PER_SEC));
    BOOST_CHECK_EQUAL(st.nGbZeroConsumeStreak, 1u);
    BOOST_CHECK(!ProcessRequest(st, req, IsStrictInbound(peer),
                                NOW + 71 * USEC_PER_SEC));
    BOOST_CHECK_EQUAL(st.nGbZeroConsumeStreak, 2u);

    BOOST_CHECK(ProcessRequest(st, req, IsStrictInbound(peer),
                               NOW + 72 * USEC_PER_SEC));
    BOOST_CHECK(st.fGbSuppressInv);
    BOOST_CHECK(st.fGbPriorZeroConsume);
    BOOST_CHECK_EQUAL(st.nGbZeroConsumeStreak, 3u);
    BOOST_CHECK_EQUAL(
        ibdmetrics::Get().getblocks_suppressed_inv_replies.load(
            std::memory_order_relaxed),
        nSuppressBefore + 1);
    BOOST_CHECK_EQUAL(
        ibdmetrics::Get().getblocks_suppressed_inv_items.load(
            std::memory_order_relaxed) -
            nSuppressBefore,
        1000ULL);
    InitGetBlocksServedInvZero(false);
}

// T3: identical (locator, hashStop, tip) requests with zero consumption are
//     suppressed once the streak threshold is met.
BOOST_AUTO_TEST_CASE(t3_identical_repeat_suppressed)
{
    InitGetBlocksServedInvZero(true);
    CNode peer(INVALID_SOCKET, TestPeerAddress(3), "gz-t3", true);
    CNode::CGetBlocksServedInvState& st = peer.getBlocksServedInv;
    CGetBlocksRequestInfo req = MakeRequest(TIP_A, 100, 1000);

    ServeReply(st, req, 1000, 101, NOW);
    ServeReply(st, req, 1000, 101, NOW + USEC_PER_SEC);

    // Two served, third identical request suppressed.
    BOOST_CHECK(!ProcessRequest(st, req, IsStrictInbound(peer),
                                NOW + 70 * USEC_PER_SEC));
    BOOST_CHECK(!ProcessRequest(st, req, IsStrictInbound(peer),
                                NOW + 71 * USEC_PER_SEC));
    BOOST_CHECK(ProcessRequest(st, req, IsStrictInbound(peer),
                               NOW + 72 * USEC_PER_SEC));
    InitGetBlocksServedInvZero(false);
}

// T4: cyclic A -> B -> A over the served range: all qualify (overlap), third
//     qualifying request suppressed.
BOOST_AUTO_TEST_CASE(t4_cyclic_repeat_suppressed)
{
    InitGetBlocksServedInvZero(true);
    CNode peer(INVALID_SOCKET, TestPeerAddress(4), "gz-t4", true);
    CNode::CGetBlocksServedInvState& st = peer.getBlocksServedInv;

    // Served window covers heights 101..1100 (2000 items over two replies).
    CGetBlocksRequestInfo reqA = MakeRequest(TIP_A, 100, 1000);
    ServeReply(st, reqA, 1000, 101, NOW);
    ServeReply(st, reqA, 1000, 101, NOW + USEC_PER_SEC);

    // B and C predict ranges that overlap the served window (201..1000 and
    // 151..1050).
    CGetBlocksRequestInfo reqB = MakeRequest(TIP_A, 200, 800);
    CGetBlocksRequestInfo reqC = MakeRequest(TIP_A, 150, 900);

    BOOST_CHECK(!ProcessRequest(st, reqB, IsStrictInbound(peer),
                                NOW + 70 * USEC_PER_SEC));
    BOOST_CHECK(!ProcessRequest(st, reqC, IsStrictInbound(peer),
                                NOW + 71 * USEC_PER_SEC));
    BOOST_CHECK(ProcessRequest(st, reqA, IsStrictInbound(peer),
                               NOW + 72 * USEC_PER_SEC));
    InitGetBlocksServedInvZero(false);
}

// T5: overlapping (not identical) served ranges and requests still collapse
//     the zero-consumption amplification.
BOOST_TEST_DECORATOR(*boost::unit_test::depends_on("getblocksservedinvzero_tests/t4_cyclic_repeat_suppressed"))
BOOST_AUTO_TEST_CASE(t5_overlapping_ranges_suppressed)
{
    InitGetBlocksServedInvZero(true);
    CNode peer(INVALID_SOCKET, TestPeerAddress(5), "gz-t5", true);
    CNode::CGetBlocksServedInvState& st = peer.getBlocksServedInv;

    // Two overlapping served windows: 101..1100 and 201..1200.
    CGetBlocksRequestInfo reqA = MakeRequest(TIP_A, 100, 1000);
    ServeReply(st, reqA, 1000, 101, NOW);
    CGetBlocksRequestInfo reqB = MakeRequest(TIP_A, 200, 1000);
    ServeReply(st, reqB, 1000, 201, NOW + USEC_PER_SEC);

    // A third request overlapping both (151..1050): qualifies.
    CGetBlocksRequestInfo reqC = MakeRequest(TIP_A, 150, 900);
    BOOST_CHECK(!ProcessRequest(st, reqC, IsStrictInbound(peer),
                                NOW + 70 * USEC_PER_SEC));
    BOOST_CHECK(!ProcessRequest(st, reqC, IsStrictInbound(peer),
                                NOW + 71 * USEC_PER_SEC));
    BOOST_CHECK(ProcessRequest(st, reqC, IsStrictInbound(peer),
                               NOW + 72 * USEC_PER_SEC));
    InitGetBlocksServedInvZero(false);
}

// T6: a legitimately advancing locator that CONSUMES is never suppressed
//     (consumption is the binding signal; a new range is served anyway).
BOOST_TEST_DECORATOR(*boost::unit_test::depends_on("getblocksservedinvzero_tests/t5_overlapping_ranges_suppressed"))
BOOST_AUTO_TEST_CASE(t6_advancing_locator_consuming_not_suppressed)
{
    InitGetBlocksServedInvZero(true);
    CNode peer(INVALID_SOCKET, TestPeerAddress(6), "gz-t6", true);
    CNode::CGetBlocksServedInvState& st = peer.getBlocksServedInv;

    CGetBlocksRequestInfo reqA = MakeRequest(TIP_A, 100, 1000);
    ServeReply(st, reqA, 1000, 101, NOW);
    ServeReply(st, reqA, 1000, 101, NOW + USEC_PER_SEC);

    // Legitimate consumption of part of the served window.
    BOOST_CHECK(!Consume(st, 100000, 101, NOW + 5 * USEC_PER_SEC));
    BOOST_CHECK_EQUAL(st.nGbGetDataMatches, 1ULL);

    // Five repeats on the same range: consumption keeps them served.
    for (int i = 0; i < 5; ++i)
        BOOST_CHECK(!ProcessRequest(st, reqA, IsStrictInbound(peer),
                                    NOW + (70 + i) * USEC_PER_SEC));

    // Advancing to a genuinely new range is served (disarm).
    CGetBlocksRequestInfo reqNew = MakeRequest(TIP_A, 3000, 1000);
    BOOST_CHECK(!ProcessRequest(st, reqNew, IsStrictInbound(peer),
                                NOW + 80 * USEC_PER_SEC));
    InitGetBlocksServedInvZero(false);
}

// T7: low-but-nonzero consumption (0.75% = 15 of 2000) is NEVER suppressed.
BOOST_TEST_DECORATOR(*boost::unit_test::depends_on("getblocksservedinvzero_tests/t6_advancing_locator_consuming_not_suppressed"))
BOOST_AUTO_TEST_CASE(t7_low_nonzero_not_suppressed)
{
    InitGetBlocksServedInvZero(true);
    CNode peer(INVALID_SOCKET, TestPeerAddress(7), "gz-t7", true);
    CNode::CGetBlocksServedInvState& st = peer.getBlocksServedInv;

    const int64_t nLowBefore =
        ibdmetrics::Get().getblocks_low_nonzero_windows.load(
            std::memory_order_relaxed);

    CGetBlocksRequestInfo req = MakeRequest(TIP_A, 100, 1000);
    ServeReply(st, req, 1000, 101, NOW);
    ServeReply(st, req, 1000, 101, NOW + USEC_PER_SEC);

    // 0.75% sparse consumption across the window (15 of 2000 served blocks).
    for (int i = 0; i < 15; ++i)
        Consume(st, 100000 + i * 10, 101 + i * 10, NOW + 5 * USEC_PER_SEC);
    BOOST_CHECK_EQUAL(st.nGbGetDataMatches, 15ULL);

    // Repeats are never suppressed despite the streak criteria otherwise
    // being met for 10 rounds.
    for (int i = 0; i < 10; ++i)
        BOOST_CHECK(!ProcessRequest(st, req, IsStrictInbound(peer),
                                    NOW + (70 + i) * USEC_PER_SEC));
    BOOST_CHECK(!st.fGbSuppressInv);

    // Each qualifying-but-nonzero window is recorded as low_nonzero.
    BOOST_CHECK(
        ibdmetrics::Get().getblocks_low_nonzero_windows.load(
            std::memory_order_relaxed) >= nLowBefore + 10);
    InitGetBlocksServedInvZero(false);
}

// T8: malicious 2000INV -> 1getdata trickle: NOT suppressed (nonzero
//     consumption) but ALSO not fully-history-reset: prior-history is kept and
//     the reentry streak applies after the window rolls.
BOOST_TEST_DECORATOR(*boost::unit_test::depends_on("getblocksservedinvzero_tests/t7_low_nonzero_not_suppressed"))
BOOST_AUTO_TEST_CASE(t8_2000inv_1getdata_trickle)
{
    InitGetBlocksServedInvZero(true);
    CNode peer(INVALID_SOCKET, TestPeerAddress(8), "gz-t8", true);
    CNode::CGetBlocksServedInvState& st = peer.getBlocksServedInv;

    CGetBlocksRequestInfo req = MakeRequest(TIP_A, 100, 1000);
    ServeReply(st, req, 1000, 101, NOW);
    ServeReply(st, req, 1000, 101, NOW + USEC_PER_SEC);

    // Arm suppression (zero consumption, streak 3).
    BOOST_CHECK(!ProcessRequest(st, req, IsStrictInbound(peer),
                                NOW + 70 * USEC_PER_SEC));
    BOOST_CHECK(!ProcessRequest(st, req, IsStrictInbound(peer),
                                NOW + 71 * USEC_PER_SEC));
    BOOST_CHECK(ProcessRequest(st, req, IsStrictInbound(peer),
                               NOW + 72 * USEC_PER_SEC));
    BOOST_CHECK(st.fGbSuppressInv);

    // The 1-getdata blip is a safety release.
    BOOST_CHECK(Consume(st, 100000, 101, NOW + 73 * USEC_PER_SEC));
    BOOST_CHECK(!st.fGbSuppressInv);
    BOOST_CHECK(st.fGbPriorZeroConsume);

    // The trickle is NOT suppressed (nonzero consumption) ...
    BOOST_CHECK(!ProcessRequest(st, req, IsStrictInbound(peer),
                                NOW + 74 * USEC_PER_SEC));
    // ... but history is NOT erased.
    BOOST_CHECK(st.fGbPriorZeroConsume);
    InitGetBlocksServedInvZero(false);
}

// T9: delayed getdata after suppression armed is a safety release, so a
//     legitimate slow consumer that only requested after the gate armed still
//     resumes syncing normally.
BOOST_TEST_DECORATOR(*boost::unit_test::depends_on("getblocksservedinvzero_tests/t8_2000inv_1getdata_trickle"))
BOOST_AUTO_TEST_CASE(t9_delayed_getdata_within_grace)
{
    InitGetBlocksServedInvZero(true);
    CNode peer(INVALID_SOCKET, TestPeerAddress(9), "gz-t9", true);
    CNode::CGetBlocksServedInvState& st = peer.getBlocksServedInv;

    CGetBlocksRequestInfo req = MakeRequest(TIP_A, 100, 1000);
    ServeReply(st, req, 1000, 101, NOW);
    ServeReply(st, req, 1000, 101, NOW + USEC_PER_SEC);
    BOOST_CHECK(!ProcessRequest(st, req, IsStrictInbound(peer),
                                NOW + 70 * USEC_PER_SEC));
    BOOST_CHECK(!ProcessRequest(st, req, IsStrictInbound(peer),
                                NOW + 71 * USEC_PER_SEC));
    BOOST_CHECK(ProcessRequest(st, req, IsStrictInbound(peer),
                               NOW + 72 * USEC_PER_SEC));
    BOOST_CHECK(st.fGbSuppressInv);

    // The getdata lands shortly after the gate armed: safety release re-arms
    // serving so legitimate sync continues; the window/history is not erased.
    BOOST_CHECK(Consume(st, 100000, 101, NOW + 75 * USEC_PER_SEC));
    BOOST_CHECK(!st.fGbSuppressInv);
    BOOST_CHECK_EQUAL(st.nGbGetDataMatches, 1ULL);
    BOOST_CHECK(st.fGbPriorZeroConsume);
    BOOST_CHECK_EQUAL(st.nGbServedInvItems, 2000ULL);
    InitGetBlocksServedInvZero(false);
}

// T10: partial getdata (a single block of a 2000-item window) keeps the peer
//      unsuppressed.
BOOST_TEST_DECORATOR(*boost::unit_test::depends_on("getblocksservedinvzero_tests/t9_delayed_getdata_within_grace"))
BOOST_AUTO_TEST_CASE(t10_partial_getdata)
{
    InitGetBlocksServedInvZero(true);
    CNode peer(INVALID_SOCKET, TestPeerAddress(10), "gz-t10", true);
    CNode::CGetBlocksServedInvState& st = peer.getBlocksServedInv;

    CGetBlocksRequestInfo req = MakeRequest(TIP_A, 100, 1000);
    ServeReply(st, req, 1000, 101, NOW);
    ServeReply(st, req, 1000, 101, NOW + USEC_PER_SEC);

    // Consume just one block, by height (middle of the window).
    BOOST_CHECK(!Consume(st, 0, 600, NOW + 5 * USEC_PER_SEC));
    BOOST_CHECK_EQUAL(st.nGbGetDataMatches, 1ULL);

    for (int i = 0; i < 6; ++i)
        BOOST_CHECK(!ProcessRequest(st, req, IsStrictInbound(peer),
                                    NOW + (70 + i) * USEC_PER_SEC));
    BOOST_CHECK(!st.fGbSuppressInv);
    InitGetBlocksServedInvZero(false);
}

// T11: multiple batch getdata => multiple matches, never suppressed.
BOOST_TEST_DECORATOR(*boost::unit_test::depends_on("getblocksservedinvzero_tests/t10_partial_getdata"))
BOOST_AUTO_TEST_CASE(t11_multiple_batch_getdata)
{
    InitGetBlocksServedInvZero(true);
    CNode peer(INVALID_SOCKET, TestPeerAddress(11), "gz-t11", true);
    CNode::CGetBlocksServedInvState& st = peer.getBlocksServedInv;

    CGetBlocksRequestInfo req = MakeRequest(TIP_A, 100, 1000);
    ServeReply(st, req, 1000, 101, NOW);
    ServeReply(st, req, 1000, 101, NOW + USEC_PER_SEC);

    for (int i = 0; i < 3; ++i)
        Consume(st, 100000 + i * 10, 101 + i * 10, NOW + 5 * USEC_PER_SEC);
    BOOST_CHECK_EQUAL(st.nGbGetDataMatches, 3ULL);

    for (int i = 0; i < 6; ++i)
        BOOST_CHECK(!ProcessRequest(st, req, IsStrictInbound(peer),
                                    NOW + (70 + i) * USEC_PER_SEC));
    BOOST_CHECK(!st.fGbSuppressInv);
    InitGetBlocksServedInvZero(false);
}

// T12: active tip advance disarms suppression; the changed-tip request is
//      served.
BOOST_TEST_DECORATOR(*boost::unit_test::depends_on("getblocksservedinvzero_tests/t11_multiple_batch_getdata"))
BOOST_AUTO_TEST_CASE(t12_active_tip_advance_disarmed)
{
    InitGetBlocksServedInvZero(true);
    CNode peer(INVALID_SOCKET, TestPeerAddress(12), "gz-t12", true);
    CNode::CGetBlocksServedInvState& st = peer.getBlocksServedInv;

    CGetBlocksRequestInfo req = MakeRequest(TIP_A, 100, 1000);
    ServeReply(st, req, 1000, 101, NOW);
    ServeReply(st, req, 1000, 101, NOW + USEC_PER_SEC);
    BOOST_CHECK(!ProcessRequest(st, req, IsStrictInbound(peer),
                                NOW + 70 * USEC_PER_SEC));
    BOOST_CHECK(!ProcessRequest(st, req, IsStrictInbound(peer),
                                NOW + 71 * USEC_PER_SEC));
    BOOST_CHECK(ProcessRequest(st, req, IsStrictInbound(peer),
                               NOW + 72 * USEC_PER_SEC));
    BOOST_CHECK(st.fGbSuppressInv);

    // Chain advances: the request is served and suppression disarmed.
    CGetBlocksRequestInfo reqAdv = MakeRequest(TIP_ADV, 100, 1000);
    BOOST_CHECK(!ProcessRequest(st, reqAdv, IsStrictInbound(peer),
                                NOW + 73 * USEC_PER_SEC));
    BOOST_CHECK(!st.fGbSuppressInv);
    BOOST_CHECK_EQUAL(st.nGbZeroConsumeStreak, 0u);
    InitGetBlocksServedInvZero(false);
}

// T13: reorg/fork to an unrelated tip disarms suppression.
BOOST_TEST_DECORATOR(*boost::unit_test::depends_on("getblocksservedinvzero_tests/t12_active_tip_advance_disarmed"))
BOOST_AUTO_TEST_CASE(t13_reorg_fork_disarmed)
{
    InitGetBlocksServedInvZero(true);
    CNode peer(INVALID_SOCKET, TestPeerAddress(13), "gz-t13", true);
    CNode::CGetBlocksServedInvState& st = peer.getBlocksServedInv;

    CGetBlocksRequestInfo req = MakeRequest(TIP_A, 100, 1000);
    ServeReply(st, req, 1000, 101, NOW);
    ServeReply(st, req, 1000, 101, NOW + USEC_PER_SEC);
    BOOST_CHECK(!ProcessRequest(st, req, IsStrictInbound(peer),
                                NOW + 70 * USEC_PER_SEC));
    BOOST_CHECK(!ProcessRequest(st, req, IsStrictInbound(peer),
                                NOW + 71 * USEC_PER_SEC));
    BOOST_CHECK(ProcessRequest(st, req, IsStrictInbound(peer),
                               NOW + 72 * USEC_PER_SEC));
    BOOST_CHECK(st.fGbSuppressInv);

    CGetBlocksRequestInfo reqReorg = MakeRequest(TIP_REORG, 100, 1000);
    BOOST_CHECK(!ProcessRequest(st, reqReorg, IsStrictInbound(peer),
                                NOW + 73 * USEC_PER_SEC));
    BOOST_CHECK(!st.fGbSuppressInv);
    InitGetBlocksServedInvZero(false);
}

// T14: empty / below-MIN_ITEMS replies are never suppressed.
BOOST_TEST_DECORATOR(*boost::unit_test::depends_on("getblocksservedinvzero_tests/t13_reorg_fork_disarmed"))
BOOST_AUTO_TEST_CASE(t14_empty_inv_never_suppressed)
{
    InitGetBlocksServedInvZero(true);
    CNode peer(INVALID_SOCKET, TestPeerAddress(14), "gz-t14", true);
    CNode::CGetBlocksServedInvState& st = peer.getBlocksServedInv;

    // Tiny window: well below MIN_ITEMS.
    CGetBlocksRequestInfo req = MakeRequest(TIP_A, 100, 10);
    ServeReply(st, req, 10, 101, NOW);
    for (int i = 0; i < 10; ++i)
        BOOST_CHECK(!ProcessRequest(st, req, IsStrictInbound(peer),
                                    NOW + (70 + i) * USEC_PER_SEC));

    // Empty replies (peer caught up) are also never suppressed.
    CNode::CGetBlocksServedInvState st2;
    ServeReply(st2, req, 0, -1, NOW);
    for (int i = 0; i < 5; ++i)
        BOOST_CHECK(!ProcessRequest(st2, req, IsStrictInbound(peer),
                                    NOW + (70 + i) * USEC_PER_SEC));
    InitGetBlocksServedInvZero(false);
}

// T15: a hashStop change that genuinely changes the predicted window is served
//      (no overlap with the served window).
BOOST_TEST_DECORATOR(*boost::unit_test::depends_on("getblocksservedinvzero_tests/t14_empty_inv_never_suppressed"))
BOOST_AUTO_TEST_CASE(t15_hashstop_change_served)
{
    InitGetBlocksServedInvZero(true);
    CNode peer(INVALID_SOCKET, TestPeerAddress(15), "gz-t15", true);
    CNode::CGetBlocksServedInvState& st = peer.getBlocksServedInv;

    CGetBlocksRequestInfo req = MakeRequest(TIP_A, 100, 1000);
    ServeReply(st, req, 1000, 101, NOW);
    ServeReply(st, req, 1000, 101, NOW + USEC_PER_SEC);
    BOOST_CHECK(!ProcessRequest(st, req, IsStrictInbound(peer),
                                NOW + 70 * USEC_PER_SEC));
    BOOST_CHECK(!ProcessRequest(st, req, IsStrictInbound(peer),
                                NOW + 71 * USEC_PER_SEC));

    // hashStop lowered so the served/predicted range no longer overlaps:
    // served (disarmed), not suppressed.
    CGetBlocksRequestInfo reqStop = MakeRequest(TIP_A, 100, 0);
    BOOST_CHECK(!ProcessRequest(st, reqStop, IsStrictInbound(peer),
                                NOW + 72 * USEC_PER_SEC));
    BOOST_CHECK(!st.fGbSuppressInv);
    InitGetBlocksServedInvZero(false);
}

// T16: reconnect -> a new CNode does NOT inherit served-inv state.
BOOST_TEST_DECORATOR(*boost::unit_test::depends_on("getblocksservedinvzero_tests/t15_hashstop_change_served"))
BOOST_AUTO_TEST_CASE(t16_reconnect_fresh_state)
{
    InitGetBlocksServedInvZero(true);
    CNode peerOld(INVALID_SOCKET, TestPeerAddress(16), "gz-t16-old", true);
    CNode::CGetBlocksServedInvState& stOld = peerOld.getBlocksServedInv;
    CGetBlocksRequestInfo req = MakeRequest(TIP_A, 100, 1000);
    ServeReply(stOld, req, 1000, 101, NOW);
    ServeReply(stOld, req, 1000, 101, NOW + USEC_PER_SEC);
    BOOST_CHECK(!ProcessRequest(stOld, req, IsStrictInbound(peerOld),
                                NOW + 70 * USEC_PER_SEC));
    BOOST_CHECK(!ProcessRequest(stOld, req, IsStrictInbound(peerOld),
                                NOW + 71 * USEC_PER_SEC));
    BOOST_CHECK(ProcessRequest(stOld, req, IsStrictInbound(peerOld),
                               NOW + 72 * USEC_PER_SEC));
    BOOST_CHECK(stOld.fGbSuppressInv);

    // Reconnect: brand new CNode, empty state, immediately served.
    CNode peerNew(INVALID_SOCKET, TestPeerAddress(16), "gz-t16-new", true);
    CNode::CGetBlocksServedInvState& stNew = peerNew.getBlocksServedInv;
    BOOST_CHECK(!stNew.fGbHaveWindow);
    BOOST_CHECK_EQUAL(stNew.nGbServedInvItems, 0ULL);
    BOOST_CHECK_EQUAL(stNew.nGbZeroConsumeStreak, 0u);
    BOOST_CHECK(stNew.vGbServedWindows.empty());
    BOOST_CHECK(!ProcessRequest(stNew, req, IsStrictInbound(peerNew),
                                NOW + 73 * USEC_PER_SEC));
    InitGetBlocksServedInvZero(false);
}

// T17: two peers with identical traffic are independent: suppressing one does
//      not affect the other.
BOOST_TEST_DECORATOR(*boost::unit_test::depends_on("getblocksservedinvzero_tests/t16_reconnect_fresh_state"))
BOOST_AUTO_TEST_CASE(t17_two_peers_independent)
{
    InitGetBlocksServedInvZero(true);
    CNode peerA(INVALID_SOCKET, TestPeerAddress(17), "gz-t17-a", true);
    CNode peerB(INVALID_SOCKET, TestPeerAddress(18), "gz-t17-b", true);
    CNode::CGetBlocksServedInvState& stA = peerA.getBlocksServedInv;
    CNode::CGetBlocksServedInvState& stB = peerB.getBlocksServedInv;
    CGetBlocksRequestInfo req = MakeRequest(TIP_A, 100, 1000);

    ServeReply(stA, req, 1000, 101, NOW);
    ServeReply(stA, req, 1000, 101, NOW + USEC_PER_SEC);
    // Peer B has its own (empty) state.
    BOOST_CHECK(!stB.fGbHaveWindow);

    // A is suppressed at streak 3 ...
    BOOST_CHECK(!ProcessRequest(stA, req, IsStrictInbound(peerA),
                                NOW + 70 * USEC_PER_SEC));
    BOOST_CHECK(!ProcessRequest(stA, req, IsStrictInbound(peerA),
                                NOW + 71 * USEC_PER_SEC));
    BOOST_CHECK(ProcessRequest(stA, req, IsStrictInbound(peerA),
                               NOW + 72 * USEC_PER_SEC));
    // ... B remains served.
    BOOST_CHECK(!ProcessRequest(stB, req, IsStrictInbound(peerB),
                                NOW + 72 * USEC_PER_SEC));
    BOOST_CHECK_EQUAL(stB.nGbZeroConsumeStreak, 0u);
    BOOST_CHECK(stB.vGbServedWindows.empty());
    InitGetBlocksServedInvZero(false);
}

// T18: the recently-served set is bounded by the cap and purges expired
//      entries.
BOOST_TEST_DECORATOR(*boost::unit_test::depends_on("getblocksservedinvzero_tests/t17_two_peers_independent"))
BOOST_AUTO_TEST_CASE(t18_recent_window_cap_and_expiry)
{
    InitGetBlocksServedInvZero(true);
    CNode peer(INVALID_SOCKET, TestPeerAddress(19), "gz-t18", true);
    CNode::CGetBlocksServedInvState& st = peer.getBlocksServedInv;

    // CAP: 12 windows within expiry -> bounded at the cap.
    int64_t t = NOW;
    for (int i = 0; i < 12; ++i)
    {
        CGetBlocksRequestInfo req = MakeRequest(TIP_A, 100 + i * 100, 50);
        ServeReply(st, req, 50, 101 + i * 100, t);
        BOOST_CHECK(st.vGbServedWindows.size() <=
                    GETBLOCKS_SERVED_INV_RECENT_WINDOW_CAP);
        t += 10 * USEC_PER_SEC;
    }
    BOOST_CHECK_EQUAL(st.vGbServedWindows.size(),
                      GETBLOCKS_SERVED_INV_RECENT_WINDOW_CAP);

    // EXPIRY: a serve well past W purges entries whose age >= W; the oldest
    // retained entry is now within the last (W - 10s).
    CGetBlocksRequestInfo reqLast = MakeRequest(TIP_A, 2000, 50);
    ServeReply(st, reqLast, 50, 2001, NOW + 121 * USEC_PER_SEC);
    for (size_t i = 0; i < st.vGbServedWindows.size(); ++i)
        BOOST_CHECK(st.vGbServedWindows[i].nServedUs >= NOW + 50 * USEC_PER_SEC);

    // Expired windows no longer match consumption.
    CNode::CGetBlocksServedInvState st2;
    CGetBlocksRequestInfo req0 = MakeRequest(TIP_A, 100, 1000);
    ServeReply(st2, req0, 1000, 101, NOW);
    BOOST_CHECK(GetBlocksServedInvWindowOverlaps(st2, req0, NOW + 10 * USEC_PER_SEC));
    BOOST_CHECK(!GetBlocksServedInvWindowOverlaps(
        st2, req0, NOW + (GETBLOCKS_SERVED_INV_WINDOW_EXPIRY_S + 1) * USEC_PER_SEC));
    InitGetBlocksServedInvZero(false);
}

// T19: release latch re-arms on consumption: a getdata matching the served
//      window while suppression is armed is a safety release.
BOOST_TEST_DECORATOR(*boost::unit_test::depends_on("getblocksservedinvzero_tests/t18_recent_window_cap_and_expiry"))
BOOST_AUTO_TEST_CASE(t19_release_latch_reams)
{
    InitGetBlocksServedInvZero(true);
    CNode peer(INVALID_SOCKET, TestPeerAddress(20), "gz-t19", true);
    CNode::CGetBlocksServedInvState& st = peer.getBlocksServedInv;

    CGetBlocksRequestInfo req = MakeRequest(TIP_A, 100, 1000);
    ServeReply(st, req, 1000, 101, NOW);
    ServeReply(st, req, 1000, 101, NOW + USEC_PER_SEC);
    BOOST_CHECK(!ProcessRequest(st, req, IsStrictInbound(peer),
                                NOW + 70 * USEC_PER_SEC));
    BOOST_CHECK(!ProcessRequest(st, req, IsStrictInbound(peer),
                                NOW + 71 * USEC_PER_SEC));
    BOOST_CHECK(ProcessRequest(st, req, IsStrictInbound(peer),
                               NOW + 72 * USEC_PER_SEC));
    BOOST_CHECK(st.fGbSuppressInv);

    const int64_t nReleaseBefore =
        ibdmetrics::Get().getblocks_release_latch_events.load(
            std::memory_order_relaxed);
    BOOST_CHECK(Consume(st, 100000, 101, NOW + 73 * USEC_PER_SEC));
    BOOST_CHECK(!st.fGbSuppressInv);
    BOOST_CHECK_EQUAL(
        ibdmetrics::Get().getblocks_release_latch_events.load(
            std::memory_order_relaxed),
        nReleaseBefore + 1);
    InitGetBlocksServedInvZero(false);
}

// T20: the release latch does NOT erase history: after a window roll, a peer
//      with prior zero-consumption history is suppressed after just one
//      qualifying request (REENTRY_STREAK = 1), while a fresh peer still
//      needs INITIAL_STREAK.
BOOST_TEST_DECORATOR(*boost::unit_test::depends_on("getblocksservedinvzero_tests/t19_release_latch_reams"))
BOOST_AUTO_TEST_CASE(t20_release_latch_keeps_history_reentry)
{
    InitGetBlocksServedInvZero(true);
    CNode peer(INVALID_SOCKET, TestPeerAddress(21), "gz-t20", true);
    CNode::CGetBlocksServedInvState& st = peer.getBlocksServedInv;

    // Arm + release on range A.
    CGetBlocksRequestInfo reqA = MakeRequest(TIP_A, 100, 1000);
    ServeReply(st, reqA, 1000, 101, NOW);
    ServeReply(st, reqA, 1000, 101, NOW + USEC_PER_SEC);
    BOOST_CHECK(!ProcessRequest(st, reqA, IsStrictInbound(peer),
                                NOW + 70 * USEC_PER_SEC));
    BOOST_CHECK(!ProcessRequest(st, reqA, IsStrictInbound(peer),
                                NOW + 71 * USEC_PER_SEC));
    BOOST_CHECK(ProcessRequest(st, reqA, IsStrictInbound(peer),
                               NOW + 72 * USEC_PER_SEC));
    BOOST_CHECK(Consume(st, 100000, 101, NOW + 73 * USEC_PER_SEC));
    BOOST_CHECK(st.fGbPriorZeroConsume);

    // Move to a new range: disarm resets the window/streak, history stays.
    CGetBlocksRequestInfo reqNew = MakeRequest(TIP_A, 3000, 1000);
    BOOST_CHECK(!ProcessRequest(st, reqNew, IsStrictInbound(peer),
                                NOW + 80 * USEC_PER_SEC));
    BOOST_CHECK_EQUAL(st.nGbZeroConsumeStreak, 0u);
    BOOST_CHECK_EQUAL(st.nGbGetDataMatches, 0ULL);
    BOOST_CHECK(st.fGbPriorZeroConsume);

    // Serve the new range (2000 items).
    ServeReply(st, reqNew, 1000, 3001, NOW + 81 * USEC_PER_SEC);
    ServeReply(st, reqNew, 1000, 3001, NOW + 82 * USEC_PER_SEC);

    // ONE zero-consumption repeat after grace -> reentry streak = 1 fires.
    BOOST_CHECK(ProcessRequest(st, reqNew, IsStrictInbound(peer),
                               NOW + 150 * USEC_PER_SEC));

    // Control: a fresh peer with the same traffic is NOT suppressed after a
    // single repeat (it needs INITIAL_STREAK = 3).
    CNode peerFresh(INVALID_SOCKET, TestPeerAddress(22), "gz-t20-fresh", true);
    CNode::CGetBlocksServedInvState& stFresh = peerFresh.getBlocksServedInv;
    ServeReply(stFresh, reqNew, 1000, 3001, NOW + 81 * USEC_PER_SEC);
    ServeReply(stFresh, reqNew, 1000, 3001, NOW + 82 * USEC_PER_SEC);
    BOOST_CHECK(!ProcessRequest(stFresh, reqNew, IsStrictInbound(peerFresh),
                                NOW + 150 * USEC_PER_SEC));
    BOOST_CHECK(!stFresh.fGbSuppressInv);
    InitGetBlocksServedInvZero(false);
}

// T21: getdata -> block serving is unaffected while INV suppression is active:
//      matching getdata is still counted/released and non-matching getdata is
//      simply ignored (block serving itself is the caller's job, untouched).
BOOST_TEST_DECORATOR(*boost::unit_test::depends_on("getblocksservedinvzero_tests/t20_release_latch_keeps_history_reentry"))
BOOST_AUTO_TEST_CASE(t21_getdata_block_serving_unaffected)
{
    InitGetBlocksServedInvZero(true);
    CNode peer(INVALID_SOCKET, TestPeerAddress(23), "gz-t21", true);
    CNode::CGetBlocksServedInvState& st = peer.getBlocksServedInv;

    CGetBlocksRequestInfo req = MakeRequest(TIP_A, 100, 1000);
    ServeReply(st, req, 1000, 101, NOW);
    ServeReply(st, req, 1000, 101, NOW + USEC_PER_SEC);
    BOOST_CHECK(!ProcessRequest(st, req, IsStrictInbound(peer),
                                NOW + 70 * USEC_PER_SEC));
    BOOST_CHECK(!ProcessRequest(st, req, IsStrictInbound(peer),
                                NOW + 71 * USEC_PER_SEC));
    BOOST_CHECK(ProcessRequest(st, req, IsStrictInbound(peer),
                               NOW + 72 * USEC_PER_SEC));
    BOOST_CHECK(st.fGbSuppressInv);

    // A getdata for a block NOT in any served window: ignored entirely, no
    // match, no release, suppression stays armed.
    BOOST_CHECK(!Consume(st, 0xdead, 5000, NOW + 73 * USEC_PER_SEC));
    BOOST_CHECK(st.fGbSuppressInv);
    BOOST_CHECK_EQUAL(st.nGbGetDataMatches, 0ULL);

    // A getdata for a block in the served window while suppressing: still
    // counted and released (block serving itself is unaffected and proceeds
    // normally in ProcessGetData).
    BOOST_CHECK(Consume(st, 100000, 101, NOW + 74 * USEC_PER_SEC));
    BOOST_CHECK(!st.fGbSuppressInv);
    BOOST_CHECK_EQUAL(st.nGbGetDataMatches, 1ULL);
    InitGetBlocksServedInvZero(false);
}

// T22: policy thresholds are tunable: overriding INITIAL_STREAK/GRACE/MIN_ITEMS
//      changes the firing point.
BOOST_TEST_DECORATOR(*boost::unit_test::depends_on("getblocksservedinvzero_tests/t21_getdata_block_serving_unaffected"))
BOOST_AUTO_TEST_CASE(t22_tunable_policy)
{
    // Defaults match the named constants.
    const GetBlocksServedInvZeroConfig& def = GetBlocksServedInvConfig();
    BOOST_CHECK_EQUAL(def.nGraceS, GETBLOCKS_SERVED_INV_GRACE_S);
    BOOST_CHECK_EQUAL(def.nMinItems, GETBLOCKS_SERVED_INV_MIN_ITEMS);
    BOOST_CHECK_EQUAL(def.nInitialStreak, GETBLOCKS_SERVED_INV_INITIAL_STREAK);
    BOOST_CHECK_EQUAL(def.nReentryStreak, GETBLOCKS_SERVED_INV_REENTRY_STREAK);
    BOOST_CHECK_EQUAL(def.nRecentWindowCap, GETBLOCKS_SERVED_INV_RECENT_WINDOW_CAP);
    BOOST_CHECK_EQUAL(def.nWindowExpiryS, GETBLOCKS_SERVED_INV_WINDOW_EXPIRY_S);

    InitGetBlocksServedInvZero(true);
    GetBlocksServedInvZeroConfig cfg = def;
    cfg.nGraceS = 1;
    cfg.nMinItems = 500;
    cfg.nInitialStreak = 1;
    SetGetBlocksServedInvZeroConfig(cfg);

    CNode peer(INVALID_SOCKET, TestPeerAddress(24), "gz-t22", true);
    CNode::CGetBlocksServedInvState& st = peer.getBlocksServedInv;
    CGetBlocksRequestInfo req = MakeRequest(TIP_A, 100, 500);
    ServeReply(st, req, 500, 101, NOW);

    // 500 items, 1s grace, streak 1: a single zero-consumption repeat after
    // 2s is already suppressed.
    BOOST_CHECK(ProcessRequest(st, req, IsStrictInbound(peer),
                               NOW + 2 * USEC_PER_SEC));

    SetGetBlocksServedInvZeroConfig(GetBlocksServedInvZeroConfig());
    InitGetBlocksServedInvZero(false);
}

// T23: flag toggle semantics.
BOOST_TEST_DECORATOR(*boost::unit_test::depends_on("getblocksservedinvzero_tests/t22_tunable_policy"))
BOOST_AUTO_TEST_CASE(t23_flag_toggle)
{
    InitGetBlocksServedInvZero(false);
    BOOST_CHECK(!GetBlocksServedInvZeroEnabled());
    InitGetBlocksServedInvZero(true);
    BOOST_CHECK(GetBlocksServedInvZeroEnabled());
    InitGetBlocksServedInvZero(false);
    BOOST_CHECK(!GetBlocksServedInvZeroEnabled());
}

// T24: telemetry export/snapshot wiring guard — the 9 feature counters must
// flow through IBDMetricsSnapshot (the layer that feeds the getinfo "ibdmetrics"
// JSON object in rpcwallet.cpp). Guards against a silent export-regression.
BOOST_AUTO_TEST_CASE(t24_telemetry_snapshot_export)
{
    // known probe values written via the same atomic counters the feature uses
    const_cast<std::atomic<int64_t>*>(&ibdmetrics::Get().getblocks_served_inv_items)->store(11);
    const_cast<std::atomic<int64_t>*>(&ibdmetrics::Get().getblocks_served_inv_bytes)->store(12);
    const_cast<std::atomic<int64_t>*>(&ibdmetrics::Get().getblocks_consumption_getdata_matches)->store(13);
    const_cast<std::atomic<int64_t>*>(&ibdmetrics::Get().getblocks_suppressed_inv_replies)->store(14);
    const_cast<std::atomic<int64_t>*>(&ibdmetrics::Get().getblocks_suppressed_inv_items)->store(15);
    const_cast<std::atomic<int64_t>*>(&ibdmetrics::Get().getblocks_suppressed_inv_bytes_avoided)->store(16);
    const_cast<std::atomic<int64_t>*>(&ibdmetrics::Get().getblocks_zero_consume_windows)->store(17);
    const_cast<std::atomic<int64_t>*>(&ibdmetrics::Get().getblocks_low_nonzero_windows)->store(18);
    const_cast<std::atomic<int64_t>*>(&ibdmetrics::Get().getblocks_release_latch_events)->store(19);

    IBDMetricsSnapshot snap;
    ibdmetrics::SnapshotAll(snap);
    BOOST_CHECK_EQUAL(snap.getblocks_served_inv_items, 11);
    BOOST_CHECK_EQUAL(snap.getblocks_served_inv_bytes, 12);
    BOOST_CHECK_EQUAL(snap.getblocks_consumption_getdata_matches, 13);
    BOOST_CHECK_EQUAL(snap.getblocks_suppressed_inv_replies, 14);
    BOOST_CHECK_EQUAL(snap.getblocks_suppressed_inv_items, 15);
    BOOST_CHECK_EQUAL(snap.getblocks_suppressed_inv_bytes_avoided, 16);
    BOOST_CHECK_EQUAL(snap.getblocks_zero_consume_windows, 17);
    BOOST_CHECK_EQUAL(snap.getblocks_low_nonzero_windows, 18);
    BOOST_CHECK_EQUAL(snap.getblocks_release_latch_events, 19);
    // T25 moves the reconnect-debt counters through the same snapshot layer.
    const_cast<std::atomic<int64_t>*>(&ibdmetrics::Get().getblocks_reconnect_debt_entries)->store(31);
    const_cast<std::atomic<int64_t>*>(&ibdmetrics::Get().getblocks_reconnect_debt_transferred)->store(32);
    const_cast<std::atomic<int64_t>*>(&ibdmetrics::Get().getblocks_reconnect_debt_evicted)->store(33);
    const_cast<std::atomic<int64_t>*>(&ibdmetrics::Get().getblocks_reconnect_debt_cleared_by_consumption)->store(34);
    ibdmetrics::SnapshotAll(snap);
    BOOST_CHECK_EQUAL(snap.getblocks_reconnect_debt_entries, 31);
    BOOST_CHECK_EQUAL(snap.getblocks_reconnect_debt_transferred, 32);
    BOOST_CHECK_EQUAL(snap.getblocks_reconnect_debt_evicted, 33);
    BOOST_CHECK_EQUAL(snap.getblocks_reconnect_debt_cleared_by_consumption, 34);
    // reset probe values
    const_cast<std::atomic<int64_t>*>(&ibdmetrics::Get().getblocks_reconnect_debt_entries)->store(0);
    const_cast<std::atomic<int64_t>*>(&ibdmetrics::Get().getblocks_reconnect_debt_transferred)->store(0);
    const_cast<std::atomic<int64_t>*>(&ibdmetrics::Get().getblocks_reconnect_debt_evicted)->store(0);
    const_cast<std::atomic<int64_t>*>(&ibdmetrics::Get().getblocks_reconnect_debt_cleared_by_consumption)->store(0);
}

// ---- Reconnect-persistent zero-consumption debt (T25+) ----

// Distinct CNetAddr helper (no DNS): "1.2.<hi>.<lo>".
static CNetAddr ReconnIP(unsigned int n)
{
    char s[32];
    snprintf(s, sizeof(s), "1.2.%u.%u", (n >> 8) & 0xffu, n & 0xffu);
    return static_cast<const CNetAddr&>(CService(s, GetDefaultPort()));
}
static void ReconnReset()
{
    InitGetBlocksServedInvZero(true);
    SetGetBlocksServedInvZeroConfig(GetBlocksServedInvZeroConfig());
    GetBlocksServedInvReconnectClearAll();
}

// T25: storage write policy + clear + sparse non-zero never written.
BOOST_AUTO_TEST_CASE(t25_reconnect_storage_write_clear)
{
    ReconnReset();
    BOOST_CHECK_EQUAL(GetBlocksServedInvReconnectSize(), 0u);
    CNode::CGetBlocksServedInvState st;
    st.fGbHaveWindow = true;
    st.nGbServedInvItems = 2500;   // >= MIN_ITEMS
    st.nGbGetDataMatches = 0;      // zero consumption
    st.fGbPriorZeroConsume = true;
    GetBlocksServedInvReconnectWriteState(st, ReconnIP(1), NOW);
    BOOST_CHECK_EQUAL(GetBlocksServedInvReconnectSize(), 1u);
    // low-but-nonzero consumer must NOT create debt
    st.nGbGetDataMatches = 1;
    GetBlocksServedInvReconnectWriteState(st, ReconnIP(2), NOW);
    BOOST_CHECK_EQUAL(GetBlocksServedInvReconnectSize(), 1u);
    // plain many-getblocks / no-time-window => no debt
    CNode::CGetBlocksServedInvState st2;
    st2.fGbHaveWindow = false;
    GetBlocksServedInvReconnectWriteState(st2, ReconnIP(3), NOW);
    BOOST_CHECK_EQUAL(GetBlocksServedInvReconnectSize(), 1u);
    GetBlocksServedInvReconnectClearAll();
    BOOST_CHECK_EQUAL(GetBlocksServedInvReconnectSize(), 0u);
}

// T26: BOOTSTRAP SAFETY — inherited debt primes a NEW connection without ever
// starting it suppressed, and grants a genuine serving opportunity.
BOOST_AUTO_TEST_CASE(t26_bootstrap_not_suppressed)
{
    ReconnReset();
    CNode::CGetBlocksServedInvState old;
    old.fGbHaveWindow = true;
    old.nGbServedInvItems = 2500;
    old.nGbGetDataMatches = 0;
    old.fGbPriorZeroConsume = true;
    GetBlocksServedInvReconnectWriteState(old, ReconnIP(10), NOW);
    CNode::CGetBlocksServedInvState neu;
    bool fPrimed = false;
    GetBlocksServedInvReconnectPrime(neu, ReconnIP(10), NOW + 1, fPrimed);
    BOOST_CHECK(fPrimed);
    BOOST_CHECK_EQUAL(neu.fGbSuppressInv, false);          // never starts suppressed
    BOOST_CHECK(neu.nGbServedInvItems > 0);                // warm cost reduction applied
    BOOST_CHECK_EQUAL(neu.fGbPriorZeroConsume, true);      // re-entry streak path
    BOOST_CHECK_EQUAL(neu.nGbZeroConsumeStreak, 0u);
    // First serving opportunity: a fresh-window request at t=fresh is NOT
    // suppressed (GRACE not elapsed).
    CGetBlocksRequestInfo req = MakeRequest(TIP_A, 100, 500);
    GetBlocksServedInvDecision d = GetBlocksServedInvEvaluate(
        neu, req, true, NOW + 1, req.nPredictedResponseCount);
    BOOST_CHECK_EQUAL(d.fSuppress, false);
}

// T27: reconnect -> zero-consume re-qualifies (items primed, re-entry streak set).
BOOST_AUTO_TEST_CASE(t27_requalify_faster)
{
    ReconnReset();
    CNode::CGetBlocksServedInvState old;
    old.fGbHaveWindow = true;
    old.nGbServedInvItems = 2500;
    old.nGbGetDataMatches = 0;
    old.fGbPriorZeroConsume = true;
    GetBlocksServedInvReconnectWriteState(old, ReconnIP(20), NOW);
    CNode::CGetBlocksServedInvState neu;
    bool fPrimed = false;
    GetBlocksServedInvReconnectPrime(neu, ReconnIP(20), NOW + 1, fPrimed);
    BOOST_CHECK(fPrimed);
    BOOST_CHECK_EQUAL(neu.fGbPriorZeroConsume, true); // re-entry => lower required streak
    GetBlocksServedInvReconnectClearAll();
}

// T28: real consumption clears the IP-keyed debt.
BOOST_AUTO_TEST_CASE(t28_consume_clears_debt)
{
    ReconnReset();
    CNode::CGetBlocksServedInvState old;
    old.fGbHaveWindow = true;
    old.nGbServedInvItems = 2500;
    old.nGbGetDataMatches = 0;
    old.fGbPriorZeroConsume = true;
    GetBlocksServedInvReconnectWriteState(old, ReconnIP(30), NOW);
    BOOST_CHECK_EQUAL(GetBlocksServedInvReconnectSize(), 1u);
    GetBlocksServedInvReconnectClearedByConsumption(ReconnIP(30), NOW + 1);
    BOOST_CHECK_EQUAL(GetBlocksServedInvReconnectSize(), 0u);
}

// T29: different IP independent.
BOOST_AUTO_TEST_CASE(t29_different_ip_independent)
{
    ReconnReset();
    CNode::CGetBlocksServedInvState old;
    old.fGbHaveWindow = true;
    old.nGbServedInvItems = 2500;
    old.nGbGetDataMatches = 0;
    old.fGbPriorZeroConsume = true;
    GetBlocksServedInvReconnectWriteState(old, ReconnIP(40), NOW);
    CNode::CGetBlocksServedInvState neu;
    bool fPrimed = false;
    GetBlocksServedInvReconnectPrime(neu, ReconnIP(41), NOW + 1, fPrimed);
    BOOST_CHECK_EQUAL(fPrimed, false);
    GetBlocksServedInvReconnectClearAll();
}

// T30: subver change on same IP does not defeat the IP-keyed debt.
BOOST_AUTO_TEST_CASE(t30_same_ip_subver_irrelevant)
{
    ReconnReset();
    CNode::CGetBlocksServedInvState old;
    old.fGbHaveWindow = true;
    old.nGbServedInvItems = 2500;
    old.nGbGetDataMatches = 0;
    old.fGbPriorZeroConsume = true;
    GetBlocksServedInvReconnectWriteState(old, ReconnIP(50), NOW);
    // Subver is not part of the key; prime again from the same IP succeeds.
    CNode::CGetBlocksServedInvState neu;
    bool fPrimed = false;
    GetBlocksServedInvReconnectPrime(neu, ReconnIP(50), NOW + 1, fPrimed);
    BOOST_CHECK(fPrimed);
    GetBlocksServedInvReconnectClearAll();
}

// T31: TTL expiry — debt older than TTL does not prime.
BOOST_AUTO_TEST_CASE(t31_ttl_expiry)
{
    ReconnReset();
    CNode::CGetBlocksServedInvState old;
    old.fGbHaveWindow = true;
    old.nGbServedInvItems = 2500;
    old.nGbGetDataMatches = 0;
    old.fGbPriorZeroConsume = true;
    GetBlocksServedInvReconnectWriteState(old, ReconnIP(60), NOW);
    CNode::CGetBlocksServedInvState neu;
    bool fPrimed = false;
    GetBlocksServedInvReconnectPrime(
        neu, ReconnIP(60), NOW + GETBLOCKS_SERVED_INV_RECONNECT_DEBT_TTL_US + 1,
        fPrimed);
    BOOST_CHECK_EQUAL(fPrimed, false);
    GetBlocksServedInvReconnectClearAll();
}

// T32: CAP/LRU — bounded map, oldest evicted first.
BOOST_AUTO_TEST_CASE(t32_cap_eviction)
{
    ReconnReset();
    CNode::CGetBlocksServedInvState old;
    old.fGbHaveWindow = true;
    old.nGbServedInvItems = 2500;
    old.nGbGetDataMatches = 0;
    old.fGbPriorZeroConsume = true;
    // fill to CAP
    for (unsigned int i = 0; i < GETBLOCKS_SERVED_INV_RECONNECT_DEBT_CAP; ++i)
        GetBlocksServedInvReconnectWriteState(old, ReconnIP(0x1000 + i), NOW + i);
    BOOST_CHECK(GetBlocksServedInvReconnectSize() <= GETBLOCKS_SERVED_INV_RECONNECT_DEBT_CAP);
    // one more distinct IP forces eviction of the oldest (smallest timestamp)
    GetBlocksServedInvReconnectWriteState(old, ReconnIP(0xffff), NOW + 999999);
    BOOST_CHECK(GetBlocksServedInvReconnectSize() <= GETBLOCKS_SERVED_INV_RECONNECT_DEBT_CAP);
}

// T33: flag OFF — legacy no-op (no write, no prime).
BOOST_AUTO_TEST_CASE(t33_flag_off_noop)
{
    InitGetBlocksServedInvZero(false);
    GetBlocksServedInvReconnectClearAll();
    CNode::CGetBlocksServedInvState old;
    old.fGbHaveWindow = true;
    old.nGbServedInvItems = 2500;
    old.nGbGetDataMatches = 0;
    old.fGbPriorZeroConsume = true;
    GetBlocksServedInvReconnectWriteState(old, ReconnIP(70), NOW);
    BOOST_CHECK_EQUAL(GetBlocksServedInvReconnectSize(), 0u);
    CNode::CGetBlocksServedInvState neu;
    bool fPrimed = false;
    GetBlocksServedInvReconnectPrime(neu, ReconnIP(70), NOW + 1, fPrimed);
    BOOST_CHECK_EQUAL(fPrimed, false);
    // restore flag for later tests (test-suite ordering independent)
    InitGetBlocksServedInvZero(true);
    SetGetBlocksServedInvZeroConfig(GetBlocksServedInvZeroConfig());
}

// T34: safety release != forget-all-history — per-connection history marker
// survives a single consumption even after a primed (re-entry) state.
BOOST_AUTO_TEST_CASE(t34_release_not_erase_history)
{
    ReconnReset();
    CNode::CGetBlocksServedInvState neu;
    // simulate a re-entry-primed peer that then consumes once
    neu.fGbHaveWindow = true;
    neu.nGbServedInvItems = 2500;
    neu.nGbGetDataMatches = 0;
    neu.fGbPriorZeroConsume = true;
    neu.hashGbLastResponseChainTip = TIP_A;
    CGetBlocksRequestInfo req = MakeRequest(TIP_A, 100, 500);
    ServeReply(neu, req, 500, 101, NOW);
    // note one real consumption (matching getdata)
    GetBlocksServedInvNoteGetData(neu, uint256(7), 101, NOW + 1);
    BOOST_CHECK(neu.nGbGetDataMatches > 0);
    BOOST_CHECK_EQUAL(neu.fGbPriorZeroConsume, true); // history NOT erased
}

BOOST_AUTO_TEST_SUITE_END()
