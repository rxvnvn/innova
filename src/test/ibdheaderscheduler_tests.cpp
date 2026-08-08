// Copyright (c) 2026 The Innova developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/test/unit_test.hpp>

#include <ctime>
#include "ibdheaderscheduler.h"

namespace {

static uint256 H(uint64_t value) { return uint256(value); }

static void RequireNode(const CIbdHeaderGraph& graph, uint64_t hash,
                        int height, CIbdHeaderNode::State state)
{
    const CIbdHeaderNode* node = graph.Lookup(H(hash));
    BOOST_REQUIRE(node != NULL);
    BOOST_CHECK(node->height == height);
    BOOST_CHECK(node->state == state);
}

static CIbdHeaderGraph LinearGraph()
{
    CIbdHeaderGraph graph;
    BOOST_REQUIRE(graph.SetAuthoritativeAnchor(H(100), 500));
    BOOST_REQUIRE(graph.Insert(H(101), H(100)) == CIbdHeaderGraph::INSERTED_ACTIVE);
    BOOST_REQUIRE(graph.Insert(H(102), H(101)) == CIbdHeaderGraph::INSERTED_ACTIVE);
    BOOST_REQUIRE(graph.Insert(H(103), H(102)) == CIbdHeaderGraph::INSERTED_ACTIVE);
    return graph;
}

} // namespace

BOOST_AUTO_TEST_SUITE(ibdheaderscheduler_tests)

BOOST_AUTO_TEST_CASE(empty_graph)
{
    CIbdHeaderGraph graph;
    BOOST_CHECK(graph.Empty());
    BOOST_CHECK(!graph.HasAnchor());
    BOOST_CHECK(graph.ActiveTip() == NULL);
    BOOST_CHECK(graph.BestKnownEligibleTip() == NULL);
    BOOST_CHECK(graph.GetActiveWindow(H(1), 10).empty());
    BOOST_CHECK(graph.CheckInvariants());
}

BOOST_AUTO_TEST_CASE(authoritative_anchor_is_explicit_identity)
{
    CIbdHeaderGraph graph;
    BOOST_CHECK(!graph.SetAuthoritativeAnchor(H(0), 5));
    BOOST_CHECK(!graph.SetAuthoritativeAnchor(H(1), -1));
    BOOST_REQUIRE(graph.SetAuthoritativeAnchor(H(100), 500));
    BOOST_CHECK(graph.Size() == 1U);
    BOOST_CHECK(graph.AnchorHash() == H(100));
    BOOST_CHECK(graph.AnchorHeight() == 500);
    RequireNode(graph, 100, 500, CIbdHeaderNode::AUTHORITATIVE_ANCHOR);
    BOOST_CHECK(graph.CheckInvariants());
}

BOOST_AUTO_TEST_CASE(first_child_and_contiguous_extension_derive_heights)
{
    CIbdHeaderGraph graph;
    BOOST_REQUIRE(graph.SetAuthoritativeAnchor(H(100), 500));
    BOOST_CHECK(graph.Insert(H(101), H(100)) == CIbdHeaderGraph::INSERTED_ACTIVE);
    BOOST_CHECK(graph.Insert(H(102), H(101)) == CIbdHeaderGraph::INSERTED_ACTIVE);
    RequireNode(graph, 101, 501, CIbdHeaderNode::ACTIVE);
    RequireNode(graph, 102, 502, CIbdHeaderNode::ACTIVE);
    BOOST_REQUIRE(graph.ActiveTip());
    BOOST_CHECK(graph.ActiveTip()->hash == H(102));
    BOOST_CHECK(graph.CheckInvariants());
}

BOOST_AUTO_TEST_CASE(duplicate_is_idempotent_and_conflict_rejected)
{
    CIbdHeaderGraph graph = LinearGraph();
    const std::size_t before = graph.Size();
    BOOST_CHECK(graph.Insert(H(102), H(101)) == CIbdHeaderGraph::DUPLICATE);
    BOOST_CHECK(graph.Insert(H(102), H(100)) == CIbdHeaderGraph::CONFLICT);
    BOOST_CHECK(graph.Insert(H(200), H(200)) == CIbdHeaderGraph::CONFLICT);
    BOOST_CHECK(graph.Size() == before);
    BOOST_CHECK(graph.CheckInvariants());
}

