// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2012 The Bitcoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "db.h"
#include "net.h"
#include "main.h"
#include "init.h"
#include "strlcpy.h"
#include "addrman.h"
#include "ui_interface.h"
#include "collateral.h"
#include "collateralnode.h"
#include "dandelion.h"
#include "shielded.h"
#include <sys/stat.h>
#include <algorithm>
#include <cctype>
#include <sstream>
#include <cstdio>

#ifdef WIN32
#include <string.h>
#endif

#ifdef USE_UPNP
#include <miniupnpc/miniwget.h>
#include <miniupnpc/miniupnpc.h>
#include <miniupnpc/upnpcommands.h>
#include <miniupnpc/upnperrors.h>
#endif

using namespace std;
using namespace boost;

namespace fs = boost::filesystem;

#ifdef USE_NATIVETOR
extern "C" {
    int tor_main(int argc, char *argv[]);
}
#endif

// Dump data of peers.dat and banlist.dat every 15 minutes (900s)
#define DUMP_DATA_INTERVAL 900

static const int MAX_OUTBOUND_CONNECTIONS = 16;

static const int CONNECTION_RATE_LIMIT_WINDOW = 60;      // Window size in seconds
static const int CONNECTION_RATE_LIMIT_MAX = 5;          // Max connections per IP per window
static CCriticalSection cs_connectionRateLimit;
static std::map<CNetAddr, std::vector<int64_t> > mapConnectionAttempts;

static const int MAX_INBOUND_PER_NETGROUP = 4;           // Max inbound connections per /16 subnet

void ThreadMessageHandler2(void* parg);
void ThreadSocketHandler2(void* parg);
void ThreadOpenConnections2(void* parg);
void ThreadOpenAddedConnections2(void* parg);
#ifdef USE_UPNP
void ThreadMapPort2(void* parg);
#endif
void ThreadDNSAddressSeed2(void* parg);
bool OpenNetworkConnection(const CAddress& addrConnect, CSemaphoreGrant *grantOutbound = NULL, const char *strDest = NULL, bool fOneShot = false);

namespace {

struct MessageStat
{
    uint64_t count;
    uint64_t bytes;

    MessageStat() : count(0), bytes(0) {}
};

struct PeerMessageStats
{
    std::map<std::string, MessageStat> incoming;
    std::map<std::string, MessageStat> outgoing;
};

struct GetHeadersLocatorState
{
    uint64_t sent;
    uint64_t suppressed;
    int64_t lastRequestTime;
    int64_t lastLogTime;

    GetHeadersLocatorState() : sent(0), suppressed(0), lastRequestTime(0), lastLogTime(0) {}
};

struct GetHeadersPeerStats
{
    std::map<std::string, GetHeadersLocatorState> locators;
    uint64_t totalSent;
    uint64_t totalSuppressed;
    int64_t lastRequestTime;

    GetHeadersPeerStats() : totalSent(0), totalSuppressed(0), lastRequestTime(0) {}
};

static CCriticalSection cs_p2pMessageStats;
static std::map<NodeId, PeerMessageStats> mapPeerMessageStats;
static std::map<std::string, MessageStat> mapGlobalP2PMsgIncoming;
static std::map<std::string, MessageStat> mapGlobalP2PMsgOutgoing;
static std::map<NodeId, GetHeadersPeerStats> mapGetHeadersPeerStats;
static int64_t nLastSyncDiagnosticsLog = 0;
static uint64_t nLastSyncDiagnosticsBytesRecv = 0;
static uint64_t nLastSyncDiagnosticsBytesSent = 0;

static void AddMessageStat(std::map<std::string, MessageStat>& stats, const std::string& command, unsigned int bytes)
{
    MessageStat& st = stats[command];
    st.count++;
    st.bytes += bytes;
}

static std::string FormatMessageStats(const std::map<std::string, MessageStat>& stats, const std::vector<std::string>& keys)
{
    std::ostringstream oss;
    bool first = true;
    for (const std::string& key : keys) {
        std::map<std::string, MessageStat>::const_iterator it = stats.find(key);
        if (it == stats.end() || (it->second.count == 0 && it->second.bytes == 0))
            continue;
        if (!first)
            oss << ' ';
        first = false;
        oss << key << '=' << it->second.count << '/' << it->second.bytes;
    }
    if (first)
        oss << "none";
    return oss.str();
}

static void GetProcessMemorySnapshot(uint64_t& nVmRssKb, uint64_t& nVmSizeKb, uint64_t& nVmDataKb, uint64_t& nVmSwapKb, int& nThreads)
{
    nVmRssKb = 0;
    nVmSizeKb = 0;
    nVmDataKb = 0;
    nVmSwapKb = 0;
    nThreads = 0;
#ifndef WIN32
    FILE* fp = fopen("/proc/self/status", "r");
    if (!fp)
        return;

    char line[256];
    while (fgets(line, sizeof(line), fp))
    {
        unsigned long long nValue = 0;
        if (sscanf(line, "VmRSS: %llu kB", &nValue) == 1)
            nVmRssKb = nValue;
        else if (sscanf(line, "VmSize: %llu kB", &nValue) == 1)
            nVmSizeKb = nValue;
        else if (sscanf(line, "VmData: %llu kB", &nValue) == 1)
            nVmDataKb = nValue;
        else if (sscanf(line, "VmSwap: %llu kB", &nValue) == 1)
            nVmSwapKb = nValue;
        else if (sscanf(line, "Threads: %d", &nThreads) == 1)
        {
        }
    }
    fclose(fp);
#endif
}

static std::string LocatorFingerprint(const CBlockLocator& locator, const uint256& hashStop)
{
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << locator << hashStop;
    return Hash(ss.begin(), ss.end()).ToString();
}

static std::string DescribePeerForDiagnostics(const CNode* pnode)
{
    std::ostringstream oss;
    oss << '#' << pnode->GetId() << ' ' << pnode->addrName << ' ' << pnode->strSubVer;
    return oss.str();
}

static int64_t GetPeerAdvertisedHeight(const CNode* pnode)
{
    return std::max(pnode->nBestKnownHeight, pnode->nChainHeight);
}

static int64_t SyncPeerScore(const CNode* pnode, int64_t nNow, int64_t nMaxPeerHeight)
{
    int64_t nPeerHeight = GetPeerAdvertisedHeight(pnode);
    if (nPeerHeight < 0)
        nPeerHeight = 0;

    int64_t nScore = nPeerHeight * 1000000;

    if (nMaxPeerHeight > nPeerHeight)
        nScore -= (nMaxPeerHeight - nPeerHeight) * 10000;

    if (pnode->nLastHeightUpdate > 0 && nNow - pnode->nLastHeightUpdate <= 120)
        nScore += 250000;

    if (pnode->nLastBlockRecv > 0 && nNow - pnode->nLastBlockRecv <= 300)
        nScore += 500000;

    if (!pnode->setBlocksInFlight.empty())
        nScore += 100000;

    if (pnode->nBlocksReceivedInBatch > 0)
        nScore += 50000;

    if (pnode->nPingUsecTime > 0) {
        int64_t nPingBonus = 300000 - (pnode->nPingUsecTime / 10);
        if (nPingBonus > 0)
            nScore += nPingBonus;
    }

    if (nNow - pnode->nTimeConnected < 120)
        nScore += 10000;

    if (pnode->fDisconnect)
        nScore -= 5000000;

    return nScore;
}

static bool CompareSyncCandidates(const std::pair<int64_t, CNode*>& a, const std::pair<int64_t, CNode*>& b)
{
    if (a.first != b.first)
        return a.first > b.first;
    return a.second->GetId() < b.second->GetId();
}

static std::string FormatPeerDiagnosticsSummary(const CNode* pnode, int64_t nNow)
{
    std::ostringstream oss;
    int64_t nPeerHeight = GetPeerAdvertisedHeight(pnode);
    int64_t nAgeSinceRecv = pnode->nLastRecv > 0 ? (nNow - pnode->nLastRecv) : -1;
    int64_t nAgeSinceSend = pnode->nLastSend > 0 ? (nNow - pnode->nLastSend) : -1;
    uint64_t nGetHeadersSent = 0;
    uint64_t nGetHeadersSuppressed = 0;
    uint64_t nGetHeadersInFlight = pnode->getHeadersSync.IsInFlight() ? 1 : 0;

    {
        std::map<NodeId, GetHeadersPeerStats>::const_iterator it = mapGetHeadersPeerStats.find(pnode->GetId());
        if (it != mapGetHeadersPeerStats.end()) {
            nGetHeadersSent = it->second.totalSent;
            nGetHeadersSuppressed = it->second.totalSuppressed;
        }
    }

    oss << "id=" << pnode->GetId()
        << " addr=" << pnode->addrName
        << " subver=" << pnode->strSubVer
        << " ver=" << pnode->nVersion
        << " h=" << pnode->nChainHeight
        << " best=" << pnode->nBestKnownHeight
        << " adv=" << nPeerHeight
        << " inflight=" << pnode->setBlocksInFlight.size()
        << " askfor=" << pnode->mapAskFor.size()
        << " getheaders_sent=" << nGetHeadersSent
        << " getheaders_suppressed=" << nGetHeadersSuppressed
        << " getheaders_inflight=" << nGetHeadersInFlight
        << " recv_age=" << nAgeSinceRecv
        << " send_age=" << nAgeSinceSend
        << " last_block=" << pnode->nLastBlockRecv
        << " last_height=" << pnode->nLastHeightUpdate
        << " ping_us=" << pnode->nPingUsecTime
        << " inbound=" << (pnode->fInbound ? 1 : 0)
        << " whitelisted=" << (pnode->fWhitelisted ? 1 : 0)
        << " connected=" << (pnode->fSuccessfullyConnected ? 1 : 0)
        << " disconnect=" << (pnode->fDisconnect ? 1 : 0)
        << " fstartsync=" << (pnode->fStartSync ? 1 : 0)
        << " queued_getblocks=" << pnode->getBlocksIndex.size();
    return oss.str();
}

} // namespace

CGetHeadersSyncState::CGetHeadersSyncState()
    : fInFlight(false),
      fHasCompleted(false),
      fHasLastRequest(false),
      nActiveSince(0),
      nLastCompletedTime(0),
      nLastRequestTime(0),
      nRequestSequence(0)
{
}

CGetHeadersSyncState::StartResult CGetHeadersSyncState::Start(
    const std::string& strRequestKey, int64_t nNow, int64_t nTimeout)
{
    LOCK(cs_state);

    bool fTimedOut = false;
    if (fInFlight)
    {
        const int64_t nAge = nNow >= nActiveSince ? nNow - nActiveSince : 0;
        if (nAge < nTimeout)
            return SUPPRESSED_ACTIVE;
        fInFlight = false;
        fTimedOut = true;
    }

    if (!fTimedOut && fHasCompleted && strRequestKey == strLastCompletedRequestKey)
        return SUPPRESSED_COMPLETED;

    fInFlight = true;
    strActiveRequestKey = strRequestKey;
    nActiveSince = nNow;
    nLastRequestTime = nNow;
    fHasLastRequest = true;
    ++nRequestSequence;
    return fTimedOut ? RETRIED_AFTER_TIMEOUT : STARTED;
}

bool CGetHeadersSyncState::Complete(int64_t nNow)
{
    LOCK(cs_state);
    if (!fInFlight)
        return false;

    fInFlight = false;
    fHasCompleted = true;
    strLastCompletedRequestKey = strActiveRequestKey;
    strActiveRequestKey.clear();
    nLastCompletedTime = nNow;
    return true;
}

bool CGetHeadersSyncState::IsInFlight() const
{
    LOCK(cs_state);
    return fInFlight;
}

bool CGetHeadersSyncState::IsTimedOut(int64_t nNow, int64_t nTimeout) const
{
    LOCK(cs_state);
    if (!fInFlight)
        return false;
    const int64_t nAge = nNow >= nActiveSince ? nNow - nActiveSince : 0;
    return nAge >= nTimeout;
}

int64_t CGetHeadersSyncState::LastRequestAge(int64_t nNow) const
{
    LOCK(cs_state);
    if (!fHasLastRequest)
        return -1;
    return nNow >= nLastRequestTime ? nNow - nLastRequestTime : 0;
}

uint64_t CGetHeadersSyncState::RequestSequence() const
{
    LOCK(cs_state);
    return nRequestSequence;
}

void RecordP2PMessageStat(const CNode* pnode, const std::string& command, unsigned int bytes, bool incoming)
{
    if (command.empty() || pnode == NULL)
        return;

    LOCK(cs_p2pMessageStats);
    PeerMessageStats& peerStats = mapPeerMessageStats[pnode->GetId()];
    if (incoming) {
        AddMessageStat(peerStats.incoming, command, bytes);
        AddMessageStat(mapGlobalP2PMsgIncoming, command, bytes);
    } else {
        AddMessageStat(peerStats.outgoing, command, bytes);
        AddMessageStat(mapGlobalP2PMsgOutgoing, command, bytes);
    }
}

static const int64_t GETBLOCKS_TOKEN_BUCKET_CAPACITY = 30000;
static const int64_t GETBLOCKS_TOKEN_REFILL_PER_SECOND = 1000;
static const unsigned int GETBLOCKS_COST_ITEMS = 250;
static const int64_t GETBLOCKS_REPEAT_BASE_COOLDOWN_MILLIS = 2000;
static const unsigned int GETBLOCKS_REPEAT_PENALTY_INTERVAL = 128;
static const unsigned int GETBLOCKS_REPEAT_DISCONNECT_THRESHOLD = 512;

CGetBlocksRequestInfo::CGetBlocksRequestInfo()
    : nResolvedHeight(-1),
      nStopHeight(-1),
      nPredictedResponseCount(0),
      nRequestTimeMillis(0)
{
}

CGetBlocksResponseInfo::CGetBlocksResponseInfo()
    : nItemCount(0),
      nMinHeight(-1),
      nMaxHeight(-1)
{
}

void CGetBlocksResponseInfo::Add(const uint256& hash, int nHeight)
{
    if (nItemCount == 0)
    {
        hashFirst = hash;
        nMinHeight = nHeight;
        nMaxHeight = nHeight;
    }
    hashLast = hash;
    nItemCount++;
    if (nHeight >= 0)
    {
        if (nMinHeight < 0 || nHeight < nMinHeight)
            nMinHeight = nHeight;
        if (nMaxHeight < 0 || nHeight > nMaxHeight)
            nMaxHeight = nHeight;
    }
}

CGetBlocksServerDecision::CGetBlocksServerDecision()
    : action(GETBLOCKS_SERVER_ALLOW),
      fIdenticalRequest(false),
      fSameResponse(false),
      fProgress(false),
      fPenalize(false),
      nPenalty(0),
      nCooldownMillis(0),
      nEstimatedBytes(0)
{
}

CGetBlocksServerState::CGetBlocksServerState()
    : fHaveLastRequest(false),
      fHaveLastResponse(false),
      fTokenBucketInitialized(false),
      nLastPredictedResponseCount(0),
      nLastStopHeight(-1),
      nLastResponseMinHeight(-1),
      nLastResponseMaxHeight(-1),
      nTokenBucketLastMillis(0),
      nTokenBucketMilliTokens(GETBLOCKS_TOKEN_BUCKET_CAPACITY),
      nPendingRequestCostMilliTokens(0),
      nUsefulGetDataSinceLastResponse(0),
      nLastResolvedHeight(-1),
      nLastResponseCount(0),
      nLastResponseBytes(0),
      nLastRequestTimeMillis(0),
      nResponseBytesAllowed(0),
      nPreviousRequestTimeMillis(0),
      nRepeatAllowedAfterMillis(0),
      nLastProgressDelta(0),
      nRequestsReceived(0),
      nResponsesAllowed(0),
      nResponsesSuppressed(0),
      nRequestsRateLimited(0),
      nIdenticalRequests(0),
      nSameLocatorRequests(0),
      nSameResponseRequests(0),
      nNonProgressingRequests(0),
      nUsefulGetData(0),
      nEstimatedSuppressedBytes(0),
      nConsecutiveIdenticalRequests(0),
      nConsecutiveNonProgressingRequests(0)
{
}

void CGetBlocksServerState::RefillTokenBucket(int64_t nNowMillis)
{
    if (!fTokenBucketInitialized)
    {
        fTokenBucketInitialized = true;
        nTokenBucketLastMillis = nNowMillis;
        nTokenBucketMilliTokens = GETBLOCKS_TOKEN_BUCKET_CAPACITY;
        return;
    }

    if (nNowMillis <= nTokenBucketLastMillis)
    {
        if (nNowMillis < nTokenBucketLastMillis)
            nTokenBucketLastMillis = nNowMillis;
        return;
    }

    const int64_t nElapsedMillis = nNowMillis - nTokenBucketLastMillis;
    const int64_t nRefill =
        (nElapsedMillis * GETBLOCKS_TOKEN_REFILL_PER_SECOND) / 1000;
    nTokenBucketMilliTokens = std::min(
        GETBLOCKS_TOKEN_BUCKET_CAPACITY,
        nTokenBucketMilliTokens + nRefill);
    nTokenBucketLastMillis = nNowMillis;
}

int64_t CGetBlocksServerState::ResponseCostMilliTokens(
    unsigned int nItems) const
{
    return 1000 +
        ((nItems + GETBLOCKS_COST_ITEMS - 1) / GETBLOCKS_COST_ITEMS) * 1000;
}

int64_t CGetBlocksServerState::RepeatCooldownMillis() const
{
    unsigned int nLevel = nConsecutiveNonProgressingRequests / 16;
    nLevel = std::min(nLevel, 4U);
    return GETBLOCKS_REPEAT_BASE_COOLDOWN_MILLIS << nLevel;
}

uint64_t CGetBlocksServerState::EstimateInvPayloadBytes(unsigned int nItems)
{
    uint64_t nBytes = 0;
    unsigned int nRemaining = nItems;
    while (nRemaining > 0)
    {
        const unsigned int nChunk = std::min(nRemaining, 1000U);
        nBytes += nChunk < 253 ? 1 : 3;
        nBytes += static_cast<uint64_t>(nChunk) * 36;
        nRemaining -= nChunk;
    }
    return nBytes;
}

CGetBlocksServerDecision CGetBlocksServerState::Evaluate(
    const CGetBlocksRequestInfo& request, bool fStrictInbound)
{
    CGetBlocksServerDecision decision;
    RefillTokenBucket(request.nRequestTimeMillis);

    const bool fHadLastRequest = fHaveLastRequest;
    const bool fSameLocator =
        fHadLastRequest && request.hashLocatorTip == hashLastLocatorTip;
    const bool fIdenticalRequest =
        fSameLocator &&
        request.nResolvedHeight == nLastResolvedHeight &&
        request.hashStop == hashLastStop;
    const bool fSamePredictedResponse =
        fHaveLastResponse &&
        request.hashChainTip == hashLastResponseChainTip &&
        request.hashPredictedFirst == hashLastPredictedFirst &&
        request.hashPredictedLast == hashLastPredictedLast &&
        request.nPredictedResponseCount == nLastPredictedResponseCount;

    nPreviousRequestTimeMillis = nLastRequestTimeMillis;
    nLastRequestTimeMillis = request.nRequestTimeMillis;
    nRequestsReceived++;
    nLastProgressDelta = fHadLastRequest
        ? request.nResolvedHeight - nLastResolvedHeight
        : 0;

    if (fSameLocator)
        nSameLocatorRequests++;
    if (fIdenticalRequest)
        nIdenticalRequests++;

    const bool fLocatorProgress =
        fHadLastRequest && request.nResolvedHeight > nLastResolvedHeight;
    const bool fStopProgress =
        fHadLastRequest &&
        request.hashStop != hashLastStop &&
        request.nStopHeight > request.nResolvedHeight &&
        request.nStopHeight > nLastStopHeight;
    const bool fNextRange =
        fHaveLastResponse &&
        nLastResponseMaxHeight >= 0 &&
        request.nResolvedHeight >= nLastResponseMaxHeight &&
        request.nResolvedHeight > nLastResolvedHeight;
    const bool fUsefulGetData = nUsefulGetDataSinceLastResponse > 0;
    decision.fProgress =
        !fHadLastRequest || fLocatorProgress || fStopProgress ||
        fNextRange || fUsefulGetData;
    decision.fIdenticalRequest = fIdenticalRequest;
    decision.fSameResponse = fSamePredictedResponse;

    if (decision.fProgress)
    {
        nConsecutiveIdenticalRequests = 0;
        nConsecutiveNonProgressingRequests = 0;
        nRepeatAllowedAfterMillis = 0;
    }
    else if (fHadLastRequest)
    {
        nConsecutiveNonProgressingRequests++;
        nNonProgressingRequests++;
        if (fIdenticalRequest)
            nConsecutiveIdenticalRequests++;
        else
            nConsecutiveIdenticalRequests = 0;
        if (fSamePredictedResponse)
            nSameResponseRequests++;
    }

    const bool fRepeatedRange =
        fHadLastRequest && !decision.fProgress &&
        (fIdenticalRequest || fSamePredictedResponse);
    if (fRepeatedRange)
    {
        decision.nCooldownMillis = RepeatCooldownMillis();
        decision.nEstimatedBytes = fHaveLastResponse
            ? nLastResponseBytes
            : EstimateInvPayloadBytes(request.nPredictedResponseCount);

        if (fStrictInbound &&
            nConsecutiveNonProgressingRequests >=
                GETBLOCKS_REPEAT_PENALTY_INTERVAL &&
            nConsecutiveNonProgressingRequests %
                GETBLOCKS_REPEAT_PENALTY_INTERVAL == 0)
        {
            decision.fPenalize = true;
            decision.nPenalty = 5;
        }

        if (fStrictInbound &&
            nConsecutiveNonProgressingRequests >=
                GETBLOCKS_REPEAT_DISCONNECT_THRESHOLD)
        {
            decision.action = GETBLOCKS_SERVER_DISCONNECT;
        }
        else if (request.nRequestTimeMillis < nRepeatAllowedAfterMillis)
        {
            decision.action = GETBLOCKS_SERVER_SUPPRESS;
        }

        if (decision.action == GETBLOCKS_SERVER_SUPPRESS ||
            decision.action == GETBLOCKS_SERVER_DISCONNECT)
        {
            nResponsesSuppressed++;
            nEstimatedSuppressedBytes += decision.nEstimatedBytes;
            nRepeatAllowedAfterMillis = std::max(
                nRepeatAllowedAfterMillis,
                request.nRequestTimeMillis + decision.nCooldownMillis);
        }
    }

    const int64_t nRequestCost =
        ResponseCostMilliTokens(request.nPredictedResponseCount);
    if (decision.action == GETBLOCKS_SERVER_ALLOW)
    {
        if (nTokenBucketMilliTokens < nRequestCost)
        {
            decision.action = GETBLOCKS_SERVER_RATE_LIMIT;
            decision.nEstimatedBytes =
                EstimateInvPayloadBytes(request.nPredictedResponseCount);
            nRequestsRateLimited++;
            nEstimatedSuppressedBytes += decision.nEstimatedBytes;
            nPendingRequestCostMilliTokens = 0;
        }
        else
        {
            nTokenBucketMilliTokens -= nRequestCost;
            nPendingRequestCostMilliTokens = nRequestCost;
        }
    }
    else
    {
        nPendingRequestCostMilliTokens = 0;
    }

    hashLastLocatorTip = request.hashLocatorTip;
    nLastResolvedHeight = request.nResolvedHeight;
    hashLastStop = request.hashStop;
    nLastStopHeight = request.nStopHeight;
    fHaveLastRequest = true;

    return decision;
}

void CGetBlocksServerState::RecordResponse(
    const CGetBlocksRequestInfo& request,
    const CGetBlocksResponseInfo& response)
{
    const bool fSameResponse =
        fHaveLastResponse &&
        request.hashChainTip == hashLastResponseChainTip &&
        response.hashFirst == hashLastResponseFirst &&
        response.hashLast == hashLastResponseLast &&
        response.nItemCount == nLastResponseCount;

    if (fHaveLastResponse && !fSameResponse)
    {
        nConsecutiveIdenticalRequests = 0;
        nConsecutiveNonProgressingRequests = 0;
        nRepeatAllowedAfterMillis = 0;
    }

    const int64_t nActualCost = ResponseCostMilliTokens(response.nItemCount);
    if (nActualCost > nPendingRequestCostMilliTokens)
    {
        const int64_t nAdditionalCost =
            nActualCost - nPendingRequestCostMilliTokens;
        nTokenBucketMilliTokens =
            std::max<int64_t>(0, nTokenBucketMilliTokens - nAdditionalCost);
    }
    nPendingRequestCostMilliTokens = 0;

    nResponsesAllowed++;
    hashLastResponseFirst = response.hashFirst;
    hashLastResponseLast = response.hashLast;
    nLastResponseCount = response.nItemCount;
    nLastResponseBytes = EstimateInvPayloadBytes(response.nItemCount);
    hashLastResponseChainTip = request.hashChainTip;
    nResponseBytesAllowed += nLastResponseBytes;
    hashLastPredictedFirst = request.hashPredictedFirst;
    hashLastPredictedLast = request.hashPredictedLast;
    nLastPredictedResponseCount = request.nPredictedResponseCount;
    nLastResponseMinHeight = response.nMinHeight;
    nLastResponseMaxHeight = response.nMaxHeight;
    nUsefulGetDataSinceLastResponse = 0;
    fHaveLastResponse = true;

    nRepeatAllowedAfterMillis = std::max(
        nRepeatAllowedAfterMillis,
        request.nRequestTimeMillis + RepeatCooldownMillis());
}

bool CGetBlocksServerState::NoteBlockGetData(
    const uint256& hashBlock, int nHeight, int64_t nNowMillis)
{
    (void)nNowMillis;
    if (!fHaveLastResponse || nLastResponseCount == 0)
        return false;

    const bool fHashMatch =
        hashBlock == hashLastResponseFirst || hashBlock == hashLastResponseLast;
    const bool fHeightMatch =
        nHeight >= 0 &&
        nLastResponseMinHeight >= 0 &&
        nLastResponseMaxHeight >= nLastResponseMinHeight &&
        nHeight >= nLastResponseMinHeight &&
        nHeight <= nLastResponseMaxHeight;
    if (!fHashMatch && !fHeightMatch)
        return false;

    nUsefulGetData++;
    nUsefulGetDataSinceLastResponse++;
    nConsecutiveIdenticalRequests = 0;
    nConsecutiveNonProgressingRequests = 0;
    nRepeatAllowedAfterMillis = 0;
    return true;
}

const char* GetBlocksServerActionName(GetBlocksServerAction action)
{
    switch (action)
    {
    case GETBLOCKS_SERVER_ALLOW:
        return "allow";
    case GETBLOCKS_SERVER_SUPPRESS:
        return "suppress";
    case GETBLOCKS_SERVER_RATE_LIMIT:
        return "rate-limit";
    case GETBLOCKS_SERVER_DISCONNECT:
        return "disconnect";
    }
    return "unknown";
}

struct ListenSocket {
    SOCKET socket;
    bool whitelisted;

    ListenSocket(SOCKET socket, bool whitelisted) : socket(socket), whitelisted(whitelisted) {}
};

//
// Global state variables
//
bool fDiscover = true;
bool fUseUPnP = false;
bool fAddressesInitialized = false;
uint64_t nLocalServices = NODE_NETWORK;
CCriticalSection cs_mapLocalHost;
map<CNetAddr, LocalServiceInfo> mapLocalHost;
static bool vfReachable[NET_MAX] = {};
static bool vfLimited[NET_MAX] = {};
static CNode* pnodeLocalHost = NULL;
static CNode* pnodeSync = NULL;
static CCriticalSection cs_pnodeSync;  // Protects pnodeSync pointer
NodeId nLastNodeId = 0;
CCriticalSection cs_nLastNodeId;

static CCriticalSection cs_stalledSyncRecovery;
static CStalledSyncRecoveryState stalledSyncRecoveryState;
static CCriticalSection cs_getInfoProbeSnapshot;
static std::string strGetInfoProbeSnapshot;
static int64_t nGetInfoProbeSnapshotTime = 0;

CStalledSyncRecoveryState::CStalledSyncRecoveryState()
    : nLastObservedHeight(-1),
      nLastProgressTime(0),
      nLastRecoveryTime(0),
      hashRejectedBlock(),
      nRejectedBlockTime(0),
      fRejectedRetryScheduled(false),
      nRecoveryAttempts(0),
      fSyncRequestSent(false)
{
}

bool CStalledSyncRecoveryState::ShouldRecover(
    int nLocalHeight, int nPeerHeight, bool fPipelineActive,
    int64_t nNow, int64_t nStallTimeout, int64_t nCooldown)
{
    if (!fSyncRequestSent)
        return false;

    if (nLastObservedHeight != nLocalHeight ||
        (nLastProgressTime != 0 && nNow < nLastProgressTime))
    {
        nLastObservedHeight = nLocalHeight;
        nLastProgressTime = nNow;
        nLastRecoveryTime = 0;
        nRecoveryAttempts = 0;
        return false;
    }

    if (nPeerHeight <= nLocalHeight || fPipelineActive)
        return false;

    const unsigned int nBackoffShift =
        std::min<unsigned int>(nRecoveryAttempts, 5);
    const int64_t nEffectiveCooldown =
        nCooldown * ((int64_t)1 << nBackoffShift);
    const int64_t nSinceProgress =
        nLastProgressTime == 0 ? 0 : nNow - nLastProgressTime;
    const int64_t nSinceRecovery =
        nLastRecoveryTime == 0
            ? nEffectiveCooldown
            : nNow - nLastRecoveryTime;
    if (nSinceProgress < nStallTimeout ||
        nSinceRecovery < nEffectiveCooldown)
    {
        return false;
    }

    nLastRecoveryTime = nNow;
    ++nRecoveryAttempts;
    return true;
}

