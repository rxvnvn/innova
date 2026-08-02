// Copyright (c) 2026 The Innova developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#ifndef WIN32
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <boost/test/unit_test.hpp>
#include <boost/thread/barrier.hpp>
#include <boost/thread/thread.hpp>

#include "checkpoints.h"
#include "ibdefficiency.h"
#include "innovarpc.h"
#include "main.h"
#include "net.h"
#include "protocol.h"
#include "util.h"
#include "version.h"

namespace {

static const int64_t TEST_TIME = 100000;

static CAddress TestPeerAddress(unsigned int nPeer)
{
    struct in_addr addr;
    addr.s_addr = 0x0100007f + (nPeer << 24);
    return CAddress(CService(addr, GetDefaultPort()));
}

// SendMessages' optimistic-write path (SocketSendData) performs a real send()
// on the peer socket, which fails and disconnects the peer when the socket is
// INVALID_SOCKET.  Install a live socket pair so such peers remain connected
// long enough to be eligible for stalled-sync recovery.
class ScopedPeerSocket
{
public:
    explicit ScopedPeerSocket(CNode& node) : nPeerSide(-1)
    {
#ifndef WIN32
        int sv[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0)
        {
            node.hSocket = sv[0];
            nPeerSide = sv[1];
        }
#endif
    }

    ~ScopedPeerSocket()
    {
#ifndef WIN32
        if (nPeerSide >= 0)
            close(nPeerSide);
#endif
    }

private:
    int nPeerSide;
};

static void PreparePeerForSendMessages(CNode& node, int nVersion)
{
    node.nVersion = nVersion;
    node.nRecvVersion = nVersion;
    node.nPingNonceSent = 1;
    node.nLastBlockRecv = GetTime();
    node.nChainHeight = nBestHeight;
    node.nBestKnownHeight = nBestHeight;
}

static std::vector<std::string> SentCommands(CNode& node)
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

static bool HasCommand(const std::vector<std::string>& commands,
                       const std::string& command)
{
    return std::find(commands.begin(), commands.end(), command) != commands.end();
}

static void PreparePeerForRecovery(CNode& node, int nVersion, int nPeerHeight)
{
    PreparePeerForSendMessages(node, nVersion);
    node.fSuccessfullyConnected = true;
    node.fClient = false;
    node.fOneShot = false;
    node.fDisconnect = false;
    node.nChainHeight = nPeerHeight;
    node.nBestKnownHeight = nPeerHeight;
    node.nLastHeightUpdate = TEST_TIME;
}

static size_t QueuedGetBlocksCount(const std::vector<CNode*>& peers)
{
    size_t count = 0;
    for (std::vector<CNode*>::const_iterator it = peers.begin(); it != peers.end(); ++it)
        count += (*it)->getBlocksIndex.size();
    return count;
}

static size_t QueuedBlockAskForCount(const std::vector<CNode*>& peers,
                                     const uint256& hashBlock)
{
    size_t count = 0;
    for (std::vector<CNode*>::const_iterator itPeer = peers.begin();
         itPeer != peers.end(); ++itPeer)
    {
        for (std::multimap<int64_t, CInv>::const_iterator itAsk = (*itPeer)->mapAskFor.begin();
             itAsk != (*itPeer)->mapAskFor.end(); ++itAsk)
        {
            if (itAsk->second.type == MSG_BLOCK && itAsk->second.hash == hashBlock)
                ++count;
        }
    }
    return count;
}

class CScopedAlreadyAskedFor
{
private:
    std::map<CInv, int64_t> saved;

public:
    CScopedAlreadyAskedFor()
    {
        LOCK(cs_mapAlreadyAskedFor);
        saved = mapAlreadyAskedFor;
        mapAlreadyAskedFor.clear();
    }

    ~CScopedAlreadyAskedFor()
    {
        LOCK(cs_mapAlreadyAskedFor);
        mapAlreadyAskedFor = saved;
    }
};

class CScopedOrphanBlocks
{
private:
    std::map<uint256, CBlock*> saved;

public:
    CScopedOrphanBlocks()
    {
        saved = mapOrphanBlocks;
        mapOrphanBlocks.clear();
    }

    ~CScopedOrphanBlocks()
    {
        for (std::map<uint256, CBlock*>::iterator it = mapOrphanBlocks.begin();
             it != mapOrphanBlocks.end(); ++it)
            delete it->second;
        mapOrphanBlocks = saved;
    }
};


class CScopedOrphanCountByNode
{
private:
    std::map<NodeId, int> saved;

public:
    CScopedOrphanCountByNode()
    {
        LOCK(cs_main);
        saved = mapOrphanCountByNode;
        mapOrphanCountByNode.clear();
    }

    ~CScopedOrphanCountByNode()
    {
        LOCK(cs_main);
        mapOrphanCountByNode = saved;
    }
};

class CScopedInitialBlockDownloadState
{
private:
    bool fRegTestSaved;
    bool fImportingSaved;
    bool fReindexSaved;
    int nBestHeightSaved;
    std::vector<CNode*> vNodesSaved;

public:
    explicit CScopedInitialBlockDownloadState(CNode* peer)
        : fRegTestSaved(fRegTest),
          fImportingSaved(fImporting),
          fReindexSaved(fReindex),
          nBestHeightSaved(nBestHeight)
    {
        fRegTest = false;
        fImporting = false;
        fReindex = false;
        nBestHeight = std::max(
            nBestHeight, Checkpoints::GetTotalBlocksEstimate());
        peer->nChainHeight = nBestHeight + 100;
        peer->nBestKnownHeight = nBestHeight + 100;
        peer->nLastHeightUpdate = GetTime();

        LOCK(cs_vNodes);
        vNodesSaved = vNodes;
        vNodes.clear();
        vNodes.push_back(peer);
    }

    ~CScopedInitialBlockDownloadState()
    {
        {
            LOCK(cs_vNodes);
            vNodes = vNodesSaved;
        }
        nBestHeight = nBestHeightSaved;
        fReindex = fReindexSaved;
        fImporting = fImportingSaved;
        fRegTest = fRegTestSaved;
    }
};

static CGetBlocksRequestInfo TestGetBlocksRequest(
    uint64_t nLocator, int nResolvedHeight, uint64_t nFirst,
    uint64_t nLast, unsigned int nResponseCount, int64_t nTimeMillis,
    uint64_t nStop = 0, int nStopHeight = -1,
    uint64_t nChainTip = 9000000)
{
    CGetBlocksRequestInfo request;
    request.hashLocatorTip = uint256(nLocator);
    request.nResolvedHeight = nResolvedHeight;
    request.hashStop = uint256(nStop);
    request.nStopHeight = nStopHeight;
    request.hashChainTip = uint256(nChainTip);
    request.hashPredictedFirst = uint256(nFirst);
    request.hashPredictedLast = uint256(nLast);
    request.nPredictedResponseCount = nResponseCount;
    request.nRequestTimeMillis = nTimeMillis;
    return request;
}

static CGetBlocksResponseInfo TestGetBlocksResponse(
    const CGetBlocksRequestInfo& request)
{
    CGetBlocksResponseInfo response;
    response.hashFirst = request.hashPredictedFirst;
    response.hashLast = request.hashPredictedLast;
    response.nItemCount = request.nPredictedResponseCount;
    if (response.nItemCount > 0)
    {
        response.nMinHeight = request.nResolvedHeight + 1;
        response.nMaxHeight =
            request.nResolvedHeight + response.nItemCount;
    }
    return response;
}

static void CheckNormalGetBlocksSync(int nVersion, unsigned int nPeer)
{
    CNode peer(
        INVALID_SOCKET, TestPeerAddress(nPeer),
        nVersion == MIN_PEER_PROTO_VERSION
            ? "normal-legacy-getblocks"
            : "normal-current-getblocks",
        true);
    peer.nVersion = nVersion;

    for (int nBatch = 0; nBatch < 4; ++nBatch)
    {
        const int nResolvedHeight = nBatch * 1000;
        const CGetBlocksRequestInfo request = TestGetBlocksRequest(
            100 + nBatch, nResolvedHeight,
            10000 + nResolvedHeight,
            10999 + nResolvedHeight,
            1000, 1000 + nBatch * 5000);
        const CGetBlocksServerDecision decision =
            peer.getBlocksServer.Evaluate(request, true);
        BOOST_CHECK_EQUAL(decision.action, GETBLOCKS_SERVER_ALLOW);
        BOOST_CHECK(decision.fProgress);
        BOOST_CHECK(!decision.fPenalize);

        const CGetBlocksResponseInfo response =
            TestGetBlocksResponse(request);
        peer.getBlocksServer.RecordResponse(request, response);
        BOOST_CHECK(peer.getBlocksServer.NoteBlockGetData(
            response.hashLast, response.nMaxHeight,
            request.nRequestTimeMillis + 1));
    }

    BOOST_CHECK_EQUAL(peer.getBlocksServer.nResponsesAllowed, 4U);
    BOOST_CHECK_EQUAL(peer.getBlocksServer.nResponsesSuppressed, 0U);
    BOOST_CHECK_EQUAL(peer.getBlocksServer.nRequestsRateLimited, 0U);
    BOOST_CHECK_EQUAL(peer.nMisbehavior, 0);
}

static size_t QueuedBlockAskForCount(const CNode& peer,
                                     const uint256& hashBlock)
{
    size_t count = 0;
    for (std::multimap<int64_t, CInv>::const_iterator it = peer.mapAskFor.begin();
         it != peer.mapAskFor.end(); ++it)
    {
        if ((it->second.type == MSG_BLOCK ||
             it->second.type == MSG_FILTERED_BLOCK) &&
            it->second.hash == hashBlock)
            ++count;
    }
    return count;
}

} // namespace

namespace {

// ---- Pipeline-wake regression helpers -------------------------------------

static const int64_t WAKE_TEST_TIME = 1000000;

static int64_t MetricGet(const std::atomic<int64_t>& counter)
{
    return counter.load(std::memory_order_relaxed);
}

// Deterministic wake-candidate peer: fixed advertised height (score tiebreak
// is the node id), no block-recency/ping bonuses, empty request containers.
static void PrepareWakeEligiblePeer(CNode& peer, int nPeerHeight, int64_t nNow)
{
    peer.nVersion = PROTOCOL_VERSION;
    peer.fSuccessfullyConnected = true;
    peer.fClient = false;
    peer.fOneShot = false;
    peer.fDisconnect = false;
    peer.nChainHeight = nPeerHeight;
    peer.nBestKnownHeight = nPeerHeight;
    peer.nLastHeightUpdate = nNow;
    peer.nLastBlockRecv = 0;
    peer.nBlocksReceivedInBatch = 0;
    peer.nPingUsecTime = 0;
}

static void MarkPeerWakeDedupBlocked(CNode& peer, CBlockIndex* pindexTip,
                                     int64_t nLastGetBlocksTime)
{
    peer.pindexLastGetBlocksBegin = pindexTip;
    peer.hashLastGetBlocksEnd = uint256(0);
    peer.nLastGetBlocksTime = nLastGetBlocksTime;
}

static void ResetPeerWakeDedupState(CNode& peer)
{
    peer.pindexLastGetBlocksBegin = NULL;
    peer.hashLastGetBlocksEnd = uint256(0);
    peer.nLastGetBlocksTime = 0;
}

static size_t TotalQueuedGetBlocks(const std::vector<CNode*>& peers)
{
    size_t nQueued = 0;
    for (std::vector<CNode*>::const_iterator it = peers.begin();
         it != peers.end(); ++it)
        nQueued += (*it)->getBlocksIndex.size();
    return nQueued;
}

static size_t TotalQueuedAskFor(const std::vector<CNode*>& peers)
{
    size_t nQueued = 0;
    for (std::vector<CNode*>::const_iterator it = peers.begin();
         it != peers.end(); ++it)
        nQueued += (*it)->setAskForBlocks.size();
    return nQueued;
}

static size_t TotalInflight(const std::vector<CNode*>& peers)
{
    size_t nInflight = 0;
    for (std::vector<CNode*>::const_iterator it = peers.begin();
         it != peers.end(); ++it)
        nInflight += (*it)->setBlocksInFlight.size();
    return nInflight;
}

static size_t TotalOutstandingGetBlocks(const std::vector<CNode*>& peers)
{
    size_t nOutstanding = 0;
    for (std::vector<CNode*>::const_iterator it = peers.begin();
         it != peers.end(); ++it)
        nOutstanding += (*it)->getBlocksOutstandingSources.size();
    return nOutstanding;
}

static size_t TotalDeferredInv(const std::vector<CNode*>& peers)
{
    size_t nDeferred = 0;
    for (std::vector<CNode*>::const_iterator it = peers.begin();
         it != peers.end(); ++it)
        nDeferred += (*it)->deferredBlockInv.size();
    return nDeferred;
}

// Mirrors the SendMessages getblocks flush: drains the queued locators and
// restores the aggregate gauges to match the now-empty containers.
static void ClearQueuedGetBlocks(CNode& peer)
{
    const int64_t nQueued = (int64_t)peer.getBlocksIndex.size();
    if (nQueued == 0)
        return;
    peer.getBlocksIndex.clear();
    peer.getBlocksHash.clear();
    peer.getBlocksSources.clear();
    peer.getBlocksRecoveryIds.clear();
    ibdmetrics::GetBlocksQueuedAdd(-nQueued, true);
}

struct PipelineWakeGauges
{
    int64_t global_active_current;
    int64_t total_queued_current;
    int64_t total_inflight_current;
    int64_t getblocks_outstanding_current;
    int64_t total_getblocks_queued_requests_current;
    int64_t peers_with_queued_getblocks_current;
    int64_t total_deferred_current;
};

static PipelineWakeGauges SnapshotWakeGauges()
{
    PipelineWakeGauges g;
    g.global_active_current =
        MetricGet(ibdmetrics::Get().global_active_current);
    g.total_queued_current = MetricGet(ibdmetrics::Get().total_queued_current);
    g.total_inflight_current =
        MetricGet(ibdmetrics::Get().total_inflight_current);
    g.getblocks_outstanding_current =
        MetricGet(ibdmetrics::Get().getblocks_outstanding_current);
    g.total_getblocks_queued_requests_current =
        MetricGet(ibdmetrics::Get().total_getblocks_queued_requests_current);
    g.peers_with_queued_getblocks_current =
        MetricGet(ibdmetrics::Get().peers_with_queued_getblocks_current);
    g.total_deferred_current =
        MetricGet(ibdmetrics::Get().total_deferred_current);
    return g;
}

// The aggregate gauges must always equal the sum of the real peer containers,
// never go negative, and never count a peer twice.
static void CheckWakeGaugeBalance(const std::vector<CNode*>& peers)
{
    const PipelineWakeGauges g = SnapshotWakeGauges();
    BOOST_CHECK_EQUAL(g.global_active_current,
                      (int64_t)(TotalQueuedAskFor(peers) + TotalInflight(peers)));
    BOOST_CHECK_EQUAL(g.total_queued_current, (int64_t)TotalQueuedAskFor(peers));
    BOOST_CHECK_EQUAL(g.total_inflight_current, (int64_t)TotalInflight(peers));
    BOOST_CHECK_EQUAL(g.getblocks_outstanding_current,
                      (int64_t)TotalOutstandingGetBlocks(peers));
    BOOST_CHECK_EQUAL(g.total_getblocks_queued_requests_current,
                      (int64_t)TotalQueuedGetBlocks(peers));
    BOOST_CHECK_EQUAL(g.total_deferred_current, (int64_t)TotalDeferredInv(peers));
    BOOST_CHECK(g.global_active_current >= 0);
    BOOST_CHECK(g.total_queued_current >= 0);
    BOOST_CHECK(g.total_inflight_current >= 0);
    BOOST_CHECK(g.getblocks_outstanding_current >= 0);
    BOOST_CHECK(g.total_getblocks_queued_requests_current >= 0);
    BOOST_CHECK(g.peers_with_queued_getblocks_current >= 0);
    BOOST_CHECK(g.total_deferred_current >= 0);
}

} // namespace

BOOST_AUTO_TEST_SUITE(p2p_sync_tests)
BOOST_AUTO_TEST_CASE(same_peer_block_askfor_is_idempotent)
{
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    const uint256 hashA(4001);
    const uint256 hashB(4002);
    const uint256 hashC(4003);
    CNode peer(INVALID_SOCKET, TestPeerAddress(40), "askfor-dedup-peer", true);

    peer.AskFor(CInv(MSG_BLOCK, hashA), BLOCKREQ_SOURCE_INV);
    peer.AskFor(CInv(MSG_BLOCK, hashA), BLOCKREQ_SOURCE_ORPHAN);
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(peer, hashA), 1U);
    BOOST_CHECK_EQUAL(peer.mapAskFor.size(), 1U);

    for (int i = 0; i < 100; ++i)
        peer.AskFor(CInv(MSG_BLOCK, hashA), BLOCKREQ_SOURCE_ORPHAN);
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(peer, hashA), 1U);
    BOOST_CHECK_EQUAL(peer.mapAskFor.size(), 1U);

    peer.AskFor(CInv(MSG_BLOCK, hashB), BLOCKREQ_SOURCE_INV);
    peer.AskFor(CInv(MSG_BLOCK, hashC), BLOCKREQ_SOURCE_INV);
    BOOST_CHECK_EQUAL(peer.mapAskFor.size(), 3U);
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(peer, hashB), 1U);
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(peer, hashC), 1U);

    peer.EraseAskForEntry(peer.mapAskFor.begin());
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(peer, hashA), 0U);
    peer.AskFor(CInv(MSG_BLOCK, hashA), BLOCKREQ_SOURCE_ORPHAN);
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(peer, hashA), 1U);
}
BOOST_AUTO_TEST_CASE(block_request_trace_reports_same_peer_skip)
{
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    const bool fPrintToConsoleSaved = fPrintToConsole;
    fPrintToConsole = true;
    BOOST_REQUIRE(InitBlockRequestTrace(true, ""));
    CNode peer(INVALID_SOCKET, TestPeerAddress(44), "askfor-trace-peer", true);
    const uint256 hash(4007);
    peer.AskFor(CInv(MSG_BLOCK, hash), BLOCKREQ_SOURCE_INV);
    peer.AskFor(CInv(MSG_BLOCK, hash), BLOCKREQ_SOURCE_ORPHAN);
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(peer, hash), 1U);
    peer.ClearAskFor();
    BOOST_CHECK(InitBlockRequestTrace(false, ""));
    fPrintToConsole = fPrintToConsoleSaved;
}
BOOST_AUTO_TEST_CASE(block_askfor_has_one_global_active_owner)
{
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    const uint256 hash(4004);
    CNode peer1(INVALID_SOCKET, TestPeerAddress(41), "askfor-peer-one", true);
    CNode peer2(INVALID_SOCKET, TestPeerAddress(42), "askfor-peer-two", true);

    peer1.AskFor(CInv(MSG_BLOCK, hash), BLOCKREQ_SOURCE_INV);
    peer2.AskFor(CInv(MSG_BLOCK, hash), BLOCKREQ_SOURCE_INV);
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(peer1, hash), 1U);
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(peer2, hash), 1U);
    BOOST_CHECK(!GetBlockRequestOwner(hash, NULL, NULL));

    BOOST_CHECK(TryAssignBlockRequestOwner(hash, peer1.GetId(), BLOCKREQ_SOURCE_INV));
    peer1.MarkBlockInFlight(hash);
    NodeId ownerPeer = -1;
    BlockRequestOwnerState ownerState = BLOCK_REQUEST_OWNER_IN_FLIGHT;
    BOOST_CHECK(GetBlockRequestOwner(hash, &ownerPeer, &ownerState));
    BOOST_CHECK_EQUAL(ownerPeer, peer1.GetId());
    BOOST_CHECK_EQUAL(ownerState, BLOCK_REQUEST_OWNER_IN_FLIGHT);

    peer1.EraseAskForEntry(peer1.mapAskFor.begin(), false);
    peer2.ClearAskFor();
    peer2.AskFor(CInv(MSG_BLOCK, hash), BLOCKREQ_SOURCE_INV);
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(peer2, hash), 0U);

    const uint256 hashTx(4005);
    peer1.AskFor(CInv(MSG_TX, hashTx), BLOCKREQ_SOURCE_INV);
    peer1.AskFor(CInv(MSG_TX, hashTx), BLOCKREQ_SOURCE_ORPHAN);
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(peer1, hash), 0U);
    BOOST_CHECK_EQUAL(peer1.mapAskFor.size(), 2U);
    peer1.ClearAskFor();
    peer2.ClearAskFor();
}

BOOST_AUTO_TEST_CASE(block_askfor_owner_releases_after_receive)
{
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    const uint256 hash(4006);
    CNode peer1(INVALID_SOCKET, TestPeerAddress(43), "owner-peer-one", true);
    CNode peer2(INVALID_SOCKET, TestPeerAddress(44), "owner-peer-two", true);

    peer1.AskFor(CInv(MSG_BLOCK, hash), BLOCKREQ_SOURCE_INV);
    BOOST_CHECK(TryAssignBlockRequestOwner(hash, peer1.GetId(), BLOCKREQ_SOURCE_INV));
    peer1.MarkBlockInFlight(hash);
    peer1.EraseAskForEntry(peer1.mapAskFor.begin(), false);
    peer1.AskFor(CInv(MSG_BLOCK, hash), BLOCKREQ_SOURCE_ORPHAN);
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(peer1, hash), 0U);
    peer2.AskFor(CInv(MSG_BLOCK, hash), BLOCKREQ_SOURCE_INV);
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(peer2, hash), 0U);

    peer1.ClearBlockInFlight(hash);
    peer1.ClearAskFor();
    peer2.AskFor(CInv(MSG_BLOCK, hash), BLOCKREQ_SOURCE_REJECT_RECOVERY);
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(peer2, hash), 1U);
}

BOOST_AUTO_TEST_CASE(orphan_traversal_requests_missing_parent_not_known_child)
{
    CScopedOrphanBlocks isolatedOrphans;
    CBlock* pChild = new CBlock();
    pChild->nTime = 7001;
    pChild->hashPrevBlock = uint256(7000);
    const uint256 hashChild = pChild->GetHash();
    mapOrphanBlocks[hashChild] = pChild;

    BOOST_CHECK(WantedByOrphan(pChild) == pChild->hashPrevBlock);
    BOOST_CHECK(WantedByOrphan(pChild) != hashChild);
}

BOOST_AUTO_TEST_CASE(orphan_chain_requests_root_missing_ancestor)
{
    CScopedOrphanBlocks isolatedOrphans;
    CBlock* pParent = new CBlock();
    pParent->nTime = 7010;
    pParent->hashPrevBlock = uint256(7009);
    const uint256 hashParent = pParent->GetHash();
    mapOrphanBlocks[hashParent] = pParent;

    CBlock* pChild = new CBlock();
    pChild->nTime = 7011;
    pChild->hashPrevBlock = hashParent;
    const uint256 hashChild = pChild->GetHash();
    mapOrphanBlocks[hashChild] = pChild;

    BOOST_CHECK(WantedByOrphan(pChild) == pParent->hashPrevBlock);
    BOOST_CHECK(WantedByOrphan(pChild) != hashChild);
    BOOST_CHECK(WantedByOrphan(pChild) != hashParent);
}


BOOST_AUTO_TEST_CASE(block_askfor_can_retry_after_inflight_expiry)
{
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    const uint256 hash(4006);
    CNode peer(INVALID_SOCKET, TestPeerAddress(43), "askfor-expiry-peer", true);

    peer.MarkBlockInFlight(hash);
    peer.mapBlockInFlightSince[hash] = GetTime() - 60;
    peer.ExpireBlockInFlight();
    peer.AskFor(CInv(MSG_BLOCK, hash), BLOCKREQ_SOURCE_REJECT_RECOVERY);
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(peer, hash), 1U);
}


BOOST_AUTO_TEST_CASE(new_headers_continue_only_after_response)
{
    CGetHeadersSyncState state;
    const std::string locatorBefore = "tip-1000|middle-a|genesis|stop-0";
    const std::string locatorAfter = "tip-2000|middle-b|genesis|stop-0";

    BOOST_CHECK_EQUAL(state.Start(locatorBefore, TEST_TIME),
                      CGetHeadersSyncState::STARTED);
    BOOST_CHECK(state.IsInFlight());
    BOOST_CHECK_EQUAL(state.Start(locatorAfter, TEST_TIME + 1),
                      CGetHeadersSyncState::SUPPRESSED_ACTIVE);
    BOOST_CHECK_EQUAL(state.RequestSequence(), 1U);
    BOOST_CHECK(state.Complete(TEST_TIME + 2));
    BOOST_CHECK(!state.IsInFlight());
    BOOST_CHECK_EQUAL(state.Start(locatorAfter, TEST_TIME + 2),
                      CGetHeadersSyncState::STARTED);
    BOOST_CHECK_EQUAL(state.RequestSequence(), 2U);
}

BOOST_AUTO_TEST_CASE(fully_known_response_stops_identical_request)
{
    CNode peer(INVALID_SOCKET, TestPeerAddress(1), "known-response-peer", true);
    const std::string requestKey = "tip-1000|middle-a|genesis|stop-0";
    BOOST_CHECK_EQUAL(peer.getHeadersSync.Start(requestKey, TEST_TIME),
                      CGetHeadersSyncState::STARTED);
    BOOST_CHECK(peer.getHeadersSync.Complete(TEST_TIME + 1));
    BOOST_CHECK_EQUAL(peer.getHeadersSync.Start(requestKey, TEST_TIME + 1),
                      CGetHeadersSyncState::SUPPRESSED_COMPLETED);
    BOOST_CHECK(!peer.getHeadersSync.IsInFlight());
    BOOST_CHECK_EQUAL(peer.nMisbehavior, 0);
}

BOOST_AUTO_TEST_CASE(partially_new_response_uses_changed_full_locator)
{
    CNode peer(INVALID_SOCKET, TestPeerAddress(6), "partial-response-peer", true);
    std::vector<uint256> oldHashes;
    oldHashes.push_back(uint256(100));
    oldHashes.push_back(uint256(50));
    oldHashes.push_back(uint256(1));
    std::vector<uint256> advancedHashes;
    advancedHashes.push_back(uint256(100));
    advancedHashes.push_back(uint256(75));
    advancedHashes.push_back(uint256(1));

    // First and last hashes are the same; only the middle locator entry
    // changes. This exercises the exact serialized locator key.
    peer.PushGetHeaders(CBlockLocator(oldHashes), uint256(0), "partial-before");
    BOOST_CHECK(peer.getHeadersSync.IsInFlight());
    RecordGetHeadersResponse(&peer, 2000, 164003);
    BOOST_CHECK(!peer.getHeadersSync.IsInFlight());
    peer.PushGetHeaders(CBlockLocator(advancedHashes), uint256(0), "partial-after");
    BOOST_CHECK(peer.getHeadersSync.IsInFlight());
    BOOST_CHECK_EQUAL(peer.getHeadersSync.RequestSequence(), 2U);
}

BOOST_AUTO_TEST_CASE(empty_response_completes_without_tight_loop)
{
    CGetHeadersSyncState state;
    const std::string requestKey = "tip-1000|middle-a|genesis|stop-0";
    BOOST_CHECK_EQUAL(state.Start(requestKey, TEST_TIME),
                      CGetHeadersSyncState::STARTED);
    BOOST_CHECK(state.Complete(TEST_TIME + 1));
    BOOST_CHECK(!state.IsInFlight());
    BOOST_CHECK_EQUAL(state.Start(requestKey, TEST_TIME + 1),
                      CGetHeadersSyncState::SUPPRESSED_COMPLETED);
    BOOST_CHECK_EQUAL(state.RequestSequence(), 1U);
}

BOOST_AUTO_TEST_CASE(missing_response_can_retry_after_timeout)
{
    CGetHeadersSyncState state;
    const std::string requestKey = "tip-1000|middle-a|genesis|stop-0";
    BOOST_CHECK_EQUAL(state.Start(requestKey, TEST_TIME),
                      CGetHeadersSyncState::STARTED);
    BOOST_CHECK(!state.IsTimedOut(TEST_TIME + GETHEADERS_REQUEST_TIMEOUT - 1));
    BOOST_CHECK_EQUAL(state.Start(requestKey, TEST_TIME + GETHEADERS_REQUEST_TIMEOUT - 1),
                      CGetHeadersSyncState::SUPPRESSED_ACTIVE);
    BOOST_CHECK(state.IsTimedOut(TEST_TIME + GETHEADERS_REQUEST_TIMEOUT));
    BOOST_CHECK_EQUAL(state.Start(requestKey, TEST_TIME + GETHEADERS_REQUEST_TIMEOUT),
                      CGetHeadersSyncState::RETRIED_AFTER_TIMEOUT);
    BOOST_CHECK(state.IsInFlight());
    BOOST_CHECK_EQUAL(state.RequestSequence(), 2U);
}

BOOST_AUTO_TEST_CASE(reconnect_starts_with_fresh_request_state)
{
    const std::string requestKey = "tip-1000|middle-a|genesis|stop-0";
    {
        CNode oldPeer(INVALID_SOCKET, TestPeerAddress(2), "old-connection", true);
        BOOST_CHECK_EQUAL(oldPeer.getHeadersSync.Start(requestKey, TEST_TIME),
                          CGetHeadersSyncState::STARTED);
        BOOST_CHECK(oldPeer.getHeadersSync.IsInFlight());
    }
    CNode newPeer(INVALID_SOCKET, TestPeerAddress(2), "new-connection", true);
    BOOST_CHECK_EQUAL(newPeer.getHeadersSync.Start(requestKey, TEST_TIME + 1),
                      CGetHeadersSyncState::STARTED);
    BOOST_CHECK_EQUAL(newPeer.getHeadersSync.RequestSequence(), 1U);
}

BOOST_AUTO_TEST_CASE(legacy_43950_peer_keeps_getblocks_path)
{
    BOOST_REQUIRE(pindexBest != NULL);
    const bool fSPVModeSaved = fSPVMode;
    fSPVMode = false;
    CNode peer(INVALID_SOCKET, TestPeerAddress(3), "legacy-43950", true);
    PreparePeerForSendMessages(peer, MIN_PEER_PROTO_VERSION);
    peer.PushGetBlocks(pindexBest, uint256(0));
    BOOST_CHECK(SendMessages(&peer, true));
    const std::vector<std::string> commands = SentCommands(peer);
    BOOST_CHECK(HasCommand(commands, "getblocks"));
    BOOST_CHECK(!HasCommand(commands, "getheaders"));
    fSPVMode = fSPVModeSaved;
}

BOOST_AUTO_TEST_CASE(block_sync_peer_version_accepts_legacy_and_current_versions)
{
    BOOST_CHECK(IsBlockSyncPeerVersion(MIN_PEER_PROTO_VERSION));
    BOOST_CHECK(IsBlockSyncPeerVersion(PROTOCOL_VERSION));

    BOOST_CHECK(IsBlockSyncPeerVersion(NOBLKS_VERSION_START - 1));
    BOOST_CHECK(!IsBlockSyncPeerVersion(NOBLKS_VERSION_START));
    BOOST_CHECK(!IsBlockSyncPeerVersion(NOBLKS_VERSION_END - 1));
    BOOST_CHECK(IsBlockSyncPeerVersion(NOBLKS_VERSION_END));
}

BOOST_AUTO_TEST_CASE(peer_stats_query_does_not_expire_stale_block_request)
{
    CNode peer(INVALID_SOCKET, TestPeerAddress(20), "stats-query-peer", true);
    PreparePeerForRecovery(peer, PROTOCOL_VERSION, nBestHeight + 10);
    const uint256 hashInFlight(3001);
    peer.MarkBlockInFlight(hashInFlight);
    peer.mapBlockInFlightSince[hashInFlight] = GetTime() - 60;

    CNodeStats stats;
    peer.copyStats(stats);

    BOOST_CHECK_EQUAL(stats.nBlocksInFlight, 1);
    BOOST_CHECK_EQUAL(peer.setBlocksInFlight.count(hashInFlight), 1U);
    BOOST_CHECK_EQUAL(peer.mapBlockInFlightSince.count(hashInFlight), 1U);
}

BOOST_AUTO_TEST_CASE(initial_block_download_query_does_not_expire_stale_request)
{
    CNode peer(INVALID_SOCKET, TestPeerAddress(21), "ibd-query-peer", true);
    PreparePeerForRecovery(peer, PROTOCOL_VERSION, nBestHeight + 10);
    const uint256 hashInFlight(3002);
    peer.MarkBlockInFlight(hashInFlight);
    peer.mapBlockInFlightSince[hashInFlight] = GetTime() - 60;

    {
        CScopedInitialBlockDownloadState scopedState(&peer);
        (void)IsInitialBlockDownload();
    }

    BOOST_CHECK_EQUAL(peer.setBlocksInFlight.count(hashInFlight), 1U);
    BOOST_CHECK_EQUAL(peer.mapBlockInFlightSince.count(hashInFlight), 1U);
}

