// Copyright (c) 2026 The Innova developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Unit tests for ping/pong lifecycle semantics and the -pinglifecycletrace
// instrumentation (src/pinglifecycletrace.cpp).
//
// These drive the real production paths wherever possible: the wire "pong"
// message is framed through CNode::ReceiveMsgBytes (the SocketHandler path)
// and dispatched through ProcessMessages -> ProcessMessage("pong"), and the
// scheduler gate is exercised through SendMessages.  They verify semantics
// (state transitions, RTT meaning), not just counter values.

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <string>
#include <vector>

#include "../main.h"
#include "../net.h"
#include "../pinglifecycletrace.h"
#include "../util.h"

namespace {

// Serialize a "pong" wire message carrying `nonce` and deliver it through the
// real framing path into vRecvMsg, exactly as SocketHandler does.
void DeliverPong(CNode& node, uint64_t nonce)
{
    CDataStream ssSend(SER_NETWORK, INIT_PROTO_VERSION);
    ssSend << CMessageHeader("pong", 0);
    ssSend << nonce;
    unsigned int nSize = ssSend.size() - CMessageHeader::HEADER_SIZE;
    memcpy((char*)&ssSend[CMessageHeader::MESSAGE_SIZE_OFFSET], &nSize, sizeof(nSize));
    uint256 hash = Hash(ssSend.begin() + CMessageHeader::HEADER_SIZE, ssSend.end());
    unsigned int nChecksum = 0;
    memcpy(&nChecksum, &hash, sizeof(nChecksum));
    memcpy((char*)&ssSend[CMessageHeader::CHECKSUM_OFFSET], &nChecksum, sizeof(nChecksum));

    LOCK(node.cs_vRecvMsg);
    BOOST_REQUIRE(node.ReceiveMsgBytes(&ssSend[0], ssSend.size()));
    BOOST_REQUIRE(!node.vRecvMsg.empty());
}

// Dispatch the framed message(s) exactly as ThreadMessageHandler does.
void DrainMessages(CNode& node)
{
    LOCK(node.cs_vRecvMsg);
    if (!node.vRecvMsg.empty())
    {
        BOOST_CHECK_MESSAGE(node.vRecvMsg[0].complete(), "message not complete");
        BOOST_CHECK_MESSAGE(node.vRecvMsg[0].hdr.GetCommand() == "pong",
                            std::string("unexpected command: ") + node.vRecvMsg[0].hdr.GetCommand());
        uint256 hash = Hash(node.vRecvMsg[0].vRecv.begin(),
                            node.vRecvMsg[0].vRecv.begin() + node.vRecvMsg[0].hdr.nMessageSize);
        unsigned int nChecksum = 0;
        memcpy(&nChecksum, &hash, sizeof(nChecksum));
        BOOST_CHECK_MESSAGE(nChecksum == node.vRecvMsg[0].hdr.nChecksum,
                            "checksum mismatch");
        BOOST_CHECK_MESSAGE(node.vRecvMsg[0].hdr.nMessageSize == 8,
                            "unexpected size");
        BOOST_CHECK_MESSAGE(!node.fDisconnect, "peer unexpectedly disconnected");
        BOOST_CHECK_MESSAGE(node.nSendSize < SendBufferSize(), "send buffer full");
    }
    BOOST_REQUIRE(ProcessMessages(&node));
}

// Put the peer in the "ping outstanding" state used by the scheduler/accounting.
void MakeOutstandingPing(CNode& node, uint64_t nonce, int64_t ageUs)
{
    node.nPingNonceSent = nonce;
    node.nPingUsecStart = GetTimeMicros() - ageUs;
    node.nPingUsecTime = 0;
    node.nPingQueuedUsec = 0;
    node.nVersion = PROTOCOL_VERSION;
}

std::vector<std::string> SentCommands(CNode& node)
{
    std::vector<std::string> commands;
    LOCK(node.cs_vSend);
    for (std::deque<CSerializeData>::const_iterator it = node.vSendMsg.begin();
         it != node.vSendMsg.end(); ++it)
    {
        CDataStream stream(*it, SER_NETWORK, INIT_PROTO_VERSION);
        CMessageHeader header;
        stream >> header;
        commands.push_back(header.GetCommand());
    }
    return commands;
}

bool HasCommand(const std::vector<std::string>& commands, const std::string& command)
{
    return std::find(commands.begin(), commands.end(), command) != commands.end();
}

} // namespace

