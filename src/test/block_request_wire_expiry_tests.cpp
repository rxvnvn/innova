#define BOOST_TEST_DYN_LINK

#include <boost/test/unit_test.hpp>

#include "main.h"
#include "net.h"
#include "util.h"

#include <memory>

BOOST_AUTO_TEST_SUITE(block_request_wire_expiry_tests)

// Builds a bare peer with a nonzero protocol version so the send path and
// wire-origin expiry machinery can be exercised directly.
static CNode* MakeReadyPeer()
{
    CNode* peer = new CNode(INVALID_SOCKET, CAddress(), "t1", false);
    peer->nVersion = PROTOCOL_VERSION;
    return peer;
}

static uint256 H(unsigned long long v) { return uint256((uint64_t)v); }

// Snapshot/restore global already-asked state for suite isolation.
class CScopedAlreadyAsked
{
private:
    std::map<CInv, int64_t> saved;
public:
    CScopedAlreadyAsked()
    {
        LOCK(cs_mapAlreadyAskedFor);
        saved = mapAlreadyAskedFor;
        mapAlreadyAskedFor.clear();
    }
    ~CScopedAlreadyAsked()
    {
        LOCK(cs_mapAlreadyAskedFor);
        mapAlreadyAskedFor = saved;
    }
};

BOOST_AUTO_TEST_CASE(queued_request_does_not_use_delivery_timeout)
{
    CScopedAlreadyAsked isIdx;
    std::unique_ptr<CNode> peer(MakeReadyPeer());
    const uint256 hashP = H(7001);
    peer->mapAskFor.insert(std::make_pair(GetTime() * 1000000LL, CInv(MSG_BLOCK, hashP)));

    // A request that is only queued (not marked inflight) must not be expired
    // by the delivery-timeout machinery even long after construction.
    peer->ExpireBlockInFlight(GetTime() + 1000000);
    BOOST_CHECK_EQUAL(peer->mapAskFor.size(), 1U);
    BOOST_CHECK(peer->mapAskFor.begin()->second.hash == hashP);
    BOOST_CHECK_EQUAL(peer->setBlocksInFlight.size(), 0U);
    BOOST_CHECK_EQUAL(peer->mapBlockInFlightSince.size(), 0U);
}

BOOST_AUTO_TEST_CASE(pending_wire_request_is_bounded)
{
    CScopedAlreadyAsked isIdx;
    std::unique_ptr<CNode> peer(MakeReadyPeer());
    const uint256 hashP = H(7002);
    const int64_t nMarkSec = GetTime();
    peer->MarkBlockInFlight(hashP);
    // Force the mark time so the pending bound is deterministic.
    peer->mapBlockInFlightSince[hashP] = nMarkSec;

    // No socket write yet: within the 1s pending-wire bound it must survive.
    peer->ExpireBlockInFlight(nMarkSec);
    BOOST_CHECK_EQUAL(peer->setBlocksInFlight.size(), 1U);
    BOOST_CHECK_EQUAL(peer->mapBlockInFlightSince.size(), 1U);

    // Still pending: a wire deadline must NOT have started (no wire timestamp).
    {
        LOCK(peer->cs_vBlockInFlightWire);
        BOOST_CHECK(peer->mapBlockInFlightWireUs.count(hashP) == 1U);
        BOOST_CHECK_EQUAL(peer->mapBlockInFlightWireUs[hashP], 0);
    }

    // Past the pending-wire bound (still no wire write): must expire.
    peer->ExpireBlockInFlight(nMarkSec + 2);
    BOOST_CHECK_EQUAL(peer->setBlocksInFlight.size(), 0U);
    BOOST_CHECK_EQUAL(peer->mapBlockInFlightSince.size(), 0U);
    {
        LOCK(peer->cs_vBlockInFlightWire);
        BOOST_CHECK_EQUAL(peer->mapBlockInFlightWireUs.count(hashP), 0U);
    }
}

BOOST_AUTO_TEST_CASE(first_socket_write_starts_wire_clock)
{
    CScopedAlreadyAsked isIdx;
    std::unique_ptr<CNode> peer(MakeReadyPeer());
    const uint256 hashP = H(7003);
    peer->MarkBlockInFlight(hashP);

    // Before the first socket write there is no wire-origin timestamp (pending).
    {
        LOCK(peer->cs_vBlockInFlightWire);
        BOOST_CHECK(peer->mapBlockInFlightWireUs.count(hashP) == 1U);
        BOOST_CHECK_EQUAL(peer->mapBlockInFlightWireUs[hashP], 0);
    }

    // First actual socket transmission stamps the wire-origin for H.
    const int64_t nWireSec = GetTime();
    peer->StampBlockInFlightWireTimes(std::vector<uint256>(1, hashP), nWireSec);
    {
        LOCK(peer->cs_vBlockInFlightWire);
        BOOST_CHECK_EQUAL(peer->mapBlockInFlightWireUs[hashP], nWireSec);
    }
}

