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

// The fast anchor path must never sweep source records: it moves the anchor
// along an already-present active path and removes no graph node, so every
// m_sources key stays valid.  This test advances the anchor dozens of times
// and asserts zero source entries examined on every fast update.
BOOST_AUTO_TEST_CASE(fast_anchor_advance_examines_no_source_records)
{
    CIbdHeadersObserver o(512); o.SetEnabled(true);
    BOOST_REQUIRE(o.UpdateAnchor(OH(1), 0));
    o.MarkHeaderRequest(7);
    o.ObserveHeaders(7, Batch(2, 200));
    BOOST_CHECK(o.PeerSupport(OH(200)) == 1U);

    for (uint64_t i = 2; i <= 100; ++i)
    {
        BOOST_REQUIRE(o.UpdateAnchor(OH(i), (int)(i - 1)));
        BOOST_CHECK_EQUAL(o.LastAnchorSourceSweepExamined(), 0U);
    }
    BOOST_CHECK(o.Graph().AnchorHash() == OH(100));
    BOOST_CHECK(o.Graph().AnchorHeight() == 99);
}

// Deterministic complexity regression: the number of source records examined
// during a single fast anchor update must be independent of the accumulated
// header-graph / m_sources size.  A 50000-entry source map is constructed and
// a fast advance must still examine zero records.
BOOST_AUTO_TEST_CASE(fast_anchor_advance_is_independent_of_source_map_size)
{
    CIbdHeadersObserver o(512); o.SetEnabled(true);
    BOOST_REQUIRE(o.UpdateAnchor(OH(1), 0));
    o.MarkHeaderRequest(7);
    o.ObserveHeaders(7, Batch(2, 50001));
    BOOST_CHECK(o.PeerSupport(OH(50001)) == 1U);
    BOOST_CHECK(o.Graph().Size() > 1000U);

    BOOST_REQUIRE(o.UpdateAnchor(OH(2), 1));
    BOOST_CHECK_EQUAL(o.LastAnchorSourceSweepExamined(), 0U);
    BOOST_REQUIRE(o.UpdateAnchor(OH(3), 2));
    BOOST_CHECK_EQUAL(o.LastAnchorSourceSweepExamined(), 0U);
    BOOST_CHECK(o.Graph().AnchorHash() == OH(3));
    BOOST_CHECK(o.PeerSupport(OH(50001)) == 1U);
}

// Source metadata must survive many fast anchor advances and remain the
// scheduler peer/candidate input: HeaderSources / PeerSupport for both
// near-anchor and deep window hashes stay correct after a long fast run.
BOOST_AUTO_TEST_CASE(fast_anchor_preserves_source_metadata_for_candidates)
{
    CIbdHeadersObserver o(512); o.SetEnabled(true);
    BOOST_REQUIRE(o.UpdateAnchor(OH(1), 0));
    o.MarkHeaderRequest(7);
    o.ObserveHeaders(7, Batch(2, 2000));
    o.MarkHeaderRequest(9);
    o.ObserveHeaders(9, Batch(2, 2000));

    for (uint64_t i = 2; i <= 500; ++i)
    {
        BOOST_REQUIRE(o.UpdateAnchor(OH(i), (int)(i - 1)));
        BOOST_CHECK_EQUAL(o.LastAnchorSourceSweepExamined(), 0U);
    }

    BOOST_CHECK(o.PeerSupport(OH(2000)) == 2U);
    BOOST_CHECK(o.PeerSupport(OH(501)) == 2U);
    std::vector<int64_t> sources = o.HeaderSources(OH(1500));
    BOOST_REQUIRE(sources.size() == 2U);
    BOOST_CHECK(sources[0] == 7 || sources[1] == 7);
    BOOST_CHECK(sources[0] == 9 || sources[1] == 9);
    // Anchored entries still resolve in the graph and are classified as part
    // of the predicted window immediately after the new anchor.
    BOOST_CHECK(o.Graph().Lookup(OH(501)) != NULL);
}

