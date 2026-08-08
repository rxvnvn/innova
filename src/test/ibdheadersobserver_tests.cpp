#include <boost/test/unit_test.hpp>
#include "ibdheaderscheduler.h"
#include <algorithm>

namespace {
uint256 OH(uint64_t n) { return uint256(n); }
std::vector<std::pair<uint256, uint256> > Batch(uint64_t first, uint64_t last)
{
    std::vector<std::pair<uint256, uint256> > out;
    for (uint64_t i = first; i <= last; ++i)
        out.push_back(std::make_pair(OH(i), OH(i - 1)));
    return out;
}
}

BOOST_AUTO_TEST_SUITE(ibdheadersobserver_tests)

BOOST_AUTO_TEST_CASE(disabled_is_inert)
{
    CIbdHeadersObserver o(4);
    BOOST_CHECK(!o.UpdateAnchor(OH(1), 0));
    o.MarkHeaderRequest(1);
    BOOST_CHECK(!o.IsHeaderResponseExpected(1));
    BOOST_CHECK(o.ObserveHeaders(1, Batch(2, 3), 2).continuationLocator.empty());
    BOOST_CHECK(o.Graph().Empty());
}

BOOST_AUTO_TEST_CASE(anchor_population_and_continuation_rounds)
{
    CIbdHeadersObserver o(512); o.SetEnabled(true);
    BOOST_REQUIRE(o.UpdateAnchor(OH(1), 0));
    o.MarkHeaderRequest(7);
    CIbdHeadersObserver::HeaderResult r = o.ObserveHeaders(7, Batch(2, 2001));
    BOOST_CHECK(r.expectedResponse); BOOST_CHECK(r.continueHeaders);
    BOOST_REQUIRE(!r.continuationLocator.empty());
    BOOST_CHECK(r.continuationLocator.front() == OH(2001));
    BOOST_CHECK(r.continuationLocator.back() == OH(1));
    o.MarkHeaderRequest(7);
    r = o.ObserveHeaders(7, Batch(2002, 4001));
    BOOST_CHECK(r.continueHeaders);
    BOOST_CHECK(r.continuationLocator.front() == OH(4001));
    BOOST_REQUIRE(o.Graph().ActiveTip());
    BOOST_CHECK(o.Graph().ActiveTip()->height == 4000);
}

BOOST_AUTO_TEST_CASE(mixed_peer_support_duplicates_and_divergence)
{
    CIbdHeadersObserver o(8); o.SetEnabled(true);
    BOOST_REQUIRE(o.UpdateAnchor(OH(1), 0));
    o.MarkHeaderRequest(1);
    o.ObserveHeaders(1, Batch(2, 5), 2000);
    o.MarkHeaderRequest(2);
    o.ObserveHeaders(2, Batch(2, 5), 2000);
    BOOST_CHECK(o.PeerSupport(OH(5)) == 2U);
    o.RemovePeer(2);
    BOOST_CHECK(o.PeerSupport(OH(5)) == 1U);
    std::vector<std::pair<uint256, uint256> > fork;
    fork.push_back(std::make_pair(OH(20), OH(2)));
    fork.push_back(std::make_pair(OH(21), OH(20)));
    o.ObserveHeaders(3, fork, 2000);
    BOOST_CHECK(o.Classify(OH(20)) == CIbdHeadersObserver::OFF_ACTIVE_BRANCH);
    std::vector<uint256> window = o.PredictedWindow();
    BOOST_CHECK(std::find(window.begin(), window.end(), OH(20)) == window.end());
    BOOST_CHECK(o.Graph().ActiveTip()->hash == OH(5));
}

BOOST_AUTO_TEST_CASE(disconnected_and_quarantined_never_predicted)
{
    CIbdHeadersObserver o(8); o.SetEnabled(true);
    BOOST_REQUIRE(o.UpdateAnchor(OH(1), 0));
    std::vector<std::pair<uint256, uint256> > disconnected;
    disconnected.push_back(std::make_pair(OH(50), OH(49)));
    disconnected.push_back(std::make_pair(OH(51), OH(50)));
    o.ObserveHeaders(4, disconnected);
    BOOST_CHECK(o.Classify(OH(50)) == CIbdHeadersObserver::OFF_ACTIVE_BRANCH);
    BOOST_CHECK(o.PredictedWindow().empty());
    BOOST_CHECK(o.Stats().disconnected == 2U);
}

BOOST_AUTO_TEST_CASE(anchor_advance_retains_suffix_and_reorg_resets)
{
    CIbdHeadersObserver o(8); o.SetEnabled(true);
    BOOST_REQUIRE(o.UpdateAnchor(OH(1), 10));
    o.ObserveHeaders(1, Batch(2, 6));
    BOOST_REQUIRE(o.UpdateAnchor(OH(3), 12));
    BOOST_REQUIRE(o.Graph().ActiveTip());
    BOOST_CHECK(o.Graph().ActiveTip()->hash == OH(6));
    BOOST_CHECK(o.PredictedWindow().front() == OH(4));
    BOOST_REQUIRE(o.UpdateAnchor(OH(90), 11));
    BOOST_CHECK(o.Graph().ActiveTip()->hash == OH(90));
    BOOST_CHECK(o.PredictedWindow().empty());
    BOOST_CHECK(!o.Graph().Lookup(OH(6)));
}

BOOST_AUTO_TEST_CASE(classification_and_observation_only_counters)
{
    CIbdHeadersObserver o(2); o.SetEnabled(true);
    BOOST_REQUIRE(o.UpdateAnchor(OH(1), 100));
    o.ObserveHeaders(1, Batch(2, 5));
    BOOST_CHECK(o.Classify(OH(2)) == CIbdHeadersObserver::IN_PREDICTED_WINDOW);
    BOOST_CHECK(o.Classify(OH(4)) == CIbdHeadersObserver::AFTER_WINDOW);
    BOOST_CHECK(o.Classify(OH(1), 100) == CIbdHeadersObserver::BEFORE_WINDOW);
    BOOST_CHECK(o.Classify(OH(99)) == CIbdHeadersObserver::UNKNOWN_TO_GRAPH);
    o.RecordClassification(0, o.Classify(OH(2)));
    o.RecordClassification(1, o.Classify(OH(4)));
    o.RecordClassification(2, o.Classify(OH(99)));
    BOOST_CHECK(o.Stats().classified[0][CIbdHeadersObserver::IN_PREDICTED_WINDOW] == 1U);
    BOOST_CHECK(o.Stats().classified[1][CIbdHeadersObserver::AFTER_WINDOW] == 1U);
    BOOST_CHECK(o.Stats().classified[2][CIbdHeadersObserver::UNKNOWN_TO_GRAPH] == 1U);
    // The observer exposes no AskFor, ownership, getdata, or admission API.
}

BOOST_AUTO_TEST_SUITE_END()