BOOST_AUTO_TEST_SUITE(pinglifecycle_tests)

// pingtime is the last COMPLETED round-trip time, not a live timer: it stays
// at the previous value while a fresh ping is still in flight, while pingwait
// keeps counting up from the outstanding ping's send time.
BOOST_AUTO_TEST_CASE(pingtime_is_last_completed_rtt)
{
    InitPingLifecycleTrace(false);
    CNode n(INVALID_SOCKET, CAddress(), "pingtime-test", true);

    // Never completed a ping: pingtime 0 regardless of wall clock.
    MakeOutstandingPing(n, 42, 500000);
    CNodeStats stats;
    n.copyStats(stats);
    BOOST_CHECK_EQUAL(stats.dPingTime, 0.0);
    BOOST_CHECK_CLOSE(stats.dPingWait, 0.5, 10.0);

    // A previously completed RTT stays as "last completed" while a different
    // ping is outstanding: pingtime must not track the in-flight ping.
    n.nPingUsecTime = 120000; // 120 ms from an earlier completed round-trip
    n.copyStats(stats);
    BOOST_CHECK_CLOSE(stats.dPingTime, 0.12, 10.0);
    BOOST_CHECK_CLOSE(stats.dPingWait, 0.5, 10.0);
}

// pingwait is the age of the current outstanding ping (now - send time) and is
// reported only while a ping is outstanding; a completed round-trip resets it
// to 0 while pingtime keeps the measured value.
BOOST_AUTO_TEST_CASE(pingwait_is_current_outstanding_age)
{
    InitPingLifecycleTrace(false);
    CNode n(INVALID_SOCKET, CAddress(), "pingwait-test", true);

    CNodeStats stats;
    n.copyStats(stats);
    BOOST_CHECK_EQUAL(stats.dPingWait, 0.0);

    MakeOutstandingPing(n, 7, 200000);
    n.copyStats(stats);
    BOOST_CHECK_CLOSE(stats.dPingWait, 0.2, 10.0);

    // Matching pong completes the round trip: outstanding state cleared.
    n.nPingUsecStart = GetTimeMicros() - 200000;
    DeliverPong(n, 7);
    DrainMessages(n);
    BOOST_CHECK_EQUAL(n.nPingNonceSent, (uint64_t)0);
    BOOST_CHECK(n.nPingUsecTime > 0);
    n.copyStats(stats);
    BOOST_CHECK_EQUAL(stats.dPingWait, 0.0);
    BOOST_CHECK_CLOSE(stats.dPingTime, 0.2, 10.0);
}

// A pong whose nonce matches the outstanding ping clears the outstanding state
// and updates pingtime (real framed-message path, trace enabled).
BOOST_AUTO_TEST_CASE(pong_matching_nonce_clears_outstanding)
{
    InitPingLifecycleTrace(true);
    CNode n(INVALID_SOCKET, CAddress(), "pong-match-test", true);
    MakeOutstandingPing(n, 0xdeadbeefULL, 300000);

    DeliverPong(n, 0xdeadbeefULL);
    DrainMessages(n);

    BOOST_CHECK_EQUAL(n.nPingNonceSent, (uint64_t)0);
    BOOST_CHECK(n.nPingUsecTime > 0);

    const PingLifecycleCountersSnapshot s = PingLifecycleCountersForTesting();
    BOOST_CHECK_EQUAL(s.pongsProcessed, (uint64_t)1);
    BOOST_CHECK_EQUAL(s.pongsMatched, (uint64_t)1);
    BOOST_CHECK_EQUAL(s.pongsUnmatched, (uint64_t)0);
}

