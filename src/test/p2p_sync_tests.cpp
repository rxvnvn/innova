// Copyright (c) 2026 The Innova developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include <boost/test/unit_test.hpp>

#include "checkpoints.h"
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

} // namespace

BOOST_AUTO_TEST_SUITE(p2p_sync_tests)

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
    peer.mapAskFor.insert(std::make_pair(
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
    peer.mapAskFor.insert(std::make_pair(
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
    peer.mapAskFor.insert(std::make_pair(
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
    peer.mapAskFor.insert(std::make_pair(
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
    ownerPeer.mapAskFor.insert(std::make_pair(
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
        (*it)->mapAskFor.clear();
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
    peer.mapAskFor.clear();
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
    peer.mapAskFor.clear();
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
    {
        LOCK(cs_vNodes);
        vNodes.push_back(&owner);
    }
    BOOST_CHECK(!EraseAlreadyAskedForIfUnowned(inv, &other));
    {
        LOCK(cs_mapAlreadyAskedFor);
        BOOST_CHECK_EQUAL(mapAlreadyAskedFor.count(inv), 1U);
    }
    owner.mapAskFor.clear();
    BOOST_CHECK(EraseAlreadyAskedForIfUnowned(inv, &other));
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

BOOST_AUTO_TEST_SUITE_END()
