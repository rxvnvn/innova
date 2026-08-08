// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2012 The Bitcoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#ifndef BITCOIN_NET_H
#define BITCOIN_NET_H

#include <deque>
#include <boost/array.hpp>
#include <boost/foreach.hpp>
#include <openssl/rand.h>

#ifndef WIN32
#include <arpa/inet.h>
#endif

#include "core.h"
#include "mruset.h"
#include "netbase.h"
#include "protocol.h"
#include "addrman.h"
#include "bloom.h"
#include "blockrequesttrace.h"
#include "pinglifecycletrace.h"
#include "ibdmetrics.h"
#include "ibdactivepath.h"
#include "ibdblocklatency.h"
#include "ibdforensic.h"
#include "ibdexptrace.h"

class CRequestTracker;
class CNode;
class CBlockIndex;
class CBlockLocator;
class CStalledSyncRecoveryState;
extern int nBestHeight;

// Armed exactly once when an ordinary (non-recovery) block-sync getblocks has
// been committed for transmission to a peer that can advance local block sync.
// Recovery-tagged getblocks and getblocks to peers that cannot advance block
// sync never touch the state.  The state-ref overload is the testable core;
// the global overload arms the process-wide recovery state.
void RecordOrdinaryGetBlocksCommitted(CStalledSyncRecoveryState& state,
                                      int64_t nNow, uint64_t nRecoveryId,
                                      bool fPeerCanAdvanceBlockSync);
void RecordOrdinaryGetBlocksCommitted(int64_t nNow, uint64_t nRecoveryId,
                                      bool fPeerCanAdvanceBlockSync);

// Test hooks for the process-wide stalled-sync recovery state.
CStalledSyncRecoveryState& GetStalledSyncRecoveryStateForTesting();
void ResetStalledSyncRecoveryStateForTesting();
void StartSyncForTesting(const std::vector<CNode*>& vNodesIn);
void ResetSyncPeerForTesting();

// Test-only accessors for the pipeline-wake state machine.  Declared here
// following the ResetStalledSyncRecoveryStateForTesting convention so the
// p2p_sync_tests suite can set up and tear down the latched requested/handled
// generation, cause bits, and getblocks cooldown timestamp deterministically.
// No production call site uses them.
void ResetPipelineWakeStateForTesting();
void GetPipelineWakeStateForTesting(uint64_t* pnRequestedGeneration,
                                    uint64_t* pnHandledGeneration,
                                    uint32_t* pnCauseBits,
                                    int64_t* pnLastGetBlocksTime);
void SetPipelineWakeLastGetBlocksTimeForTesting(int64_t nTime);
void SetPipelineWakeRequestedForTesting(uint64_t nRequestedGeneration,
                                        uint64_t nHandledGeneration,
                                        uint32_t nCauseBits);

class CStalledSyncRecoveryState
{
private:
    int nLastObservedHeight;
    int64_t nLastProgressTime;
    int64_t nLastRecoveryTime;
    uint256 hashRejectedBlock;
    int64_t nRejectedBlockTime;
    bool fRejectedRetryScheduled;
    unsigned int nRecoveryAttempts;
    bool fSyncRequestSent;

public:
    CStalledSyncRecoveryState();

    bool ShouldRecover(int nLocalHeight, int nPeerHeight,
                       bool fPipelineActive, int64_t nNow,
                       int64_t nStallTimeout, int64_t nCooldown);
    void RecordRejectedBlock(
        const uint256& hashBlock, int64_t nNow,
        bool fRetryEligible = true);
    void ClearRejectedBlock(const uint256& hashBlock);
    bool TakeRejectedBlockForRetry(uint256& hashBlock);
    const uint256& RejectedBlock() const { return hashRejectedBlock; }
    int LastObservedHeight() const { return nLastObservedHeight; }
    int64_t LastProgressTime() const { return nLastProgressTime; }
    int64_t LastRecoveryTime() const { return nLastRecoveryTime; }
    int64_t RejectedBlockTime() const { return nRejectedBlockTime; }
    unsigned int RecoveryAttempts() const { return nRecoveryAttempts; }
    void MarkSyncRequestSent(int64_t nNow);
    bool SyncRequestSent() const { return fSyncRequestSent; }
};

enum GetBlocksServerAction
{
    GETBLOCKS_SERVER_ALLOW,
    GETBLOCKS_SERVER_SUPPRESS,
    GETBLOCKS_SERVER_RATE_LIMIT,
    GETBLOCKS_SERVER_DISCONNECT
};

struct CGetBlocksRequestInfo
{
    uint256 hashLocatorTip;
    int nResolvedHeight;
    uint256 hashStop;
    int nStopHeight;
    uint256 hashChainTip;
    uint256 hashPredictedFirst;
    uint256 hashPredictedLast;
    unsigned int nPredictedResponseCount;
    int64_t nRequestTimeMillis;

    CGetBlocksRequestInfo();
};

struct CGetBlocksResponseInfo
{
    uint256 hashFirst;
    uint256 hashLast;
    unsigned int nItemCount;
    int nMinHeight;
    int nMaxHeight;

    CGetBlocksResponseInfo();
    void Add(const uint256& hash, int nHeight);
};

struct CGetBlocksServerDecision
{
    GetBlocksServerAction action;
    bool fIdenticalRequest;
    bool fSameResponse;
    bool fProgress;
    bool fPenalize;
    int nPenalty;
    int64_t nCooldownMillis;
    uint64_t nEstimatedBytes;

    CGetBlocksServerDecision();
};

class CGetBlocksServerState
{
private:
    bool fHaveLastRequest;
    bool fHaveLastResponse;
    bool fTokenBucketInitialized;
    uint256 hashLastPredictedFirst;
    uint256 hashLastPredictedLast;
    unsigned int nLastPredictedResponseCount;
    int nLastStopHeight;
    uint256 hashLastResponseChainTip;
    int nLastResponseMinHeight;
    int nLastResponseMaxHeight;
    int64_t nTokenBucketLastMillis;
    int64_t nTokenBucketMilliTokens;
    int64_t nPendingRequestCostMilliTokens;
    uint64_t nUsefulGetDataSinceLastResponse;

    void RefillTokenBucket(int64_t nNowMillis);
    int64_t ResponseCostMilliTokens(unsigned int nItems) const;
    int64_t RepeatCooldownMillis() const;

public:
    uint256 hashLastLocatorTip;
    int nLastResolvedHeight;
    uint256 hashLastStop;
    uint256 hashLastResponseFirst;
    uint256 hashLastResponseLast;
    unsigned int nLastResponseCount;
    uint64_t nLastResponseBytes;
    int64_t nLastRequestTimeMillis;
    uint64_t nResponseBytesAllowed;
    int64_t nPreviousRequestTimeMillis;
    int64_t nRepeatAllowedAfterMillis;
    int nLastProgressDelta;
    uint64_t nRequestsReceived;
    uint64_t nResponsesAllowed;
    uint64_t nResponsesSuppressed;
    uint64_t nRequestsRateLimited;
    uint64_t nIdenticalRequests;
    uint64_t nSameLocatorRequests;
    uint64_t nSameResponseRequests;
    uint64_t nNonProgressingRequests;
    uint64_t nUsefulGetData;
    uint64_t nEstimatedSuppressedBytes;
    unsigned int nConsecutiveIdenticalRequests;
    unsigned int nConsecutiveNonProgressingRequests;

    CGetBlocksServerState();

    CGetBlocksServerDecision Evaluate(const CGetBlocksRequestInfo& request,
                                      bool fStrictInbound);
    void RecordResponse(const CGetBlocksRequestInfo& request,
                        const CGetBlocksResponseInfo& response);
    bool NoteBlockGetData(const uint256& hashBlock, int nHeight,
                          int64_t nNowMillis);
    static uint64_t EstimateInvPayloadBytes(unsigned int nItems);
};

const char* GetBlocksServerActionName(GetBlocksServerAction action);

class CSyncLockDiagnostics
{
private:
    const char* pszLocation;
    const char* pszLocks;
    int64_t nWaitStartTime;
    int64_t nAcquiredTime;
    bool fEnabled;

public:
    CSyncLockDiagnostics(const char* pszLocationIn,
                         const char* pszLocksIn);
    ~CSyncLockDiagnostics();
    void Acquired();
};


/** Time between pings automatically sent out for latency probing and keepalive (in seconds). */
static const int PING_INTERVAL = 2 * 60;
/** Time after which to disconnect, after waiting for a ping response (or inactivity). */
static const int TIMEOUT_INTERVAL = 20 * 60;

inline unsigned int ReceiveFloodSize() { return 1000*GetArg("-maxreceivebuffer", 5*1000); }
inline unsigned int SendBufferSize() { return 1000*GetArg("-maxsendbuffer", 1*1000); }

void AddOneShot(std::string strDest);
bool OpenNetworkConnectionSimple(const CAddress& addrConnect, const char *strDest = NULL);
bool RecvLine(SOCKET hSocket, std::string& strLine);
bool GetMyExternalIP(CNetAddr& ipRet);
void AddressCurrentlyConnected(const CService& addr);
CNode* FindNode(const CNetAddr& ip);
CNode* FindNode(const CSubNet& subNet);
CNode* FindNode(const std::string& addrName);
CNode* FindNode(const CService& ip);
//CNode* ConnectNode(CAddress addrConnect, const char *strDest = NULL);
CNode* ConnectNode(CAddress addrConnect, const char *strDest = NULL, bool colLateralMaster=false);
void MapPort();
unsigned short GetListenPort();
bool BindListenPort(const CService &bindAddr, std::string& strError=REF(std::string()), bool fWhitelisted = false);
void StartTor(void* parg);
void StartNode(void* parg);
bool StopNode();
/** Explicit, idempotent release of all network runtime state: closes sockets,
 *  deletes CNode objects and clears global containers.  Called from StopNode
 *  after every network thread has been joined, before process exit, so that
 *  static destruction (CNetCleanup) has no meaningful work left to do. */
void CleanupNetworkState();
void SocketSendData(CNode *pnode);
void RecordP2PMessageStat(const CNode* pnode, const std::string& command, unsigned int bytes, bool incoming);

enum PipelineWakeCause
{
    WAKE_CAUSE_CLEAR_INFLIGHT = 1U << 0,
    WAKE_CAUSE_INFLIGHT_TIMEOUT = 1U << 1,
    WAKE_CAUSE_ASKFOR_ALREADY_HAVE = 1U << 2,
    WAKE_CAUSE_ASKFOR_OWNER_CONFLICT = 1U << 3,
    WAKE_CAUSE_QUEUE_REMOVAL = 1U << 4,
    WAKE_CAUSE_CLEAR_ASKFOR = 1U << 5,
    WAKE_CAUSE_DISCONNECT_CLEANUP = 1U << 6,
    WAKE_CAUSE_GETBLOCKS_OUTSTANDING_CLEARED = 1U << 7,
    WAKE_CAUSE_OTHER = 1U << 8,
    WAKE_CAUSE_GETBLOCKS_OUTSTANDING_TIMEOUT = 1U << 9
};

enum PipelineWakeOutcome
{
    PIPELINE_WAKE_OUTCOME_NONE = 0,
    PIPELINE_WAKE_TRANSIENT_CS_MAIN_TRYLOCK_FAILED,
    PIPELINE_WAKE_TRANSIENT_CS_VNODES_TRYLOCK_FAILED,
    PIPELINE_WAKE_TRANSIENT_COOLDOWN_ACTIVE,
    PIPELINE_WAKE_TRANSIENT_DEDUP_ALL,
    PIPELINE_WAKE_TRANSIENT_INCOMPLETE_PEER_SCAN,
    PIPELINE_WAKE_TRANSIENT_SHUTDOWN,
    PIPELINE_WAKE_TERMINAL_NOT_IBD,
    PIPELINE_WAKE_TERMINAL_PIPELINE_NOT_EMPTY,
    PIPELINE_WAKE_TERMINAL_DEFERRED_REFILL_CREATED_WORK,
    PIPELINE_WAKE_TERMINAL_GETBLOCKS_QUEUED,
    PIPELINE_WAKE_TERMINAL_NO_ELIGIBLE_AHEAD_PEER,
    PIPELINE_WAKE_TERMINAL_EXISTING_QUEUED_GETBLOCKS,
    PIPELINE_WAKE_TERMINAL_OUTSTANDING_GETBLOCKS_PRESENT
};

typedef int NodeId;

void RequestBlockPipelineWake(uint32_t nCause);

// Experiment A (future-supply diversification): cached -ibddivfuture /
// -ibddivfrac configuration.  Loaded lazily on first use so the arguments are
// read exactly once; unit tests reload them through
// ResetFutureSupplyDiversificationConfigForTesting().
bool IsFutureSupplyDiversificationEnabled();
// -ibddivfrac clamped to [0,1] and expressed in permille (0..1000).
int GetFutureSupplyDiversificationFractionPermille();
void ResetFutureSupplyDiversificationConfigForTesting();

// Experiment A attribution.  When a deferred future candidate is dispatched to
// a non-announcer lane (a diversified dispatch), the announcer's peer id is
// recorded per hash so the in-flight-mark and timeout paths can attribute the
// request.  Guarded by cs_mapAlreadyAskedFor (the same lock taken by AskFor).
void RecordDiversifyDispatch(const uint256& hash, NodeId announcePeer);
// Find-only probe: sets *pAnnouncePeer and returns true when the hash was
// dispatched to a non-announcer lane.
bool GetDiversifyAnnounce(const uint256& hash, NodeId* pAnnouncePeer);
// Find-and-erase probe: like GetDiversifyAnnounce but consumes the record
// (used when the dispatch finishes: receive, timeout, or pre-dispatch removal).
bool TakeDiversifyAnnounce(const uint256& hash, NodeId* pAnnouncePeer);
// Erase any record whose value is peer (no-op probe variant).
void ClearDiversifyDispatch(const uint256& hash);
void ClearDiversifyDispatchForPeer(NodeId peer);
// Test-only: clear the diversification attribution ledger.
void ResetDiversifyDispatchLedgerForTesting();

