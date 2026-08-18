// Copyright (c) 2026 The Innova developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <map>
#include <vector>

#include <boost/test/unit_test.hpp>

#include "headersservededup.h"
#include "main.h"
#include "net.h"
#include "protocol.h"
#include "util.h"
#include "version.h"

namespace {

static const int64_t TTL_US = HEADER_SERVED_DEDUP_TTL * 1000000;
static const int64_t NOW = 1000000LL * 1000000;

// Distinct active-chain tip hashes used to model response-context changes:
// TIP_A is the "current" tip, TIP_ADV is an advance on top of it, and
// TIP_REORG is a reorg/fork to an unrelated tip.
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

static CBlockLocator Loc(const std::vector<uint256>& vHave)
{
    return CBlockLocator(vHave);
}

// Mirror of the production suppression decision in the getheaders handler:
// the request is suppressed iff a fresh entry exists for the fingerprint.
static bool WouldSuppress(const CNode& node, const uint256& fp, int64_t nNowUs)
{
    std::map<uint256, CNode::CHeadersServedDedupEntry>::const_iterator it =
        node.mapHeadersServedDedup.find(fp);
    return it != node.mapHeadersServedDedup.end() &&
           HeaderServedDedupEntryFresh(it->second, nNowUs);
}

// Mirror of a served response: fingerprint is computed from the full
// (locator, hashStop, tipHash) triple (the handler computes it under
// cs_main; tests pass an explicit tip argument) and upserted.
static void Serve(CNode& node, const CBlockLocator& locator,
                  const uint256& hashStop, const uint256& tipHash,
                  int64_t nNowUs, uint32_t nHeadersCount, uint64_t nBytes)
{
    const uint256 fp = HeaderServedDedupFingerprint(locator, hashStop, tipHash);
    HeaderServedDedupUpsert(node.mapHeadersServedDedup, fp, nNowUs,
                            nHeadersCount, nBytes);
}

} // namespace

BOOST_AUTO_TEST_SUITE(headersservededup_tests)

// T1: identical (locator, hashStop, tip) twice within TTL -> first served,
//     second suppressed.
BOOST_AUTO_TEST_CASE(t1_identical_within_ttl_suppressed)
{
    CNode peer(INVALID_SOCKET, TestPeerAddress(1), "dedup-t1", true);
    const CBlockLocator locator = Loc(std::vector<uint256>{H(100), H(90), H(80)});
    const uint256 hashStop = H(999);
    const uint256 fp = HeaderServedDedupFingerprint(locator, hashStop, TIP_A);

    // First request: no entry yet -> served (not suppressed).
    BOOST_CHECK(!WouldSuppress(peer, fp, NOW));
    Serve(peer, locator, hashStop, TIP_A, NOW, 10, 1234);
    std::map<uint256, CNode::CHeadersServedDedupEntry>::const_iterator it =
        peer.mapHeadersServedDedup.find(fp);
    BOOST_REQUIRE(it != peer.mapHeadersServedDedup.end());
    BOOST_CHECK_EQUAL(it->second.nRepeat, 1u);
    BOOST_CHECK_EQUAL(it->second.nHeadersCount, 10u);
    BOOST_CHECK_EQUAL(it->second.nBytes, 1234u);

    // Second identical request 1s later: suppressed.
    BOOST_CHECK(WouldSuppress(peer, fp, NOW + 1000000));
}

// T2: identical locator after TTL -> served again.
BOOST_AUTO_TEST_CASE(t2_identical_after_ttl_served)
{
    CNode peer(INVALID_SOCKET, TestPeerAddress(2), "dedup-t2", true);
    const CBlockLocator locator = Loc(std::vector<uint256>{H(100), H(90)});
    const uint256 hashStop = H(998);
    const uint256 fp = HeaderServedDedupFingerprint(locator, hashStop, TIP_A);

    Serve(peer, locator, hashStop, TIP_A, NOW, 5, 600);
    BOOST_CHECK(WouldSuppress(peer, fp, NOW + 1000000));

    // Just past the TTL: no longer fresh -> served again.  The expired entry
    // is purged by the upsert, so the refreshed entry starts a new cycle.
    BOOST_CHECK(!WouldSuppress(peer, fp, NOW + TTL_US + 1));
    Serve(peer, locator, hashStop, TIP_A, NOW + TTL_US + 1, 5, 600);
    std::map<uint256, CNode::CHeadersServedDedupEntry>::const_iterator it =
        peer.mapHeadersServedDedup.find(fp);
    BOOST_REQUIRE(it != peer.mapHeadersServedDedup.end());
    BOOST_CHECK_EQUAL(it->second.nRepeat, 1u);
    BOOST_CHECK_EQUAL(it->second.nServedUs, NOW + TTL_US + 1);
    // Refreshed timestamp suppresses the next identical request again.
    BOOST_CHECK(WouldSuppress(peer, fp, NOW + TTL_US + 1 + 1000000));
}