BOOST_AUTO_TEST_CASE(unknown_parent_is_not_scheduler_usable)
{
    CIbdHeaderGraph graph;
    BOOST_REQUIRE(graph.SetAuthoritativeAnchor(H(100), 500));
    BOOST_CHECK(graph.Insert(H(202), H(201)) == CIbdHeaderGraph::INSERTED_QUARANTINED);
    RequireNode(graph, 202, -1, CIbdHeaderNode::QUARANTINED);
    BOOST_CHECK(!graph.Lookup(H(202))->IsUsable());
    BOOST_CHECK(graph.ActiveTip()->hash == H(100));
    BOOST_CHECK(graph.GetActiveWindow(H(100), 10).empty());
    BOOST_CHECK(graph.CheckInvariants());
}

BOOST_AUTO_TEST_CASE(disconnected_chain_connects_only_when_ancestry_reaches_anchor)
{
    CIbdHeaderGraph graph;
    BOOST_REQUIRE(graph.SetAuthoritativeAnchor(H(100), 500));
    BOOST_REQUIRE(graph.Insert(H(203), H(202)) == CIbdHeaderGraph::INSERTED_QUARANTINED);
    BOOST_REQUIRE(graph.Insert(H(202), H(201)) == CIbdHeaderGraph::INSERTED_QUARANTINED);
    RequireNode(graph, 202, -1, CIbdHeaderNode::QUARANTINED);
    RequireNode(graph, 203, -1, CIbdHeaderNode::QUARANTINED);

    BOOST_REQUIRE(graph.Insert(H(201), H(100)) == CIbdHeaderGraph::INSERTED_ACTIVE);
    RequireNode(graph, 201, 501, CIbdHeaderNode::ACTIVE);
    RequireNode(graph, 202, 502, CIbdHeaderNode::ACTIVE);
    RequireNode(graph, 203, 503, CIbdHeaderNode::ACTIVE);
    BOOST_CHECK(graph.ActiveTip()->hash == H(203));
    BOOST_CHECK(graph.CheckInvariants());
}

BOOST_AUTO_TEST_CASE(competing_branch_does_not_replace_active_path)
{
    CIbdHeaderGraph graph = LinearGraph();
    BOOST_REQUIRE(graph.Insert(H(202), H(101)) == CIbdHeaderGraph::INSERTED_ELIGIBLE);
    BOOST_REQUIRE(graph.Insert(H(203), H(202)) == CIbdHeaderGraph::INSERTED_ELIGIBLE);
    BOOST_REQUIRE(graph.Insert(H(204), H(203)) == CIbdHeaderGraph::INSERTED_ELIGIBLE);
    BOOST_CHECK(graph.ActiveTip()->hash == H(103));
    RequireNode(graph, 202, 502, CIbdHeaderNode::ELIGIBLE);
    RequireNode(graph, 204, 504, CIbdHeaderNode::ELIGIBLE);
    BOOST_REQUIRE(graph.BestKnownEligibleTip());
    BOOST_CHECK(graph.BestKnownEligibleTip()->hash == H(204));

    const std::vector<uint256> window = graph.GetActiveWindow(H(100), 10);
    BOOST_REQUIRE(window.size() == 3U);
    BOOST_CHECK(window[0] == H(101));
    BOOST_CHECK(window[1] == H(102));
    BOOST_CHECK(window[2] == H(103));
    BOOST_CHECK(graph.CheckInvariants());
}

BOOST_AUTO_TEST_CASE(explicit_branch_activation_is_scheduler_only)
{
    CIbdHeaderGraph graph = LinearGraph();
    BOOST_REQUIRE(graph.Insert(H(202), H(101)) == CIbdHeaderGraph::INSERTED_ELIGIBLE);
    BOOST_REQUIRE(graph.Insert(H(203), H(202)) == CIbdHeaderGraph::INSERTED_ELIGIBLE);
    BOOST_REQUIRE(graph.ActivateBranch(H(203)));
    BOOST_CHECK(graph.ActiveTip()->hash == H(203));
    RequireNode(graph, 102, 502, CIbdHeaderNode::ELIGIBLE);
    RequireNode(graph, 103, 503, CIbdHeaderNode::ELIGIBLE);
    RequireNode(graph, 202, 502, CIbdHeaderNode::ACTIVE);
    RequireNode(graph, 203, 503, CIbdHeaderNode::ACTIVE);
    BOOST_CHECK(graph.CheckInvariants());
}

BOOST_AUTO_TEST_CASE(quarantine_excludes_branch_and_descendants)
{
    CIbdHeaderGraph graph = LinearGraph();
    BOOST_REQUIRE(graph.Insert(H(202), H(101)) == CIbdHeaderGraph::INSERTED_ELIGIBLE);
    BOOST_REQUIRE(graph.Insert(H(203), H(202)) == CIbdHeaderGraph::INSERTED_ELIGIBLE);
    BOOST_REQUIRE(graph.QuarantineBranch(H(202)));
    RequireNode(graph, 202, 502, CIbdHeaderNode::QUARANTINED);
    RequireNode(graph, 203, 503, CIbdHeaderNode::QUARANTINED);
    BOOST_CHECK(!graph.Lookup(H(202))->IsUsable());
    BOOST_CHECK(!graph.ActivateBranch(H(203)));
    BOOST_CHECK(!graph.QuarantineBranch(H(100)));
    BOOST_CHECK(graph.ActiveTip()->hash == H(103));
    BOOST_CHECK(graph.CheckInvariants());
}

