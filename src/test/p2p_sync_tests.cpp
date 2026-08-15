// Copyright (c) 2026 The Innova developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <algorithm>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#ifndef WIN32
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <boost/filesystem.hpp>
#include <boost/test/unit_test.hpp>
#include <boost/thread/barrier.hpp>
#include <boost/thread/thread.hpp>

#include "checkpoints.h"
#include "ibdactivepath.h"
#include "ibdefficiency.h"
#include "ibdforensic.h"
#include "innovarpc.h"
#include "kernel.h"
#include "main.h"
#include "net.h"
#include "protocol.h"
#include "txdb.h"
#include "util.h"
#include "version.h"

namespace {

static const int64_t TEST_TIME = 100000;

class CInspectableBlockLocator : public CBlockLocator
{
public:
    explicit CInspectableBlockLocator(const CBlockIndex* pindex)
        : CBlockLocator(pindex)
    {
    }

    bool Contains(const uint256& hash) const
    {
        return std::find(vHave.begin(), vHave.end(), hash) != vHave.end();
    }
};

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

static CSerializeData FramedMessage(const std::string& command, size_t payloadBytes = 0)
{
    CDataStream stream(SER_NETWORK, INIT_PROTO_VERSION);
    stream << CMessageHeader(command.c_str(), 0);
    if (payloadBytes)
    {
        std::vector<unsigned char> payload(payloadBytes, 0);
        stream.write((const char*)&payload[0], payload.size());
    }
    unsigned int size = stream.size() - CMessageHeader::HEADER_SIZE;
    memcpy((char*)&stream[CMessageHeader::MESSAGE_SIZE_OFFSET], &size, sizeof(size));
    uint256 hash = Hash(stream.begin() + CMessageHeader::HEADER_SIZE, stream.end());
    unsigned int checksum = 0;
    memcpy(&checksum, &hash, sizeof(checksum));
    memcpy((char*)&stream[CMessageHeader::CHECKSUM_OFFSET], &checksum, sizeof(checksum));
    CSerializeData out;
    stream.GetAndClear(out);
    return out;
}

static CSerializeData FramedPayload(const std::string& command,
                                      const CSerializeData& payload)
{
    CDataStream wire(SER_NETWORK, INIT_PROTO_VERSION);
    wire << CMessageHeader(command.c_str(), 0);
    if (!payload.empty())
        wire.write((const char*)&payload[0], payload.size());
    unsigned int size = payload.size();
    memcpy((char*)&wire[CMessageHeader::MESSAGE_SIZE_OFFSET], &size, sizeof(size));
    uint256 hash = Hash(payload.begin(), payload.end());
    unsigned int checksum = 0;
    memcpy(&checksum, &hash, sizeof(checksum));
    memcpy((char*)&wire[CMessageHeader::CHECKSUM_OFFSET], &checksum, sizeof(checksum));
    CSerializeData out;
    wire.GetAndClear(out);
    return out;
}

static uint256 DeliverBlock(CNode& node, unsigned int seed, bool requested)
{
    CBlock block;
    block.nTime = seed;
    block.hashPrevBlock = uint256(seed + 1000);
    const uint256 hash = block.GetHash();
    if (requested)
    {
        BOOST_CHECK(TryAssignBlockRequestOwner(hash, node.GetId(),
                                                BLOCKREQ_SOURCE_INV));
        node.MarkBlockInFlight(hash);
    }
    CDataStream payload(SER_NETWORK, INIT_PROTO_VERSION);
    payload << block;
    CSerializeData payloadBytes;
    payload.GetAndClear(payloadBytes);
    const CSerializeData wire = FramedPayload("block", payloadBytes);
    LOCK(node.cs_vRecvMsg);
    BOOST_CHECK(node.ReceiveMsgBytes((const char*)&wire[0], wire.size()));
    return hash;
}

static void DeliverFramed(CNode& node, const std::string& command, size_t payloadBytes = 0)
{
    CSerializeData bytes = FramedMessage(command, payloadBytes);
    LOCK(node.cs_vRecvMsg);
    BOOST_REQUIRE(node.ReceiveMsgBytes((const char*)&bytes[0], bytes.size()));
}

static void DeliverEmptyHeaders(CNode& node)
{
    CDataStream payload(SER_NETWORK, INIT_PROTO_VERSION);
    std::vector<CBlock> headers;
    payload << headers;
    CDataStream wire(SER_NETWORK, INIT_PROTO_VERSION);
    wire << CMessageHeader("headers", 0);
    wire.write((const char*)&payload[0], payload.size());
    unsigned int size = payload.size();
    memcpy((char*)&wire[CMessageHeader::MESSAGE_SIZE_OFFSET], &size, sizeof(size));
    uint256 hash = Hash(payload.begin(), payload.end());
    unsigned int checksum = 0;
    memcpy(&checksum, &hash, sizeof(checksum));
    memcpy((char*)&wire[CMessageHeader::CHECKSUM_OFFSET], &checksum, sizeof(checksum));
    LOCK(node.cs_vRecvMsg);
    BOOST_REQUIRE(node.ReceiveMsgBytes((const char*)&wire[0], wire.size()));
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

// Full orphan-storage snapshot: block table, by-prev index, per-peer bookkeeping
// and the proof-of-stake kernel marker set, restored on scope exit.
class CScopedOrphanStorage
{
private:
    std::map<uint256, CBlock*> savedBlocks;
    std::multimap<uint256, CBlock*> savedByPrev;
    std::map<uint256, NodeId> savedByNode;
    std::map<NodeId, int> savedCount;
    std::set<std::pair<COutPoint, unsigned int> > savedStakeSeen;

public:
    CScopedOrphanStorage()
    {
        LOCK(cs_main);
        savedBlocks = mapOrphanBlocks;
        savedByPrev = mapOrphanBlocksByPrev;
        savedByNode = mapOrphanBlocksByNode;
        savedCount = mapOrphanCountByNode;
        savedStakeSeen = setStakeSeenOrphan;
        mapOrphanBlocks.clear();
        mapOrphanBlocksByPrev.clear();
        mapOrphanBlocksByNode.clear();
        mapOrphanCountByNode.clear();
        setStakeSeenOrphan.clear();
    }

    ~CScopedOrphanStorage()
    {
        LOCK(cs_main);
        for (std::map<uint256, CBlock*>::iterator it = mapOrphanBlocks.begin();
             it != mapOrphanBlocks.end(); ++it)
            delete it->second;
        mapOrphanBlocks = savedBlocks;
        mapOrphanBlocksByPrev = savedByPrev;
        mapOrphanBlocksByNode = savedByNode;
        mapOrphanCountByNode = savedCount;
        setStakeSeenOrphan = savedStakeSeen;
    }
};

// Overrides -maxorphanblocks for the duration of the scope so PruneOrphanBlocks
// can be exercised with a tiny table.
class CScopedMaxOrphanBlocks
{
private:
    std::string strSaved;
    bool fHad;

public:
    explicit CScopedMaxOrphanBlocks(const std::string& strValue)
    {
        fHad = mapArgs.count("-maxorphanblocks") != 0;
        if (fHad)
            strSaved = mapArgs["-maxorphanblocks"];
        mapArgs["-maxorphanblocks"] = strValue;
    }

    ~CScopedMaxOrphanBlocks()
    {
        if (fHad)
            mapArgs["-maxorphanblocks"] = strSaved;
        else
            mapArgs.erase("-maxorphanblocks");
    }
};

// Builds a proof-of-stake block whose kernel identity is exactly (prevout,
// nTime), with a distinct block hash per seed.
static CBlock* MakePosOrphanBlock(
    const std::pair<COutPoint, unsigned int>& stake, unsigned int nSeed)
{
    CBlock* pblock = new CBlock();
    pblock->nTime = 1000 + nSeed;
    pblock->hashPrevBlock = uint256(nSeed);

    CTransaction coinstake;
    coinstake.nTime = stake.second;
    CTxIn in(stake.first);
    coinstake.vin.push_back(in);
    CTxOut emptyOut;
    emptyOut.SetEmpty();
    coinstake.vout.push_back(emptyOut);
    CTxOut valueOut;
    valueOut.nValue = 1;
    coinstake.vout.push_back(valueOut);

    pblock->vtx.push_back(CTransaction());
    pblock->vtx.push_back(coinstake);
    return pblock;
}

// Mirrors the orphan-store bookkeeping on the receive path (mapOrphanBlocks,
// mapOrphanBlocksByPrev, setStakeSeenOrphan, and optional per-peer counts).
static void RegisterPosOrphan(CBlock* pblock, NodeId peer = -1)
{
    const uint256 hash = pblock->GetHash();
    mapOrphanBlocks[hash] = pblock;
    mapOrphanBlocksByPrev.insert(std::make_pair(pblock->hashPrevBlock, pblock));
    if (pblock->IsProofOfStake())
        setStakeSeenOrphan.insert(pblock->GetProofOfStake());
    if (peer >= 0)
    {
        mapOrphanBlocksByNode[hash] = peer;
        mapOrphanCountByNode[peer]++;
    }
}

// Mirrors the duplicate-stake-orphan reject gate on the receive path
// (main.cpp, the setStakeSeenOrphan guard).
static bool WouldRejectDuplicateStakeOrphan(const CBlock* pblock)
{
    return pblock->IsProofOfStake() &&
           setStakeSeenOrphan.count(pblock->GetProofOfStake()) != 0 &&
           mapOrphanBlocksByPrev.count(pblock->GetHash()) == 0 &&
           !Checkpoints::WantedByPendingSyncCheckpoint(pblock->GetHash());
}

// Every stored orphan kernel must be marked and every marked kernel must be
// referenced by at least one stored orphan.
static void CheckOrphanStakeInvariant()
{
    for (std::map<uint256, CBlock*>::const_iterator it = mapOrphanBlocks.begin();
         it != mapOrphanBlocks.end(); ++it)
    {
        if (it->second->IsProofOfStake())
            BOOST_CHECK_EQUAL(setStakeSeenOrphan.count(it->second->GetProofOfStake()), 1U);
    }
    for (std::set<std::pair<COutPoint, unsigned int> >::const_iterator it =
             setStakeSeenOrphan.begin();
         it != setStakeSeenOrphan.end(); ++it)
    {
        bool fReferenced = false;
        for (std::map<uint256, CBlock*>::const_iterator mo = mapOrphanBlocks.begin();
             mo != mapOrphanBlocks.end(); ++mo)
        {
            if (mo->second->IsProofOfStake() &&
                mo->second->GetProofOfStake() == *it)
            {
                fReferenced = true;
                break;
            }
        }
        BOOST_CHECK_MESSAGE(fReferenced, "stale stake kernel with no orphan");
    }
}

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

// Scoped -ibddivfuture / -ibddivfrac configuration.  Reloads the lazy config
// cache and clears the diversification attribution ledger on both entry and
// exit so unit tests never leak state between cases.
class CScopedFutureSupplyDiversificationConfig
{
private:
    bool fEnabled;
    std::string strFracSaved;

public:
    explicit CScopedFutureSupplyDiversificationConfig(
        bool fOn, const std::string& strFraction = "")
        : fEnabled(fOn)
    {
        ResetFutureSupplyDiversificationConfigForTesting();
        ResetDiversifyDispatchLedgerForTesting();
        strFracSaved = "";
        if (mapArgs.count("-ibddivfrac"))
            strFracSaved = mapArgs["-ibddivfrac"];
        if (fEnabled)
            mapArgs["-ibddivfuture"] = "1";
        else
            mapArgs.erase("-ibddivfuture");
        if (strFraction.empty())
            mapArgs.erase("-ibddivfrac");
        else
            mapArgs["-ibddivfrac"] = strFraction;
        ResetFutureSupplyDiversificationConfigForTesting();
        BOOST_CHECK_EQUAL(IsFutureSupplyDiversificationEnabled(), fEnabled);
    }

    ~CScopedFutureSupplyDiversificationConfig()
    {
        mapArgs.erase("-ibddivfuture");
        if (strFracSaved.empty())
            mapArgs.erase("-ibddivfrac");
        else
            mapArgs["-ibddivfrac"] = strFracSaved;
        ResetFutureSupplyDiversificationConfigForTesting();
        ResetDiversifyDispatchLedgerForTesting();
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

// Mirrors the SendMessages flush: marks the single-flight cycle active for a
// peer and increments the aggregate outstanding gauge.  Test-only; production
// uses CNode::SetOutstandingGetBlocks plus GetBlocksOutstandingAdd.
static void MarkOutstandingForTest(CNode& peer,
                                   ibdmetrics::GetBlocksSource source)
{
    peer.SetOutstandingGetBlocks(source, NULL, uint256(0));
    ibdmetrics::GetBlocksOutstandingAdd(1);
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
        nOutstanding += (*it)->HasOutstandingGetBlocks() ? 1 : 0;
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

// ---- IBD peer-quality ranking helpers -------------------------------------

// Isolate the global quality ledgers (mapBlockAlternateAnnouncers and
// mapBlockLastTimeoutOwner) between test cases.
class CScopedIbdQualityState
{
public:
    CScopedIbdQualityState() { ResetIbdQualityStateForTesting(); }
    ~CScopedIbdQualityState() { ResetIbdQualityStateForTesting(); }
};

// Scoped membership of peers in vNodes so ChooseIbdBlockRequestTarget can find
// redirect candidates.  Saves/restores the pre-existing membership.
class CScopedVNodes
{
private:
    std::vector<CNode*> saved;

public:
    CScopedVNodes()
    {
        LOCK(cs_vNodes);
        saved = vNodes;
        vNodes.clear();
    }
    ~CScopedVNodes()
    {
        LOCK(cs_vNodes);
        vNodes = saved;
    }
    void Add(CNode* pnode)
    {
        LOCK(cs_vNodes);
        vNodes.push_back(pnode);
    }
};

// Deterministic eligible redirect lane.
static void PrepareIbdRedirectLane(CNode& peer)
{
    peer.nVersion = PROTOCOL_VERSION;
    peer.fSuccessfullyConnected = true;
    peer.fClient = false;
    peer.fOneShot = false;
    peer.fDisconnect = false;
}

static void SetIbdPeerPressure(CNode& peer, int64_t nPressure)
{
    peer.peerLiveActivePressure.store(nPressure, std::memory_order_relaxed);
}

// Number of completed outcomes needed to cross the scaled
// IBD_PEER_QUALITY_MIN_SCORE confidence threshold (8*16 = 128 scaled units;
// ~11-16 events with decay, comfortably above the threshold).
static const int QUALITY_EVENT_COUNT = 16;

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

BOOST_AUTO_TEST_CASE(orphan_prune_releases_stale_stake_kernel)
{
    CScopedOrphanStorage isolated;
    CScopedMaxOrphanBlocks scopedMax("1");

    const std::pair<COutPoint, unsigned int> stakeA(
        COutPoint(uint256(9001), 0), 1001);
    const std::pair<COutPoint, unsigned int> stakeB(
        COutPoint(uint256(9002), 0), 1002);
    CBlock* pA = MakePosOrphanBlock(stakeA, 1);
    CBlock* pB = MakePosOrphanBlock(stakeB, 2);
    RegisterPosOrphan(pA, 10);
    RegisterPosOrphan(pB, 10);
    BOOST_REQUIRE_EQUAL(mapOrphanBlocks.size(), 2U);
    BOOST_REQUIRE_EQUAL(setStakeSeenOrphan.size(), 2U);

    PruneOrphanBlocks();

    BOOST_CHECK_EQUAL(mapOrphanBlocks.size(), 1U);
    BOOST_CHECK_EQUAL(setStakeSeenOrphan.size(), 1U);
    CheckOrphanStakeInvariant();
}

BOOST_AUTO_TEST_CASE(orphan_prune_decrements_per_peer_count)
{
    CScopedOrphanStorage isolated;
    CScopedMaxOrphanBlocks scopedMax("1");

    const std::pair<COutPoint, unsigned int> stakeA(
        COutPoint(uint256(9101), 0), 1101);
    const std::pair<COutPoint, unsigned int> stakeB(
        COutPoint(uint256(9102), 0), 1102);
    CBlock* pA = MakePosOrphanBlock(stakeA, 11);
    CBlock* pB = MakePosOrphanBlock(stakeB, 12);
    RegisterPosOrphan(pA, 100);
    RegisterPosOrphan(pB, 200);
    BOOST_REQUIRE_EQUAL(mapOrphanCountByNode[100], 1);
    BOOST_REQUIRE_EQUAL(mapOrphanCountByNode[200], 1);

    PruneOrphanBlocks();

    BOOST_CHECK_EQUAL(mapOrphanBlocks.size(), 1U);
    for (std::map<NodeId, int>::const_iterator it = mapOrphanCountByNode.begin();
         it != mapOrphanCountByNode.end(); ++it)
    {
        int nOwned = 0;
        for (std::map<uint256, CBlock*>::const_iterator mo = mapOrphanBlocks.begin();
             mo != mapOrphanBlocks.end(); ++mo)
        {
            std::map<uint256, NodeId>::const_iterator ni =
                mapOrphanBlocksByNode.find(mo->first);
            if (ni != mapOrphanBlocksByNode.end() && ni->second == it->first)
                ++nOwned;
        }
        BOOST_CHECK_EQUAL(it->second, nOwned);
    }
    CheckOrphanStakeInvariant();
}

BOOST_AUTO_TEST_CASE(orphan_connection_releases_stake_kernel)
{
    CScopedOrphanStorage isolated;

    const std::pair<COutPoint, unsigned int> stake(
        COutPoint(uint256(9201), 0), 1201);
    CBlock* pblock = MakePosOrphanBlock(stake, 21);
    RegisterPosOrphan(pblock);
    BOOST_REQUIRE_EQUAL(setStakeSeenOrphan.count(stake), 1U);

    // Parent connected: the orphan leaves the table and the parent-connect
    // loop releases the kernel through the shared cleanup helper.
    mapOrphanBlocks.erase(pblock->GetHash());
    EraseStakeSeenOrphanIfUnreferenced(stake);
    BOOST_CHECK_EQUAL(setStakeSeenOrphan.count(stake), 0U);
    BOOST_CHECK(setStakeSeenOrphan.empty());

    // A re-delivery of the same block no longer trips the
    // duplicate-stake-orphan reject gate.
    BOOST_CHECK(!WouldRejectDuplicateStakeOrphan(pblock));
    delete pblock;
}

BOOST_AUTO_TEST_CASE(pruned_orphan_redelivery_is_not_stale_kernel_rejected)
{
    CScopedOrphanStorage isolated;
    CScopedMaxOrphanBlocks scopedMax("1");

    const std::pair<COutPoint, unsigned int> stakeA(
        COutPoint(uint256(9301), 0), 1301);
    const std::pair<COutPoint, unsigned int> stakeB(
        COutPoint(uint256(9302), 0), 1302);
    CBlock* pA = MakePosOrphanBlock(stakeA, 31);
    CBlock* pB = MakePosOrphanBlock(stakeB, 32);
    const uint256 hashA = pA->GetHash();
    const uint256 hashB = pB->GetHash();
    RegisterPosOrphan(pA, 300);
    RegisterPosOrphan(pB, 300);

    PruneOrphanBlocks();
    BOOST_REQUIRE_EQUAL(mapOrphanBlocks.size(), 1U);

    // Identify which orphan survived and which was evicted.
    uint256 survivor = mapOrphanBlocks.begin()->first;
    const uint256 evictedHash = (survivor == hashA) ? hashB : hashA;
    const std::pair<COutPoint, unsigned int> evictedStake =
        (survivor == hashA) ? stakeB : stakeA;

    // The evicted orphan's kernel marker must be gone, so its re-delivery is
    // accepted again instead of being terminally rejected as a duplicate stake.
    BOOST_CHECK_EQUAL(setStakeSeenOrphan.count(evictedStake), 0U);
    const bool fWouldReject =
        setStakeSeenOrphan.count(evictedStake) != 0 &&
        mapOrphanBlocksByPrev.count(evictedHash) == 0 &&
        !Checkpoints::WantedByPendingSyncCheckpoint(evictedHash);
    BOOST_CHECK(!fWouldReject);
    CheckOrphanStakeInvariant();
}

BOOST_AUTO_TEST_CASE(shared_stake_kernel_survives_single_orphan_removal)
{
    CScopedOrphanStorage isolated;

    // Two distinct blocks sharing one stake identity (duplicate stake allowed
    // on the orphan path while an orphan child depends on the block).
    const std::pair<COutPoint, unsigned int> stake(
        COutPoint(uint256(9501), 0), 1501);
    CBlock* pOne = MakePosOrphanBlock(stake, 41);
    CBlock* pTwo = MakePosOrphanBlock(stake, 42);
    BOOST_REQUIRE(pOne->GetHash() != pTwo->GetHash());
    RegisterPosOrphan(pOne);
    RegisterPosOrphan(pTwo);
    BOOST_REQUIRE_EQUAL(setStakeSeenOrphan.size(), 1U);

    // Removing one orphan must not drop the kernel while the other still holds it.
    mapOrphanBlocks.erase(pOne->GetHash());
    EraseStakeSeenOrphanIfUnreferenced(stake);
    BOOST_CHECK_EQUAL(setStakeSeenOrphan.count(stake), 1U);

    // Removing the last reference drops the kernel.
    mapOrphanBlocks.erase(pTwo->GetHash());
    EraseStakeSeenOrphanIfUnreferenced(stake);
    BOOST_CHECK_EQUAL(setStakeSeenOrphan.count(stake), 0U);
    BOOST_CHECK(setStakeSeenOrphan.empty());

    delete pOne;
    delete pTwo;
}

BOOST_AUTO_TEST_CASE(orphan_prune_keeps_kernel_shared_by_survivor)
{
    CScopedOrphanStorage isolated;
    CScopedMaxOrphanBlocks scopedMax("1");

    const std::pair<COutPoint, unsigned int> stakeK(
        COutPoint(uint256(9601), 0), 1601);
    const std::pair<COutPoint, unsigned int> stakeU(
        COutPoint(uint256(9602), 0), 1602);
    CBlock* pK1 = MakePosOrphanBlock(stakeK, 51);
    CBlock* pK2 = MakePosOrphanBlock(stakeK, 52);
    CBlock* pU = MakePosOrphanBlock(stakeU, 53);
    RegisterPosOrphan(pK1);
    RegisterPosOrphan(pK2);
    RegisterPosOrphan(pU);
    BOOST_REQUIRE_EQUAL(setStakeSeenOrphan.size(), 2U);

    PruneOrphanBlocks();
    BOOST_CHECK_EQUAL(mapOrphanBlocks.size(), 2U);
    CheckOrphanStakeInvariant();
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

BOOST_AUTO_TEST_CASE(headers_continuation_allows_only_one_outstanding_per_peer)
{
    CNode peer(INVALID_SOCKET, TestPeerAddress(301), "headers-gate-one", true);
    CBlockLocator locator;
    BOOST_CHECK(peer.PushHeadersContinuation(locator, uint256(0), "test-continuation"));
    BOOST_CHECK(peer.HeadersContinuationPending());
    BOOST_CHECK(peer.getHeadersSync.IsInFlight());
    BOOST_CHECK(!peer.PushHeadersContinuation(locator, uint256(1), "test-continuation-duplicate"));
    BOOST_CHECK_EQUAL(peer.getHeadersSync.RequestSequence(), 1U);
}

BOOST_AUTO_TEST_CASE(headers_continuation_resumes_after_batch_processed)
{
    CNode peer(INVALID_SOCKET, TestPeerAddress(302), "headers-gate-resume", true);
    CBlockLocator locator;
    BOOST_REQUIRE(peer.PushHeadersContinuation(locator, uint256(0), "test-continuation"));
    RecordGetHeadersResponse(&peer, 2000, 164003);
    BOOST_CHECK(!peer.HeadersContinuationPending());
    BOOST_CHECK(peer.PushHeadersContinuation(locator, uint256(1), "test-continuation-next"));
    BOOST_CHECK_EQUAL(peer.getHeadersSync.RequestSequence(), 2U);
}

BOOST_AUTO_TEST_CASE(headers_backpressure_is_per_peer)
{
    CNode peerA(INVALID_SOCKET, TestPeerAddress(303), "headers-gate-a", true);
    CNode peerB(INVALID_SOCKET, TestPeerAddress(304), "headers-gate-b", true);
    CBlockLocator locator;
    BOOST_REQUIRE(peerA.PushHeadersContinuation(locator, uint256(0), "test-continuation-a"));
    BOOST_CHECK(!peerA.PushHeadersContinuation(locator, uint256(1), "test-continuation-a-duplicate"));
    BOOST_CHECK(peerB.PushHeadersContinuation(locator, uint256(2), "test-continuation-b"));
    BOOST_CHECK(peerA.HeadersContinuationPending());
    BOOST_CHECK(peerB.HeadersContinuationPending());
}

BOOST_AUTO_TEST_CASE(headers_backpressure_disconnect_clears_state)
{
    CNode peer(INVALID_SOCKET, TestPeerAddress(305), "headers-gate-disconnect", true);
    CBlockLocator locator;
    BOOST_REQUIRE(peer.PushHeadersContinuation(locator, uint256(0), "test-continuation"));
    BOOST_CHECK(peer.HeadersContinuationPending());
    peer.Cleanup(NODE_CLEANUP_FINAL_TEARDOWN);
    BOOST_CHECK(!peer.HeadersContinuationPending());
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

BOOST_AUTO_TEST_CASE(regtest_ibd_override_is_explicit_and_normal)
{
    BOOST_REQUIRE(pindexBest != NULL);
    const bool regSaved = fRegTest;
    const bool regIbdSaved = fRegTestIbd;
    const bool importingSaved = fImporting;
    const bool reindexSaved = fReindex;
    const int heightSaved = nBestHeight;
    std::vector<CNode*> nodesSaved;
    {
        LOCK(cs_vNodes);
        nodesSaved = vNodes;
        vNodes.clear();
    }

    CNode peer(INVALID_SOCKET, TestPeerAddress(56), "regtest-ibd-override", true);
    fRegTest = true;
    fImporting = false;
    fReindex = false;
    fRegTestIbd = false;
    nBestHeight = Checkpoints::GetTotalBlocksEstimate();
    peer.fClient = false;
    peer.nBestKnownHeight = nBestHeight + 100;
    peer.nChainHeight = nBestHeight + 100;
    peer.nLastHeightUpdate = GetTime();
    {
        LOCK(cs_vNodes);
        vNodes.push_back(&peer);
    }

    BOOST_CHECK(!IsInitialBlockDownload());

    fRegTestIbd = true;
    BOOST_CHECK(IsInitialBlockDownload());

    peer.nBestKnownHeight = nBestHeight;
    peer.nChainHeight = nBestHeight;
    peer.nLastHeightUpdate = GetTime();
    peer.setBlocksInFlight.clear();
    peer.mapBlockInFlightSince.clear();
    BOOST_CHECK(!IsInitialBlockDownload());

    {
        LOCK(cs_vNodes);
        vNodes = nodesSaved;
    }
    nBestHeight = heightSaved;
    fReindex = reindexSaved;
    fImporting = importingSaved;
    fRegTestIbd = regIbdSaved;
    fRegTest = regSaved;
}
BOOST_AUTO_TEST_CASE(ibd_peer_snapshot_unavailable_is_conservative)
{
    BOOST_REQUIRE(pindexBest != NULL);
    const bool regSaved = fRegTest;
    const bool testnetSaved = fTestNet;
    const bool importingSaved = fImporting;
    const bool reindexSaved = fReindex;
    const int heightSaved = nBestHeight;
    std::vector<CNode*> nodesSaved;
    { LOCK(cs_vNodes); nodesSaved = vNodes; vNodes.clear(); }

    CNode peer(INVALID_SOCKET, TestPeerAddress(59), "ibd-snapshot-unavailable", true);
    fRegTest = false;
    fTestNet = false;
    fImporting = false;
    fReindex = false;
    nBestHeight = Checkpoints::GetTotalBlocksEstimate();
    peer.fClient = false;
    peer.nBestKnownHeight = nBestHeight + 1000;
    peer.nChainHeight = peer.nBestKnownHeight;
    peer.nLastHeightUpdate = GetTime();
    { LOCK(cs_vNodes); vNodes.push_back(&peer); }

    g_forceIbdPeerSnapshotUnavailableForTesting = false;
    BOOST_CHECK(IsInitialBlockDownload());
    g_forceIbdPeerSnapshotUnavailableForTesting = true;
    BOOST_CHECK(IsInitialBlockDownload());
    BOOST_CHECK(IsInitialBlockDownload());
    g_forceIbdPeerSnapshotUnavailableForTesting = false;
    BOOST_CHECK(IsInitialBlockDownload());

    peer.nBestKnownHeight = nBestHeight;
    peer.nChainHeight = nBestHeight;
    peer.nLastHeightUpdate = GetTime();
    BOOST_CHECK(!IsInitialBlockDownload());
    g_forceIbdPeerSnapshotUnavailableForTesting = true;
    BOOST_CHECK(IsInitialBlockDownload());
    g_forceIbdPeerSnapshotUnavailableForTesting = false;
    BOOST_CHECK(!IsInitialBlockDownload());

    { LOCK(cs_vNodes); vNodes = nodesSaved; }
    nBestHeight = heightSaved;
    fReindex = reindexSaved;
    fImporting = importingSaved;
    fTestNet = testnetSaved;
    fRegTest = regSaved;
}

BOOST_AUTO_TEST_CASE(ibd_stale_height_with_continuing_blocks_is_conservative)
{
    BOOST_REQUIRE(pindexBest != NULL);
    const bool regSaved = fRegTest;
    const bool testnetSaved = fTestNet;
    const bool importingSaved = fImporting;
    const bool reindexSaved = fReindex;
    const int heightSaved = nBestHeight;
    std::vector<CNode*> nodesSaved;
    { LOCK(cs_vNodes); nodesSaved = vNodes; vNodes.clear(); }

    CNode peer(INVALID_SOCKET, TestPeerAddress(60), "ibd-stale-height", true);
    fRegTest = false;
    fTestNet = false;
    fImporting = false;
    fReindex = false;
    nBestHeight = Checkpoints::GetTotalBlocksEstimate();
    peer.fClient = false;
    peer.nBestKnownHeight = nBestHeight + 1000;
    peer.nChainHeight = peer.nBestKnownHeight;
    peer.nLastHeightUpdate = GetTime() - 121;
    peer.nLastBlockRecv = GetTime();
    { LOCK(cs_vNodes); vNodes.push_back(&peer); }

    BOOST_CHECK(IsInitialBlockDownload());

    nBestHeight = peer.nBestKnownHeight;
    BOOST_CHECK(!IsInitialBlockDownload());

    { LOCK(cs_vNodes); vNodes = nodesSaved; }
    nBestHeight = heightSaved;
    fReindex = reindexSaved;
    fImporting = importingSaved;
    fTestNet = testnetSaved;
    fRegTest = regSaved;
}



BOOST_AUTO_TEST_CASE(regtest_ibd_override_does_not_change_mainnet_or_testnet)
{
    BOOST_REQUIRE(pindexBest != NULL);
    const bool regSaved = fRegTest;
    const bool testnetSaved = fTestNet;
    const bool regIbdSaved = fRegTestIbd;
    const bool importingSaved = fImporting;
    const bool reindexSaved = fReindex;
    const int heightSaved = nBestHeight;
    std::vector<CNode*> nodesSaved;
    {
        LOCK(cs_vNodes);
        nodesSaved = vNodes;
        vNodes.clear();
    }

    CNode peer(INVALID_SOCKET, TestPeerAddress(57), "non-regtest-ibd-override", true);
    fImporting = false;
    fReindex = false;
    nBestHeight = Checkpoints::GetTotalBlocksEstimate();
    peer.fClient = false;
    peer.nBestKnownHeight = nBestHeight + 100;
    peer.nChainHeight = nBestHeight + 100;
    peer.nLastHeightUpdate = GetTime();
    {
        LOCK(cs_vNodes);
        vNodes.push_back(&peer);
    }

    fRegTest = false;
    fTestNet = false;
    fRegTestIbd = false;
    const bool mainnetWithoutOverride = IsInitialBlockDownload();
    fRegTestIbd = true;
    BOOST_CHECK_EQUAL(IsInitialBlockDownload(), mainnetWithoutOverride);

    fRegTest = false;
    fTestNet = true;
    fRegTestIbd = false;
    const bool testnetWithoutOverride = IsInitialBlockDownload();
    fRegTestIbd = true;
    BOOST_CHECK_EQUAL(IsInitialBlockDownload(), testnetWithoutOverride);

    {
        LOCK(cs_vNodes);
        vNodes = nodesSaved;
    }
    nBestHeight = heightSaved;
    fReindex = reindexSaved;
    fImporting = importingSaved;
    fRegTestIbd = regIbdSaved;
    fTestNet = testnetSaved;
    fRegTest = regSaved;
}

BOOST_AUTO_TEST_CASE(regtest_ibd_override_is_independent_from_headers_scheduler_flag)
{
    BOOST_REQUIRE(pindexBest != NULL);
    const bool regSaved = fRegTest;
    const bool regIbdSaved = fRegTestIbd;
    const bool schedulerSaved = fIbdHeaderScheduler;
    const bool observeSaved = fIbdHeadersObserve;
    const bool importingSaved = fImporting;
    const bool reindexSaved = fReindex;
    const bool spvSaved = fSPVMode;
    const int heightSaved = nBestHeight;
    std::vector<CNode*> nodesSaved;
    {
        LOCK(cs_vNodes);
        nodesSaved = vNodes;
        vNodes.clear();
    }

    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CNode peer(INVALID_SOCKET, TestPeerAddress(58), "regtest-scheduler-independence", true);
    PreparePeerForSendMessages(peer, PROTOCOL_VERSION);
    fRegTest = true;
    fRegTestIbd = false;
    fIbdHeaderScheduler = true;
    fIbdHeadersObserve = false;
    fImporting = false;
    fReindex = false;
    fSPVMode = false;
    nBestHeight = Checkpoints::GetTotalBlocksEstimate();
    peer.fClient = false;
    peer.fOneShot = false;
    peer.fDisconnect = false;
    peer.nBestKnownHeight = nBestHeight + 100;
    peer.nChainHeight = nBestHeight + 100;
    peer.nLastHeightUpdate = GetTime();
    {
        LOCK(cs_vNodes);
        vNodes.push_back(&peer);
    }

    BOOST_CHECK(!IsInitialBlockDownload());
    ResetIbdHeaderSchedulerStateForTesting();
    BOOST_REQUIRE(SeedIbdHeaderSchedulerAnchorForTesting(uint256(600), 1500));
    std::vector<std::pair<uint256, uint256> > headers;
    headers.push_back(std::make_pair(uint256(601), uint256(600)));
    BOOST_REQUIRE(SeedIbdHeaderSchedulerHeadersForTesting(peer.GetId(), headers));
    SeedIbdHeaderSchedulerInvAvailabilityForTesting(peer.GetId(), uint256(601));

    std::vector<CNode*> peers(1, &peer);
    BOOST_CHECK_EQUAL(RefillOrderedHeaderBlockRequestsForTesting(peers), 0U);
    BOOST_CHECK(peer.setAskForBlocks.empty());
    BOOST_CHECK(!HasCommand(SentCommands(peer), "getheaders"));

    peer.ClearAskFor();
    ResetIbdHeaderSchedulerStateForTesting();
    {
        LOCK(cs_vNodes);
        vNodes = nodesSaved;
    }
    nBestHeight = heightSaved;
    fSPVMode = spvSaved;
    fReindex = reindexSaved;
    fImporting = importingSaved;
    fIbdHeadersObserve = observeSaved;
    fIbdHeaderScheduler = schedulerSaved;
    fRegTestIbd = regIbdSaved;
    fRegTest = regSaved;
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

BOOST_AUTO_TEST_CASE(cooldown_expired_lookup_erases_without_alias_uaf)
{
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CNode peerA(INVALID_SOCKET, TestPeerAddress(66), "cooldown-expired-lookup-a", true);
    const CInv inv(MSG_BLOCK, uint256(3200));
    const int64_t nBase = GetTimeMicros();
    const int64_t nUntil = nBase + 5000;
    const int64_t nLookup = nUntil + 1;

    // A live, unexpired cooldown entry for this key.
    RecordOrphanLimitRejectedBlock(peerA.GetId(), inv, nUntil, uint256(3199));
    BOOST_CHECK(IsOrphanLimitRejectedBlockInCooldown(
        peerA.GetId(), inv, nUntil - 1, NULL));

    // Expired lookup must return false and erase every index for the key,
    // without reading the freed block-map node (alias-before-erase UAF).
    BOOST_CHECK(!IsOrphanLimitRejectedBlockInCooldown(
        peerA.GetId(), inv, nLookup, NULL));
    BOOST_CHECK_EQUAL(GetOrphanLimitRejectedEntryCount(), 0U);
    BOOST_CHECK_EQUAL(GetOrphanLimitRejectedEntryCountForPeer(peerA.GetId()), 0U);

    // Repeating the expired lookup stays stable: no leftover index entry and
    // no corruption on re-entry.
    BOOST_CHECK(!IsOrphanLimitRejectedBlockInCooldown(
        peerA.GetId(), inv, nLookup, NULL));
    BOOST_CHECK_EQUAL(GetOrphanLimitRejectedEntryCount(), 0U);
    BOOST_CHECK_EQUAL(GetOrphanLimitRejectedEntryCountForPeer(peerA.GetId()), 0U);

    // The expiry index must remain consistent: the same key can be recorded
    // and looked up again afterwards.
    const int64_t nFreshUntil = GetTimeMicros() + 60000;
    RecordOrphanLimitRejectedBlock(peerA.GetId(), inv, nFreshUntil, uint256(3199));
    BOOST_CHECK(IsOrphanLimitRejectedBlockInCooldown(
        peerA.GetId(), inv, GetTimeMicros(), NULL));
    BOOST_CHECK_EQUAL(GetOrphanLimitRejectedEntryCountForPeer(peerA.GetId()), 1U);

    // Flood up to the per-peer cap: exercises the expiry-index lower_bound
    // eviction path, so a stale index node would desynchronise the counts.
    for (size_t i = 0; i < MAX_ORPHAN_LIMIT_REJECTED_PER_PEER + 5; ++i)
        RecordOrphanLimitRejectedBlock(
            peerA.GetId(),
            CInv(MSG_BLOCK, uint256(3300 + i)),
            GetTimeMicros() + static_cast<int64_t>(i) * 1000,
            uint256(3199));
    BOOST_CHECK_EQUAL(GetOrphanLimitRejectedEntryCountForPeer(peerA.GetId()),
                      MAX_ORPHAN_LIMIT_REJECTED_PER_PEER);
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

BOOST_AUTO_TEST_CASE(acceptblock_reject_trace_enable_and_reason_mapping)
{
    BOOST_CHECK(InitAcceptBlockRejectTrace(false));
    BOOST_CHECK(!AcceptBlockRejectTraceEnabled());
    BOOST_CHECK(InitAcceptBlockRejectTrace(true));
    BOOST_CHECK(AcceptBlockRejectTraceEnabled());

    BOOST_CHECK_EQUAL(AcceptBlockRejectReasonName(ABREJECT_UNKNOWN_BLOCK_VERSION),
                      std::string("unknown-block-version"));
    BOOST_CHECK_EQUAL(AcceptBlockRejectReasonName(ABREJECT_DUPLICATE),
                      std::string("duplicate"));
    BOOST_CHECK_EQUAL(AcceptBlockRejectReasonName(ABREJECT_PREV_NOT_FOUND),
                      std::string("prev-not-found"));
    BOOST_CHECK_EQUAL(AcceptBlockRejectReasonName(ABREJECT_OPERATOR_INVALIDATED),
                      std::string("operator-invalidated"));
    BOOST_CHECK_EQUAL(AcceptBlockRejectReasonName(ABREJECT_PREV_OPERATOR_INVALIDATED),
                      std::string("prev-operator-invalidated"));
    BOOST_CHECK_EQUAL(AcceptBlockRejectReasonName(ABREJECT_POS_AFTER_DAG),
                      std::string("pos-after-dag"));
    BOOST_CHECK_EQUAL(AcceptBlockRejectReasonName(ABREJECT_BLOCK_SIZE),
                      std::string("block-size"));
    BOOST_CHECK_EQUAL(AcceptBlockRejectReasonName(ABREJECT_INCORRECT_BITS),
                      std::string("incorrect-bits"));
    BOOST_CHECK_EQUAL(AcceptBlockRejectReasonName(ABREJECT_TIMESTAMP_TOO_EARLY),
                      std::string("timestamp-too-early"));
    BOOST_CHECK_EQUAL(AcceptBlockRejectReasonName(ABREJECT_NON_FINAL_TX),
                      std::string("non-final-tx"));
    BOOST_CHECK_EQUAL(AcceptBlockRejectReasonName(ABREJECT_HARDENED_CHECKPOINT),
                      std::string("hardened-checkpoint"));
    BOOST_CHECK_EQUAL(AcceptBlockRejectReasonName(ABREJECT_CHECK_POS_FAILED),
                      std::string("check-pos-failed"));
    BOOST_CHECK_EQUAL(AcceptBlockRejectReasonName(ABREJECT_RINGSIG_DEPRECATION),
                      std::string("ringsig-deprecation"));
    BOOST_CHECK_EQUAL(AcceptBlockRejectReasonName(ABREJECT_SYNC_CHECKPOINT),
                      std::string("sync-checkpoint"));
    BOOST_CHECK_EQUAL(AcceptBlockRejectReasonName(ABREJECT_COINBASE_HEIGHT),
                      std::string("coinbase-height"));
    BOOST_CHECK_EQUAL(AcceptBlockRejectReasonName(ABREJECT_DAG_PARENT),
                      std::string("dag-parent"));
    BOOST_CHECK_EQUAL(AcceptBlockRejectReasonName(ABREJECT_DISK_SPACE),
                      std::string("disk-space"));
    BOOST_CHECK_EQUAL(AcceptBlockRejectReasonName(ABREJECT_WRITE_TO_DISK),
                      std::string("write-to-disk"));
    BOOST_CHECK_EQUAL(AcceptBlockRejectReasonName(ABREJECT_ADD_TO_BLOCK_INDEX),
                      std::string("add-to-block-index"));

    BOOST_CHECK_EQUAL(AcceptBlockRejectStageName(ABREJECT_UNKNOWN_BLOCK_VERSION),
                      std::string("version"));
    BOOST_CHECK_EQUAL(AcceptBlockRejectStageName(ABREJECT_DUPLICATE),
                      std::string("duplicate"));
    BOOST_CHECK_EQUAL(AcceptBlockRejectStageName(ABREJECT_PREV_NOT_FOUND),
                      std::string("prev"));
    BOOST_CHECK_EQUAL(AcceptBlockRejectStageName(ABREJECT_OPERATOR_INVALIDATED),
                      std::string("operator"));
    BOOST_CHECK_EQUAL(AcceptBlockRejectStageName(ABREJECT_INCORRECT_BITS),
                      std::string("consensus"));
    BOOST_CHECK_EQUAL(AcceptBlockRejectStageName(ABREJECT_TIMESTAMP_TOO_EARLY),
                      std::string("timestamp"));
    BOOST_CHECK_EQUAL(AcceptBlockRejectStageName(ABREJECT_CHECK_POS_FAILED),
                      std::string("pos"));
    BOOST_CHECK_EQUAL(AcceptBlockRejectStageName(ABREJECT_SYNC_CHECKPOINT),
                      std::string("checkpoint"));
    BOOST_CHECK_EQUAL(AcceptBlockRejectStageName(ABREJECT_COINBASE_HEIGHT),
                      std::string("coinbase"));
    BOOST_CHECK_EQUAL(AcceptBlockRejectStageName(ABREJECT_DAG_PARENT),
                      std::string("dag"));
    BOOST_CHECK_EQUAL(AcceptBlockRejectStageName(ABREJECT_WRITE_TO_DISK),
                      std::string("disk"));
    BOOST_CHECK_EQUAL(AcceptBlockRejectStageName(ABREJECT_ADD_TO_BLOCK_INDEX),
                      std::string("index"));

    BOOST_CHECK(InitAcceptBlockRejectTrace(false));
    BOOST_CHECK(!AcceptBlockRejectTraceEnabled());
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

// NODE_CLEANUP_FINAL_TEARDOWN (the CNetCleanup::~CNetCleanup static-destructor
// path) must still free scheduler state (ownership, outstanding getblocks) but
// must NOT invoke any forensic recorder: their static mutexes and containers
// may already be destroyed by the time the terminal destructor runs during
// exit().
BOOST_AUTO_TEST_CASE(terminal_cleanup_frees_state_without_forensic_records)
{
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;

    const boost::filesystem::path forensicsPath =
        boost::filesystem::temp_directory_path() /
        "p2p_terminal_cleanup_forensics.log";
    ibdforensic::SetEnabled(true, forensicsPath.string());
    ibdforensic::ResetForTesting();

    CNode peer(INVALID_SOCKET, TestPeerAddress(951), "terminal-cleanup", true);
    const uint256 hashA(9510);
    const uint256 hashB(9511);
    BOOST_CHECK(TryAssignBlockRequestOwner(hashA, peer.GetId(), BLOCKREQ_SOURCE_INV));
    BOOST_CHECK(TryAssignBlockRequestOwner(hashB, peer.GetId(), BLOCKREQ_SOURCE_INV));
    peer.MarkBlockInFlight(hashB);
    MarkOutstandingForTest(peer, ibdmetrics::GETBLOCKS_SOURCE_INITIAL);

    peer.Cleanup(NODE_CLEANUP_FINAL_TEARDOWN);

    // Ownership released and outstanding getblocks cleared.
    BOOST_CHECK(!GetBlockRequestOwner(hashA, NULL, NULL));
    BOOST_CHECK(!GetBlockRequestOwner(hashB, NULL, NULL));
    BOOST_CHECK(!peer.HasOutstandingGetBlocks());

    // The generation opened by MarkBlockInFlight stays OPEN: terminal cleanup
    // must not call RecordGenerationEnd.
    const std::map<uint256, std::vector<ibdforensic::GenerationRecord> > gens =
        ibdforensic::GenerationsForTesting();
    const std::map<uint256, std::vector<ibdforensic::GenerationRecord> >::const_iterator gi =
        gens.find(hashB);
    BOOST_REQUIRE(gi != gens.end());
    BOOST_REQUIRE(!gi->second.empty());
    BOOST_CHECK_EQUAL(gi->second[0].releaseUs, (int64_t)0);

    // Outstanding getblocks were cleared without feeding the forensic counter.
    const ibdforensic::GetBlocksRateCounters rate =
        ibdforensic::RateCounters();
    BOOST_CHECK_EQUAL(rate.outstandingNoResponse, (uint64_t)0);

    ibdforensic::SetEnabled(false, "");
}

// NODE_CLEANUP_RUNTIME (the normal disconnect path) keeps recording forensics:
// it closes the generation and counts dropped outstanding getblocks.
BOOST_AUTO_TEST_CASE(runtime_cleanup_still_records_forensics)
{
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;

    const boost::filesystem::path forensicsPath =
        boost::filesystem::temp_directory_path() /
        "p2p_runtime_cleanup_forensics.log";
    ibdforensic::SetEnabled(true, forensicsPath.string());
    ibdforensic::ResetForTesting();

    CNode peer(INVALID_SOCKET, TestPeerAddress(952), "runtime-cleanup", true);
    const uint256 hash(9520);
    BOOST_CHECK(TryAssignBlockRequestOwner(hash, peer.GetId(), BLOCKREQ_SOURCE_INV));
    peer.MarkBlockInFlight(hash);
    MarkOutstandingForTest(peer, ibdmetrics::GETBLOCKS_SOURCE_INITIAL);

    peer.Cleanup(NODE_CLEANUP_RUNTIME);

    BOOST_CHECK(!GetBlockRequestOwner(hash, NULL, NULL));
    BOOST_CHECK(!peer.HasOutstandingGetBlocks());

    // Runtime cleanup closes the generation (RecordGenerationEnd runs).
    const std::map<uint256, std::vector<ibdforensic::GenerationRecord> > gens =
        ibdforensic::GenerationsForTesting();
    const std::map<uint256, std::vector<ibdforensic::GenerationRecord> >::const_iterator gi =
        gens.find(hash);
    BOOST_REQUIRE(gi != gens.end());
    BOOST_REQUIRE(!gi->second.empty());
    BOOST_CHECK(gi->second[0].releaseUs != 0);

    // Outstanding getblocks were counted as no-response.
    const ibdforensic::GetBlocksRateCounters rate =
        ibdforensic::RateCounters();
    BOOST_CHECK_EQUAL(rate.outstandingNoResponse, (uint64_t)1);

    ibdforensic::SetEnabled(false, "");
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
    BOOST_REQUIRE(!peer.HasOutstandingGetBlocks());
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
    ibdmetrics::GetBlocksQueuedAdd(1, true);

    const int64_t nWakeSignalsBefore =
        MetricGet(ibdmetrics::Get().pipeline_wake_signals);
    const int64_t nDisconnectWakeBefore =
        MetricGet(ibdmetrics::Get().pipeline_wake_signal_disconnect_cleanup);
    uint64_t nRequestedBefore = 0, nHandledBefore = 0;
    GetPipelineWakeStateForTesting(&nRequestedBefore, &nHandledBefore, NULL,
                                   NULL);

    // Single-flight pending queue is coalesced to at most one entry.
    BOOST_REQUIRE_EQUAL(peer.getBlocksIndex.size(), 1U);
    BOOST_REQUIRE_EQUAL(peer.getBlocksHash.size(), 1U);
    BOOST_REQUIRE_EQUAL(peer.getBlocksSources.size(), 1U);
    BOOST_REQUIRE_EQUAL(peer.getBlocksRecoveryIds.size(), 1U);
    BOOST_CHECK_EQUAL(peer.setAskForBlocks.size(), 0U);
    BOOST_CHECK_EQUAL(peer.setBlocksInFlight.size(), 0U);
    BOOST_CHECK(!peer.HasOutstandingGetBlocks());
    BOOST_CHECK_EQUAL(
        MetricGet(ibdmetrics::Get().total_getblocks_queued_requests_current),
        1);
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
    BOOST_CHECK(!peer.HasOutstandingGetBlocks());
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
    BOOST_CHECK(!peer.HasOutstandingGetBlocks());
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

    MarkOutstandingForTest(peer, ibdmetrics::GETBLOCKS_SOURCE_INITIAL);
    const int64_t nGaugeBefore =
        MetricGet(ibdmetrics::Get().getblocks_outstanding_current);
    BOOST_REQUIRE_EQUAL(nGaugeBefore, 1);

    uint64_t nRequestedBefore = 0, nHandledBefore = 0;
    GetPipelineWakeStateForTesting(&nRequestedBefore, &nHandledBefore, NULL,
                                   NULL);

    BOOST_CHECK(peer.ConsumeGetBlocksResponse());
    BOOST_CHECK(!peer.HasOutstandingGetBlocks());
    BOOST_CHECK_EQUAL(
        MetricGet(ibdmetrics::Get().getblocks_outstanding_current),
        nGaugeBefore - 1);

    uint32_t nCauseBits = 0;
    uint64_t nRequested = 0, nHandled = 0;
    GetPipelineWakeStateForTesting(&nRequested, &nHandled, &nCauseBits, NULL);
    BOOST_CHECK(nCauseBits & WAKE_CAUSE_GETBLOCKS_OUTSTANDING_CLEARED);
    BOOST_CHECK_EQUAL(nRequested, nRequestedBefore + 1);
    BOOST_CHECK_EQUAL(nHandled, nHandledBefore);

    // A repeated call on the inactive slot is a pure no-op.
    const uint64_t nRequestedBeforeRepeat = nRequested;
    BOOST_CHECK(!peer.ConsumeGetBlocksResponse());
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

    // Single-flight: the outstanding slot is a single boolean cycle, so one
    // cleanup clears exactly one outstanding gauge count and one no-response
    // record.
    MarkOutstandingForTest(peer, ibdmetrics::GETBLOCKS_SOURCE_INITIAL);
    const int64_t nGaugeBefore =
        MetricGet(ibdmetrics::Get().getblocks_outstanding_current);
    BOOST_REQUIRE_EQUAL(nGaugeBefore, 1);
    const uint64_t nWakeBefore =
        MetricGet(ibdmetrics::Get().pipeline_wake_signals);
    const int64_t nNoResponseBefore =
        MetricGet(ibdmetrics::Get().getblocks_no_response_disconnect_cleanup);

    // No active work, only outstanding getblocks: the wake must be caused by
    // the outstanding-cleared + disconnect-cleanup pair alone.
    BOOST_REQUIRE(peer.setAskForBlocks.empty());
    BOOST_REQUIRE(peer.setBlocksInFlight.empty());
    peer.Cleanup();

    BOOST_CHECK(!peer.HasOutstandingGetBlocks());
    BOOST_CHECK_EQUAL(
        MetricGet(ibdmetrics::Get().getblocks_outstanding_current),
        nGaugeBefore - 1);
    BOOST_CHECK_EQUAL(
        MetricGet(ibdmetrics::Get().getblocks_no_response_disconnect_cleanup),
        nNoResponseBefore + 1);
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
    MarkOutstandingForTest(peerOutstanding, ibdmetrics::GETBLOCKS_SOURCE_INITIAL);
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
    BOOST_CHECK(peerOutstanding.ConsumeGetBlocksResponse());
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
    BOOST_CHECK(peer.HasOutstandingGetBlocks());
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
    peer.ConsumeGetBlocksResponse();
    SetPipelineWakeRequestedForTesting(1, 0, WAKE_CAUSE_OTHER);
    const PipelineWakeOutcome wakeOutcome2 = MaybeProcessPipelineWake(peers);
    BOOST_CHECK_EQUAL(wakeOutcome2, PIPELINE_WAKE_TERMINAL_GETBLOCKS_QUEUED);
    BOOST_CHECK_EQUAL(recoveryState.RecoveryAttempts(), 1U);
    BOOST_CHECK_EQUAL(recoveryState.LastRecoveryTime(), nNow + STALL_TIMEOUT + 1);
    CheckWakeGaugeBalance(peers);

    ClearQueuedGetBlocks(peer);
    ResetPeerWakeDedupState(peer);
    if (peer.HasOutstandingGetBlocks())
        peer.ConsumeGetBlocksResponse();
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
    BOOST_CHECK(!disc.HasOutstandingGetBlocks());
    BOOST_CHECK_EQUAL(disc.vSendMsg.size(), 0U);
    // The connected peers after it were still processed.
    BOOST_CHECK_EQUAL(connA.getBlocksIndex.size(), 0U);
    BOOST_CHECK(connA.HasOutstandingGetBlocks());
    BOOST_CHECK_EQUAL(connB.getBlocksIndex.size(), 0U);
    BOOST_CHECK(connB.HasOutstandingGetBlocks());

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
    BOOST_CHECK(!peer.HasOutstandingGetBlocks());
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

BOOST_AUTO_TEST_CASE(ibd_per_peer_cap_default_128)
{
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CScopedOrphanCountByNode isolatedOrphanCounts;
    ResetMaxActiveBlockRequestsPerPeerConfigForTesting();
    mapArgs.erase("-ibdmaxactiveperpeer");
    CNode peer(INVALID_SOCKET, TestPeerAddress(70), "per-peer-cap-default", true);
    CScopedInitialBlockDownloadState ibdState(&peer);

    // No argument: the effective per-peer IBD window is the default 128.
    BOOST_CHECK_EQUAL(GetMaxActiveBlockRequestsPerPeer(),
                      MAX_DEFERRED_INV_ACTIVE_PER_PEER);

    // 128 queued block requests fill the default window exactly: the deferred
    // admission budget is zero until the window drains.
    for (int i = 0; i < MAX_DEFERRED_INV_ACTIVE_PER_PEER; ++i)
        peer.AskFor(CInv(MSG_BLOCK, uint256(70000 + i)), BLOCKREQ_SOURCE_INV);
    BOOST_CHECK_EQUAL(peer.setAskForBlocks.size(),
                      (size_t)MAX_DEFERRED_INV_ACTIVE_PER_PEER);
    {
        LOCK(cs_main);
        BOOST_CHECK_EQUAL(GetDeferredBlockRequestBudget(&peer), 0);
    }
    peer.ClearAskFor();
    ResetMaxActiveBlockRequestsPerPeerConfigForTesting();
}

BOOST_AUTO_TEST_CASE(ibd_per_peer_cap_256)
{
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CScopedOrphanCountByNode isolatedOrphanCounts;
    ResetMaxActiveBlockRequestsPerPeerConfigForTesting();
    mapArgs["-ibdmaxactiveperpeer"] = "256";
    CNode peer(INVALID_SOCKET, TestPeerAddress(71), "per-peer-cap-256", true);
    PreparePeerForRecovery(peer, PROTOCOL_VERSION, nBestHeight + 100);
    CScopedInitialBlockDownloadState ibdState(&peer);
    BOOST_CHECK_EQUAL(GetMaxActiveBlockRequestsPerPeer(), 256);

    // 128 inflight does not block a 129th request when the window is 256.
    for (size_t i = 0; i < 128; ++i)
        peer.MarkBlockInFlight(uint256(71000 + i));
    const uint256 hashAllowed(71256);
    peer.AddAskForEntry((GetTime() - 10) * 1000000,
                        CInv(MSG_BLOCK, hashAllowed));
    BOOST_CHECK(SendMessages(&peer, true));
    BOOST_CHECK(HasCommand(SentCommands(peer), "getdata"));
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(peer, hashAllowed), 0U);

    // 256 inflight blocks the 257th request (hashAllowed is now in flight,
    // so 127 more marks bring the total to exactly 256).
    for (size_t i = 128; i < 255; ++i)
        peer.MarkBlockInFlight(uint256(71000 + i));
    BOOST_CHECK_EQUAL(peer.setBlocksInFlight.size(), 256U);
    const uint256 hashBlocked(71257);
    peer.AddAskForEntry((GetTime() - 10) * 1000000,
                        CInv(MSG_BLOCK, hashBlocked));
    BOOST_CHECK(SendMessages(&peer, true));
    // SentCommands is cumulative, so assert on the request staying queued:
    // a blocked 257th request is never moved out of mapAskFor.
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(peer, hashBlocked), 1U);

    peer.ClearAskFor();
    for (size_t i = 0; i < 255; ++i)
        peer.ClearBlockInFlight(uint256(71000 + i));
    mapArgs.erase("-ibdmaxactiveperpeer");
    ResetMaxActiveBlockRequestsPerPeerConfigForTesting();
}

BOOST_AUTO_TEST_CASE(ibd_per_peer_cap_respects_global_512)
{
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CScopedOrphanCountByNode isolatedOrphanCounts;
    ResetMaxActiveBlockRequestsPerPeerConfigForTesting();
    mapArgs["-ibdmaxactiveperpeer"] = "256";
    CNode peerA(INVALID_SOCKET, TestPeerAddress(80), "global-512-a", true);
    CNode peerB(INVALID_SOCKET, TestPeerAddress(81), "global-512-b", true);
    CNode peerC(INVALID_SOCKET, TestPeerAddress(82), "global-512-c", true);
    CScopedInitialBlockDownloadState ibdState(&peerA);
    {
        LOCK(cs_vNodes);
        vNodes.push_back(&peerB);
        vNodes.push_back(&peerC);
    }

    // Peer A: 200 active.  Per-peer cap 256 binds: 56 slots free.
    for (int i = 0; i < 200; ++i)
        peerA.AskFor(CInv(MSG_BLOCK, uint256(80000 + i)), BLOCKREQ_SOURCE_INV);
    {
        LOCK(cs_main);
        BOOST_CHECK_EQUAL(GetDeferredBlockRequestBudget(&peerA), 56);
    }

    // Peer B: global pressure 400, still below the 512 global cap.
    for (int i = 0; i < 200; ++i)
        peerB.AskFor(CInv(MSG_BLOCK, uint256(81000 + i)), BLOCKREQ_SOURCE_INV);
    {
        LOCK(cs_main);
        BOOST_CHECK_EQUAL(GetDeferredBlockRequestBudget(&peerB), 56);
    }

    // Peer C: total pressure reaches 600 > 512, so the global cap binds and
    // the budget is zero even though the per-peer cap alone would permit 56.
    for (int i = 0; i < 200; ++i)
        peerC.AskFor(CInv(MSG_BLOCK, uint256(82000 + i)), BLOCKREQ_SOURCE_INV);
    {
        LOCK(cs_main);
        BOOST_CHECK_EQUAL(GetDeferredBlockRequestBudget(&peerC), 0);
        BOOST_CHECK(GetMaxActiveBlockRequestsPerPeer() <=
                    MAX_DEFERRED_INV_ACTIVE_GLOBAL);
    }

    peerA.ClearAskFor();
    peerB.ClearAskFor();
    peerC.ClearAskFor();
    mapArgs.erase("-ibdmaxactiveperpeer");
    ResetMaxActiveBlockRequestsPerPeerConfigForTesting();
}

BOOST_AUTO_TEST_CASE(ibd_per_peer_cap_not_used_outside_ibd)
{
    ResetMaxActiveBlockRequestsPerPeerConfigForTesting();
    mapArgs["-ibdmaxactiveperpeer"] = "256";

    // Outside IBD the window is fixed at 128 even with -ibdmaxactiveperpeer=256.
    BOOST_CHECK_EQUAL(GetMaxActiveBlockRequestsPerPeer(),
                      MAX_DEFERRED_INV_ACTIVE_PER_PEER);

    // The getdata gate reverts to 128 outside IBD: 128 inflight blocks the
    // 129th request despite the configured 256.
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CScopedOrphanCountByNode isolatedOrphanCounts;
    CNode peer(INVALID_SOCKET, TestPeerAddress(72), "outside-ibd-128", true);
    PreparePeerForRecovery(peer, PROTOCOL_VERSION, nBestHeight + 10);
    for (size_t i = 0; i < MAX_DEFERRED_INV_ACTIVE_PER_PEER; ++i)
        peer.MarkBlockInFlight(uint256(72000 + i));
    BOOST_CHECK_EQUAL(peer.setBlocksInFlight.size(),
                      (size_t)MAX_DEFERRED_INV_ACTIVE_PER_PEER);
    const uint256 hashAsk(72129);
    peer.AddAskForEntry((GetTime() - 10) * 1000000, CInv(MSG_BLOCK, hashAsk));
    BOOST_CHECK(SendMessages(&peer, true));
    BOOST_CHECK(!HasCommand(SentCommands(peer), "getdata"));
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(peer, hashAsk), 1U);

    peer.ClearAskFor();
    for (size_t i = 0; i < MAX_DEFERRED_INV_ACTIVE_PER_PEER; ++i)
        peer.ClearBlockInFlight(uint256(72000 + i));
    mapArgs.erase("-ibdmaxactiveperpeer");
    ResetMaxActiveBlockRequestsPerPeerConfigForTesting();
}

BOOST_AUTO_TEST_CASE(ibd_per_peer_cap_invalid_values)
{
    ResetMaxActiveBlockRequestsPerPeerConfigForTesting();
    CNode peer(INVALID_SOCKET, TestPeerAddress(73), "invalid-values", true);
    CScopedInitialBlockDownloadState ibdState(&peer);

    mapArgs["-ibdmaxactiveperpeer"] = "abc";
    ResetMaxActiveBlockRequestsPerPeerConfigForTesting();
    BOOST_CHECK_EQUAL(GetMaxActiveBlockRequestsPerPeer(),
                      MAX_DEFERRED_INV_ACTIVE_PER_PEER);

    mapArgs["-ibdmaxactiveperpeer"] = "0";
    ResetMaxActiveBlockRequestsPerPeerConfigForTesting();
    BOOST_CHECK_EQUAL(GetMaxActiveBlockRequestsPerPeer(),
                      MAX_DEFERRED_INV_ACTIVE_PER_PEER);

    mapArgs["-ibdmaxactiveperpeer"] = "-5";
    ResetMaxActiveBlockRequestsPerPeerConfigForTesting();
    BOOST_CHECK_EQUAL(GetMaxActiveBlockRequestsPerPeer(),
                      MAX_DEFERRED_INV_ACTIVE_PER_PEER);

    mapArgs["-ibdmaxactiveperpeer"] = "600";
    ResetMaxActiveBlockRequestsPerPeerConfigForTesting();
    BOOST_CHECK_EQUAL(GetMaxActiveBlockRequestsPerPeer(),
                      MAX_DEFERRED_INV_ACTIVE_GLOBAL);

    mapArgs["-ibdmaxactiveperpeer"] = "1";
    ResetMaxActiveBlockRequestsPerPeerConfigForTesting();
    BOOST_CHECK_EQUAL(GetMaxActiveBlockRequestsPerPeer(), 1);

    mapArgs.erase("-ibdmaxactiveperpeer");
    ResetMaxActiveBlockRequestsPerPeerConfigForTesting();
}

BOOST_AUTO_TEST_CASE(inflight_distribution_metrics)
{
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CScopedOrphanCountByNode isolatedOrphanCounts;
    ResetMaxActiveBlockRequestsPerPeerConfigForTesting();
    mapArgs.erase("-ibdmaxactiveperpeer");
    ibdactivepath::InitIBDActivePathTrace(true);

    CNode peerA(INVALID_SOCKET, TestPeerAddress(90), "dist-a", true);
    CNode peerB(INVALID_SOCKET, TestPeerAddress(91), "dist-b", true);
    PreparePeerForRecovery(peerA, PROTOCOL_VERSION, nBestHeight + 10);
    PreparePeerForRecovery(peerB, PROTOCOL_VERSION, nBestHeight + 10);

    // Peer A: 100 inflight + 20 queued.  Peer B: 20 inflight, 0 queued.
    for (size_t i = 0; i < 100; ++i)
        peerA.MarkBlockInFlight(uint256(90000 + i));
    for (size_t i = 0; i < 20; ++i)
        peerB.MarkBlockInFlight(uint256(91000 + i));
    for (size_t i = 0; i < 20; ++i)
        peerA.AddAskForEntry((GetTime() - 10) * 1000000 + i,
                             CInv(MSG_BLOCK, uint256(90200 + i)));

    std::vector<CNode*> vPeers;
    vPeers.push_back(&peerA);
    vPeers.push_back(&peerB);
    ibdactivepath::EmitIBDActive1s(vPeers);

    ibdactivepath::ActivePathCounters& c = ibdactivepath::GetCounters();
    BOOST_CHECK_EQUAL(
        c.peers_inflight_gt0.load(std::memory_order_relaxed), 2);
    BOOST_CHECK_EQUAL(
        c.peers_queued_gt0.load(std::memory_order_relaxed), 1);
    BOOST_CHECK_EQUAL(
        c.inflight_peer_max.load(std::memory_order_relaxed), 100);
    BOOST_CHECK_EQUAL(
        c.queued_peer_max.load(std::memory_order_relaxed), 20);
    // Dominant share = 100 / (100+20) = 83%.
    BOOST_CHECK_EQUAL(
        c.dominant_peer_inflight_share_pct.load(std::memory_order_relaxed), 83);
    // Global free slots stay within [0, 512].
    BOOST_CHECK(c.global_free_active_slots.load(std::memory_order_relaxed) >= 0);
    BOOST_CHECK(c.global_free_active_slots.load(std::memory_order_relaxed) <=
                MAX_DEFERRED_INV_ACTIVE_GLOBAL);

    peerA.ClearAskFor();
    ibdactivepath::InitIBDActivePathTrace(false);
    for (size_t i = 0; i < 100; ++i)
        peerA.ClearBlockInFlight(uint256(90000 + i));
    for (size_t i = 0; i < 20; ++i)
        peerB.ClearBlockInFlight(uint256(91000 + i));
    ResetMaxActiveBlockRequestsPerPeerConfigForTesting();
}

BOOST_AUTO_TEST_CASE(single_peer_dominance_sample)
{
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    ResetMaxActiveBlockRequestsPerPeerConfigForTesting();
    ibdactivepath::InitIBDActivePathTrace(true);

    CNode peer(INVALID_SOCKET, TestPeerAddress(92), "dominance", true);
    PreparePeerForRecovery(peer, PROTOCOL_VERSION, nBestHeight + 10);
    for (size_t i = 0; i < 100; ++i)
        peer.MarkBlockInFlight(uint256(92000 + i));
    std::vector<CNode*> vPeers;
    vPeers.push_back(&peer);

    ibdactivepath::ActivePathCounters& c = ibdactivepath::GetCounters();
    const int64_t nBefore =
        c.samples_single_peer_over_75pct.load(std::memory_order_relaxed);

    // A single peer holding all inflight requests: share = 100%, sampled once.
    ibdactivepath::EmitIBDActive1s(vPeers);
    BOOST_CHECK_EQUAL(
        c.dominant_peer_inflight_share_pct.load(std::memory_order_relaxed), 100);
    BOOST_CHECK_EQUAL(
        c.samples_single_peer_over_75pct.load(std::memory_order_relaxed),
        nBefore + 1);

    // A second emission within the same second is suppressed by the 1s gate:
    // the sample counter increments at most once per emission.
    ibdactivepath::EmitIBDActive1s(vPeers);
    BOOST_CHECK_EQUAL(
        c.samples_single_peer_over_75pct.load(std::memory_order_relaxed),
        nBefore + 1);

    ibdactivepath::InitIBDActivePathTrace(false);
    for (size_t i = 0; i < 100; ++i)
        peer.ClearBlockInFlight(uint256(92000 + i));
    ResetMaxActiveBlockRequestsPerPeerConfigForTesting();
}

BOOST_AUTO_TEST_CASE(block_request_wire_latency_monotonic)
{
    ResetMaxActiveBlockRequestsPerPeerConfigForTesting();
    ibdactivepath::InitIBDActivePathTrace(true);

    ibdactivepath::ActivePathCounters& c = ibdactivepath::GetCounters();
    const int64_t nCountBefore =
        c.block_request_wire_latency_count.load(std::memory_order_relaxed);
    const int64_t nMaxBefore =
        c.block_request_wire_latency_us_max.load(std::memory_order_relaxed);

    const uint256 hashLat(93001);
    ibdactivepath::RecordBlockRequestEnqueued(hashLat);
    ibdactivepath::RecordBlockRequestSent(hashLat);

    BOOST_CHECK_EQUAL(
        c.block_request_wire_latency_count.load(std::memory_order_relaxed),
        nCountBefore + 1);
    BOOST_CHECK(c.block_request_wire_latency_us_max.load(
                    std::memory_order_relaxed) >= nMaxBefore);

    // Sending a hash that was never enqueued is a no-op.
    const int64_t nCountMid =
        c.block_request_wire_latency_count.load(std::memory_order_relaxed);
    ibdactivepath::RecordBlockRequestSent(uint256(93002));
    BOOST_CHECK_EQUAL(
        c.block_request_wire_latency_count.load(std::memory_order_relaxed),
        nCountMid);

    ibdactivepath::InitIBDActivePathTrace(false);
    ResetMaxActiveBlockRequestsPerPeerConfigForTesting();
}

static int64_t DiversifyMetric(const char* pszName)
{
    IBDMetricsSnapshot metrics;
    ibdmetrics::SnapshotAll(metrics);
    if (strcmp(pszName, "candidates") == 0)
        return metrics.diversify_candidates;
    if (strcmp(pszName, "other_lane") == 0)
        return metrics.diversify_picked_other_lane;
    if (strcmp(pszName, "announcer") == 0)
        return metrics.diversify_picked_announcer;
    if (strcmp(pszName, "snapshot_skip") == 0)
        return metrics.diversify_snapshot_skip_lock;
    if (strcmp(pszName, "no_other_lane") == 0)
        return metrics.diversify_no_other_lane;
    if (strcmp(pszName, "other_lane_timeout") == 0)
        return metrics.diversify_other_lane_timeout;
    return -1;
}

static void PrepareDiversifyLane(CNode& node, int nBase)
{
    PreparePeerForRecovery(node, PROTOCOL_VERSION, nBestHeight + 100);
    for (int i = 0; i < nBase; ++i)
        node.AddAskForEntry(
            (GetTime() - 100) * 1000000, CInv(MSG_BLOCK, uint256(70000 + i)));
}

BOOST_AUTO_TEST_CASE(diversify_config_parse_and_clamp)
{
    CScopedFutureSupplyDiversificationConfig cfg(true, "0.5");
    BOOST_CHECK_EQUAL(GetFutureSupplyDiversificationFractionPermille(), 500);

    // Non-numeric, negative, and above-1 fractions fall back to the 0.15
    // default (150 permille).
    mapArgs["-ibddivfrac"] = "abc";
    ResetFutureSupplyDiversificationConfigForTesting();
    BOOST_CHECK_EQUAL(GetFutureSupplyDiversificationFractionPermille(), 150);
    mapArgs["-ibddivfrac"] = "-0.2";
    ResetFutureSupplyDiversificationConfigForTesting();
    BOOST_CHECK_EQUAL(GetFutureSupplyDiversificationFractionPermille(), 150);
    mapArgs["-ibddivfrac"] = "2.0";
    ResetFutureSupplyDiversificationConfigForTesting();
    BOOST_CHECK_EQUAL(GetFutureSupplyDiversificationFractionPermille(), 150);

    mapArgs["-ibddivfrac"] = "0";
    ResetFutureSupplyDiversificationConfigForTesting();
    BOOST_CHECK_EQUAL(GetFutureSupplyDiversificationFractionPermille(), 0);
    mapArgs["-ibddivfrac"] = "1";
    ResetFutureSupplyDiversificationConfigForTesting();
    BOOST_CHECK_EQUAL(GetFutureSupplyDiversificationFractionPermille(), 1000);
}

BOOST_AUTO_TEST_CASE(atomic_live_pressure_mirror_tracks_all_six_loci)
{
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CNode peer(INVALID_SOCKET, TestPeerAddress(301), "mirror-loci", true);

    // Locus 1: AddAskForEntry insert +1.
    peer.AskFor(CInv(MSG_BLOCK, uint256(900000)), BLOCKREQ_SOURCE_INV);
    BOOST_CHECK_EQUAL(peer.peerLiveActivePressure.load(), 1);
    // Locus 2: EraseAskForEntry erase -1.
    peer.EraseAskForEntry(peer.mapAskFor.begin());
    BOOST_CHECK_EQUAL(peer.peerLiveActivePressure.load(), 0);
    // Queued + in-flight.
    peer.AskFor(CInv(MSG_BLOCK, uint256(900001)), BLOCKREQ_SOURCE_INV);
    peer.MarkBlockInFlight(uint256(900002));
    BOOST_CHECK_EQUAL(peer.peerLiveActivePressure.load(), 2);
    // Locus 5a: ClearBlockInFlight erase -1.
    peer.ClearBlockInFlight(uint256(900002));
    BOOST_CHECK_EQUAL(peer.peerLiveActivePressure.load(), 1);
    // Locus 3: ClearAskFor resets to zero.
    peer.ClearAskFor();
    BOOST_CHECK_EQUAL(peer.peerLiveActivePressure.load(), 0);
    // Locus 4: MarkBlockInFlight insert +1.
    peer.MarkBlockInFlight(uint256(900003));
    BOOST_CHECK_EQUAL(peer.peerLiveActivePressure.load(), 1);
    // Locus 5b: ExpireBlockInFlight erase -1.
    peer.mapBlockInFlightSince[uint256(900003)] = GetTime() - 10;
    peer.ExpireBlockInFlight();
    BOOST_CHECK_EQUAL(peer.peerLiveActivePressure.load(), 0);
}

BOOST_AUTO_TEST_CASE(diversify_routes_future_to_other_peer_with_capacity)
{
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CScopedOrphanCountByNode isolatedOrphanCounts;
    CScopedFutureSupplyDiversificationConfig cfg(true, "1.0");
    CNode pfrom(INVALID_SOCKET, TestPeerAddress(310), "div-announcer", true);
    CNode lane(INVALID_SOCKET, TestPeerAddress(311), "div-lane", true);
    CScopedInitialBlockDownloadState ibdState(&pfrom);
    PreparePeerForRecovery(pfrom, PROTOCOL_VERSION, nBestHeight + 100);
    PreparePeerForRecovery(lane, PROTOCOL_VERSION, nBestHeight + 100);
    pfrom.nLastHeightUpdate = GetTime();
    const uint256 hash(930000);

    {
        LOCK(cs_main);
        BOOST_REQUIRE(pfrom.DeferBlockInv(hash));
    }
    std::vector<CNode*> vNodesCopy;
    vNodesCopy.push_back(&pfrom);
    vNodesCopy.push_back(&lane);
    {
        LOCK(cs_main);
        BOOST_CHECK_EQUAL(RefillDeferredBlockRequests(&pfrom, vNodesCopy), 1U);
    }
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(pfrom, hash), 0U);
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(lane, hash), 1U);
    // No ownership is assigned at queue time.
    BOOST_CHECK(!GetBlockRequestOwner(hash, NULL, NULL));
    lane.ClearAskFor();
}

BOOST_AUTO_TEST_CASE(diversify_keeps_announcer_when_no_other_capacity)
{
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CScopedOrphanCountByNode isolatedOrphanCounts;
    CScopedFutureSupplyDiversificationConfig cfg(true, "1.0");
    CNode pfrom(INVALID_SOCKET, TestPeerAddress(312), "div-announcer-full-other", true);
    CNode lane(INVALID_SOCKET, TestPeerAddress(313), "div-lane-full", true);
    CScopedInitialBlockDownloadState ibdState(&pfrom);
    PreparePeerForRecovery(pfrom, PROTOCOL_VERSION, nBestHeight + 100);
    PreparePeerForRecovery(lane, PROTOCOL_VERSION, nBestHeight + 100);
    pfrom.nLastHeightUpdate = GetTime();
    FillPeerActiveWindow(lane, 931000);
    BOOST_CHECK_EQUAL(lane.peerLiveActivePressure.load(),
                      (int32_t)MAX_DEFERRED_INV_ACTIVE_PER_PEER);
    const uint256 hash(930001);
    {
        LOCK(cs_main);
        BOOST_REQUIRE(pfrom.DeferBlockInv(hash));
    }
    const int64_t nNoOtherBefore = DiversifyMetric("no_other_lane");
    std::vector<CNode*> vNodesCopy;
    vNodesCopy.push_back(&pfrom);
    vNodesCopy.push_back(&lane);
    {
        LOCK(cs_main);
        BOOST_CHECK_EQUAL(RefillDeferredBlockRequests(&pfrom, vNodesCopy), 1U);
    }
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(pfrom, hash), 1U);
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(lane, hash), 0U);
    BOOST_CHECK_EQUAL(DiversifyMetric("no_other_lane") - nNoOtherBefore, 1);
    lane.ClearAskFor();
    pfrom.ClearAskFor();
}

BOOST_AUTO_TEST_CASE(diversify_falls_back_to_announcer_without_snapshot)
{
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CScopedOrphanCountByNode isolatedOrphanCounts;
    CScopedFutureSupplyDiversificationConfig cfg(true, "1.0");
    CNode pfrom(INVALID_SOCKET, TestPeerAddress(314), "div-no-snapshot", true);
    CNode lane(INVALID_SOCKET, TestPeerAddress(315), "div-lane-no-snapshot", true);
    CScopedInitialBlockDownloadState ibdState(&pfrom);
    PreparePeerForRecovery(pfrom, PROTOCOL_VERSION, nBestHeight + 100);
    PreparePeerForRecovery(lane, PROTOCOL_VERSION, nBestHeight + 100);
    pfrom.nLastHeightUpdate = GetTime();
    const uint256 hash(930002);
    {
        LOCK(cs_main);
        BOOST_REQUIRE(pfrom.DeferBlockInv(hash));
    }
    const int64_t nSkipBefore = DiversifyMetric("snapshot_skip");
    std::vector<CNode*> vNodesEmpty;
    {
        LOCK(cs_main);
        BOOST_CHECK_EQUAL(
            RefillDeferredBlockRequests(&pfrom, vNodesEmpty), 1U);
    }
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(pfrom, hash), 1U);
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(lane, hash), 0U);
    BOOST_CHECK_EQUAL(DiversifyMetric("snapshot_skip") - nSkipBefore, 1);
    lane.ClearAskFor();
    pfrom.ClearAskFor();
}

BOOST_AUTO_TEST_CASE(diversify_bounded_by_frac_keeps_announcer_majority)
{
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CScopedOrphanCountByNode isolatedOrphanCounts;
    CScopedFutureSupplyDiversificationConfig cfg(true, "0");
    CNode pfrom(INVALID_SOCKET, TestPeerAddress(316), "div-frac-announcer", true);
    CNode lane(INVALID_SOCKET, TestPeerAddress(317), "div-frac-lane", true);
    CScopedInitialBlockDownloadState ibdState(&pfrom);
    PreparePeerForRecovery(pfrom, PROTOCOL_VERSION, nBestHeight + 100);
    PreparePeerForRecovery(lane, PROTOCOL_VERSION, nBestHeight + 100);
    pfrom.nLastHeightUpdate = GetTime();
    std::vector<CNode*> vNodesCopy;
    vNodesCopy.push_back(&pfrom);
    vNodesCopy.push_back(&lane);

    // frac = 0: never leave the announcer, even with an eligible other lane.
    std::vector<uint256> vHashes;
    for (int i = 0; i < 4; ++i)
        vHashes.push_back(uint256(930100 + i));
    {
        LOCK(cs_main);
        for (size_t i = 0; i < vHashes.size(); ++i)
            BOOST_REQUIRE(pfrom.DeferBlockInv(vHashes[i]));
        BOOST_CHECK_EQUAL(
            RefillDeferredBlockRequests(&pfrom, vNodesCopy),
            vHashes.size());
    }
    for (size_t i = 0; i < vHashes.size(); ++i)
    {
        BOOST_CHECK_EQUAL(QueuedBlockAskForCount(pfrom, vHashes[i]), 1U);
        BOOST_CHECK_EQUAL(QueuedBlockAskForCount(lane, vHashes[i]), 0U);
    }
    pfrom.ClearAskFor();
    lane.ClearAskFor();

    // frac = 1.0: with capacity on the announcer, every candidate leaves.
    CScopedFutureSupplyDiversificationConfig cfgFull(true, "1.0");
    const uint256 hashFull(930200);
    {
        LOCK(cs_main);
        BOOST_REQUIRE(pfrom.DeferBlockInv(hashFull));
        BOOST_CHECK_EQUAL(
            RefillDeferredBlockRequests(&pfrom, vNodesCopy), 1U);
    }
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(pfrom, hashFull), 0U);
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(lane, hashFull), 1U);
    lane.ClearAskFor();
}

BOOST_AUTO_TEST_CASE(diversify_frontier_deferred_lane_unchanged)
{
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CScopedOrphanCountByNode isolatedOrphanCounts;
    CScopedFutureSupplyDiversificationConfig cfg(true, "1.0");
    CNode pfrom(INVALID_SOCKET, TestPeerAddress(318), "div-frontier", true);
    CNode lane(INVALID_SOCKET, TestPeerAddress(319), "div-frontier-lane", true);
    CScopedInitialBlockDownloadState ibdState(&pfrom);
    PreparePeerForRecovery(pfrom, PROTOCOL_VERSION, nBestHeight + 100);
    PreparePeerForRecovery(lane, PROTOCOL_VERSION, nBestHeight + 100);
    pfrom.nLastHeightUpdate = GetTime();
    const uint256 hash(930300);
    pfrom.nFrontierDeferredHash = hash;
    {
        LOCK(cs_main);
        BOOST_REQUIRE(pfrom.DeferBlockInv(hash));
    }
    std::vector<CNode*> vNodesCopy;
    vNodesCopy.push_back(&pfrom);
    vNodesCopy.push_back(&lane);
    {
        LOCK(cs_main);
        BOOST_CHECK_EQUAL(RefillDeferredBlockRequests(&pfrom, vNodesCopy), 1U);
    }
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(pfrom, hash), 1U);
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(lane, hash), 0U);
    BOOST_CHECK(pfrom.nFrontierDeferredHash == uint256(0));
    lane.ClearAskFor();
    pfrom.ClearAskFor();
}

BOOST_AUTO_TEST_CASE(diversify_never_steals_inflight_owner)
{
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CScopedOrphanCountByNode isolatedOrphanCounts;
    CScopedFutureSupplyDiversificationConfig cfg(true, "1.0");
    CNode pfrom(INVALID_SOCKET, TestPeerAddress(320), "div-no-steal", true);
    CNode lane(INVALID_SOCKET, TestPeerAddress(321), "div-no-steal-lane", true);
    CScopedInitialBlockDownloadState ibdState(&pfrom);
    PreparePeerForRecovery(pfrom, PROTOCOL_VERSION, nBestHeight + 100);
    PreparePeerForRecovery(lane, PROTOCOL_VERSION, nBestHeight + 100);
    const uint256 hash(930400);

    // The hash is in-flight-owned by pfrom. A diversified AskFor on the lane
    // is rejected at queue time, so no getdata can ever be sent for it and the
    // owner is untouched.
    BOOST_REQUIRE(
        TryAssignBlockRequestOwner(hash, pfrom.GetId(), BLOCKREQ_SOURCE_INV));
    pfrom.MarkBlockInFlight(hash);
    BOOST_CHECK_EQUAL(lane.AskFor(CInv(MSG_BLOCK, hash), BLOCKREQ_SOURCE_INV),
                      ASKFOR_OWNED_BY_OTHER);
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(lane, hash), 0U);
    BOOST_CHECK(SendMessages(&lane, true));
    BOOST_CHECK(!HasCommand(SentCommands(lane), "getdata"));
    NodeId ownerPeer = -1;
    BlockRequestOwnerState ownerState = BLOCK_REQUEST_OWNER_QUEUED;
    BOOST_CHECK(GetBlockRequestOwner(hash, &ownerPeer, &ownerState));
    BOOST_CHECK_EQUAL(ownerPeer, pfrom.GetId());
    BOOST_CHECK_EQUAL(ownerState, BLOCK_REQUEST_OWNER_IN_FLIGHT);

    // The refill path likewise refuses to re-dispatch an owned hash: pfrom
    // already holds it in-flight, so the redundant deferred entry is dropped
    // without any lane being asked.
    pfrom.nLastHeightUpdate = GetTime();
    {
        LOCK(cs_main);
        BOOST_REQUIRE(pfrom.DeferBlockInv(hash));
        std::vector<CNode*> vNodesCopy;
        vNodesCopy.push_back(&pfrom);
        vNodesCopy.push_back(&lane);
        BOOST_CHECK_EQUAL(RefillDeferredBlockRequests(&pfrom, vNodesCopy), 0U);
    }
    BOOST_CHECK(!pfrom.IsBlockInvDeferred(hash));
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(lane, hash), 0U);
    BOOST_CHECK(GetBlockRequestOwner(hash, &ownerPeer, &ownerState));
    BOOST_CHECK_EQUAL(ownerPeer, pfrom.GetId());
    BOOST_CHECK_EQUAL(ownerState, BLOCK_REQUEST_OWNER_IN_FLIGHT);
    lane.ClearAskFor();
    pfrom.ClearBlockInFlight(hash);
}

BOOST_AUTO_TEST_CASE(diversify_attribution_lifecycle_and_timeout)
{
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CScopedFutureSupplyDiversificationConfig cfg(true, "1.0");
    CNode lane(INVALID_SOCKET, TestPeerAddress(322), "div-attr", true);
    const uint256 hash(930500);

    // No record -> not attributed.
    NodeId nAnnounce = -1;
    BOOST_CHECK(!GetDiversifyAnnounce(hash, &nAnnounce));
    // Record -> attributed to the announcing peer.
    RecordDiversifyDispatch(hash, 77);
    BOOST_CHECK(GetDiversifyAnnounce(hash, &nAnnounce));
    BOOST_CHECK_EQUAL(nAnnounce, 77);
    // Consume -> record gone.
    BOOST_CHECK(TakeDiversifyAnnounce(hash, &nAnnounce));
    BOOST_CHECK_EQUAL(nAnnounce, 77);
    BOOST_CHECK(!TakeDiversifyAnnounce(hash, &nAnnounce));

    // A diversified in-flight request that expires is counted.
    RecordDiversifyDispatch(hash, 78);
    lane.MarkBlockInFlight(hash);
    lane.mapBlockInFlightSince[hash] = GetTime() - 10;
    // Wire-origin clock (see ibd-conservative-block-request-expiration): the
    // getdata was sent 61 s ago, so the 60 s wire-origin deadline has passed
    // and this expires as a genuine timeout, not a pending-wire release.
    {
        LOCK(lane.cs_vBlockInFlightWire);
        lane.mapBlockInFlightWireUs[hash] = GetTimeMicros() - 61000000;
    }
    const int64_t nTimeoutBefore = DiversifyMetric("other_lane_timeout");
    lane.ExpireBlockInFlight();
    BOOST_CHECK_EQUAL(DiversifyMetric("other_lane_timeout") - nTimeoutBefore, 1);
    // Consumption on expiry prevents the re-request from being mis-attributed.
    BOOST_CHECK(!GetDiversifyAnnounce(hash, &nAnnounce));
}

BOOST_AUTO_TEST_CASE(diversify_disconnected_ineligible_peer_not_chosen)
{
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CScopedOrphanCountByNode isolatedOrphanCounts;
    CScopedFutureSupplyDiversificationConfig cfg(true, "1.0");
    CNode pfrom(INVALID_SOCKET, TestPeerAddress(323), "div-ineligible", true);
    CNode disc(INVALID_SOCKET, TestPeerAddress(324), "div-disconnected", true);
    CNode client(INVALID_SOCKET, TestPeerAddress(325), "div-client", true);
    CNode behind(INVALID_SOCKET, TestPeerAddress(326), "div-behind", true);
    CScopedInitialBlockDownloadState ibdState(&pfrom);
    PreparePeerForRecovery(pfrom, PROTOCOL_VERSION, nBestHeight + 100);
    PreparePeerForRecovery(disc, PROTOCOL_VERSION, nBestHeight + 100);
    PreparePeerForRecovery(client, PROTOCOL_VERSION, nBestHeight + 100);
    PreparePeerForRecovery(behind, PROTOCOL_VERSION, nBestHeight - 10);
    pfrom.nLastHeightUpdate = GetTime();
    disc.fDisconnect = true;
    client.fClient = true;

    std::vector<CNode*> vNodesCopy;
    vNodesCopy.push_back(&pfrom);
    vNodesCopy.push_back(&disc);
    vNodesCopy.push_back(&client);
    vNodesCopy.push_back(&behind);

    const uint256 hash(930600);
    {
        LOCK(cs_main);
        BOOST_REQUIRE(pfrom.DeferBlockInv(hash));
        BOOST_CHECK_EQUAL(RefillDeferredBlockRequests(&pfrom, vNodesCopy), 1U);
    }
    // None of the ineligible peers may be asked; the announcer keeps it.
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(pfrom, hash), 1U);
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(disc, hash), 0U);
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(client, hash), 0U);
    BOOST_CHECK_EQUAL(QueuedBlockAskForCount(behind, hash), 0U);
    pfrom.ClearAskFor();
}

BOOST_AUTO_TEST_CASE(diversify_tie_break_lowest_pressure_and_round_robin)
{
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CScopedFutureSupplyDiversificationConfig cfg(true, "1.0");
    CNode pfrom(INVALID_SOCKET, TestPeerAddress(327), "div-tie-announcer", true);
    CNode laneA(INVALID_SOCKET, TestPeerAddress(328), "div-tie-a", true);
    CNode laneB(INVALID_SOCKET, TestPeerAddress(329), "div-tie-b", true);
    PreparePeerForRecovery(pfrom, PROTOCOL_VERSION, nBestHeight + 100);
    PreparePeerForRecovery(laneA, PROTOCOL_VERSION, nBestHeight + 100);
    PreparePeerForRecovery(laneB, PROTOCOL_VERSION, nBestHeight + 100);
    std::vector<CNode*> vNodesCopy;
    vNodesCopy.push_back(&pfrom);
    vNodesCopy.push_back(&laneA);
    vNodesCopy.push_back(&laneB);

    // laneB has lower pressure -> chosen.
    PrepareDiversifyLane(laneA, 5);
    PrepareDiversifyLane(laneB, 3);
    BOOST_CHECK_EQUAL(
        ChooseDeferredDispatchLane(&pfrom, uint256(930700), vNodesCopy),
        &laneB);
    // Equal pressure, equal seq -> first lane, then round-robin alternation.
    laneA.ClearAskFor();
    laneB.ClearAskFor();
    laneA.peerDiversifySeq = 0;
    laneB.peerDiversifySeq = 0;
    BOOST_CHECK_EQUAL(
        ChooseDeferredDispatchLane(&pfrom, uint256(930701), vNodesCopy),
        &laneA);
    BOOST_CHECK_EQUAL(laneA.peerDiversifySeq, 1);
    BOOST_CHECK_EQUAL(
        ChooseDeferredDispatchLane(&pfrom, uint256(930702), vNodesCopy),
        &laneB);
    BOOST_CHECK_EQUAL(laneB.peerDiversifySeq, 1);
    BOOST_CHECK_EQUAL(
        ChooseDeferredDispatchLane(&pfrom, uint256(930703), vNodesCopy),
        &laneA);
    laneA.ClearAskFor();
    laneB.ClearAskFor();
}

BOOST_AUTO_TEST_CASE(flags_off_preserves_baseline_behavior)
{
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CScopedOrphanCountByNode isolatedOrphanCounts;
    CScopedFutureSupplyDiversificationConfig cfg(false);
    CNode pfrom(INVALID_SOCKET, TestPeerAddress(330), "div-off-announcer", true);
    CNode lane(INVALID_SOCKET, TestPeerAddress(331), "div-off-lane", true);
    CScopedInitialBlockDownloadState ibdState(&pfrom);
    PreparePeerForRecovery(pfrom, PROTOCOL_VERSION, nBestHeight + 100);
    PreparePeerForRecovery(lane, PROTOCOL_VERSION, nBestHeight + 100);
    pfrom.nLastHeightUpdate = GetTime();
    std::vector<CNode*> vNodesCopy;
    vNodesCopy.push_back(&pfrom);
    vNodesCopy.push_back(&lane);

    const int64_t nCandidatesBefore = DiversifyMetric("candidates");
    const int64_t nOtherBefore = DiversifyMetric("other_lane");
    const int64_t nAnnouncerBefore = DiversifyMetric("announcer");
    std::vector<uint256> vHashes;
    for (int i = 0; i < 3; ++i)
        vHashes.push_back(uint256(930800 + i));
    {
        LOCK(cs_main);
        for (size_t i = 0; i < vHashes.size(); ++i)
            BOOST_REQUIRE(pfrom.DeferBlockInv(vHashes[i]));
        BOOST_CHECK_EQUAL(
            RefillDeferredBlockRequests(&pfrom, vNodesCopy),
            vHashes.size());
    }
    for (size_t i = 0; i < vHashes.size(); ++i)
    {
        BOOST_CHECK_EQUAL(QueuedBlockAskForCount(pfrom, vHashes[i]), 1U);
        BOOST_CHECK_EQUAL(QueuedBlockAskForCount(lane, vHashes[i]), 0U);
    }
    // No diversification decisions were made at all.
    BOOST_CHECK_EQUAL(DiversifyMetric("candidates") - nCandidatesBefore, 0);
    BOOST_CHECK_EQUAL(DiversifyMetric("other_lane") - nOtherBefore, 0);
    BOOST_CHECK_EQUAL(DiversifyMetric("announcer") - nAnnouncerBefore, 0);
    pfrom.ClearAskFor();
    lane.ClearAskFor();
}

// ---------------------------------------------------------------------------
// Single-flight getblocks lifecycle (tests A-F, I): one flushed outstanding
// request per peer, a coalesced pending slot bounded to one, a finite timeout,
// and deterministic wake re-arm.  The wire carries no request id, so "any inv
// closes the cycle" remains heuristic; the helper-level assertions below
// exercise the same state transitions the inv handler drives.
// ---------------------------------------------------------------------------

// A+B: the SendMessages flush emits at most one getblocks per pass and only
// when no cycle is active, and PushGetBlocks keeps the pending slot bounded to
// a single most-meaningful request (drop on equal priority, replace on higher
// priority, coalesce on equivalence).
BOOST_AUTO_TEST_CASE(single_flight_flush_and_pending_coalescing)
{
    const bool fSPVModeSaved = fSPVMode;
    fSPVMode = false;
    CNode peer(INVALID_SOCKET, TestPeerAddress(1200), "single-flight-peer", true);
    PreparePeerForRecovery(peer, PROTOCOL_VERSION, nBestHeight + 10);
    CScopedInitialBlockDownloadState ibdState(&peer);
    ResetPipelineWakeStateForTesting();
    ibdmetrics::ResetPipelineWakeMetricsForTesting();
    std::vector<CNode*> peers(1, &peer);

    const uint256 hashA(120001);
    const uint256 hashB(120002);
    const uint256 hashC(120003);

    // Equal-priority distinct pending requests: the second is dropped and the
    // pending slot never grows beyond one.
    const int64_t nDropBefore =
        MetricGet(ibdmetrics::Get().getblocks_pending_drop);
    BOOST_CHECK(peer.PushGetBlocks(pindexBest, hashA,
                                   ibdmetrics::GETBLOCKS_SOURCE_PREFETCH));
    BOOST_CHECK(!peer.PushGetBlocks(pindexBest, hashB,
                                    ibdmetrics::GETBLOCKS_SOURCE_PREFETCH));
    BOOST_CHECK_EQUAL(peer.getBlocksIndex.size(), 1U);
    BOOST_CHECK_EQUAL(MetricGet(ibdmetrics::Get().getblocks_pending_drop),
                      nDropBefore + 1);

    // A higher-priority source supersedes the stale pending request (recovery
    // intent is never lost behind prefetch).
    const int64_t nReplaceBefore =
        MetricGet(ibdmetrics::Get().getblocks_pending_replaced);
    BOOST_CHECK(peer.PushGetBlocks(pindexBest, hashC,
                                   ibdmetrics::GETBLOCKS_SOURCE_RECOVERY));
    BOOST_CHECK_EQUAL(peer.getBlocksIndex.size(), 1U);
    BOOST_REQUIRE_EQUAL(peer.getBlocksSources.size(), 1U);
    BOOST_CHECK_EQUAL((int)peer.getBlocksSources[0],
                      (int)ibdmetrics::GETBLOCKS_SOURCE_RECOVERY);
    BOOST_CHECK(peer.getBlocksHash[0] == hashC);
    BOOST_CHECK_EQUAL(MetricGet(ibdmetrics::Get().getblocks_pending_replaced),
                      nReplaceBefore + 1);

    // An equivalent pending request coalesces in place rather than stacking.
    const int64_t nCoalesceBefore =
        MetricGet(ibdmetrics::Get().getblocks_pending_coalesce);
    peer.nLastGetBlocksTime = 0; // escape the identical-locator 5s dedup
    BOOST_CHECK(peer.PushGetBlocks(pindexBest, hashC,
                                   ibdmetrics::GETBLOCKS_SOURCE_RECOVERY));
    BOOST_CHECK_EQUAL(peer.getBlocksIndex.size(), 1U);
    BOOST_CHECK_EQUAL(MetricGet(ibdmetrics::Get().getblocks_pending_coalesce),
                      nCoalesceBefore + 1);

    // First SendMessages flushes exactly one getblocks and arms single-flight.
    BOOST_CHECK(SendMessages(&peer, true));
    // First SendMessages flushes exactly one getblocks and arms single-flight.
    BOOST_CHECK(SendMessages(&peer, true));
    {
        const std::vector<std::string> commands = SentCommands(peer);
        BOOST_CHECK_EQUAL(std::count(commands.begin(), commands.end(),
                                     "getblocks"),
                          1);
    }
    BOOST_CHECK(peer.HasOutstandingGetBlocks());
    BOOST_CHECK_EQUAL(
        MetricGet(ibdmetrics::Get().getblocks_outstanding_current), 1);
    BOOST_CHECK(peer.getBlocksIndex.empty());
    CheckWakeGaugeBalance(peers);

    // Second SendMessages with the cycle active sends nothing further.
    BOOST_CHECK(SendMessages(&peer, true));
    {
        const std::vector<std::string> commands = SentCommands(peer);
        BOOST_CHECK_EQUAL(std::count(commands.begin(), commands.end(),
                                     "getblocks"),
                          1);
    }

    // After the cycle closes, a fresh pending request flushes next pass.
    BOOST_CHECK(peer.ConsumeGetBlocksResponse());
    BOOST_CHECK(peer.PushGetBlocks(pindexBest, hashB,
                                   ibdmetrics::GETBLOCKS_SOURCE_CONTINUATION));
    BOOST_CHECK(SendMessages(&peer, true));
    {
        const std::vector<std::string> commands = SentCommands(peer);
        BOOST_CHECK_EQUAL(std::count(commands.begin(), commands.end(),
                                     "getblocks"),
                          2);
    }
    BOOST_CHECK(peer.HasOutstandingGetBlocks());

    peer.ConsumeGetBlocksResponse();
    ClearQueuedGetBlocks(peer);
    CheckWakeGaugeBalance(peers);
    ResetPipelineWakeStateForTesting();
    SetMockTime(0);
    fSPVMode = fSPVModeSaved;
}

// C: the finite outstanding timeout releases the slot, decrements the gauge
// exactly once, records a timeout, invalidates the per-peer dedup identity,
// clears the wake cooldown, and re-arms the wake.  The cycle is repeatable.
BOOST_AUTO_TEST_CASE(timeout_expires_slot_and_wakes_repeat_cycle)
{
    const int64_t nNow = WAKE_TEST_TIME;
    SetMockTime(nNow);
    ResetPipelineWakeStateForTesting();
    ibdmetrics::ResetPipelineWakeMetricsForTesting();

    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CNode peer(INVALID_SOCKET, TestPeerAddress(1201), "timeout-peer", true);
    std::vector<CNode*> peers(1, &peer);

    const int64_t nBaseUs = 1000000;
    for (int cycle = 0; cycle < 3; ++cycle)
    {
        MarkOutstandingForTest(peer, ibdmetrics::GETBLOCKS_SOURCE_INITIAL);
        peer.getBlocksOutstanding.sent_time_us = nBaseUs;
        const int64_t nGaugeBefore =
            MetricGet(ibdmetrics::Get().getblocks_outstanding_current);
        const int64_t nTimeoutBefore =
            MetricGet(ibdmetrics::Get().getblocks_outstanding_timeout);
        uint64_t nRequestedBefore = 0, nHandledBefore = 0;
        GetPipelineWakeStateForTesting(&nRequestedBefore, &nHandledBefore,
                                       NULL, NULL);

        // Just short of the timeout the slot stays reserved.
        BOOST_CHECK(!peer.ExpireGetBlocksOutstanding(
            nBaseUs + GETBLOCKS_RESPONSE_TIMEOUT * 1000000 - 1));
        BOOST_CHECK(peer.HasOutstandingGetBlocks());
        BOOST_CHECK_EQUAL(
            MetricGet(ibdmetrics::Get().getblocks_outstanding_timeout),
            nTimeoutBefore);

        // At/after the timeout the slot is released and accounted once.
        BOOST_CHECK(peer.ExpireGetBlocksOutstanding(
            nBaseUs + GETBLOCKS_RESPONSE_TIMEOUT * 1000000 + 1));
        BOOST_CHECK(!peer.HasOutstandingGetBlocks());
        BOOST_CHECK_EQUAL(
            MetricGet(ibdmetrics::Get().getblocks_outstanding_current),
            nGaugeBefore - 1);
        BOOST_CHECK_EQUAL(
            MetricGet(ibdmetrics::Get().getblocks_outstanding_timeout),
            nTimeoutBefore + 1);
        BOOST_CHECK(peer.pindexLastGetBlocksBegin == NULL);
        BOOST_CHECK(peer.hashLastGetBlocksEnd == uint256(0));
        BOOST_CHECK_EQUAL(peer.nLastGetBlocksTime, 0);
        uint32_t nCauseBits = 0;
        int64_t nLastGetBlocks = -1;
        GetPipelineWakeStateForTesting(&nRequestedBefore, &nHandledBefore,
                                       &nCauseBits, &nLastGetBlocks);
        BOOST_CHECK(nCauseBits & WAKE_CAUSE_GETBLOCKS_OUTSTANDING_CLEARED);
        BOOST_CHECK(nCauseBits & WAKE_CAUSE_GETBLOCKS_OUTSTANDING_TIMEOUT);
        BOOST_CHECK_EQUAL(nLastGetBlocks, 0);
        CheckWakeGaugeBalance(peers);
    }

    // Repeat expiry on an inactive slot is a pure no-op.
    const int64_t nTimeoutAfter =
        MetricGet(ibdmetrics::Get().getblocks_outstanding_timeout);
    BOOST_CHECK(!peer.ExpireGetBlocksOutstanding(
        nBaseUs + GETBLOCKS_RESPONSE_TIMEOUT * 1000000 + 2));
    BOOST_CHECK_EQUAL(
        MetricGet(ibdmetrics::Get().getblocks_outstanding_timeout),
        nTimeoutAfter);
    CheckWakeGaugeBalance(peers);

    ResetPipelineWakeStateForTesting();
    SetMockTime(0);
}

// D: a TCP-alive peer that never answers an inv is excluded from wake
// candidates while its cycle is active, then becomes eligible again the moment
// the outstanding timeout releases the slot.
BOOST_AUTO_TEST_CASE(no_response_live_peer_becomes_eligible_after_timeout)
{
    const int64_t nNow = WAKE_TEST_TIME;
    SetMockTime(nNow);
    ResetPipelineWakeStateForTesting();
    ibdmetrics::ResetPipelineWakeMetricsForTesting();

    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CNode peer(INVALID_SOCKET, TestPeerAddress(1202), "timeout-eligible", true);
    CScopedInitialBlockDownloadState ibdState(&peer);
    PrepareWakeEligiblePeer(peer, nBestHeight + 100, nNow);
    std::vector<CNode*> peers(1, &peer);

    // While the cycle is active the peer is not an eligible wake candidate.
    MarkOutstandingForTest(peer, ibdmetrics::GETBLOCKS_SOURCE_INITIAL);
    peer.getBlocksOutstanding.sent_time_us = nNow * 1000000;
    RequestBlockPipelineWake(WAKE_CAUSE_OTHER);
    BOOST_CHECK_EQUAL(MaybeProcessPipelineWake(peers),
                      PIPELINE_WAKE_TERMINAL_OUTSTANDING_GETBLOCKS_PRESENT);

    // After the timeout: slot free, dedup cleared, cooldown bypassed, and the
    // wake handler queues a fresh getblocks on the next pass.
    BOOST_CHECK(peer.ExpireGetBlocksOutstanding(
        nNow * 1000000 + GETBLOCKS_RESPONSE_TIMEOUT * 1000000 + 1));
    uint64_t nRequested = 0, nHandled = 0;
    GetPipelineWakeStateForTesting(&nRequested, &nHandled, NULL, NULL);
    SetPipelineWakeRequestedForTesting(
        nRequested + 1, nHandled, WAKE_CAUSE_GETBLOCKS_OUTSTANDING_TIMEOUT);
    const PipelineWakeOutcome outcome = MaybeProcessPipelineWake(peers);
    BOOST_CHECK_EQUAL(outcome, PIPELINE_WAKE_TERMINAL_GETBLOCKS_QUEUED);
    BOOST_CHECK_EQUAL(peer.getBlocksIndex.size(), 1U);

    ClearQueuedGetBlocks(peer);
    ResetPeerWakeDedupState(peer);
    CheckWakeGaugeBalance(peers);
    ResetPipelineWakeStateForTesting();
    SetMockTime(0);
}

// E: any inv message closes the active cycle.  The variants the inv handler
// can produce (block invs, known-only, empty, tx-only) all share the same
// close-slot semantics; the helper-level assertions below verify each closes
// exactly one slot and decrements the gauge exactly once, with a second
// consume on the same message being a no-op.
BOOST_AUTO_TEST_CASE(response_closes_slot_for_all_inv_variants)
{
    ResetPipelineWakeStateForTesting();
    ibdmetrics::ResetPipelineWakeMetricsForTesting();
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;

    for (int variant = 0; variant < 4; ++variant)
    {
        CNode peer(INVALID_SOCKET, TestPeerAddress(1203 + variant),
                   "response-variant", true);
        MarkOutstandingForTest(peer, ibdmetrics::GETBLOCKS_SOURCE_INITIAL);
        const int64_t nGaugeBefore =
            MetricGet(ibdmetrics::Get().getblocks_outstanding_current);
        BOOST_CHECK(peer.HasOutstandingGetBlocks());
        BOOST_CHECK(peer.ConsumeGetBlocksResponse());
        BOOST_CHECK(!peer.HasOutstandingGetBlocks());
        BOOST_CHECK_EQUAL(
            MetricGet(ibdmetrics::Get().getblocks_outstanding_current),
            nGaugeBefore - 1);
        // A second consume on the same inv message is a no-op (single cycle).
        BOOST_CHECK(!peer.ConsumeGetBlocksResponse());
        BOOST_CHECK_EQUAL(
            MetricGet(ibdmetrics::Get().getblocks_outstanding_current),
            nGaugeBefore - 1);
        CheckWakeGaugeBalance(std::vector<CNode*>(1, &peer));
        peer.Cleanup();
    }
    ResetPipelineWakeStateForTesting();
    SetMockTime(0);
}

// F: overlapping getblocks attempts while a cycle is active never accumulate;
// the pending slot stays bounded to one and the outstanding gauge stays at
// one.
BOOST_AUTO_TEST_CASE(overlapping_requests_never_accumulate)
{
    ResetPipelineWakeStateForTesting();
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CNode peer(INVALID_SOCKET, TestPeerAddress(1207), "no-accumulate", true);
    std::vector<CNode*> peers(1, &peer);

    MarkOutstandingForTest(peer, ibdmetrics::GETBLOCKS_SOURCE_INITIAL);
    const int64_t nGaugeBefore =
        MetricGet(ibdmetrics::Get().getblocks_outstanding_current);
    BOOST_REQUIRE_EQUAL(nGaugeBefore, 1);

    for (int i = 0; i < 1000; ++i)
    {
        peer.PushGetBlocks(pindexBest, uint256(600000 + i),
                           ibdmetrics::GETBLOCKS_SOURCE_CONTINUATION);
        BOOST_CHECK_LE(peer.getBlocksIndex.size(), 1U);
        BOOST_CHECK_EQUAL(
            MetricGet(ibdmetrics::Get().getblocks_outstanding_current), 1);
    }
    BOOST_CHECK_LE(peer.getBlocksIndex.size(), 1U);
    BOOST_CHECK_LE(peer.getBlocksHash.size(), 1U);
    BOOST_CHECK_LE(peer.getBlocksSources.size(), 1U);
    BOOST_CHECK_LE(peer.getBlocksRecoveryIds.size(), 1U);
    BOOST_CHECK(peer.HasOutstandingGetBlocks());

    // Recovery intent still supersedes within the bounded slot.
    BOOST_CHECK(peer.PushGetBlocks(pindexBest, uint256(605000),
                                   ibdmetrics::GETBLOCKS_SOURCE_RECOVERY));
    BOOST_CHECK_EQUAL(peer.getBlocksIndex.size(), 1U);

    ClearQueuedGetBlocks(peer);
    peer.ConsumeGetBlocksResponse();
    CheckWakeGaugeBalance(peers);
    ResetPipelineWakeStateForTesting();
    SetMockTime(0);
}

// G: disconnect cleanup of a mixed peer (both a pending request and an active
// cycle) releases both, decrements the gauge exactly once, records the
// no-response once, and signals the wake once without double accounting.
BOOST_AUTO_TEST_CASE(disconnect_cleanup_clears_pending_and_outstanding_once)
{
    const int64_t nNow = WAKE_TEST_TIME;
    SetMockTime(nNow);
    ResetPipelineWakeStateForTesting();
    ibdmetrics::ResetPipelineWakeMetricsForTesting();

    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CNode peer(INVALID_SOCKET, TestPeerAddress(1208), "cleanup-both", true);
    std::vector<CNode*> peers(1, &peer);

    BOOST_CHECK(peer.PushGetBlocks(pindexBest, uint256(120801),
                                   ibdmetrics::GETBLOCKS_SOURCE_INITIAL));
    MarkOutstandingForTest(peer, ibdmetrics::GETBLOCKS_SOURCE_INITIAL);
    BOOST_REQUIRE_EQUAL(peer.getBlocksIndex.size(), 1U);
    BOOST_REQUIRE_EQUAL(
        MetricGet(ibdmetrics::Get().getblocks_outstanding_current), 1);
    const int64_t nWakeBefore =
        MetricGet(ibdmetrics::Get().pipeline_wake_signals);
    const int64_t nNoResponseBefore =
        MetricGet(ibdmetrics::Get().getblocks_no_response_disconnect_cleanup);

    peer.Cleanup();

    BOOST_CHECK(peer.getBlocksIndex.empty());
    BOOST_CHECK(!peer.HasOutstandingGetBlocks());
    BOOST_CHECK_EQUAL(
        MetricGet(ibdmetrics::Get().getblocks_outstanding_current), 0);
    BOOST_CHECK_EQUAL(
        MetricGet(ibdmetrics::Get().getblocks_no_response_disconnect_cleanup),
        nNoResponseBefore + 1);
    BOOST_CHECK_EQUAL(MetricGet(ibdmetrics::Get().pipeline_wake_signals),
                      nWakeBefore + 1);

    // Repeated cleanup must not re-account.
    const int64_t nWakeAfter =
        MetricGet(ibdmetrics::Get().pipeline_wake_signals);
    peer.Cleanup();
    BOOST_CHECK_EQUAL(MetricGet(ibdmetrics::Get().pipeline_wake_signals),
                      nWakeAfter);
    CheckWakeGaugeBalance(peers);

    ResetPipelineWakeStateForTesting();
    SetMockTime(0);
}

// I: the global outstanding gauge always equals the number of peers with an
// active single-flight cycle, never exceeds the connected-peer count, and
// tracks transitions across consume and disconnect-cleanup.
BOOST_AUTO_TEST_CASE(outstanding_gauge_tracks_single_flight_across_peers)
{
    ResetPipelineWakeStateForTesting();
    ibdmetrics::ResetPipelineWakeMetricsForTesting();
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;

    const int nPeers = 8;
    std::vector<CNode*> peers;
    for (int i = 0; i < nPeers; ++i)
        peers.push_back(new CNode(
            INVALID_SOCKET, TestPeerAddress(1300 + i), "gauge-peer", true));

    // 1. No cycles active: gauge zero and balanced.
    CheckWakeGaugeBalance(peers);
    BOOST_CHECK_EQUAL(
        MetricGet(ibdmetrics::Get().getblocks_outstanding_current), 0);

    // 2. Half the peers hold an active cycle: gauge == live count.
    for (int i = 0; i < nPeers; i += 2)
        MarkOutstandingForTest(*peers[i], ibdmetrics::GETBLOCKS_SOURCE_INITIAL);
    BOOST_CHECK_EQUAL(
        MetricGet(ibdmetrics::Get().getblocks_outstanding_current),
        nPeers / 2);
    CheckWakeGaugeBalance(peers);

    // 3. Responses close cycles one at a time: the gauge tracks the live
    //    count and never exceeds the connected-peer count.
    for (int i = 0; i < nPeers; i += 2)
    {
        BOOST_CHECK(peers[i]->ConsumeGetBlocksResponse());
        const int64_t nLive =
            MetricGet(ibdmetrics::Get().getblocks_outstanding_current);
        BOOST_CHECK_EQUAL(nLive, (int64_t)TotalOutstandingGetBlocks(peers));
        BOOST_CHECK_LE(nLive, (int64_t)peers.size());
        CheckWakeGaugeBalance(peers);
    }
    BOOST_CHECK_EQUAL(
        MetricGet(ibdmetrics::Get().getblocks_outstanding_current), 0);

    // 4. Disconnect cleanup of the remaining cycles keeps the invariant.
    for (int i = 1; i < nPeers; i += 2)
        MarkOutstandingForTest(*peers[i], ibdmetrics::GETBLOCKS_SOURCE_INITIAL);
    BOOST_CHECK_EQUAL(
        MetricGet(ibdmetrics::Get().getblocks_outstanding_current),
        nPeers / 2);
    for (int i = 0; i < nPeers; ++i)
        peers[i]->Cleanup();
    BOOST_CHECK_EQUAL(
        MetricGet(ibdmetrics::Get().getblocks_outstanding_current), 0);
    CheckWakeGaugeBalance(peers);

    for (int i = 0; i < nPeers; ++i)
        delete peers[i];
    ResetPipelineWakeStateForTesting();
    SetMockTime(0);
}

// --- In-flight accounting reconciliation on peer cleanup ---------------------

// Mark -> Cleanup -> Expire: the expiry pass on the stale peer must not
// decrement the in-flight gauge a second time.  Cleanup clears the live set
// (accounted once) and the timestamp container, so a later Expire is a no-op.
BOOST_AUTO_TEST_CASE(cleanup_then_expire_does_not_double_decrement)
{
    CNode peer(INVALID_SOCKET, TestPeerAddress(1400), "cleanup-then-expire", true);
    const uint256 hash(1400001);
    const int64_t nGaugeBefore =
        MetricGet(ibdmetrics::Get().total_inflight_current);

    peer.MarkBlockInFlight(hash);
    BOOST_CHECK_EQUAL(
        MetricGet(ibdmetrics::Get().total_inflight_current), nGaugeBefore + 1);
    peer.mapBlockInFlightSince[hash] = GetTime() - 60;
    BOOST_CHECK_EQUAL(peer.peerLiveActivePressure.load(), 1);

    peer.Cleanup();
    BOOST_CHECK(peer.setBlocksInFlight.empty());
    BOOST_CHECK(peer.mapBlockInFlightSince.empty());
    BOOST_CHECK_EQUAL(
        MetricGet(ibdmetrics::Get().total_inflight_current), nGaugeBefore);
    BOOST_CHECK_EQUAL(peer.peerLiveActivePressure.load(), 0);

    peer.ExpireBlockInFlight();
    BOOST_CHECK_EQUAL(
        MetricGet(ibdmetrics::Get().total_inflight_current), nGaugeBefore);
    BOOST_CHECK_EQUAL(peer.peerLiveActivePressure.load(), 0);
    BOOST_CHECK(peer.mapBlockInFlightSince.empty());
}

// A stale timestamp entry (present in mapBlockInFlightSince but absent from
// the live in-flight set) is dropped by ExpireBlockInFlight without touching
// the gauge or peer pressure.
BOOST_AUTO_TEST_CASE(stale_map_entry_is_noop)
{
    CNode peer(INVALID_SOCKET, TestPeerAddress(1401), "stale-map-entry", true);
    const int64_t nGaugeBefore =
        MetricGet(ibdmetrics::Get().total_inflight_current);

    peer.mapBlockInFlightSince[uint256(1400002)] = GetTime() - 60;
    BOOST_CHECK(peer.setBlocksInFlight.empty());

    peer.ExpireBlockInFlight();
    BOOST_CHECK(peer.mapBlockInFlightSince.empty());
    BOOST_CHECK(peer.setBlocksInFlight.empty());
    BOOST_CHECK_EQUAL(
        MetricGet(ibdmetrics::Get().total_inflight_current), nGaugeBefore);
    BOOST_CHECK_EQUAL(peer.peerLiveActivePressure.load(), 0);
}

// ExpireBlockInFlight is idempotent: a second pass on an already-expired
// request changes no gauge and no peer pressure.
BOOST_AUTO_TEST_CASE(repeated_expire_is_idempotent)
{
    CNode peer(INVALID_SOCKET, TestPeerAddress(1402), "repeated-expire", true);
    const uint256 hash(1400003);
    const int64_t nGaugeBefore =
        MetricGet(ibdmetrics::Get().total_inflight_current);

    peer.MarkBlockInFlight(hash);
    peer.mapBlockInFlightSince[hash] = GetTime() - 60;
    BOOST_CHECK_EQUAL(peer.peerLiveActivePressure.load(), 1);

    peer.ExpireBlockInFlight();
    BOOST_CHECK_EQUAL(
        MetricGet(ibdmetrics::Get().total_inflight_current), nGaugeBefore);
    BOOST_CHECK_EQUAL(peer.peerLiveActivePressure.load(), 0);
    BOOST_CHECK(peer.setBlocksInFlight.empty());
    BOOST_CHECK(peer.mapBlockInFlightSince.empty());

    peer.ExpireBlockInFlight();
    BOOST_CHECK_EQUAL(
        MetricGet(ibdmetrics::Get().total_inflight_current), nGaugeBefore);
    BOOST_CHECK_EQUAL(peer.peerLiveActivePressure.load(), 0);
    BOOST_CHECK(peer.setBlocksInFlight.empty());
    BOOST_CHECK(peer.mapBlockInFlightSince.empty());
}

// Cleanup reconciles every in-flight container: the live set, the timestamp
// container, and the gauges all return to their pre-request values together.
BOOST_AUTO_TEST_CASE(cleanup_preserves_invariants)
{
    CNode peer(INVALID_SOCKET, TestPeerAddress(1403), "cleanup-invariants", true);
    const int64_t nGaugeBefore =
        MetricGet(ibdmetrics::Get().total_inflight_current);

    peer.MarkBlockInFlight(uint256(1400031));
    peer.MarkBlockInFlight(uint256(1400032));
    peer.AskFor(CInv(MSG_BLOCK, uint256(1400033)), BLOCKREQ_SOURCE_INV);
    BOOST_CHECK_EQUAL(peer.setBlocksInFlight.size(), 2U);
    BOOST_CHECK_EQUAL(peer.mapBlockInFlightSince.size(), 2U);
    BOOST_CHECK_EQUAL(peer.setAskForBlocks.size(), 1U);
    BOOST_CHECK_EQUAL(
        MetricGet(ibdmetrics::Get().total_inflight_current), nGaugeBefore + 2);
    BOOST_CHECK_EQUAL(peer.peerLiveActivePressure.load(), 3);

    peer.Cleanup();
    BOOST_CHECK(peer.setBlocksInFlight.empty());
    BOOST_CHECK(peer.mapBlockInFlightSince.empty());
    BOOST_CHECK(peer.setAskForBlocks.empty());
    BOOST_CHECK_EQUAL(
        MetricGet(ibdmetrics::Get().total_inflight_current), nGaugeBefore);
    BOOST_CHECK_EQUAL(peer.peerLiveActivePressure.load(), 0);

    // A subsequent expiry pass (as a stale snapshot would drive) stays a no-op.
    peer.ExpireBlockInFlight();
    BOOST_CHECK_EQUAL(
        MetricGet(ibdmetrics::Get().total_inflight_current), nGaugeBefore);
    BOOST_CHECK_EQUAL(peer.peerLiveActivePressure.load(), 0);
}

// ---- Timeout-aware IBD peer-quality ranking --------------------------------

// Cold-start neutrality: a fresh peer is UNKNOWN, not GOOD or DEGRADED.
BOOST_AUTO_TEST_CASE(ibd_quality_cold_start_is_unknown)
{
    CNode peer(INVALID_SOCKET, TestPeerAddress(21101), "quality-cold", true);
    const IbdPeerQualitySnapshot snap = peer.GetIbdQualitySnapshot();
    BOOST_CHECK_EQUAL((int)snap.tier, (int)IBD_PEER_QUALITY_UNKNOWN);
    BOOST_CHECK_EQUAL(snap.latency_ewma_us, 0);
    BOOST_CHECK_EQUAL(peer.ibdQuality.requests_issued.load(), 0u);
    // UNKNOWN must rank worse than GOOD so cold-start peers never displace a
    // proven-good supplier.
    BOOST_CHECK(IBD_PEER_QUALITY_GOOD < IBD_PEER_QUALITY_UNKNOWN);
}

// The EWMA seeds from the sample count, never from a sentinel latency value: a
// first sample of exactly 0 must still mark the peer as latency-measured and
// the second sample must blend in (not wholesale-replace the EWMA).
BOOST_AUTO_TEST_CASE(ibd_quality_ewma_zero_first_sample_seeds)
{
    CNode peer(INVALID_SOCKET, TestPeerAddress(21481), "ewma-zero", true);
    IbdPeerQualitySnapshot snap = peer.GetIbdQualitySnapshot();
    BOOST_CHECK(!snap.has_latency_sample);
    BOOST_CHECK_EQUAL(snap.latency_ewma_us, 0);

    peer.RecordIbdBlockDelivery(0, false);
    snap = peer.GetIbdQualitySnapshot();
    BOOST_CHECK(snap.has_latency_sample);
    BOOST_CHECK_EQUAL(snap.latency_ewma_us, 0);

    // Second sample blends: 0 - (0 >> 3) + (8000 >> 3) == 1000.  The old
    // sentinel behaviour would have replaced the EWMA with 8000 instead.
    peer.RecordIbdBlockDelivery(8000, false);
    snap = peer.GetIbdQualitySnapshot();
    BOOST_CHECK(snap.has_latency_sample);
    BOOST_CHECK_EQUAL(snap.latency_ewma_us, 1000);
}

// A steady receive stream promotes a peer to GOOD and fills the latency EWMA.
BOOST_AUTO_TEST_CASE(ibd_quality_receive_stream_promotes_to_good)
{
    CNode peer(INVALID_SOCKET, TestPeerAddress(21102), "quality-good", true);
    const int64_t nBefore =
        MetricGet(ibdmetrics::Get().peer_quality_receive_outcomes);
    for (int i = 0; i < QUALITY_EVENT_COUNT; ++i)
        peer.RecordIbdBlockDelivery(1000, false);
    const IbdPeerQualitySnapshot snap = peer.GetIbdQualitySnapshot();
    BOOST_CHECK_EQUAL((int)snap.tier, (int)IBD_PEER_QUALITY_GOOD);
    BOOST_CHECK_EQUAL(snap.latency_ewma_us, 1000);
    BOOST_CHECK_EQUAL(peer.ibdQuality.releases_by_receive.load(),
                      (uint64_t)QUALITY_EVENT_COUNT);
    BOOST_CHECK_EQUAL(peer.ibdQuality.releases_by_timeout.load(), 0u);
    BOOST_CHECK_EQUAL(
        MetricGet(ibdmetrics::Get().peer_quality_receive_outcomes),
        nBefore + QUALITY_EVENT_COUNT);
}

// A burst of timeouts degrades a peer.
BOOST_AUTO_TEST_CASE(ibd_quality_timeout_burst_degrades_peer)
{
    CNode peer(INVALID_SOCKET, TestPeerAddress(21103), "quality-degraded", true);
    for (int i = 0; i < QUALITY_EVENT_COUNT; ++i)
        peer.RecordIbdBlockTimeout();
    BOOST_CHECK_EQUAL((int)peer.GetIbdQualitySnapshot().tier,
                      (int)IBD_PEER_QUALITY_DEGRADED);
    BOOST_CHECK_EQUAL(peer.ibdQuality.releases_by_timeout.load(),
                      (uint64_t)QUALITY_EVENT_COUNT);
}

// Reputation recovers without a restart: a long enough receive stream fades
// the timeout history (both decayed scores decay on every outcome).
BOOST_AUTO_TEST_CASE(ibd_quality_recovers_without_restart)
{
    CNode peer(INVALID_SOCKET, TestPeerAddress(21104), "quality-recover", true);
    for (int i = 0; i < QUALITY_EVENT_COUNT; ++i)
        peer.RecordIbdBlockTimeout();
    BOOST_CHECK_EQUAL((int)peer.GetIbdQualitySnapshot().tier,
                      (int)IBD_PEER_QUALITY_DEGRADED);
    for (int i = 0; i < 4 * QUALITY_EVENT_COUNT; ++i)
        peer.RecordIbdBlockDelivery(1000, false);
    BOOST_CHECK_EQUAL((int)peer.GetIbdQualitySnapshot().tier,
                      (int)IBD_PEER_QUALITY_GOOD);
}

// The tier is a rate over the rolling window, not a raw count: a peer with a
// large majority of successes stays GOOD even with one timeout.
BOOST_AUTO_TEST_CASE(ibd_quality_mixed_outcomes_rate_behavior)
{
    // A timeout share above the 250 permille threshold degrades the peer even
    // though the raw receive count is higher.
    CNode mixed(INVALID_SOCKET, TestPeerAddress(21105), "quality-mixed-a", true);
    for (int i = 0; i < 15; ++i)
        mixed.RecordIbdBlockDelivery(1000, false);
    for (int i = 0; i < 5; ++i)
        mixed.RecordIbdBlockTimeout();
    BOOST_CHECK_EQUAL((int)mixed.GetIbdQualitySnapshot().tier,
                      (int)IBD_PEER_QUALITY_DEGRADED);

    // A peer with a large majority of successes stays GOOD despite a single
    // timeout.
    CNode good(INVALID_SOCKET, TestPeerAddress(21106), "quality-mixed-b", true);
    for (int i = 0; i < 40; ++i)
        good.RecordIbdBlockDelivery(1000, false);
    good.RecordIbdBlockTimeout();
    BOOST_CHECK_EQUAL((int)good.GetIbdQualitySnapshot().tier,
                      (int)IBD_PEER_QUALITY_GOOD);
}

// Alternate-announcer ledger: record / duplicate / read / gauge bookkeeping.
BOOST_AUTO_TEST_CASE(ibd_alternate_announcer_ledger_semantics)
{
    CScopedIbdQualityState scope;
    CNode peerA(INVALID_SOCKET, TestPeerAddress(21201), "alt-a", true);
    CNode peerB(INVALID_SOCKET, TestPeerAddress(21202), "alt-b", true);
    CNode peerC(INVALID_SOCKET, TestPeerAddress(21203), "alt-c", true);
    const uint256 hashA(212001);
    const uint256 hashB(212002);

    const int64_t nRecordedBefore =
        MetricGet(ibdmetrics::Get().alternate_announcer_recorded);
    BOOST_CHECK(RecordAlternateBlockAnnouncer(hashA, peerA.GetId()));
    BOOST_CHECK_EQUAL(CountBlockAlternateAnnouncerHashes(), 1U);
    BOOST_CHECK_EQUAL(MetricGet(ibdmetrics::Get().alternate_hashes_current), 1);
    BOOST_CHECK_EQUAL(
        MetricGet(ibdmetrics::Get().alternate_announcer_recorded),
        nRecordedBefore + 1);

    // Duplicate refresh does not grow the list.
    const int64_t nDupBefore =
        MetricGet(ibdmetrics::Get().alternate_announcer_duplicate);
    BOOST_CHECK(!RecordAlternateBlockAnnouncer(hashA, peerA.GetId()));
    BOOST_CHECK_EQUAL(
        MetricGet(ibdmetrics::Get().alternate_announcer_duplicate),
        nDupBefore + 1);
    BOOST_CHECK_EQUAL(CountBlockAlternateAnnouncerHashes(), 1U);

    // A second peer for the same hash is returned in read order.
    RecordAlternateBlockAnnouncer(hashA, peerB.GetId());
    std::vector<NodeId> announcers;
    GetBlockAlternateAnnouncers(hashA, announcers);
    BOOST_CHECK_EQUAL(announcers.size(), 2U);
    BOOST_CHECK(std::find(announcers.begin(), announcers.end(),
                          peerA.GetId()) != announcers.end());
    BOOST_CHECK(std::find(announcers.begin(), announcers.end(),
                          peerB.GetId()) != announcers.end());

    // A second hash grows the hash gauge.
    RecordAlternateBlockAnnouncer(hashB, peerC.GetId());
    BOOST_CHECK_EQUAL(CountBlockAlternateAnnouncerHashes(), 2U);
    BOOST_CHECK_EQUAL(MetricGet(ibdmetrics::Get().alternate_hashes_current), 2);
}

// The per-hash slot bound evicts the oldest entry deterministically.
BOOST_AUTO_TEST_CASE(ibd_alternate_announcer_per_hash_bound)
{
    CScopedIbdQualityState scope;
    CNode peerA(INVALID_SOCKET, TestPeerAddress(21211), "alt-bnd-a", true);
    CNode peerB(INVALID_SOCKET, TestPeerAddress(21212), "alt-bnd-b", true);
    CNode peerC(INVALID_SOCKET, TestPeerAddress(21213), "alt-bnd-c", true);
    CNode peerD(INVALID_SOCKET, TestPeerAddress(21214), "alt-bnd-d", true);
    CNode peerE(INVALID_SOCKET, TestPeerAddress(21215), "alt-bnd-e", true);
    CNode peerF(INVALID_SOCKET, TestPeerAddress(21216), "alt-bnd-f", true);
    CNode* peers[6] = {&peerA, &peerB, &peerC, &peerD, &peerE, &peerF};
    const uint256 hash(212101);
    const int64_t nEvictedBefore =
        MetricGet(ibdmetrics::Get().alternate_announcer_evicted);

    for (int i = 0; i < 4; ++i)
        RecordAlternateBlockAnnouncer(hash, peers[i]->GetId());
    std::vector<NodeId> announcers;
    GetBlockAlternateAnnouncers(hash, announcers);
    BOOST_CHECK_EQUAL(announcers.size(),
                      (size_t)IBD_ALTERNATE_ANNOUNCERS_PER_HASH);

    // A fifth distinct announcer evicts the oldest (peerA).
    RecordAlternateBlockAnnouncer(hash, peerE.GetId());
    GetBlockAlternateAnnouncers(hash, announcers);
    BOOST_CHECK_EQUAL(announcers.size(),
                      (size_t)IBD_ALTERNATE_ANNOUNCERS_PER_HASH);
    BOOST_CHECK_EQUAL(
        MetricGet(ibdmetrics::Get().alternate_announcer_evicted),
        nEvictedBefore + 1);
    BOOST_CHECK(std::find(announcers.begin(), announcers.end(),
                          peerA.GetId()) == announcers.end());
    BOOST_CHECK(std::find(announcers.begin(), announcers.end(),
                          peerE.GetId()) != announcers.end());
    BOOST_CHECK_EQUAL(MetricGet(ibdmetrics::Get().alternate_hashes_current), 1);
}

// The global hash bound keeps the ledger bounded across many hashes.
BOOST_AUTO_TEST_CASE(ibd_alternate_announcer_global_bound)
{
    CScopedIbdQualityState scope;
    CNode peer(INVALID_SOCKET, TestPeerAddress(21217), "alt-global", true);
    // Exceed IBD_ALTERNATE_ANNOUNCER_MAX_HASHES by a little.
    const size_t nExtra = 16;
    for (size_t i = 0; i < IBD_ALTERNATE_ANNOUNCER_MAX_HASHES + nExtra; ++i)
        RecordAlternateBlockAnnouncer(uint256(220000 + i), peer.GetId());
    BOOST_CHECK_LE(CountBlockAlternateAnnouncerHashes(),
                   (size_t)IBD_ALTERNATE_ANNOUNCER_MAX_HASHES);
    BOOST_CHECK_LE(MetricGet(ibdmetrics::Get().alternate_hashes_current),
                   (int64_t)IBD_ALTERNATE_ANNOUNCER_MAX_HASHES);
}

// TTL expiry frees hashes and restores the gauge.
BOOST_AUTO_TEST_CASE(ibd_alternate_announcer_ttl_expiry)
{
    CScopedIbdQualityState scope;
    CNode peerA(INVALID_SOCKET, TestPeerAddress(21221), "alt-ttl-a", true);
    CNode peerB(INVALID_SOCKET, TestPeerAddress(21222), "alt-ttl-b", true);
    const uint256 hashA(212201);
    const uint256 hashB(212202);

    SetIbdQualityClockForTesting(1000000);
    RecordAlternateBlockAnnouncer(hashA, peerA.GetId());
    RecordAlternateBlockAnnouncer(hashB, peerB.GetId());
    BOOST_CHECK_EQUAL(CountBlockAlternateAnnouncerHashes(), 2U);
    const int64_t nExpiredBefore =
        MetricGet(ibdmetrics::Get().alternate_announcer_expired);

    const int64_t nAfter =
        1000000 + IBD_ALTERNATE_ANNOUNCER_TTL_US + 1;
    SetIbdQualityClockForTesting(nAfter);
    ExpireBlockAlternateAnnouncersForTesting(nAfter);
    BOOST_CHECK_EQUAL(CountBlockAlternateAnnouncerHashes(), 0U);
    BOOST_CHECK_EQUAL(MetricGet(ibdmetrics::Get().alternate_hashes_current), 0);
    BOOST_CHECK_EQUAL(
        MetricGet(ibdmetrics::Get().alternate_announcer_expired),
        nExpiredBefore + 2);
    UnsetIbdQualityClockForTesting();
}

// Last-timeout-owner ledger: record / overwrite / query.
BOOST_AUTO_TEST_CASE(ibd_last_timeout_owner_ledger)
{
    CScopedIbdQualityState scope;
    CNode peerA(INVALID_SOCKET, TestPeerAddress(21231), "timeout-owner-a", true);
    CNode peerB(INVALID_SOCKET, TestPeerAddress(21232), "timeout-owner-b", true);
    const uint256 hashA(212301);
    const uint256 hashB(212302);

    RecordBlockLastTimeoutOwner(hashA, peerA.GetId());
    NodeId nOwner = -1;
    BOOST_CHECK(GetBlockLastTimeoutOwner(hashA, &nOwner));
    BOOST_CHECK_EQUAL(nOwner, peerA.GetId());
    BOOST_CHECK(WasBlockLastTimedOutByPeer(hashA, peerA.GetId()));
    BOOST_CHECK(!WasBlockLastTimedOutByPeer(hashA, peerB.GetId()));
    BOOST_CHECK(!GetBlockLastTimeoutOwner(hashB, &nOwner));

    // A newer timeout overwrites the owner for the same hash.
    RecordBlockLastTimeoutOwner(hashA, peerB.GetId());
    BOOST_CHECK(WasBlockLastTimedOutByPeer(hashA, peerB.GetId()));
    BOOST_CHECK(!WasBlockLastTimedOutByPeer(hashA, peerA.GetId()));

    ResetBlockLastTimeoutOwnerForTesting();
    BOOST_CHECK(!GetBlockLastTimeoutOwner(hashA, &nOwner));
}

// Disconnect cleanup removes a peer from both quality ledgers.
BOOST_AUTO_TEST_CASE(ibd_disconnect_cleans_up_quality_ledgers)
{
    CScopedIbdQualityState scope;
    CNode peerA(INVALID_SOCKET, TestPeerAddress(21241), "cleanup-alt-a", true);
    CNode peerB(INVALID_SOCKET, TestPeerAddress(21242), "cleanup-alt-b", true);
    const uint256 hashA(212401);
    const uint256 hashB(212402);

    RecordAlternateBlockAnnouncer(hashA, peerA.GetId());
    RecordAlternateBlockAnnouncer(hashB, peerA.GetId());
    RecordAlternateBlockAnnouncer(hashB, peerB.GetId());
    RecordBlockLastTimeoutOwner(hashA, peerA.GetId());
    BOOST_CHECK_EQUAL(CountBlockAlternateAnnouncerHashes(), 2U);

    const int64_t nCleanupBefore =
        MetricGet(ibdmetrics::Get().alternate_announcer_cleanup);
    peerA.Cleanup();
    BOOST_CHECK_EQUAL(CountBlockAlternateAnnouncerHashes(), 1U);
    BOOST_CHECK_EQUAL(MetricGet(ibdmetrics::Get().alternate_hashes_current), 1);
    BOOST_CHECK_EQUAL(
        MetricGet(ibdmetrics::Get().alternate_announcer_cleanup),
        nCleanupBefore + 2);

    std::vector<NodeId> announcers;
    GetBlockAlternateAnnouncers(hashB, announcers);
    BOOST_CHECK_EQUAL(announcers.size(), 1U);
    BOOST_CHECK_EQUAL(announcers[0], peerB.GetId());

    NodeId nOwner = -1;
    BOOST_CHECK(!GetBlockLastTimeoutOwner(hashA, &nOwner));
}

// Fast path: a healthy announcer with no exact-hash timeout keeps the request.
BOOST_AUTO_TEST_CASE(ibd_redirect_keeps_healthy_announcer)
{
    CScopedIbdQualityState scope;
    CNode announcer(INVALID_SOCKET, TestPeerAddress(21301), "redirect-fast", true);
    CNode alt(INVALID_SOCKET, TestPeerAddress(21302), "redirect-fast-alt", true);
    PrepareIbdRedirectLane(alt);
    CScopedVNodes vnodes;
    vnodes.Add(&alt);
    const uint256 hash(213001);

    for (int i = 0; i < QUALITY_EVENT_COUNT; ++i)
        announcer.RecordIbdBlockDelivery(1000, false);
    RecordAlternateBlockAnnouncer(hash, alt.GetId());

    IbdAskForRedirectReason reason = IBD_REDIRECT_NONE;
    bool noAlt = true;
    CNode* target =
        ChooseIbdBlockRequestTarget(&announcer, hash, false, &reason, &noAlt);
    BOOST_CHECK(target == NULL);
    BOOST_CHECK_EQUAL((int)reason, (int)IBD_REDIRECT_NONE);
    BOOST_CHECK(!noAlt);
}

// Exact-hash rule: the announcer that timed out a hash is bypassed for a
// recorded alternate.
BOOST_AUTO_TEST_CASE(ibd_redirect_on_exact_hash_timeout)
{
    CScopedIbdQualityState scope;
    CNode announcer(INVALID_SOCKET, TestPeerAddress(21311), "redirect-exact", true);
    CNode altA(INVALID_SOCKET, TestPeerAddress(21312), "redirect-exact-a", true);
    CNode altB(INVALID_SOCKET, TestPeerAddress(21313), "redirect-exact-b", true);
    PrepareIbdRedirectLane(altA);
    PrepareIbdRedirectLane(altB);
    CScopedVNodes vnodes;
    vnodes.Add(&altA);
    vnodes.Add(&altB);
    const uint256 hash(213101);

    for (int i = 0; i < QUALITY_EVENT_COUNT; ++i)
        announcer.RecordIbdBlockDelivery(1000, false);
    RecordAlternateBlockAnnouncer(hash, altA.GetId());
    RecordAlternateBlockAnnouncer(hash, altB.GetId());
    RecordBlockLastTimeoutOwner(hash, announcer.GetId());

    const int64_t nCrossBefore =
        MetricGet(ibdmetrics::Get().cross_peer_reissue_after_timeout);
    IbdAskForRedirectReason reason = IBD_REDIRECT_NONE;
    bool noAlt = false;
    CNode* target =
        ChooseIbdBlockRequestTarget(&announcer, hash, false, &reason, &noAlt);
    BOOST_CHECK(target != NULL);
    BOOST_CHECK_EQUAL((int)reason, (int)IBD_REDIRECT_HASH_TIMEOUT);
    BOOST_CHECK(!noAlt);
    BOOST_CHECK(target == &altA || target == &altB);
    BOOST_CHECK_EQUAL(
        MetricGet(ibdmetrics::Get().cross_peer_reissue_after_timeout),
        nCrossBefore + 1);
}

// No alternative to the failed owner: the announcer keeps the request
// (frontier-safe fallback) and the reissue is attributed.
BOOST_AUTO_TEST_CASE(ibd_redirect_no_alternative_reissues_same_peer)
{
    CScopedIbdQualityState scope;
    CNode announcer(INVALID_SOCKET, TestPeerAddress(21321), "redirect-noalt", true);
    const uint256 hash(213201);

    for (int i = 0; i < QUALITY_EVENT_COUNT; ++i)
        announcer.RecordIbdBlockDelivery(1000, false);
    RecordBlockLastTimeoutOwner(hash, announcer.GetId());

    const int64_t nSameBefore =
        MetricGet(ibdmetrics::Get().same_peer_reissue_after_timeout);
    const int64_t nNoAltBefore =
        MetricGet(ibdmetrics::Get().timeout_reissue_no_alternative);
    IbdAskForRedirectReason reason = IBD_REDIRECT_NONE;
    bool noAlt = false;
    CNode* target =
        ChooseIbdBlockRequestTarget(&announcer, hash, false, &reason, &noAlt);
    BOOST_CHECK(target == NULL);
    BOOST_CHECK_EQUAL((int)reason, (int)IBD_REDIRECT_NONE);
    BOOST_CHECK(noAlt);
    BOOST_CHECK_EQUAL(
        MetricGet(ibdmetrics::Get().same_peer_reissue_after_timeout),
        nSameBefore + 1);
    BOOST_CHECK_EQUAL(
        MetricGet(ibdmetrics::Get().timeout_reissue_no_alternative),
        nNoAltBefore + 1);
}

// Quality rule: a degraded announcer is bypassed for a good alternate.
BOOST_AUTO_TEST_CASE(ibd_redirect_on_degraded_announcer)
{
    CScopedIbdQualityState scope;
    CNode announcer(INVALID_SOCKET, TestPeerAddress(21331), "redirect-degraded", true);
    CNode alt(INVALID_SOCKET, TestPeerAddress(21332), "redirect-degraded-alt", true);
    PrepareIbdRedirectLane(alt);
    CScopedVNodes vnodes;
    vnodes.Add(&alt);
    const uint256 hash(213301);

    for (int i = 0; i < QUALITY_EVENT_COUNT; ++i)
        announcer.RecordIbdBlockTimeout();
    for (int i = 0; i < QUALITY_EVENT_COUNT; ++i)
        alt.RecordIbdBlockDelivery(1000, false);
    RecordAlternateBlockAnnouncer(hash, alt.GetId());

    IbdAskForRedirectReason reason = IBD_REDIRECT_NONE;
    bool noAlt = false;
    CNode* target =
        ChooseIbdBlockRequestTarget(&announcer, hash, false, &reason, &noAlt);
    BOOST_CHECK(target == &alt);
    BOOST_CHECK_EQUAL((int)reason, (int)IBD_REDIRECT_QUALITY);
    BOOST_CHECK(!noAlt);
}

// Quality rule with no alternate: the degraded announcer still serves
// (frontier-safe fallback, never a hard drop).
BOOST_AUTO_TEST_CASE(ibd_redirect_degraded_no_alternative_keeps_announcer)
{
    CScopedIbdQualityState scope;
    CNode announcer(INVALID_SOCKET, TestPeerAddress(21341), "redirect-deg-noalt", true);
    const uint256 hash(213401);

    for (int i = 0; i < QUALITY_EVENT_COUNT; ++i)
        announcer.RecordIbdBlockTimeout();

    const int64_t nNoAltBefore =
        MetricGet(ibdmetrics::Get().peer_quality_no_alternative);
    IbdAskForRedirectReason reason = IBD_REDIRECT_NONE;
    bool noAlt = false;
    CNode* target =
        ChooseIbdBlockRequestTarget(&announcer, hash, false, &reason, &noAlt);
    BOOST_CHECK(target == NULL);
    BOOST_CHECK(noAlt);
    BOOST_CHECK_EQUAL(
        MetricGet(ibdmetrics::Get().peer_quality_no_alternative),
        nNoAltBefore + 1);
}

// Frontier exemption: the frontier candidate / deferred frontier hash always
// keeps the announcer verbatim, even when the peer is degraded.
BOOST_AUTO_TEST_CASE(ibd_redirect_frontier_exempt_keeps_announcer)
{
    CScopedIbdQualityState scope;
    CNode announcer(INVALID_SOCKET, TestPeerAddress(21351), "redirect-frontier", true);
    CNode alt(INVALID_SOCKET, TestPeerAddress(21352), "redirect-frontier-alt", true);
    PrepareIbdRedirectLane(alt);
    CScopedVNodes vnodes;
    vnodes.Add(&alt);
    const uint256 hash(213501);

    for (int i = 0; i < QUALITY_EVENT_COUNT; ++i)
        announcer.RecordIbdBlockTimeout();
    RecordAlternateBlockAnnouncer(hash, alt.GetId());
    RecordBlockLastTimeoutOwner(hash, announcer.GetId());

    IbdAskForRedirectReason reason = IBD_REDIRECT_NONE;
    bool noAlt = false;
    CNode* target =
        ChooseIbdBlockRequestTarget(&announcer, hash, true, &reason, &noAlt);
    BOOST_CHECK(target == NULL);
    BOOST_CHECK_EQUAL((int)reason, (int)IBD_REDIRECT_NONE);
    BOOST_CHECK(!noAlt);
}

// Concentration rule: a dominant peer is bypassed for a fresh admission when
// another eligible lane exists.
BOOST_AUTO_TEST_CASE(ibd_redirect_on_concentration)
{
    CScopedIbdQualityState scope;
    CNode announcer(INVALID_SOCKET, TestPeerAddress(21361), "redirect-conc", true);
    CNode alt(INVALID_SOCKET, TestPeerAddress(21362), "redirect-conc-alt", true);
    PrepareIbdRedirectLane(announcer);
    PrepareIbdRedirectLane(alt);
    CScopedVNodes vnodes;
    vnodes.Add(&announcer);
    vnodes.Add(&alt);
    const uint256 hash(213601);

    for (int i = 0; i < QUALITY_EVENT_COUNT; ++i)
        announcer.RecordIbdBlockDelivery(1000, false);
    for (int i = 0; i < QUALITY_EVENT_COUNT; ++i)
        alt.RecordIbdBlockDelivery(1000, false);
    // The redirect target must be a provable holder of the hash: only a
    // recorded alternate announcer is ever eligible.
    RecordAlternateBlockAnnouncer(hash, alt.GetId());

    const int64_t nGaugeSaved =
        MetricGet(ibdmetrics::Get().global_active_current);
    // Dominant share: the announcer holds 60 of 100 global active requests.
    ibdmetrics::Get().global_active_current.store(100, std::memory_order_relaxed);
    SetIbdPeerPressure(announcer, 60);
    SetIbdPeerPressure(alt, 40);

    IbdAskForRedirectReason reason = IBD_REDIRECT_NONE;
    bool noAlt = false;
    CNode* target =
        ChooseIbdBlockRequestTarget(&announcer, hash, false, &reason, &noAlt);
    BOOST_CHECK(target == &alt);
    BOOST_CHECK_EQUAL((int)reason, (int)IBD_REDIRECT_CONCENTRATION);
    BOOST_CHECK(!noAlt);

    ibdmetrics::Get().global_active_current.store(
        nGaugeSaved, std::memory_order_relaxed);
}

// Concentration rule with no other lane: there is no hard global ban; the
// dominant peer keeps the request.
BOOST_AUTO_TEST_CASE(ibd_redirect_concentration_no_alternative_keeps_announcer)
{
    CScopedIbdQualityState scope;
    CNode announcer(INVALID_SOCKET, TestPeerAddress(21371), "redirect-conc-noalt", true);
    PrepareIbdRedirectLane(announcer);
    CScopedVNodes vnodes;
    vnodes.Add(&announcer);
    const uint256 hash(213701);

    for (int i = 0; i < QUALITY_EVENT_COUNT; ++i)
        announcer.RecordIbdBlockDelivery(1000, false);

    const int64_t nGaugeSaved =
        MetricGet(ibdmetrics::Get().global_active_current);
    ibdmetrics::Get().global_active_current.store(100, std::memory_order_relaxed);
    SetIbdPeerPressure(announcer, 90);

    const int64_t nNoAltBefore =
        MetricGet(ibdmetrics::Get().peer_concentration_no_alternative);
    IbdAskForRedirectReason reason = IBD_REDIRECT_NONE;
    bool noAlt = false;
    CNode* target =
        ChooseIbdBlockRequestTarget(&announcer, hash, false, &reason, &noAlt);
    BOOST_CHECK(target == NULL);
    BOOST_CHECK(noAlt);
    BOOST_CHECK_EQUAL(
        MetricGet(ibdmetrics::Get().peer_concentration_no_alternative),
        nNoAltBefore + 1);

    ibdmetrics::Get().global_active_current.store(
        nGaugeSaved, std::memory_order_relaxed);
}

// AskForBlockInvWithQualityRedirection: the ask lands on the redirected target,
// the announcer is recorded as an alternate for future reassignment, and the
// redirect / selected-tier counters increment.
BOOST_AUTO_TEST_CASE(ibd_askfor_wiring_redirects_and_records_alternate)
{
    CScopedIbdQualityState scope;
    CScopedAlreadyAskedFor already;
    CNode announcer(INVALID_SOCKET, TestPeerAddress(21381), "wiring-announcer", true);
    CNode alt(INVALID_SOCKET, TestPeerAddress(21382), "wiring-alt", true);
    PrepareIbdRedirectLane(alt);
    CScopedVNodes vnodes;
    vnodes.Add(&alt);
    const uint256 hash(213801);
    const CInv inv(MSG_BLOCK, hash);

    for (int i = 0; i < QUALITY_EVENT_COUNT; ++i)
        announcer.RecordIbdBlockTimeout();
    for (int i = 0; i < QUALITY_EVENT_COUNT; ++i)
        alt.RecordIbdBlockDelivery(1000, false);
    RecordAlternateBlockAnnouncer(hash, alt.GetId());

    const int64_t nRedirectBefore =
        MetricGet(ibdmetrics::Get().peer_quality_redirects);
    const int64_t nGoodSelectedBefore =
        MetricGet(ibdmetrics::Get().quality_good_selected);
    AskForResult res = AskForBlockInvWithQualityRedirection(
        &announcer, inv, BLOCKREQ_SOURCE_INV, false);
    BOOST_CHECK_EQUAL((int)res, (int)ASKFOR_QUEUED);
    BOOST_CHECK_EQUAL(
        MetricGet(ibdmetrics::Get().peer_quality_redirects),
        nRedirectBefore + 1);
    BOOST_CHECK_EQUAL(
        MetricGet(ibdmetrics::Get().quality_good_selected),
        nGoodSelectedBefore + 1);

    // The ask landed on the alternate, not the announcer.
    BOOST_CHECK(alt.setAskForBlocks.count(hash));
    BOOST_CHECK(!announcer.setAskForBlocks.count(hash));

    // The announcer was recorded as an alternate for future reassignment.
    std::vector<NodeId> announcers;
    GetBlockAlternateAnnouncers(hash, announcers);
    BOOST_CHECK(std::find(announcers.begin(), announcers.end(),
                          announcer.GetId()) != announcers.end());

    alt.ClearAskFor();
}

// The OWNED_BY_OTHER branch of AskFor records the announcer as an alternate.
BOOST_AUTO_TEST_CASE(ibd_askfor_owned_by_other_records_alternate)
{
    CScopedIbdQualityState scope;
    CScopedAlreadyAskedFor already;
    CNode owner(INVALID_SOCKET, TestPeerAddress(21391), "ownedby-owner", true);
    CNode announcer(INVALID_SOCKET, TestPeerAddress(21392), "ownedby-announcer", true);
    const uint256 hash(213901);
    const CInv inv(MSG_BLOCK, hash);

    BOOST_CHECK(TryAssignBlockRequestOwner(hash, owner.GetId()));
    AskForResult res = announcer.AskFor(inv, BLOCKREQ_SOURCE_INV);
    BOOST_CHECK_EQUAL((int)res, (int)ASKFOR_OWNED_BY_OTHER);
    BOOST_CHECK_EQUAL(announcer.setAskForBlocks.size(), 0U);

    std::vector<NodeId> announcers;
    GetBlockAlternateAnnouncers(hash, announcers);
    BOOST_CHECK_EQUAL(announcers.size(), 1U);
    BOOST_CHECK_EQUAL(announcers[0], announcer.GetId());

    ReleaseBlockRequestOwnersForPeer(owner.GetId(), "disconnect");
    ResetBlockAlternateAnnouncersForTesting();
}

// In-flight lifecycle: an expired request feeds the quality record, the last
// timeout-owner ledger, and the timeout metric.
BOOST_AUTO_TEST_CASE(ibd_inflight_lifecycle_feeds_quality_and_ledgers)
{
    CScopedIbdQualityState scope;
    CNode peer(INVALID_SOCKET, TestPeerAddress(21401), "lifecycle", true);
    const uint256 hash(214001);

    const int64_t nTimeoutBefore =
        MetricGet(ibdmetrics::Get().peer_quality_timeout_outcomes);
    peer.MarkBlockInFlight(hash);
    // Backdate the timestamps so the block expires on the next pass.  The wire
    // stamp is 61 s old so the 60 s wire-origin deadline has passed and this
    // records a genuine timeout (not a pending-wire release).
    peer.mapBlockInFlightSince[hash] = GetTime() - 60;
    peer.mapBlockInFlightMarkUs[hash] = 1;
    {
        LOCK(peer.cs_vBlockInFlightWire);
        peer.mapBlockInFlightWireUs[hash] = GetTimeMicros() - 61000000;
    }
    peer.ExpireBlockInFlight();

    BOOST_CHECK(peer.setBlocksInFlight.empty());
    BOOST_CHECK_EQUAL(peer.ibdQuality.releases_by_timeout.load(), 1u);
    BOOST_CHECK_EQUAL(
        MetricGet(ibdmetrics::Get().peer_quality_timeout_outcomes),
        nTimeoutBefore + 1);
    BOOST_CHECK(WasBlockLastTimedOutByPeer(hash, peer.GetId()));

    // A delivery that is reported after the timeout is counted as late.
    const int64_t nLateBefore =
        MetricGet(ibdmetrics::Get().peer_quality_late_outcomes);
    peer.RecordIbdBlockDelivery(5000, true);
    BOOST_CHECK_EQUAL(peer.ibdQuality.received_after_timeout.load(), 1u);
    BOOST_CHECK_EQUAL(
        MetricGet(ibdmetrics::Get().peer_quality_late_outcomes),
        nLateBefore + 1);
    BOOST_CHECK_GT(peer.ibdQuality.late_delivery_score.load(), 0);
}

// Wire-origin clock (ibd-conservative-block-request-expiration §10.1): a
// request expires from its getdata batch's first socket send + 60 s, not from
// enqueue.  Deferring the wire by N s must defer the deadline by N s even when
// the enqueue timestamp alone would already have expired the request.
BOOST_AUTO_TEST_CASE(expiry_clock_is_wire_origin)
{
    CScopedIbdQualityState scope;
    CNode peer(INVALID_SOCKET, TestPeerAddress(21431), "wire-clock", true);
    const uint256 hash(214301);

    peer.MarkBlockInFlight(hash);
    // Enqueued 40 s ago (would have expired long ago under the old 5 s
    // enqueue-bound), but the getdata only reached the wire 20 s ago.
    peer.mapBlockInFlightSince[hash] = GetTime() - 40;
    {
        LOCK(peer.cs_vBlockInFlightWire);
        peer.mapBlockInFlightWireUs[hash] = GetTimeMicros() - 20000000;
    }
    // Deadline = wire + 60 s = 40 s from now: nothing expires yet.
    peer.ExpireBlockInFlight();
    BOOST_CHECK_EQUAL(peer.setBlocksInFlight.count(hash), 1U);
    BOOST_CHECK_EQUAL(peer.mapBlockInFlightSince.count(hash), 1U);
    BOOST_CHECK(!WasBlockLastTimedOutByPeer(hash, peer.GetId()));

    // Advance the wire stamp past the deadline: now it expires as a timeout.
    {
        LOCK(peer.cs_vBlockInFlightWire);
        peer.mapBlockInFlightWireUs[hash] = GetTimeMicros() - 61000000;
    }
    peer.ExpireBlockInFlight();
    BOOST_CHECK_EQUAL(peer.setBlocksInFlight.count(hash), 0U);
    BOOST_CHECK_EQUAL(peer.mapBlockInFlightSince.count(hash), 0U);
    BOOST_CHECK(WasBlockLastTimedOutByPeer(hash, peer.GetId()));
}

// Pending-wire safety net (ibd-conservative-block-request-expiration §10.2): a
// request whose getdata never reaches the socket still expires from enqueue
// after the pending-wire bound, releasing ownership and gauges exactly once,
// with no peer timeout/quality penalty and no late-delivery expectation.
BOOST_AUTO_TEST_CASE(pending_wire_expiry_releases_without_penalty)
{
    CScopedIbdQualityState scope;
    CNode peer(INVALID_SOCKET, TestPeerAddress(21432), "pending-wire", true);
    const uint256 hash(214302);

    const int64_t nInflightBefore =
        MetricGet(ibdmetrics::Get().global_active_current);
    const int64_t nTimeoutOutcomesBefore =
        MetricGet(ibdmetrics::Get().peer_quality_timeout_outcomes);

    BOOST_CHECK(TryAssignBlockRequestOwner(hash, peer.GetId(), BLOCKREQ_SOURCE_INV));
    peer.MarkBlockInFlight(hash);
    BOOST_CHECK(IsBlockRequestOwnedByAnyPeer(hash));
    // Never send the getdata: the wire stamp stays at the pending sentinel 0.
    // Enqueue was 2 s ago, past the 1 s pending-wire bound.
    peer.mapBlockInFlightSince[hash] = GetTime() - 2;
    peer.ExpireBlockInFlight();

    BOOST_CHECK_EQUAL(peer.setBlocksInFlight.count(hash), 0U);
    BOOST_CHECK_EQUAL(peer.mapBlockInFlightSince.count(hash), 0U);
    // Ownership and gauges released exactly once.
    BOOST_CHECK(!IsBlockRequestOwnedByAnyPeer(hash));
    BOOST_CHECK_EQUAL(
        MetricGet(ibdmetrics::Get().global_active_current), nInflightBefore);
    // No peer timeout/quality penalty and no late-delivery expectation.
    BOOST_CHECK_EQUAL(
        MetricGet(ibdmetrics::Get().peer_quality_timeout_outcomes),
        nTimeoutOutcomesBefore);
    BOOST_CHECK(!WasBlockLastTimedOutByPeer(hash, peer.GetId()));
    NodeId nPeer = -1;
    int64_t nMarkUs = 0;
    BOOST_CHECK(!TakeBlockLateDeliveryExpectation(hash, &nPeer, &nMarkUs));

    // A second pass must not double-release anything.
    peer.ExpireBlockInFlight();
    BOOST_CHECK_EQUAL(
        MetricGet(ibdmetrics::Get().global_active_current), nInflightBefore);
    BOOST_CHECK(!IsBlockRequestOwnedByAnyPeer(hash));
}

// Concentration invariant: a dominant announcer with NO recorded alternate is
// never redirected -- even when a plain eligible lane exists in vNodes.  A
// redirect requires proof the target holds the hash.
BOOST_AUTO_TEST_CASE(concentrated_unique_announcer_is_not_redirected)
{
    CScopedIbdQualityState scope;
    CNode announcer(INVALID_SOCKET, TestPeerAddress(21411), "conc-unique-a", true);
    CNode lane(INVALID_SOCKET, TestPeerAddress(21412), "conc-unique-lane", true);
    PrepareIbdRedirectLane(announcer);
    PrepareIbdRedirectLane(lane);
    CScopedVNodes vnodes;
    vnodes.Add(&announcer);
    vnodes.Add(&lane);
    const uint256 hash(214101);

    for (int i = 0; i < QUALITY_EVENT_COUNT; ++i)
        announcer.RecordIbdBlockDelivery(1000, false);

    const int64_t nGaugeSaved =
        MetricGet(ibdmetrics::Get().global_active_current);
    ibdmetrics::Get().global_active_current.store(100, std::memory_order_relaxed);
    SetIbdPeerPressure(announcer, 90);
    SetIbdPeerPressure(lane, 10);

    const int64_t nNoAltBefore =
        MetricGet(ibdmetrics::Get().peer_concentration_no_alternative);
    IbdAskForRedirectReason reason = IBD_REDIRECT_NONE;
    bool noAlt = false;
    CNode* target =
        ChooseIbdBlockRequestTarget(&announcer, hash, false, &reason, &noAlt);
    // The eligible-but-unproven lane must not be chosen.
    BOOST_CHECK(target == NULL);
    BOOST_CHECK(noAlt);
    BOOST_CHECK_EQUAL(
        MetricGet(ibdmetrics::Get().peer_concentration_no_alternative),
        nNoAltBefore + 1);

    ibdmetrics::Get().global_active_current.store(
        nGaugeSaved, std::memory_order_relaxed);
}

// Concentration invariant: when both a recorded alternate and an unproven lane
// exist, the redirect goes only to the recorded alternate -- the unproven lane
// is never a candidate.
BOOST_AUTO_TEST_CASE(concentrated_announcer_redirects_only_to_known_alternative)
{
    CScopedIbdQualityState scope;
    CNode announcer(INVALID_SOCKET, TestPeerAddress(21421), "conc-known-a", true);
    CNode alt(INVALID_SOCKET, TestPeerAddress(21422), "conc-known-alt", true);
    CNode lane(INVALID_SOCKET, TestPeerAddress(21423), "conc-known-lane", true);
    PrepareIbdRedirectLane(announcer);
    PrepareIbdRedirectLane(alt);
    PrepareIbdRedirectLane(lane);
    CScopedVNodes vnodes;
    vnodes.Add(&announcer);
    vnodes.Add(&alt);
    vnodes.Add(&lane);
    const uint256 hash(214201);

    for (int i = 0; i < QUALITY_EVENT_COUNT; ++i)
        announcer.RecordIbdBlockDelivery(1000, false);
    for (int i = 0; i < QUALITY_EVENT_COUNT; ++i)
        alt.RecordIbdBlockDelivery(1000, false);
    RecordAlternateBlockAnnouncer(hash, alt.GetId());

    const int64_t nGaugeSaved =
        MetricGet(ibdmetrics::Get().global_active_current);
    ibdmetrics::Get().global_active_current.store(100, std::memory_order_relaxed);
    SetIbdPeerPressure(announcer, 70);
    SetIbdPeerPressure(alt, 20);
    SetIbdPeerPressure(lane, 10);

    IbdAskForRedirectReason reason = IBD_REDIRECT_NONE;
    bool noAlt = false;
    CNode* target =
        ChooseIbdBlockRequestTarget(&announcer, hash, false, &reason, &noAlt);
    BOOST_CHECK(target == &alt);
    BOOST_CHECK_EQUAL((int)reason, (int)IBD_REDIRECT_CONCENTRATION);
    BOOST_CHECK(!noAlt);

    ibdmetrics::Get().global_active_current.store(
        nGaugeSaved, std::memory_order_relaxed);
}

// Concentration invariant: a recorded alternate that is not redirect-eligible
// (disconnected) cannot be used; the dominant announcer keeps the request
// rather than falling back to an unproven lane.
BOOST_AUTO_TEST_CASE(ineligible_known_alternative_falls_back_to_original_announcer)
{
    CScopedIbdQualityState scope;
    CNode announcer(INVALID_SOCKET, TestPeerAddress(21431), "conc-inel-a", true);
    CNode alt(INVALID_SOCKET, TestPeerAddress(21432), "conc-inel-alt", true);
    CNode lane(INVALID_SOCKET, TestPeerAddress(21433), "conc-inel-lane", true);
    PrepareIbdRedirectLane(announcer);
    PrepareIbdRedirectLane(lane);
    // alt is recorded as holding the hash but is NOT an eligible lane.
    CScopedVNodes vnodes;
    vnodes.Add(&announcer);
    vnodes.Add(&alt);
    vnodes.Add(&lane);
    const uint256 hash(214301);

    for (int i = 0; i < QUALITY_EVENT_COUNT; ++i)
        announcer.RecordIbdBlockDelivery(1000, false);
    RecordAlternateBlockAnnouncer(hash, alt.GetId());

    const int64_t nGaugeSaved =
        MetricGet(ibdmetrics::Get().global_active_current);
    ibdmetrics::Get().global_active_current.store(100, std::memory_order_relaxed);
    SetIbdPeerPressure(announcer, 80);
    SetIbdPeerPressure(lane, 20);

    const int64_t nNoAltBefore =
        MetricGet(ibdmetrics::Get().peer_concentration_no_alternative);
    IbdAskForRedirectReason reason = IBD_REDIRECT_NONE;
    bool noAlt = false;
    CNode* target =
        ChooseIbdBlockRequestTarget(&announcer, hash, false, &reason, &noAlt);
    BOOST_CHECK(target == NULL);
    BOOST_CHECK(noAlt);
    BOOST_CHECK_EQUAL(
        MetricGet(ibdmetrics::Get().peer_concentration_no_alternative),
        nNoAltBefore + 1);

    ibdmetrics::Get().global_active_current.store(
        nGaugeSaved, std::memory_order_relaxed);
}

// Timeout-owner ledger: the hard global bound keeps the map bounded even when
// far more hashes than the cap are recorded.
BOOST_AUTO_TEST_CASE(last_timeout_owner_global_bound)
{
    CScopedIbdQualityState scope;
    CNode peer(INVALID_SOCKET, TestPeerAddress(21441), "timeout-bound", true);
    const size_t nExtra = 32;
    for (size_t i = 0; i < IBD_LAST_TIMEOUT_OWNER_MAX_HASHES + nExtra; ++i)
        RecordBlockLastTimeoutOwner(uint256(214401 + i), peer.GetId());
    BOOST_CHECK_LE(CountBlockLastTimeoutOwnerHashes(),
                   (size_t)IBD_LAST_TIMEOUT_OWNER_MAX_HASHES);
}

// Timeout-owner ledger: the bound is enforced after every insert -- the ledger
// never exceeds the cap at any intermediate step (no off-by-one).
BOOST_AUTO_TEST_CASE(last_timeout_owner_bound_does_not_exceed_limit)
{
    CScopedIbdQualityState scope;
    CNode peer(INVALID_SOCKET, TestPeerAddress(21442), "timeout-bound-exact", true);
    for (size_t i = 0; i < IBD_LAST_TIMEOUT_OWNER_MAX_HASHES + 64; ++i)
    {
        RecordBlockLastTimeoutOwner(uint256(214601 + i), peer.GetId());
        BOOST_CHECK_LE(CountBlockLastTimeoutOwnerHashes(),
                       (size_t)IBD_LAST_TIMEOUT_OWNER_MAX_HASHES);
    }
}

// Timeout-owner ledger: the per-request lookup TTL-checks only the found entry
// and erases it on expiry (O(log N) cost, no full-map scan).
BOOST_AUTO_TEST_CASE(last_timeout_owner_lookup_expires_single_entry)
{
    CScopedIbdQualityState scope;
    CNode peer(INVALID_SOCKET, TestPeerAddress(21443), "timeout-lookup", true);
    const uint256 hashA(214701);
    const uint256 hashB(214702);

    const int64_t nT0 = 200000000;
    SetIbdQualityClockForTesting(nT0);
    RecordBlockLastTimeoutOwner(hashA, peer.GetId());
    BOOST_CHECK_EQUAL(CountBlockLastTimeoutOwnerHashes(), 1U);

    const int64_t nAfter = nT0 + IBD_LAST_TIMEOUT_OWNER_TTL_US + 1;
    SetIbdQualityClockForTesting(nAfter);
    NodeId nOwner = -1;
    BOOST_CHECK(!GetBlockLastTimeoutOwner(hashA, &nOwner));
    // The found entry was expired and erased by the per-entry check.
    BOOST_CHECK_EQUAL(CountBlockLastTimeoutOwnerHashes(), 0U);
    UnsetIbdQualityClockForTesting();
}

// Timeout-owner ledger: the full-map prune is cadence-limited.  Within the
// lazy cadence window an access does not run a full scan; once the cadence
// elapses and entries are TTL-expired, the full prune reclaims them.
BOOST_AUTO_TEST_CASE(last_timeout_owner_prune_is_cadence_limited)
{
    CScopedIbdQualityState scope;
    CNode peerA(INVALID_SOCKET, TestPeerAddress(21444), "timeout-cadence-a", true);
    CNode peerB(INVALID_SOCKET, TestPeerAddress(21445), "timeout-cadence-b", true);
    const uint256 hashA(214801);
    const uint256 hashB(214802);
    const uint256 hashMissing(214899);

    const int64_t nT0 = 300000000;
    SetIbdQualityClockForTesting(nT0);
    // First insert anchors the cadence timer (the initial 0 runs the prune).
    RecordBlockLastTimeoutOwner(hashA, peerA.GetId());
    // Within the cadence window the full-map prune does not re-run.
    SetIbdQualityClockForTesting(nT0 + 3000000);
    RecordBlockLastTimeoutOwner(hashB, peerB.GetId());
    NodeId nOwner = -1;
    BOOST_CHECK(!GetBlockLastTimeoutOwner(hashMissing, &nOwner));
    BOOST_CHECK_EQUAL(CountBlockLastTimeoutOwnerHashes(), 2U);

    // After the cadence elapses and the TTL passes, an access runs the full
    // prune and reclaims the expired entries.
    SetIbdQualityClockForTesting(nT0 + IBD_LAST_TIMEOUT_OWNER_TTL_US + 6000000);
    BOOST_CHECK(!GetBlockLastTimeoutOwner(hashMissing, &nOwner));
    BOOST_CHECK_EQUAL(CountBlockLastTimeoutOwnerHashes(), 0U);
    UnsetIbdQualityClockForTesting();
}

// Late-delivery ledger: an early receive (live mark present) does not touch the
// late-delivery expectation; it survives until consumed or TTL-expired.
BOOST_AUTO_TEST_CASE(late_delivery_early_receive_does_not_touch_expectation)
{
    CScopedIbdQualityState scope;
    CNode peer(INVALID_SOCKET, TestPeerAddress(21451), "late-early", true);
    const uint256 hash(214501);

    SetIbdQualityClockForTesting(500000000);
    peer.MarkBlockInFlight(hash);
    RecordBlockLateDeliveryExpectation(hash, peer.GetId(), 1000, 506000000);
    // Early receive with a live mark: the expectation must remain untouched.
    peer.ClearBlockInFlight(hash);
    NodeId nPeer = -1;
    int64_t nMarkUs = 0;
    BOOST_CHECK(TakeBlockLateDeliveryExpectation(hash, &nPeer, &nMarkUs));
    BOOST_CHECK_EQUAL(nPeer, peer.GetId());
    BOOST_CHECK_EQUAL(nMarkUs, 1000);
    // Consumed by the take: nothing left.
    BOOST_CHECK(!TakeBlockLateDeliveryExpectation(hash, &nPeer, &nMarkUs));
    UnsetIbdQualityClockForTesting();
}

// Late-delivery ledger: a delivery after the request timed out is attributed to
// the peer that requested the hash as a late outcome, with the original mark
// latency, and the expectation is consumed (erased).
BOOST_AUTO_TEST_CASE(late_delivery_after_timeout_fixes_late_outcome)
{
    CScopedIbdQualityState scope;
    CNode peer(INVALID_SOCKET, TestPeerAddress(21452), "late-outcome", true);
    CScopedVNodes vnodes;
    vnodes.Add(&peer);
    const uint256 hash(214502);

    const int64_t nMarkUs = 1000000;
    SetIbdQualityClockForTesting(nMarkUs);
    peer.MarkBlockInFlight(hash);
    peer.mapBlockInFlightSince[hash] = GetTime() - 60;
    // Wire-origin clock: the getdata went out 61 s ago, so the 60 s deadline
    // has passed and expiry records a genuine timeout.
    {
        LOCK(peer.cs_vBlockInFlightWire);
        peer.mapBlockInFlightWireUs[hash] = GetTimeMicros() - 61000000;
    }
    // Force expiry at a known time so the late latency is deterministic.
    SetIbdQualityClockForTesting(nMarkUs + 9000000);
    peer.ExpireBlockInFlight();
    BOOST_CHECK(WasBlockLastTimedOutByPeer(hash, peer.GetId()));
    BOOST_CHECK_EQUAL(peer.ibdQuality.received_after_timeout.load(), 0u);

    // Late arrival: ClearBlockInFlight consumes the expectation and records a
    // late outcome for the requesting peer (without a second lifecycle release).
    SetIbdQualityClockForTesting(nMarkUs + 15000000);
    peer.ClearBlockInFlight(hash);
    BOOST_CHECK_EQUAL(peer.ibdQuality.received_after_timeout.load(), 1u);
    BOOST_CHECK_EQUAL(peer.ibdQuality.releases_by_receive.load(), 1u);
    // The late latency (16s mark -> 16s delivery = 15s) blended into the EWMA.
    BOOST_CHECK_EQUAL(peer.ibdQuality.latency_ewma_us.load(), 15000000);
    // Expectation consumed.
    NodeId nPeer = -1;
    int64_t nMarkUs2 = 0;
    BOOST_CHECK(!TakeBlockLateDeliveryExpectation(hash, &nPeer, &nMarkUs2));
    UnsetIbdQualityClockForTesting();
}

// Late-delivery ledger: expectations are TTL-expired and globally bounded.
BOOST_AUTO_TEST_CASE(late_delivery_expectation_ttl_and_bound)
{
    CScopedIbdQualityState scope;
    CNode peerA(INVALID_SOCKET, TestPeerAddress(21453), "late-ttl-a", true);
    CNode peerB(INVALID_SOCKET, TestPeerAddress(21454), "late-ttl-b", true);
    const uint256 hashA(214551);
    const uint256 hashB(214552);

    const int64_t nT0 = 400000000;
    SetIbdQualityClockForTesting(nT0);
    RecordBlockLateDeliveryExpectation(hashA, peerA.GetId(), 1000, nT0);
    RecordBlockLateDeliveryExpectation(hashB, peerB.GetId(), 1000, nT0);

    // TTL expiry removes both expectations via the prune hook.
    const int64_t nAfter = nT0 + IBD_LATE_DELIVERY_TTL_US + 1;
    SetIbdQualityClockForTesting(nAfter);
    ExpireBlockAlternateAnnouncersForTesting(nAfter);
    NodeId nPeer = -1;
    int64_t nMarkUs = 0;
    BOOST_CHECK(!TakeBlockLateDeliveryExpectation(hashA, &nPeer, &nMarkUs));
    BOOST_CHECK(!TakeBlockLateDeliveryExpectation(hashB, &nPeer, &nMarkUs));
    UnsetIbdQualityClockForTesting();

    // Global bound: recording beyond the cap keeps the ledger bounded.
    for (size_t i = 0; i < IBD_LATE_DELIVERY_MAX_HASHES + 16; ++i)
        RecordBlockLateDeliveryExpectation(
            uint256(214701 + i), peerA.GetId(), 1, 1);
    BOOST_CHECK_LE(CountBlockLateDeliveryExpectationHashes(),
                   (size_t)IBD_LATE_DELIVERY_MAX_HASHES);
}

// requests_issued is a wire-send signal: enqueueing an ask-for does not count;
// marking the block in flight (the getdata wire transition) does.
BOOST_AUTO_TEST_CASE(requests_issued_counts_wire_send_not_enqueue)
{
    CScopedIbdQualityState scope;
    CScopedAlreadyAskedFor already;
    CNode peer(INVALID_SOCKET, TestPeerAddress(21461), "req-issued", true);
    const uint256 hash(214601);
    const CInv inv(MSG_BLOCK, hash);

    const uint64_t nBefore = peer.ibdQuality.requests_issued.load();
    // Enqueue path: no wire request happened yet.
    peer.AddAskForEntry(GetTimeMicros() + 1000000, inv);
    BOOST_CHECK_EQUAL(peer.ibdQuality.requests_issued.load(), nBefore);
    BOOST_CHECK_EQUAL(peer.mapAskFor.size(), 1U);
    peer.EraseAskForEntry(peer.mapAskFor.begin());

    // Wire-send transition: MarkBlockInFlight counts the issued request.
    peer.MarkBlockInFlight(hash);
    BOOST_CHECK_EQUAL(peer.ibdQuality.requests_issued.load(), nBefore + 1);
    peer.ClearBlockInFlight(hash);
}

// The late-delivery outcome is attributed to the requesting peer even when the
// block arrives via a different node (the timeout peer stays the ledger owner).
BOOST_AUTO_TEST_CASE(late_delivery_attributed_to_requesting_peer)
{
    CScopedIbdQualityState scope;
    CNode requester(INVALID_SOCKET, TestPeerAddress(21471), "late-req", true);
    CNode deliverer(INVALID_SOCKET, TestPeerAddress(21472), "late-deliver", true);
    PrepareIbdRedirectLane(requester);
    PrepareIbdRedirectLane(deliverer);
    CScopedVNodes vnodes;
    vnodes.Add(&requester);
    vnodes.Add(&deliverer);
    const uint256 hash(214701);

    const int64_t nMarkUs = 1000000;
    SetIbdQualityClockForTesting(nMarkUs);
    requester.MarkBlockInFlight(hash);
    requester.mapBlockInFlightSince[hash] = GetTime() - 60;
    {
        LOCK(requester.cs_vBlockInFlightWire);
        requester.mapBlockInFlightWireUs[hash] = GetTimeMicros() - 61000000;
    }
    SetIbdQualityClockForTesting(nMarkUs + 9000000);
    requester.ExpireBlockInFlight();

    // The block finally arrives on a different peer; the late outcome still
    // lands on the peer that requested it.
    SetIbdQualityClockForTesting(nMarkUs + 20000000);
    deliverer.ClearBlockInFlight(hash);
    BOOST_CHECK_EQUAL(requester.ibdQuality.received_after_timeout.load(), 1u);
    BOOST_CHECK_EQUAL(deliverer.ibdQuality.received_after_timeout.load(), 0u);
    UnsetIbdQualityClockForTesting();
}

// A late block response from the previous owner must not erase ownership that
// was reassigned after the timeout.  This is the primary regression: it fails
// on pre-fix code, where ReleaseBlockRequestOwnerOnReceive cleared the current
// owner without verifying the delivering peer still owned the request.
BOOST_AUTO_TEST_CASE(late_receive_does_not_release_reassigned_owner)
{
    CScopedIbdQualityState scope;
    CScopedAlreadyAskedFor already;
    CNode peerA(INVALID_SOCKET, TestPeerAddress(21501), "late-owner-a", true);
    CNode peerB(INVALID_SOCKET, TestPeerAddress(21502), "late-owner-b", true);
    const uint256 hash(215001);

    // A obtains ownership of H and marks it in flight.
    BOOST_CHECK(TryAssignBlockRequestOwner(hash, peerA.GetId(), BLOCKREQ_SOURCE_INV));
    peerA.MarkBlockInFlight(hash);
    NodeId owner = -1;
    BlockRequestOwnerState state = BLOCK_REQUEST_OWNER_QUEUED;
    BOOST_CHECK(GetBlockRequestOwner(hash, &owner, &state));
    BOOST_CHECK_EQUAL(owner, peerA.GetId());

    // A times out through the real lifecycle path, releasing ownership.
    peerA.mapBlockInFlightSince[hash] = GetTime() - 60;
    peerA.ExpireBlockInFlight();
    BOOST_CHECK(!GetBlockRequestOwner(hash, &owner, &state));

    // H is reassigned to B.
    BOOST_CHECK(TryAssignBlockRequestOwner(hash, peerB.GetId(), BLOCKREQ_SOURCE_INV));
    peerB.MarkBlockInFlight(hash);
    BOOST_CHECK(GetBlockRequestOwner(hash, &owner, &state));
    BOOST_CHECK_EQUAL(owner, peerB.GetId());

    // The old response from A arrives late: run the receive-side cleanup for A.
    peerA.ClearBlockInFlight(hash);
    ReleaseBlockRequestOwnerOnReceive(hash, peerA.GetId());

    // Ownership must remain with B.
    BOOST_CHECK(GetBlockRequestOwner(hash, &owner, &state));
    BOOST_CHECK_EQUAL(owner, peerB.GetId());

    peerB.ClearBlockInFlight(hash);
    ReleaseBlockRequestOwnerOnReceive(hash, peerB.GetId());
    BOOST_CHECK(!GetBlockRequestOwner(hash, &owner, &state));
}

// A normal on-time receive from the current owner releases ownership exactly
// once; a second cleanup is an idempotent no-op.
BOOST_AUTO_TEST_CASE(current_owner_receive_releases_owner)
{
    CScopedIbdQualityState scope;
    CScopedAlreadyAskedFor already;
    CNode peerB(INVALID_SOCKET, TestPeerAddress(21503), "owner-recv-b", true);
    const uint256 hash(215002);

    BOOST_CHECK(TryAssignBlockRequestOwner(hash, peerB.GetId(), BLOCKREQ_SOURCE_INV));
    peerB.MarkBlockInFlight(hash);
    NodeId owner = -1;
    BlockRequestOwnerState state = BLOCK_REQUEST_OWNER_QUEUED;
    BOOST_CHECK(GetBlockRequestOwner(hash, &owner, &state));
    BOOST_CHECK_EQUAL(owner, peerB.GetId());

    BOOST_CHECK(ReleaseBlockRequestOwnerOnReceive(hash, peerB.GetId()));
    BOOST_CHECK(!GetBlockRequestOwner(hash, &owner, &state));
    BOOST_CHECK(!ReleaseBlockRequestOwnerOnReceive(hash, peerB.GetId()));
    BOOST_CHECK(!GetBlockRequestOwner(hash, &owner, &state));

    peerB.ClearBlockInFlight(hash);
}

// Receive-side ownership cleanup with no owner is an idempotent no-op.
BOOST_AUTO_TEST_CASE(unowned_receive_is_idempotent)
{
    CScopedIbdQualityState scope;
    CScopedAlreadyAskedFor already;
    CNode peerA(INVALID_SOCKET, TestPeerAddress(21504), "unowned-a", true);
    const uint256 hash(215003);

    BOOST_CHECK(!GetBlockRequestOwner(hash, NULL, NULL));
    BOOST_CHECK(!ReleaseBlockRequestOwnerOnReceive(hash, peerA.GetId()));
    BOOST_CHECK(!GetBlockRequestOwner(hash, NULL, NULL));
    BOOST_CHECK(!ReleaseBlockRequestOwnerOnReceive(hash, peerA.GetId()));
    BOOST_CHECK(!GetBlockRequestOwner(hash, NULL, NULL));
}

// A receive from a peer that never owned the hash must not clear the current
// owner, and must be observable via the mismatch-preserved counter.
BOOST_AUTO_TEST_CASE(foreign_receive_does_not_release_current_owner)
{
    CScopedIbdQualityState scope;
    CScopedAlreadyAskedFor already;
    CNode peerB(INVALID_SOCKET, TestPeerAddress(21505), "foreign-b", true);
    CNode peerC(INVALID_SOCKET, TestPeerAddress(21506), "foreign-c", true);
    const uint256 hash(215004);

    BOOST_CHECK(TryAssignBlockRequestOwner(hash, peerB.GetId(), BLOCKREQ_SOURCE_INV));
    peerB.MarkBlockInFlight(hash);

    const int64_t nMismatchBefore =
        MetricGet(ibdmetrics::Get().block_owner_receive_mismatch_preserved);
    BOOST_CHECK(!ReleaseBlockRequestOwnerOnReceive(hash, peerC.GetId()));
    BOOST_CHECK_EQUAL(
        MetricGet(ibdmetrics::Get().block_owner_receive_mismatch_preserved),
        nMismatchBefore + 1);

    NodeId owner = -1;
    BlockRequestOwnerState state = BLOCK_REQUEST_OWNER_QUEUED;
    BOOST_CHECK(GetBlockRequestOwner(hash, &owner, &state));
    BOOST_CHECK_EQUAL(owner, peerB.GetId());

    peerB.ClearBlockInFlight(hash);
}

// Full lifecycle convergence: late old-owner receive preserves B, then B's
// normal receive clears ownership.
BOOST_AUTO_TEST_CASE(late_old_owner_then_new_owner_receive)
{
    CScopedIbdQualityState scope;
    CScopedAlreadyAskedFor already;
    CNode peerA(INVALID_SOCKET, TestPeerAddress(21507), "conv-a", true);
    CNode peerB(INVALID_SOCKET, TestPeerAddress(21508), "conv-b", true);
    const uint256 hash(215005);

    BOOST_CHECK(TryAssignBlockRequestOwner(hash, peerA.GetId(), BLOCKREQ_SOURCE_INV));
    peerA.MarkBlockInFlight(hash);
    peerA.mapBlockInFlightSince[hash] = GetTime() - 60;
    peerA.ExpireBlockInFlight();
    BOOST_CHECK(!GetBlockRequestOwner(hash, NULL, NULL));

    BOOST_CHECK(TryAssignBlockRequestOwner(hash, peerB.GetId(), BLOCKREQ_SOURCE_INV));
    peerB.MarkBlockInFlight(hash);
    NodeId owner = -1;
    BlockRequestOwnerState state = BLOCK_REQUEST_OWNER_QUEUED;
    BOOST_CHECK(GetBlockRequestOwner(hash, &owner, &state));
    BOOST_CHECK_EQUAL(owner, peerB.GetId());

    // Late receive from A must not disturb B.
    peerA.ClearBlockInFlight(hash);
    ReleaseBlockRequestOwnerOnReceive(hash, peerA.GetId());
    BOOST_CHECK(GetBlockRequestOwner(hash, &owner, &state));
    BOOST_CHECK_EQUAL(owner, peerB.GetId());

    // Normal receive from B converges to empty.
    peerB.ClearBlockInFlight(hash);
    ReleaseBlockRequestOwnerOnReceive(hash, peerB.GetId());
    BOOST_CHECK(!GetBlockRequestOwner(hash, &owner, &state));
}

// After A timeout -> B reassignment -> late A receive, the hash cannot be
// reopened for a third parallel owner while B still owns it.
BOOST_AUTO_TEST_CASE(late_receive_does_not_enable_duplicate_parallel_owner)
{
    CScopedIbdQualityState scope;
    CScopedAlreadyAskedFor already;
    CNode peerA(INVALID_SOCKET, TestPeerAddress(21509), "dup-a", true);
    CNode peerB(INVALID_SOCKET, TestPeerAddress(21510), "dup-b", true);
    CNode peerC(INVALID_SOCKET, TestPeerAddress(21511), "dup-c", true);
    const uint256 hash(215006);

    BOOST_CHECK(TryAssignBlockRequestOwner(hash, peerA.GetId(), BLOCKREQ_SOURCE_INV));
    peerA.MarkBlockInFlight(hash);
    peerA.mapBlockInFlightSince[hash] = GetTime() - 60;
    peerA.ExpireBlockInFlight();
    BOOST_CHECK(TryAssignBlockRequestOwner(hash, peerB.GetId(), BLOCKREQ_SOURCE_INV));
    peerB.MarkBlockInFlight(hash);

    // Late receive from A.
    peerA.ClearBlockInFlight(hash);
    ReleaseBlockRequestOwnerOnReceive(hash, peerA.GetId());

    // C cannot claim ownership while B owns it (ownership conflict).
    NodeId existingPeer = -1;
    BlockRequestOwnerState existingState = BLOCK_REQUEST_OWNER_QUEUED;
    BOOST_CHECK(!TryAssignBlockRequestOwner(
        hash, peerC.GetId(), BLOCKREQ_SOURCE_INV, &existingPeer, &existingState));
    BOOST_CHECK_EQUAL(existingPeer, peerB.GetId());

    // C's AskFor is also refused with an ownership conflict.
    AskForResult res = peerC.AskFor(CInv(MSG_BLOCK, hash), BLOCKREQ_SOURCE_INV);
    BOOST_CHECK_EQUAL((int)res, (int)ASKFOR_OWNED_BY_OTHER);

    NodeId owner = -1;
    BlockRequestOwnerState state = BLOCK_REQUEST_OWNER_QUEUED;
    BOOST_CHECK(GetBlockRequestOwner(hash, &owner, &state));
    BOOST_CHECK_EQUAL(owner, peerB.GetId());

    peerB.ClearBlockInFlight(hash);
}

// The late-owner mismatch path must not double-decrement the inflight / global
// active gauges, must not change peer pressure, and must not emit an
// in-flight-clear wake: the timeout already accounted the old owner exactly once.
BOOST_AUTO_TEST_CASE(ownership_gauge_and_active_state_remain_consistent)
{
    CScopedIbdQualityState scope;
    CScopedAlreadyAskedFor already;
    CNode peerA(INVALID_SOCKET, TestPeerAddress(21512), "gauge-a", true);
    CNode peerB(INVALID_SOCKET, TestPeerAddress(21513), "gauge-b", true);
    const uint256 hash(215007);

    const int64_t nGlobal0 =
        MetricGet(ibdmetrics::Get().global_active_current);
    const int64_t nInflight0 =
        MetricGet(ibdmetrics::Get().total_inflight_current);

    // A owns and times out; the timeout accounts A exactly once.
    BOOST_CHECK(TryAssignBlockRequestOwner(hash, peerA.GetId(), BLOCKREQ_SOURCE_INV));
    peerA.MarkBlockInFlight(hash);
    BOOST_CHECK_EQUAL(peerA.peerLiveActivePressure.load(), 1);
    peerA.mapBlockInFlightSince[hash] = GetTime() - 60;
    peerA.ExpireBlockInFlight();
    BOOST_CHECK_EQUAL(peerA.peerLiveActivePressure.load(), 0);
    BOOST_CHECK_EQUAL(MetricGet(ibdmetrics::Get().total_inflight_current), nInflight0);
    BOOST_CHECK_EQUAL(MetricGet(ibdmetrics::Get().global_active_current), nGlobal0);

    // B owns: one live request is added.
    BOOST_CHECK(TryAssignBlockRequestOwner(hash, peerB.GetId(), BLOCKREQ_SOURCE_INV));
    peerB.MarkBlockInFlight(hash);
    const int64_t nGlobalB = MetricGet(ibdmetrics::Get().global_active_current);
    const int64_t nInflightB = MetricGet(ibdmetrics::Get().total_inflight_current);
    BOOST_CHECK_EQUAL(nGlobalB, nGlobal0 + 1);
    BOOST_CHECK_EQUAL(nInflightB, nInflight0 + 1);
    BOOST_CHECK_EQUAL(peerB.peerLiveActivePressure.load(), 1);

    // Late receive from A must not touch any of the live gauges or B's pressure,
    // and must not emit an in-flight-clear wake (A's set is already empty).
    const int64_t nWakeBefore =
        MetricGet(ibdmetrics::Get().pipeline_wake_signal_clear_inflight);
    peerA.ClearBlockInFlight(hash);
    ReleaseBlockRequestOwnerOnReceive(hash, peerA.GetId());
    BOOST_CHECK_EQUAL(MetricGet(ibdmetrics::Get().total_inflight_current), nInflightB);
    BOOST_CHECK_EQUAL(MetricGet(ibdmetrics::Get().global_active_current), nGlobalB);
    BOOST_CHECK_EQUAL(peerB.peerLiveActivePressure.load(), 1);
    BOOST_CHECK_EQUAL(peerA.peerLiveActivePressure.load(), 0);
    BOOST_CHECK_EQUAL(
        MetricGet(ibdmetrics::Get().pipeline_wake_signal_clear_inflight),
        nWakeBefore);

    // Convergence: B's normal receive returns to the baseline.
    peerB.ClearBlockInFlight(hash);
    ReleaseBlockRequestOwnerOnReceive(hash, peerB.GetId());
    BOOST_CHECK_EQUAL(MetricGet(ibdmetrics::Get().total_inflight_current), nInflight0);
    BOOST_CHECK_EQUAL(MetricGet(ibdmetrics::Get().global_active_current), nGlobal0);
}

// End-to-end lifecycle with real production helpers: mark A -> timeout A ->
// mark B -> late block receive from A -> B ownership survives -> B completes ->
// every ownership/inflight structure converges to empty.
BOOST_AUTO_TEST_CASE(late_receive_preserves_reassigned_owner_full_lifecycle)
{
    CScopedIbdQualityState scope;
    CScopedAlreadyAskedFor already;
    CNode peerA(INVALID_SOCKET, TestPeerAddress(21514), "e2e-a", true);
    CNode peerB(INVALID_SOCKET, TestPeerAddress(21515), "e2e-b", true);
    const uint256 hash(215008);

    BOOST_CHECK(TryAssignBlockRequestOwner(hash, peerA.GetId(), BLOCKREQ_SOURCE_INV));
    peerA.MarkBlockInFlight(hash);
    peerA.mapBlockInFlightSince[hash] = GetTime() - 60;
    peerA.ExpireBlockInFlight();

    BOOST_CHECK(TryAssignBlockRequestOwner(hash, peerB.GetId(), BLOCKREQ_SOURCE_INV));
    peerB.MarkBlockInFlight(hash);

    // Late block receive from A.
    peerA.ClearBlockInFlight(hash);
    ReleaseBlockRequestOwnerOnReceive(hash, peerA.GetId());

    // B ownership survives the late receive.
    NodeId owner = -1;
    BlockRequestOwnerState state = BLOCK_REQUEST_OWNER_QUEUED;
    BOOST_CHECK(GetBlockRequestOwner(hash, &owner, &state));
    BOOST_CHECK_EQUAL(owner, peerB.GetId());
    BOOST_CHECK_EQUAL(peerB.setBlocksInFlight.count(hash), 1U);

    // B completes normally; all structures converge to empty.
    peerB.ClearBlockInFlight(hash);
    ReleaseBlockRequestOwnerOnReceive(hash, peerB.GetId());
    BOOST_CHECK(!GetBlockRequestOwner(hash, &owner, &state));
    BOOST_CHECK(peerA.setBlocksInFlight.empty());
    BOOST_CHECK(peerB.setBlocksInFlight.empty());
    BOOST_CHECK_EQUAL(peerA.mapBlockInFlightSince.count(hash), 0U);
    BOOST_CHECK_EQUAL(peerB.mapBlockInFlightSince.count(hash), 0U);
}

BOOST_AUTO_TEST_CASE(priority_headers_outbound_bypasses_unsent_bulk)
{
    CNode peer(INVALID_SOCKET, TestPeerAddress(70), "priority-out", true);
    peer.vSendMsg.push_back(FramedMessage("block", 32));
    peer.vSendMeta.push_back(SendMessageMeta());
    peer.vSendMeta.back().command = "block";
    peer.nSendSize = peer.vSendMsg.front().size();
    peer.fNextHeadersResponsePriority = true;
    std::vector<CBlock> headers;
    peer.PushMessage("headers", headers);
    const std::vector<std::string> commands = SentCommands(peer);
    BOOST_REQUIRE_EQUAL(commands.size(), 2U);
    BOOST_CHECK_EQUAL(commands[0], "headers");
    BOOST_CHECK_EQUAL(commands[1], "block");
}

BOOST_AUTO_TEST_CASE(priority_headers_never_interrupts_partial_send)
{
    CNode peer(INVALID_SOCKET, TestPeerAddress(71), "priority-partial", true);
    peer.vSendMsg.push_back(FramedMessage("block", 32));
    peer.vSendMeta.push_back(SendMessageMeta());
    peer.vSendMeta.back().command = "block";
    peer.nSendSize = peer.vSendMsg.front().size();
    peer.nSendOffset = 1;
    peer.fNextHeadersResponsePriority = true;
    std::vector<CBlock> headers;
    peer.PushMessage("headers", headers);
    const std::vector<std::string> commands = SentCommands(peer);
    BOOST_REQUIRE_EQUAL(commands.size(), 2U);
    BOOST_CHECK_EQUAL(commands[0], "block");
    BOOST_CHECK_EQUAL(commands[1], "headers");
}

BOOST_AUTO_TEST_CASE(ordinary_headers_outbound_remains_fifo)
{
    CNode peer(INVALID_SOCKET, TestPeerAddress(72), "ordinary-out", true);
    peer.vSendMsg.push_back(FramedMessage("block", 32));
    peer.vSendMeta.push_back(SendMessageMeta());
    peer.vSendMeta.back().command = "block";
    peer.nSendSize = peer.vSendMsg.front().size();
    std::vector<CBlock> headers;
    peer.PushMessage("headers", headers);
    const std::vector<std::string> commands = SentCommands(peer);
    BOOST_REQUIRE_EQUAL(commands.size(), 2U);
    BOOST_CHECK_EQUAL(commands[0], "block");
    BOOST_CHECK_EQUAL(commands[1], "headers");
}

BOOST_AUTO_TEST_CASE(processmessages_drains_requested_blocks_in_bounded_burst)
{
    CScopedIbdQualityState quality;
    const bool savedObserve = fIbdHeadersObserve;
    const bool savedSpv = fSPVMode;
    fIbdHeadersObserve = true; fSPVMode = false;
    CNode peer(INVALID_SOCKET, TestPeerAddress(730), "burst-drain", true);
    for (unsigned int i = 0; i < 8; ++i)
        DeliverBlock(peer, 1000 + i, true);
    { LOCK(peer.cs_vRecvMsg); BOOST_REQUIRE(ProcessMessages(&peer, criticalblock)); }
    LOCK(peer.cs_vRecvMsg);
    BOOST_CHECK(peer.vRecvMsg.size() < 8U);
    BOOST_CHECK_EQUAL(peer.vRecvMsg.size(), 0U);
    fSPVMode = savedSpv; fIbdHeadersObserve = savedObserve;
}

BOOST_AUTO_TEST_CASE(processmessages_requested_block_burst_respects_count_limit)
{
    CScopedIbdQualityState quality;
    const bool savedObserve = fIbdHeadersObserve;
    const bool savedSpv = fSPVMode;
    fIbdHeadersObserve = true; fSPVMode = false;
    CNode peer(INVALID_SOCKET, TestPeerAddress(731), "burst-limit", true);
    for (unsigned int i = 0; i < 40; ++i)
        DeliverBlock(peer, 2000 + i, true);
    { LOCK(peer.cs_vRecvMsg); BOOST_REQUIRE(ProcessMessages(&peer, criticalblock)); }
    LOCK(peer.cs_vRecvMsg);
    BOOST_CHECK_EQUAL(peer.vRecvMsg.size(), 8U);
    fSPVMode = savedSpv; fIbdHeadersObserve = savedObserve;
}

BOOST_AUTO_TEST_CASE(processmessages_block_burst_does_not_drain_unsolicited_blocks)
{
    CScopedIbdQualityState quality;
    const bool savedObserve = fIbdHeadersObserve;
    const bool savedSpv = fSPVMode;
    fIbdHeadersObserve = true; fSPVMode = false;
    CNode peer(INVALID_SOCKET, TestPeerAddress(732), "burst-unsolicited", true);
    DeliverBlock(peer, 3000, true);
    DeliverBlock(peer, 3001, false);
    DeliverBlock(peer, 3002, false);
    { LOCK(peer.cs_vRecvMsg); BOOST_REQUIRE(ProcessMessages(&peer, criticalblock)); }
    LOCK(peer.cs_vRecvMsg);
    BOOST_CHECK_EQUAL(peer.vRecvMsg.size(), 2U);
    BOOST_CHECK_EQUAL(peer.vRecvMsg.front().hdr.GetCommand(), "block");
    fSPVMode = savedSpv; fIbdHeadersObserve = savedObserve;
}

BOOST_AUTO_TEST_CASE(processmessages_block_burst_preserves_nonblock_fairness)
{
    CScopedIbdQualityState quality;
    const bool savedObserve = fIbdHeadersObserve;
    const bool savedSpv = fSPVMode;
    fIbdHeadersObserve = true; fSPVMode = false;
    CNode peer(INVALID_SOCKET, TestPeerAddress(733), "burst-fair", true);
    for (unsigned int i = 0; i < 8; ++i)
        DeliverBlock(peer, 4000 + i, true);
    DeliverEmptyHeaders(peer);
    { LOCK(peer.cs_vRecvMsg); BOOST_REQUIRE(ProcessMessages(&peer, criticalblock)); }
    { LOCK(peer.cs_vRecvMsg); BOOST_REQUIRE_EQUAL(peer.vRecvMsg.size(), 1U);
      BOOST_CHECK_EQUAL(peer.vRecvMsg.front().hdr.GetCommand(), "headers"); }
    { LOCK(peer.cs_vRecvMsg); BOOST_REQUIRE(ProcessMessages(&peer, criticalblock)); }
    LOCK(peer.cs_vRecvMsg);
    BOOST_CHECK(peer.vRecvMsg.empty());
    fSPVMode = savedSpv; fIbdHeadersObserve = savedObserve;
}

BOOST_AUTO_TEST_CASE(solicited_headers_inbound_bypasses_complete_blocks)
{
    const bool savedObserve = fIbdHeadersObserve;
    const bool savedSpv = fSPVMode;
    fIbdHeadersObserve = true; fSPVMode = false;
    CNode peer(INVALID_SOCKET, TestPeerAddress(73), "priority-in", true);
    BOOST_REQUIRE_EQUAL(peer.getHeadersSync.Start("priority", TEST_TIME),
                        CGetHeadersSyncState::STARTED);
    DeliverFramed(peer, "block", 8);
    DeliverEmptyHeaders(peer);
    { LOCK(peer.cs_vRecvMsg); BOOST_REQUIRE(ProcessMessages(&peer, criticalblock)); }
    BOOST_REQUIRE_EQUAL(peer.vRecvMsg.size(), 1U);
    BOOST_CHECK_EQUAL(peer.vRecvMsg.front().hdr.GetCommand(), "block");
    BOOST_CHECK(peer.mapAskFor.empty());
    BOOST_CHECK(peer.setBlocksInFlight.empty());
    BOOST_CHECK_EQUAL(peer.deferredBlockInv.size(), 0U);
    fSPVMode = savedSpv; fIbdHeadersObserve = savedObserve;
}

BOOST_AUTO_TEST_CASE(priority_inbound_obeys_message_scan_bound)
{
    const bool savedObserve = fIbdHeadersObserve;
    const bool savedSpv = fSPVMode;
    fIbdHeadersObserve = true; fSPVMode = false;
    CNode peer(INVALID_SOCKET, TestPeerAddress(74), "priority-count", true);
    peer.getHeadersSync.Start("count", TEST_TIME);
    for (size_t i = 0; i < 256; ++i) DeliverFramed(peer, "block", 1);
    DeliverEmptyHeaders(peer);
    { LOCK(peer.cs_vRecvMsg); BOOST_REQUIRE(ProcessMessages(&peer, criticalblock)); }
    BOOST_REQUIRE_EQUAL(peer.vRecvMsg.size(), 256U);
    BOOST_CHECK_EQUAL(peer.vRecvMsg.front().hdr.GetCommand(), "block");
    BOOST_CHECK_EQUAL(peer.vRecvMsg.back().hdr.GetCommand(), "headers");
    fSPVMode = savedSpv; fIbdHeadersObserve = savedObserve;
}

BOOST_AUTO_TEST_CASE(priority_inbound_obeys_byte_scan_bound)
{
    const bool savedObserve = fIbdHeadersObserve;
    const bool savedSpv = fSPVMode;
    fIbdHeadersObserve = true; fSPVMode = false;
    CNode peer(INVALID_SOCKET, TestPeerAddress(75), "priority-bytes", true);
    peer.getHeadersSync.Start("bytes", TEST_TIME);
    DeliverFramed(peer, "block", 16 * 1024 * 1024);
    DeliverEmptyHeaders(peer);
    { LOCK(peer.cs_vRecvMsg); BOOST_REQUIRE(ProcessMessages(&peer, criticalblock)); }
    BOOST_REQUIRE_EQUAL(peer.vRecvMsg.size(), 1U);
    BOOST_CHECK_EQUAL(peer.vRecvMsg.front().hdr.GetCommand(), "headers");
    fSPVMode = savedSpv; fIbdHeadersObserve = savedObserve;
}

BOOST_AUTO_TEST_CASE(priority_inbound_dispatches_at_most_one_per_pass)
{
    const bool savedObserve = fIbdHeadersObserve;
    const bool savedSpv = fSPVMode;
    fIbdHeadersObserve = true; fSPVMode = false;
    CNode peer(INVALID_SOCKET, TestPeerAddress(76), "priority-one", true);
    peer.getHeadersSync.Start("one", TEST_TIME);
    DeliverFramed(peer, "block", 1);
    DeliverEmptyHeaders(peer);
    DeliverEmptyHeaders(peer);
    { LOCK(peer.cs_vRecvMsg); BOOST_REQUIRE(ProcessMessages(&peer, criticalblock)); }
    BOOST_REQUIRE_EQUAL(peer.vRecvMsg.size(), 2U);
    BOOST_CHECK_EQUAL(peer.vRecvMsg.front().hdr.GetCommand(), "block");
    BOOST_CHECK_EQUAL(peer.vRecvMsg.back().hdr.GetCommand(), "headers");
    { LOCK(peer.cs_vRecvMsg); BOOST_REQUIRE(ProcessMessages(&peer, criticalblock)); }
    BOOST_REQUIRE_EQUAL(peer.vRecvMsg.size(), 1U);
    BOOST_CHECK_EQUAL(peer.vRecvMsg.front().hdr.GetCommand(), "headers");
    { LOCK(peer.cs_vRecvMsg); BOOST_REQUIRE(ProcessMessages(&peer, criticalblock)); }
    BOOST_CHECK(peer.vRecvMsg.empty());
    fSPVMode = savedSpv; fIbdHeadersObserve = savedObserve;
}
BOOST_AUTO_TEST_CASE(priority_inbound_fairly_alternates_headers_and_blocks)
{
    const bool savedObserve = fIbdHeadersObserve;
    const bool savedSpv = fSPVMode;
    fIbdHeadersObserve = true; fSPVMode = false;
    CNode peer(INVALID_SOCKET, TestPeerAddress(91), "priority-fair", true);
    peer.getHeadersSync.Start("fair", TEST_TIME);
    for (size_t i = 0; i < 128; ++i) DeliverFramed(peer, "block", 1);
    for (size_t i = 0; i < 128; ++i) DeliverEmptyHeaders(peer);

    for (size_t i = 0; i < 128; ++i)
    {
        LOCK(peer.cs_vRecvMsg);
        BOOST_REQUIRE(ProcessMessages(&peer, criticalblock));
    }

    size_t remainingBlocks = 0;
    size_t remainingHeaders = 0;
    {
        LOCK(peer.cs_vRecvMsg);
        for (std::deque<CNetMessage>::const_iterator it = peer.vRecvMsg.begin();
             it != peer.vRecvMsg.end(); ++it)
        {
            if (it->hdr.GetCommand() == "block") ++remainingBlocks;
            if (it->hdr.GetCommand() == "headers") ++remainingHeaders;
        }
    }
    BOOST_CHECK_EQUAL(remainingBlocks, 64U);
    BOOST_CHECK_EQUAL(remainingHeaders, 64U);

    for (size_t i = 0; i < 128; ++i)
    {
        LOCK(peer.cs_vRecvMsg);
        BOOST_REQUIRE(ProcessMessages(&peer, criticalblock));
    }
    {
        LOCK(peer.cs_vRecvMsg);
        BOOST_CHECK(peer.vRecvMsg.empty());
    }
    fSPVMode = savedSpv; fIbdHeadersObserve = savedObserve;
}



BOOST_AUTO_TEST_CASE(priority_disabled_and_spv_preserve_fifo)
{
    const bool savedObserve = fIbdHeadersObserve;
    const bool savedSpv = fSPVMode;
    CNode peer(INVALID_SOCKET, TestPeerAddress(77), "priority-disabled", true);
    peer.getHeadersSync.Start("disabled", TEST_TIME);
    fIbdHeadersObserve = false; fSPVMode = false;
    DeliverFramed(peer, "block", 1); DeliverEmptyHeaders(peer);
    { LOCK(peer.cs_vRecvMsg); BOOST_REQUIRE(ProcessMessages(&peer, criticalblock)); }
    BOOST_CHECK_EQUAL(peer.vRecvMsg.front().hdr.GetCommand(), "headers");
    CNode spvPeer(INVALID_SOCKET, TestPeerAddress(78), "priority-spv", true);
    spvPeer.getHeadersSync.Start("spv", TEST_TIME);
    fIbdHeadersObserve = true; fSPVMode = true;
    DeliverFramed(spvPeer, "block", 1); DeliverEmptyHeaders(spvPeer);
    { LOCK(spvPeer.cs_vRecvMsg); BOOST_REQUIRE(ProcessMessages(&spvPeer, criticalblock)); }
    BOOST_CHECK_EQUAL(spvPeer.vRecvMsg.front().hdr.GetCommand(), "headers");
    fSPVMode = savedSpv; fIbdHeadersObserve = savedObserve;
}

namespace {
boost::mutex g_extract_hook_mutex;
boost::condition_variable g_extract_hook_cv;
bool g_extract_hook_entered = false;
bool g_extract_hook_release = false;

void WaitAfterExtractHook()
{
    boost::unique_lock<boost::mutex> lock(g_extract_hook_mutex);
    g_extract_hook_entered = true;
    g_extract_hook_cv.notify_all();
    while (!g_extract_hook_release)
        g_extract_hook_cv.wait(lock);
}

void ProcessOneQueuedMessage(CNode* peer)
{
    LOCK(peer->cs_vRecvMsg);
    ProcessMessages(peer, criticalblock);
}
}

BOOST_AUTO_TEST_CASE(extracted_message_releases_queue_lock_and_survives_mutation)
{
    CNode peer(INVALID_SOCKET, TestPeerAddress(79), "extract-lock", true);
    DeliverFramed(peer, "unknown-one", 1024);
    g_extract_hook_entered = false;
    g_extract_hook_release = false;
    g_processMessagesPostExtractHook = &WaitAfterExtractHook;

    boost::thread handler(boost::bind(&ProcessOneQueuedMessage, &peer));
    {
        boost::unique_lock<boost::mutex> lock(g_extract_hook_mutex);
        while (!g_extract_hook_entered)
            g_extract_hook_cv.wait(lock);
    }

    // The handler is paused after extraction but before dispatch. This is the
    // same lock/socket mutation performed by ReceiveMsgBytes.
    DeliverFramed(peer, "unknown-two", 8);
    {
        LOCK(peer.cs_vRecvMsg);
        BOOST_REQUIRE_EQUAL(peer.vRecvMsg.size(), 1U);
        BOOST_CHECK_EQUAL(peer.vRecvMsg.front().hdr.GetCommand(), "unknown-two");
    }
    {
        boost::unique_lock<boost::mutex> lock(g_extract_hook_mutex);
        g_extract_hook_release = true;
        g_extract_hook_cv.notify_all();
    }
    handler.join();
    g_processMessagesPostExtractHook = NULL;

    { LOCK(peer.cs_vRecvMsg); BOOST_REQUIRE(ProcessMessages(&peer, criticalblock)); }
    LOCK(peer.cs_vRecvMsg);
    BOOST_CHECK(peer.vRecvMsg.empty());
}

BOOST_AUTO_TEST_CASE(extraction_is_exactly_once_without_loss_or_duplication)
{
    CNode peer(INVALID_SOCKET, TestPeerAddress(80), "extract-once", true);
    DeliverFramed(peer, "unknown-a", 1);
    DeliverFramed(peer, "unknown-b", 1);
    DeliverFramed(peer, "unknown-c", 1);
    for (size_t expected = 2; ; --expected)
    {
        { LOCK(peer.cs_vRecvMsg); BOOST_REQUIRE(ProcessMessages(&peer, criticalblock)); }
        LOCK(peer.cs_vRecvMsg);
        BOOST_REQUIRE_EQUAL(peer.vRecvMsg.size(), expected);
        if (expected == 0) break;
        const std::string wanted = expected == 2 ? "unknown-b" : "unknown-c";
        BOOST_CHECK_EQUAL(peer.vRecvMsg.front().hdr.GetCommand(), wanted);
    }
}

BOOST_AUTO_TEST_CASE(incomplete_frame_is_not_extracted)
{
    CNode peer(INVALID_SOCKET, TestPeerAddress(81), "extract-incomplete", true);
    CSerializeData bytes = FramedMessage("unknown-partial", 8);
    {
        LOCK(peer.cs_vRecvMsg);
        BOOST_REQUIRE(peer.ReceiveMsgBytes((const char*)&bytes[0], 12));
        BOOST_REQUIRE(ProcessMessages(&peer, criticalblock));
        BOOST_REQUIRE_EQUAL(peer.vRecvMsg.size(), 1U);
        BOOST_CHECK(!peer.vRecvMsg.front().complete());
    }
}

BOOST_AUTO_TEST_CASE(disconnect_before_extraction_preserves_queued_frame)
{
    CNode peer(INVALID_SOCKET, TestPeerAddress(82), "extract-disconnect", true);
    DeliverFramed(peer, "unknown-disconnect", 1);
    peer.fDisconnect = true;
    {
        LOCK(peer.cs_vRecvMsg);
        BOOST_REQUIRE(ProcessMessages(&peer, criticalblock));
        BOOST_REQUIRE_EQUAL(peer.vRecvMsg.size(), 1U);
    }
}


BOOST_AUTO_TEST_CASE(headers_scheduler_refill_selects_only_active_window_hashes)
{
    const bool savedScheduler = fIbdHeaderScheduler;
    const bool savedObserve = fIbdHeadersObserve;
    const bool savedSpv = fSPVMode;
    fIbdHeaderScheduler = true;
    fIbdHeadersObserve = false;
    fSPVMode = false;
    ResetIbdHeaderSchedulerStateForTesting();

    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CNode peer(INVALID_SOCKET, TestPeerAddress(83), "sched-window", true);
    PreparePeerForSendMessages(peer, PROTOCOL_VERSION);
    CScopedInitialBlockDownloadState ibdState(&peer);

    BOOST_REQUIRE(SeedIbdHeaderSchedulerAnchorForTesting(uint256(100), 500));
    std::vector<std::pair<uint256, uint256> > headers;
    headers.push_back(std::make_pair(uint256(101), uint256(100)));
    headers.push_back(std::make_pair(uint256(102), uint256(101)));
    headers.push_back(std::make_pair(uint256(103), uint256(102)));
    headers.push_back(std::make_pair(uint256(104), uint256(103)));
    BOOST_REQUIRE(SeedIbdHeaderSchedulerHeadersForTesting(peer.GetId(), headers));
    for (int i = 101; i <= 104; ++i)
        SeedIbdHeaderSchedulerInvAvailabilityForTesting(peer.GetId(), uint256(i));

    std::vector<CNode*> peers(1, &peer);
    BOOST_CHECK_EQUAL(RefillOrderedHeaderBlockRequestsForTesting(peers), 4U);
    BOOST_CHECK(peer.IsBlockAskForQueued(uint256(101)));
    BOOST_CHECK(peer.IsBlockAskForQueued(uint256(102)));
    BOOST_CHECK(peer.IsBlockAskForQueued(uint256(103)));
    BOOST_CHECK(peer.IsBlockAskForQueued(uint256(104)));
    BOOST_CHECK(!peer.IsBlockAskForQueued(uint256(105)));

    peer.ClearAskFor();
    ResetIbdHeaderSchedulerStateForTesting();
    fSPVMode = savedSpv;
    fIbdHeadersObserve = savedObserve;
    fIbdHeaderScheduler = savedScheduler;
}

BOOST_AUTO_TEST_CASE(headers_scheduler_refill_distributes_by_candidate_peer)
{
    const bool savedScheduler = fIbdHeaderScheduler;
    const bool savedObserve = fIbdHeadersObserve;
    const bool savedSpv = fSPVMode;
    fIbdHeaderScheduler = true;
    fIbdHeadersObserve = false;
    fSPVMode = false;
    ResetIbdHeaderSchedulerStateForTesting();

    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CNode peerA(INVALID_SOCKET, TestPeerAddress(84), "sched-a", true);
    CNode peerB(INVALID_SOCKET, TestPeerAddress(85), "sched-b", true);
    PreparePeerForSendMessages(peerA, PROTOCOL_VERSION);
    PreparePeerForSendMessages(peerB, PROTOCOL_VERSION);
    CScopedInitialBlockDownloadState ibdState(&peerA);
    {
        LOCK(cs_vNodes);
        vNodes.push_back(&peerB);
    }

    BOOST_REQUIRE(SeedIbdHeaderSchedulerAnchorForTesting(uint256(200), 700));
    std::vector<std::pair<uint256, uint256> > headers;
    headers.push_back(std::make_pair(uint256(201), uint256(200)));
    headers.push_back(std::make_pair(uint256(202), uint256(201)));
    headers.push_back(std::make_pair(uint256(203), uint256(202)));
    headers.push_back(std::make_pair(uint256(204), uint256(203)));
    BOOST_REQUIRE(SeedIbdHeaderSchedulerHeadersForTesting(peerA.GetId(), headers));
    SeedIbdHeaderSchedulerInvAvailabilityForTesting(peerA.GetId(), uint256(201));
    SeedIbdHeaderSchedulerInvAvailabilityForTesting(peerB.GetId(), uint256(202));
    SeedIbdHeaderSchedulerInvAvailabilityForTesting(peerA.GetId(), uint256(203));
    SeedIbdHeaderSchedulerInvAvailabilityForTesting(peerB.GetId(), uint256(204));

    std::vector<CNode*> peers;
    peers.push_back(&peerA);
    peers.push_back(&peerB);
    BOOST_CHECK_EQUAL(RefillOrderedHeaderBlockRequestsForTesting(peers), 4U);
    BOOST_CHECK(peerA.IsBlockAskForQueued(uint256(201)));
    BOOST_CHECK(!peerB.IsBlockAskForQueued(uint256(201)));
    BOOST_CHECK(peerB.IsBlockAskForQueued(uint256(202)));
    BOOST_CHECK(peerA.IsBlockAskForQueued(uint256(203)));
    BOOST_CHECK(peerB.IsBlockAskForQueued(uint256(204)));

    peerA.ClearAskFor();
    peerB.ClearAskFor();
    ResetIbdHeaderSchedulerStateForTesting();
    fSPVMode = savedSpv;
    fIbdHeadersObserve = savedObserve;
    fIbdHeaderScheduler = savedScheduler;
}

BOOST_AUTO_TEST_CASE(headers_scheduler_owned_hash_blocks_duplicate_assignment)
{
    const bool savedScheduler = fIbdHeaderScheduler;
    const bool savedObserve = fIbdHeadersObserve;
    const bool savedSpv = fSPVMode;
    fIbdHeaderScheduler = true;
    fIbdHeadersObserve = false;
    fSPVMode = false;
    ResetIbdHeaderSchedulerStateForTesting();

    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CNode peerA(INVALID_SOCKET, TestPeerAddress(86), "sched-owner-a", true);
    CNode peerB(INVALID_SOCKET, TestPeerAddress(87), "sched-owner-b", true);
    PreparePeerForSendMessages(peerA, PROTOCOL_VERSION);
    PreparePeerForSendMessages(peerB, PROTOCOL_VERSION);
    CScopedInitialBlockDownloadState ibdState(&peerA);
    {
        LOCK(cs_vNodes);
        vNodes.push_back(&peerB);
    }

    BOOST_REQUIRE(SeedIbdHeaderSchedulerAnchorForTesting(uint256(300), 900));
    std::vector<std::pair<uint256, uint256> > headers;
    headers.push_back(std::make_pair(uint256(301), uint256(300)));
    BOOST_REQUIRE(SeedIbdHeaderSchedulerHeadersForTesting(peerA.GetId(), headers));
    SeedIbdHeaderSchedulerInvAvailabilityForTesting(peerA.GetId(), uint256(301));
    SeedIbdHeaderSchedulerInvAvailabilityForTesting(peerB.GetId(), uint256(301));
    BOOST_REQUIRE_EQUAL(peerA.AskFor(CInv(MSG_BLOCK, uint256(301)), BLOCKREQ_SOURCE_INV), ASKFOR_QUEUED);

    std::vector<CNode*> peers;
    peers.push_back(&peerA);
    peers.push_back(&peerB);
    BOOST_CHECK_EQUAL(RefillOrderedHeaderBlockRequestsForTesting(peers), 0U);
    BOOST_CHECK(peerA.IsBlockAskForQueued(uint256(301)));
    BOOST_CHECK(!peerB.IsBlockAskForQueued(uint256(301)));

    peerA.ClearAskFor();
    peerB.ClearAskFor();
    ResetIbdHeaderSchedulerStateForTesting();
    fSPVMode = savedSpv;
    fIbdHeadersObserve = savedObserve;
    fIbdHeaderScheduler = savedScheduler;
}

BOOST_AUTO_TEST_CASE(headers_scheduler_disconnect_release_makes_hash_requestable_again)
{
    const bool savedScheduler = fIbdHeaderScheduler;
    const bool savedObserve = fIbdHeadersObserve;
    const bool savedSpv = fSPVMode;
    fIbdHeaderScheduler = true;
    fIbdHeadersObserve = false;
    fSPVMode = false;
    ResetIbdHeaderSchedulerStateForTesting();

    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CNode peerA(INVALID_SOCKET, TestPeerAddress(88), "sched-rel-a", true);
    CNode peerB(INVALID_SOCKET, TestPeerAddress(89), "sched-rel-b", true);
    PreparePeerForSendMessages(peerA, PROTOCOL_VERSION);
    PreparePeerForSendMessages(peerB, PROTOCOL_VERSION);
    CScopedInitialBlockDownloadState ibdState(&peerA);
    {
        LOCK(cs_vNodes);
        vNodes.push_back(&peerB);
    }

    BOOST_REQUIRE(SeedIbdHeaderSchedulerAnchorForTesting(uint256(400), 1000));
    std::vector<std::pair<uint256, uint256> > headers;
    headers.push_back(std::make_pair(uint256(401), uint256(400)));
    BOOST_REQUIRE(SeedIbdHeaderSchedulerHeadersForTesting(peerA.GetId(), headers));
    SeedIbdHeaderSchedulerInvAvailabilityForTesting(peerA.GetId(), uint256(401));
    SeedIbdHeaderSchedulerInvAvailabilityForTesting(peerB.GetId(), uint256(401));
    BOOST_REQUIRE_EQUAL(peerA.AskFor(CInv(MSG_BLOCK, uint256(401)), BLOCKREQ_SOURCE_INV), ASKFOR_QUEUED);
    peerA.ClearAskFor();
    BOOST_CHECK_EQUAL(ReleaseBlockRequestOwnersForPeer(peerA.GetId(), "disconnect"), 0U);

    std::vector<CNode*> peers;
    peers.push_back(&peerA);
    peers.push_back(&peerB);
    BOOST_CHECK_EQUAL(RefillOrderedHeaderBlockRequestsForTesting(peers), 1U);
    BOOST_CHECK(peerB.IsBlockAskForQueued(uint256(401)) || peerA.IsBlockAskForQueued(uint256(401)));

    peerA.ClearAskFor();
    peerB.ClearAskFor();
    ResetIbdHeaderSchedulerStateForTesting();
    fSPVMode = savedSpv;
    fIbdHeadersObserve = savedObserve;
    fIbdHeaderScheduler = savedScheduler;
}

BOOST_AUTO_TEST_CASE(front_preempt_migrates_stale_head_to_proven_alternative)
{
    const bool savedScheduler = fIbdHeaderScheduler;
    const bool savedObserve = fIbdHeadersObserve;
    const bool savedSpv = fSPVMode;
    fIbdHeaderScheduler = true;
    fIbdHeadersObserve = false;
    fSPVMode = false;
    ResetIbdHeaderSchedulerStateForTesting();
    ResetBlockPreemptExpectationsForTesting();

    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CNode peerA(INVALID_SOCKET, TestPeerAddress(91), "preempt-a", true);
    CNode peerB(INVALID_SOCKET, TestPeerAddress(92), "preempt-b", true);
    PreparePeerForSendMessages(peerA, PROTOCOL_VERSION);
    PreparePeerForSendMessages(peerB, PROTOCOL_VERSION);
    CScopedInitialBlockDownloadState ibdState(&peerA);
    {
        LOCK(cs_vNodes);
        vNodes.push_back(&peerB);
    }

    BOOST_REQUIRE(SeedIbdHeaderSchedulerAnchorForTesting(uint256(700), 700));
    std::vector<std::pair<uint256, uint256> > headers;
    headers.push_back(std::make_pair(uint256(701), uint256(700)));
    BOOST_REQUIRE(SeedIbdHeaderSchedulerHeadersForTesting(peerA.GetId(), headers));
    BOOST_REQUIRE(SeedIbdHeaderSchedulerHeadersForTesting(peerB.GetId(), headers));
    SeedIbdHeaderSchedulerInvAvailabilityForTesting(peerA.GetId(), uint256(701));
    SeedIbdHeaderSchedulerInvAvailabilityForTesting(peerB.GetId(), uint256(701));

    // Owner A has the ordered head 701 in flight and has exceeded the
    // preempt wire-age threshold.
    BOOST_REQUIRE(TryAssignBlockRequestOwner(uint256(701), peerA.GetId(),
                                             BLOCKREQ_SOURCE_HEADERS_SCHEDULER));
    peerA.MarkBlockInFlight(uint256(701));
    {
        LOCK(peerA.cs_vBlockInFlightWire);
        peerA.mapBlockInFlightWireUs[uint256(701)] =
            CNode::QualityNowUs() - GetIbdOrderedPreemptWireAgeUs() - 1000000;
    }
    NodeId ownerPeer = -1;
    BlockRequestOwnerState ownerState = BLOCK_REQUEST_OWNER_QUEUED;
    BOOST_REQUIRE(GetBlockRequestOwnerDetails(uint256(701), &ownerPeer, &ownerState, NULL));
    BOOST_CHECK(ownerPeer == peerA.GetId());
    BOOST_CHECK(ownerState == BLOCK_REQUEST_OWNER_IN_FLIGHT);

    const int64_t nActiveBefore =
        MetricGet(ibdmetrics::Get().global_active_current);
    const int64_t nTransfersBefore =
        MetricGet(ibdmetrics::Get().front_preempt_transfers);
    const int64_t nAttemptsBefore =
        MetricGet(ibdmetrics::Get().front_preempt_attempts);

    std::vector<CNode*> peers;
    peers.push_back(&peerA);
    peers.push_back(&peerB);
    BOOST_CHECK_EQUAL(RefillOrderedHeaderBlockRequestsForTesting(peers), 0U);

    // The head slot migrated from A (in-flight) to B (queued).
    BOOST_REQUIRE(GetBlockRequestOwnerDetails(uint256(701), &ownerPeer, &ownerState, NULL));
    BOOST_CHECK(ownerPeer == peerB.GetId());
    BOOST_CHECK(ownerState == BLOCK_REQUEST_OWNER_QUEUED);
    BOOST_CHECK(!peerA.setBlocksInFlight.count(uint256(701)));
    // A's wire-origin stamp is destroyed at the commit phase, on the success
    // path.
    {
        LOCK(peerA.cs_vBlockInFlightWire);
        BOOST_CHECK(peerA.mapBlockInFlightWireUs.count(uint256(701)) == 0);
    }
    BOOST_CHECK(peerB.IsBlockAskForQueued(uint256(701)));
    // The request count is unchanged: -1 in-flight (A) offset by +1 queued (B).
    BOOST_CHECK_EQUAL(MetricGet(ibdmetrics::Get().global_active_current),
                      nActiveBefore);
    BOOST_CHECK_EQUAL(MetricGet(ibdmetrics::Get().front_preempt_transfers),
                      nTransfersBefore + 1);
    BOOST_CHECK_EQUAL(MetricGet(ibdmetrics::Get().front_preempt_attempts),
                      nAttemptsBefore + 1);
    BOOST_CHECK_EQUAL(CountBlockPreemptExpectationHashes(), 1U);

    peerA.ClearAskFor();
    peerB.ClearAskFor();
    ResetBlockPreemptExpectationsForTesting();
    ResetIbdHeaderSchedulerStateForTesting();
    fSPVMode = savedSpv;
    fIbdHeadersObserve = savedObserve;
    fIbdHeaderScheduler = savedScheduler;
}

BOOST_AUTO_TEST_CASE(front_preempt_abort_preserves_old_owner_wire_stamp)
{
    const bool savedScheduler = fIbdHeaderScheduler;
    const bool savedObserve = fIbdHeadersObserve;
    const bool savedSpv = fSPVMode;
    fIbdHeaderScheduler = true;
    fIbdHeadersObserve = false;
    fSPVMode = false;
    ResetIbdHeaderSchedulerStateForTesting();
    ResetBlockPreemptExpectationsForTesting();

    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CNode peerA(INVALID_SOCKET, TestPeerAddress(93), "preempt-abort-a", true);
    CNode peerB(INVALID_SOCKET, TestPeerAddress(94), "preempt-abort-b", true);
    PreparePeerForSendMessages(peerA, PROTOCOL_VERSION);
    PreparePeerForSendMessages(peerB, PROTOCOL_VERSION);
    CScopedInitialBlockDownloadState ibdState(&peerA);

    // Owner A holds the hash in flight with a live wire-origin stamp.
    const uint256 hash = uint256(702);
    BOOST_REQUIRE(TryAssignBlockRequestOwner(hash, peerA.GetId(),
                                             BLOCKREQ_SOURCE_HEADERS_SCHEDULER));
    peerA.MarkBlockInFlight(hash);
    const int64_t nWireUs = CNode::QualityNowUs() - 1000000;
    {
        LOCK(peerA.cs_vBlockInFlightWire);
        peerA.mapBlockInFlightWireUs[hash] = nWireUs;
    }

    const int64_t nActiveBefore =
        MetricGet(ibdmetrics::Get().global_active_current);

    // Force the abort: the prospective new owner already carries the hash, so
    // PreemptBlockRequestToPeer must fail its validation and leave A's request
    // exactly as it was.
    peerB.setBlocksInFlight.insert(hash);
    BOOST_CHECK(!PreemptBlockRequestToPeer(hash, &peerA, &peerB));

    // Abort-path invariant: ownership and the in-flight slot are untouched...
    NodeId ownerPeer = -1;
    BlockRequestOwnerState ownerState = BLOCK_REQUEST_OWNER_QUEUED;
    BOOST_REQUIRE(GetBlockRequestOwnerDetails(hash, &ownerPeer, &ownerState, NULL));
    BOOST_CHECK(ownerPeer == peerA.GetId());
    BOOST_CHECK(ownerState == BLOCK_REQUEST_OWNER_IN_FLIGHT);
    BOOST_CHECK(peerA.setBlocksInFlight.count(hash) == 1);
    // ...A's wire-origin timestamp is intact...
    {
        LOCK(peerA.cs_vBlockInFlightWire);
        std::map<uint256, int64_t>::const_iterator it =
            peerA.mapBlockInFlightWireUs.find(hash);
        BOOST_REQUIRE(it != peerA.mapBlockInFlightWireUs.end());
        BOOST_CHECK(it->second == nWireUs);
    }
    // ...so normal expiry semantics hold: the request keeps its wire-origin
    // deadline and is not misclassified as pending-wire (which would expire it
    // on the 1 s bound).
    peerA.ExpireBlockInFlight();
    BOOST_CHECK(peerA.setBlocksInFlight.count(hash) == 1);
    // No gauge movement and no preempt expectation were recorded.
    BOOST_CHECK_EQUAL(MetricGet(ibdmetrics::Get().global_active_current),
                      nActiveBefore);
    BOOST_CHECK_EQUAL(CountBlockPreemptExpectationHashes(), 0U);

    peerB.setBlocksInFlight.erase(hash);
    peerA.ClearBlockInFlight(hash);
    peerA.ClearAskFor();
    peerB.ClearAskFor();
    ResetBlockPreemptExpectationsForTesting();
    ResetIbdHeaderSchedulerStateForTesting();
    fSPVMode = savedSpv;
    fIbdHeadersObserve = savedObserve;
    fIbdHeaderScheduler = savedScheduler;
}

BOOST_AUTO_TEST_CASE(headers_scheduler_empty_window_requests_headers_not_blocks)
{
    const bool savedScheduler = fIbdHeaderScheduler;
    const bool savedObserve = fIbdHeadersObserve;
    const bool savedSpv = fSPVMode;
    fIbdHeaderScheduler = true;
    fIbdHeadersObserve = false;
    fSPVMode = false;
    ResetIbdHeaderSchedulerStateForTesting();

    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CNode peer(INVALID_SOCKET, TestPeerAddress(90), "sched-empty", true);
    PreparePeerForSendMessages(peer, PROTOCOL_VERSION);
    CScopedInitialBlockDownloadState ibdState(&peer);

    BOOST_REQUIRE(SeedIbdHeaderSchedulerAnchorForTesting(uint256(500), 1200));
    std::vector<CNode*> peers(1, &peer);
    BOOST_CHECK_EQUAL(RefillOrderedHeaderBlockRequestsForTesting(peers), 0U);
    BOOST_CHECK(peer.setAskForBlocks.empty());
    BOOST_CHECK(peer.getBlocksIndex.empty());
    BOOST_CHECK(HasCommand(SentCommands(peer), "getheaders"));

    peer.ClearAskFor();
    ResetIbdHeaderSchedulerStateForTesting();
    fSPVMode = savedSpv;
    fIbdHeadersObserve = savedObserve;
    fIbdHeaderScheduler = savedScheduler;
}

BOOST_AUTO_TEST_CASE(headers_scheduler_sequential_refill_uses_incremental_cursor)
{
    const bool savedScheduler = fIbdHeaderScheduler;
    const bool savedObserve = fIbdHeadersObserve;
    const bool savedSpv = fSPVMode;
    fIbdHeaderScheduler = true;
    fIbdHeadersObserve = false;
    fSPVMode = false;
    ResetIbdHeaderSchedulerStateForTesting();

    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CNode peer(INVALID_SOCKET, TestPeerAddress(91), "sched-incremental", true);
    PreparePeerForSendMessages(peer, PROTOCOL_VERSION);
    CScopedInitialBlockDownloadState ibdState(&peer);

    const uint256 anchor(1000000);
    BOOST_REQUIRE(SeedIbdHeaderSchedulerAnchorForTesting(anchor, 0));
    std::vector<std::pair<uint256, uint256> > headers;
    headers.reserve(10001);
    uint256 previous = anchor;
    for (int i = 1; i <= 10001; ++i)
    {
        const uint256 hash(1000000 + i);
        headers.push_back(std::make_pair(hash, previous));
        previous = hash;
    }
    BOOST_REQUIRE(SeedIbdHeaderSchedulerHeadersForTesting(peer.GetId(), headers));

    std::vector<CNode*> peers(1, &peer);
    BOOST_CHECK_EQUAL(RefillOrderedHeaderBlockRequestsForTesting(peers), 128U);

    for (int height = 1; height <= 10000; ++height)
    {
        const uint256 frontier(1000000 + height);
        BOOST_REQUIRE_MESSAGE(peer.IsBlockAskForQueued(frontier), "missing frontier " << height);
        for (std::multimap<int64_t, CInv>::iterator it = peer.mapAskFor.begin();
             it != peer.mapAskFor.end(); ++it)
        {
            if (it->second.hash == frontier)
            {
                peer.EraseAskForEntry(it, false);
                break;
            }
        }
        BOOST_REQUIRE(SeedIbdHeaderSchedulerAnchorForTesting(frontier, height));
        RefillOrderedHeaderBlockRequestsForTesting(peers);
        const std::vector<uint256> window =
            GetIbdHeaderSchedulerWindowForTesting(frontier);
        BOOST_REQUIRE(!window.empty());
        BOOST_CHECK(window.front() == uint256(1000000 + height + 1));
    }

    const IbdHeaderSchedulerRefillStats stats =
        GetIbdHeaderSchedulerRefillStatsForTesting();
    BOOST_CHECK_EQUAL(stats.fullRefillCalls, 1U);
    BOOST_CHECK_EQUAL(stats.fullEntriesExamined, 512U);
    BOOST_CHECK_EQUAL(stats.incrementalRefillCalls, 10000U);
    BOOST_CHECK_EQUAL(stats.incrementalEntriesExamined, 9873U);
    BOOST_CHECK_EQUAL(stats.incrementalAdmitted, 9873U);
    BOOST_CHECK(stats.incrementalRefillUs / stats.incrementalRefillCalls < stats.fullRefillUs);

    peer.ClearAskFor();
    ResetIbdHeaderSchedulerStateForTesting();
    fSPVMode = savedSpv;
    fIbdHeadersObserve = savedObserve;
    fIbdHeaderScheduler = savedScheduler;
}

BOOST_AUTO_TEST_CASE(headers_scheduler_contiguous_admission_keeps_wide_pipeline)
{
    const bool savedScheduler = fIbdHeaderScheduler;
    const bool savedObserve = fIbdHeadersObserve;
    const bool savedSpv = fSPVMode;
    fIbdHeaderScheduler = true;
    fIbdHeadersObserve = false;
    fSPVMode = false;
    ResetIbdHeaderSchedulerStateForTesting();

    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CNode peer(INVALID_SOCKET, TestPeerAddress(93), "sched-wide", true);
    PreparePeerForSendMessages(peer, PROTOCOL_VERSION);
    CScopedInitialBlockDownloadState ibdState(&peer);

    const uint256 anchor(2000000);
    BOOST_REQUIRE(SeedIbdHeaderSchedulerAnchorForTesting(anchor, 0));
    std::vector<std::pair<uint256, uint256> > headers;
    headers.reserve(600);
    uint256 previous = anchor;
    for (int i = 1; i <= 600; ++i)
    {
        const uint256 hash(2000000 + i);
        headers.push_back(std::make_pair(hash, previous));
        previous = hash;
    }
    BOOST_REQUIRE(SeedIbdHeaderSchedulerHeadersForTesting(peer.GetId(), headers));
    for (int i = 1; i <= 600; ++i)
        SeedIbdHeaderSchedulerInvAvailabilityForTesting(peer.GetId(), uint256(2000000 + i));

    std::vector<CNode*> peers(1, &peer);
    BOOST_CHECK_EQUAL(RefillOrderedHeaderBlockRequestsForTesting(peers), 128U);
    BOOST_CHECK_EQUAL(peer.setAskForBlocks.size(), 128U);
    BOOST_CHECK(peer.IsBlockAskForQueued(uint256(2000001)));
    BOOST_CHECK(peer.IsBlockAskForQueued(uint256(2000128)));
    BOOST_CHECK(!peer.IsBlockAskForQueued(uint256(2000129)));

    const IbdHeaderSchedulerRefillStats stats =
        GetIbdHeaderSchedulerRefillStatsForTesting();
    BOOST_CHECK_EQUAL(stats.fullRefillCalls, 1U);
    BOOST_CHECK_EQUAL(stats.incrementalAdmitted, 0U);

    peer.ClearAskFor();
    ResetIbdHeaderSchedulerStateForTesting();
    fSPVMode = savedSpv;
    fIbdHeadersObserve = savedObserve;
    fIbdHeaderScheduler = savedScheduler;
}

BOOST_AUTO_TEST_CASE(headers_scheduler_active_ancestor_closes_exact_source_hole)
{
    // Peer C never sent header 5, but it sent active-path descendants 6-10.
    // That strictly-above exact evidence makes C a normal candidate for 5,
    // closing the old exact-source metadata hole without inv availability.
    // Disconnecting B removes B exact evidence but must not recreate the hole.
    const bool savedScheduler = fIbdHeaderScheduler;
    const bool savedObserve = fIbdHeadersObserve;
    const bool savedSpv = fSPVMode;
    fIbdHeaderScheduler = true;
    fIbdHeadersObserve = false;
    fSPVMode = false;
    ResetIbdHeaderSchedulerStateForTesting();

    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CNode peerA(INVALID_SOCKET, TestPeerAddress(94), "sched-hole-a", true);
    CNode peerB(INVALID_SOCKET, TestPeerAddress(97), "sched-hole-b", true);
    CNode peerC(INVALID_SOCKET, TestPeerAddress(98), "sched-hole-c", true);
    PreparePeerForSendMessages(peerA, PROTOCOL_VERSION);
    PreparePeerForSendMessages(peerB, PROTOCOL_VERSION);
    PreparePeerForSendMessages(peerC, PROTOCOL_VERSION);
    CScopedInitialBlockDownloadState ibdState(&peerA);

    const uint256 anchor(3000000);
    BOOST_REQUIRE(SeedIbdHeaderSchedulerAnchorForTesting(anchor, 0));

    std::vector<std::pair<uint256, uint256> > headersA;
    headersA.reserve(4);
    uint256 previous = anchor;
    for (int i = 1; i <= 4; ++i)
    {
        const uint256 hash(3000000 + i);
        headersA.push_back(std::make_pair(hash, previous));
        previous = hash;
    }
    BOOST_REQUIRE(SeedIbdHeaderSchedulerHeadersForTesting(peerA.GetId(), headersA));

    std::vector<std::pair<uint256, uint256> > headersB;
    headersB.reserve(10);
    previous = anchor;
    for (int i = 1; i <= 10; ++i)
    {
        const uint256 hash(3000000 + i);
        headersB.push_back(std::make_pair(hash, previous));
        previous = hash;
    }
    BOOST_REQUIRE(SeedIbdHeaderSchedulerHeadersForTesting(peerB.GetId(), headersB));

    std::vector<std::pair<uint256, uint256> > headersC;
    headersC.reserve(5);
    previous = uint256(3000005);
    for (int i = 6; i <= 10; ++i)
    {
        const uint256 hash(3000000 + i);
        headersC.push_back(std::make_pair(hash, previous));
        previous = hash;
    }
    BOOST_REQUIRE(SeedIbdHeaderSchedulerHeadersForTesting(peerC.GetId(), headersC));

    // B was the only exact source of block 5.  Disconnecting B removes that
    // exact source; C remains inferred-eligible through active descendants.
    IbdHeadersObserverPeerDisconnected(peerB.GetId());

    std::vector<CNode*> peers;
    peers.push_back(&peerA);
    peers.push_back(&peerC);
    BOOST_CHECK_EQUAL(RefillOrderedHeaderBlockRequestsForTesting(peers), 10U);
    for (int i = 1; i <= 4; ++i)
        BOOST_CHECK(peerA.IsBlockAskForQueued(uint256(3000000 + i)));
    for (int i = 5; i <= 10; ++i)
        BOOST_CHECK(!peerA.IsBlockAskForQueued(uint256(3000000 + i)));
    for (int i = 6; i <= 10; ++i)
        BOOST_CHECK(peerC.IsBlockAskForQueued(uint256(3000000 + i)));

    const uint256 inflight(3000002);
    BOOST_REQUIRE(TryAssignBlockRequestOwner(inflight, peerA.GetId(), BLOCKREQ_SOURCE_ASKFOR));
    peerA.MarkBlockInFlight(inflight);
    for (std::multimap<int64_t, CInv>::iterator it = peerA.mapAskFor.begin();
         it != peerA.mapAskFor.end(); ++it)
    {
        if (it->second.hash == inflight)
        {
            peerA.EraseAskForEntry(it, false);
            break;
        }
    }
    NodeId ownerPeer = -1;
    BlockRequestOwnerState ownerState = BLOCK_REQUEST_OWNER_QUEUED;
    BOOST_CHECK(GetBlockRequestOwner(inflight, &ownerPeer, &ownerState));
    BOOST_CHECK_EQUAL(ownerPeer, peerA.GetId());
    BOOST_CHECK_EQUAL(ownerState, BLOCK_REQUEST_OWNER_IN_FLIGHT);

    BOOST_CHECK_EQUAL(RefillOrderedHeaderBlockRequestsForTesting(peers), 0U);
    BOOST_CHECK(GetBlockRequestOwner(inflight, &ownerPeer, &ownerState));
    BOOST_CHECK_EQUAL(ownerPeer, peerA.GetId());
    BOOST_CHECK_EQUAL(ownerState, BLOCK_REQUEST_OWNER_IN_FLIGHT);
    for (int i = 5; i <= 10; ++i)
        BOOST_CHECK(!peerA.IsBlockAskForQueued(uint256(3000000 + i)));
    for (int i = 6; i <= 10; ++i)
        BOOST_CHECK(peerC.IsBlockAskForQueued(uint256(3000000 + i)));
    for (int i = 1; i <= 4; ++i)
    {
        if (i != 2)
            BOOST_CHECK(peerA.IsBlockAskForQueued(uint256(3000000 + i)));
    }

    peerA.ClearAskFor();
    peerC.ClearAskFor();
    ResetIbdHeaderSchedulerStateForTesting();
    fSPVMode = savedSpv;
    fIbdHeadersObserve = savedObserve;
    fIbdHeaderScheduler = savedScheduler;
}

BOOST_AUTO_TEST_CASE(headers_scheduler_branch_switch_barrier_purges_only_obsolete_queue)
{
    const bool savedScheduler = fIbdHeaderScheduler;
    const bool savedObserve = fIbdHeadersObserve;
    const bool savedSpv = fSPVMode;
    fIbdHeaderScheduler = true;
    fIbdHeadersObserve = false;
    fSPVMode = false;
    ResetIbdHeaderSchedulerStateForTesting();

    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CNode peerA(INVALID_SOCKET, TestPeerAddress(120), "branch-a", true);
    CNode peerB(INVALID_SOCKET, TestPeerAddress(121), "branch-b", true);
    PreparePeerForSendMessages(peerA, PROTOCOL_VERSION);
    PreparePeerForSendMessages(peerB, PROTOCOL_VERSION);
    CScopedInitialBlockDownloadState ibdState(&peerA);
    {
        LOCK(cs_vNodes);
        vNodes.push_back(&peerB);
    }

    const uint256 anchor(6000000);
    const uint256 a1(6000001), a2(6000002), a3(6000003), a4(6000004);
    const uint256 b2(6000012), b3(6000013), b4(6000014);
    BOOST_REQUIRE(SeedIbdHeaderSchedulerAnchorForTesting(anchor, 0));

    std::vector<std::pair<uint256, uint256> > oldHeaders;
    oldHeaders.push_back(std::make_pair(a1, anchor));
    oldHeaders.push_back(std::make_pair(a2, a1));
    oldHeaders.push_back(std::make_pair(a3, a2));
    oldHeaders.push_back(std::make_pair(a4, a3));
    BOOST_REQUIRE(SeedIbdHeaderSchedulerHeadersForTesting(peerA.GetId(), oldHeaders));
    for (int i = 1; i <= 4; ++i)
        SeedIbdHeaderSchedulerInvAvailabilityForTesting(
            peerA.GetId(), uint256(6000000 + i));

    std::vector<CNode*> peers;
    peers.push_back(&peerA);
    peers.push_back(&peerB);
    BOOST_CHECK_EQUAL(RefillOrderedHeaderBlockRequestsForTesting(peers), 4U);

    NodeId oldOwner = -1;
    BlockRequestOwnerState oldState = BLOCK_REQUEST_OWNER_QUEUED;
    CNode* oldOwnerNode = NULL;
    for (std::vector<CNode*>::const_iterator pit = peers.begin();
         pit != peers.end() && oldOwnerNode == NULL; ++pit)
    {
        for (std::multimap<int64_t, CInv>::const_iterator qi =
                 (*pit)->mapAskFor.begin(); qi != (*pit)->mapAskFor.end(); ++qi)
            if (qi->second.hash == a2)
            {
                oldOwnerNode = *pit;
                break;
            }
    }
    BOOST_REQUIRE(oldOwnerNode != NULL);
    for (std::multimap<int64_t, CInv>::iterator it = oldOwnerNode->mapAskFor.begin();
         it != oldOwnerNode->mapAskFor.end(); ++it)
    {
        if (it->second.hash == a2)
        {
            oldOwnerNode->EraseAskForEntry(it, false);
            break;
        }
    }
    BOOST_REQUIRE(TryAssignBlockRequestOwner(
        a2, oldOwnerNode->GetId(), BLOCKREQ_SOURCE_HEADERS_SCHEDULER));
    oldOwnerNode->MarkBlockInFlight(a2);

    // Add an alternate active branch sharing only the connected prefix a1.
    std::vector<std::pair<uint256, uint256> > newHeaders;
    newHeaders.push_back(std::make_pair(a1, anchor));
    newHeaders.push_back(std::make_pair(b2, a1));
    newHeaders.push_back(std::make_pair(b3, b2));
    newHeaders.push_back(std::make_pair(b4, b3));
    BOOST_REQUIRE(SeedIbdHeaderSchedulerHeadersForTesting(peerB.GetId(), newHeaders));
    for (int i = 2; i <= 4; ++i)
        SeedIbdHeaderSchedulerInvAvailabilityForTesting(
            peerB.GetId(), uint256(6000000 + 10 + i));
    BOOST_REQUIRE(ActivateIbdHeaderSchedulerBranchForTesting(b4));

    BOOST_CHECK_EQUAL(RefillOrderedHeaderBlockRequestsForTesting(peers), 3U);
    BOOST_CHECK(peerA.IsBlockAskForQueued(a1) ||
                peerB.IsBlockAskForQueued(a1));
    BOOST_CHECK(!peerA.IsBlockAskForQueued(a3));
    BOOST_CHECK(!peerB.IsBlockAskForQueued(a3));
    BOOST_CHECK(!peerA.IsBlockAskForQueued(a4));
    BOOST_CHECK(!peerB.IsBlockAskForQueued(a4));
    BOOST_CHECK(GetBlockRequestOwner(a2, &oldOwner, &oldState));
    BOOST_CHECK_EQUAL(oldState, BLOCK_REQUEST_OWNER_IN_FLIGHT);
    BOOST_CHECK(!peerA.IsBlockAskForQueued(a2));
    BOOST_CHECK(!peerB.IsBlockAskForQueued(a2));
    BOOST_CHECK(peerB.IsBlockAskForQueued(b2));
    BOOST_CHECK(peerB.IsBlockAskForQueued(b3));
    BOOST_CHECK(peerB.IsBlockAskForQueued(b4));

    // Repeating the same branch decision is idempotent and does not reissue
    // the stale in-flight hash or duplicate the new-path queue.
    const size_t queuedBefore = peerA.setAskForBlocks.size() +
                                peerB.setAskForBlocks.size();
    BOOST_CHECK_EQUAL(RefillOrderedHeaderBlockRequestsForTesting(peers), 0U);
    BOOST_CHECK_EQUAL(peerA.setAskForBlocks.size() +
                      peerB.setAskForBlocks.size(), queuedBefore);
    BOOST_CHECK(GetBlockRequestOwner(a2, &oldOwner, &oldState));
    BOOST_CHECK_EQUAL(oldState, BLOCK_REQUEST_OWNER_IN_FLIGHT);

    peerA.ClearAskFor();
    peerB.ClearAskFor();
    oldOwnerNode->ClearBlockInFlight(a2);
    ResetIbdHeaderSchedulerStateForTesting();
    fSPVMode = savedSpv;
    fIbdHeadersObserve = savedObserve;
    fIbdHeaderScheduler = savedScheduler;
}

// Production discriminator: the real branch handoff is a full re-anchor of the
// connected chain, NOT ActivateBranchForTesting().  SeedIbdHeaderSchedulerAnchorForTesting
// drives the production entry point g_ibdHeadersObserver.UpdateAnchor(pindexBest)
// -> CIbdHeaderGraph::Reanchor: because the new best tip is not the old anchor's
// ACTIVE child, Reanchor performs a FULL re-anchor that wipes the graph and
// reseeds it around the new authoritative hash (graphTip == new best, empty
// window until headers repopulate it).  The barrier must still fire exactly
// once on the first refill after the re-anchor (while cursorValid is still true),
// purge every obsolete queued old-path hash, preserve the stale in-flight
// request exactly once, and then admit the new generation after the header
// graph is repopulated.  Repeated same-branch refill must be idempotent.
BOOST_AUTO_TEST_CASE(headers_scheduler_production_reanchor_reorg_barrier)
{
    const bool savedScheduler = fIbdHeaderScheduler;
    const bool savedObserve = fIbdHeadersObserve;
    const bool savedSpv = fSPVMode;
    fIbdHeaderScheduler = true;
    fIbdHeadersObserve = false;
    fSPVMode = false;
    ResetIbdHeaderSchedulerStateForTesting();

    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CNode peerA(INVALID_SOCKET, TestPeerAddress(128), "reanchor-a", true);
    CNode peerB(INVALID_SOCKET, TestPeerAddress(129), "reanchor-b", true);
    PreparePeerForSendMessages(peerA, PROTOCOL_VERSION);
    PreparePeerForSendMessages(peerB, PROTOCOL_VERSION);
    ScopedPeerSocket socketA(peerA);
    ScopedPeerSocket socketB(peerB);
    CScopedInitialBlockDownloadState ibdState(&peerA);
    {
        LOCK(cs_vNodes);
        vNodes.push_back(&peerB);
    }

    const int64_t activeBefore =
        ibdmetrics::Get().global_active_current.load(std::memory_order_relaxed);

    const uint256 anchor(6005000);
    const uint256 a1(6005001), a2(6005002), a3(6005003), a4(6005004);
    const uint256 c0(6005020);
    const uint256 c1(6005021), c2(6005022), c3(6005023);
    BOOST_REQUIRE(SeedIbdHeaderSchedulerAnchorForTesting(anchor, 0));

    std::vector<std::pair<uint256, uint256> > oldHeaders;
    oldHeaders.push_back(std::make_pair(a1, anchor));
    oldHeaders.push_back(std::make_pair(a2, a1));
    oldHeaders.push_back(std::make_pair(a3, a2));
    oldHeaders.push_back(std::make_pair(a4, a3));
    BOOST_REQUIRE(SeedIbdHeaderSchedulerHeadersForTesting(peerA.GetId(), oldHeaders));
    for (int i = 1; i <= 4; ++i)
        SeedIbdHeaderSchedulerInvAvailabilityForTesting(
            peerA.GetId(), uint256(6005000 + i));

    std::vector<CNode*> peers;
    peers.push_back(&peerA);
    peers.push_back(&peerB);
    BOOST_CHECK_EQUAL(RefillOrderedHeaderBlockRequestsForTesting(peers), 4U);

    CNode* oldOwnerNode = NULL;
    for (std::vector<CNode*>::const_iterator pit = peers.begin();
         pit != peers.end() && oldOwnerNode == NULL; ++pit)
        for (std::multimap<int64_t, CInv>::const_iterator qi =
                 (*pit)->mapAskFor.begin(); qi != (*pit)->mapAskFor.end(); ++qi)
            if (qi->second.hash == a2)
            {
                oldOwnerNode = *pit;
                break;
            }
    BOOST_REQUIRE(oldOwnerNode != NULL);
    for (std::multimap<int64_t, CInv>::iterator it = oldOwnerNode->mapAskFor.begin();
         it != oldOwnerNode->mapAskFor.end(); ++it)
        if (it->second.hash == a2)
        {
            oldOwnerNode->EraseAskForEntry(it, false);
            break;
        }
    BOOST_REQUIRE(TryAssignBlockRequestOwner(
        a2, oldOwnerNode->GetId(), BLOCKREQ_SOURCE_HEADERS_SCHEDULER));
    oldOwnerNode->MarkBlockInFlight(a2);

    // Real production competing-branch replacement: pindexBest moves to a tip
    // that is not on the active scheduler path.  UpdateAnchor -> Reanchor does
    // a full re-anchor (graph wipe + reseed around c0), so graphTip == c0 ==
    // the new frontier and the new-branch window is empty until headers arrive.
    BOOST_REQUIRE(SeedIbdHeaderSchedulerAnchorForTesting(c0, 0));

    // Barrier refill (exactly one chance, before the empty-window fallback
    // invalidates the cursor): obsolete queued a1/a3/a4 are purged; the stale
    // in-flight a2 is preserved exactly once.
    BOOST_CHECK_EQUAL(RefillOrderedHeaderBlockRequestsForTesting(peers), 0U);
    BOOST_CHECK(!peerA.IsBlockAskForQueued(a1));
    BOOST_CHECK(!peerA.IsBlockAskForQueued(a3));
    BOOST_CHECK(!peerA.IsBlockAskForQueued(a4));
    BOOST_CHECK(!peerB.IsBlockAskForQueued(a1));
    BOOST_CHECK(!peerB.IsBlockAskForQueued(a3));
    BOOST_CHECK(!peerB.IsBlockAskForQueued(a4));
    NodeId oldOwner = -1;
    BlockRequestOwnerState oldState = BLOCK_REQUEST_OWNER_QUEUED;
    BOOST_CHECK(GetBlockRequestOwner(a2, &oldOwner, &oldState));
    BOOST_CHECK_EQUAL(oldState, BLOCK_REQUEST_OWNER_IN_FLIGHT);
    BOOST_CHECK(!peerA.IsBlockAskForQueued(a2));
    BOOST_CHECK(!peerB.IsBlockAskForQueued(a2));

    // Header graph repopulation along the new branch (getheaders/headers).
    std::vector<std::pair<uint256, uint256> > newHeaders;
    newHeaders.push_back(std::make_pair(c1, c0));
    newHeaders.push_back(std::make_pair(c2, c1));
    newHeaders.push_back(std::make_pair(c3, c2));
    BOOST_REQUIRE(SeedIbdHeaderSchedulerHeadersForTesting(peerB.GetId(), newHeaders));
    for (int i = 1; i <= 3; ++i)
        SeedIbdHeaderSchedulerInvAvailabilityForTesting(
            peerB.GetId(), uint256(6005020 + i));

    BOOST_CHECK_EQUAL(RefillOrderedHeaderBlockRequestsForTesting(peers), 3U);
    BOOST_CHECK(peerB.IsBlockAskForQueued(c1));
    BOOST_CHECK(peerB.IsBlockAskForQueued(c2));
    BOOST_CHECK(peerB.IsBlockAskForQueued(c3));
    BOOST_CHECK(!peerA.IsBlockAskForQueued(a1));
    BOOST_CHECK(!peerA.IsBlockAskForQueued(a3));
    BOOST_CHECK(!peerA.IsBlockAskForQueued(a4));
    BOOST_CHECK(!peerB.IsBlockAskForQueued(a1));
    BOOST_CHECK(!peerB.IsBlockAskForQueued(a3));
    BOOST_CHECK(!peerB.IsBlockAskForQueued(a4));
    BOOST_CHECK(GetBlockRequestOwner(a2, &oldOwner, &oldState));
    BOOST_CHECK_EQUAL(oldState, BLOCK_REQUEST_OWNER_IN_FLIGHT);
    BOOST_CHECK(!peerA.IsBlockAskForQueued(a2));
    BOOST_CHECK(!peerB.IsBlockAskForQueued(a2));

    // Repeated same-branch refill is idempotent and never reissues the stale
    // in-flight hash or duplicates the new-path queue.
    const size_t queuedBefore = peerA.setAskForBlocks.size() +
                                peerB.setAskForBlocks.size();
    BOOST_CHECK_EQUAL(RefillOrderedHeaderBlockRequestsForTesting(peers), 0U);
    BOOST_CHECK_EQUAL(peerA.setAskForBlocks.size() +
                      peerB.setAskForBlocks.size(), queuedBefore);
    BOOST_CHECK(GetBlockRequestOwner(a2, &oldOwner, &oldState));
    BOOST_CHECK_EQUAL(oldState, BLOCK_REQUEST_OWNER_IN_FLIGHT);

    peerA.ClearAskFor();
    peerB.ClearAskFor();
    oldOwnerNode->ClearBlockInFlight(a2);
    const int64_t activeAfter =
        ibdmetrics::Get().global_active_current.load(std::memory_order_relaxed);
    BOOST_CHECK_EQUAL(activeAfter - activeBefore, 0LL);

    ResetIbdHeaderSchedulerStateForTesting();
    fSPVMode = savedSpv;
    fIbdHeadersObserve = savedObserve;
    fIbdHeaderScheduler = savedScheduler;
}

// Saturation discriminator: 512 old-generation requests are IN_FLIGHT within
// the real global cap (512), the connected chain performs the production
// full-re-anchor handoff to a disjoint branch, and the new generation initially
// receives zero admission budget.  The stale old-path requests are then expired
// through the REAL expiry decision (ExpireBlockInFlight + the ordered scheduler
// decision).  On the production re-anchor path the old nodes are no longer
// graph-resident, so they cannot receive the 300 s progress defer: they expire
// at their ordinary 60 s wire-origin deadline, freeing the budget so the new
// generation resumes.  This distinguishes a correctness failure from a bounded
// liveness delay.
BOOST_AUTO_TEST_CASE(headers_scheduler_saturation_handoff_bounded_liveness)
{
    // Force the effective per-peer IBD request window to the 512-slot global
    // cap so a single peer can hold 512 in-flight requests within real caps.
    const bool fHadPerPeerArg = mapArgs.count("-ibdmaxactiveperpeer") != 0;
    const std::string strPerPeerArgSaved = mapArgs["-ibdmaxactiveperpeer"];
    mapArgs["-ibdmaxactiveperpeer"] = "512";
    ResetMaxActiveBlockRequestsPerPeerConfigForTesting();

    const bool savedScheduler = fIbdHeaderScheduler;
    const bool savedObserve = fIbdHeadersObserve;
    const bool savedSpv = fSPVMode;
    fIbdHeaderScheduler = true;
    fIbdHeadersObserve = false;
    fSPVMode = false;
    ResetIbdHeaderSchedulerStateForTesting();

    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CNode peerA(INVALID_SOCKET, TestPeerAddress(130), "saturate-a", true);
    CNode peerB(INVALID_SOCKET, TestPeerAddress(131), "saturate-b", true);
    PreparePeerForSendMessages(peerA, PROTOCOL_VERSION);
    PreparePeerForSendMessages(peerB, PROTOCOL_VERSION);
    ScopedPeerSocket socketA(peerA);
    ScopedPeerSocket socketB(peerB);
    CScopedInitialBlockDownloadState ibdState(&peerA);
    {
        LOCK(cs_vNodes);
        vNodes.push_back(&peerB);
    }

    const uint256 anchor(7000000);
    BOOST_REQUIRE(SeedIbdHeaderSchedulerAnchorForTesting(anchor, 0));
    std::vector<std::pair<uint256, uint256> > headers;
    headers.reserve(1600);
    uint256 previous = anchor;
    for (int i = 1; i <= 1600; ++i)
    {
        const uint256 hash(7000000 + i);
        headers.push_back(std::make_pair(hash, previous));
        previous = hash;
    }
    BOOST_REQUIRE(SeedIbdHeaderSchedulerHeadersForTesting(peerA.GetId(), headers));
    for (int i = 1; i <= 512; ++i)
        SeedIbdHeaderSchedulerInvAvailabilityForTesting(
            peerA.GetId(), uint256(7000000 + i));

    std::vector<CNode*> peers;
    peers.push_back(&peerA);
    peers.push_back(&peerB);
    const int64_t activeBefore =
        ibdmetrics::Get().global_active_current.load(std::memory_order_relaxed);

    // Saturate: 512 queued ordered requests on branch A.
    BOOST_REQUIRE_EQUAL(RefillOrderedHeaderBlockRequestsForTesting(peers), 512U);
    BOOST_CHECK_EQUAL(peerA.setAskForBlocks.size(), 512U);
    BOOST_CHECK_EQUAL(
        ibdmetrics::Get().global_active_current.load(std::memory_order_relaxed) -
            activeBefore, 512LL);

    // Send-transition every queued old-generation hash to IN_FLIGHT exactly as
    // the getdata send loop does: assign owner, mark in flight, then remove the
    // askfor entry from the queue accounting (SENT_TRANSITION, no owner release).
    while (!peerA.mapAskFor.empty())
    {
        const uint256 hash = peerA.mapAskFor.begin()->second.hash;
        BOOST_REQUIRE(TryAssignBlockRequestOwner(
            hash, peerA.GetId(), BLOCKREQ_SOURCE_ASKFOR));
        peerA.MarkBlockInFlight(hash);
        peerA.EraseAskForEntry(
            peerA.mapAskFor.begin(), false,
            ibdmetrics::ACTIVE_DECREMENT_ASKFOR_SENT_TRANSITION);
    }
    BOOST_CHECK_EQUAL(peerA.setAskForBlocks.size(), 0U);
    BOOST_CHECK_EQUAL(peerA.setBlocksInFlight.size(), 512U);
    BOOST_CHECK_EQUAL(
        ibdmetrics::Get().global_active_current.load(std::memory_order_relaxed) -
            activeBefore, 512LL);

    // Production full-re-anchor handoff to a disjoint branch (disjoint hash
    // space: 8000000 is not a branch-A header, so Reanchor fully wipes the
    // graph instead of retaining the old active suffix).
    const uint256 c0(8000000);
    BOOST_REQUIRE(SeedIbdHeaderSchedulerAnchorForTesting(c0, 0));
    BOOST_CHECK_EQUAL(RefillOrderedHeaderBlockRequestsForTesting(peers), 0U);
    BOOST_CHECK_EQUAL(peerA.setBlocksInFlight.size(), 512U);

    // New branch headers are observed; the new generation initially receives
    // zero admission budget because all 512 slots are held by stale old-path
    // in-flight requests.
    std::vector<std::pair<uint256, uint256> > newHeaders;
    previous = c0;
    for (int i = 1; i <= 16; ++i)
    {
        const uint256 hash(8000000 + i);
        newHeaders.push_back(std::make_pair(hash, previous));
        previous = hash;
    }
    BOOST_REQUIRE(SeedIbdHeaderSchedulerHeadersForTesting(peerB.GetId(), newHeaders));
    for (int i = 1; i <= 16; ++i)
        SeedIbdHeaderSchedulerInvAvailabilityForTesting(
            peerB.GetId(), uint256(8000000 + i));
    BOOST_CHECK_EQUAL(RefillOrderedHeaderBlockRequestsForTesting(peers), 0U);
    BOOST_CHECK_EQUAL(peerB.setAskForBlocks.size(), 0U);
    BOOST_CHECK_EQUAL(
        ibdmetrics::Get().global_active_current.load(std::memory_order_relaxed) -
            activeBefore, 512LL);

    // Expire the stale old-path in-flight work through the REAL expiry decision
    // at a simulated 61 s wire age (> 60 s wire-origin deadline).  After the
    // full re-anchor the old hashes are no longer graph-resident and their
    // progress baselines were pruned, so the ordered decision must NOT defer
    // them to the 300 s ceiling: they expire immediately.
    const int64_t nNowUs = GetTimeMicros();
    {
        LOCK(peerA.cs_vBlockInFlightWire);
        for (std::set<uint256>::const_iterator si = peerA.setBlocksInFlight.begin();
             si != peerA.setBlocksInFlight.end(); ++si)
            peerA.mapBlockInFlightWireUs[*si] = nNowUs - 61LL * 1000000;
    }
    const IbdHeaderSchedulerRefillStats statsBefore =
        GetIbdHeaderSchedulerRefillStatsForTesting();
    peerA.ExpireBlockInFlight(nNowUs);
    const IbdHeaderSchedulerRefillStats statsAfter =
        GetIbdHeaderSchedulerRefillStatsForTesting();
    // The stale old-path requests must NOT receive the 300 s progress defer:
    // after the full re-anchor they are not graph-resident ordered descendants,
    // so the ordered expiry decision never defers them (no defer counter move).
    BOOST_CHECK_EQUAL(
        statsAfter.orderedExpiryDeferredDueProgress -
            statsBefore.orderedExpiryDeferredDueProgress, 0U);
    BOOST_CHECK_EQUAL(peerA.setBlocksInFlight.size(), 0U);
    BOOST_CHECK_EQUAL(peerA.mapBlockInFlightSince.size(), 0U);
    BOOST_CHECK_EQUAL(peerA.setAskForBlocks.size(), 0U);
    {
        BOOST_CHECK(!GetBlockRequestOwner(uint256(7000001), NULL, NULL));
        BOOST_CHECK(!GetBlockRequestOwner(uint256(7000512), NULL, NULL));
    }
    BOOST_CHECK_EQUAL(
        ibdmetrics::Get().global_active_current.load(std::memory_order_relaxed) -
            activeBefore, 0LL);

    // The budget is now free: the new generation is admitted.
    BOOST_CHECK_EQUAL(RefillOrderedHeaderBlockRequestsForTesting(peers), 16U);
    BOOST_CHECK_EQUAL(peerB.setAskForBlocks.size(), 16U);
    BOOST_CHECK_EQUAL(
        ibdmetrics::Get().global_active_current.load(std::memory_order_relaxed) -
            activeBefore, 16LL);

    // Repeated same-branch refill is idempotent.
    BOOST_CHECK_EQUAL(RefillOrderedHeaderBlockRequestsForTesting(peers), 0U);
    BOOST_CHECK_EQUAL(peerB.setAskForBlocks.size(), 16U);

    peerA.ClearAskFor();
    peerB.ClearAskFor();
    const int64_t activeAfter =
        ibdmetrics::Get().global_active_current.load(std::memory_order_relaxed);
    BOOST_CHECK_EQUAL(activeAfter - activeBefore, 0LL);

    ResetIbdHeaderSchedulerStateForTesting();
    fSPVMode = savedSpv;
    fIbdHeadersObserve = savedObserve;
    fIbdHeaderScheduler = savedScheduler;

    // Restore the effective per-peer window so later tests see the default.
    if (fHadPerPeerArg)
        mapArgs["-ibdmaxactiveperpeer"] = strPerPeerArgSaved;
    else
        mapArgs.erase("-ibdmaxactiveperpeer");
    ResetMaxActiveBlockRequestsPerPeerConfigForTesting();
}

BOOST_AUTO_TEST_CASE(headers_scheduler_descendant_timeout_recovery_without_full_refill)
{
    const bool savedScheduler = fIbdHeaderScheduler;
    const bool savedObserve = fIbdHeadersObserve;
    const bool savedSpv = fSPVMode;
    fIbdHeaderScheduler = true;
    fIbdHeadersObserve = false;
    fSPVMode = false;
    ResetIbdHeaderSchedulerStateForTesting();

    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CNode peer(INVALID_SOCKET, TestPeerAddress(95), "sched-timeout", true);
    PreparePeerForSendMessages(peer, PROTOCOL_VERSION);
    CScopedInitialBlockDownloadState ibdState(&peer);

    const uint256 anchor(4000000);
    BOOST_REQUIRE(SeedIbdHeaderSchedulerAnchorForTesting(anchor, 0));
    std::vector<std::pair<uint256, uint256> > headers;
    headers.reserve(600);
    uint256 previous = anchor;
    for (int i = 1; i <= 600; ++i)
    {
        const uint256 hash(4000000 + i);
        headers.push_back(std::make_pair(hash, previous));
        previous = hash;
    }
    BOOST_REQUIRE(SeedIbdHeaderSchedulerHeadersForTesting(peer.GetId(), headers));
    for (int i = 1; i <= 600; ++i)
        SeedIbdHeaderSchedulerInvAvailabilityForTesting(peer.GetId(), uint256(4000000 + i));

    std::vector<CNode*> peers(1, &peer);
    BOOST_CHECK_EQUAL(RefillOrderedHeaderBlockRequestsForTesting(peers), 128U);
    BOOST_CHECK_EQUAL(peer.setAskForBlocks.size(), 128U);

    const uint256 timedOut(4000002);
    BOOST_REQUIRE(TryAssignBlockRequestOwner(timedOut, peer.GetId(), BLOCKREQ_SOURCE_ASKFOR));
    for (std::multimap<int64_t, CInv>::iterator it = peer.mapAskFor.begin();
         it != peer.mapAskFor.end(); ++it)
    {
        if (it->second.hash == timedOut)
        {
            peer.EraseAskForEntry(it, false);
            break;
        }
    }
    BOOST_REQUIRE(ReleaseBlockRequestOwner(timedOut, peer.GetId(), "timeout"));
    RecordBlockLastTimeoutOwner(timedOut, peer.GetId());
    BOOST_CHECK(!GetIbdHeaderSchedulerRefillStatsForTesting().cursorInvalidated);
    BOOST_CHECK(GetIbdHeaderSchedulerRefillStatsForTesting().cursorRecoveryNeeded);
    BOOST_CHECK_EQUAL(peer.setAskForBlocks.size(), 127U);

    BOOST_CHECK_EQUAL(RefillOrderedHeaderBlockRequestsForTesting(peers), 1U);
    BOOST_CHECK(peer.IsBlockAskForQueued(timedOut));
    BOOST_CHECK_EQUAL(peer.setAskForBlocks.size(), 128U);
    BOOST_CHECK(peer.IsBlockAskForQueued(uint256(4000001)));
    BOOST_CHECK(peer.IsBlockAskForQueued(uint256(4000003)));

    const IbdHeaderSchedulerRefillStats stats =
        GetIbdHeaderSchedulerRefillStatsForTesting();
    // The recovery round no longer returns before the round gate: recovery
    // re-admits the released hash and then FALLS THROUGH to the round-gated
    // full refill (which finds every prefix entry covered and admits 0).  So
    // the full refill has run twice: once for the initial admission, once for
    // the recovery round.
    BOOST_CHECK_EQUAL(stats.fullRefillCalls, 2U);
    BOOST_CHECK(!stats.cursorInvalidated);
    BOOST_CHECK(!stats.cursorRecoveryNeeded);

    BOOST_CHECK_EQUAL(RefillOrderedHeaderBlockRequestsForTesting(peers), 0U);
    BOOST_CHECK_EQUAL(peer.setAskForBlocks.size(), 128U);

    peer.ClearAskFor();
    ResetIbdHeaderSchedulerStateForTesting();
    fSPVMode = savedSpv;
    fIbdHeadersObserve = savedObserve;
    fIbdHeaderScheduler = savedScheduler;
}

BOOST_AUTO_TEST_CASE(headers_scheduler_recovery_allowed_with_incremental_backlog)
{
    const bool savedScheduler = fIbdHeaderScheduler;
    const bool savedObserve = fIbdHeadersObserve;
    const bool savedSpv = fSPVMode;
    fIbdHeaderScheduler = true;
    fIbdHeadersObserve = false;
    fSPVMode = false;
    ResetIbdHeaderSchedulerStateForTesting();

    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CNode peer(INVALID_SOCKET, TestPeerAddress(97), "sched-recv-backlog", true);
    PreparePeerForSendMessages(peer, PROTOCOL_VERSION);
    CScopedInitialBlockDownloadState ibdState(&peer);

    const uint256 anchor(6000000);
    BOOST_REQUIRE(SeedIbdHeaderSchedulerAnchorForTesting(anchor, 0));
    // Keep the window small (6 descendants) so the peer's request budget is
    // never exhausted: the recovery scan must be able to re-admit released
    // hashes even while an incremental backlog is pending.
    std::vector<std::pair<uint256, uint256> > headers;
    headers.reserve(6);
    uint256 previous = anchor;
    for (int i = 1; i <= 6; ++i)
    {
        const uint256 hash(6000000 + i);
        headers.push_back(std::make_pair(hash, previous));
        previous = hash;
    }
    BOOST_REQUIRE(SeedIbdHeaderSchedulerHeadersForTesting(peer.GetId(), headers));
    for (int i = 1; i <= 6; ++i)
        SeedIbdHeaderSchedulerInvAvailabilityForTesting(peer.GetId(), uint256(6000000 + i));

    std::vector<CNode*> peers(1, &peer);
    // Full refill admits the whole 6-hash window (positions 0..5).
    BOOST_CHECK_EQUAL(RefillOrderedHeaderBlockRequestsForTesting(peers), 6U);
    BOOST_CHECK_EQUAL(peer.setAskForBlocks.size(), 6U);

    // Simulate send + timeout for two prefix hashes: the front "hole"
    // (position 3, height 4) and a second one (position 4, height 5).
    const uint256 hole(6000004);
    const uint256 secondHole(6000005);
    for (int i = 0; i < 2; ++i)
    {
        const uint256 hash = i == 0 ? hole : secondHole;
        BOOST_REQUIRE(TryAssignBlockRequestOwner(hash, peer.GetId(), BLOCKREQ_SOURCE_ASKFOR));
        for (std::multimap<int64_t, CInv>::iterator it = peer.mapAskFor.begin();
             it != peer.mapAskFor.end(); ++it)
        {
            if (it->second.hash == hash)
            {
                peer.EraseAskForEntry(it, false);
                break;
            }
        }
        BOOST_REQUIRE(ReleaseBlockRequestOwner(hash, peer.GetId(), "timeout"));
    }
    BOOST_CHECK(GetIbdHeaderSchedulerRefillStatsForTesting().cursorRecoveryNeeded);
    BOOST_CHECK_EQUAL(peer.setAskForBlocks.size(), 4U);

    // Advance the frontier by 2 (height 0 -> 2).  The cursor shift creates a
    // pending incremental backlog: the window shrinks to the graph tip, the
    // incremental entry is past the end (cursorNextIndex 4 == window size 4),
    // so no slot is admitted and cursorPendingSlots stays at 2, while both
    // released holes remain in the claimed prefix [0, cursorNextIndex).
    BOOST_REQUIRE(SeedIbdHeaderSchedulerAnchorForTesting(uint256(6000002), 2));
    BOOST_CHECK_EQUAL(RefillOrderedHeaderBlockRequestsForTesting(peers), 0U);
    BOOST_CHECK(!peer.IsBlockAskForQueued(hole));
    BOOST_CHECK_EQUAL(peer.setAskForBlocks.size(), 4U);
    IbdHeaderSchedulerRefillStats stats = GetIbdHeaderSchedulerRefillStatsForTesting();
    BOOST_CHECK_EQUAL(stats.cursorPendingSlots, 2U);
    BOOST_CHECK_EQUAL(stats.cursorNextIndex, 4U);
    BOOST_CHECK(stats.cursorRecoveryNeeded);

    // The next round must run the bounded recovery scan even though a pending
    // incremental backlog exists, re-admitting both released hashes without
    // touching the cursor.  (Pre-fix, the recovery gate required
    // cursorPendingSlots == 0 and the pipeline stalled on the holes forever.)
    BOOST_CHECK_EQUAL(RefillOrderedHeaderBlockRequestsForTesting(peers), 2U);
    BOOST_CHECK(peer.IsBlockAskForQueued(hole));
    BOOST_CHECK(peer.IsBlockAskForQueued(secondHole));
    BOOST_CHECK_EQUAL(peer.setAskForBlocks.size(), 6U);
    stats = GetIbdHeaderSchedulerRefillStatsForTesting();
    BOOST_CHECK_EQUAL(stats.cursorPendingSlots, 2U);
    BOOST_CHECK_EQUAL(stats.cursorNextIndex, 4U);
    BOOST_CHECK(!stats.cursorRecoveryNeeded);
    BOOST_CHECK(!stats.cursorInvalidated);

    peer.ClearAskFor();
    ResetIbdHeaderSchedulerStateForTesting();
    fSPVMode = savedSpv;
    fIbdHeadersObserve = savedObserve;
    fIbdHeaderScheduler = savedScheduler;
}

BOOST_AUTO_TEST_CASE(headers_scheduler_unresolved_prefix_hole_no_candidate_keeps_rounds_running)
{
    // Regression: an unresolved released prefix hole (its only source peer
    // disconnected -> no candidate) must not livelock the ordered scheduler.
    // Round after round the recovery scan re-runs over the hole without being
    // able to re-admit it; the fixed control flow still FALLS THROUGH to the
    // round-gated full refill every round (fullRefillCalls keeps advancing)
    // and parks the cursor at the first uncovered prefix position.  When a
    // source appears the next round's refill re-covers the prefix and the
    // cursor resumes.
    const bool savedScheduler = fIbdHeaderScheduler;
    const bool savedObserve = fIbdHeadersObserve;
    const bool savedSpv = fSPVMode;
    fIbdHeaderScheduler = true;
    fIbdHeadersObserve = false;
    fSPVMode = false;
    ResetIbdHeaderSchedulerStateForTesting();

    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CNode peerA(INVALID_SOCKET, TestPeerAddress(110), "sched-nocand-a", true);
    CNode peerB(INVALID_SOCKET, TestPeerAddress(111), "sched-nocand-b", true);
    PreparePeerForSendMessages(peerA, PROTOCOL_VERSION);
    PreparePeerForSendMessages(peerB, PROTOCOL_VERSION);
    CScopedInitialBlockDownloadState ibdState(&peerA);

    const uint256 anchor(7000000);
    BOOST_REQUIRE(SeedIbdHeaderSchedulerAnchorForTesting(anchor, 0));
    std::vector<std::pair<uint256, uint256> > headersA;
    uint256 previous = anchor;
    for (int i = 1; i <= 64; ++i)
    {
        const uint256 hash(7000000 + i);
        headersA.push_back(std::make_pair(hash, previous));
        previous = hash;
    }
    BOOST_REQUIRE(SeedIbdHeaderSchedulerHeadersForTesting(peerA.GetId(), headersA));
    // B announces only 5..64: blocks 1..4 keep A as their only header source.
    // No inv availability anywhere, so candidates come from the graph alone.
    std::vector<std::pair<uint256, uint256> > headersB;
    previous = uint256(7000004);
    for (int i = 5; i <= 64; ++i)
    {
        const uint256 hash(7000000 + i);
        headersB.push_back(std::make_pair(hash, previous));
        previous = hash;
    }
    BOOST_REQUIRE(SeedIbdHeaderSchedulerHeadersForTesting(peerB.GetId(), headersB));

    std::vector<CNode*> peers(1, &peerA);
    BOOST_CHECK_EQUAL(RefillOrderedHeaderBlockRequestsForTesting(peers), 64U);
    BOOST_CHECK_EQUAL(peerA.setAskForBlocks.size(), 64U);
    IbdHeaderSchedulerRefillStats stats = GetIbdHeaderSchedulerRefillStatsForTesting();
    BOOST_CHECK_EQUAL(stats.fullRefillCalls, 1U);
    BOOST_CHECK_EQUAL(stats.cursorNextIndex, 64U);
    BOOST_CHECK(!stats.cursorRecoveryNeeded);

    // A was the only source of the 4-block front prefix; drop it from the
    // header observer so the prefix becomes a structural hole with no
    // candidate peer, then release A's claimed hashes.
    IbdHeadersObserverPeerDisconnected(peerA.GetId());
    for (int i = 1; i <= 4; ++i)
    {
        const uint256 hash(7000000 + i);
        BOOST_REQUIRE(TryAssignBlockRequestOwner(hash, peerA.GetId(), BLOCKREQ_SOURCE_ASKFOR));
        for (std::multimap<int64_t, CInv>::iterator it = peerA.mapAskFor.begin();
             it != peerA.mapAskFor.end(); ++it)
        {
            if (it->second.hash == hash)
            {
                peerA.EraseAskForEntry(it, false);
                break;
            }
        }
        BOOST_REQUIRE(ReleaseBlockRequestOwner(hash, peerA.GetId(), "timeout"));
    }
    BOOST_CHECK(GetIbdHeaderSchedulerRefillStatsForTesting().cursorRecoveryNeeded);
    BOOST_CHECK_EQUAL(peerA.setAskForBlocks.size(), 60U);

    // The hole has no candidate, so recovery cannot re-admit it and the flag
    // must stay true (recovery does not clear it merely to force progress).
    // The fix: the recovery pass then falls through to the round gate, so the
    // round-gated full refill still runs (fullRefillCalls 1 -> 2) and the
    // cursor retreats to the first uncovered prefix position (0) instead of
    // parking at a stale 64.  Pre-fix this round returned before the round
    // gate: fullRefillCalls stayed 1 and the cursor stayed at 64.
    BOOST_CHECK_EQUAL(RefillOrderedHeaderBlockRequestsForTesting(peers), 0U);
    stats = GetIbdHeaderSchedulerRefillStatsForTesting();
    BOOST_CHECK(stats.cursorRecoveryNeeded);
    BOOST_CHECK_EQUAL(stats.fullRefillCalls, 2U);
    BOOST_CHECK_EQUAL(stats.cursorNextIndex, 0U);
    BOOST_CHECK(!peerA.IsBlockAskForQueued(uint256(7000001)));
    BOOST_CHECK_EQUAL(peerA.setAskForBlocks.size(), 60U);

    // Rounds keep running while the hole is unresolved: the full refill runs
    // again this round.  Pre-fix, this round returned at the recovery gate
    // (fullRefillCalls stayed 2).
    BOOST_CHECK_EQUAL(RefillOrderedHeaderBlockRequestsForTesting(peers), 0U);
    stats = GetIbdHeaderSchedulerRefillStatsForTesting();
    BOOST_CHECK_EQUAL(stats.fullRefillCalls, 3U);
    BOOST_CHECK_EQUAL(stats.cursorNextIndex, 0U);

    // A source appears (inv availability on B).  The next round's refill
    // re-covers the whole prefix and the cursor resumes; the old code would
    // only reach this refill after the hole had already resolved.
    for (int i = 1; i <= 4; ++i)
        SeedIbdHeaderSchedulerInvAvailabilityForTesting(peerB.GetId(), uint256(7000000 + i));
    std::vector<CNode*> peersAB;
    peersAB.push_back(&peerA);
    peersAB.push_back(&peerB);
    BOOST_CHECK_EQUAL(RefillOrderedHeaderBlockRequestsForTesting(peersAB), 4U);
    stats = GetIbdHeaderSchedulerRefillStatsForTesting();
    BOOST_CHECK(!stats.cursorRecoveryNeeded);
    BOOST_CHECK_EQUAL(stats.fullRefillCalls, 4U);
    BOOST_CHECK_EQUAL(stats.cursorNextIndex, 64U);
    for (int i = 1; i <= 4; ++i)
        BOOST_CHECK(peerB.IsBlockAskForQueued(uint256(7000000 + i)));

    peerA.ClearAskFor();
    peerB.ClearAskFor();
    ResetIbdHeaderSchedulerStateForTesting();
    fSPVMode = savedSpv;
    fIbdHeadersObserve = savedObserve;
    fIbdHeaderScheduler = savedScheduler;
}

BOOST_AUTO_TEST_CASE(headers_scheduler_unresolved_prefix_hole_budget_exhausted_keeps_rounds_running)
{
    // Regression: an unresolved released prefix hole whose peer's request
    // budget is exhausted must not livelock the ordered scheduler.  Recovery
    // cannot re-admit the hash (budget == 0) so cursorRecoveryNeeded stays
    // true, but the fixed control flow still falls through to the round-gated
    // full refill which retreats the cursor to the hole; once the budget
    // returns the refill re-admits the hole and the cursor resumes.
    const bool savedScheduler = fIbdHeaderScheduler;
    const bool savedObserve = fIbdHeadersObserve;
    const bool savedSpv = fSPVMode;
    fIbdHeaderScheduler = true;
    fIbdHeadersObserve = false;
    fSPVMode = false;
    ResetIbdHeaderSchedulerStateForTesting();

    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CNode peer(INVALID_SOCKET, TestPeerAddress(112), "sched-nobudget", true);
    PreparePeerForSendMessages(peer, PROTOCOL_VERSION);
    CScopedInitialBlockDownloadState ibdState(&peer);

    const uint256 anchor(7500000);
    BOOST_REQUIRE(SeedIbdHeaderSchedulerAnchorForTesting(anchor, 0));
    std::vector<std::pair<uint256, uint256> > headers;
    uint256 previous = anchor;
    for (int i = 1; i <= 600; ++i)
    {
        const uint256 hash(7500000 + i);
        headers.push_back(std::make_pair(hash, previous));
        previous = hash;
    }
    BOOST_REQUIRE(SeedIbdHeaderSchedulerHeadersForTesting(peer.GetId(), headers));
    for (int i = 1; i <= 600; ++i)
        SeedIbdHeaderSchedulerInvAvailabilityForTesting(peer.GetId(), uint256(7500000 + i));

    std::vector<CNode*> peers(1, &peer);
    BOOST_CHECK_EQUAL(RefillOrderedHeaderBlockRequestsForTesting(peers), 128U);
    BOOST_CHECK_EQUAL(peer.setAskForBlocks.size(), 128U);

    // Release the front hash and saturate the peer's request budget with an
    // unrelated in-flight hash so recovery cannot re-admit it.
    const uint256 hole(7500001);
    BOOST_REQUIRE(TryAssignBlockRequestOwner(hole, peer.GetId(), BLOCKREQ_SOURCE_ASKFOR));
    for (std::multimap<int64_t, CInv>::iterator it = peer.mapAskFor.begin();
         it != peer.mapAskFor.end(); ++it)
    {
        if (it->second.hash == hole)
        {
            peer.EraseAskForEntry(it, false);
            break;
        }
    }
    BOOST_REQUIRE(ReleaseBlockRequestOwner(hole, peer.GetId(), "timeout"));
    const uint256 inflight(9500000);
    peer.MarkBlockInFlight(inflight);
    BOOST_CHECK(GetIbdHeaderSchedulerRefillStatsForTesting().cursorRecoveryNeeded);
    BOOST_CHECK_EQUAL(peer.setAskForBlocks.size(), 127U);

    // Recovery hits budget == 0, keeps cursorRecoveryNeeded true, and the
    // fall-through full refill still runs (fullRefillCalls 1 -> 2) and parks
    // the cursor at the hole.  Pre-fix this round returned at the recovery
    // gate: fullRefillCalls stayed 1 and the cursor stayed at 128.
    BOOST_CHECK_EQUAL(RefillOrderedHeaderBlockRequestsForTesting(peers), 0U);
    IbdHeaderSchedulerRefillStats stats = GetIbdHeaderSchedulerRefillStatsForTesting();
    BOOST_CHECK(stats.cursorRecoveryNeeded);
    BOOST_CHECK_EQUAL(stats.fullRefillCalls, 2U);
    BOOST_CHECK_EQUAL(stats.cursorNextIndex, 0U);
    BOOST_CHECK(!peer.IsBlockAskForQueued(hole));

    // Budget returns (in-flight cleared): recovery has nothing left to scan
    // (cursorNextIndex 0), falls through to the full refill which re-admits
    // the hole and advances the cursor to the covered prefix.
    peer.ClearBlockInFlight(inflight);
    BOOST_CHECK_EQUAL(RefillOrderedHeaderBlockRequestsForTesting(peers), 1U);
    stats = GetIbdHeaderSchedulerRefillStatsForTesting();
    BOOST_CHECK(!stats.cursorRecoveryNeeded);
    BOOST_CHECK_EQUAL(stats.fullRefillCalls, 3U);
    BOOST_CHECK_EQUAL(stats.cursorNextIndex, 128U);
    BOOST_CHECK(peer.IsBlockAskForQueued(hole));

    peer.ClearAskFor();
    ResetIbdHeaderSchedulerStateForTesting();
    fSPVMode = savedSpv;
    fIbdHeadersObserve = savedObserve;
    fIbdHeaderScheduler = savedScheduler;
}

BOOST_AUTO_TEST_CASE(headers_scheduler_unresolved_prefix_hole_askfor_refusal_keeps_rounds_running)
{
    // Regression: an unresolved released prefix hole whose re-admission is
    // refused by AskFor (hash still marked in flight) must not livelock the
    // ordered scheduler.  Same shape as the budget-exhausted case: recovery
    // keeps the flag true, the fall-through full refill keeps running and
    // parks the cursor at the hole, and progress resumes once the refusal
    // clears.
    const bool savedScheduler = fIbdHeaderScheduler;
    const bool savedObserve = fIbdHeadersObserve;
    const bool savedSpv = fSPVMode;
    fIbdHeaderScheduler = true;
    fIbdHeadersObserve = false;
    fSPVMode = false;
    ResetIbdHeaderSchedulerStateForTesting();

    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CNode peer(INVALID_SOCKET, TestPeerAddress(113), "sched-refused", true);
    PreparePeerForSendMessages(peer, PROTOCOL_VERSION);
    CScopedInitialBlockDownloadState ibdState(&peer);

    const uint256 anchor(8000000);
    BOOST_REQUIRE(SeedIbdHeaderSchedulerAnchorForTesting(anchor, 0));
    std::vector<std::pair<uint256, uint256> > headers;
    uint256 previous = anchor;
    for (int i = 1; i <= 600; ++i)
    {
        const uint256 hash(8000000 + i);
        headers.push_back(std::make_pair(hash, previous));
        previous = hash;
    }
    BOOST_REQUIRE(SeedIbdHeaderSchedulerHeadersForTesting(peer.GetId(), headers));
    for (int i = 1; i <= 600; ++i)
        SeedIbdHeaderSchedulerInvAvailabilityForTesting(peer.GetId(), uint256(8000000 + i));

    std::vector<CNode*> peers(1, &peer);
    BOOST_CHECK_EQUAL(RefillOrderedHeaderBlockRequestsForTesting(peers), 128U);
    BOOST_CHECK_EQUAL(peer.setAskForBlocks.size(), 128U);

    // Release two prefix hashes and mark the front one in flight: recovery
    // has budget (127 pressure) but AskFor refuses the front hash.
    const uint256 hole(8000001);
    const uint256 secondHole(8000002);
    for (int i = 0; i < 2; ++i)
    {
        const uint256 hash = i == 0 ? hole : secondHole;
        BOOST_REQUIRE(TryAssignBlockRequestOwner(hash, peer.GetId(), BLOCKREQ_SOURCE_ASKFOR));
        for (std::multimap<int64_t, CInv>::iterator it = peer.mapAskFor.begin();
             it != peer.mapAskFor.end(); ++it)
        {
            if (it->second.hash == hash)
            {
                peer.EraseAskForEntry(it, false);
                break;
            }
        }
        BOOST_REQUIRE(ReleaseBlockRequestOwner(hash, peer.GetId(), "timeout"));
    }
    peer.MarkBlockInFlight(hole);
    BOOST_CHECK(GetIbdHeaderSchedulerRefillStatsForTesting().cursorRecoveryNeeded);
    BOOST_CHECK_EQUAL(peer.setAskForBlocks.size(), 126U);

    // Recovery budget is 1 but AskFor returns ASKFOR_INFLIGHT for the front
    // hole: cursorRecoveryNeeded stays true and the fall-through full refill
    // still runs (fullRefillCalls 1 -> 2), parking the cursor at the hole.
    BOOST_CHECK_EQUAL(RefillOrderedHeaderBlockRequestsForTesting(peers), 0U);
    IbdHeaderSchedulerRefillStats stats = GetIbdHeaderSchedulerRefillStatsForTesting();
    BOOST_CHECK(stats.cursorRecoveryNeeded);
    BOOST_CHECK_EQUAL(stats.fullRefillCalls, 2U);
    BOOST_CHECK_EQUAL(stats.cursorNextIndex, 0U);
    BOOST_CHECK(!peer.IsBlockAskForQueued(hole));

    // The refusal clears: recovery (nothing to scan, cursor at 0) falls
    // through to the full refill which re-admits both released hashes.
    peer.ClearBlockInFlight(hole);
    BOOST_CHECK_EQUAL(RefillOrderedHeaderBlockRequestsForTesting(peers), 2U);
    stats = GetIbdHeaderSchedulerRefillStatsForTesting();
    BOOST_CHECK(!stats.cursorRecoveryNeeded);
    BOOST_CHECK_EQUAL(stats.fullRefillCalls, 3U);
    BOOST_CHECK_EQUAL(stats.cursorNextIndex, 128U);
    BOOST_CHECK(peer.IsBlockAskForQueued(hole));
    BOOST_CHECK(peer.IsBlockAskForQueued(secondHole));

    peer.ClearAskFor();
    ResetIbdHeaderSchedulerStateForTesting();
    fSPVMode = savedSpv;
    fIbdHeadersObserve = savedObserve;
    fIbdHeaderScheduler = savedScheduler;
}

BOOST_AUTO_TEST_CASE(headers_scheduler_cursor_stops_at_first_uncovered_prefix_position)
{
    // Pins the cursor-advance rule in full refill: the ordered cursor stops
    // at the FIRST prefix position whose request cannot be made, while tail
    // work beyond a budget-saturated peer may still be admitted to other
    // peers.  Block 1 can only be served by P1 (inv preference) whose request
    // budget is saturated, so the cursor must park at position 0 even though
    // 128 later hashes flow to P2.  Pre-fix the cursor advanced to
    // window.size() (512) and skipped straight past the uncovered prefix.
    const bool savedScheduler = fIbdHeaderScheduler;
    const bool savedObserve = fIbdHeadersObserve;
    const bool savedSpv = fSPVMode;
    fIbdHeaderScheduler = true;
    fIbdHeadersObserve = false;
    fSPVMode = false;
    ResetIbdHeaderSchedulerStateForTesting();

    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CNode peerA(INVALID_SOCKET, TestPeerAddress(114), "sched-prefix-a", true);
    CNode peerB(INVALID_SOCKET, TestPeerAddress(115), "sched-prefix-b", true);
    PreparePeerForSendMessages(peerA, PROTOCOL_VERSION);
    PreparePeerForSendMessages(peerB, PROTOCOL_VERSION);
    CScopedInitialBlockDownloadState ibdState(&peerA);

    const uint256 anchor(8500000);
    BOOST_REQUIRE(SeedIbdHeaderSchedulerAnchorForTesting(anchor, 0));
    std::vector<std::pair<uint256, uint256> > headers;
    uint256 previous = anchor;
    for (int i = 1; i <= 600; ++i)
    {
        const uint256 hash(8500000 + i);
        headers.push_back(std::make_pair(hash, previous));
        previous = hash;
    }
    BOOST_REQUIRE(SeedIbdHeaderSchedulerHeadersForTesting(peerA.GetId(), headers));
    BOOST_REQUIRE(SeedIbdHeaderSchedulerHeadersForTesting(peerB.GetId(), headers));
    // Block 1 is best served by A only; blocks 2+ by B only.
    SeedIbdHeaderSchedulerInvAvailabilityForTesting(peerA.GetId(), uint256(8500001));
    for (int i = 2; i <= 600; ++i)
        SeedIbdHeaderSchedulerInvAvailabilityForTesting(peerB.GetId(), uint256(8500000 + i));

    // Saturate A's request budget with unrelated in-flight hashes.
    for (int i = 0; i < 128; ++i)
        peerA.MarkBlockInFlight(uint256(95000000 + i));

    std::vector<CNode*> peers;
    peers.push_back(&peerA);
    peers.push_back(&peerB);
    BOOST_CHECK_EQUAL(RefillOrderedHeaderBlockRequestsForTesting(peers), 128U);
    BOOST_CHECK_EQUAL(peerA.setAskForBlocks.size(), 0U);
    BOOST_CHECK_EQUAL(peerB.setAskForBlocks.size(), 128U);
    IbdHeaderSchedulerRefillStats stats = GetIbdHeaderSchedulerRefillStatsForTesting();
    BOOST_CHECK_EQUAL(stats.cursorNextIndex, 0U);
    BOOST_CHECK(!peerB.IsBlockAskForQueued(uint256(8500001)));
    BOOST_CHECK(peerB.IsBlockAskForQueued(uint256(8500002)));
    BOOST_CHECK(peerB.IsBlockAskForQueued(uint256(8500129)));

    // A's budget returns: the refill admits block 1 to A and the cursor
    // advances to the now-covered prefix.
    for (int i = 0; i < 128; ++i)
        peerA.ClearBlockInFlight(uint256(95000000 + i));
    BOOST_CHECK_EQUAL(RefillOrderedHeaderBlockRequestsForTesting(peers), 1U);
    stats = GetIbdHeaderSchedulerRefillStatsForTesting();
    BOOST_CHECK_EQUAL(stats.cursorNextIndex, 129U);
    BOOST_CHECK_EQUAL(peerA.setAskForBlocks.size(), 1U);
    BOOST_CHECK(peerA.IsBlockAskForQueued(uint256(8500001)));

    peerA.ClearAskFor();
    peerB.ClearAskFor();
    ResetIbdHeaderSchedulerStateForTesting();
    fSPVMode = savedSpv;
    fIbdHeadersObserve = savedObserve;
    fIbdHeaderScheduler = savedScheduler;
}

BOOST_AUTO_TEST_CASE(headers_scheduler_multipeer_single_cursor_recovery_and_refill_per_round)
{
    // Pins the round ownership invariant across recovery: with two peers both
    // needing recovery in the same round, recovery runs once per peer (each
    // re-admits its own released hash) but the cursor-mutating full refill
    // still runs exactly once per round (the first pass that observes the
    // round).  Pre-fix the recovery branch returned before the round gate, so
    // the first pass never ran the full refill and fullRefillCalls stayed 1.
    const bool savedScheduler = fIbdHeaderScheduler;
    const bool savedObserve = fIbdHeadersObserve;
    const bool savedSpv = fSPVMode;
    fIbdHeaderScheduler = true;
    fIbdHeadersObserve = false;
    fSPVMode = false;
    ResetIbdHeaderSchedulerStateForTesting();

    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CNode peerA(INVALID_SOCKET, TestPeerAddress(116), "sched-multi-a", true);
    CNode peerB(INVALID_SOCKET, TestPeerAddress(117), "sched-multi-b", true);
    PreparePeerForSendMessages(peerA, PROTOCOL_VERSION);
    PreparePeerForSendMessages(peerB, PROTOCOL_VERSION);
    CScopedInitialBlockDownloadState ibdState(&peerA);

    const uint256 anchor(9000000);
    BOOST_REQUIRE(SeedIbdHeaderSchedulerAnchorForTesting(anchor, 0));
    std::vector<std::pair<uint256, uint256> > headers;
    uint256 previous = anchor;
    for (int i = 1; i <= 600; ++i)
    {
        const uint256 hash(9000000 + i);
        headers.push_back(std::make_pair(hash, previous));
        previous = hash;
    }
    BOOST_REQUIRE(SeedIbdHeaderSchedulerHeadersForTesting(peerA.GetId(), headers));
    BOOST_REQUIRE(SeedIbdHeaderSchedulerHeadersForTesting(peerB.GetId(), headers));
    // Block 1 is best served by A only; blocks 2+ by B only.
    SeedIbdHeaderSchedulerInvAvailabilityForTesting(peerA.GetId(), uint256(9000001));
    for (int i = 2; i <= 600; ++i)
        SeedIbdHeaderSchedulerInvAvailabilityForTesting(peerB.GetId(), uint256(9000000 + i));

    std::vector<CNode*> peers;
    peers.push_back(&peerA);
    peers.push_back(&peerB);
    BOOST_CHECK_EQUAL(RefillOrderedHeaderBlockRequestsForTesting(peers), 129U);
    BOOST_CHECK_EQUAL(peerA.setAskForBlocks.size(), 1U);
    BOOST_CHECK_EQUAL(peerB.setAskForBlocks.size(), 128U);
    IbdHeaderSchedulerRefillStats stats = GetIbdHeaderSchedulerRefillStatsForTesting();
    BOOST_CHECK_EQUAL(stats.fullRefillCalls, 1U);
    BOOST_CHECK_EQUAL(stats.cursorNextIndex, 129U);

    // Release each peer's front hash.
    const uint256 holeA(9000001);
    const uint256 holeB(9000002);
    for (int i = 0; i < 2; ++i)
    {
        CNode& owner = i == 0 ? peerA : peerB;
        const uint256 hash = i == 0 ? holeA : holeB;
        BOOST_REQUIRE(TryAssignBlockRequestOwner(hash, owner.GetId(), BLOCKREQ_SOURCE_ASKFOR));
        for (std::multimap<int64_t, CInv>::iterator it = owner.mapAskFor.begin();
             it != owner.mapAskFor.end(); ++it)
        {
            if (it->second.hash == hash)
            {
                owner.EraseAskForEntry(it, false);
                break;
            }
        }
        BOOST_REQUIRE(ReleaseBlockRequestOwner(hash, owner.GetId(), "timeout"));
    }
    BOOST_CHECK(GetIbdHeaderSchedulerRefillStatsForTesting().cursorRecoveryNeeded);

    // Round with two peers: A's pass recovers holeA (its own) and then owns
    // the round; its fall-through full refill re-admits holeB to B.  A's
    // recovery re-examined the whole prefix (holeB is best served by B, so it
    // is skipped, not flagged unresolved) and cleared the flag, so B's pass
    // runs no recovery and is gated out of the refill.  The cursor-mutating
    // full refill therefore runs exactly once per round (fullRefillCalls
    // 1 -> 2) while the recovery scan ran once.  Pre-fix the recovery branch
    // returned before the round gate, so A's pass never ran the full refill:
    // holeB stayed released, fullRefillCalls stayed 1, and this round
    // admitted only 1 hash instead of 2.
    BOOST_CHECK_EQUAL(RefillOrderedHeaderBlockRequestsForTesting(peers), 2U);
    stats = GetIbdHeaderSchedulerRefillStatsForTesting();
    BOOST_CHECK(!stats.cursorRecoveryNeeded);
    BOOST_CHECK_EQUAL(stats.fullRefillCalls, 2U);
    BOOST_CHECK_EQUAL(stats.incrementalRefillCalls, 1U);
    BOOST_CHECK_EQUAL(stats.cursorNextIndex, 129U);
    BOOST_CHECK(peerA.IsBlockAskForQueued(holeA));
    BOOST_CHECK(peerB.IsBlockAskForQueued(holeB));

    peerA.ClearAskFor();
    peerB.ClearAskFor();
    ResetIbdHeaderSchedulerStateForTesting();
    fSPVMode = savedSpv;
    fIbdHeadersObserve = savedObserve;
    fIbdHeaderScheduler = savedScheduler;
}

BOOST_AUTO_TEST_CASE(headers_scheduler_structural_disconnect_still_invalidates_cursor)
{
    const bool savedScheduler = fIbdHeaderScheduler;
    const bool savedObserve = fIbdHeadersObserve;
    const bool savedSpv = fSPVMode;
    fIbdHeaderScheduler = true;
    fIbdHeadersObserve = false;
    fSPVMode = false;
    ResetIbdHeaderSchedulerStateForTesting();

    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CNode peer(INVALID_SOCKET, TestPeerAddress(96), "sched-disconnect", true);
    PreparePeerForSendMessages(peer, PROTOCOL_VERSION);
    CScopedInitialBlockDownloadState ibdState(&peer);

    const uint256 anchor(5000000);
    BOOST_REQUIRE(SeedIbdHeaderSchedulerAnchorForTesting(anchor, 0));
    std::vector<std::pair<uint256, uint256> > headers;
    headers.push_back(std::make_pair(uint256(5000001), anchor));
    BOOST_REQUIRE(SeedIbdHeaderSchedulerHeadersForTesting(peer.GetId(), headers));
    SeedIbdHeaderSchedulerInvAvailabilityForTesting(peer.GetId(), uint256(5000001));

    std::vector<CNode*> peers(1, &peer);
    BOOST_CHECK_EQUAL(RefillOrderedHeaderBlockRequestsForTesting(peers), 1U);

    peer.ClearAskFor();
    BOOST_CHECK_EQUAL(ReleaseBlockRequestOwnersForPeer(peer.GetId(), "disconnect"), 0U);
    BOOST_CHECK(GetIbdHeaderSchedulerRefillStatsForTesting().cursorInvalidated);

    BOOST_CHECK_EQUAL(RefillOrderedHeaderBlockRequestsForTesting(peers), 1U);
    BOOST_CHECK_EQUAL(GetIbdHeaderSchedulerRefillStatsForTesting().fullRefillCalls, 2U);

    peer.ClearAskFor();
    ResetIbdHeaderSchedulerStateForTesting();
    fSPVMode = savedSpv;
    fIbdHeadersObserve = savedObserve;
    fIbdHeaderScheduler = savedScheduler;
}

// ---------------------------------------------------------------------------
// Phase 2: progress-aware ordered-IBD expiration.
// ---------------------------------------------------------------------------

// Build an ordered pipeline: anchor 100 @ height 1000, contiguous headers
// 101..200 (heights 1001..1100) with INV availability on `peer`, and run one
// full refill round so the ordered scheduler admits the window prefix and
// records the Phase 2 frontier baseline for each admitted hash.
static void SetupOrderedPipelineForExpiryTest(CNode& peer,
                                              std::vector<CNode*>& peers)
{
    BOOST_REQUIRE(SeedIbdHeaderSchedulerAnchorForTesting(uint256(100), 1000));
    std::vector<std::pair<uint256, uint256> > headers;
    for (int i = 101; i <= 200; ++i)
        headers.push_back(std::make_pair(uint256(i), uint256(i - 1)));
    BOOST_REQUIRE(SeedIbdHeaderSchedulerHeadersForTesting(peer.GetId(), headers));
    for (int i = 101; i <= 200; ++i)
        SeedIbdHeaderSchedulerInvAvailabilityForTesting(peer.GetId(), uint256(i));
    peers.clear();
    peers.push_back(&peer);
    RefillOrderedHeaderBlockRequestsForTesting(peers);
    // Hash 160 (height 1060) is the 60th window entry and was admitted.
    BOOST_REQUIRE(peer.IsBlockAskForQueued(uint256(160)));
}

// 1. A deep ordered descendant whose wire age passed the fixed 60 s deadline
//    is protected while the frontier keeps advancing toward it (but has not
//    reached it).  Owner/inflight survive and no timeout is accounted.
BOOST_AUTO_TEST_CASE(ordered_expiry_descendant_protected_while_not_head)
{
    const bool savedScheduler = fIbdHeaderScheduler;
    const bool savedObserve = fIbdHeadersObserve;
    const bool savedSpv = fSPVMode;
    fIbdHeaderScheduler = true;
    fIbdHeadersObserve = false;
    fSPVMode = false;
    ResetIbdHeaderSchedulerStateForTesting();

    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CScopedIbdQualityState scope;
    CNode peer(INVALID_SOCKET, TestPeerAddress(3001), "expiry-progress", true);
    PreparePeerForSendMessages(peer, PROTOCOL_VERSION);
    CScopedInitialBlockDownloadState ibdState(&peer);

    std::vector<CNode*> peers;
    SetupOrderedPipelineForExpiryTest(peer, peers);

    const uint256 hashDescendant(160);  // height 1060, admitted at frontier 1000
    BOOST_REQUIRE(TryAssignBlockRequestOwner(
        hashDescendant, peer.GetId(), BLOCKREQ_SOURCE_ASKFOR));
    peer.MarkBlockInFlight(hashDescendant);
    {
        LOCK(peer.cs_vBlockInFlightWire);
        peer.mapBlockInFlightWireUs[hashDescendant] =
            GetTimeMicros() - 61000000;  // fixed 60 s wire deadline passed
    }

    const int64_t nTimeoutBefore =
        MetricGet(ibdmetrics::Get().peer_quality_timeout_outcomes);
    const uint64_t nDeferredBefore =
        GetIbdHeaderSchedulerRefillStatsForTesting()
            .orderedExpiryDeferredDueProgress;

    // Frontier advances 40 blocks to 1040 without yet reaching 1060.
    BOOST_REQUIRE(SeedIbdHeaderSchedulerAnchorForTesting(uint256(140), 1040));
    peer.ExpireBlockInFlight();

    BOOST_CHECK_EQUAL(peer.setBlocksInFlight.count(hashDescendant), 1U);
    BOOST_CHECK_EQUAL(peer.mapBlockInFlightSince.count(hashDescendant), 1U);
    BOOST_CHECK(IsBlockRequestOwnedByAnyPeer(hashDescendant));
    BOOST_CHECK(!WasBlockLastTimedOutByPeer(hashDescendant, peer.GetId()));
    BOOST_CHECK_EQUAL(
        MetricGet(ibdmetrics::Get().peer_quality_timeout_outcomes),
        nTimeoutBefore);
    BOOST_CHECK_EQUAL(
        GetIbdHeaderSchedulerRefillStatsForTesting()
            .orderedExpiryDeferredDueProgress,
        nDeferredBefore + 1);

    peer.ClearAskFor();
    ReleaseBlockRequestOwnersForPeer(peer.GetId(), "disconnect");
    ResetIbdHeaderSchedulerStateForTesting();
    fSPVMode = savedSpv;
    fIbdHeadersObserve = savedObserve;
    fIbdHeaderScheduler = savedScheduler;
}

// 2. A request admitted as a descendant loses progress protection once it
//    becomes the current ordered head, even before the 300 s hard ceiling.
BOOST_AUTO_TEST_CASE(ordered_expiry_old_descendant_expires_when_becomes_head)
{
    const bool savedScheduler = fIbdHeaderScheduler;
    const bool savedObserve = fIbdHeadersObserve;
    const bool savedSpv = fSPVMode;
    fIbdHeaderScheduler = true;
    fIbdHeadersObserve = false;
    fSPVMode = false;
    ResetIbdHeaderSchedulerStateForTesting();

    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CScopedIbdQualityState scope;
    CNode peer(INVALID_SOCKET, TestPeerAddress(3010), "expiry-becomes-head", true);
    PreparePeerForSendMessages(peer, PROTOCOL_VERSION);
    CScopedInitialBlockDownloadState ibdState(&peer);

    std::vector<CNode*> peers;
    SetupOrderedPipelineForExpiryTest(peer, peers);

    const uint256 hashOldDescendant(160);  // admitted at frontier 1000
    BOOST_REQUIRE(TryAssignBlockRequestOwner(
        hashOldDescendant, peer.GetId(), BLOCKREQ_SOURCE_ASKFOR));
    peer.MarkBlockInFlight(hashOldDescendant);
    {
        LOCK(peer.cs_vBlockInFlightWire);
        peer.mapBlockInFlightWireUs[hashOldDescendant] =
            GetTimeMicros() - 61000000;
    }

    // Historical progress exists, but H=1060 is now frontier+1.
    BOOST_REQUIRE(SeedIbdHeaderSchedulerAnchorForTesting(uint256(159), 1059));
    peer.ExpireBlockInFlight();

    BOOST_CHECK_EQUAL(peer.setBlocksInFlight.count(hashOldDescendant), 0U);
    BOOST_CHECK(!IsBlockRequestOwnedByAnyPeer(hashOldDescendant));
    BOOST_CHECK(WasBlockLastTimedOutByPeer(hashOldDescendant, peer.GetId()));

    peer.ClearAskFor();
    ReleaseBlockRequestOwnersForPeer(peer.GetId(), "disconnect");
    ResetIbdHeaderSchedulerStateForTesting();
    fSPVMode = savedSpv;
    fIbdHeadersObserve = savedObserve;
    fIbdHeaderScheduler = savedScheduler;
}

// 3. The same old descendant expires when the frontier has not advanced since
//    admission: owner is released, the timeout is accounted honestly, and the
//    ordered expiry diagnostics classify it as a genuine expiry.
BOOST_AUTO_TEST_CASE(ordered_expiry_stalled_frontier_expires)
{
    const bool savedScheduler = fIbdHeaderScheduler;
    const bool savedObserve = fIbdHeadersObserve;
    const bool savedSpv = fSPVMode;
    fIbdHeaderScheduler = true;
    fIbdHeadersObserve = false;
    fSPVMode = false;
    ResetIbdHeaderSchedulerStateForTesting();

    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CScopedIbdQualityState scope;
    CNode peer(INVALID_SOCKET, TestPeerAddress(3002), "expiry-stall", true);
    PreparePeerForSendMessages(peer, PROTOCOL_VERSION);
    CScopedInitialBlockDownloadState ibdState(&peer);

    std::vector<CNode*> peers;
    SetupOrderedPipelineForExpiryTest(peer, peers);

    const uint256 hashDescendant(160);
    BOOST_REQUIRE(TryAssignBlockRequestOwner(
        hashDescendant, peer.GetId(), BLOCKREQ_SOURCE_ASKFOR));
    peer.MarkBlockInFlight(hashDescendant);
    {
        LOCK(peer.cs_vBlockInFlightWire);
        peer.mapBlockInFlightWireUs[hashDescendant] =
            GetTimeMicros() - 61000000;
    }

    const int64_t nTimeoutBefore =
        MetricGet(ibdmetrics::Get().peer_quality_timeout_outcomes);
    const uint64_t nActualBefore =
        GetIbdHeaderSchedulerRefillStatsForTesting().orderedExpiryActual;

    // Frontier never advances: the fixed deadline has passed, so it expires.
    peer.ExpireBlockInFlight();

    BOOST_CHECK_EQUAL(peer.setBlocksInFlight.count(hashDescendant), 0U);
    BOOST_CHECK_EQUAL(peer.mapBlockInFlightSince.count(hashDescendant), 0U);
    BOOST_CHECK(!IsBlockRequestOwnedByAnyPeer(hashDescendant));
    BOOST_CHECK(WasBlockLastTimedOutByPeer(hashDescendant, peer.GetId()));
    BOOST_CHECK_EQUAL(
        MetricGet(ibdmetrics::Get().peer_quality_timeout_outcomes),
        nTimeoutBefore + 1);
    BOOST_CHECK_EQUAL(
        GetIbdHeaderSchedulerRefillStatsForTesting().orderedExpiryActual,
        nActualBefore + 1);

    peer.ClearAskFor();
    ReleaseBlockRequestOwnersForPeer(peer.GetId(), "disconnect");
    ResetIbdHeaderSchedulerStateForTesting();
    fSPVMode = savedSpv;
    fIbdHeadersObserve = savedObserve;
    fIbdHeaderScheduler = savedScheduler;
}

// 3. No infinite protection: even with a live advancing frontier, a request
//    whose wire age crosses the hard safety ceiling must still expire.
BOOST_AUTO_TEST_CASE(ordered_expiry_no_infinite_protection)
{
    const bool savedScheduler = fIbdHeaderScheduler;
    const bool savedObserve = fIbdHeadersObserve;
    const bool savedSpv = fSPVMode;
    fIbdHeaderScheduler = true;
    fIbdHeadersObserve = false;
    fSPVMode = false;
    ResetIbdHeaderSchedulerStateForTesting();

    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CScopedIbdQualityState scope;
    CNode peer(INVALID_SOCKET, TestPeerAddress(3003), "expiry-ceiling", true);
    PreparePeerForSendMessages(peer, PROTOCOL_VERSION);
    CScopedInitialBlockDownloadState ibdState(&peer);

    std::vector<CNode*> peers;
    SetupOrderedPipelineForExpiryTest(peer, peers);

    const uint256 hashDescendant(160);
    BOOST_REQUIRE(TryAssignBlockRequestOwner(
        hashDescendant, peer.GetId(), BLOCKREQ_SOURCE_ASKFOR));
    peer.MarkBlockInFlight(hashDescendant);
    {
        LOCK(peer.cs_vBlockInFlightWire);
        peer.mapBlockInFlightWireUs[hashDescendant] =
            GetTimeMicros() - 310000000;  // 310 s >= 300 s ceiling
    }

    const int64_t nTimeoutBefore =
        MetricGet(ibdmetrics::Get().peer_quality_timeout_outcomes);
    const uint64_t nDeferredBefore =
        GetIbdHeaderSchedulerRefillStatsForTesting()
            .orderedExpiryDeferredDueProgress;

    // Frontier advances, but the ceiling bounds the protection.
    BOOST_REQUIRE(SeedIbdHeaderSchedulerAnchorForTesting(uint256(140), 1040));
    peer.ExpireBlockInFlight();

    BOOST_CHECK_EQUAL(peer.setBlocksInFlight.count(hashDescendant), 0U);
    BOOST_CHECK_EQUAL(peer.mapBlockInFlightSince.count(hashDescendant), 0U);
    BOOST_CHECK_EQUAL(
        MetricGet(ibdmetrics::Get().peer_quality_timeout_outcomes),
        nTimeoutBefore + 1);
    BOOST_CHECK_EQUAL(
        GetIbdHeaderSchedulerRefillStatsForTesting()
            .orderedExpiryDeferredDueProgress,
        nDeferredBefore);

    peer.ClearAskFor();
    ReleaseBlockRequestOwnersForPeer(peer.GetId(), "disconnect");
    ResetIbdHeaderSchedulerStateForTesting();
    fSPVMode = savedSpv;
    fIbdHeadersObserve = savedObserve;
    fIbdHeaderScheduler = savedScheduler;
}

// 4. A genuine old frontier (front-of-pipeline) request with no progress still
//    expires, and the Phase 1 bounded recovery re-admits it in the next round
//    without a full-window refill.
BOOST_AUTO_TEST_CASE(ordered_expiry_frontier_request_recoverable)
{
    const bool savedScheduler = fIbdHeaderScheduler;
    const bool savedObserve = fIbdHeadersObserve;
    const bool savedSpv = fSPVMode;
    fIbdHeaderScheduler = true;
    fIbdHeadersObserve = false;
    fSPVMode = false;
    ResetIbdHeaderSchedulerStateForTesting();

    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CScopedIbdQualityState scope;
    CNode peer(INVALID_SOCKET, TestPeerAddress(3004), "expiry-front", true);
    PreparePeerForSendMessages(peer, PROTOCOL_VERSION);
    CScopedInitialBlockDownloadState ibdState(&peer);

    std::vector<CNode*> peers;
    SetupOrderedPipelineForExpiryTest(peer, peers);

    // The front of the ordered pipeline: window[0], height 1001.
    const uint256 hashFrontier(101);
    BOOST_REQUIRE(TryAssignBlockRequestOwner(
        hashFrontier, peer.GetId(), BLOCKREQ_SOURCE_ASKFOR));
    peer.MarkBlockInFlight(hashFrontier);
    // Model the live send path: the askfor entry is consumed when the getdata
    // goes out, so a later timeout leaves the hash released with no queued
    // retry -- exactly the case the bounded recovery scan must re-admit.
    for (std::multimap<int64_t, CInv>::iterator itAsk = peer.mapAskFor.begin();
         itAsk != peer.mapAskFor.end(); ++itAsk)
    {
        if (itAsk->second.hash == hashFrontier)
        {
            peer.EraseAskForEntry(itAsk, false);
            break;
        }
    }
    {
        LOCK(peer.cs_vBlockInFlightWire);
        peer.mapBlockInFlightWireUs[hashFrontier] =
            GetTimeMicros() - 61000000;
    }

    peer.ExpireBlockInFlight();  // stalled frontier: expires and releases
    BOOST_CHECK_EQUAL(peer.setBlocksInFlight.count(hashFrontier), 0U);
    BOOST_CHECK(!IsBlockRequestOwnedByAnyPeer(hashFrontier));
    BOOST_CHECK(WasBlockLastTimedOutByPeer(hashFrontier, peer.GetId()));

    // Phase 1 bounded recovery re-admits it in the next round.  The recovery
    // round FALLS THROUGH to the round-gated refill (livelock fix: an
    // unresolved hole must not suppress the normal refill), so exactly one
    // additional full refill runs and finds the window covered.
    const uint64_t nFullBefore =
        GetIbdHeaderSchedulerRefillStatsForTesting().fullRefillCalls;
    BOOST_CHECK_GE(RefillOrderedHeaderBlockRequestsForTesting(peers), 1U);
    BOOST_CHECK(peer.IsBlockAskForQueued(hashFrontier));
    BOOST_CHECK_EQUAL(
        GetIbdHeaderSchedulerRefillStatsForTesting().fullRefillCalls,
        nFullBefore + 1);

    peer.ClearAskFor();
    ReleaseBlockRequestOwnersForPeer(peer.GetId(), "disconnect");
    ResetIbdHeaderSchedulerStateForTesting();
    fSPVMode = savedSpv;
    fIbdHeadersObserve = savedObserve;
    fIbdHeaderScheduler = savedScheduler;
}

// 5. Legacy behavior unchanged: a request that is not an ordered-scheduler
//    descendant keeps the fixed wire-origin deadline, and the new ordered
//    diagnostics never touch it.
BOOST_AUTO_TEST_CASE(ordered_expiry_legacy_unchanged)
{
    const bool savedScheduler = fIbdHeaderScheduler;
    const bool savedObserve = fIbdHeadersObserve;
    const bool savedSpv = fSPVMode;
    fIbdHeaderScheduler = true;
    fIbdHeadersObserve = false;
    fSPVMode = false;
    ResetIbdHeaderSchedulerStateForTesting();

    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CScopedIbdQualityState scope;
    CNode peer(INVALID_SOCKET, TestPeerAddress(3005), "expiry-legacy", true);
    PreparePeerForSendMessages(peer, PROTOCOL_VERSION);
    CScopedInitialBlockDownloadState ibdState(&peer);

    // Scheduler active with a seeded graph, but the legacy hash is not an
    // ordered descendant and carries no scheduler baseline.
    std::vector<CNode*> peers;
    SetupOrderedPipelineForExpiryTest(peer, peers);

    const uint256 hashLegacy(990001);
    BOOST_REQUIRE(TryAssignBlockRequestOwner(
        hashLegacy, peer.GetId(), BLOCKREQ_SOURCE_INV));
    peer.MarkBlockInFlight(hashLegacy);
    {
        LOCK(peer.cs_vBlockInFlightWire);
        peer.mapBlockInFlightWireUs[hashLegacy] =
            GetTimeMicros() - 61000000;
    }

    const int64_t nTimeoutBefore =
        MetricGet(ibdmetrics::Get().peer_quality_timeout_outcomes);
    peer.ExpireBlockInFlight();

    BOOST_CHECK_EQUAL(peer.setBlocksInFlight.count(hashLegacy), 0U);
    BOOST_CHECK_EQUAL(peer.mapBlockInFlightSince.count(hashLegacy), 0U);
    BOOST_CHECK(WasBlockLastTimedOutByPeer(hashLegacy, peer.GetId()));
    BOOST_CHECK_EQUAL(
        MetricGet(ibdmetrics::Get().peer_quality_timeout_outcomes),
        nTimeoutBefore + 1);
    BOOST_CHECK_EQUAL(
        GetIbdHeaderSchedulerRefillStatsForTesting()
            .orderedExpiryDeferredDueProgress,
        0U);
    BOOST_CHECK_EQUAL(
        GetIbdHeaderSchedulerRefillStatsForTesting().orderedExpiryActual,
        0U);

    peer.ClearAskFor();
    ReleaseBlockRequestOwnersForPeer(peer.GetId(), "disconnect");
    ResetIbdHeaderSchedulerStateForTesting();
    fSPVMode = savedSpv;
    fIbdHeadersObserve = savedObserve;
    fIbdHeaderScheduler = savedScheduler;
}

// ---------------------------------------------------------------------------
// Ordered-head retry owner escape.
static void ExpireOrderedHeadForRetry(CNode& owner, const uint256& hash)
{
    BOOST_REQUIRE(owner.IsBlockAskForQueued(hash));
    BOOST_REQUIRE(TryAssignBlockRequestOwner(
        hash, owner.GetId(), BLOCKREQ_SOURCE_ASKFOR));
    owner.MarkBlockInFlight(hash);
    for (std::multimap<int64_t, CInv>::iterator it = owner.mapAskFor.begin();
         it != owner.mapAskFor.end(); ++it)
    {
        if (it->second.hash == hash)
        {
            owner.EraseAskForEntry(it, false);
            break;
        }
    }
    {
        LOCK(owner.cs_vBlockInFlightWire);
        owner.mapBlockInFlightWireUs[hash] = GetTimeMicros() - 61000000;
    }
    owner.ExpireBlockInFlight();
    BOOST_REQUIRE(WasBlockLastTimedOutByPeer(hash, owner.GetId()));
    BOOST_REQUIRE(!IsBlockRequestOwnedByAnyPeer(hash));
}

BOOST_AUTO_TEST_CASE(ordered_head_retry_avoids_last_timeout_owner_with_alternative)
{
    const bool savedScheduler = fIbdHeaderScheduler;
    const bool savedObserve = fIbdHeadersObserve;
    const bool savedSpv = fSPVMode;
    fIbdHeaderScheduler = true;
    fIbdHeadersObserve = false;
    fSPVMode = false;
    ResetIbdHeaderSchedulerStateForTesting();

    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CScopedIbdQualityState scope;
    CNode peerA(INVALID_SOCKET, TestPeerAddress(3011), "retry-owner-a", true);
    CNode peerB(INVALID_SOCKET, TestPeerAddress(3012), "retry-owner-b", true);
    PreparePeerForRecovery(peerA, PROTOCOL_VERSION, nBestHeight + 200);
    PreparePeerForRecovery(peerB, PROTOCOL_VERSION, nBestHeight + 100);
    CScopedInitialBlockDownloadState ibdState(&peerA);

    const uint256 anchor(7200000);
    const uint256 head(7200001);
    BOOST_REQUIRE(SeedIbdHeaderSchedulerAnchorForTesting(anchor, 0));
    std::vector<std::pair<uint256, uint256> > headers;
    headers.push_back(std::make_pair(head, anchor));
    BOOST_REQUIRE(SeedIbdHeaderSchedulerHeadersForTesting(peerA.GetId(), headers));
    BOOST_REQUIRE(SeedIbdHeaderSchedulerHeadersForTesting(peerB.GetId(), headers));

    std::vector<CNode*> peers;
    peers.push_back(&peerA);
    peers.push_back(&peerB);
    BOOST_REQUIRE_EQUAL(RefillOrderedHeaderBlockRequestsForTesting(peers), 1U);
    BOOST_REQUIRE(peerA.IsBlockAskForQueued(head));
    ExpireOrderedHeadForRetry(peerA, head);

    BOOST_REQUIRE_GE(RefillOrderedHeaderBlockRequestsForTesting(peers), 1U);
    BOOST_CHECK(!peerA.IsBlockAskForQueued(head));
    BOOST_CHECK(peerB.IsBlockAskForQueued(head));
    BOOST_CHECK_EQUAL(
        (peerA.IsBlockAskForQueued(head) ? 1U : 0U) +
        (peerB.IsBlockAskForQueued(head) ? 1U : 0U), 1U);

    peerA.ClearAskFor();
    peerB.ClearAskFor();
    ReleaseBlockRequestOwnersForPeer(peerA.GetId(), "disconnect");
    ReleaseBlockRequestOwnersForPeer(peerB.GetId(), "disconnect");
    ResetIbdHeaderSchedulerStateForTesting();
    fSPVMode = savedSpv;
    fIbdHeadersObserve = savedObserve;
    fIbdHeaderScheduler = savedScheduler;
}

BOOST_AUTO_TEST_CASE(ordered_head_retry_allows_last_owner_when_no_alternative)
{
    const bool savedScheduler = fIbdHeaderScheduler;
    const bool savedObserve = fIbdHeadersObserve;
    const bool savedSpv = fSPVMode;
    fIbdHeaderScheduler = true;
    fIbdHeadersObserve = false;
    fSPVMode = false;
    ResetIbdHeaderSchedulerStateForTesting();

    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CScopedIbdQualityState scope;
    CNode peerA(INVALID_SOCKET, TestPeerAddress(3013), "retry-only-a", true);
    PreparePeerForRecovery(peerA, PROTOCOL_VERSION, nBestHeight + 100);
    CScopedInitialBlockDownloadState ibdState(&peerA);

    const uint256 anchor(7201000);
    const uint256 head(7201001);
    BOOST_REQUIRE(SeedIbdHeaderSchedulerAnchorForTesting(anchor, 0));
    std::vector<std::pair<uint256, uint256> > headers;
    headers.push_back(std::make_pair(head, anchor));
    BOOST_REQUIRE(SeedIbdHeaderSchedulerHeadersForTesting(peerA.GetId(), headers));

    std::vector<CNode*> peers(1, &peerA);
    BOOST_REQUIRE_EQUAL(RefillOrderedHeaderBlockRequestsForTesting(peers), 1U);
    ExpireOrderedHeadForRetry(peerA, head);

    BOOST_REQUIRE_GE(RefillOrderedHeaderBlockRequestsForTesting(peers), 1U);
    BOOST_CHECK(peerA.IsBlockAskForQueued(head));

    peerA.ClearAskFor();
    ReleaseBlockRequestOwnersForPeer(peerA.GetId(), "disconnect");
    ResetIbdHeaderSchedulerStateForTesting();
    fSPVMode = savedSpv;
    fIbdHeadersObserve = savedObserve;
    fIbdHeaderScheduler = savedScheduler;
}

// ---------------------------------------------------------------------------
// Active-ancestor source evidence for the normal ordered window.
BOOST_AUTO_TEST_CASE(ordered_window_uses_late_peer_with_active_descendant_proof)
{
    const bool savedScheduler = fIbdHeaderScheduler;
    const bool savedObserve = fIbdHeadersObserve;
    const bool savedSpv = fSPVMode;
    fIbdHeaderScheduler = true;
    fIbdHeadersObserve = false;
    fSPVMode = false;
    ResetIbdHeaderSchedulerStateForTesting();
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CScopedIbdQualityState scope;
    CNode peerA(INVALID_SOCKET, TestPeerAddress(400), "ancestor-a", true);
    CNode peerB(INVALID_SOCKET, TestPeerAddress(401), "ancestor-b", true);
    PreparePeerForSendMessages(peerA, PROTOCOL_VERSION);
    PreparePeerForRecovery(peerB, PROTOCOL_VERSION, nBestHeight);
    CScopedInitialBlockDownloadState ibdState(&peerA);

    const uint256 anchor(7000000);
    peerB.nChainHeight = peerB.nBestKnownHeight = peerA.nChainHeight + 100;
    BOOST_REQUIRE(SeedIbdHeaderSchedulerAnchorForTesting(anchor, 0));
    std::vector<std::pair<uint256, uint256> > headersA;
    uint256 previous = anchor;
    for (int i = 1; i <= 600; ++i)
    {
        const uint256 hash(7000000 + i);
        headersA.push_back(std::make_pair(hash, previous));
        previous = hash;
    }
    BOOST_REQUIRE(SeedIbdHeaderSchedulerHeadersForTesting(peerA.GetId(), headersA));
    std::vector<std::pair<uint256, uint256> > headersB;
    headersB.push_back(std::make_pair(uint256(7000600), uint256(7000599)));
    BOOST_REQUIRE(SeedIbdHeaderSchedulerHeadersForTesting(peerB.GetId(), headersB));

    std::vector<CNode*> peers;
    peers.push_back(&peerA);
    peers.push_back(&peerB);
    BOOST_CHECK_GT(RefillOrderedHeaderBlockRequestsForTesting(peers), 0U);
    BOOST_CHECK(peerB.IsBlockAskForQueued(uint256(7000001)));

    peerA.ClearAskFor();
    peerB.ClearAskFor();
    ResetIbdHeaderSchedulerStateForTesting();
    fSPVMode = savedSpv;
    fIbdHeadersObserve = savedObserve;
    fIbdHeaderScheduler = savedScheduler;
}

BOOST_AUTO_TEST_CASE(ordered_window_active_ancestor_proof_excludes_sibling_peer)
{
    const bool savedScheduler = fIbdHeaderScheduler;
    const bool savedObserve = fIbdHeadersObserve;
    const bool savedSpv = fSPVMode;
    fIbdHeaderScheduler = true;
    fIbdHeadersObserve = false;
    fSPVMode = false;
    ResetIbdHeaderSchedulerStateForTesting();
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CScopedIbdQualityState scope;
    CNode peerA(INVALID_SOCKET, TestPeerAddress(402), "sibling-a", true);
    CNode peerB(INVALID_SOCKET, TestPeerAddress(403), "sibling-b", true);
    PreparePeerForSendMessages(peerA, PROTOCOL_VERSION);
    PreparePeerForRecovery(peerB, PROTOCOL_VERSION, nBestHeight + 100);
    CScopedInitialBlockDownloadState ibdState(&peerA);

    const uint256 anchor(7001000);
    BOOST_REQUIRE(SeedIbdHeaderSchedulerAnchorForTesting(anchor, 0));
    std::vector<std::pair<uint256, uint256> > active;
    uint256 previous = anchor;
    for (int i = 1; i <= 8; ++i)
    {
        const uint256 hash(7001000 + i);
        active.push_back(std::make_pair(hash, previous));
        previous = hash;
    }
    BOOST_REQUIRE(SeedIbdHeaderSchedulerHeadersForTesting(peerA.GetId(), active));
    std::vector<std::pair<uint256, uint256> > sibling;
    sibling.push_back(std::make_pair(uint256(7101001), anchor));
    sibling.push_back(std::make_pair(uint256(7101002), uint256(7101001)));
    BOOST_REQUIRE(SeedIbdHeaderSchedulerHeadersForTesting(peerB.GetId(), sibling));

    std::vector<CNode*> peers;
    peers.push_back(&peerA);
    peers.push_back(&peerB);
    BOOST_CHECK_EQUAL(RefillOrderedHeaderBlockRequestsForTesting(peers), 8U);
    BOOST_CHECK(peerA.IsBlockAskForQueued(uint256(7001001)));
    BOOST_CHECK(!peerB.IsBlockAskForQueued(uint256(7001001)));

    peerA.ClearAskFor();
    peerB.ClearAskFor();
    ResetIbdHeaderSchedulerStateForTesting();
    fSPVMode = savedSpv;
    fIbdHeadersObserve = savedObserve;
    fIbdHeaderScheduler = savedScheduler;
}

BOOST_AUTO_TEST_CASE(ordered_window_active_ancestor_proof_reorg_safe)
{
    const bool savedScheduler = fIbdHeaderScheduler;
    const bool savedObserve = fIbdHeadersObserve;
    const bool savedSpv = fSPVMode;
    fIbdHeaderScheduler = true;
    fIbdHeadersObserve = false;
    fSPVMode = false;
    ResetIbdHeaderSchedulerStateForTesting();
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CScopedIbdQualityState scope;
    CNode peerA(INVALID_SOCKET, TestPeerAddress(404), "reorg-a", true);
    CNode peerB(INVALID_SOCKET, TestPeerAddress(405), "reorg-b", true);
    PreparePeerForSendMessages(peerA, PROTOCOL_VERSION);
    PreparePeerForRecovery(peerB, PROTOCOL_VERSION, nBestHeight);
    CScopedInitialBlockDownloadState ibdState(&peerA);

    const uint256 oldAnchor(7002000);
    peerB.nChainHeight = peerB.nBestKnownHeight = peerA.nChainHeight + 100;
    BOOST_REQUIRE(SeedIbdHeaderSchedulerAnchorForTesting(oldAnchor, 0));
    std::vector<std::pair<uint256, uint256> > oldPath;
    oldPath.push_back(std::make_pair(uint256(7002001), oldAnchor));
    oldPath.push_back(std::make_pair(uint256(7002002), uint256(7002001)));
    BOOST_REQUIRE(SeedIbdHeaderSchedulerHeadersForTesting(peerA.GetId(), oldPath));
    std::vector<std::pair<uint256, uint256> > oldDescendant;
    oldDescendant.push_back(
        std::make_pair(uint256(7002002), uint256(7002001)));
    BOOST_REQUIRE(SeedIbdHeaderSchedulerHeadersForTesting(
        peerB.GetId(), oldDescendant));

    std::vector<CNode*> peers;
    peers.push_back(&peerA);
    peers.push_back(&peerB);
    BOOST_CHECK_EQUAL(RefillOrderedHeaderBlockRequestsForTesting(peers), 2U);
    BOOST_CHECK(peerB.IsBlockAskForQueued(uint256(7002001)));
    peerA.ClearAskFor();
    peerB.ClearAskFor();

    const uint256 newAnchor(7003000);
    BOOST_REQUIRE(SeedIbdHeaderSchedulerAnchorForTesting(newAnchor, 0));
    std::vector<std::pair<uint256, uint256> > newPath;
    newPath.push_back(std::make_pair(uint256(7003001), newAnchor));
    newPath.push_back(std::make_pair(uint256(7003002), uint256(7003001)));
    BOOST_REQUIRE(SeedIbdHeaderSchedulerHeadersForTesting(peerA.GetId(), newPath));
    BOOST_CHECK_EQUAL(RefillOrderedHeaderBlockRequestsForTesting(peers), 2U);
    BOOST_CHECK(peerA.IsBlockAskForQueued(uint256(7003001)));
    BOOST_CHECK(!peerB.IsBlockAskForQueued(uint256(7003001)));

    peerA.ClearAskFor();
    peerB.ClearAskFor();
    ResetIbdHeaderSchedulerStateForTesting();
    fSPVMode = savedSpv;
    fIbdHeadersObserve = savedObserve;
    fIbdHeaderScheduler = savedScheduler;
}

BOOST_AUTO_TEST_CASE(ordered_window_late_sources_enable_multipeer_distribution)
{
    const bool savedScheduler = fIbdHeaderScheduler;
    const bool savedObserve = fIbdHeadersObserve;
    const bool savedSpv = fSPVMode;
    fIbdHeaderScheduler = true;
    fIbdHeadersObserve = false;
    fSPVMode = false;
    ResetIbdHeaderSchedulerStateForTesting();
    CScopedAlreadyAskedFor isolatedAlreadyAskedFor;
    CScopedIbdQualityState scope;
    CNode peerA(INVALID_SOCKET, TestPeerAddress(406), "spread-a", true);
    CNode peerB(INVALID_SOCKET, TestPeerAddress(407), "spread-b", true);
    CNode peerC(INVALID_SOCKET, TestPeerAddress(408), "spread-c", true);
    PreparePeerForSendMessages(peerA, PROTOCOL_VERSION);
    PreparePeerForRecovery(peerB, PROTOCOL_VERSION, nBestHeight);
    PreparePeerForRecovery(peerC, PROTOCOL_VERSION, nBestHeight);
    CScopedInitialBlockDownloadState ibdState(&peerA);

    const uint256 anchor(7004000);
    peerB.nChainHeight = peerB.nBestKnownHeight = peerA.nChainHeight + 100;
    peerC.nChainHeight = peerC.nBestKnownHeight = peerA.nChainHeight + 100;
    BOOST_REQUIRE(SeedIbdHeaderSchedulerAnchorForTesting(anchor, 0));
    std::vector<std::pair<uint256, uint256> > headersA;
    uint256 previous = anchor;
    for (int i = 1; i <= 600; ++i)
    {
        const uint256 hash(7004000 + i);
        headersA.push_back(std::make_pair(hash, previous));
        previous = hash;
    }
    BOOST_REQUIRE(SeedIbdHeaderSchedulerHeadersForTesting(peerA.GetId(), headersA));
    std::vector<std::pair<uint256, uint256> > suffix;
    suffix.push_back(std::make_pair(uint256(7004600), uint256(7004599)));
    BOOST_REQUIRE(SeedIbdHeaderSchedulerHeadersForTesting(peerB.GetId(), suffix));
    BOOST_REQUIRE(SeedIbdHeaderSchedulerHeadersForTesting(peerC.GetId(), suffix));

    std::vector<CNode*> peers;
    peers.push_back(&peerA);
    peers.push_back(&peerB);
    peers.push_back(&peerC);
    BOOST_CHECK_GT(RefillOrderedHeaderBlockRequestsForTesting(peers), 0U);
    BOOST_CHECK_GT(peerB.setAskForBlocks.size(), 0U);
    BOOST_CHECK_GT(peerC.setAskForBlocks.size(), 0U);
    BOOST_CHECK_LT(peerA.setAskForBlocks.size(), 512U);

    peerA.ClearAskFor();
    peerB.ClearAskFor();
    peerC.ClearAskFor();
    ResetIbdHeaderSchedulerStateForTesting();
    fSPVMode = savedSpv;
    fIbdHeadersObserve = savedObserve;
    fIbdHeaderScheduler = savedScheduler;
}

BOOST_AUTO_TEST_CASE(block_locator_reaches_common_ancestor_beyond_2729_block_fork)
{
    const int nTipHeight = 8000;
    const int nForkDepth = 2729;
    const int nCommonHeight = nTipHeight - nForkDepth;
    std::vector<CBlockIndex> index(nTipHeight + 1);
    std::vector<uint256> hashes(nTipHeight + 1);

    for (int height = 0; height <= nTipHeight; ++height)
    {
        hashes[height] = uint256(8000000 + height);
        index[height].phashBlock = &hashes[height];
        index[height].nHeight = height;
        index[height].pprev = height == 0 ? NULL : &index[height - 1];
    }

    const CInspectableBlockLocator locator(&index[nTipHeight]);
    bool fContainsSharedNonGenesisAncestor = false;
    for (int height = 1; height <= nCommonHeight; ++height)
    {
        if (locator.Contains(hashes[height]))
        {
            fContainsSharedNonGenesisAncestor = true;
            break;
        }
    }

    BOOST_CHECK(fContainsSharedNonGenesisAncestor);
}

BOOST_AUTO_TEST_CASE(pos_kernel_modifier_uses_candidate_branch_ancestry)
{
    const uint256 hashSource(8100000);
    const uint256 hashFork(8100001);
    const uint256 hashMain(8100002);
    CBlockIndex source;
    CBlockIndex forkTip;
    CBlockIndex mainTip;
    source.phashBlock = &hashSource;
    source.nHeight = 100;
    source.nTime = 1000;
    source.pnext = &forkTip;
    forkTip.phashBlock = &hashFork;
    forkTip.pprev = &source;
    forkTip.nHeight = 101;
    forkTip.nTime = 1000000;
    forkTip.nStakeModifier = 0xf0f0;
    forkTip.nFlags |= CBlockIndex::BLOCK_STAKE_MODIFIER;
    mainTip.phashBlock = &hashMain;
    mainTip.pprev = &source;
    mainTip.nHeight = 101;
    mainTip.nTime = 1000000;
    mainTip.nStakeModifier = 0x0f0f;
    mainTip.nFlags |= CBlockIndex::BLOCK_STAKE_MODIFIER;

    const std::pair<std::map<uint256, CBlockIndex*>::iterator, bool> inserted =
        mapBlockIndex.insert(std::make_pair(hashSource, &source));
    BOOST_REQUIRE(inserted.second);

    uint64_t modifier = 0;
    int modifierHeight = 0;
    int64_t modifierTime = 0;
    BOOST_REQUIRE(GetKernelStakeModifier(
        hashSource, &mainTip, modifier, modifierHeight, modifierTime, false));
    BOOST_CHECK_EQUAL(modifier, mainTip.nStakeModifier);
    BOOST_CHECK_NE(modifier, forkTip.nStakeModifier);

    CBlockIndex unrelated;
    const uint256 hashUnrelated(8100003);
    unrelated.phashBlock = &hashUnrelated;
    unrelated.nHeight = 101;
    unrelated.nTime = 1000000;
    BOOST_CHECK(!GetKernelStakeModifier(
        hashSource, &unrelated, modifier, modifierHeight, modifierTime, false));

    mapBlockIndex.erase(inserted.first);
}

BOOST_AUTO_TEST_CASE(pos_stake_source_resolves_only_from_candidate_side_branch)
{
    CTransaction txMain;
    txMain.nTime = 1700000100;
    txMain.vout.push_back(CTxOut(COIN, CScript() << OP_TRUE));
    CTransaction txFork;
    txFork.nTime = 1700000101;
    txFork.vout.push_back(CTxOut(2 * COIN, CScript() << OP_TRUE));

    CBlock blockMain;
    blockMain.nTime = 1700000200;
    blockMain.vtx.push_back(txMain);
    blockMain.hashMerkleRoot = blockMain.BuildMerkleTree();
    CBlock blockFork;
    blockFork.nTime = 1700000201;
    blockFork.vtx.push_back(txFork);
    blockFork.hashMerkleRoot = blockFork.BuildMerkleTree();


    const uint256 hashCommon(8200000);
    const uint256 hashMain = blockMain.GetHash();
    const uint256 hashFork = blockFork.GetHash();
    CBlockIndex common;
    CBlockIndex mainTip(1, 100, blockMain);
    CBlockIndex forkTip(1, 200, blockFork);
    common.phashBlock = &hashCommon;
    common.nHeight = 100;
    common.pnext = &forkTip;
    mainTip.phashBlock = &hashMain;
    mainTip.pprev = &common;
    mainTip.nHeight = 101;
    forkTip.phashBlock = &hashFork;
    forkTip.pprev = &common;
    forkTip.nHeight = 101;

    CBlockIndex* savedBest = pindexBest;
    pindexBest = &forkTip;
    CTransaction foundTx;
    CTxIndex foundIndex;
    CBlock foundBlock;
    std::map<uint256, CBlock> candidateBlocks;
    candidateBlocks[hashMain] = blockMain;
    candidateBlocks[hashFork] = blockFork;
    CTxDB txdb("r");
    CTransaction globallyIndexedTx;
    CTxIndex globallyIndexedTxIndex;
    const bool absentFromGlobalIndex = !globallyIndexedTx.ReadFromDisk(
        txdb, COutPoint(txMain.GetHash(), 0), globallyIndexedTxIndex);
    const bool foundOnMain = ReadStakeSourceTransactionForTesting(
        &mainTip, COutPoint(txMain.GetHash(), 0), foundTx, foundIndex,
        foundBlock, candidateBlocks);
    const uint256 foundTxHash = foundTx.GetHash();
    const uint256 foundBlockHash = foundBlock.GetHash();
    const bool mainRejectedOnFork = !ReadStakeSourceTransactionForTesting(
        &forkTip, COutPoint(txMain.GetHash(), 0), foundTx, foundIndex,
        foundBlock, candidateBlocks);
    const bool forkRejectedOnMain = !ReadStakeSourceTransactionForTesting(
        &mainTip, COutPoint(txFork.GetHash(), 0), foundTx, foundIndex,
        foundBlock, candidateBlocks);
    pindexBest = savedBest;

    BOOST_CHECK(absentFromGlobalIndex);
    BOOST_CHECK(foundOnMain);
    BOOST_CHECK(foundTxHash == txMain.GetHash());
    BOOST_CHECK(foundBlockHash == blockMain.GetHash());
    BOOST_CHECK(mainRejectedOnFork);
    BOOST_CHECK(forkRejectedOnMain);
}

BOOST_AUTO_TEST_SUITE_END()