// T3: monotonically advancing locators -> zero suppression.
BOOST_AUTO_TEST_CASE(t3_advancing_locators_no_suppression)
{
    CNode peer(INVALID_SOCKET, TestPeerAddress(3), "dedup-t3", true);
    const uint256 hashStop = H(997);
    int64_t nNow = NOW;
    std::vector<std::pair<uint256, int64_t> > served;
    for (size_t i = 1; i <= 20; ++i)
    {
        std::vector<uint256> vHave;
        for (size_t k = i + 1; k >= 1; --k)
            vHave.push_back(H(k));
        const CBlockLocator locator = Loc(vHave);
        const uint256 fp = HeaderServedDedupFingerprint(locator, hashStop, TIP_A);
        // Genuinely distinct key each step -> never suppressed.
        BOOST_CHECK(!WouldSuppress(peer, fp, nNow));
        Serve(peer, locator, hashStop, TIP_A, nNow, 8, 1000);
        served.push_back(std::make_pair(fp, nNow));
        nNow += 1000000;
    }
    // Only entries still inside the TTL survive the upsert purge (the last
    // purge ran at the final serve time): with a 1s step and an 8s TTL the
    // map naturally holds the most recent 8 frames.
    size_t nRetained = 0;
    const int64_t nLastServe = served.back().second;
    for (size_t i = 0; i < served.size(); ++i)
        if (nLastServe - served[i].second < TTL_US)
            ++nRetained;
    BOOST_CHECK_EQUAL(peer.mapHeadersServedDedup.size(), nRetained);
    BOOST_CHECK(peer.mapHeadersServedDedup.count(served.back().first) == 1);
}

// T4: cyclic A->B->A -> repeat of A within TTL is suppressed.
BOOST_AUTO_TEST_CASE(t4_cyclic_repeat_suppressed)
{
    CNode peer(INVALID_SOCKET, TestPeerAddress(4), "dedup-t4", true);
    const CBlockLocator locA = Loc(std::vector<uint256>{H(100)});
    const CBlockLocator locB = Loc(std::vector<uint256>{H(200)});
    const CBlockLocator locC = Loc(std::vector<uint256>{H(300)});
    const uint256 hashStop = H(996);
    const uint256 fpA = HeaderServedDedupFingerprint(locA, hashStop, TIP_A);
    const uint256 fpB = HeaderServedDedupFingerprint(locB, hashStop, TIP_A);

    Serve(peer, locA, hashStop, TIP_A, NOW, 4, 500);
    Serve(peer, locB, hashStop, TIP_A, NOW + 1000000, 4, 500);
    Serve(peer, locC, hashStop, TIP_A, NOW + 2000000, 4, 500);

    // Re-ask A within TTL: suppressed.
    BOOST_CHECK(WouldSuppress(peer, fpA, NOW + 3000000));
    BOOST_CHECK(WouldSuppress(peer, fpB, NOW + 3000000));
}

// T5: rewind A->B->older-A with a GENUINELY different locator is not
//     identical, so it is served.
BOOST_AUTO_TEST_CASE(t5_rewind_different_locator_served)
{
    CNode peer(INVALID_SOCKET, TestPeerAddress(5), "dedup-t5", true);
    const CBlockLocator locA = Loc(std::vector<uint256>{H(100)});
    const CBlockLocator locB = Loc(std::vector<uint256>{H(200)});
    const CBlockLocator locOlderA = Loc(std::vector<uint256>{H(100), H(90)});
    const uint256 hashStop = H(995);

    const uint256 fpA = HeaderServedDedupFingerprint(locA, hashStop, TIP_A);
    const uint256 fpOlderA = HeaderServedDedupFingerprint(locOlderA, hashStop, TIP_A);
    BOOST_CHECK(fpA != fpOlderA);

    Serve(peer, locA, hashStop, TIP_A, NOW, 2, 300);
    Serve(peer, locB, hashStop, TIP_A, NOW + 1000000, 2, 300);
    // Older-A rewind is a different fingerprint -> served, not suppressed.
    BOOST_CHECK(!WouldSuppress(peer, fpOlderA, NOW + 2000000));
    Serve(peer, locOlderA, hashStop, TIP_A, NOW + 2000000, 3, 450);
    BOOST_CHECK(WouldSuppress(peer, fpOlderA, NOW + 3000000));
}