void RecordSyncRequestSent(int64_t nNow)
{
    LOCK(cs_stalledSyncRecovery);
    stalledSyncRecoveryState.MarkSyncRequestSent(nNow);
}

void CStalledSyncRecoveryState::MarkSyncRequestSent(int64_t nNow)
{
    fSyncRequestSent = true;
    if (nLastProgressTime == 0)
    {
        nLastProgressTime = nNow;
        nLastObservedHeight = nBestHeight;
    }
}

void CStalledSyncRecoveryState::RecordRejectedBlock(
    const uint256& hashBlock, int64_t nNow)
{
    if (hashBlock == 0)
        return;
    if (hashRejectedBlock != hashBlock)
    {
        fRejectedRetryScheduled = false;
        nRecoveryAttempts = 0;
    }
    hashRejectedBlock = hashBlock;
    nRejectedBlockTime = nNow;
}

void CStalledSyncRecoveryState::ClearRejectedBlock(const uint256& hashBlock)
{
    if (hashRejectedBlock != hashBlock)
        return;
    hashRejectedBlock = 0;
    nRejectedBlockTime = 0;
    fRejectedRetryScheduled = false;
}

bool CStalledSyncRecoveryState::TakeRejectedBlockForRetry(uint256& hashBlock)
{
    if (hashRejectedBlock == 0 || fRejectedRetryScheduled)
        return false;
    hashBlock = hashRejectedBlock;
    fRejectedRetryScheduled = true;
    return true;
}

void RecordRejectedBlockForSync(const uint256& hashBlock)
{
    LOCK(cs_stalledSyncRecovery);
    stalledSyncRecoveryState.RecordRejectedBlock(hashBlock, GetTime());
    if (SyncTraceEnabled())
        printf("SYNC_EVENT time_us=%lld event=REJECT_RETRY_RECORDED hash=%s\n",
               (long long)GetTimeMicros(), hashBlock.ToString().c_str());
}

void ClearRejectedBlockForSync(const uint256& hashBlock)
{
    LOCK(cs_stalledSyncRecovery);
    stalledSyncRecoveryState.ClearRejectedBlock(hashBlock);
}

bool SyncTraceEnabled()
{
    // Detailed sync lifecycle events require the explicit block-request trace.
    // Probe and lock diagnostics must not enable this stream.
    return BlockRequestTraceEnabled();
}
CSyncLockDiagnostics::CSyncLockDiagnostics(
    const char* pszLocationIn, const char* pszLocksIn)
    : pszLocation(pszLocationIn),
      pszLocks(pszLocksIn),
      nWaitStartTime(GetBoolArg("-synclockdiagnostics", false)
                         ? GetTimeMicros() : 0),
      nAcquiredTime(0),
      fEnabled(GetBoolArg("-synclockdiagnostics", false))
{
}

void CSyncLockDiagnostics::Acquired()
{
    if (fEnabled && nAcquiredTime == 0)
        nAcquiredTime = GetTimeMicros();
}

CSyncLockDiagnostics::~CSyncLockDiagnostics()
{
    if (!fEnabled || nAcquiredTime == 0)
        return;

    const int64_t nEndTime = GetTimeMicros();
    const int64_t nWaitMicros =
        std::max<int64_t>(0, nAcquiredTime - nWaitStartTime);
    const int64_t nHoldMicros =
        std::max<int64_t>(0, nEndTime - nAcquiredTime);
    const int64_t nThresholdMicros =
        std::max<int64_t>(
            1, GetArg("-synclockthresholdms", 250)) * 1000;
    if (nWaitMicros < nThresholdMicros &&
        nHoldMicros < nThresholdMicros)
    {
        return;
    }

    std::ostringstream threadName;
    threadName << boost::this_thread::get_id();
    printf("SYNCLOCK time_us=%lld thread=%s location=%s locks=%s wait_us=%lld hold_us=%lld threshold_us=%lld\n",
           (long long)nEndTime, threadName.str().c_str(),
           pszLocation, pszLocks, (long long)nWaitMicros,
           (long long)nHoldMicros, (long long)nThresholdMicros);
}

void LogGetInfoSyncProbe(const char* pszEvent,
                         int64_t nRequestStartTime,
                         int64_t nLockWaitMicros)
{
    if (!GetBoolArg("-getinfosyncprobe", false))
        return;

    std::string strSnapshot;
    int64_t nSnapshotTime = 0;
    {
        LOCK(cs_getInfoProbeSnapshot);
        strSnapshot = strGetInfoProbeSnapshot;
        nSnapshotTime = nGetInfoProbeSnapshotTime;
    }

    const int64_t nNow = GetTimeMicros();
    printf("GETINFO_PROBE_%s time_us=%lld request_start_us=%lld lock_wait_us=%lld snapshot_time_us=%lld snapshot_age_us=%lld %s\n",
           pszEvent, (long long)nNow,
           (long long)nRequestStartTime,
           (long long)nLockWaitMicros,
           (long long)nSnapshotTime,
           (long long)(nSnapshotTime == 0
                           ? -1
                           : std::max<int64_t>(0, nNow - nSnapshotTime)),
           strSnapshot.empty() ? "snapshot=unavailable"
                               : strSnapshot.c_str());
}


RecoveryResponseWindowState::RecoveryResponseWindowState() : active(false), recovery_id(0), send_time_us(0), deadline_us(0), inv_message_count(0), total_inv(0), block_inv(0), unknown_blocks(0), known_active_blocks(0), known_nonactive_indexed_blocks(0), known_orphan_blocks(0), first_block_elapsed_us(-1), first_unknown_elapsed_us(-1) {}
void RecoveryResponseWindowState::Start(uint64_t id, int64_t send_us) { active=true; recovery_id=id; send_time_us=send_us; deadline_us=send_us+RECOVERY_RESPONSE_WINDOW_US; inv_message_count=total_inv=block_inv=unknown_blocks=known_active_blocks=known_nonactive_indexed_blocks=known_orphan_blocks=0; first_block_hash=first_unknown_block_hash=0; first_block_elapsed_us=first_unknown_elapsed_us=-1; }
bool RecoveryResponseWindowState::Finish(int64_t now, RecoveryResponseOutcome outcome, RecoveryResponseResult& r) { if(!active) return false; r.outcome=outcome; r.recovery_id=recovery_id; r.send_time_us=send_time_us; r.elapsed_us=now-send_time_us; r.inv_message_count=inv_message_count; r.total_inv=total_inv; r.block_inv=block_inv; r.unknown_blocks=unknown_blocks; r.known_active_blocks=known_active_blocks; r.known_nonactive_indexed_blocks=known_nonactive_indexed_blocks; r.known_orphan_blocks=known_orphan_blocks; r.first_block_hash=first_block_hash; r.first_unknown_block_hash=first_unknown_block_hash; r.first_block_elapsed_us=first_block_elapsed_us; r.first_unknown_elapsed_us=first_unknown_elapsed_us; active=false; return true; }
bool RecoveryResponseWindowState::ObserveInv(int64_t now,const RecoveryResponseObservation& o,RecoveryResponseResult& r) { if(!active) return false; if(now>=deadline_us) { Expire(now,r); return true; } ++inv_message_count; total_inv+=o.total_inv; block_inv+=o.block_inv; unknown_blocks+=o.unknown_blocks; known_active_blocks+=o.known_active_blocks; known_nonactive_indexed_blocks+=o.known_nonactive_indexed_blocks; known_orphan_blocks+=o.known_orphan_blocks; if(block_inv && first_block_hash==0 && o.first_block_hash!=0){first_block_hash=o.first_block_hash; first_block_elapsed_us=now-send_time_us;} if(unknown_blocks && first_unknown_block_hash==0 && o.first_unknown_block_hash!=0){first_unknown_block_hash=o.first_unknown_block_hash; first_unknown_elapsed_us=now-send_time_us;} return false; }
bool RecoveryResponseWindowState::Expire(int64_t now,RecoveryResponseResult& r) { if(!active || now<deadline_us) return false; return Finish(now, unknown_blocks ? RECOVERY_OUTCOME_USEFUL : (block_inv ? RECOVERY_OUTCOME_KNOWN_ONLY_TIMEOUT : RECOVERY_OUTCOME_EMPTY_TIMEOUT), r); }
bool RecoveryResponseWindowState::Supersede(int64_t now,RecoveryResponseResult& r) { return Finish(now, RECOVERY_OUTCOME_SUPERSEDED_BY_NEXT_RECOVERY, r); }
bool RecoveryResponseWindowState::Disconnect(int64_t now,RecoveryResponseResult& r) { return Finish(now, RECOVERY_OUTCOME_DISCONNECTED, r); }

void CNode::StartRecoveryResponseWindow(uint64_t id, int64_t send_us) { recovery_response_window.Start(id, send_us); }
bool CNode::ObserveRecoveryResponseInv(int64_t now, const RecoveryResponseObservation& o, RecoveryResponseResult& r) { return recovery_response_window.ObserveInv(now, o, r); }
bool CNode::ExpireRecoveryResponseWindow(int64_t now, RecoveryResponseResult& r) { return recovery_response_window.Expire(now, r); }
bool CNode::SupersedeRecoveryResponseWindow(int64_t now, RecoveryResponseResult& r) { return recovery_response_window.Supersede(now, r); }
bool CNode::DisconnectRecoveryResponseWindow(int64_t now, RecoveryResponseResult& r) { return recovery_response_window.Disconnect(now, r); }

const char* RecoveryResponseOutcomeName(RecoveryResponseOutcome outcome)
{
    switch (outcome) {
    case RECOVERY_OUTCOME_USEFUL: return "useful";
    case RECOVERY_OUTCOME_KNOWN_ONLY_TIMEOUT: return "known_only_timeout";
    case RECOVERY_OUTCOME_EMPTY_TIMEOUT: return "empty_timeout";
    case RECOVERY_OUTCOME_DISCONNECTED: return "disconnected";
    case RECOVERY_OUTCOME_SUPERSEDED_BY_NEXT_RECOVERY: return "superseded_by_next_recovery";
    }
    return "unknown";
}

std::string FormatRecoveryResponseSummary(int64_t peer_id, const RecoveryResponseResult& r)
{
    return strprintf("RECOVERY_SUMMARY recovery_id=%llu peer_id=%lld outcome=%s elapsed_us=%lld total_inv=%llu block_inv=%llu unknown_blocks=%llu known_active_blocks=%llu known_nonactive_indexed_blocks=%llu known_orphan_blocks=%llu inv_message_count=%llu first_block_hash=%s first_unknown_block_hash=%s first_block_elapsed_us=%lld first_unknown_elapsed_us=%lld",
        (unsigned long long)r.recovery_id, (long long)peer_id, RecoveryResponseOutcomeName(r.outcome),
        (long long)r.elapsed_us, (unsigned long long)r.total_inv, (unsigned long long)r.block_inv,
        (unsigned long long)r.unknown_blocks, (unsigned long long)r.known_active_blocks,
        (unsigned long long)r.known_nonactive_indexed_blocks, (unsigned long long)r.known_orphan_blocks,
        (unsigned long long)r.inv_message_count, r.first_block_hash.ToString().c_str(),
        r.first_unknown_block_hash.ToString().c_str(), (long long)r.first_block_elapsed_us,
        (long long)r.first_unknown_elapsed_us);
}

static uint64_t nNextRecoveryTraceId = 0;

uint64_t RecoveryTraceTrigger(CNode* pnode, int nLocalHeight, int nPeerHeight,
                              int64_t nStallAge, unsigned int nAttempt)
{
    if (!BlockRequestTraceEnabled()) return 0;
    const uint64_t id = ++nNextRecoveryTraceId;
    printf("RECOVERY_TRIGGER recovery_id=%llu time_us=%lld peer_id=%d peer_addr=%s peer_version=%d local_height=%d peer_height=%d stall_age=%lld recovery_attempt=%u\n",
           (unsigned long long)id, (long long)GetTimeMicros(), pnode->GetId(),
           pnode->addr.ToString().c_str(), pnode->nVersion, nLocalHeight,
           nPeerHeight, (long long)nStallAge, nAttempt);
    return id;
}

void RecoveryTraceQueue(CNode* pnode, uint64_t id, CBlockIndex* pindexBegin,
                        uint256 hashStop, size_t before, size_t after)
{
    if (!id || !BlockRequestTraceEnabled()) return;
    printf("RECOVERY_GETBLOCKS_QUEUE recovery_id=%llu peer_id=%d locator_tip=%s locator_height=%d stop_hash=%s queue_before=%zu queue_after=%zu\n",
           (unsigned long long)id, pnode->GetId(),
           pindexBegin ? pindexBegin->GetBlockHash().ToString().c_str() : uint256(0).ToString().c_str(),
           pindexBegin ? pindexBegin->nHeight : -1, hashStop.ToString().c_str(), before, after);
}

void RecoveryTraceSend(CNode* pnode, uint64_t id, CBlockIndex* pindexBegin,
                       uint256 hashStop, size_t before)
{
    if (!id || !BlockRequestTraceEnabled()) return;
    printf("RECOVERY_GETBLOCKS_SEND recovery_id=%llu peer_id=%d locator_tip=%s locator_height=%d stop_hash=%s queue_size_before_clear=%zu\n",
           (unsigned long long)id, pnode->GetId(),
           pindexBegin ? pindexBegin->GetBlockHash().ToString().c_str() : uint256(0).ToString().c_str(),
           pindexBegin ? pindexBegin->nHeight : -1, hashStop.ToString().c_str(), before);
    RecoveryResponseResult previous;
    if (pnode->SupersedeRecoveryResponseWindow(GetTimeMicros(), previous))
        printf("%s\n", FormatRecoveryResponseSummary(pnode->GetId(), previous).c_str());
    pnode->StartRecoveryResponseWindow(id, GetTimeMicros());
}

CNode* MaybeQueueStalledSyncRecovery(
    const std::vector<CNode*>& vNodesIn, CBlockIndex* pindexTip,
    int nLocalHeight, int64_t nNow, int64_t nStallTimeout,
    int64_t nCooldown, CStalledSyncRecoveryState& state)
{
    int64_t nMaxPeerHeight = -1;
    bool fPipelineActive = false;
    std::vector<CNode*> vEligiblePeers;

    CNode* pnodeSyncCopy = NULL;
    {
        LOCK(cs_pnodeSync);
        if (pnodeSync != NULL &&
            std::find(vNodesIn.begin(), vNodesIn.end(), pnodeSync) !=
                vNodesIn.end())
        {
            pnodeSyncCopy = pnodeSync;
        }
    }

    BOOST_FOREACH(CNode* pnode, vNodesIn)
    {
        if (pnode == NULL || pnode->fDisconnect ||
            !pnode->fSuccessfullyConnected || pnode->fClient ||
            pnode->fOneShot || !IsBlockSyncPeerVersion(pnode->nVersion))
        {
            continue;
        }

        const int64_t nPeerHeight = GetPeerAdvertisedHeight(pnode);
        nMaxPeerHeight = std::max(nMaxPeerHeight, nPeerHeight);
        if (nPeerHeight > nLocalHeight)
            vEligiblePeers.push_back(pnode);

        if (!pnode->setBlocksInFlight.empty() ||
            !pnode->getBlocksIndex.empty())
        {
            fPipelineActive = true;
        }
    }
    std::vector<std::pair<int64_t, CNode*> > vCandidates;
    BOOST_FOREACH(CNode* pnode, vEligiblePeers)
    {
        vCandidates.push_back(std::make_pair(
            SyncPeerScore(pnode, nNow, nMaxPeerHeight), pnode));
    }

    const int nObservedHeightBefore = state.LastObservedHeight();
    const int64_t nStallStartBefore = state.LastProgressTime();
    const int64_t nLastRecoveryBefore = state.LastRecoveryTime();
    const unsigned int nRecoveryAttemptsBefore = state.RecoveryAttempts();
    const int64_t nStallAgeBefore = nStallStartBefore == 0
        ? -1 : std::max<int64_t>(0, nNow - nStallStartBefore);
    const unsigned int nBackoffShift =
        std::min<unsigned int>(nRecoveryAttemptsBefore, 5);
    const int64_t nEffectiveCooldown =
        nCooldown * ((int64_t)1 << nBackoffShift);
    const int64_t nSinceRecovery = nLastRecoveryBefore == 0
        ? nEffectiveCooldown : nNow - nLastRecoveryBefore;
    const int64_t nCooldownRemaining =
        std::max<int64_t>(0, nEffectiveCooldown - nSinceRecovery);

    bool fShouldRecover = false;
    bool fShouldRecoverEvaluated = false;
    if (pindexTip != NULL && !vEligiblePeers.empty())
    {
        fShouldRecoverEvaluated = true;
        fShouldRecover = state.ShouldRecover(
            nLocalHeight, (int)nMaxPeerHeight, fPipelineActive, nNow,
            nStallTimeout, nCooldown);
    }

    const char* pszFinalSkipReason = "none";
    if (pindexTip == NULL)
        pszFinalSkipReason = "missing_tip";
    else if (vEligiblePeers.empty())
        pszFinalSkipReason = "no_eligible_peers";
    else if (!fShouldRecover)
    {
        if (nObservedHeightBefore != nLocalHeight)
            pszFinalSkipReason = "local_height_changed";
        else if (nStallStartBefore != 0 && nNow < nStallStartBefore)
            pszFinalSkipReason = "clock_reversal";
        else if (nMaxPeerHeight <= nLocalHeight)
            pszFinalSkipReason = "peer_not_ahead";
        else if (fPipelineActive)
            pszFinalSkipReason = "pipeline_active";
        else if (nStallAgeBefore < nStallTimeout)
            pszFinalSkipReason = "stall_timeout_not_reached";
        else if (nCooldownRemaining > 0)
            pszFinalSkipReason = "cooldown_active";
        else
            pszFinalSkipReason = "should_recover_false_unclassified";
    }

    // Deferred block-type inventory requests are diagnostic state, not active
    // downloads.  Keep reporting them without treating them as pipeline work.
    size_t nSyncPeerBlockAskFor = 0;
    if (BlockRequestTraceEnabled() && pnodeSyncCopy != NULL)
    {
        for (std::multimap<int64_t, CInv>::const_iterator it =
                 pnodeSyncCopy->mapAskFor.begin();
             it != pnodeSyncCopy->mapAskFor.end(); ++it)
        {
            if (it->second.type == MSG_BLOCK ||
                it->second.type == MSG_FILTERED_BLOCK)
            {
                ++nSyncPeerBlockAskFor;
            }
        }
    }
    const int nPeerStartHeight =
        pnodeSyncCopy == NULL ? -1 : pnodeSyncCopy->nChainHeight;
    const int nPeerBestKnownHeight =
        pnodeSyncCopy == NULL ? -1 : pnodeSyncCopy->nBestKnownHeight;
    const int nEffectivePeerHeight = pnodeSyncCopy == NULL
        ? -1
        : (pnodeSyncCopy->nBestKnownHeight >= 0
               ? pnodeSyncCopy->nBestKnownHeight
               : pnodeSyncCopy->nChainHeight);
    const int nCanAdvanceBlockSync = pnodeSyncCopy == NULL
        ? -1
        : (pnodeSyncCopy->CanAdvanceBlockSync(nLocalHeight) ? 1 : 0);
    if (BlockRequestTraceEnabled())
    {
        printf("IBD_RECOVERY_DECISION time=%lld peer=%lld local_height=%d peer_start_height=%d peer_best_known_height=%d effective_peer_height=%d max_peer_height=%lld fStartSync=%d blocks_in_flight=%u askfor_block_requests=%u queued_getblocks=%u pipeline_active=%d stall_start_time=%lld stall_age=%lld stall_timeout=%lld cooldown_remaining=%lld recovery_attempts=%u can_advance_block_sync=%d should_recover=%d final_skip_reason=%s\n",
               (long long)nNow,
               (long long)(pnodeSyncCopy == NULL ? -1 : pnodeSyncCopy->GetId()),
                   nLocalHeight, nPeerStartHeight, nPeerBestKnownHeight,
                   nEffectivePeerHeight, (long long)nMaxPeerHeight,
                   pnodeSyncCopy == NULL ? -1 : (pnodeSyncCopy->fStartSync ? 1 : 0),
                   (unsigned int)(pnodeSyncCopy == NULL
                                      ? 0 : pnodeSyncCopy->setBlocksInFlight.size()),
                   (unsigned int)nSyncPeerBlockAskFor,
                   (unsigned int)(pnodeSyncCopy == NULL
                                      ? 0 : pnodeSyncCopy->getBlocksIndex.size()),
                   fPipelineActive ? 1 : 0, (long long)state.LastProgressTime(),
                   (long long)(state.LastProgressTime() == 0
                                   ? -1
                                   : std::max<int64_t>(
                                         0, nNow - state.LastProgressTime())),
                   (long long)nStallTimeout, (long long)nCooldownRemaining,
                   state.RecoveryAttempts(), nCanAdvanceBlockSync,
                   fShouldRecoverEvaluated ? (fShouldRecover ? 1 : 0) : -1,
                   pszFinalSkipReason);
    }

    if (!fShouldRecover)
    {
        return NULL;
    }
    std::sort(vCandidates.begin(), vCandidates.end(), CompareSyncCandidates);
    const size_t nOwnerIndex =
        (state.RecoveryAttempts() - 1) % vCandidates.size();
    CNode* pnodeRecovery = vCandidates[nOwnerIndex].second;
    pnodeRecovery->nRecoveryTracePendingId = RecoveryTraceTrigger(
        pnodeRecovery, nLocalHeight, (int)nMaxPeerHeight, nStallAgeBefore,
        state.RecoveryAttempts());
    pnodeRecovery->PushGetBlocks(pindexTip, uint256(0));

    uint256 hashRejected;
    if (state.TakeRejectedBlockForRetry(hashRejected))
    {
        pnodeRecovery->AskFor(CInv(MSG_BLOCK, hashRejected),
                              BLOCKREQ_SOURCE_REJECT_RECOVERY);
        if (SyncTraceEnabled())
            printf("SYNC_EVENT time_us=%lld event=REJECT_RETRY_SCHEDULED peer=%d hash=%s\n",
                   (long long)GetTimeMicros(),
                   pnodeRecovery->GetId(),
                   hashRejected.ToString().c_str());
    }

    if (BlockRequestTraceEnabled())
    {
        const int64_t nAge = state.LastProgressTime() == 0
            ? -1 : std::max<int64_t>(0, nNow - state.LastProgressTime());
        const std::vector<uint256> vNoErasedHashes;
        BlockRequestTraceStallRecovery(
            pnodeRecovery, nLocalHeight,
            (int)GetPeerAdvertisedHeight(pnodeRecovery), nAge,
            pindexTip->GetBlockHash(), pindexTip->nHeight,
            uint256(0), vNoErasedHashes);
    }
    if (SyncTraceEnabled())
    {
        printf("SYNC_EVENT time_us=%lld event=STALL_RECOVERY_OWNER peer=%d local_height=%d peer_height=%lld rejected_retry=%s\n",
               (long long)GetTimeMicros(), pnodeRecovery->GetId(),
               nLocalHeight,
               (long long)GetPeerAdvertisedHeight(pnodeRecovery),
               hashRejected.ToString().c_str());
    }
    return pnodeRecovery;
}

static void UpdateGetInfoSyncProbeSnapshot(
    const std::vector<CNode*>& vNodesCopy)
{
    if (!GetBoolArg("-getinfosyncprobe", false))
        return;

    int nLocalHeight = -1;
    int nBestHeaderHeight = -1;
    int64_t nLastAcceptedBlockTime = 0;
    bool fInitialBlockDownload = true;
    {
        TRY_LOCK(cs_main, lockMain);
        if (!lockMain)
            return;
        nLocalHeight = nBestHeight;
        // Full-node mode has no independent header chain in this client.
        nBestHeaderHeight =
            fSPVMode && pindexBest ? pindexBest->nHeight : nBestHeight;
        nLastAcceptedBlockTime = nTimeBestReceived;
        fInitialBlockDownload = IsInitialBlockDownload();
    }

    CNode* pnodeSyncCopy = NULL;
    {
        LOCK(cs_pnodeSync);
        if (pnodeSync != NULL &&
            std::find(vNodesCopy.begin(), vNodesCopy.end(),
                      pnodeSync) != vNodesCopy.end())
        {
            pnodeSyncCopy = pnodeSync;
        }
        else
        {
            pnodeSync = NULL;
        }
    }

    int64_t nMaxPeerHeight = -1;
    size_t nBlocksInFlight = 0;
    size_t nQueuedGetBlocks = 0;
    size_t nTotalAskFor = 0;
    int64_t nLastBlockReceiveTime = 0;
    int64_t nLastGetBlocksTime = 0;
    int64_t nLastGetDataTime = 0;
    std::ostringstream peerStates;
    bool fFirstPeer = true;
    BOOST_FOREACH(CNode* pnode, vNodesCopy)
    {
        const int64_t nPeerHeight =
            GetPeerAdvertisedHeight(pnode);
        nMaxPeerHeight =
            std::max(nMaxPeerHeight, nPeerHeight);
        nBlocksInFlight += pnode->setBlocksInFlight.size();
        nQueuedGetBlocks += pnode->getBlocksIndex.size();
        nTotalAskFor += pnode->mapAskFor.size();
        nLastBlockReceiveTime =
            std::max(nLastBlockReceiveTime,
                     pnode->nLastBlockRecv);
        nLastGetBlocksTime =
            std::max(nLastGetBlocksTime,
                     pnode->nLastGetBlocksTime);
        nLastGetDataTime =
            std::max(nLastGetDataTime,
                     pnode->nLastGetDataTime);

        if (!fFirstPeer)
            peerStates << ';';
        fFirstPeer = false;
        peerStates
            << pnode->GetId() << ':'
            << nPeerHeight << ':'
            << (pnode->fStartSync ? 1 : 0) << ':'
            << pnode->mapAskFor.size() << ':'
            << pnode->setBlocksInFlight.size() << ':'
            << pnode->getBlocksIndex.size() << ':'
            << pnode->nLastBlockRecv << ':'
            << pnode->nLastGetBlocksTime << ':'
            << pnode->nLastGetDataTime;
    }

    size_t nAlreadyAskedFor = 0;
    size_t nAlreadyAskedForBlocks = 0;
    size_t nAlreadyAskedForTransactions = 0;
    size_t nAlreadyAskedForOther = 0;
    {
        LOCK(cs_mapAlreadyAskedFor);
        nAlreadyAskedFor = mapAlreadyAskedFor.size();
        for (std::map<CInv, int64_t>::const_iterator it =
                 mapAlreadyAskedFor.begin();
             it != mapAlreadyAskedFor.end(); ++it)
        {
            if (it->first.type == MSG_BLOCK ||
                it->first.type == MSG_FILTERED_BLOCK)
                ++nAlreadyAskedForBlocks;
            else if (it->first.type == MSG_TX)
                ++nAlreadyAskedForTransactions;
            else
                ++nAlreadyAskedForOther;
        }
    }

    int nCollateralListState = 0;
    size_t nCollateralListCount = 0;
    unsigned int nCollateralMedianCount = 0;
    {
        TRY_LOCK(cs_collateralnodes, lockCollateralnodes);
        if (lockCollateralnodes)
        {
            nCollateralListState = 1;
            nCollateralListCount = vecCollateralnodes.size();
            nCollateralMedianCount = mnCount;
        }
    }

    uint256 hashRejected;
    int64_t nRejectedBlockTime = 0;
    int64_t nRecoveryLastProgress = 0;
    int64_t nRecoveryLastAttempt = 0;
    unsigned int nRecoveryAttempts = 0;
    {
        LOCK(cs_stalledSyncRecovery);
        hashRejected = stalledSyncRecoveryState.RejectedBlock();
        nRejectedBlockTime =
            stalledSyncRecoveryState.RejectedBlockTime();
        nRecoveryLastProgress =
            stalledSyncRecoveryState.LastProgressTime();
        nRecoveryLastAttempt =
            stalledSyncRecoveryState.LastRecoveryTime();
        nRecoveryAttempts =
            stalledSyncRecoveryState.RecoveryAttempts();
    }

    std::ostringstream snapshot;
    snapshot
        << "local_height=" << nLocalHeight
        << " best_header=" << nBestHeaderHeight
        << " max_peer_height=" << nMaxPeerHeight
        << " initialblockdownload="
        << (fInitialBlockDownload ? 1 : 0)
        << " connections=" << vNodesCopy.size()
        << " sync_peer="
        << (pnodeSyncCopy ? pnodeSyncCopy->GetId() : -1)
        << " blocks_in_flight=" << nBlocksInFlight
        << " queued_getblocks=" << nQueuedGetBlocks
        << " total_askfor=" << nTotalAskFor
        << " mapAskFor_size=" << nTotalAskFor
        << " mapAlreadyAskedFor_size=" << nAlreadyAskedFor
        << " mapAlreadyAskedFor_blocks=" << nAlreadyAskedForBlocks
        << " mapAlreadyAskedFor_transactions=" << nAlreadyAskedForTransactions
        << " mapAlreadyAskedFor_other=" << nAlreadyAskedForOther
        << " mapAlreadyAskedFor_cap=" << MAX_ALREADY_ASKED_FOR_SIZE
        << " last_block_receive_time="
        << nLastBlockReceiveTime
        << " last_accepted_block_time="
        << nLastAcceptedBlockTime
        << " last_getblocks_time=" << nLastGetBlocksTime
        << " last_getdata_time=" << nLastGetDataTime
        << " collateralnode_sync_state=unavailable"
        << " collateralnode_list_height=-1"
        << " collateralnode_list_state="
        << nCollateralListState
        << " collateralnode_list_count="
        << nCollateralListCount
        << " collateralnode_median_count="
        << nCollateralMedianCount
        << " recovery_last_progress_time="
        << nRecoveryLastProgress
        << " recovery_last_attempt_time="
        << nRecoveryLastAttempt
        << " recovery_attempts=" << nRecoveryAttempts
        << " rejected_retry_hash="
        << hashRejected.ToString()
        << " rejected_retry_time=" << nRejectedBlockTime
        << " cs_main_diagnostics="
        << (GetBoolArg("-synclockdiagnostics", false)
                ? "threshold"
                : "disabled")
        << " peer_state_format=id:height:fStartSync:askfor:inflight:getblocks:last_block:last_getblocks:last_getdata"
        << " peer_states=" << peerStates.str();

    const int64_t nSnapshotTime = GetTimeMicros();
    {
        LOCK(cs_getInfoProbeSnapshot);
        strGetInfoProbeSnapshot = snapshot.str();
        nGetInfoProbeSnapshotTime = nSnapshotTime;
    }
}