BOOST_AUTO_TEST_CASE(rpc_queries_do_not_mutate_block_download_state)
{
    BOOST_REQUIRE(pindexBest != NULL);
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CNode peer(INVALID_SOCKET, TestPeerAddress(22), "rpc-query-peer", true);
    PreparePeerForRecovery(peer, PROTOCOL_VERSION, nBestHeight + 100);

    const uint256 hashInFlight(3003);
    const uint256 hashAskFor(3004);
    const CInv globalRequest(MSG_BLOCK, uint256(3005));
    peer.MarkBlockInFlight(hashInFlight);
    peer.mapBlockInFlightSince[hashInFlight] = GetTime() - 60;
    peer.AddAskForEntry(std::make_pair(
        (GetTime() + 60) * 1000000, CInv(MSG_BLOCK, hashAskFor)));
    peer.getBlocksIndex.push_back(pindexBest);
    peer.getBlocksHash.push_back(uint256(0));
    peer.fStartSync = true;
    {
        LOCK(cs_mapAlreadyAskedFor);
        mapAlreadyAskedFor[globalRequest] = (GetTime() + 60) * 1000000;
    }

    const bool fHybridSPVSaved = fHybridSPV;
    fHybridSPV = false;
    {
        CScopedInitialBlockDownloadState scopedState(&peer);
        const json_spirit::Array params;
        BOOST_CHECK_NO_THROW(tableRPC.execute("getinfo", params));
        BOOST_CHECK_NO_THROW(tableRPC.execute("getblockcount", params));
        BOOST_CHECK_NO_THROW(tableRPC.execute("getbestblockhash", params));
        BOOST_CHECK_NO_THROW(tableRPC.execute("getpeerinfo", params));
        BOOST_CHECK_NO_THROW(tableRPC.execute("getblockchaininfo", params));
    }
    fHybridSPV = fHybridSPVSaved;

    BOOST_CHECK_EQUAL(peer.setBlocksInFlight.count(hashInFlight), 1U);
    BOOST_CHECK_EQUAL(peer.mapBlockInFlightSince.count(hashInFlight), 1U);
    const std::vector<CNode*> peers(1, &peer);
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(peers, hashAskFor), 1U);
    BOOST_CHECK_EQUAL(peer.getBlocksIndex.size(), 1U);
    BOOST_CHECK_EQUAL(peer.getBlocksHash.size(), 1U);
    BOOST_CHECK(peer.fStartSync);
    {
        LOCK(cs_mapAlreadyAskedFor);
        BOOST_CHECK_EQUAL(mapAlreadyAskedFor.count(globalRequest), 1U);
    }
}

BOOST_AUTO_TEST_CASE(stalled_sync_recovery_queues_exactly_one_getblocks)
{
    BOOST_REQUIRE(pindexBest != NULL);
    static const int64_t STALL_TIMEOUT = 15;
    static const int64_t RECOVERY_COOLDOWN = 30;

    CNode legacyPeer(INVALID_SOCKET, TestPeerAddress(12), "legacy-recovery-peer", true);
    CNode currentPeer(INVALID_SOCKET, TestPeerAddress(13), "current-recovery-peer", true);
    PreparePeerForRecovery(legacyPeer, MIN_PEER_PROTO_VERSION, nBestHeight + 5);
    PreparePeerForRecovery(currentPeer, PROTOCOL_VERSION, nBestHeight + 10);

    std::vector<CNode*> peers;
    peers.push_back(&legacyPeer);
    peers.push_back(&currentPeer);
    CStalledSyncRecoveryState state;
    state.MarkSyncRequestSent(TEST_TIME);

    BOOST_CHECK(MaybeQueueStalledSyncRecovery(
                    peers, pindexBest, nBestHeight, TEST_TIME,
                    STALL_TIMEOUT, RECOVERY_COOLDOWN, state) == NULL);
    BOOST_CHECK_EQUAL(QueuedGetBlocksCount(peers), 0U);
    BOOST_CHECK(MaybeQueueStalledSyncRecovery(
                    peers, pindexBest, nBestHeight,
                    TEST_TIME + STALL_TIMEOUT - 1,
                    STALL_TIMEOUT, RECOVERY_COOLDOWN, state) == NULL);
    BOOST_CHECK_EQUAL(QueuedGetBlocksCount(peers), 0U);

    CNode* owner = MaybeQueueStalledSyncRecovery(
        peers, pindexBest, nBestHeight, TEST_TIME + STALL_TIMEOUT,
        STALL_TIMEOUT, RECOVERY_COOLDOWN, state);
    BOOST_REQUIRE(owner != NULL);
    BOOST_CHECK_EQUAL(QueuedGetBlocksCount(peers), 1U);
    BOOST_CHECK_EQUAL(owner->getBlocksIndex.size(), 1U);
}

BOOST_AUTO_TEST_CASE(stalled_sync_recovery_ignores_fstartsync_without_block_requests)
{
    BOOST_REQUIRE(pindexBest != NULL);
    static const int64_t STALL_TIMEOUT = 15;
    static const int64_t RECOVERY_COOLDOWN = 30;

    CNode peer(INVALID_SOCKET, TestPeerAddress(16), "fstartsync-only-peer", true);
    PreparePeerForRecovery(peer, PROTOCOL_VERSION, nBestHeight + 10);
    peer.fStartSync = true;

    std::vector<CNode*> peers(1, &peer);
    CStalledSyncRecoveryState state;
    state.MarkSyncRequestSent(TEST_TIME);

    BOOST_CHECK(MaybeQueueStalledSyncRecovery(
                    peers, pindexBest, nBestHeight, TEST_TIME,
                    STALL_TIMEOUT, RECOVERY_COOLDOWN, state) == NULL);
    BOOST_CHECK_EQUAL(QueuedGetBlocksCount(peers), 0U);

    CNode* owner = MaybeQueueStalledSyncRecovery(
        peers, pindexBest, nBestHeight, TEST_TIME + STALL_TIMEOUT + 1,
        STALL_TIMEOUT, RECOVERY_COOLDOWN, state);
    BOOST_REQUIRE(owner != NULL);
    BOOST_CHECK_EQUAL(owner, &peer);
    BOOST_CHECK_EQUAL(QueuedGetBlocksCount(peers), 1U);
    BOOST_CHECK_EQUAL(peer.getBlocksIndex.size(), 1U);
}

BOOST_AUTO_TEST_CASE(stalled_sync_recovery_cooldown_suppresses_repeat)
{
    BOOST_REQUIRE(pindexBest != NULL);
    static const int64_t STALL_TIMEOUT = 15;
    static const int64_t RECOVERY_COOLDOWN = 30;

    CNode peer(INVALID_SOCKET, TestPeerAddress(14), "cooldown-recovery-peer", true);
    PreparePeerForRecovery(peer, PROTOCOL_VERSION, nBestHeight + 10);
    std::vector<CNode*> peers(1, &peer);
    CStalledSyncRecoveryState state;
    state.MarkSyncRequestSent(TEST_TIME);

    BOOST_CHECK(MaybeQueueStalledSyncRecovery(
                    peers, pindexBest, nBestHeight, TEST_TIME,
                    STALL_TIMEOUT, RECOVERY_COOLDOWN, state) == NULL);
    const int64_t nRecoveryTime = TEST_TIME + STALL_TIMEOUT + 1;
    BOOST_REQUIRE(MaybeQueueStalledSyncRecovery(
                      peers, pindexBest, nBestHeight, nRecoveryTime,
                      STALL_TIMEOUT, RECOVERY_COOLDOWN, state) == &peer);
    BOOST_CHECK_EQUAL(QueuedGetBlocksCount(peers), 1U);

    peer.getBlocksIndex.clear();
    peer.getBlocksHash.clear();
    BOOST_CHECK(MaybeQueueStalledSyncRecovery(
                    peers, pindexBest, nBestHeight,
                    nRecoveryTime + RECOVERY_COOLDOWN - 1,
                    STALL_TIMEOUT, RECOVERY_COOLDOWN, state) == NULL);
    BOOST_CHECK_EQUAL(QueuedGetBlocksCount(peers), 0U);
}

BOOST_AUTO_TEST_CASE(stalled_sync_recovery_uses_capped_exponential_cooldown)
{
    BOOST_REQUIRE(pindexBest != NULL);
    static const int64_t STALL_TIMEOUT = 15;
    static const int64_t RECOVERY_COOLDOWN = 15;
    static const int64_t EXPECTED_COOLDOWNS[] = {
        30, 60, 120, 240, 480, 480
    };

    CNode peer(INVALID_SOCKET, TestPeerAddress(18), "backoff-recovery-peer", true);
    PreparePeerForRecovery(peer, PROTOCOL_VERSION, nBestHeight + 10);
    std::vector<CNode*> peers(1, &peer);
    CStalledSyncRecoveryState state;
    state.MarkSyncRequestSent(TEST_TIME);

    BOOST_CHECK(MaybeQueueStalledSyncRecovery(
                    peers, pindexBest, nBestHeight, TEST_TIME,
                    STALL_TIMEOUT, RECOVERY_COOLDOWN, state) == NULL);

    const int64_t nFirstRecoveryTime = TEST_TIME + STALL_TIMEOUT + 1;
    BOOST_REQUIRE(MaybeQueueStalledSyncRecovery(
                      peers, pindexBest, nBestHeight, nFirstRecoveryTime,
                      STALL_TIMEOUT, RECOVERY_COOLDOWN, state) == &peer);
    BOOST_CHECK_EQUAL(QueuedGetBlocksCount(peers), 1U);

    int64_t nPreviousRecoveryTime = nFirstRecoveryTime;
    for (size_t wave = 0; wave < ARRAYLEN(EXPECTED_COOLDOWNS); ++wave)
    {
        peer.getBlocksIndex.clear();
        peer.getBlocksHash.clear();
        peer.pindexLastGetBlocksBegin = NULL;
        peer.hashLastGetBlocksEnd = 0;
        peer.nLastGetBlocksTime = 0;

        const int64_t nEffectiveCooldown = EXPECTED_COOLDOWNS[wave];
        BOOST_CHECK(MaybeQueueStalledSyncRecovery(
                        peers, pindexBest, nBestHeight,
                        nPreviousRecoveryTime + nEffectiveCooldown - 1,
                        STALL_TIMEOUT, RECOVERY_COOLDOWN, state) == NULL);
        BOOST_CHECK_EQUAL(QueuedGetBlocksCount(peers), 0U);

        nPreviousRecoveryTime += nEffectiveCooldown;
        BOOST_REQUIRE(MaybeQueueStalledSyncRecovery(
                          peers, pindexBest, nBestHeight, nPreviousRecoveryTime,
                          STALL_TIMEOUT, RECOVERY_COOLDOWN, state) == &peer);
        BOOST_CHECK_EQUAL(QueuedGetBlocksCount(peers), 1U);
        BOOST_CHECK(MaybeQueueStalledSyncRecovery(
                        peers, pindexBest, nBestHeight, nPreviousRecoveryTime,
                        STALL_TIMEOUT, RECOVERY_COOLDOWN, state) == NULL);
        BOOST_CHECK_EQUAL(QueuedGetBlocksCount(peers), 1U);
    }
}

BOOST_AUTO_TEST_CASE(stalled_sync_recovery_inflight_block_suppresses_request)
{
    BOOST_REQUIRE(pindexBest != NULL);
    static const int64_t STALL_TIMEOUT = 15;
    static const int64_t RECOVERY_COOLDOWN = 30;
    const uint256 hashPending(1001);

    CNode peer(INVALID_SOCKET, TestPeerAddress(15), "inflight-pipeline-peer", true);
    PreparePeerForRecovery(peer, PROTOCOL_VERSION, nBestHeight + 10);
    std::vector<CNode*> peers(1, &peer);

    CStalledSyncRecoveryState state;
    state.MarkSyncRequestSent(TEST_TIME);
    BOOST_CHECK(MaybeQueueStalledSyncRecovery(
                    peers, pindexBest, nBestHeight, TEST_TIME,
                    STALL_TIMEOUT, RECOVERY_COOLDOWN, state) == NULL);
    peer.setBlocksInFlight.insert(hashPending);
    BOOST_CHECK(MaybeQueueStalledSyncRecovery(
                    peers, pindexBest, nBestHeight, TEST_TIME + STALL_TIMEOUT + 1,
                    STALL_TIMEOUT, RECOVERY_COOLDOWN, state) == NULL);
    BOOST_CHECK_EQUAL(QueuedGetBlocksCount(peers), 0U);
}

BOOST_AUTO_TEST_CASE(stalled_sync_recovery_pending_getblocks_suppresses_request)
{
    BOOST_REQUIRE(pindexBest != NULL);
    static const int64_t STALL_TIMEOUT = 15;
    static const int64_t RECOVERY_COOLDOWN = 30;

    CNode peer(INVALID_SOCKET, TestPeerAddress(25), "pending-getblocks-peer", true);
    PreparePeerForRecovery(peer, PROTOCOL_VERSION, nBestHeight + 10);
    std::vector<CNode*> peers(1, &peer);
    CStalledSyncRecoveryState state;
    state.MarkSyncRequestSent(TEST_TIME);

    BOOST_CHECK(MaybeQueueStalledSyncRecovery(
                    peers, pindexBest, nBestHeight, TEST_TIME,
                    STALL_TIMEOUT, RECOVERY_COOLDOWN, state) == NULL);
    peer.getBlocksIndex.push_back(pindexBest);
    peer.getBlocksHash.push_back(uint256(0));
    BOOST_CHECK(MaybeQueueStalledSyncRecovery(
                    peers, pindexBest, nBestHeight, TEST_TIME + STALL_TIMEOUT + 1,
                    STALL_TIMEOUT, RECOVERY_COOLDOWN, state) == NULL);
    BOOST_CHECK_EQUAL(QueuedGetBlocksCount(peers), 1U);
}

BOOST_AUTO_TEST_CASE(stalled_sync_recovery_ignores_future_block_askfor)
{
    BOOST_REQUIRE(pindexBest != NULL);
    static const int64_t STALL_TIMEOUT = 15;
    static const int64_t RECOVERY_COOLDOWN = 30;
    const uint256 hashPending(1003);

    CNode peer(INVALID_SOCKET, TestPeerAddress(26), "block-askfor-only-peer", true);
    PreparePeerForRecovery(peer, PROTOCOL_VERSION, nBestHeight + 10);
    std::vector<CNode*> peers(1, &peer);
    CStalledSyncRecoveryState state;
    state.MarkSyncRequestSent(TEST_TIME);

    BOOST_CHECK(MaybeQueueStalledSyncRecovery(
                    peers, pindexBest, nBestHeight, TEST_TIME,
                    STALL_TIMEOUT, RECOVERY_COOLDOWN, state) == NULL);
    peer.AddAskForEntry(std::make_pair(
        (TEST_TIME + STALL_TIMEOUT + 60) * 1000000,
        CInv(MSG_BLOCK, hashPending)));

    CNode* owner = MaybeQueueStalledSyncRecovery(
        peers, pindexBest, nBestHeight, TEST_TIME + STALL_TIMEOUT + 1,
        STALL_TIMEOUT, RECOVERY_COOLDOWN, state);
    BOOST_REQUIRE(owner != NULL);
    BOOST_CHECK_EQUAL(owner, &peer);
    BOOST_CHECK_EQUAL(QueuedGetBlocksCount(peers), 1U);
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(peers, hashPending), 1U);
}

BOOST_AUTO_TEST_CASE(stalled_sync_recovery_ignores_tx_askfor)
{
    BOOST_REQUIRE(pindexBest != NULL);
    static const int64_t STALL_TIMEOUT = 15;
    static const int64_t RECOVERY_COOLDOWN = 30;

    CNode peer(INVALID_SOCKET, TestPeerAddress(27), "tx-askfor-only-peer", true);
    PreparePeerForRecovery(peer, PROTOCOL_VERSION, nBestHeight + 10);
    std::vector<CNode*> peers(1, &peer);
    CStalledSyncRecoveryState state;
    state.MarkSyncRequestSent(TEST_TIME);

    BOOST_CHECK(MaybeQueueStalledSyncRecovery(
                    peers, pindexBest, nBestHeight, TEST_TIME,
                    STALL_TIMEOUT, RECOVERY_COOLDOWN, state) == NULL);
    peer.AddAskForEntry(std::make_pair(
        (TEST_TIME + STALL_TIMEOUT + 60) * 1000000,
        CInv(MSG_TX, uint256(1004))));

    BOOST_REQUIRE(MaybeQueueStalledSyncRecovery(
                      peers, pindexBest, nBestHeight,
                      TEST_TIME + STALL_TIMEOUT + 1,
                      STALL_TIMEOUT, RECOVERY_COOLDOWN, state) == &peer);
    BOOST_CHECK_EQUAL(QueuedGetBlocksCount(peers), 1U);
    BOOST_CHECK_EQUAL(peer.mapAskFor.size(), 1U);
}

BOOST_AUTO_TEST_CASE(stalled_sync_recovery_mixed_askfor_and_inflight_uses_inflight)
{
    BOOST_REQUIRE(pindexBest != NULL);
    static const int64_t STALL_TIMEOUT = 15;
    static const int64_t RECOVERY_COOLDOWN = 30;
    const uint256 hashPending(1005);

    CNode peer(INVALID_SOCKET, TestPeerAddress(28), "mixed-pipeline-peer", true);
    PreparePeerForRecovery(peer, PROTOCOL_VERSION, nBestHeight + 10);
    std::vector<CNode*> peers(1, &peer);
    CStalledSyncRecoveryState state;
    state.MarkSyncRequestSent(TEST_TIME);

    BOOST_CHECK(MaybeQueueStalledSyncRecovery(
                    peers, pindexBest, nBestHeight, TEST_TIME,
                    STALL_TIMEOUT, RECOVERY_COOLDOWN, state) == NULL);
    peer.AddAskForEntry(std::make_pair(
        (TEST_TIME + STALL_TIMEOUT + 60) * 1000000,
        CInv(MSG_BLOCK, hashPending)));
    peer.setBlocksInFlight.insert(hashPending);

    const int64_t nRecoveryTime = TEST_TIME + STALL_TIMEOUT + 1;
    BOOST_CHECK(MaybeQueueStalledSyncRecovery(
                    peers, pindexBest, nBestHeight, nRecoveryTime,
                    STALL_TIMEOUT, RECOVERY_COOLDOWN, state) == NULL);
    BOOST_CHECK_EQUAL(QueuedGetBlocksCount(peers), 0U);

    peer.setBlocksInFlight.clear();
    BOOST_REQUIRE(MaybeQueueStalledSyncRecovery(
                      peers, pindexBest, nBestHeight, nRecoveryTime,
                      STALL_TIMEOUT, RECOVERY_COOLDOWN, state) == &peer);
    BOOST_CHECK_EQUAL(QueuedGetBlocksCount(peers), 1U);
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(peers, hashPending), 1U);
}

BOOST_AUTO_TEST_CASE(stalled_sync_recovery_ignores_cross_peer_block_askfor)
{
    BOOST_REQUIRE(pindexBest != NULL);
    static const int64_t STALL_TIMEOUT = 15;
    static const int64_t RECOVERY_COOLDOWN = 30;
    const uint256 hashOwnedByOtherPeer(1002);

    CNode preferredPeer(INVALID_SOCKET, TestPeerAddress(23),
                        "preferred-recovery-peer", true);
    CNode ownerPeer(INVALID_SOCKET, TestPeerAddress(24),
                    "existing-request-owner", true);
    PreparePeerForRecovery(preferredPeer, PROTOCOL_VERSION,
                           nBestHeight + 20);
    PreparePeerForRecovery(ownerPeer, MIN_PEER_PROTO_VERSION,
                           nBestHeight + 10);
    std::vector<CNode*> peers;
    peers.push_back(&preferredPeer);
    peers.push_back(&ownerPeer);
    CStalledSyncRecoveryState state;
    state.MarkSyncRequestSent(TEST_TIME);

    BOOST_CHECK(MaybeQueueStalledSyncRecovery(
                    peers, pindexBest, nBestHeight, TEST_TIME,
                    STALL_TIMEOUT, RECOVERY_COOLDOWN, state) == NULL);
    ownerPeer.AddAskForEntry(std::make_pair(
        (TEST_TIME + 1) * 1000000,
        CInv(MSG_BLOCK, hashOwnedByOtherPeer)));

    BOOST_CHECK(MaybeQueueStalledSyncRecovery(
                    peers, pindexBest, nBestHeight,
                    TEST_TIME + STALL_TIMEOUT + 1,
                    STALL_TIMEOUT, RECOVERY_COOLDOWN, state) == &preferredPeer);
    BOOST_CHECK_EQUAL(QueuedGetBlocksCount(peers), 1U);
    BOOST_CHECK_EQUAL(preferredPeer.getBlocksIndex.size(), 1U);
    BOOST_CHECK_EQUAL(
        QueuedBlockAskForCount(peers, hashOwnedByOtherPeer), 1U);
}

BOOST_AUTO_TEST_CASE(stalled_sync_recovery_local_height_progress_resets_timer)
{
    BOOST_REQUIRE(pindexBest != NULL);
    static const int64_t STALL_TIMEOUT = 15;
    static const int64_t RECOVERY_COOLDOWN = 30;

    CNode peer(INVALID_SOCKET, TestPeerAddress(29), "height-progress-peer", true);
    PreparePeerForRecovery(peer, PROTOCOL_VERSION, nBestHeight + 10);
    std::vector<CNode*> peers(1, &peer);
    CStalledSyncRecoveryState state;
    state.MarkSyncRequestSent(TEST_TIME);

    BOOST_CHECK(MaybeQueueStalledSyncRecovery(
                    peers, pindexBest, nBestHeight, TEST_TIME,
                    STALL_TIMEOUT, RECOVERY_COOLDOWN, state) == NULL);

    const int nAdvancedHeight = nBestHeight + 1;
    const int64_t nProgressTime = TEST_TIME + STALL_TIMEOUT + 1;
    BOOST_CHECK(MaybeQueueStalledSyncRecovery(
                    peers, pindexBest, nAdvancedHeight, nProgressTime,
                    STALL_TIMEOUT, RECOVERY_COOLDOWN, state) == NULL);
    BOOST_CHECK_EQUAL(QueuedGetBlocksCount(peers), 0U);
    BOOST_CHECK(MaybeQueueStalledSyncRecovery(
                    peers, pindexBest, nAdvancedHeight,
                    nProgressTime + STALL_TIMEOUT - 1,
                    STALL_TIMEOUT, RECOVERY_COOLDOWN, state) == NULL);
    BOOST_CHECK_EQUAL(QueuedGetBlocksCount(peers), 0U);

    BOOST_REQUIRE(MaybeQueueStalledSyncRecovery(
                      peers, pindexBest, nAdvancedHeight,
                      nProgressTime + STALL_TIMEOUT,
                      STALL_TIMEOUT, RECOVERY_COOLDOWN, state) == &peer);
    BOOST_CHECK_EQUAL(QueuedGetBlocksCount(peers), 1U);
}

BOOST_AUTO_TEST_CASE(rejected_block_recovery_queues_one_cross_peer_askfor)
{
    BOOST_REQUIRE(pindexBest != NULL);
    static const int64_t STALL_TIMEOUT = 15;
    static const int64_t RECOVERY_COOLDOWN = 30;
    const uint256 hashRejected(2002);
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;

    CNode legacyPeer(INVALID_SOCKET, TestPeerAddress(16), "legacy-reject-peer", true);
    CNode currentPeer(INVALID_SOCKET, TestPeerAddress(17), "current-reject-peer", true);
    PreparePeerForRecovery(legacyPeer, MIN_PEER_PROTO_VERSION, nBestHeight + 5);
    PreparePeerForRecovery(currentPeer, PROTOCOL_VERSION, nBestHeight + 10);
    std::vector<CNode*> peers;
    peers.push_back(&legacyPeer);
    peers.push_back(&currentPeer);
    CStalledSyncRecoveryState state;
    state.MarkSyncRequestSent(TEST_TIME);

    BOOST_CHECK(MaybeQueueStalledSyncRecovery(
                    peers, pindexBest, nBestHeight, TEST_TIME,
                    STALL_TIMEOUT, RECOVERY_COOLDOWN, state) == NULL);
    state.RecordRejectedBlock(hashRejected, TEST_TIME + 1);

    const int64_t nRecoveryTime = TEST_TIME + STALL_TIMEOUT + 1;
    CNode* owner = MaybeQueueStalledSyncRecovery(
        peers, pindexBest, nBestHeight, nRecoveryTime,
        STALL_TIMEOUT, RECOVERY_COOLDOWN, state);
    BOOST_REQUIRE(owner != NULL);
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(peers, hashRejected), 1U);
    BOOST_CHECK_EQUAL(QueuedGetBlocksCount(peers), 1U);

    for (std::vector<CNode*>::iterator it = peers.begin(); it != peers.end(); ++it)
    {
        (*it)->ClearAskFor();
        (*it)->getBlocksIndex.clear();
        (*it)->getBlocksHash.clear();
        (*it)->pindexLastGetBlocksBegin = NULL;
        (*it)->hashLastGetBlocksEnd = 0;
        (*it)->nLastGetBlocksTime = 0;
    }
    state.RecordRejectedBlock(hashRejected, nRecoveryTime + 1);

    CNode* nextOwner = MaybeQueueStalledSyncRecovery(
        peers, pindexBest, nBestHeight,
        nRecoveryTime + 2 * RECOVERY_COOLDOWN,
        STALL_TIMEOUT, RECOVERY_COOLDOWN, state);
    BOOST_REQUIRE(nextOwner != NULL);
    BOOST_CHECK(nextOwner != owner);
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(peers, hashRejected), 0U);
    BOOST_CHECK_EQUAL(QueuedGetBlocksCount(peers), 1U);
}

BOOST_AUTO_TEST_CASE(accepted_rejected_block_is_not_directly_retried)
{
    BOOST_REQUIRE(pindexBest != NULL);
    static const int64_t STALL_TIMEOUT = 15;
    static const int64_t RECOVERY_COOLDOWN = 30;
    const uint256 hashAccepted(2003);
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;

    CNode peer(INVALID_SOCKET, TestPeerAddress(19), "accepted-reject-peer", true);
    PreparePeerForRecovery(peer, PROTOCOL_VERSION, nBestHeight + 10);
    std::vector<CNode*> peers(1, &peer);
    CStalledSyncRecoveryState state;
    state.MarkSyncRequestSent(TEST_TIME);

    BOOST_CHECK(MaybeQueueStalledSyncRecovery(
                    peers, pindexBest, nBestHeight, TEST_TIME,
                    STALL_TIMEOUT, RECOVERY_COOLDOWN, state) == NULL);
    state.RecordRejectedBlock(hashAccepted, TEST_TIME + 1);
    state.ClearRejectedBlock(hashAccepted);

    BOOST_REQUIRE(MaybeQueueStalledSyncRecovery(
                      peers, pindexBest, nBestHeight,
                      TEST_TIME + STALL_TIMEOUT + 1,
                      STALL_TIMEOUT, RECOVERY_COOLDOWN, state) == &peer);
    BOOST_CHECK_EQUAL(QueuedGetBlocksCount(peers), 1U);
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(peers, hashAccepted), 0U);
}

BOOST_AUTO_TEST_CASE(orphan_limit_reject_does_not_retry_every_five_seconds)
{
    BOOST_REQUIRE(pindexBest != NULL);
    static const int64_t STALL_TIMEOUT = 15;
    static const int64_t RECOVERY_COOLDOWN = 30;
    const uint256 hashRejectedAtLimit(2004);
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;

    CNode peer(INVALID_SOCKET, TestPeerAddress(45), "orphan-limit-retry-peer", true);
    PreparePeerForRecovery(peer, PROTOCOL_VERSION, nBestHeight + 10);
    std::vector<CNode*> peers(1, &peer);
    CStalledSyncRecoveryState state;
    state.MarkSyncRequestSent(TEST_TIME);
    state.RecordRejectedBlock(hashRejectedAtLimit, TEST_TIME + 1, false);

    BOOST_REQUIRE(MaybeQueueStalledSyncRecovery(
                      peers, pindexBest, nBestHeight,
                      TEST_TIME + STALL_TIMEOUT + 1,
                      STALL_TIMEOUT, RECOVERY_COOLDOWN, state) == &peer);
    BOOST_CHECK_EQUAL(QueuedGetBlocksCount(peers), 1U);
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(peers, hashRejectedAtLimit), 0U);
}

BOOST_AUTO_TEST_CASE(ordinary_getblocks_initial_sync_arms_recovery)
{
    BOOST_REQUIRE(pindexBest != NULL);
    static const int64_t STALL_TIMEOUT = 15;
    static const int64_t RECOVERY_COOLDOWN = 30;
    const bool fSPVModeSaved = fSPVMode;
    fSPVMode = false;

    CNode peer(INVALID_SOCKET, TestPeerAddress(60), "arm-initial-sync", true);
    PreparePeerForRecovery(peer, PROTOCOL_VERSION, nBestHeight + 10);
    ScopedPeerSocket peerSocket(peer);
    std::vector<CNode*> peers(1, &peer);

    ResetSyncPeerForTesting();
    ResetStalledSyncRecoveryStateForTesting();
    StartSyncForTesting(peers);
    BOOST_CHECK(peer.fStartSync);
    BOOST_CHECK(SendMessages(&peer, true));
    BOOST_CHECK(SendMessages(&peer, true));

    CStalledSyncRecoveryState& state =
        GetStalledSyncRecoveryStateForTesting();
    BOOST_CHECK(state.SyncRequestSent());
    BOOST_CHECK_EQUAL(state.LastObservedHeight(), nBestHeight);
    BOOST_CHECK(state.LastProgressTime() != 0);

    // Armed with a static height and an empty pipeline, recovery must fire
    // once the stall timeout elapses.  The recovery getblocks is the same
    // locator sent during SendMessages, so clear the peer's real-clock dedup
    // window (a genuine stall is always >= stall-timeout seconds old).
    peer.nLastGetBlocksTime = 0;
    std::string reason;
    CNode* owner = MaybeQueueStalledSyncRecovery(
        peers, pindexBest, nBestHeight, GetTime() + STALL_TIMEOUT + 1,
        STALL_TIMEOUT, RECOVERY_COOLDOWN, state, &reason);
    BOOST_REQUIRE(owner != NULL);
    BOOST_CHECK_EQUAL(QueuedGetBlocksCount(peers), 1U);

    ResetSyncPeerForTesting();
    fSPVMode = fSPVModeSaved;
}

BOOST_AUTO_TEST_CASE(ordinary_getblocks_non_initial_arms_recovery)
{
    BOOST_REQUIRE(pindexBest != NULL);
    const bool fSPVModeSaved = fSPVMode;
    fSPVMode = false;

    CNode peer(INVALID_SOCKET, TestPeerAddress(61), "arm-non-initial", true);
    PreparePeerForRecovery(peer, PROTOCOL_VERSION, nBestHeight + 10);
    ScopedPeerSocket peerSocket(peer);
    std::vector<CNode*> peers(1, &peer);

    ResetStalledSyncRecoveryStateForTesting();
    // A non-initial, INV/orphan-continuation style getblocks: pushed directly,
    // never gated on the peer's initial-sync lifecycle flags.
    peer.PushGetBlocks(pindexBest, uint256(0));
    BOOST_CHECK(!peer.fInitialSyncRequestPending);
    BOOST_CHECK(SendMessages(&peer, true));

    CStalledSyncRecoveryState& state =
        GetStalledSyncRecoveryStateForTesting();
    BOOST_CHECK(state.SyncRequestSent());
    BOOST_CHECK_EQUAL(state.LastObservedHeight(), nBestHeight);
    BOOST_CHECK(state.LastProgressTime() != 0);

    // A getblocks committed to a peer that cannot advance block sync must NOT
    // arm the recovery state: arming is tied to an ahead peer.
    ResetStalledSyncRecoveryStateForTesting();
    CNode behindPeer(INVALID_SOCKET, TestPeerAddress(62), "arm-behind", true);
    PreparePeerForRecovery(behindPeer, PROTOCOL_VERSION, nBestHeight - 1);
    behindPeer.PushGetBlocks(pindexBest, uint256(0));
    BOOST_CHECK(SendMessages(&behindPeer, true));
    BOOST_CHECK(!GetStalledSyncRecoveryStateForTesting().SyncRequestSent());

    fSPVMode = fSPVModeSaved;
}

BOOST_AUTO_TEST_CASE(recovery_tagged_getblocks_preserves_state)
{
    BOOST_REQUIRE(pindexBest != NULL);
    const bool fSPVModeSaved = fSPVMode;
    fSPVMode = false;

    CNode peer(INVALID_SOCKET, TestPeerAddress(63), "arm-recovery-tagged", true);
    PreparePeerForRecovery(peer, PROTOCOL_VERSION, nBestHeight + 10);
    ScopedPeerSocket peerSocket(peer);
    std::vector<CNode*> peers(1, &peer);

    ResetStalledSyncRecoveryStateForTesting();
    CStalledSyncRecoveryState& state =
        GetStalledSyncRecoveryStateForTesting();
    state.MarkSyncRequestSent(TEST_TIME);

    // A recovery-tagged getblocks must neither re-arm nor reset the timer.
    peer.nRecoveryTracePendingId = 4242;
    peer.PushGetBlocks(pindexBest, uint256(0));
    BOOST_CHECK(SendMessages(&peer, true));

    BOOST_CHECK(state.SyncRequestSent());
    BOOST_CHECK_EQUAL(state.LastProgressTime(), TEST_TIME);
    BOOST_CHECK_EQUAL(state.LastObservedHeight(), nBestHeight);
    BOOST_CHECK_EQUAL(state.RecoveryAttempts(), 0U);

    fSPVMode = fSPVModeSaved;
}