// T6: fork/reorg locator change -> different fingerprint -> served.
BOOST_AUTO_TEST_CASE(t6_fork_locator_served)
{
    CNode peer(INVALID_SOCKET, TestPeerAddress(6), "dedup-t6", true);
    const CBlockLocator locMain = Loc(std::vector<uint256>{H(100), H(90), H(80)});
    const CBlockLocator locFork = Loc(std::vector<uint256>{H(100), H(95), H(80)});
    const uint256 hashStop = H(994);

    const uint256 fpMain = HeaderServedDedupFingerprint(locMain, hashStop, TIP_A);
    const uint256 fpFork = HeaderServedDedupFingerprint(locFork, hashStop, TIP_A);
    BOOST_CHECK(fpMain != fpFork);

    Serve(peer, locMain, hashStop, TIP_A, NOW, 7, 900);
    BOOST_CHECK(!WouldSuppress(peer, fpFork, NOW + 1000000));
    Serve(peer, locFork, hashStop, TIP_A, NOW + 1000000, 7, 900);
    // Re-ask of either fingerprint within the TTL is suppressed (identical
    // locator+hashStop+tip); the fork change itself was served because its
    // fingerprint differed.
    BOOST_CHECK(WouldSuppress(peer, fpFork, NOW + 2000000));
    BOOST_CHECK(WouldSuppress(peer, fpMain, NOW + 2000000));
    // After the TTL both may be served again (fork served at NOW+1s).
    BOOST_CHECK(!WouldSuppress(peer, fpMain, NOW + TTL_US + 1));
    BOOST_CHECK(!WouldSuppress(peer, fpFork, NOW + TTL_US + 1000000 + 1));
}

// T7: two DIFFERENT peers, same locator -> independent, both served.
BOOST_AUTO_TEST_CASE(t7_peers_independent)
{
    CNode peerA(INVALID_SOCKET, TestPeerAddress(7), "dedup-t7-a", true);
    CNode peerB(INVALID_SOCKET, TestPeerAddress(8), "dedup-t7-b", true);
    const CBlockLocator locator = Loc(std::vector<uint256>{H(100), H(90)});
    const uint256 hashStop = H(993);
    const uint256 fp = HeaderServedDedupFingerprint(locator, hashStop, TIP_A);

    Serve(peerA, locator, hashStop, TIP_A, NOW, 6, 700);
    // Peer B has no state at all -> its request is served.
    BOOST_CHECK(!WouldSuppress(peerB, fp, NOW + 1000000));
    Serve(peerB, locator, hashStop, TIP_A, NOW + 1000000, 6, 700);
    BOOST_CHECK(WouldSuppress(peerB, fp, NOW + 2000000));
    BOOST_CHECK_EQUAL(peerA.mapHeadersServedDedup.size(), 1u);
    BOOST_CHECK_EQUAL(peerB.mapHeadersServedDedup.size(), 1u);
}

// T8: disconnect/reconnect -> a new CNode does NOT inherit dedup state.
BOOST_AUTO_TEST_CASE(t8_reconnect_fresh_state)
{
    CNode peerOld(INVALID_SOCKET, TestPeerAddress(9), "dedup-t8-old", true);
    const CBlockLocator locator = Loc(std::vector<uint256>{H(100)});
    const uint256 hashStop = H(992);
    const uint256 fp = HeaderServedDedupFingerprint(locator, hashStop, TIP_A);

    Serve(peerOld, locator, hashStop, TIP_A, NOW, 4, 400);
    BOOST_CHECK(WouldSuppress(peerOld, fp, NOW + 1000000));

    // Reconnect: brand new CNode object, same address semantics, no state.
    CNode peerNew(INVALID_SOCKET, TestPeerAddress(9), "dedup-t8-new", true);
    BOOST_CHECK(peerNew.mapHeadersServedDedup.empty());
    BOOST_CHECK(!WouldSuppress(peerNew, fp, NOW + 1000000));
    Serve(peerNew, locator, hashStop, TIP_A, NOW + 1000000, 4, 400);
    BOOST_CHECK(WouldSuppress(peerNew, fp, NOW + 2000000));
}