void LogSyncDiagnosticsMaybe()
{
    if (!GetBoolArg("-getinfosyncprobe", false))
        return;

    const int64_t nNow = GetTime();
    const int64_t nInterval = 45;
    if (nLastSyncDiagnosticsLog != 0 && nNow - nLastSyncDiagnosticsLog < nInterval)
        return;

    std::vector<CNode*> vNodesCopy;
    CNode* pnodeSyncCopy = NULL;
    {
        LOCK(cs_vNodes);
        vNodesCopy = vNodes;
        BOOST_FOREACH(CNode* pnode, vNodesCopy)
            pnode->AddRef();
    }
    {
        LOCK(cs_pnodeSync);
        if (pnodeSync != NULL &&
            std::find(vNodesCopy.begin(), vNodesCopy.end(),
                      pnodeSync) != vNodesCopy.end())
        {
            pnodeSyncCopy = pnodeSync;
        }
        else
        {
            pnodeSync = NULL;
        }
    }

    int nLocalHeight = -1;
    int nBestHeaderHeight = -1;
    bool fInitialBlockDownload = true;
    {
        TRY_LOCK(cs_main, lockMain);
        if (!lockMain)
        {
            BOOST_FOREACH(CNode* pnode, vNodesCopy)
                pnode->Release();
            return;
        }
        nLocalHeight = nBestHeight;
        nBestHeaderHeight =
            fSPVMode && pindexBest ? pindexBest->nHeight : nBestHeight;
        fInitialBlockDownload = IsInitialBlockDownload();
    }

    int64_t nMaxPeerHeight = -1;
    int64_t nTotalBlocksInFlight = 0;
    int64_t nTotalQueuedGetBlocks = 0;
    uint64_t nTotalBytesRecv = 0;
    uint64_t nTotalBytesSent = 0;
    uint64_t nTotalSendQueueBytes = 0;
    uint64_t nTotalRecvQueueBytes = 0;
    uint64_t nTotalAskFor = 0;

    BOOST_FOREACH(CNode* pnode, vNodesCopy) {
        nTotalBlocksInFlight += pnode->setBlocksInFlight.size();
        nTotalQueuedGetBlocks += pnode->getBlocksIndex.size();
        nTotalBytesRecv += pnode->nRecvBytes;
        nTotalBytesSent += pnode->nSendBytes;

        int64_t nPeerHeight = GetPeerAdvertisedHeight(pnode);
        if (nPeerHeight > nMaxPeerHeight)
            nMaxPeerHeight = nPeerHeight;
    }

    std::vector<std::pair<int64_t, CNode*> > vCandidates;
    BOOST_FOREACH(CNode* pnode, vNodesCopy) {
        if (pnode->fDisconnect || !pnode->fSuccessfullyConnected || pnode->fClient || pnode->fOneShot)
            continue;
        if (!IsBlockSyncPeerVersion(pnode->nVersion))
            continue;
        vCandidates.push_back(std::make_pair(SyncPeerScore(pnode, nNow, nMaxPeerHeight), pnode));
    }
    std::sort(vCandidates.begin(), vCandidates.end(), CompareSyncCandidates);

    uint64_t nDeltaRecv =
        (nLastSyncDiagnosticsBytesRecv == 0 ||
         nTotalBytesRecv < nLastSyncDiagnosticsBytesRecv)
            ? 0
            : nTotalBytesRecv - nLastSyncDiagnosticsBytesRecv;
    uint64_t nDeltaSent =
        (nLastSyncDiagnosticsBytesSent == 0 ||
         nTotalBytesSent < nLastSyncDiagnosticsBytesSent)
            ? 0
            : nTotalBytesSent - nLastSyncDiagnosticsBytesSent;
    uint64_t nProcessVmRssKb = 0;
    uint64_t nProcessVmSizeKb = 0;
    uint64_t nProcessVmDataKb = 0;
    uint64_t nProcessVmSwapKb = 0;
    int nProcessThreads = 0;
    GetProcessMemorySnapshot(nProcessVmRssKb, nProcessVmSizeKb, nProcessVmDataKb, nProcessVmSwapKb, nProcessThreads);

    BOOST_FOREACH(CNode* pnode, vNodesCopy) {
        size_t nSendQueueBytes = 0;
        size_t nRecvQueueBytes = 0;
        size_t nAskForSize = 0;
        {
            TRY_LOCK(pnode->cs_vSend, lockSend);
            if (lockSend)
                nSendQueueBytes = pnode->nSendSize;
        }
        {
            TRY_LOCK(pnode->cs_vRecvMsg, lockRecv);
            if (lockRecv)
                nRecvQueueBytes = pnode->GetTotalRecvSize();
        }
        nAskForSize = pnode->mapAskFor.size();
        nTotalSendQueueBytes += nSendQueueBytes;
        nTotalRecvQueueBytes += nRecvQueueBytes;
        nTotalAskFor += nAskForSize;
    }

    {
        LOCK(cs_p2pMessageStats);
        std::vector<std::string> vCommands;
        vCommands.push_back("version");
        vCommands.push_back("verack");
        vCommands.push_back("addr");
        vCommands.push_back("inv");
        vCommands.push_back("getdata");
        vCommands.push_back("block");
        vCommands.push_back("tx");
        vCommands.push_back("headers");
        vCommands.push_back("getheaders");
        vCommands.push_back("getblocks");
        vCommands.push_back("ping");
        vCommands.push_back("pong");
        vCommands.push_back("reject");
        vCommands.push_back("notfound");
        printf("SYNCSTATE: time_us=%lld local_height=%d best_header=%d max_peer_height=%lld initialblockdownload=%d connections=%zu sync_peer=%s blocks_in_flight=%lld queued_getblocks=%lld datareceived=%llu delta_recv=%llu delta_sent=%llu\n",
               (long long)GetTimeMicros(),
               nLocalHeight,
               nBestHeaderHeight,
               (long long)nMaxPeerHeight,
               fInitialBlockDownload ? 1 : 0,
               vNodesCopy.size(),
               pnodeSyncCopy ? DescribePeerForDiagnostics(pnodeSyncCopy).c_str() : "none",
               (long long)nTotalBlocksInFlight,
               (long long)nTotalQueuedGetBlocks,
               (unsigned long long)nTotalBytesRecv,
               (unsigned long long)nDeltaRecv,
               (unsigned long long)nDeltaSent);
        printf("GLOBAL_P2PMSG: incoming %s\n", FormatMessageStats(mapGlobalP2PMsgIncoming, vCommands).c_str());
        printf("GLOBAL_P2PMSG: outgoing %s\n", FormatMessageStats(mapGlobalP2PMsgOutgoing, vCommands).c_str());
        printf("MEMSTATE: process_rss_kb=%llu process_vmsize_kb=%llu process_vmdata_kb=%llu process_vmswap_kb=%llu threads=%d connections=%zu total_send_queue_bytes=%llu total_recv_queue_bytes=%llu total_askfor=%llu total_blocks_in_flight=%lld total_queued_getblocks=%lld\n",
               (unsigned long long)nProcessVmRssKb,
               (unsigned long long)nProcessVmSizeKb,
               (unsigned long long)nProcessVmDataKb,
               (unsigned long long)nProcessVmSwapKb,
               nProcessThreads,
               vNodesCopy.size(),
               (unsigned long long)nTotalSendQueueBytes,
               (unsigned long long)nTotalRecvQueueBytes,
               (unsigned long long)nTotalAskFor,
               (long long)nTotalBlocksInFlight,
               (long long)nTotalQueuedGetBlocks);
        if (!vCandidates.empty()) {
            std::ostringstream oss;
            oss << "SYNCPEER_CANDIDATES:";
            size_t nLimit = std::min<size_t>(vCandidates.size(), 4);
            for (size_t i = 0; i < nLimit; ++i) {
                oss << ' ' << "[score=" << (long long)vCandidates[i].first << ' ' << DescribePeerForDiagnostics(vCandidates[i].second) << ']';
            }
            printf("%s\n", oss.str().c_str());
        }
        BOOST_FOREACH(CNode* pnode, vNodesCopy) {
            PeerMessageStats& stats = mapPeerMessageStats[pnode->GetId()];
            printf("PEERSTATE: %s in{%s} out{%s}\n",
                   FormatPeerDiagnosticsSummary(pnode, nNow).c_str(),
                   FormatMessageStats(stats.incoming, vCommands).c_str(),
                   FormatMessageStats(stats.outgoing, vCommands).c_str());

            size_t nSendQueueBytes = 0;
            size_t nSendQueueMsgs = 0;
            size_t nRecvQueueBytes = 0;
            size_t nRecvQueueMsgs = 0;
            size_t nAskForSize = 0;
            {
                TRY_LOCK(pnode->cs_vSend, lockSend);
                if (lockSend)
                {
                    nSendQueueBytes = pnode->nSendSize;
                    nSendQueueMsgs = pnode->vSendMsg.size();
                }
            }
            {
                TRY_LOCK(pnode->cs_vRecvMsg, lockRecv);
                if (lockRecv)
                {
                    nRecvQueueMsgs = pnode->vRecvMsg.size();
                    nRecvQueueBytes = pnode->GetTotalRecvSize();
                }
            }
            nAskForSize = pnode->mapAskFor.size();

            printf("PEERMEM: id=%d addr=%s send_queue_bytes=%zu send_queue_msgs=%zu recv_queue_bytes=%zu recv_queue_msgs=%zu askfor=%zu blocks_in_flight=%zu connected=%d disconnect=%d\n",
                   pnode->GetId(),
                   pnode->addrName.c_str(),
                   nSendQueueBytes,
                   nSendQueueMsgs,
                   nRecvQueueBytes,
                   nRecvQueueMsgs,
                   nAskForSize,
                   pnode->setBlocksInFlight.size(),
                   pnode->fSuccessfullyConnected ? 1 : 0,
                   pnode->fDisconnect ? 1 : 0);
        }
    }

    nLastSyncDiagnosticsLog = nNow;
    nLastSyncDiagnosticsBytesRecv = nTotalBytesRecv;
    nLastSyncDiagnosticsBytesSent = nTotalBytesSent;

    BOOST_FOREACH(CNode* pnode, vNodesCopy)
        pnode->Release();
}

CAddress addrSeenByPeer(CService("0.0.0.0", 0), nLocalServices);
uint64_t nLocalHostNonce = 0;
boost::array<int, THREAD_MAX> vnThreadsRunning;
static std::vector<ListenSocket> vhListenSocket;
CAddrMan addrman;

vector<CNode*> vNodes;
CCriticalSection cs_vNodes;
map<CInv, CDataStream> mapRelay;
deque<pair<int64_t, CInv> > vRelayExpiration;
CCriticalSection cs_mapRelay;
map<CInv, int64_t> mapAlreadyAskedFor;
// mutex for mapAlreadyAskedFor
CCriticalSection cs_mapAlreadyAskedFor;

bool IsBlockRequestOwnedByAnyPeer(const uint256& hash, const CNode* extra_peer)
{
    LOCK(cs_vNodes);
    for (std::vector<CNode*>::const_iterator it = vNodes.begin();
         it != vNodes.end(); ++it)
    {
        const CNode* pnode = *it;
        if (pnode != NULL &&
            (pnode->setBlocksInFlight.count(hash) != 0 ||
             std::find_if(pnode->mapAskFor.begin(), pnode->mapAskFor.end(),
                          [&hash](const std::pair<const int64_t, CInv>& item) {
                              return item.second.hash == hash &&
                                     (item.second.type == MSG_BLOCK ||
                                      item.second.type == MSG_FILTERED_BLOCK);
                          }) != pnode->mapAskFor.end()))
            return true;
    }
    if (extra_peer != NULL &&
        (extra_peer->setBlocksInFlight.count(hash) != 0 ||
         std::find_if(extra_peer->mapAskFor.begin(), extra_peer->mapAskFor.end(),
                      [&hash](const std::pair<const int64_t, CInv>& item) {
                          return item.second.hash == hash &&
                                 (item.second.type == MSG_BLOCK ||
                                  item.second.type == MSG_FILTERED_BLOCK);
                      }) != extra_peer->mapAskFor.end()))
        return true;
    return false;
}

bool EraseAlreadyAskedForIfUnowned(const CInv& inv, const CNode* extra_peer)
{
    if (inv.type != MSG_BLOCK && inv.type != MSG_FILTERED_BLOCK)
        return false;
    if (IsBlockRequestOwnedByAnyPeer(inv.hash, extra_peer))
        return false;
    LOCK(cs_mapAlreadyAskedFor);
    return mapAlreadyAskedFor.erase(inv) != 0;
}

size_t PruneAlreadyAskedFor(int64_t nNowMicros)
{
    static int64_t nLastPruneMicros = 0;
    size_t nRemoved = 0;
    std::vector<CInv> dueOrphanedBlocks;
    {
        LOCK(cs_mapAlreadyAskedFor);
        if (nLastPruneMicros != 0 && nNowMicros >= nLastPruneMicros &&
            nNowMicros - nLastPruneMicros < 1000000)
            return 0;
        nLastPruneMicros = nNowMicros;
        for (std::map<CInv, int64_t>::const_iterator it =
                 mapAlreadyAskedFor.begin();
             it != mapAlreadyAskedFor.end(); ++it)
        {
            const bool fBlock = (it->first.type == MSG_BLOCK ||
                                 it->first.type == MSG_FILTERED_BLOCK);
            if (it->second == 0)
                dueOrphanedBlocks.push_back(it->first);
            else if (fBlock && it->second <= nNowMicros)
                dueOrphanedBlocks.push_back(it->first);
            else if (nNowMicros - it->second >
                     ALREADY_ASKED_FOR_RETENTION_US)
                dueOrphanedBlocks.push_back(it->first);
        }
    }

    std::vector<CInv> unownedBlocks;
    for (std::vector<CInv>::const_iterator it = dueOrphanedBlocks.begin();
         it != dueOrphanedBlocks.end(); ++it)
    {
        const bool fBlock = (it->type == MSG_BLOCK ||
                             it->type == MSG_FILTERED_BLOCK);
        if (!fBlock || !IsBlockRequestOwnedByAnyPeer(it->hash))
            unownedBlocks.push_back(*it);
    }

    {
        LOCK(cs_mapAlreadyAskedFor);
        for (std::vector<CInv>::const_iterator it = unownedBlocks.begin();
             it != unownedBlocks.end(); ++it)
            nRemoved += mapAlreadyAskedFor.erase(*it);
    }
    if (nRemoved != 0 && BlockRequestTraceEnabled())
        printf("BLOCKREQTRACE time_us=%lld event=ALREADY_ASKED_PRUNE removed=%zu remaining=%zu retention_us=%lld\n",
               (long long)nNowMicros, nRemoved, mapAlreadyAskedFor.size(),
               (long long)ALREADY_ASKED_FOR_RETENTION_US);
    return nRemoved;
}

namespace {

static const size_t BLOCK_REQUEST_TRACE_MAX_HASHES = 16384;
static const size_t BLOCK_REQUEST_TRACE_MAX_PEERS_PER_HASH = 16;
static const int64_t BLOCK_REQUEST_TRACE_NORMAL_RETENTION_US = 60 * 1000000;
static const int64_t BLOCK_REQUEST_TRACE_ANOMALY_RETENTION_US = 30 * 60 * 1000000;
static const int64_t BLOCK_REQUEST_TRACE_SUMMARY_INTERVAL_US = 60 * 1000000;

struct BlockRequestTracePeerState
{
    uint64_t queuedCount;
    bool inFlight;
    int64_t inFlightSince;
    std::string address;

    BlockRequestTracePeerState()
        : queuedCount(0), inFlight(false), inFlightSince(0)
    {
    }
};

struct BlockRequestTraceEntry
{
    int64_t firstSeenTime;
    int64_t firstRequestTime;
    int64_t firstScheduledTime;
    int64_t lastRequestTime;
    int64_t lastGlobalAskedTime;
    int64_t firstSendTime;
    int64_t previousSendTime;
    int64_t lastSendTime;
    int64_t firstReceiveTime;
    int64_t lastReceiveTime;
    int64_t lastTouchedTime;
    NodeId firstRequestPeer;
    NodeId firstSendPeer;
    NodeId firstReceivePeer;
    std::string firstRequestAddress;
    std::string firstSendAddress;
    std::string firstReceiveAddress;
    uint64_t requestCount;
    uint64_t sendCount;
    uint64_t receiveCount;
    uint64_t duplicateOrKnownReceiveCount;
    uint64_t stallRecoveryCount;
    uint64_t getBlocksWaveCount;
    BlockRequestTraceSource lastSource;
    BlockRequestTraceSource firstRequestSource;
    BlockRequestTraceSource firstSendSource;
    BlockRequestTraceResult lastResult;
    std::string lastRemovalReason;
    int lastKnownHeight;
    bool indexed;
    bool activeChain;
    bool anomalous;
    bool completed;
    std::map<NodeId, BlockRequestTracePeerState> peers;

    explicit BlockRequestTraceEntry(int64_t nNow)
        : firstSeenTime(nNow),
          firstRequestTime(0),
          firstScheduledTime(0),
          lastRequestTime(0),
          lastGlobalAskedTime(0),
          firstSendTime(0),
          previousSendTime(0),
          lastSendTime(0),
          firstReceiveTime(0),
          lastReceiveTime(0),
          lastTouchedTime(nNow),
          firstRequestPeer(-1),
          firstSendPeer(-1),
          firstReceivePeer(-1),
          requestCount(0),
          sendCount(0),
          receiveCount(0),
          duplicateOrKnownReceiveCount(0),
          stallRecoveryCount(0),
          getBlocksWaveCount(0),
          lastSource(BLOCKREQ_SOURCE_OTHER),
          firstRequestSource(BLOCKREQ_SOURCE_OTHER),
          firstSendSource(BLOCKREQ_SOURCE_OTHER),
          lastResult(BLOCKREQ_RESULT_UNKNOWN),
          lastKnownHeight(-1),
          indexed(false),
          activeChain(false),
          anomalous(false),
          completed(false)
    {
    }
};

struct BlockRequestTraceCounters
{
    uint64_t uniqueHashesScheduled;
    uint64_t totalSchedules;
    uint64_t duplicateSchedules;
    uint64_t totalSends;
    uint64_t duplicateSends;
    uint64_t knownSends;
    uint64_t failedTryLockSends;
    uint64_t crossPeerOverlapCount;
    uint64_t totalReceives;
    uint64_t duplicateOrKnownReceives;
    uint64_t stallRecoveryCount;
    uint64_t batch75TriggerCount;
    uint64_t pipelineTriggerCount;
    uint64_t maxSimultaneousOwnership;
    uint64_t registryDrops;
    uint64_t peerStateDrops;

    BlockRequestTraceCounters()
        : uniqueHashesScheduled(0),
          totalSchedules(0),
          duplicateSchedules(0),
          totalSends(0),
          duplicateSends(0),
          knownSends(0),
          failedTryLockSends(0),
          crossPeerOverlapCount(0),
          totalReceives(0),
          duplicateOrKnownReceives(0),
          stallRecoveryCount(0),
          batch75TriggerCount(0),
          pipelineTriggerCount(0),
          maxSimultaneousOwnership(0),
          registryDrops(0),
          peerStateDrops(0)
    {
    }
};

// Hooks pass production state as values. Code under this lock never acquires
// cs_main, cs_vNodes, or a peer lock, so tracing cannot invert their order.
static CCriticalSection cs_blockRequestTrace;
// Initialized once in AppInit2 before networking threads start, then immutable.
static bool fBlockRequestTraceEnabled = false;
static bool fBlockRequestTraceFilter = false;
static std::string strBlockRequestTraceFilter;
static std::map<uint256, BlockRequestTraceEntry> mapBlockRequestTrace;
static BlockRequestTraceCounters blockRequestTraceCounters;
static uint64_t nBlockRequestTraceOperations = 0;
static int64_t nLastBlockRequestTraceSummary = 0;

static const char* BlockRequestTraceSourceName(BlockRequestTraceSource source)
{
    switch (source)
    {
    case BLOCKREQ_SOURCE_ASKFOR:
        return "askfor";
    case BLOCKREQ_SOURCE_INV:
        return "inv";
    case BLOCKREQ_SOURCE_HEADERS_DIRECT:
        return "headers-direct";
    case BLOCKREQ_SOURCE_ORPHAN:
        return "orphan";
    case BLOCKREQ_SOURCE_CHECKPOINT:
        return "checkpoint";
    case BLOCKREQ_SOURCE_REJECT_RECOVERY:
        return "reject-recovery";
    default:
        return "other";
    }
}

static const char* BlockRequestTraceResultName(BlockRequestTraceResult result)
{
    switch (result)
    {
    case BLOCKREQ_RESULT_ACCEPTED_ACTIVE:
        return "accepted-active";
    case BLOCKREQ_RESULT_ACCEPTED_INDEXED:
        return "accepted-indexed";
    case BLOCKREQ_RESULT_ORPHAN_NEW:
        return "orphan-new";
    case BLOCKREQ_RESULT_ALREADY_KNOWN:
        return "already-known";
    case BLOCKREQ_RESULT_ORPHAN_DUPLICATE:
        return "orphan-duplicate";
    case BLOCKREQ_RESULT_REJECTED:
        return "rejected-or-invalid";
    case BLOCKREQ_RESULT_TRUE_UNINDEXED:
        return "process-true-unindexed";
    default:
        return "unknown";
    }
}

static std::string BlockRequestTracePeerAddress(const CNode* pnode)
{
    std::string value = pnode->addrName.empty() ? pnode->addr.ToString() : pnode->addrName;
    for (size_t i = 0; i < value.size(); ++i)
    {
        unsigned char ch = static_cast<unsigned char>(value[i]);
        if (std::isspace(ch) || value[i] == '=')
            value[i] = '_';
    }
    return value.empty() ? "unknown" : value;
}

static bool BlockRequestTraceMatchesFilter(const uint256& hash)
{
    return !fBlockRequestTraceFilter || hash.ToString() == strBlockRequestTraceFilter;
}

static bool BlockRequestTraceInteresting(const BlockRequestTraceEntry& entry)
{
    return fBlockRequestTraceFilter || entry.anomalous ||
           entry.requestCount >= 2 || entry.sendCount >= 2 || entry.receiveCount >= 2;
}

static size_t BlockRequestTraceCountQueued(
    const BlockRequestTraceEntry& entry, NodeId excludePeer)
{
    size_t count = 0;
    for (std::map<NodeId, BlockRequestTracePeerState>::const_iterator it = entry.peers.begin();
         it != entry.peers.end(); ++it)
    {
        if (it->first != excludePeer && it->second.queuedCount != 0)
            ++count;
    }
    return count;
}

static size_t BlockRequestTraceCountInFlight(
    const BlockRequestTraceEntry& entry, NodeId excludePeer)
{
    size_t count = 0;
    for (std::map<NodeId, BlockRequestTracePeerState>::const_iterator it = entry.peers.begin();
         it != entry.peers.end(); ++it)
    {
        if (it->first != excludePeer && it->second.inFlight)
            ++count;
    }
    return count;
}

static size_t BlockRequestTraceOwnershipCount(const BlockRequestTraceEntry& entry)
{
    size_t count = 0;
    for (std::map<NodeId, BlockRequestTracePeerState>::const_iterator it = entry.peers.begin();
         it != entry.peers.end(); ++it)
    {
        if (it->second.queuedCount != 0 || it->second.inFlight)
            ++count;
    }
    return count;
}

static bool BlockRequestTraceHasOwnership(const BlockRequestTraceEntry& entry)
{
    return BlockRequestTraceOwnershipCount(entry) != 0;
}

static BlockRequestTracePeerState* BlockRequestTracePeerLocked(
    BlockRequestTraceEntry& entry, CNode* pnode, const uint256& hash)
{
    NodeId peer = pnode->GetId();
    std::map<NodeId, BlockRequestTracePeerState>::iterator it =
        entry.peers.find(peer);
    if (it == entry.peers.end())
    {
        if (entry.peers.size() >= BLOCK_REQUEST_TRACE_MAX_PEERS_PER_HASH)
        {
            for (it = entry.peers.begin(); it != entry.peers.end(); ++it)
            {
                if (it->second.queuedCount == 0 && !it->second.inFlight)
                {
                    entry.peers.erase(it);
                    break;
                }
            }
        }
        if (entry.peers.size() >= BLOCK_REQUEST_TRACE_MAX_PEERS_PER_HASH)
        {
            ++blockRequestTraceCounters.peerStateDrops;
            entry.anomalous = true;
            printf("BLOCKREQTRACE time_us=%lld event=REGISTRY_DROP hash=%s peer=%d addr=%s reason=per_hash_peer_cap cap=%zu\n",
                   (long long)GetTimeMicros(), hash.ToString().c_str(), peer,
                   BlockRequestTracePeerAddress(pnode).c_str(),
                   BLOCK_REQUEST_TRACE_MAX_PEERS_PER_HASH);
            return NULL;
        }
        it = entry.peers.insert(
            std::make_pair(peer, BlockRequestTracePeerState())).first;
    }
    it->second.address = BlockRequestTracePeerAddress(pnode);
    return &it->second;
}

static void BlockRequestTracePruneLocked(int64_t nNow, bool fForce)
{
    ++nBlockRequestTraceOperations;
    if (!fForce && (nBlockRequestTraceOperations % 256) != 0 &&
        mapBlockRequestTrace.size() < BLOCK_REQUEST_TRACE_MAX_HASHES)
        return;

    for (std::map<uint256, BlockRequestTraceEntry>::iterator it = mapBlockRequestTrace.begin();
         it != mapBlockRequestTrace.end(); )
    {
        BlockRequestTraceEntry& entry = it->second;
        int64_t nRetention = entry.anomalous
            ? BLOCK_REQUEST_TRACE_ANOMALY_RETENTION_US
            : BLOCK_REQUEST_TRACE_NORMAL_RETENTION_US;
        if (entry.completed && nNow - entry.lastTouchedTime > nRetention)
            it = mapBlockRequestTrace.erase(it);
        else
            ++it;
    }
}

static bool BlockRequestTraceEvictOneLocked()
{
    std::map<uint256, BlockRequestTraceEntry>::iterator best = mapBlockRequestTrace.end();
    for (std::map<uint256, BlockRequestTraceEntry>::iterator it = mapBlockRequestTrace.begin();
         it != mapBlockRequestTrace.end(); ++it)
    {
        if (!it->second.completed)
            continue;
        if (best == mapBlockRequestTrace.end() ||
            (best->second.anomalous && !it->second.anomalous) ||
            (best->second.anomalous == it->second.anomalous &&
             it->second.lastTouchedTime < best->second.lastTouchedTime))
        {
            best = it;
        }
    }
    if (best == mapBlockRequestTrace.end())
        return false;
    mapBlockRequestTrace.erase(best);
    return true;
}

static BlockRequestTraceEntry* BlockRequestTraceGetLocked(
    const uint256& hash, int64_t nNow, bool fCreate)
{
    if (!BlockRequestTraceMatchesFilter(hash))
        return NULL;

    std::map<uint256, BlockRequestTraceEntry>::iterator it = mapBlockRequestTrace.find(hash);
    if (it != mapBlockRequestTrace.end())
    {
        it->second.lastTouchedTime = nNow;
        return &it->second;
    }
    if (!fCreate)
        return NULL;

    BlockRequestTracePruneLocked(nNow, false);
    while (mapBlockRequestTrace.size() >= BLOCK_REQUEST_TRACE_MAX_HASHES)
    {
        if (!BlockRequestTraceEvictOneLocked())
        {
            ++blockRequestTraceCounters.registryDrops;
            return NULL;
        }
    }

    return &mapBlockRequestTrace.insert(
        std::make_pair(hash, BlockRequestTraceEntry(nNow))).first->second;
}

static void BlockRequestTraceUpdateMaxOwnershipLocked(const BlockRequestTraceEntry& entry)
{
    uint64_t count = BlockRequestTraceOwnershipCount(entry);
    if (count > blockRequestTraceCounters.maxSimultaneousOwnership)
        blockRequestTraceCounters.maxSimultaneousOwnership = count;
}

static void BlockRequestTraceMaybeSummaryLocked(int64_t nNow)
{
    if (nLastBlockRequestTraceSummary != 0 &&
        nNow - nLastBlockRequestTraceSummary < BLOCK_REQUEST_TRACE_SUMMARY_INTERVAL_US)
        return;
    nLastBlockRequestTraceSummary = nNow;
    printf("BLOCKREQTRACE time_us=%lld event=SUMMARY unique_hashes_scheduled=%llu total_schedules=%llu duplicate_schedules=%llu total_getdata_sends=%llu duplicate_getdata_sends=%llu known_getdata_sends=%llu failed_trylock_sends=%llu cross_peer_overlaps=%llu total_receives=%llu duplicate_known_receives=%llu stall_recoveries=%llu batch75_triggers=%llu pipeline_triggers=%llu max_peer_ownership=%llu registry_size=%zu registry_drops=%llu peer_state_drops=%llu\n",
           (long long)nNow,
           (unsigned long long)blockRequestTraceCounters.uniqueHashesScheduled,
           (unsigned long long)blockRequestTraceCounters.totalSchedules,
           (unsigned long long)blockRequestTraceCounters.duplicateSchedules,
           (unsigned long long)blockRequestTraceCounters.totalSends,
           (unsigned long long)blockRequestTraceCounters.duplicateSends,
           (unsigned long long)blockRequestTraceCounters.knownSends,
           (unsigned long long)blockRequestTraceCounters.failedTryLockSends,
           (unsigned long long)blockRequestTraceCounters.crossPeerOverlapCount,
           (unsigned long long)blockRequestTraceCounters.totalReceives,
           (unsigned long long)blockRequestTraceCounters.duplicateOrKnownReceives,
           (unsigned long long)blockRequestTraceCounters.stallRecoveryCount,
           (unsigned long long)blockRequestTraceCounters.batch75TriggerCount,
           (unsigned long long)blockRequestTraceCounters.pipelineTriggerCount,
           (unsigned long long)blockRequestTraceCounters.maxSimultaneousOwnership,
           mapBlockRequestTrace.size(),
           (unsigned long long)blockRequestTraceCounters.registryDrops,
           (unsigned long long)blockRequestTraceCounters.peerStateDrops);
}

} // namespace