BOOST_AUTO_TEST_CASE(unarmed_state_reports_sync_request_not_sent)
{
    BOOST_REQUIRE(pindexBest != NULL);
    static const int64_t STALL_TIMEOUT = 15;
    static const int64_t RECOVERY_COOLDOWN = 30;

    CNode peer(INVALID_SOCKET, TestPeerAddress(64), "unarmed-recovery-peer", true);
    PreparePeerForRecovery(peer, PROTOCOL_VERSION, nBestHeight + 10);
    std::vector<CNode*> peers(1, &peer);
    CStalledSyncRecoveryState state; // never armed

    std::string reason;
    BOOST_CHECK(MaybeQueueStalledSyncRecovery(
                    peers, pindexBest, nBestHeight, TEST_TIME + 100,
                    STALL_TIMEOUT, RECOVERY_COOLDOWN, state, &reason) == NULL);
    BOOST_CHECK_EQUAL(reason, "sync_request_not_sent");

    // A differing height must NOT masquerade as the cause while unarmed.
    reason.clear();
    BOOST_CHECK(MaybeQueueStalledSyncRecovery(
                    peers, pindexBest, nBestHeight + 7, TEST_TIME + 100,
                    STALL_TIMEOUT, RECOVERY_COOLDOWN, state, &reason) == NULL);
    BOOST_CHECK_EQUAL(reason, "sync_request_not_sent");
}

BOOST_AUTO_TEST_CASE(armed_state_reaches_recovery_on_empty_pipeline)
{
    BOOST_REQUIRE(pindexBest != NULL);
    static const int64_t STALL_TIMEOUT = 15;
    static const int64_t RECOVERY_COOLDOWN = 30;

    CNode peer(INVALID_SOCKET, TestPeerAddress(65), "armed-empty-pipeline", true);
    PreparePeerForRecovery(peer, PROTOCOL_VERSION, nBestHeight + 10);
    std::vector<CNode*> peers(1, &peer);
    CStalledSyncRecoveryState state;
    state.MarkSyncRequestSent(TEST_TIME);

    std::string reason;
    BOOST_CHECK(MaybeQueueStalledSyncRecovery(
                    peers, pindexBest, nBestHeight, TEST_TIME,
                    STALL_TIMEOUT, RECOVERY_COOLDOWN, state, &reason) == NULL);
    BOOST_CHECK_EQUAL(reason, "stall_timeout_not_reached");

    CNode* owner = MaybeQueueStalledSyncRecovery(
        peers, pindexBest, nBestHeight, TEST_TIME + STALL_TIMEOUT + 1,
        STALL_TIMEOUT, RECOVERY_COOLDOWN, state);
    BOOST_REQUIRE(owner != NULL);
    BOOST_CHECK_EQUAL(QueuedGetBlocksCount(peers), 1U);
}

BOOST_AUTO_TEST_CASE(height_advance_resets_stall_timer)
{
    BOOST_REQUIRE(pindexBest != NULL);
    static const int64_t STALL_TIMEOUT = 15;
    static const int64_t RECOVERY_COOLDOWN = 30;

    CNode peer(INVALID_SOCKET, TestPeerAddress(66), "height-advance-peer", true);
    PreparePeerForRecovery(peer, PROTOCOL_VERSION, nBestHeight + 10);
    std::vector<CNode*> peers(1, &peer);
    CStalledSyncRecoveryState state;
    state.MarkSyncRequestSent(TEST_TIME);

    // A real height advance resets the stall timer: recovery is suppressed
    // even though the pre-advance stall window has long since elapsed.
    const int64_t nAdvanceTime = TEST_TIME + 40;
    std::string reason;
    BOOST_CHECK(MaybeQueueStalledSyncRecovery(
                    peers, pindexBest, nBestHeight + 1, nAdvanceTime,
                    STALL_TIMEOUT, RECOVERY_COOLDOWN, state, &reason) == NULL);
    BOOST_CHECK_EQUAL(reason, "local_height_changed");
    BOOST_CHECK_EQUAL(state.LastObservedHeight(), nBestHeight + 1);
    BOOST_CHECK_EQUAL(state.LastProgressTime(), nAdvanceTime);

    // Nothing fires until a full fresh timeout window from the reset.
    BOOST_CHECK(MaybeQueueStalledSyncRecovery(
                    peers, pindexBest, nBestHeight + 1,
                    nAdvanceTime + STALL_TIMEOUT - 1,
                    STALL_TIMEOUT, RECOVERY_COOLDOWN, state) == NULL);

    CNode* owner = MaybeQueueStalledSyncRecovery(
        peers, pindexBest, nBestHeight + 1, nAdvanceTime + STALL_TIMEOUT + 1,
        STALL_TIMEOUT, RECOVERY_COOLDOWN, state);
    BOOST_REQUIRE(owner != NULL);
    BOOST_CHECK_EQUAL(QueuedGetBlocksCount(peers), 1U);
}

BOOST_AUTO_TEST_CASE(orphan_capacity_release_allows_deferred_block_retry)
{
    BOOST_REQUIRE(pindexBest != NULL);
    static const int64_t STALL_TIMEOUT = 15;
    static const int64_t RECOVERY_COOLDOWN = 30;
    const uint256 hashDeferred(2005);
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;

    CNode peer(INVALID_SOCKET, TestPeerAddress(46), "orphan-capacity-peer", true);
    PreparePeerForRecovery(peer, PROTOCOL_VERSION, nBestHeight + 10);
    std::vector<CNode*> peers(1, &peer);
    CStalledSyncRecoveryState state;
    state.MarkSyncRequestSent(TEST_TIME);
    state.RecordRejectedBlock(hashDeferred, TEST_TIME + 1, false);

    BOOST_REQUIRE(MaybeQueueStalledSyncRecovery(
                      peers, pindexBest, nBestHeight,
                      TEST_TIME + STALL_TIMEOUT + 1,
                      STALL_TIMEOUT, RECOVERY_COOLDOWN, state) == &peer);
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(peers, hashDeferred), 0U);

    peer.ClearAskFor();
    peer.getBlocksIndex.clear();
    peer.getBlocksHash.clear();
    peer.pindexLastGetBlocksBegin = NULL;
    peer.hashLastGetBlocksEnd = 0;
    peer.nLastGetBlocksTime = 0;
    const int64_t nDeferredRetryTime =
        TEST_TIME + STALL_TIMEOUT + 2 * RECOVERY_COOLDOWN + 1;
    state.RecordRejectedBlock(hashDeferred, nDeferredRetryTime, true);

    BOOST_REQUIRE(MaybeQueueStalledSyncRecovery(
                      peers, pindexBest, nBestHeight,
                      nDeferredRetryTime,
                      STALL_TIMEOUT, RECOVERY_COOLDOWN, state) == &peer);
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(peers, hashDeferred), 1U);
}

BOOST_AUTO_TEST_CASE(orphan_limit_cooldown_is_peer_local)
{
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    const uint256 hashRejectedAtLimit(2006);
    const uint256 hashParent(2006 - 1);
    const CInv inv(MSG_BLOCK, hashRejectedAtLimit);
    CNode peerA(INVALID_SOCKET, TestPeerAddress(48), "orphan-limit-peer-a", true);
    CNode peerB(INVALID_SOCKET, TestPeerAddress(49), "orphan-limit-peer-b", true);

    RecordOrphanLimitRejectedBlock(
        peerA.GetId(), inv,
        GetTimeMicros() + ORPHAN_LIMIT_REJECT_RETRY_COOLDOWN_US,
        hashParent);

    // The peer that saturated its orphan window remains suppressed.
    peerA.AskFor(inv, BLOCKREQ_SOURCE_INV);
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(peerA, hashRejectedAtLimit), 0U);
    NodeId ownerPeer = -1;
    BlockRequestOwnerState ownerState = BLOCK_REQUEST_OWNER_IN_FLIGHT;
    BOOST_CHECK(!GetBlockRequestOwner(hashRejectedAtLimit, &ownerPeer, &ownerState));

    // A different peer is admitted: the cooldown is peer-local, so the hash is
    // not globally unrequestable and retained pipeline work can exist.
    peerB.AskFor(inv, BLOCKREQ_SOURCE_INV);
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(peerB, hashRejectedAtLimit), 1U);
}

BOOST_AUTO_TEST_CASE(orphan_limit_cooldown_frontier_retry_on_parent_connect)
{
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    const uint256 hashParent(2100);
    const uint256 hashRejectedAtLimit(2101);
    const CInv inv(MSG_BLOCK, hashRejectedAtLimit);
    CNode peerA(INVALID_SOCKET, TestPeerAddress(50), "orphan-limit-retry-peer-a", true);
    CNode peerConnect(INVALID_SOCKET, TestPeerAddress(51), "orphan-limit-retry-peer-c", true);

    // Child rejected at the orphan limit while its parent is missing.
    RecordOrphanLimitRejectedBlock(
        peerA.GetId(), inv,
        GetTimeMicros() + ORPHAN_LIMIT_REJECT_RETRY_COOLDOWN_US,
        hashParent);

    // Suppressed on the rejecting peer: no request, no spontaneous retry.
    peerA.AskFor(inv, BLOCKREQ_SOURCE_INV);
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(peerA, hashRejectedAtLimit), 0U);

    // The parent connects: the rejected child is deterministically re-requested
    // from the connecting peer without waiting out the 120s cooldown and without
    // relying on a spontaneous re-announcement.
    RetryOrphanLimitRejectedOnParentConnect(hashParent, &peerConnect);
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(peerConnect, hashRejectedAtLimit), 1U);

    // The cooldown entry is gone after the retry, so the originally rejecting
    // peer is no longer suppressed either: the block is now useful (its parent
    // is indexed), so re-requesting it is legitimate.
    peerA.AskFor(inv, BLOCKREQ_SOURCE_INV);
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(peerA, hashRejectedAtLimit), 1U);
}

BOOST_AUTO_TEST_CASE(orphan_limit_cooldown_no_request_storm)
{
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    const uint256 hashParent(2200);
    const uint256 hashRejectedAtLimit(2201);
    const CInv inv(MSG_BLOCK, hashRejectedAtLimit);
    CNode peerA(INVALID_SOCKET, TestPeerAddress(52), "orphan-limit-storm-peer-a", true);

    RecordOrphanLimitRejectedBlock(
        peerA.GetId(), inv,
        GetTimeMicros() + ORPHAN_LIMIT_REJECT_RETRY_COOLDOWN_US,
        hashParent);

    // Repeated announcements of the same missing-parent block from the
    // rejecting peer never translate into repeated queued requests during the
    // cooldown window.
    for (int i = 0; i < 10; ++i)
        peerA.AskFor(inv, BLOCKREQ_SOURCE_INV);
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(peerA, hashRejectedAtLimit), 0U);

    // The parent-connect retry queues exactly one bounded request...
    RetryOrphanLimitRejectedOnParentConnect(hashParent, &peerA);
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(peerA, hashRejectedAtLimit), 1U);

    // ...and a second parent-connect event does not fire a second retry.
    RetryOrphanLimitRejectedOnParentConnect(hashParent, &peerA);
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(peerA, hashRejectedAtLimit), 1U);
}

BOOST_AUTO_TEST_CASE(orphan_limit_cooldown_pipeline_retains_work)
{
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    const uint256 hashParent(2300);
    const uint256 hashRejectedAtLimit(2301);
    const CInv inv(MSG_BLOCK, hashRejectedAtLimit);
    CNode peerA(INVALID_SOCKET, TestPeerAddress(53), "orphan-limit-pipe-peer-a", true);
    CNode peerB(INVALID_SOCKET, TestPeerAddress(54), "orphan-limit-pipe-peer-b", true);

    const PipelineWakeGauges before = SnapshotWakeGauges();

    RecordOrphanLimitRejectedBlock(
        peerA.GetId(), inv,
        GetTimeMicros() + ORPHAN_LIMIT_REJECT_RETRY_COOLDOWN_US,
        hashParent);

    // Empty-pipeline condition on the rejecting peer: nothing queued.
    peerA.AskFor(inv, BLOCKREQ_SOURCE_INV);
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(peerA, hashRejectedAtLimit), 0U);

    // The eligible alternate peer provides retained pipeline work: the hash is
    // admitted and globally counted despite the rejecting peer's cooldown, so
    // an empty-pipeline wake finds new work instead of stalling.
    peerB.AskFor(inv, BLOCKREQ_SOURCE_INV);
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(peerB, hashRejectedAtLimit), 1U);

    const PipelineWakeGauges after = SnapshotWakeGauges();
    BOOST_CHECK_EQUAL(after.total_queued_current,
                      before.total_queued_current + 1);
    BOOST_CHECK_EQUAL(after.global_active_current,
                      before.global_active_current + 1);
}

BOOST_AUTO_TEST_CASE(frontier_retry_survives_mapalreadyasked_cap)
{
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    const uint256 hashParent(2400);
    const uint256 hashChild(2401);
    const CInv inv(MSG_BLOCK, hashChild);
    CNode peerA(INVALID_SOCKET, TestPeerAddress(60), "frontier-cap-peer-a", true);
    CNode peerConnect(INVALID_SOCKET, TestPeerAddress(61), "frontier-cap-peer-c", true);

    RecordOrphanLimitRejectedBlock(
        peerA.GetId(), inv,
        GetTimeMicros() + ORPHAN_LIMIT_REJECT_RETRY_COOLDOWN_US,
        hashParent);

    // Saturate the peer-agnostic map so the retry's AskFor hits the hard cap.
    // MSG_TX entries with a fresh timestamp survive PruneAlreadyAskedFor.
    {
        LOCK(cs_mapAlreadyAskedFor);
        const int64_t nNow = GetTimeMicros();
        for (size_t i = 0; i < MAX_ALREADY_ASKED_FOR_SIZE; ++i)
            mapAlreadyAskedFor[CInv(MSG_TX, uint256(2402 + i))] = nNow;
        BOOST_CHECK_EQUAL(mapAlreadyAskedFor.size(),
                          MAX_ALREADY_ASKED_FOR_SIZE);
    }

    const int64_t nQueuedBefore =
        MetricGet(ibdmetrics::Get().orphan_limit_frontier_retry_queued);
    const int64_t nPendingBefore =
        MetricGet(ibdmetrics::Get().orphan_limit_frontier_retry_pending);

    // The cap-full retry cannot queue the child, but must not lose it: the
    // cooldown entry stays as a bounded retry state.
    RetryOrphanLimitRejectedOnParentConnect(hashParent, &peerConnect);
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(peerConnect, hashChild), 0U);
    BOOST_CHECK_EQUAL(GetOrphanLimitRejectedEntryCountForPeer(peerA.GetId()),
                      1U);
    BOOST_CHECK_EQUAL(
        MetricGet(ibdmetrics::Get().orphan_limit_frontier_retry_queued) -
            nQueuedBefore,
        0);
    BOOST_CHECK_EQUAL(
        MetricGet(ibdmetrics::Get().orphan_limit_frontier_retry_pending) -
            nPendingBefore,
        1);

    // Once the cap is freed the very next retry deterministically creates a
    // retained request and releases the cooldown entry.
    {
        LOCK(cs_mapAlreadyAskedFor);
        mapAlreadyAskedFor.clear();
    }
    RetryOrphanLimitRejectedOnParentConnect(hashParent, &peerConnect);
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(peerConnect, hashChild), 1U);
    BOOST_CHECK_EQUAL(GetOrphanLimitRejectedEntryCountForPeer(peerA.GetId()),
                      0U);
    BOOST_CHECK_EQUAL(
        MetricGet(ibdmetrics::Get().orphan_limit_frontier_retry_queued) -
            nQueuedBefore,
        1);
    BOOST_CHECK_EQUAL(
        MetricGet(ibdmetrics::Get().orphan_limit_frontier_retry_pending) -
            nPendingBefore,
        1);
}

BOOST_AUTO_TEST_CASE(frontier_retry_counter_means_retained)
{
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    const uint256 hashParent(2410);
    const uint256 hashChild1(2411);
    const uint256 hashChild2(2412);
    const CInv inv1(MSG_BLOCK, hashChild1);
    const CInv inv2(MSG_BLOCK, hashChild2);
    CNode peerA(INVALID_SOCKET, TestPeerAddress(62), "frontier-count-peer-a", true);
    CNode peerConnect(INVALID_SOCKET, TestPeerAddress(63), "frontier-count-peer-c", true);

    RecordOrphanLimitRejectedBlock(
        peerA.GetId(), inv1,
        GetTimeMicros() + ORPHAN_LIMIT_REJECT_RETRY_COOLDOWN_US,
        hashParent);

    const int64_t nQueuedBefore =
        MetricGet(ibdmetrics::Get().orphan_limit_frontier_retry_queued);
    const int64_t nPendingBefore =
        MetricGet(ibdmetrics::Get().orphan_limit_frontier_retry_pending);

    // A plain AskFor admission attempt (suppressed here) never drives the
    // frontier-retry counters: they count only parent-connect retries.
    peerA.AskFor(inv1, BLOCKREQ_SOURCE_INV);
    BOOST_CHECK_EQUAL(
        MetricGet(ibdmetrics::Get().orphan_limit_frontier_retry_queued) -
            nQueuedBefore,
        0);
    BOOST_CHECK_EQUAL(
        MetricGet(ibdmetrics::Get().orphan_limit_frontier_retry_pending) -
            nPendingBefore,
        0);

    // A retained retry (queued) counts as frontier-retry-queued work.
    RetryOrphanLimitRejectedOnParentConnect(hashParent, &peerConnect);
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(peerConnect, hashChild1), 1U);
    BOOST_CHECK_EQUAL(
        MetricGet(ibdmetrics::Get().orphan_limit_frontier_retry_queued) -
            nQueuedBefore,
        1);
    BOOST_CHECK_EQUAL(
        MetricGet(ibdmetrics::Get().orphan_limit_frontier_retry_pending) -
            nPendingBefore,
        0);

    // A non-retaining retry (cap full) counts as pending, not queued.
    RecordOrphanLimitRejectedBlock(
        peerA.GetId(), inv2,
        GetTimeMicros() + ORPHAN_LIMIT_REJECT_RETRY_COOLDOWN_US,
        hashParent);
    {
        LOCK(cs_mapAlreadyAskedFor);
        const int64_t nNow = GetTimeMicros();
        for (size_t i = 0; i < MAX_ALREADY_ASKED_FOR_SIZE; ++i)
            mapAlreadyAskedFor[CInv(MSG_TX, uint256(2413 + i))] = nNow;
    }
    RetryOrphanLimitRejectedOnParentConnect(hashParent, &peerConnect);
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(peerConnect, hashChild2), 0U);
    BOOST_CHECK_EQUAL(GetOrphanLimitRejectedEntryCountForPeer(peerA.GetId()),
                      1U);
    BOOST_CHECK_EQUAL(
        MetricGet(ibdmetrics::Get().orphan_limit_frontier_retry_queued) -
            nQueuedBefore,
        1);
    BOOST_CHECK_EQUAL(
        MetricGet(ibdmetrics::Get().orphan_limit_frontier_retry_pending) -
            nPendingBefore,
        1);
}

BOOST_AUTO_TEST_CASE(cooldown_map_per_peer_cap)
{
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CNode peerA(INVALID_SOCKET, TestPeerAddress(64), "cooldown-per-peer-a", true);
    const int64_t nBase = GetTimeMicros();
    for (size_t i = 0;
         i < MAX_ORPHAN_LIMIT_REJECTED_PER_PEER + 100; ++i)
    {
        RecordOrphanLimitRejectedBlock(
            peerA.GetId(),
            CInv(MSG_BLOCK, uint256(3000 + i)),
            nBase + static_cast<int64_t>(i) * 1000,
            uint256(3999));
    }

    // The strict per-peer bound holds: a hostile flood from one peer keeps
    // exactly MAX_ORPHAN_LIMIT_REJECTED_PER_PEER entries.
    BOOST_CHECK_EQUAL(GetOrphanLimitRejectedEntryCountForPeer(peerA.GetId()),
                      MAX_ORPHAN_LIMIT_REJECTED_PER_PEER);
    BOOST_CHECK(GetOrphanLimitRejectedEntryCount() <=
                MAX_ORPHAN_LIMIT_REJECTED_GLOBAL);

    // Deterministic earliest-expiry eviction: the oldest entry is gone while
    // the newest (largest expiry) entry survives.
    BOOST_CHECK(!IsOrphanLimitRejectedBlockInCooldown(
        peerA.GetId(), CInv(MSG_BLOCK, uint256(3000)), nBase, NULL));
    const size_t nLastIndex =
        MAX_ORPHAN_LIMIT_REJECTED_PER_PEER + 100 - 1;
    BOOST_CHECK(IsOrphanLimitRejectedBlockInCooldown(
        peerA.GetId(), CInv(MSG_BLOCK, uint256(3000 + nLastIndex)),
        nBase, NULL));
}

BOOST_AUTO_TEST_CASE(cooldown_map_global_cap)
{
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    std::vector<CNode*> peers;
    const int64_t nBase = GetTimeMicros();
    for (size_t p = 0; p < 70; ++p)
        peers.push_back(new CNode(INVALID_SOCKET,
                                  TestPeerAddress(100 + p),
                                  "cooldown-global-peer", true));

    // 70 peers * 750 entries each far exceeds the process-global bound.
    for (size_t p = 0; p < peers.size(); ++p)
    {
        for (size_t i = 0; i < MAX_ORPHAN_LIMIT_REJECTED_PER_PEER; ++i)
        {
            RecordOrphanLimitRejectedBlock(
                peers[p]->GetId(),
                CInv(MSG_BLOCK, uint256(4000 + p * 1000 + i)),
                nBase + static_cast<int64_t>(p * 1000 + i) * 1000,
                uint256(4999));
        }
    }

    // The process-global bound holds even though every peer individually
    // saturates its per-peer bound.
    BOOST_CHECK_EQUAL(GetOrphanLimitRejectedEntryCount(),
                      MAX_ORPHAN_LIMIT_REJECTED_GLOBAL);
    for (size_t p = 0; p < peers.size(); ++p)
        BOOST_CHECK(GetOrphanLimitRejectedEntryCountForPeer(peers[p]->GetId()) <=
                    MAX_ORPHAN_LIMIT_REJECTED_PER_PEER);

    for (size_t p = 0; p < peers.size(); ++p)
        delete peers[p];
}

BOOST_AUTO_TEST_CASE(cap_does_not_restore_cross_peer_suppression)
{
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    const uint256 hashParent(2700);
    const uint256 hashChild(2701);
    const CInv inv(MSG_BLOCK, hashChild);
    CNode peerA(INVALID_SOCKET, TestPeerAddress(65), "cap-cross-peer-a", true);
    CNode peerB(INVALID_SOCKET, TestPeerAddress(66), "cap-cross-peer-b", true);

    // Saturate peer A's cooldown map: the child is recorded last with the
    // largest expiry so it survives the per-peer evictions.
    const int64_t nBase = GetTimeMicros();
    for (size_t i = 0;
         i < MAX_ORPHAN_LIMIT_REJECTED_PER_PEER - 1; ++i)
    {
        RecordOrphanLimitRejectedBlock(
            peerA.GetId(),
            CInv(MSG_BLOCK, uint256(2800 + i)),
            nBase + static_cast<int64_t>(i) * 1000,
            uint256(2799));
    }
    RecordOrphanLimitRejectedBlock(
        peerA.GetId(), inv,
        nBase + static_cast<int64_t>(MAX_ORPHAN_LIMIT_REJECTED_PER_PEER) * 1000,
        hashParent);
    BOOST_CHECK_EQUAL(GetOrphanLimitRejectedEntryCountForPeer(peerA.GetId()),
                      MAX_ORPHAN_LIMIT_REJECTED_PER_PEER);

    // The rejection path writes only the short negative cooldown into the
    // peer-agnostic map - never the 120s peer-local blocker.
    RecordRejectedBlockGlobalNegativeCooldown(inv);
    {
        LOCK(cs_mapAlreadyAskedFor);
        BOOST_CHECK(mapAlreadyAskedFor[inv] <=
                    GetTimeMicros() + ALREADY_ASKED_FOR_NEGATIVE_COOLDOWN_US);
    }

    // Another peer's cooldown is active for this hash, but peer B is still
    // admitted: the cap and the peer-local cooldown do not restore the old
    // cross-peer 120s suppression.
    BOOST_CHECK(IsOrphanLimitRejectedByOtherPeer(
        peerB.GetId(), inv, GetTimeMicros()));
    peerB.AskFor(inv, BLOCKREQ_SOURCE_INV);
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(peerB, hashChild), 1U);
}

BOOST_AUTO_TEST_CASE(preexisting_sent_request_may_complete_after_suppression)
{
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    const uint256 hashRejectedAtLimit(2007);
    const CInv inv(MSG_BLOCK, hashRejectedAtLimit);
    CNode peer(INVALID_SOCKET, TestPeerAddress(49), "preexisting-send-peer", true);

    peer.MarkBlockInFlight(hashRejectedAtLimit);
    {
        LOCK(cs_mapAlreadyAskedFor);
        mapAlreadyAskedFor[inv] = GetTimeMicros() +
            ORPHAN_LIMIT_REJECT_RETRY_COOLDOWN_US;
    }

    BOOST_CHECK_EQUAL(peer.setBlocksInFlight.count(hashRejectedAtLimit), 1U);
    peer.ClearBlockInFlight(hashRejectedAtLimit);
    BOOST_CHECK_EQUAL(peer.setBlocksInFlight.count(hashRejectedAtLimit), 0U);
}

BOOST_AUTO_TEST_CASE(send_messages_does_not_own_stall_recovery)
{
    BOOST_REQUIRE(pindexBest != NULL);
    const bool fSPVModeSaved = fSPVMode;
    fSPVMode = false;

    CNode behindPeer(INVALID_SOCKET, TestPeerAddress(7), "behind-stall-peer", true);
    PreparePeerForSendMessages(behindPeer, PROTOCOL_VERSION);
    behindPeer.nChainHeight = nBestHeight - 1;
    behindPeer.nBestKnownHeight = nBestHeight - 1;
    behindPeer.nLastBlockRecv = GetTime() - 20;
    BOOST_CHECK(!behindPeer.CanAdvanceBlockSync(nBestHeight));
    BOOST_CHECK(!behindPeer.ShouldContinueKnownBlockInventory(nBestHeight, true));
    BOOST_CHECK(behindPeer.ShouldContinueKnownBlockInventory(nBestHeight, false));
    BOOST_CHECK(SendMessages(&behindPeer, true));
    BOOST_CHECK(!HasCommand(SentCommands(behindPeer), "getblocks"));

    CNode aheadPeer(INVALID_SOCKET, TestPeerAddress(8), "ahead-stall-peer", true);
    PreparePeerForSendMessages(aheadPeer, PROTOCOL_VERSION);
    aheadPeer.nChainHeight = nBestHeight + 1;
    aheadPeer.nBestKnownHeight = nBestHeight + 1;
    aheadPeer.nLastBlockRecv = GetTime() - 20;
    BOOST_CHECK(aheadPeer.CanAdvanceBlockSync(nBestHeight));
    BOOST_CHECK(aheadPeer.ShouldContinueKnownBlockInventory(nBestHeight, true));
    BOOST_CHECK(SendMessages(&aheadPeer, true));
    // Recovery is coordinated once per message-handler tick.  Per-peer
    // SendMessages must not independently create a second getblocks wave.
    BOOST_CHECK(!HasCommand(SentCommands(aheadPeer), "getblocks"));

    fSPVMode = fSPVModeSaved;
}

BOOST_AUTO_TEST_CASE(start_sync_flag_uses_only_ahead_full_node_peer)
{
    BOOST_REQUIRE(pindexBest != NULL);
    const bool fSPVModeSaved = fSPVMode;
    fSPVMode = false;

    CNode behindPeer(INVALID_SOCKET, TestPeerAddress(9), "behind-start-peer", true);
    PreparePeerForSendMessages(behindPeer, PROTOCOL_VERSION);
    behindPeer.nChainHeight = nBestHeight - 1;
    behindPeer.nBestKnownHeight = nBestHeight - 1;
    behindPeer.fStartSync = true;
    BOOST_CHECK(SendMessages(&behindPeer, true));
    BOOST_CHECK(!behindPeer.fStartSync);
    BOOST_CHECK(!HasCommand(SentCommands(behindPeer), "getblocks"));

    CNode aheadPeer(INVALID_SOCKET, TestPeerAddress(10), "ahead-start-peer", true);
    PreparePeerForSendMessages(aheadPeer, PROTOCOL_VERSION);
    aheadPeer.nChainHeight = nBestHeight + 1;
    aheadPeer.nBestKnownHeight = nBestHeight + 1;
    aheadPeer.fStartSync = true;
    BOOST_CHECK(SendMessages(&aheadPeer, true));
    BOOST_CHECK(!aheadPeer.fStartSync);
    BOOST_CHECK(HasCommand(SentCommands(aheadPeer), "getblocks"));

    CNode spvPeer(INVALID_SOCKET, TestPeerAddress(11), "spv-start-peer", true);
    PreparePeerForSendMessages(spvPeer, PROTOCOL_VERSION);
    spvPeer.nChainHeight = nBestHeight + 1;
    spvPeer.nBestKnownHeight = nBestHeight + 1;
    spvPeer.fStartSync = true;
    fSPVMode = true;
    BOOST_CHECK(SendMessages(&spvPeer, true));
    BOOST_CHECK(!spvPeer.fStartSync);
    BOOST_CHECK(!HasCommand(SentCommands(spvPeer), "getblocks"));

    fSPVMode = fSPVModeSaved;
}

BOOST_AUTO_TEST_CASE(getblocks_response_is_inv_not_headers)
{
    BOOST_REQUIRE(pindexGenesisBlock != NULL);
    const bool fSPVModeSaved = fSPVMode;
    fSPVMode = false;
    const CInv genesisInv(MSG_BLOCK, pindexGenesisBlock->GetBlockHash());

    CNode requestedPeer(INVALID_SOCKET, TestPeerAddress(4), "requested-inventory", true);
    PreparePeerForSendMessages(requestedPeer, PROTOCOL_VERSION);
    requestedPeer.fPreferHeaders = true;
    requestedPeer.PushGetBlocksInventory(genesisInv);
    BOOST_CHECK(SendMessages(&requestedPeer, true));
    const std::vector<std::string> requestedCommands = SentCommands(requestedPeer);
    BOOST_CHECK(HasCommand(requestedCommands, "inv"));
    BOOST_CHECK(!HasCommand(requestedCommands, "headers"));

    CNode announcementPeer(INVALID_SOCKET, TestPeerAddress(5), "header-announcement", true);
    PreparePeerForSendMessages(announcementPeer, PROTOCOL_VERSION);
    announcementPeer.fPreferHeaders = true;
    announcementPeer.PushInventory(genesisInv);
    BOOST_CHECK(SendMessages(&announcementPeer, true));
    const std::vector<std::string> announcementCommands = SentCommands(announcementPeer);
    BOOST_CHECK(HasCommand(announcementCommands, "headers"));
    BOOST_CHECK(!HasCommand(announcementCommands, "inv"));
    fSPVMode = fSPVModeSaved;
}

BOOST_AUTO_TEST_CASE(getblocks_server_normal_legacy_sync)
{
    CheckNormalGetBlocksSync(MIN_PEER_PROTO_VERSION, 30);
}

BOOST_AUTO_TEST_CASE(getblocks_server_normal_current_sync)
{
    CheckNormalGetBlocksSync(PROTOCOL_VERSION, 31);
}

BOOST_AUTO_TEST_CASE(getblocks_server_suppresses_identical_repeat)
{
    CGetBlocksServerState state;
    const CGetBlocksRequestInfo request = TestGetBlocksRequest(
        1, 1000, 1001, 2000, 1000, 1000);
    CGetBlocksServerDecision decision = state.Evaluate(request, true);
    BOOST_REQUIRE_EQUAL(decision.action, GETBLOCKS_SERVER_ALLOW);
    state.RecordResponse(request, TestGetBlocksResponse(request));

    for (int i = 1; i <= 20; ++i)
    {
        CGetBlocksRequestInfo repeated = request;
        repeated.nRequestTimeMillis = 1000 + i * 20;
        decision = state.Evaluate(repeated, true);
        BOOST_CHECK_EQUAL(
            decision.action, GETBLOCKS_SERVER_SUPPRESS);
        BOOST_CHECK(decision.fIdenticalRequest);
        BOOST_CHECK(decision.fSameResponse);
    }

    BOOST_CHECK_EQUAL(state.nResponsesAllowed, 1U);
    BOOST_CHECK_EQUAL(state.nResponsesSuppressed, 20U);
    BOOST_CHECK_EQUAL(state.nIdenticalRequests, 20U);
    BOOST_CHECK_GT(
        state.nEstimatedSuppressedBytes,
        CGetBlocksServerState::EstimateInvPayloadBytes(1000) * 19);
}

BOOST_AUTO_TEST_CASE(getblocks_server_suppresses_same_range_from_changed_locator)
{
    CGetBlocksServerState state;
    const CGetBlocksRequestInfo first = TestGetBlocksRequest(
        10, 0, 1, 1000, 1000, 1000);
    BOOST_REQUIRE_EQUAL(
        state.Evaluate(first, true).action,
        GETBLOCKS_SERVER_ALLOW);
    state.RecordResponse(first, TestGetBlocksResponse(first));

    CGetBlocksRequestInfo cycled = first;
    cycled.hashLocatorTip = uint256(11);
    cycled.nRequestTimeMillis = 1100;
    const CGetBlocksServerDecision decision =
        state.Evaluate(cycled, true);
    BOOST_CHECK(!decision.fIdenticalRequest);
    BOOST_CHECK(decision.fSameResponse);
    BOOST_CHECK_EQUAL(
        decision.action, GETBLOCKS_SERVER_SUPPRESS);
}