// T9: bounded state: loading more than the cap never grows unbounded;
//     cap and oldest-entry eviction are honored.
BOOST_AUTO_TEST_CASE(t9_bounded_state_cap_eviction)
{
    CNode peer(INVALID_SOCKET, TestPeerAddress(10), "dedup-t9", true);
    const uint256 hashStop = H(991);
    const int64_t STEP = 1000000;
    std::vector<uint256> fps;
    int64_t nNow = NOW;
    for (size_t i = 0; i < HEADER_SERVED_DEDUP_CAP * 3; ++i)
    {
        const CBlockLocator locator = Loc(std::vector<uint256>{H(i + 1)});
        const uint256 fp = HeaderServedDedupFingerprint(locator, hashStop, TIP_A);
        fps.push_back(fp);
        Serve(peer, locator, hashStop, TIP_A, nNow, 2, 250);
        // Never exceeds the cap (TTL purging also trims expired entries).
        BOOST_CHECK(peer.mapHeadersServedDedup.size() <= HEADER_SERVED_DEDUP_CAP);
        nNow += STEP;
    }
    // Bounded: never exceeds the cap.
    BOOST_CHECK(peer.mapHeadersServedDedup.size() <= HEADER_SERVED_DEDUP_CAP);

    // The oldest entries were purged/evicted; the most recent survive.
    BOOST_CHECK(peer.mapHeadersServedDedup.count(fps[0]) == 0);
    BOOST_CHECK(peer.mapHeadersServedDedup.count(fps.back()) == 1);

    // Explicit cap-eviction check without TTL purging: fill a fresh peer to
    // the cap with strictly increasing serve times, then add one more -> the
    // oldest entry (fps2[0]) is evicted, size stays at the cap.
    CNode peer2(INVALID_SOCKET, TestPeerAddress(11), "dedup-t9b", true);
    std::vector<uint256> fps2;
    for (size_t i = 0; i < HEADER_SERVED_DEDUP_CAP; ++i)
    {
        const CBlockLocator locator = Loc(std::vector<uint256>{H(1000 + i)});
        const uint256 fp = HeaderServedDedupFingerprint(locator, hashStop, TIP_A);
        fps2.push_back(fp);
        Serve(peer2, locator, hashStop, TIP_A, NOW + static_cast<int64_t>(i), 2, 250);
    }
    BOOST_CHECK_EQUAL(peer2.mapHeadersServedDedup.size(), HEADER_SERVED_DEDUP_CAP);

    const CBlockLocator locNew = Loc(std::vector<uint256>{H(9999)});
    const uint256 fpNew = HeaderServedDedupFingerprint(locNew, hashStop, TIP_A);
    Serve(peer2, locNew, hashStop, TIP_A, NOW + HEADER_SERVED_DEDUP_CAP, 2, 250);
    BOOST_CHECK_EQUAL(peer2.mapHeadersServedDedup.size(), HEADER_SERVED_DEDUP_CAP);
    BOOST_CHECK(peer2.mapHeadersServedDedup.count(fps2[0]) == 0);
    BOOST_CHECK(peer2.mapHeadersServedDedup.count(fpNew) == 1);

    // Refreshing an existing fingerprint at capacity does not grow the map.
    HeaderServedDedupUpsert(peer2.mapHeadersServedDedup, fpNew,
                            NOW + 1000000, 9, 999);
    BOOST_CHECK_EQUAL(peer2.mapHeadersServedDedup.size(), HEADER_SERVED_DEDUP_CAP);
    BOOST_CHECK_EQUAL(peer2.mapHeadersServedDedup[fpNew].nRepeat, 2u);
}