BOOST_AUTO_TEST_CASE(quarantining_active_branch_rolls_back_scheduler_tip)
{
    CIbdHeaderGraph graph = LinearGraph();
    BOOST_REQUIRE(graph.QuarantineBranch(H(102)));
    BOOST_CHECK(graph.ActiveTip()->hash == H(101));
    RequireNode(graph, 102, 502, CIbdHeaderNode::QUARANTINED);
    RequireNode(graph, 103, 503, CIbdHeaderNode::QUARANTINED);
    const std::vector<uint256> window = graph.GetActiveWindow(H(100), 10);
    BOOST_REQUIRE(window.size() == 1U);
    BOOST_CHECK(window[0] == H(101));
    BOOST_CHECK(graph.CheckInvariants());
}

BOOST_AUTO_TEST_CASE(ancestor_lookup_covers_self_parent_deep_and_anchor)
{
    CIbdHeaderGraph graph = LinearGraph();
    uint256 ancestor;
    BOOST_CHECK(graph.GetAncestor(H(103), 503, ancestor));
    BOOST_CHECK(ancestor == H(103));
    BOOST_CHECK(graph.GetAncestor(H(103), 502, ancestor));
    BOOST_CHECK(ancestor == H(102));
    BOOST_CHECK(graph.GetAncestor(H(103), 500, ancestor));
    BOOST_CHECK(ancestor == H(100));
    BOOST_CHECK(!graph.GetAncestor(H(103), 499, ancestor));
    BOOST_CHECK(!graph.GetAncestor(H(103), 504, ancestor));
    BOOST_CHECK(!graph.GetAncestor(H(999), 500, ancestor));
    BOOST_CHECK(graph.IsDescendantOf(H(103), H(101)));
    BOOST_CHECK(!graph.IsDescendantOf(H(101), H(103)));
}

BOOST_AUTO_TEST_CASE(ancestor_lookup_remains_structural_on_quarantined_branch)
{
    CIbdHeaderGraph graph = LinearGraph();
    BOOST_REQUIRE(graph.Insert(H(202), H(101)) == CIbdHeaderGraph::INSERTED_ELIGIBLE);
    BOOST_REQUIRE(graph.Insert(H(203), H(202)) == CIbdHeaderGraph::INSERTED_ELIGIBLE);
    BOOST_REQUIRE(graph.QuarantineBranch(H(202)));
    uint256 ancestor;
    BOOST_CHECK(graph.GetAncestor(H(203), 502, ancestor));
    BOOST_CHECK(ancestor == H(202));
    BOOST_CHECK(graph.GetAncestor(H(203), 501, ancestor));
    BOOST_CHECK(ancestor == H(101));
}

BOOST_AUTO_TEST_CASE(ordered_window_is_contiguous_and_truncated)
{
    CIbdHeaderGraph graph = LinearGraph();
    const std::vector<uint256> full = graph.GetActiveWindow(H(100), 10);
    BOOST_REQUIRE(full.size() == 3U);
    BOOST_CHECK(full[0] == H(101));
    BOOST_CHECK(full[1] == H(102));
    BOOST_CHECK(full[2] == H(103));
    BOOST_CHECK(graph.Lookup(full[1])->prev == full[0]);
    BOOST_CHECK(graph.Lookup(full[2])->prev == full[1]);

    const std::vector<uint256> truncated = graph.GetActiveWindow(H(100), 2);
    BOOST_REQUIRE(truncated.size() == 2U);
    BOOST_CHECK(truncated[0] == H(101));
    BOOST_CHECK(truncated[1] == H(102));
    BOOST_CHECK(graph.GetActiveWindow(H(100), 0).empty());
}

BOOST_AUTO_TEST_CASE(window_requires_frontier_on_active_path)
{
    CIbdHeaderGraph graph = LinearGraph();
    BOOST_REQUIRE(graph.Insert(H(202), H(101)) == CIbdHeaderGraph::INSERTED_ELIGIBLE);
    BOOST_CHECK(graph.GetActiveWindow(H(202), 10).empty());
    BOOST_CHECK(graph.GetActiveWindow(H(999), 10).empty());
    const std::vector<uint256> suffix = graph.GetActiveWindow(H(101), 10);
    BOOST_REQUIRE(suffix.size() == 2U);
    BOOST_CHECK(suffix[0] == H(102));
    BOOST_CHECK(suffix[1] == H(103));
}