BOOST_AUTO_TEST_CASE(getblocks_server_50_per_second_spam_disconnects)
{
    CGetBlocksServerState state;
    const CGetBlocksRequestInfo request = TestGetBlocksRequest(
        20, 0, 1, 1000, 1000, 1000);
    BOOST_REQUIRE_EQUAL(
        state.Evaluate(request, true).action,
        GETBLOCKS_SERVER_ALLOW);
    state.RecordResponse(request, TestGetBlocksResponse(request));

    unsigned int nPenalties = 0;
    CGetBlocksServerDecision decision;
    for (int i = 1; i <= 600; ++i)
    {
        CGetBlocksRequestInfo repeated = request;
        repeated.nRequestTimeMillis = 1000 + i * 20;
        decision = state.Evaluate(repeated, true);
        if (decision.fPenalize)
        {
            BOOST_CHECK_EQUAL(decision.nPenalty, 5);
            nPenalties++;
        }
        if (decision.action == GETBLOCKS_SERVER_DISCONNECT)
            break;
        BOOST_CHECK_EQUAL(
            decision.action, GETBLOCKS_SERVER_SUPPRESS);
    }

    BOOST_CHECK_EQUAL(
        decision.action, GETBLOCKS_SERVER_DISCONNECT);
    BOOST_CHECK_EQUAL(state.nResponsesAllowed, 1U);
    BOOST_CHECK_EQUAL(state.nResponsesSuppressed, 512U);
    BOOST_CHECK_EQUAL(nPenalties, 4U);
    BOOST_CHECK_GE(state.nConsecutiveNonProgressingRequests, 512U);
    BOOST_CHECK_GE(
        state.nEstimatedSuppressedBytes,
        CGetBlocksServerState::EstimateInvPayloadBytes(1000) * 512);
}

BOOST_AUTO_TEST_CASE(getblocks_server_cost_rate_limit_bounds_changed_ranges)
{
    CGetBlocksServerState state;
    unsigned int nAllowed = 0;
    unsigned int nLimited = 0;

    for (int i = 0; i < 50; ++i)
    {
        const CGetBlocksRequestInfo request = TestGetBlocksRequest(
            100 + i, 0, 1000 + i, 2000 + i,
            1000, 1000 + i * 20, 3000 + i, -1);
        const CGetBlocksServerDecision decision =
            state.Evaluate(request, true);
        if (decision.action == GETBLOCKS_SERVER_ALLOW)
        {
            nAllowed++;
            state.RecordResponse(
                request, TestGetBlocksResponse(request));
        }
        else
        {
            BOOST_CHECK_EQUAL(
                decision.action, GETBLOCKS_SERVER_RATE_LIMIT);
            nLimited++;
        }
    }

    BOOST_CHECK_EQUAL(nAllowed, 6U);
    BOOST_CHECK_EQUAL(nLimited, 44U);
    BOOST_CHECK_EQUAL(state.nResponsesAllowed, 6U);
    BOOST_CHECK_EQUAL(state.nRequestsRateLimited, 44U);
}

BOOST_AUTO_TEST_CASE(getblocks_server_timeout_retry_after_cooldown)
{
    CGetBlocksServerState state;
    const CGetBlocksRequestInfo request = TestGetBlocksRequest(
        30, 0, 1, 1000, 1000, 1000);
    BOOST_REQUIRE_EQUAL(
        state.Evaluate(request, true).action,
        GETBLOCKS_SERVER_ALLOW);
    state.RecordResponse(request, TestGetBlocksResponse(request));

    CGetBlocksRequestInfo early = request;
    early.nRequestTimeMillis = 1500;
    BOOST_CHECK_EQUAL(
        state.Evaluate(early, true).action,
        GETBLOCKS_SERVER_SUPPRESS);

    CGetBlocksRequestInfo timeout = request;
    timeout.nRequestTimeMillis = 4000;
    const CGetBlocksServerDecision decision =
        state.Evaluate(timeout, true);
    BOOST_CHECK(decision.fIdenticalRequest);
    BOOST_CHECK_EQUAL(decision.action, GETBLOCKS_SERVER_ALLOW);
    state.RecordResponse(timeout, TestGetBlocksResponse(timeout));
    BOOST_CHECK_EQUAL(state.nResponsesAllowed, 2U);
}

BOOST_AUTO_TEST_CASE(getblocks_server_rapid_locator_progress_uses_burst)
{
    CGetBlocksServerState state;
    for (int i = 0; i < 6; ++i)
    {
        const CGetBlocksRequestInfo request = TestGetBlocksRequest(
            40 + i, i * 1000, 10000 + i * 1000,
            10999 + i * 1000, 1000, 1000);
        const CGetBlocksServerDecision decision =
            state.Evaluate(request, true);
        BOOST_CHECK(decision.fProgress);
        BOOST_CHECK_EQUAL(decision.action, GETBLOCKS_SERVER_ALLOW);
        state.RecordResponse(
            request, TestGetBlocksResponse(request));
    }

    BOOST_CHECK_EQUAL(state.nResponsesAllowed, 6U);
    BOOST_CHECK_EQUAL(state.nResponsesSuppressed, 0U);
    BOOST_CHECK_EQUAL(state.nRequestsRateLimited, 0U);
}

BOOST_AUTO_TEST_CASE(getblocks_server_getdata_resets_repeat_state)
{
    CGetBlocksServerState state;
    const CGetBlocksRequestInfo request = TestGetBlocksRequest(
        50, 0, 1, 1000, 1000, 1000);
    BOOST_REQUIRE_EQUAL(
        state.Evaluate(request, true).action,
        GETBLOCKS_SERVER_ALLOW);
    const CGetBlocksResponseInfo response =
        TestGetBlocksResponse(request);
    state.RecordResponse(request, response);

    CGetBlocksRequestInfo early = request;
    early.nRequestTimeMillis = 1100;
    BOOST_REQUIRE_EQUAL(
        state.Evaluate(early, true).action,
        GETBLOCKS_SERVER_SUPPRESS);
    BOOST_REQUIRE(state.NoteBlockGetData(
        response.hashFirst, response.nMinHeight, 1200));

    CGetBlocksRequestInfo afterGetData = request;
    afterGetData.nRequestTimeMillis = 1300;
    const CGetBlocksServerDecision decision =
        state.Evaluate(afterGetData, true);
    BOOST_CHECK(decision.fProgress);
    BOOST_CHECK_EQUAL(decision.action, GETBLOCKS_SERVER_ALLOW);
    BOOST_CHECK_EQUAL(state.nConsecutiveNonProgressingRequests, 0U);
}

BOOST_AUTO_TEST_CASE(getblocks_server_state_is_fixed_and_per_peer)
{
    BOOST_CHECK_LE(sizeof(CGetBlocksServerState), 1024U);

    CGetBlocksServerState abusiveConnection;
    CGetBlocksServerState parallelConnection;
    const CGetBlocksRequestInfo request = TestGetBlocksRequest(
        60, 0, 1, 1000, 1000, 1000);
    BOOST_REQUIRE_EQUAL(
        abusiveConnection.Evaluate(request, true).action,
        GETBLOCKS_SERVER_ALLOW);
    abusiveConnection.RecordResponse(
        request, TestGetBlocksResponse(request));

    CGetBlocksRequestInfo repeated = request;
    repeated.nRequestTimeMillis = 1100;
    BOOST_CHECK_EQUAL(
        abusiveConnection.Evaluate(repeated, true).action,
        GETBLOCKS_SERVER_SUPPRESS);
    BOOST_CHECK_EQUAL(
        parallelConnection.Evaluate(repeated, true).action,
        GETBLOCKS_SERVER_ALLOW);
    BOOST_CHECK_EQUAL(parallelConnection.nResponsesSuppressed, 0U);
}


BOOST_AUTO_TEST_CASE(recovery_response_window_orphan_then_unknown)
{
    RecoveryResponseWindowState state;
    RecoveryResponseResult result;
    state.Start(1, TEST_TIME);
    RecoveryResponseObservation orphan;
    orphan.total_inv = orphan.block_inv = orphan.known_orphan_blocks = 2;
    orphan.first_block_hash = uint256(1);
    BOOST_CHECK(!state.ObserveInv(TEST_TIME + 1, orphan, result));
    RecoveryResponseObservation unknown;
    unknown.total_inv = unknown.block_inv = unknown.unknown_blocks = 3;
    unknown.first_block_hash = uint256(2);
    unknown.first_unknown_block_hash = uint256(3);
    BOOST_CHECK(!state.ObserveInv(TEST_TIME + 2, unknown, result));
    BOOST_CHECK(state.Expire(TEST_TIME + RECOVERY_RESPONSE_WINDOW_US, result));
    BOOST_CHECK_EQUAL(result.outcome, RECOVERY_OUTCOME_USEFUL);
    BOOST_CHECK_EQUAL(result.inv_message_count, 2U);
    BOOST_CHECK_EQUAL(result.total_inv, 5U);
    BOOST_CHECK_EQUAL(result.block_inv, 5U);
    BOOST_CHECK_EQUAL(result.unknown_blocks + result.known_orphan_blocks, result.block_inv);
    BOOST_CHECK(result.first_block_hash == uint256(1));
    BOOST_CHECK(result.first_unknown_block_hash == uint256(3));
}

BOOST_AUTO_TEST_CASE(recovery_response_window_timeout_and_late_observation)
{
    RecoveryResponseWindowState state;
    RecoveryResponseResult result;
    state.Start(2, TEST_TIME);
    RecoveryResponseObservation known;
    known.total_inv = known.block_inv = 4;
    known.known_active_blocks = 1;
    known.known_nonactive_indexed_blocks = 1;
    known.known_orphan_blocks = 2;
    BOOST_CHECK(!state.ObserveInv(TEST_TIME + 1, known, result));
    BOOST_CHECK(!state.Expire(TEST_TIME + RECOVERY_RESPONSE_WINDOW_US - 1, result));
    BOOST_CHECK(state.Expire(TEST_TIME + RECOVERY_RESPONSE_WINDOW_US, result));
    BOOST_CHECK_EQUAL(result.outcome, RECOVERY_OUTCOME_KNOWN_ONLY_TIMEOUT);
    BOOST_CHECK(!state.ObserveInv(TEST_TIME + RECOVERY_RESPONSE_WINDOW_US + 1, known, result));
    BOOST_CHECK(!state.Expire(TEST_TIME + RECOVERY_RESPONSE_WINDOW_US + 2, result));
    BOOST_CHECK(!state.Disconnect(TEST_TIME + RECOVERY_RESPONSE_WINDOW_US + 3, result));

    state.Start(3, TEST_TIME);
    RecoveryResponseObservation tx;
    tx.total_inv = 4;
    BOOST_CHECK(!state.ObserveInv(TEST_TIME + 1, tx, result));
    BOOST_CHECK(state.Expire(TEST_TIME + RECOVERY_RESPONSE_WINDOW_US, result));
    BOOST_CHECK_EQUAL(result.outcome, RECOVERY_OUTCOME_EMPTY_TIMEOUT);
    BOOST_CHECK_EQUAL(result.inv_message_count, 1U);
    BOOST_CHECK_EQUAL(result.total_inv, 4U);
    BOOST_CHECK_EQUAL(result.block_inv, 0U);
}

BOOST_AUTO_TEST_CASE(recovery_response_window_supersede_disconnect_and_reset)
{
    RecoveryResponseWindowState state;
    RecoveryResponseResult result;
    state.Start(10, TEST_TIME);
    RecoveryResponseObservation mixed;
    mixed.total_inv = 5; mixed.block_inv = 4; mixed.unknown_blocks = 1;
    mixed.known_active_blocks = 1; mixed.known_nonactive_indexed_blocks = 1;
    mixed.known_orphan_blocks = 1;
    BOOST_CHECK(!state.ObserveInv(TEST_TIME + 1, mixed, result));
    BOOST_CHECK(state.Supersede(TEST_TIME + 2, result));
    BOOST_CHECK_EQUAL(result.outcome, RECOVERY_OUTCOME_SUPERSEDED_BY_NEXT_RECOVERY);
    BOOST_CHECK(!state.Supersede(TEST_TIME + 3, result));
    state.Start(11, TEST_TIME + 10);
    BOOST_CHECK(state.IsActive());
    BOOST_CHECK_EQUAL(state.Disconnect(TEST_TIME + 11, result), true);
    BOOST_CHECK_EQUAL(result.outcome, RECOVERY_OUTCOME_DISCONNECTED);
    BOOST_CHECK(!state.Disconnect(TEST_TIME + 12, result));
    BOOST_CHECK(!state.Expire(TEST_TIME + RECOVERY_RESPONSE_WINDOW_US + 20, result));
}



BOOST_AUTO_TEST_CASE(recovery_response_window_deadline_disconnect_and_formatter)
{
    RecoveryResponseWindowState state;
    RecoveryResponseResult result;
    RecoveryResponseObservation block;
    block.total_inv = block.block_inv = block.unknown_blocks = 1;
    block.first_block_hash = uint256(7);
    block.first_unknown_block_hash = uint256(7);

    state.Start(20, TEST_TIME);
    BOOST_CHECK(state.ObserveInv(TEST_TIME + RECOVERY_RESPONSE_WINDOW_US,
                                block, result));
    BOOST_CHECK(!state.IsActive());
    BOOST_CHECK_EQUAL(result.recovery_id, 20U);
    BOOST_CHECK_EQUAL(result.block_inv, 0U);
    BOOST_CHECK_EQUAL(result.outcome, RECOVERY_OUTCOME_EMPTY_TIMEOUT);
    BOOST_CHECK(!state.ObserveInv(TEST_TIME + RECOVERY_RESPONSE_WINDOW_US + 1,
                                  block, result));
    BOOST_CHECK(!state.Expire(TEST_TIME + RECOVERY_RESPONSE_WINDOW_US + 2,
                              result));

    state.Start(21, TEST_TIME + 10);
    BOOST_CHECK(!state.ObserveInv(TEST_TIME + 11, block, result));
    BOOST_CHECK(state.Disconnect(TEST_TIME + 12, result));
    BOOST_CHECK_EQUAL(result.outcome, RECOVERY_OUTCOME_DISCONNECTED);
    BOOST_CHECK_EQUAL(result.block_inv, 1U);
    BOOST_CHECK_EQUAL(result.unknown_blocks, 1U);
    BOOST_CHECK(!state.Disconnect(TEST_TIME + 13, result));

    const std::string summary = FormatRecoveryResponseSummary(42, result);
    BOOST_CHECK(summary.find("peer_id=42") != std::string::npos);
    BOOST_CHECK(summary.find("outcome=disconnected") != std::string::npos);
    BOOST_CHECK(summary.find("recovery_id=21") != std::string::npos);

    const RecoveryResponseOutcome outcomes[] = {
        RECOVERY_OUTCOME_USEFUL,
        RECOVERY_OUTCOME_KNOWN_ONLY_TIMEOUT,
        RECOVERY_OUTCOME_EMPTY_TIMEOUT,
        RECOVERY_OUTCOME_DISCONNECTED,
        RECOVERY_OUTCOME_SUPERSEDED_BY_NEXT_RECOVERY
    };
    const char* names[] = {"useful", "known_only_timeout", "empty_timeout",
                           "disconnected", "superseded_by_next_recovery"};
    for (unsigned int i = 0; i < sizeof(outcomes) / sizeof(outcomes[0]); ++i)
        BOOST_CHECK_EQUAL(std::string(RecoveryResponseOutcomeName(outcomes[i])),
                          names[i]);
}

BOOST_AUTO_TEST_CASE(block_request_trace_requires_explicit_enable)
{
    const std::string hash = uint256(1).ToString();
    BOOST_CHECK(InitBlockRequestTrace(false, hash));
    BOOST_CHECK(!BlockRequestTraceEnabled());

    BOOST_CHECK(InitBlockRequestTrace(true, hash));
    BOOST_CHECK(BlockRequestTraceEnabled());

    BOOST_CHECK(InitBlockRequestTrace(false, ""));
    BOOST_CHECK(!BlockRequestTraceEnabled());
}

BOOST_AUTO_TEST_CASE(processblock_false_paths_emit_specific_reason)
{
    BOOST_CHECK(InitProcessBlockRejectTrace(false));
    BOOST_CHECK(!ProcessBlockRejectTraceEnabled());
    BOOST_CHECK(InitProcessBlockRejectTrace(true));
    BOOST_CHECK(ProcessBlockRejectTraceEnabled());

    BOOST_CHECK_EQUAL(ProcessBlockRejectReasonName(PBREJECT_DUPLICATE_INDEXED),
                      std::string("DUPLICATE_INDEXED"));
    BOOST_CHECK_EQUAL(ProcessBlockRejectReasonName(PBREJECT_DUPLICATE_ORPHAN),
                      std::string("DUPLICATE_ORPHAN"));
    BOOST_CHECK_EQUAL(ProcessBlockRejectReasonName(PBREJECT_POS_AFTER_DAG),
                      std::string("POS_AFTER_DAG"));
    BOOST_CHECK_EQUAL(ProcessBlockRejectReasonName(PBREJECT_CHECKBLOCK_FALSE),
                      std::string("CHECKBLOCK_FALSE"));
    BOOST_CHECK_EQUAL(ProcessBlockRejectReasonName(PBREJECT_ORPHAN_LIMIT_IBD),
                      std::string("ORPHAN_LIMIT_IBD"));
    BOOST_CHECK_EQUAL(ProcessBlockRejectReasonName(PBREJECT_DUPLICATE_STAKE_ORPHAN),
                      std::string("DUPLICATE_STAKE_ORPHAN"));

    CBlock invalidBlock;
    const char* pszCheckBlockReason = NULL;
    BOOST_CHECK(!invalidBlock.CheckBlock(true, true, true,
                                         &pszCheckBlockReason));
    BOOST_REQUIRE(pszCheckBlockReason != NULL);
    BOOST_CHECK_EQUAL(std::string(pszCheckBlockReason),
                      std::string("CHECKBLOCK_SIZE"));

    BOOST_CHECK(InitProcessBlockRejectTrace(false));
    BOOST_CHECK(!ProcessBlockRejectTraceEnabled());
}

BOOST_AUTO_TEST_CASE(already_asked_for_stale_entries_are_pruned_and_refill)
{
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    const int64_t nNow = TEST_TIME;

    {
        LOCK(cs_mapAlreadyAskedFor);
        for (size_t i = 0; i < MAX_ALREADY_ASKED_FOR_SIZE; ++i)
            mapAlreadyAskedFor[CInv(MSG_BLOCK, uint256(i + 1))] =
                nNow - ALREADY_ASKED_FOR_RETENTION_US - 1;
        BOOST_CHECK_EQUAL(mapAlreadyAskedFor.size(),
                          MAX_ALREADY_ASKED_FOR_SIZE);
    }

    BOOST_CHECK_EQUAL(PruneAlreadyAskedFor(nNow),
                      MAX_ALREADY_ASKED_FOR_SIZE);
    {
        LOCK(cs_mapAlreadyAskedFor);
        BOOST_CHECK(mapAlreadyAskedFor.empty());
    }

    CNode peer(INVALID_SOCKET, TestPeerAddress(30), "already-asked-refill", true);
    PreparePeerForRecovery(peer, PROTOCOL_VERSION, nBestHeight + 1000);
    for (unsigned int i = 0; i < 1000; ++i)
        peer.AskFor(CInv(MSG_BLOCK, uint256(100000 + i)),
                    BLOCKREQ_SOURCE_INV);

    BOOST_CHECK_EQUAL(peer.mapAskFor.size(), 1000U);
    BOOST_CHECK(SendMessages(&peer, true));
    BOOST_CHECK(HasCommand(SentCommands(peer), "getdata"));
    {
        LOCK(cs_mapAlreadyAskedFor);
        BOOST_CHECK(mapAlreadyAskedFor.size() <= MAX_ALREADY_ASKED_FOR_SIZE);
    }
}

BOOST_AUTO_TEST_CASE(already_asked_for_negative_cooldown_lifecycle)
{
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    const CInv rejected(MSG_BLOCK, uint256(600000));
    const int64_t nStart = GetTimeMicros() + 10000000;
    {
        LOCK(cs_mapAlreadyAskedFor);
        mapAlreadyAskedFor[rejected] =
            nStart + ALREADY_ASKED_FOR_NEGATIVE_COOLDOWN_US;
    }

    BOOST_CHECK_EQUAL(PruneAlreadyAskedFor(nStart + 1000000), 0U);
    CNode peer(INVALID_SOCKET, TestPeerAddress(34), "negative-cooldown", true);
    PreparePeerForRecovery(peer, PROTOCOL_VERSION, nBestHeight + 1);
    peer.AskFor(rejected, BLOCKREQ_SOURCE_INV);
    BOOST_CHECK(!peer.mapAskFor.empty());
    BOOST_CHECK(peer.mapAskFor.begin()->first > GetTimeMicros() * 1000000);
    peer.ClearAskFor();
    {
        LOCK(cs_mapAlreadyAskedFor);
        mapAlreadyAskedFor[rejected] =
            nStart + ALREADY_ASKED_FOR_NEGATIVE_COOLDOWN_US;
    }

    BOOST_CHECK_EQUAL(PruneAlreadyAskedFor(
                          nStart + ALREADY_ASKED_FOR_NEGATIVE_COOLDOWN_US + 1), 1U);
    {
        LOCK(cs_mapAlreadyAskedFor);
        BOOST_CHECK_EQUAL(mapAlreadyAskedFor.count(rejected), 0U);
    }
    peer.ClearAskFor();
    peer.AskFor(rejected, BLOCKREQ_SOURCE_INV);
    BOOST_CHECK(!peer.mapAskFor.empty());
}

BOOST_AUTO_TEST_CASE(already_asked_for_future_scheduled_entry_is_not_pruned)
{
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    const CInv scheduled(MSG_BLOCK, uint256(600001));
    const int64_t nScheduled = GetTimeMicros() + 10000000;
    {
        LOCK(cs_mapAlreadyAskedFor);
        mapAlreadyAskedFor[scheduled] = nScheduled;
    }

    BOOST_CHECK_EQUAL(PruneAlreadyAskedFor(nScheduled - 1000000), 0U);
    {
        LOCK(cs_mapAlreadyAskedFor);
        BOOST_CHECK_EQUAL(mapAlreadyAskedFor.count(scheduled), 1U);
    }
    BOOST_CHECK_EQUAL(PruneAlreadyAskedFor(nScheduled + 1), 1U);
    {
        LOCK(cs_mapAlreadyAskedFor);
        BOOST_CHECK_EQUAL(mapAlreadyAskedFor.count(scheduled), 0U);
    }
}

BOOST_AUTO_TEST_CASE(already_asked_for_lifecycle_is_cross_peer_safe)
{
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CNode owner(INVALID_SOCKET, TestPeerAddress(32), "ownership-owner", true);
    CNode other(INVALID_SOCKET, TestPeerAddress(33), "ownership-other", true);
    PreparePeerForRecovery(owner, PROTOCOL_VERSION, nBestHeight + 1);
    PreparePeerForRecovery(other, PROTOCOL_VERSION, nBestHeight + 1);

    const CInv inv(MSG_BLOCK, uint256(400000));
    owner.AskFor(inv, BLOCKREQ_SOURCE_INV);
    BOOST_CHECK(TryAssignBlockRequestOwner(inv.hash, owner.GetId(), BLOCKREQ_SOURCE_INV));
    {
        LOCK(cs_vNodes);
        vNodes.push_back(&owner);
    }
    BOOST_CHECK(!EraseAlreadyAskedForIfUnowned(inv));
    {
        LOCK(cs_mapAlreadyAskedFor);
        BOOST_CHECK_EQUAL(mapAlreadyAskedFor.count(inv), 1U);
    }
    owner.ClearAskFor();
    BOOST_CHECK(EraseAlreadyAskedForIfUnowned(inv));
    {
        LOCK(cs_mapAlreadyAskedFor);
        BOOST_CHECK_EQUAL(mapAlreadyAskedFor.count(inv), 0U);
    }
    {
        LOCK(cs_vNodes);
        vNodes.erase(std::remove(vNodes.begin(), vNodes.end(), &owner),
                      vNodes.end());
    }

    for (unsigned int i = 0; i < MAX_ALREADY_ASKED_FOR_SIZE + 100; ++i)
    {
        const CInv sequential(MSG_BLOCK, uint256(500000 + i));
        {
            LOCK(cs_mapAlreadyAskedFor);
            mapAlreadyAskedFor[sequential] = GetTimeMicros();
        }
        BOOST_CHECK(EraseAlreadyAskedForIfUnowned(sequential));
    }
    LOCK(cs_mapAlreadyAskedFor);
    BOOST_CHECK(mapAlreadyAskedFor.size() < MAX_ALREADY_ASKED_FOR_SIZE);
}

BOOST_AUTO_TEST_CASE(already_asked_for_recent_bound_remains_anti_spam)
{
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    {
        LOCK(cs_mapAlreadyAskedFor);
        const int64_t nRecent = GetTimeMicros();
        for (size_t i = 0; i < MAX_ALREADY_ASKED_FOR_SIZE; ++i)
            mapAlreadyAskedFor[CInv(MSG_BLOCK, uint256(200000 + i))] = nRecent;
    }

    CNode peer(INVALID_SOCKET, TestPeerAddress(31), "already-asked-bound", true);
    PreparePeerForRecovery(peer, PROTOCOL_VERSION, nBestHeight + 1);
    peer.AskFor(CInv(MSG_BLOCK, uint256(300000)), BLOCKREQ_SOURCE_INV);
    BOOST_CHECK_EQUAL(peer.mapAskFor.size(), 0U);
    {
        LOCK(cs_mapAlreadyAskedFor);
        BOOST_CHECK_EQUAL(mapAlreadyAskedFor.size(),
                          MAX_ALREADY_ASKED_FOR_SIZE);
    }
}



static void AddQueuedBlockRequests(CNode& peer, int nCount, int nBase)
{
    const int64_t nBaseTime = GetTimeMicros();
    for (int i = 0; i < nCount; ++i)
        peer.AddAskForEntry(nBaseTime + i, CInv(MSG_BLOCK, uint256(nBase + i)));
}

static void AddSentBlockRequests(CNode& peer, int nCount, int nBase)
{
    for (int i = 0; i < nCount; ++i)
        peer.MarkBlockInFlight(uint256(nBase + i));
}

static void ClearSentBlockRequests(CNode& peer, int nCount, int nBase)
{
    for (int i = 0; i < nCount; ++i)
        peer.ClearBlockInFlight(uint256(nBase + i));
}

BOOST_AUTO_TEST_CASE(pipeline_pressure_blocks_before_physical_limit)
{
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CScopedOrphanCountByNode isolatedOrphanCounts;
    CNode peer(INVALID_SOCKET, TestPeerAddress(45), "pipeline-pressure", true);
    CScopedInitialBlockDownloadState ibdState(&peer);
    const CInv inv(MSG_BLOCK, uint256(800001));
    int nOrphanCountPeer = 0;
    int nQueuedBlockRequests = 0;
    int nSentBlockRequests = 0;
    int nProjectedPressure = 0;

    AddQueuedBlockRequests(peer, 300, 810000);
    AddSentBlockRequests(peer, 100, 820000);
    {
        LOCK(cs_main);
        mapOrphanCountByNode[peer.GetId()] = 100;
        BOOST_CHECK(ShouldSkipBlockInvForOrphanPressure(
            &peer, inv, false, &nOrphanCountPeer,
            &nQueuedBlockRequests, &nSentBlockRequests,
            &nProjectedPressure));
    }
    BOOST_CHECK_EQUAL(nOrphanCountPeer, 100);
    BOOST_CHECK_EQUAL(nQueuedBlockRequests, 300);
    BOOST_CHECK_EQUAL(nSentBlockRequests, 100);
    BOOST_CHECK_EQUAL(nProjectedPressure, MAX_PROJECTED_ORPHAN_PRESSURE);
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(peer, inv.hash), 0U);
    BOOST_CHECK(!GetBlockRequestOwner(inv.hash, NULL, NULL));
    {
        LOCK(cs_mapAlreadyAskedFor);
        BOOST_CHECK_EQUAL(mapAlreadyAskedFor.count(inv), 0U);
    }
    peer.ClearAskFor();
    ClearSentBlockRequests(peer, 100, 820000);
}

BOOST_AUTO_TEST_CASE(low_physical_count_large_pipeline_is_blocked)
{
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CScopedOrphanCountByNode isolatedOrphanCounts;
    CNode peer(INVALID_SOCKET, TestPeerAddress(46), "low-physical-pipeline", true);
    CScopedInitialBlockDownloadState ibdState(&peer);
    const CInv inv(MSG_BLOCK, uint256(800002));
    int nProjectedPressure = 0;

    AddQueuedBlockRequests(peer, MAX_PROJECTED_ORPHAN_PRESSURE - 10, 830000);
    {
        LOCK(cs_main);
        mapOrphanCountByNode[peer.GetId()] = 10;
        BOOST_CHECK(ShouldSkipBlockInvForOrphanPressure(
            &peer, inv, false, NULL, NULL, NULL, &nProjectedPressure));
    }
    BOOST_CHECK_EQUAL(nProjectedPressure, MAX_PROJECTED_ORPHAN_PRESSURE);
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(peer, inv.hash), 0U);
    peer.ClearAskFor();
}

BOOST_AUTO_TEST_CASE(pipeline_drain_resumes_inv)
{
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CScopedOrphanCountByNode isolatedOrphanCounts;
    CNode peer(INVALID_SOCKET, TestPeerAddress(47), "pipeline-drain", true);
    CScopedInitialBlockDownloadState ibdState(&peer);
    const CInv blocked(MSG_BLOCK, uint256(800003));
    const CInv admitted(MSG_BLOCK, uint256(800004));
    int nProjectedPressure = 0;

    AddQueuedBlockRequests(peer, 300, 840000);
    AddSentBlockRequests(peer, 100, 850000);
    {
        LOCK(cs_main);
        mapOrphanCountByNode[peer.GetId()] = 100;
        BOOST_CHECK(ShouldSkipBlockInvForOrphanPressure(
            &peer, blocked, false, NULL, NULL, NULL, &nProjectedPressure));
    }
    BOOST_CHECK_EQUAL(nProjectedPressure, MAX_PROJECTED_ORPHAN_PRESSURE);
    peer.ClearAskFor();
    ClearSentBlockRequests(peer, 100, 850000);
    AddQueuedBlockRequests(peer, 100, 860000);
    {
        LOCK(cs_main);
        mapOrphanCountByNode[peer.GetId()] = 100;
        BOOST_CHECK(!ShouldSkipBlockInvForOrphanPressure(
            &peer, admitted, false, NULL, NULL, NULL, &nProjectedPressure));
    }
    BOOST_CHECK_EQUAL(nProjectedPressure, 200);
    peer.AskFor(admitted, BLOCKREQ_SOURCE_INV);
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(peer, admitted.hash), 1U);
    peer.ClearAskFor();
}

BOOST_AUTO_TEST_CASE(orphan_recovery_exempt_under_projected_pressure)
{
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CScopedOrphanCountByNode isolatedOrphanCounts;
    CNode peer(INVALID_SOCKET, TestPeerAddress(48), "projected-pressure-parent", true);
    CScopedInitialBlockDownloadState ibdState(&peer);
    const CInv parent(MSG_BLOCK, uint256(800005));

    AddQueuedBlockRequests(peer, MAX_PROJECTED_ORPHAN_PRESSURE, 870000);
    {
        LOCK(cs_main);
        mapOrphanCountByNode[peer.GetId()] = MAX_PROJECTED_ORPHAN_PRESSURE;
    }
    peer.AskFor(parent, BLOCKREQ_SOURCE_ORPHAN);
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(peer, parent.hash), 1U);
    BOOST_CHECK(TryAssignBlockRequestOwner(parent.hash, peer.GetId(), BLOCKREQ_SOURCE_ORPHAN));
    peer.MarkBlockInFlight(parent.hash);
    NodeId ownerPeer = -1;
    BlockRequestOwnerState ownerState = BLOCK_REQUEST_OWNER_IN_FLIGHT;
    BOOST_CHECK(GetBlockRequestOwner(parent.hash, &ownerPeer, &ownerState));
    BOOST_CHECK_EQUAL(ownerPeer, peer.GetId());
    BOOST_CHECK_EQUAL(ownerState, BLOCK_REQUEST_OWNER_IN_FLIGHT);
    peer.ClearAskFor();
    peer.ClearBlockInFlight(parent.hash);
}

BOOST_AUTO_TEST_CASE(no_double_count_between_queued_and_sent)
{
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CScopedOrphanCountByNode isolatedOrphanCounts;
    CNode peer(INVALID_SOCKET, TestPeerAddress(49), "pipeline-transition", true);
    CScopedInitialBlockDownloadState ibdState(&peer);
    const CInv candidate(MSG_BLOCK, uint256(800006));
    const uint256 hashRequest(880000);
    int nProjectedQueued = 0;
    int nProjectedSent = 0;

    peer.AddAskForEntry(GetTimeMicros(), CInv(MSG_BLOCK, hashRequest));
    {
        LOCK(cs_main);
        mapOrphanCountByNode[peer.GetId()] = 100;
        BOOST_CHECK(!ShouldSkipBlockInvForOrphanPressure(
            &peer, candidate, false, NULL, NULL, NULL, &nProjectedQueued));
    }
    peer.EraseAskForEntry(peer.mapAskFor.begin(), false);
    peer.MarkBlockInFlight(hashRequest);
    {
        LOCK(cs_main);
        BOOST_CHECK(!ShouldSkipBlockInvForOrphanPressure(
            &peer, candidate, false, NULL, NULL, NULL, &nProjectedSent));
    }
    BOOST_CHECK_EQUAL(nProjectedQueued, 101);
    BOOST_CHECK_EQUAL(nProjectedSent, 101);
    peer.ClearBlockInFlight(hashRequest);
}

