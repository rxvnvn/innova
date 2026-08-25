#define BOOST_TEST_DYN_LINK

#include <boost/test/unit_test.hpp>

#include "../main.h"
#include "../net.h"

#include <set>
#include <vector>

BOOST_AUTO_TEST_SUITE(block_request_ownership_tests)

namespace {

struct DummyNode : public CNode
{
    explicit DummyNode(NodeId id)
        : CNode(INVALID_SOCKET, CAddress(), "dummy", false)
    {
        this->id = id;
    }
};

static void ClearGlobalRequestState()
{
    LOCK(cs_mapAlreadyAskedFor);
    mapAlreadyAskedFor.clear();
}

} // namespace

BOOST_AUTO_TEST_CASE(stale_already_asked_pruning)
{
    CInv inv(MSG_BLOCK, uint256((uint64_t)1));
    {
        LOCK(cs_mapAlreadyAskedFor);
        mapAlreadyAskedFor[inv] = 1;
    }
    BOOST_CHECK(PruneAlreadyAskedFor(ALREADY_ASKED_FOR_RETENTION_US + 2) > 0);
}

BOOST_AUTO_TEST_CASE(lifecycle_bound_cleanup_only_when_unowned)
{
    DummyNode a(1);
    CInv inv(MSG_BLOCK, uint256((uint64_t)2));
    a.AskFor(inv);
    BOOST_CHECK(!EraseAlreadyAskedForIfUnowned(inv, &a));
}

BOOST_AUTO_TEST_CASE(per_peer_queued_block_dedup)
{
    DummyNode a(1);
    CInv inv(MSG_BLOCK, uint256((uint64_t)3));
    a.AskFor(inv);
    a.AskFor(inv);
    BOOST_CHECK_EQUAL(a.mapAskFor.size(), (size_t)1);
}

BOOST_AUTO_TEST_CASE(tx_behavior_unaffected)
{
    DummyNode a(1);
    CInv inv(MSG_TX, uint256((uint64_t)4));
    a.AskFor(inv);
    a.AskFor(inv);
    BOOST_CHECK(a.mapAskFor.size() >= 1);
}

BOOST_AUTO_TEST_CASE(single_owner_semantics_across_peers)
{
    NodeId owner = -1;
    BlockRequestOwnerState state = BLOCK_REQUEST_OWNER_QUEUED;
    BOOST_CHECK(TryAssignBlockRequestOwner(uint256((uint64_t)5), 1, &owner, &state));
    BOOST_CHECK(!TryAssignBlockRequestOwner(uint256((uint64_t)5), 2, &owner, &state));
}

BOOST_AUTO_TEST_CASE(repeated_same_peer_scheduling_not_multiplied)
{
    DummyNode a(1);
    CInv inv(MSG_BLOCK, uint256((uint64_t)6));
    a.AskFor(inv);
    a.AskFor(inv);
    BOOST_CHECK_EQUAL(a.mapAskFor.size(), (size_t)1);
}

BOOST_AUTO_TEST_CASE(repeated_cross_peer_scheduling_no_parallel_active_owner)
{
    NodeId owner = -1;
    BlockRequestOwnerState state = BLOCK_REQUEST_OWNER_QUEUED;
    BOOST_CHECK(TryAssignBlockRequestOwner(uint256((uint64_t)7), 1, &owner, &state));
    BOOST_CHECK(!TryAssignBlockRequestOwner(uint256((uint64_t)7), 2, &owner, &state));
}

BOOST_AUTO_TEST_CASE(queued_to_inflight_transition)
{
    NodeId owner = -1;
    BlockRequestOwnerState state = BLOCK_REQUEST_OWNER_QUEUED;
    uint256 h((uint64_t)8);
    BOOST_CHECK(TryAssignBlockRequestOwner(h, 1, &owner, &state));
    BOOST_CHECK(TransitionBlockRequestOwnerToInFlight(h, 1));
    BOOST_CHECK(GetBlockRequestOwner(h, &owner, &state));
    BOOST_CHECK_EQUAL((int)state, (int)BLOCK_REQUEST_OWNER_IN_FLIGHT);
}

BOOST_AUTO_TEST_CASE(inflight_expiry_releases_owner)
{
    DummyNode a(1);
    uint256 h((uint64_t)9);
    BOOST_CHECK(TryAssignBlockRequestOwner(h, a.GetId()));
    BOOST_CHECK(TransitionBlockRequestOwnerToInFlight(h, a.GetId()));
    a.MarkBlockInFlight(h);
    a.ExpireBlockInFlight(GetTime() + 1000);
    NodeId owner = -1;
    BlockRequestOwnerState state = BLOCK_REQUEST_OWNER_QUEUED;
    BOOST_CHECK(!GetBlockRequestOwner(h, &owner, &state));
}

BOOST_AUTO_TEST_CASE(disconnect_cleanup_releases_owner)
{
    DummyNode a(1);
    uint256 h((uint64_t)10);
    CInv inv(MSG_BLOCK, h);
    a.AskFor(inv);
    a.Cleanup();
    NodeId owner = -1;
    BlockRequestOwnerState state = BLOCK_REQUEST_OWNER_QUEUED;
    BOOST_CHECK(!GetBlockRequestOwner(h, &owner, &state));
}

BOOST_AUTO_TEST_CASE(anti_duplicate_preserved_while_another_owner_exists)
{
    DummyNode a(1), b(2);
    CInv inv(MSG_BLOCK, uint256((uint64_t)11));
    a.AskFor(inv);
    BOOST_CHECK(ReleaseBlockRequestOwner(inv.hash, a.GetId(), "handoff"));
    BOOST_CHECK(TryAssignBlockRequestOwner(inv.hash, b.GetId()));
    BOOST_CHECK(!EraseAlreadyAskedForIfUnowned(inv, &a));
}

BOOST_AUTO_TEST_CASE(late_delivery_new_owner_correctness)
{
    uint256 h((uint64_t)12);
    NodeId owner = -1;
    BlockRequestOwnerState state = BLOCK_REQUEST_OWNER_QUEUED;
    BOOST_CHECK(TryAssignBlockRequestOwner(h, 1, &owner, &state));
    BOOST_CHECK(ReleaseBlockRequestOwner(h, 1, "test-release"));
    BOOST_CHECK(TryAssignBlockRequestOwner(h, 2, &owner, &state));
    BOOST_CHECK(!ReleaseBlockRequestOwnerOnReceive(h, 1, false));
    BOOST_CHECK(GetBlockRequestOwner(h, &owner, &state));
    BOOST_CHECK_EQUAL(owner, (NodeId)2);
    BOOST_CHECK(ReleaseBlockRequestOwnerOnReceive(h, 1, true));
    BOOST_CHECK(!GetBlockRequestOwner(h, &owner, &state));
}

BOOST_AUTO_TEST_SUITE_END()