// Experiment A lane selection.
struct FutureSupplyLane
{
    CNode* node;
    int32_t peerLiveActivePressure;
    FutureSupplyLane(CNode* p, int32_t n)
        : node(p), peerLiveActivePressure(n)
    {
    }
};

// Collect the eligible non-announcer future-supply lanes from vNodesCopy
// (already AddRef'd by the caller for the whole message-handler pass, so the
// returned CNode* pointers are valid for the caller's scope).  A peer is
// eligible iff it is a connected full node, not disconnecting, able to advance
// block sync beyond nBestHeight, and has a free window slot.  pfrom is NOT
// included; it is handled separately by ChooseDeferredDispatchLane.
std::vector<FutureSupplyLane> CollectEligibleFutureSupplyLanes(
    const std::vector<CNode*>& vNodesCopy, int nBestHeight);

// Decide which lane should re-request the deferred future candidate hash that
// was announced by pfrom.  Returns pfrom (the announcer) when diversification
// is disabled, when no snapshot (vNodesCopy) is available, when the candidate
// is pfrom's single deferred frontier candidate, or when no other eligible
// lane exists.  Otherwise returns the eligible lane with the lowest
// peer-active-pressure (tie-break peerDiversifySeq), picking pfrom instead
// with probability (1 - -ibddivfrac) while pfrom still has capacity.
CNode* ChooseDeferredDispatchLane(CNode* pfrom, const uint256& hash,
                                  const std::vector<CNode*>& vNodesCopy);
PipelineWakeOutcome MaybeProcessPipelineWake(
    const std::vector<CNode*>& vNodesCopy,
    bool forceMainLockFailureForTest = false);
void RecordGetHeadersResponse(CNode* pnode, size_t nHeaders, unsigned int nBytes);
void LogSyncDiagnosticsMaybe();
CNode* MaybeQueueStalledSyncRecovery(const std::vector<CNode*>& vNodes,
                                     CBlockIndex* pindexTip,
                                     int nLocalHeight,
                                     int64_t nNow,
                                     int64_t nStallTimeout,
                                     int64_t nCooldown,
                                     CStalledSyncRecoveryState& state,
                                     std::string* pstrSkipReason = NULL);
void RecordRejectedBlockForSync(
    const uint256& hashBlock, bool fRetryEligible = true);
void ClearRejectedBlockForSync(const uint256& hashBlock);
void RecordOrphanLimitRejectedBlock(NodeId peer, const CInv& inv,
                                    int64_t nUntilMicros,
                                    const uint256& hashParent);
bool IsOrphanLimitRejectedBlockInCooldown(NodeId peer, const CInv& inv,
                                          int64_t nNowMicros,
                                          int64_t* nUntilMicros = NULL);
bool IsOrphanLimitRejectedByOtherPeer(NodeId peer, const CInv& inv,
                                      int64_t nNowMicros);
void ReleaseOrphanLimitRejectedForPeer(NodeId peer);
void RetryOrphanLimitRejectedOnParentConnect(const uint256& hashParent,
                                             CNode* pfrom);
// Writes the short, peer-agnostic negative cooldown for a rejected block to
// mapAlreadyAskedFor.  The long (120s) orphan-limit suppression is stored only
// in the peer-local cooldown map by RecordOrphanLimitRejectedBlock; the global
// map never carries an orphan-limit blocker that would delay a different peer.
void RecordRejectedBlockGlobalNegativeCooldown(const CInv& inv);
// Bounded-map inspection used by diagnostics and tests.
size_t GetOrphanLimitRejectedEntryCount();
size_t GetOrphanLimitRejectedEntryCountForPeer(NodeId peer);
bool SyncTraceEnabled();
static const int64_t RECOVERY_RESPONSE_WINDOW_US = 2000000;

enum RecoveryResponseOutcome
{
    RECOVERY_OUTCOME_USEFUL,
    RECOVERY_OUTCOME_KNOWN_ONLY_TIMEOUT,
    RECOVERY_OUTCOME_EMPTY_TIMEOUT,
    RECOVERY_OUTCOME_DISCONNECTED,
    RECOVERY_OUTCOME_SUPERSEDED_BY_NEXT_RECOVERY
};

struct RecoveryResponseObservation
{
    uint64_t total_inv;
    uint64_t block_inv;
    uint64_t unknown_blocks;
    uint64_t known_active_blocks;
    uint64_t known_nonactive_indexed_blocks;
    uint64_t known_orphan_blocks;
    uint256 first_block_hash;
    uint256 first_unknown_block_hash;
    RecoveryResponseObservation() : total_inv(0), block_inv(0), unknown_blocks(0), known_active_blocks(0), known_nonactive_indexed_blocks(0), known_orphan_blocks(0) {}
};

struct RecoveryResponseResult
{
    RecoveryResponseOutcome outcome;
    uint64_t recovery_id;
    int64_t send_time_us;
    int64_t elapsed_us;
    uint64_t inv_message_count;
    uint64_t total_inv;
    uint64_t block_inv;
    uint64_t unknown_blocks;
    uint64_t known_active_blocks;
    uint64_t known_nonactive_indexed_blocks;
    uint64_t known_orphan_blocks;
    uint256 first_block_hash;
    uint256 first_unknown_block_hash;
    int64_t first_block_elapsed_us;
    int64_t first_unknown_elapsed_us;
};

class RecoveryResponseWindowState
{
    bool active;
    uint64_t recovery_id;
    int64_t send_time_us;
    int64_t deadline_us;
    uint64_t inv_message_count, total_inv, block_inv, unknown_blocks;
    uint64_t known_active_blocks, known_nonactive_indexed_blocks, known_orphan_blocks;
    uint256 first_block_hash, first_unknown_block_hash;
    int64_t first_block_elapsed_us, first_unknown_elapsed_us;
    bool Finish(int64_t now_us, RecoveryResponseOutcome outcome, RecoveryResponseResult& result);
public:
    RecoveryResponseWindowState();
    void Start(uint64_t id, int64_t send_us);
    bool ObserveInv(int64_t now_us, const RecoveryResponseObservation& observation, RecoveryResponseResult& completed);
    bool Expire(int64_t now_us, RecoveryResponseResult& completed);
    bool Supersede(int64_t now_us, RecoveryResponseResult& completed);
    bool Disconnect(int64_t now_us, RecoveryResponseResult& completed);
    bool IsActive() const { return active; }
};
uint64_t RecoveryTraceTrigger(CNode* pnode, int nLocalHeight, int nPeerHeight, int64_t nStallAge, unsigned int nAttempt);
void RecoveryTraceQueue(CNode* pnode, uint64_t nRecoveryId, CBlockIndex* pindexBegin, uint256 hashStop, size_t nQueueBefore, size_t nQueueAfter);
void RecoveryTraceSend(CNode* pnode, uint64_t nRecoveryId, CBlockIndex* pindexBegin, uint256 hashStop, size_t nQueueBeforeClear);
const char* RecoveryResponseOutcomeName(RecoveryResponseOutcome outcome);
std::string FormatRecoveryResponseSummary(int64_t peer_id, const RecoveryResponseResult& result);
void LogGetInfoSyncProbe(const char* pszEvent, int64_t nRequestStartTime = 0,
                         int64_t nLockWaitMicros = -1);

// Signals for message handling
struct CNodeSignals
{
    boost::signals2::signal<bool (CNode*)> ProcessMessages;
    boost::signals2::signal<bool (CNode*, bool)> SendMessages;
};

CNodeSignals& GetNodeSignals();

enum
{
    LOCAL_NONE,   // unknown
    LOCAL_IF,     // address a local interface listens on
    LOCAL_BIND,   // address explicit bound to
    LOCAL_UPNP,   // address reported by UPnP
    LOCAL_HTTP,   // address reported by whatismyip.com and similar
    LOCAL_MANUAL, // address explicitly specified (-externalip=)

    LOCAL_MAX
};

void SetLimited(enum Network net, bool fLimited = true);
bool IsLimited(enum Network net);
bool IsLimited(const CNetAddr& addr);
bool AddLocal(const CService& addr, int nScore = LOCAL_NONE);
bool AddLocal(const CNetAddr& addr, int nScore = LOCAL_NONE);
bool SeenLocal(const CService& addr);
bool IsLocal(const CService& addr);
bool GetLocal(CService &addr, const CNetAddr *paddrPeer = NULL);
bool IsReachable(const CNetAddr &addr);
void SetReachable(enum Network net, bool fFlag = true);
CAddress GetLocalAddress(const CNetAddr *paddrPeer = NULL);

extern std::vector<std::string> vAddedNodes;
extern CCriticalSection cs_vAddedNodes;

enum
{
    MSG_TX = 1,
    MSG_BLOCK,
    // Nodes may always request a MSG_FILTERED_BLOCK in a getdata, however,
    // MSG_FILTERED_BLOCK should not appear in any invs except as a part of getdata.
    MSG_FILTERED_BLOCK,
    MSG_TXLOCK_REQUEST,
    MSG_TXLOCK_VOTE,
    MSG_SPORK,
    MSG_COLLATERALNODE_WINNER
};

class CRequestTracker
{
public:
    void (*fn)(void*, CDataStream&);
    void* param1;

    explicit CRequestTracker(void (*fnIn)(void*, CDataStream&)=NULL, void* param1In=NULL)
    {
        fn = fnIn;
        param1 = param1In;
    }

    bool IsNull()
    {
        return fn == NULL;
    }
};


/** Thread types */
enum threadId
{
    THREAD_SOCKETHANDLER,
    THREAD_OPENCONNECTIONS,
    THREAD_MESSAGEHANDLER,
    THREAD_RPCLISTENER,
    THREAD_UPNP,
    THREAD_DNSSEED,
    THREAD_ADDEDCONNECTIONS,
    THREAD_DUMPADDRESS,
    THREAD_RPCHANDLER,
    THREAD_STAKE_MINER,
    THREAD_TORNET,
    THREAD_ONIONSEED,

    THREAD_MAX
};

extern bool fDiscover;
extern bool fUseUPnP;
extern uint64_t nLocalServices;
extern uint64_t nLocalHostNonce;
extern CAddress addrSeenByPeer;
extern boost::array<int, THREAD_MAX> vnThreadsRunning;
extern CAddrMan addrman;

extern std::vector<CNode*> vNodes;
extern CCriticalSection cs_vNodes;
extern std::map<CInv, CDataStream> mapRelay;
extern std::deque<std::pair<int64_t, CInv> > vRelayExpiration;
extern CCriticalSection cs_mapRelay;
extern std::map<CInv, int64_t> mapAlreadyAskedFor;
extern CCriticalSection cs_mapAlreadyAskedFor;
static const size_t MAX_ALREADY_ASKED_FOR_SIZE = 50000;
/** Normal INV active scheduler window per peer during IBD. */
static const int MAX_DEFERRED_INV_ACTIVE_PER_PEER = 128;
/** Normal INV active scheduler window across all peers during IBD. */
static const int MAX_DEFERRED_INV_ACTIVE_GLOBAL = 512;
/** Maximum retained ordered legacy block INV candidates per peer. */
static const size_t MAX_DEFERRED_BLOCK_INV_PER_PEER = 1000;
/** Maximum deferred candidates examined in one refill pump. */
static const size_t MAX_DEFERRED_BLOCK_INV_REFILL_WORK = 256;

// Effective per-peer active block request window during IBD.  Returns
// MAX_DEFERRED_INV_ACTIVE_PER_PEER (128) when not in IBD.  The experimental
// -ibdmaxactiveperpeer=<n> runtime window is clamped to
// [1, MAX_DEFERRED_INV_ACTIVE_GLOBAL]; zero, negative, and non-numeric values
// are rejected and fall back to the default.  The configured value is read
// once and cached at first use; hot paths must not re-read the argument.
int GetMaxActiveBlockRequestsPerPeer();

// Test hook: force the cached -ibdmaxactiveperpeer value to reload from
// mapArgs on the next GetMaxActiveBlockRequestsPerPeer() call.
void ResetMaxActiveBlockRequestsPerPeerConfigForTesting();

enum BlockRequestOwnerState
{
    BLOCK_REQUEST_OWNER_QUEUED = 0,
    BLOCK_REQUEST_OWNER_IN_FLIGHT
};

// Result of one CNode::AskFor admission attempt.  The retry path for
// orphan-limit rejected blocks (RetryOrphanLimitRejectedOnParentConnect) uses
// this typed result to decide whether a cooldown entry may be permanently
// erased: the entry is only removed when the result proves the request was
// retained (queued, already queued, in flight, or owned by another peer).
enum AskForResult
{
    ASKFOR_QUEUED = 0,
    ASKFOR_ALREADY_QUEUED,
    ASKFOR_INFLIGHT,
    ASKFOR_OWNED_BY_OTHER,
    ASKFOR_COOLDOWN,
    ASKFOR_CAP_FULL
};

bool TryAssignBlockRequestOwner(const uint256& hash, NodeId peer,
                                BlockRequestTraceSource source = BLOCKREQ_SOURCE_OTHER,
                                NodeId* existingPeer = NULL,
                                BlockRequestOwnerState* existingState = NULL);
bool TryAssignBlockRequestOwnerLocked(const uint256& hash, NodeId peer,
                                      BlockRequestTraceSource source = BLOCKREQ_SOURCE_OTHER,
                                      NodeId* existingPeer = NULL,
                                      BlockRequestOwnerState* existingState = NULL);
bool GetBlockRequestOwner(const uint256& hash, NodeId* ownerPeer,
                          BlockRequestOwnerState* ownerState);
bool TransitionBlockRequestOwnerToInFlight(const uint256& hash, NodeId peer);
bool ReleaseBlockRequestOwner(const uint256& hash, NodeId peer,
                              const char* pszReason);
bool ReleaseBlockRequestOwnerOnReceive(const uint256& hash, NodeId peer);
size_t ReleaseBlockRequestOwnersForPeer(NodeId peer, const char* pszReason,
                                        bool fRecordForensics = true);
const char* BlockRequestOwnerStateName(BlockRequestOwnerState state);