BOOST_AUTO_TEST_CASE(existing_pipeline_not_cancelled)
{
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CScopedOrphanCountByNode isolatedOrphanCounts;
    CNode peer(INVALID_SOCKET, TestPeerAddress(50), "pipeline-existing", true);
    CScopedInitialBlockDownloadState ibdState(&peer);
    const CInv inv(MSG_BLOCK, uint256(800007));
    const uint256 hashQueued(890000);
    const uint256 hashSent(890001);

    peer.AddAskForEntry(GetTimeMicros(), CInv(MSG_BLOCK, hashQueued));
    peer.MarkBlockInFlight(hashSent);
    {
        LOCK(cs_main);
        mapOrphanCountByNode[peer.GetId()] = MAX_PROJECTED_ORPHAN_PRESSURE;
        BOOST_CHECK(ShouldSkipBlockInvForOrphanPressure(&peer, inv, false));
    }
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(peer, hashQueued), 1U);
    BOOST_CHECK_EQUAL(peer.setBlocksInFlight.count(hashSent), 1U);
    peer.ClearAskFor();
    peer.ClearBlockInFlight(hashSent);
}

BOOST_AUTO_TEST_CASE(per_peer_pipeline_pressure)
{
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CScopedOrphanCountByNode isolatedOrphanCounts;
    CNode peerA(INVALID_SOCKET, TestPeerAddress(51), "pipeline-peer-a", true);
    CNode peerB(INVALID_SOCKET, TestPeerAddress(52), "pipeline-peer-b", true);
    CScopedInitialBlockDownloadState ibdState(&peerB);
    const CInv invA(MSG_BLOCK, uint256(800008));
    const CInv invB(MSG_BLOCK, uint256(800009));

    AddQueuedBlockRequests(peerA, MAX_PROJECTED_ORPHAN_PRESSURE, 900000);
    AddQueuedBlockRequests(peerB, MAX_PROJECTED_ORPHAN_PRESSURE - 1, 901000);
    {
        LOCK(cs_main);
        BOOST_CHECK(ShouldSkipBlockInvForOrphanPressure(&peerA, invA, false));
        BOOST_CHECK(!ShouldSkipBlockInvForOrphanPressure(&peerB, invB, false));
    }
    peerB.AskFor(invB, BLOCKREQ_SOURCE_INV);
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(peerA, invA.hash), 0U);
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(peerB, invB.hash), 1U);
    peerA.ClearAskFor();
    peerB.ClearAskFor();
}

BOOST_AUTO_TEST_CASE(cross_peer_parent_ownership_preserved)
{
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CScopedOrphanCountByNode isolatedOrphanCounts;
    CNode owner(INVALID_SOCKET, TestPeerAddress(53), "parent-owner", true);
    CNode other(INVALID_SOCKET, TestPeerAddress(54), "parent-other", true);
    CScopedInitialBlockDownloadState ibdState(&owner);
    const CInv parent(MSG_BLOCK, uint256(800010));

    owner.AskFor(parent, BLOCKREQ_SOURCE_ORPHAN);
    BOOST_CHECK(TryAssignBlockRequestOwner(parent.hash, owner.GetId(), BLOCKREQ_SOURCE_ORPHAN));
    owner.MarkBlockInFlight(parent.hash);
    other.AskFor(parent, BLOCKREQ_SOURCE_ORPHAN);
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(owner, parent.hash), 1U);
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(other, parent.hash), 0U);
    NodeId ownerPeer = -1;
    BlockRequestOwnerState ownerState = BLOCK_REQUEST_OWNER_IN_FLIGHT;
    BOOST_CHECK(GetBlockRequestOwner(parent.hash, &ownerPeer, &ownerState));
    BOOST_CHECK_EQUAL(ownerPeer, owner.GetId());
    BOOST_CHECK_EQUAL(ownerState, BLOCK_REQUEST_OWNER_IN_FLIGHT);
    owner.ClearAskFor();
}

BOOST_AUTO_TEST_CASE(non_ibd_unaffected)
{
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CScopedOrphanCountByNode isolatedOrphanCounts;
    const bool fRegTestSaved = fRegTest;
    fRegTest = true;
    CNode peer(INVALID_SOCKET, TestPeerAddress(55), "non-ibd-pressure", true);
    const CInv inv(MSG_BLOCK, uint256(800011));

    AddQueuedBlockRequests(peer, MAX_PROJECTED_ORPHAN_PRESSURE, 902000);
    {
        LOCK(cs_main);
        mapOrphanCountByNode[peer.GetId()] = MAX_PROJECTED_ORPHAN_PRESSURE;
        BOOST_CHECK(!ShouldSkipBlockInvForOrphanPressure(&peer, inv, false));
    }
    peer.AskFor(inv, BLOCKREQ_SOURCE_INV);
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(peer, inv.hash), 1U);
    peer.ClearAskFor();
    fRegTest = fRegTestSaved;
}

BOOST_AUTO_TEST_CASE(stalled_recovery_is_inactive_before_initial_send)
{
    CNode peer(INVALID_SOCKET, TestPeerAddress(39), "initial-not-started", true);
    PreparePeerForRecovery(peer, PROTOCOL_VERSION, nBestHeight + 10);
    std::vector<CNode*> peers(1, &peer);
    CStalledSyncRecoveryState state;
    BOOST_CHECK(MaybeQueueStalledSyncRecovery(
                    peers, pindexBest, nBestHeight, TEST_TIME + 100,
                    15, 15, state) == NULL);
    BOOST_CHECK(!state.SyncRequestSent());
    BOOST_CHECK_EQUAL(state.LastProgressTime(), 0);
}

BOOST_AUTO_TEST_CASE(initial_sync_lifecycle_starts_modern_peer)
{
    const bool fSPVModeSaved = fSPVMode;
    fSPVMode = false;
    CNode peer(INVALID_SOCKET, TestPeerAddress(40), "initial-modern", true);
    PreparePeerForRecovery(peer, PROTOCOL_VERSION, nBestHeight + 10);
    std::vector<CNode*> peers(1, &peer);
    ResetSyncPeerForTesting();
    StartSyncForTesting(peers);
    BOOST_CHECK(peer.fStartSync);
    BOOST_CHECK(!peer.fInitialSyncRequestSent);
    BOOST_CHECK(SendMessages(&peer, true));
    BOOST_CHECK(SendMessages(&peer, true));
    const std::vector<std::string> commands = SentCommands(peer);
    BOOST_CHECK_EQUAL(std::count(commands.begin(), commands.end(), "getblocks"), 1);
    BOOST_CHECK(!peer.fInitialSyncRequestPending);
    BOOST_CHECK(peer.fInitialSyncRequestSent);
    fSPVMode = fSPVModeSaved;
}

BOOST_AUTO_TEST_CASE(initial_sync_lifecycle_starts_legacy_peer)
{
    const bool fSPVModeSaved = fSPVMode;
    fSPVMode = false;
    CNode peer(INVALID_SOCKET, TestPeerAddress(41), "initial-legacy", true);
    PreparePeerForRecovery(peer, MIN_PEER_PROTO_VERSION, nBestHeight + 10);
    std::vector<CNode*> peers(1, &peer);
    ResetSyncPeerForTesting();
    StartSyncForTesting(peers);
    BOOST_CHECK(SendMessages(&peer, true));
    const std::vector<std::string> commands = SentCommands(peer);
    BOOST_CHECK_EQUAL(std::count(commands.begin(), commands.end(), "getblocks"), 1);
    BOOST_CHECK(peer.fInitialSyncRequestSent);
    fSPVMode = fSPVModeSaved;
}

BOOST_AUTO_TEST_CASE(ibd_efficiency_counters_origin_mutual_exclusion)
{
    IBDEfficiencyCounters c;

    c.RecordBlock(100, true, true, false, false, true, false, false, false, false);
    c.RecordBlock(200, false, true, false, false, true, false, false, false, false);
    c.RecordBlock(300, true, true, false, false, true, false, false, false, false);

    BOOST_CHECK_EQUAL(c.received_requested.load(), 2U);
    BOOST_CHECK_EQUAL(c.received_unsolicited.load(), 1U);
    BOOST_CHECK_EQUAL(c.bytes_requested.load(), 400U);
    BOOST_CHECK_EQUAL(c.bytes_unsolicited.load(), 200U);

    uint64_t total_count = c.received_requested.load() + c.received_unsolicited.load();
    BOOST_CHECK_EQUAL(total_count, 3U);
}

BOOST_AUTO_TEST_CASE(ibd_efficiency_counters_novelty_mutual_exclusion)
{
    IBDEfficiencyCounters c;

    c.RecordBlock(100, true, true, false, false, true, false, false, false, false);
    c.RecordBlock(100, true, false, true, false, true, false, false, false, false);
    c.RecordBlock(100, true, false, false, true, true, false, false, false, false);

    BOOST_CHECK_EQUAL(c.received_unique.load(), 1U);
    BOOST_CHECK_EQUAL(c.received_duplicate_indexed.load(), 1U);
    BOOST_CHECK_EQUAL(c.received_duplicate_orphan.load(), 1U);
    BOOST_CHECK_EQUAL(c.bytes_unique.load(), 100U);
    BOOST_CHECK_EQUAL(c.bytes_duplicate.load(), 200U);
}

BOOST_AUTO_TEST_CASE(ibd_efficiency_counters_outcome_mutual_exclusion)
{
    IBDEfficiencyCounters c;

    c.RecordBlock(100, true, true, false, false, true, false, false, false, false);
    c.RecordBlock(200, true, true, false, false, false, true, false, false, false);
    c.RecordBlock(300, true, true, false, false, false, false, true, false, false);
    c.RecordBlock(400, true, true, false, false, false, false, false, true, false);

    BOOST_CHECK_EQUAL(c.received_accepted_active.load(), 1U);
    BOOST_CHECK_EQUAL(c.received_accepted_side.load(), 1U);
    BOOST_CHECK_EQUAL(c.received_orphan_new.load(), 1U);
    BOOST_CHECK_EQUAL(c.received_rejected.load(), 1U);
    BOOST_CHECK_EQUAL(c.bytes_accepted_active.load(), 100U);
    BOOST_CHECK_EQUAL(c.bytes_accepted_side.load(), 200U);
    BOOST_CHECK_EQUAL(c.bytes_orphan_new.load(), 300U);
    BOOST_CHECK_EQUAL(c.bytes_rejected.load(), 400U);

    uint64_t total_outcome = c.received_accepted_active.load() +
                             c.received_accepted_side.load() +
                             c.received_orphan_new.load() +
                             c.received_rejected.load();
    BOOST_CHECK_EQUAL(total_outcome, 4U);
}

BOOST_AUTO_TEST_CASE(ibd_efficiency_counters_retry_is_subset_of_rejected)
{
    IBDEfficiencyCounters c;

    c.RecordBlock(100, true, true, false, false, false, false, false, true, true);
    c.RecordBlock(200, true, true, false, false, false, false, false, true, false);

    BOOST_CHECK_EQUAL(c.received_rejected.load(), 2U);
    BOOST_CHECK_EQUAL(c.received_retry_recorded.load(), 1U);
}

BOOST_AUTO_TEST_CASE(ibd_efficiency_counters_reset_delta)
{
    IBDEfficiencyCounters c;

    c.RecordBlock(100, true, true, false, false, true, false, false, false, false);
    c.RecordBlock(200, false, true, false, false, true, false, false, false, false);

    BOOST_CHECK_EQUAL(c.received_requested.load(), 1U);
    BOOST_CHECK_EQUAL(c.received_unsolicited.load(), 1U);

    c.ResetDelta();

    BOOST_CHECK_EQUAL(c.received_requested.load(), 0U);
    BOOST_CHECK_EQUAL(c.received_unsolicited.load(), 0U);
    BOOST_CHECK_EQUAL(c.received_unique.load(), 0U);
    BOOST_CHECK_EQUAL(c.received_accepted_active.load(), 0U);
    BOOST_CHECK_EQUAL(c.bytes_requested.load(), 0U);
    BOOST_CHECK_EQUAL(c.bytes_unsolicited.load(), 0U);
}

BOOST_AUTO_TEST_CASE(ibd_efficiency_counters_zero_overhead_when_disabled)
{
    InitIBDEfficiencyTrace(false);
    BOOST_CHECK(!IBDEfficiencyTraceEnabled());
    IBDEfficiencyRecordBlock(1000, true, true, false, false, true, false, false, false, false);
    IBDEfficiencyMaybeSummary(GetTimeMicros());
    IBDEfficiencyShutdownSummary();
}

BOOST_AUTO_TEST_CASE(ibd_efficiency_init_and_enable)
{
    BOOST_CHECK(InitIBDEfficiencyTrace(true));
    BOOST_CHECK(IBDEfficiencyTraceEnabled());
    InitIBDEfficiencyTrace(false);
    BOOST_CHECK(!IBDEfficiencyTraceEnabled());
}

BOOST_AUTO_TEST_CASE(ibd_efficiency_counters_print_summary_no_crash)
{
    IBDEfficiencyCounters c;
    c.RecordBlock(500, true, true, false, false, true, false, false, false, false);
    c.RecordBlock(300, false, true, false, false, false, false, true, false, false);
    c.PrintSummary("TEST", 100, 105, 30);
}

BOOST_AUTO_TEST_CASE(ibd_efficiency_periodic_summary)
{
    InitIBDEfficiencyTrace(true);
    IBDEfficiencyRecordBlock(123, true, true, false, false, true, false, false, false, false);
    IBDEfficiencyMaybeSummary(GetTimeMicros() + 60000000LL);
    InitIBDEfficiencyTrace(false);
}

BOOST_AUTO_TEST_CASE(inflight_limit_preserves_askfor_order)
{
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    const uint256 hashP(6001);
    const uint256 hashC(6002);
    const uint256 hashD(6003);

    CNode peer(INVALID_SOCKET, TestPeerAddress(60), "inflight-order", true);
    PreparePeerForRecovery(peer, PROTOCOL_VERSION, nBestHeight + 100);

    int64_t nBaseKey = (GetTime() - 10) * 1000000;
    peer.AddAskForEntry(nBaseKey, CInv(MSG_BLOCK, hashP));
    peer.AddAskForEntry(nBaseKey + 1, CInv(MSG_BLOCK, hashC));
    peer.AddAskForEntry(nBaseKey + 2, CInv(MSG_BLOCK, hashD));
    BOOST_CHECK_EQUAL(peer.mapAskFor.size(), 3U);

    for (size_t i = 0; i < 128; ++i)
        peer.MarkBlockInFlight(uint256(7000 + i));
    BOOST_CHECK_EQUAL(peer.setBlocksInFlight.size(), 128U);

    std::multimap<int64_t, CInv>::const_iterator it = peer.mapAskFor.begin();
    BOOST_CHECK(it->second.hash == hashP); ++it;
    BOOST_CHECK(it->second.hash == hashC); ++it;
    BOOST_CHECK(it->second.hash == hashD);
    BOOST_CHECK_EQUAL(peer.mapAskFor.begin()->first, nBaseKey);

    BOOST_CHECK(SendMessages(&peer, true));
    std::vector<std::string> commands = SentCommands(peer);
    BOOST_CHECK(!HasCommand(commands, "getdata"));
    BOOST_CHECK_EQUAL(peer.mapAskFor.size(), 3U);
    it = peer.mapAskFor.begin();
    BOOST_CHECK(it->second.hash == hashP); ++it;
    BOOST_CHECK(it->second.hash == hashC); ++it;
    BOOST_CHECK(it->second.hash == hashD);
    BOOST_CHECK_EQUAL(peer.mapAskFor.begin()->first, nBaseKey);
    BOOST_CHECK(!GetBlockRequestOwner(hashP, NULL, NULL));

    peer.ClearBlockInFlight(uint256(7000));
    BOOST_CHECK_EQUAL(peer.setBlocksInFlight.size(), 127U);

    BOOST_CHECK(SendMessages(&peer, true));
    BOOST_CHECK_EQUAL(peer.mapAskFor.size(), 2U);
    it = peer.mapAskFor.begin();
    BOOST_CHECK(it->second.hash == hashC); ++it;
    BOOST_CHECK(it->second.hash == hashD);
    NodeId ownerPeer = -1;
    BlockRequestOwnerState ownerState = BLOCK_REQUEST_OWNER_QUEUED;
    BOOST_CHECK(GetBlockRequestOwner(hashP, &ownerPeer, &ownerState));
    BOOST_CHECK_EQUAL(ownerPeer, peer.GetId());
    BOOST_CHECK_EQUAL(ownerState, BLOCK_REQUEST_OWNER_IN_FLIGHT);
    BOOST_CHECK(!GetBlockRequestOwner(hashC, NULL, NULL));
    peer.ClearAskFor();
    peer.ClearBlockInFlight(hashP);
}


static void FillPeerActiveWindow(CNode& peer, int nBase)
{
    for (int i = 0; i < MAX_DEFERRED_INV_ACTIVE_PER_PEER; ++i)
        peer.AskFor(CInv(MSG_BLOCK, uint256(nBase + i)), BLOCKREQ_SOURCE_INV);
}

BOOST_AUTO_TEST_CASE(large_inv_does_not_create_unbounded_active_queue)
{
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CScopedOrphanCountByNode isolatedOrphanCounts;
    CNode peer(INVALID_SOCKET, TestPeerAddress(61), "deferred-active-bound", true);
    CScopedInitialBlockDownloadState ibdState(&peer);

    FillPeerActiveWindow(peer, 910000);
    BOOST_CHECK_EQUAL(peer.setAskForBlocks.size(),
                      (size_t)MAX_DEFERRED_INV_ACTIVE_PER_PEER);
    {
        LOCK(cs_main);
        BOOST_CHECK_EQUAL(GetDeferredBlockRequestBudget(&peer), 0);
        BOOST_CHECK(peer.DeferBlockInv(uint256(920000)));
        BOOST_CHECK_EQUAL(RefillDeferredBlockRequests(&peer), 0U);
    }
    BOOST_CHECK_EQUAL(peer.setAskForBlocks.size(),
                      (size_t)MAX_DEFERRED_INV_ACTIVE_PER_PEER);
    BOOST_CHECK_EQUAL(peer.deferredBlockInv.size(), 1U);
    peer.ClearAskFor();
}


BOOST_AUTO_TEST_CASE(global_active_window_sum_is_bounded)
{
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CScopedOrphanCountByNode isolatedOrphanCounts;
    CNode peerA(INVALID_SOCKET, TestPeerAddress(79), "global-window-a", true);
    CNode peerB(INVALID_SOCKET, TestPeerAddress(80), "global-window-b", true);
    CNode peerC(INVALID_SOCKET, TestPeerAddress(81), "global-window-c", true);
    CNode peerD(INVALID_SOCKET, TestPeerAddress(82), "global-window-d", true);
    CScopedInitialBlockDownloadState ibdState(&peerA);

    FillPeerActiveWindow(peerA, 970000);
    FillPeerActiveWindow(peerB, 971000);
    FillPeerActiveWindow(peerC, 972000);
    FillPeerActiveWindow(peerD, 973000);

    {
        LOCK(cs_vNodes);
        vNodes.push_back(&peerA);
        vNodes.push_back(&peerB);
        vNodes.push_back(&peerC);
        vNodes.push_back(&peerD);
    }
    {
        LOCK(cs_main);
        BOOST_CHECK_EQUAL(GetDeferredBlockRequestBudget(&peerA), 0);
    }

    size_t nGlobalActive = peerA.setAskForBlocks.size() +
        peerA.setBlocksInFlight.size() +
        peerB.setAskForBlocks.size() +
        peerB.setBlocksInFlight.size() +
        peerC.setAskForBlocks.size() +
        peerC.setBlocksInFlight.size() +
        peerD.setAskForBlocks.size() +
        peerD.setBlocksInFlight.size();
    BOOST_CHECK(nGlobalActive <=
                (size_t)MAX_DEFERRED_INV_ACTIVE_GLOBAL);

    {
        LOCK(cs_vNodes);
        vNodes.erase(std::remove(vNodes.begin(), vNodes.end(), &peerA),
                     vNodes.end());
        vNodes.erase(std::remove(vNodes.begin(), vNodes.end(), &peerB),
                     vNodes.end());
        vNodes.erase(std::remove(vNodes.begin(), vNodes.end(), &peerC),
                     vNodes.end());
        vNodes.erase(std::remove(vNodes.begin(), vNodes.end(), &peerD),
                     vNodes.end());
    }
    peerA.ClearAskFor();
    peerB.ClearAskFor();
    peerC.ClearAskFor();
    peerD.ClearAskFor();
}

BOOST_AUTO_TEST_CASE(excess_inv_is_deferred_not_lost)
{
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CScopedOrphanCountByNode isolatedOrphanCounts;
    CNode peer(INVALID_SOCKET, TestPeerAddress(62), "deferred-not-lost", true);
    CScopedInitialBlockDownloadState ibdState(&peer);
    const uint256 deferred(920001);

    FillPeerActiveWindow(peer, 921000);
    {
        LOCK(cs_main);
        BOOST_CHECK(peer.DeferBlockInv(deferred));
        BOOST_CHECK(peer.IsBlockInvDeferred(deferred));
    }
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(peer, deferred), 0U);
    BOOST_CHECK(!GetBlockRequestOwner(deferred, NULL, NULL));
    peer.ClearAskFor();
}

BOOST_AUTO_TEST_CASE(deferred_inv_is_eventually_admitted_after_window_drain)
{
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CScopedOrphanCountByNode isolatedOrphanCounts;
    CNode peer(INVALID_SOCKET, TestPeerAddress(63), "deferred-drain", true);
    CScopedInitialBlockDownloadState ibdState(&peer);
    const uint256 deferred(920002);

    FillPeerActiveWindow(peer, 922000);
    BOOST_REQUIRE(peer.DeferBlockInv(deferred));
    peer.EraseAskForEntry(peer.mapAskFor.begin());
    {
        LOCK(cs_main);
        BOOST_CHECK_EQUAL(RefillDeferredBlockRequests(&peer), 1U);
    }
    BOOST_CHECK_EQUAL(peer.deferredBlockInv.size(), 0U);
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(peer, deferred), 1U);
    BOOST_CHECK(!GetBlockRequestOwner(deferred, NULL, NULL));
    peer.ClearAskFor();
}

BOOST_AUTO_TEST_CASE(duplicate_inv_does_not_duplicate_deferred_entry)
{
    CNode peer(INVALID_SOCKET, TestPeerAddress(64), "deferred-duplicate", true);
    const uint256 hash(920003);
    BOOST_CHECK(peer.DeferBlockInv(hash));
    BOOST_CHECK(!peer.DeferBlockInv(hash));
    BOOST_CHECK_EQUAL(peer.deferredBlockInv.size(), 1U);
    BOOST_CHECK_EQUAL(peer.deferredBlockInvIndex.size(), 1U);
}

BOOST_AUTO_TEST_CASE(deferred_hash_has_no_owner_before_admission)
{
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CNode peer(INVALID_SOCKET, TestPeerAddress(65), "deferred-no-owner", true);
    const uint256 hash(920004);
    BOOST_REQUIRE(peer.DeferBlockInv(hash));
    BOOST_CHECK(!GetBlockRequestOwner(hash, NULL, NULL));
    LOCK(cs_mapAlreadyAskedFor);
    BOOST_CHECK_EQUAL(mapAlreadyAskedFor.count(CInv(MSG_BLOCK, hash)), 0U);
}

BOOST_AUTO_TEST_CASE(ownership_is_claimed_on_getdata_send_only)
{
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CScopedOrphanCountByNode isolatedOrphanCounts;
    CNode peer(INVALID_SOCKET, TestPeerAddress(66), "deferred-owner-admit", true);
    PreparePeerForRecovery(peer, PROTOCOL_VERSION, nBestHeight + 100);
    CScopedInitialBlockDownloadState ibdState(&peer);
    const uint256 hash(920005);

    BOOST_REQUIRE(peer.DeferBlockInv(hash));
    BOOST_CHECK(!GetBlockRequestOwner(hash, NULL, NULL));
    {
        LOCK(cs_main);
        BOOST_CHECK_EQUAL(RefillDeferredBlockRequests(&peer), 1U);
    }
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(peer, hash), 1U);
    BOOST_CHECK(!GetBlockRequestOwner(hash, NULL, NULL));

    BOOST_CHECK(SendMessages(&peer, true));
    BOOST_CHECK(HasCommand(SentCommands(peer), "getdata"));
    NodeId ownerPeer = -1;
    BlockRequestOwnerState ownerState = BLOCK_REQUEST_OWNER_QUEUED;
    BOOST_CHECK(GetBlockRequestOwner(hash, &ownerPeer, &ownerState));
    BOOST_CHECK_EQUAL(ownerPeer, peer.GetId());
    BOOST_CHECK_EQUAL(ownerState, BLOCK_REQUEST_OWNER_IN_FLIGHT);
    peer.ClearAskFor();
    peer.ClearBlockInFlight(hash);
}

BOOST_AUTO_TEST_CASE(already_known_hash_is_removed_during_refill)
{
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CScopedOrphanCountByNode isolatedOrphanCounts;
    BOOST_REQUIRE(pindexBest != NULL);
    CNode peer(INVALID_SOCKET, TestPeerAddress(67), "deferred-known", true);
    CScopedInitialBlockDownloadState ibdState(&peer);
    const uint256 known = pindexBest->GetBlockHash();

    BOOST_REQUIRE(peer.DeferBlockInv(known));
    {
        LOCK(cs_main);
        BOOST_CHECK_EQUAL(RefillDeferredBlockRequests(&peer), 0U);
    }
    BOOST_CHECK(!peer.IsBlockInvDeferred(known));
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(peer, known), 0U);
}

BOOST_AUTO_TEST_CASE(active_owned_hash_is_not_duplicate_admitted)
{
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CScopedOrphanCountByNode isolatedOrphanCounts;
    CNode owner(INVALID_SOCKET, TestPeerAddress(68), "deferred-owner", true);
    CNode peer(INVALID_SOCKET, TestPeerAddress(69), "deferred-other", true);
    CScopedInitialBlockDownloadState ibdState(&peer);
    const uint256 owned(920006);
    const uint256 available(920007);

    BOOST_CHECK(TryAssignBlockRequestOwner(owned, owner.GetId(), BLOCKREQ_SOURCE_INV));
    owner.MarkBlockInFlight(owned);
    BOOST_REQUIRE(peer.DeferBlockInv(owned));
    BOOST_REQUIRE(peer.DeferBlockInv(available));
    {
        LOCK(cs_main);
        BOOST_CHECK_EQUAL(RefillDeferredBlockRequests(&peer), 1U);
    }
    BOOST_CHECK(peer.IsBlockInvDeferred(owned));
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(peer, owned), 0U);
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(peer, available), 1U);
    owner.ClearAskFor();
    peer.ClearAskFor();
}

BOOST_AUTO_TEST_CASE(peer_disconnect_clears_deferred_backlog)
{
    CNode* peer = new CNode(INVALID_SOCKET, TestPeerAddress(70),
                            "deferred-disconnect", true);
    BOOST_REQUIRE(peer->DeferBlockInv(uint256(920008)));
    BOOST_CHECK_EQUAL(peer->deferredBlockInv.size(), 1U);
    delete peer;
}

BOOST_AUTO_TEST_CASE(deferred_backlog_is_bounded)
{
    CNode peer(INVALID_SOCKET, TestPeerAddress(71), "deferred-bound", true);
    for (size_t i = 0; i < MAX_DEFERRED_BLOCK_INV_PER_PEER; ++i)
        BOOST_REQUIRE(peer.DeferBlockInv(uint256(930000 + i)));
    BOOST_CHECK(!peer.DeferBlockInv(uint256(940000)));
    BOOST_CHECK_EQUAL(peer.deferredBlockInv.size(),
                      MAX_DEFERRED_BLOCK_INV_PER_PEER);
}

BOOST_AUTO_TEST_CASE(overflow_does_not_touch_mapAlreadyAskedFor)
{
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CNode peer(INVALID_SOCKET, TestPeerAddress(72), "deferred-overflow", true);
    for (size_t i = 0; i < MAX_DEFERRED_BLOCK_INV_PER_PEER; ++i)
        BOOST_REQUIRE(peer.DeferBlockInv(uint256(941000 + i)));
    const uint256 overflow(950000);
    BOOST_CHECK(!peer.DeferBlockInv(overflow));
    LOCK(cs_mapAlreadyAskedFor);
    BOOST_CHECK_EQUAL(mapAlreadyAskedFor.count(CInv(MSG_BLOCK, overflow)), 0U);
}

BOOST_AUTO_TEST_CASE(single_blocked_hash_does_not_stall_sliding_window)
{
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CScopedOrphanCountByNode isolatedOrphanCounts;
    CNode owner(INVALID_SOCKET, TestPeerAddress(73), "sliding-owner", true);
    CNode peer(INVALID_SOCKET, TestPeerAddress(74), "sliding-peer", true);
    CScopedInitialBlockDownloadState ibdState(&peer);
    const uint256 blocked(950001);
    const uint256 nextA(950002);
    const uint256 nextB(950003);

    BOOST_CHECK(TryAssignBlockRequestOwner(blocked, owner.GetId(), BLOCKREQ_SOURCE_INV));
    owner.MarkBlockInFlight(blocked);
    BOOST_REQUIRE(peer.DeferBlockInv(blocked));
    BOOST_REQUIRE(peer.DeferBlockInv(nextA));
    BOOST_REQUIRE(peer.DeferBlockInv(nextB));
    {
        LOCK(cs_main);
        BOOST_CHECK_EQUAL(RefillDeferredBlockRequests(&peer), 2U);
    }
    BOOST_CHECK(peer.IsBlockInvDeferred(blocked));
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(peer, nextA), 1U);
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(peer, nextB), 1U);
    owner.ClearAskFor();
    peer.ClearAskFor();
}

BOOST_AUTO_TEST_CASE(orphan_recovery_has_priority_over_normal_deferred_inv)
{
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CScopedOrphanCountByNode isolatedOrphanCounts;
    CNode peer(INVALID_SOCKET, TestPeerAddress(75), "orphan-priority", true);
    CScopedInitialBlockDownloadState ibdState(&peer);
    const uint256 deferred(950004);
    const uint256 parent(950005);

    FillPeerActiveWindow(peer, 951000);
    BOOST_REQUIRE(peer.DeferBlockInv(deferred));
    peer.AskFor(CInv(MSG_BLOCK, parent), BLOCKREQ_SOURCE_ORPHAN);
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(peer, parent), 1U);
    BOOST_CHECK(peer.IsBlockInvDeferred(deferred));
    peer.ClearAskFor();
}

BOOST_AUTO_TEST_CASE(cross_peer_ownership_remains_correct)
{
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CNode owner(INVALID_SOCKET, TestPeerAddress(76), "cross-owner", true);
    CNode peer(INVALID_SOCKET, TestPeerAddress(77), "cross-deferred", true);
    const uint256 hash(950006);

    BOOST_CHECK(TryAssignBlockRequestOwner(hash, owner.GetId(), BLOCKREQ_SOURCE_INV));
    owner.MarkBlockInFlight(hash);
    BOOST_REQUIRE(peer.DeferBlockInv(hash));
    NodeId ownerPeer = -1;
    BlockRequestOwnerState ownerState = BLOCK_REQUEST_OWNER_IN_FLIGHT;
    BOOST_CHECK(GetBlockRequestOwner(hash, &ownerPeer, &ownerState));
    BOOST_CHECK_EQUAL(ownerPeer, owner.GetId());
    BOOST_CHECK_EQUAL(ownerState, BLOCK_REQUEST_OWNER_IN_FLIGHT);
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(peer, hash), 0U);
    owner.ClearAskFor();
}

BOOST_AUTO_TEST_CASE(continuation_candidate_is_eventually_reached)
{
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CScopedOrphanCountByNode isolatedOrphanCounts;
    CNode peer(INVALID_SOCKET, TestPeerAddress(78), "continuation-deferred", true);
    CScopedInitialBlockDownloadState ibdState(&peer);
    const uint256 continuation(960999);

    FillPeerActiveWindow(peer, 960000);
    for (int i = 0; i < 999; ++i)
        BOOST_REQUIRE(peer.DeferBlockInv(uint256(961000 + i)));
    BOOST_REQUIRE(peer.DeferBlockInv(continuation));
    peer.ClearAskFor();
    {
        LOCK(cs_main);
        size_t admitted = 0;
        for (int i = 0; i < 8 && peer.IsBlockInvDeferred(continuation); ++i)
        {
            admitted += RefillDeferredBlockRequests(&peer);
            peer.ClearAskFor();
        }
        BOOST_CHECK(admitted >= 1000U);
    }
    BOOST_CHECK(!peer.IsBlockInvDeferred(continuation));
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(peer, continuation), 0U);
}