// A pong with the wrong nonce must NOT clear the outstanding ping and must NOT
// update pingtime.
BOOST_AUTO_TEST_CASE(unmatched_pong_does_not_clear_outstanding)
{
    InitPingLifecycleTrace(true);
    CNode n(INVALID_SOCKET, CAddress(), "pong-mismatch-test", true);
    MakeOutstandingPing(n, 0xdeadbeefULL, 300000);
    const int64_t nPingTimeBefore = n.nPingUsecTime;

    DeliverPong(n, 0x55555555ULL);
    DrainMessages(n);

    BOOST_CHECK_EQUAL(n.nPingNonceSent, (uint64_t)0xdeadbeefULL);
    BOOST_CHECK_EQUAL(n.nPingUsecTime, nPingTimeBefore);

    const PingLifecycleCountersSnapshot s = PingLifecycleCountersForTesting();
    BOOST_CHECK_EQUAL(s.pongsProcessed, (uint64_t)1);
    BOOST_CHECK_EQUAL(s.pongsMatched, (uint64_t)0);
    BOOST_CHECK_EQUAL(s.pongsUnmatched, (uint64_t)1);
}

// The scheduler must not send a new ping while a pong is outstanding unless an
// explicit ping request (RPC) queues one.  Under a normal periodic schedule the
// outstanding nonce is preserved (no silent replacement).
BOOST_AUTO_TEST_CASE(new_ping_does_not_silently_replace_outstanding)
{
    InitPingLifecycleTrace(true);

    CNode n(INVALID_SOCKET, CAddress(), "ping-gate-test", true);
    n.nVersion = PROTOCOL_VERSION;
    n.nRecvVersion = PROTOCOL_VERSION;
    n.nLastBlockRecv = GetTime();
    n.nChainHeight = nBestHeight;
    n.nBestKnownHeight = nBestHeight;

    // Outstanding ping: the periodic gate (nPingNonceSent == 0) is closed.
    MakeOutstandingPing(n, 55, 100000);
    n.fPingQueued = false;
    std::vector<CNode*> vNodesCopy(1, &n);
    BOOST_REQUIRE(SendMessages(&n, true, vNodesCopy));

    BOOST_CHECK_EQUAL(n.nPingNonceSent, (uint64_t)55);
    BOOST_CHECK(!HasCommand(SentCommands(n), "ping"));
    {
        const PingLifecycleCountersSnapshot s = PingLifecycleCountersForTesting();
        BOOST_CHECK_EQUAL(s.pingsScheduled, (uint64_t)0);
        BOOST_CHECK_EQUAL(s.pingsReplaced, (uint64_t)0);
    }

    // An explicit ping request while a pong is outstanding is the one overlap
    // case: a new nonce is generated and the old outstanding nonce is dropped.
    n.fPingQueued = true;
    BOOST_REQUIRE(SendMessages(&n, true, vNodesCopy));

    BOOST_CHECK(n.nPingNonceSent != 0 && n.nPingNonceSent != 55);
    BOOST_CHECK(!n.fPingQueued);
    BOOST_CHECK(HasCommand(SentCommands(n), "ping"));
    {
        const PingLifecycleCountersSnapshot s = PingLifecycleCountersForTesting();
        BOOST_CHECK_EQUAL(s.pingsScheduled, (uint64_t)1);
        BOOST_CHECK_EQUAL(s.pingsReplaced, (uint64_t)1);
    }
}

// The frame->process delay metric measures the queueing gap between the pong
// arriving on the wire and ProcessMessage dispatching it (Model B detector).
BOOST_AUTO_TEST_CASE(pong_frame_to_process_delay_is_measured)
{
    InitPingLifecycleTrace(true);
    CNode n(INVALID_SOCKET, CAddress(), "delay-measure-test", true);
    MakeOutstandingPing(n, 9, 100000);

    const int64_t nFrameUs = GetTimeMicros() - 3000000; // frame 3 s ago
    PingLifecycleTracePongProcessBegin(&n, nFrameUs);

    const PingLifecycleCountersSnapshot s = PingLifecycleCountersForTesting();
    BOOST_CHECK_EQUAL(s.pongsProcessed, (uint64_t)1);
    BOOST_CHECK_EQUAL(s.pongFrameToProcessCount, (uint64_t)1);
    BOOST_CHECK_EQUAL(s.pongFrameToProcessSumUs, (uint64_t)3000000);
    BOOST_CHECK_EQUAL(s.pongFrameToProcessMaxUs, (uint64_t)3000000);
}