bool IsBlockRequestOwnedByAnyPeer(const uint256& hash);
bool EraseAlreadyAskedForIfUnowned(const CInv& inv);
static const int64_t ALREADY_ASKED_FOR_RETENTION_US = 60LL * 60 * 1000000;
static const int64_t ALREADY_ASKED_FOR_NEGATIVE_COOLDOWN_US = 5LL * 1000000;
static const int64_t ORPHAN_LIMIT_REJECT_RETRY_COOLDOWN_US = 2LL * 60 * 1000000;
// Strict memory bounds for the peer-local orphan-limit reject cooldown map
// (mapOrphanLimitRejectedBlocks in net.cpp).  Per-peer mirrors
// MAX_ORPHAN_BLOCKS_PER_PEER so one saturating peer cannot hoard entries;
// process-global mirrors MAX_ALREADY_ASKED_FOR_SIZE so many peers together
// cannot.  When a bound is exceeded the earliest-expiry entry (least
// remaining protective time) is evicted deterministically.
static const size_t MAX_ORPHAN_LIMIT_REJECTED_PER_PEER = 750;
static const size_t MAX_ORPHAN_LIMIT_REJECTED_GLOBAL = 50000;
size_t PruneAlreadyAskedFor(int64_t nNowMicros);

// ----------------------------------------------------------------------------
// Timeout-aware IBD peer quality ranking.
//
// Every block request lifecycle observable on a peer (queued / in-flight mark,
// delivery receive, timeout expiry) feeds a small per-peer quality record used
// only to *prefer* better suppliers when a block request is admitted or when a
// timed-out hash is re-requested.  The ranking is best-effort scheduling
// preference only: it never bans, disconnects, or Misbehaves a peer, ownership
// stays exclusive (one active owner per hash, no hedged requests), and a
// degraded-but-sole supplier always keeps working (frontier-safe fallback).
// A fresh peer is UNKNOWN (neutral): it is never treated as better than a
// proven-good peer, but it can serve as an alternative to a degraded one.
// Reputation recovers without a restart because the scores decay with every
// completed outcome (rolling window in event space).
// ----------------------------------------------------------------------------

enum IbdPeerQualityTier
{
    IBD_PEER_QUALITY_GOOD = 0,
    IBD_PEER_QUALITY_UNKNOWN,
    IBD_PEER_QUALITY_DEGRADED
};

// Completed-outcome events that feed the per-peer quality record.  Used to
// route a block-request admission to a better supplier.
enum IbdAskForRedirectReason
{
    IBD_REDIRECT_NONE = 0,      // keep the announcer (fast path)
    IBD_REDIRECT_HASH_TIMEOUT,  // announcer timed out this exact hash before
    IBD_REDIRECT_QUALITY,       // announcer is degraded
    IBD_REDIRECT_CONCENTRATION  // announcer dominates the global active window
};

// Fixed-point scale for the decayed scores.  Scores are kept at 16x resolution
// so the per-event decay (multiply by 15/16) is well-defined for small counts:
// an unscaled integer score below 16 would never decay, pinning a peer's tier
// forever.  A single outcome contributes 1*SCALE and the steady state of a
// pure event stream is SCALE << DECAY_SHIFT = 256.
static const int64_t IBD_PEER_QUALITY_SCALE = 16;
// Decay applied to a score on every completed outcome:
// score = score - (score >> SHIFT), i.e. multiply by 15/16 per event.  With
// per-event decay of 1/16, influence of an event halves after ~11 events, so
// the scores behave like a rolling window in event space.
static const int64_t IBD_PEER_QUALITY_DECAY_SHIFT = 4;
// Decayed-score baseline (in scaled units) for trusting a quality tier.  A
// steady receive stream holds the combined score near 256, so the threshold of
// 8*SCALE = 128 means roughly the last ~10-11 completed outcomes.
static const int64_t IBD_PEER_QUALITY_MIN_SCORE = 8;
// A peer whose decayed timeout share exceeds 250 permille (25%) of its decayed
// completed outcomes is ranked DEGRADED.  Both scores decay on every completed
// outcome (see CNode::RecordIbdBlockDelivery / RecordIbdBlockTimeout), so the
// timeout share is a rolling window in event space and a recovering peer is
// re-promoted to GOOD without a restart.
static const int64_t IBD_PEER_QUALITY_DEGRADED_RATE_PERMILLE = 250;
// Delivery-latency EWMA smoothing factor: new_sample = (7*old + 1*new) / 8.
static const int64_t IBD_PEER_LATENCY_EWMA_SHIFT = 3;

// Alternate-announcer ledger bounds.  The ledger records, per block hash,
// peers that announced the hash while another peer owned it (a strong signal
// the peer can serve it) so a timed-out request can be reassigned away from
// the failed owner.  Entries are bounded per hash and globally, and expire by
// TTL so stale announcers never pin a hash forever.
static const size_t IBD_ALTERNATE_ANNOUNCERS_PER_HASH = 4;
static const size_t IBD_ALTERNATE_ANNOUNCER_MAX_HASHES = 2048;
static const int64_t IBD_ALTERNATE_ANNOUNCER_TTL_US = 60LL * 1000000;

// Last-timeout-owner ledger bounds.  This ledger is on the hottest path (every
// admitted block INV reads it), so lookups must stay O(log N): only the
// requested entry is TTL-checked, and the full map is pruned at most once per
// lazy cadence or when the hard global bound is exceeded (see
// LAST_TIMEOUT_OWNER_PRUNE_INTERVAL_US in net.cpp).
static const int64_t IBD_LAST_TIMEOUT_OWNER_TTL_US = 60LL * 1000000;
static const size_t IBD_LAST_TIMEOUT_OWNER_MAX_HASHES = 2048;

// Late-delivery expectation ledger bounds.  When a request for a hash times
// out, a short-lived expectation is recorded so a later arrival of the block
// (via any peer) can be attributed as a late outcome to the peer that actually
// requested it.  Bounded and TTL-expired like the other quality ledgers; never
// stored in the live in-flight containers.
static const int64_t IBD_LATE_DELIVERY_TTL_US = 60LL * 1000000;
static const size_t IBD_LATE_DELIVERY_MAX_HASHES = 2048;

// Concentration guard: once a single peer holds >= 40% of the global active
// request window (queued + in-flight) and the window sample is large enough,
// fresh admissions are biased toward other eligible lanes.  There is no hard
// global ban: the guard never refuses a request to a dominant peer when no
// other lane exists.
static const int64_t IBD_CONCENTRATION_THRESHOLD_PERMILLE = 400;
static const int64_t IBD_CONCENTRATION_SAMPLE_MIN = 32;

// Per-peer delivery-quality record.  All fields are relaxed atomics: the
// message-handler thread and the network-loop thread both touch them (timeout
// expiry runs from the network loop), and the values are only ever used for
// best-effort scheduling preference, so torn reads are acceptable.
struct IbdPeerDeliveryQuality
{
    std::atomic<uint64_t> requests_issued;       // block requests queued / marked
    std::atomic<uint64_t> releases_by_receive;   // completed deliveries
    std::atomic<uint64_t> releases_by_timeout;   // expirations
    std::atomic<uint64_t> received_after_timeout;// deliveries that arrived late
    std::atomic<uint64_t> latency_sum_us;        // delivery-latency sum
    std::atomic<uint64_t> latency_samples;       // count for latency_sum_us
    std::atomic<int64_t>  latency_ewma_us;       // smoothed delivery latency
    std::atomic<int64_t>  last_delivery_time_us; // last receive (us)
    std::atomic<int64_t>  last_timeout_time_us;  // last timeout (us)
    std::atomic<int64_t>  timeout_score;         // decayed timeout count
    std::atomic<int64_t>  receive_score;         // decayed receive count
    std::atomic<int64_t>  late_delivery_score;   // decayed late-delivery count

    IbdPeerDeliveryQuality();
};

struct IbdPeerQualitySnapshot
{
    IbdPeerQualityTier tier;
    int64_t latency_ewma_us;
    // Whether at least one latency sample was observed.  Consumers must not
    // treat latency_ewma_us == 0 as "measured zero latency"; it means "no
    // sample yet" unless has_latency_sample is true.
    bool has_latency_sample;
    int64_t last_timeout_time_us;
    int64_t last_delivery_time_us;
};

// Record a peer in the alternate-announcer ledger for a hash.  Guarded by
// cs_mapAlreadyAskedFor.  Returns true when a new entry was recorded.
bool RecordAlternateBlockAnnouncer(const uint256& hash, NodeId peer);
// Populate out with the current (non-expired) alternate announcers for a hash.
void GetBlockAlternateAnnouncers(const uint256& hash, std::vector<NodeId>& out);
// Remove all quality-ledger entries that reference a peer (disconnect cleanup):
// the alternate-announcer ledger, the last-timeout-owner ledger, and the
// late-delivery expectation ledger.
size_t RemoveBlockAlternateAnnouncersForPeer(NodeId peer);
// Number of hashes currently holding at least one alternate-announcer entry.
size_t CountBlockAlternateAnnouncerHashes();
// Test hook: clear the whole ledger.
void ResetBlockAlternateAnnouncersForTesting();
// Test hook: force-expire entries older than nNowUs.
void ExpireBlockAlternateAnnouncersForTesting(int64_t nNowUs);

// Record which peer most recently timed out a request for a hash.  Used by the
// exact-hash tier-1 ranking rule (never prefer the peer that failed this hash
// before when an alternative exists).  Guarded by cs_mapAlreadyAskedFor.  The
// map is hard-bounded and lazily pruned (see net.cpp).
void RecordBlockLastTimeoutOwner(const uint256& hash, NodeId peer);
bool GetBlockLastTimeoutOwner(const uint256& hash, NodeId* peer);
// True when the peer was the last owner that timed out a request for the hash.
bool WasBlockLastTimedOutByPeer(const uint256& hash, NodeId peer);
// Number of hashes currently in the last-timeout-owner ledger.
size_t CountBlockLastTimeoutOwnerHashes();
// Test hook: clear the last-timeout-owner ledger.
void ResetBlockLastTimeoutOwnerForTesting();

// Record that a request for a hash timed out on a peer, so a later arrival of
// the block can be attributed as a late outcome.  Guarded by cs_mapAlreadyAskedFor.
void RecordBlockLateDeliveryExpectation(const uint256& hash, NodeId peer,
                                        int64_t markUs, int64_t timeoutUs);
// Consume (and erase) the late-delivery expectation for a hash.  Returns false
// when no expectation is outstanding.
bool TakeBlockLateDeliveryExpectation(const uint256& hash, NodeId* peer,
                                      int64_t* markUs);
// Number of hashes currently in the late-delivery expectation ledger.
size_t CountBlockLateDeliveryExpectationHashes();
// Attribute a late delivery to the peer that originally requested the hash.
// Looks the peer up in vNodes; no-op when the peer is already gone.
void RecordLateDeliveryOutcome(NodeId peer, int64_t latencyUs);
// Test hook: clear the late-delivery expectation ledger.
void ResetLateDeliveryExpectationForTesting();

// Pick the peer that should be asked for a block hash.  Returns NULL to keep
// the announcer (no redirect).  The caller (TryAdmitBlockInvOrDefer and the
// deferred refill) must already hold cs_main.  fExemptFromRedirect reserves
// the frontier semantics (frontier candidate / deferred frontier hash): such
// admissions always keep the announcer verbatim.
CNode* ChooseIbdBlockRequestTarget(CNode* pfrom, const uint256& hash,
                                   bool fExemptFromRedirect,
                                   IbdAskForRedirectReason* pReasonOut,
                                   bool* pNoAlternativeOut);
// Ask for a block inv applying the timeout-aware quality redirection.  Counts
// the redirect / reissue / no-alternative metrics and records the announcer as
// an alternate when the ask is redirected away from it.
AskForResult AskForBlockInvWithQualityRedirection(CNode* pfrom, const CInv& inv,
                                                  BlockRequestTraceSource source,
                                                  bool fExemptFromRedirect);

// Test hook: clear global gauges used by the ranking path.
void ResetIbdQualityStateForTesting();

// Test hook: override the wall-clock used by the quality timestamps so unit
// tests can exercise the decay and TTL paths deterministically.
void SetIbdQualityClockForTesting(int64_t nNowUs);

// Test hook: restore the real clock.
void UnsetIbdQualityClockForTesting();

// Single-slot IBD frontier admission exemption.
//
// During IBD the deferred block-request budget (see
// GetDeferredBlockRequestBudget) can reach zero because a peer's unresolved
// orphan count saturates the per-peer active window.  Every block inv from
// that peer is then deferred and, once the deferred queue is full, dropped --
// including the first block after the active tip that would reconnect the
// orphan forest to the chain.  The frontier exemption lets exactly one such
// announced block enter AskFor while the budget is zero so the connectable
// frontier block can be requested and orphan pressure drained.
//
// The exemption is offered only to the first unknown block inv of a getblocks
// response that was requested with the current active-tip locator, and only
// while the active tip still matches that locator context.  At most one
// frontier candidate can be outstanding at any time, which bounds the DoS
// surface of admitting a block past a zero budget.
//
// The marker is cleared when the block is received, when the owning peer
// releases the request (queue removal, timeout, disconnect), or when a new
// active tip invalidates the locator context.
static const int64_t FRONTIER_ADMISSION_EXPIRE_US = 30LL * 1000000;

bool FrontierCandidateCanAdmit(int64_t nNow, NodeId peer, const uint256& hash,
                               int nTipHeight, int nLocatorHeight);
void ClearFrontierCandidateForBlock(const uint256& hash);
void ClearFrontierCandidateForPeer(NodeId peer);
void ClearFrontierCandidate();
void InvalidateFrontierOnTipChange();

extern NodeId nLastNodeId;
extern CCriticalSection cs_nLastNodeId;

struct LocalServiceInfo {
    int nScore;
    int nPort;
};

extern CCriticalSection cs_mapLocalHost;
extern std::map<CNetAddr, LocalServiceInfo> mapLocalHost;

class CNodeStateStats
{
public:
    int nMisbehavior;
    int nSyncHeight;
    int nCommonHeight;
    std::vector<int> vHeightInFlight;
};