BOOST_AUTO_TEST_CASE(inflight_expiry_under_cs_vsend_does_not_need_cs_vnodes)
{
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    const uint256 hash(5001);
    CNode peer(INVALID_SOCKET, TestPeerAddress(80), "inflight-expiry-peer", true);

    peer.MarkBlockInFlight(hash);
    peer.mapBlockInFlightSince[hash] = GetTime() - 60;

    {
        LOCK(peer.cs_vSend);
        peer.ExpireBlockInFlight();
    }

    BOOST_CHECK_EQUAL(peer.setBlocksInFlight.count(hash), 0U);
    BOOST_CHECK_EQUAL(peer.mapBlockInFlightSince.count(hash), 0U);
}

BOOST_AUTO_TEST_CASE(cross_peer_ownership_blocks_already_asked_erase)
{
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    const uint256 hash(5002);
    CNode owner(INVALID_SOCKET, TestPeerAddress(81), "cross-owner-one", true);
    CNode other(INVALID_SOCKET, TestPeerAddress(82), "cross-owner-other", true);
    PreparePeerForRecovery(owner, PROTOCOL_VERSION, nBestHeight + 1);
    PreparePeerForRecovery(other, PROTOCOL_VERSION, nBestHeight + 1);

    const CInv inv(MSG_BLOCK, hash);
    owner.AskFor(inv, BLOCKREQ_SOURCE_INV);
    BOOST_CHECK(TryAssignBlockRequestOwner(hash, owner.GetId(), BLOCKREQ_SOURCE_INV));
    owner.MarkBlockInFlight(hash);

    BOOST_CHECK(!EraseAlreadyAskedForIfUnowned(inv));
    {
        LOCK(cs_mapAlreadyAskedFor);
        BOOST_CHECK_EQUAL(mapAlreadyAskedFor.count(inv), 1U);
    }

    owner.ClearAskFor();
    BOOST_CHECK(EraseAlreadyAskedForIfUnowned(inv));
}

BOOST_AUTO_TEST_CASE(owner_erase_toctou_race)
{
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    const uint256 hash(5008);
    CNode peer(INVALID_SOCKET, TestPeerAddress(87), "toctou-peer", true);

    const CInv inv(MSG_BLOCK, hash);

    BOOST_CHECK(TryAssignBlockRequestOwner(hash, peer.GetId(), BLOCKREQ_SOURCE_INV));

    {
        LOCK(cs_mapAlreadyAskedFor);
        mapAlreadyAskedFor[inv] = GetTimeMicros();
    }

    BOOST_CHECK(!EraseAlreadyAskedForIfUnowned(inv));
    {
        LOCK(cs_mapAlreadyAskedFor);
        BOOST_CHECK_EQUAL(mapAlreadyAskedFor.count(inv), 1U);
    }

    ReleaseBlockRequestOwner(hash, peer.GetId(), "test");

    BOOST_CHECK(EraseAlreadyAskedForIfUnowned(inv));
}

BOOST_AUTO_TEST_CASE(disconnect_cleans_up_block_request_owners)
{
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    const uint256 hashA(5003);
    const uint256 hashB(5004);
    CNode peer(INVALID_SOCKET, TestPeerAddress(83), "disconnect-owner", true);

    BOOST_CHECK(TryAssignBlockRequestOwner(hashA, peer.GetId(), BLOCKREQ_SOURCE_INV));
    BOOST_CHECK(TryAssignBlockRequestOwner(hashB, peer.GetId(), BLOCKREQ_SOURCE_INV));
    peer.MarkBlockInFlight(hashB);

    NodeId ownerPeer = -1;
    BlockRequestOwnerState ownerState = BLOCK_REQUEST_OWNER_IN_FLIGHT;
    BOOST_CHECK(GetBlockRequestOwner(hashA, &ownerPeer, &ownerState));
    BOOST_CHECK_EQUAL(ownerPeer, peer.GetId());
    BOOST_CHECK_EQUAL(ownerState, BLOCK_REQUEST_OWNER_QUEUED);

    ownerPeer = -1;
    ownerState = BLOCK_REQUEST_OWNER_QUEUED;
    BOOST_CHECK(GetBlockRequestOwner(hashB, &ownerPeer, &ownerState));
    BOOST_CHECK_EQUAL(ownerPeer, peer.GetId());
    BOOST_CHECK_EQUAL(ownerState, BLOCK_REQUEST_OWNER_IN_FLIGHT);

    peer.Cleanup();

    BOOST_CHECK(!GetBlockRequestOwner(hashA, NULL, NULL));
    BOOST_CHECK(!GetBlockRequestOwner(hashB, NULL, NULL));
}

BOOST_AUTO_TEST_CASE(disconnect_cleanup_last_active_work_signals_wake)
{
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CNode peer(INVALID_SOCKET, TestPeerAddress(850), "disconnect-wake", true);
    const uint256 hash(5850);
    const std::vector<CNode*> noPeers;
    const PipelineWakeOutcome initialWakeOutcome =
        MaybeProcessPipelineWake(noPeers);
    BOOST_CHECK(initialWakeOutcome == PIPELINE_WAKE_OUTCOME_NONE ||
                initialWakeOutcome == PIPELINE_WAKE_TERMINAL_NOT_IBD);
    BOOST_CHECK_EQUAL(
        MaybeProcessPipelineWake(noPeers),
        PIPELINE_WAKE_OUTCOME_NONE);
    const uint64_t nWakeBefore =
        ibdmetrics::Get().pipeline_wake_signals.load(std::memory_order_relaxed);
    const int64_t nDisconnectWakeBefore =
        ibdmetrics::Get().pipeline_wake_signal_disconnect_cleanup.load(
            std::memory_order_relaxed);
    const int64_t nActiveBefore =
        ibdmetrics::Get().global_active_current.load(std::memory_order_relaxed);

    peer.AddAskForEntry(GetTimeMicros(), CInv(MSG_BLOCK, hash));
    BOOST_REQUIRE_EQUAL(peer.setAskForBlocks.size(), 1U);
    BOOST_REQUIRE_EQUAL(peer.setBlocksInFlight.size(), 0U);
    BOOST_REQUIRE_EQUAL(peer.getBlocksOutstandingSources.size(), 0U);
    BOOST_REQUIRE_EQUAL(
        ibdmetrics::Get().global_active_current.load(std::memory_order_relaxed),
        nActiveBefore + 1);

    peer.Cleanup();

    BOOST_CHECK(peer.setAskForBlocks.empty());
    BOOST_CHECK(peer.setBlocksInFlight.empty());
    BOOST_CHECK_EQUAL(
        ibdmetrics::Get().global_active_current.load(std::memory_order_relaxed),
        nActiveBefore);
    BOOST_CHECK_EQUAL(
        ibdmetrics::Get().pipeline_wake_signal_disconnect_cleanup.load(
            std::memory_order_relaxed),
        nDisconnectWakeBefore + 1);

    BOOST_CHECK_EQUAL(
        ibdmetrics::Get().pipeline_wake_signals.load(std::memory_order_relaxed),
        nWakeBefore + 1);

    BOOST_CHECK_EQUAL(
        MaybeProcessPipelineWake(noPeers),
        PIPELINE_WAKE_TERMINAL_NOT_IBD);

    const int64_t nActiveAfter =
        ibdmetrics::Get().global_active_current.load(std::memory_order_relaxed);
    const uint64_t nWakeAfter =
        ibdmetrics::Get().pipeline_wake_signals.load(std::memory_order_relaxed);
    peer.Cleanup();
    BOOST_CHECK_EQUAL(
        ibdmetrics::Get().global_active_current.load(std::memory_order_relaxed),
        nActiveAfter);
    BOOST_CHECK_EQUAL(
        ibdmetrics::Get().pipeline_wake_signals.load(std::memory_order_relaxed),
        nWakeAfter);
}

BOOST_AUTO_TEST_CASE(disconnect_cleanup_queued_unsent_getblocks_signals_wake)
{
    const int64_t nNow = WAKE_TEST_TIME;
    SetMockTime(nNow);
    ResetPipelineWakeStateForTesting();
    ibdmetrics::ResetPipelineWakeMetricsForTesting();

    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CNode peer(INVALID_SOCKET, TestPeerAddress(851), "disconnect-queued-gb", true);
    CScopedInitialBlockDownloadState ibdState(&peer);
    PrepareWakeEligiblePeer(peer, nBestHeight + 100, nNow);
    std::vector<CNode*> peers(1, &peer);

    peer.getBlocksIndex.push_back(pindexBest);
    peer.getBlocksHash.push_back(uint256(0));
    peer.getBlocksSources.push_back(
        ibdmetrics::GETBLOCKS_SOURCE_EMPTY_PIPELINE_WAKE);
    peer.getBlocksRecoveryIds.push_back(0);
    peer.getBlocksIndex.push_back(pindexBest);
    peer.getBlocksHash.push_back(uint256(0));
    peer.getBlocksSources.push_back(
        ibdmetrics::GETBLOCKS_SOURCE_EMPTY_PIPELINE_WAKE);
    peer.getBlocksRecoveryIds.push_back(0);
    ibdmetrics::GetBlocksQueuedAdd(2, true);

    const int64_t nWakeSignalsBefore =
        MetricGet(ibdmetrics::Get().pipeline_wake_signals);
    const int64_t nDisconnectWakeBefore =
        MetricGet(ibdmetrics::Get().pipeline_wake_signal_disconnect_cleanup);
    uint64_t nRequestedBefore = 0, nHandledBefore = 0;
    GetPipelineWakeStateForTesting(&nRequestedBefore, &nHandledBefore, NULL,
                                   NULL);

    BOOST_REQUIRE_EQUAL(peer.getBlocksIndex.size(), 2U);
    BOOST_REQUIRE_EQUAL(peer.getBlocksHash.size(), 2U);
    BOOST_REQUIRE_EQUAL(peer.getBlocksSources.size(), 2U);
    BOOST_REQUIRE_EQUAL(peer.getBlocksRecoveryIds.size(), 2U);
    BOOST_CHECK_EQUAL(peer.setAskForBlocks.size(), 0U);
    BOOST_CHECK_EQUAL(peer.setBlocksInFlight.size(), 0U);
    BOOST_CHECK_EQUAL(peer.getBlocksOutstandingSources.size(), 0U);
    BOOST_CHECK_EQUAL(
        MetricGet(ibdmetrics::Get().total_getblocks_queued_requests_current),
        2);
    BOOST_CHECK_EQUAL(
        MetricGet(ibdmetrics::Get().peers_with_queued_getblocks_current), 1);
    CheckWakeGaugeBalance(peers);

    peer.Cleanup();

    BOOST_CHECK(peer.getBlocksIndex.empty());
    BOOST_CHECK(peer.getBlocksHash.empty());
    BOOST_CHECK(peer.getBlocksSources.empty());
    BOOST_CHECK(peer.getBlocksRecoveryIds.empty());
    BOOST_CHECK_EQUAL(
        MetricGet(ibdmetrics::Get().total_getblocks_queued_requests_current),
        0);
    BOOST_CHECK_EQUAL(
        MetricGet(ibdmetrics::Get().peers_with_queued_getblocks_current), 0);
    BOOST_CHECK_EQUAL(MetricGet(ibdmetrics::Get().pipeline_wake_signals),
                      nWakeSignalsBefore + 1);
    BOOST_CHECK_EQUAL(
        MetricGet(ibdmetrics::Get().pipeline_wake_signal_disconnect_cleanup),
        nDisconnectWakeBefore + 1);

    uint64_t nRequested = 0, nHandled = 0;
    GetPipelineWakeStateForTesting(&nRequested, &nHandled, NULL, NULL);
    BOOST_CHECK_EQUAL(nRequested, nRequestedBefore + 1);
    BOOST_CHECK_EQUAL(nHandled, nHandledBefore);
    CheckWakeGaugeBalance(peers);

    // Repeated Cleanup must not re-decrement gauges or re-signal the wake.
    const int64_t nWakeAfter =
        MetricGet(ibdmetrics::Get().pipeline_wake_signals);
    peer.Cleanup();
    BOOST_CHECK_EQUAL(MetricGet(ibdmetrics::Get().pipeline_wake_signals),
                      nWakeAfter);
    BOOST_CHECK_EQUAL(
        MetricGet(ibdmetrics::Get().total_getblocks_queued_requests_current),
        0);
    BOOST_CHECK_EQUAL(
        MetricGet(ibdmetrics::Get().peers_with_queued_getblocks_current), 0);

    // The latched disconnect wake is still processable by the handler.
    const PipelineWakeOutcome outcome = MaybeProcessPipelineWake(peers);
    BOOST_CHECK_EQUAL(outcome, PIPELINE_WAKE_TERMINAL_GETBLOCKS_QUEUED);
    GetPipelineWakeStateForTesting(&nRequested, &nHandled, NULL, NULL);
    BOOST_CHECK_EQUAL(nHandled, nRequested);
    CheckWakeGaugeBalance(peers);

    ClearQueuedGetBlocks(peer);
    ResetPeerWakeDedupState(peer);
    ResetPipelineWakeStateForTesting();
    SetMockTime(0);
}

// --- Pipeline-wake regression matrix ----------------------------------------

BOOST_AUTO_TEST_CASE(cooldown_active_keeps_wake_latched)
{
    const int64_t nNow = WAKE_TEST_TIME;
    SetMockTime(nNow);
    ResetPipelineWakeStateForTesting();
    ibdmetrics::ResetPipelineWakeMetricsForTesting();

    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CNode peer(INVALID_SOCKET, TestPeerAddress(900), "wake-cooldown", true);
    CScopedInitialBlockDownloadState ibdState(&peer);
    PrepareWakeEligiblePeer(peer, nBestHeight + 100, nNow);
    std::vector<CNode*> peers(1, &peer);

    // Pending wake plus an active 1-second getblocks cooldown.
    RequestBlockPipelineWake(WAKE_CAUSE_OTHER);
    SetPipelineWakeLastGetBlocksTimeForTesting(nNow);

    const PipelineWakeOutcome firstOutcome = MaybeProcessPipelineWake(peers);
    BOOST_CHECK_EQUAL(firstOutcome, PIPELINE_WAKE_TRANSIENT_COOLDOWN_ACTIVE);

    uint64_t nRequested = 0, nHandled = 0;
    uint32_t nCauseBits = 0;
    int64_t nLastGetBlocks = 0;
    GetPipelineWakeStateForTesting(&nRequested, &nHandled, &nCauseBits,
                                   &nLastGetBlocks);
    BOOST_CHECK_EQUAL(nRequested, 1U);
    BOOST_CHECK_EQUAL(nHandled, 0U);
    BOOST_CHECK_EQUAL(nLastGetBlocks, nNow);
    BOOST_CHECK_EQUAL(peer.getBlocksIndex.size(), 0U);
    BOOST_CHECK_EQUAL(
        MetricGet(ibdmetrics::Get().total_getblocks_queued_requests_current),
        0);
    CheckWakeGaugeBalance(peers);

    // Advance past the cooldown; the same latched wake must still be served.
    SetMockTime(nNow + 2);
    const PipelineWakeOutcome secondOutcome = MaybeProcessPipelineWake(peers);
    BOOST_CHECK_EQUAL(secondOutcome, PIPELINE_WAKE_TERMINAL_GETBLOCKS_QUEUED);
    BOOST_CHECK_EQUAL(peer.getBlocksIndex.size(), 1U);
    BOOST_REQUIRE_EQUAL(peer.getBlocksSources.size(), 1U);
    BOOST_CHECK_EQUAL((int)peer.getBlocksSources[0],
                      (int)ibdmetrics::GETBLOCKS_SOURCE_EMPTY_PIPELINE_WAKE);
    BOOST_CHECK_EQUAL(
        MetricGet(ibdmetrics::Get().total_getblocks_queued_requests_current),
        1);
    CheckWakeGaugeBalance(peers);

    GetPipelineWakeStateForTesting(&nRequested, &nHandled, &nCauseBits,
                                   &nLastGetBlocks);
    BOOST_CHECK_EQUAL(nHandled, 1U);
    BOOST_CHECK_EQUAL(nLastGetBlocks, nNow + 2);

    ClearQueuedGetBlocks(peer);
    ResetPeerWakeDedupState(peer);
    ResetPipelineWakeStateForTesting();
    SetMockTime(0);
}

BOOST_AUTO_TEST_CASE(dedup_all_keeps_wake_latched)
{
    const int64_t nNow = WAKE_TEST_TIME;
    SetMockTime(nNow);
    ResetPipelineWakeStateForTesting();
    ibdmetrics::ResetPipelineWakeMetricsForTesting();

    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CNode peerA(INVALID_SOCKET, TestPeerAddress(910), "wake-dedup-a", true);
    CNode peerB(INVALID_SOCKET, TestPeerAddress(911), "wake-dedup-b", true);
    CNode peerC(INVALID_SOCKET, TestPeerAddress(912), "wake-dedup-c", true);
    CScopedInitialBlockDownloadState ibdState(&peerA);
    PrepareWakeEligiblePeer(peerA, nBestHeight + 100, nNow);
    PrepareWakeEligiblePeer(peerB, nBestHeight + 200, nNow);
    PrepareWakeEligiblePeer(peerC, nBestHeight + 300, nNow);

    // Every eligible peer holds the identical locator state inside the
    // 5-second dedup window.
    const int64_t nDedupTime = nNow - 1;
    MarkPeerWakeDedupBlocked(peerA, pindexBest, nDedupTime);
    MarkPeerWakeDedupBlocked(peerB, pindexBest, nDedupTime);
    MarkPeerWakeDedupBlocked(peerC, pindexBest, nDedupTime);

    std::vector<CNode*> peers;
    peers.push_back(&peerA);
    peers.push_back(&peerB);
    peers.push_back(&peerC);

    const int64_t nDedupBefore =
        MetricGet(ibdmetrics::Get().pipeline_wake_getblocks_dedup);
    RequestBlockPipelineWake(WAKE_CAUSE_OTHER);

    const PipelineWakeOutcome firstOutcome = MaybeProcessPipelineWake(peers);
    BOOST_CHECK_EQUAL(firstOutcome, PIPELINE_WAKE_TRANSIENT_DEDUP_ALL);

    uint64_t nRequested = 0, nHandled = 0;
    uint32_t nCauseBits = 0;
    int64_t nLastGetBlocks = 0;
    GetPipelineWakeStateForTesting(&nRequested, &nHandled, &nCauseBits,
                                   &nLastGetBlocks);
    BOOST_CHECK_EQUAL(nHandled, 0U);
    BOOST_CHECK_EQUAL(nLastGetBlocks, 0);
    BOOST_CHECK_EQUAL(TotalQueuedGetBlocks(peers), 0U);
    BOOST_CHECK_EQUAL(
        MetricGet(ibdmetrics::Get().pipeline_wake_getblocks_dedup),
        nDedupBefore + 3);
    CheckWakeGaugeBalance(peers);

    // Advance past the 5-second dedup window; same latched wake serves one
    // wake getblocks to exactly one peer.
    SetMockTime(nNow + 6);
    const PipelineWakeOutcome secondOutcome = MaybeProcessPipelineWake(peers);
    BOOST_CHECK_EQUAL(secondOutcome, PIPELINE_WAKE_TERMINAL_GETBLOCKS_QUEUED);
    BOOST_CHECK_EQUAL(TotalQueuedGetBlocks(peers), 1U);
    size_t nWakeQueued = 0;
    for (std::vector<CNode*>::const_iterator it = peers.begin();
         it != peers.end(); ++it)
    {
        if ((*it)->getBlocksIndex.size() == 1U)
        {
            ++nWakeQueued;
            BOOST_REQUIRE_EQUAL((*it)->getBlocksSources.size(), 1U);
            BOOST_CHECK_EQUAL(
                (int)(*it)->getBlocksSources[0],
                (int)ibdmetrics::GETBLOCKS_SOURCE_EMPTY_PIPELINE_WAKE);
        }
    }
    BOOST_CHECK_EQUAL(nWakeQueued, 1U);
    GetPipelineWakeStateForTesting(&nRequested, &nHandled, &nCauseBits,
                                   &nLastGetBlocks);
    BOOST_CHECK_EQUAL(nHandled, 1U);
    CheckWakeGaugeBalance(peers);

    for (std::vector<CNode*>::const_iterator it = peers.begin();
         it != peers.end(); ++it)
    {
        ClearQueuedGetBlocks(**it);
        ResetPeerWakeDedupState(**it);
    }
    ResetPipelineWakeStateForTesting();
    SetMockTime(0);
}

BOOST_AUTO_TEST_CASE(forced_cs_main_failure_keeps_generation_pending)
{
    const int64_t nNow = WAKE_TEST_TIME;
    SetMockTime(nNow);
    ResetPipelineWakeStateForTesting();
    ibdmetrics::ResetPipelineWakeMetricsForTesting();

    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CNode peer(INVALID_SOCKET, TestPeerAddress(920), "wake-csmain-fail", true);
    CScopedInitialBlockDownloadState ibdState(&peer);
    PrepareWakeEligiblePeer(peer, nBestHeight + 100, nNow);
    std::vector<CNode*> peers(1, &peer);

    RequestBlockPipelineWake(WAKE_CAUSE_OTHER);
    uint64_t nRequestedBefore = 0, nHandledBefore = 0;
    int64_t nLastGetBlocksBefore = 0;
    GetPipelineWakeStateForTesting(&nRequestedBefore, &nHandledBefore, NULL,
                                   &nLastGetBlocksBefore);

    const PipelineWakeOutcome outcome = MaybeProcessPipelineWake(peers, true);
    BOOST_CHECK_EQUAL(outcome, PIPELINE_WAKE_TRANSIENT_CS_MAIN_TRYLOCK_FAILED);

    uint64_t nRequested = 0, nHandled = 0;
    uint32_t nCauseBits = 0;
    int64_t nLastGetBlocks = 0;
    GetPipelineWakeStateForTesting(&nRequested, &nHandled, &nCauseBits,
                                   &nLastGetBlocks);
    BOOST_CHECK_EQUAL(nRequested, nRequestedBefore);
    BOOST_CHECK_EQUAL(nHandled, nHandledBefore);
    BOOST_CHECK_EQUAL(nLastGetBlocks, nLastGetBlocksBefore);
    BOOST_CHECK_EQUAL(peer.getBlocksIndex.size(), 0U);
    BOOST_CHECK_EQUAL(peer.setAskForBlocks.size(), 0U);
    BOOST_CHECK_EQUAL(peer.setBlocksInFlight.size(), 0U);
    BOOST_CHECK_EQUAL(peer.getBlocksOutstandingSources.size(), 0U);
    BOOST_CHECK(peer.pindexLastGetBlocksBegin == NULL);
    CheckWakeGaugeBalance(peers);

    // The same pending generation is served by a normal call.
    const PipelineWakeOutcome secondOutcome = MaybeProcessPipelineWake(peers);
    BOOST_CHECK_EQUAL(secondOutcome, PIPELINE_WAKE_TERMINAL_GETBLOCKS_QUEUED);
    BOOST_CHECK_EQUAL(peer.getBlocksIndex.size(), 1U);
    GetPipelineWakeStateForTesting(&nRequested, &nHandled, NULL, NULL);
    BOOST_CHECK_EQUAL(nHandled, nRequestedBefore);
    BOOST_CHECK(nHandled >= nHandledBefore);
    CheckWakeGaugeBalance(peers);

    ClearQueuedGetBlocks(peer);
    ResetPeerWakeDedupState(peer);
    ResetPipelineWakeStateForTesting();
    SetMockTime(0);
}

BOOST_AUTO_TEST_CASE(cs_vnodes_trylock_failure_keeps_generation_pending)
{
    const int64_t nNow = WAKE_TEST_TIME;
    SetMockTime(nNow);
    ResetPipelineWakeStateForTesting();
    ibdmetrics::ResetPipelineWakeMetricsForTesting();

    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CNode peer(INVALID_SOCKET, TestPeerAddress(930), "wake-vnodes-fail", true);
    CScopedInitialBlockDownloadState ibdState(&peer);
    PrepareWakeEligiblePeer(peer, nBestHeight + 100, nNow);
    std::vector<CNode*> peers(1, &peer);

    RequestBlockPipelineWake(WAKE_CAUSE_OTHER);
    uint64_t nRequestedBefore = 0, nHandledBefore = 0;
    int64_t nLastGetBlocksBefore = 0;
    GetPipelineWakeStateForTesting(&nRequestedBefore, &nHandledBefore, NULL,
                                   &nLastGetBlocksBefore);
    const PipelineWakeGauges gBefore = SnapshotWakeGauges();

    // A helper thread deterministically holds cs_vNodes so the handler's
    // second TRY_LOCK fails.  No production state is changed.
    boost::barrier barrier(2);
    boost::thread t([&barrier]() {
        LOCK(cs_vNodes);
        barrier.wait();
        barrier.wait();
    });
    barrier.wait();

    const PipelineWakeOutcome outcome = MaybeProcessPipelineWake(peers);
    BOOST_CHECK_EQUAL(outcome, PIPELINE_WAKE_TRANSIENT_CS_VNODES_TRYLOCK_FAILED);

    barrier.wait();
    t.join();

    uint64_t nRequested = 0, nHandled = 0;
    uint32_t nCauseBits = 0;
    int64_t nLastGetBlocks = 0;
    GetPipelineWakeStateForTesting(&nRequested, &nHandled, &nCauseBits,
                                   &nLastGetBlocks);
    BOOST_CHECK_EQUAL(nRequested, nRequestedBefore);
    BOOST_CHECK_EQUAL(nHandled, nHandledBefore);
    BOOST_CHECK_EQUAL(nLastGetBlocks, nLastGetBlocksBefore);
    BOOST_CHECK_EQUAL(peer.getBlocksIndex.size(), 0U);
    BOOST_CHECK_EQUAL(peer.setAskForBlocks.size(), 0U);
    BOOST_CHECK_EQUAL(peer.getBlocksOutstandingSources.size(), 0U);
    BOOST_CHECK_EQUAL(SnapshotWakeGauges().global_active_current,
                      gBefore.global_active_current);
    CheckWakeGaugeBalance(peers);

    // The same pending generation is served by a normal call.
    const PipelineWakeOutcome secondOutcome = MaybeProcessPipelineWake(peers);
    BOOST_CHECK_EQUAL(secondOutcome, PIPELINE_WAKE_TERMINAL_GETBLOCKS_QUEUED);
    BOOST_CHECK_EQUAL(peer.getBlocksIndex.size(), 1U);
    GetPipelineWakeStateForTesting(&nRequested, &nHandled, NULL, NULL);
    BOOST_CHECK_EQUAL(nHandled, nRequestedBefore);
    CheckWakeGaugeBalance(peers);

    ClearQueuedGetBlocks(peer);
    ResetPeerWakeDedupState(peer);
    ResetPipelineWakeStateForTesting();
    SetMockTime(0);
}

BOOST_AUTO_TEST_CASE(outstanding_response_clear_signals_wake)
{
    const int64_t nNow = WAKE_TEST_TIME;
    SetMockTime(nNow);
    ResetPipelineWakeStateForTesting();

    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CNode peer(INVALID_SOCKET, TestPeerAddress(940), "wake-outstanding", true);

    peer.getBlocksOutstandingSources.push_back(
        ibdmetrics::GETBLOCKS_SOURCE_INITIAL);
    ibdmetrics::GetBlocksOutstandingAdd(1);
    const int64_t nGaugeBefore =
        MetricGet(ibdmetrics::Get().getblocks_outstanding_current);
    BOOST_REQUIRE_EQUAL(nGaugeBefore, 1);

    uint64_t nRequestedBefore = 0, nHandledBefore = 0;
    GetPipelineWakeStateForTesting(&nRequestedBefore, &nHandledBefore, NULL,
                                   NULL);

    BOOST_CHECK(peer.ConsumeGetBlocksResponseFront());
    BOOST_CHECK(peer.getBlocksOutstandingSources.empty());
    BOOST_CHECK_EQUAL(
        MetricGet(ibdmetrics::Get().getblocks_outstanding_current),
        nGaugeBefore - 1);

    uint32_t nCauseBits = 0;
    uint64_t nRequested = 0, nHandled = 0;
    GetPipelineWakeStateForTesting(&nRequested, &nHandled, &nCauseBits, NULL);
    BOOST_CHECK(nCauseBits & WAKE_CAUSE_GETBLOCKS_OUTSTANDING_CLEARED);
    BOOST_CHECK_EQUAL(nRequested, nRequestedBefore + 1);
    BOOST_CHECK_EQUAL(nHandled, nHandledBefore);

    // A repeated call on the empty deque is a pure no-op.
    const uint64_t nRequestedBeforeRepeat = nRequested;
    BOOST_CHECK(!peer.ConsumeGetBlocksResponseFront());
    GetPipelineWakeStateForTesting(&nRequested, &nHandled, NULL, NULL);
    BOOST_CHECK_EQUAL(nRequested, nRequestedBeforeRepeat);
    BOOST_CHECK_EQUAL(
        MetricGet(ibdmetrics::Get().getblocks_outstanding_current),
        nGaugeBefore - 1);

    ResetPipelineWakeStateForTesting();
    SetMockTime(0);
}

BOOST_AUTO_TEST_CASE(cleanup_outstanding_clear_signals_once)
{
    const int64_t nNow = WAKE_TEST_TIME;
    SetMockTime(nNow);
    ResetPipelineWakeStateForTesting();

    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CNode peer(INVALID_SOCKET, TestPeerAddress(950), "wake-cleanup", true);

    const size_t kOutstanding = 3;
    for (size_t i = 0; i < kOutstanding; ++i)
        peer.getBlocksOutstandingSources.push_back(
            ibdmetrics::GETBLOCKS_SOURCE_INITIAL);
    ibdmetrics::GetBlocksOutstandingAdd((int64_t)kOutstanding);
    const int64_t nGaugeBefore =
        MetricGet(ibdmetrics::Get().getblocks_outstanding_current);
    BOOST_REQUIRE_EQUAL(nGaugeBefore, (int64_t)kOutstanding);
    const uint64_t nWakeBefore =
        MetricGet(ibdmetrics::Get().pipeline_wake_signals);
    const int64_t nNoResponseBefore =
        MetricGet(ibdmetrics::Get().getblocks_no_response_disconnect_cleanup);

    // No active work, only outstanding getblocks: the wake must be caused by
    // the outstanding-cleared + disconnect-cleanup pair alone.
    BOOST_REQUIRE(peer.setAskForBlocks.empty());
    BOOST_REQUIRE(peer.setBlocksInFlight.empty());
    peer.Cleanup();

    BOOST_CHECK(peer.getBlocksOutstandingSources.empty());
    BOOST_CHECK_EQUAL(
        MetricGet(ibdmetrics::Get().getblocks_outstanding_current),
        nGaugeBefore - (int64_t)kOutstanding);
    BOOST_CHECK_EQUAL(
        MetricGet(ibdmetrics::Get().getblocks_no_response_disconnect_cleanup),
        nNoResponseBefore + (int64_t)kOutstanding);
    BOOST_CHECK_EQUAL(MetricGet(ibdmetrics::Get().pipeline_wake_signals),
                      nWakeBefore + 1);

    uint32_t nCauseBits = 0;
    GetPipelineWakeStateForTesting(NULL, NULL, &nCauseBits, NULL);
    BOOST_CHECK(nCauseBits & WAKE_CAUSE_GETBLOCKS_OUTSTANDING_CLEARED);
    BOOST_CHECK(nCauseBits & WAKE_CAUSE_DISCONNECT_CLEANUP);

    // A repeated Cleanup must not re-account or re-signal.
    const int64_t nGaugeAfter =
        MetricGet(ibdmetrics::Get().getblocks_outstanding_current);
    const uint64_t nWakeAfter = MetricGet(ibdmetrics::Get().pipeline_wake_signals);
    peer.Cleanup();
    BOOST_CHECK_EQUAL(
        MetricGet(ibdmetrics::Get().getblocks_outstanding_current), nGaugeAfter);
    BOOST_CHECK_EQUAL(MetricGet(ibdmetrics::Get().pipeline_wake_signals),
                      nWakeAfter);

    ResetPipelineWakeStateForTesting();
    SetMockTime(0);
}

BOOST_AUTO_TEST_CASE(deferred_refill_precedes_wake_getblocks)
{
    const int64_t nNow = WAKE_TEST_TIME;
    SetMockTime(nNow);
    ResetPipelineWakeStateForTesting();
    ibdmetrics::ResetPipelineWakeMetricsForTesting();

    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CNode peer(INVALID_SOCKET, TestPeerAddress(960), "wake-refill", true);
    CScopedInitialBlockDownloadState ibdState(&peer);
    PrepareWakeEligiblePeer(peer, nBestHeight + 100, nNow);

    const uint256 deferred(930000);
    BOOST_REQUIRE(peer.DeferBlockInv(deferred));
    std::vector<CNode*> peers(1, &peer);

    const PipelineWakeGauges gBefore = SnapshotWakeGauges();
    BOOST_CHECK_EQUAL(gBefore.total_deferred_current, 1);

    RequestBlockPipelineWake(WAKE_CAUSE_OTHER);
    const PipelineWakeOutcome outcome = MaybeProcessPipelineWake(peers);
    BOOST_CHECK_EQUAL(outcome, PIPELINE_WAKE_TERMINAL_DEFERRED_REFILL_CREATED_WORK);

    // The deferred block was admitted into the queued AskFor set...
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(peer, deferred), 1U);
    BOOST_CHECK(!peer.IsBlockInvDeferred(deferred));
    // ... and no wake getblocks was queued.
    BOOST_CHECK_EQUAL(peer.getBlocksIndex.size(), 0U);
    BOOST_CHECK_EQUAL(
        MetricGet(ibdmetrics::Get().total_getblocks_queued_requests_current),
        0);

    uint64_t nRequested = 0, nHandled = 0;
    GetPipelineWakeStateForTesting(&nRequested, &nHandled, NULL, NULL);
    BOOST_CHECK_EQUAL(nHandled, nRequested);

    const PipelineWakeGauges gAfter = SnapshotWakeGauges();
    BOOST_CHECK_EQUAL(gAfter.global_active_current,
                      gBefore.global_active_current + 1);
    BOOST_CHECK_EQUAL(gAfter.total_queued_current,
                      gBefore.total_queued_current + 1);
    BOOST_CHECK_EQUAL(gAfter.total_deferred_current,
                      gBefore.total_deferred_current - 1);
    CheckWakeGaugeBalance(peers);

    peer.ClearAskFor();
    ResetPipelineWakeStateForTesting();
    SetMockTime(0);
}