// Flag toggle semantics: default off, and the module-scope setter controls it.
BOOST_AUTO_TEST_CASE(t10_flag_toggle)
{
    InitHeadersServedDedup(false);
    BOOST_CHECK(!HeadersServedDedupEnabled());
    InitHeadersServedDedup(true);
    BOOST_CHECK(HeadersServedDedupEnabled());
    InitHeadersServedDedup(false);
    BOOST_CHECK(!HeadersServedDedupEnabled());
}

// R1: identical locator + SAME tip inside TTL -> suppressed.
BOOST_AUTO_TEST_CASE(r1_same_tip_inside_ttl_suppressed)
{
    CNode peer(INVALID_SOCKET, TestPeerAddress(21), "dedup-r1", true);
    const CBlockLocator locator = Loc(std::vector<uint256>{H(100), H(90)});
    const uint256 hashStop = H(901);
    const uint256 fp = HeaderServedDedupFingerprint(locator, hashStop, TIP_A);

    Serve(peer, locator, hashStop, TIP_A, NOW, 3, 400);
    BOOST_CHECK(WouldSuppress(peer, fp, NOW + 1000000));
    // Still inside TTL (well below TTL_US, which is < 60s).
    BOOST_CHECK(WouldSuppress(peer, fp, NOW + TTL_US - 1));
}

// R2: identical locator + SAME tip after TTL -> served.
BOOST_AUTO_TEST_CASE(r2_same_tip_after_ttl_served)
{
    CNode peer(INVALID_SOCKET, TestPeerAddress(22), "dedup-r2", true);
    const CBlockLocator locator = Loc(std::vector<uint256>{H(100), H(90)});
    const uint256 hashStop = H(902);
    const uint256 fp = HeaderServedDedupFingerprint(locator, hashStop, TIP_A);

    Serve(peer, locator, hashStop, TIP_A, NOW, 3, 400);
    BOOST_CHECK(!WouldSuppress(peer, fp, NOW + TTL_US + 1));
}

// R3: identical locator + TIP ADVANCED inside TTL -> served (fingerprint
//     includes the tip, so a new tip is a new key).
BOOST_AUTO_TEST_CASE(r3_tip_advanced_inside_ttl_served)
{
    CNode peer(INVALID_SOCKET, TestPeerAddress(23), "dedup-r3", true);
    const CBlockLocator locator = Loc(std::vector<uint256>{H(100), H(90)});
    const uint256 hashStop = H(903);

    Serve(peer, locator, hashStop, TIP_A, NOW, 3, 400);
    const uint256 fpOld = HeaderServedDedupFingerprint(locator, hashStop, TIP_A);
    const uint256 fpNew = HeaderServedDedupFingerprint(locator, hashStop, TIP_ADV);
    BOOST_CHECK(fpOld != fpNew);
    // Same locator/hashStop, but tip advanced 1s after the serve: MUST SERVE.
    BOOST_CHECK(!WouldSuppress(peer, fpNew, NOW + 1000000));
    Serve(peer, locator, hashStop, TIP_ADV, NOW + 1000000, 3, 400);
    // Identical request under the new (unchanged) tip is now suppressed again.
    BOOST_CHECK(WouldSuppress(peer, fpNew, NOW + 2000000));
}

// R4: identical locator + REORG/NEW tip inside TTL -> served.
BOOST_AUTO_TEST_CASE(r4_reorg_new_tip_inside_ttl_served)
{
    CNode peer(INVALID_SOCKET, TestPeerAddress(24), "dedup-r4", true);
    const CBlockLocator locator = Loc(std::vector<uint256>{H(100), H(90)});
    const uint256 hashStop = H(904);

    Serve(peer, locator, hashStop, TIP_A, NOW, 3, 400);
    const uint256 fpReorg = HeaderServedDedupFingerprint(locator, hashStop, TIP_REORG);
    // A reorg swaps the active tip to an unrelated block: MUST SERVE.
    BOOST_CHECK(!WouldSuppress(peer, fpReorg, NOW + 1000000));
    Serve(peer, locator, hashStop, TIP_REORG, NOW + 1000000, 3, 400);
    BOOST_CHECK(WouldSuppress(peer, fpReorg, NOW + 2000000));
}