bool InitBlockRequestTrace(bool fEnabled, const std::string& strHashFilter)
{
    std::string normalized = strHashFilter;
    for (size_t i = 0; i < normalized.size(); ++i)
        normalized[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(normalized[i])));

    if (!normalized.empty())
    {
        if (normalized.size() != 64)
            return false;
        for (size_t i = 0; i < normalized.size(); ++i)
            if (!std::isxdigit(static_cast<unsigned char>(normalized[i])))
                return false;
    }

    LOCK(cs_blockRequestTrace);
    fBlockRequestTraceEnabled = fEnabled;
    fBlockRequestTraceFilter = fEnabled && !normalized.empty();
    strBlockRequestTraceFilter = normalized;
    mapBlockRequestTrace.clear();
    blockRequestTraceCounters = BlockRequestTraceCounters();
    nBlockRequestTraceOperations = 0;
    nLastBlockRequestTraceSummary = 0;

    if (fBlockRequestTraceEnabled)
    {
        printf("BLOCKREQTRACE time_us=%lld event=START enabled=1 filter=%s hash_cap=%zu peer_cap_per_hash=%zu normal_retention_s=60 anomaly_retention_s=1800\n",
               (long long)GetTimeMicros(),
               fBlockRequestTraceFilter ? strBlockRequestTraceFilter.c_str() : "none",
               BLOCK_REQUEST_TRACE_MAX_HASHES,
               BLOCK_REQUEST_TRACE_MAX_PEERS_PER_HASH);
    }
    return true;
}

bool BlockRequestTraceEnabled()
{
    return fBlockRequestTraceEnabled;
}

void BlockRequestTraceAskSchedule(CNode* pnode, const uint256& hash,
                                  BlockRequestTraceSource source,
                                  int64_t nScheduledTime,
                                  int64_t nPreviousGlobalTime,
                                  bool fSamePeerInFlight)
{
    if (!fBlockRequestTraceEnabled)
        return;
    int64_t nNow = GetTimeMicros();
    LOCK(cs_blockRequestTrace);
    BlockRequestTraceEntry* entry = BlockRequestTraceGetLocked(hash, nNow, true);
    if (!entry)
        return;

    NodeId peer = pnode->GetId();
    size_t nOtherQueued = BlockRequestTraceCountQueued(*entry, peer);
    size_t nOtherInFlight = BlockRequestTraceCountInFlight(*entry, peer);
    if (entry->requestCount == 0)
    {
        entry->firstRequestTime = nNow;
        entry->firstScheduledTime = nScheduledTime;
        entry->firstRequestPeer = peer;
        entry->firstRequestAddress = BlockRequestTracePeerAddress(pnode);
        entry->firstRequestSource = source;
        ++blockRequestTraceCounters.uniqueHashesScheduled;
    }
    ++entry->requestCount;
    ++blockRequestTraceCounters.totalSchedules;
    if (entry->requestCount >= 2)
    {
        ++blockRequestTraceCounters.duplicateSchedules;
        entry->anomalous = true;
    }
    if (nOtherQueued != 0 || nOtherInFlight != 0)
    {
        ++blockRequestTraceCounters.crossPeerOverlapCount;
        entry->anomalous = true;
    }

    entry->lastRequestTime = nNow;
    entry->lastGlobalAskedTime = nScheduledTime;
    entry->lastSource = source;
    entry->completed = false;
    BlockRequestTracePeerState* peerState =
        BlockRequestTracePeerLocked(*entry, pnode, hash);
    if (!peerState)
    {
        BlockRequestTraceMaybeSummaryLocked(nNow);
        return;
    }
    uint64_t nSamePeerQueuedBefore = peerState->queuedCount;
    ++peerState->queuedCount;
    BlockRequestTraceUpdateMaxOwnershipLocked(*entry);

    if (entry->requestCount >= 2 || nOtherQueued != 0 || nOtherInFlight != 0 ||
        fBlockRequestTraceFilter)
    {
        printf("BLOCKREQTRACE time_us=%lld event=ASK_SCHEDULE hash=%s peer=%d addr=%s source=%s scheduled_us=%lld previous_global_us=%lld request_count=%llu first_seen_us=%lld first_request_us=%lld last_request_us=%lld first_scheduled_us=%lld first_peer=%d first_addr=%s first_source=%s first_send_us=%lld last_send_us=%lld first_send_peer=%d first_send_addr=%s first_send_source=%s first_receive_us=%lld last_receive_us=%lld first_receive_peer=%d first_receive_addr=%s previous_result=%s previous_indexed=%d previous_active=%d previous_height=%d known_index=-1 same_peer_inflight=%d same_peer_queued_before=%llu other_peer_queued=%zu other_peer_inflight=%zu last_removal=%s\n",
               (long long)nNow, hash.ToString().c_str(), peer,
               peerState->address.c_str(), BlockRequestTraceSourceName(source),
               (long long)nScheduledTime, (long long)nPreviousGlobalTime,
               (unsigned long long)entry->requestCount,
               (long long)entry->firstSeenTime,
               (long long)entry->firstRequestTime,
               (long long)entry->lastRequestTime,
               (long long)entry->firstScheduledTime,
               entry->firstRequestPeer,
               entry->firstRequestAddress.c_str(),
               BlockRequestTraceSourceName(entry->firstRequestSource),
               (long long)entry->firstSendTime,
               (long long)entry->lastSendTime,
               entry->firstSendPeer,
               entry->firstSendAddress.empty()
                   ? "unknown" : entry->firstSendAddress.c_str(),
               BlockRequestTraceSourceName(entry->firstSendSource),
               (long long)entry->firstReceiveTime,
               (long long)entry->lastReceiveTime,
               entry->firstReceivePeer,
               entry->firstReceiveAddress.empty()
                   ? "unknown" : entry->firstReceiveAddress.c_str(),
               BlockRequestTraceResultName(entry->lastResult),
               entry->indexed ? 1 : 0,
               entry->activeChain ? 1 : 0,
               entry->lastKnownHeight,
               fSamePeerInFlight ? 1 : 0,
               (unsigned long long)nSamePeerQueuedBefore,
               nOtherQueued, nOtherInFlight,
               entry->lastRemovalReason.empty()
                   ? "none" : entry->lastRemovalReason.c_str());
    }
    BlockRequestTraceMaybeSummaryLocked(nNow);
}

void BlockRequestTraceAskRemoved(CNode* pnode, const uint256& hash,
                                 const char* pszReason,
                                 int nKnownInBlockIndex)
{
    if (!fBlockRequestTraceEnabled)
        return;
    int64_t nNow = GetTimeMicros();
    LOCK(cs_blockRequestTrace);
    BlockRequestTraceEntry* entry =
        BlockRequestTraceGetLocked(hash, nNow, false);
    if (!entry)
        return;

    BlockRequestTracePeerState* peerState =
        BlockRequestTracePeerLocked(*entry, pnode, hash);
    if (!peerState)
        return;
    if (peerState->queuedCount != 0)
        --peerState->queuedCount;
    entry->lastRemovalReason = pszReason;
    size_t nOtherQueued =
        BlockRequestTraceCountQueued(*entry, pnode->GetId());
    size_t nOtherInFlight =
        BlockRequestTraceCountInFlight(*entry, pnode->GetId());
    entry->completed = !BlockRequestTraceHasOwnership(*entry);

    if (BlockRequestTraceInteresting(*entry))
    {
        printf("BLOCKREQTRACE time_us=%lld event=ASK_REMOVE hash=%s peer=%d addr=%s reason=%s same_peer_queued=%llu other_peer_queued=%zu other_peer_inflight=%zu known_index=%d\n",
               (long long)nNow, hash.ToString().c_str(), pnode->GetId(),
               peerState->address.c_str(), pszReason,
               (unsigned long long)peerState->queuedCount,
               nOtherQueued, nOtherInFlight, nKnownInBlockIndex);
    }
}

void BlockRequestTraceGetDataSend(CNode* pnode, const uint256& hash,
                                  BlockRequestTraceSource path,
                                  int nKnownInBlockIndex,
                                  bool fCsMainCheckPerformed,
                                  bool fCsMainCheckResult,
                                  bool fSamePeerInFlight,
                                  bool fMapAskForPresent,
                                  int64_t nPreviousGlobalAskedTime,
                                  int64_t nWrittenGlobalAskedTime)
{
    if (!fBlockRequestTraceEnabled)
        return;
    int64_t nNow = GetTimeMicros();
    LOCK(cs_blockRequestTrace);
    BlockRequestTraceEntry* entry = BlockRequestTraceGetLocked(hash, nNow, true);
    if (!entry)
        return;

    NodeId peer = pnode->GetId();
    size_t nOtherInFlight = BlockRequestTraceCountInFlight(*entry, peer);
    BlockRequestTracePeerState* peerState =
        BlockRequestTracePeerLocked(*entry, pnode, hash);
    if (!peerState)
        return;
    fMapAskForPresent =
        fMapAskForPresent || peerState->queuedCount != 0;
    if (path == BLOCKREQ_SOURCE_HEADERS_DIRECT)
        entry->lastSource = path;

    entry->previousSendTime = entry->lastSendTime;
    entry->lastSendTime = nNow;
    if (entry->firstSendTime == 0)
    {
        entry->firstSendTime = nNow;
        entry->firstSendPeer = peer;
        entry->firstSendAddress = peerState->address;
        entry->firstSendSource = entry->lastSource;
    }
    if (nWrittenGlobalAskedTime >= 0)
        entry->lastGlobalAskedTime = nWrittenGlobalAskedTime;
    else if (nPreviousGlobalAskedTime >= 0)
        entry->lastGlobalAskedTime = nPreviousGlobalAskedTime;
    ++entry->sendCount;
    ++blockRequestTraceCounters.totalSends;
    if (entry->sendCount >= 2)
    {
        ++blockRequestTraceCounters.duplicateSends;
        entry->anomalous = true;
    }
    if (nKnownInBlockIndex > 0)
    {
        ++blockRequestTraceCounters.knownSends;
        entry->anomalous = true;
    }
    if (!fCsMainCheckPerformed && path != BLOCKREQ_SOURCE_HEADERS_DIRECT)
    {
        ++blockRequestTraceCounters.failedTryLockSends;
        entry->anomalous = true;
    }
    if (nOtherInFlight != 0)
    {
        ++blockRequestTraceCounters.crossPeerOverlapCount;
        entry->anomalous = true;
    }
    if (BlockRequestTraceInteresting(*entry) ||
        nKnownInBlockIndex > 0 || nOtherInFlight != 0 ||
        !fCsMainCheckPerformed || path == BLOCKREQ_SOURCE_HEADERS_DIRECT ||
        fBlockRequestTraceFilter)
    {
        printf("BLOCKREQTRACE time_us=%lld event=GETDATA_SEND hash=%s peer=%d addr=%s path=%s source=%s request_count=%llu send_count=%llu first_seen_us=%lld first_request_us=%lld last_request_us=%lld first_scheduled_us=%lld first_peer=%d first_addr=%s first_source=%s known_index=%d cs_main_check_performed=%d cs_main_check_result=%d same_peer_inflight=%d same_peer_queued=%llu other_peer_inflight=%zu mapaskfor_present=%d previous_global_asked_us=%lld written_global_asked_us=%lld global_asked_us=%lld first_send_us=%lld last_send_us=%lld first_send_peer=%d first_send_addr=%s first_send_source=%s previous_send_us=%lld first_receive_us=%lld last_receive_us=%lld first_receive_peer=%d first_receive_addr=%s previous_result=%s previous_indexed=%d previous_active=%d previous_height=%d last_removal=%s\n",
               (long long)nNow, hash.ToString().c_str(), peer,
               peerState->address.c_str(), BlockRequestTraceSourceName(path),
               BlockRequestTraceSourceName(entry->lastSource),
               (unsigned long long)entry->requestCount,
               (unsigned long long)entry->sendCount,
               (long long)entry->firstSeenTime,
               (long long)entry->firstRequestTime,
               (long long)entry->lastRequestTime,
               (long long)entry->firstScheduledTime,
               entry->firstRequestPeer,
               entry->firstRequestAddress.empty()
                   ? "unknown" : entry->firstRequestAddress.c_str(),
               BlockRequestTraceSourceName(entry->firstRequestSource),
               nKnownInBlockIndex,
               fCsMainCheckPerformed ? 1 : 0,
               fCsMainCheckResult ? 1 : 0,
               fSamePeerInFlight ? 1 : 0,
               (unsigned long long)peerState->queuedCount,
               nOtherInFlight,
               fMapAskForPresent ? 1 : 0,
               (long long)nPreviousGlobalAskedTime,
               (long long)nWrittenGlobalAskedTime,
               (long long)entry->lastGlobalAskedTime,
               (long long)entry->firstSendTime,
               (long long)entry->lastSendTime,
               entry->firstSendPeer,
               entry->firstSendAddress.c_str(),
               BlockRequestTraceSourceName(entry->firstSendSource),
               (long long)entry->previousSendTime,
               (long long)entry->firstReceiveTime,
               (long long)entry->lastReceiveTime,
               entry->firstReceivePeer,
               entry->firstReceiveAddress.empty()
                   ? "unknown" : entry->firstReceiveAddress.c_str(),
               BlockRequestTraceResultName(entry->lastResult),
               entry->indexed ? 1 : 0,
               entry->activeChain ? 1 : 0,
               entry->lastKnownHeight,
               entry->lastRemovalReason.empty()
                   ? "none" : entry->lastRemovalReason.c_str());
    }
    BlockRequestTraceMaybeSummaryLocked(nNow);
}

void BlockRequestTraceInFlightMark(CNode* pnode, const uint256& hash,
                                   bool fConsumesQueuedEntry)
{
    if (!fBlockRequestTraceEnabled)
        return;
    int64_t nNow = GetTimeMicros();
    LOCK(cs_blockRequestTrace);
    BlockRequestTraceEntry* entry = BlockRequestTraceGetLocked(hash, nNow, false);
    if (!entry)
        return;
    BlockRequestTracePeerState* peerState =
        BlockRequestTracePeerLocked(*entry, pnode, hash);
    if (!peerState)
        return;
    if (fConsumesQueuedEntry && peerState->queuedCount != 0)
        --peerState->queuedCount;
    peerState->inFlight = true;
    peerState->inFlightSince = nNow;
    entry->completed = false;
    BlockRequestTraceUpdateMaxOwnershipLocked(*entry);
}

void BlockRequestTraceBlockReceive(CNode* pnode, const uint256& hash,
                                   bool fKnownBefore,
                                   bool fSenderInFlightBefore,
                                   int64_t nSenderInFlightAge)
{
    if (!fBlockRequestTraceEnabled)
        return;
    int64_t nNow = GetTimeMicros();
    LOCK(cs_blockRequestTrace);
    BlockRequestTraceEntry* entry = BlockRequestTraceGetLocked(hash, nNow, true);
    if (!entry)
        return;

    NodeId peer = pnode->GetId();
    size_t nOtherQueued = BlockRequestTraceCountQueued(*entry, peer);
    size_t nOtherInFlight = BlockRequestTraceCountInFlight(*entry, peer);
    BlockRequestTracePeerState* peerState =
        BlockRequestTracePeerLocked(*entry, pnode, hash);
    if (!peerState)
        return;
    bool fUnexpectedPeer =
        peerState->queuedCount == 0 && !peerState->inFlight;

    BlockRequestTraceResult previousResult = entry->lastResult;
    bool fPreviousIndexed = entry->indexed;
    bool fPreviousActive = entry->activeChain;
    int nPreviousHeight = entry->lastKnownHeight;
    if (entry->receiveCount == 0)
    {
        entry->firstReceiveTime = nNow;
        entry->firstReceivePeer = peer;
        entry->firstReceiveAddress = peerState->address;
    }
    ++entry->receiveCount;
    ++blockRequestTraceCounters.totalReceives;
    entry->lastReceiveTime = nNow;
    if (entry->receiveCount >= 2 || fKnownBefore)
    {
        ++entry->duplicateOrKnownReceiveCount;
        ++blockRequestTraceCounters.duplicateOrKnownReceives;
        entry->anomalous = true;
    }
    if (fUnexpectedPeer || nOtherQueued != 0 || nOtherInFlight != 0)
        entry->anomalous = true;

    int64_t nSinceFirstSend = entry->firstSendTime == 0 ? -1 : nNow - entry->firstSendTime;
    int64_t nSincePreviousSend = entry->lastSendTime == 0 ? -1 : nNow - entry->lastSendTime;
    if (BlockRequestTraceInteresting(*entry) ||
        fKnownBefore || fUnexpectedPeer ||
        nOtherQueued != 0 || nOtherInFlight != 0 || fBlockRequestTraceFilter)
    {
        printf("BLOCKREQTRACE time_us=%lld event=BLOCK_RECEIVE hash=%s peer=%d addr=%s receive_count=%llu first_seen_us=%lld first_request_us=%lld last_request_us=%lld first_send_us=%lld last_send_us=%lld first_receive_us=%lld last_receive_us=%lld first_receive_peer=%d first_receive_addr=%s known_before=%d sender_inflight_before=%d sender_inflight_age_s=%lld unexpected_peer=%d same_peer_queued=%llu other_peer_queued=%zu other_peer_inflight=%zu elapsed_first_send_us=%lld elapsed_previous_send_us=%lld previous_result=%s previous_indexed=%d previous_active=%d previous_height=%d\n",
               (long long)nNow, hash.ToString().c_str(), peer,
               peerState->address.c_str(),
               (unsigned long long)entry->receiveCount,
               (long long)entry->firstSeenTime,
               (long long)entry->firstRequestTime,
               (long long)entry->lastRequestTime,
               (long long)entry->firstSendTime,
               (long long)entry->lastSendTime,
               (long long)entry->firstReceiveTime,
               (long long)entry->lastReceiveTime,
               entry->firstReceivePeer,
               entry->firstReceiveAddress.c_str(),
               fKnownBefore ? 1 : 0,
               fSenderInFlightBefore ? 1 : 0,
               (long long)nSenderInFlightAge,
               fUnexpectedPeer ? 1 : 0,
               (unsigned long long)peerState->queuedCount,
               nOtherQueued, nOtherInFlight,
               (long long)nSinceFirstSend,
               (long long)nSincePreviousSend,
               BlockRequestTraceResultName(previousResult),
               fPreviousIndexed ? 1 : 0,
               fPreviousActive ? 1 : 0,
               nPreviousHeight);
    }
    peerState->inFlight = false;
    peerState->inFlightSince = 0;
    BlockRequestTraceMaybeSummaryLocked(nNow);
}

void BlockRequestTraceBlockResult(CNode* pnode, const uint256& hash,
                                  BlockRequestTraceResult result,
                                  bool fProcessBlockResult,
                                  bool fIndexedAfter,
                                  bool fActiveChainAfter,
                                  bool fBestChainAfter,
                                  int nHeightAfter)
{
    if (!fBlockRequestTraceEnabled)
        return;
    int64_t nNow = GetTimeMicros();
    LOCK(cs_blockRequestTrace);
    BlockRequestTraceEntry* entry = BlockRequestTraceGetLocked(hash, nNow, true);
    if (!entry)
        return;

    entry->lastResult = result;
    entry->indexed = fIndexedAfter;
    entry->activeChain = fActiveChainAfter;
    entry->lastKnownHeight = nHeightAfter;
    if (result != BLOCKREQ_RESULT_ACCEPTED_ACTIVE &&
        result != BLOCKREQ_RESULT_ACCEPTED_INDEXED)
        entry->anomalous = true;
    entry->completed = !BlockRequestTraceHasOwnership(*entry);

    if (BlockRequestTraceInteresting(*entry))
    {
        printf("BLOCKREQTRACE time_us=%lld event=BLOCK_RESULT hash=%s peer=%d addr=%s result=%s processblock_result=%d indexed_after=%d active_chain_after=%d best_chain_after=%d height=%d setbest_state=%s request_count=%llu send_count=%llu receive_count=%llu first_seen_us=%lld first_request_us=%lld last_request_us=%lld first_send_us=%lld last_send_us=%lld first_receive_us=%lld last_receive_us=%lld\n",
               (long long)nNow, hash.ToString().c_str(), pnode->GetId(),
               BlockRequestTracePeerAddress(pnode).c_str(),
               BlockRequestTraceResultName(result),
               fProcessBlockResult ? 1 : 0,
               fIndexedAfter ? 1 : 0,
               fActiveChainAfter ? 1 : 0,
               fBestChainAfter ? 1 : 0,
               nHeightAfter,
               fActiveChainAfter ? "proven-active-chain" : "not-proven",
               (unsigned long long)entry->requestCount,
               (unsigned long long)entry->sendCount,
               (unsigned long long)entry->receiveCount,
               (long long)entry->firstSeenTime,
               (long long)entry->firstRequestTime,
               (long long)entry->lastRequestTime,
               (long long)entry->firstSendTime,
               (long long)entry->lastSendTime,
               (long long)entry->firstReceiveTime,
               (long long)entry->lastReceiveTime);
    }
    BlockRequestTraceMaybeSummaryLocked(nNow);
}

void BlockRequestTraceInFlightClear(CNode* pnode, const uint256& hash,
                                    const char* pszReason,
                                    int64_t nAge,
                                    bool fKnownInBlockIndex)
{
    if (!fBlockRequestTraceEnabled)
        return;
    int64_t nNow = GetTimeMicros();
    LOCK(cs_blockRequestTrace);
    BlockRequestTraceEntry* entry = BlockRequestTraceGetLocked(hash, nNow, false);
    if (!entry)
        return;

    BlockRequestTracePeerState* peerState =
        BlockRequestTracePeerLocked(*entry, pnode, hash);
    if (!peerState)
        return;
    peerState->inFlight = false;
    peerState->inFlightSince = 0;
    entry->lastRemovalReason = pszReason;
    entry->indexed = entry->indexed || fKnownInBlockIndex;
    size_t nOtherQueued = BlockRequestTraceCountQueued(*entry, pnode->GetId());
    size_t nOtherInFlight = BlockRequestTraceCountInFlight(*entry, pnode->GetId());
    entry->completed = !BlockRequestTraceHasOwnership(*entry);

    if (BlockRequestTraceInteresting(*entry))
    {
        printf("BLOCKREQTRACE time_us=%lld event=INFLIGHT_CLEAR hash=%s peer=%d addr=%s reason=%s age_s=%lld other_peer_queued=%zu other_peer_inflight=%zu known_index=%d\n",
               (long long)nNow, hash.ToString().c_str(), pnode->GetId(),
               peerState->address.c_str(), pszReason, (long long)nAge,
               nOtherQueued, nOtherInFlight, fKnownInBlockIndex ? 1 : 0);
    }
}

void BlockRequestTraceInFlightExpire(CNode* pnode, const uint256& hash,
                                     int64_t nAge)
{
    if (!fBlockRequestTraceEnabled)
        return;
    int64_t nNow = GetTimeMicros();
    LOCK(cs_blockRequestTrace);
    BlockRequestTraceEntry* entry = BlockRequestTraceGetLocked(hash, nNow, false);
    if (!entry)
        return;

    BlockRequestTracePeerState* peerState =
        BlockRequestTracePeerLocked(*entry, pnode, hash);
    if (!peerState)
        return;
    peerState->inFlight = false;
    peerState->inFlightSince = 0;
    entry->lastRemovalReason = "timeout";
    size_t nOtherQueued = BlockRequestTraceCountQueued(*entry, pnode->GetId());
    size_t nOtherInFlight = BlockRequestTraceCountInFlight(*entry, pnode->GetId());
    entry->completed = !BlockRequestTraceHasOwnership(*entry);
    if (BlockRequestTraceInteresting(*entry))
    {
        printf("BLOCKREQTRACE time_us=%lld event=INFLIGHT_EXPIRE hash=%s peer=%d addr=%s reason=timeout age_s=%lld other_peer_queued=%zu other_peer_inflight=%zu known_index=%d\n",
               (long long)nNow, hash.ToString().c_str(), pnode->GetId(),
               peerState->address.c_str(), (long long)nAge,
               nOtherQueued, nOtherInFlight, entry->indexed ? 1 : 0);
    }
}

void BlockRequestTracePeerClosed(CNode* pnode)
{
    if (!fBlockRequestTraceEnabled)
        return;
    int64_t nNow = GetTimeMicros();
    LOCK(cs_blockRequestTrace);
    for (std::map<uint256, BlockRequestTraceEntry>::iterator it = mapBlockRequestTrace.begin();
         it != mapBlockRequestTrace.end(); ++it)
    {
        BlockRequestTraceEntry& entry = it->second;
        std::map<NodeId, BlockRequestTracePeerState>::iterator peerIt =
            entry.peers.find(pnode->GetId());
        if (peerIt == entry.peers.end() ||
            (peerIt->second.queuedCount == 0 && !peerIt->second.inFlight))
            continue;

        int64_t nAge = peerIt->second.inFlightSince == 0
            ? -1 : (nNow - peerIt->second.inFlightSince) / 1000000;
        peerIt->second.queuedCount = 0;
        peerIt->second.inFlight = false;
        peerIt->second.inFlightSince = 0;
        entry.lastRemovalReason = "peer-destruction";
        entry.lastTouchedTime = nNow;
        if (BlockRequestTraceInteresting(entry))
        {
            printf("BLOCKREQTRACE time_us=%lld event=INFLIGHT_CLEAR hash=%s peer=%d addr=%s reason=peer-destruction age_s=%lld other_peer_queued=%zu other_peer_inflight=%zu known_index=%d\n",
                   (long long)nNow, it->first.ToString().c_str(), pnode->GetId(),
                   BlockRequestTracePeerAddress(pnode).c_str(), (long long)nAge,
                   BlockRequestTraceCountQueued(entry, pnode->GetId()),
                   BlockRequestTraceCountInFlight(entry, pnode->GetId()),
                   entry.indexed ? 1 : 0);
        }
        entry.completed = !BlockRequestTraceHasOwnership(entry);
    }
    BlockRequestTracePruneLocked(nNow, true);
    BlockRequestTraceMaybeSummaryLocked(nNow);
}

void BlockRequestTraceGetBlocksQueued(CNode* pnode,
                                      const uint256& hashBegin,
                                      int nBeginHeight,
                                      const uint256& hashStop)
{
    if (!fBlockRequestTraceEnabled)
        return;
    int64_t nNow = GetTimeMicros();
    LOCK(cs_blockRequestTrace);
    if (fBlockRequestTraceFilter &&
        !BlockRequestTraceMatchesFilter(hashBegin) &&
        !BlockRequestTraceMatchesFilter(hashStop))
        return;
    printf("BLOCKREQTRACE time_us=%lld event=GETBLOCKS_QUEUE peer=%d addr=%s begin_hash=%s begin_height=%d stop_hash=%s\n",
           (long long)nNow, pnode->GetId(), BlockRequestTracePeerAddress(pnode).c_str(),
           hashBegin.ToString().c_str(), nBeginHeight, hashStop.ToString().c_str());
    BlockRequestTraceMaybeSummaryLocked(nNow);
}

void BlockRequestTraceStallRecovery(CNode* pnode,
                                    int nLocalHeight,
                                    int nPeerHeight,
                                    int64_t nLastBlockAge,
                                    const uint256& hashBegin,
                                    int nBeginHeight,
                                    const uint256& hashStop,
                                    const std::vector<uint256>& vErasedHashes)
{
    if (!fBlockRequestTraceEnabled)
        return;
    int64_t nNow = GetTimeMicros();
    LOCK(cs_blockRequestTrace);
    ++blockRequestTraceCounters.stallRecoveryCount;
    size_t nAnomalousTouched = 0;
    for (std::vector<uint256>::const_iterator it = vErasedHashes.begin();
         it != vErasedHashes.end(); ++it)
    {
        BlockRequestTraceEntry* entry = BlockRequestTraceGetLocked(*it, nNow, false);
        if (!entry || !BlockRequestTraceInteresting(*entry))
            continue;
        ++nAnomalousTouched;
        ++entry->stallRecoveryCount;
        entry->anomalous = true;
    }

    if (fBlockRequestTraceFilter && nAnomalousTouched == 0 &&
        !BlockRequestTraceMatchesFilter(hashBegin) &&
        !BlockRequestTraceMatchesFilter(hashStop))
    {
        BlockRequestTraceMaybeSummaryLocked(nNow);
        return;
    }

    printf("BLOCKREQTRACE time_us=%lld event=STALL_RECOVERY peer=%d addr=%s local_height=%d peer_height=%d last_block_age_s=%lld begin_hash=%s begin_height=%d stop_hash=%s removed_msg_block=%zu removed_anomalous=%zu\n",
           (long long)nNow, pnode->GetId(), BlockRequestTracePeerAddress(pnode).c_str(),
           nLocalHeight, nPeerHeight, (long long)nLastBlockAge,
           hashBegin.ToString().c_str(), nBeginHeight, hashStop.ToString().c_str(),
           vErasedHashes.size(), nAnomalousTouched);

    for (std::vector<uint256>::const_iterator it = vErasedHashes.begin();
         it != vErasedHashes.end(); ++it)
    {
        std::map<uint256, BlockRequestTraceEntry>::iterator entryIt =
            mapBlockRequestTrace.find(*it);
        if (entryIt == mapBlockRequestTrace.end() ||
            !BlockRequestTraceInteresting(entryIt->second))
            continue;
        printf("BLOCKREQTRACE time_us=%lld event=STALL_TOUCH hash=%s peer=%d addr=%s stall_count=%llu\n",
               (long long)nNow, it->ToString().c_str(), pnode->GetId(),
               BlockRequestTracePeerAddress(pnode).c_str(),
               (unsigned long long)entryIt->second.stallRecoveryCount);
    }
    BlockRequestTraceMaybeSummaryLocked(nNow);
}