BOOST_AUTO_TEST_CASE(refill_zero_then_continuation)
{
    const int64_t nNow = WAKE_TEST_TIME;
    SetMockTime(nNow);
    ResetPipelineWakeStateForTesting();
    ibdmetrics::ResetPipelineWakeMetricsForTesting();

    BOOST_REQUIRE(pindexBest != NULL);
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CNode ownerPeer(INVALID_SOCKET, TestPeerAddress(972), "wake-owner", true);
    CNode peer(INVALID_SOCKET, TestPeerAddress(971), "wake-refill-zero", true);
    CScopedInitialBlockDownloadState ibdState(&peer);
    PrepareWakeEligiblePeer(peer, nBestHeight + 100, nNow);

    // All deferred entries are unusable: already-have and two other-peer-owned
    // blocks.  The wake handler only runs the refill on an empty pipeline, so
    // a same-peer queued blocker is deliberately not used here (it would trip
    // PIPELINE_WAKE_TERMINAL_PIPELINE_NOT_EMPTY before the refill pass).
    const uint256 alreadyHave = pindexBest->GetBlockHash();
    const uint256 ownedA(930100);
    const uint256 ownedB(930200);
    BOOST_CHECK(TryAssignBlockRequestOwner(ownedA, ownerPeer.GetId(),
                                           BLOCKREQ_SOURCE_INV));
    BOOST_CHECK(TryAssignBlockRequestOwner(ownedB, ownerPeer.GetId(),
                                           BLOCKREQ_SOURCE_INV));
    BOOST_REQUIRE(peer.DeferBlockInv(alreadyHave));
    BOOST_REQUIRE(peer.DeferBlockInv(ownedA));
    BOOST_REQUIRE(peer.DeferBlockInv(ownedB));

    std::vector<CNode*> peers(1, &peer);
    const int64_t nRefillAttemptsBefore =
        MetricGet(ibdmetrics::Get().pipeline_wake_refill_attempts);
    RequestBlockPipelineWake(WAKE_CAUSE_OTHER);

    const PipelineWakeOutcome outcome = MaybeProcessPipelineWake(peers);
    BOOST_CHECK_EQUAL(outcome, PIPELINE_WAKE_TERMINAL_GETBLOCKS_QUEUED);

    // Refill was attempted first and admitted nothing.
    BOOST_CHECK_EQUAL(MetricGet(ibdmetrics::Get().pipeline_wake_refill_attempts),
                      nRefillAttemptsBefore + 1);
    BOOST_CHECK_EQUAL(MetricGet(ibdmetrics::Get().pipeline_wake_refill_admitted),
                      0);
    // The already-have entry was dropped; the rotated other-peer-owned entries
    // are still deferred and still owned.
    BOOST_CHECK(!peer.IsBlockInvDeferred(alreadyHave));
    BOOST_CHECK(peer.IsBlockInvDeferred(ownedA));
    BOOST_CHECK(peer.IsBlockInvDeferred(ownedB));
    NodeId nOwnerPeer = -1;
    BlockRequestOwnerState ownerState = BLOCK_REQUEST_OWNER_QUEUED;
    BOOST_CHECK(GetBlockRequestOwner(ownedA, &nOwnerPeer, &ownerState));
    BOOST_CHECK_EQUAL(nOwnerPeer, ownerPeer.GetId());
    BOOST_CHECK(GetBlockRequestOwner(ownedB, &nOwnerPeer, &ownerState));
    BOOST_CHECK_EQUAL(nOwnerPeer, ownerPeer.GetId());
    // Nothing new was admitted into the AskFor set.
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(peer, ownedA), 0U);
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(peer, ownedB), 0U);
    // The continuation queued exactly one wake getblocks.
    BOOST_CHECK_EQUAL(peer.getBlocksIndex.size(), 1U);
    BOOST_REQUIRE_EQUAL(peer.getBlocksSources.size(), 1U);
    BOOST_CHECK_EQUAL((int)peer.getBlocksSources[0],
                      (int)ibdmetrics::GETBLOCKS_SOURCE_EMPTY_PIPELINE_WAKE);

    uint64_t nRequested = 0, nHandled = 0;
    GetPipelineWakeStateForTesting(&nRequested, &nHandled, NULL, NULL);
    BOOST_CHECK_EQUAL(nHandled, nRequested);
    CheckWakeGaugeBalance(peers);

    peer.ClearAskFor();
    ClearQueuedGetBlocks(peer);
    peer.PopFrontDeferredBlockInv();
    peer.PopFrontDeferredBlockInv();
    ReleaseBlockRequestOwner(ownedA, ownerPeer.GetId(), "test");
    ReleaseBlockRequestOwner(ownedB, ownerPeer.GetId(), "test");
    ResetPeerWakeDedupState(peer);
    ResetPipelineWakeStateForTesting();
    SetMockTime(0);
}

BOOST_AUTO_TEST_CASE(multiple_eligible_peers_queue_exactly_one)
{
    const int64_t nNow = WAKE_TEST_TIME;
    SetMockTime(nNow);
    ResetPipelineWakeStateForTesting();
    ibdmetrics::ResetPipelineWakeMetricsForTesting();

    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CNode peerA(INVALID_SOCKET, TestPeerAddress(981), "wake-multi-a", true);
    CNode peerB(INVALID_SOCKET, TestPeerAddress(982), "wake-multi-b", true);
    CNode peerC(INVALID_SOCKET, TestPeerAddress(983), "wake-multi-c", true);
    CScopedInitialBlockDownloadState ibdState(&peerA);
    PrepareWakeEligiblePeer(peerA, nBestHeight + 100, nNow);
    PrepareWakeEligiblePeer(peerB, nBestHeight + 200, nNow);
    PrepareWakeEligiblePeer(peerC, nBestHeight + 300, nNow);

    std::vector<CNode*> peers;
    peers.push_back(&peerA);
    peers.push_back(&peerB);
    peers.push_back(&peerC);

    const int64_t nAttemptedBefore =
        MetricGet(ibdmetrics::Get().pipeline_wake_getblocks_attempted);
    const int64_t nQueuedBefore =
        MetricGet(ibdmetrics::Get().pipeline_wake_getblocks_queued);
    RequestBlockPipelineWake(WAKE_CAUSE_OTHER);

    const PipelineWakeOutcome outcome = MaybeProcessPipelineWake(peers);
    BOOST_CHECK_EQUAL(outcome, PIPELINE_WAKE_TERMINAL_GETBLOCKS_QUEUED);

    BOOST_CHECK_EQUAL(TotalQueuedGetBlocks(peers), 1U);
    BOOST_CHECK_EQUAL(
        MetricGet(ibdmetrics::Get().pipeline_wake_getblocks_attempted),
        nAttemptedBefore + 1);
    BOOST_CHECK_EQUAL(
        MetricGet(ibdmetrics::Get().pipeline_wake_getblocks_queued),
        nQueuedBefore + 1);
    BOOST_CHECK_EQUAL(MetricGet(ibdmetrics::Get().pipeline_wake_getblocks_dedup),
                      0);

    // The global cooldown is set exactly once, by the single queue.
    int64_t nLastGetBlocks = 0;
    GetPipelineWakeStateForTesting(NULL, NULL, NULL, &nLastGetBlocks);
    BOOST_CHECK_EQUAL(nLastGetBlocks, nNow);

    // Exactly one peer carries the wake getblocks, sourced as a wake.
    size_t nWakeQueued = 0;
    for (std::vector<CNode*>::const_iterator it = peers.begin();
         it != peers.end(); ++it)
    {
        if ((*it)->getBlocksIndex.size() == 1U)
        {
            ++nWakeQueued;
            BOOST_REQUIRE_EQUAL((*it)->getBlocksSources.size(), 1U);
            BOOST_CHECK_EQUAL(
                (int)(*it)->getBlocksSources[0],
                (int)ibdmetrics::GETBLOCKS_SOURCE_EMPTY_PIPELINE_WAKE);
        }
    }
    BOOST_CHECK_EQUAL(nWakeQueued, 1U);

    uint64_t nRequested = 0, nHandled = 0;
    GetPipelineWakeStateForTesting(&nRequested, &nHandled, NULL, NULL);
    BOOST_CHECK_EQUAL(nHandled, nRequested);
    CheckWakeGaugeBalance(peers);

    for (std::vector<CNode*>::const_iterator it = peers.begin();
         it != peers.end(); ++it)
    {
        ClearQueuedGetBlocks(**it);
        ResetPeerWakeDedupState(**it);
    }
    ResetPipelineWakeStateForTesting();
    SetMockTime(0);
}

BOOST_AUTO_TEST_CASE(top_peer_dedup_rotates_to_next)
{
    const int64_t nNow = WAKE_TEST_TIME;
    SetMockTime(nNow);
    ResetPipelineWakeStateForTesting();
    ibdmetrics::ResetPipelineWakeMetricsForTesting();

    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CNode peerTop(INVALID_SOCKET, TestPeerAddress(985), "wake-top", true);
    CNode peerSecond(INVALID_SOCKET, TestPeerAddress(986), "wake-second", true);
    CScopedInitialBlockDownloadState ibdState(&peerTop);
    PrepareWakeEligiblePeer(peerTop, nBestHeight + 300, nNow);
    PrepareWakeEligiblePeer(peerSecond, nBestHeight + 200, nNow);

    // Top-scoring peer is dedup-blocked; the second peer is clean.
    MarkPeerWakeDedupBlocked(peerTop, pindexBest, nNow - 1);

    std::vector<CNode*> peers;
    peers.push_back(&peerTop);
    peers.push_back(&peerSecond);

    // requested=2 so the candidate rotation starts at the top-scoring peer.
    SetPipelineWakeRequestedForTesting(2, 0, WAKE_CAUSE_OTHER);
    const int64_t nDedupBefore =
        MetricGet(ibdmetrics::Get().pipeline_wake_getblocks_dedup);
    const int64_t nQueuedBefore =
        MetricGet(ibdmetrics::Get().pipeline_wake_getblocks_queued);

    const PipelineWakeOutcome outcome = MaybeProcessPipelineWake(peers);
    BOOST_CHECK_EQUAL(outcome, PIPELINE_WAKE_TERMINAL_GETBLOCKS_QUEUED);

    BOOST_CHECK_EQUAL(peerTop.getBlocksIndex.size(), 0U);
    BOOST_CHECK_EQUAL(peerSecond.getBlocksIndex.size(), 1U);
    BOOST_REQUIRE_EQUAL(peerSecond.getBlocksSources.size(), 1U);
    BOOST_CHECK_EQUAL((int)peerSecond.getBlocksSources[0],
                      (int)ibdmetrics::GETBLOCKS_SOURCE_EMPTY_PIPELINE_WAKE);
    BOOST_CHECK_EQUAL(
        MetricGet(ibdmetrics::Get().pipeline_wake_getblocks_dedup),
        nDedupBefore + 1);
    BOOST_CHECK_EQUAL(
        MetricGet(ibdmetrics::Get().pipeline_wake_getblocks_queued),
        nQueuedBefore + 1);
    BOOST_CHECK_EQUAL(
        MetricGet(ibdmetrics::Get().pipeline_wake_getblocks_attempted),
        1);

    // The global cooldown is consumed only once, after the second peer queue.
    int64_t nLastGetBlocks = 0;
    GetPipelineWakeStateForTesting(NULL, NULL, NULL, &nLastGetBlocks);
    BOOST_CHECK_EQUAL(nLastGetBlocks, nNow);

    uint64_t nRequested = 0, nHandled = 0;
    GetPipelineWakeStateForTesting(&nRequested, &nHandled, NULL, NULL);
    BOOST_CHECK_EQUAL(nHandled, nRequested);
    CheckWakeGaugeBalance(peers);

    ClearQueuedGetBlocks(peerTop);
    ClearQueuedGetBlocks(peerSecond);
    ResetPeerWakeDedupState(peerTop);
    ResetPeerWakeDedupState(peerSecond);
    ResetPipelineWakeStateForTesting();
    SetMockTime(0);
}

BOOST_AUTO_TEST_CASE(queued_and_outstanding_suppress_wake)
{
    const int64_t nNow = WAKE_TEST_TIME;
    SetMockTime(nNow);
    ResetPipelineWakeStateForTesting();
    ibdmetrics::ResetPipelineWakeMetricsForTesting();

    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;

    // Subcase A: a pre-existing queued getblocks suppresses the wake.
    CNode peerQueued(INVALID_SOCKET, TestPeerAddress(987), "wake-queued", true);
    CScopedInitialBlockDownloadState ibdState(&peerQueued);
    PrepareWakeEligiblePeer(peerQueued, nBestHeight + 100, nNow);
    peerQueued.PushGetBlocks(pindexBest, uint256(0),
                             ibdmetrics::GETBLOCKS_SOURCE_INITIAL);
    std::vector<CNode*> peersQueued(1, &peerQueued);

    RequestBlockPipelineWake(WAKE_CAUSE_OTHER);
    const PipelineWakeOutcome outcomeA = MaybeProcessPipelineWake(peersQueued);
    BOOST_CHECK_EQUAL(outcomeA, PIPELINE_WAKE_TERMINAL_EXISTING_QUEUED_GETBLOCKS);
    BOOST_CHECK_EQUAL(peerQueued.getBlocksIndex.size(), 1U);
    BOOST_REQUIRE_EQUAL(peerQueued.getBlocksSources.size(), 1U);
    BOOST_CHECK_EQUAL((int)peerQueued.getBlocksSources[0],
                      (int)ibdmetrics::GETBLOCKS_SOURCE_INITIAL);
    uint64_t nRequested = 0, nHandled = 0;
    GetPipelineWakeStateForTesting(&nRequested, &nHandled, NULL, NULL);
    BOOST_CHECK_EQUAL(nHandled, nRequested);
    CheckWakeGaugeBalance(peersQueued);
    ClearQueuedGetBlocks(peerQueued);
    ResetPeerWakeDedupState(peerQueued);

    // Subcase B: a pre-existing outstanding getblocks suppresses the wake and
    // clearing it raises a fresh wake signal.
    CNode peerOutstanding(INVALID_SOCKET, TestPeerAddress(988),
                          "wake-outstanding-suppress", true);
    CScopedInitialBlockDownloadState ibdStateB(&peerOutstanding);
    PrepareWakeEligiblePeer(peerOutstanding, nBestHeight + 100, nNow);
    peerOutstanding.getBlocksOutstandingSources.push_back(
        ibdmetrics::GETBLOCKS_SOURCE_INITIAL);
    ibdmetrics::GetBlocksOutstandingAdd(1);
    std::vector<CNode*> peersOutstanding(1, &peerOutstanding);

    RequestBlockPipelineWake(WAKE_CAUSE_OTHER);
    const PipelineWakeOutcome outcomeB =
        MaybeProcessPipelineWake(peersOutstanding);
    BOOST_CHECK_EQUAL(outcomeB,
                      PIPELINE_WAKE_TERMINAL_OUTSTANDING_GETBLOCKS_PRESENT);
    BOOST_CHECK_EQUAL(peerOutstanding.getBlocksIndex.size(), 0U);
    GetPipelineWakeStateForTesting(&nRequested, &nHandled, NULL, NULL);
    BOOST_CHECK_EQUAL(nHandled, nRequested);
    CheckWakeGaugeBalance(peersOutstanding);

    uint64_t nRequestedBeforePop = 0, nHandledBeforePop = 0;
    uint32_t nCauseBitsBeforePop = 0;
    GetPipelineWakeStateForTesting(&nRequestedBeforePop, &nHandledBeforePop,
                                   &nCauseBitsBeforePop, NULL);
    BOOST_CHECK(peerOutstanding.ConsumeGetBlocksResponseFront());
    uint64_t nRequestedAfter = 0;
    uint32_t nCauseBitsAfter = 0;
    GetPipelineWakeStateForTesting(&nRequestedAfter, NULL, &nCauseBitsAfter,
                                   NULL);
    BOOST_CHECK_EQUAL(nRequestedAfter, nRequestedBeforePop + 1);
    BOOST_CHECK(nCauseBitsAfter & WAKE_CAUSE_GETBLOCKS_OUTSTANDING_CLEARED);
    CheckWakeGaugeBalance(peersOutstanding);

    ResetPipelineWakeStateForTesting();
    SetMockTime(0);
}

BOOST_AUTO_TEST_CASE(recovery_remains_fallback_with_outstanding_wake)
{
    const int64_t nNow = WAKE_TEST_TIME;
    const int64_t STALL_TIMEOUT = 15;
    SetMockTime(nNow);
    ResetPipelineWakeStateForTesting();
    ResetStalledSyncRecoveryStateForTesting();
    ibdmetrics::ResetPipelineWakeMetricsForTesting();

    BOOST_REQUIRE(pindexBest != NULL);
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CNode peer(INVALID_SOCKET, TestPeerAddress(989), "wake-recovery", true);
    PreparePeerForRecovery(peer, PROTOCOL_VERSION, nBestHeight + 10);
    CScopedInitialBlockDownloadState ibdState(&peer);
    ScopedPeerSocket socketScope(peer);
    std::vector<CNode*> peers(1, &peer);

    CStalledSyncRecoveryState& recoveryState =
        GetStalledSyncRecoveryStateForTesting();
    recoveryState.MarkSyncRequestSent(nNow);

    // Wake getblocks really queued and flushed over the wire.
    RequestBlockPipelineWake(WAKE_CAUSE_OTHER);
    const PipelineWakeOutcome wakeOutcome = MaybeProcessPipelineWake(peers);
    BOOST_CHECK_EQUAL(wakeOutcome, PIPELINE_WAKE_TERMINAL_GETBLOCKS_QUEUED);
    BOOST_CHECK_EQUAL(peer.getBlocksIndex.size(), 1U);
    BOOST_CHECK(SendMessages(&peer, false));
    BOOST_CHECK(peer.getBlocksIndex.empty());
    BOOST_CHECK_EQUAL(peer.getBlocksOutstandingSources.size(), 1U);
    BOOST_CHECK_EQUAL(
        MetricGet(ibdmetrics::Get().getblocks_outstanding_current), 1);
    BOOST_CHECK(peer.setBlocksInFlight.empty());
    BOOST_CHECK(peer.setAskForBlocks.empty());

    // Advance past the stall timeout with the outstanding wake getblocks still
    // unanswered.  The outstanding request is NOT pipeline-active work, so the
    // stalled-sync recovery still fires.
    SetMockTime(nNow + STALL_TIMEOUT + 1);
    CNode* pnodeRecovery = MaybeQueueStalledSyncRecovery(
        peers, pindexBest, nBestHeight, nNow + STALL_TIMEOUT + 1,
        STALL_TIMEOUT, STALL_TIMEOUT, recoveryState);
    BOOST_REQUIRE(pnodeRecovery == &peer);
    BOOST_CHECK_EQUAL(peer.getBlocksIndex.size(), 1U);
    BOOST_REQUIRE_EQUAL(peer.getBlocksSources.size(), 1U);
    BOOST_CHECK_EQUAL((int)peer.getBlocksSources[0],
                      (int)ibdmetrics::GETBLOCKS_SOURCE_RECOVERY);
    BOOST_CHECK_EQUAL(recoveryState.RecoveryAttempts(), 1U);
    BOOST_CHECK_EQUAL(recoveryState.LastRecoveryTime(), nNow + STALL_TIMEOUT + 1);

    // A subsequent wake execution does not reset the recovery backoff.
    ClearQueuedGetBlocks(peer);
    ResetPeerWakeDedupState(peer);
    // Drain the outstanding wake getblocks (flushed over the wire earlier) so
    // the peer is an eligible wake candidate again.
    peer.getBlocksOutstandingSources.clear();
    ibdmetrics::GetBlocksOutstandingAdd(-1);
    SetPipelineWakeRequestedForTesting(1, 0, WAKE_CAUSE_OTHER);
    const PipelineWakeOutcome wakeOutcome2 = MaybeProcessPipelineWake(peers);
    BOOST_CHECK_EQUAL(wakeOutcome2, PIPELINE_WAKE_TERMINAL_GETBLOCKS_QUEUED);
    BOOST_CHECK_EQUAL(recoveryState.RecoveryAttempts(), 1U);
    BOOST_CHECK_EQUAL(recoveryState.LastRecoveryTime(), nNow + STALL_TIMEOUT + 1);
    CheckWakeGaugeBalance(peers);

    ClearQueuedGetBlocks(peer);
    ResetPeerWakeDedupState(peer);
    peer.getBlocksOutstandingSources.clear();
    ResetStalledSyncRecoveryStateForTesting();
    ResetPipelineWakeStateForTesting();
    SetMockTime(0);
}

BOOST_AUTO_TEST_CASE(repeated_drain_refill_preserves_invariants)
{
    const int64_t nNow = WAKE_TEST_TIME;
    SetMockTime(nNow);
    ResetPipelineWakeStateForTesting();
    ibdmetrics::ResetPipelineWakeMetricsForTesting();

    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CNode peer(INVALID_SOCKET, TestPeerAddress(991), "wake-drain-loop", true);
    CScopedInitialBlockDownloadState ibdState(&peer);
    PrepareWakeEligiblePeer(peer, nBestHeight + 100, nNow);
    std::vector<CNode*> peers(1, &peer);

    for (int cycle = 0; cycle < 100; ++cycle)
    {
        const uint256 hash(1000000 + cycle);
        const uint256 deferredHash(2000000 + cycle);

        // 1. Active request (queued) plus an owner, drained through the wake
        //    machinery every cycle.
        peer.AskFor(CInv(MSG_BLOCK, hash), BLOCKREQ_SOURCE_INV);
        BOOST_CHECK(TryAssignBlockRequestOwner(hash, peer.GetId(),
                                               BLOCKREQ_SOURCE_INV));
        NodeId nOwnerPeer = -1;
        BlockRequestOwnerState ownerState = BLOCK_REQUEST_OWNER_QUEUED;
        BOOST_CHECK(GetBlockRequestOwner(hash, &nOwnerPeer, &ownerState));
        BOOST_CHECK_EQUAL(nOwnerPeer, peer.GetId());

        // Owner uniqueness: a second peer can never claim the same hash.
        NodeId nExisting = -1;
        BlockRequestOwnerState nExistingState = BLOCK_REQUEST_OWNER_QUEUED;
        BOOST_CHECK(!TryAssignBlockRequestOwner(
            hash, peer.GetId() + 1, BLOCKREQ_SOURCE_INV, &nExisting,
            &nExistingState));
        BOOST_CHECK_EQUAL(nExisting, peer.GetId());

        // 2. Drain.
        peer.ClearAskFor();

        // 3. Fresh wake.
        ResetPipelineWakeStateForTesting();
        RequestBlockPipelineWake(WAKE_CAUSE_OTHER);

        // 4. Refill or continuation.
        if (cycle % 2 == 0)
        {
            BOOST_REQUIRE(peer.DeferBlockInv(deferredHash));
            const PipelineWakeOutcome outcome = MaybeProcessPipelineWake(peers);
            BOOST_CHECK_EQUAL(outcome,
                              PIPELINE_WAKE_TERMINAL_DEFERRED_REFILL_CREATED_WORK);
            BOOST_CHECK_EQUAL(QueuedBlockAskForCount(peer, deferredHash), 1U);
        }
        else
        {
            const PipelineWakeOutcome outcome = MaybeProcessPipelineWake(peers);
            BOOST_CHECK_EQUAL(outcome, PIPELINE_WAKE_TERMINAL_GETBLOCKS_QUEUED);
            BOOST_CHECK_EQUAL(peer.getBlocksIndex.size(), 1U);
        }
        // No duplicate getblocks per wake execution.
        BOOST_CHECK(TotalQueuedGetBlocks(peers) <= 1U);

        // 5. Clear/reset.
        peer.ClearAskFor();
        ClearQueuedGetBlocks(peer);
        ResetPeerWakeDedupState(peer);
        peer.pindexLastGetBlocksBegin = NULL;
        peer.hashLastGetBlocksEnd = 0;
        peer.nLastGetBlocksTime = 0;

        uint64_t nRequested = 0, nHandled = 0;
        GetPipelineWakeStateForTesting(&nRequested, &nHandled, NULL, NULL);
        BOOST_CHECK(nHandled <= nRequested);

        // Invariants: gauges balance, ownership released by drain.
        CheckWakeGaugeBalance(peers);
        BOOST_CHECK(!GetBlockRequestOwner(hash, NULL, NULL));
    }

    ResetPipelineWakeStateForTesting();
    SetMockTime(0);
}

BOOST_AUTO_TEST_CASE(send_pass_skips_disconnected_peer)
{
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CNode disc(INVALID_SOCKET, TestPeerAddress(995), "sendpass-disc", true);
    CNode connA(INVALID_SOCKET, TestPeerAddress(996), "sendpass-conn-a", true);
    CNode connB(INVALID_SOCKET, TestPeerAddress(997), "sendpass-conn-b", true);

    PreparePeerForSendMessages(disc, PROTOCOL_VERSION);
    PreparePeerForSendMessages(connA, PROTOCOL_VERSION);
    PreparePeerForSendMessages(connB, PROTOCOL_VERSION);
    disc.PushGetBlocks(pindexBest, uint256(0), ibdmetrics::GETBLOCKS_SOURCE_INITIAL);
    connA.PushGetBlocks(pindexBest, uint256(0), ibdmetrics::GETBLOCKS_SOURCE_INITIAL);
    connB.PushGetBlocks(pindexBest, uint256(0), ibdmetrics::GETBLOCKS_SOURCE_INITIAL);
    disc.fDisconnect = true;

    // Live sockets for the peers that must flush.  `disc` keeps INVALID_SOCKET
    // so that a send pass which fails to skip it would attempt a real send on
    // a dead socket and disconnect it, failing the assertions below.
    ScopedPeerSocket socketA(connA);
    ScopedPeerSocket socketB(connB);

    std::vector<CNode*> vNodesCopy;
    vNodesCopy.push_back(&disc);
    vNodesCopy.push_back(&connA);
    vNodesCopy.push_back(&connB);

    // Mirror of the production send pass: skip fDisconnect peers, then send.
    BOOST_FOREACH(CNode* pnode, vNodesCopy)
    {
        if (pnode->fDisconnect)
            continue;
        TRY_LOCK(pnode->cs_vSend, lockSend);
        if (lockSend)
            SendMessages(pnode, false);
    }

    // The disconnected peer's getblocks was never flushed (no real send path).
    BOOST_CHECK_EQUAL(disc.getBlocksIndex.size(), 1U);
    BOOST_CHECK_EQUAL(disc.getBlocksOutstandingSources.size(), 0U);
    BOOST_CHECK_EQUAL(disc.vSendMsg.size(), 0U);
    // The connected peers after it were still processed.
    BOOST_CHECK_EQUAL(connA.getBlocksIndex.size(), 0U);
    BOOST_CHECK_EQUAL(connA.getBlocksOutstandingSources.size(), 1U);
    BOOST_CHECK_EQUAL(connB.getBlocksIndex.size(), 0U);
    BOOST_CHECK_EQUAL(connB.getBlocksOutstandingSources.size(), 1U);

    ClearQueuedGetBlocks(disc);
    ResetPeerWakeDedupState(disc);
    ResetPipelineWakeStateForTesting();
}

BOOST_AUTO_TEST_CASE(shutdown_with_pending_wake_is_safe)
{
    const int64_t nNow = WAKE_TEST_TIME;
    SetMockTime(nNow);
    ResetPipelineWakeStateForTesting();
    ibdmetrics::ResetPipelineWakeMetricsForTesting();

    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CNode peer(INVALID_SOCKET, TestPeerAddress(998), "wake-shutdown", true);
    CScopedInitialBlockDownloadState ibdState(&peer);
    PrepareWakeEligiblePeer(peer, nBestHeight + 100, nNow);
    std::vector<CNode*> peers(1, &peer);

    const bool fShutdownSaved = fShutdown;
    fShutdown = true;

    RequestBlockPipelineWake(WAKE_CAUSE_OTHER);
    uint64_t nRequestedBefore = 0, nHandledBefore = 0;
    GetPipelineWakeStateForTesting(&nRequestedBefore, &nHandledBefore, NULL,
                                   NULL);

    const PipelineWakeOutcome outcome = MaybeProcessPipelineWake(peers);
    BOOST_CHECK_EQUAL(outcome, PIPELINE_WAKE_TRANSIENT_SHUTDOWN);

    uint64_t nRequested = 0, nHandled = 0;
    GetPipelineWakeStateForTesting(&nRequested, &nHandled, NULL, NULL);
    BOOST_CHECK_EQUAL(nRequested, nRequestedBefore);
    BOOST_CHECK_EQUAL(nHandled, nHandledBefore);
    BOOST_CHECK_EQUAL(peer.getBlocksIndex.size(), 0U);
    BOOST_CHECK_EQUAL(peer.setAskForBlocks.size(), 0U);
    BOOST_CHECK_EQUAL(peer.setBlocksInFlight.size(), 0U);
    BOOST_CHECK_EQUAL(peer.getBlocksOutstandingSources.size(), 0U);
    // No lock leak: both locks are immediately re-acquirable.
    {
        LOCK(cs_main);
    }
    {
        LOCK(cs_vNodes);
    }
    CheckWakeGaugeBalance(peers);

    fShutdown = fShutdownSaved;

    // The still-pending generation processes safely once shutdown clears.
    const PipelineWakeOutcome secondOutcome = MaybeProcessPipelineWake(peers);
    BOOST_CHECK_EQUAL(secondOutcome, PIPELINE_WAKE_TERMINAL_GETBLOCKS_QUEUED);
    BOOST_CHECK_EQUAL(peer.getBlocksIndex.size(), 1U);
    GetPipelineWakeStateForTesting(&nRequested, &nHandled, NULL, NULL);
    BOOST_CHECK_EQUAL(nHandled, nRequested);
    CheckWakeGaugeBalance(peers);

    ClearQueuedGetBlocks(peer);
    ResetPeerWakeDedupState(peer);
    ResetPipelineWakeStateForTesting();
    SetMockTime(0);
}

BOOST_AUTO_TEST_CASE(terminal_pipeline_not_empty_ends_latency_window)
{
    const int64_t nNow = WAKE_TEST_TIME;
    SetMockTime(nNow);
    ResetPipelineWakeStateForTesting();
    ibdmetrics::ResetPipelineWakeMetricsForTesting();

    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CNode peer(INVALID_SOCKET, TestPeerAddress(901), "wake-latency-nonempty",
               true);
    CScopedInitialBlockDownloadState ibdState(&peer);
    PrepareWakeEligiblePeer(peer, nBestHeight + 100, nNow);
    std::vector<CNode*> peers(1, &peer);

    const uint256 hash(5901);
    peer.MarkBlockInFlight(hash);
    BOOST_REQUIRE_EQUAL(peer.setBlocksInFlight.size(), 1U);

    // A latched latency window from an earlier pending wake.
    ibdmetrics::Get().pipeline_wake_signal_start_ms.store(
        5555, std::memory_order_relaxed);

    RequestBlockPipelineWake(WAKE_CAUSE_OTHER);
    const PipelineWakeOutcome outcome = MaybeProcessPipelineWake(peers);
    BOOST_CHECK_EQUAL(outcome, PIPELINE_WAKE_TERMINAL_PIPELINE_NOT_EMPTY);
    BOOST_CHECK_EQUAL(
        MetricGet(ibdmetrics::Get().pipeline_wake_signal_start_ms), 0);

    peer.ClearBlockInFlight(hash);
    ResetPipelineWakeStateForTesting();
    ibdmetrics::ResetPipelineWakeMetricsForTesting();
    SetMockTime(0);
}