// R5: identical request after a normal 60s retry interval -> served (a legit
//     lost-response retry lands at ~GETHEADERS_REQUEST_TIMEOUT, far beyond the
//     8s TTL, so it is never suppressed).
BOOST_AUTO_TEST_CASE(r5_normal_60s_retry_served)
{
    CNode peer(INVALID_SOCKET, TestPeerAddress(25), "dedup-r5", true);
    const CBlockLocator locator = Loc(std::vector<uint256>{H(100), H(90)});
    const uint256 hashStop = H(905);
    const uint256 fp = HeaderServedDedupFingerprint(locator, hashStop, TIP_A);

    Serve(peer, locator, hashStop, TIP_A, NOW, 3, 400);
    // The request-timeout retry interval is 60s (net.h).  TTL is far below it.
    BOOST_CHECK(HEADER_SERVED_DEDUP_TTL < GETHEADERS_REQUEST_TIMEOUT);
    BOOST_CHECK(!WouldSuppress(peer, fp,
                               NOW + GETHEADERS_REQUEST_TIMEOUT * 1000000));
    // And the retried request is served again (fresh upsert).
    Serve(peer, locator, hashStop, TIP_A,
          NOW + GETHEADERS_REQUEST_TIMEOUT * 1000000, 3, 400);
    BOOST_CHECK(WouldSuppress(peer, fp,
                              NOW + GETHEADERS_REQUEST_TIMEOUT * 1000000 + 1000000));
}

// R6: EMPTY response + new block/tip arrives -> immediate re-request (same
//     locator/hashStop, new tip) is SERVED.
BOOST_AUTO_TEST_CASE(r6_empty_then_new_tip_served)
{
    CNode peer(INVALID_SOCKET, TestPeerAddress(26), "dedup-r6", true);
    const CBlockLocator locator = Loc(std::vector<uint256>{H(100), H(90)});
    const uint256 hashStop = H(906);

    // Empty response served at TIP_A (e.g. peer already has the tip).
    Serve(peer, locator, hashStop, TIP_A, NOW, 0, 0);
    const uint256 fpOld = HeaderServedDedupFingerprint(locator, hashStop, TIP_A);
    BOOST_CHECK(WouldSuppress(peer, fpOld, NOW + 1000000));

    // A new block arrives on-chain; the peer re-asks with the same
    // locator/hashStop but the active tip has changed: MUST SERVE.
    const uint256 fpNew = HeaderServedDedupFingerprint(locator, hashStop, TIP_ADV);
    BOOST_CHECK(!WouldSuppress(peer, fpNew, NOW + 1000000));
    Serve(peer, locator, hashStop, TIP_ADV, NOW + 1000000, 5, 700);
    BOOST_CHECK(WouldSuppress(peer, fpNew, NOW + 2000000));
}

// R7: cyclic A->B->A at UNCHANGED tip -> repeated A suppressed (within TTL).
BOOST_AUTO_TEST_CASE(r7_cyclic_at_unchanged_tip_suppressed)
{
    CNode peer(INVALID_SOCKET, TestPeerAddress(27), "dedup-r7", true);
    const CBlockLocator locA = Loc(std::vector<uint256>{H(100)});
    const CBlockLocator locB = Loc(std::vector<uint256>{H(200)});
    const uint256 hashStop = H(907);
    const uint256 fpA = HeaderServedDedupFingerprint(locA, hashStop, TIP_A);
    const uint256 fpB = HeaderServedDedupFingerprint(locB, hashStop, TIP_A);

    Serve(peer, locA, hashStop, TIP_A, NOW, 2, 250);
    Serve(peer, locB, hashStop, TIP_A, NOW + 1000000, 2, 250);
    // Repeated A and repeated B within TTL: both suppressed.  The tip never
    // changed, so A's fingerprint is unchanged and its repeat collapses.
    BOOST_CHECK(WouldSuppress(peer, fpA, NOW + 2000000));
    BOOST_CHECK(WouldSuppress(peer, fpB, NOW + 2000000));
    BOOST_CHECK(WouldSuppress(peer, fpA, NOW + 3000000));
}