void BlockRequestTraceGetBlocksTrigger(CNode* pnode,
                                       const char* pszTrigger,
                                       const uint256& hashCause,
                                       int nReceived,
                                       int nExpected,
                                       bool fPrefetchSentBefore,
                                       const uint256& hashLastBatch,
                                       BlockRequestTraceResult lastResult,
                                       const uint256& hashBegin,
                                       int nBeginHeight,
                                       const uint256& hashStop)
{
    if (!fBlockRequestTraceEnabled)
        return;
    int64_t nNow = GetTimeMicros();
    LOCK(cs_blockRequestTrace);
    if (std::string(pszTrigger) == "batch75")
        ++blockRequestTraceCounters.batch75TriggerCount;
    else if (std::string(pszTrigger) == "pipeline-drained")
        ++blockRequestTraceCounters.pipelineTriggerCount;

    BlockRequestTraceEntry* entry =
        BlockRequestTraceGetLocked(hashCause, nNow, false);
    if (entry)
        ++entry->getBlocksWaveCount;

    if (fBlockRequestTraceFilter &&
        !BlockRequestTraceMatchesFilter(hashCause) &&
        !BlockRequestTraceMatchesFilter(hashLastBatch) &&
        !BlockRequestTraceMatchesFilter(hashBegin) &&
        !BlockRequestTraceMatchesFilter(hashStop))
    {
        BlockRequestTraceMaybeSummaryLocked(nNow);
        return;
    }

    printf("BLOCKREQTRACE time_us=%lld event=GETBLOCKS_TRIGGER hash=%s peer=%d addr=%s trigger=%s received=%d expected=%d prefetch_sent_before=%d last_batch_hash=%s previous_block_result=%s begin_hash=%s begin_height=%d stop_hash=%s hash_wave_count=%llu\n",
           (long long)nNow, hashCause.ToString().c_str(), pnode->GetId(),
           BlockRequestTracePeerAddress(pnode).c_str(), pszTrigger,
           nReceived, nExpected, fPrefetchSentBefore ? 1 : 0,
           hashLastBatch.ToString().c_str(),
           BlockRequestTraceResultName(lastResult),
           hashBegin.ToString().c_str(), nBeginHeight,
           hashStop.ToString().c_str(),
           (unsigned long long)(entry ? entry->getBlocksWaveCount : 0));
    BlockRequestTraceMaybeSummaryLocked(nNow);
}

static deque<string> vOneShots;
CCriticalSection cs_vOneShots;

set<CNetAddr> setservAddNodeAddresses;
CCriticalSection cs_setservAddNodeAddresses;

vector<std::string> vAddedNodes;
CCriticalSection cs_vAddedNodes;

static CSemaphore *semOutbound = NULL;

// Signals for message handling
static CNodeSignals g_signals;
CNodeSignals& GetNodeSignals() { return g_signals; }

void AddOneShot(string strDest)
{
    LOCK(cs_vOneShots);
    vOneShots.push_back(strDest);
}

unsigned short GetListenPort()
{
    return (unsigned short)(GetArg("-port", GetDefaultPort()));
}


void CNode::QueueInitialSyncRequest(CBlockIndex* pindexTip)
{
    if (fInitialSyncRequestPending || fInitialSyncRequestSent)
        return;
    const size_t nQueueBefore = getBlocksIndex.size();
    PushGetBlocks(pindexTip, uint256(0));
    if (getBlocksIndex.size() > nQueueBefore)
        fInitialSyncRequestPending = true;
}

void CNode::PushGetBlocks(CBlockIndex* pindexBegin, uint256 hashEnd)
{
    int64_t nNow = GetTime();

    if (pindexBegin == pindexLastGetBlocksBegin && hashEnd == hashLastGetBlocksEnd) {
        if (nNow - nLastGetBlocksTime < 5)
            return;
    }

    pindexLastGetBlocksBegin = pindexBegin;
    hashLastGetBlocksEnd = hashEnd;
    nLastGetBlocksTime = nNow;

    const size_t nQueueBefore = getBlocksIndex.size();
    getBlocksIndex.push_back(pindexBegin);
    getBlocksHash.push_back(hashEnd);
    const uint64_t nRecoveryId = nRecoveryTracePendingId;
    nRecoveryTracePendingId = 0;
    getBlocksRecoveryIds.push_back(nRecoveryId);
    RecoveryTraceQueue(this, nRecoveryId, pindexBegin, hashEnd,
                       nQueueBefore, getBlocksIndex.size());

    if (BlockRequestTraceEnabled())
    {
        BlockRequestTraceGetBlocksQueued(
            this,
            pindexBegin ? pindexBegin->GetBlockHash() : uint256(0),
            pindexBegin ? pindexBegin->nHeight : -1,
            hashEnd);
    }

    //PushMessage("getblocks", CBlockLocator(pindexBegin), hashEnd);
}

void CNode::PushGetHeaders(const CBlockLocator& locator, uint256 hashStop, const std::string& strReason)
{
    const int64_t nNow = GetTime();
    const std::string strReasonLabel = strReason.empty() ? std::string("unspecified") : strReason;
    const std::string strLocatorKey = LocatorFingerprint(locator, hashStop);
    uint256 hashLocatorTip = 0;
    uint256 hashLocatorTail = 0;
    const bool fHasLocator = locator.GetHashes(hashLocatorTip, hashLocatorTail);
    int nLocalHeight = -1;
    int nBestHeaderHeight = -1;
    int nPeerBestKnownHeight = -1;
    int nLocatorTipHeight = -1;
    {
        LOCK(cs_main);
        nLocalHeight = nBestHeight;
        nBestHeaderHeight = fSPVMode && pindexBest ? pindexBest->nHeight : -1;
        nPeerBestKnownHeight = nBestKnownHeight;
        if (fHasLocator) {
            std::map<uint256, CBlockIndex*>::const_iterator mi = mapBlockIndex.find(hashLocatorTip);
            if (mi != mapBlockIndex.end() && mi->second != NULL)
                nLocatorTipHeight = mi->second->nHeight;
        }
    }
    uint64_t nLocatorCount = 0;
    uint64_t nPeerSent = 0;
    uint64_t nPeerSuppressed = 0;
    const int64_t nPreviousRequestAge = getHeadersSync.LastRequestAge(nNow);
    const CGetHeadersSyncState::StartResult startResult =
        getHeadersSync.Start(strLocatorKey, nNow);
    const bool fSuppressed =
        startResult == CGetHeadersSyncState::SUPPRESSED_ACTIVE ||
        startResult == CGetHeadersSyncState::SUPPRESSED_COMPLETED;
    const char* pszStateReason =
        startResult == CGetHeadersSyncState::RETRIED_AFTER_TIMEOUT ? "timeout-retry" :
        startResult == CGetHeadersSyncState::SUPPRESSED_ACTIVE ? "active-request" :
        startResult == CGetHeadersSyncState::SUPPRESSED_COMPLETED ? "completed-request" :
        "new-request";
    {
        LOCK(cs_p2pMessageStats);
        GetHeadersPeerStats& peerStats = mapGetHeadersPeerStats[GetId()];
        GetHeadersLocatorState& locatorStats = peerStats.locators[strLocatorKey];
        if (fSuppressed) {
            locatorStats.suppressed++;
            peerStats.totalSuppressed++;
        } else {
            locatorStats.sent++;
            locatorStats.lastRequestTime = nNow;
            peerStats.totalSent++;
            peerStats.lastRequestTime = nNow;
        }
        nLocatorCount = locatorStats.sent + locatorStats.suppressed;
        nPeerSent = peerStats.totalSent;
        nPeerSuppressed = peerStats.totalSuppressed;
    }

    if (fDebugNet) {
        printf("GETHEADERS_TRACE: peer=%s peer_id=%lld reason=%s action=%s request_state=%s local_height=%d best_header_height=%d peer_best_known_height=%d locator_tip_hash=%s locator_tip_height=%d locator_size=%zu hash_stop=%s previous_request_age=%lld request_sequence=%llu locator_repeat=%llu peer_sent=%llu peer_suppressed=%llu inflight=%d\n",
               addrName.c_str(),
               (long long)GetId(),
               strReasonLabel.c_str(),
               fSuppressed ? "suppress" : "send",
               pszStateReason,
               nLocalHeight,
               nBestHeaderHeight,
               nPeerBestKnownHeight,
               fHasLocator ? hashLocatorTip.ToString().c_str() : "none",
               nLocatorTipHeight,
               locator.Size(),
               hashStop.ToString().c_str(),
               (long long)nPreviousRequestAge,
               (unsigned long long)getHeadersSync.RequestSequence(),
               (unsigned long long)nLocatorCount,
               (unsigned long long)nPeerSent,
               (unsigned long long)nPeerSuppressed,
               getHeadersSync.IsInFlight() ? 1 : 0);
    }

    if (fSuppressed)
        return;

    PushMessage("getheaders", locator, hashStop);
}

void RecordGetHeadersResponse(CNode* pnode, size_t nHeaders, unsigned int nBytes)
{
    (void)nHeaders;
    (void)nBytes;
    if (pnode == NULL)
        return;
    pnode->getHeadersSync.Complete(GetTime());
}



// find 'best' local address for a particular peer
bool GetLocal(CService& addr, const CNetAddr *paddrPeer)
{
    if (fNoListen)
        return false;

    int nBestScore = -1;
    int nBestReachability = -1;
    {
        LOCK(cs_mapLocalHost);
        for (map<CNetAddr, LocalServiceInfo>::iterator it = mapLocalHost.begin(); it != mapLocalHost.end(); it++)
        {
            int nScore = (*it).second.nScore;
            int nReachability = (*it).first.GetReachabilityFrom(paddrPeer);
            if (nReachability > nBestReachability || (nReachability == nBestReachability && nScore > nBestScore))
            {
                addr = CService((*it).first, (*it).second.nPort);
                nBestReachability = nReachability;
                nBestScore = nScore;
            }
        }
    }
    return nBestScore >= 0;
}

// get best local address for a particular peer as a CAddress
CAddress GetLocalAddress(const CNetAddr *paddrPeer)
{
    CAddress ret(CService("0.0.0.0",0),0);
    CService addr;
    if (GetLocal(addr, paddrPeer))
    {
        ret = CAddress(addr);
        ret.nServices = nLocalServices;
        ret.nTime = GetAdjustedTime();
    }
    return ret;
}

bool RecvLine(SOCKET hSocket, string& strLine)
{
    strLine = "";
    while (true)
    {
        char c;
        int nBytes = recv(hSocket, &c, 1, 0);
        if (nBytes > 0)
        {
            if (c == '\n')
                continue;
            if (c == '\r')
                return true;
            strLine += c;
            if (strLine.size() >= 9000)
                return true;
        }
        else if (nBytes <= 0)
        {
            if (fShutdown)
                return false;
            if (nBytes < 0)
            {
                int nErr = WSAGetLastError();
                if (nErr == WSAEMSGSIZE)
                    continue;
                if (nErr == WSAEWOULDBLOCK || nErr == WSAEINTR || nErr == WSAEINPROGRESS)
                {
                    MilliSleep(10);
                    continue;
                }
            }
            if (!strLine.empty())
                return true;
            if (nBytes == 0)
            {
                // socket closed
                printf("socket closed\n");
                return false;
            }
            else
            {
                // socket error
                int nErr = WSAGetLastError();
                printf("recv failed: %d\n", nErr);
                return false;
            }
        }
    }
}

// used when scores of local addresses may have changed
// pushes better local address to peers
void static AdvertizeLocal()
{
    LOCK(cs_vNodes);
    for (CNode* pnode : vNodes)
    {
        if (pnode->fSuccessfullyConnected)
        {
            CAddress addrLocal = GetLocalAddress(&pnode->addr);
            if (addrLocal.IsRoutable() && (CService)addrLocal != (CService)pnode->addrLocal)
            {
                pnode->PushAddress(addrLocal);
                pnode->addrLocal = addrLocal;
            }
        }
    }
}

void SetReachable(enum Network net, bool fFlag)
{
    LOCK(cs_mapLocalHost);
    vfReachable[net] = fFlag;
    if (net == NET_IPV6 && fFlag)
        vfReachable[NET_IPV4] = true;
}

// learn a new local address
bool AddLocal(const CService& addr, int nScore)
{
    if (!addr.IsRoutable())
        return false;

    if (!fDiscover && nScore < LOCAL_MANUAL)
        return false;

    if (IsLimited(addr))
        return false;

    printf("AddLocal(%s,%i)\n", addr.ToString().c_str(), nScore);

    {
        LOCK(cs_mapLocalHost);
        bool fAlready = mapLocalHost.count(addr) > 0;
        LocalServiceInfo &info = mapLocalHost[addr];
        if (!fAlready || nScore >= info.nScore) {
            info.nScore = nScore + (fAlready ? 1 : 0);
            info.nPort = addr.GetPort();
        }
        SetReachable(addr.GetNetwork());
    }

    AdvertizeLocal();

    return true;
}

bool AddLocal(const CNetAddr &addr, int nScore)
{
    return AddLocal(CService(addr, GetListenPort()), nScore);
}

/** Make a particular network entirely off-limits (no automatic connects to it) */
void SetLimited(enum Network net, bool fLimited)
{
    if (net == NET_UNROUTABLE)
        return;
    LOCK(cs_mapLocalHost);
    vfLimited[net] = fLimited;
}

bool IsLimited(enum Network net)
{
    LOCK(cs_mapLocalHost);
    return vfLimited[net];
}

bool IsLimited(const CNetAddr &addr)
{
    return IsLimited(addr.GetNetwork());
}

/** vote for a local address */
bool SeenLocal(const CService& addr)
{
    {
        LOCK(cs_mapLocalHost);
        if (mapLocalHost.count(addr) == 0)
            return false;
        mapLocalHost[addr].nScore++;
    }

    AdvertizeLocal();

    return true;
}

/** check whether a given address is potentially local */
bool IsLocal(const CService& addr)
{
    LOCK(cs_mapLocalHost);
    return mapLocalHost.count(addr) > 0;
}

/** check whether a given address is in a network we can probably connect to */
bool IsReachable(const CNetAddr& addr)
{
    LOCK(cs_mapLocalHost);
    enum Network net = addr.GetNetwork();
    return vfReachable[net] && !vfLimited[net];
}

#if 0
bool GetMyExternalIP2(const CService& addrConnect, const char* pszGet, const char* pszKeyword, CNetAddr& ipRet)
{
    SOCKET hSocket;
	bool bProxyConnectionFailed = false;
	int ntimeout = 1000;
    if (!ConnectSocket(addrConnect, hSocket, ntimeout, &bProxyConnectionFailed))
        return error("GetMyExternalIP() : connection to %s failed", addrConnect.ToString().c_str());

    send(hSocket, pszGet, strlen(pszGet), MSG_NOSIGNAL);

    string strLine;
    while (RecvLine(hSocket, strLine))
    {
        if (strLine.empty()) // HTTP response is separated from headers by blank line
        {
            while (true)
            {
                if (!RecvLine(hSocket, strLine))
                {
                    closesocket(hSocket);
                    return false;
                }
                if (pszKeyword == NULL)
                    break;
                if (strLine.find(pszKeyword) != string::npos)
                {
                    strLine = strLine.substr(strLine.find(pszKeyword) + strlen(pszKeyword));
                    break;
                }
            }
            closesocket(hSocket);
            if (strLine.find("<") != string::npos)
                strLine = strLine.substr(0, strLine.find("<"));
            strLine = strLine.substr(strspn(strLine.c_str(), " \t\n\r"));
            while (strLine.size() > 0 && isspace(strLine[strLine.size()-1]))
                strLine.resize(strLine.size()-1);
            CService addr(strLine,0,true);
            printf("GetMyExternalIP() received [%s] %s\n", strLine.c_str(), addr.ToString().c_str());
            if (!addr.IsValid() || !addr.IsRoutable())
                return false;
            ipRet.SetIP(addr);
            return true;
        }
    }
    closesocket(hSocket);
    return error("GetMyExternalIP() : connection closed");
}

// We now get our external IP from the IRC server first and only use this as a backup
bool GetMyExternalIP(CNetAddr& ipRet)
{
    CService addrConnect;
    const char* pszGet = NULL;
    const char* pszKeyword = NULL;

    for (int nLookup = 0; nLookup <= 1; nLookup++)
    for (int nHost = 1; nHost <= 2; nHost++)
    {
        // We should be phasing out our use of sites like these.  If we need
        // replacements, we should ask for volunteers to put this simple
        // php file on their web server that prints the client IP:
        //  <?php echo $_SERVER["REMOTE_ADDR"]; ?>
        if (nHost == 1)
        {
            addrConnect = CService("91.198.22.70",80); // checkip.dyndns.org

            if (nLookup == 1)
            {
                CService addrIP("checkip.dyndns.org", 80, true);
                if (addrIP.IsValid())
                    addrConnect = addrIP;
            }

            pszGet = "GET / HTTP/1.1\r\n"
                     "Host: checkip.dyndns.org\r\n"
                     "User-Agent: Innova\r\n"
                     "Connection: close\r\n"
                     "\r\n";

            pszKeyword = "Address:";
        }
        else if (nHost == 2)
        {
            addrConnect = CService("74.208.43.192", 80); // www.showmyip.com

            if (nLookup == 1)
            {
                CService addrIP("www.showmyip.com", 80, true);
                if (addrIP.IsValid())
                    addrConnect = addrIP;
            }

            pszGet = "GET /simple/ HTTP/1.1\r\n"
                     "Host: www.showmyip.com\r\n"
                     "User-Agent: Innova\r\n"
                     "Connection: close\r\n"
                     "\r\n";

            pszKeyword = NULL; // Returns just IP address
        }

        if (GetMyExternalIP2(addrConnect, pszGet, pszKeyword, ipRet))
            return true;
    }

    return false;
}
#endif

/*--------------------------------------------------------------------------*/
// from file stun.cpp
int GetExternalIPbySTUN(uint64_t rnd, struct sockaddr_in *mapped, const char **srv);

/*--------------------------------------------------------------------------*/
bool GetMyExternalIP_STUN(CNetAddr& ipRet) {
    struct sockaddr_in mapped;
    uint64_t rnd = GetRand(~0LL);
    const char *srv;
    int rc = GetExternalIPbySTUN(rnd, &mapped, &srv);
    if(rc > 0) {
        ipRet = CNetAddr(mapped.sin_addr);
        CNetAddr addrLocalHost;
        printf("GetExternalIPbySTUN() returned %s in attempt %u; flags=%x; Server=%s\n", addrLocalHost.ToStringIP().c_str(), rc >> 8, (uint8_t)rc, srv);
        return true;
    }
    return false;
} // GetMyExternalIP_STUN

void ThreadGetMyExternalIP(void* parg)
{
    // Make this thread recognisable as the external IP detection thread
    RenameThread("innova-ext-ip");

    CNetAddr addrLocalHost;
    if (GetMyExternalIP_STUN(addrLocalHost)) //GetMyExternalIP by STUN instead now
    {
        //printf("GetMyExternalIP() returned %s\n", addrLocalHost.ToStringIP().c_str());
        AddLocal(addrLocalHost, LOCAL_HTTP);
    }
}





void AddressCurrentlyConnected(const CService& addr)
{
    addrman.Connected(addr);
}





uint64_t CNode::nTotalBytesRecv = 0;
uint64_t CNode::nTotalBytesSent = 0;
CCriticalSection CNode::cs_totalBytesRecv;
CCriticalSection CNode::cs_totalBytesSent;

uint64_t CNode::nMaxOutboundLimit = 0;
uint64_t CNode::nMaxOutboundTotalBytesSentInCycle = 0;
uint64_t CNode::nMaxOutboundTimeframe = 60*60*24; //1 day
uint64_t CNode::nMaxOutboundCycleStartTime = 0;

CNode* FindNode(const CNetAddr& ip)
{
    {
        LOCK(cs_vNodes);
        for (CNode* pnode : vNodes)
            if ((CNetAddr)pnode->addr == ip)
                return (pnode);
    }
    return NULL;
}

CNode* FindNode(const CSubNet& subNet)
{
    vector<CNode*> vNodesCopy;
    {
        LOCK(cs_vNodes);
        vNodesCopy = vNodes;
    }

    for (CNode* pnode : vNodesCopy)
        if (pnode && subNet.Match((CNetAddr)pnode->addr))
            return (pnode);
    return NULL;
}

CNode* FindNode(const std::string& addrName)
{
    LOCK(cs_vNodes);
    for (CNode* pnode : vNodes)
        if (pnode->addrName == addrName)
            return (pnode);
    return NULL;
}

CNode* FindNode(const CService& addr)
{
    {
        LOCK(cs_vNodes);
        for (CNode* pnode : vNodes)
            if ((CService)pnode->addr == addr)
                return (pnode);
    }
    return NULL;
}

CNode* ConnectNode(CAddress addrConnect, const char *pszDest, bool colLateralMaster)
{
    if (pszDest == NULL) {
        if (IsLocal(addrConnect))
            return NULL;

        // Look for an existing connection
        CNode* pnode = FindNode((CService)addrConnect);
        if (pnode)
        {

        if(colLateralMaster)
                pnode->fColLateralMaster = true;
            pnode->AddRef();

            pnode->PushMessage("mktinv", GetTime() - (7 * 24 * 60 * 60));

            return pnode;
        }
    }


    /// debug print
        if (fDebugNet) printf("net: trying connection %s lastseen=%.1fhrs\n",
        pszDest ? pszDest : addrConnect.ToString().c_str(),
        pszDest ? 0 : (double)(GetAdjustedTime() - addrConnect.nTime)/3600.0);

    // Connect
    SOCKET hSocket;
    bool proxyConnectionFailed = false;
    if (pszDest ? ConnectSocketByName(addrConnect, hSocket, pszDest, GetDefaultPort(), nConnectTimeout, &proxyConnectionFailed) :
                  ConnectSocket(addrConnect, hSocket, nConnectTimeout, &proxyConnectionFailed))
    {
        if (!IsSelectableSocket(hSocket)) {
            printf("Cannot create connection: non-selectable socket created (fd >= FD_SETSIZE ?)\n");
            closesocket(hSocket);
            return NULL;
        }
        addrman.Attempt(addrConnect);

        if (fDebugNet) printf("net: connected %s\n", pszDest ? pszDest : addrConnect.ToString().c_str());

        // Set to non-blocking
#ifdef WIN32
        u_long nOne = 1;
        if (ioctlsocket(hSocket, FIONBIO, &nOne) == SOCKET_ERROR)
            printf("ConnectSocket() : ioctlsocket non-blocking setting failed, error %d\n", WSAGetLastError());
#else
        if (fcntl(hSocket, F_SETFL, O_NONBLOCK) == SOCKET_ERROR)
            printf("ConnectSocket() : fcntl non-blocking setting failed, error %d\n", errno);
#endif

        // Add node
        CNode* pnode = new CNode(hSocket, addrConnect, pszDest ? pszDest : "", false);
        pnode->AddRef();

        {
            LOCK(cs_vNodes);
            vNodes.push_back(pnode);
        }

        if(colLateralMaster)
                pnode->fColLateralMaster = true;
        pnode->nTimeConnected = GetTime();
        return pnode;
    } else if (!proxyConnectionFailed) {
        // If connecting to the node failed, and failure is not caused by a problem connecting to
        // the proxy, mark this as an attempt.
        addrman.Attempt(addrConnect);
    }

    return NULL;
}

void CNode::CloseSocketDisconnect()
{
    fDisconnect = true;

    dandelionRouter.OnStemPeerDisconnect(GetId());

    if (hSocket != INVALID_SOCKET)
    {
        printf("Net() Disconnecting node %s\n", addrName.c_str());
        closesocket(hSocket);
        hSocket = INVALID_SOCKET;

        // in case this fails, we'll empty the recv buffer when the CNode is deleted
        TRY_LOCK(cs_vRecvMsg, lockRecv);
        if (lockRecv)
            vRecvMsg.clear();
    }
}

void CNode::Cleanup()
{
}


void CNode::PushVersion()
{
    /// when NTP implemented, change to just nTime = GetAdjustedTime()
    int64_t nTime = (fInbound ? GetAdjustedTime() : GetTime());
    CAddress addrYou = (addr.IsRoutable() && !IsProxy(addr) ? addr : CAddress(CService("0.0.0.0",0)));
    CAddress addrMe = GetLocalAddress(&addr);
    RAND_bytes((unsigned char*)&nLocalHostNonce, sizeof(nLocalHostNonce));
    printf("send version message: version %d, blocks=%d, us=%s, them=%s, peer=%s\n", PROTOCOL_VERSION, nBestHeight, addrMe.ToString().c_str(), addrYou.ToString().c_str(), addr.ToString().c_str());
    PushMessage("version", PROTOCOL_VERSION, nLocalServices, nTime, addrYou, addrMe,
                nLocalHostNonce, FormatSubVersion(CLIENT_NAME, CLIENT_VERSION, std::vector<string>()), nBestHeight);
}





banmap_t CNode::setBanned;
CCriticalSection CNode::cs_setBanned;
bool CNode::setBannedIsDirty;

void CNode::ClearBanned()
{
    LOCK(cs_setBanned);
    setBanned.clear();
    setBannedIsDirty = true;
}

bool CNode::IsBanned(CNetAddr ip)
{
    bool fResult = false;
    {
        LOCK(cs_setBanned);
        for (banmap_t::iterator it = setBanned.begin(); it != setBanned.end(); it++)
        {
            CSubNet subNet = (*it).first;
            CBanEntry banEntry = (*it).second;

            if(subNet.Match(ip) && GetTime() < banEntry.nBanUntil)
                fResult = true;
        }
    }
    return fResult;
}

bool CNode::IsBanned(CSubNet subnet)
{
    bool fResult = false;
    {
        LOCK(cs_setBanned);
        banmap_t::iterator i = setBanned.find(subnet);
        if (i != setBanned.end()) {
            CBanEntry banEntry = (*i).second;
            if (GetTime() < banEntry.nBanUntil)
                fResult = true;
        }
    }
    return fResult;
}

void CNode::Ban(const CNetAddr& addr, const BanReason &banReason, int64_t bantimeoffset, bool sinceUnixEpoch)
{
    CSubNet subNet(addr);
    Ban(subNet, banReason, bantimeoffset, sinceUnixEpoch);
}

void CNode::Ban(const CSubNet& subNet, const BanReason &banReason, int64_t bantimeoffset, bool sinceUnixEpoch) {
    CBanEntry banEntry(GetTime());
    banEntry.banReason = banReason;
    if (bantimeoffset <= 0)
    {
        bantimeoffset = GetArg("-bantime", 60*60*24); // Default 24-hour ban
        sinceUnixEpoch = false;
    }
    banEntry.nBanUntil = (sinceUnixEpoch ? 0 : GetTime() )+bantimeoffset;

    LOCK(cs_setBanned);
    if (setBanned[subNet].nBanUntil < banEntry.nBanUntil)
        setBanned[subNet] = banEntry;

    setBannedIsDirty = true;
}

bool CNode::Unban(const CNetAddr &addr)
{
    CSubNet subNet(addr);
    return Unban(subNet);
}

bool CNode::Unban(const CSubNet &subNet)
{
    LOCK(cs_setBanned);
    if (setBanned.erase(subNet))
    {
        setBannedIsDirty = true;
        return true;
    }
    return false;
}

void CNode::GetBanned(banmap_t &banMap)
{
    LOCK(cs_setBanned);
    // Sweep the banlist so expired bans are not returned
    SweepBanned();
    banMap = setBanned; //create a thread safe copy
}

void CNode::SetBanned(const banmap_t &banMap)
{
    LOCK(cs_setBanned);
    setBanned = banMap;
    setBannedIsDirty = true;
}

void CNode::SweepBanned()
{
    int64_t now = GetTime();

    LOCK(cs_setBanned);
    banmap_t::iterator it = setBanned.begin();
    while(it != setBanned.end())
    {
        CBanEntry banEntry = (*it).second;
        if(now > banEntry.nBanUntil)
        {
            setBanned.erase(it++);
            setBannedIsDirty = true;
        }
        else
            ++it;
    }
}

bool CNode::BannedSetIsDirty()
{
    LOCK(cs_setBanned);
    return setBannedIsDirty;
}

void CNode::SetBannedSetDirty(bool dirty)
{
    LOCK(cs_setBanned); //reuse setBanned lock for the isDirty flag
    setBannedIsDirty = dirty;
}


std::vector<CSubNet> CNode::vWhitelistedRange;
CCriticalSection CNode::cs_vWhitelistedRange;

bool CNode::IsWhitelistedRange(const CNetAddr& addr)
{
    LOCK(cs_vWhitelistedRange);
    for (const CSubNet& subnet : vWhitelistedRange) {
        if (subnet.Match(addr))
            return true;
    }
    return false;
}

void CNode::AddWhitelistedRange(const CSubNet& subnet)
{
    LOCK(cs_vWhitelistedRange);
    vWhitelistedRange.push_back(subnet);
}