BOOST_AUTO_TEST_CASE(transient_cooldown_keeps_latency_window)
{
    const int64_t nNow = WAKE_TEST_TIME;
    SetMockTime(nNow);
    ResetPipelineWakeStateForTesting();
    ibdmetrics::ResetPipelineWakeMetricsForTesting();

    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CNode peer(INVALID_SOCKET, TestPeerAddress(902), "wake-latency-cooldown",
               true);
    CScopedInitialBlockDownloadState ibdState(&peer);
    PrepareWakeEligiblePeer(peer, nBestHeight + 100, nNow);
    std::vector<CNode*> peers(1, &peer);

    RequestBlockPipelineWake(WAKE_CAUSE_OTHER);
    SetPipelineWakeLastGetBlocksTimeForTesting(nNow);
    const int64_t nWindowStart =
        MetricGet(ibdmetrics::Get().pipeline_wake_signal_start_ms);
    BOOST_CHECK(nWindowStart != 0);

    const PipelineWakeOutcome firstOutcome = MaybeProcessPipelineWake(peers);
    BOOST_CHECK_EQUAL(firstOutcome, PIPELINE_WAKE_TRANSIENT_COOLDOWN_ACTIVE);
    BOOST_CHECK_EQUAL(
        MetricGet(ibdmetrics::Get().pipeline_wake_signal_start_ms),
        nWindowStart);

    // The same wake later queues a getblocks; the window stays open because
    // restoration is now attributable to it.
    SetMockTime(nNow + 2);
    const PipelineWakeOutcome secondOutcome = MaybeProcessPipelineWake(peers);
    BOOST_CHECK_EQUAL(secondOutcome, PIPELINE_WAKE_TERMINAL_GETBLOCKS_QUEUED);
    BOOST_CHECK_EQUAL(
        MetricGet(ibdmetrics::Get().pipeline_wake_signal_start_ms),
        nWindowStart);

    ClearQueuedGetBlocks(peer);
    ResetPeerWakeDedupState(peer);
    ResetPipelineWakeStateForTesting();
    ibdmetrics::ResetPipelineWakeMetricsForTesting();
    SetMockTime(0);
}

BOOST_AUTO_TEST_CASE(new_wake_after_terminal_starts_fresh_latency_window)
{
    const int64_t nNow = WAKE_TEST_TIME;
    SetMockTime(nNow);
    ResetPipelineWakeStateForTesting();
    ibdmetrics::ResetPipelineWakeMetricsForTesting();

    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CNode peer(INVALID_SOCKET, TestPeerAddress(903), "wake-latency-fresh",
               true);
    CScopedInitialBlockDownloadState ibdState(&peer);
    PrepareWakeEligiblePeer(peer, nBestHeight + 100, nNow);
    std::vector<CNode*> peers(1, &peer);

    const uint256 hash(5903);
    peer.MarkBlockInFlight(hash);
    BOOST_REQUIRE_EQUAL(peer.setBlocksInFlight.size(), 1U);

    // A terminal outcome that creates no work must close the stale window so
    // the next wake opens a fresh one.
    ibdmetrics::Get().pipeline_wake_signal_start_ms.store(
        9999, std::memory_order_relaxed);
    RequestBlockPipelineWake(WAKE_CAUSE_OTHER);
    const PipelineWakeOutcome firstOutcome = MaybeProcessPipelineWake(peers);
    BOOST_CHECK_EQUAL(firstOutcome, PIPELINE_WAKE_TERMINAL_PIPELINE_NOT_EMPTY);
    BOOST_CHECK_EQUAL(
        MetricGet(ibdmetrics::Get().pipeline_wake_signal_start_ms), 0);

    // A brand-new wake opens a fresh window instead of inheriting the old start.
    RequestBlockPipelineWake(WAKE_CAUSE_OTHER);
    const int64_t nNewWindow =
        MetricGet(ibdmetrics::Get().pipeline_wake_signal_start_ms);
    BOOST_CHECK(nNewWindow != 0);
    BOOST_CHECK(nNewWindow != 9999);

    peer.ClearBlockInFlight(hash);
    ResetPipelineWakeStateForTesting();
    ibdmetrics::ResetPipelineWakeMetricsForTesting();
    SetMockTime(0);
}

BOOST_AUTO_TEST_CASE(ownerless_queued_entry_erase_is_safe)
{
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    const uint256 hash(5005);
    CNode peer(INVALID_SOCKET, TestPeerAddress(84), "ownerless-askfor", true);

    peer.AddAskForEntry(GetTimeMicros(), CInv(MSG_BLOCK, hash));

    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(peer, hash), 1U);

    const CInv inv(MSG_BLOCK, hash);
    {
        LOCK(cs_mapAlreadyAskedFor);
        mapAlreadyAskedFor[inv] = GetTimeMicros();
    }

    BOOST_CHECK(EraseAlreadyAskedForIfUnowned(inv));
    {
        LOCK(cs_mapAlreadyAskedFor);
        BOOST_CHECK_EQUAL(mapAlreadyAskedFor.count(inv), 0U);
    }
}

BOOST_AUTO_TEST_CASE(is_block_request_owned_by_any_peer_no_cs_vnodes)
{
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    const uint256 hash(5006);
    CNode peer(INVALID_SOCKET, TestPeerAddress(85), "ownership-no-vnodes", true);

    BOOST_CHECK(TryAssignBlockRequestOwner(hash, peer.GetId(), BLOCKREQ_SOURCE_INV));
    peer.MarkBlockInFlight(hash);

    {
        LOCK(peer.cs_vSend);
        BOOST_CHECK(IsBlockRequestOwnedByAnyPeer(hash));
    }

    ReleaseBlockRequestOwner(hash, peer.GetId(), "test");

    {
        LOCK(peer.cs_vSend);
        BOOST_CHECK(!IsBlockRequestOwnedByAnyPeer(hash));
    }

    peer.setBlocksInFlight.clear();
}

BOOST_AUTO_TEST_CASE(concurrent_cs_vnodes_and_cs_vsend_no_deadlock)
{
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CNode peer(INVALID_SOCKET, TestPeerAddress(86), "concurrent-peer", true);
    const uint256 hash(5007);
    peer.MarkBlockInFlight(hash);
    peer.mapBlockInFlightSince[hash] = GetTime() - 60;

    boost::barrier barrier(2);

    boost::thread t([&peer, &barrier]() {
        LOCK(cs_vNodes);
        barrier.wait();
        barrier.wait();
    });

    barrier.wait();
    {
        LOCK(peer.cs_vSend);
        peer.ExpireBlockInFlight();
    }
    barrier.wait();

    t.join();
    BOOST_CHECK_EQUAL(peer.setBlocksInFlight.count(hash), 0U);
}

BOOST_AUTO_TEST_CASE(continuity_break_is_one_shot_and_interval_gated)
{
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    const bool fPrintToConsoleSaved = fPrintToConsole;
    fPrintToConsole = true;
    BOOST_REQUIRE(InitBlockRequestTrace(true, ""));

    const std::map<std::string, std::string> mapArgsSaved = mapArgs;
    CNode peer(INVALID_SOCKET, TestPeerAddress(91), "continuity-peer", true);
    const uint256 hash(910001);
    const uint256 prev(910002);
    const uint256 tip(910003);
    const uint256 peerBest(910004);

    // Default interval (60s): a recent acceptance suppresses the event.
    mapArgs["-continuitybreakms"] = "60000";
    BOOST_CHECK(!BlockRequestTraceContinuityBreak(
        &peer, hash, prev, 100, tip, 10, 200, peerBest,
        false, false, false, false, -1, "none", 0, 0, 1));

    // No block ever connected (-1): startup transient, always suppressed.
    mapArgs["-continuitybreakms"] = "0";
    BOOST_CHECK(!BlockRequestTraceContinuityBreak(
        &peer, hash, prev, 100, tip, -1, 200, peerBest,
        false, false, false, false, -1, "none", 0, 0, 1));

    // Once the gap exceeds the interval the event fires exactly once.
    mapArgs["-continuitybreakms"] = "0";
    BOOST_CHECK(BlockRequestTraceContinuityBreak(
        &peer, hash, prev, 100, tip, 120, 200, peerBest,
        true, false, true, true, 7, "queued", 3, 5, 0));
    BOOST_CHECK(!BlockRequestTraceContinuityBreak(
        &peer, hash, prev, 100, tip, 120, 200, peerBest,
        true, false, true, true, 7, "queued", 3, 5, 0));

    // Re-enabling the trace resets the one-shot gate.
    BOOST_CHECK(InitBlockRequestTrace(false, ""));
    BOOST_CHECK(InitBlockRequestTrace(true, ""));
    BOOST_CHECK(BlockRequestTraceContinuityBreak(
        &peer, hash, prev, 100, tip, 120, 200, peerBest,
        false, false, false, false, -1, "none", 0, 0, 1));

    // Disabled trace: never fires.
    BOOST_CHECK(InitBlockRequestTrace(false, ""));
    BOOST_CHECK(!BlockRequestTraceContinuityBreak(
        &peer, hash, prev, 100, tip, 120, 200, peerBest,
        false, false, false, false, -1, "none", 0, 0, 1));

    mapArgs = mapArgsSaved;
    fPrintToConsole = fPrintToConsoleSaved;
}

BOOST_AUTO_TEST_CASE(missing_parent_request_resolution_lifecycle)
{
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    const bool fPrintToConsoleSaved = fPrintToConsole;
    fPrintToConsole = true;
    BOOST_REQUIRE(InitBlockRequestTrace(true, ""));

    CNode peer(INVALID_SOCKET, TestPeerAddress(92), "missing-parent-peer", true);
    const uint256 orphan(920001);
    const uint256 orphanPrev(920002);
    const uint256 wanted(920003);

    // Nothing pending yet -> no resolution reported.
    BOOST_CHECK(!BlockRequestTraceMissingParentResolved(&peer, wanted, true, 101));

    BlockRequestTraceMissingParentRequest(&peer, orphan, orphanPrev, wanted,
                                          true, true, 2, 3);

    // Arrival of the wanted hash resolves the pending request.
    BOOST_CHECK(BlockRequestTraceMissingParentResolved(&peer, wanted, true, 101));
    // A second delivery of the same hash is not a pending request anymore.
    BOOST_CHECK(!BlockRequestTraceMissingParentResolved(&peer, wanted, true, 101));

    BOOST_CHECK(InitBlockRequestTrace(false, ""));
    fPrintToConsole = fPrintToConsoleSaved;
}

BOOST_AUTO_TEST_CASE(watermark_events_fire_at_thresholds)
{
    const bool fPrintToConsoleSaved = fPrintToConsole;
    fPrintToConsole = true;
    BOOST_REQUIRE(InitBlockRequestTrace(true, ""));

    const int peer = 901;
    BOOST_CHECK(!BlockRequestTraceOrphanWatermark(peer, 10, "add"));
    BOOST_CHECK(BlockRequestTraceOrphanWatermark(peer, 64, "add"));
    BOOST_CHECK(!BlockRequestTraceOrphanWatermark(peer, 65, "add"));
    BOOST_CHECK(BlockRequestTraceOrphanWatermark(peer, 128, "add"));
    BOOST_CHECK(BlockRequestTraceOrphanWatermark(peer, 256, "add"));
    BOOST_CHECK(BlockRequestTraceOrphanWatermark(peer, 512, "add"));
    BOOST_CHECK(BlockRequestTraceOrphanWatermark(peer, 700, "add"));
    BOOST_CHECK(BlockRequestTraceOrphanWatermark(peer, 750, "add"));

    BOOST_CHECK(!BlockRequestTraceDeferredWatermark(peer, 63, "add"));
    BOOST_CHECK(BlockRequestTraceDeferredWatermark(peer, 64, "add"));

    BOOST_CHECK(InitBlockRequestTrace(false, ""));
    fPrintToConsole = fPrintToConsoleSaved;
}

// --- Frontier admission exemption -----------------------------------------
//
// During IBD the active tip H is present; the getblocks response announced
// H+1 as its first unknown block inv.  When the peer's request window is full
// (queued + in-flight at the per-peer cap) the deferred budget is zero,
// ordinary admission is denied and the INV would otherwise be deferred (and
// eventually dropped).  The frontier exemption admits exactly one such
// candidate so the connectable block is requested, and clears the slot when
// the block is received, after which drained pressure resumes ordinary
// deferred admission.  Note the budget is zero because the request window is
// full -- orphan storage pressure no longer counts toward the budget (see
// request_budget_ignores_orphan_storage_pressure), but a non-zero orphan
// count is still required for the exemption to apply as a stall backup.

class CScopedFrontierState
{
public:
    CScopedFrontierState()
    {
        ClearFrontierCandidate();
    }

    ~CScopedFrontierState()
    {
        ClearFrontierCandidate();
    }
};

BOOST_AUTO_TEST_CASE(frontier_admission_breaks_full_request_window_stall)
{
    BOOST_REQUIRE(pindexBest != NULL);
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CScopedOrphanCountByNode isolatedOrphanCounts;
    CScopedFrontierState isolatedFrontier;

    const uint256 hashFrontier(910001);
    const uint256 hashUnrelated(910002);
    const CInv invFrontier(MSG_BLOCK, hashFrontier);
    const CInv invUnrelated(MSG_BLOCK, hashUnrelated);

    CNode peer(INVALID_SOCKET, TestPeerAddress(60), "frontier-fixed-point", true);
    CScopedInitialBlockDownloadState ibdState(&peer);
    {
        LOCK(cs_main);
        mapOrphanCountByNode[peer.GetId()] = 200;
    }
    peer.fFrontierResponsePending = true;
    peer.nFrontierLocatorHeight = nBestHeight;
    // The request window is full (queued at the per-peer cap).  Ordinary
    // admission is denied even though the peer holds 200 orphans: orphan
    // storage pressure no longer zeroes the request budget -- a full request
    // window does.
    FillPeerActiveWindow(peer, 913000);

    // Ordinary admission is denied: the request window is full.
    {
        LOCK(cs_main);
        BOOST_CHECK(!TryAdmitBlockInvOrDefer(&peer, invUnrelated, false));
    }
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(peer, hashUnrelated), 0U);
    BOOST_CHECK(peer.IsBlockInvDeferred(hashUnrelated));

    // The frontier candidate (first unknown block inv of the active-tip-locator
    // getblocks response) bypasses the zero budget and is queued for getdata.
    {
        LOCK(cs_main);
        BOOST_CHECK(TryAdmitBlockInvOrDefer(&peer, invFrontier, true));
    }
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(peer, hashFrontier), 1U);
    BOOST_CHECK(!peer.IsBlockInvDeferred(hashFrontier));

    // Simulate the full receive path: ownership assigned on getdata send,
    // then the block arrives and releases the request.  The receive clears
    // the frontier slot (ClearBlockInFlight -> ReleaseBlockRequestOwner, and
    // ProcessMessage's ReleaseBlockRequestOwnerOnReceive).
    BOOST_CHECK(TryAssignBlockRequestOwner(hashFrontier, peer.GetId(),
                                           BLOCKREQ_SOURCE_ASKFOR));
    peer.MarkBlockInFlight(hashFrontier);
    peer.ClearBlockInFlight(hashFrontier);
    ReleaseBlockRequestOwnerOnReceive(hashFrontier, peer.GetId());

    // Window drained: ordinary deferred admission resumes.
    peer.ClearAskFor();
    {
        LOCK(cs_main);
        mapOrphanCountByNode[peer.GetId()] = 10;
        BOOST_CHECK(TryAdmitBlockInvOrDefer(&peer, invUnrelated, false));
    }
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(peer, hashUnrelated), 1U);
    peer.ClearAskFor();
}

BOOST_AUTO_TEST_CASE(frontier_exemption_holds_single_slot_across_peers)
{
    BOOST_REQUIRE(pindexBest != NULL);
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CScopedOrphanCountByNode isolatedOrphanCounts;
    CScopedFrontierState isolatedFrontier;

    const uint256 hashFrontier(910011);
    const uint256 hashSecond(910012);
    const CInv invFrontier(MSG_BLOCK, hashFrontier);
    const CInv invSecond(MSG_BLOCK, hashSecond);

    CNode peerA(INVALID_SOCKET, TestPeerAddress(62), "frontier-slot-a", true);
    CScopedInitialBlockDownloadState ibdState(&peerA);
    CNode peerB(INVALID_SOCKET, TestPeerAddress(63), "frontier-slot-b", true);
    peerA.fFrontierResponsePending = true;
    peerA.nFrontierLocatorHeight = nBestHeight;
    peerB.fFrontierResponsePending = true;
    peerB.nFrontierLocatorHeight = nBestHeight;
    {
        LOCK(cs_main);
        mapOrphanCountByNode[peerA.GetId()] = 200;
        mapOrphanCountByNode[peerB.GetId()] = 200;
    }
    // Both peers have full request windows, so both are at a zero budget.
    FillPeerActiveWindow(peerA, 914000);
    FillPeerActiveWindow(peerB, 914500);

    // Exactly one exemption outstanding: A claims it for H+1...
    {
        LOCK(cs_main);
        BOOST_CHECK(TryAdmitBlockInvOrDefer(&peerA, invFrontier, true));
    }
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(peerA, hashFrontier), 1U);

    // ...so a different frontier candidate from another peer is refused while
    // the slot is busy, despite an identical zero-budget situation.
    {
        LOCK(cs_main);
        BOOST_CHECK(!TryAdmitBlockInvOrDefer(&peerB, invSecond, true));
    }
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(peerB, hashSecond), 0U);
    BOOST_CHECK(peerB.IsBlockInvDeferred(hashSecond));

    // The same candidate re-offered by a different peer cannot duplicate the
    // owned request.
    {
        LOCK(cs_main);
        BOOST_CHECK(!TryAdmitBlockInvOrDefer(&peerB, invFrontier, true));
    }
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(peerA, hashFrontier), 1U);
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(peerB, hashFrontier), 0U);

    // Disconnect of the slot-holding peer releases the frontier state; a
    // fresh candidate can then be admitted.
    ReleaseBlockRequestOwnersForPeer(peerA.GetId(), "disconnect");
    {
        LOCK(cs_main);
        BOOST_CHECK(TryAdmitBlockInvOrDefer(&peerB, invSecond, true));
    }
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(peerB, hashSecond), 1U);
    peerA.ClearAskFor();
    peerB.ClearAskFor();
}

BOOST_AUTO_TEST_CASE(frontier_admission_refused_when_locator_is_stale)
{
    BOOST_REQUIRE(pindexBest != NULL);
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CScopedOrphanCountByNode isolatedOrphanCounts;
    CScopedFrontierState isolatedFrontier;

    const uint256 hashFrontier(910021);
    const CInv invFrontier(MSG_BLOCK, hashFrontier);

    CNode peer(INVALID_SOCKET, TestPeerAddress(64), "frontier-stale-locator", true);
    CScopedInitialBlockDownloadState ibdState(&peer);
    {
        LOCK(cs_main);
        mapOrphanCountByNode[peer.GetId()] = 200;
    }
    peer.fFrontierResponsePending = true;
    // The response was requested against a locator built from an earlier tip.
    peer.nFrontierLocatorHeight = nBestHeight - 1;
    // A full request window puts the peer at a zero budget.
    FillPeerActiveWindow(peer, 915000);

    {
        LOCK(cs_main);
        BOOST_CHECK(!TryAdmitBlockInvOrDefer(&peer, invFrontier, true));
    }
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(peer, hashFrontier), 0U);
    BOOST_CHECK(peer.IsBlockInvDeferred(hashFrontier));

    // The old locator context is invalidated; a fresh frontier response with
    // the current tip context is admitted.
    peer.nFrontierLocatorHeight = nBestHeight;
    {
        LOCK(cs_main);
        BOOST_CHECK(TryAdmitBlockInvOrDefer(&peer, invFrontier, true));
    }
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(peer, hashFrontier), 1U);
    peer.ClearAskFor();
}

BOOST_AUTO_TEST_CASE(frontier_admission_cannot_be_replayed_to_bypass_bound)
{
    BOOST_REQUIRE(pindexBest != NULL);
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CScopedOrphanCountByNode isolatedOrphanCounts;
    CScopedFrontierState isolatedFrontier;

    const uint256 hashFrontier(910031);
    const uint256 hashUnrelated(910032);
    const CInv invFrontier(MSG_BLOCK, hashFrontier);
    const CInv invUnrelated(MSG_BLOCK, hashUnrelated);

    CNode peer(INVALID_SOCKET, TestPeerAddress(65), "frontier-replay-peer", true);
    CScopedInitialBlockDownloadState ibdState(&peer);
    {
        LOCK(cs_main);
        mapOrphanCountByNode[peer.GetId()] = 200;
    }
    peer.fFrontierResponsePending = true;
    peer.nFrontierLocatorHeight = nBestHeight;
    // A full request window puts the peer at a zero budget.
    FillPeerActiveWindow(peer, 916000);

    // First announcement claims the single slot and is admitted.
    {
        LOCK(cs_main);
        BOOST_CHECK(TryAdmitBlockInvOrDefer(&peer, invFrontier, true));
    }
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(peer, hashFrontier), 1U);

    // A malicious peer re-sends the same response (frontier flag re-armed) and
    // the same candidate: the slot is already admitted, so no second grant.
    peer.fFrontierResponsePending = true;
    {
        LOCK(cs_main);
        BOOST_CHECK(!TryAdmitBlockInvOrDefer(&peer, invFrontier, true));
    }
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(peer, hashFrontier), 1U);

    // Only the first unknown block inv of a response is offered the slot; a
    // subsequent unrelated inv in the same response remains blocked.
    peer.fFrontierResponsePending = true;
    {
        LOCK(cs_main);
        BOOST_CHECK(!TryAdmitBlockInvOrDefer(&peer, invUnrelated, true));
    }
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(peer, hashUnrelated), 0U);
    BOOST_CHECK(peer.IsBlockInvDeferred(hashUnrelated));

    peer.ClearAskFor();
}

// --- Request budget semantics: orphan pressure decoupled -------------------
//
// The deferred request budget reflects request pressure only (queued +
// in-flight), never orphan storage pressure.  A peer that holds many orphans
// must still be able to request and download the missing chain.  Orphan
// storage is bounded separately by the hard storage caps on the receive path
// (PeerOrphanStorageLimitExceeded / MAX_ORPHAN_BLOCKS_PER_PEER).

BOOST_AUTO_TEST_CASE(request_budget_ignores_orphan_storage_pressure)
{
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CScopedOrphanCountByNode isolatedOrphanCounts;
    CNode peer(INVALID_SOCKET, TestPeerAddress(70), "budget-no-orphan-pressure", true);
    CScopedInitialBlockDownloadState ibdState(&peer);

    // 750 stored orphans and zero queued/in-flight requests: the full request
    // budget remains available.  Orphan storage pressure is reported
    // separately but does not eat into the transport budget.
    {
        LOCK(cs_main);
        mapOrphanCountByNode[peer.GetId()] = MAX_ORPHAN_BLOCKS_PER_PEER;
        int nOrphan = 0, nPeer = -1, nGlobal = -1;
        BOOST_CHECK_EQUAL(
            GetDeferredBlockRequestBudget(&peer, &nOrphan, NULL, NULL,
                                          &nPeer, &nGlobal),
            (int)MAX_DEFERRED_INV_ACTIVE_PER_PEER);
        BOOST_CHECK_EQUAL(nOrphan, (int)MAX_ORPHAN_BLOCKS_PER_PEER);
        BOOST_CHECK_EQUAL(nPeer, 0);
        BOOST_CHECK_EQUAL(nGlobal, 0);
    }
    peer.ClearAskFor();
}

BOOST_AUTO_TEST_CASE(request_budget_zero_at_full_peer_request_window_regardless_of_orphans)
{
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CScopedOrphanCountByNode isolatedOrphanCounts;
    CNode peer(INVALID_SOCKET, TestPeerAddress(71), "budget-full-request-window", true);
    CScopedInitialBlockDownloadState ibdState(&peer);
    FillPeerActiveWindow(peer, 920000);

    // Queued + in-flight at the per-peer cap (128): budget is zero no matter
    // how many orphans the peer holds.
    {
        LOCK(cs_main);
        mapOrphanCountByNode[peer.GetId()] = MAX_ORPHAN_BLOCKS_PER_PEER;
        BOOST_CHECK_EQUAL(GetDeferredBlockRequestBudget(&peer), 0);
        BOOST_CHECK_EQUAL(peer.setAskForBlocks.size() + peer.setBlocksInFlight.size(),
                          (size_t)MAX_DEFERRED_INV_ACTIVE_PER_PEER);
    }
    peer.ClearAskFor();
}

BOOST_AUTO_TEST_CASE(request_budget_zero_at_global_request_cap)
{
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CScopedOrphanCountByNode isolatedOrphanCounts;
    CNode peerA(INVALID_SOCKET, TestPeerAddress(82), "global-cap-a", true);
    CNode peerB(INVALID_SOCKET, TestPeerAddress(83), "global-cap-b", true);
    CNode peerC(INVALID_SOCKET, TestPeerAddress(84), "global-cap-c", true);
    CNode peerD(INVALID_SOCKET, TestPeerAddress(85), "global-cap-d", true);
    CNode peerE(INVALID_SOCKET, TestPeerAddress(86), "global-cap-e", true);
    CScopedInitialBlockDownloadState ibdState(&peerA);
    FillPeerActiveWindow(peerA, 920100);
    FillPeerActiveWindow(peerB, 920200);
    FillPeerActiveWindow(peerC, 920300);
    FillPeerActiveWindow(peerD, 920400);
    {
        LOCK(cs_vNodes);
        vNodes.push_back(&peerB);
        vNodes.push_back(&peerC);
        vNodes.push_back(&peerD);
    }
    {
        LOCK(cs_main);
        // Four full peers (including the fixture-registered peerA) reach the
        // 512 global request cap; a peer with none of its own requests still
        // gets a zero global budget.
        BOOST_CHECK_EQUAL(GetDeferredBlockRequestBudget(&peerE), 0);
        int nPeer = -1, nGlobal = -1;
        GetDeferredBlockRequestBudget(&peerE, NULL, NULL, NULL,
                                      &nPeer, &nGlobal);
        BOOST_CHECK_EQUAL(nPeer, 0);
        BOOST_CHECK_EQUAL(nGlobal, (int)MAX_DEFERRED_INV_ACTIVE_GLOBAL);
    }
    {
        LOCK(cs_vNodes);
        vNodes.erase(std::remove(vNodes.begin(), vNodes.end(), &peerB),
                     vNodes.end());
        vNodes.erase(std::remove(vNodes.begin(), vNodes.end(), &peerC),
                     vNodes.end());
        vNodes.erase(std::remove(vNodes.begin(), vNodes.end(), &peerD),
                     vNodes.end());
    }
    peerA.ClearAskFor();
    peerB.ClearAskFor();
    peerC.ClearAskFor();
    peerD.ClearAskFor();
    peerE.ClearAskFor();
}

BOOST_AUTO_TEST_CASE(orphan_storage_cap_remains_enforced_independently_of_request_budget)
{
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CScopedOrphanCountByNode isolatedOrphanCounts;
    CNode peer(INVALID_SOCKET, TestPeerAddress(72), "orphan-storage-cap", true);
    CScopedInitialBlockDownloadState ibdState(&peer);

    {
        LOCK(cs_main);
        // The hard per-peer storage cap is still enforced: a peer may store
        // orphans up to MAX_ORPHAN_BLOCKS_PER_PEER but not past it.
        mapOrphanCountByNode[peer.GetId()] = MAX_ORPHAN_BLOCKS_PER_PEER - 1;
        int nCount = -1;
        BOOST_CHECK(!PeerOrphanStorageLimitExceeded(peer.GetId(), &nCount));
        BOOST_CHECK_EQUAL(nCount, (int)MAX_ORPHAN_BLOCKS_PER_PEER - 1);

        mapOrphanCountByNode[peer.GetId()] = MAX_ORPHAN_BLOCKS_PER_PEER;
        BOOST_CHECK(PeerOrphanStorageLimitExceeded(peer.GetId(), &nCount));
        BOOST_CHECK_EQUAL(nCount, (int)MAX_ORPHAN_BLOCKS_PER_PEER);

        mapOrphanCountByNode[peer.GetId()] = MAX_ORPHAN_BLOCKS_PER_PEER + 1;
        BOOST_CHECK(PeerOrphanStorageLimitExceeded(peer.GetId(), &nCount));
        BOOST_CHECK_EQUAL(nCount, (int)MAX_ORPHAN_BLOCKS_PER_PEER + 1);
        mapOrphanCountByNode.erase(peer.GetId());

        // Decoupling does not weaken the storage cap: at the storage cap the
        // request budget stays fully positive (storage and transport pressure
        // are independent dimensions).
        mapOrphanCountByNode[peer.GetId()] = MAX_ORPHAN_BLOCKS_PER_PEER;
        BOOST_CHECK(PeerOrphanStorageLimitExceeded(peer.GetId(), NULL));
        BOOST_CHECK_EQUAL(GetDeferredBlockRequestBudget(&peer),
                          (int)MAX_DEFERRED_INV_ACTIVE_PER_PEER);
    }
    peer.ClearAskFor();
}

BOOST_AUTO_TEST_CASE(getblocks_continuation_keeps_bounded_pipeline_under_orphan_pressure)
{
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CScopedOrphanCountByNode isolatedOrphanCounts;
    CNode peer(INVALID_SOCKET, TestPeerAddress(73), "bounded-pipeline-orphans", true);
    CScopedInitialBlockDownloadState ibdState(&peer);
    {
        LOCK(cs_main);
        mapOrphanCountByNode[peer.GetId()] = MAX_ORPHAN_BLOCKS_PER_PEER;
    }

    // A deferred backlog under heavy orphan pressure.  The getblocks
    // continuation (the chain-front candidate) sits at position 128, inside
    // the first full-window refill.
    for (size_t i = 0; i < MAX_DEFERRED_INV_ACTIVE_PER_PEER - 1; ++i)
        BOOST_REQUIRE(peer.DeferBlockInv(uint256(930000 + i)));
    const uint256 continuation(930200);
    BOOST_REQUIRE(peer.DeferBlockInv(continuation));
    const uint256 tail(930300);
    BOOST_REQUIRE(peer.DeferBlockInv(tail));

    {
        LOCK(cs_main);
        // Refill admits up to the full per-peer request window despite the
        // orphan pressure -- but never beyond it.
        size_t nAdmitted = RefillDeferredBlockRequests(&peer);
        BOOST_CHECK_EQUAL(nAdmitted,
                          (size_t)MAX_DEFERRED_INV_ACTIVE_PER_PEER);
        BOOST_CHECK_EQUAL(peer.setAskForBlocks.size(),
                          (size_t)MAX_DEFERRED_INV_ACTIVE_PER_PEER);
        // The continuation (chain front) was admitted within the bounded
        // pipeline; the tail stays deferred for a later pump.
        BOOST_CHECK_EQUAL(QueuedBlockAskForCount(peer, continuation), 1U);
        BOOST_CHECK_EQUAL(QueuedBlockAskForCount(peer, tail), 0U);
        BOOST_CHECK(peer.IsBlockInvDeferred(tail));
        BOOST_CHECK_EQUAL(GetDeferredBlockRequestBudget(&peer), 0);
    }
    peer.ClearAskFor();
}

BOOST_AUTO_TEST_CASE(malicious_unrelated_inv_traffic_cannot_exceed_active_request_caps)
{
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CScopedOrphanCountByNode isolatedOrphanCounts;
    CNode peer(INVALID_SOCKET, TestPeerAddress(74), "unrelated-inv-cap", true);
    CScopedInitialBlockDownloadState ibdState(&peer);
    FillPeerActiveWindow(peer, 931000);
    {
        LOCK(cs_main);
        mapOrphanCountByNode[peer.GetId()] = MAX_ORPHAN_BLOCKS_PER_PEER;
    }

    // A flood of unrelated announcements at a full request window is deferred,
    // never admitted: the active queued + in-flight set cannot exceed the
    // per-peer cap even for a peer at the orphan storage limit.
    size_t nAdmitted = 0;
    for (int i = 0; i < 256; ++i)
    {
        const CInv inv(MSG_BLOCK, uint256(932000 + i));
        {
            LOCK(cs_main);
            if (TryAdmitBlockInvOrDefer(&peer, inv, false))
                ++nAdmitted;
        }
    }
    BOOST_CHECK_EQUAL(nAdmitted, 0U);
    BOOST_CHECK_EQUAL(peer.setAskForBlocks.size() + peer.setBlocksInFlight.size(),
                      (size_t)MAX_DEFERRED_INV_ACTIVE_PER_PEER);
    BOOST_CHECK(peer.deferredBlockInv.size() <=
                (size_t)MAX_DEFERRED_BLOCK_INV_PER_PEER);
    peer.ClearAskFor();
}

BOOST_AUTO_TEST_CASE(terminal_stall_remains_fixed_after_request_window_drain)
{
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CScopedOrphanCountByNode isolatedOrphanCounts;
    CNode peer(INVALID_SOCKET, TestPeerAddress(75), "terminal-stall-regress", true);
    CScopedInitialBlockDownloadState ibdState(&peer);
    FillPeerActiveWindow(peer, 933000);
    {
        LOCK(cs_main);
        mapOrphanCountByNode[peer.GetId()] = MAX_ORPHAN_BLOCKS_PER_PEER;
    }
    const uint256 deferred(934000);
    {
        LOCK(cs_main);
        BOOST_CHECK(peer.DeferBlockInv(deferred));
        BOOST_CHECK_EQUAL(GetDeferredBlockRequestBudget(&peer), 0);
    }

    // The window drains by one slot: the budget goes positive and the deferred
    // item is admitted, so IBD progress resumes -- no permanent stall even
    // while the peer remains at the orphan storage cap.
    peer.EraseAskForEntry(peer.mapAskFor.begin());
    {
        LOCK(cs_main);
        BOOST_CHECK_EQUAL(GetDeferredBlockRequestBudget(&peer), 1);
        BOOST_CHECK_EQUAL(RefillDeferredBlockRequests(&peer), 1U);
    }
    BOOST_CHECK_EQUAL(peer.deferredBlockInv.size(), 0U);
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(peer, deferred), 1U);
    peer.ClearAskFor();
}

BOOST_AUTO_TEST_SUITE_END()
