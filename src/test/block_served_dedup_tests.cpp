#define BOOST_TEST_DYN_LINK

#include <boost/test/unit_test.hpp>

#include "main.h"
#include "net.h"

#include <memory>

BOOST_AUTO_TEST_SUITE(block_served_dedup_tests)

static uint256 H(unsigned long long v) { return uint256(v); }

static CNode* MakeReadyPeer()
{
    return new CNode(INVALID_SOCKET, CAddress(), "ts1a-dedup", false);
}

BOOST_AUTO_TEST_CASE(first_request_served_normally)
{
    // A fresh peer has no served-getblocks fingerprint, so the first request is
    // not suppressed and must be served normally.
    std::unique_ptr<CNode> peer(MakeReadyPeer());
    const uint256 start = H(1), stop = H(2), tip = H(3);
    BOOST_CHECK(!peer->GetBlocksServedFingerprintMatch(start, stop, tip));
    BOOST_CHECK(!peer->fServedGetBlocks);
}

BOOST_AUTO_TEST_CASE(identical_request_unchanged_tip_suppressed)
{
    std::unique_ptr<CNode> peer(MakeReadyPeer());
    const uint256 start = H(1), stop = H(2), tip = H(3);
    peer->RecordServedGetBlocks(start, stop, tip);
    // Identical (start, stop, active-tip) after serving -> suppressed.
    BOOST_CHECK(peer->GetBlocksServedFingerprintMatch(start, stop, tip));
}

BOOST_AUTO_TEST_CASE(tip_advance_invalidates)
{
    std::unique_ptr<CNode> peer(MakeReadyPeer());
    const uint256 start = H(1), stop = H(2);
    peer->RecordServedGetBlocks(start, stop, H(30));
    // Active tip advances -> must serve again.
    BOOST_CHECK(!peer->GetBlocksServedFingerprintMatch(start, stop, H(31)));
}

BOOST_AUTO_TEST_CASE(same_height_reorg_invalidates)
{
    std::unique_ptr<CNode> peer(MakeReadyPeer());
    const uint256 start = H(1), stop = H(2);
    peer->RecordServedGetBlocks(start, stop, H(40));
    // A different tip hash at the same height (reorg) must also invalidate,
    // because the active tip is keyed by HASH, not by height.
    BOOST_CHECK(!peer->GetBlocksServedFingerprintMatch(start, stop, H(41)));
}

BOOST_AUTO_TEST_CASE(hashstop_change_invalidates)
{
    std::unique_ptr<CNode> peer(MakeReadyPeer());
    const uint256 start = H(1), tip = H(3);
    peer->RecordServedGetBlocks(start, H(20), tip);
    // Same start/tip but a different stop hash -> must serve again.
    BOOST_CHECK(!peer->GetBlocksServedFingerprintMatch(start, H(21), tip));
}

BOOST_AUTO_TEST_CASE(reconnect_resets_state)
{
    std::unique_ptr<CNode> peer1(MakeReadyPeer());
    peer1->RecordServedGetBlocks(H(1), H(2), H(3));
    // A brand-new CNode (reconnect) has no inherited served fingerprint.
    std::unique_ptr<CNode> peer2(MakeReadyPeer());
    BOOST_CHECK(!peer2->GetBlocksServedFingerprintMatch(H(1), H(2), H(3)));
    // peer1's own state is unaffected.
    BOOST_CHECK(peer1->GetBlocksServedFingerprintMatch(H(1), H(2), H(3)));
}

BOOST_AUTO_TEST_CASE(continuation_not_suppressed)
{
    // After a peer advances its locator (resolved start changes), the new
    // request is a legitimate continuation and must NOT be suppressed.
    std::unique_ptr<CNode> peer(MakeReadyPeer());
    const uint256 stop = H(100), tip = H(3);
    peer->RecordServedGetBlocks(H(50), stop, tip);
    BOOST_CHECK(!peer->GetBlocksServedFingerprintMatch(H(60), stop, tip));
}

BOOST_AUTO_TEST_CASE(methods_do_not_touch_inventory)
{
    // The suppression decision must not mutate inventory, queueing, or
    // ordering: Record/Match only update/read the served fingerprint.
    std::unique_ptr<CNode> peer(MakeReadyPeer());
    peer->vInventoryToSend.push_back(CInv(MSG_BLOCK, H(9)));
    const size_t nBefore = peer->vInventoryToSend.size();
    peer->RecordServedGetBlocks(H(1), H(2), H(3));
    BOOST_CHECK(peer->GetBlocksServedFingerprintMatch(H(1), H(2), H(3)));
    BOOST_CHECK(!peer->GetBlocksServedFingerprintMatch(H(5), H(2), H(3)));
    BOOST_CHECK_EQUAL(peer->vInventoryToSend.size(), nBefore);
    // Ordering of the queued inventory is untouched.
    BOOST_CHECK(peer->vInventoryToSend[0].hash == H(9));
}

BOOST_AUTO_TEST_CASE(unresolved_start_is_never_suppressed)
{
    // The handler only records a fingerprint when it actually has a serve range
    // (fHadServeRange), so an unresolved/empty locator (start == 0) must never
    // match a previously recorded real start.
    std::unique_ptr<CNode> peer(MakeReadyPeer());
    peer->RecordServedGetBlocks(H(1), H(2), H(3));
    BOOST_CHECK(!peer->GetBlocksServedFingerprintMatch(uint256(0), H(2), H(3)));
}

BOOST_AUTO_TEST_SUITE_END()