//Whitelisted peers/nodes
//std::vector<CSubNet> CNode::vWhitelistedRange;
//CCriticalSection CNode::cs_vWhitelistedRange;
/*
bool CNode::IsWhitelistedRange(const CNetAddr &addr) {
    LOCK(cs_vWhitelistedRange);
    BOOST_FOREACH(const CSubNet& subnet, vWhitelistedRange) {
        if (subnet.Match(addr))
            return true;
    }
    return false;
}

void CNode::AddWhitelistedRange(const CSubNet &subnet) {
    LOCK(cs_vWhitelistedRange);
    vWhitelistedRange.push_back(subnet);
}
 */

bool CNode::Misbehaving(int howmuch)
{
    CSubNet subNet(addr);
    if (addr.IsLocal())
    {
        printf("Warning: Local node %s misbehaving (delta: %d)!\n", addrName.c_str(), howmuch);
        return false;
    }

    int nCurrentMisbehavior;
    {
        LOCK(cs_nMisbehavior);
        nMisbehavior += howmuch;
        nCurrentMisbehavior = nMisbehavior;
    }

    if (nCurrentMisbehavior >= GetArg("-banscore", 100))
    {
        int64_t banTime = GetTime()+GetArg("-bantime", 60*60*24);  // Default 24-hour ban
        printf("Misbehaving: %s (%d -> %d) DISCONNECTING\n", addr.ToString().c_str(), nCurrentMisbehavior-howmuch, nCurrentMisbehavior);
        {
            LOCK(cs_setBanned);
            if (setBanned[subNet].nBanUntil < banTime)
                setBanned[subNet] = banTime;
        }
        CloseSocketDisconnect();
        return true;
    } else
        printf("Misbehaving: %s (%d -> %d)\n", addr.ToString().c_str(), nCurrentMisbehavior-howmuch, nCurrentMisbehavior);
    return false;
}

#undef X
#define X(name) stats.name = name
void CNode::copyStats(CNodeStats &stats)
{
	stats.nodeid = this->GetId();
    X(nServices);
    X(nLastSend);
    X(nLastRecv);
    X(nTimeConnected);
    X(addrName);
    X(nVersion);
    X(strSubVer);
    X(fInbound);
    X(nChainHeight);
    X(nBestKnownHeight);
    stats.hashBestKnownBlock = hashBestKnownBlock.ToString();
    X(nLastBlockRecv);
    X(nLastHeightUpdate);
    stats.nBlocksInFlight = (int)setBlocksInFlight.size();
    stats.nAskForSize = (int)mapAskFor.size();
    X(nMisbehavior);
    {
        LOCK(cs_vSend);
        X(nSendBytes);
    }
    {
        LOCK(cs_vRecv);
        X(nRecvBytes);
    }
    X(fWhitelisted);

    // It is common for nodes with good ping times to suddenly become lagged,
    // due to a new block arriving or other large transfer.
    // Merely reporting pingtime might fool the caller into thinking the node was still responsive,
    // since pingtime does not update until the ping is complete, which might take a while.
    // So, if a ping is taking an unusually long time in flight,
    // the caller can immediately detect that this is happening.
    int64_t nPingUsecWait = 0;
    if ((0 != nPingNonceSent) && (0 != nPingUsecStart)) {
        nPingUsecWait = GetTimeMicros() - nPingUsecStart;
    }

    // Raw ping time is in microseconds, but show it to user as whole seconds (Bitcoin users should be well used to small numbers with many decimal places by now :)
    stats.dPingTime = (((double)nPingUsecTime) / 1e6);
    stats.dPingWait = (((double)nPingUsecWait) / 1e6);

    // Leave string empty if addrLocal invalid (not filled in yet)
    stats.addrLocal = addrLocal.IsValid() ? addrLocal.ToString() : "";
}
#undef X

void CNode::RecordBytesRecv(uint64_t bytes)
{
    LOCK(cs_totalBytesRecv);
    nTotalBytesRecv += bytes;
}

void CNode::RecordBytesSent(uint64_t bytes)
{
    LOCK(cs_totalBytesSent);
    nTotalBytesSent += bytes;

    uint64_t now = GetTime();
    if (nMaxOutboundCycleStartTime + nMaxOutboundTimeframe < now)
    {
        // timeframe expired, reset cycle
        nMaxOutboundCycleStartTime = now;
        nMaxOutboundTotalBytesSentInCycle = 0;
    }

    // TODO, exclude whitebind peers
    nMaxOutboundTotalBytesSentInCycle += bytes;
}

void CNode::SetMaxOutboundTarget(uint64_t limit)
{
    LOCK(cs_totalBytesSent);
    uint64_t recommendedMinimum = (nMaxOutboundTimeframe / 600) * MAX_BLOCK_SIZE;
    nMaxOutboundLimit = limit;

    if (limit < recommendedMinimum)
        printf("Max outbound target is very small (%d) and will be overshot. Recommended minimum is %d\n.", nMaxOutboundLimit, recommendedMinimum);
}

uint64_t CNode::GetMaxOutboundTarget()
{
    LOCK(cs_totalBytesSent);
    return nMaxOutboundLimit;
}

uint64_t CNode::GetMaxOutboundTimeframe()
{
    LOCK(cs_totalBytesSent);
    return nMaxOutboundTimeframe;
}

uint64_t CNode::GetMaxOutboundTimeLeftInCycle()
{
    LOCK(cs_totalBytesSent);
    if (nMaxOutboundLimit == 0)
        return 0;

    if (nMaxOutboundCycleStartTime == 0)
        return nMaxOutboundTimeframe;

    uint64_t cycleEndTime = nMaxOutboundCycleStartTime + nMaxOutboundTimeframe;
    uint64_t now = GetTime();
    return (cycleEndTime < now) ? 0 : cycleEndTime - GetTime();
}

void CNode::SetMaxOutboundTimeframe(uint64_t timeframe)
{
    LOCK(cs_totalBytesSent);
    if (nMaxOutboundTimeframe != timeframe)
    {
        // reset measure-cycle in case of changing
        // the timeframe
        nMaxOutboundCycleStartTime = GetTime();
    }
    nMaxOutboundTimeframe = timeframe;
}

bool CNode::OutboundTargetReached(bool historicalBlockServingLimit)
{
    LOCK(cs_totalBytesSent);
    if (nMaxOutboundLimit == 0)
        return false;

    if (historicalBlockServingLimit)
    {
        // keep a large enought buffer to at least relay each block once
        uint64_t timeLeftInCycle = GetMaxOutboundTimeLeftInCycle();
        uint64_t buffer = timeLeftInCycle / 600 * MAX_BLOCK_SIZE;
        if (buffer >= nMaxOutboundLimit || nMaxOutboundTotalBytesSentInCycle >= nMaxOutboundLimit - buffer)
            return true;
    }
    else if (nMaxOutboundTotalBytesSentInCycle >= nMaxOutboundLimit)
        return true;

    return false;
}

uint64_t CNode::GetOutboundTargetBytesLeft()
{
    LOCK(cs_totalBytesSent);
    if (nMaxOutboundLimit == 0)
        return 0;

    return (nMaxOutboundTotalBytesSentInCycle >= nMaxOutboundLimit) ? 0 : nMaxOutboundLimit - nMaxOutboundTotalBytesSentInCycle;
}

uint64_t CNode::GetTotalBytesRecv()
{
    LOCK(cs_totalBytesRecv);
    return nTotalBytesRecv;
}

uint64_t CNode::GetTotalBytesSent()
{
    LOCK(cs_totalBytesSent);
    return nTotalBytesSent;
}

// requires LOCK(cs_vRecvMsg)
bool CNode::ReceiveMsgBytes(const char *pch, unsigned int nBytes)
{
    while (nBytes > 0) {

        // get current incomplete message, or create a new one
        if (vRecvMsg.empty() ||
            vRecvMsg.back().complete())
            vRecvMsg.push_back(CNetMessage(SER_NETWORK, nRecvVersion));

        CNetMessage& msg = vRecvMsg.back();

        // absorb network data
        int handled;
        if (!msg.in_data)
            handled = msg.readHeader(pch, nBytes);
        else
            handled = msg.readData(pch, nBytes);

        if (handled < 0)
                return false;

        pch += handled;
        nBytes -= handled;

        if (msg.complete())
            msg.nTime = GetTimeMicros();
    }

    return true;
}

int CNetMessage::readHeader(const char *pch, unsigned int nBytes)
{
    // copy data to temporary parsing buffer
    unsigned int nRemaining = 24 - nHdrPos;
    unsigned int nCopy = std::min(nRemaining, nBytes);

    memcpy(&hdrbuf[nHdrPos], pch, nCopy);
    nHdrPos += nCopy;

    // if header incomplete, exit
    if (nHdrPos < 24)
        return nCopy;

    // deserialize to CMessageHeader
    try {
        hdrbuf >> hdr;
    }
    catch (std::exception &e) {
        return -1;
    }

    // reject messages larger than MAX_SIZE
    if (hdr.nMessageSize > MAX_SIZE)
            return -1;

    // switch state to reading message data
    in_data = true;
    vRecv.resize(hdr.nMessageSize);

    return nCopy;
}

int CNetMessage::readData(const char *pch, unsigned int nBytes)
{
    if (nDataPos >= hdr.nMessageSize)
        return -1;
    unsigned int nRemaining = hdr.nMessageSize - nDataPos;
    unsigned int nCopy = std::min(nRemaining, nBytes);

    if (nCopy > 0)
        memcpy(&vRecv[nDataPos], pch, nCopy);
    nDataPos += nCopy;

    return nCopy;
}