class CNodeStats
{
public:
    NodeId nodeid;
    uint64_t nServices;
    int64_t nLastSend;
    int64_t nLastRecv;
    uint64_t nSendBytes;
    uint64_t nRecvBytes;
    int64_t nTimeConnected;
    int64_t nTimeOffset;
    std::string addrName;
    int nVersion;
    int nTypeInd;
    std::string strSubVer;
    bool fInbound;
    int nChainHeight;
    int nBestKnownHeight;
    std::string hashBestKnownBlock;
    int64_t nLastBlockRecv;
    int64_t nLastHeightUpdate;
    int nBlocksInFlight;
    int nAskForSize;
    int nMisbehavior;
    bool fSyncNode;
    bool fWhitelisted;
    double dPingTime;
    double dPingWait;
    std::string addrLocal;
};




class CNetMessage {
public:
    bool in_data;                   // parsing header (false) or data (true)

    CDataStream hdrbuf;             // partially received header
    CMessageHeader hdr;             // complete header
    unsigned int nHdrPos;

    CDataStream vRecv;              // received message data
    unsigned int nDataPos;

    int64_t nTime;                  // time (in microseconds) of message receipt.

    CNetMessage(int nTypeIn, int nVersionIn) : hdrbuf(nTypeIn, nVersionIn), vRecv(nTypeIn, nVersionIn) {
        hdrbuf.resize(24);
        in_data = false;
        nHdrPos = 0;
        nDataPos = 0;
        nTime = 0;
    }

    bool complete() const
    {
        if (!in_data)
            return false;
        return (hdr.nMessageSize == nDataPos);
    }

    void SetVersion(int nVersionIn)
    {
        hdrbuf.SetVersion(nVersionIn);
        vRecv.SetVersion(nVersionIn);
    }

    int readHeader(const char *pch, unsigned int nBytes);
    int readData(const char *pch, unsigned int nBytes);
};




class SecMsgNode
{
public:
    SecMsgNode()
    {
        lastSeen        = 0;
        lastMatched     = 0;
        ignoreUntil     = 0;
        nWakeCounter    = 0;
        nPeerId         = 0;
        fEnabled        = false;
        lastTypingReceived = 0;
        nTypingViolations  = 0;
    };

    ~SecMsgNode() {};

    int64_t                     lastSeen;
    int64_t                     lastMatched;
    int64_t                     ignoreUntil;
    uint32_t                    nWakeCounter;
    uint32_t                    nPeerId;
    bool                        fEnabled;
    int64_t                     lastTypingReceived;  // Last typing timestamp (rate limiting)
    uint32_t                    nTypingViolations;

};

typedef enum BanReason
{
    BanReasonUnknown          = 0,
    BanReasonNodeMisbehaving  = 1,
    BanReasonManuallyAdded    = 2
} BanReason;

class CBanEntry
{
public:
    static const int CURRENT_VERSION=1;
    int nVersion;
    int64_t nCreateTime;
    int64_t nBanUntil;
    uint8_t banReason;

    CBanEntry()
    {
        SetNull();
    }

    CBanEntry(int64_t nCreateTimeIn)
    {
        SetNull();
        nCreateTime = nCreateTimeIn;
    }

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action, int nType, int nVersion) {
        unsigned int nSerSize = 0;
        READWRITE(this->nVersion);
        nVersion = this->nVersion;
        READWRITE(nCreateTime);
        READWRITE(nBanUntil);
        READWRITE(banReason);
    }

    void SetNull()
    {
        nVersion = CBanEntry::CURRENT_VERSION;
        nCreateTime = 0;
        nBanUntil = 0;
        banReason = BanReasonUnknown;
    }

    std::string banReasonToString()
    {
        switch (banReason) {
            case BanReasonNodeMisbehaving:
                return "node misbehaving";
            case BanReasonManuallyAdded:
                return "manually added";
            default:
                return "unknown";
        }
    }
};

typedef std::map<CSubNet, CBanEntry> banmap_t;

static const int64_t GETHEADERS_REQUEST_TIMEOUT = 60;

// Seconds a flushed single-flight getblocks request may remain unanswered
// before its slot is released.  15 s is conservative: comfortably above the
// expected getblocks->inv round trip on a slow link yet at or below the
// default stalled-sync timeout (-syncstalltimeout, default 15 s) so the slot
// re-arms before/around the moment recovery would also be triggered.  The
// timeout is intentionally not refreshed by pings/tx traffic: it tracks the
// specific outstanding getblocks cycle, matching the failure class of a peer
// that is TCP-alive but never answers an inv.
static const int64_t GETBLOCKS_RESPONSE_TIMEOUT = 15;

class CGetHeadersSyncState
{
public:
    enum StartResult
    {
        STARTED,
        RETRIED_AFTER_TIMEOUT,
        SUPPRESSED_ACTIVE,
        SUPPRESSED_COMPLETED
    };

    CGetHeadersSyncState();

    StartResult Start(const std::string& strRequestKey, int64_t nNow,
                      int64_t nTimeout = GETHEADERS_REQUEST_TIMEOUT);
    bool Complete(int64_t nNow);
    bool IsInFlight() const;
    bool IsTimedOut(int64_t nNow,
                    int64_t nTimeout = GETHEADERS_REQUEST_TIMEOUT) const;
    int64_t LastRequestAge(int64_t nNow) const;
    uint64_t RequestSequence() const;

private:
    mutable CCriticalSection cs_state;
    bool fInFlight;
    bool fHasCompleted;
    bool fHasLastRequest;
    std::string strActiveRequestKey;
    std::string strLastCompletedRequestKey;
    int64_t nActiveSince;
    int64_t nLastCompletedTime;
    int64_t nLastRequestTime;
    uint64_t nRequestSequence;
};

// Parallel metadata for one entry of CNode::vSendMsg.  The two deques are
// pushed together in EndMessage (under cs_vSend) and erased together in
// SocketSendData, so they stay exactly aligned: vSendMeta[i] describes
// vSendMsg[i].  This exists only for observation (getdata first-send
// attribution); it never changes what or when bytes are written.
struct SendMessageMeta
{
    std::string command;         // p2p command of the message
    int64_t firstSendUs;         // wall-clock of the first successful send() (0 = pending)
    size_t nSendSizeAtFirstSend; // peer nSendSize at that first send (0 = pending)
    bool fStampPending;          // true until the first byte of this message is written
    // For a getdata message: the block hashes it carries, in wire order,
    // associated so the socket-send path can stamp each in-flight hash's
    // wire-origin time.  Empty for every other message type.
    std::vector<uint256> vBlockHashes;

    SendMessageMeta()
        : firstSendUs(0), nSendSizeAtFirstSend(0), fStampPending(true)
    {
    }
};

// How a CNode's state is being torn down.
//
// NODE_CLEANUP_RUNTIME is the normal disconnect path (scheduler still running):
// ownership release records forensic/observation callbacks as usual.
//
// NODE_CLEANUP_FINAL_TEARDOWN runs from CNetCleanup::~CNetCleanup during exit()
// static destruction.  It still frees scheduler state (ownership, outstanding
// getblocks) but skips forensic and other observation callbacks, whose static
// mutexes and containers may already be destroyed by the time the terminal
// destructor runs.
enum NodeCleanupMode
{
    NODE_CLEANUP_RUNTIME = 0,
    NODE_CLEANUP_FINAL_TEARDOWN
};

// True only while CNetCleanup::~CNetCleanup is running during exit() static
// destruction.  CNode::~CNode consults this to pick the cleanup mode so that
// terminal deletes free scheduler state without invoking forensic (or other
// observation) callbacks, whose static state may already be destroyed.
bool InFinalNodeTeardown();

/** Information about a peer */
class CNode
{
public:
    // socket
    uint64_t nServices;
    SOCKET hSocket;
    CDataStream ssSend;
    std::string strMessageCommand;
    size_t nSendSize; // total size of all vSendMsg entries
    size_t nSendOffset; // offset inside the first vSendMsg already sent
    std::deque<CSerializeData> vSendMsg;
    std::deque<SendMessageMeta> vSendMeta; // parallel to vSendMsg (cs_vSend)
    CCriticalSection cs_vSend;
    CCriticalSection cs_vRecv;
	std::deque<CInv> vRecvGetData;
    std::deque<CNetMessage> vRecvMsg;
    CCriticalSection cs_vRecvMsg;
    int nRecvVersion;

    int64_t nLastSend;
    int64_t nLastRecv;

    uint64_t nSendBytes;
    uint64_t nRecvBytes;

    int64_t nLastSendEmpty;
    int64_t nTimeConnected;
    int64_t nLastDseg;
    CAddress addr;
    std::string addrName;
    CService addrLocal;
    int nVersion;
    std::string strSubVer;
    bool fWhitelisted; // This peer can bypass DoS banning.
    bool fOneShot;
    bool fClient;
    bool fInbound;
    bool fVerified;
    bool fNetworkNode;
    bool fSuccessfullyConnected;
    bool fDisconnect;
	// We use fRelayTxes for two purposes -
    // a) it allows us to not relay tx invs before receiving the peer's version message
    // b) the peer may tell us in their version message that we should not relay tx invs
    //    until they have initialized their bloom filter.
    bool fRelayTxes;
    bool fPreferHeaders;
    bool fColLateralMaster;
    CBloomFilter* pfilter;
    CCriticalSection cs_filter;
    CSemaphoreGrant grantOutbound;
    int nRefCount;
	NodeId id;
protected:
    // Denial-of-service detection/prevention
    // Key is IP address, value is banned-until-time
    static banmap_t setBanned;
    static CCriticalSection cs_setBanned;
    static bool setBannedIsDirty;

    // Whitelisted ranges. Any node connecting from these is automatically
    // whitelisted (as well as those connecting to whitelisted binds).
    static std::vector<CSubNet> vWhitelistedRange;
    static CCriticalSection cs_vWhitelistedRange;

    // Whitelisted ranges. Any node connecting from these is automatically
    // whitelisted (as well as those connecting to whitelisted binds).
    //static std::vector<CSubNet> vWhitelistedRange;
    //static CCriticalSection cs_vWhitelistedRange;

	std::vector<std::string> vecRequestsFulfilled; //keep track of what client has asked for

public:
    std::map<uint256, CRequestTracker> mapRequests;
    CCriticalSection cs_mapRequests;
    uint256 hashContinue;
    CGetBlocksServerState getBlocksServer;
    CBlockIndex* pindexLastGetBlocksBegin;
    RecoveryResponseWindowState recovery_response_window;
    std::vector<CBlockIndex*> getBlocksIndex;
    std::vector<uint256> getBlocksHash;
    std::vector<uint64_t> getBlocksRecoveryIds;
    std::vector<ibdmetrics::GetBlocksSource> getBlocksSources;
    // Single-flight getblocks lifecycle: at most one flushed outstanding
    // request per peer.  The pending queue (getBlocksIndex/... above) is
    // coalesced by PushGetBlocks to at most one meaningful request.
    // The response is heuristic — the next inv message from this peer closes
    // the cycle — because the wire protocol carries no request id.  A finite
    // timeout (ExpireGetBlocksOutstanding) releases the slot when no inv
    // arrives, so a silent-but-alive peer can never wedge the peer forever.
    struct GetBlocksOutstandingState
    {
        bool active;
        ibdmetrics::GetBlocksSource source;
        int64_t sent_time_us;
        CBlockIndex* locator_begin;
        uint256 hash_stop;

        GetBlocksOutstandingState()
            : active(false), source(ibdmetrics::GETBLOCKS_SOURCE_OTHER),
              sent_time_us(0), locator_begin(NULL), hash_stop(0)
        {
        }

        void Reset()
        {
            active = false;
            source = ibdmetrics::GETBLOCKS_SOURCE_OTHER;
            sent_time_us = 0;
            locator_begin = NULL;
            hash_stop = 0;
        }
    };
    GetBlocksOutstandingState getBlocksOutstanding;
    uint64_t nRecoveryTracePendingId;
    uint256 hashLastGetBlocksEnd;
    int64_t nLastGetBlocksTime;
    // Set while a getblocks carrying the current active-tip locator has been
    // flushed and its inv response has not yet been consumed.  The first
    // unknown block inv of that response is eligible for the single-slot IBD
    // frontier admission exemption.
    bool fFrontierResponsePending;
    // Active-tip height at the moment the frontier getblocks was flushed; the
    // exemption is refused if the tip advances before the response arrives.
    int nFrontierLocatorHeight;
    // Diagnostics: 1 while the deferred block-request budget is zero for this
    // peer (see IBDMetricsPeerZeroStateChange); maintained as a relaxed atomic
    // transition mirror, not used for scheduling.
    std::atomic<int> nDeferredBudgetZero;
    bool fIbdMetricsCleanupAccounted;
    int64_t nLastGetDataTime;
    int nChainHeight;
    int nBestKnownHeight;
    uint256 hashBestKnownBlock;
    int64_t nLastHeightUpdate;
	bool fStartSync;
    bool fInitialSyncRequestPending;
    bool fInitialSyncRequestSent;
    int64_t nLastBlockRecv;

    // Experiment A (future-supply diversification) state.  peerLiveActivePressure
    // mirrors setAskForBlocks.size() + setBlocksInFlight.size(), updated at the
    // same six loci that maintain the counted metrics; a relaxed atomic written
    // only by the message-handler thread.  peerDiversifySeq is the round-robin
    // tie-break tick for lane selection among equal-pressure eligible peers.
    // nFrontierDeferredHash is the hash of the single frontier candidate
    // deferred into this peer's deferred pool (0 = none); it is exempt from
    // diversification and always re-requested from the announcer.
    std::atomic<int32_t> peerLiveActivePressure;
    uint64_t peerDiversifySeq;
    uint256 nFrontierDeferredHash;

    int nBlocksReceivedInBatch;
    int nExpectedBatchSize;
    bool fPrefetchSent;
    uint256 hashLastBlockInBatch;
	CGetHeadersSyncState getHeadersSync;
	int nMisbehavior;
    mutable CCriticalSection cs_nMisbehavior;

    // flood relay
    std::vector<CAddress> vAddrToSend;
    mruset<CAddress> setAddrKnown;
    bool fGetAddr;
    std::set<uint256> setKnown;
    uint256 hashCheckpointKnown; // ppcoin: known sent sync-checkpoint