// Full re-anchor resets and rebuilds the graph, removing every non-suffix
// node.  Stale source records for wiped nodes must be pruned exactly there,
// while sources for the retained suffix survive; the sweep must account the
// exact number of entries examined.
BOOST_AUTO_TEST_CASE(full_reanchor_prunes_stale_sources_and_retains_suffix)
{
    CIbdHeadersObserver o(8); o.SetEnabled(true);
    BOOST_REQUIRE(o.UpdateAnchor(OH(1), 10));
    o.MarkHeaderRequest(7);
    o.ObserveHeaders(7, Batch(2, 6));

    // Jump to an unrelated branch: full re-anchor wipes every graph node.
    BOOST_REQUIRE(o.UpdateAnchor(OH(90), 11));
    BOOST_CHECK(o.Graph().Lookup(OH(3)) == NULL);
    BOOST_CHECK_EQUAL(o.LastAnchorSourceSweepExamined(), 5U);
    BOOST_CHECK(o.PeerSupport(OH(3)) == 0U);
    BOOST_CHECK(o.HeaderSources(OH(3)).empty());

    // Re-establish a chain above the new anchor, then full re-anchor to a
    // descendant.  The suffix (new anchor .. active tip) is retained by the
    // rebuild; sources below the new anchor are stale and must be pruned.
    o.MarkHeaderRequest(7);
    o.ObserveHeaders(7, Batch(91, 95));
    BOOST_REQUIRE(o.UpdateAnchor(OH(92), 13));
    BOOST_CHECK_EQUAL(o.LastAnchorSourceSweepExamined(), 5U);
    BOOST_CHECK(o.Graph().Lookup(OH(91)) == NULL);
    BOOST_CHECK(o.Graph().Lookup(OH(93)) != NULL);
    BOOST_CHECK(o.PeerSupport(OH(91)) == 0U);
    BOOST_CHECK(o.HeaderSources(OH(91)).empty());
    BOOST_CHECK(o.PeerSupport(OH(93)) == 1U);
    BOOST_CHECK(o.PeerSupport(OH(95)) == 1U);
    BOOST_REQUIRE(o.HeaderSources(OH(95)).size() == 1U);
    BOOST_CHECK(o.HeaderSources(OH(95)).front() == 7);
}

// Clear (SetEnabled transition) must remove every source record together with
// the graph, keeping keys(m_sources) <= nodes(m_graph) after the reset, and a
// fresh anchor population must rebuild both consistently.
BOOST_AUTO_TEST_CASE(clear_resets_sources_with_graph)
{
    CIbdHeadersObserver o(8); o.SetEnabled(true);
    BOOST_REQUIRE(o.UpdateAnchor(OH(1), 0));
    o.MarkHeaderRequest(7);
    o.ObserveHeaders(7, Batch(2, 5));
    BOOST_CHECK(o.PeerSupport(OH(4)) == 1U);

    o.SetEnabled(false);
    BOOST_CHECK(o.Graph().Empty());
    BOOST_CHECK(o.PeerSupport(OH(4)) == 0U);
    BOOST_CHECK(o.HeaderSources(OH(4)).empty());

    o.SetEnabled(true);
    BOOST_REQUIRE(o.UpdateAnchor(OH(1), 0));
    o.MarkHeaderRequest(7);
    o.ObserveHeaders(7, Batch(2, 5));
    BOOST_CHECK(o.PeerSupport(OH(4)) == 1U);
    BOOST_REQUIRE(o.UpdateAnchor(OH(2), 1));
    BOOST_CHECK_EQUAL(o.LastAnchorSourceSweepExamined(), 0U);
}

// Peer removal only drops the peer from each source set; it must not remove
// valid keys, and fast anchor advancement afterwards must not disturb the
// remaining peer associations.
BOOST_AUTO_TEST_CASE(remove_peer_keeps_fast_anchor_sources_consistent)
{
    CIbdHeadersObserver o(8); o.SetEnabled(true);
    BOOST_REQUIRE(o.UpdateAnchor(OH(1), 0));
    o.MarkHeaderRequest(7);
    o.ObserveHeaders(7, Batch(2, 6));
    o.MarkHeaderRequest(9);
    o.ObserveHeaders(9, Batch(2, 6));
    BOOST_CHECK(o.PeerSupport(OH(5)) == 2U);

    o.RemovePeer(9);
    BOOST_CHECK(o.PeerSupport(OH(5)) == 1U);
    BOOST_REQUIRE(o.UpdateAnchor(OH(2), 1));
    BOOST_CHECK_EQUAL(o.LastAnchorSourceSweepExamined(), 0U);
    BOOST_CHECK(o.PeerSupport(OH(6)) == 1U);
    BOOST_REQUIRE(o.HeaderSources(OH(6)).size() == 1U);
    BOOST_CHECK(o.HeaderSources(OH(6)).front() == 7);
}

BOOST_AUTO_TEST_SUITE_END()