BOOST_AUTO_TEST_CASE(permanent_quarantine_survives_later_parent_connection)
{
    CIbdHeaderGraph graph;
    BOOST_REQUIRE(graph.SetAuthoritativeAnchor(H(100), 500));
    BOOST_REQUIRE(graph.Insert(H(202), H(201)) == CIbdHeaderGraph::INSERTED_QUARANTINED);
    BOOST_REQUIRE(graph.QuarantineBranch(H(202)));
    BOOST_REQUIRE(graph.Insert(H(201), H(100)) == CIbdHeaderGraph::INSERTED_ACTIVE);
    RequireNode(graph, 202, -1, CIbdHeaderNode::QUARANTINED);
    BOOST_CHECK(!graph.Lookup(H(202))->IsUsable());
    BOOST_CHECK(graph.ActiveTip()->hash == H(201));
    BOOST_CHECK(graph.CheckInvariants());
}

BOOST_AUTO_TEST_CASE(clear_resets_all_state)
{
    CIbdHeaderGraph graph = LinearGraph();
    graph.Clear();
    BOOST_CHECK(graph.Empty());
    BOOST_CHECK(!graph.HasAnchor());
    BOOST_CHECK(graph.ActiveTip() == NULL);
    BOOST_CHECK(graph.CheckInvariants());
}

BOOST_AUTO_TEST_CASE(batch_insert_matches_single_insert_and_scales)
{
    for (std::size_t n = 100; n <= 8000; n *= 2)
    {
        std::vector<std::pair<uint256, uint256> > headers;
        headers.reserve(n);
        uint64_t prev = 1000;
        for (std::size_t i = 0; i < n; ++i)
        {
            const uint64_t hash = prev + 1;
            headers.push_back(std::make_pair(H(hash), H(prev)));
            prev = hash;
        }
        CIbdHeaderGraph batch;
        CIbdHeaderGraph single;
        BOOST_REQUIRE(batch.SetAuthoritativeAnchor(H(1000), 0));
        BOOST_REQUIRE(single.SetAuthoritativeAnchor(H(1000), 0));
        const std::clock_t begin = std::clock();
        const std::vector<CIbdHeaderGraph::InsertResult> result =
            batch.InsertBatch(headers);
        const double elapsedMs = 1000.0 * (std::clock() - begin) / CLOCKS_PER_SEC;
        BOOST_REQUIRE_EQUAL(result.size(), n);
        BOOST_CHECK(batch.Size() == n + 1);
        BOOST_CHECK(batch.ActiveTip()->hash == H(1000 + n));
        BOOST_CHECK(batch.CheckInvariants());
        for (std::size_t i = 0; i < n; ++i)
            BOOST_REQUIRE(single.Insert(headers[i].first, headers[i].second) ==
                          CIbdHeaderGraph::INSERTED_ACTIVE);
        BOOST_CHECK(single.ActiveTip()->hash == batch.ActiveTip()->hash);
        BOOST_CHECK(single.GetActiveWindow(H(1000), n).size() ==
                    batch.GetActiveWindow(H(1000), n).size());
        std::printf("IBD_HEADER_GRAPH_BENCH n=%zu batch_ms=%.3f\n", n, elapsedMs);
    }
}

BOOST_AUTO_TEST_CASE(batch_insert_preserves_quarantine_and_reanchor)
{
    CIbdHeaderGraph graph;
    BOOST_REQUIRE(graph.SetAuthoritativeAnchor(H(100), 10));
    std::vector<std::pair<uint256, uint256> > headers;
    headers.push_back(std::make_pair(H(101), H(100)));
    headers.push_back(std::make_pair(H(102), H(101)));
    headers.push_back(std::make_pair(H(500), H(499)));
    const std::vector<CIbdHeaderGraph::InsertResult> result = graph.InsertBatch(headers);
    BOOST_CHECK(result[0] == CIbdHeaderGraph::INSERTED_ACTIVE);
    BOOST_CHECK(result[1] == CIbdHeaderGraph::INSERTED_ACTIVE);
    BOOST_CHECK(result[2] == CIbdHeaderGraph::INSERTED_QUARANTINED);
    BOOST_REQUIRE(graph.Reanchor(H(101), 11));
    BOOST_CHECK(graph.CheckInvariants());
    BOOST_CHECK(graph.ActiveTip()->hash == H(102));
}

BOOST_AUTO_TEST_SUITE_END()