    // inventory based relay
    mruset<CInv> setInventoryKnown;
    std::vector<CInv> vInventoryToSend;
    std::vector<CInv> vGetBlocksInventoryToSend;
    CCriticalSection cs_inventory;
    std::multimap<int64_t, CInv> mapAskFor;
    std::set<uint256> setAskForBlocks;
    std::deque<uint256> deferredBlockInv;
    std::set<uint256> deferredBlockInvIndex;

    std::set<uint256> setBlocksInFlight;
    std::map<uint256, int64_t> mapBlockInFlightSince;
    // Parallel to mapBlockInFlightSince with microsecond precision for the
    // delivery-latency EWMA; maintained on the same loci.
    std::map<uint256, int64_t> mapBlockInFlightMarkUs;
    // Wire-origin clock for the conservative block-request expiry (see
    // doc/design/ibd-conservative-block-request-expiration.md): for a hash in
    // flight, the wall-clock microseconds at which its getdata batch was first
    // written to the socket (0 = pending wire).  Written by the socket handler
    // (SocketSendData) and by the message handler (Mark/Clear/Expire), so it is
    // guarded by cs_vBlockInFlightWire; unlike the maps above, this one is not
    // single-thread.
    std::map<uint256, int64_t> mapBlockInFlightWireUs;
    CCriticalSection cs_vBlockInFlightWire;
    // Staging for the block hashes of the getdata message currently being
    // serialized; moved into SendMessageMeta::vBlockHashes by EndMessage.
    // Guarded by cs_vSend.
    std::vector<uint256> vPendingGetDataHashes;
    // Per-peer delivery quality used by the timeout-aware IBD ranking.  See
    // the IbdPeerDeliveryQuality / IbdPeerQualityTier documentation above.
    IbdPeerDeliveryQuality ibdQuality;

    SecMsgNode smsgData;

    // Ping time measurement:
    // The pong reply we're expecting, or 0 if no pong expected.
    uint64_t nPingNonceSent;
    // Time (in usec) the last ping was sent, or 0 if no ping was ever sent.
    int64_t nPingUsecStart;
    // Last measured round-trip time.
    int64_t nPingUsecTime;
    // Whether a ping is requested.
    bool fPingQueued;
    // Observation only: time (usec) the current ping message was appended to
    // vSendMsg (0 = none pending).  Used only by -pinglifecycletrace; it never
    // affects scheduling, the timeout, or peer selection.
    int64_t nPingQueuedUsec;

    uint64_t nInvCount;        // Count of inv items in current window
    int64_t nInvWindowStart;   // Start time of current rate limit window (seconds)

    CNode(SOCKET hSocketIn, CAddress addrIn, std::string addrNameIn = "", bool fInboundIn=false) : ssSend(SER_NETWORK, INIT_PROTO_VERSION), setAddrKnown(5000)
    {
        nServices = 0;
        hSocket = hSocketIn;
        nRecvVersion = INIT_PROTO_VERSION;
        nLastSend = 0;
        nLastRecv = 0;
        nSendBytes = 0;
        nRecvBytes = 0;
        nLastSendEmpty = GetTime();
        nTimeConnected = GetTime();
        addr = addrIn;
        addrName = addrNameIn == "" ? addr.ToStringIPPort() : addrNameIn;
        nVersion = 0;
        strSubVer = "";
        fOneShot = false;
        fClient = false; // set by version message
        fInbound = fInboundIn;
        fVerified = false;
        fWhitelisted = false;
        fNetworkNode = false;
        fSuccessfullyConnected = false;
        fDisconnect = false;
        nRefCount = 0;
        {
            LOCK(cs_nLastNodeId);
            id = ++nLastNodeId;
        }
        nSendSize = 0;
        nSendOffset = 0;
        hashContinue = 0;
        pindexLastGetBlocksBegin = 0;
        nRecoveryTracePendingId = 0;
        hashLastGetBlocksEnd = 0;
        nLastGetBlocksTime = 0;
        getBlocksOutstanding.Reset();
        fInitialSyncRequestPending = false;
        fInitialSyncRequestSent = false;
        fFrontierResponsePending = false;
        nFrontierLocatorHeight = -1;
        nDeferredBudgetZero = 0;
        fIbdMetricsCleanupAccounted = false;
        peerLiveActivePressure = 0;
        peerDiversifySeq = 0;
        nFrontierDeferredHash = 0;
        nLastGetDataTime = 0;
        nChainHeight = -1;
        nBestKnownHeight = -1;
        hashBestKnownBlock = 0;
        nLastHeightUpdate = 0;
        fStartSync = false;
        nLastBlockRecv = 0;
        nBlocksReceivedInBatch = 0;
        nExpectedBatchSize = 0;
        fPrefetchSent = false;
        hashLastBlockInBatch = 0;
        fGetAddr = false;
        nMisbehavior = 0;
        hashCheckpointKnown = 0;
        setInventoryKnown.max_size(SendBufferSize() / 1000);
        nPingNonceSent = 0;
        nPingUsecStart = 0;
        nPingUsecTime = 0;
        fPingQueued = false;
        nPingQueuedUsec = 0;
        nInvCount = 0;
        nInvWindowStart = GetTime();
        fColLateralMaster = false;
        fRelayTxes = false;
        fPreferHeaders = false;
        pfilter = NULL;
        nLastDseg = GetTime();

        if (hSocket != INVALID_SOCKET)
            PushVersion();
    }

    ~CNode()
    {
        Cleanup(InFinalNodeTeardown() ? NODE_CLEANUP_FINAL_TEARDOWN
                                      : NODE_CLEANUP_RUNTIME);
        if (BlockRequestTraceEnabled())
            BlockRequestTracePeerClosed(this);
        if (PingLifecycleTraceEnabled())
            PingLifecycleTracePeerClosed(this);
        RecoveryResponseResult recoveryResult;
        if (DisconnectRecoveryResponseWindow(GetTimeMicros(), recoveryResult) &&
            BlockRequestTraceEnabled())
            printf("%s\n", FormatRecoveryResponseSummary(
                GetId(), recoveryResult).c_str());
        if (hSocket != INVALID_SOCKET)
        {
            closesocket(hSocket);
            hSocket = INVALID_SOCKET;
        }
        if (pfilter)
        {
            delete pfilter;
            pfilter = NULL;
        }
    }

private:
    CNode(const CNode&);
    void operator=(const CNode&);

    // Network usage totals
    static CCriticalSection cs_totalBytesRecv;
    static CCriticalSection cs_totalBytesSent;
    static uint64_t nTotalBytesRecv;
    static uint64_t nTotalBytesSent;

    // outbound limit & stats
    static uint64_t nMaxOutboundTotalBytesSentInCycle;
    static uint64_t nMaxOutboundCycleStartTime;
    static uint64_t nMaxOutboundLimit;
    static uint64_t nMaxOutboundTimeframe;
public:
	NodeId GetId() const {
      return id;
    }

    int GetRefCount()
    {
        assert(nRefCount >= 0);
        return nRefCount;
    }

    // requires LOCK(cs_vRecvMsg)
    unsigned int GetTotalRecvSize()
    {
        unsigned int total = 0;
        for (const CNetMessage &msg : vRecvMsg)
            total += msg.vRecv.size() + 24;
        return total;
    }

    // requires LOCK(cs_vRecvMsg)
    bool ReceiveMsgBytes(const char *pch, unsigned int nBytes);

    // requires LOCK(cs_vRecvMsg)
    void SetRecvVersion(int nVersionIn)
    {
        nRecvVersion = nVersionIn;
        for (CNetMessage &msg : vRecvMsg)
            msg.SetVersion(nVersionIn);
    }

    CNode* AddRef()
    {
        nRefCount++;
        return this;
    }

    void Release()
    {
        nRefCount--;
    }



    void AddAddressKnown(const CAddress& addr)
    {
        setAddrKnown.insert(addr);
    }

    void PushAddress(const CAddress& addr)
    {
        // Known checking here is only to save space from duplicates.
        // SendMessages will filter it again for knowns that were added
        // after addresses were pushed.
        static const size_t MAX_ADDR_TO_SEND = 1000;
        if (addr.IsValid() && !setAddrKnown.count(addr) && vAddrToSend.size() < MAX_ADDR_TO_SEND)
            vAddrToSend.push_back(addr);
    }


    void AddInventoryKnown(const CInv& inv)
    {
        {
            LOCK(cs_inventory);
            setInventoryKnown.insert(inv);
        }
    }

    void PushInventory(const CInv& inv)
    {
        {
            LOCK(cs_inventory);
            static const size_t MAX_INV_TO_SEND = 50000;
            if (!setInventoryKnown.count(inv) && vInventoryToSend.size() < MAX_INV_TO_SEND)
                vInventoryToSend.push_back(inv);
        }
    }

    bool PushGetBlocksInventory(const CInv& inv)
    {
        LOCK(cs_inventory);
        static const size_t MAX_INV_TO_SEND = 50000;
        if (vGetBlocksInventoryToSend.size() >= MAX_INV_TO_SEND)
            return false;
        vGetBlocksInventoryToSend.push_back(inv);
        return true;
    }

    bool IsBlockAskForQueued(const uint256& hash) const
    {
        return setAskForBlocks.count(hash) != 0;
    }

    bool IsBlockInvDeferred(const uint256& hash) const
    {
        return deferredBlockInvIndex.count(hash) != 0;
    }

    bool DeferBlockInv(const uint256& hash)
    {
        if (deferredBlockInvIndex.count(hash) != 0)
            return false;
        if (deferredBlockInv.size() >= MAX_DEFERRED_BLOCK_INV_PER_PEER)
            return false;
        deferredBlockInv.push_back(hash);
        deferredBlockInvIndex.insert(hash);
        ibdmetrics::DeferredAdd(1);
        if (BlockRequestTraceEnabled())
            BlockRequestTraceDeferredWatermark(
                GetId(), (int)deferredBlockInv.size(), "add");
        return true;
    }

    bool RemoveDeferredBlockInv(const uint256& hash)
    {
        if (deferredBlockInvIndex.erase(hash) == 0)
            return false;
        if (nFrontierDeferredHash == hash)
            nFrontierDeferredHash = 0;
        for (std::deque<uint256>::iterator it = deferredBlockInv.begin();
             it != deferredBlockInv.end(); ++it)
        {
            if (*it == hash)
            {
                deferredBlockInv.erase(it);
                ibdmetrics::DeferredAdd(-1);
                break;
            }
        }
        if (BlockRequestTraceEnabled())
            BlockRequestTraceDeferredWatermark(
                GetId(), (int)deferredBlockInv.size(), "remove");
        return true;
    }

    void PopFrontDeferredBlockInv()
    {
        if (deferredBlockInv.empty())
            return;
        const uint256 hash = deferredBlockInv.front();
        if (nFrontierDeferredHash == hash)
            nFrontierDeferredHash = 0;
        deferredBlockInvIndex.erase(hash);
        deferredBlockInv.pop_front();
        ibdmetrics::DeferredAdd(-1);
        if (BlockRequestTraceEnabled())
            BlockRequestTraceDeferredWatermark(
                GetId(), (int)deferredBlockInv.size(), "remove");
    }

    void RotateFrontDeferredBlockInv()
    {
        if (deferredBlockInv.size() <= 1)
            return;
        const uint256 hash = deferredBlockInv.front();
        deferredBlockInv.pop_front();
        deferredBlockInv.push_back(hash);
    }

    void AddAskForEntry(int64_t nRequestTime, const CInv& inv)
    {
        mapAskFor.insert(std::make_pair(nRequestTime, inv));
        if (inv.type == MSG_BLOCK || inv.type == MSG_FILTERED_BLOCK)
        {
            if (setAskForBlocks.insert(inv.hash).second)
            {
                peerLiveActivePressure.fetch_add(
                    1, std::memory_order_relaxed);
                ibdmetrics::QueuedAdd(1);
                ibdmetrics::GlobalActiveAdd(1);
            }
        }
    }

    void AddAskForEntry(const std::pair<int64_t, CInv>& entry)
    {
        AddAskForEntry(entry.first, entry.second);
    }

    void EraseAskForEntry(std::multimap<int64_t, CInv>::iterator it,
                          bool fReleaseOwner = true,
                          ibdmetrics::ActiveDecrementCause cause =
                              ibdmetrics::ACTIVE_DECREMENT_OTHER)
    {
        if (it == mapAskFor.end())
            return;
        const CInv inv = it->second;
        if (inv.type == MSG_BLOCK || inv.type == MSG_FILTERED_BLOCK)
        {
            if (setAskForBlocks.erase(inv.hash))
            {
                peerLiveActivePressure.fetch_sub(
                    1, std::memory_order_relaxed);
                if (cause != ibdmetrics::ACTIVE_DECREMENT_ASKFOR_SENT_TRANSITION)
                    ClearDiversifyDispatch(inv.hash);
                ibdmetrics::QueuedAdd(-1);
                ibdmetrics::GlobalActiveAdd(-1, cause);
                if (cause != ibdmetrics::ACTIVE_DECREMENT_ASKFOR_SENT_TRANSITION)
                {
                    uint32_t nWakeCause = WAKE_CAUSE_QUEUE_REMOVAL;
                    if (cause == ibdmetrics::ACTIVE_DECREMENT_ASKFOR_REMOVED_ALREADY_HAVE)
                        nWakeCause = WAKE_CAUSE_ASKFOR_ALREADY_HAVE;
                    else if (cause == ibdmetrics::ACTIVE_DECREMENT_ASKFOR_REMOVED_OWNER_CONFLICT)
                        nWakeCause = WAKE_CAUSE_ASKFOR_OWNER_CONFLICT;
                    else if (cause == ibdmetrics::ACTIVE_DECREMENT_CLEAR_ASKFOR)
                        nWakeCause = WAKE_CAUSE_CLEAR_ASKFOR;
                    else if (cause == ibdmetrics::ACTIVE_DECREMENT_DISCONNECT_CLEANUP)
                        nWakeCause = WAKE_CAUSE_DISCONNECT_CLEANUP;
                    else if (cause == ibdmetrics::ACTIVE_DECREMENT_OTHER)
                        nWakeCause = WAKE_CAUSE_OTHER;
                    RequestBlockPipelineWake(nWakeCause);
                }
            }
        }
        mapAskFor.erase(it);
        if (fReleaseOwner &&
            (inv.type == MSG_BLOCK || inv.type == MSG_FILTERED_BLOCK))
            ReleaseBlockRequestOwner(inv.hash, GetId(), "queue-removal");
    }