// R8: advancing locators remain unsuppressed (zero suppression across the
//     whole run even under an unchanged tip).
BOOST_AUTO_TEST_CASE(r8_advancing_locators_zero_suppression)
{
    CNode peer(INVALID_SOCKET, TestPeerAddress(28), "dedup-r8", true);
    const uint256 hashStop = H(908);
    int64_t nNow = NOW;
    std::vector<std::pair<uint256, int64_t> > served;
    for (size_t i = 1; i <= 50; ++i)
    {
        std::vector<uint256> vHave;
        for (size_t k = i; k >= 1; --k)
            vHave.push_back(H(1000 + k));
        const CBlockLocator locator = Loc(vHave);
        const uint256 fp = HeaderServedDedupFingerprint(locator, hashStop, TIP_A);
        BOOST_CHECK(!WouldSuppress(peer, fp, nNow));
        Serve(peer, locator, hashStop, TIP_A, nNow, 4, 500);
        served.push_back(std::make_pair(fp, nNow));
        nNow += 1000000;
    }
    // TTL purge keeps only the frames served within the last TTL (1s step).
    size_t nRetained = 0;
    const int64_t nLastServe = served.back().second;
    for (size_t i = 0; i < served.size(); ++i)
        if (nLastServe - served[i].second < TTL_US)
            ++nRetained;
    BOOST_CHECK_EQUAL(peer.mapHeadersServedDedup.size(), nRetained);
    BOOST_CHECK(peer.mapHeadersServedDedup.count(served.back().first) == 1);
}

// R9: different peers remain independent -> both served despite the same
//     (locator, hashStop, tip).
BOOST_AUTO_TEST_CASE(r9_peers_independent_both_served)
{
    CNode peerA(INVALID_SOCKET, TestPeerAddress(29), "dedup-r9-a", true);
    CNode peerB(INVALID_SOCKET, TestPeerAddress(30), "dedup-r9-b", true);
    const CBlockLocator locator = Loc(std::vector<uint256>{H(100), H(90)});
    const uint256 hashStop = H(909);
    const uint256 fp = HeaderServedDedupFingerprint(locator, hashStop, TIP_A);

    Serve(peerA, locator, hashStop, TIP_A, NOW, 3, 400);
    BOOST_CHECK(WouldSuppress(peerA, fp, NOW + 1000000));
    // Peer B never saw the request -> served.
    BOOST_CHECK(!WouldSuppress(peerB, fp, NOW + 1000000));
    Serve(peerB, locator, hashStop, TIP_A, NOW + 1000000, 3, 400);
    BOOST_CHECK(WouldSuppress(peerB, fp, NOW + 2000000));
}

// R10: bounded-state / reconnect behavior remains correct: cap+eviction hold
//      under the tip-aware fingerprint, and a fresh CNode on reconnect has an
//      empty map (state dies with the connection).
BOOST_AUTO_TEST_CASE(r10_bounded_state_and_reconnect)
{
    // Cap + eviction.
    CNode peer(INVALID_SOCKET, TestPeerAddress(31), "dedup-r10", true);
    const uint256 hashStop = H(910);
    std::vector<uint256> fps;
    for (size_t i = 0; i < HEADER_SERVED_DEDUP_CAP + 10; ++i)
    {
        const CBlockLocator locator = Loc(std::vector<uint256>{H(2000 + i)});
        const uint256 fp = HeaderServedDedupFingerprint(locator, hashStop, TIP_A);
        fps.push_back(fp);
        Serve(peer, locator, hashStop, TIP_A, NOW + static_cast<int64_t>(i), 2, 250);
        BOOST_CHECK(peer.mapHeadersServedDedup.size() <= HEADER_SERVED_DEDUP_CAP);
    }
    BOOST_CHECK(peer.mapHeadersServedDedup.size() <= HEADER_SERVED_DEDUP_CAP);
    BOOST_CHECK(peer.mapHeadersServedDedup.count(fps[0]) == 0);
    BOOST_CHECK(peer.mapHeadersServedDedup.count(fps.back()) == 1);

    // Reconnect: fresh CNode, empty map -> immediately served.
    CNode peerNew(INVALID_SOCKET, TestPeerAddress(31), "dedup-r10-new", true);
    BOOST_CHECK(peerNew.mapHeadersServedDedup.empty());
    const CBlockLocator locator = Loc(std::vector<uint256>{H(100), H(90)});
    const uint256 fp = HeaderServedDedupFingerprint(locator, hashStop, TIP_A);
    BOOST_CHECK(!WouldSuppress(peerNew, fp, NOW));
}

BOOST_AUTO_TEST_SUITE_END()