// With the flag off the trace must be inert: counters stay zero while a full
// ping/pong round-trip still completes normally through the real paths.
BOOST_AUTO_TEST_CASE(flags_off_preserves_baseline)
{
    InitPingLifecycleTrace(false);
    BOOST_CHECK(!PingLifecycleTraceEnabled());

    CNode n(INVALID_SOCKET, CAddress(), "baseline-test", true);
    n.nVersion = PROTOCOL_VERSION;
    n.nRecvVersion = PROTOCOL_VERSION;
    n.nLastBlockRecv = GetTime();
    n.nChainHeight = nBestHeight;
    n.nBestKnownHeight = nBestHeight;

    // A fresh peer schedules its periodic ping immediately on SendMessages.
    std::vector<CNode*> vNodesCopy(1, &n);
    BOOST_REQUIRE(SendMessages(&n, true, vNodesCopy));
    BOOST_CHECK(HasCommand(SentCommands(n), "ping"));

    const uint64_t nOutstanding = n.nPingNonceSent;
    BOOST_CHECK(nOutstanding != 0);

    // The optimistic send on an INVALID_SOCKET disconnects the peer; that is a
    // test-only artifact, so clear it before exercising the receive path.
    n.fDisconnect = false;

    // Matching pong completes the round trip with the flag off.
    DeliverPong(n, nOutstanding);
    DrainMessages(n);
    BOOST_CHECK_EQUAL(n.nPingNonceSent, (uint64_t)0);
    BOOST_CHECK(n.nPingUsecTime > 0);

    // No trace accounting happened at all.
    const PingLifecycleCountersSnapshot s = PingLifecycleCountersForTesting();
    BOOST_CHECK_EQUAL(s.pingsScheduled, (uint64_t)0);
    BOOST_CHECK_EQUAL(s.pingsReplaced, (uint64_t)0);
    BOOST_CHECK_EQUAL(s.pingsQueued, (uint64_t)0);
    BOOST_CHECK_EQUAL(s.pingsSent, (uint64_t)0);
    BOOST_CHECK_EQUAL(s.pongsFrameComplete, (uint64_t)0);
    BOOST_CHECK_EQUAL(s.pongsProcessed, (uint64_t)0);
    BOOST_CHECK_EQUAL(s.pongsMatched, (uint64_t)0);
    BOOST_CHECK_EQUAL(s.pongsUnmatched, (uint64_t)0);
}

// The outstanding ping state lives and dies with the CNode: destroying a peer
// with an outstanding ping emits the disconnect event, and a fresh peer starts
// with clean ping accounting (no cross-node leakage).
BOOST_AUTO_TEST_CASE(disconnect_clears_ping_lifecycle_state)
{
    InitPingLifecycleTrace(true);

    {
        CNode n(INVALID_SOCKET, CAddress(), "disconnect-test", true);
        MakeOutstandingPing(n, 123, 1000000);
        BOOST_CHECK_EQUAL(n.nPingNonceSent, (uint64_t)123);
        // scope exit -> ~CNode -> PingLifecycleTracePeerClosed
    }

    const PingLifecycleCountersSnapshot s = PingLifecycleCountersForTesting();
    BOOST_CHECK_EQUAL(s.peerDisconnects, (uint64_t)1);

    CNode fresh(INVALID_SOCKET, CAddress(), "disconnect-test-fresh", true);
    BOOST_CHECK_EQUAL(fresh.nPingNonceSent, (uint64_t)0);
    BOOST_CHECK_EQUAL(fresh.nPingUsecStart, (int64_t)0);
    BOOST_CHECK_EQUAL(fresh.nPingUsecTime, (int64_t)0);
    BOOST_CHECK_EQUAL(fresh.nPingQueuedUsec, (int64_t)0);
}

BOOST_AUTO_TEST_SUITE_END()