    void ClearAskFor()
    {
        for (std::multimap<int64_t, CInv>::const_iterator it = mapAskFor.begin();
             it != mapAskFor.end(); ++it)
        {
            if (it->second.type == MSG_BLOCK ||
                it->second.type == MSG_FILTERED_BLOCK)
                ReleaseBlockRequestOwner(it->second.hash, GetId(), "clear");
        }
        if (!setAskForBlocks.empty())
        {
            peerLiveActivePressure = 0;
            if (IsFutureSupplyDiversificationEnabled())
            {
                for (std::set<uint256>::const_iterator si =
                         setAskForBlocks.begin();
                     si != setAskForBlocks.end(); ++si)
                    ClearDiversifyDispatch(*si);
            }
            ibdmetrics::QueuedAdd(-(int64_t)setAskForBlocks.size());
            ibdmetrics::GlobalActiveAdd(
                -(int64_t)setAskForBlocks.size(),
                ibdmetrics::ACTIVE_DECREMENT_CLEAR_ASKFOR);
            RequestBlockPipelineWake(WAKE_CAUSE_CLEAR_ASKFOR);
        }
        mapAskFor.clear();
        setAskForBlocks.clear();
    }

    AskForResult AskFor(const CInv& inv, BlockRequestTraceSource source = BLOCKREQ_SOURCE_OTHER)
    {
        const int64_t nPruneNow = GetTimeMicros();
        PruneAlreadyAskedFor(nPruneNow);
        bool fBlockRequest = (inv.type == MSG_BLOCK || inv.type == MSG_FILTERED_BLOCK);
        if (fBlockRequest)
        {
            // EXPTRACE HOOK: explicit orphan-parent walk-back ask.
            if (source == BLOCKREQ_SOURCE_ORPHAN)
                ibdexptrace::NoteOrphanParentAsk(inv.hash);
            ExpireBlockInFlight();
            if (setBlocksInFlight.count(inv.hash))
            {
                ibdmetrics::Get().askfor_skip_inflight.fetch_add(
                    1, std::memory_order_relaxed);
                return ASKFOR_INFLIGHT;
            }
            if (IsBlockAskForQueued(inv.hash))
            {
                if (BlockRequestTraceEnabled())
                    BlockRequestTraceAskSkip(this, inv.hash, source,
                                             "same-peer-already-queued");
                ibdmetrics::Get().askfor_skip_already_queued.fetch_add(
                    1, std::memory_order_relaxed);
                return ASKFOR_ALREADY_QUEUED;
            }
        }

        if (fBlockRequest &&
            source != BLOCKREQ_SOURCE_ORPHAN_LIMIT_RETRY &&
            IsOrphanLimitRejectedBlockInCooldown(GetId(), inv, nPruneNow))
        {
            if (BlockRequestTraceEnabled())
                BlockRequestTraceAskSkip(this, inv.hash, source,
                                         "orphan-limit-cooldown");
            ibdmetrics::Get().askfor_skip_orphan_limit_cooldown.fetch_add(
                1, std::memory_order_relaxed);
            return ASKFOR_COOLDOWN;
        }
        if (fBlockRequest &&
            IsOrphanLimitRejectedByOtherPeer(GetId(), inv, nPruneNow))
        {
            // This peer is not the one that saturated its orphan window for
            // this hash; the peer-local cooldown belongs to another peer, so
            // the request is admitted and counted as cross-peer recovery.
            ibdmetrics::Get().orphan_limit_cross_peer_admitted.fetch_add(
                1, std::memory_order_relaxed);
        }

        LOCK(cs_mapAlreadyAskedFor);
        if (mapAlreadyAskedFor.size() >= MAX_ALREADY_ASKED_FOR_SIZE)
        {
            if (fDebugNet)
                printf("AskFor: mapAlreadyAskedFor full (%u entries), skipping %s\n",
                       (unsigned int)mapAlreadyAskedFor.size(), inv.ToString().c_str());
            if (BlockRequestTraceEnabled())
                printf("BLOCKREQTRACE time_us=%lld event=ALREADY_ASKED_CAP_SKIP peer=%d type=%d hash=%s size=%zu cap=%zu\n",
                       (long long)nPruneNow, GetId(), inv.type,
                       inv.hash.ToString().c_str(), mapAlreadyAskedFor.size(),
                       MAX_ALREADY_ASKED_FOR_SIZE);
            ibdmetrics::Get().askfor_skip_mapalreadyasked_cap.fetch_add(
                1, std::memory_order_relaxed);
            return ASKFOR_CAP_FULL;
        }

        NodeId nOwnerPeer = -1;
        BlockRequestOwnerState ownerState = BLOCK_REQUEST_OWNER_QUEUED;
        if (fBlockRequest &&
            GetBlockRequestOwner(inv.hash, &nOwnerPeer, &ownerState) &&
            nOwnerPeer != GetId())
        {
            if (BlockRequestTraceEnabled())
                BlockRequestTraceAskSkip(this, inv.hash, source,
                                         "other-peer-active-owner",
                                         nOwnerPeer,
                                         BlockRequestOwnerStateName(ownerState));
            ibdmetrics::Get().askfor_skip_other_peer_owner.fetch_add(
                1, std::memory_order_relaxed);
            // This peer announced a hash that another peer owns: strong
            // evidence it can serve the block, so remember it as an
            // alternative for a later timeout reassignment.
            RecordAlternateBlockAnnouncer(inv.hash, GetId());
            return ASKFOR_OWNED_BY_OTHER;
        }

        // We're using mapAskFor as a priority queue,
        // the key is the earliest time the request can be sent
        int64_t& nRequestTime = mapAlreadyAskedFor[inv];
        int64_t nPreviousRequestTime = nRequestTime;
        if (fDebugNet)
            printf("askfor %s   %lld (%s)\n", inv.ToString().c_str(), (long long)nRequestTime, DateTimeStrFormat("%H:%M:%S", nRequestTime/1000000).c_str());

        // Make sure not to reuse time indexes to keep things in the same order
        int64_t nNow = (GetTime() - 1) * 1000000;
        static int64_t nLastTime;
        ++nLastTime;
        nNow = std::max(nNow, nLastTime);
        nLastTime = nNow;

        if (fBlockRequest)
        {
            static const int64_t BLOCK_ASK_RETRY_US = 1000000;
            static const int64_t BLOCK_ASK_DEFER_US = 250000;
            if (setBlocksInFlight.size() >= (size_t)GetMaxActiveBlockRequestsPerPeer())
                nRequestTime = std::max(nRequestTime + BLOCK_ASK_RETRY_US, nNow + BLOCK_ASK_DEFER_US);
            else
                nRequestTime = std::max(nRequestTime + BLOCK_ASK_RETRY_US, nNow);
        }
        else
        {
            nRequestTime = std::max(nRequestTime + 10 * 1000000, nNow);
        }
        AddAskForEntry(nRequestTime, inv);
        if (fBlockRequest)
            ibdactivepath::RecordBlockRequestEnqueued(inv.hash);
        if (fBlockRequest)
            ibdblocklatency::RecordAskForEnqueue(
                inv.hash, GetId(),
                peerLiveActivePressure.load(std::memory_order_relaxed));
        if (fBlockRequest)
            ibdmetrics::RecordZeroLatency(ibdmetrics::ZERO_LATENCY_ASKFOR);
        if (BlockRequestTraceEnabled() && inv.type == MSG_BLOCK)
            BlockRequestTraceAskSchedule(this, inv.hash, source, nRequestTime,
                                         nPreviousRequestTime, false);
        return ASKFOR_QUEUED;
    }



    void BeginMessage(const char* pszCommand) EXCLUSIVE_LOCK_FUNCTION(cs_vSend)
    {
        ENTER_CRITICAL_SECTION(cs_vSend);
        assert(ssSend.size() == 0);
        strMessageCommand = pszCommand;
        ssSend << CMessageHeader(pszCommand, 0);
        if (fDebugNet)
            printf("net: to %s: %s ", this->addr.ToString().c_str(), pszCommand);
    }

    void AbortMessage() UNLOCK_FUNCTION(cs_vSend)
    {
        ssSend.clear();
        strMessageCommand.clear();
        vPendingGetDataHashes.clear();

        LEAVE_CRITICAL_SECTION(cs_vSend);

        if (fDebugNet)
            printf("(aborted)\n");
    }

    void EndMessage() UNLOCK_FUNCTION(cs_vSend)
    {
        if (mapArgs.count("-dropmessagestest") && GetRand(atoi(mapArgs["-dropmessagestest"])) == 0)
        {
            printf("dropmessages DROPPING SEND MESSAGE\n");
            AbortMessage();
            return;
        }

        if (ssSend.size() == 0)
            return;

        // Set the size
        unsigned int nSize = ssSend.size() - CMessageHeader::HEADER_SIZE;
        memcpy((char*)&ssSend[CMessageHeader::MESSAGE_SIZE_OFFSET], &nSize, sizeof(nSize));

        // Set the checksum
        uint256 hash = Hash(ssSend.begin() + CMessageHeader::HEADER_SIZE, ssSend.end());
        unsigned int nChecksum = 0;
        memcpy(&nChecksum, &hash, sizeof(nChecksum));
        assert(ssSend.size () >= CMessageHeader::CHECKSUM_OFFSET + sizeof(nChecksum));
        memcpy((char*)&ssSend[CMessageHeader::CHECKSUM_OFFSET], &nChecksum, sizeof(nChecksum));

        if (fDebugNet) {
            printf("(%d bytes)\n", nSize);
        }

        std::deque<CSerializeData>::iterator it = vSendMsg.insert(vSendMsg.end(), CSerializeData());
        ssSend.GetAndClear(*it);
        nSendSize += (*it).size();
        vSendMeta.push_back(SendMessageMeta());
        vSendMeta.back().command = strMessageCommand;
        // Associate the block hashes carried by this getdata message so the
        // socket-send path can stamp each in-flight hash's wire-origin time.
        // The staging vector is only non-empty while PushBlockGetData is
        // serializing a batch; it is swapped (emptied) here, so a getdata built
        // through plain PushMessage carries no hash association.
        if (strMessageCommand == "getdata" && !vPendingGetDataHashes.empty())
            vSendMeta.back().vBlockHashes.swap(vPendingGetDataHashes);
        else
            vPendingGetDataHashes.clear();

        // If write queue empty, attempt "optimistic write"
        if (it == vSendMsg.begin())
            SocketSendData(this);

        RecordP2PMessageStat(this, strMessageCommand, nSize, false);
        strMessageCommand.clear();

        LEAVE_CRITICAL_SECTION(cs_vSend);
    }

    void PushVersion();


    // Send a getdata batch, recording the block hashes it carries so the
    // socket-send path can stamp each in-flight hash's wire-origin time.  Same
    // serialization path as PushMessage("getdata", vGetData); the extra
    // association is observation only and never changes what or when bytes are
    // written.
    void PushBlockGetData(const std::vector<CInv>& vGetData)
    {
        try
        {
            BeginMessage("getdata");
            vPendingGetDataHashes.clear();
            for (size_t i = 0; i < vGetData.size(); ++i)
            {
                if (vGetData[i].type == MSG_BLOCK ||
                    vGetData[i].type == MSG_FILTERED_BLOCK)
                    vPendingGetDataHashes.push_back(vGetData[i].hash);
            }
            ssSend << vGetData;
            EndMessage();
        }
        catch (...)
        {
            AbortMessage();
            throw;
        }
    }

    void PushMessage(const char* pszCommand)
    {
        try
        {
            BeginMessage(pszCommand);
            EndMessage();
        }
        catch (...)
        {
            AbortMessage();
            throw;
        }
    }

    template<typename T1>
    void PushMessage(const char* pszCommand, const T1& a1)
    {
        try
        {
            BeginMessage(pszCommand);
            ssSend << a1;
            EndMessage();
        }
        catch (...)
        {
            AbortMessage();
            throw;
        }
    }

    template<typename T1, typename T2>
    void PushMessage(const char* pszCommand, const T1& a1, const T2& a2)
    {
        try
        {
            BeginMessage(pszCommand);
            ssSend << a1 << a2;
            EndMessage();
        }
        catch (...)
        {
            AbortMessage();
            throw;
        }
    }

    template<typename T1, typename T2, typename T3>
    void PushMessage(const char* pszCommand, const T1& a1, const T2& a2, const T3& a3)
    {
        try
        {
            BeginMessage(pszCommand);
            ssSend << a1 << a2 << a3;
            EndMessage();
        }
        catch (...)
        {
            AbortMessage();
            throw;
        }
    }

    template<typename T1, typename T2, typename T3, typename T4>
    void PushMessage(const char* pszCommand, const T1& a1, const T2& a2, const T3& a3, const T4& a4)
    {
        try
        {
            BeginMessage(pszCommand);
            ssSend << a1 << a2 << a3 << a4;
            EndMessage();
        }
        catch (...)
        {
            AbortMessage();
            throw;
        }
    }

    template<typename T1, typename T2, typename T3, typename T4, typename T5>
    void PushMessage(const char* pszCommand, const T1& a1, const T2& a2, const T3& a3, const T4& a4, const T5& a5)
    {
        try
        {
            BeginMessage(pszCommand);
            ssSend << a1 << a2 << a3 << a4 << a5;
            EndMessage();
        }
        catch (...)
        {
            AbortMessage();
            throw;
        }
    }

    template<typename T1, typename T2, typename T3, typename T4, typename T5, typename T6>
    void PushMessage(const char* pszCommand, const T1& a1, const T2& a2, const T3& a3, const T4& a4, const T5& a5, const T6& a6)
    {
        try
        {
            BeginMessage(pszCommand);
            ssSend << a1 << a2 << a3 << a4 << a5 << a6;
            EndMessage();
        }
        catch (...)
        {
            AbortMessage();
            throw;
        }
    }