BOOST_AUTO_TEST_CASE(wire_origin_deadline)
{
    CScopedAlreadyAsked isIdx;
    std::unique_ptr<CNode> peer(MakeReadyPeer());
    const uint256 hashP = H(7004);
    const int64_t nMarkSec = GetTime();
    peer->MarkBlockInFlight(hashP);
    peer->mapBlockInFlightSince[hashP] = nMarkSec;

    const int64_t nWireSec = nMarkSec + 100;
    peer->StampBlockInFlightWireTimes(std::vector<uint256>(1, hashP), nWireSec);

    // Live before the 60s wire deadline (after the 1s pending bound).
    peer->ExpireBlockInFlight(nWireSec + 5);
    BOOST_CHECK_EQUAL(peer->setBlocksInFlight.size(), 1U);

    // Dead after the 60s wire deadline.
    peer->ExpireBlockInFlight(nWireSec + 61);
    BOOST_CHECK_EQUAL(peer->setBlocksInFlight.size(), 0U);
    {
        LOCK(peer->cs_vBlockInFlightWire);
        BOOST_CHECK_EQUAL(peer->mapBlockInFlightWireUs.count(hashP), 0U);
    }
}

BOOST_AUTO_TEST_CASE(per_hash_wire_attribution)
{
    CScopedAlreadyAsked isIdx;
    std::unique_ptr<CNode> peer(MakeReadyPeer());
    const uint256 hashP = H(7011);
    const uint256 hashQ = H(7012);
    const int64_t nMarkSec = GetTime();
    peer->MarkBlockInFlight(hashP);
    peer->MarkBlockInFlight(hashQ);
    peer->mapBlockInFlightSince[hashP] = nMarkSec;
    peer->mapBlockInFlightSince[hashQ] = nMarkSec;

    // Different wire times per hash: a later wire write must not clobber an
    // earlier one, so each hash's deadline is attributed independently.
    const int64_t nWireP = nMarkSec + 100;
    const int64_t nWireQ = nMarkSec + 200;
    std::vector<uint256> both;
    both.push_back(hashP);
    both.push_back(hashQ);
    peer->StampBlockInFlightWireTimes(both, nWireP); // stamps both to nWireP
    peer->StampBlockInFlightWireTimes(std::vector<uint256>(1, hashQ), nWireQ);

    {
        LOCK(peer->cs_vBlockInFlightWire);
        BOOST_CHECK_EQUAL(peer->mapBlockInFlightWireUs[hashP], nWireP);
        BOOST_CHECK_EQUAL(peer->mapBlockInFlightWireUs[hashQ], nWireQ);
    }

    // At nWireP+61, H(P) expires but H(Q) (wire at nWireQ, still within its
    // 60s window) stays live.
    peer->ExpireBlockInFlight(nWireP + 61);
    BOOST_CHECK_EQUAL(peer->setBlocksInFlight.count(hashP), 0U);
    BOOST_CHECK_EQUAL(peer->setBlocksInFlight.count(hashQ), 1U);
    {
        LOCK(peer->cs_vBlockInFlightWire);
        BOOST_CHECK_EQUAL(peer->mapBlockInFlightWireUs.count(hashP), 0U);
        BOOST_CHECK_EQUAL(peer->mapBlockInFlightWireUs[hashQ], nWireQ);
    }
}

BOOST_AUTO_TEST_CASE(cleanup_on_receive_releases_wire_state)
{
    CScopedAlreadyAsked isIdx;
    std::unique_ptr<CNode> peer(MakeReadyPeer());
    const uint256 hashP = H(7021);
    peer->MarkBlockInFlight(hashP);
    peer->StampBlockInFlightWireTimes(std::vector<uint256>(1, hashP), GetTime());

    peer->ClearBlockInFlight(hashP);
    BOOST_CHECK_EQUAL(peer->setBlocksInFlight.count(hashP), 0U);
    BOOST_CHECK_EQUAL(peer->mapBlockInFlightSince.count(hashP), 0U);
    {
        LOCK(peer->cs_vBlockInFlightWire);
        BOOST_CHECK_EQUAL(peer->mapBlockInFlightWireUs.count(hashP), 0U);
    }
}