// requires LOCK(cs_vSend)
void SocketSendData(CNode *pnode)
{
    std::deque<CSerializeData>::iterator it = pnode->vSendMsg.begin();

    while (it != pnode->vSendMsg.end()) {
        const CSerializeData &data = *it;
        if (data.size() <= pnode->nSendOffset)
        {
            printf("DEBUG-DISCONNECT SocketSendData corrupt state data=%zu offset=%zu peer=%s\n",
                   data.size(), (size_t)pnode->nSendOffset, pnode->addr.ToString().c_str());
            pnode->CloseSocketDisconnect();
            return;
        }
        int nBytes = send(pnode->hSocket, &data[pnode->nSendOffset], data.size() - pnode->nSendOffset, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (nBytes > 0) {
            pnode->nLastSend = GetTime();
            pnode->nSendOffset += nBytes;

            pnode->nSendBytes += nBytes;
            pnode->RecordBytesSent(nBytes);

            if (pnode->nSendOffset == data.size()) {
                pnode->nSendOffset = 0;
                pnode->nSendSize -= data.size();
                it++;
            } else {
                // could not send full message; stop sending more
                break;
            }
        } else {
            if (nBytes < 0) {
                // error
                int nErr = WSAGetLastError();
                if (nErr != WSAEWOULDBLOCK && nErr != WSAEMSGSIZE && nErr != WSAEINTR && nErr != WSAEINPROGRESS)
                {
                    printf("DEBUG-DISCONNECT send-error %d peer=%s\n", nErr, pnode->addr.ToString().c_str());
                    pnode->CloseSocketDisconnect();
                }
            }
            else if (nBytes > 100000) // 100,000 Bytes
            {
                // error
                int nErr = WSAGetLastError();
                if (nErr != WSAEWOULDBLOCK && nErr != WSAEMSGSIZE && nErr != WSAEINTR && nErr != WSAEINPROGRESS)
                {
                    if (!pnode->fDisconnect)
                        printf("Closing send socket, output too large %d\n", nErr);
                    pnode->CloseSocketDisconnect();
                }
            }
            // couldn't send anything at all
            break;
        }
    }

    if (it == pnode->vSendMsg.end()) {
        if (pnode->nSendOffset != 0 || pnode->nSendSize != 0)
        {
            printf("SocketSendData: warning - queue empty but nSendOffset=%zu nSendSize=%zu, resetting\n",
                   (size_t)pnode->nSendOffset, (size_t)pnode->nSendSize);
            pnode->nSendOffset = 0;
            pnode->nSendSize = 0;
        }
    }
    pnode->vSendMsg.erase(pnode->vSendMsg.begin(), it);
}

static list<CNode*> vNodesDisconnected;

static void AcceptConnection(const ListenSocket& hListenSocket) {

    struct sockaddr_storage sockaddr;
    socklen_t len = sizeof(sockaddr);
    SOCKET hSocket = accept(hListenSocket.socket, (struct sockaddr *) &sockaddr, &len);
    CAddress addr;
    int nInbound = 0;
    // Reserve outbound slots to prevent eclipse attacks
    int nMaxOutbound = GetArg("-maxoutbound", 8);
    int nMaxInbound = GetArg("-maxconnections", 125) - nMaxOutbound;

    if (hSocket != INVALID_SOCKET)
        if (!addr.SetSockAddr((const struct sockaddr *) &sockaddr))
            printf("Warning: Unknown socket family\n");

    bool whitelisted = hListenSocket.whitelisted || CNode::IsWhitelistedRange(addr);

    vector<CNode*> vNodesCopy;
    {
        LOCK(cs_vNodes);
        vNodesCopy = vNodes;
    }

    for (CNode *pnode : vNodesCopy)
        if (pnode && pnode->fInbound)
            nInbound++;

    if (hSocket == INVALID_SOCKET) {
        int nErr = WSAGetLastError();
        if (nErr != WSAEWOULDBLOCK)
            printf("socket error accept failed: %s\n", nErr);
        return;
    }

    if (!IsSelectableSocket(hSocket)) {
        printf("connection from %s dropped: non-selectable socket\n", addr.ToString().c_str());
        CloseSocket(hSocket);
        return;
    }

    if (CNode::IsBanned(addr)) { //  && !whitelisted
        printf("connection from %s dropped (banned)\n", addr.ToString().c_str());
        CloseSocket(hSocket);
        return;
    }

    if (!whitelisted)
    {
        LOCK(cs_connectionRateLimit);
        int64_t nNow = GetTime();
        CNetAddr netAddr = addr;

        std::vector<int64_t>& vAttempts = mapConnectionAttempts[netAddr];
        vAttempts.erase(std::remove_if(vAttempts.begin(), vAttempts.end(),
            [nNow](int64_t t) { return (nNow - t) > CONNECTION_RATE_LIMIT_WINDOW; }), vAttempts.end());

        if ((int)vAttempts.size() >= CONNECTION_RATE_LIMIT_MAX)
        {
            printf("connection from %s dropped (rate limit: %d connections in %d seconds)\n",
                   addr.ToString().c_str(), CONNECTION_RATE_LIMIT_MAX, CONNECTION_RATE_LIMIT_WINDOW);
            CloseSocket(hSocket);
            return;
        }

        vAttempts.push_back(nNow);

        static int64_t nLastCleanup = 0;
        if (nNow - nLastCleanup > 300)
        {
            for (auto it = mapConnectionAttempts.begin(); it != mapConnectionAttempts.end(); )
            {
                if (it->second.empty())
                    it = mapConnectionAttempts.erase(it);
                else
                    ++it;
            }
            nLastCleanup = nNow;
        }
    }

    if (nInbound >= nMaxInbound) {
        printf("connection from %s dropped (full)\n", addr.ToString().c_str());
        CloseSocket(hSocket);
        return;
    }

    if (nInbound >= GetArg("-maxconnections", 125) - MAX_OUTBOUND_CONNECTIONS) {
        printf("connection from %s dropped (full)\n", addr.ToString().c_str());
        CloseSocket(hSocket);
        return;
    }

    if (!whitelisted)
    {
        std::vector<unsigned char> vchNetGroup = addr.GetGroup();
        int nSameNetGroup = 0;

        {
            LOCK(cs_vNodes);
            for (CNode* pnode : vNodes)
            {
                if (pnode && pnode->fInbound)
                {
                    std::vector<unsigned char> vchPeerGroup = pnode->addr.GetGroup();
                    if (vchPeerGroup == vchNetGroup)
                        nSameNetGroup++;
                }
            }
        }

        if (nSameNetGroup >= MAX_INBOUND_PER_NETGROUP)
        {
            printf("connection from %s dropped (too many from same /16 subnet: %d/%d)\n",
                   addr.ToString().c_str(), nSameNetGroup, MAX_INBOUND_PER_NETGROUP);
            CloseSocket(hSocket);
            return;
        }
    }

        CNode *pnode = new CNode(hSocket, addr, "", true);
        if(!pnode) return;

        pnode->AddRef();
        pnode->fWhitelisted = whitelisted;
        pnode->nTimeConnected = GetTime();
        {
            LOCK(cs_vNodes);
            vNodes.push_back(pnode);
        }
    }

void ThreadSocketHandler(void* parg)
{
    // Make this thread recognisable as the networking thread
    RenameThread("innova-net");

    try
    {
        vnThreadsRunning[THREAD_SOCKETHANDLER]++;
        ThreadSocketHandler2(parg);
        vnThreadsRunning[THREAD_SOCKETHANDLER]--;
    }
    catch (std::exception& e) {
        vnThreadsRunning[THREAD_SOCKETHANDLER]--;
        PrintException(&e, "ThreadSocketHandler()");
    } catch (...) {
        vnThreadsRunning[THREAD_SOCKETHANDLER]--;
        throw; // support pthread_cancel()
    }
    printf("ThreadSocketHandler exited\n");
}

void ThreadSocketHandler2(void* parg)
{
    printf("ThreadSocketHandler started\n");
    list<CNode*> vNodesDisconnected;
    unsigned int nPrevNodeCount = 0;

    while (true)
    {
        //
        // Disconnect nodes
        //
        {
            LOCK(cs_vNodes);
            // Disconnect unused nodes
            vector<CNode*> vNodesCopy = vNodes;
            for (CNode* pnode : vNodesCopy)
            {
                if (pnode->fDisconnect ||
                    (pnode->GetRefCount() <= 0 && pnode->vRecvMsg.empty() && pnode->nSendSize == 0 && pnode->ssSend.empty()))
                {
                    if (!pnode->fDisconnect)
                        printf("DEBUG-DISCONNECT cleanup-loop ref=%d recvEmpty=%d sendSize=%zu ssEmpty=%d peer=%s\n",
                               pnode->GetRefCount(), pnode->vRecvMsg.empty(), pnode->nSendSize, pnode->ssSend.empty(),
                               pnode->addr.ToString().c_str());
                    // remove from vNodes
                    vNodes.erase(remove(vNodes.begin(), vNodes.end(), pnode), vNodes.end());

                    // release outbound grant (if any)
                    pnode->grantOutbound.Release();

                    // close socket and cleanup
                    pnode->CloseSocketDisconnect();
                    pnode->Cleanup();

                    // hold in disconnected pool until all refs are released
                    if (pnode->fNetworkNode || pnode->fInbound)
                        pnode->Release();
                    vNodesDisconnected.push_back(pnode);
                }
            }

            // Delete disconnected nodes
            list<CNode*> vNodesDisconnectedCopy = vNodesDisconnected;
            BOOST_FOREACH(CNode* pnode, vNodesDisconnectedCopy)
            {
                // wait until threads are done using it
                if (pnode->GetRefCount() <= 0)
                {
                    bool fDelete = false;
                    {
                        TRY_LOCK(pnode->cs_vSend, lockSend);
                        if (lockSend)
                        {
                            TRY_LOCK(pnode->cs_vRecvMsg, lockRecv);
                            if (lockRecv)
                            {
                                TRY_LOCK(pnode->cs_mapRequests, lockReq);
                                if (lockReq)
                                {
                                    TRY_LOCK(pnode->cs_inventory, lockInv);
                                    if (lockInv)
                                        fDelete = true;
                                }
                            }
                        }
                    }
                    if (fDelete)
                    {
                        vNodesDisconnected.remove(pnode);
                        delete pnode;
                    }
                }
            }
        }
        if (vNodes.size() != nPrevNodeCount)
        {
            nPrevNodeCount = vNodes.size();
            uiInterface.NotifyNumConnectionsChanged(vNodes.size());
        }


        //
        // Find which sockets have data to receive
        //
        struct timeval timeout;
        timeout.tv_sec  = 0;
        timeout.tv_usec = 50000; // frequency to poll pnode->vSend

        fd_set fdsetRecv;
        fd_set fdsetSend;
        fd_set fdsetError;
        FD_ZERO(&fdsetRecv);
        FD_ZERO(&fdsetSend);
        FD_ZERO(&fdsetError);
        SOCKET hSocketMax = 0;
        bool have_fds = false;

        for (const ListenSocket& hListenSocket : vhListenSocket) {
            FD_SET(hListenSocket.socket, &fdsetRecv);
            hSocketMax = max(hSocketMax, hListenSocket.socket);
            have_fds = true;
        }

        {
            LOCK(cs_vNodes);
            for (CNode* pnode : vNodes)
            {
                if (pnode->hSocket == INVALID_SOCKET)
                    continue;
                {
                    TRY_LOCK(pnode->cs_vSend, lockSend);
                    if (lockSend) {
                        // do not read, if draining write queue
                        if (!pnode->vSendMsg.empty())
                            FD_SET(pnode->hSocket, &fdsetSend);
                        else
                            FD_SET(pnode->hSocket, &fdsetRecv);
                        FD_SET(pnode->hSocket, &fdsetError);
                        hSocketMax = max(hSocketMax, pnode->hSocket);
                        have_fds = true;
                    }
                }
            }
        }

        vnThreadsRunning[THREAD_SOCKETHANDLER]--;
        int nSelect = select(have_fds ? hSocketMax + 1 : 0,
                             &fdsetRecv, &fdsetSend, &fdsetError, &timeout);
        vnThreadsRunning[THREAD_SOCKETHANDLER]++;
        if (fShutdown)
            return;
        if (nSelect == SOCKET_ERROR)
        {
            if (have_fds)
            {
                int nErr = WSAGetLastError();
                printf("socket select error %d\n", nErr);
                for (unsigned int i = 0; i <= hSocketMax; i++)
                    FD_SET(i, &fdsetRecv);
            }
            FD_ZERO(&fdsetSend);
            FD_ZERO(&fdsetError);
            MilliSleep(timeout.tv_usec/1000);
        }


        //
        // Accept new connections
        //
        for (const ListenSocket& hListenSocket : vhListenSocket) {
            if (hListenSocket.socket != INVALID_SOCKET && FD_ISSET(hListenSocket.socket, &fdsetRecv)) {
                AcceptConnection(hListenSocket);
            }
        }


        //
        // Service each socket
        //
        vector<CNode*> vNodesCopy;
        {
            LOCK(cs_vNodes);
            vNodesCopy = vNodes;
            for (CNode* pnode : vNodesCopy)
                pnode->AddRef();
        }
        for (CNode* pnode : vNodesCopy)
        {
            if (fShutdown)
                return;

            //
            // Receive
            //
            if (pnode->hSocket == INVALID_SOCKET)
                continue;
            if (FD_ISSET(pnode->hSocket, &fdsetRecv) || FD_ISSET(pnode->hSocket, &fdsetError))
            {
                TRY_LOCK(pnode->cs_vRecvMsg, lockRecv);
                if (lockRecv)
                {
                    if (pnode->GetTotalRecvSize() > ReceiveFloodSize()) {
                        if (!pnode->fDisconnect)
                            if (fDebug)
                                printf("DEBUG-DISCONNECT recv-flood (%u bytes) peer=%s\n", pnode->GetTotalRecvSize(), pnode->addr.ToString().c_str());
                        pnode->CloseSocketDisconnect();
                    }
                    else {
                        // typical socket buffer is 8K-64K
                        char pchBuf[0x10000];
                        int nBytes = recv(pnode->hSocket, pchBuf, sizeof(pchBuf), MSG_DONTWAIT);
                        if (nBytes > 0)
                        {
                            if (!pnode->ReceiveMsgBytes(pchBuf, nBytes)) {
                                printf("DEBUG-DISCONNECT ReceiveMsgBytes failed peer=%s\n", pnode->addr.ToString().c_str());
                                pnode->CloseSocketDisconnect();
                            }
                            pnode->nLastRecv = GetTime();
                            pnode->nRecvBytes += nBytes;
                            pnode->RecordBytesRecv(nBytes);
                        }
                        else if (nBytes == 0)
                        {
                            // socket closed gracefully
                            if (!pnode->fDisconnect)
                                printf("DEBUG-DISCONNECT socket-closed-by-peer peer=%s\n", pnode->addr.ToString().c_str());
                            pnode->CloseSocketDisconnect();
                        }
                        else if (nBytes < 0)
                        {
                            // error
                            int nErr = WSAGetLastError();
                            if (nErr != WSAEWOULDBLOCK && nErr != WSAEMSGSIZE && nErr != WSAEINTR && nErr != WSAEINPROGRESS)
                            {
                                if (!pnode->fDisconnect)
                                    printf("DEBUG-DISCONNECT recv-error %d peer=%s\n", nErr, pnode->addr.ToString().c_str());
                                pnode->CloseSocketDisconnect();
                            }
                        }
                        else if (nBytes > 500000) // 500,000 Bytes
                        {
                            // error
                            int nErr = WSAGetLastError();
                            if (nErr != WSAEWOULDBLOCK && nErr != WSAEMSGSIZE && nErr != WSAEINTR && nErr != WSAEINPROGRESS)
                            {
                                if (!pnode->fDisconnect)
                                    printf("DEBUG-DISCONNECT output-too-large %d peer=%s\n", nErr, pnode->addr.ToString().c_str());
                                pnode->CloseSocketDisconnect();
                            }
                        }
                    }
                }
            }

            //
            // Send
            //
            if (pnode->hSocket == INVALID_SOCKET)
                continue;
            if (FD_ISSET(pnode->hSocket, &fdsetSend))
            {
                TRY_LOCK(pnode->cs_vSend, lockSend);
                if (lockSend)
                    SocketSendData(pnode);
            }

            //
            // Inactivity checking
            //
            int64_t nTime = GetTime();
            if (nTime - pnode->nTimeConnected > 60)
            {
                if (pnode->nLastRecv == 0 || pnode->nLastSend == 0)
                {
                    printf("socket no message in first 60 seconds, %d %d\n", pnode->nLastRecv != 0, pnode->nLastSend != 0);
                    pnode->fDisconnect = true;
                }
                else if (pnode->nVersion == 0 && nTime - pnode->nTimeConnected > 90)
                {
                    printf("socket handshake timeout: peer %s did not send version within 90s\n",
                           pnode->addr.ToString().c_str());
                    pnode->fDisconnect = true;
                }
                else if (nTime - pnode->nLastSend > TIMEOUT_INTERVAL)
                {
                    printf("socket sending timeout: %" PRId64"s\n", nTime - pnode->nLastSend);
                    pnode->fDisconnect = true;
                }
                else if (nTime - pnode->nLastRecv > (pnode->nVersion > BIP0031_VERSION ? TIMEOUT_INTERVAL : 90*60))
                {
                    printf("socket receive timeout: %" PRId64"s\n", nTime - pnode->nLastRecv);
                    pnode->fDisconnect = true;
                }
                else if (pnode->nPingNonceSent && pnode->nPingUsecStart + TIMEOUT_INTERVAL * 1000000 < GetTimeMicros())
                {
                    printf("ping timeout: %fs\n", 0.000001 * (GetTimeMicros() - pnode->nPingUsecStart));
                    pnode->fDisconnect = true;
                }

                if (!pnode->fDisconnect && pnode->nVersion != 0 &&
                    nTime - pnode->nLastRecv > 90 && nTime - pnode->nTimeConnected > 90)
                {
                    char probe = 0;
                    int nSent = send(pnode->hSocket, &probe, 0, MSG_DONTWAIT);
                    if (nSent < 0)
                    {
                        int nErr = WSAGetLastError();
                        if (nErr != WSAEWOULDBLOCK && nErr != WSAEINPROGRESS)
                        {
                            printf("keepalive: dead socket detected for %s (err=%d, silent=%ds), disconnecting\n",
                                   pnode->addr.ToString().c_str(), nErr, (int)(nTime - pnode->nLastRecv));
                            pnode->fDisconnect = true;
                        }
                    }
                }
            }
        }
        {
            LOCK(cs_vNodes);
            BOOST_FOREACH(CNode* pnode, vNodesCopy)
                pnode->Release();
        }

        MilliSleep(10);
    }
}









#ifdef USE_UPNP
void ThreadMapPort(void* parg)
{
    // Make this thread recognisable as the UPnP thread
    RenameThread("innova-UPnP");

    try
    {
        vnThreadsRunning[THREAD_UPNP]++;
        ThreadMapPort2(parg);
        vnThreadsRunning[THREAD_UPNP]--;
    }
    catch (std::exception& e) {
        vnThreadsRunning[THREAD_UPNP]--;
        PrintException(&e, "ThreadMapPort()");
    } catch (...) {
        vnThreadsRunning[THREAD_UPNP]--;
        PrintException(NULL, "ThreadMapPort()");
    }
    printf("ThreadMapPort exited\n");
}

void ThreadMapPort2(void* parg)
{
    printf("ThreadMapPort started\n");

    std::string port = strprintf("%u", GetListenPort());
    const char * multicastif = 0;
    const char * minissdpdpath = 0;
    struct UPNPDev * devlist = 0;
    char lanaddr[64];

#ifndef UPNPDISCOVER_SUCCESS
    /* miniupnpc 1.5 */
    devlist = upnpDiscover(2000, multicastif, minissdpdpath, 0);
#else
    #if MINIUPNPC_API_VERSION >= 14
        /* miniupnpc API_VERSION 14 and above requires ttl as arg */
        int error = 0;
        devlist = upnpDiscover(2000, multicastif, minissdpdpath, 0, 0, 2, &error);
    #else
        /* miniupnpc 1.6 and above */
        int error = 0;
        devlist = upnpDiscover(2000, multicastif, minissdpdpath, 0, 0, &error);
    #endif

#endif

    struct UPNPUrls urls;
    struct IGDdatas data;
    int r;
    char wanaddr[40] = "";

#if MINIUPNPC_API_VERSION >= 18
    r = UPNP_GetValidIGD(devlist, &urls, &data, lanaddr, sizeof(lanaddr), wanaddr, sizeof(wanaddr));
#else
    r = UPNP_GetValidIGD(devlist, &urls, &data, lanaddr, sizeof(lanaddr));
#endif
    if (r == 1)
    {
        if (fDiscover) {
            char externalIPAddress[40];
            r = UPNP_GetExternalIPAddress(urls.controlURL, data.first.servicetype, externalIPAddress);
            if(r != UPNPCOMMAND_SUCCESS)
                printf("UPnP: GetExternalIPAddress() returned %d\n", r);
            else
            {
                if(externalIPAddress[0])
                {
                    printf("UPnP: ExternalIPAddress = %s\n", externalIPAddress);
                    AddLocal(CNetAddr(externalIPAddress), LOCAL_UPNP);
                }
                else
                    printf("UPnP: GetExternalIPAddress failed.\n");
            }
        }

        string strDesc = "Innova " + FormatFullVersion();
#ifndef UPNPDISCOVER_SUCCESS
        /* miniupnpc 1.5 */
        r = UPNP_AddPortMapping(urls.controlURL, data.first.servicetype,
                            port.c_str(), port.c_str(), lanaddr, strDesc.c_str(), "TCP", 0);
#else
        /* miniupnpc 1.6 */
        r = UPNP_AddPortMapping(urls.controlURL, data.first.servicetype,
                            port.c_str(), port.c_str(), lanaddr, strDesc.c_str(), "TCP", 0, "0");
#endif

        if(r!=UPNPCOMMAND_SUCCESS)
            printf("AddPortMapping(%s, %s, %s) failed with code %d (%s)\n",
                port.c_str(), port.c_str(), lanaddr, r, strupnperror(r));
        else
            printf("UPnP Port Mapping successful.\n");
        int i = 1;
        while (true)
        {
            if (fShutdown || !fUseUPnP)
            {
                r = UPNP_DeletePortMapping(urls.controlURL, data.first.servicetype, port.c_str(), "TCP", 0);
                printf("UPNP_DeletePortMapping() returned : %d\n", r);
                freeUPNPDevlist(devlist); devlist = 0;
                FreeUPNPUrls(&urls);
                return;
            }
            if (i % 600 == 0) // Refresh every 20 minutes
            {
#ifndef UPNPDISCOVER_SUCCESS
                /* miniupnpc 1.5 */
                r = UPNP_AddPortMapping(urls.controlURL, data.first.servicetype,
                                    port.c_str(), port.c_str(), lanaddr, strDesc.c_str(), "TCP", 0);
#else
                /* miniupnpc 1.6 */
                r = UPNP_AddPortMapping(urls.controlURL, data.first.servicetype,
                                    port.c_str(), port.c_str(), lanaddr, strDesc.c_str(), "TCP", 0, "0");
#endif

                if(r!=UPNPCOMMAND_SUCCESS)
                    printf("AddPortMapping(%s, %s, %s) failed with code %d (%s)\n",
                        port.c_str(), port.c_str(), lanaddr, r, strupnperror(r));
                else
                    printf("UPnP Port Mapping successful.\n");;
            }
            MilliSleep(2000);
            i++;
        }
    } else {
        printf("No valid UPnP IGDs found\n");
        freeUPNPDevlist(devlist); devlist = 0;
        if (r != 0)
            FreeUPNPUrls(&urls);
        while (true)
        {
            if (fShutdown || !fUseUPnP)
                return;
            MilliSleep(2000);
        }
    }
}

void MapPort()
{
    if (fUseUPnP && vnThreadsRunning[THREAD_UPNP] < 1)
    {
        if (!NewThread(ThreadMapPort, NULL))
            printf("Error: ThreadMapPort(ThreadMapPort) failed\n");
    }
}
#else
void MapPort()
{
    // Intentionally left blank.
}
#endif



/* Tor implementation ---------------------------------*/

// hidden service seeds
static const char *strMainNetOnionSeed[][1] = {
    {NULL}
};

static const char *strTestNetOnionSeed[][1] = {
    {NULL}
};

#ifdef USE_NATIVETOR
void ThreadOnionSeed(void* parg)
{
    if(fNativeTor)
    {
        // Make this thread recognisable as the Tor Onion Thread
        RenameThread("toronion");

        static const char *(*strOnionSeed)[1] = fTestNet ? strTestNetOnionSeed : strMainNetOnionSeed;

        int found = 0;
        printf("Loading addresses from .onion seeds\n");

        for (unsigned int seed_idx = 0; strOnionSeed[seed_idx][0] != NULL; seed_idx++) {
            CNetAddr parsed;
            if (!parsed.SetSpecial(strOnionSeed[seed_idx][0]))
            {
                throw runtime_error("ThreadOnionSeed() : invalid .onion seed");
            }
            int nOneDay = 24*3600;
            CAddress addr = CAddress(CService(parsed, GetDefaultPort()));
            addr.nTime = GetTime() - 3*nOneDay - GetRand(4*nOneDay); // use a random age between 3 and 7 days old
            found++;
            addrman.Add(addr, parsed);
        }

        printf("%d addresses found from .onion seeds\n", found);
    }
};
#endif




// DNS seeds
// Each pair gives a source name and a seed name.
// The first name is used as information source for addrman.
// The second name should resolve to a list of seed addresses.

static const char *strDNSSeed[][2] = {
    {"45.77.164.87", "45.77.164.87"},
    {"165.22.181.170", "165.22.181.170"},
    {"159.223.114.4", "159.223.114.4"},
    {"165.22.187.43", "165.22.187.43"},
    {"157.245.139.16", "157.245.139.16"},
    {"167.71.19.57", "167.71.19.57"},
    {"68.183.110.135", "68.183.110.135"},
    {"165.227.206.77:14539", "165.227.206.77:14539"},
    {"159.223.100.10:14539", "159.223.100.10:14539"},
    {"159.223.104.144:14539", "159.223.104.144:14539"},
    {"159.223.104.83:14539", "159.223.104.83:14539"}
};

static const char *strDNSSeedTestnet[][2] = {
    {"45.32.161.27", "45.32.161.27"},
    {"45.77.164.87", "45.77.164.87"},
    {"45.77.118.217", "45.77.118.217"},
    {"45.32.168.101", "45.32.168.101"},
    {"144.202.37.36", "144.202.37.36"}
};


void ThreadDNSAddressSeed(void* parg)
{
    if(!fNativeTor)
    {
        // Make this thread recognisable as the DNS seeding thread
        RenameThread("innova-dnsseed");

        try
        {
            vnThreadsRunning[THREAD_DNSSEED]++;
            ThreadDNSAddressSeed2(parg);
            vnThreadsRunning[THREAD_DNSSEED]--;
        }
        catch (std::exception& e) {
            vnThreadsRunning[THREAD_DNSSEED]--;
            PrintException(&e, "ThreadDNSAddressSeed()");
        } catch (...) {
            vnThreadsRunning[THREAD_DNSSEED]--;
            throw; // support pthread_cancel()
        }
        printf("ThreadDNSAddressSeed exited\n");
    }
};

void ThreadDNSAddressSeed2(void* parg)
{
    if(!fNativeTor)
    {
        printf("ThreadDNSAddressSeed started\n");
        int found = 0;

        {
            const char *(*seeds)[2];
            unsigned int nSeeds;
            if (fTestNet) {
                seeds = strDNSSeedTestnet;
                nSeeds = ARRAYLEN(strDNSSeedTestnet);
            } else {
                seeds = strDNSSeed;
                nSeeds = ARRAYLEN(strDNSSeed);
            }

            printf("Loading addresses from DNS seeds (could take a while)\n");

            for (unsigned int seed_idx = 0; seed_idx < nSeeds; seed_idx++) {
                if (HaveNameProxy()) {
                    AddOneShot(seeds[seed_idx][1]);
                } else {
                    vector<CNetAddr> vaddr;
                    vector<CAddress> vAdd;
                    if (LookupHost(seeds[seed_idx][1], vaddr))
                    {
                        for (CNetAddr& ip : vaddr)
                        {
                            int nOneDay = 24*3600;
                            CAddress addr = CAddress(CService(ip, GetDefaultPort()));
                            addr.nTime = GetTime() - 3*nOneDay - GetRand(4*nOneDay);
                            vAdd.push_back(addr);
                            found++;
                        }
                    }
                    addrman.Add(vAdd, CNetAddr(seeds[seed_idx][0], true));
                }
            }
        }

        printf("%d addresses found from DNS seeds\n", found);
    }
};









unsigned int pnSeed[] =
{
    0x42ac0c50,
};



void DumpAddresses()
{
    int64_t nStart = GetTimeMillis();

    CAddrDB adb;
    adb.Write(addrman);

    printf("Flushed %d addresses to peers.dat  %" PRId64"ms\n",
           addrman.size(), GetTimeMillis() - nStart);
}

// Dump the peers.dat and banlist.dat together
void DumpData()
{
    DumpAddresses();

    if (CNode::BannedSetIsDirty())
        printf("Banned set is dirty, writing banlist...\n");
        DumpBanlist();

}

void ThreadDumpAddress2(void* parg)
{
    vnThreadsRunning[THREAD_DUMPADDRESS]++;
    while (!fShutdown)
    {
        DumpData();
        vnThreadsRunning[THREAD_DUMPADDRESS]--;
        MilliSleep(600000);
        vnThreadsRunning[THREAD_DUMPADDRESS]++;
    }
    vnThreadsRunning[THREAD_DUMPADDRESS]--;
}

void ThreadDumpAddress(void* parg)
{
    // Make this thread recognisable as the address dumping thread
    RenameThread("innova-adrdump");

    try
    {
        ThreadDumpAddress2(parg);
    }
    catch (std::exception& e) {
        PrintException(&e, "ThreadDumpAddress()");
    }
    printf("ThreadDumpAddress exited\n");
}

void ThreadOpenConnections(void* parg)
{
    // Make this thread recognisable as the connection opening thread
    RenameThread("innova-opencon");

    try
    {
        vnThreadsRunning[THREAD_OPENCONNECTIONS]++;
        ThreadOpenConnections2(parg);
        vnThreadsRunning[THREAD_OPENCONNECTIONS]--;
    }
    catch (std::exception& e) {
        vnThreadsRunning[THREAD_OPENCONNECTIONS]--;
        PrintException(&e, "ThreadOpenConnections()");
    } catch (...) {
        vnThreadsRunning[THREAD_OPENCONNECTIONS]--;
        PrintException(NULL, "ThreadOpenConnections()");
    }
    printf("ThreadOpenConnections exited\n");
}

void static ProcessOneShot()
{
    string strDest;
    {
        LOCK(cs_vOneShots);
        if (vOneShots.empty())
            return;
        strDest = vOneShots.front();
        vOneShots.pop_front();
    }
    CAddress addr;
    CSemaphoreGrant grant(*semOutbound, true);
    if (grant) {
        if (!OpenNetworkConnection(addr, &grant, strDest.c_str(), true))
            AddOneShot(strDest);
    }
}

void static ThreadStakeMiner(void* parg)
{
    printf("ThreadStakeMiner started\n");
    CWallet* pwallet = (CWallet*)parg;
    try
    {
        vnThreadsRunning[THREAD_STAKE_MINER]++;
        StakeMiner(pwallet);
        vnThreadsRunning[THREAD_STAKE_MINER]--;
    }
    catch (std::exception& e) {
        vnThreadsRunning[THREAD_STAKE_MINER]--;
        PrintException(&e, "ThreadStakeMiner()");
    } catch (...) {
        vnThreadsRunning[THREAD_STAKE_MINER]--;
        PrintException(NULL, "ThreadStakeMiner()");
    }
    printf("ThreadStakeMiner exiting, %d threads remaining\n", vnThreadsRunning[THREAD_STAKE_MINER]);
}

void ThreadOpenConnections2(void* parg)
{
    printf("ThreadOpenConnections started\n");

    // Connect to specific addresses
    if (mapArgs.count("-connect") && mapMultiArgs["-connect"].size() > 0)
    {
        for (int64_t nLoop = 0;; nLoop++)
        {
            ProcessOneShot();
            BOOST_FOREACH(string strAddr, mapMultiArgs["-connect"])
            {
                CAddress addr;
                OpenNetworkConnection(addr, NULL, strAddr.c_str());
                for (int i = 0; i < 10 && i < nLoop; i++)
                {
                    MilliSleep(500);
                    if (fShutdown)
                        return;
                }
            }
            MilliSleep(500);
        }
    }

    // Initiate network connections
    int64_t nStart = GetTime();
    while (true)
    {
        ProcessOneShot();

        vnThreadsRunning[THREAD_OPENCONNECTIONS]--;
        MilliSleep(500);
        vnThreadsRunning[THREAD_OPENCONNECTIONS]++;
        if (fShutdown)
            return;


        vnThreadsRunning[THREAD_OPENCONNECTIONS]--;
        CSemaphoreGrant grant(*semOutbound);
        vnThreadsRunning[THREAD_OPENCONNECTIONS]++;
        if (fShutdown)
            return;

        // Add seed nodes if IRC isn't working
        if (addrman.size()==0 && (GetTime() - nStart > 60) && !fTestNet)
        {
            std::vector<CAddress> vAdd;
            for (unsigned int i = 0; i < ARRAYLEN(pnSeed); i++)
            {
                // It'll only connect to one or two seed nodes because once it connects,
                // it'll get a pile of addresses with newer timestamps.
                // Seed nodes are given a random 'last seen time' of between one and two
                // weeks ago.
                const int64_t nOneWeek = 7*24*60*60;
                struct in_addr ip;
                memcpy(&ip, &pnSeed[i], sizeof(ip));
                CAddress addr(CService(ip, GetDefaultPort()));
                addr.nTime = GetTime()-GetRand(nOneWeek)-nOneWeek;
                vAdd.push_back(addr);
            }
            addrman.Add(vAdd, CNetAddr("127.0.0.1"));
        }

        //
        // Choose an address to connect to based on most recently seen
        //
        CAddress addrConnect;

        // Only connect out to one peer per network group (/16 for IPv4).
        // Do this here so we don't have to critsect vNodes inside mapAddresses critsect.
        int nOutbound = 0;
        set<vector<unsigned char> > setConnected;
        {
            LOCK(cs_vNodes);
            for (CNode* pnode : vNodes) {
                if (!pnode->fInbound) {
                    setConnected.insert(pnode->addr.GetGroup());
                    nOutbound++;
                }
            }
        }

        int64_t nANow = GetAdjustedTime();

        if (IsInitialBlockDownload())
        {
            int nCNSyncSlots = GetArg("-cnsyncslots", 4);
            int nCNConnected = 0;
            {
                LOCK(cs_vNodes);
                for (CNode* pnode : vNodes)
                {
                    if (pnode->fColLateralMaster && !pnode->fInbound)
                        nCNConnected++;
                }
            }

            if (nCNConnected < nCNSyncSlots)
            {
                // Try to connect to a known collateral node for faster sync
                LOCK(cs_collateralnodes);
                for (CCollateralNode& mn : vecCollateralnodes)
                {
                    if (!mn.IsEnabled())
                        continue;
                    if (setConnected.count(mn.addr.GetGroup()))
                        continue;

                    addrConnect = CAddress(mn.addr);
                    if (addrConnect.IsValid())
                    {
                        if (fDebug)
                            printf("CN-assisted sync: connecting to collateral node %s\n", addrConnect.ToString().c_str());
                        OpenNetworkConnection(addrConnect, &grant, NULL, false);
                        break;
                    }
                }
                // If we connected to a CN, continue the outer loop
                if (addrConnect.IsValid())
                    continue;
            }
        }

        int nTries = 0;
        while (true)
        {
            // use an nUnkBias between 10 (no outgoing connections) and 90 (8 outgoing connections)
            CAddress addr = addrman.Select(10 + min(nOutbound,8)*10);

            // if we selected an invalid address, restart
            if (!addr.IsValid() || setConnected.count(addr.GetGroup()) || IsLocal(addr))
                break;

            // If we didn't find an appropriate destination after trying 100 addresses fetched from addrman,
            // stop this loop, and let the outer loop run again (which sleeps, adds seed nodes, recalculates
            // already-connected network ranges, ...) before trying new addrman addresses.
            nTries++;
            if (nTries > 100)
                break;

            if (IsLimited(addr))
                continue;

            // only consider very recently tried nodes after 30 failed attempts
            if (nANow - addr.nLastTry < 600 && nTries < 30)
                continue;

            // do not allow non-default ports, unless after 50 invalid addresses selected already
            if (addr.GetPort() != GetDefaultPort() && nTries < 50)
                continue;

            addrConnect = addr;
            break;
        }

        if (addrConnect.IsValid())
            OpenNetworkConnection(addrConnect, &grant);
    }
}

void ThreadOpenAddedConnections(void* parg)
{
    // Make this thread recognisable as the connection opening thread
    RenameThread("innova-opencon");

    try
    {
        vnThreadsRunning[THREAD_ADDEDCONNECTIONS]++;
        ThreadOpenAddedConnections2(parg);
        vnThreadsRunning[THREAD_ADDEDCONNECTIONS]--;
    }
    catch (std::exception& e) {
        vnThreadsRunning[THREAD_ADDEDCONNECTIONS]--;
        PrintException(&e, "ThreadOpenAddedConnections()");
    } catch (...) {
        vnThreadsRunning[THREAD_ADDEDCONNECTIONS]--;
        PrintException(NULL, "ThreadOpenAddedConnections()");
    }
    printf("ThreadOpenAddedConnections exited\n");
}

void ThreadOpenAddedConnections2(void* parg)
{
    {
        LOCK(cs_vAddedNodes);
        vAddedNodes = mapMultiArgs["-addnode"];
    }

    if (HaveNameProxy()) {
        while(true) {
            list<string> lAddresses(0);
            {
                LOCK(cs_vAddedNodes);
                for (string& strAddNode : vAddedNodes)
                    lAddresses.push_back(strAddNode);
            }
            for (string& strAddNode : lAddresses) {
                CAddress addr;
                CSemaphoreGrant grant(*semOutbound);
                OpenNetworkConnection(addr, &grant, strAddNode.c_str());
                MilliSleep(500);
            }
			if (fShutdown)
                return;
            vnThreadsRunning[THREAD_ADDEDCONNECTIONS]--;
            MilliSleep(120000); // Retry every 2 minutes
			vnThreadsRunning[THREAD_ADDEDCONNECTIONS]++;
            if (fShutdown)
                return;
        }
    }

    for (unsigned int i = 0; true; i++)
    {
        list<string> lAddresses(0);
        {
            LOCK(cs_vAddedNodes);
            for (string& strAddNode : vAddedNodes)
                lAddresses.push_back(strAddNode);
        }

        list<vector<CService> > lservAddressesToAdd(0);
        for (string& strAddNode : lAddresses)
        {
            vector<CService> vservNode(0);
            if(Lookup(strAddNode.c_str(), vservNode, GetDefaultPort(), fNameLookup, 0))
            {
                lservAddressesToAdd.push_back(vservNode);
                {
                    LOCK(cs_setservAddNodeAddresses);
                    for (CService& serv : vservNode)
                        setservAddNodeAddresses.insert(serv);
                }
            }
        }
        // Attempt to connect to each IP for each addnode entry until at least one is successful per addnode entry
        // (keeping in mind that addnode entries can have many IPs if fNameLookup)
        {
            LOCK(cs_vNodes);
            for (CNode* pnode : vNodes)
                for (list<vector<CService> >::iterator it = lservAddressesToAdd.begin(); it != lservAddressesToAdd.end(); it++)
                    for (CService& addrNode : *(it))
                        if (pnode->addr == addrNode)
                        {
                            it = lservAddressesToAdd.erase(it);
                            it--;
                            break;
                        }
        }
        for (vector<CService>& vserv : lservAddressesToAdd)
        {
            CSemaphoreGrant grant(*semOutbound);
            OpenNetworkConnection(CAddress(vserv[i % vserv.size()]), &grant);
            MilliSleep(500);
			if (fShutdown)
                return;
        }
		vnThreadsRunning[THREAD_ADDEDCONNECTIONS]--;
        MilliSleep(120000); // Retry every 2 minutes
        vnThreadsRunning[THREAD_ADDEDCONNECTIONS]++;
        if (fShutdown)
            return;
    }
}

// if successful, this moves the passed grant to the constructed node
bool OpenNetworkConnection(const CAddress& addrConnect, CSemaphoreGrant *grantOutbound, const char *strDest, bool fOneShot)
{
    //
    // Initiate outbound network connection
    //
    if (fShutdown)
        return false;
    if (!strDest) {
        if (IsLocal(addrConnect) ||
            FindNode((CNetAddr) addrConnect) || CNode::IsBanned(addrConnect) ||
            FindNode(addrConnect.ToStringIPPort()))
            return false;
    } else if (FindNode(strDest))
        return false;

    vnThreadsRunning[THREAD_OPENCONNECTIONS]--;
    CNode* pnode = ConnectNode(addrConnect, strDest);
    vnThreadsRunning[THREAD_OPENCONNECTIONS]++;
    if (fShutdown)
        return false;
    if (!pnode)
        return false;
    if (grantOutbound)
        grantOutbound->MoveTo(pnode->grantOutbound);
    pnode->fNetworkNode = true;
    if (fOneShot)
        pnode->fOneShot = true;

    return true;
}

// Open a network connection without one-shot semantics (used by collateralnode auto-connect)
bool OpenNetworkConnectionSimple(const CAddress& addrConnect, const char *strDest)
{
    if (fShutdown)
        return false;
    if (!strDest) {
        if (IsLocal(addrConnect) ||
            FindNode((CNetAddr) addrConnect) || CNode::IsBanned(addrConnect) ||
            FindNode(addrConnect.ToStringIPPort()))
            return false;
    } else if (FindNode(strDest))
        return false;

    CNode* pnode = ConnectNode(addrConnect, strDest);
    if (!pnode)
        return false;
    pnode->fNetworkNode = true;
    return true;
}



static bool NodeVectorContains(const vector<CNode*>& vNodesIn,
                               const CNode* pnodeFind)
{
    return pnodeFind != NULL &&
           std::find(vNodesIn.begin(), vNodesIn.end(), pnodeFind) !=
               vNodesIn.end();
}

static CNode* GetValidSyncPeer(const vector<CNode*>& vNodesIn)
{
    LOCK(cs_pnodeSync);
    if (!NodeVectorContains(vNodesIn, pnodeSync))
        pnodeSync = NULL;
    return pnodeSync;
}

static CNode* AssignSyncPeer(const vector<CNode*>& vNodesIn,
                             CNode* pnodeNewSync,
                             bool fQueueInitialSync)
{
    CNode* pnodeOldSync = NULL;
    bool fSyncPeerChanged = false;
    {
        LOCK(cs_pnodeSync);
        pnodeOldSync =
            NodeVectorContains(vNodesIn, pnodeSync) ? pnodeSync : NULL;
        fSyncPeerChanged = pnodeOldSync != pnodeNewSync;
        if (fSyncPeerChanged)
        {
            if (pnodeOldSync != NULL)
                pnodeOldSync->fStartSync = false;
            pnodeSync = pnodeNewSync;
            if (pnodeNewSync != NULL && fQueueInitialSync)
                pnodeNewSync->fStartSync = true;
        }
        else
        {
            pnodeSync = pnodeNewSync;
        }
    }

    if (fSyncPeerChanged)
    {
        int64_t nStallStart = 0;
        {
            LOCK(cs_stalledSyncRecovery);
            nStallStart = stalledSyncRecoveryState.LastProgressTime();
        }
        const int64_t nNow = GetTime();
        const int64_t nStallAge = nStallStart == 0
            ? -1 : std::max<int64_t>(0, nNow - nStallStart);
        printf("SYNCPEER_RECOVERY_STATE time=%lld old_peer=%lld new_peer=%lld stall_timer_preserved=1 old_stall_age=%lld new_stall_age=%lld reset_reason=none\n",
               (long long)nNow,
               (long long)(pnodeOldSync == NULL
                               ? -1 : pnodeOldSync->GetId()),
               (long long)(pnodeNewSync == NULL
                               ? -1 : pnodeNewSync->GetId()),
               (long long)nStallAge, (long long)nStallAge);
    }
    return pnodeOldSync;
}

void ResetSyncPeerForTesting()
{
    LOCK(cs_pnodeSync);
    pnodeSync = NULL;
}

void static StartSync(const vector<CNode*> &vNodesIn)
{
    CNode* pnodeNewSync = NULL;
    int64_t nBestScore = 0;
    int64_t nMaxPeerHeight = -1;

    // fImporting and fReindex are checked again in SendMessages.
    if (fImporting || fReindex || fSPVMode)
        return;

    int nLocalHeight = -1;
    {
        TRY_LOCK(cs_main, lockMain);
        if (!lockMain)
            return;
        nLocalHeight = nBestHeight;
    }

    BOOST_FOREACH(CNode* pnode, vNodesIn)
    {
        if (pnode->fClient || pnode->fOneShot || pnode->fDisconnect ||
            !pnode->fSuccessfullyConnected ||
            !IsBlockSyncPeerVersion(pnode->nVersion) ||
            !pnode->CanAdvanceBlockSync(nLocalHeight))
        {
            continue;
        }
        nMaxPeerHeight =
            std::max(nMaxPeerHeight, GetPeerAdvertisedHeight(pnode));
    }

    std::vector<std::pair<int64_t, CNode*> > vCandidates;
    BOOST_FOREACH(CNode* pnode, vNodesIn)
    {
        if (pnode->fClient || pnode->fOneShot || pnode->fDisconnect ||
            !pnode->fSuccessfullyConnected ||
            !IsBlockSyncPeerVersion(pnode->nVersion) ||
            !pnode->CanAdvanceBlockSync(nLocalHeight))
        {
            continue;
        }

        const int64_t nScore =
            SyncPeerScore(pnode, GetTime(), nMaxPeerHeight);
        vCandidates.push_back(std::make_pair(nScore, pnode));
        if (pnodeNewSync == NULL || nScore > nBestScore ||
            (nScore == nBestScore &&
             pnode->GetId() < pnodeNewSync->GetId()))
        {
            pnodeNewSync = pnode;
            nBestScore = nScore;
        }
    }
    std::sort(vCandidates.begin(), vCandidates.end(),
              CompareSyncCandidates);

    CNode* pnodeOldSync =
        AssignSyncPeer(vNodesIn, pnodeNewSync, true);

    if (pnodeOldSync != pnodeNewSync)
    {
        std::ostringstream oss;
        oss << "SYNCPEER_SELECT: old="
            << (pnodeOldSync
                    ? DescribePeerForDiagnostics(pnodeOldSync)
                    : std::string("none"))
            << " new="
            << (pnodeNewSync
                    ? DescribePeerForDiagnostics(pnodeNewSync)
                    : std::string("none"))
            << " max_peer_height=" << (long long)nMaxPeerHeight
            << " candidates=" << vCandidates.size();
        if (pnodeNewSync)
            oss << " score=" << (long long)nBestScore;
        if (!vCandidates.empty())
        {
            oss << " top=";
            const size_t nLimit =
                std::min<size_t>(vCandidates.size(), 4);
            for (size_t i = 0; i < nLimit; ++i)
            {
                if (i != 0)
                    oss << ',';
                oss << '[' << (long long)vCandidates[i].first << ':'
                    << DescribePeerForDiagnostics(
                           vCandidates[i].second)
                    << ']';
            }
        }
        printf("%s\n", oss.str().c_str());
    }
}

void StartSyncForTesting(const vector<CNode*>& vNodesIn)
{
    StartSync(vNodesIn);
}

void ThreadMessageHandler(void* parg)
{
    // Make this thread recognisable as the message handling thread
    RenameThread("innova-msghand");

    try
    {
        vnThreadsRunning[THREAD_MESSAGEHANDLER]++;
        ThreadMessageHandler2(parg);
        vnThreadsRunning[THREAD_MESSAGEHANDLER]--;
    }
    catch (std::exception& e) {
        vnThreadsRunning[THREAD_MESSAGEHANDLER]--;
        PrintException(&e, "ThreadMessageHandler()");
    } catch (...) {
        vnThreadsRunning[THREAD_MESSAGEHANDLER]--;
        PrintException(NULL, "ThreadMessageHandler()");
    }
    printf("ThreadMessageHandler exited\n");
}

void ThreadMessageHandler2(void* parg)
{
    printf("ThreadMessageHandler started\n");
    SetThreadPriority(THREAD_PRIORITY_BELOW_NORMAL);
    while (!fShutdown)
    {
        bool fHaveSyncNode = false;
        vector<CNode*> vNodesCopy;
        {
            LOCK(cs_vNodes);
            vNodesCopy = vNodes;
            BOOST_FOREACH(CNode* pnode, vNodesCopy)
                pnode->AddRef();
        }

        CNode* pnodeSyncCopy = GetValidSyncPeer(vNodesCopy);
        if (pnodeSyncCopy != NULL && !pnodeSyncCopy->fDisconnect &&
            pnodeSyncCopy->fSuccessfullyConnected &&
            pnodeSyncCopy->nLastRecv > GetTime() - 5)
        {
            fHaveSyncNode = true;
        }

        if (!fHaveSyncNode)
            StartSync(vNodesCopy);
        bool fSleep = true;

        // Poll the connected nodes for messages
        CNode* pnodeTrickle = NULL;
        if (!vNodesCopy.empty())
            pnodeTrickle = vNodesCopy[GetRand(vNodesCopy.size())];
        BOOST_FOREACH(CNode* pnode, vNodesCopy)
        {
            if (pnode->fDisconnect)
                continue;

            // Receive messages
            {
                TRY_LOCK(pnode->cs_vRecvMsg, lockRecv);
                if (lockRecv)
                {
                    if (!ProcessMessages(pnode))
                        pnode->CloseSocketDisconnect();

                    if (pnode->nSendSize < SendBufferSize()) {
                        if (!pnode->vRecvGetData.empty() || (!pnode->vRecvMsg.empty() && pnode->vRecvMsg[0].complete())) {
                            fSleep = false; // no sleep for the weak
                        }
                    }
                }
            }

            boost::this_thread::interruption_point();

            if (fShutdown)
                return;

            // Send messages
            {
                TRY_LOCK(pnode->cs_vSend, lockSend);
                if (lockSend)
                    SendMessages(pnode, pnode == pnodeTrickle);
            }
            if (fShutdown)
                return;
        }

        if (!fImporting && !fReindex && !fSPVMode)
        {
            const int64_t nNow = GetTime();
            BOOST_FOREACH(CNode* pnode, vNodesCopy)
                pnode->ExpireBlockInFlight(nNow);

            CBlockIndex* pindexTip = NULL;
            int nLocalHeight = -1;
            bool fHaveChainState = false;
            {
                TRY_LOCK(cs_main, lockMain);
                if (lockMain)
                {
                    pindexTip = pindexBest;
                    nLocalHeight = nBestHeight;
                    fHaveChainState = true;
                }
            }

            if (fHaveChainState)
            {
                CNode* pnodeRecovery = NULL;
                {
                    LOCK(cs_stalledSyncRecovery);
                    pnodeRecovery = MaybeQueueStalledSyncRecovery(
                        vNodesCopy, pindexTip, nLocalHeight, nNow,
                        std::max<int64_t>(
                            5, GetArg("-syncstalltimeout", 15)),
                        std::max<int64_t>(
                            5, GetArg("-syncstallcooldown", 15)),
                        stalledSyncRecoveryState);
                }
                if (pnodeRecovery != NULL)
                    AssignSyncPeer(vNodesCopy, pnodeRecovery, false);
            }
        }
        UpdateGetInfoSyncProbeSnapshot(vNodesCopy);

        {
            LOCK(cs_vNodes);
            BOOST_FOREACH(CNode* pnode, vNodesCopy)
                pnode->Release();
        }

        // Wait and allow messages to bunch up.
        // Reduce vnThreadsRunning so StopNode has permission to exit while
        // we're sleeping, but we must always check fShutdown after doing this.
        vnThreadsRunning[THREAD_MESSAGEHANDLER]--;
        if (fSleep)
            MilliSleep(100);
        LogSyncDiagnosticsMaybe();

        if (fRequestShutdown)
            StartShutdown();
        vnThreadsRunning[THREAD_MESSAGEHANDLER]++;
        if (fShutdown)
            return;
    }
}






bool BindListenPort(const CService &addrBind, string& strError, bool fWhitelisted)
{
    strError = "";
    int nOne = 1;

#ifdef WIN32
    // Initialize Windows Sockets
    WSADATA wsadata;
    int ret = WSAStartup(MAKEWORD(2,2), &wsadata);
    if (ret != NO_ERROR)
    {
        strError = strprintf("Error: TCP/IP socket library failed to start (WSAStartup returned error %d)", ret);
        printf("%s\n", strError.c_str());
        return false;
    }
#endif

    // Create socket for listening for incoming connections
    struct sockaddr_storage sockaddr;
    socklen_t len = sizeof(sockaddr);
    if (!addrBind.GetSockAddr((struct sockaddr*)&sockaddr, &len))
    {
        strError = strprintf("Error: bind address family for %s not supported", addrBind.ToString().c_str());
        printf("%s\n", strError.c_str());
        return false;
    }

    SOCKET hListenSocket = socket(((struct sockaddr*)&sockaddr)->sa_family, SOCK_STREAM, IPPROTO_TCP);
    if (hListenSocket == INVALID_SOCKET)
    {
        strError = strprintf("Error: Couldn't open socket for incoming connections (socket returned error %d)", WSAGetLastError());
        printf("%s\n", strError.c_str());
        return false;
    }
    if (!IsSelectableSocket(hListenSocket))
    {
        strError = "Error: Couldn't create a listenable socket for incoming connections";
        printf("%s\n", strError.c_str());
        return false;
    }

#ifdef SO_NOSIGPIPE
    // Different way of disabling SIGPIPE on BSD
    setsockopt(hListenSocket, SOL_SOCKET, SO_NOSIGPIPE, (void*)&nOne, sizeof(int));
#endif

#ifndef WIN32
    // Allow binding if the port is still in TIME_WAIT state after
    // the program was closed and restarted.  Not an issue on windows.
    setsockopt(hListenSocket, SOL_SOCKET, SO_REUSEADDR, (void*)&nOne, sizeof(int));
#endif


#ifdef WIN32
    // Set to non-blocking, incoming connections will also inherit this
    if (ioctlsocket(hListenSocket, FIONBIO, (u_long*)&nOne) == SOCKET_ERROR)
#else
    if (fcntl(hListenSocket, F_SETFL, O_NONBLOCK) == SOCKET_ERROR)
#endif
    {
        strError = strprintf("Error: Couldn't set properties on socket for incoming connections (error %d)", WSAGetLastError());
        printf("%s\n", strError.c_str());
        return false;
    }

    // some systems don't have IPV6_V6ONLY but are always v6only; others do have the option
    // and enable it by default or not. Try to enable it, if possible.
    if (addrBind.IsIPv6()) {
#ifdef IPV6_V6ONLY
#ifdef WIN32
        setsockopt(hListenSocket, IPPROTO_IPV6, IPV6_V6ONLY, (const char*)&nOne, sizeof(int));
#else
        setsockopt(hListenSocket, IPPROTO_IPV6, IPV6_V6ONLY, (void*)&nOne, sizeof(int));
#endif
#endif
#ifdef WIN32
        int nProtLevel = 10 /* PROTECTION_LEVEL_UNRESTRICTED */;
        int nParameterId = 23 /* IPV6_PROTECTION_LEVEl */;
        // this call is allowed to fail
        setsockopt(hListenSocket, IPPROTO_IPV6, nParameterId, (const char*)&nProtLevel, sizeof(int));
#endif
    }

    if (::bind(hListenSocket, (struct sockaddr*)&sockaddr, len) == SOCKET_ERROR)
    {
        int nErr = WSAGetLastError();
        if (nErr == WSAEADDRINUSE)
            strError = strprintf(_("Unable to bind to %s on this computer. Innova is probably already running."), addrBind.ToString().c_str());
        else
            strError = strprintf(_("Unable to bind to %s on this computer (bind returned error %d, %s)"), addrBind.ToString().c_str(), nErr, strerror(nErr));
        printf("%s\n", strError.c_str());
        return false;
    }
    printf("Bound to %s\n", addrBind.ToString().c_str());

    // Listen for incoming connections
    if (listen(hListenSocket, SOMAXCONN) == SOCKET_ERROR)
    {
        strError = strprintf("Error: Listening for incoming connections failed (listen returned error %d)", WSAGetLastError());
        printf("%s\n", strError.c_str());
        return false;
    }

    vhListenSocket.push_back(ListenSocket(hListenSocket, fWhitelisted));

    if (addrBind.IsRoutable() && fDiscover && !fWhitelisted)
        AddLocal(addrBind, LOCAL_BIND);

    return true;
}


void static Discover()
{
    if (!fDiscover || fNativeTor)
        return;

#ifdef WIN32
    // Get local host IP
    char pszHostName[1000] = "";
    if (gethostname(pszHostName, sizeof(pszHostName)) != SOCKET_ERROR)
    {
        vector<CNetAddr> vaddr;
        if (LookupHost(pszHostName, vaddr))
        {
            BOOST_FOREACH (const CNetAddr &addr, vaddr)
            {
                AddLocal(addr, LOCAL_IF);
            }
        }
    }
#else
    // Get local host ip
    struct ifaddrs* myaddrs;
    if (getifaddrs(&myaddrs) == 0)
    {
        for (struct ifaddrs* ifa = myaddrs; ifa != NULL; ifa = ifa->ifa_next)
        {
            if (ifa->ifa_addr == NULL) continue;
            if ((ifa->ifa_flags & IFF_UP) == 0) continue;
            if (strcmp(ifa->ifa_name, "lo") == 0) continue;
            if (strcmp(ifa->ifa_name, "lo0") == 0) continue;
            if (ifa->ifa_addr->sa_family == AF_INET)
            {
                struct sockaddr_in* s4 = (struct sockaddr_in*)(ifa->ifa_addr);
                CNetAddr addr(s4->sin_addr);
                if (AddLocal(addr, LOCAL_IF))
                    printf("IPv4 %s: %s\n", ifa->ifa_name, addr.ToString().c_str());
            }
            else if (ifa->ifa_addr->sa_family == AF_INET6)
            {
                struct sockaddr_in6* s6 = (struct sockaddr_in6*)(ifa->ifa_addr);
                CNetAddr addr(s6->sin6_addr);
                if (AddLocal(addr, LOCAL_IF))
                    printf("IPv6 %s: %s\n", ifa->ifa_name, addr.ToString().c_str());
            }
        }
        freeifaddrs(myaddrs);
    }
#endif

    // Don't use external IPv4 discovery, when -onlynet="IPv6"
    if (!IsLimited(NET_IPV4))
        NewThread(ThreadGetMyExternalIP, NULL);
}

static char *convert_str(const std::string &s) {
    size_t len = s.size() + 1;
    char *pc = new char[len];
    std::strncpy(pc, s.c_str(), len);
    pc[len - 1] = '\0';
    return pc;
}

#ifdef USE_NATIVETOR
// Start Tor Threads
static void run_tor() {
  if(fNativeTor)
  {
      printf("Tor Onion Thread Started!\n");

      std::string logDecl = "notice file " + GetDataDir().string() + "/tor/tor.log";
      char *argvLogDecl = (char*) logDecl.c_str();
      std::string rc = GetDataDir().string() + "/tor/torrc";
      char *rc_c = (char*) rc.c_str();

      char * clientTransportPlugin = NULL;

      struct stat sb;

      if ((stat("obfs4proxy", &sb) == 0 && sb.st_mode & S_IXUSR) || !std::system("which obfs4proxy")) {
          clientTransportPlugin = "obfs4 exec /usr/bin/obfs4proxy";
      } else if (stat("obfs4proxy.exe", &sb) == 0 && sb.st_mode & S_IXUSR) {
          clientTransportPlugin = "obfs4 exec obfs4proxy.exe";
      }

      if (clientTransportPlugin != NULL) {
          printf("Using OBFS4.\n");
          char* argv[] = {
            "tor",
            "--Log", argvLogDecl,
            "--SocksPort", "9089",
            "--ClientTransportPlugin", clientTransportPlugin,
            "--UseBridges", "1",
            "--Bridge", "38.229.33.135:443 8CF38F8AC7CA1ACF6051CACE5C84F3E5B3832CD1",
            "--Bridge", "obfs4 94.242.249.2:44939 E53EEA7DE6E170328F0A2C4338EE4E4DC2398218 cert=VpistQqdnS5zgkARR3he8rt03OrKhk2oobUUhLmFWAWK27pYMvjrBi6zAn1ebIcPH2xbcQ iat-mode=0",
            "--Bridge", "178.63.238.40:456 8E9B0C1B87837FF6CA730CDFB2A59DAB2D85DF08",
            "--ignore-missing-torrc",
            "-f", rc_c,
          };
          tor_main(16, argv);
      }
      else {
          printf("No OBFS4 found, not using it.\n");
          char* argv[] = {
            "tor",
            "--Log", argvLogDecl,
            "--SocksPort", "9089",
            "--ignore-missing-torrc",
            "-f", rc_c,
          };
          tor_main(6, argv);
      }
  }
}

void StartTor(void* parg)
{
  if(fNativeTor)
  {
      // Make this thread recognisable as the onion thread
      RenameThread("onion");
      try
      {
        run_tor();
      }
      catch (std::exception& e) {
        PrintException(&e, "StartTor()");
      }
      printf("Onion thread exited.");
  }
}
#endif

void StartNode(void* parg)
{
    // Make this thread recognisable as the startup thread
    RenameThread("innova-start");

    int64_t nStart = GetTimeMillis();
    // Attempt to find any banned nodes via our banlist.dat
    CBanDB bandb;
    banmap_t banmap;
    if (bandb.Read(banmap)) {
        CNode::SetBanned(banmap); // thread save setter
        CNode::SetBannedSetDirty(false); // no need to write down, just read data
        CNode::SweepBanned(); // sweep out unused entries

        printf("Loaded %d banned node ips/subnets from banlist.dat %" PRId64"ms\n",
                 banmap.size(), GetTimeMillis() - nStart);
    } else {
        printf("Invalid or missing banlist.dat...recreating\n");
        CNode::SetBannedSetDirty(true);
        DumpBanlist();
    }

    fAddressesInitialized = true;

    if (semOutbound == NULL) {
        // initialize semaphore
        int nMaxOutbound = min(MAX_OUTBOUND_CONNECTIONS, (int)GetArg("-maxconnections", 125));
        semOutbound = new CSemaphore(nMaxOutbound);
    }

    if (pnodeLocalHost == NULL)
        pnodeLocalHost = new CNode(INVALID_SOCKET, CAddress(CService("127.0.0.1", 0), nLocalServices));

    Discover();

    //
    // Start threads
    //

    if(!fNativeTor)
    {
        if (!GetBoolArg("-dnsseed", true))
            printf("DNS seeding disabled\n");
        else
            if (!NewThread(ThreadDNSAddressSeed, NULL))
                printf("Error: NewThread(ThreadDNSAddressSeed) failed\n");
    } else
    {
#ifdef USE_NATIVETOR
        // start the onion seeder
        if (!GetBoolArg("-onionseed", true))
            printf(".onion seeding disabled\n");
        else
            if (!NewThread(ThreadOnionSeed, NULL))
				printf("Error: could not start .onion seeding\n");
#endif
    };

    // Map ports with UPnP
    if (fUseUPnP)
        MapPort();

    // Send and receive from sockets, accept connections
    if (!NewThread(ThreadSocketHandler, NULL))
        printf("Error: NewThread(ThreadSocketHandler) failed\n");

    // Initiate outbound connections from -addnode
    if (!NewThread(ThreadOpenAddedConnections, NULL))
        printf("Error: NewThread(ThreadOpenAddedConnections) failed\n");

    // Initiate outbound connections
    if (!NewThread(ThreadOpenConnections, NULL))
        printf("Error: NewThread(ThreadOpenConnections) failed\n");

    // Process messages
    if (!NewThread(ThreadMessageHandler, NULL))
        printf("Error: NewThread(ThreadMessageHandler) failed\n");

    // Dump network addresses
    if (!NewThread(ThreadDumpAddress, NULL))
        printf("Error; NewThread(ThreadDumpAddress) failed\n");

    // Mine proof-of-stake blocks in the background
    if (!GetBoolArg("-staking", true))
        printf("Staking disabled\n");
    else
        if (!NewThread(ThreadStakeMiner, pwalletMain))
            printf("Error: NewThread(ThreadStakeMiner) failed\n");

    // Dump network addresses and bans upon bootup
    // DumpData();
}

bool StopNode()
{
    printf("StopNode()\n");
    fShutdown = true;
    mempool.AddTransactionsUpdated(1);
    int64_t nStart = GetTime();
    if (semOutbound)
        for (int i=0; i<MAX_OUTBOUND_CONNECTIONS; i++)
            semOutbound->post();

    if (fAddressesInitialized) {
        DumpData();
        fAddressesInitialized = false;
    }

    do
    {
        int nThreadsRunning = 0;
        for (int n = 0; n < THREAD_MAX; n++)
            nThreadsRunning += vnThreadsRunning[n];
        if (nThreadsRunning == 0)
            break;
        if (GetTime() - nStart > 20)
            break;
        MilliSleep(20);
    } while(true);
    if (vnThreadsRunning[THREAD_SOCKETHANDLER] > 0) printf("ThreadSocketHandler still running\n");
    if (vnThreadsRunning[THREAD_OPENCONNECTIONS] > 0) printf("ThreadOpenConnections still running\n");
    if (vnThreadsRunning[THREAD_MESSAGEHANDLER] > 0) printf("ThreadMessageHandler still running\n");
    if (vnThreadsRunning[THREAD_RPCLISTENER] > 0) printf("ThreadRPCListener still running\n");
    if (vnThreadsRunning[THREAD_RPCHANDLER] > 0) printf("ThreadsRPCServer still running\n");
#ifdef USE_UPNP
    if (vnThreadsRunning[THREAD_UPNP] > 0) printf("ThreadMapPort still running\n");
#endif
    if (vnThreadsRunning[THREAD_DNSSEED] > 0) printf("ThreadDNSAddressSeed still running\n");
    if (vnThreadsRunning[THREAD_ADDEDCONNECTIONS] > 0) printf("ThreadOpenAddedConnections still running\n");
    if (vnThreadsRunning[THREAD_DUMPADDRESS] > 0) printf("ThreadDumpAddresses still running\n");
    if (vnThreadsRunning[THREAD_STAKE_MINER] > 0) printf("ThreadStakeMiner still running\n");
    while (vnThreadsRunning[THREAD_MESSAGEHANDLER] > 0 || vnThreadsRunning[THREAD_RPCHANDLER] > 0)
        MilliSleep(20);
    MilliSleep(50);

    return true;
}

class CNetCleanup
{
public:
    CNetCleanup()
    {
    }
    ~CNetCleanup()
    {
        // Close sockets
        for (CNode* pnode : vNodes)
            if (pnode->hSocket != INVALID_SOCKET)
                CloseSocket(pnode->hSocket);
        for (ListenSocket& hListenSocket : vhListenSocket)
            if (hListenSocket.socket != INVALID_SOCKET)
                if (!CloseSocket(hListenSocket.socket))
                    printf("CloseSocket(hListenSocket) failed with error %d\n", WSAGetLastError());

        // Clean up for helping leak detection
        for (CNode* pnode : vNodes)
            delete pnode;
        for (CNode* pnode : vNodesDisconnected)
            delete pnode;
        vNodes.clear();
        vNodesDisconnected.clear();
        vhListenSocket.clear();
        delete semOutbound;
        semOutbound = NULL;
        delete pnodeLocalHost;
        pnodeLocalHost = NULL;

#ifdef WIN32
        // Shutdown Windows Sockets
        WSACleanup();
#endif
    }
} instance_of_cnetcleanup;

void RelayTransaction(const CTransaction& tx, const uint256& hash)
{
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss.reserve(10000);
    ss << tx;
    RelayTransaction(tx, hash, ss);
}

void RelayTransaction(const CTransaction& tx, const uint256& hash, const CDataStream& ss)
{
    CInv inv(MSG_TX, hash);
    {
        LOCK(cs_mapRelay);
        // Expire old relay messages
        while (!vRelayExpiration.empty() && vRelayExpiration.front().first < GetTime())
        {
            mapRelay.erase(vRelayExpiration.front().second);
            vRelayExpiration.pop_front();
        }

        static const size_t MAX_RELAY_SIZE = 100000;
        if (mapRelay.size() >= MAX_RELAY_SIZE)
        {
            if (!vRelayExpiration.empty())
            {
                mapRelay.erase(vRelayExpiration.front().second);
                vRelayExpiration.pop_front();
            }
        }

        // Save original serialized message so newer versions are preserved
        mapRelay.insert(std::make_pair(inv, ss));
        vRelayExpiration.push_back(std::make_pair(GetTime() + 15 * 60, inv));
    }

    bool fShielded = tx.IsShielded();
    if (dandelionState.IsEnabled())
    {
        if (dandelionState.AddTransaction(hash, fShielded, false))
        {
            std::vector<int> vPeerIds;
            {
                LOCK(cs_vNodes);
                for (CNode* pnode : vNodes)
                    vPeerIds.push_back(pnode->GetId());
            }
            dandelionRouter.UpdateEpoch(GetTime(), vPeerIds);

            CDandelionTxState txState;
            if (!dandelionState.GetTxState(hash, txState))
            {
                txState.fShielded = fShielded;
            }
            if (!dandelionRouter.ShouldFluff(txState))
            {
                int nStemPeerId = dandelionRouter.GetStemPeer(hash);
                if (nStemPeerId >= 0)
                {
                    LOCK(cs_vNodes);
                    for (CNode* pnode : vNodes)
                    {
                        if (pnode->GetId() == nStemPeerId)
                        {
                            pnode->PushInventory(inv);
                            break;
                        }
                    }
                    return; // Stem relay done, do not broadcast to all
                }
            }
            dandelionState.TransitionToFluff(hash);
        }
    }

    RelayInventory(inv);
}

void RelayTransactionLockReq(const CTransaction& tx, const uint256& hash, bool relayToAll)
{
    CInv inv(MSG_TXLOCK_REQUEST, tx.GetHash());

    //broadcast the new lock
    LOCK(cs_vNodes);
    BOOST_FOREACH(CNode* pnode, vNodes)
    {
        if(!relayToAll && !pnode->fRelayTxes)
            continue;

        pnode->PushMessage("txlreq", tx);
    }

}

void RelayCollaTeralFinalTransaction(const int sessionID, const CTransaction& txNew)
{
    LOCK(cs_vNodes);
    BOOST_FOREACH(CNode* pnode, vNodes)
    {
        pnode->PushMessage("dsf", sessionID, txNew);
    }
}

void RelayCollaTeralIn(const std::vector<CTxIn>& in, const int64_t& nAmount, const CTransaction& txCollateral, const std::vector<CTxOut>& out)
{
    LOCK(cs_vNodes);

    BOOST_FOREACH(CNode* pnode, vNodes)
    {
        if((CNetAddr)colLateralPool.submittedToCollateralnode != (CNetAddr)pnode->addr) continue;
        printf("RelayCollaTeralIn - found master, relaying message - %s \n", pnode->addr.ToString().c_str());
        pnode->PushMessage("dsi", in, nAmount, txCollateral, out);
    }
}

void RelayCollaTeralStatus(const int sessionID, const int newState, const int newEntriesCount, const int newAccepted, const std::string error)
{
    LOCK(cs_vNodes);
    BOOST_FOREACH(CNode* pnode, vNodes)
    {
        pnode->PushMessage("dssu", sessionID, newState, newEntriesCount, newAccepted, error);
    }
}

void RelayCollaTeralElectionEntry(const CTxIn vin, const CService addr, const std::vector<unsigned char> vchSig, const int64_t nNow, const CPubKey pubkey, const CPubKey pubkey2, const int count, const int current, const int64_t lastUpdated, const int protocolVersion)
{
    LOCK(cs_vNodes);
    BOOST_FOREACH(CNode* pnode, vNodes)
    {
        if(!pnode->fRelayTxes) continue;

        pnode->PushMessage("isee", vin, addr, vchSig, nNow, pubkey, pubkey2, count, current, lastUpdated, protocolVersion);
    }
}

/*
void RelayCollaTeralElectionEntry(const CTxIn vin, const CService addr, const std::vector<unsigned char> vchSig, const int64 nNow, const CPubKey pubkey, const CPubKey pubkey2, const int count, const int current, const int64 lastUpdated)
{
    LOCK(cs_vNodes);
    BOOST_FOREACH(CNode* pnode, vNodes)
    {
        pnode->PushMessage("isee", vin, addr, vchSig, nNow, pubkey, pubkey2, count, current, lastUpdated);
    }
}
*/

void SendCollaTeralElectionEntry(const CTxIn vin, const CService addr, const std::vector<unsigned char> vchSig, const int64_t nNow, const CPubKey pubkey, const CPubKey pubkey2, const int count, const int current, const int64_t lastUpdated, const int protocolVersion)
{
    LOCK(cs_vNodes);
    BOOST_FOREACH(CNode* pnode, vNodes)
    {
        pnode->PushMessage("isee", vin, addr, vchSig, nNow, pubkey, pubkey2, count, current, lastUpdated, protocolVersion);
    }
}

void RelayCollaTeralElectionEntryPing(const CTxIn vin, const std::vector<unsigned char> vchSig, const int64_t nNow, const bool stop)
{
    LOCK(cs_vNodes);
    BOOST_FOREACH(CNode* pnode, vNodes)
    {
        if(!pnode->fRelayTxes) continue;

        pnode->PushMessage("iseep", vin, vchSig, nNow, stop);
    }
}

void SendCollaTeralElectionEntryPing(const CTxIn vin, const std::vector<unsigned char> vchSig, const int64_t nNow, const bool stop)
{
    LOCK(cs_vNodes);
    BOOST_FOREACH(CNode* pnode, vNodes)
    {
        pnode->PushMessage("iseep", vin, vchSig, nNow, stop);
    }
}

void RelayCollaTeralCompletedTransaction(const int sessionID, const bool error, const std::string errorMessage)
{
    LOCK(cs_vNodes);
    BOOST_FOREACH(CNode* pnode, vNodes)
    {
        pnode->PushMessage("dsc", sessionID, error, errorMessage);
    }
}

//
// CBanListDB
//

CBanDB::CBanDB()
{
    pathBanlist = GetDataDir() / "banlist.dat";
}

bool CBanDB::Write(const banmap_t& banSet)
{
    // Generate random temporary filename
    unsigned short randv = 0;
    GetRandBytes((unsigned char*)&randv, sizeof(randv));
    std::string tmpfn = strprintf("banlist.dat.%04x", randv);

    // serialize banlist, checksum data up to that point, then append csum
    CDataStream ssBanlist(SER_DISK, CLIENT_VERSION);
    ssBanlist << FLATDATA(pchMessageStart);
    ssBanlist << banSet;
    uint256 hash = Hash(ssBanlist.begin(), ssBanlist.end());
    ssBanlist << hash;

    // open temp output file, and associate with CAutoFile
    boost::filesystem::path pathTmp = GetDataDir() / tmpfn;
    FILE *file = fopen(pathTmp.string().c_str(), "wb");
    CAutoFile fileout(file, SER_DISK, CLIENT_VERSION);
    if (fileout.IsNull())
        return error("%s: Failed to open file %s", __func__, pathTmp.string());

    // Write and commit header, data
    try {
        fileout << ssBanlist;
    }
    catch (const std::exception& e) {
        return error("%s: Serialize or I/O error - %s", __func__, e.what());
    }
    FileCommit(fileout.Get());
    fileout.fclose();

    // replace existing banlist.dat, if any, with new banlist.dat.XXXX
    if (!RenameOver(pathTmp, pathBanlist))
        return error("%s: Rename-into-place failed", __func__);

    return true;
}

bool CBanDB::Read(banmap_t& banSet)
{
    // open input file, and associate with CAutoFile
    FILE *file = fopen(pathBanlist.string().c_str(), "rb");
    CAutoFile filein(file, SER_DISK, CLIENT_VERSION);
    if (filein.IsNull())
        return error("%s: Failed to open file %s", __func__, pathBanlist.string());

    // use file size to size memory buffer
    uint64_t fileSize = boost::filesystem::file_size(pathBanlist);
    uint64_t dataSize = 0;
    // Don't try to resize to a negative number if file is small
    if (fileSize >= sizeof(uint256))
        dataSize = fileSize - sizeof(uint256);
    vector<unsigned char> vchData;
    vchData.resize(dataSize);
    uint256 hashIn;

    // read data and checksum from file
    try {
        filein.read((char *)&vchData[0], dataSize);
        filein >> hashIn;
    }
    catch (const std::exception& e) {
        return error("%s: Deserialize or I/O error - %s", __func__, e.what());
    }
    filein.fclose();

    CDataStream ssBanlist(vchData, SER_DISK, CLIENT_VERSION);

    // verify stored checksum matches input data
    uint256 hashTmp = Hash(ssBanlist.begin(), ssBanlist.end());
    if (hashIn != hashTmp)
        return error("%s: Checksum mismatch, data corrupted", __func__);

    unsigned char pchMsgTmp[4];
    try {
        // de-serialize file header (network specific magic number) and ..
        ssBanlist >> FLATDATA(pchMsgTmp);

        // ... verify the network matches ours
        if (memcmp(pchMsgTmp, pchMessageStart, sizeof(pchMsgTmp)))
            return error("%s: Invalid network magic number", __func__);

        // de-serialize address data into one CAddrMan object
        ssBanlist >> banSet;
    }
    catch (const std::exception& e) {
        return error("%s: Deserialize or I/O error - %s", __func__, e.what());
    }

    return true;
}

void DumpBanlist()
{
    CNode::SweepBanned(); // clean unused entries (if bantime has expired)

    if (!CNode::BannedSetIsDirty())
        return;

    int64_t nStart = GetTimeMillis();

    CBanDB bandb;
    banmap_t banmap;
    CNode::GetBanned(banmap);
    if (bandb.Write(banmap))
        CNode::SetBannedSetDirty(false);

    printf("Flushed %d banned node ips/subnets to banlist.dat  %" PRId64"ms\n",
             banmap.size(), GetTimeMillis() - nStart);
}

bool FetchBlockForStaking(const uint256& hashBlock)
{
    {
        LOCK(cs_main);
        if (mapBlockIndex.count(hashBlock))
        {
            CBlockIndex* pindex = mapBlockIndex[hashBlock];
            if (pindex->nFile > 0)
                return true;
        }
    }

    LOCK(cs_vNodes);
    int nRequested = 0;
    const int MAX_PEERS_TO_REQUEST = 3;

    for (CNode* pnode : vNodes)
    {
        if (nRequested >= MAX_PEERS_TO_REQUEST)
            break;

        if (!pnode->fSuccessfullyConnected)
            continue;
        if (pnode->nVersion < MIN_PEER_PROTO_VERSION)
            continue;

        std::vector<CInv> vGetData;
        vGetData.push_back(CInv(MSG_BLOCK, hashBlock));
        pnode->nLastGetDataTime = GetTime();
        pnode->PushMessage("getdata", vGetData);
        nRequested++;

        if (fDebug)
            printf("HybridSPV: Requested block %s from peer %s for staking (%d/%d)\n",
                   hashBlock.ToString().c_str(), pnode->addrName.c_str(),
                   nRequested, MAX_PEERS_TO_REQUEST);
    }

    return nRequested > 0;
}