    template<typename T1, typename T2, typename T3, typename T4, typename T5, typename T6, typename T7>
    void PushMessage(const char* pszCommand, const T1& a1, const T2& a2, const T3& a3, const T4& a4, const T5& a5, const T6& a6, const T7& a7)
    {
        try
        {
            BeginMessage(pszCommand);
            ssSend << a1 << a2 << a3 << a4 << a5 << a6 << a7;
            EndMessage();
        }
        catch (...)
        {
            AbortMessage();
            throw;
        }
    }

    template<typename T1, typename T2, typename T3, typename T4, typename T5, typename T6, typename T7, typename T8>
    void PushMessage(const char* pszCommand, const T1& a1, const T2& a2, const T3& a3, const T4& a4, const T5& a5, const T6& a6, const T7& a7, const T8& a8)
    {
        try
        {
            BeginMessage(pszCommand);
            ssSend << a1 << a2 << a3 << a4 << a5 << a6 << a7 << a8;
            EndMessage();
        }
        catch (...)
        {
            AbortMessage();
            throw;
        }
    }

    template<typename T1, typename T2, typename T3, typename T4, typename T5, typename T6, typename T7, typename T8, typename T9>
    void PushMessage(const char* pszCommand, const T1& a1, const T2& a2, const T3& a3, const T4& a4, const T5& a5, const T6& a6, const T7& a7, const T8& a8, const T9& a9)
    {
        try
        {
            BeginMessage(pszCommand);
            ssSend << a1 << a2 << a3 << a4 << a5 << a6 << a7 << a8 << a9;
            EndMessage();
        }
        catch (...)
        {
            AbortMessage();
            throw;
        }
    }

template<typename T1, typename T2, typename T3, typename T4, typename T5, typename T6, typename T7, typename T8, typename T9, typename T10>
    void PushMessage(const char* pszCommand, const T1& a1, const T2& a2, const T3& a3, const T4& a4, const T5& a5, const T6& a6, const T7& a7, const T8& a8, const T9& a9, const T10& a10)
    {
        try
        {
            BeginMessage(pszCommand);
            ssSend << a1 << a2 << a3 << a4 << a5 << a6 << a7 << a8 << a9 << a10;
            EndMessage();
        }
        catch (...)
        {
            AbortMessage();
            throw;
        }
    }

    bool HasFulfilledRequest(std::string strRequest)
    {
        for (std::string& type : vecRequestsFulfilled)
        {
            if(type == strRequest) return true;
        }
        return false;
    }

    void FulfilledRequest(std::string strRequest)
    {
        if(HasFulfilledRequest(strRequest)) return;
        vecRequestsFulfilled.push_back(strRequest);
    }

    // Enqueue (or coalesce into) the single pending getblocks request for this
    // peer.  Returns true when the request was meaningfully admitted: newly
    // queued, or coalesced into an equivalent pending entry.  Returns false
    // when it was dedup-skipped or superseded by a more meaningful pending
    // request (the callers must not treat a dropped request as queued).
    bool PushGetBlocks(CBlockIndex* pindexBegin, uint256 hashEnd,
                       ibdmetrics::GetBlocksSource source =
                           ibdmetrics::GETBLOCKS_SOURCE_OTHER);
    // Marks a flushed pending request as the active single-flight cycle.
    // Called from the SendMessages flush loop immediately after the getblocks
    // message is committed to the wire.  Assumes the caller already checked
    // HasOutstandingGetBlocks() == false.
    void SetOutstandingGetBlocks(ibdmetrics::GetBlocksSource source,
                                 CBlockIndex* pindexBegin, uint256 hashStop)
    {
        getBlocksOutstanding.active = true;
        getBlocksOutstanding.source = source;
        getBlocksOutstanding.sent_time_us = GetTimeMicros();
        getBlocksOutstanding.locator_begin = pindexBegin;
        getBlocksOutstanding.hash_stop = hashStop;
    }
    // True while a single-flight getblocks cycle is active for this peer
    // (a request was flushed to the wire and no inv has closed it yet).
    bool HasOutstandingGetBlocks() const
    {
        return getBlocksOutstanding.active;
    }
    ibdmetrics::GetBlocksSource GetOutstandingGetBlocksSource() const
    {
        return getBlocksOutstanding.source;
    }
    // The next inv message from this peer closes the active cycle.  This is a
    // heuristic response match (no request id on the wire), not exact
    // protocol correlation.  Returns true when a cycle was closed.
    bool ConsumeGetBlocksResponse();
    // Disconnect/shutdown cleanup of the active cycle.  Records the dropped
    // no-response cycle for forensic accounting when fRecordForensics is set.
    void ClearGetBlocksOutstandingForCleanup(bool fRecordForensics = true);
    // Finite outstanding timeout: releases the slot, decrements the global
    // gauge once, records a no-response timeout, invalidates the per-peer
    // dedup identity, and re-arms pipeline wake.  Returns true when a cycle
    // was expired.  Called from the network loop and SendMessages.
    bool ExpireGetBlocksOutstanding(int64_t now_us = GetTimeMicros());
    void StartRecoveryResponseWindow(uint64_t id, int64_t send_us);
    bool ObserveRecoveryResponseInv(int64_t now_us, const RecoveryResponseObservation& observation, RecoveryResponseResult& result);
    bool ExpireRecoveryResponseWindow(int64_t now_us, RecoveryResponseResult& result);
    bool SupersedeRecoveryResponseWindow(int64_t now_us, RecoveryResponseResult& result);
    bool DisconnectRecoveryResponseWindow(int64_t now_us, RecoveryResponseResult& result);
    bool HasActiveRecoveryResponseWindow() const { return recovery_response_window.IsActive(); }
    void PushGetHeaders(const CBlockLocator& locator, uint256 hashStop, const std::string& strReason = std::string());

    void QueueInitialSyncRequest(CBlockIndex* pindexTip);

    bool CanAdvanceBlockSync(int nLocalHeight) const
    {
        const int nPeerHeight = nBestKnownHeight >= 0 ? nBestKnownHeight : nChainHeight;
        return nPeerHeight > nLocalHeight;
    }

    bool ShouldContinueKnownBlockInventory(int nLocalHeight, bool fLastBlockInMainChain) const
    {
        return CanAdvanceBlockSync(nLocalHeight) || !fLastBlockInMainChain;
    }

    void UpdateBestKnownBlock(int nHeight, const uint256& hashBlock)
    {
        if (nHeight < 0)
            return;
        if (nHeight > nBestKnownHeight || (nHeight == nBestKnownHeight && hashBlock != 0))
        {
            nBestKnownHeight = nHeight;
            if (hashBlock != 0)
                hashBestKnownBlock = hashBlock;
            nLastHeightUpdate = GetTime();
        }
    }