BOOST_AUTO_TEST_CASE(cleanup_on_timeout_releases_wire_state)
{
    CScopedAlreadyAsked isIdx;
    std::unique_ptr<CNode> peer(MakeReadyPeer());
    const uint256 hashP = H(7022);
    const int64_t nMarkSec = GetTime();
    peer->MarkBlockInFlight(hashP);
    peer->mapBlockInFlightSince[hashP] = nMarkSec;
    peer->StampBlockInFlightWireTimes(std::vector<uint256>(1, hashP), nMarkSec);

    peer->ExpireBlockInFlight(nMarkSec + 61);
    BOOST_CHECK_EQUAL(peer->setBlocksInFlight.count(hashP), 0U);
    {
        LOCK(peer->cs_vBlockInFlightWire);
        BOOST_CHECK_EQUAL(peer->mapBlockInFlightWireUs.count(hashP), 0U);
    }
}

BOOST_AUTO_TEST_CASE(cleanup_on_disconnect_releases_wire_state)
{
    CScopedAlreadyAsked isIdx;
    std::unique_ptr<CNode> peer(MakeReadyPeer());
    const uint256 hashP = H(7023);
    peer->MarkBlockInFlight(hashP);
    peer->StampBlockInFlightWireTimes(std::vector<uint256>(1, hashP), GetTime());

    peer->Cleanup();
    BOOST_CHECK_EQUAL(peer->setBlocksInFlight.size(), 0U);
    BOOST_CHECK_EQUAL(peer->mapBlockInFlightSince.size(), 0U);
    {
        LOCK(peer->cs_vBlockInFlightWire);
        BOOST_CHECK_EQUAL(peer->mapBlockInFlightWireUs.size(), 0U);
    }
}

BOOST_AUTO_TEST_CASE(no_request_loss_from_wire_timing)
{
    CScopedAlreadyAsked isIdx;
    std::unique_ptr<CNode> peer(MakeReadyPeer());
    const uint256 hashP = H(7031);
    const uint256 hashQ = H(7032);
    peer->mapAskFor.insert(std::make_pair(GetTime() * 1000000LL, CInv(MSG_BLOCK, hashP)));
    peer->mapAskFor.insert(std::make_pair(GetTime() * 1000000LL + 1, CInv(MSG_BLOCK, hashQ)));
    const int64_t nMarkSec = GetTime();
    peer->MarkBlockInFlight(hashP);
    peer->mapBlockInFlightSince[hashP] = nMarkSec;
    peer->StampBlockInFlightWireTimes(std::vector<uint256>(1, hashP), nMarkSec);

    peer->ExpireBlockInFlight(nMarkSec + 61);

    // Expiry of the in-flight entry must not drop or disturb the queued work.
    BOOST_CHECK_EQUAL(peer->mapAskFor.size(), 2U);
    BOOST_CHECK_EQUAL(peer->setBlocksInFlight.count(hashP), 0U);
    BOOST_CHECK_EQUAL(peer->setBlocksInFlight.count(hashQ), 0U);
}

BOOST_AUTO_TEST_CASE(wire_timing_does_not_create_duplicate_inflight_entries)
{
    CScopedAlreadyAsked isIdx;
    std::unique_ptr<CNode> peer(MakeReadyPeer());
    const uint256 hashP = H(7041);
    const int64_t nMarkSec = GetTime();
    peer->MarkBlockInFlight(hashP);
    peer->mapBlockInFlightSince[hashP] = nMarkSec;

    // Repeated stamping of the same hash must not create duplicate inflight
    // entries nor duplicate pending timestamps.
    peer->StampBlockInFlightWireTimes(std::vector<uint256>(1, hashP), nMarkSec + 1);
    peer->StampBlockInFlightWireTimes(std::vector<uint256>(1, hashP), nMarkSec + 2);
    peer->ExpireBlockInFlight(nMarkSec + 2);
    BOOST_CHECK_EQUAL(peer->setBlocksInFlight.count(hashP), 1U);
    {
        LOCK(peer->cs_vBlockInFlightWire);
        BOOST_CHECK_EQUAL(peer->mapBlockInFlightWireUs.count(hashP), 1U);
    }
}

BOOST_AUTO_TEST_SUITE_END()