    void ExpireBlockInFlight(int64_t nNowUs = GetTimeMicros())
    {
        // Wire-origin deadline (data-derived knee, see
        // doc/design/ibd-conservative-block-request-expiration.md §2-3): a
        // block request expires at its getdata batch's first socket send + 60 s.
        static const int64_t BLOCK_IN_FLIGHT_TIMEOUT_US = 60LL * 1000000;
        // Pending-wire safety bound (same design §5): a request whose getdata
        // never reached the socket expires from enqueue after this bound.
        // 1 s is a conservative engineering safety bound, not a data-derived
        // network parameter: it sits well above the ~10 ms socket-drain cadence
        // (so a queued-but-legit getdata is never falsely expired) and far
        // below the 60 s wire-origin deadline.
        static const int64_t MAX_PENDING_WIRE_US = 1000000;
        const int64_t nNowSec = nNowUs / 1000000;
        for (std::map<uint256, int64_t>::iterator it = mapBlockInFlightSince.begin();
             it != mapBlockInFlightSince.end(); )
        {
            bool fPendingWire = false;
            int64_t nDeadlineUs;
            {
                LOCK(cs_vBlockInFlightWire);
                std::map<uint256, int64_t>::const_iterator wi =
                    mapBlockInFlightWireUs.find(it->first);
                if (wi != mapBlockInFlightWireUs.end() && wi->second > 0)
                    nDeadlineUs = wi->second + BLOCK_IN_FLIGHT_TIMEOUT_US;
                else
                {
                    // The getdata never reached the wire: local send-path
                    // failure/backlog, bounded from enqueue.
                    fPendingWire = true;
                    nDeadlineUs =
                        it->second * 1000000LL + MAX_PENDING_WIRE_US;
                }
            }
            if (nNowUs > nDeadlineUs)
            {
                if (BlockRequestTraceEnabled())
                    BlockRequestTraceInFlightExpire(
                        this, it->first, nNowSec - it->second);
                const uint256 hashExpired = it->first;
                // Only account the timeout when the hash is still in the live
                // in-flight set.  A stale timestamp entry (e.g. left behind by
                // a peer that was cleaned up while a stale CNode snapshot still
                // iterates it) must not release peer pressure / IBD gauges a
                // second time -- cleanup already accounted for the request.
                if (setBlocksInFlight.erase(hashExpired))
                {
                    peerLiveActivePressure.fetch_sub(
                        1, std::memory_order_relaxed);
                    ibdmetrics::InflightAdd(-1);
                    if (fPendingWire)
                    {
                        // Local send-path failure/backlog, not a peer outcome:
                        // release the request so the block can be retried, but
                        // with no peer timeout/quality penalty and no
                        // late-delivery expectation against the peer.
                        ibdmetrics::GlobalActiveAdd(
                            -1, ibdmetrics::ACTIVE_DECREMENT_OTHER);
                        RequestBlockPipelineWake(WAKE_CAUSE_INFLIGHT_TIMEOUT);
                        // EXPTRACE HOOK: request released without reaching the
                        // wire (pending-wire bound), not a timeout.
                        ibdexptrace::NoteRequestPendingWireRelease(hashExpired);
                    }
                    else
                    {
                        ibdmetrics::GlobalActiveAdd(
                            -1, ibdmetrics::ACTIVE_DECREMENT_INFLIGHT_TIMEOUT);
                        RequestBlockPipelineWake(WAKE_CAUSE_INFLIGHT_TIMEOUT);
                        RecordIbdBlockTimeout();
                        // EXPTRACE HOOK: in-flight request released by timeout.
                        ibdexptrace::NoteRequestTimeout(hashExpired);
                    }
                    // Capture the mark time before erasing so a later arrival
                    // of the block can be measured as a late delivery.
                    int64_t nExpiredMarkUs = 0;
                    std::map<uint256, int64_t>::iterator miExpiredUs =
                        mapBlockInFlightMarkUs.find(hashExpired);
                    if (miExpiredUs != mapBlockInFlightMarkUs.end())
                    {
                        nExpiredMarkUs = miExpiredUs->second;
                        mapBlockInFlightMarkUs.erase(miExpiredUs);
                    }
                    if (!fPendingWire)
                    {
                        RecordBlockLastTimeoutOwner(hashExpired, GetId());
                        RecordBlockLateDeliveryExpectation(
                            hashExpired, GetId(), nExpiredMarkUs,
                            QualityNowUs());
                    }
                    NodeId nDiversifyAnnounce = -1;
                    if (!fPendingWire &&
                        TakeDiversifyAnnounce(hashExpired, &nDiversifyAnnounce))
                        ibdmetrics::Get().diversify_other_lane_timeout.fetch_add(
                            1, std::memory_order_relaxed);
                    // Head age = age of the oldest in-flight hash at the moment
                    // this hash expires (the expiring hash is still in the set).
                    // O(n) per expiry and only computed when forensic is enabled.
                    int64_t nHeadAgeUs = 0;
                    if (ibdforensic::IsEnabled())
                    {
                        int64_t nEarliestMark = INT64_MAX;
                        for (std::map<uint256, int64_t>::const_iterator mi =
                                 mapBlockInFlightSince.begin();
                             mi != mapBlockInFlightSince.end(); ++mi)
                            if (mi->second < nEarliestMark)
                                nEarliestMark = mi->second;
                        if (nEarliestMark != INT64_MAX)
                            nHeadAgeUs =
                                (nNowSec - nEarliestMark) * 1000000LL;
                    }
                    ibdforensic::RecordExpired(
                        GetId(), hashExpired, GetTimeMicros(), nHeadAgeUs);
                    ReleaseBlockRequestOwner(
                        hashExpired, GetId(),
                        fPendingWire ? "local-fail" : "timeout");
                    EraseAlreadyAskedForIfUnowned(
                        CInv(MSG_BLOCK, hashExpired));
                }
                {
                    LOCK(cs_vBlockInFlightWire);
                    mapBlockInFlightWireUs.erase(hashExpired);
                }
                it = mapBlockInFlightSince.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    bool IsBlockInFlight(const uint256& hashBlock)
    {
        ExpireBlockInFlight();
        return setBlocksInFlight.count(hashBlock) > 0;
    }

    void MarkBlockInFlight(const uint256& hashBlock)
    {
        ExpireBlockInFlight();
        if (setBlocksInFlight.insert(hashBlock).second)
        {
            peerLiveActivePressure.fetch_add(
                1, std::memory_order_relaxed);
            NodeId nAnnouncePeer = -1;
            const bool fDiversified =
                GetDiversifyAnnounce(hashBlock, &nAnnouncePeer);
            ibdforensic::RecordGenerationStart(
                GetId(), hashBlock, GetTimeMicros(),
                fDiversified ? nAnnouncePeer : GetId(), fDiversified);
            ibdmetrics::InflightAdd(1);
            ibdmetrics::GlobalActiveAdd(1);
            RecordIbdBlockIssued();
            // EXPTRACE HOOK: request actually sent to a peer.
            ibdexptrace::NoteRequestSent(hashBlock);
        }
        mapBlockInFlightSince[hashBlock] = GetTime();
        mapBlockInFlightMarkUs[hashBlock] = QualityNowUs();
        {
            // Pending-wire sentinel: the wire-origin clock starts at 0 until
            // the getdata batch carrying this hash is first written to the
            // socket.  A pre-existing >0 stamp (hash re-marked while its batch
            // is already queued/sending) is preserved.
            LOCK(cs_vBlockInFlightWire);
            if (mapBlockInFlightWireUs.find(hashBlock) ==
                mapBlockInFlightWireUs.end())
                mapBlockInFlightWireUs[hashBlock] = 0;
        }
        TransitionBlockRequestOwnerToInFlight(hashBlock, GetId());
    }

    void ClearBlockInFlight(const uint256& hashBlock)
    {
        if (setBlocksInFlight.erase(hashBlock))
        {
            peerLiveActivePressure.fetch_sub(
                1, std::memory_order_relaxed);
            ibdmetrics::InflightAdd(-1);
            ibdmetrics::GlobalActiveAdd(
                -1, ibdmetrics::ACTIVE_DECREMENT_RECEIVE_CLEAR_INFLIGHT);
            RequestBlockPipelineWake(WAKE_CAUSE_CLEAR_INFLIGHT);
        }
        mapBlockInFlightSince.erase(hashBlock);
        {
            LOCK(cs_vBlockInFlightWire);
            mapBlockInFlightWireUs.erase(hashBlock);
        }
        std::map<uint256, int64_t>::iterator miUs =
            mapBlockInFlightMarkUs.find(hashBlock);
        if (miUs != mapBlockInFlightMarkUs.end())
        {
            const int64_t nLatencyUs = std::max<int64_t>(
                0, QualityNowUs() - miUs->second);
            mapBlockInFlightMarkUs.erase(miUs);
            RecordIbdBlockDelivery(nLatencyUs,
                                   WasBlockLastTimedOutByPeer(hashBlock, GetId()));
        }
        else
        {
            // No live mark: the request already timed out (ExpireBlockInFlight
            // consumed the mark and left a late-delivery expectation).  This is
            // a late arrival -- attribute the outcome to the peer that actually
            // requested the block, without a second lifecycle decrement.
            NodeId nTimeoutPeer = -1;
            int64_t nMarkUs = 0;
            if (TakeBlockLateDeliveryExpectation(
                    hashBlock, &nTimeoutPeer, &nMarkUs))
            {
                const int64_t nLatencyUs = std::max<int64_t>(
                    0, QualityNowUs() - nMarkUs);
                RecordLateDeliveryOutcome(nTimeoutPeer, nLatencyUs);
            }
        }
        TakeDiversifyAnnounce(hashBlock, NULL);
        ReleaseBlockRequestOwner(hashBlock, GetId(), "receive");
        // EXPTRACE HOOK: request received (in-flight slot cleared by receive).
        ibdexptrace::NoteRequestReceived(hashBlock);
    }

    // ------------------------------------------------------------------
    // IBD peer-quality observation hooks.  All are relaxed-atomic updates;
    // they never change scheduling behavior directly, only the ranking
    // signals consumed by ChooseIbdBlockRequestTarget.
    // ------------------------------------------------------------------
    // A block request was queued for / marked in flight on this peer.
    void RecordIbdBlockIssued()
    {
        ibdQuality.requests_issued.fetch_add(1, std::memory_order_relaxed);
        ibdmetrics::Get().peer_quality_requests_observed.fetch_add(
            1, std::memory_order_relaxed);
    }
    // A request was released because the block was received (possibly late).
    // Every completed outcome decays both decayed scores, so a peer's timeout
    // history fades as its receive stream continues (rolling-window recovery
    // without a restart).
    void RecordIbdBlockDelivery(int64_t latencyUs, bool receivedAfterTimeout)
    {
        const int64_t nNowUs = QualityNowUs();
        ibdQuality.releases_by_receive.fetch_add(1, std::memory_order_relaxed);
        ibdQuality.timeout_score = DecayIbdScore(
            ibdQuality.timeout_score.load(std::memory_order_relaxed));
        ibdQuality.receive_score = DecayIbdScore(
            ibdQuality.receive_score.load(std::memory_order_relaxed));
        ibdQuality.receive_score.fetch_add(
            IBD_PEER_QUALITY_SCALE, std::memory_order_relaxed);
        {
            // The EWMA initializes from the sample count, never from a sentinel
            // latency value: a first sample of exactly 0 must still seed the
            // EWMA so the second sample blends in instead of replacing it.
            const int64_t nSample = std::max<int64_t>(0, latencyUs);
            ibdQuality.latency_sum_us.fetch_add(nSample, std::memory_order_relaxed);
            const uint64_t nSamplesBefore =
                ibdQuality.latency_samples.fetch_add(1, std::memory_order_relaxed);
            const int64_t nEwma =
                ibdQuality.latency_ewma_us.load(std::memory_order_relaxed);
            const int64_t nNext =
                nSamplesBefore == 0
                    ? nSample
                    : nEwma -
                      (nEwma >> IBD_PEER_LATENCY_EWMA_SHIFT) +
                      (nSample >> IBD_PEER_LATENCY_EWMA_SHIFT);
            ibdQuality.latency_ewma_us.store(nNext, std::memory_order_relaxed);
        }
        ibdQuality.last_delivery_time_us.store(nNowUs, std::memory_order_relaxed);
        if (receivedAfterTimeout)
        {
            ibdQuality.received_after_timeout.fetch_add(
                1, std::memory_order_relaxed);
            ibdQuality.late_delivery_score = DecayIbdScore(
                ibdQuality.late_delivery_score.load(std::memory_order_relaxed));
            ibdQuality.late_delivery_score.fetch_add(
                IBD_PEER_QUALITY_SCALE, std::memory_order_relaxed);
            ibdmetrics::Get().peer_quality_late_outcomes.fetch_add(
                1, std::memory_order_relaxed);
        }
        ibdmetrics::Get().peer_quality_receive_outcomes.fetch_add(
            1, std::memory_order_relaxed);
    }
    // A request was released because it expired.  Also decays the receive
    // score so a burst of timeouts cannot be re-triggered by stale history.
    void RecordIbdBlockTimeout()
    {
        const int64_t nNowUs = QualityNowUs();
        ibdQuality.releases_by_timeout.fetch_add(1, std::memory_order_relaxed);
        ibdQuality.receive_score = DecayIbdScore(
            ibdQuality.receive_score.load(std::memory_order_relaxed));
        ibdQuality.timeout_score = DecayIbdScore(
            ibdQuality.timeout_score.load(std::memory_order_relaxed));
        ibdQuality.timeout_score.fetch_add(
            IBD_PEER_QUALITY_SCALE, std::memory_order_relaxed);
        ibdQuality.last_timeout_time_us.store(nNowUs, std::memory_order_relaxed);
        ibdmetrics::Get().peer_quality_timeout_outcomes.fetch_add(
            1, std::memory_order_relaxed);
    }
    // Snapshot of the current quality for ranking.  Relaxed loads only.
    IbdPeerQualitySnapshot GetIbdQualitySnapshot() const
    {
        IbdPeerQualitySnapshot snap;
        const int64_t nReceiveScore =
            ibdQuality.receive_score.load(std::memory_order_relaxed);
        const int64_t nTimeoutScore =
            ibdQuality.timeout_score.load(std::memory_order_relaxed);
        snap.tier = TierFromScores(nReceiveScore, nTimeoutScore);
        snap.latency_ewma_us =
            ibdQuality.latency_ewma_us.load(std::memory_order_relaxed);
        snap.has_latency_sample =
            ibdQuality.latency_samples.load(std::memory_order_relaxed) > 0;
        snap.last_timeout_time_us =
            ibdQuality.last_timeout_time_us.load(std::memory_order_relaxed);
        snap.last_delivery_time_us =
            ibdQuality.last_delivery_time_us.load(std::memory_order_relaxed);
        return snap;
    }
    // Deterministic decayed-score step: each completed outcome halves the
    // influence of every earlier outcome roughly every ~11 events, giving a
    // rolling window (and therefore reputation recovery without a restart).
    static int64_t DecayIbdScore(int64_t score)
    {
        return score - (score >> IBD_PEER_QUALITY_DECAY_SHIFT);
    }
    // Tier from decayed (fixed-point scaled) outcome scores.  UNKNOWN until
    // enough completed outcomes are observed (cold-start neutrality).
    static IbdPeerQualityTier TierFromScores(int64_t receiveScore,
                                             int64_t timeoutScore)
    {
        if (receiveScore < 0)
            receiveScore = 0;
        if (timeoutScore < 0)
            timeoutScore = 0;
        if (receiveScore + timeoutScore <
            IBD_PEER_QUALITY_MIN_SCORE * IBD_PEER_QUALITY_SCALE)
            return IBD_PEER_QUALITY_UNKNOWN;
        const int64_t nPermille =
            (1000LL * timeoutScore) / (receiveScore + timeoutScore);
        return nPermille >= IBD_PEER_QUALITY_DEGRADED_RATE_PERMILLE
            ? IBD_PEER_QUALITY_DEGRADED : IBD_PEER_QUALITY_GOOD;
    }
    // Wall clock for quality timestamps (override for tests).
    static int64_t QualityNowUs();
    bool IsSubscribed(unsigned int nChannel);
    void Subscribe(unsigned int nChannel, unsigned int nHops=0);
    void CancelSubscribe(unsigned int nChannel);
    void CloseSocketDisconnect();
	void Cleanup(NodeCleanupMode mode = NODE_CLEANUP_RUNTIME);

    // Denial-of-service detection/prevention
    // The idea is to detect peers that are behaving
    // badly and disconnect/ban them, but do it in a
    // one-coding-mistake-won't-shatter-the-entire-network
    // way.
    // IMPORTANT:  There should be nothing I can give a
    // node that it will forward on that will make that
    // node's peers drop it. If there is, an attacker
    // can isolate a node and/or try to split the network.
    // Dropping a node for sending stuff that is invalid
    // now but might be valid in a later version is also
    // dangerous, because it can cause a network split
    // between nodes running old code and nodes running
    // new code.
    static void ClearBanned(); // needed for unit testing
    static bool IsBanned(CNetAddr ip);
    static bool IsBanned(CSubNet subnet);
    static void Ban(const CNetAddr &ip, const BanReason &banReason, int64_t bantimeoffset = 0, bool sinceUnixEpoch = false);
    static void Ban(const CSubNet &subNet, const BanReason &banReason, int64_t bantimeoffset = 0, bool sinceUnixEpoch = false);
    static bool Unban(const CNetAddr &ip);
    static bool Unban(const CSubNet &ip);
    static void GetBanned(banmap_t &banmap);
    static void SetBanned(const banmap_t &banmap);

    //!check is the banlist has unwritten changes
    static bool BannedSetIsDirty();
    //!set the "dirty" flag for the banlist
    static void SetBannedSetDirty(bool dirty=true);
    //!clean unused entires (if bantime has expired)
    static void SweepBanned();

    bool Misbehaving(int howmuch); // 1 == a little, 100 == a lot

    void copyStats(CNodeStats &stats);

    static bool IsWhitelistedRange(const CNetAddr& ip);
    static void AddWhitelistedRange(const CSubNet& subnet);

    // Network stats
    static void RecordBytesRecv(uint64_t bytes);
    static void RecordBytesSent(uint64_t bytes);

    static uint64_t GetTotalBytesRecv();
    static uint64_t GetTotalBytesSent();

    //!set the max outbound target in bytes
    static void SetMaxOutboundTarget(uint64_t limit);
    static uint64_t GetMaxOutboundTarget();

    //!set the timeframe for the max outbound target
    static void SetMaxOutboundTimeframe(uint64_t timeframe);
    static uint64_t GetMaxOutboundTimeframe();

    //!check if the outbound target is reached
    // if param historicalBlockServingLimit is set true, the function will
    // response true if the limit for serving historical blocks has been reached
    static bool OutboundTargetReached(bool historicalBlockServingLimit);

    //!response the bytes left in the current max outbound cycle
    // in case of no limit, it will always response 0
    static uint64_t GetOutboundTargetBytesLeft();

    //!response the time in second left in the current max outbound cycle
    // in case of no limit, it will always response 0
    static uint64_t GetMaxOutboundTimeLeftInCycle();
};

inline void RelayInventory(const CInv& inv)
{
    // Put on lists to offer to the other nodes
    {
        LOCK(cs_vNodes);
        for (CNode* pnode : vNodes)
            pnode->PushInventory(inv);
    }
}

class CTransaction;
void RelayTransaction(const CTransaction& tx, const uint256& hash);
void RelayTransaction(const CTransaction& tx, const uint256& hash, const CDataStream& ss);
void RelayTransactionLockReq(const CTransaction& tx, const uint256& hash, bool relayToAll=false);
void RelayCollaTeralFinalTransaction(const int sessionID, const CTransaction& txNew);
void RelayCollaTeralIn(const std::vector<CTxIn>& in, const int64_t& nAmount, const CTransaction& txCollateral, const std::vector<CTxOut>& out);
void RelayCollaTeralStatus(const int sessionID, const int newState, const int newEntriesCount, const int newAccepted, const std::string error="");
void RelayCollaTeralElectionEntry(const CTxIn vin, const CService addr, const std::vector<unsigned char> vchSig, const int64_t nNow, const CPubKey pubkey, const CPubKey pubkey2, const int count, const int current, const int64_t lastUpdated, const int protocolVersion);
void SendCollaTeralElectionEntry(const CTxIn vin, const CService addr, const std::vector<unsigned char> vchSig, const int64_t nNow, const CPubKey pubkey, const CPubKey pubkey2, const int count, const int current, const int64_t lastUpdated, const int protocolVersion);
void RelayCollaTeralElectionEntryPing(const CTxIn vin, const std::vector<unsigned char> vchSig, const int64_t nNow, const bool stop);
void SendCollaTeralElectionEntryPing(const CTxIn vin, const std::vector<unsigned char> vchSig, const int64_t nNow, const bool stop);
void RelayCollaTeralCompletedTransaction(const int sessionID, const bool error, const std::string errorMessage);
void RelayCollaTeralCollateralNodeContestant();


/** Access to the banlist database (banlist.dat) */
class CBanDB
{
private:
    boost::filesystem::path pathBanlist;
public:
    CBanDB();
    bool Write(const banmap_t& banSet);
    bool Read(banmap_t& banSet);
};

void DumpBanlist();

bool FetchBlockForStaking(const uint256& hashBlock);

#endif
