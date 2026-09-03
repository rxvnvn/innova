// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2012 The Bitcoin developers
// Copyright (c) 2017-2021 The Denarius developers
// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "alert.h"
#include "bloom.h"
#include "checkpoints.h"
#include "db.h"
#include "txdb.h"
#include "net.h"
#include "init.h"
#include "wallet.h"
#include "ui_interface.h"
#include "ibdefficiency.h"
#include "ibdmetrics.h"
#include "ibdactivepath.h"
#include "ibdblocklatency.h"
#include "pinglifecycletrace.h"
#include "ibdforensic.h"
#include "ibdheaderscheduler.h"
#include "headersservededup.h"
#include "getblocksservedinvzero.h"
#include "kernel.h"
#include "collateral.h"
#include "collateralnode.h"
#include "nullsend.h"
#include "spork.h"
#include "smessage.h"
#include "namecoin.h"
#include "dandelion.h"
#include "lelantus.h"
#include "curvetree.h"
#include "finality.h"
#include "dag.h"
#include "candidate_frontier.h"
#include "blockindex_hot_owner.h"
#include "hreg_registration.h"
#include <boost/algorithm/string/replace.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <algorithm>

#if BOOST_VERSION >= 107300
#include <boost/bind/bind.hpp>
using boost::placeholders::_1;
using boost::placeholders::_2;
#else
#include <boost/bind.hpp>
#endif

using namespace std;
namespace fs = boost::filesystem;

static bool LoadFCMPValidationRoot(CTxDB& txdb, int nBlockHeight,
                                   CCurveTreeNode& rootOut,
                                   uint256& hashExpectedRootOut,
                                   std::string& strErrorOut);
static bool AlreadyHave(CTxDB& txdb, const CInv& inv);

//
// Global state
//

CCriticalSection cs_setpwalletRegistered;
set<CWallet*> setpwalletRegistered;

CCriticalSection cs_main;

CTxMemPool mempool;
//unsigned int nTransactionsUpdated = 0;

map<uint256, CBlockIndex*> mapBlockIndex;
set<pair<COutPoint, unsigned int> > setStakeSeen;

CBigNum bnProofOfWorkLimit(~uint256(0) >> 20);      // "standard" scrypt target limit for proof of work, results with 0,000244140625 proof-of-work difficulty
CBigNum bnProofOfStakeLimit(~uint256(0) >> 20);
CBigNum bnProofOfStakeLimitTestNet(~uint256(0) >> 10); // 1024x easier for testnet
CBigNum bnProofOfWorkLimitTestNet(~uint256(0) >> 16);

/** Fees smaller than this (in innovai) are considered zero fee (for relaying and mining) */
// CFeeRate minRelayTxFee = CFeeRate(SUBCENT);

// Block Variables

unsigned int nTargetSpacing     = 15;               // 15 seconds
unsigned int nStakeMinAge       = 10 * 60 * 60;     // 10 hour min stake age
unsigned int nStakeMaxAge       = -1;               // unlimited (original behavior)
unsigned int nModifierInterval  = 10 * 60;          // time to elapse before new modifier is computed
int64_t nLastCoinStakeSearchTime = GetAdjustedTime();
int nCoinbaseMaturity = 65; //75 on Mainnet I n n o v a
CBlockIndex* pindexGenesisBlock = NULL;
int nBestHeight = -1;
static CCriticalSection cs_getHeadersDiag;
static std::map<NodeId, uint64_t> mapGetHeadersSendDiagCount;
static std::map<NodeId, uint64_t> mapHeadersRecvDiagCount;

bool CollateralNReorgBlock = true;
uint256 nBestChainTrust = 0;
uint256 nBestInvalidTrust = 0;
std::set<uint256> setInvalidBlockHash;

// Candidate tip frontier — persisted subset of all-known-tips.
std::map<uint256, CandidateTipRecord> mapCandidateTips;
uint64_t nCandidateTipGeneration = 0;
bool fCandidateFrontierShadowActive = true;  // Shadow-only by default

// Peer that delivered the block currently being processed on this thread.
// Populated by ProcessBlock so trace hooks inside SetBestChain/Reorganize can
// attribute tip advances and reorgs to the supplying peer.
static NodeId g_nBlockTraceSourcePeer = -1;

uint256 hashBestChain = 0;
CBlockIndex* pindexBest = NULL;
int64_t nTimeBestReceived = 0;

bool fImporting = false;
bool fReindex = false;
bool fAddrIndex = false;

bool fSPVMode = false;
bool fIbdHeadersObserve = false;
bool fIbdHeaderScheduler = false;
bool fRegTestIbd = false;
static CIbdHeadersObserver g_ibdHeadersObserver(512); // OBSERVATION WINDOW, not policy

namespace
{

// Default effective ordered-IBD block request window.
static const std::size_t IBD_HEADERS_SCHEDULER_WINDOW_DEFAULT = 8192;

// Effective ordered-IBD block request window (W): how many heights ahead of
// the frontier the scheduler may hold as candidate work.  Loaded lazily from
// -ibdblockwindow on first use so the argument is read exactly once; unit
// tests reload it through ResetIbdBlockWindowConfigForTesting().
std::size_t g_nIbdBlockWindowConfigured = IBD_HEADERS_SCHEDULER_WINDOW_DEFAULT;
bool g_nIbdBlockWindowLoaded = false;

std::size_t LoadIbdBlockWindowConfig()
{
    const std::size_t nMin = 512;
    const std::size_t nMax = 16384;
    std::size_t nRaw = IBD_HEADERS_SCHEDULER_WINDOW_DEFAULT;
    if (mapArgs.count("-ibdblockwindow"))
    {
        int64_t nParse = 0;
        if (ParseInt64(mapArgs["-ibdblockwindow"], &nParse) && nParse >= 1)
            nRaw = (std::size_t)nParse;
    }
    if (nRaw < nMin)
        nRaw = nMin;
    if (nRaw > nMax)
        nRaw = nMax;
    return nRaw;
}

} // namespace

std::size_t GetIbdBlockWindow()
{
    if (!g_nIbdBlockWindowLoaded)
    {
        g_nIbdBlockWindowConfigured = LoadIbdBlockWindowConfig();
        g_nIbdBlockWindowLoaded = true;
        ibdmetrics::Get().scheduler_block_window.store(
            (int64_t)g_nIbdBlockWindowConfigured, std::memory_order_relaxed);
    }
    return g_nIbdBlockWindowConfigured;
}

void ResetIbdBlockWindowConfigForTesting()
{
    g_nIbdBlockWindowLoaded = false;
}

bool IbdHeadersControlPlaneEnabled()
{
    return fIbdHeadersObserve || fIbdHeaderScheduler;
}

static bool IbdHeaderSchedulerSelectActive()
{
    return fIbdHeaderScheduler && !fSPVMode && IsInitialBlockDownload();
}

namespace
{
struct IbdHeaderSchedulerState
{
    std::map<uint256, std::set<NodeId> > invAvailability;
    uint64_t refillCalls;
    uint64_t refillAdmissions;
    uint64_t incrementalRefillCalls;
    uint64_t fullRefillCalls;
    uint64_t incrementalEntriesExamined;
    uint64_t fullEntriesExamined;
    uint64_t incrementalAdmitted;
    uint64_t incrementalRefillUs;
    uint64_t fullRefillUs;
    std::vector<uint256> cursorWindow;
    std::vector<NodeId> cursorPeers;
    uint256 cursorFrontier;
    uint256 cursorTip;
    int cursorHeight;
    uint64_t availabilityEpoch;
    uint64_t cursorAvailabilityEpoch;
    bool cursorValid;
    bool cursorInvalidated;
    bool cursorRecoveryNeeded;
    size_t cursorNextIndex;
    size_t cursorPendingSlots;
    uint64_t refillRound;
    uint64_t cursorRound;
    uint64_t fallbackCount;
    uint64_t invInsideWindow;
    uint64_t invBeforeWindow;
    uint64_t invAfterWindow;
    uint64_t invOffBranch;
    uint64_t invUnknown;
    uint64_t invPrevented;
    // Progress-aware expiration state (Phase 2).  Each hash admitted to the
    // ordered pipeline records the connected frontier height at admission; the
    // expiry path compares it against the live frontier to decide whether a
    // request whose fixed wire-origin deadline passed is being legitimately
    // approached (defer) or truly abandoned (expire).  Entries are pruned to
    // ~one window in the refill so the map stays bounded.
    std::map<uint256, int> orderedRequestFrontierBaseline;
    uint256 orderedRequestFrontierBaselinePruneCursor;
    bool orderedRequestFrontierBaselinePruneCursorValid;
    uint64_t baselinePruneEntriesExamined;
    uint64_t baselinePruneEntriesErased;
    uint64_t orderedExpiryDeferredDueProgress;
    uint64_t orderedExpiryActual;
    // FRONT_PREEMPT (v1): decision counters for ordered-head slot migration.
    uint64_t frontPreemptAttempts;
    uint64_t frontPreemptTransfers;

    IbdHeaderSchedulerState()
        : refillCalls(0), refillAdmissions(0), incrementalRefillCalls(0),
          fullRefillCalls(0), incrementalEntriesExamined(0),
          fullEntriesExamined(0), incrementalAdmitted(0),
          incrementalRefillUs(0), fullRefillUs(0), refillRound(0),
          cursorRound(0), fallbackCount(0),
          invInsideWindow(0), invBeforeWindow(0), invAfterWindow(0),
          invOffBranch(0), invUnknown(0), invPrevented(0),
          orderedRequestFrontierBaselinePruneCursorValid(false),
          baselinePruneEntriesExamined(0), baselinePruneEntriesErased(0),
          orderedExpiryDeferredDueProgress(0), orderedExpiryActual(0),
          frontPreemptAttempts(0), frontPreemptTransfers(0)
    {
    }

    void Clear()
    {
        invAvailability.clear();
        refillCalls = 0;
        refillAdmissions = 0;
        incrementalRefillCalls = 0;
        fullRefillCalls = 0;
        incrementalEntriesExamined = 0;
        fullEntriesExamined = 0;
        incrementalAdmitted = 0;
        incrementalRefillUs = 0;
        fullRefillUs = 0;
        cursorWindow.clear();
        cursorPeers.clear();
        cursorFrontier = 0;
        cursorTip = 0;
        cursorHeight = -1;
        availabilityEpoch = 0;
        cursorAvailabilityEpoch = 0;
        cursorValid = false;
        cursorInvalidated = false;
        cursorRecoveryNeeded = false;
        cursorNextIndex = 0;
        cursorPendingSlots = 0;
        refillRound = 0;
        cursorRound = 0;
        fallbackCount = 0;
        invInsideWindow = 0;
        invBeforeWindow = 0;
        invAfterWindow = 0;
        invOffBranch = 0;
        invUnknown = 0;
        invPrevented = 0;
        orderedRequestFrontierBaseline.clear();
        orderedRequestFrontierBaselinePruneCursor = 0;
        orderedRequestFrontierBaselinePruneCursorValid = false;
        baselinePruneEntriesExamined = 0;
        baselinePruneEntriesErased = 0;
        orderedExpiryDeferredDueProgress = 0;
        orderedExpiryActual = 0;
        frontPreemptAttempts = 0;
        frontPreemptTransfers = 0;
    }

    void RemovePeer(NodeId peer)
    {
        ++availabilityEpoch;
        for (std::map<uint256, std::set<NodeId> >::iterator it =
                 invAvailability.begin(); it != invAvailability.end();)
        {
            it->second.erase(peer);
            if (it->second.empty())
                invAvailability.erase(it++);
            else
                ++it;
        }
    }
};

static IbdHeaderSchedulerState g_ibdHeaderSchedulerState;
static CBlockIndex g_ibdHeaderSchedulerTestTip;
static uint256 g_ibdHeaderSchedulerTestHash;
static CBlockIndex* g_ibdHeaderSchedulerSavedPindexBest = NULL;
static uint256 g_ibdHeaderSchedulerSavedHashBestChain;
static int g_ibdHeaderSchedulerSavedBestHeight = -1;
static bool g_ibdHeaderSchedulerTestAnchorActive = false;
}

static const size_t ORDERED_BASELINE_PRUNE_BUDGET = 64;

static void PruneOrderedRequestFrontierBaselineBounded(
    const CIbdHeaderGraph& graph, int nFrontierHeight)
{
    std::map<uint256, int>& baseline =
        g_ibdHeaderSchedulerState.orderedRequestFrontierBaseline;
    if (baseline.empty())
    {
        g_ibdHeaderSchedulerState.orderedRequestFrontierBaselinePruneCursor = 0;
        g_ibdHeaderSchedulerState.orderedRequestFrontierBaselinePruneCursorValid = false;
        return;
    }

    size_t nBudget = std::min<size_t>(ORDERED_BASELINE_PRUNE_BUDGET,
                                      baseline.size());
    std::map<uint256, int>::iterator it =
        g_ibdHeaderSchedulerState.orderedRequestFrontierBaselinePruneCursorValid
            ? baseline.lower_bound(
                  g_ibdHeaderSchedulerState.orderedRequestFrontierBaselinePruneCursor)
            : baseline.begin();
    if (it == baseline.end())
        it = baseline.begin();

    for (size_t i = 0; i < nBudget && !baseline.empty(); ++i)
    {
        if (it == baseline.end())
            it = baseline.begin();
        std::map<uint256, int>::iterator current = it++;
        ++g_ibdHeaderSchedulerState.baselinePruneEntriesExamined;
        const CIbdHeaderNode* nB = graph.Lookup(current->first);
        if (nB == NULL || !nB->IsAnchored() ||
            nB->height <= nFrontierHeight ||
            (size_t)(nB->height - nFrontierHeight) > GetIbdBlockWindow())
        {
            baseline.erase(current);
            ++g_ibdHeaderSchedulerState.baselinePruneEntriesErased;
        }
    }

    if (baseline.empty())
    {
        g_ibdHeaderSchedulerState.orderedRequestFrontierBaselinePruneCursor = 0;
        g_ibdHeaderSchedulerState.orderedRequestFrontierBaselinePruneCursorValid = false;
    }
    else
    {
        if (it == baseline.end())
            it = baseline.begin();
        g_ibdHeaderSchedulerState.orderedRequestFrontierBaselinePruneCursor = it->first;
        g_ibdHeaderSchedulerState.orderedRequestFrontierBaselinePruneCursorValid = true;
    }
}

void AdvanceIbdHeaderSchedulerRound()
{
    ++g_ibdHeaderSchedulerState.refillRound;
}

void ResetIbdHeaderSchedulerStateForTesting()
{
    g_ibdHeaderSchedulerState.Clear();
    g_ibdHeadersObserver.SetEnabled(false);
    if (g_ibdHeaderSchedulerTestAnchorActive)
    {
        pindexBest = g_ibdHeaderSchedulerSavedPindexBest;
        hashBestChain = g_ibdHeaderSchedulerSavedHashBestChain;
        nBestHeight = g_ibdHeaderSchedulerSavedBestHeight;
        g_ibdHeaderSchedulerSavedPindexBest = NULL;
        g_ibdHeaderSchedulerSavedHashBestChain = 0;
        g_ibdHeaderSchedulerSavedBestHeight = -1;
        g_ibdHeaderSchedulerTestAnchorActive = false;
    }
    g_ibdHeaderSchedulerTestHash = 0;
    g_ibdHeaderSchedulerTestTip = CBlockIndex();
}

bool SeedIbdHeaderSchedulerAnchorForTesting(const uint256& hash, int height)
{
    if (!g_ibdHeaderSchedulerTestAnchorActive)
    {
        g_ibdHeaderSchedulerSavedPindexBest = pindexBest;
        g_ibdHeaderSchedulerSavedHashBestChain = hashBestChain;
        g_ibdHeaderSchedulerSavedBestHeight = nBestHeight;
        g_ibdHeaderSchedulerTestAnchorActive = true;
    }
    g_ibdHeaderSchedulerTestHash = hash;
    g_ibdHeaderSchedulerTestTip = CBlockIndex();
    g_ibdHeaderSchedulerTestTip.phashBlock = &g_ibdHeaderSchedulerTestHash;
    g_ibdHeaderSchedulerTestTip.nHeight = height;
    pindexBest = &g_ibdHeaderSchedulerTestTip;
    hashBestChain = hash;
    nBestHeight = height;
    g_ibdHeadersObserver.SetEnabled(true);
    return g_ibdHeadersObserver.UpdateAnchor(hash, height);
}

bool SeedIbdHeaderSchedulerHeadersForTesting(NodeId peer,
    const std::vector<std::pair<uint256, uint256> >& headers)
{
    g_ibdHeadersObserver.SetEnabled(true);
    g_ibdHeadersObserver.ObserveHeaders(peer, headers, 0);
    return headers.empty() ||
           g_ibdHeadersObserver.Graph().Lookup(headers.back().first) != NULL;
}

bool ActivateIbdHeaderSchedulerBranchForTesting(const uint256& tip)
{
    g_ibdHeadersObserver.SetEnabled(true);
    CIbdHeaderGraph& graph = const_cast<CIbdHeaderGraph&>(
        g_ibdHeadersObserver.Graph());
    return graph.ActivateBranch(tip);
}

void SeedIbdHeaderSchedulerInvAvailabilityForTesting(NodeId peer,
    const uint256& hash)
{
    if (g_ibdHeaderSchedulerState.invAvailability[hash].insert(peer).second)
        ++g_ibdHeaderSchedulerState.availabilityEpoch;
}

static bool PrepareIbdHeadersObserverRequest(CNode* pnode, CBlockLocator& locatorOut)
{
    if (!IbdHeadersControlPlaneEnabled() || fSPVMode || !pnode || pnode->fClient ||
        pnode->fOneShot || !IsInitialBlockDownload() || !pindexBest)
        return false;
    g_ibdHeadersObserver.SetEnabled(true);
    g_ibdHeadersObserver.SetLookaheadCap(
        GetIbdBlockWindow() + IBD_HEADER_LOOKAHEAD_CAP_MARGIN,
        GetIbdBlockWindow() + IBD_HEADER_LOOKAHEAD_RESUME_MARGIN);
    if (!g_ibdHeadersObserver.UpdateAnchor(pindexBest->GetBlockHash(),
                                           pindexBest->nHeight))
        return false;
    // Stage 3 bounded lookahead: never issue a header request when the graph
    // already holds at least the cap ahead of the frontier.  The graph tip
    // cannot be pushed beyond the cap by a new batch, BuildContinuationLocator
    // cost stays bounded, and this also guards the timeout/retry paths from
    // re-requesting headers that are already in hand.
    const CIbdHeaderNode* graphTip = g_ibdHeadersObserver.Graph().ActiveTip();
    if (graphTip && graphTip->height - pindexBest->nHeight >=
            (int)(GetIbdBlockWindow() + IBD_HEADER_LOOKAHEAD_CAP_MARGIN))
        return false;
    const std::vector<uint256> hashes =
        g_ibdHeadersObserver.Graph().BuildContinuationLocator();
    if (hashes.empty()) return false;
    g_ibdHeadersObserver.MarkHeaderRequest(pnode->GetId());
    locatorOut = CBlockLocator(hashes);
    printf("IBD_HEADERS_OBSERVE event=getheaders_intent peer=%lld locator_size=%zu locator_first=%s locator_last=%s anchor_height=%d\n",
           (long long)pnode->GetId(), hashes.size(), hashes.front().ToString().c_str(),
           hashes.back().ToString().c_str(), g_ibdHeadersObserver.Graph().AnchorHeight());
    return true;
}

static void TraceIbdHeadersObserverEvent(const char* event, CNode* pnode,
    const uint256& hash, int knownHeight)
{
    if (!IbdHeadersControlPlaneEnabled() || !IsInitialBlockDownload()) return;
    LOCK(cs_main);
    if (!g_ibdHeadersObserver.Enabled()) return;
    CIbdHeadersObserver::Classification c =
        g_ibdHeadersObserver.Classify(hash, knownHeight);
    unsigned int kind = strcmp(event, "request") == 0 ? 0 :
                        strcmp(event, "receive") == 0 ? 1 : 2;
    g_ibdHeadersObserver.RecordClassification(kind, c);
    const CIbdHeaderNode* tip = g_ibdHeadersObserver.Graph().ActiveTip();
    printf("IBD_HEADERS_OBSERVE event=%s peer=%lld hash=%s class=%s anchor_height=%d graph_tip_height=%d graph_ahead=%d\n",
           event, pnode ? (long long)pnode->GetId() : -1LL,
           hash.ToString().c_str(),
           CIbdHeadersObserver::ClassificationName(c),
           g_ibdHeadersObserver.Graph().HasAnchor() ?
               g_ibdHeadersObserver.Graph().AnchorHeight() : -1,
           tip ? tip->height : -1,
           tip && g_ibdHeadersObserver.Graph().HasAnchor() ?
               tip->height - g_ibdHeadersObserver.Graph().AnchorHeight() : -1);
}

void InvalidateIbdHeaderSchedulerRefillCursor()
{
    g_ibdHeaderSchedulerState.cursorInvalidated = true;
}

void MarkIbdHeaderSchedulerRecoveryNeeded()
{
    g_ibdHeaderSchedulerState.cursorRecoveryNeeded = true;
}

void IbdHeadersObserverPeerDisconnected(NodeId peer)
{
    if (!IbdHeadersControlPlaneEnabled()) return;
    TRY_LOCK(cs_main, lockMain);
    if (lockMain)
    {
        g_ibdHeadersObserver.RemovePeer(peer);
        g_ibdHeaderSchedulerState.RemovePeer(peer);
    }
}
bool fSPVHeadersOnly = false;
int nSPVStartHeight = 0;

bool fHybridSPV = false;
bool fSPVStakingEnabled = false;
StakingMode nStakingMode = STAKE_TRANSPARENT;
CCriticalSection cs_stakingMode;

int nLastFinalizedHeight = 0;
uint256 hashLastFinalized = 0;
CCriticalSection cs_finality;

CMedianFilter<int> cPeerBlockCounts(5, 0); // Amount of blocks that other nodes claim to have

std::map<int64_t, CAnonOutputCount> mapAnonOutputStats;
//map<int64_t, CAnonOutputCount> mapAnonOutputStats; // display only, not 100% accurate, height could become inaccurate due to undos
map<uint256, CBlock*> mapOrphanBlocks;
multimap<uint256, CBlock*> mapOrphanBlocksByPrev;
map<uint256, NodeId> mapOrphanBlocksByNode;
map<NodeId, int> mapOrphanCountByNode;
set<pair<COutPoint, unsigned int> > setStakeSeenOrphan;

void EraseStakeSeenOrphanIfUnreferenced(const std::pair<COutPoint, unsigned int>& stake)
{
    if (!setStakeSeenOrphan.count(stake))
        return;
    for (std::map<uint256, CBlock*>::const_iterator mi = mapOrphanBlocks.begin();
         mi != mapOrphanBlocks.end(); ++mi)
    {
        if (mi->second->IsProofOfStake() &&
            mi->second->GetProofOfStake() == stake)
            return;
    }
    setStakeSeenOrphan.erase(stake);
}

static int GetPeerOrphanCount(NodeId nodeid)
{
    std::map<NodeId, int>::const_iterator it = mapOrphanCountByNode.find(nodeid);
    return it == mapOrphanCountByNode.end() ? 0 : it->second;
}

// Hard per-peer orphan storage cap.  The deferred request scheduler no longer
// subtracts orphan storage pressure from the active request budget, so this
// predicate is the sole enforcement point that keeps one peer's orphan
// storage bounded at MAX_ORPHAN_BLOCKS_PER_PEER.  Kept behavior-identical to
// the inline checks it replaces; extracted so the cap is unit-testable.
bool PeerOrphanStorageLimitExceeded(NodeId peer, int* pnOrphanCountPeer)
{
    const int nOrphanCountPeer = GetPeerOrphanCount(peer);
    if (pnOrphanCountPeer)
        *pnOrphanCountPeer = nOrphanCountPeer;
    return nOrphanCountPeer >= MAX_ORPHAN_BLOCKS_PER_PEER;
}

// Returns 1 when the local active-chain tip is an ancestor of the peer's
// advertised best-known block, 0 when the peer is on a competing branch, and
// -1 when the peer's best-known block is unknown or too deep to check.
static int TipAncestorOfPeerBestKnown(const uint256& hashPeerBest)
{
    if (hashPeerBest == 0)
        return -1;
    std::map<uint256, CBlockIndex*>::const_iterator mi =
        mapBlockIndex.find(hashPeerBest);
    if (mi == mapBlockIndex.end())
        return -1;
    const CBlockIndex* pindex = mi->second;
    if (pindex->nHeight < nBestHeight)
        return 0;
    for (int i = 0; pindex != NULL && i < 8000000; ++i, pindex = pindex->pprev)
    {
        if (pindex->nHeight == nBestHeight)
            return (pindex->GetBlockHash() == hashBestChain) ? 1 : 0;
        if (pindex->nHeight < nBestHeight)
            return 0;
    }
    return -1;
}

bool ShouldSkipBlockInvForOrphanPressure(CNode* pfrom, const CInv& inv,
                                          bool fAlreadyHave,
                                          int* pnOrphanCountPeer,
                                          int* pnQueuedBlockRequests,
                                          int* pnSentBlockRequests,
                                          int* pnProjectedPressure)
{
    AssertLockHeld(cs_main);
    if (pnOrphanCountPeer)
        *pnOrphanCountPeer = 0;
    if (pnQueuedBlockRequests)
        *pnQueuedBlockRequests = 0;
    if (pnSentBlockRequests)
        *pnSentBlockRequests = 0;
    if (pnProjectedPressure)
        *pnProjectedPressure = 0;
    if (pfrom == NULL || fAlreadyHave || inv.type != MSG_BLOCK)
        return false;
    if (!IsInitialBlockDownload())
        return false;

    const int nOrphanCountPeer = GetPeerOrphanCount(pfrom->GetId());
    const int nQueuedBlockRequests = (int)pfrom->setAskForBlocks.size();
    const int nSentBlockRequests = (int)pfrom->setBlocksInFlight.size();
    const int nProjectedPressure = nOrphanCountPeer +
        nQueuedBlockRequests + nSentBlockRequests;
    if (pnOrphanCountPeer)
        *pnOrphanCountPeer = nOrphanCountPeer;
    if (pnQueuedBlockRequests)
        *pnQueuedBlockRequests = nQueuedBlockRequests;
    if (pnSentBlockRequests)
        *pnSentBlockRequests = nSentBlockRequests;
    if (pnProjectedPressure)
        *pnProjectedPressure = nProjectedPressure;
    return nProjectedPressure >= MAX_PROJECTED_ORPHAN_PRESSURE;
}

static void BlockRequestTraceOrphanPressure(CNode* pfrom, int nOrphanCountPeer,
                                            int nQueuedBlockRequests,
                                            int nSentBlockRequests,
                                            int nProjectedPressure)
{
    if (!BlockRequestTraceEnabled() || pfrom == NULL)
        return;

    static std::map<NodeId, int64_t> mapLastTraceByPeer;
    const int64_t nNow = GetTimeMicros();
    int64_t& nLastTrace = mapLastTraceByPeer[pfrom->GetId()];
    if (nLastTrace != 0 && nNow - nLastTrace < 10 * 1000000)
        return;
    nLastTrace = nNow;

    printf("BLOCKREQTRACE time_us=%lld event=ORPHAN_PRESSURE peer=%d addr=%s orphan_count=%d queued_blocks=%d sent_blocks=%d projected_pressure=%d budget=%d hard_limit=%d ask_queue=%u local_height=%d\n",
           (long long)nNow, pfrom->GetId(), pfrom->addrName.c_str(),
           nOrphanCountPeer, nQueuedBlockRequests, nSentBlockRequests,
           nProjectedPressure, MAX_PROJECTED_ORPHAN_PRESSURE,
           MAX_ORPHAN_BLOCKS_PER_PEER,
           (unsigned int)pfrom->mapAskFor.size(),
           nBestHeight);
}



static int CountGlobalActiveBlockRequests(CNode* extraPeer,
                                          bool* pfVNodesLockFailed = NULL)
{
    int nGlobalActive = 0;
    TRY_LOCK(cs_vNodes, lockNodes);
    if (pfVNodesLockFailed)
        *pfVNodesLockFailed = !lockNodes;
    if (lockNodes)
    {
        for (std::vector<CNode*>::const_iterator it = vNodes.begin();
             it != vNodes.end(); ++it)
        {
            const CNode* pnode = *it;
            if (pnode == NULL)
                continue;
            nGlobalActive += (int)pnode->setAskForBlocks.size();
            nGlobalActive += (int)pnode->setBlocksInFlight.size();
        }
    }
    if (!lockNodes)
        return GetMaxActiveBlockRequestsGlobal() + 1;
    else if (extraPeer != NULL &&
             std::find(vNodes.begin(), vNodes.end(), extraPeer) == vNodes.end())
    {
        nGlobalActive += (int)extraPeer->setAskForBlocks.size();
        nGlobalActive += (int)extraPeer->setBlocksInFlight.size();
    }
    return nGlobalActive;
}

int GetDeferredBlockRequestBudget(CNode* pfrom,
                                  int* pnOrphanCountPeer,
                                  int* pnQueuedBlockRequests,
                                  int* pnSentBlockRequests,
                                  int* pnPeerActivePressure,
                                  int* pnGlobalActivePressure)
{
    AssertLockHeld(cs_main);
    if (pnOrphanCountPeer)
        *pnOrphanCountPeer = 0;
    if (pnQueuedBlockRequests)
        *pnQueuedBlockRequests = 0;
    if (pnSentBlockRequests)
        *pnSentBlockRequests = 0;
    if (pnPeerActivePressure)
        *pnPeerActivePressure = 0;
    if (pnGlobalActivePressure)
        *pnGlobalActivePressure = 0;
    if (pfrom == NULL || !IsInitialBlockDownload())
    {
        if (pfrom != NULL)
        {
            const int nOldZero = pfrom->nDeferredBudgetZero.exchange(
                0, std::memory_order_relaxed);
            ibdmetrics::PeerZeroStateChange(nOldZero, 0);
        }
        return GetMaxActiveBlockRequestsPerPeer();
    }

    const int nOrphanCountPeer = GetPeerOrphanCount(pfrom->GetId());
    const int nQueuedBlockRequests = (int)pfrom->setAskForBlocks.size();
    const int nSentBlockRequests = (int)pfrom->setBlocksInFlight.size();
    // Request pressure only: queued + in-flight block requests.  Orphan
    // storage pressure is deliberately NOT subtracted from the active
    // transport budget -- a peer that holds many orphans must still be able
    // to request and download the missing chain.  Orphan storage is bounded
    // separately by the hard storage caps (MAX_ORPHAN_BLOCKS_PER_PEER /
    // DEFAULT_MAX_ORPHAN_BLOCKS) enforced on the receive path.
    const int nPeerActivePressure = nQueuedBlockRequests + nSentBlockRequests;
    bool fVNodesLockFailed = false;
    const int nGlobalActivePressure =
        CountGlobalActiveBlockRequests(pfrom, &fVNodesLockFailed);
    if (pnOrphanCountPeer)
        *pnOrphanCountPeer = nOrphanCountPeer;
    if (pnQueuedBlockRequests)
        *pnQueuedBlockRequests = nQueuedBlockRequests;
    if (pnSentBlockRequests)
        *pnSentBlockRequests = nSentBlockRequests;
    if (pnPeerActivePressure)
        *pnPeerActivePressure = nPeerActivePressure;
    if (pnGlobalActivePressure)
        *pnGlobalActivePressure = nGlobalActivePressure;

    const int nPeerBudget = GetMaxActiveBlockRequestsPerPeer() - nPeerActivePressure;
    const int nGlobalBudget = GetMaxActiveBlockRequestsGlobal() - nGlobalActivePressure;
    const int nBudget = std::max(0, std::min(nPeerBudget, nGlobalBudget));

    {
        ibdmetrics::Counters& metrics = ibdmetrics::Get();
        metrics.scheduler_budget_per_peer.store(
            GetMaxActiveBlockRequestsPerPeer(), std::memory_order_relaxed);
        metrics.scheduler_budget_global.store(
            GetMaxActiveBlockRequestsGlobal(), std::memory_order_relaxed);
        metrics.deferred_budget_calls.fetch_add(1, std::memory_order_relaxed);
        if (nBudget > 0)
            metrics.deferred_budget_positive.fetch_add(
                1, std::memory_order_relaxed);
        else
        {
            metrics.deferred_budget_zero.fetch_add(1, std::memory_order_relaxed);
            if (nPeerBudget <= 0)
                metrics.deferred_budget_zero_peer_pressure.fetch_add(
                    1, std::memory_order_relaxed);
            if (nGlobalBudget <= 0)
                metrics.deferred_budget_zero_global_pressure.fetch_add(
                    1, std::memory_order_relaxed);
            if (fVNodesLockFailed)
                metrics.deferred_budget_zero_vnodes_lock_failed.fetch_add(
                    1, std::memory_order_relaxed);
        }
        ibdmetrics::AtomicMax(metrics.peer_pressure_max, nPeerActivePressure);
        ibdmetrics::AtomicMax(metrics.orphan_pressure_max, nOrphanCountPeer);
    }

    const int nZero = nBudget == 0 ? 1 : 0;
    const int nOldZero = pfrom->nDeferredBudgetZero.exchange(
        nZero, std::memory_order_relaxed);
    ibdmetrics::PeerZeroStateChange(nOldZero, nZero);

    return nBudget;
}

static void RecordIbdHeaderSchedulerInvAvailability(CNode* pfrom,
                                                    const uint256& hash)
{
    if (pfrom == NULL || hash == 0)
        return;
    if (g_ibdHeaderSchedulerState.invAvailability[hash].insert(pfrom->GetId()).second)
        ++g_ibdHeaderSchedulerState.availabilityEpoch;
}

struct IbdHeaderSchedulerActiveAncestorEvidence
{
    std::vector<std::set<NodeId> > exactSources;
    std::map<int64_t, int> deepestActiveHeight;
};

static IbdHeaderSchedulerActiveAncestorEvidence
IbdHeaderSchedulerBuildActiveAncestorEvidence(
    const std::vector<uint256>& activeWindow)
{
    IbdHeaderSchedulerActiveAncestorEvidence evidence;
    evidence.exactSources.resize(activeWindow.size());
    evidence.deepestActiveHeight =
        g_ibdHeadersObserver.ActiveHeaderSourceClaims();
    for (size_t i = 0; i < activeWindow.size(); ++i)
    {
        const std::vector<int64_t> sources =
            g_ibdHeadersObserver.HeaderSources(activeWindow[i]);
        evidence.exactSources[i].insert(sources.begin(), sources.end());
    }
    return evidence;
}


int GetIbdHeaderHeadPrefixHeight()
{
    const std::size_t W = GetIbdBlockWindow();
    return (int)std::min<std::size_t>(512, std::max<std::size_t>(64, W / 8));
}

int GetIbdHeaderRedundancyPrefixHeight()
{
    const std::size_t W = GetIbdBlockWindow();
    return (int)std::min<std::size_t>(256, std::max<std::size_t>(32, W / 32));
}

static bool IbdHeaderSchedulerHashNeedsRedundancy(int nHeight,
                                                  int nFrontierHeight)
{
    if (nHeight <= nFrontierHeight)
        return false;
    const int nRedundancyPrefix = GetIbdHeaderRedundancyPrefixHeight();
    return nHeight <= nFrontierHeight + nRedundancyPrefix;
}

// Stage 4 hard quality gate: for head-prefix positions, a candidate peer must
// have (1) at least 2 confirmed recent block deliveries, (2) an EWMA delivery
// latency no more than 1.5x the median across active peers with samples, and
// (3) no timeout in the last IBD_PEER_QUALITY_TIMEOUT_COOLDOWN_US.  To avoid
// draining the pipeline when history is insufficient or every peer fails the
// criteria, the gate is bypassed when the filtered set would be empty.
static bool IbdPeerQualifiesForHeadPrefix(CNode* pnode,
    int64_t nMedianLatencyUs, bool fHaveEnoughHistory)
{
    if (pnode == NULL) return false;
    const IbdPeerQualitySnapshot snap = pnode->GetIbdQualitySnapshot();
    // Criterion 1: at least 2 confirmed recent block deliveries.
    if (snap.releases_by_receive < 2)
        return false;
    // Criterion 3: not in timeout cooldown.
    if (snap.last_timeout_time_us > 0)
    {
        const int64_t nNowUs = CNode::QualityNowUs();
        if (nNowUs - snap.last_timeout_time_us <
            IBD_PEER_QUALITY_TIMEOUT_COOLDOWN_US)
            return false;
    }
    // Criterion 2: latency <= 1.5 * median (only if enough history exists).
    if (fHaveEnoughHistory && snap.has_latency_sample &&
        snap.latency_ewma_us > nMedianLatencyUs + nMedianLatencyUs / 2)
        return false;
    return true;
}

struct IbdHeaderSchedulerCandidateSet
{
    std::vector<CNode*> qualified;
    std::vector<CNode*> all;
};

static IbdHeaderSchedulerCandidateSet IbdHeaderSchedulerCandidatePeers(
    const uint256& hash, size_t activeIndex, int activeHeight,
    const IbdHeaderSchedulerActiveAncestorEvidence& evidence,
    const std::vector<CNode*>& vNodesCopy,
    int nFrontierHeight)
{
    IbdHeaderSchedulerCandidateSet result;

    // BlockCandidateSources(H) = ExactSources(H) union InvAvailability(H)
    // union InferredActiveAncestorSources(H).  The inferred set contains a
    // peer only when its deepest exact claim is STRICTLY above H in this
    // current active-window snapshot.  Reported heights are never evidence.
    std::set<NodeId> peerIds;
    if (activeIndex < evidence.exactSources.size())
        peerIds.insert(evidence.exactSources[activeIndex].begin(),
                       evidence.exactSources[activeIndex].end());
    for (std::map<int64_t, int>::const_iterator it =
             evidence.deepestActiveHeight.begin();
         it != evidence.deepestActiveHeight.end(); ++it)
        if (it->second > activeHeight)
            peerIds.insert(it->first);
    std::map<uint256, std::set<NodeId> >::const_iterator itAvail =
        g_ibdHeaderSchedulerState.invAvailability.find(hash);
    if (itAvail != g_ibdHeaderSchedulerState.invAvailability.end())
        peerIds.insert(itAvail->second.begin(), itAvail->second.end());

    std::vector<CNode*> all;
    for (std::vector<CNode*>::const_iterator it = vNodesCopy.begin();
         it != vNodesCopy.end(); ++it)
    {
        CNode* pnode = *it;
        if (pnode == NULL || pnode->fDisconnect || pnode->fClient ||
            pnode->fOneShot || pnode->nVersion == 0)
            continue;
        if (peerIds.count(pnode->GetId()) == 0)
            continue;
        all.push_back(pnode);
    }

    // Stage 4 hard quality gate: only for the head-prefix region
    // (frontier, frontier + HP].  Outside the head-prefix the existing
    // candidate rules are unchanged.
    std::vector<CNode*> qualified;
    const std::size_t nHeadPrefix = GetIbdHeaderHeadPrefixHeight();
    if (activeHeight > nFrontierHeight &&
        (std::size_t)(activeHeight - nFrontierHeight) <= nHeadPrefix)
    {
        // Collect EWMA latencies from all active peers that have samples, so
        // the median is computed over the peer population, not only over peers
        // that happen to be candidates for this hash.
        std::vector<int64_t> vLatencies;
        for (std::vector<CNode*>::const_iterator it = vNodesCopy.begin();
             it != vNodesCopy.end(); ++it)
        {
            CNode* pnode = *it;
            if (pnode == NULL || pnode->fDisconnect || pnode->fClient ||
                pnode->fOneShot || pnode->nVersion == 0)
                continue;
            const IbdPeerQualitySnapshot snap = pnode->GetIbdQualitySnapshot();
            if (snap.has_latency_sample)
                vLatencies.push_back(snap.latency_ewma_us);
        }
        const bool fHaveEnoughHistory = vLatencies.size() >= 2;
        int64_t nMedianLatencyUs = 0;
        if (fHaveEnoughHistory)
        {
            std::sort(vLatencies.begin(), vLatencies.end());
            const size_t n = vLatencies.size();
            nMedianLatencyUs = (vLatencies[(n - 1) / 2] + vLatencies[n / 2]) / 2;
        }

        for (std::vector<CNode*>::const_iterator it = all.begin();
             it != all.end(); ++it)
        {
            if (IbdPeerQualifiesForHeadPrefix(*it, nMedianLatencyUs,
                                              fHaveEnoughHistory))
                qualified.push_back(*it);
        }
    }

    // Primary selection uses the qualified set when non-empty, otherwise falls
    // back to all candidates.  Backup selection always uses the qualified set
    // (empty outside the head-prefix or when no peer passes the gate).
    result.qualified = qualified;
    result.all = qualified.empty() ? all : qualified;
    return result;
}

static int64_t IbdHeaderSchedulerPeerScore(const CNode* pnode,
                                          int64_t nNow,
                                          int64_t nMaxPeerHeight)
{
    int64_t nPeerHeight = std::max((int64_t)pnode->nBestKnownHeight,
                                   (int64_t)pnode->nChainHeight);
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
    if (pnode->nPingUsecTime > 0)
    {
        const int64_t nPingBonus = 300000 - (pnode->nPingUsecTime / 10);
        if (nPingBonus > 0)
            nScore += nPingBonus;
    }
    if (nNow - pnode->nTimeConnected < 120)
        nScore += 10000;
    if (pnode->fDisconnect)
        nScore -= 5000000;
    return nScore;
}

static CNode* IbdHeaderSchedulerBestPeerForHash(
    const uint256& hash,
    const std::vector<CNode*>& candidates,
    std::map<CNode*, int>* pBudgetByPeer = NULL)
{
    if (candidates.empty())
        return NULL;

    // A timed-out hash should escape its last owner when another proven
    // candidate can admit it now.  Keep the old owner as the liveness
    // fallback when every alternative is saturated or unavailable.
    NodeId nLastTimeoutOwner = -1;
    const bool fHasLastTimeoutOwner =
        GetBlockLastTimeoutOwner(hash, &nLastTimeoutOwner);
    bool fHasUsableAlternative = false;
    if (fHasLastTimeoutOwner)
    {
        for (std::vector<CNode*>::const_iterator it = candidates.begin();
             it != candidates.end(); ++it)
        {
            CNode* pnode = *it;
            if (pnode == NULL || pnode->GetId() == nLastTimeoutOwner)
                continue;
            int nBudget = 0;
            if (pBudgetByPeer != NULL)
            {
                std::map<CNode*, int>::iterator itB =
                    pBudgetByPeer->find(pnode);
                if (itB == pBudgetByPeer->end())
                    itB = pBudgetByPeer->insert(std::make_pair(
                        pnode, GetDeferredBlockRequestBudget(pnode))).first;
                nBudget = itB->second;
            }
            else
            {
                nBudget = GetDeferredBlockRequestBudget(pnode);
            }
            if (nBudget > 0)
            {
                fHasUsableAlternative = true;
                break;
            }
        }
    }

    std::vector<CNode*> eligibleCandidates;
    for (std::vector<CNode*>::const_iterator it = candidates.begin();
         it != candidates.end(); ++it)
    {
        CNode* pnode = *it;
        if (fHasUsableAlternative && pnode != NULL &&
            pnode->GetId() == nLastTimeoutOwner)
            continue;
        eligibleCandidates.push_back(pnode);
    }

    std::vector<CNode*> preferred;
    std::map<uint256, std::set<NodeId> >::const_iterator itPreferred =
        g_ibdHeaderSchedulerState.invAvailability.find(hash);
    if (itPreferred != g_ibdHeaderSchedulerState.invAvailability.end())
    {
        for (std::vector<CNode*>::const_iterator it = eligibleCandidates.begin();
             it != eligibleCandidates.end(); ++it)
        {
            CNode* pnode = *it;
            if (itPreferred->second.count(pnode->GetId()) != 0)
                preferred.push_back(pnode);
        }
    }

    const std::vector<CNode*>& selectionPool =
        preferred.empty() ? eligibleCandidates : preferred;
    int64_t nMaxPeerHeight = nBestHeight;
    for (std::vector<CNode*>::const_iterator it = selectionPool.begin();
         it != selectionPool.end(); ++it)
    {
        const CNode* pnode = *it;
        const int64_t nPeerHeight = std::max(
            (int64_t)pnode->nBestKnownHeight, (int64_t)pnode->nChainHeight);
        nMaxPeerHeight = std::max(nMaxPeerHeight, nPeerHeight);
    }

    // Budget-aware ranking (Finding 1): a candidate that has no spare request
    // budget cannot admit this hash, so it must not block admission while an
    // admissible candidate exists.  Saturated candidates are skipped unless
    // every candidate is saturated, in which case the score-best peer is
    // returned so the caller parks the cursor and retries later (budget
    // self-heals once the saturated peer's in-flight requests drain).  When no
    // budget cache is supplied (recovery path) every candidate is admissible
    // and this reduces to the original score ranking.
    CNode* pBest = NULL;
    int64_t nBestScore = std::numeric_limits<int64_t>::min();
    int32_t nBestPressure = 0;
    CNode* pFallback = NULL;
    int64_t nFallbackScore = std::numeric_limits<int64_t>::min();
    int32_t nFallbackPressure = 0;
    for (std::vector<CNode*>::const_iterator it = selectionPool.begin();
         it != selectionPool.end(); ++it)
    {
        CNode* pnode = *it;
        const int64_t nScore =
            IbdHeaderSchedulerPeerScore(pnode, GetTime(), nMaxPeerHeight);
        const int32_t nPressure =
            pnode->peerLiveActivePressure.load(std::memory_order_relaxed);
        if (pFallback == NULL || nScore > nFallbackScore ||
            (nScore == nFallbackScore && nPressure < nFallbackPressure) ||
            (nScore == nFallbackScore && nPressure == nFallbackPressure &&
             pnode->GetId() < pFallback->GetId()))
        {
            pFallback = pnode;
            nFallbackScore = nScore;
            nFallbackPressure = nPressure;
        }
        if (pBudgetByPeer != NULL)
        {
            std::map<CNode*, int>::iterator itB = pBudgetByPeer->find(pnode);
            if (itB == pBudgetByPeer->end())
                itB = pBudgetByPeer->insert(std::make_pair(
                    pnode, GetDeferredBlockRequestBudget(pnode))).first;
            if (itB->second <= 0)
                continue;
        }
        if (pBest == NULL || nScore > nBestScore ||
            (nScore == nBestScore && nPressure < nBestPressure) ||
            (nScore == nBestScore && nPressure == nBestPressure &&
             pnode->GetId() < pBest->GetId()))
        {
            pBest = pnode;
            nBestScore = nScore;
            nBestPressure = nPressure;
        }
    }
    if (pBest == NULL)
        pBest = pFallback;
    return pBest;
}

static CNode* IbdHeaderSchedulerBestPeerForHashExcluding(
    const uint256& hash,
    const std::vector<CNode*>& candidates,
    const std::set<NodeId>& excluded,
    std::map<CNode*, int>* pBudgetByPeer = NULL)
{
    std::vector<CNode*> filtered;
    filtered.reserve(candidates.size());
    for (std::vector<CNode*>::const_iterator it = candidates.begin();
         it != candidates.end(); ++it)
    {
        CNode* pnode = *it;
        if (pnode != NULL && excluded.count(pnode->GetId()) == 0)
            filtered.push_back(pnode);
    }
    if (filtered.empty())
        return NULL;
    return IbdHeaderSchedulerBestPeerForHash(hash, filtered, pBudgetByPeer);
}

static bool IbdHeaderSchedulerHashIsQueuedOnAnyPeer(
    const uint256& hash, const std::vector<CNode*>& vNodesCopy)
{
    for (std::vector<CNode*>::const_iterator pit = vNodesCopy.begin();
         pit != vNodesCopy.end(); ++pit)
        if (*pit && (*pit)->IsBlockAskForQueued(hash))
            return true;
    return false;
}

static size_t RecoverOrderedReleasedBlocks(
    CNode* pto, const std::vector<CNode*>& vNodesCopy)
{
    AssertLockHeld(cs_main);
    const int64_t recoveryStartUs = GetTimeMicros();
    if (!IbdHeaderSchedulerSelectActive() || pto == NULL || pindexBest == NULL ||
        pto->fDisconnect || pto->fClient || pto->fOneShot ||
        !IbdHeadersControlPlaneEnabled())
        return 0;

    const std::vector<uint256>& window =
        g_ibdHeaderSchedulerState.cursorWindow;
    const size_t nBound = std::min(
        g_ibdHeaderSchedulerState.cursorNextIndex, window.size());
    if (nBound == 0)
    {
        g_ibdHeaderSchedulerState.cursorRecoveryNeeded = false;
        return 0;
    }

    int nBudget = GetDeferredBlockRequestBudget(pto);
    size_t nAdmitted = 0;
    bool fUnresolved = false;
    const IbdHeaderSchedulerActiveAncestorEvidence evidence =
        IbdHeaderSchedulerBuildActiveAncestorEvidence(window);

    CTxDB txdb("r");
    for (size_t i = 0; i < nBound; ++i)
    {
        const uint256& hash = window[i];
        if (GetBlockRequestOwnerDetails(hash, NULL, NULL, NULL))
            continue;
        if (IbdHeaderSchedulerHashIsQueuedOnAnyPeer(hash, vNodesCopy))
            continue;
        if (AlreadyHave(txdb, CInv(MSG_BLOCK, hash)))
            continue;
        if (nBudget <= 0)
        {
            fUnresolved = true;
            break;
        }
        const CIbdHeaderNode* node = g_ibdHeadersObserver.Graph().Lookup(hash);
        if (node == NULL || node->state != CIbdHeaderNode::ACTIVE)
        {
            fUnresolved = true;
            break;
        }
        const IbdHeaderSchedulerCandidateSet candidates =
            IbdHeaderSchedulerCandidatePeers(hash, i, node->height,
                                             evidence, vNodesCopy,
                                             pindexBest->nHeight);
        if (candidates.all.empty())
        {
            fUnresolved = true;
            break;
        }
        CNode* pBest = IbdHeaderSchedulerBestPeerForHash(hash, candidates.all);
        if (pBest != pto)
            continue;
        if (pto->AskFor(CInv(MSG_BLOCK, hash),
                        BLOCKREQ_SOURCE_HEADERS_SCHEDULER) != ASKFOR_QUEUED)
        {
            fUnresolved = true;
            break;
        }
        ++nAdmitted;
        --nBudget;
        ++g_ibdHeaderSchedulerState.refillAdmissions;
        // Phase 2: frontier baseline for progress-aware expiration.  A
        // recovered hash is re-admitted at the current frontier.
        g_ibdHeaderSchedulerState.orderedRequestFrontierBaseline[hash] =
            pindexBest->nHeight;
    }
    g_ibdHeaderSchedulerState.cursorRecoveryNeeded = fUnresolved;
    g_ibdHeaderSchedulerState.incrementalRefillUs +=
        GetTimeMicros() - recoveryStartUs;
    return nAdmitted;
}

// A branch switch invalidates only the queued portion of the old ordered
// suffix. Already-sent requests remain in flight so the new path cannot
// immediately create duplicate wire requests. Both paths are bounded ordered
// windows; this does not scan the header graph or block history.
static size_t PurgeObsoleteOrderedQueuedWork(
    const std::vector<uint256>& oldWindow,
    const std::vector<uint256>& newWindow,
    const std::vector<CNode*>& vNodesCopy)
{
    std::set<uint256> newPath(newWindow.begin(), newWindow.end());
    std::set<uint256> obsolete;
    for (std::vector<uint256>::const_iterator it = oldWindow.begin();
         it != oldWindow.end(); ++it)
        if (newPath.count(*it) == 0)
            obsolete.insert(*it);

    for (std::set<uint256>::const_iterator it = obsolete.begin();
         it != obsolete.end(); ++it)
        g_ibdHeaderSchedulerState.orderedRequestFrontierBaseline.erase(*it);
    if (g_ibdHeaderSchedulerState.orderedRequestFrontierBaseline.empty())
    {
        g_ibdHeaderSchedulerState.orderedRequestFrontierBaselinePruneCursor = 0;
        g_ibdHeaderSchedulerState.orderedRequestFrontierBaselinePruneCursorValid = false;
    }

    size_t nPurged = 0;
    for (std::vector<CNode*>::const_iterator pit = vNodesCopy.begin();
         pit != vNodesCopy.end(); ++pit)
    {
        CNode* pnode = *pit;
        if (pnode == NULL || obsolete.empty())
            continue;
        for (std::multimap<int64_t, CInv>::iterator it = pnode->mapAskFor.begin();
             it != pnode->mapAskFor.end();)
        {
            const CInv inv = it->second;
            if ((inv.type != MSG_BLOCK && inv.type != MSG_FILTERED_BLOCK) ||
                obsolete.count(inv.hash) == 0)
            {
                ++it;
                continue;
            }

            NodeId ownerPeer = -1;
            BlockRequestOwnerState ownerState = BLOCK_REQUEST_OWNER_QUEUED;
            const bool fHasOwner = GetBlockRequestOwner(
                inv.hash, &ownerPeer, &ownerState);
            // Never remove a different peer's live owner, and never touch an
            // already in-flight request during a branch handoff.
            if ((fHasOwner && ownerPeer != pnode->GetId()) ||
                (fHasOwner && ownerState != BLOCK_REQUEST_OWNER_QUEUED))
            {
                ++it;
                continue;
            }

            std::multimap<int64_t, CInv>::iterator eraseIt = it++;
            pnode->EraseAskForEntry(
                eraseIt, true,
                ibdmetrics::ACTIVE_DECREMENT_OTHER,
                "branch-switch");
            ++nPurged;
        }
    }
    return nPurged;
}

// FRONT_PREEMPT (v1): migrate the ordered head's in-flight slot to a proven
// alternative peer.  Runs inside the cursor-owning refill walk only, when the
// walk reaches the head (nGap == 1) entry and finds it owned IN_FLIGHT.  The
// decision requires the head to have been in flight on the wire past the
// preempt threshold (GetIbdOrderedPreemptWireAgeUs()); the migration itself is
// atomic and gauge-neutral (net.cpp PreemptBlockRequestToPeer), so the ordered
// frontier keeps exactly one active owner and forward progress is preserved.
static bool MaybePreemptOrderedHeadSlot(
    const uint256& hash, NodeId ownerPeer,
    const std::vector<CNode*>& candidates,
    const std::vector<CNode*>& vNodesCopy)
{
    AssertLockHeld(cs_main);
    if (ownerPeer < 0)
        return false;
    CNode* pOwner = NULL;
    for (size_t i = 0; i < vNodesCopy.size(); ++i)
    {
        if (vNodesCopy[i] != NULL && vNodesCopy[i]->GetId() == ownerPeer)
        {
            pOwner = vNodesCopy[i];
            break;
        }
    }
    if (pOwner == NULL || pOwner->fDisconnect)
    {
        ibdmetrics::Get().front_preempt_abort_no_target.fetch_add(
            1, std::memory_order_relaxed);
        return false;
    }
    // Wire age: the head must have been in flight on the wire past the preempt
    // threshold.  A pending-wire stamp (0) means the getdata never reached the
    // socket -- a local send-path failure handled by ordinary expiry, not a
    // supplier-quality question.
    int64_t nWireUs = 0;
    {
        LOCK(pOwner->cs_vBlockInFlightWire);
        std::map<uint256, int64_t>::const_iterator itWire =
            pOwner->mapBlockInFlightWireUs.find(hash);
        if (itWire != pOwner->mapBlockInFlightWireUs.end())
            nWireUs = itWire->second;
    }
    const int64_t nNowUs = CNode::QualityNowUs();
    if (nWireUs <= 0 || nNowUs - nWireUs < GetIbdOrderedPreemptWireAgeUs())
    {
        ibdmetrics::Get().front_preempt_abort_wire_young.fetch_add(
            1, std::memory_order_relaxed);
        return false;
    }
    // Pick the best proven alternative: highest scheduler score among
    // candidates that are not the current owner, do not already carry the hash,
    // and have deferred budget to admit it.  The ranking mirrors the admission
    // path (best-by-score with no sign floor): a candidate that announced the
    // exact frontier header is a proven supplier even when its reported height
    // is stale, and a negative score only orders candidates, it does not
    // disqualify them.
    CNode* pTarget = NULL;
    int64_t nTargetScore = std::numeric_limits<int64_t>::min();
    const int64_t nNow = GetTime();
    for (size_t i = 0; i < candidates.size(); ++i)
    {
        CNode* pnode = candidates[i];
        if (pnode == NULL || pnode->GetId() == ownerPeer || pnode->fDisconnect ||
            pnode->fClient || pnode->fOneShot || pnode->nVersion == 0)
            continue;
        if (pnode->IsBlockAskForQueued(hash) || pnode->IsBlockInFlight(hash))
            continue;
        if (GetDeferredBlockRequestBudget(pnode) <= 0)
            continue;
        const int64_t nScore = IbdHeaderSchedulerPeerScore(
            pnode, nNow, pindexBest != NULL ? pindexBest->nHeight : 0);
        if (pTarget == NULL || nScore > nTargetScore)
        {
            nTargetScore = nScore;
            pTarget = pnode;
        }
    }
    if (pTarget == NULL)
    {
        ibdmetrics::Get().front_preempt_abort_no_target.fetch_add(
            1, std::memory_order_relaxed);
        return false;
    }
    if (!PreemptBlockRequestToPeer(hash, pOwner, pTarget))
    {
        ibdmetrics::Get().front_preempt_abort_transfer_failed.fetch_add(
            1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

static size_t RefillOrderedHeaderBlockRequests(
    CNode* pto, const std::vector<CNode*>& vNodesCopy)
{
    AssertLockHeld(cs_main);
    const int64_t refillStartUs = GetTimeMicros();
    if (!IbdHeaderSchedulerSelectActive() || pto == NULL || pindexBest == NULL ||
        pto->fDisconnect || pto->fClient || pto->fOneShot ||
        !IbdHeadersControlPlaneEnabled())
        return 0;

    g_ibdHeadersObserver.SetEnabled(true);
    g_ibdHeadersObserver.SetLookaheadCap(
        GetIbdBlockWindow() + IBD_HEADER_LOOKAHEAD_CAP_MARGIN,
        GetIbdBlockWindow() + IBD_HEADER_LOOKAHEAD_RESUME_MARGIN);
    if (!g_ibdHeadersObserver.UpdateAnchor(pindexBest->GetBlockHash(),
                                           pindexBest->nHeight))
        return 0;

    ++g_ibdHeaderSchedulerState.refillCalls;
    const uint256 hashFrontier = pindexBest->GetBlockHash();
    const CIbdHeaderGraph& graph = g_ibdHeadersObserver.Graph();
    const CIbdHeaderNode* graphTip = graph.ActiveTip();
    const int nLookahead = graphTip ? graphTip->height - pindexBest->nHeight : -1;
    std::vector<NodeId> peerIds;
    for (std::vector<CNode*>::const_iterator pit = vNodesCopy.begin();
         pit != vNodesCopy.end(); ++pit)
        if (*pit) peerIds.push_back((*pit)->GetId());
    std::sort(peerIds.begin(), peerIds.end());

    std::vector<uint256> window;
    std::vector<uint256> incrementalEntries;
    bool incremental = false;
    size_t frontierDelta = 0;
    const bool fCursorUsable =
        g_ibdHeaderSchedulerState.cursorValid &&
        !g_ibdHeaderSchedulerState.cursorInvalidated &&
        g_ibdHeaderSchedulerState.cursorAvailabilityEpoch ==
            g_ibdHeaderSchedulerState.availabilityEpoch &&
        g_ibdHeaderSchedulerState.cursorPeers == peerIds && graphTip &&
        (g_ibdHeaderSchedulerState.cursorTip == graphTip->hash ||
         graph.IsDescendantOf(graphTip->hash,
                              g_ibdHeaderSchedulerState.cursorTip));

    // A graph-tip move to an incompatible active path is a generation
    // handoff, not an ordinary cursor invalidation. Purge obsolete queued
    // work before the new full-window refill can admit anything. In-flight
    // old-path requests are deliberately left for normal receive/timeout
    // cleanup and are not reissued here.
    const bool fBranchSwitch =
        g_ibdHeaderSchedulerState.cursorValid &&
        !g_ibdHeaderSchedulerState.cursorWindow.empty() && graphTip &&
        g_ibdHeaderSchedulerState.cursorTip != uint256(0) &&
        !graph.IsDescendantOf(graphTip->hash,
                              g_ibdHeaderSchedulerState.cursorTip);
    if (fBranchSwitch)
    {
        const std::vector<uint256> newBranchWindow =
            graph.GetActiveWindow(hashFrontier,
                                  GetIbdBlockWindow());
        PurgeObsoleteOrderedQueuedWork(
            g_ibdHeaderSchedulerState.cursorWindow,
            newBranchWindow, vNodesCopy);
        g_ibdHeaderSchedulerState.cursorInvalidated = true;
        g_ibdHeaderSchedulerState.cursorRecoveryNeeded = false;
    }

    // Released-work recovery is bounded per peer and never mutates the
    // ordered cursor, so every peer's refill pass may recover its own
    // released hashes in the same round.  Recovery must also run while an
    // incremental backlog exists (cursorPendingSlots > 0): a released hole
    // sits in the already-claimed prefix [0, cursorNextIndex), ahead of the
    // backlog, so it must be re-admitted before the incremental cursor can
    // advance past it.  The recovery scan and the incremental path are
    // independent, so deferring recovery until the backlog drains would
    // stall the pipeline on the hole.
    //
    // Livelock fix: recovery runs and then FALLS THROUGH to the round-gated
    // refill below.  Previously this branch returned immediately whenever
    // cursorRecoveryNeeded was set, so an unresolved released prefix hole (no
    // candidate peer, exhausted budget, or AskFor refusal) made every pass of
    // every round re-run recovery and exit before the round gate: the ordered
    // cursor was never advanced, no new work was admitted, and the scheduler
    // stalled forever at a stable chain height.  Recovery is bounded, does not
    // clear cursorRecoveryNeeded merely to force progress (it stays true while
    // the hole is unresolved), and never mutates the cursor; the round gate
    // below still allows exactly one cursor-mutating refill pass per round.
    // The returned admission count always includes recovered hashes so a pass
    // whose refill is gated out still reports the work its recovery admitted.
    size_t nRecoveryAdmitted = 0;
    if (fCursorUsable &&
        pindexBest->nHeight == g_ibdHeaderSchedulerState.cursorHeight &&
        g_ibdHeaderSchedulerState.cursorRecoveryNeeded)
    {
        nRecoveryAdmitted = RecoverOrderedReleasedBlocks(pto, vNodesCopy);
        ++g_ibdHeaderSchedulerState.incrementalRefillCalls;
    }

    // Exactly one refill pass per scheduling round owns the ordered cursor.
    // The pass that first observes the current round walks the window and
    // advances the cursor once; every other pass in the same round is a
    // no-op.  This makes cursor mutation global/round-scoped instead of
    // per-peer, which is the prerequisite for incremental refill with any
    // number of peers.
    if (!fBranchSwitch && g_ibdHeaderSchedulerState.refillRound ==
        g_ibdHeaderSchedulerState.cursorRound)
        return nRecoveryAdmitted;
    g_ibdHeaderSchedulerState.cursorRound =
        g_ibdHeaderSchedulerState.refillRound;

    // Phase 2 bounded housekeeping: prune a fixed budget of obsolete baseline
    // entries per refill round instead of rescanning the full map each pass.
    PruneOrderedRequestFrontierBaselineBounded(graph, pindexBest->nHeight);

    if (fCursorUsable)
    {
        if (pindexBest->nHeight == g_ibdHeaderSchedulerState.cursorHeight &&
            g_ibdHeaderSchedulerState.cursorPendingSlots > 0 &&
            g_ibdHeaderSchedulerState.cursorNextIndex <
                g_ibdHeaderSchedulerState.cursorWindow.size())
        {
            incrementalEntries.push_back(
                g_ibdHeaderSchedulerState.cursorWindow[
                    g_ibdHeaderSchedulerState.cursorNextIndex]);
            incremental = true;
        }
        if (pindexBest->nHeight == g_ibdHeaderSchedulerState.cursorHeight &&
            g_ibdHeaderSchedulerState.cursorPendingSlots > 0 &&
            g_ibdHeaderSchedulerState.cursorNextIndex >=
                g_ibdHeaderSchedulerState.cursorWindow.size() &&
            g_ibdHeaderSchedulerState.cursorTip == graphTip->hash)
        {
            ++g_ibdHeaderSchedulerState.incrementalRefillCalls;
            g_ibdHeaderSchedulerState.incrementalRefillUs +=
                GetTimeMicros() - refillStartUs;
            return nRecoveryAdmitted;
        }
        if (pindexBest->nHeight > g_ibdHeaderSchedulerState.cursorHeight)
        {
            frontierDelta = (size_t)(pindexBest->nHeight -
                                     g_ibdHeaderSchedulerState.cursorHeight);
            const std::vector<uint256>& oldWindow =
                g_ibdHeaderSchedulerState.cursorWindow;
            if (frontierDelta <= oldWindow.size() &&
                oldWindow[frontierDelta - 1] == hashFrontier)
            {
                std::vector<uint256> shifted(oldWindow.begin() + frontierDelta,
                                             oldWindow.end());
                bool consistent = true;
                for (size_t i = 0; i < frontierDelta; ++i)
                {
                    uint256 successor;
                    if (!graph.GetActiveSuccessor(shifted.empty() ?
                                                      oldWindow.back() :
                                                      shifted.back(),
                                                  successor))
                    {
                        const uint256& tail = shifted.empty() ?
                            oldWindow.back() : shifted.back();
                        if (graphTip->hash != tail)
                            consistent = false;
                        break;
                    }
                    shifted.push_back(successor);
                }
                if (consistent)
                {
                    for (size_t i = 0; i < frontierDelta; ++i)
                        g_ibdHeaderSchedulerState.orderedRequestFrontierBaseline.erase(
                            oldWindow[i]);
                    if (g_ibdHeaderSchedulerState.orderedRequestFrontierBaseline.empty())
                    {
                        g_ibdHeaderSchedulerState.orderedRequestFrontierBaselinePruneCursor = 0;
                        g_ibdHeaderSchedulerState.orderedRequestFrontierBaselinePruneCursorValid = false;
                    }
                    g_ibdHeaderSchedulerState.cursorWindow.swap(shifted);
                    g_ibdHeaderSchedulerState.cursorNextIndex =
                        g_ibdHeaderSchedulerState.cursorNextIndex > frontierDelta
                            ? g_ibdHeaderSchedulerState.cursorNextIndex - frontierDelta
                            : 0;
                    g_ibdHeaderSchedulerState.cursorPendingSlots += frontierDelta;
                    incremental = true;
                    if (g_ibdHeaderSchedulerState.cursorPendingSlots > 0 &&
                        g_ibdHeaderSchedulerState.cursorNextIndex <
                            g_ibdHeaderSchedulerState.cursorWindow.size())
                        incrementalEntries.push_back(
                            g_ibdHeaderSchedulerState.cursorWindow[
                                g_ibdHeaderSchedulerState.cursorNextIndex]);
                }
            }
        }
    }

    if (incremental)
        window = incrementalEntries;
    if (!incremental)
    {
        window = graph.GetActiveWindow(hashFrontier,
                                       GetIbdBlockWindow());
        ++g_ibdHeaderSchedulerState.fullRefillCalls;
        g_ibdHeaderSchedulerState.fullEntriesExamined += window.size();
    }
    else
    {
        ++g_ibdHeaderSchedulerState.incrementalRefillCalls;
        g_ibdHeaderSchedulerState.incrementalEntriesExamined +=
            incrementalEntries.size();
    }
    if (window.empty() && !incremental)
    {
        g_ibdHeaderSchedulerState.cursorValid = false;
        ++g_ibdHeaderSchedulerState.fallbackCount;
        CBlockLocator locator;
        if (!pto->getHeadersSync.IsInFlight() &&
            PrepareIbdHeadersObserverRequest(pto, locator))
            pto->PushHeadersContinuation(locator, uint256(0), "ibd-select-refill");
        printf("IBD_HEADERS_SCHED event=fallback peer=%d reason=window-empty frontier_height=%d graph_tip_height=%d lookahead=%d\n",
               pto->GetId(), pindexBest->nHeight,
               graphTip ? graphTip->height : -1, nLookahead);
        return nRecoveryAdmitted;
    }
    // Stage 3 proactive resume: the header lookahead has decayed into the
    // resume band [W, W+RESUME_MARGIN].  At this point the block window is
    // still full, but the delivery margin is low; top the header graph back
    // up without stalling the block admission walk.  Guarded by the per-peer
    // continuation-in-flight state; in incremental mode the cursor window is
    // already captured, so topping up headers is deferred to the next full
    // refill pass.
    if (!incremental && nLookahead >= 0 &&
        (uint64_t)nLookahead >= GetIbdBlockWindow() &&
        (uint64_t)nLookahead <=
            GetIbdBlockWindow() + IBD_HEADER_LOOKAHEAD_RESUME_MARGIN)
    {
        CBlockLocator locator;
        if (!pto->getHeadersSync.IsInFlight() &&
            PrepareIbdHeadersObserverRequest(pto, locator))
            pto->PushHeadersContinuation(locator, uint256(0), "ibd-select-topup");
    }

    // Per-peer admission budgets: the round owner walks the ordered window
    // once and assigns each requestable hash to its best candidate peer, so
    // work spreads across all eligible peers instead of collapsing onto the
    // peer whose SendMessages pass triggered the walk.
    std::map<CNode*, int> budgetByPeer;
    size_t nHave = 0;
    size_t nOwned = 0;
    size_t nQueued = 0;
    size_t nInflight = 0;
    size_t nRequestable = 0;
    size_t nUnknownAvailability = 0;
    size_t nAdmitted = 0;
    size_t fullNextIndex = window.size();
    bool fullNextIndexSet = false;
    bool fPrefixCovered = true;
    // Finding 2: in incremental mode the walk examines exactly one entry (the
    // cursor head).  It is "resolved" when it was admitted or is already
    // covered (AlreadyHave / owned / queued); only a resolved head may be
    // advanced past.  A requestable head that could not be admitted keeps the
    // cursor parked on it so the released/missing work stays discoverable.
    bool fIncrementalHeadResolved = false;
    // In full mode the ordered cursor must never advance through a truly
    // uncovered prefix: record the first position whose request cannot be
    // made (no candidate peer, no best peer, exhausted budget, or AskFor
    // refusal).  Admission beyond a budget-saturated entry is still allowed
    // (multi-peer refill must not stop at one saturated peer), but the cursor
    // itself stops there so the release/recovery machinery keeps re-examining
    // it; the same guard applies to genuine holes so the cursor does not
    // silently skip past missing ordered work.
    const auto setFullNextIndexAt =
        [&](size_t index)
        {
            if (!incremental && !fullNextIndexSet)
            {
                fullNextIndex = index;
                fullNextIndexSet = true;
            }
        };
    uint256 frontHash = uint256(0);
    int frontHeight = -1;
    NodeId frontOwner = -1;
    BlockRequestOwnerState frontOwnerState = BLOCK_REQUEST_OWNER_QUEUED;
    int64_t frontAssignedUs = 0;
    size_t frontAlternatives = 0;
    CTxDB txdb("r");
    const std::vector<uint256>& evidenceWindow = incremental ?
        g_ibdHeaderSchedulerState.cursorWindow : window;
    const IbdHeaderSchedulerActiveAncestorEvidence evidence =
        IbdHeaderSchedulerBuildActiveAncestorEvidence(evidenceWindow);
    const size_t evidenceOffset = incremental ?
        g_ibdHeaderSchedulerState.cursorNextIndex : 0;

    for (std::vector<uint256>::const_iterator it = window.begin();
         it != window.end(); ++it)
    {
        const uint256& hash = *it;
        const CIbdHeaderNode* node = g_ibdHeadersObserver.Graph().Lookup(hash);
        const int nHeight = node ? node->height : -1;
        if (AlreadyHave(txdb, CInv(MSG_BLOCK, hash)))
        {
            ++nHave;
            fIncrementalHeadResolved = true;
            continue;
        }

        const size_t activeIndex = evidenceOffset +
            (size_t)(it - window.begin());
        const IbdHeaderSchedulerCandidateSet candidates =
            IbdHeaderSchedulerCandidatePeers(hash, activeIndex, nHeight,
                                             evidence, vNodesCopy,
                                             pindexBest->nHeight);
        if (frontHash == 0)
        {
            frontHash = hash;
            frontHeight = nHeight;
            frontAlternatives = candidates.all.size();
        }

        const int nDesiredOwners =
            IbdHeaderSchedulerHashNeedsRedundancy(nHeight, pindexBest->nHeight)
                ? 2 : 1;
        const size_t nOwnerCount = GetBlockRequestOwnerCount(hash);
        if (nOwnerCount >= 1)
        {
            NodeId ownerPeer = -1;
            BlockRequestOwnerState ownerState = BLOCK_REQUEST_OWNER_QUEUED;
            int64_t assignedUs = 0;
            GetBlockRequestOwnerDetails(hash, &ownerPeer, &ownerState,
                                        &assignedUs);
            if (frontHash == hash)
            {
                frontOwner = ownerPeer;
                frontOwnerState = ownerState;
                frontAssignedUs = assignedUs;
            }
            ++nOwned;
            if (ownerState == BLOCK_REQUEST_OWNER_IN_FLIGHT)
            {
                // FRONT_PREEMPT (v1): only attempt when the head has a single
                // active owner.  Stage 5 keeps redundant head slots; adaptive
                // preempt/reassign is Stage 6.
                if (nOwnerCount == 1 && frontHash == hash && node != NULL &&
                    node->IsAnchored() &&
                    node->height == pindexBest->nHeight + 1)
                {
                    ++g_ibdHeaderSchedulerState.frontPreemptAttempts;
                    ibdmetrics::Get().front_preempt_attempts.fetch_add(
                        1, std::memory_order_relaxed);
                    if (MaybePreemptOrderedHeadSlot(
                            hash, ownerPeer, candidates.all, vNodesCopy))
                    {
                        ++g_ibdHeaderSchedulerState.frontPreemptTransfers;
                        ibdmetrics::Get().front_preempt_transfers.fetch_add(
                            1, std::memory_order_relaxed);
                        ++nQueued;
                        fIncrementalHeadResolved = true;
                        continue;
                    }
                }
                ++nInflight;
            }
            else
                ++nQueued;
            fIncrementalHeadResolved = true;

            // Opportunistically top up a missing backup copy inside the
            // redundancy prefix.  Backup failure must not block the logical
            // cursor, so it is never reflected in fullNextIndex.
            if (nOwnerCount < (size_t)nDesiredOwners &&
                !candidates.qualified.empty())
            {
                std::set<NodeId> owners;
                GetBlockRequestOwnerPeers(hash, owners);
                CNode* pBackup = IbdHeaderSchedulerBestPeerForHashExcluding(
                    hash, candidates.qualified, owners, &budgetByPeer);
                if (pBackup != NULL)
                {
                    std::map<CNode*, int>::iterator itBBudget =
                        budgetByPeer.find(pBackup);
                    if (itBBudget == budgetByPeer.end())
                        itBBudget = budgetByPeer.insert(std::make_pair(
                            pBackup,
                            GetDeferredBlockRequestBudget(pBackup))).first;
                    if (itBBudget->second > 0 && fPrefixCovered)
                    {
                    if (pBackup->AskFor(
                            CInv(MSG_BLOCK, hash),
                            BLOCKREQ_SOURCE_HEADERS_SCHEDULER,
                            nDesiredOwners) == ASKFOR_QUEUED)
                    {
                        ++nAdmitted;
                        --itBBudget->second;
                        ++g_ibdHeaderSchedulerState.refillAdmissions;
                        TryAssignBlockRequestOwner(
                            hash, pBackup->GetId(),
                            BLOCKREQ_SOURCE_HEADERS_SCHEDULER,
                            NULL, NULL, nDesiredOwners);
                    }
                }
            }
        }
        continue;
    }

    // A hash already queued on some peer (e.g. re-admitted by the released-
    // work recovery path, which does not claim ownership) is covered work: the
    // getdata send path will claim ownership when it transmits the request.
    // Re-running the no-owner path here would fail AskFor with
    // ASKFOR_ALREADY_QUEUED and incorrectly park the cursor on it.
    if (IbdHeaderSchedulerHashIsQueuedOnAnyPeer(hash, vNodesCopy))
    {
        ++nQueued;
        fIncrementalHeadResolved = true;
        continue;
    }

    // No owner yet: must admit at least the primary copy.
    if (candidates.all.empty())
    {
        ++nUnknownAvailability;
        setFullNextIndexAt((size_t)(it - window.begin()));
        fPrefixCovered = false;
        continue;
    }

        ++nRequestable;
        CNode* pBest =
            IbdHeaderSchedulerBestPeerForHash(hash, candidates.all, &budgetByPeer);
        if (pBest == NULL)
        {
            setFullNextIndexAt((size_t)(it - window.begin()));
            fPrefixCovered = false;
            continue;
        }
        std::map<CNode*, int>::iterator itBudget = budgetByPeer.find(pBest);
        if (itBudget == budgetByPeer.end())
            itBudget = budgetByPeer.insert(std::make_pair(
                pBest, GetDeferredBlockRequestBudget(pBest))).first;
        if (itBudget->second <= 0)
        {
            setFullNextIndexAt((size_t)(it - window.begin()));
            continue;
        }
        if (!fPrefixCovered)
            continue;

        if (pBest->AskFor(CInv(MSG_BLOCK, hash),
                          BLOCKREQ_SOURCE_HEADERS_SCHEDULER,
                          nDesiredOwners) != ASKFOR_QUEUED)
        {
            setFullNextIndexAt((size_t)(it - window.begin()));
            fPrefixCovered = false;
            continue;
        }

        ++nAdmitted;
        fIncrementalHeadResolved = true;
        --itBudget->second;
        ++g_ibdHeaderSchedulerState.refillAdmissions;
        TryAssignBlockRequestOwner(
            hash, pBest->GetId(), BLOCKREQ_SOURCE_HEADERS_SCHEDULER,
            NULL, NULL, nDesiredOwners);
        g_ibdHeaderSchedulerState.orderedRequestFrontierBaseline[hash] =
            pindexBest->nHeight;

        // Inside the redundancy prefix, try to add a backup copy on a
        // different qualified peer.  Backup absence must not block the cursor.
        if (nDesiredOwners > 1 && !candidates.qualified.empty())
        {
            std::set<NodeId> selected;
            selected.insert(pBest->GetId());
            CNode* pBackup = IbdHeaderSchedulerBestPeerForHashExcluding(
                hash, candidates.qualified, selected, &budgetByPeer);
            if (pBackup != NULL)
            {
                std::map<CNode*, int>::iterator itBBudget =
                    budgetByPeer.find(pBackup);
                if (itBBudget == budgetByPeer.end())
                    itBBudget = budgetByPeer.insert(std::make_pair(
                        pBackup,
                        GetDeferredBlockRequestBudget(pBackup))).first;
                if (itBBudget->second > 0 && fPrefixCovered)
                {
                    if (pBackup->AskFor(
                            CInv(MSG_BLOCK, hash),
                            BLOCKREQ_SOURCE_HEADERS_SCHEDULER,
                            nDesiredOwners) == ASKFOR_QUEUED)
                    {
                        ++nAdmitted;
                        --itBBudget->second;
                        ++g_ibdHeaderSchedulerState.refillAdmissions;
                        TryAssignBlockRequestOwner(
                            hash, pBackup->GetId(),
                            BLOCKREQ_SOURCE_HEADERS_SCHEDULER,
                            NULL, NULL, nDesiredOwners);
                    }
                }
            }
        }
    }

    g_ibdHeaderSchedulerState.cursorFrontier = hashFrontier;
    g_ibdHeaderSchedulerState.cursorHeight = pindexBest->nHeight;
    g_ibdHeaderSchedulerState.cursorTip = graphTip ? graphTip->hash : uint256(0);
    g_ibdHeaderSchedulerState.cursorPeers = peerIds;
    g_ibdHeaderSchedulerState.cursorAvailabilityEpoch =
        g_ibdHeaderSchedulerState.availabilityEpoch;
    if (!incremental)
    {
        g_ibdHeaderSchedulerState.cursorWindow = window;
        g_ibdHeaderSchedulerState.cursorNextIndex = fullNextIndex;
        g_ibdHeaderSchedulerState.cursorPendingSlots = 0;
    }
    else if (!incrementalEntries.empty())
    {
        // Finding 2 fix: the incremental cursor advances only past a head
        // entry that was actually resolved (admitted, or already covered as
        // AlreadyHave/owned/queued).  A requestable head that could not be
        // admitted (no candidate, saturated best peer, or AskFor refusal)
        // leaves the cursor on the current hash and flags recovery, so the
        // released/missing work stays discoverable and is re-examined on
        // every round until it resolves -- the same park-and-retry semantics
        // full mode uses for an uncovered prefix.  This preserves the
        // recovery fall-through (recovery never returns early).
        if (fIncrementalHeadResolved)
        {
            ++g_ibdHeaderSchedulerState.cursorNextIndex;
            if (g_ibdHeaderSchedulerState.cursorPendingSlots > 0)
                --g_ibdHeaderSchedulerState.cursorPendingSlots;
        }
        else
        {
            g_ibdHeaderSchedulerState.cursorRecoveryNeeded = true;
        }
    }
    g_ibdHeaderSchedulerState.cursorValid = incremental || !window.empty();
    g_ibdHeaderSchedulerState.cursorInvalidated = false;
    if (incremental)
    {
        g_ibdHeaderSchedulerState.incrementalAdmitted += nAdmitted;
        g_ibdHeaderSchedulerState.incrementalRefillUs +=
            GetTimeMicros() - refillStartUs;
    }
    else
        g_ibdHeaderSchedulerState.fullRefillUs +=
            GetTimeMicros() - refillStartUs;
    printf("IBD_HEADERS_SCHED event=refill peer=%d frontier_height=%d graph_tip_height=%d lookahead=%d window_size=%zu front_hash=%s front_height=%d front_owner=%d front_owner_state=%s front_age_us=%lld front_alternatives=%zu have=%zu owned=%zu queued=%zu inflight=%zu requestable=%zu unknown_availability=%zu admitted=%zu peer_pressure=%d mode=%s duration_us=%lld incremental_calls=%llu full_calls=%llu incremental_examined=%llu full_examined=%llu incremental_admitted=%llu\n",
           pto->GetId(), pindexBest->nHeight,
           graphTip ? graphTip->height : -1, nLookahead, window.size(),
           frontHash == 0 ? "none" : frontHash.ToString().c_str(),
           frontHeight, frontOwner,
           frontHash == 0 ? "none" : BlockRequestOwnerStateName(frontOwnerState),
           frontAssignedUs > 0 ? (long long)(GetTimeMicros() - frontAssignedUs) : -1LL,
           frontAlternatives, nHave, nOwned, nQueued, nInflight,
           nRequestable, nUnknownAvailability, nAdmitted,
           (int)pto->peerLiveActivePressure.load(std::memory_order_relaxed),
           incremental ? "incremental" : "full",
           (long long)(GetTimeMicros() - refillStartUs),
           (unsigned long long)g_ibdHeaderSchedulerState.incrementalRefillCalls,
           (unsigned long long)g_ibdHeaderSchedulerState.fullRefillCalls,
           (unsigned long long)g_ibdHeaderSchedulerState.incrementalEntriesExamined,
           (unsigned long long)g_ibdHeaderSchedulerState.fullEntriesExamined,
            (unsigned long long)g_ibdHeaderSchedulerState.incrementalAdmitted);
    return nAdmitted + nRecoveryAdmitted;
}

size_t RefillOrderedHeaderBlockRequestsForTesting(
    const std::vector<CNode*>& vNodesCopy)
{
    LOCK(cs_main);
    // Each test invocation models one scheduling round: exactly one refill
    // pass owns the cursor, matching the production round boundary.
    AdvanceIbdHeaderSchedulerRound();
    size_t nTotal = 0;
    for (std::vector<CNode*>::const_iterator it = vNodesCopy.begin();
         it != vNodesCopy.end(); ++it)
        nTotal += RefillOrderedHeaderBlockRequests(*it, vNodesCopy);
    return nTotal;
}

bool PrepareIbdHeadersObserverRequestForTesting(CNode* pnode,
    CBlockLocator& locatorOut)
{
    LOCK(cs_main);
    return PrepareIbdHeadersObserverRequest(pnode, locatorOut);
}

IbdHeaderSchedulerRefillStats GetIbdHeaderSchedulerRefillStatsForTesting()
{
    IbdHeaderSchedulerRefillStats out;
    out.incrementalRefillCalls = g_ibdHeaderSchedulerState.incrementalRefillCalls;
    out.fullRefillCalls = g_ibdHeaderSchedulerState.fullRefillCalls;
    out.incrementalEntriesExamined = g_ibdHeaderSchedulerState.incrementalEntriesExamined;
    out.fullEntriesExamined = g_ibdHeaderSchedulerState.fullEntriesExamined;
    out.baselinePruneEntriesExamined = g_ibdHeaderSchedulerState.baselinePruneEntriesExamined;
    out.baselinePruneEntriesErased = g_ibdHeaderSchedulerState.baselinePruneEntriesErased;
    out.incrementalAdmitted = g_ibdHeaderSchedulerState.incrementalAdmitted;
    out.incrementalRefillUs = g_ibdHeaderSchedulerState.incrementalRefillUs;
    out.fullRefillUs = g_ibdHeaderSchedulerState.fullRefillUs;
    out.cursorValid = g_ibdHeaderSchedulerState.cursorValid;
    out.cursorInvalidated = g_ibdHeaderSchedulerState.cursorInvalidated;
    out.cursorRecoveryNeeded = g_ibdHeaderSchedulerState.cursorRecoveryNeeded;
    out.cursorNextIndex = g_ibdHeaderSchedulerState.cursorNextIndex;
    out.cursorPendingSlots = g_ibdHeaderSchedulerState.cursorPendingSlots;
    out.orderedExpiryDeferredDueProgress =
        g_ibdHeaderSchedulerState.orderedExpiryDeferredDueProgress;
    out.orderedExpiryActual = g_ibdHeaderSchedulerState.orderedExpiryActual;
    out.frontPreemptAttempts =
        g_ibdHeaderSchedulerState.frontPreemptAttempts;
    out.frontPreemptTransfers =
        g_ibdHeaderSchedulerState.frontPreemptTransfers;
    return out;
}

void SeedIbdHeaderSchedulerBaselineForTesting(
    const std::vector<std::pair<uint256, int> >& entries)
{
    LOCK(cs_main);
    g_ibdHeaderSchedulerState.orderedRequestFrontierBaseline.clear();
    for (size_t i = 0; i < entries.size(); ++i)
        g_ibdHeaderSchedulerState.orderedRequestFrontierBaseline[entries[i].first] = entries[i].second;
    g_ibdHeaderSchedulerState.orderedRequestFrontierBaselinePruneCursor = 0;
    g_ibdHeaderSchedulerState.orderedRequestFrontierBaselinePruneCursorValid = false;
    g_ibdHeaderSchedulerState.baselinePruneEntriesExamined = 0;
    g_ibdHeaderSchedulerState.baselinePruneEntriesErased = 0;
}

size_t GetIbdHeaderSchedulerBaselineSizeForTesting()
{
    LOCK(cs_main);
    return g_ibdHeaderSchedulerState.orderedRequestFrontierBaseline.size();
}

size_t IbdHeaderSchedulerBaselinePruneBudgetForTesting()
{
    return ORDERED_BASELINE_PRUNE_BUDGET;
}

void RunIbdHeaderSchedulerBaselinePruneForTesting()
{
    LOCK(cs_main);
    if (!pindexBest)
        return;
    g_ibdHeadersObserver.SetEnabled(true);
    g_ibdHeadersObserver.SetLookaheadCap(
        GetIbdBlockWindow() + IBD_HEADER_LOOKAHEAD_CAP_MARGIN,
        GetIbdBlockWindow() + IBD_HEADER_LOOKAHEAD_RESUME_MARGIN);
    if (!g_ibdHeadersObserver.UpdateAnchor(pindexBest->GetBlockHash(),
                                           pindexBest->nHeight))
        return;
    PruneOrderedRequestFrontierBaselineBounded(
        g_ibdHeadersObserver.Graph(), pindexBest->nHeight);
}

std::vector<uint256> GetIbdHeaderSchedulerWindowForTesting(
    const uint256& frontier)
{
    return g_ibdHeadersObserver.PredictedWindowFromFrontier(frontier);
}

// Progress-aware ordered-expiry policy (Phase 2).  The fixed wire-origin
// deadline (60 s, ibd-conservative-block-request-expiration.md) is correct for
// legacy requests but misclassifies deep-but-legitimate ordered descendants: a
// healthy multi-peer pipeline can legitimately hold work hundreds of blocks
// ahead of a frontier that is still advancing (mainnet runtime: ~250 ahead at
// 1-2 block/s), so tail requests age past 60 s before the queue reaches them.
//
// A descendant request is deferred while ALL of the following hold:
//   * it was admitted by the ordered scheduler (baseline recorded), not a
//     legacy request;
//   * it remains deeper than the current ordered head, inside the bounded
//     ordered window (the request has not fallen out of the pipeline);
//   * the frontier has advanced since admission (progress, not stall);
//   * its wire age is below the hard safety ceiling (no infinite protection).
// Otherwise the request is a genuine expiry candidate.  Pending-wire requests
// (getdata never reached the socket) are never deferred: they are a local
// send-path failure, not a frontier-progress question.
//
// Called from CNode::ExpireBlockInFlight under TRY_LOCK(cs_main); must be
// invoked with cs_main held.
static const int64_t IBD_ORDERED_MAX_WIRE_AGE_US = 300LL * 1000000;

IbdOrderedExpiryOutcome IbdHeaderSchedulerOrderedExpiryDecide(
    const uint256& hash, int64_t nNowUs, int64_t nWireUs)
{
    AssertLockHeld(cs_main);
    if (!IbdHeaderSchedulerSelectActive() || pindexBest == NULL)
        return IBD_ORDERED_EXPIRY_NOT_ORDERED;
    std::map<uint256, int>::const_iterator itBaseline =
        g_ibdHeaderSchedulerState.orderedRequestFrontierBaseline.find(hash);
    if (itBaseline ==
        g_ibdHeaderSchedulerState.orderedRequestFrontierBaseline.end())
        return IBD_ORDERED_EXPIRY_NOT_ORDERED;
    const CIbdHeaderNode* node = g_ibdHeadersObserver.Graph().Lookup(hash);
    const int nGap = (node != NULL && node->IsAnchored())
                         ? (node->height - pindexBest->nHeight)
                         : 0;
    // nGap == 1 is the current contiguous-prefix blocker.  It keeps the
    // ordinary wire deadline: historical progress since admission must not
    // protect a descendant after it becomes the ordered head.
    if (nGap > 1 && (size_t)nGap <= GetIbdBlockWindow() &&
        pindexBest->nHeight > itBaseline->second && nWireUs > 0 &&
        nNowUs - nWireUs < IBD_ORDERED_MAX_WIRE_AGE_US)
    {
        ++g_ibdHeaderSchedulerState.orderedExpiryDeferredDueProgress;
        return IBD_ORDERED_EXPIRY_DEFER;
    }
    ++g_ibdHeaderSchedulerState.orderedExpiryActual;
    return IBD_ORDERED_EXPIRY_EXPIRE;
}

static void TraceDeferredWindowState(CNode* pfrom, const char* pszEvent,
                                     const uint256& hash, const char* pszReason,
                                     int nBudget, int nAdmitted, int nDropped)
{
    if (!BlockRequestTraceEnabled() || pfrom == NULL)
        return;
    AssertLockHeld(cs_main);
    int nOrphanCountPeer = 0;
    int nQueuedBlockRequests = 0;
    int nSentBlockRequests = 0;
    int nPeerActivePressure = 0;
    int nGlobalActivePressure = 0;
    GetDeferredBlockRequestBudget(pfrom, &nOrphanCountPeer,
                                  &nQueuedBlockRequests,
                                  &nSentBlockRequests,
                                  &nPeerActivePressure,
                                  &nGlobalActivePressure);
    printf("BLOCKREQTRACE time_us=%lld event=%s peer=%d hash=%s deferred_size=%zu peer_queued=%d peer_inflight=%d global_active=%d orphan_count=%d peer_pressure=%d active_budget=%d active_limit=%d global_limit=%d deferred_limit=%zu reason=%s source=inv admitted=%d dropped=%d\n",
           (long long)GetTimeMicros(), pszEvent, pfrom->GetId(),
           hash.ToString().c_str(), pfrom->deferredBlockInv.size(),
           nQueuedBlockRequests, nSentBlockRequests, nGlobalActivePressure,
           nOrphanCountPeer, nPeerActivePressure, nBudget,
           GetMaxActiveBlockRequestsPerPeer(),
           GetMaxActiveBlockRequestsGlobal(),
           MAX_DEFERRED_BLOCK_INV_PER_PEER,
           pszReason ? pszReason : "none", nAdmitted, nDropped);
}

static bool DeferBlockInv(CNode* pfrom, const uint256& hash,
                          const char* pszReason)
{
    if (pfrom == NULL)
        return false;
    if (pfrom->IsBlockAskForQueued(hash) || pfrom->setBlocksInFlight.count(hash) != 0)
        return false;
    if (pfrom->IsBlockInvDeferred(hash))
    {
        TraceDeferredWindowState(pfrom, "INV_DEFER_DUPLICATE", hash,
                                 pszReason, 0, 0, 0);
        return true;
    }
    if (!pfrom->DeferBlockInv(hash))
    {
        TraceDeferredWindowState(pfrom, "INV_DEFER_OVERFLOW", hash,
                                 pszReason, 0, 0, 1);
        return false;
    }
    TraceDeferredWindowState(pfrom, "INV_DEFER", hash, pszReason, 0, 0, 0);
    return true;
}

bool TryAdmitBlockInvOrDefer(CNode* pfrom, const CInv& inv,
                             bool fFrontierCandidate)
{
    AssertLockHeld(cs_main);
    if (pfrom == NULL || inv.type != MSG_BLOCK)
        return false;
    ibdmetrics::Get().block_inv_unknown_total.fetch_add(
        1, std::memory_order_relaxed);
    if (!IsInitialBlockDownload())
    {
        AskForBlockInvWithQualityRedirection(pfrom, inv, BLOCKREQ_SOURCE_INV,
                                             false);
        return true;
    }
    int nOrphanCountPeer = 0;
    int nQueuedBlockRequests = 0;
    int nSentBlockRequests = 0;
    int nPeerActivePressure = 0;
    int nGlobalActivePressure = 0;
    const int nBudget = GetDeferredBlockRequestBudget(
        pfrom, &nOrphanCountPeer, &nQueuedBlockRequests,
        &nSentBlockRequests, &nPeerActivePressure, &nGlobalActivePressure);
    if (nBudget <= 0)
    {
        ibdmetrics::Get().block_inv_deferred_no_budget.fetch_add(
            1, std::memory_order_relaxed);
        // Frontier admission exemption: permit exactly one block past a zero
        // budget caused by a full request window.  The candidate is the first
        // unknown block inv of a getblocks response that was requested with
        // the current active-tip locator; see FrontierCandidateCanAdmit for
        // the outstanding-count and locator-staleness bounds.  All other
        // request invariants (AlreadyHave handled by the caller, block-request
        // ownership, duplicate askfor state, orphan-limit cooldown, inflight
        // cap, normal validation) are enforced by AskFor.
        if (fFrontierCandidate &&
            nOrphanCountPeer > 0 &&
            nGlobalActivePressure < GetMaxActiveBlockRequestsGlobal() &&
            FrontierCandidateCanAdmit(GetTimeMicros(), pfrom->GetId(),
                                      inv.hash, nBestHeight,
                                      pfrom->nFrontierLocatorHeight))
        {
            if (BlockRequestTraceEnabled())
                printf("BLOCKREQTRACE time_us=%lld event=FRONTIER_GRANT hash=%s peer=%d orphan_count_peer=%d global_active=%d budget=%d\n",
                       (long long)GetTimeMicros(), inv.hash.ToString().c_str(),
                       pfrom->GetId(), nOrphanCountPeer,
                       nGlobalActivePressure, nBudget);
            pfrom->RemoveDeferredBlockInv(inv.hash);
            // Frontier admission is exempt from quality redirection so the
            // single-slot exemption keeps the announcer verbatim.
            AskForBlockInvWithQualityRedirection(pfrom, inv, BLOCKREQ_SOURCE_INV,
                                                 true);
            ibdmetrics::Get().block_inv_admitted.fetch_add(
                1, std::memory_order_relaxed);
            ibdmetrics::Get().frontier_exemption_admitted.fetch_add(
                1, std::memory_order_relaxed);
            return true;
        }
        if (fFrontierCandidate)
            ibdmetrics::Get().frontier_reject_other.fetch_add(
                1, std::memory_order_relaxed);
        if (BlockRequestTraceEnabled())
        {
            BlockRequestTraceAskSkipOrphanPressure(
                pfrom, inv.hash, BLOCKREQ_SOURCE_INV,
                nOrphanCountPeer, nQueuedBlockRequests,
                nSentBlockRequests, nPeerActivePressure,
                GetMaxActiveBlockRequestsPerPeer(),
                MAX_ORPHAN_BLOCKS_PER_PEER);
            BlockRequestTraceOrphanPressure(
                pfrom, nOrphanCountPeer, nQueuedBlockRequests,
                nSentBlockRequests, nPeerActivePressure);
            TraceDeferredWindowState(pfrom, "DEFERRED_SKIP_NO_BUDGET",
                                     inv.hash, "active-window-full",
                                     nBudget, 0, 0);
        }
        if (fFrontierCandidate)
            pfrom->nFrontierDeferredHash = inv.hash;
        if (DeferBlockInv(pfrom, inv.hash, "active-window-full"))
            ibdmetrics::Get().block_inv_deferred.fetch_add(
                1, std::memory_order_relaxed);
        else
            ibdmetrics::Get().block_inv_deferred_overflow.fetch_add(
                1, std::memory_order_relaxed);
        return false;
    }
    pfrom->RemoveDeferredBlockInv(inv.hash);
    AskForBlockInvWithQualityRedirection(pfrom, inv, BLOCKREQ_SOURCE_INV,
                                         fFrontierCandidate);
    ibdmetrics::Get().block_inv_admitted.fetch_add(1, std::memory_order_relaxed);
    return true;
}

size_t RefillDeferredBlockRequests(
    CNode* pfrom, const std::vector<CNode*>& vNodesCopy)
{
    AssertLockHeld(cs_main);
    if (pfrom == NULL || !IsInitialBlockDownload())
        return 0;
    if (pfrom->deferredBlockInv.empty())
    {
        ibdmetrics::Get().refill_called_deferred_empty.fetch_add(
            1, std::memory_order_relaxed);
        return 0;
    }

    CTxDB txdb("r");
    ibdmetrics::Get().refill_txdb_opens.fetch_add(1, std::memory_order_relaxed);
    size_t nAdmitted = 0;
    size_t nDropped = 0;
    size_t nExamined = 0;
    const size_t nInitialSize = pfrom->deferredBlockInv.size();
    int nBudget = GetDeferredBlockRequestBudget(pfrom);
    const bool fPositiveBudgetNonempty = nBudget > 0;
    ibdmetrics::Get().refill_calls.fetch_add(1, std::memory_order_relaxed);
    if (nBudget <= 0)
        ibdmetrics::Get().refill_calls_zero_budget.fetch_add(
            1, std::memory_order_relaxed);
    else
        ibdmetrics::Get().refill_called_positive_budget_nonempty.fetch_add(
            1, std::memory_order_relaxed);

    while (!pfrom->deferredBlockInv.empty() && nBudget > 0 &&
           nExamined < MAX_DEFERRED_BLOCK_INV_REFILL_WORK &&
           nExamined < nInitialSize)
    {
        const uint256 hash = pfrom->deferredBlockInv.front();
        const CInv inv(MSG_BLOCK, hash);
        ++nExamined;
        ibdmetrics::Get().refill_alreadyhave_checks.fetch_add(
            1, std::memory_order_relaxed);

        if (AlreadyHave(txdb, inv))
        {
            ibdmetrics::Get().refill_items_already_have.fetch_add(
                1, std::memory_order_relaxed);
            pfrom->PopFrontDeferredBlockInv();
            ++nDropped;
            TraceDeferredWindowState(pfrom, "DEFERRED_SKIP_KNOWN", hash,
                                     "already-have", nBudget,
                                     (int)nAdmitted, (int)nDropped);
            continue;
        }
        if (pfrom->IsBlockAskForQueued(hash) ||
            pfrom->setBlocksInFlight.count(hash) != 0)
        {
            ibdmetrics::Get().refill_items_active_owner.fetch_add(
                1, std::memory_order_relaxed);
            pfrom->PopFrontDeferredBlockInv();
            ++nDropped;
            TraceDeferredWindowState(pfrom, "DEFERRED_SKIP_ACTIVE_OWNER", hash,
                                     "same-peer-active", nBudget,
                                     (int)nAdmitted, (int)nDropped);
            continue;
        }
        NodeId nOwnerPeer = -1;
        BlockRequestOwnerState ownerState = BLOCK_REQUEST_OWNER_QUEUED;
        if (GetBlockRequestOwner(hash, &nOwnerPeer, &ownerState))
        {
            ibdmetrics::Get().refill_items_active_owner.fetch_add(
                1, std::memory_order_relaxed);
            pfrom->RotateFrontDeferredBlockInv();
            TraceDeferredWindowState(pfrom, "DEFERRED_SKIP_ACTIVE_OWNER", hash,
                                     nOwnerPeer == pfrom->GetId()
                                         ? "same-peer-owner"
                                         : "other-peer-owner",
                                     nBudget, (int)nAdmitted, (int)nDropped);
            continue;
        }

        const bool fFrontierDeferred =
            (pfrom->nFrontierDeferredHash == hash);
        pfrom->PopFrontDeferredBlockInv();
        CNode* pDispatch = pfrom;
        const bool fDiversificationEnabled =
            IsFutureSupplyDiversificationEnabled();
        if (fDiversificationEnabled)
        {
            ibdmetrics::Get().diversify_candidates.fetch_add(
                1, std::memory_order_relaxed);
            // The single deferred frontier candidate keeps the announcer path
            // verbatim (frontier-exemption semantics preserved unchanged;
            // orphan-source retries never pass through the deferred refill,
            // so the orphan lane is inherently untouched).
            if (!fFrontierDeferred)
                pDispatch =
                    ChooseDeferredDispatchLane(pfrom, hash, vNodesCopy);
        }
        // The single deferred frontier candidate keeps the announcer path
        // verbatim (fFrontierDeferred), so quality redirection is exempted for
        // it just like the admission-path frontier exemption.
        AskForBlockInvWithQualityRedirection(pDispatch, inv, BLOCKREQ_SOURCE_INV,
                                             fFrontierDeferred);
        if (fDiversificationEnabled)
        {
            if (pDispatch != pfrom)
            {
                ibdmetrics::Get().diversify_picked_other_lane.fetch_add(
                    1, std::memory_order_relaxed);
                RecordDiversifyDispatch(hash, pfrom->GetId());
            }
            else
            {
                ibdmetrics::Get().diversify_picked_announcer.fetch_add(
                    1, std::memory_order_relaxed);
            }
        }
        ++nAdmitted;
        ibdmetrics::Get().refill_items_admitted.fetch_add(
            1, std::memory_order_relaxed);
        nBudget = GetDeferredBlockRequestBudget(pfrom);
    }

    ibdmetrics::Get().refill_items_examined.fetch_add(
        nExamined, std::memory_order_relaxed);
    if (nBudget > 0 && !pfrom->deferredBlockInv.empty() &&
        nExamined >= MAX_DEFERRED_BLOCK_INV_REFILL_WORK)
        ibdmetrics::Get().refill_work_limit_hit.fetch_add(
            1, std::memory_order_relaxed);

    TraceDeferredWindowState(pfrom, "DEFERRED_REFILL", uint256(0),
                             nBudget <= 0 ? "no-budget" : "pump",
                             nBudget, (int)nAdmitted, (int)nDropped);
    if (nBudget <= 0 && !pfrom->deferredBlockInv.empty())
        TraceDeferredWindowState(pfrom, "WINDOW_STATE", pfrom->deferredBlockInv.front(),
                                 "deferred-remaining", nBudget,
                                 (int)nAdmitted, (int)nDropped);
    if (fPositiveBudgetNonempty)
    {
        if (nAdmitted == 0)
            ibdmetrics::Get().refill_positive_budget_admitted_zero.fetch_add(
                1, std::memory_order_relaxed);
        else
            ibdmetrics::Get().refill_positive_budget_admitted_count.fetch_add(
                nAdmitted, std::memory_order_relaxed);
    }
    return nAdmitted;
}

static bool fProcessBlockRejectTraceEnabled = false;
static CCriticalSection cs_processBlockRejectTrace;
static std::map<uint256, std::string> mapProcessBlockRejectTraceLastReason;

bool InitProcessBlockRejectTrace(bool fEnabled)
{
    fProcessBlockRejectTraceEnabled = fEnabled;
    if (fProcessBlockRejectTraceEnabled)
        printf("PBREJECT: process block reject trace enabled\n");
    return true;
}

bool ProcessBlockRejectTraceEnabled()
{
    return fProcessBlockRejectTraceEnabled;
}

const char* ProcessBlockRejectReasonName(ProcessBlockRejectReason reason)
{
    switch (reason) {
    case PBREJECT_DUPLICATE_INDEXED: return "DUPLICATE_INDEXED";
    case PBREJECT_DUPLICATE_ORPHAN: return "DUPLICATE_ORPHAN";
    case PBREJECT_DUPLICATE_INDEXED_STAKE: return "DUPLICATE_INDEXED_STAKE";
    case PBREJECT_POS_AFTER_DAG: return "POS_AFTER_DAG";
    case PBREJECT_CHECKBLOCK_FALSE: return "CHECKBLOCK_FALSE";
    case PBREJECT_WEAK_CHECKPOINT: return "WEAK_CHECKPOINT";
    case PBREJECT_ORPHAN_LIMIT_IBD: return "ORPHAN_LIMIT_IBD";
    case PBREJECT_ORPHAN_LIMIT_NORMAL: return "ORPHAN_LIMIT_NORMAL";
    case PBREJECT_DUPLICATE_STAKE_ORPHAN: return "DUPLICATE_STAKE_ORPHAN";
    case PBREJECT_OPERATOR_INVALIDATED: return "OPERATOR_INVALIDATED";
    case PBREJECT_ACCEPTBLOCK_FALSE: return "ACCEPTBLOCK_FALSE";
    case PBREJECT_UNKNOWN_FALSE: return "UNKNOWN_FALSE";
    }
    return "UNKNOWN_FALSE";
}

std::string ProcessBlockRejectTraceLastReason(const uint256& hash)
{
    LOCK(cs_processBlockRejectTrace);
    std::map<uint256, std::string>::const_iterator it =
        mapProcessBlockRejectTraceLastReason.find(hash);
    return it == mapProcessBlockRejectTraceLastReason.end()
        ? std::string("none") : it->second;
}

static void ProcessBlockRejectTraceRemember(const uint256& hash,
                                            const char* pszReason)
{
    if (!fProcessBlockRejectTraceEnabled)
        return;
    LOCK(cs_processBlockRejectTrace);
    mapProcessBlockRejectTraceLastReason[hash] = pszReason;
    while (mapProcessBlockRejectTraceLastReason.size() > 4096)
    {
        std::map<uint256, std::string>::iterator it =
            mapProcessBlockRejectTraceLastReason.begin();
        if (it->first == hash)
            ++it;
        if (it == mapProcessBlockRejectTraceLastReason.end())
            break;
        mapProcessBlockRejectTraceLastReason.erase(it);
    }
}

static bool fAcceptBlockRejectTraceEnabled = false;

bool InitAcceptBlockRejectTrace(bool fEnabled)
{
    fAcceptBlockRejectTraceEnabled = fEnabled;
    if (fAcceptBlockRejectTraceEnabled)
        printf("ACCEPTBLOCK_REJECT: accept block reject trace enabled\n");
    return true;
}

bool AcceptBlockRejectTraceEnabled()
{
    return fAcceptBlockRejectTraceEnabled;
}

const char* AcceptBlockRejectReasonName(AcceptBlockRejectReason reason)
{
    switch (reason) {
    case ABREJECT_UNKNOWN_BLOCK_VERSION: return "unknown-block-version";
    case ABREJECT_DUPLICATE: return "duplicate";
    case ABREJECT_PREV_NOT_FOUND: return "prev-not-found";
    case ABREJECT_OPERATOR_INVALIDATED: return "operator-invalidated";
    case ABREJECT_PREV_OPERATOR_INVALIDATED: return "prev-operator-invalidated";
    case ABREJECT_POS_AFTER_DAG: return "pos-after-dag";
    case ABREJECT_BLOCK_SIZE: return "block-size";
    case ABREJECT_INCORRECT_BITS: return "incorrect-bits";
    case ABREJECT_TIMESTAMP_TOO_EARLY: return "timestamp-too-early";
    case ABREJECT_NON_FINAL_TX: return "non-final-tx";
    case ABREJECT_HARDENED_CHECKPOINT: return "hardened-checkpoint";
    case ABREJECT_CHECK_POS_FAILED: return "check-pos-failed";
    case ABREJECT_RINGSIG_DEPRECATION: return "ringsig-deprecation";
    case ABREJECT_SYNC_CHECKPOINT: return "sync-checkpoint";
    case ABREJECT_COINBASE_HEIGHT: return "coinbase-height";
    case ABREJECT_DAG_PARENT: return "dag-parent";
    case ABREJECT_DISK_SPACE: return "disk-space";
    case ABREJECT_WRITE_TO_DISK: return "write-to-disk";
    case ABREJECT_ADD_TO_BLOCK_INDEX: return "add-to-block-index";
    }
    return "unknown";
}

const char* AcceptBlockRejectStageName(AcceptBlockRejectReason reason)
{
    switch (reason) {
    case ABREJECT_UNKNOWN_BLOCK_VERSION: return "version";
    case ABREJECT_DUPLICATE: return "duplicate";
    case ABREJECT_PREV_NOT_FOUND: return "prev";
    case ABREJECT_OPERATOR_INVALIDATED:
    case ABREJECT_PREV_OPERATOR_INVALIDATED: return "operator";
    case ABREJECT_POS_AFTER_DAG:
    case ABREJECT_BLOCK_SIZE:
    case ABREJECT_INCORRECT_BITS: return "consensus";
    case ABREJECT_TIMESTAMP_TOO_EARLY: return "timestamp";
    case ABREJECT_NON_FINAL_TX: return "txfinal";
    case ABREJECT_HARDENED_CHECKPOINT:
    case ABREJECT_SYNC_CHECKPOINT: return "checkpoint";
    case ABREJECT_CHECK_POS_FAILED: return "pos";
    case ABREJECT_RINGSIG_DEPRECATION: return "ringsig";
    case ABREJECT_COINBASE_HEIGHT: return "coinbase";
    case ABREJECT_DAG_PARENT: return "dag";
    case ABREJECT_DISK_SPACE:
    case ABREJECT_WRITE_TO_DISK: return "disk";
    case ABREJECT_ADD_TO_BLOCK_INDEX: return "index";
    }
    return "unknown";
}

static void TraceAcceptBlockReject(const CBlock& block, int nHeight,
                                   AcceptBlockRejectReason reason)
{
    if (!fAcceptBlockRejectTraceEnabled)
        return;
    printf("ACCEPTBLOCK_REJECT hash=%s height=%d prev=%s reason=%s stage=%s\n",
           block.GetHash().ToString().c_str(),
           nHeight,
           block.hashPrevBlock.ToString().c_str(),
           AcceptBlockRejectReasonName(reason),
           AcceptBlockRejectStageName(reason));
}

static bool FindStakeKeyInIndex(const std::pair<COutPoint, unsigned int>& stake,
                                uint256& hashOut)
{
    for (std::map<uint256, CBlockIndex*>::const_iterator it = mapBlockIndex.begin();
         it != mapBlockIndex.end(); ++it) {
        CBlockIndex* pindex = it->second;
        if (pindex && pindex->IsProofOfStake() &&
            pindex->prevoutStake == stake.first &&
            pindex->nStakeTime == stake.second) {
            hashOut = it->first;
            return true;
        }
    }
    return false;
}

static bool FindStakeKeyInOrphans(const std::pair<COutPoint, unsigned int>& stake,
                                  uint256& hashOut)
{
    for (std::map<uint256, CBlock*>::const_iterator it = mapOrphanBlocks.begin();
         it != mapOrphanBlocks.end(); ++it) {
        if (it->second && it->second->IsProofOfStake() &&
            it->second->GetProofOfStake() == stake) {
            hashOut = it->first;
            return true;
        }
    }
    return false;
}

// Central exactly-once accounting for terminal ProcessBlock reject outcomes.
// Every terminal reject site calls TraceProcessBlockReject() exactly once
// and returns immediately afterwards, so classifying here -- rather than at
// each reject site -- is exactly-once with no double counting.  Orphan-limit
// rejects (missing parent, no validation failure) are attributed to their own
// dedicated counter and are deliberately excluded from the general
// validation-reject total.
static void RecordBlockResultReject(ProcessBlockRejectReason reason)
{
    ibdmetrics::Counters& c = ibdmetrics::Get();
    switch (reason)
    {
    case PBREJECT_ORPHAN_LIMIT_IBD:
    case PBREJECT_ORPHAN_LIMIT_NORMAL:
        c.block_result_orphan_limit_rejected.fetch_add(
            1, std::memory_order_relaxed);
        break;
    case PBREJECT_DUPLICATE_INDEXED:
    case PBREJECT_DUPLICATE_ORPHAN:
        // Already-have/duplicate deliveries are not validation rejects.
        break;
    default:
        c.block_result_rejected_total.fetch_add(
            1, std::memory_order_relaxed);
        break;
    }
}

static void TraceProcessBlockReject(CNode* pfrom, CBlock* pblock,
                                    ProcessBlockRejectReason reason,
                                    const char* pszCheckBlockReason = NULL,
                                    const std::string& extra = std::string())
{
    RecordBlockResultReject(reason);
    if (!fProcessBlockRejectTraceEnabled || !pblock)
        return;
    const uint256 hash = pblock->GetHash();
    const bool fIsPos = pblock->IsProofOfStake();
    const std::pair<COutPoint, unsigned int> stake = fIsPos ?
        pblock->GetProofOfStake() : std::make_pair(COutPoint(), 0U);
    const bool fStakeSeenIndex = fIsPos && setStakeSeen.count(stake) != 0;
    const bool fStakeSeenOrphan = fIsPos && setStakeSeenOrphan.count(stake) != 0;
    int nOrphanCountPeer = -1;
    if (pfrom) {
        std::map<NodeId, int>::const_iterator it = mapOrphanCountByNode.find(pfrom->GetId());
        nOrphanCountPeer = it == mapOrphanCountByNode.end() ? 0 : it->second;
    }
    uint256 hashExisting;
    const char* pszExistingState = "unknown";
    if (fIsPos && FindStakeKeyInIndex(stake, hashExisting))
        pszExistingState = "indexed";
    else if (fIsPos && FindStakeKeyInOrphans(stake, hashExisting))
        pszExistingState = "orphan";
    const char* pszReason = ProcessBlockRejectReasonName(reason);
    ProcessBlockRejectTraceRemember(hash, pszReason);
    printf("PBREJECT time_us=%lld hash=%s prev=%s peer=%d reason=%s checkblock_reason=%s is_pos=%d dos=%d prev_in_index=%d prev_in_orphans=%d block_in_index=%d block_in_orphans=%d orphan_count_peer=%d stake_seen_index=%d stake_seen_orphan=%d sync_checkpoint=%d local_height=%d proof_of_stake_hash=%s stake_time=%u mapOrphanBlocksByPrev_count=%zu hashPrevBlock=%s existing_block_hash=%s existing_block_state=%s%s%s\n",
           (long long)GetTimeMicros(), hash.ToString().c_str(), pblock->hashPrevBlock.ToString().c_str(), pfrom ? pfrom->GetId() : -1, pszReason, pszCheckBlockReason ? pszCheckBlockReason : "none", fIsPos ? 1 : 0, pblock->nDoS, mapBlockIndex.count(pblock->hashPrevBlock) ? 1 : 0, mapOrphanBlocks.count(pblock->hashPrevBlock) ? 1 : 0, mapBlockIndex.count(hash) ? 1 : 0, mapOrphanBlocks.count(hash) ? 1 : 0, nOrphanCountPeer, fStakeSeenIndex ? 1 : 0, fStakeSeenOrphan ? 1 : 0, Checkpoints::GetLastSyncCheckpoint() ? 1 : 0, nBestHeight, fIsPos ? stake.first.ToString().c_str() : uint256(0).ToString().c_str(), fIsPos ? stake.second : 0, mapOrphanBlocksByPrev.count(hash), pblock->hashPrevBlock.ToString().c_str(), hashExisting.ToString().c_str(), pszExistingState, extra.empty() ? "" : " ", extra.c_str());
}


map<uint256, CTransaction> mapOrphanTransactions;
map<uint256, set<uint256> > mapOrphanTransactionsByPrev;

// Constant stuff for coinbase transactions we create:
CScript COINBASE_FLAGS;

const string strMessageMagic = "Innova Signed Message:\n";

// Settings
int64_t nTransactionFee = MIN_TX_FEE;
int64_t nReserveBalance = 0;
int64_t nMinimumInputValue = 0;

unsigned int nCoinCacheSize = 5000;

extern enum Checkpoints::CPMode CheckpointsMode;

std::set<uint256> setValidatedTx;

CHooks* hooks; // This adds Innova Name DB hooks which allow splicing of code inside standard Innova functions.

//////////////////////////////////////////////////////////////////////////////
//
// dispatching functions
//

// These functions dispatch to one or all registered wallets

namespace {

// The block-request hashes of a pending getdata message, in wire order.
// Used only to feed the passive ibdforensic recorder; the non-block invs
// (e.g. transactions) that share the getdata message are filtered out here.
std::vector<uint256> BlockHashesOfGetData(
    const std::vector<CInv>& vGetData)
{
    std::vector<uint256> vHashes;
    for (size_t i = 0; i < vGetData.size(); ++i)
    {
        if (vGetData[i].type == MSG_BLOCK ||
            vGetData[i].type == MSG_FILTERED_BLOCK)
            vHashes.push_back(vGetData[i].hash);
    }
    return vHashes;
}

struct CMainSignals {
    // Notifies listeners of updated transaction data (passing hash, transaction, and optionally the block it is found in.
    boost::signals2::signal<void (const CTransaction &, const CBlock *, bool)> SyncTransaction;
    // Notifies listeners of an erased transaction (currently disabled, requires transaction replacement).
    boost::signals2::signal<void (const uint256 &)> EraseTransaction;
    // Notifies listeners of an updated transaction without new data (for now: a coinbase potentially becoming visible).
    boost::signals2::signal<void (const uint256 &)> UpdatedTransaction;
    // Notifies listeners of a new active block chain.
    boost::signals2::signal<void (const CBlockLocator &)> SetBestChain;
    // Notifies listeners about an inventory item being seen on the network.
    boost::signals2::signal<void (const uint256 &)> Inventory;
    // Tells listeners to broadcast their data.
    boost::signals2::signal<void (bool)> Broadcast;

} g_signals;
}

void RegisterWallet(CWallet* pwalletIn) {
    g_signals.EraseTransaction.connect(boost::bind(&CWallet::EraseFromWallet, pwalletIn, _1));
    g_signals.UpdatedTransaction.connect(boost::bind(&CWallet::UpdatedTransaction, pwalletIn, _1));
    g_signals.SetBestChain.connect(boost::bind(&CWallet::SetBestChain, pwalletIn, _1));
    g_signals.Inventory.connect(boost::bind(&CWallet::Inventory, pwalletIn, _1));
    g_signals.Broadcast.connect(boost::bind(&CWallet::ResendWalletTransactions, pwalletIn, _1));
    {
            LOCK(cs_setpwalletRegistered);
            setpwalletRegistered.insert(pwalletIn);
    }
}

void UnregisterWallet(CWallet* pwalletIn) {
    g_signals.Broadcast.disconnect(boost::bind(&CWallet::ResendWalletTransactions, pwalletIn, _1));
    g_signals.Inventory.disconnect(boost::bind(&CWallet::Inventory, pwalletIn, _1));
    g_signals.SetBestChain.disconnect(boost::bind(&CWallet::SetBestChain, pwalletIn, _1));
    g_signals.UpdatedTransaction.disconnect(boost::bind(&CWallet::UpdatedTransaction, pwalletIn, _1));
    g_signals.EraseTransaction.disconnect(boost::bind(&CWallet::EraseFromWallet, pwalletIn, _1));
    {
            LOCK(cs_setpwalletRegistered);
            setpwalletRegistered.erase(pwalletIn);
    }
}


// check whether the passed transaction is from us
bool static IsFromMe(CTransaction& tx)
{
    for (CWallet* pwallet : setpwalletRegistered)
        if (pwallet->IsFromMe(tx))
            return true;
    return false;
}


// get the wallet transaction with the given hash (if it exists)
bool static GetTransaction(const uint256& hashTx, CWalletTx& wtx)
{
    for (CWallet* pwallet : setpwalletRegistered)
        if (pwallet->GetTransaction(hashTx,wtx))
            return true;
    return false;
}

// erases transaction with the given hash from all wallets
void static EraseFromWallets(uint256 hash)
{
    for (CWallet* pwallet : setpwalletRegistered)
        pwallet->EraseFromWallet(hash);
}

// make sure all wallets know about the given transaction, in the given block
void SyncWithWallets(const CTransaction& tx, const CBlock* pblock, bool fUpdate, bool fConnect)
{
    if (!fConnect)
    {
        // ppcoin: wallets need to refund inputs when disconnecting coinstake
        if (tx.IsCoinStake())
        {
            for (CWallet* pwallet : setpwalletRegistered)
            {
                if (pwallet->IsFromMe(tx))
                    pwallet->DisableTransaction(tx);
            };
        };

        if (tx.nVersion == ANON_TXN_VERSION)
        {
            for (CWallet* pwallet : setpwalletRegistered)
                pwallet->UndoAnonTransaction(tx);
        };
        return;
    };

    //uint256 hash = tx.GetHash();
    for (CWallet* pwallet : setpwalletRegistered)
        pwallet->AddToWalletIfInvolvingMe(tx, pblock, fUpdate);
}

// notify wallets about a new best chain
void static SetBestChain(const CBlockLocator& loc)
{
    for (CWallet* pwallet : setpwalletRegistered)
        pwallet->SetBestChain(loc);
}

// notify wallets about an updated transaction
void static UpdatedTransaction(const uint256& hashTx)
{
    for (CWallet* pwallet : setpwalletRegistered)
        pwallet->UpdatedTransaction(hashTx);
}
/*
// dump all wallets
void static PrintWallets(const CBlock& block)
{
    BOOST_FOREACH(CWallet* pwallet, setpwalletRegistered)
        pwallet->PrintWallet(block);
} */

// notify wallets about an incoming inventory (for request counts)
void static Inventory(const uint256& hash)
{
    for (CWallet* pwallet : setpwalletRegistered)
        pwallet->Inventory(hash);
}

// ask wallets to resend their transactions
void ResendWalletTransactions(bool fForce)
{
    for (CWallet* pwallet : setpwalletRegistered)
        pwallet->ResendWalletTransactions(fForce);
}

bool Finalise()
{
    printf("Finalise()");

    LOCK(cs_main);

    //nTransactionsUpdated++;
    mempool.AddTransactionsUpdated(1);
    bitdb.Flush(false);
    StopNode();
    bitdb.Flush(true);
    fs::remove(GetPidFile());
    UnregisterWallet(pwalletMain);
    delete pwalletMain;

    finaliseRingSigs();

    CTxDB().Close();


    return true;
}

bool AbortNode(const std::string &strMessage, const std::string &userMessage) {
    strMiscWarning = strMessage;
    printf("*** %s\n", strMessage.c_str());
	/*
    uiInterface.ThreadSafeMessageBox(
        userMessage.empty() ? _("Error: A fatal internal error occured, see debug.log for details") : userMessage,
        "", CClientUIInterface::MSG_ERROR);
		*/
    StartShutdown();
    return false;
}

bool GetNodeStateStats(NodeId nodeid, CNodeStateStats &stats)
{
    // TODO:
    return false;
}

//////////////////////////////////////////////////////////////////////////////
//
// mapOrphanTransactions
//

bool AddOrphanTx(const CTransaction& tx)
{
    uint256 hash = tx.GetHash();
    if (mapOrphanTransactions.count(hash))
        return false;

    // Ignore big transactions, to avoid a
    // send-big-orphans memory exhaustion attack. If a peer has a legitimate
    // large transaction with a missing parent then we assume
    // it will rebroadcast it later, after the parent transaction(s)
    // have been mined or received.
    // 10,000 orphans, each of which is at most 5,000 bytes big is
    // at most 500 megabytes of orphans:

    size_t nSize = tx.GetSerializeSize(SER_NETWORK, CTransaction::CURRENT_VERSION);

    if (nSize > 5000)
    {
        printf("ignoring large orphan tx (size: %" PRIszu", hash: %s)\n", nSize, hash.ToString().substr(0,10).c_str());
        return false;
    };

    mapOrphanTransactions[hash] = tx;
    for (const CTxIn& txin : tx.vin)
        mapOrphanTransactionsByPrev[txin.prevout.hash].insert(hash);

    printf("stored orphan tx %s (mapsz %" PRIszu")\n", hash.ToString().substr(0,10).c_str(),
        mapOrphanTransactions.size());
    return true;
}

void static EraseOrphanTx(uint256 hash)
{
    if (!mapOrphanTransactions.count(hash))
        return;
    const CTransaction& tx = mapOrphanTransactions[hash];
    for (const CTxIn& txin : tx.vin)
    {
        mapOrphanTransactionsByPrev[txin.prevout.hash].erase(hash);
        if (mapOrphanTransactionsByPrev[txin.prevout.hash].empty())
            mapOrphanTransactionsByPrev.erase(txin.prevout.hash);
    }
    mapOrphanTransactions.erase(hash);
}

unsigned int LimitOrphanTxSize(unsigned int nMaxOrphans)
{
    unsigned int nEvicted = 0;
    while (mapOrphanTransactions.size() > nMaxOrphans)
    {
        // Evict a random orphan:
        uint256 randomhash = GetRandHash();
        map<uint256, CTransaction>::iterator it = mapOrphanTransactions.lower_bound(randomhash);
        if (it == mapOrphanTransactions.end())
            it = mapOrphanTransactions.begin();
        EraseOrphanTx(it->first);
        ++nEvicted;
    }
    return nEvicted;
}







//////////////////////////////////////////////////////////////////////////////
//
// CTransaction and CTxIndex
//

// CMutableTransaction::CMutableTransaction() : nVersion(CTransaction::CURRENT_VERSION), nTime(GetAdjustedTime()), nLockTime(0) {}
// CMutableTransaction::CMutableTransaction(const CTransaction& tx) : nVersion(tx.nVersion), nTime(tx.nTime), vin(tx.vin), vout(tx.vout), nLockTime(tx.nLockTime) {}

// uint256 CMutableTransaction::GetHash() const
// {
//     return SerializeHash(*this);
// }

// void CTransaction::UpdateHash() const
// {
//     *const_cast<uint256*>(&hash) = SerializeHash(*this);
// }

// CTransaction::CTransaction() : hash(0), nVersion(CTransaction::CURRENT_VERSION), nTime(GetAdjustedTime()), vin(), vout(), nLockTime(0) { }

// CTransaction::CTransaction(const CMutableTransaction &tx) : nVersion(tx.nVersion), nTime(tx.nTime), vin(tx.vin), vout(tx.vout), nLockTime(tx.nLockTime) {
//     UpdateHash();
// }



bool CTransaction::ReadFromDisk(CTxDB& txdb, COutPoint prevout, CTxIndex& txindexRet)
{
    SetNull();
    if (!txdb.ReadTxIndex(prevout.hash, txindexRet))
        return false;
    if (!ReadFromDisk(txindexRet.pos))
        return false;
    if (prevout.n >= vout.size())
    {
        SetNull();
        return false;
    }
    return true;
}

bool CTransaction::ReadFromDisk(CTxDB& txdb, COutPoint prevout)
{
    CTxIndex txindex;
    return ReadFromDisk(txdb, prevout, txindex);
}

bool CTransaction::ReadFromDisk(COutPoint prevout)
{
    CTxDB txdb("r");
    CTxIndex txindex;
    return ReadFromDisk(txdb, prevout, txindex);
}

// bool CTransaction::IsStandard() const
// {
//     if (nVersion > CTransaction::CURRENT_VERSION)
//         return false;

//     BOOST_FOREACH(const CTxIn& txin, vin)
//     {
//         // Biggest 'standard' txin is a 3-signature 3-of-3 CHECKMULTISIG
//         // pay-to-script-hash, which is 3 ~80-byte signatures, 3
//         // ~65-byte public keys, plus a few script ops.
//         if (txin.scriptSig.size() > 500)
//             return false;
//         if (!txin.scriptSig.IsPushOnly())
//             return false;
//         if (fEnforceCanonical && !txin.scriptSig.HasCanonicalPushes()) {
//             return false;
//         }
//     }

//     unsigned int nDataOut = 0;
//     unsigned int nTxnOut = 0;

//     txnouttype whichType;
//     BOOST_FOREACH(const CTxOut& txout, vout) {
//         if (!::IsStandard(txout.scriptPubKey, whichType))
//             return false;
//         if (whichType == TX_NULL_DATA)
//         {
//             nDataOut++;
//         } else
//         {
//             if (txout.nValue == 0)
//                 return false;
//             nTxnOut++;
//         }
//         if (fEnforceCanonical && !txout.scriptPubKey.HasCanonicalPushes()) {
//             return false;
//         }
//     }

//     // only one OP_RETURN txout per txn out is permitted
//     if (nDataOut > nTxnOut) {
//         return false;
//     }

//     return true;
// }

bool IsStandardTx(const CTransaction& tx, string& reason)
{
    if (tx.nVersion > CTransaction::CURRENT_VERSION && tx.nVersion != ANON_TXN_VERSION && tx.nVersion != NAMECOIN_TX_VERSION && !tx.IsShielded()) { //WIP
        reason = "version";
        return false;
    }

    // Treat non-final transactions as non-standard to prevent a specific type
    // of double-spend attack, as well as DoS attacks. (if the transaction
    // can't be mined, the attacker isn't expending resources broadcasting it)
    // Basically we don't want to propagate transactions that can't be included in
    // the next block.
    //
    // However, IsFinalTx() is confusing... Without arguments, it uses
    // chainActive.Height() to evaluate nLockTime; when a block is accepted, chainActive.Height()
    // is set to the value of nHeight in the block. However, when IsFinalTx()
    // is called within CBlock::AcceptBlock(), the height of the block *being*
    // evaluated is what is used. Thus if we want to know if a transaction can
    // be part of the *next* block, we need to call IsFinalTx() with one more
    // than chainActive.Height().
    //
    // Timestamps on the other hand don't get any special treatment, because we
    // can't know what timestamp the next block will have, and there aren't
    // timestamp applications where it matters.
    //if (!IsFinalTx(tx, nBestHeight + 1)) {
	  if (!tx.IsFinal(nBestHeight + 1)) {
        reason = "non-final";
        return false;
    }
    // nTime has different purpose from nLockTime but can be used in similar attacks
    if (tx.nTime > FutureDrift(GetAdjustedTime())) {
        reason = "time-too-new";
        return false;
    }

    // Extremely large transactions with lots of inputs can cost the network
    // almost as much to process as they cost the sender in fees, because
    // computing signature hashes is O(ninputs*txsize). Limiting transactions
    // to MAX_STANDARD_TX_SIZE mitigates CPU exhaustion attacks.
    unsigned int sz = tx.GetSerializeSize(SER_NETWORK, CTransaction::CURRENT_VERSION);
    if (sz >= MAX_STANDARD_TX_SIZE) {
        reason = "tx-size";
        return false;
    }

    for (const CTxIn& txin : tx.vin)
    {
        if (txin.IsAnonInput())
        {
            int nRingSize = txin.ExtractRingSize();

            if (tx.nVersion != ANON_TXN_VERSION
                || nRingSize < (int)MIN_RING_SIZE
                || nRingSize > (int)MAX_RING_SIZE
                || txin.scriptSig.size() > sizeof(COutPoint) + 2 + (33 + 32 + 32) * nRingSize)
            {
                printf("IsStandard() anon txin failed.\n");
                return false;
            };
            continue;
        };
        // Biggest 'standard' txin is a 15-of-15 P2SH multisig with compressed
        // keys. (remember the 520 byte limit on redeemScript size) That works
        // out to a (15*(33+1))+3=513 byte redeemScript, 513+1+15*(73+1)+3=1627
        // bytes of scriptSig, which we round off to 1650 bytes for some minor
        // future-proofing. That's also enough to spend a 20-of-20
        // CHECKMULTISIG scriptPubKey, though such a scriptPubKey is not
        // considered standard)
        if (txin.scriptSig.size() > 1650) {
            reason = "scriptsig-size";
            return false;
        }
        if (!txin.scriptSig.IsPushOnly()) {
            reason = "scriptsig-not-pushonly";
            return false;
        }
        if (!txin.scriptSig.HasCanonicalPushes()) {
            reason = "scriptsig-non-canonical-push";
            return false;
        }
    }

    unsigned int nDataOut = 0;
    unsigned int nTxnOut = 0;

    txnouttype whichType;
    for (const CTxOut& txout : tx.vout) {
        if (txout.IsAnonOutput())
        {
            if (tx.nVersion != ANON_TXN_VERSION
                || txout.nValue < 1
                || txout.scriptPubKey.size() > MIN_ANON_OUT_SIZE + MAX_ANON_NARRATION_SIZE)
            {
                printf("IsStandard() anon txout failed.\n");
                return false;
            }
            //nTxnOut++; anon outputs don't count (narrations are embedded in scriptPubKey)
            continue;
        };

         if (!::IsStandard(txout.scriptPubKey, whichType)) {
             reason = "scriptpubkey";
             return false;
         }
         if (whichType == TX_NULL_DATA)
         {
             nDataOut++;
         } else
         {
             if (txout.nValue == 0)
                 return false;
             nTxnOut++;
         }
         if (fEnforceCanonical && !txout.scriptPubKey.HasCanonicalPushes()) {
             reason = "scriptpubkey-non-canonical-push";
             return false;
         }
    }

    // only one OP_RETURN txout per txn out is permitted
    if (nDataOut > nTxnOut) {
        reason = "multi-op-return";
        return false;
    }

    return true;
}

bool IsFinalTx(const CTransaction &tx, int nBlockHeight, int64_t nBlockTime)
{
    AssertLockHeld(cs_main);
    // Time based nLockTime implemented in 0.1.6
    if (tx.nLockTime == 0)
        return true;
    if (nBlockHeight == 0)
        nBlockHeight = nBestHeight;
    if (nBlockTime == 0)
        nBlockTime = GetAdjustedTime();
    if ((int64_t)tx.nLockTime < ((int64_t)tx.nLockTime < LOCKTIME_THRESHOLD ? (int64_t)nBlockHeight : nBlockTime))
        return true;
    for (const CTxIn& txin : tx.vin)
        if (!txin.IsFinal())
            return false;
    return true;
}

//
// Check transaction inputs, and make sure any
// pay-to-script-hash transactions are evaluating IsStandard scripts
//
// Why bother? To avoid denial-of-service attacks; an attacker
// can submit a standard HASH... OP_EQUAL transaction,
// which will get accepted into blocks. The redemption
// script can be anything; an attacker could use a very
// expensive-to-check-upon-redemption script like:
//   DUP CHECKSIG DROP ... repeated 100 times... OP_1
//
bool AreInputsStandard(const CTransaction& tx, const MapPrevTx& mapInputs)
{
    if (tx.IsCoinBase())
        return true; // Coinbases don't use vin normally

    for (unsigned int i = 0; i < tx.vin.size(); i++)
    {
        if (tx.nVersion == ANON_TXN_VERSION
            && tx.vin[i].IsAnonInput())
            continue;

        const CTxOut& prev = tx.GetOutputFor(tx.vin[i], mapInputs);

        vector<vector<unsigned char> > vSolutions;
        txnouttype whichType;
        // get the scriptPubKey corresponding to this input:
        const CScript& prevScript = prev.scriptPubKey;
        if (!Solver(prevScript, whichType, vSolutions))
            return false;
        int nArgsExpected = ScriptSigArgsExpected(whichType, vSolutions);
        if (nArgsExpected < 0)
            return false;

        // Transactions with extra stuff in their scriptSigs are
        // non-standard. Note that this EvalScript() call will
        // be quick, because if there are any operations
        // beside "push data" in the scriptSig
        // IsStandard() will have already returned false
        // and this method isn't called.
        vector<vector<unsigned char> > stack;
        if (!EvalScript(stack, tx.vin[i].scriptSig, tx, i, SCRIPT_VERIFY_NONE, 0))
            return false;

        if (whichType == TX_SCRIPTHASH)
        {
            if (stack.empty())
                return false;
            CScript subscript(stack.back().begin(), stack.back().end());
            vector<vector<unsigned char> > vSolutions2;
            txnouttype whichType2;
            if (Solver(subscript, whichType2, vSolutions2))
            {
                int tmpExpected = ScriptSigArgsExpected(whichType2, vSolutions2);
                if (tmpExpected < 0)
                    return false;
                nArgsExpected += tmpExpected;
            }
            else
            {
                // Any other Script with less than 15 sigops OK:
                unsigned int sigops = subscript.GetSigOpCount(true);
                // ... extra data left on the stack after execution is OK, too:
                return (sigops <= MAX_P2SH_SIGOPS);
            }
        }

        if (stack.size() != (unsigned int)nArgsExpected)
            return false;
    }

    return true;
}

bool CTransaction::HasStealthOutput() const
{
    // -- todo: scan without using GetOp

    std::vector<uint8_t> vchEphemPK;
    opcodetype opCode;

    for (vector<CTxOut>::const_iterator it = vout.begin(); it != vout.end(); ++it)
    {
        if (nVersion == ANON_TXN_VERSION
            && it->IsAnonOutput())
            continue;

        CScript::const_iterator itScript = it->scriptPubKey.begin();

        if (!it->scriptPubKey.GetOp(itScript, opCode, vchEphemPK)
            || opCode != OP_RETURN
            || !it->scriptPubKey.GetOp(itScript, opCode, vchEphemPK) // rule out np narrations
            || vchEphemPK.size() != ec_compressed_size)
            continue;

        return true;
    };

    return false;
};

unsigned int CTransaction::GetLegacySigOpCount() const
{
    unsigned int nSigOps = 0;
    for (const CTxIn& txin : vin)
    {
        nSigOps += txin.scriptSig.GetSigOpCount(false);
    };
    for (const CTxOut& txout : vout)
    {
        nSigOps += txout.scriptPubKey.GetSigOpCount(false);
    };
    return nSigOps;
}


int CMerkleTx::SetMerkleBranch(const CBlock* pblock)
{
    AssertLockHeld(cs_main);

    CBlock blockTmp;
    if (pblock == NULL)
    {
        // Load the block this tx is in
        CTxIndex txindex;
        if (!CTxDB("r").ReadTxIndex(GetHash(), txindex))
            return 0;
        if (!blockTmp.ReadFromDisk(txindex.pos.nFile, txindex.pos.nBlockPos))
            return 0;
        pblock = &blockTmp;
    }

    // Update the tx's hashBlock
    hashBlock = pblock->GetHash();

    // Locate the transaction
    for (nIndex = 0; nIndex < (int)pblock->vtx.size(); nIndex++)
        if (pblock->vtx[nIndex] == *(CTransaction*)this)
            break;
    if (nIndex == (int)pblock->vtx.size())
    {
        vMerkleBranch.clear();
        nIndex = -1;
        printf("ERROR: SetMerkleBranch() : couldn't find tx in block\n");
        return 0;
    }

    // Fill in merkle branch
    vMerkleBranch = pblock->GetMerkleBranch(nIndex);

    // Is the tx in a block that's in the main chain
    map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(hashBlock);
    if (mi == mapBlockIndex.end())
        return 0;
    CBlockIndex* pindex = (*mi).second;
    if (!pindex || !pindex->IsInMainChain())
        return 0;

    if (!pindexBest)
        return 0;

    return pindexBest->nHeight - pindex->nHeight + 1;
}







// ---------------------------------------------------------------------------
// Adaptive Block Size (Monero-inspired, tuned for 1s DAG blocks)
// ---------------------------------------------------------------------------

unsigned int GetAdaptiveBlockSizeLimit(const CBlockIndex* pindex)
{
    if (!pindex)
        return MAX_BLOCK_SIZE_LEGACY;

    // Pre-DAG: fixed 1 MB
    if (pindex->nHeight < FORK_HEIGHT_DAG)
        return MAX_BLOCK_SIZE_LEGACY;

    // Walk back ADAPTIVE_MEDIAN_WINDOW blocks and collect sizes
    std::vector<unsigned int> vSizes;
    vSizes.reserve(ADAPTIVE_MEDIAN_WINDOW);
    const CBlockIndex* pWalk = pindex;

    for (unsigned int i = 0; i < ADAPTIVE_MEDIAN_WINDOW && pWalk; i++)
    {
        vSizes.push_back(pWalk->nSize > 0 ? pWalk->nSize : 1);
        pWalk = pWalk->pprev;
    }

    if (vSizes.empty())
        return ADAPTIVE_BLOCK_FLOOR;

    // Short-term median
    std::sort(vSizes.begin(), vSizes.end());
    unsigned int nShortMedian = vSizes[vSizes.size() / 2];

    // Apply floor: penalty-free zone
    if (nShortMedian < ADAPTIVE_BLOCK_FLOOR)
        nShortMedian = ADAPTIVE_BLOCK_FLOOR;

    // Long-term median anchor (independent window, starts after short-term window)
    std::vector<unsigned int> vLongSizes;
    // pWalk is already at the end of the short-term window — continue from there
    unsigned int nLongSamples = std::min(ADAPTIVE_LONG_MEDIAN_WINDOW, (unsigned int)50000);
    for (unsigned int i = 0; i < nLongSamples && pWalk; i++)
    {
        vLongSizes.push_back(pWalk->nSize > 0 ? pWalk->nSize : 1);
        pWalk = pWalk->pprev;
    }

    if (!vLongSizes.empty())
    {
        std::sort(vLongSizes.begin(), vLongSizes.end());
        unsigned int nLongMedian = vLongSizes[vLongSizes.size() / 2];
        if (nLongMedian < ADAPTIVE_BLOCK_FLOOR)
            nLongMedian = ADAPTIVE_BLOCK_FLOOR;

        // Cap short-term median at ADAPTIVE_LONG_MEDIAN_CAP * long-term median (overflow-safe)
        uint64_t nCap64 = (uint64_t)nLongMedian * ADAPTIVE_LONG_MEDIAN_CAP;
        unsigned int nCap = (nCap64 > ADAPTIVE_BLOCK_CEILING) ? ADAPTIVE_BLOCK_CEILING : (unsigned int)nCap64;
        if (nShortMedian > nCap)
            nShortMedian = nCap;
    }

    // Effective limit = 2x median (max allowed size, matches Monero) — overflow-safe
    uint64_t nEffective64 = (uint64_t)nShortMedian * 2;
    unsigned int nEffectiveLimit = (nEffective64 > ADAPTIVE_BLOCK_CEILING) ? ADAPTIVE_BLOCK_CEILING : (unsigned int)nEffective64;

    // Clamp to ceiling
    if (nEffectiveLimit > ADAPTIVE_BLOCK_CEILING)
        nEffectiveLimit = ADAPTIVE_BLOCK_CEILING;

    return nEffectiveLimit;
}

int64_t GetBlockSizePenalty(unsigned int nBlockSize, unsigned int nMedianSize)
{
    // No penalty if block is at or below the median
    if (nBlockSize <= nMedianSize || nMedianSize == 0)
        return 0;

    // Quadratic penalty: penalty = baseReward * ((blockSize / median) - 1)^2
    // Returns the penalty as a fraction of COIN (COIN = 100% of block reward lost)
    // At blockSize == 2 * median: penalty = COIN (100% — miner gets nothing)
    // Clamp ratio: block can't exceed 2x median by consensus, so cap at 2*COIN
    int64_t nRatio = ((int64_t)nBlockSize * COIN) / nMedianSize;
    if (nRatio > 2 * COIN)
        nRatio = 2 * COIN;
    int64_t nExcess = nRatio - COIN; // (blockSize/median - 1) * COIN
    if (nExcess <= 0)
        return 0;

    // penalty = excess^2 / COIN (quadratic, overflow-safe with clamped nExcess <= COIN)
    int64_t nPenalty = (nExcess * nExcess) / COIN;

    // Cap at COIN (100% penalty)
    if (nPenalty > COIN)
        nPenalty = COIN;

    return nPenalty;
}

/** Apply adaptive block size penalty to a reward. Returns adjusted reward.
 *  Must be called with the block being validated and its parent index. */
int64_t ApplyBlockSizePenalty(int64_t nReward, const CBlock& block, const CBlockIndex* pindexPrev)
{
    if (!pindexPrev || pindexPrev->nHeight + 1 < FORK_HEIGHT_DAG)
        return nReward;

    unsigned int nBlockBytes = ::GetSerializeSize(block, SER_NETWORK, PROTOCOL_VERSION);
    // The adaptive limit is 2x median; the median is limit/2
    unsigned int nMedian = GetAdaptiveBlockSizeLimit(pindexPrev) / 2;
    if (nMedian < ADAPTIVE_BLOCK_FLOOR)
        nMedian = ADAPTIVE_BLOCK_FLOOR;

    int64_t nPenalty = GetBlockSizePenalty(nBlockBytes, nMedian);
    if (nPenalty > 0 && nReward > 0)
    {
        int64_t nPenaltyAmount = (nReward * nPenalty) / COIN;
        nReward -= nPenaltyAmount;
        if (nReward < 0) nReward = 0;
    }
    return nReward;
}


bool CTransaction::CheckTransaction() const
{
    // Basic checks that don't depend on any context
    if (vin.empty() && !IsShielded())
        return DoS(10, error("CTransaction::CheckTransaction() : vin empty"));
    if (vout.empty() && !IsShielded())
        return DoS(10, error("CTransaction::CheckTransaction() : vout empty"));
    // Size limits
    if (::GetSerializeSize(*this, SER_NETWORK, PROTOCOL_VERSION) > MAX_BLOCK_SIZE)
        return DoS(100, error("CTransaction::CheckTransaction() : size limits failed"));

    // Check for negative or overflow output values
    int64_t nValueOut = 0;
    for (unsigned int i = 0; i < vout.size(); i++)
    {
        const CTxOut& txout = vout[i];
        if (txout.IsEmpty() && !IsCoinBase() && !IsCoinStake())
            return DoS(100, error("CTransaction::CheckTransaction() : txout empty for user transaction"));
        if (txout.nValue < 0)
            return DoS(100, error("CTransaction::CheckTransaction() : txout.nValue negative"));
        if (txout.nValue > MAX_MONEY)
            return DoS(100, error("CTransaction::CheckTransaction() : txout.nValue too high"));
        nValueOut += txout.nValue;
        if (!MoneyRange(nValueOut))
            return DoS(100, error("CTransaction::CheckTransaction() : txout total out of range"));
    }

    // Check for duplicate inputs
    set<COutPoint> vInOutPoints;
    for (const CTxIn& txin : vin)
    {
        if (nVersion == ANON_TXN_VERSION
            && txin.IsAnonInput())
        {
            // -- blank the upper 3 bytes of n to prevent the same keyimage passing with different ring sizes
            COutPoint opTest = txin.prevout;
            opTest.n &= 0xFF;
            if (vInOutPoints.count(opTest))
            {
                if (fDebugRingSig)
                    printf("CheckTransaction() failed - found duplicate keyimage in txn %s\n", GetHash().ToString().c_str());
                return false;
            };
            vInOutPoints.insert(opTest);
            continue;
        };

        if (vInOutPoints.count(txin.prevout))
            return false;
        vInOutPoints.insert(txin.prevout);
    };

    if (nVersion == ANON_TXN_VERSION)
    {
        // -- Check for duplicate anon outputs
        // NOTE: is this necessary, duplicate coins would not be spendable anyway?
        set<CPubKey> vAnonOutPubkeys;
        CPubKey pkTest;
        for (const CTxOut& txout : vout)
        {
            if (!txout.IsAnonOutput())
                continue;

            const CScript &s = txout.scriptPubKey;
            pkTest = CPubKey(&s[2+1], 33);
            if (vAnonOutPubkeys.count(pkTest))
                return false;
            vAnonOutPubkeys.insert(pkTest);
        };
    };

    if (IsShielded())
    {
        if (IsCoinBase())
            return DoS(100, error("CTransaction::CheckTransaction() : shielded transaction cannot be coinbase"));
        if (IsCoinStake() && nVersion != SHIELDED_TX_VERSION_NULLSTAKE && nVersion != SHIELDED_TX_VERSION_NULLSTAKE_V2 && nVersion != SHIELDED_TX_VERSION_NULLSTAKE_COLD)
            return DoS(100, error("CTransaction::CheckTransaction() : shielded transaction cannot be coinstake"));

        if (vShieldedSpend.empty() && vShieldedOutput.empty())
            return DoS(100, error("CTransaction::CheckTransaction() : shielded tx has no shielded components"));

        if (vShieldedSpend.size() > MAX_SHIELDED_INPUTS)
            return DoS(100, error("CTransaction::CheckTransaction() : too many shielded spends (%u > %u)",
                                  (unsigned int)vShieldedSpend.size(), (unsigned int)MAX_SHIELDED_INPUTS));
        if (vShieldedOutput.size() > MAX_SHIELDED_OUTPUTS)
            return DoS(100, error("CTransaction::CheckTransaction() : too many shielded outputs (%u > %u)",
                                  (unsigned int)vShieldedOutput.size(), (unsigned int)MAX_SHIELDED_OUTPUTS));

        if (nValueBalance < -MAX_MONEY || nValueBalance > MAX_MONEY)
            return DoS(100, error("CTransaction::CheckTransaction() : shielded nValueBalance out of range"));

        set<uint256> vNullifiers;
        for (const CShieldedSpendDescription& spend : vShieldedSpend)
        {
            if (spend.nullifier == 0)
                return DoS(100, error("CTransaction::CheckTransaction() : zero shielded nullifier"));

            if (vNullifiers.count(spend.nullifier))
                return DoS(100, error("CTransaction::CheckTransaction() : duplicate shielded nullifier"));
            vNullifiers.insert(spend.nullifier);
        }

        if (IsDSP())
        {
            if (nBestHeight < FORK_HEIGHT_DSP)
                return DoS(100, error("CTransaction::CheckTransaction() : DSP transactions not active until height %d", FORK_HEIGHT_DSP));

            if (nPrivacyMode > PRIVACY_MODE_MASK)
                return DoS(100, error("CTransaction::CheckTransaction() : invalid privacy mode %d (max 7)", nPrivacyMode));

            bool fHideAmount   = DSP_HideAmount(nPrivacyMode);
            bool fHideSender   = DSP_HideSender(nPrivacyMode);
            bool fHideReceiver = DSP_HideReceiver(nPrivacyMode);

            for (size_t i = 0; i < vShieldedSpend.size(); i++)
            {
                const CShieldedSpendDescription& spend = vShieldedSpend[i];
                if (!fHideAmount)
                {
                    if (spend.nPlaintextValue < 0 || spend.nPlaintextValue > MAX_MONEY)
                        return DoS(100, error("CTransaction::CheckTransaction() : DSP spend %u invalid plaintext value", (unsigned int)i));
                    if (spend.vchPlaintextBlind.size() != 32)
                        return DoS(100, error("CTransaction::CheckTransaction() : DSP spend %u missing blinding factor", (unsigned int)i));
                }
                else
                {
                    if (spend.nPlaintextValue != -1)
                        return DoS(100, error("CTransaction::CheckTransaction() : DSP spend %u has plaintext value in hidden-amount mode", (unsigned int)i));
                    if (!spend.vchPlaintextBlind.empty())
                        return DoS(100, error("CTransaction::CheckTransaction() : DSP spend %u has blinding factor in hidden-amount mode", (unsigned int)i));
                }
                if (!fHideSender)
                {
                    if (!spend.vchLelantusProof.empty() || !spend.vAnonSet.empty())
                        return DoS(100, error("CTransaction::CheckTransaction() : DSP spend %u has Lelantus proof in public-sender mode", (unsigned int)i));
                }
            }

            for (size_t i = 0; i < vShieldedOutput.size(); i++)
            {
                const CShieldedOutputDescription& output = vShieldedOutput[i];
                if (!fHideAmount)
                {
                    if (output.nPlaintextValue < 0 || output.nPlaintextValue > MAX_MONEY)
                        return DoS(100, error("CTransaction::CheckTransaction() : DSP output %u invalid plaintext value", (unsigned int)i));
                    if (output.vchPlaintextBlind.size() != 32)
                        return DoS(100, error("CTransaction::CheckTransaction() : DSP output %u missing blinding factor", (unsigned int)i));
                }
                else
                {
                    if (output.nPlaintextValue != -1)
                        return DoS(100, error("CTransaction::CheckTransaction() : DSP output %u has plaintext value in hidden-amount mode", (unsigned int)i));
                    if (!output.vchPlaintextBlind.empty())
                        return DoS(100, error("CTransaction::CheckTransaction() : DSP output %u has blinding factor in hidden-amount mode", (unsigned int)i));
                }
                if (fHideReceiver)
                {
                    if (!output.vchRecipientScript.empty())
                        return DoS(100, error("CTransaction::CheckTransaction() : DSP output %u has public recipient in hidden-receiver mode", (unsigned int)i));
                }
            }
        }
    };

    if (IsCoinBase())
    {
        if (vin[0].scriptSig.size() < 2 || vin[0].scriptSig.size() > 100)
            return DoS(100, error("CTransaction::CheckTransaction() : coinbase script size is invalid"));
    }
    else
    {
        for (const CTxIn& txin : vin)
            if (txin.prevout.IsNull())
                return DoS(10, error("CTransaction::CheckTransaction() : prevout is null"));
    } //New ban code for hybrid collateralnodes and FMPS - Not for prime time yet, may or may not be used
	/*
	else
	{
		BOOST_FOREACH(const CTxIn& txin, vin)
			if (txin.prevout.IsBanned()){ // new function that checks if the txin.prevout matches an address
				txin.prevout.SetNull(); // this should set the UTXO to null
				return DoS(10, error("CheckTransaction(): You have been caught trying to cheat. Kthxbai"));
			}
	}
	*/
    //return hooks->CheckTransaction(*this);
    return true;
}

int64_t CTransaction::GetMinFee(unsigned int nBlockSize, enum GetMinFee_mode mode, unsigned int nBytes) const
{
    // Base fee is either MIN_TX_FEE or MIN_RELAY_TX_FEE for standard txns, and MIN_TX_FEE_ANON for anon txns

    if (nVersion == ANON_TXN_VERSION || IsShielded())
        mode = GMF_ANON;

    int64_t nBaseFee;
    switch (mode)
    {
        case GMF_RELAY: nBaseFee = MIN_RELAY_TX_FEE; break;
        case GMF_ANON:  nBaseFee = MIN_TX_FEE_ANON;  break;
        default:        nBaseFee = MIN_TX_FEE;       break;
    };

    unsigned int nNewBlockSize = nBlockSize + nBytes;
    int64_t nMinFee = (1 + (int64_t)nBytes / 1000) * nBaseFee;

    // To limit dust spam, require MIN_TX_FEE/MIN_RELAY_TX_FEE if any output is less than 0.01
    if (nMinFee < nBaseFee)
    {
        for (const CTxOut& txout : vout)
            if (txout.nValue < CENT)
                nMinFee = nBaseFee;
    };

    // Raise the price as the block approaches full
    if (mode != GMF_ANON && nBlockSize != 1 && nNewBlockSize >= MAX_BLOCK_SIZE_GEN/2)
    {
        if (nNewBlockSize >= MAX_BLOCK_SIZE_GEN)
            return MAX_MONEY;
        nMinFee *= MAX_BLOCK_SIZE_GEN / (MAX_BLOCK_SIZE_GEN - nNewBlockSize);
    };

    if (!MoneyRange(nMinFee))
        nMinFee = MAX_MONEY;
    return nMinFee;
}

bool CTxMemPool::accept(CTxDB& txdb, CTransaction &tx, bool fCheckInputs,
                        bool* pfMissingInputs, bool fOnlyCheckWithoutAdding)
{
    AssertLockHeld(cs_main);
    printf("CTxMemPool::accept, fCheckInputs = %d, fOnlyCheckWithoutAdding = %d, ver=%d, vin=%u, vout=%u, vSS=%u, vSO=%u\n",
           fCheckInputs, fOnlyCheckWithoutAdding, tx.nVersion, (unsigned)tx.vin.size(),
           (unsigned)tx.vout.size(), (unsigned)tx.vShieldedSpend.size(), (unsigned)tx.vShieldedOutput.size());
    if (pfMissingInputs)
        *pfMissingInputs = false;

    size_t nMaxMempoolSize = GetArg("-maxmempool", DEFAULT_MAX_MEMPOOL_SIZE) * 1000000;
    if (GetTotalMemoryUsage() >= nMaxMempoolSize)
        return error("CTxMemPool::accept() : mempool full (%" PRIu64" bytes)", (uint64_t)nMaxMempoolSize);

    if (!tx.CheckTransaction())
        return error("CTxMemPool::accept() : CheckTransaction failed");

    // Coinbase is only valid in a block, not as a loose transaction
    if (tx.IsCoinBase())
        return tx.DoS(100, error("CTxMemPool::accept() : coinbase as individual tx"));

    // ppcoin: coinstake is also only valid in a block, not as a loose transaction
    if (tx.IsCoinStake())
        return tx.DoS(100, error("CTxMemPool::accept() : coinstake as individual tx"));

    //bool isNameTx = hooks->IsNameFeeEnough(txdb, tx); //accept name tx with correct fee.
    bool isNameTx = tx.nVersion == NAMECOIN_TX_VERSION;
    // Rather not work on nonstandard transactions (unless -testnet)
    string reason;
    if (!fTestNet && !IsStandardTx(tx, reason) && !isNameTx) //!IsStandardTx(tx, reason)
        return error("CTxMemPool::accept() : nonstandard transaction type");

    // Do we already have it?
    uint256 hash = tx.GetHash();
    {
        LOCK(cs);
        if (mapTx.count(hash))
            return false;
        // IDAG Phase 3: Reject txids already included in a DAG sibling block
        if (setDAGSeenTxids.count(hash))
            return error("CTxMemPool::accept() : tx %s already in DAG sibling block", hash.ToString().substr(0, 20).c_str());
    }

    if (txdb.ContainsTx(hash))
        return false;

    // Check for conflicts with in-memory transactions
    CTransaction* ptxOld = NULL;
    for (unsigned int i = 0; i < tx.vin.size(); i++)
    {
        COutPoint outpoint = tx.vin[i].prevout;
        if (mapNextTx.count(outpoint))
        {
            // Disable replacement feature for now
            return false;

            // Allow replacing with a newer version of the same transaction
            if (i != 0)
                return false;
            ptxOld = mapNextTx[outpoint].ptx;
            if (ptxOld->IsFinal())
                return false;
            if (!tx.IsNewerThan(*ptxOld))
                return false;
            for (unsigned int i = 0; i < tx.vin.size(); i++)
            {
                COutPoint outpoint = tx.vin[i].prevout;
                if (!mapNextTx.count(outpoint) || mapNextTx[outpoint].ptx != ptxOld)
                    return false;
            }
            break;
        }
    }

    {
        MapPrevTx mapInputs;
        //map<uint256, CTxIndex> mapUnused;
		std::map<uint256, CTxIndex> mapUnused;
        bool fInvalid = false;
        int64_t nFees;
        if (!tx.FetchInputs(txdb, mapUnused, false, false, mapInputs, fInvalid))
        {
            if (fInvalid)
            {
                if (fDebug)
                    return error("CTxMemPool::accept() : FetchInputs found invalid tx %s", hash.ToString().substr(0,10).c_str());
                else return false;
            }

            if (pfMissingInputs)
                *pfMissingInputs = true;
            return false;
        }
            // Check for non-standard pay-to-script-hash in inputs
            if (!AreInputsStandard(tx, mapInputs) && !fTestNet && !isNameTx)
                return error("CTxMemPool::accept() : nonstandard transaction input");

            nFees = tx.GetValueIn(mapInputs) - tx.GetValueOut();

            if (tx.IsShielded() && tx.nValueBalance != 0)
                nFees += tx.nValueBalance;

            GetMinFee_mode feeMode = GMF_RELAY;

            if (tx.nVersion == ANON_TXN_VERSION)
            {
                if (nBestHeight >= FORK_HEIGHT_RINGSIG_DEPRECATION)
                    return error("CTxMemPool::accept() : ring signature transactions (ANON_TXN_VERSION) deprecated after height %d. Use shielded transactions.", FORK_HEIGHT_RINGSIG_DEPRECATION);

                int64_t nSumAnon;
                if (!tx.CheckAnonInputs(txdb, nSumAnon, fInvalid, true))
                {
                    if (fInvalid)
                        return error("CTxMemPool::accept() : CheckAnonInputs found invalid tx %s", hash.ToString().substr(0,10).c_str());
                    if (pfMissingInputs)
                        *pfMissingInputs = true;
                    return false;
                };

                nFees += nSumAnon;

                feeMode = GMF_ANON;
            };

            if (tx.IsShielded())
            {
                if (pindexBest && pindexBest->nHeight < FORK_HEIGHT_SHIELDED)
                    return error("CTxMemPool::accept() : shielded tx rejected before fork height %d", FORK_HEIGHT_SHIELDED);

                for (const CShieldedSpendDescription& spend : tx.vShieldedSpend)
                {
                    CShieldedNullifierSpent nfs;
                    if (txdb.ReadShieldedNullifier(spend.nullifier, nfs))
                        return error("CTxMemPool::accept() : shielded nullifier %s already spent",
                                     spend.nullifier.ToString().substr(0,10).c_str());

                    CShieldedNullifierSpent nfsMem;
                    if (lookupShieldedNullifier(spend.nullifier, nfsMem))
                        return error("CTxMemPool::accept() : shielded nullifier %s already in mempool",
                                     spend.nullifier.ToString().substr(0,10).c_str());

                    if (!txdb.ReadShieldedAnchor(spend.anchor))
                        return error("CTxMemPool::accept() : shielded anchor %s not found",
                                     spend.anchor.ToString().substr(0,10).c_str());
                    int nAnchorHeight = 0;
                    if (txdb.ReadShieldedAnchorHeight(spend.anchor, nAnchorHeight))
                    {
                        if (nBestHeight - nAnchorHeight < MIN_SHIELDED_SPEND_DEPTH)
                            return error("CTxMemPool::accept() : shielded anchor %s too recent (height=%d, need %d confirmations)",
                                         spend.anchor.ToString().substr(0,10).c_str(),
                                         nAnchorHeight, MIN_SHIELDED_SPEND_DEPTH);
                    }
                }

                int64_t nTransparentIn = tx.GetValueIn(mapInputs);
                int64_t nTransparentOut = tx.GetValueOut();

                int64_t nEffectiveIn = nTransparentIn;
                if (tx.nValueBalance > 0)
                {
                    if (nEffectiveIn > MAX_MONEY - tx.nValueBalance)
                        return error("CTxMemPool::accept() : nEffectiveIn overflow");
                    nEffectiveIn += tx.nValueBalance;
                }

                int64_t nEffectiveOut = nTransparentOut;
                if (tx.nValueBalance < 0)
                {
                    if (tx.nValueBalance == std::numeric_limits<int64_t>::min())
                        return error("CTxMemPool::accept() : nValueBalance is INT64_MIN");
                    int64_t nAbsBalance = -tx.nValueBalance;
                    if (nEffectiveOut > MAX_MONEY - nAbsBalance)
                        return error("CTxMemPool::accept() : nEffectiveOut overflow");
                    nEffectiveOut += nAbsBalance;
                }

                if (nEffectiveIn < nEffectiveOut)
                    return error("CTxMemPool::accept() : shielded value balance mismatch (in=%" PRId64 " out=%" PRId64 ")",
                                 nEffectiveIn, nEffectiveOut);

                nFees = nEffectiveIn - nEffectiveOut;

                if (!CZKContext::IsInitialized())
                    return error("CTxMemPool::accept() : ZK context not initialized, cannot validate shielded tx");

                {
                    uint256 sighash = tx.GetBindingSigHash();

                    bool fHideAmount   = tx.IsDSP() ? DSP_HideAmount(tx.nPrivacyMode)   : true;
                    bool fHideSender   = tx.IsDSP() ? DSP_HideSender(tx.nPrivacyMode)   : true;

                    if (nBestHeight >= FORK_HEIGHT_FCMP_VALIDATION && !tx.vShieldedSpend.empty()
                        && tx.nVersion < SHIELDED_TX_VERSION_FCMP)
                    {
                        return error("CTxMemPool::accept() : tx version %d with shielded spends rejected after FCMP fork (need version >= %d)",
                                     tx.nVersion, SHIELDED_TX_VERSION_FCMP);
                    }

                    if (tx.nVersion >= SHIELDED_TX_VERSION_FCMP && nBestHeight >= FORK_HEIGHT_FCMP_VALIDATION
                        && !tx.vShieldedSpend.empty())
                    {
                        for (size_t j = 0; j < tx.vShieldedSpend.size(); j++)
                        {
                            if (tx.vShieldedSpend[j].fcmpProof.IsNull())
                                return error("CTxMemPool::accept() : FCMP version tx spend %u missing mandatory FCMP proof", (unsigned)j);
                        }
                    }

                    CCurveTreeNode fcmpRootNode;
                    uint256 hashExpectedFCMPRoot = 0;
                    if (tx.nVersion >= SHIELDED_TX_VERSION_FCMP && nBestHeight >= FORK_HEIGHT_FCMP_VALIDATION
                        && !tx.vShieldedSpend.empty())
                    {
                        CTxDB txdb("r");
                        std::string strFCMPError;
                        if (!LoadFCMPValidationRoot(txdb, nBestHeight, fcmpRootNode, hashExpectedFCMPRoot, strFCMPError))
                            return error("CTxMemPool::accept() : %s", strFCMPError.c_str());
                        if (!CheckFCMPSpendRoots(tx, nBestHeight, hashExpectedFCMPRoot, strFCMPError))
                            return error("CTxMemPool::accept() : %s", strFCMPError.c_str());
                    }

                    for (size_t i = 0; i < tx.vShieldedSpend.size(); i++)
                    {
                        if (fHideAmount)
                        {
                            if (!VerifyBulletproofRangeProof(tx.vShieldedSpend[i].cv, tx.vShieldedSpend[i].rangeProof))
                                return error("CTxMemPool::accept() : shielded spend %d range proof failed", (int)i);
                        }
                        else
                        {
                            if (!VerifyPedersenCommitment(tx.vShieldedSpend[i].cv,
                                                           tx.vShieldedSpend[i].nPlaintextValue,
                                                           tx.vShieldedSpend[i].vchPlaintextBlind))
                                return error("CTxMemPool::accept() : DSP spend %d commitment opening proof failed", (int)i);
                        }

                        if (tx.vShieldedSpend[i].vchSpendAuthSig.empty() || tx.vShieldedSpend[i].vchRk.empty())
                            return error("CTxMemPool::accept() : shielded spend %d missing spend auth signature or rk", (int)i);

                        if (!VerifySpendAuthSignature(tx.vShieldedSpend[i].vchRk, sighash, tx.vShieldedSpend[i].vchSpendAuthSig))
                            return error("CTxMemPool::accept() : shielded spend %d spend auth signature failed", (int)i);

                        if (fHideSender)
                        {
                            if (tx.vShieldedSpend[i].vchLelantusProof.empty() || tx.vShieldedSpend[i].vAnonSet.empty())
                                return error("CTxMemPool::accept() : shielded spend %d missing mandatory Lelantus proof", (int)i);

                            if ((int)tx.vShieldedSpend[i].vAnonSet.size() < LELANTUS_MIN_SET_SIZE)
                                return error("CTxMemPool::accept() : shielded spend %d anonymity set size %d below minimum %d",
                                             (int)i, (int)tx.vShieldedSpend[i].vAnonSet.size(), LELANTUS_MIN_SET_SIZE);

                            {
                                // Verify all vAnonSet commitments exist on-chain
                                {
                                    CTxDB txdb("r");
                                    std::set<std::vector<unsigned char>> setChainCommitments;
                                    uint64_t nCommitCount = 0;
                                    txdb.ReadShieldedCommitmentCount(nCommitCount);
                                    for (uint64_t ci = 0; ci < nCommitCount; ci++)
                                    {
                                        CPedersenCommitment chainCommit;
                                        if (txdb.ReadShieldedCommitment(ci, chainCommit))
                                            setChainCommitments.insert(chainCommit.vchCommitment);
                                    }

                                    for (size_t j = 0; j < tx.vShieldedSpend[i].vAnonSet.size(); j++)
                                    {
                                        if (setChainCommitments.find(tx.vShieldedSpend[i].vAnonSet[j].vchCommitment) == setChainCommitments.end())
                                            return error("CTxMemPool::accept() : shielded spend %d anonymity set commitment %d not found in chain state", (int)i, (int)j);
                                    }
                                }

                                CAnonymitySet anonSet;
                                anonSet.vCommitments = tx.vShieldedSpend[i].vAnonSet;
                                CLelantusProof proof;
                                proof.vchProof = tx.vShieldedSpend[i].vchLelantusProof;
                                proof.serialNumber = tx.vShieldedSpend[i].lelantusSerial;

                                if (!VerifyLelantusProof(anonSet, proof, tx.vShieldedSpend[i].cv))
                                    return error("CTxMemPool::accept() : shielded spend %d Lelantus proof failed", (int)i);
                            }
                        }

                        // FCMP++ proof: required after FORK_HEIGHT_FCMP_VALIDATION for FCMP tx versions
                        if (tx.nVersion >= SHIELDED_TX_VERSION_FCMP && nBestHeight >= FORK_HEIGHT_FCMP_VALIDATION)
                        {
                            if (tx.vShieldedSpend[i].fcmpProof.IsNull())
                                return error("CTxMemPool::accept() : shielded spend %d missing FCMP++ proof (required post-fork)", (int)i);

                            if (!VerifyFCMPProof(fcmpRootNode, tx.vShieldedSpend[i].fcmpProof, tx.vShieldedSpend[i].cv))
                                return error("CTxMemPool::accept() : shielded spend %d FCMP++ proof failed", (int)i);
                        }

                        if (fDebug)
                            printf("CTxMemPool::accept() : spend %d passed all checks\n", (int)i);
                    }

                    if (fDebug)
                        printf("CTxMemPool::accept() : verifying output proofs\n");
                    for (size_t i = 0; i < tx.vShieldedOutput.size(); i++)
                    {
                        if (fHideAmount)
                        {
                            if (!VerifyBulletproofRangeProof(tx.vShieldedOutput[i].cv, tx.vShieldedOutput[i].rangeProof))
                                return error("CTxMemPool::accept() : shielded output %d range proof failed", (int)i);
                        }
                        else
                        {
                            if (!VerifyPedersenCommitment(tx.vShieldedOutput[i].cv,
                                                           tx.vShieldedOutput[i].nPlaintextValue,
                                                           tx.vShieldedOutput[i].vchPlaintextBlind))
                                return error("CTxMemPool::accept() : DSP output %d commitment opening proof failed", (int)i);
                        }
                    }

                    if (!fHideAmount)
                    {
                        int64_t nPlainIn = 0, nPlainOut = 0;
                        for (size_t i = 0; i < tx.vShieldedSpend.size(); i++)
                            nPlainIn += tx.vShieldedSpend[i].nPlaintextValue;
                        for (size_t i = 0; i < tx.vShieldedOutput.size(); i++)
                            nPlainOut += tx.vShieldedOutput[i].nPlaintextValue;
                        if (nPlainIn - nPlainOut != tx.nValueBalance)
                            return error("CTxMemPool::accept() : DSP plaintext value balance mismatch (in=%" PRId64 " out=%" PRId64 " balance=%" PRId64 ")",
                                         nPlainIn, nPlainOut, tx.nValueBalance);
                    }

                    if (fDebug)
                        printf("CTxMemPool::accept() : output proofs passed\n");

                    if (tx.bindingSig.IsNull())
                        return error("CTxMemPool::accept() : shielded tx missing mandatory binding signature");

                    {
                        std::vector<CPedersenCommitment> vInCommits, vOutCommits;
                        for (size_t i = 0; i < tx.vShieldedSpend.size(); i++)
                            vInCommits.push_back(tx.vShieldedSpend[i].cv);
                        for (size_t i = 0; i < tx.vShieldedOutput.size(); i++)
                            vOutCommits.push_back(tx.vShieldedOutput[i].cv);

                        if (!VerifyBindingSignature(vInCommits, vOutCommits, tx.nValueBalance, sighash, tx.bindingSig.bindingSig))
                            return error("CTxMemPool::accept() : shielded binding signature verification failed");
                    }
                }
            };

            // Note: if you modify this code to accept non-standard transactions, then
            // you should add code here to check that the transaction does a
            // reasonable number of ECDSA signature verifications.

            unsigned int nSize = ::GetSerializeSize(tx, SER_NETWORK, PROTOCOL_VERSION);
            // Don't accept it if it can't get into a block

            int64_t txMinFee = tx.GetMinFee(1000, feeMode, nSize);

            if (nFees < txMinFee && !isNameTx)
            {
                return error("CTxMemPool::accept() : not enough fees %s, %" PRId64" < %" PRId64,
                             hash.ToString().c_str(),
                             nFees, txMinFee);
            };

            // Continuously rate-limit free transactions
            // This mitigates 'penny-flooding' -- sending thousands of free transactions just to
            // be annoying or make others' transactions take longer to confirm.
            if (nFees < MIN_RELAY_TX_FEE)
            {
                static CCriticalSection cs;
                static double dFreeCount;
                static int64_t nLastTime;
                int64_t nNow = GetTime();

                {
                    LOCK(cs);
                    // Use an exponentially decaying ~10-minute window:
                    dFreeCount *= pow(1.0 - 1.0/600.0, (double)(nNow - nLastTime));
                    nLastTime = nNow;
                    // -limitfreerelay unit is thousand-bytes-per-minute
                    // At default rate it would take over a month to fill 1GB
                    if (dFreeCount > GetArg("-limitfreerelay", 15)*10*1000 && !IsFromMe(tx))
                        return error("CTxMemPool::accept() : free transaction rejected by rate limiter");
                    if (fDebug)
                        printf("Rate limit dFreeCount: %g => %g\n", dFreeCount, dFreeCount+nSize);
                    dFreeCount += nSize;
                }
            };

            // Check against previous transactions
            // This is done last to help prevent CPU exhaustion denial-of-service attacks.
            printf("CTxMemPool::accept() : calling ConnectInputs for %s\n", hash.ToString().substr(0,10).c_str());
            if (!tx.ConnectInputs(txdb, mapInputs, mapUnused, CDiskTxPos(1,1,1), pindexBest, false, false))
            {
                return error("CTxMemPool::accept() : ConnectInputs failed %s", hash.ToString().substr(0,10).c_str());
            };
        };

    // Do not write to memory if read only mode.
    if(!fOnlyCheckWithoutAdding)
    {
        // Store transaction in memory
        {
            LOCK(cs);
            if (ptxOld) {
                printf("CTxMemPool::accept() : replacing tx %s with new version\n", ptxOld->GetHash().ToString().c_str());
                remove(*ptxOld);
            }
            addUnchecked(hash, tx);

            if (tx.IsShielded())
            {
                for (unsigned int i = 0; i < tx.vShieldedSpend.size(); i++)
                {
                    CShieldedNullifierSpent nfs;
                    nfs.txnHash = hash;
                    nfs.nIndex = i;
                    insertShieldedNullifier(tx.vShieldedSpend[i].nullifier, nfs);
                }
            }

            //Add the TX to our Pending Names in Name DB
            hooks->AddToPendingNames(tx);
        }

        ///// are we sure this is ok when loading transactions or restoring block txes
        // If updated, erase old tx from wallet
        if (ptxOld)
            EraseFromWallets(ptxOld->GetHash());

        printf("CTxMemPool::accept() : accepted %s (poolsz %" PRIszu")\n", hash.ToString().substr(0,10).c_str(), mapTx.size());
    }
    return true;
}

bool CTransaction::AcceptToMemoryPool(CTxDB& txdb,  bool fCheckInputs, bool* pfMissingInputs, bool fOnlyCheckWithoutAdding)
{
    return mempool.accept(txdb, *this, fCheckInputs, pfMissingInputs, fOnlyCheckWithoutAdding);
}

bool AcceptableInputs(CTxMemPool& pool, const CTransaction &txo, bool fLimitFree,
                        bool* pfMissingInputs)
{
    AssertLockHeld(cs_main);
    if (pfMissingInputs)
        *pfMissingInputs = false;

    CTransaction tx(txo);

    if (!tx.CheckTransaction())
        return error("AcceptableInputs : CheckTransaction failed");

    if (tx.IsShielded())
        return error("AcceptableInputs : shielded transactions require full validation");

    // Coinbase is only valid in a block, not as a loose transaction
    if (tx.IsCoinBase())
        return tx.DoS(100, error("AcceptableInputs : coinbase as individual tx"));

    // ppcoin: coinstake is also only valid in a block, not as a loose transaction
    if (tx.IsCoinStake())
        return tx.DoS(100, error("AcceptableInputs : coinstake as individual tx"));

    // Rather not work on nonstandard transactions (unless -testnet)
    string reason;
    if (false && !fTestNet && !IsStandardTx(tx, reason))
        return error("AcceptableInputs : nonstandard transaction");

    // is it already in the memory pool?
    uint256 hash = tx.GetHash();
    if (pool.exists(hash))
        return false;

    // Check for conflicts with in-memory transactions
    {
    LOCK(pool.cs); // protect pool.mapNextTx
    for (unsigned int i = 0; i < tx.vin.size(); i++)
    {
        COutPoint outpoint = tx.vin[i].prevout;
        if (pool.mapNextTx.count(outpoint))
        {
            // Disable replacement feature for now
            return false;
        }
    }
    }

    {
        CTxDB txdb("r");

        // do we already have it?
        if (txdb.ContainsTx(hash))
            return false;

        MapPrevTx mapInputs;
        map<uint256, CTxIndex> mapUnused;
        bool fInvalid = false;
        if (!tx.FetchInputs(txdb, mapUnused, false, false, mapInputs, fInvalid))
        {
            if (fInvalid)
                if (fDebugNet) return error("AcceptableInputs : FetchInputs found invalid tx %s", hash.ToString().substr(0,10).c_str());
                return false;
            if (pfMissingInputs)
                *pfMissingInputs = true;
            return false;
        }

        // Check for non-standard pay-to-script-hash in inputs
        //if (!fTestNet() && !tx.AreInputsStandard(mapInputs))
          //  return error("AcceptToMemoryPool : nonstandard transaction input");

	    // Check that the transaction doesn't have an excessive number of
        // sigops, making it impossible to mine. Since the coinbase transaction
        // itself can contain sigops MAX_TX_SIGOPS is less than
        // MAX_BLOCK_SIGOPS; we still consider this an invalid rather than
        // merely non-standard transaction.
	    unsigned int nSigOps = tx.GetLegacySigOpCount();
	    nSigOps += tx.GetP2SHSigOpCount(mapInputs);
        if (nSigOps > MAX_TX_SIGOPS)
            return tx.DoS(0,
                          error("AcceptToMemoryPool : too many sigops %s, %d > %d",
                                hash.ToString().c_str(), nSigOps, MAX_TX_SIGOPS));

        int64_t nFees = tx.GetValueIn(mapInputs)-tx.GetValueOut();
        unsigned int nSize = ::GetSerializeSize(tx, SER_NETWORK, PROTOCOL_VERSION);

        // Don't accept it if it can't get into a block
        int64_t txMinFee = tx.GetMinFee(1000, GMF_RELAY, nSize);
        if ((fLimitFree && nFees < txMinFee) || (!fLimitFree && nFees < MIN_TX_FEE))
            return error("AcceptableInputs : not enough fees %s, %ld < %ld",
                         hash.ToString().c_str(),
                         nFees, txMinFee);

        // Continuously rate-limit free transactions
        // This mitigates 'penny-flooding' -- sending thousands of free transactions just to
        // be annoying or make others' transactions take longer to confirm.
        if (fLimitFree && nFees < MIN_RELAY_TX_FEE)
        {
            static CCriticalSection csFreeLimiter;
            static double dFreeCount;
            static int64_t nLastTime;
            int64_t nNow = GetTime();

            LOCK(csFreeLimiter);

            // Use an exponentially decaying ~10-minute window:
            dFreeCount *= pow(1.0 - 1.0/600.0, (double)(nNow - nLastTime));
            nLastTime = nNow;
            // -limitfreerelay unit is thousand-bytes-per-minute
            // At default rate it would take over a month to fill 1GB
            if (dFreeCount > GetArg("-limitfreerelay", 15)*10*1000)
                return error("AcceptableInputs : free transaction rejected by rate limiter");
            printf("mempool: Rate limit dFreeCount: %g => %g\n", dFreeCount, dFreeCount+nSize);
            dFreeCount += nSize;
        }

        // Check against previous transactions
        // This is done last to help prevent CPU exhaustion denial-of-service attacks.
        if (!tx.ConnectInputs(txdb, mapInputs, mapUnused, CDiskTxPos(1,1,1), pindexBest, true, false, STANDARD_SCRIPT_VERIFY_FLAGS, false))
        {
            return error("AcceptableInputs : ConnectInputs failed %s", hash.ToString().c_str());
        }
    }

	//Minimize debug spam
    if (fDebug) {
        printf("mempool: AcceptableInputs : accepted %s (poolsz %lu)\n",
               hash.ToString().substr(0,10).c_str(),
               pool.mapTx.size());
    }
    return true;
}

int GetInputAge(CTxIn& vin, CBlockIndex* pindex)
{
    const uint256& prevHash = vin.prevout.hash;
    CTransaction tx;
    uint256 hashBlock;
    bool fFound = GetTransaction(prevHash, tx, hashBlock);
    if(fFound)
    {
    if(mapBlockIndex.find(hashBlock) != mapBlockIndex.end())
    {
        return pindex->nHeight - mapBlockIndex[hashBlock]->nHeight;
    }
    else
        return 0;
    }
    else
        return 0;
}

unsigned int CTxMemPool::GetTransactionsUpdated() const
{
    LOCK(cs);
    return nTransactionsUpdated;
}

void CTxMemPool::AddTransactionsUpdated(unsigned int n)
{
    LOCK(cs);
    nTransactionsUpdated += n;
}

bool CTxMemPool::addUnchecked(const uint256& hash, CTransaction &tx)
{
    // Add to memory pool without checking anything.  Don't call this directly,
    // call CTxMemPool::accept to properly check the transaction first.
    {
        mapTx[hash] = tx;
        for (unsigned int i = 0; i < tx.vin.size(); i++)
            mapNextTx[tx.vin[i].prevout] = CInPoint(&mapTx[hash], i);
        nTransactionsUpdated++;
    }
    return true;
}


bool CTxMemPool::remove(const CTransaction &tx, bool fRecursive)
{
    // Remove transaction from memory pool
    {
        LOCK(cs);
        uint256 hash = tx.GetHash();
        if (mapTx.count(hash))
        {
            if (fRecursive)
            {
                for (unsigned int i = 0; i < tx.vout.size(); i++)
                {
                    std::map<COutPoint, CInPoint>::iterator it = mapNextTx.find(COutPoint(hash, i));
                    if (it != mapNextTx.end())
                        remove(*it->second.ptx, true);
                };
            };
            for (const CTxIn& txin : tx.vin)
                mapNextTx.erase(txin.prevout);
            mapTx.erase(hash);

            if (tx.nVersion == ANON_TXN_VERSION)
            {
                // -- remove key images
                for (unsigned int i = 0; i < tx.vin.size(); ++i)
                {
                    const CTxIn& txin = tx.vin[i];

                    if (!txin.IsAnonInput())
                        continue;

                    ec_point vchImage;
                    txin.ExtractKeyImage(vchImage);

                    mapKeyImage.erase(vchImage);
                };
            };

            if (tx.IsShielded())
            {
                for (const CShieldedSpendDescription& spend : tx.vShieldedSpend)
                {
                    removeShieldedNullifier(spend.nullifier);
                }
            };

            nTransactionsUpdated++;
        };
    }
    return true;
}

bool CTxMemPool::removeConflicts(const CTransaction &tx)
{
    // Remove transactions which depend on inputs of tx, recursively
    LOCK(cs);
    for (const CTxIn &txin : tx.vin) {
        std::map<COutPoint, CInPoint>::iterator it = mapNextTx.find(txin.prevout);
        if (it != mapNextTx.end()) {
            const CTransaction &txConflict = *it->second.ptx;
            if (txConflict != tx)
                remove(txConflict, true);
        }
    }

    if (tx.IsShielded())
    {
        for (const CShieldedSpendDescription& spend : tx.vShieldedSpend)
        {
            for (std::map<uint256, CTransaction>::iterator mi = mapTx.begin(); mi != mapTx.end(); )
            {
                const CTransaction& txPool = mi->second;
                if (txPool.GetHash() == tx.GetHash()) { ++mi; continue; }
                if (!txPool.IsShielded()) { ++mi; continue; }

                bool fConflict = false;
                for (const CShieldedSpendDescription& poolSpend : txPool.vShieldedSpend)
                {
                    if (poolSpend.nullifier == spend.nullifier)
                    {
                        fConflict = true;
                        break;
                    }
                }
                if (fConflict)
                {
                    CTransaction txToRemove = txPool;
                    ++mi;
                    remove(txToRemove, true);
                }
                else
                {
                    ++mi;
                }
            }
        }
    }

    return true;
}

// IDAG Phase 3: Remove mempool transactions that appear in a DAG sibling block
void CTxMemPool::RemoveDAGConflicts(const uint256& hashBlock)
{
    CBlock block;
    std::map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(hashBlock);
    if (mi == mapBlockIndex.end())
        return;

    if (!block.ReadFromDisk(mi->second))
        return;

    LOCK(cs);

    // Cap setDAGSeenTxids to prevent unbounded memory growth
    static const size_t MAX_DAG_SEEN_TXIDS = 100000;
    if (setDAGSeenTxids.size() > MAX_DAG_SEEN_TXIDS)
        setDAGSeenTxids.clear(); // periodic reset when cap exceeded

    int nRemoved = 0;
    for (const CTransaction& tx : block.vtx)
    {
        uint256 txHash = tx.GetHash();
        setDAGSeenTxids.insert(txHash);

        if (mapTx.count(txHash))
        {
            // Full cleanup: mapTx, mapNextTx, mapShieldedNullifier
            CTransaction txCopy = mapTx[txHash];

            for (const CTxIn& txin : txCopy.vin)
                mapNextTx.erase(txin.prevout);

            // Clean up shielded nullifiers
            for (const CShieldedSpendDescription& spend : txCopy.vShieldedSpend)
                mapShieldedNullifier.erase(spend.nullifier);

            mapTx.erase(txHash);
            nRemoved++;
            ++nTransactionsUpdated;
        }
    }

    if (nRemoved > 0)
        printf("RemoveDAGConflicts: removed %d txs from mempool (sibling block %s)\n",
               nRemoved, hashBlock.ToString().substr(0, 20).c_str());
}

void CTxMemPool::clear()
{
    LOCK(cs);
    mapTx.clear();
    mapNextTx.clear();
    mapKeyImage.clear();
    mapShieldedNullifier.clear();
    setDAGSeenTxids.clear();
    ++nTransactionsUpdated;
}

void CTxMemPool::queryHashes(std::vector<uint256>& vtxid)
{
    vtxid.clear();

    LOCK(cs);
    vtxid.reserve(mapTx.size());
    for (map<uint256, CTransaction>::iterator mi = mapTx.begin(); mi != mapTx.end(); ++mi)
        vtxid.push_back((*mi).first);
}

int CMerkleTx::GetDepthInMainChainINTERNAL(CBlockIndex* &pindexRet) const
{
    if (hashBlock == 0 || nIndex == -1)
        return 0;
    AssertLockHeld(cs_main);

    // Find the block it claims to be in
    map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(hashBlock);
    if (mi == mapBlockIndex.end())
        return 0;
    CBlockIndex* pindex = (*mi).second;
    if (!pindex || !pindex->IsInMainChain())
        return 0;

    // Make sure the merkle branch connects to this block
    if (!fMerkleVerified)
    {
        if (CBlock::CheckMerkleBranch(GetHash(), vMerkleBranch, nIndex) != pindex->hashMerkleRoot)
            return 0;
        fMerkleVerified = true;
    }

    pindexRet = pindex;
    if (!pindexBest)
        return 0;
    return pindexBest->nHeight - pindex->nHeight + 1;
}

int CMerkleTx::GetDepthInMainChain(CBlockIndex* &pindexRet) const
{
    AssertLockHeld(cs_main);
    int nResult = GetDepthInMainChainINTERNAL(pindexRet);
    if (nResult == 0 && !mempool.exists(GetHash()))
        return -1; // Not in chain, not in mempool

    return nResult;
}

int CMerkleTx::GetBlocksToMaturity() const
{
    if (!(IsCoinBase() || IsCoinStake()))
        return 0;
    int nWalletMaturity = fRegTest ? nCoinbaseMaturity : nCoinbaseMaturity + 10;
    return max(0, nWalletMaturity - GetDepthInMainChain());
}


bool CMerkleTx::AcceptToMemoryPool(CTxDB& txdb)
{
    return CTransaction::AcceptToMemoryPool(txdb);
}

bool CMerkleTx::AcceptToMemoryPool()
{
    CTxDB txdb("r");
    return AcceptToMemoryPool(txdb);
}

bool CWalletTx::AcceptWalletTransaction(CTxDB& txdb)
{

    {
        // Add previous supporting transactions first
        for (CMerkleTx& tx : vtxPrev)
        {
            if (!(tx.IsCoinBase() || tx.IsCoinStake()))
            {
                uint256 hash = tx.GetHash();
                if (!mempool.exists(hash) && !txdb.ContainsTx(hash))
                    tx.AcceptToMemoryPool(txdb);
            }
        }
        return AcceptToMemoryPool(txdb);
    }
    return false;
}

bool CWalletTx::AcceptWalletTransaction()
{
    CTxDB txdb("r");
    return AcceptWalletTransaction(txdb);
}

int CTxIndex::GetDepthInMainChain() const
{
    // Read block header
    CBlock block;
    if (!block.ReadFromDisk(pos.nFile, pos.nBlockPos, false))
        return 0;
    // Find the block in the index
    map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(block.GetHash());
    if (mi == mapBlockIndex.end())
        return 0;
    CBlockIndex* pindex = (*mi).second;
    if (!pindex || !pindex->IsInMainChain())
        return 0;
    return 1 + nBestHeight - pindex->nHeight;
}

// Return transaction in tx, and if it was found inside a block, its hash is placed in hashBlock
bool GetTransaction(const uint256 &hash, CTransaction &tx, uint256 &hashBlock, bool s)
{
    {
        if(s)
        {
          LOCK(cs_main);
          {
            if (mempool.lookup(hash, tx))
            {
                return true;
            }
          }
        }
        CTxDB txdb("r");
        CTxIndex txindex;
        if (tx.ReadFromDisk(txdb, COutPoint(hash, 0), txindex))
        {
            CBlock block;
            if (block.ReadFromDisk(txindex.pos.nFile, txindex.pos.nBlockPos, false))
                hashBlock = block.GetHash();
            return true;
        }
    }
    return false;
}

bool GetKeyImage(CTxDB* ptxdb, ec_point& keyImage, CKeyImageSpent& keyImageSpent, bool& fInMempool)
{
    AssertLockHeld(cs_main);


    // -- check txdb first
    fInMempool = false;
    if (ptxdb->ReadKeyImage(keyImage, keyImageSpent))
        return true;

    if (mempool.lookupKeyImage(keyImage, keyImageSpent))
    {
        fInMempool = true;
        return true;
    };

    return false;
};

bool TxnHashInSystem(CTxDB* ptxdb, uint256& txnHash)
{
    // -- is the transaction hash known in the system

    AssertLockHeld(cs_main);

    // TODO: thin mode

    if (mempool.exists(txnHash))
        return true;

    CTxIndex txnIndex;
    if (ptxdb->ReadTxIndex(txnHash, txnIndex))
    {
        if (txnIndex.GetDepthInMainChain() > 0)
            return true;
    };

    return false;
};

//////////////////////////////////////////////////////////////////////////////
//
// CBlock and CBlockIndex
//

// bool ReadBlockFromDisk(CBlock& block, const CDiskBlockPos& pos)
// {
//     block.SetNull();

//     // Open history file to read
//     CAutoFile filein(OpenBlockFile(pos, true), SER_DISK, CLIENT_VERSION);
//     if (filein.IsNull())
//         return error("ReadBlockFromDisk : OpenBlockFile failed");

//     // Read block
//     try {
//         filein >> block;
//     }
//     catch (std::exception &e) {
//         return error("%s : Deserialize or I/O error - %s", __func__, e.what());
//     }

//     // Check the header
//     if (block.IsProofOfWork() && !CheckProofOfWork(block.GetHash(), block.nBits))
//         return error("ReadBlockFromDisk : Errors in block header");

//     return true;
// }

// bool ReadBlockFromDisk(CBlock& block, const CBlockIndex* pindex)
// {
//     if (!ReadBlockFromDisk(block, pindex->nBlockPos))
//         return false;
//     if (block.GetHash() != pindex->GetBlockHash())
//         return error("ReadBlockFromDisk(CBlock&, CBlockIndex*) : GetHash() doesn't match index");
//     return true;
// }

static CBlockIndex* pblockindexFBBHLast;

void ClearFindBlockByHeightCache()
{
    pblockindexFBBHLast = NULL;
}

namespace {

uint64_t gBlockIndexSkipStatCalls = 0;
uint64_t gBlockIndexSkipStatEdges = 0;

static int InvertLowestOne(int n)
{
    return n & (n - 1);
}

static int GetSkipHeight(int height)
{
    if (height < 2)
        return 0;
    return (height & 1)
        ? InvertLowestOne(InvertLowestOne(height - 1)) + 1
        : InvertLowestOne(height);
}

} // namespace

void ResetBlockIndexSkipStats()
{
    gBlockIndexSkipStatCalls = 0;
    gBlockIndexSkipStatEdges = 0;
}

uint64_t GetBlockIndexSkipStatsCalls()
{
    return gBlockIndexSkipStatCalls;
}

uint64_t GetBlockIndexSkipStatsEdges()
{
    return gBlockIndexSkipStatEdges;
}

void CBlockIndex::BuildSkip()
{
    if (pprev)
        pskip = pprev->GetAncestor(GetSkipHeight(nHeight));
    else
        pskip = NULL;
}

const CBlockIndex* CBlockIndex::GetAncestor(int nHeightTarget) const
{
    ++gBlockIndexSkipStatCalls;
    if (nHeightTarget > nHeight || nHeightTarget < 0)
        return NULL;

    const CBlockIndex* pindexWalk = this;
    int nHeightWalk = nHeight;
    while (nHeightWalk > nHeightTarget)
    {
        const int nHeightSkip = GetSkipHeight(nHeightWalk);
        const int nHeightSkipPrev = GetSkipHeight(nHeightWalk - 1);
        if (pindexWalk->pskip &&
            (nHeightSkip == nHeightTarget ||
             (nHeightSkip > nHeightTarget &&
              !(nHeightSkipPrev < nHeightSkip - 2 &&
                nHeightSkipPrev >= nHeightTarget))))
        {
            pindexWalk = pindexWalk->pskip;
            nHeightWalk = nHeightSkip;
        }
        else
        {
            pindexWalk = pindexWalk->pprev;
            --nHeightWalk;
        }
        ++gBlockIndexSkipStatEdges;
    }
    return pindexWalk;
}

CBlockIndex* CBlockIndex::GetAncestor(int nHeightTarget)
{
    return const_cast<CBlockIndex*>(static_cast<const CBlockIndex*>(this)->GetAncestor(nHeightTarget));
}

CBlockIndex* FindBlockByHeight(int nHeight)
{
    CBlockIndex *pblockindex;
    if (nHeight < nBestHeight / 2)
        pblockindex = pindexGenesisBlock;
    else
        pblockindex = pindexBest;
    if (pblockindexFBBHLast && abs(nHeight - pblockindex->nHeight) > abs(nHeight - pblockindexFBBHLast->nHeight))
        pblockindex = pblockindexFBBHLast;
    while (pblockindex->nHeight > nHeight)
        pblockindex = pblockindex->pprev;
    while (pblockindex->nHeight < nHeight)
        pblockindex = pblockindex->pnext;
    pblockindexFBBHLast = pblockindex;
    return pblockindex;
}

bool CBlock::ReadFromDisk(const CBlockIndex* pindex, bool fReadTransactions)
{
    if (!fReadTransactions)
    {
        *this = pindex->GetBlockHeader();
        return true;
    }
    if (!ReadFromDisk(pindex->nFile, pindex->nBlockPos, fReadTransactions))
        return false;
    if (GetHash() != pindex->GetBlockHash())
        return error("CBlock::ReadFromDisk() : GetHash() doesn't match index");
    return true;
}

uint256 static GetOrphanRoot(const CBlock* pblock)
{
    // Work back to the first block in the orphan chain
    while (mapOrphanBlocks.count(pblock->hashPrevBlock))
        pblock = mapOrphanBlocks[pblock->hashPrevBlock];
    return pblock->GetHash();
}

// ppcoin: find block wanted by given orphan block
uint256 WantedByOrphan(const CBlock* pblockOrphan)
{
    // Work back to the first block in the orphan chain
    while (mapOrphanBlocks.count(pblockOrphan->hashPrevBlock))
        pblockOrphan = mapOrphanBlocks[pblockOrphan->hashPrevBlock];
    return pblockOrphan->hashPrevBlock;
}

// Diagnostic-only: decode the serialized block height from the coinbase
// scriptSig (CScript() << nHeight == push_int64).  Returns -1 when it cannot
// be decoded.  Used only by opt-in orphan tracing (fDebug); never affects
// validation, admission, or any return condition.
static int DecodeCoinbaseHeightForTrace(const CBlock& block)
{
    if (block.vtx.empty() || block.vtx[0].vin.empty())
        return -1;
    const std::vector<unsigned char>& scriptSig = block.vtx[0].vin[0].scriptSig;
    if (scriptSig.empty())
        return -1;

    const unsigned int nOp = scriptSig[0];
    unsigned int nSize = 0;
    unsigned int nOffset = 1;
    if (nOp <= OP_PUSHDATA4)
    {
        if (nOp < OP_PUSHDATA1)
        {
            nSize = nOp;
        }
        else if (nOp == OP_PUSHDATA1)
        {
            if (scriptSig.size() < 2) return -1;
            nSize = scriptSig[1];
            nOffset = 2;
        }
        else if (nOp == OP_PUSHDATA2)
        {
            if (scriptSig.size() < 3) return -1;
            nSize = scriptSig[1] | (scriptSig[2] << 8);
            nOffset = 3;
        }
        else
        {
            if (scriptSig.size() < 5) return -1;
            nSize = scriptSig[1] | (scriptSig[2] << 8) |
                    (scriptSig[3] << 16) | (scriptSig[4] << 24);
            nOffset = 5;
        }
    }
    else if (nOp >= OP_1 && nOp <= OP_16)
    {
        return (int)(nOp - OP_1 + 1);
    }
    else
    {
        return -1;
    }

    if (nOffset + nSize > scriptSig.size())
        return -1;
    std::vector<unsigned char> vch(scriptSig.begin() + nOffset,
                                   scriptSig.begin() + nOffset + nSize);
    CScriptNum sn(vch, false);
    return sn.getint();
}

static const char* BlockRequestTraceParentStatusLocked(const uint256& hash)
{
    if (hash == uint256(0))
        return "none";
    std::map<uint256, CBlockIndex*>::const_iterator mi = mapBlockIndex.find(hash);
    if (mi != mapBlockIndex.end())
        return mi->second->IsInMainChain() ? "active" : "indexed";
    if (mapOrphanBlocks.count(hash) != 0)
        return "orphan";
    return "missing";
}

static void BlockRequestTraceUpdateBlockContextLocked(
    const uint256& hash,
    const uint256& parentHash,
    const uint256& orphanChildHash)
{
    if (!BlockRequestTraceEnabled())
        return;
    int nBlockIndexHeight = -1;
    bool fBodyKnown = false;
    std::map<uint256, CBlockIndex*>::const_iterator mi = mapBlockIndex.find(hash);
    if (mi != mapBlockIndex.end())
    {
        nBlockIndexHeight = mi->second->nHeight;
        fBodyKnown = true;
    }
    else if (mapOrphanBlocks.count(hash) != 0)
    {
        fBodyKnown = true;
    }
    BlockRequestTraceSetBlockContext(
        hash, parentHash, BlockRequestTraceParentStatusLocked(parentHash),
        nBestHeight, nBlockIndexHeight, fBodyKnown, orphanChildHash);
}

// Remove a random orphan block (which does not have any dependent orphans).
void PruneOrphanBlocks()
{
    if (mapOrphanBlocksByPrev.size() <= (size_t)std::max((int64_t)0, GetArg("-maxorphanblocks", DEFAULT_MAX_ORPHAN_BLOCKS)))
        return;

    unsigned char randBytes[4];
    unsigned int randVal;
    if (RAND_bytes(randBytes, sizeof(randBytes)) != 1) {
        randVal = GetTime() ^ (unsigned int)mapOrphanBlocksByPrev.size();
    } else {
        randVal = (randBytes[0] << 24) | (randBytes[1] << 16) |
                  (randBytes[2] << 8) | randBytes[3];
    }
    int pos = randVal % mapOrphanBlocksByPrev.size();
    std::multimap<uint256, CBlock*>::iterator it = mapOrphanBlocksByPrev.begin();
    while (pos--) it++;

    // As long as this block has other orphans depending on it, move to one of those successors.
    do {
        std::multimap<uint256, CBlock*>::iterator it2 = mapOrphanBlocksByPrev.find(it->second->GetHash());
        if (it2 == mapOrphanBlocksByPrev.end())
            break;
        it = it2;
    } while(1);

    uint256 hash = it->second->GetHash();
    const bool fIsProofOfStake = it->second->IsProofOfStake();
    const std::pair<COutPoint, unsigned int> stake = it->second->GetProofOfStake();
    delete it->second;
    mapOrphanBlocksByPrev.erase(it);
    mapOrphanBlocks.erase(hash);

    map<uint256, NodeId>::iterator nodeIt = mapOrphanBlocksByNode.find(hash);
    if (nodeIt != mapOrphanBlocksByNode.end()) {
        mapOrphanCountByNode[nodeIt->second]--;
        mapOrphanBlocksByNode.erase(nodeIt);
    }

    // A pruned orphan must release its stake marker, otherwise later
    // re-deliveries of the same block are terminally rejected as duplicate
    // proof-of-stake (PBREJECT_DUPLICATE_STAKE_ORPHAN) and the request budget
    // keeps churning on a block the peer can never re-store.  Only release the
    // kernel when no other stored orphan still references it.
    if (fIsProofOfStake)
        EraseStakeSeenOrphanIfUnreferenced(stake);
}


static bool ShouldLogGetBlocksAbuse(uint64_t nCount)
{
    return nCount <= 4 ||
        (nCount != 0 && (nCount & (nCount - 1)) == 0) ||
        nCount % 128 == 0;
}

static void LogGetBlocksServerEvent(
    const char* pszEvent, const CNode* pfrom,
    const CGetBlocksRequestInfo& request,
    const CGetBlocksResponseInfo* response,
    const CGetBlocksServerDecision& decision)
{
    const CGetBlocksServerState& state = pfrom->getBlocksServer;
    const uint256& hashResponseFirst = response
        ? response->hashFirst
        : state.hashLastResponseFirst;
    const uint256& hashResponseLast = response
        ? response->hashLast
        : state.hashLastResponseLast;
    const unsigned int nResponseCount = response
        ? response->nItemCount
        : state.nLastResponseCount;
    const uint64_t nResponseBytes = response
        ? CGetBlocksServerState::EstimateInvPayloadBytes(response->nItemCount)
        : state.nLastResponseBytes;
    const int64_t nCooldownRemaining = std::max<int64_t>(
        0, state.nRepeatAllowedAfterMillis - request.nRequestTimeMillis);

    printf("%s: request_time_ms=%lld previous_request_time_ms=%lld "
           "peer_id=%d peer=%s subver=%s version=%d inbound=%d "
           "locator_tip=%s resolved_height=%d stop_hash=%s stop_height=%d "
           "predicted_first=%s predicted_last=%s predicted_count=%u "
           "response_first=%s response_last=%s response_count=%u "
           "response_bytes=%llu repeat_count=%u same_locator_count=%llu "
           "identical_requests=%llu same_response_count=%llu "
           "nonprogressing_count=%u nonprogressing_requests=%llu "
           "progress_delta=%d useful_getdata=%llu cooldown_ms=%lld "
           "cooldown_remaining_ms=%lld action=%s requests_received=%llu "
           "responses_allowed=%llu response_bytes_allowed=%llu "
           "responses_suppressed=%llu "
           "rate_limited=%llu estimated_suppressed_bytes=%llu\n",
           pszEvent,
           (long long)request.nRequestTimeMillis,
           (long long)state.nPreviousRequestTimeMillis,
           pfrom->GetId(),
           pfrom->addrName.c_str(),
           pfrom->strSubVer.empty() ? "-" : pfrom->strSubVer.c_str(),
           pfrom->nVersion,
           pfrom->fInbound ? 1 : 0,
           request.hashLocatorTip.ToString().c_str(),
           request.nResolvedHeight,
           request.hashStop.ToString().c_str(),
           request.nStopHeight,
           request.hashPredictedFirst.ToString().c_str(),
           request.hashPredictedLast.ToString().c_str(),
           request.nPredictedResponseCount,
           hashResponseFirst.ToString().c_str(),
           hashResponseLast.ToString().c_str(),
           nResponseCount,
           (unsigned long long)nResponseBytes,
           state.nConsecutiveIdenticalRequests,
           (unsigned long long)state.nSameLocatorRequests,
           (unsigned long long)state.nIdenticalRequests,
           (unsigned long long)state.nSameResponseRequests,
           state.nConsecutiveNonProgressingRequests,
           (unsigned long long)state.nNonProgressingRequests,
           state.nLastProgressDelta,
           (unsigned long long)state.nUsefulGetData,
           (long long)decision.nCooldownMillis,
           (long long)nCooldownRemaining,
           GetBlocksServerActionName(decision.action),
           (unsigned long long)state.nRequestsReceived,
           (unsigned long long)state.nResponsesAllowed,
           (unsigned long long)state.nResponseBytesAllowed,
           (unsigned long long)state.nResponsesSuppressed,
           (unsigned long long)state.nRequestsRateLimited,
           (unsigned long long)state.nEstimatedSuppressedBytes);
}

static bool CheckFinalityVoteRewardOutputs(const CBlock& block, const std::vector<CFinalityVote>& vVotes, int64_t& nFinalityRewardOut)
{
    nFinalityRewardOut = 0;
    if (vVotes.empty())
        return true;
    if (vVotes.size() > FINALITY_MAX_BLOCK_VOTES)
        return error("CheckFinalityVoteRewardOutputs() : too many finality votes in block");
    if (block.vtx.empty())
        return false;

    std::set<uint256> setNullifiers;
    std::vector<bool> vMatched(block.vtx[0].vout.size(), false);

    for (const CFinalityVote& vote : vVotes)
    {
        if (!setNullifiers.insert(vote.nullifier).second)
            return error("CheckFinalityVoteRewardOutputs() : duplicate finality vote nullifier");
        if (vote.IsPrivate())
            continue;
        if (vote.nReward < 0 || !MoneyRange(vote.nReward))
            return error("CheckFinalityVoteRewardOutputs() : finality reward out of range");
        if (nFinalityRewardOut > MAX_MONEY - vote.nReward)
            return error("CheckFinalityVoteRewardOutputs() : finality reward total overflow");

        CPubKey pubkey(vote.vchPubKey);
        if (!pubkey.IsValid())
            return error("CheckFinalityVoteRewardOutputs() : finality vote pubkey invalid");
        CScript rewardScript = GetScriptForDestination(pubkey.GetID());

        bool fFoundReward = (vote.nReward == 0);
        for (unsigned int i = 0; i < block.vtx[0].vout.size(); i++)
        {
            if (vMatched[i])
                continue;
            const CTxOut& out = block.vtx[0].vout[i];
            if (out.nValue == vote.nReward && out.scriptPubKey == rewardScript)
            {
                vMatched[i] = true;
                fFoundReward = true;
                break;
            }
        }
        if (!fFoundReward)
            return error("CheckFinalityVoteRewardOutputs() : missing finality reward output for voter");

        nFinalityRewardOut += vote.nReward;
    }

    return true;
}

static bool CheckFinalityStakeProofsNotSpentInBlock(const CBlock& block, const std::vector<CFinalityVote>& vVotes)
{
    if (vVotes.empty())
        return true;

    std::set<COutPoint> setSpentInBlock;
    for (const CTransaction& tx : block.vtx)
    {
        if (tx.IsCoinBase())
            continue;
        for (const CTxIn& txin : tx.vin)
            setSpentInBlock.insert(txin.prevout);
    }

    for (const CFinalityVote& vote : vVotes)
    {
        if (vote.IsPrivate())
            continue;
        for (const COutPoint& proof : vote.vStakeProof)
        {
            if (setSpentInBlock.count(proof))
                return error("CheckFinalityStakeProofsNotSpentInBlock() : finality stake proof spent in same block");
        }
    }

    return true;
}

static bool LoadFCMPValidationRoot(CTxDB& txdb, int nBlockHeight,
                                   CCurveTreeNode& rootOut,
                                   uint256& hashExpectedRootOut,
                                   std::string& strErrorOut)
{
    CCurveTree curveTree;

    if (nBlockHeight >= FORK_HEIGHT_EPOCH_ROOT_FCMP)
    {
        CEpochState finalizedEpochState;
        if (!g_dagManager.GetLastFinalizedEpochState(finalizedEpochState))
        {
            strErrorOut = "missing finalized epoch FCMP root";
            return false;
        }
        if (!txdb.ReadCurveTreeAtEpoch(finalizedEpochState.nEpoch, curveTree))
        {
            strErrorOut = "missing finalized epoch curve-tree snapshot";
            return false;
        }
        if (!curveTree.IsEmpty())
            curveTree.RebuildParentNodes();
        hashExpectedRootOut = curveTree.GetRoot();
        if (hashExpectedRootOut == 0 || hashExpectedRootOut != finalizedEpochState.hashCurveRoot)
        {
            strErrorOut = "finalized epoch curve-tree root mismatch";
            return false;
        }
    }
    else
    {
        if (!txdb.ReadCurveTree(curveTree))
        {
            strErrorOut = "failed to read mutable curve tree";
            return false;
        }
        if (!curveTree.IsEmpty())
            curveTree.RebuildParentNodes();
        hashExpectedRootOut = curveTree.GetRoot();
    }

    if (curveTree.IsEmpty() || hashExpectedRootOut == 0)
    {
        strErrorOut = "empty curve tree";
        return false;
    }

    rootOut = curveTree.GetRootNode();
    return true;
}

bool CheckFCMPSpendRoots(const CTransaction& tx, int nBlockHeight,
                         const uint256& hashExpectedRoot,
                         std::string& strErrorOut)
{
    if (nBlockHeight < FORK_HEIGHT_EPOCH_ROOT_FCMP)
        return true;

    for (size_t i = 0; i < tx.vShieldedSpend.size(); i++)
    {
        const CShieldedSpendDescription& spend = tx.vShieldedSpend[i];
        if (spend.curveTreeRoot != hashExpectedRoot)
        {
            strErrorOut = strprintf("shielded spend %u FCMP root %s does not match finalized epoch root %s",
                                    (unsigned)i,
                                    spend.curveTreeRoot.ToString().substr(0,10).c_str(),
                                    hashExpectedRoot.ToString().substr(0,10).c_str());
            return false;
        }
    }

    return true;
}

// Proof of Work miner's coin base reward
int64_t GetProofOfWorkReward(int nHeight, int64_t nFees)
{
  int64_t nSubsidy = 1 * COIN;

  // use nHeight parameter instead of pindexBest->nHeight
  // to correctly compute reward during validation of non-tip blocks
  if (fRegTest) {
       // Regtest: simple flat reward for easy testing (similar to Bitcoin regtest)
       if (nHeight == 0)
           nSubsidy = 0;  // Genesis block has no spendable reward
       else
           nSubsidy = 50 * COIN;  // 50 INN per block

       if (fDebug && GetBoolArg("-printcreation"))
           printf("GetProofOfWorkReward() : create=%s nSubsidy=%" PRId64"\n", FormatMoney(nSubsidy).c_str(), nSubsidy);

       return nSubsidy + nFees;
  } else if (fTestNet) {
       if (nHeight == 1)
           nSubsidy = 1000000 * COIN;  // 10m INN Premine for Testnet for testing
       else if (nHeight <= FAIR_LAUNCH_BLOCK) // Block 490, Instamine prevention
           nSubsidy = 1 * COIN/2;
       else if (nHeight <= 5000)
           nSubsidy = 10 * COIN;
       else if (nHeight > 5000) // Block 5000
           nSubsidy = 0;
     else if (nHeight > 10000)
          nSubsidy = 10000; // PoW Reward 0.0001

       if (fDebug && GetBoolArg("-printcreation"))
           printf("GetProofOfWorkReward() : create=%s nSubsidy=%" PRId64"\n", FormatMoney(nSubsidy).c_str(), nSubsidy);

       return nSubsidy + nFees;
   } else {
  // use nHeight parameter throughout (was pindexBest->nHeight)
  if (nHeight == 1)
  		nSubsidy = 10350000 * COIN;  //Swap amount for Innova Chain v0.12 + Founders Fund 2.25 million
  	else if (nHeight <= FAIR_LAUNCH_BLOCK) // Block 490, Instamine prevention
      nSubsidy = 0.165 * COIN/2;
  	else if (nHeight <= 5000)
  		nSubsidy = 0.33 * COIN;
    else if (nHeight <= 10000)
    	nSubsidy = 0.66 * COIN;
    else if (nHeight <= 15000)
      nSubsidy = 0.99 * COIN;
    else if (nHeight <= 20000)
    	nSubsidy = 1.32 * COIN;
    else if (nHeight <= 25000)
      nSubsidy = 1.65 * COIN;
  	else if (nHeight <= 27500)
  		nSubsidy = 1.485 * COIN;
    else if (nHeight <= 30000)
    	nSubsidy = 1.32 * COIN;
    else if (nHeight <= 32500)
      nSubsidy = 1.155 * COIN;
    else if (nHeight <= 35000)
      nSubsidy = 0.99 * COIN;
    else if (nHeight <= 37500)
      nSubsidy = 0.825 * COIN;
  	else if (nHeight <= 40000)
  		nSubsidy = 0.66 * COIN;
    else if (nHeight <= 42500)
    	nSubsidy = 0.495 * COIN;
    else if (nHeight <= 45000)
    	nSubsidy = 0.33 * COIN;
    else if (nHeight <= 47500)
    	nSubsidy = 0.165 * COIN;
    else if (nHeight <= 50000)
      nSubsidy = 0.0825 * COIN;
    else if (nHeight > ZERO_POW_BLOCK && nHeight < 2000000)
      nSubsidy = 0 * COIN;
    else if (nHeight > 2000000 && nHeight <= 2080000) // Hard Fork roll back - Innova Foundation Fund hack
      nSubsidy = 1 * COIN;
    else if (nHeight <= 2150000)
      nSubsidy = 0.5 * COIN;
    else if (nHeight <= 2400000)
      nSubsidy = 0.1 * COIN;
    else if (nHeight <= 2700000) // New PoW Structure restarts here
      nSubsidy = 0.0001 * COIN;
    else if (nHeight <= 2750000) // 0.15 Coin PoW Reward to release 7,500 INN in 50,000 blocks
      nSubsidy = 0.15 * COIN;
    else if (nHeight <= 3000000) // 0.2 Coin PoW Reward to release 50,000 INN in 250,000 blocks
      nSubsidy = 0.2 * COIN;
    else if (nHeight <= 3250000) // 0.25 Coin PoW Reward to release 62,500 INN in 250,000 blocks
      nSubsidy = 0.25 * COIN;
    else if (nHeight <= 3500000) // 0.5 Coin PoW Reward to release 125,000 INN in 250,000 blocks
      nSubsidy = 0.5 * COIN;
    else if (nHeight <= 3750000) // 0.75 Coin PoW Reward to release 187,500 INN in 250,000 blocks
      nSubsidy = 0.75 * COIN;
    else if (nHeight <= 4000000) // 0.5 Coin PoW Reward to release 125,000 INN in 250,000 blocks
      nSubsidy = 0.5 * COIN;
    else if (nHeight <= 4025000) // 1 Coin PoW Reward for peak payout in new cycle to release 1 CollateralNode in 25,000 blocks!!
      nSubsidy = 1 * COIN;
    else if (nHeight <= 4250000) // 0.5 Coin PoW Reward to release 112,500 INN in 225,000 blocks
      nSubsidy = 0.5 * COIN;
    else if (nHeight <= 4500000) // 0.25 Coin PoW Reward to release 62,500 INN in 250,000 blocks
      nSubsidy = 0.25 * COIN;
    else if (nHeight <= 4750000) // 0.2 Coin PoW Reward to release 50,000 INN in 250,000 blocks
      nSubsidy = 0.2 * COIN;
    else if (nHeight <= 5000000) // 0.15 Coin PoW Reward to release 37,500 INN in 250,000 blocks
      nSubsidy = 0.15 * COIN;
    else if (nHeight <= 5250000) // 0.1 Coin PoW Reward to release 1 CollateralNode in 250,000 blocks
      nSubsidy = 0.1 * COIN;
    else if (nHeight <= 5500000) // 0.05 Coin PoW Reward to release 12,500 INN in 250,000 blocks
      nSubsidy = 0.05 * COIN;
    else if (nHeight <= 5750000) // 0.01 Coin PoW Reward to release 2,500 INN in 250,000 blocks
      nSubsidy = 0.01 * COIN;
    else if (nHeight <= 6000000) // 0.1 Coin PoW Reward for peak payout in new cycle to release 1 Collateral Node in 250,000 blocks
      nSubsidy = 0.1 * COIN;
    else if (nHeight <= 6250000) // 0.15 Coin PoW Reward to release 37,500 INN in 250,000 blocks
      nSubsidy = 0.15 * COIN;
    else if (nHeight <= 6500000) // 0.2 Coin PoW Reward to release 50,000 INN in 250,000 blocks
      nSubsidy = 0.2 * COIN;
    else if (nHeight <= 6750000) // 0.25 Coin PoW Reward to release 62,500 INN in 250,000 blocks
      nSubsidy = 0.25 * COIN;
    else if (nHeight <= 7000000) // 0.5 Coin PoW Reward to release 125,000 INN in 250,000 blocks
      nSubsidy = 0.5 * COIN;
    else if (nHeight <= 7250000) // 0.75 Coin PoW Reward to release 187,500 INN in 250,000 blocks
      nSubsidy = 0.75 * COIN;
    else if (nHeight <= 7500000) // 0.5 Coin PoW Reward to release 125,000 INN in 250,000 blocks
      nSubsidy = 0.5 * COIN;
    else if (nHeight <= 7525000) // 1 Coin PoW Reward for peak payout in new cycle to release 1 CollateralNode in 25,000 blocks!!
      nSubsidy = 1 * COIN;
    else if (nHeight <= 7750000) // 0.5 Coin PoW Reward to release 112,500 INN in 225,000 blocks
      nSubsidy = 0.5 * COIN;
    else if (nHeight <= 8000000) // 0.25 Coin PoW Reward to release 62,500 INN in 250,000 blocks
      nSubsidy = 0.25 * COIN;
    else if (nHeight <= 8250000) // 0.2 Coin PoW Reward to release 50,000 INN in 250,000 blocks
      nSubsidy = 0.2 * COIN;
    else if (nHeight <= 8500000) // 0.15 Coin PoW Reward to release 37,500 INN in 250,000 blocks
      nSubsidy = 0.15 * COIN;
    else if (nHeight <= 8750000) // 0.1 Coin PoW Reward to release 1 CollateralNode in 250,000 blocks
      nSubsidy = 0.1 * COIN;
    else if (nHeight <= 9000000) // 0.05 Coin PoW Reward to release 12,500 INN in 250,000 blocks
      nSubsidy = 0.05 * COIN;
    else if (nHeight <= 9250000) // 0.01 Coin PoW Reward to release 2,500 INN in 250,000 blocks
      nSubsidy = 0.01 * COIN;
    else if (nHeight <= 9500000) // 0.05 Coin PoW Reward to release 12,500 INN in 250,000 blocks
      nSubsidy = 0.05 * COIN;
    else if (nHeight <= 9750000) // 0.1 Coin PoW Reward to release 1 CollateralNode in 250,000 blocks
      nSubsidy = 0.1 * COIN;
    else if (nHeight <= 10000000) // 0.2 Coin PoW Reward to release 50,000 INN in 250,000 blocks
      nSubsidy = 0.2 * COIN;
    else if (nHeight >= 10000000) // 0.0001 Coin PoW Reward to release ~200 INN per year
      nSubsidy = 0.0001 * COIN; // Final PoW Reward 0.0001 INN @ block 10 mln

    if (fDebug && GetBoolArg("-printcreation"))
      printf("GetProofOfWorkReward() : create=%s nSubsidy=%" PRId64"\n", FormatMoney(nSubsidy).c_str(), nSubsidy);

      return nSubsidy + nFees;
    }
}

const int YEARLY_BLOCKCOUNT = 2103792; // Amount of Blocks per year

// Proof of Stake miner's coin stake reward based on coin age spent (coin-days)
int64_t GetProofOfStakeReward(int64_t nCoinAge, int64_t nFees)
{
    // CON-AUDIT-2: Guard pindexBest NULL, use nBestHeight for reward calculation
    int nHeight = 0;
    {
        LOCK(cs_main);
        if (pindexBest)
            nHeight = pindexBest->nHeight;
    }
    if (nHeight > (YEARLY_BLOCKCOUNT*9000)) // It's Over 9000!! [years] - Vegeta
        return nFees;

    int64_t nRewardCoinYear;
    nRewardCoinYear = COIN_YEAR_REWARD; // 0.06 6%

    int64_t nSubsidy;
    nSubsidy = nCoinAge / 365 * nRewardCoinYear + nCoinAge % 365 * nRewardCoinYear / 365;

    if (fDebug && GetBoolArg("-printcreation"))
        printf("GetProofOfStakeReward(): create=%s nCoinAge=%" PRId64"\n", FormatMoney(nSubsidy).c_str(), nCoinAge);

    return nSubsidy + nFees;
}

static const int64_t nTargetTimespan = 30;

//
// maximum nBits value could possible be required nTime after
//
unsigned int ComputeMaxBits(CBigNum bnTargetLimit, unsigned int nBase, int64_t nTime)
{
    CBigNum bnResult;
    bnResult.SetCompact(nBase);
    bnResult *= 2;
    while (nTime > 0 && bnResult < bnTargetLimit)
    {
        // Maximum 200% adjustment per day...
        bnResult *= 2;
        nTime -= 24 * 60 * 60;
    }
    if (bnResult > bnTargetLimit)
        bnResult = bnTargetLimit;
    return bnResult.GetCompact();
}

//
// minimum amount of work that could possibly be required nTime after
// minimum proof-of-work required was nBase
//
unsigned int ComputeMinWork(unsigned int nBase, int64_t nTime)
{
    return ComputeMaxBits(bnProofOfWorkLimit, nBase, nTime);
}

//
// minimum amount of stake that could possibly be required nTime after
// minimum proof-of-stake required was nBase
//
unsigned int ComputeMinStake(unsigned int nBase, int64_t nTime, unsigned int nBlockTime)
{
    return ComputeMaxBits(bnProofOfStakeLimit, nBase, nTime);
}


// ppcoin: find last block index up to pindex
const CBlockIndex* GetLastBlockIndex(const CBlockIndex* pindex, bool fProofOfStake)
{
    while (pindex && pindex->pprev && (pindex->IsProofOfStake() != fProofOfStake))
        pindex = pindex->pprev;
    return pindex;
}

unsigned int GetNextTargetRequired(const CBlockIndex* pindexLast, bool fProofOfStake)
{
    CBigNum bnTargetLimit = fProofOfStake ? bnProofOfStakeLimit : bnProofOfWorkLimit;

    if (pindexLast == NULL)
        return bnTargetLimit.GetCompact(); // genesis block

    const CBlockIndex* pindexPrev = GetLastBlockIndex(pindexLast, fProofOfStake);
    if (pindexPrev->pprev == NULL)
        return bnTargetLimit.GetCompact(); // first block
    const CBlockIndex* pindexPrevPrev = GetLastBlockIndex(pindexPrev->pprev, fProofOfStake);
    if (pindexPrevPrev->pprev == NULL)
        return bnTargetLimit.GetCompact(); // second block

    int64_t nActualSpacing = pindexPrev->GetBlockTime() - pindexPrevPrev->GetBlockTime();

    // Clamp nActualSpacing (tighter bounds post-fork, negative-only pre-fork)
    int nNextHeight = pindexLast->nHeight + 1;
    unsigned int nEffectiveSpacing = GetTargetSpacingForHeight(nNextHeight);

    if (nNextHeight < FORK_HEIGHT_TIGHTER_DRIFT)
    {
        if (nActualSpacing < 0)
            nActualSpacing = nEffectiveSpacing;
    }
    else
    {
        int nClampFactor = (nNextHeight >= FORK_HEIGHT_TIGHTER_DRIFT) ? 4 : 10;
        int64_t nMinSpacing = (int64_t)nEffectiveSpacing / nClampFactor;
        if (nMinSpacing < 1) nMinSpacing = 1;
        int64_t nMaxSpacing = (int64_t)nEffectiveSpacing * nClampFactor;

        if (nActualSpacing < nMinSpacing)
        {
            if (nActualSpacing < 0)
                printf("WARNING: GetNextTargetRequired() : negative actual spacing %" PRId64 " (clamping to %" PRId64 ")\n",
                       nActualSpacing, nMinSpacing);
            nActualSpacing = nMinSpacing;
        }
        if (nActualSpacing > nMaxSpacing)
            nActualSpacing = nMaxSpacing;
    }

    CBigNum bnNew;
    bnNew.SetCompact(pindexPrev->nBits);
    int64_t nSmoothTimespan = (nNextHeight >= FORK_HEIGHT_TIGHTER_DRIFT) ? 180 : nTargetTimespan;
    int64_t nInterval = nSmoothTimespan / nEffectiveSpacing;
    bnNew *= ((nInterval - 1) * nEffectiveSpacing + nActualSpacing + nActualSpacing);
    bnNew /= ((nInterval + 1) * nEffectiveSpacing);

    if (bnNew <= 0 || bnNew > bnTargetLimit)
        bnNew = bnTargetLimit;

    return bnNew.GetCompact();
}

bool CheckProofOfWork(uint256 hash, unsigned int nBits)
{
    CBigNum bnTarget;
    bnTarget.SetCompact(nBits);

    // Check range
    if (bnTarget <= 0 || bnTarget > bnProofOfWorkLimit)
        return error("CheckProofOfWork() : nBits below minimum work");

    // Check proof of work matches claimed amount
    if (hash > bnTarget.getuint256())
        return error("CheckProofOfWork() : hash doesn't match nBits");

    return true;
}

// Return maximum amount of blocks that other nodes claim to have
int GetNumBlocksOfPeers()
{
    int nPeerHeight = -1;
    int64_t nNow = GetTime();
    TRY_LOCK(cs_vNodes, lockNodes);
    if (lockNodes)
    {
        for (CNode* pnode : vNodes)
        {
            if (!pnode || pnode->fClient)
                continue;
            if (pnode->nBestKnownHeight > 0 && pnode->nLastHeightUpdate > 0 && nNow - pnode->nLastHeightUpdate <= 120)
                nPeerHeight = std::max(nPeerHeight, pnode->nBestKnownHeight);
        }
    }
    if (nPeerHeight >= 0)
        return std::max(nPeerHeight, Checkpoints::GetTotalBlocksEstimate());
    return std::max(cPeerBlockCounts.median(), Checkpoints::GetTotalBlocksEstimate());
}

bool IsSynchronized() {
  static bool rc = false;
  if(rc == false) rc = !IsInitialBlockDownload();
  return rc;
}

bool g_forceIbdPeerSnapshotUnavailableForTesting = false;

bool IsInitialBlockDownload()
{
    int nFreshPeerHeight = -1;
    int nKnownPeerHeight = -1;
    int64_t nFreshPeerLastBlockRecv = 0;
    int64_t nLocalLastBlockRecv = 0;
    const auto RecordAndReturn = [&](bool fInitialBlockDownload,
                                     int nIBDReason) -> bool {
        ibdmetrics::RecordIBDState(fInitialBlockDownload);
        ibdactivepath::RecordIBDStateTrace(
            fInitialBlockDownload, nIBDReason, nBestHeight,
            nFreshPeerHeight, GetTime(),
            nFreshPeerLastBlockRecv, nLocalLastBlockRecv);
        return fInitialBlockDownload;
    };
    if (fRegTest && !fRegTestIbd && pindexBest != NULL)
        return RecordAndReturn(false, ibdactivepath::IBD_REASON_REGTEST);
    if (fImporting || fReindex || pindexBest == NULL)
        return RecordAndReturn(true,
                                ibdactivepath::IBD_REASON_IMPORT_REINDEX_NULL);

    if (nBestHeight < Checkpoints::GetTotalBlocksEstimate())
        return RecordAndReturn(true,
                                ibdactivepath::IBD_REASON_BELOW_ESTIMATE);

    int64_t nNow = GetTime();
    bool fActiveCatchup = false;
    bool fPeerSnapshotAvailable = false;
    {
        TRY_LOCK(cs_vNodes, lockNodes);
        fPeerSnapshotAvailable = lockNodes && !g_forceIbdPeerSnapshotUnavailableForTesting;
        if (fPeerSnapshotAvailable)
        {
            for (CNode* pnode : vNodes)
            {
                if (!pnode || pnode->fClient)
                    continue;

                nKnownPeerHeight = std::max(
                    nKnownPeerHeight,
                    std::max(pnode->nBestKnownHeight, pnode->nChainHeight));
                bool fFreshHeight = pnode->nBestKnownHeight > 0 &&
                                    pnode->nLastHeightUpdate > 0 &&
                                    nNow - pnode->nLastHeightUpdate <= 120;
                if (fFreshHeight)
                    nFreshPeerHeight = std::max(nFreshPeerHeight, pnode->nBestKnownHeight);

                if (pnode->nLastBlockRecv > nFreshPeerLastBlockRecv)
                    nFreshPeerLastBlockRecv = pnode->nLastBlockRecv;

                bool fPeerHasWork = fFreshHeight && pnode->nBestKnownHeight > nBestHeight + 2;
                if (fPeerHasWork && (!pnode->setBlocksInFlight.empty() || !pnode->mapAskFor.empty()))
                    fActiveCatchup = true;
            }
        }
    }
    if (!fPeerSnapshotAvailable)
        return RecordAndReturn(true,
            ibdactivepath::IBD_REASON_PEER_SNAPSHOT_UNAVAILABLE);


    unsigned int nTargetSpacing = GetTargetSpacingForHeight(nBestHeight + 1);
    int nLagTolerance = (nTargetSpacing <= 1) ? (int)GetArg("-ibdheightlag", 8) : nCoinbaseMaturity * 2;
    if (std::max(nFreshPeerHeight, nKnownPeerHeight) >
        nBestHeight + nLagTolerance)
        return RecordAndReturn(true, ibdactivepath::IBD_REASON_PEER_AHEAD_LAG);

    if (fActiveCatchup)
        return RecordAndReturn(true, ibdactivepath::IBD_REASON_ACTIVE_CATCHUP);

    nLocalLastBlockRecv = nTimeBestReceived > 0 ? nTimeBestReceived : pindexBest->GetBlockTime();
    int64_t nStaleSeconds = (nTargetSpacing <= 1) ? GetArg("-ibdstaleseconds", 30) : 300;
    if (nFreshPeerHeight > nBestHeight &&
        nFreshPeerLastBlockRecv > nLocalLastBlockRecv + 2 &&
        nNow - nLocalLastBlockRecv > nStaleSeconds)
        return RecordAndReturn(true, ibdactivepath::IBD_REASON_STALE_RECV);

    return RecordAndReturn(false, ibdactivepath::IBD_REASON_NOT_IBD);

}

void static InvalidChainFound(CBlockIndex* pindexNew)
{
    if (pindexNew->nChainTrust > nBestInvalidTrust)
    {
        nBestInvalidTrust = pindexNew->nChainTrust;
        CTxDB().WriteBestInvalidTrust(CBigNum(nBestInvalidTrust));
        if (pindexBest)
        {
            static int64_t nLastInvalidChainNotify = 0;
            int64_t nNow = GetTime();
            if (nNow - nLastInvalidChainNotify >= 5)
            {
                nLastInvalidChainNotify = nNow;
                uiInterface.NotifyBlocksChanged(pindexBest->nHeight, GetNumBlocksOfPeers());
            }
        }
    }

    uint256 nBestInvalidBlockTrust = (pindexNew->nHeight != 0 && pindexNew->pprev != NULL) ? (pindexNew->nChainTrust - pindexNew->pprev->nChainTrust) : pindexNew->nChainTrust;

    printf("InvalidChainFound: invalid block=%s  height=%d  trust=%s  blocktrust=%" PRId64"  date=%s\n",
      pindexNew->GetBlockHash().ToString().substr(0,20).c_str(), pindexNew->nHeight,
      CBigNum(pindexNew->nChainTrust).ToString().c_str(), nBestInvalidBlockTrust.Get64(),
      DateTimeStrFormat("%x %H:%M:%S", pindexNew->GetBlockTime()).c_str());
    if (pindexBest)
    {
        uint256 nBestBlockTrust = (pindexBest->nHeight != 0 && pindexBest->pprev != NULL) ? (pindexBest->nChainTrust - pindexBest->pprev->nChainTrust) : pindexBest->nChainTrust;
        printf("InvalidChainFound:  current best=%s  height=%d  trust=%s  blocktrust=%" PRId64"  date=%s\n",
          hashBestChain.ToString().substr(0,20).c_str(), nBestHeight,
          CBigNum(pindexBest->nChainTrust).ToString().c_str(),
          nBestBlockTrust.Get64(),
          DateTimeStrFormat("%x %H:%M:%S", pindexBest->GetBlockTime()).c_str());
    }
}


void CBlock::UpdateTime(const CBlockIndex* pindexPrev)
{
    nTime = max(GetBlockTime(), GetAdjustedTime());
}





// Requires cs_main.
void Misbehaving(NodeId pnode, int howmuch)
{
    if (howmuch == 0)
        return;

    LOCK(cs_vNodes);
    for (CNode* pn : vNodes)
    {
        if(pn->GetId() == pnode)
        {
            LOCK(pn->cs_nMisbehavior);
            pn->nMisbehavior += howmuch;
            int banscore = GetArg("-banscore", 100);
            if (pn->nMisbehavior >= banscore)
            {
                printf("Misbehaving: %s (%d -> %d) BAN THRESHOLD EXCEEDED\n", pn->addrName.c_str(), pn->nMisbehavior-howmuch, pn->nMisbehavior);
                pn->fDisconnect = true;
            }
            else
                printf("Misbehaving: %s (%d -> %d)\n", pn->addrName.c_str(), pn->nMisbehavior-howmuch, pn->nMisbehavior);

            break;
        }
    }
}





bool CTransaction::DisconnectInputs(CTxDB& txdb)
{
    //hooks->DisconnectInputs(*this); //Disconnect Name DB Inputs

    // Relinquish previous transactions' spent pointers
    if (!IsCoinBase())
    {
        for (const CTxIn& txin : vin)
        {
            COutPoint prevout = txin.prevout;

            // Get prev txindex from disk
            CTxIndex txindex;
            if (!txdb.ReadTxIndex(prevout.hash, txindex))
                return error("DisconnectInputs() : ReadTxIndex failed");

            if (prevout.n >= txindex.vSpent.size())
                return error("DisconnectInputs() : prevout.n out of range");

            // Mark outpoint as not spent
            txindex.vSpent[prevout.n].SetNull();

            // Write back
            if (!txdb.UpdateTxIndex(prevout.hash, txindex))
                return error("DisconnectInputs() : UpdateTxIndex failed");
        }
    }

    // Remove transaction from index
    // This can fail if a duplicate of this transaction was in a chain that got
    // reorganized away. This is only possible if this transaction was completely
    // spent, so erasing it would be a no-op anyway.
    txdb.EraseTxIndex(*this);

    return true;
}


bool CTransaction::FetchInputs(CTxDB& txdb, const map<uint256, CTxIndex>& mapTestPool,
                               bool fBlock, bool fMiner, MapPrevTx& inputsRet, bool& fInvalid)
{
    // FetchInputs can return false either because we just haven't seen some inputs
    // (in which case the transaction should be stored as an orphan)
    // or because the transaction is malformed (in which case the transaction should
    // be dropped).  If tx is definitely invalid, fInvalid will be set to true.
    fInvalid = false;

    if (IsCoinBase())
        return true; // Coinbase transactions have no inputs to fetch.

    for (unsigned int i = 0; i < vin.size(); i++)
    {
        if (nVersion == ANON_TXN_VERSION
            && vin[i].IsAnonInput())
            continue;

        COutPoint prevout = vin[i].prevout;
        if (inputsRet.count(prevout.hash))
            continue; // Got it already

        // Read txindex
        CTxIndex& txindex = inputsRet[prevout.hash].first;
        bool fFound = true;
        if ((fBlock || fMiner) && mapTestPool.count(prevout.hash))
        {
            // Get txindex from current proposed changes
            txindex = mapTestPool.find(prevout.hash)->second;
        }
        else
        {
            // Read txindex from txdb
            fFound = txdb.ReadTxIndex(prevout.hash, txindex);
        }
        if (!fFound && (fBlock || fMiner))
            return fMiner ? false : error("FetchInputs() : %s prev tx %s index entry not found", GetHash().ToString().substr(0,10).c_str(),  prevout.hash.ToString().substr(0,10).c_str());

        // Read txPrev
        CTransaction& txPrev = inputsRet[prevout.hash].second;
        if (!fFound || txindex.pos == CDiskTxPos(1,1,1))
        {
            // Get prev tx from single transactions in memory
            if (!mempool.lookup(prevout.hash, txPrev))
                return error("FetchInputs() : %s mempool Tx prev not found %s", GetHash().ToString().substr(0,10).c_str(),  prevout.hash.ToString().substr(0,10).c_str());
            if (!fFound)
                txindex.vSpent.resize(txPrev.vout.size());
        }
        else
        {
            // Get prev tx from disk
            if (!txPrev.ReadFromDisk(txindex.pos))
                return error("FetchInputs() : %s ReadFromDisk prev tx %s failed", GetHash().ToString().substr(0,10).c_str(),  prevout.hash.ToString().substr(0,10).c_str());
        }
    }

    // Make sure all prevout.n indexes are valid:
    for (unsigned int i = 0; i < vin.size(); i++)
    {
        if (nVersion == ANON_TXN_VERSION
            && vin[i].IsAnonInput())
            continue;

        const COutPoint prevout = vin[i].prevout;
        if (inputsRet.count(prevout.hash) == 0)
            return DoS(100, error("ConnectInputs() : missing input %s", prevout.hash.ToString().substr(0,10).c_str()));
        const CTxIndex& txindex = inputsRet[prevout.hash].first;
        const CTransaction& txPrev = inputsRet[prevout.hash].second;
        if (prevout.n >= txPrev.vout.size() || prevout.n >= txindex.vSpent.size())
        {
            // Revisit this if/when transaction replacement is implemented and allows
            // adding inputs:
            fInvalid = true;
            if (fDebugNet)
                return DoS(100, error("FetchInputs() : %s prevout.n out of range %d %" PRIszu" %" PRIszu" prev tx %s\n%s", GetHash().ToString().substr(0,10).c_str(), prevout.n, txPrev.vout.size(), txindex.vSpent.size(), prevout.hash.ToString().substr(0,10).c_str(), txPrev.ToString().c_str()));
            return DoS(100, false);

        }
    }

    return true;
}

// Ring Signatures - I n n o v a
static bool CheckAnonInputAB(CTxDB &txdb, const CTxIn &txin, int i, int nRingSize, std::vector<uint8_t> &vchImage, uint256 &preimage, int64_t &nCoinValue)
{
    const CScript &s = txin.scriptSig;

    CPubKey pkRingCoin;
    CAnonOutput ao;
    CTxIndex txindex;

    ec_point pSigC;
    pSigC.resize(ec_secret_size);
    memcpy(&pSigC[0], &s[2], ec_secret_size);
    const unsigned char *pSigS    = &s[2 + ec_secret_size];
    const unsigned char *pPubkeys = &s[2 + ec_secret_size + ec_secret_size * nRingSize];
    for (int ri = 0; ri < nRingSize; ++ri)
    {
        pkRingCoin = CPubKey(&pPubkeys[ri * ec_compressed_size], ec_compressed_size);
        if (!txdb.ReadAnonOutput(pkRingCoin, ao))
        {
            printf("CheckAnonInputsAB(): Error input %d, element %d AnonOutput %s not found.\n", i, ri);
            return false;
        };

        if (nCoinValue == -1)
        {
            nCoinValue = ao.nValue;
        } else
        if (nCoinValue != ao.nValue)
        {
            printf("CheckAnonInputsAB(): Error input %d, element %d ring amount mismatch %d, %d.\n", i, ri, nCoinValue, ao.nValue);
            return false;
        };

        if (ao.nBlockHeight == 0
            || nBestHeight - ao.nBlockHeight < MIN_ANON_SPEND_DEPTH)
        {
            printf("CheckAnonInputsAB(): Error input %d, element %d depth < MIN_ANON_SPEND_DEPTH.\n", i, ri);
            return false;
        };
    };

    if (verifyRingSignatureAB(vchImage, preimage, nRingSize, pPubkeys, pSigC, pSigS) != 0)
    {
        printf("CheckAnonInputsAB(): Error input %d verifyRingSignatureAB() failed.\n", i);
        return false;
    };

    return true;
};

bool CTransaction::CheckAnonInputs(CTxDB& txdb, int64_t& nSumValue, bool& fInvalid, bool fCheckExists)
{
    AssertLockHeld(cs_main);
    // - fCheckExists should only run for anonInputs entering this node

    fInvalid = false;

    nSumValue = 0;

    uint256 preimage;
    if (pwalletMain->GetTxnPreImage(*this, preimage) != 0)
    {
        printf("CheckAnonInputs(): Error GetTxnPreImage() failed.\n");
        fInvalid = true; return false;
    };

    uint256 txnHash = GetHash();

    for (uint32_t i = 0; i < vin.size(); i++)
    {
        const CTxIn &txin = vin[i];

        if (!txin.IsAnonInput())
            continue;

        const CScript &s = txin.scriptSig;

        std::vector<uint8_t> vchImage;
        txin.ExtractKeyImage(vchImage);

        CKeyImageSpent spentKeyImage;
        bool fInMemPool;
        if (GetKeyImage(&txdb, vchImage, spentKeyImage, fInMemPool))
        {
            // -- this can happen for transactions created by the local node
            if (spentKeyImage.txnHash == txnHash)
            {
                if (fDebugRingSig)
                    printf("Input %d keyimage %s matches txn %s.\n", i, HexStr(vchImage).c_str(), txnHash.ToString().c_str());
            } else
            {
                if (fCheckExists
                    && !TxnHashInSystem(&txdb, spentKeyImage.txnHash))
                {
                    printf("CheckAnonInputs(): Warning input %d keyimage %s spent by unknown txn %s - rejecting for safety.\n",
                           i, HexStr(vchImage).c_str(), spentKeyImage.txnHash.ToString().c_str());
                    fInvalid = true; return false;
                } else
                {
                    printf("CheckAnonInputs(): Error input %d keyimage %s already spent.\n", i, HexStr(vchImage).c_str());
                    fInvalid = true; return false;
                };
            };
        };

        int64_t nCoinValue = -1;
        int nRingSize = txin.ExtractRingSize();
        if (nRingSize < (int)MIN_RING_SIZE
          ||nRingSize > (pindexBest->nHeight ? (int)MAX_RING_SIZE : (int)MAX_RING_SIZE_OLD))
        {
            printf("CheckAnonInputs(): Error input %d ringsize %d not in range [%d, %d].\n", i, nRingSize, MIN_RING_SIZE, MAX_RING_SIZE);
            fInvalid = true; return false;
        };


        if (nRingSize > 1 && s.size() == 2 + ec_secret_size + (ec_secret_size + ec_compressed_size) * nRingSize)
        {
            // ringsig AB
            if (!CheckAnonInputAB(txdb, txin, i, nRingSize, vchImage, preimage, nCoinValue))
            {
                fInvalid = true; return false;
            };

            nSumValue += nCoinValue;
            continue;
        };

        if (s.size() < 2 + (ec_compressed_size + ec_secret_size + ec_secret_size) * nRingSize)
        {
            printf("CheckAnonInputs(): Error input %d scriptSig too small.\n", i);
            fInvalid = true; return false;
        };


        CPubKey pkRingCoin;
        CAnonOutput ao;
        CTxIndex txindex;
        const unsigned char* pPubkeys = &s[2];
        const unsigned char* pSigc    = &s[2 + ec_compressed_size * nRingSize];
        const unsigned char* pSigr    = &s[2 + (ec_compressed_size + ec_secret_size) * nRingSize];
        for (int ri = 0; ri < nRingSize; ++ri)
        {
            pkRingCoin = CPubKey(&pPubkeys[ri * ec_compressed_size], ec_compressed_size);
            if (!txdb.ReadAnonOutput(pkRingCoin, ao))
            {
                printf("CheckAnonInputs(): Error input %d, element %d AnonOutput %s not found.\n", i, ri);
                fInvalid = true; return false;
            };

            if (nCoinValue == -1)
            {
                nCoinValue = ao.nValue;
            } else
            if (nCoinValue != ao.nValue)
            {
                printf("CheckAnonInputs(): Error input %d, element %d ring amount mismatch %d, %d.\n", i, ri, nCoinValue, ao.nValue);
                fInvalid = true; return false;
            };

            if (ao.nBlockHeight == 0
                || nBestHeight - ao.nBlockHeight < MIN_ANON_SPEND_DEPTH)
            {
                printf("CheckAnonInputs(): Error input %d, element %d depth < MIN_ANON_SPEND_DEPTH.\n", i, ri);
                fInvalid = true; return false;
            };
        };

        if (verifyRingSignature(vchImage, preimage, nRingSize, pPubkeys, pSigc, pSigr) != 0)
        {
            printf("CheckAnonInputs(): Error input %d verifyRingSignature() failed.\n", i);
            fInvalid = true; return false;
        };

        nSumValue += nCoinValue;
    };

    return true;
};

const CTxOut& CTransaction::GetOutputFor(const CTxIn& input, const MapPrevTx& inputs) const
{
    MapPrevTx::const_iterator mi = inputs.find(input.prevout.hash);
    if (mi == inputs.end())
        throw std::runtime_error("CTransaction::GetOutputFor() : prevout.hash not found");

    const CTransaction& txPrev = (mi->second).second;
    if (input.prevout.n >= txPrev.vout.size())
        throw std::runtime_error("CTransaction::GetOutputFor() : prevout.n out of range");

    return txPrev.vout[input.prevout.n];
}

int64_t CTransaction::GetValueIn(const MapPrevTx& inputs) const
{
    if (IsCoinBase())
        return 0;

    int64_t nResult = 0;
    for (unsigned int i = 0; i < vin.size(); i++)
    {
        if (nVersion == ANON_TXN_VERSION
            && vin[i].IsAnonInput())
        {
            continue;
        };
        nResult += GetOutputFor(vin[i], inputs).nValue;
    };

    return nResult;
}

unsigned int CTransaction::GetP2SHSigOpCount(const MapPrevTx& inputs) const
{
    if (IsCoinBase())
        return 0;

    unsigned int nSigOps = 0;
    for (unsigned int i = 0; i < vin.size(); i++)
    {
        if (nVersion == ANON_TXN_VERSION
            && vin[i].IsAnonInput())
            continue;
        const CTxOut& prevout = GetOutputFor(vin[i], inputs);
        if (prevout.scriptPubKey.IsPayToScriptHash())
            nSigOps += prevout.scriptPubKey.GetSigOpCount(vin[i].scriptSig);
    };

    return nSigOps;
}

bool CTransaction::ConnectInputs(CTxDB& txdb, MapPrevTx inputs, map<uint256, CTxIndex>& mapTestPool, const CDiskTxPos& posThisTx,
    const CBlockIndex* pindexBlock, bool fBlock, bool fMiner, unsigned int flags, bool fValidateSig, bool fSkipFCMP)
{
    // Take over previous transactions' spent pointers
    // fBlock is true when this is called from AcceptBlock when a new best-block is added to the blockchain
    // fMiner is true when called from the internal bitcoin miner
    // ... both are false when called from CTransaction::AcceptToMemoryPool
    if (!IsCoinBase())
    {
        // vector<CTransaction> vTxPrev;
        // vector<CTxIndex> vTxindex;
        int64_t nValueIn = 0;
        int64_t nFees = 0;
        for (unsigned int i = 0; i < vin.size(); i++)
        {
            if (nVersion == ANON_TXN_VERSION && vin[i].IsAnonInput())
                continue;
            COutPoint prevout = vin[i].prevout;
            if (inputs.count(prevout.hash) == 0)
                return DoS(100, error("ConnectInputs() : missing input %s", prevout.hash.ToString().c_str()));
            CTxIndex& txindex = inputs[prevout.hash].first;
            CTransaction& txPrev = inputs[prevout.hash].second;

            if (prevout.n >= txPrev.vout.size() || prevout.n >= txindex.vSpent.size())
                return DoS(100, error("ConnectInputs() : %s prevout.n out of range %d %" PRIszu" %" PRIszu" prev tx %s\n%s", GetHash().ToString().substr(0,10).c_str(), prevout.n, txPrev.vout.size(), txindex.vSpent.size(), prevout.hash.ToString().substr(0,10).c_str(), txPrev.ToString().c_str()));

            // If prev is coinbase or coinstake, check that it's matured
            if (txPrev.IsCoinBase() || txPrev.IsCoinStake())
                for (const CBlockIndex* pindex = pindexBlock; pindex && pindexBlock->nHeight - pindex->nHeight < nCoinbaseMaturity; pindex = pindex->pprev)
                    if (pindex->nBlockPos == txindex.pos.nBlockPos && pindex->nFile == txindex.pos.nFile)
                        return error("ConnectInputs() : tried to spend %s at depth %d", txPrev.IsCoinBase() ? "coinbase" : "coinstake", pindexBlock->nHeight - pindex->nHeight);

            // ppcoin: check transaction timestamp
            if (txPrev.nTime > nTime)
                return DoS(100, error("ConnectInputs() : transaction timestamp earlier than input transaction"));

            // Check for negative or overflow input values
            nValueIn += txPrev.vout[prevout.n].nValue;
            if (!MoneyRange(txPrev.vout[prevout.n].nValue) || !MoneyRange(nValueIn))
                return DoS(100, error("ConnectInputs() : txin values out of range"));

        }
        // The first loop above does all the inexpensive checks.
        // Only if ALL inputs pass do we perform expensive ECDSA signature checks.
        // Helps prevent CPU exhaustion attacks.
        for (unsigned int i = 0; i < vin.size(); i++)
        {
            if (nVersion == ANON_TXN_VERSION
                && vin[i].IsAnonInput())
                continue;
            COutPoint prevout = vin[i].prevout;
            if (inputs.count(prevout.hash) == 0)
                return DoS(100, error("ConnectInputs() : missing input %s", prevout.hash.ToString().c_str()));
            CTxIndex& txindex = inputs[prevout.hash].first;
            CTransaction& txPrev = inputs[prevout.hash].second;

            // Check for conflicts (double-spend)
            // This doesn't trigger the DoS code on purpose; if it did, it would make it easier
            // for an attacker to attempt to split the network.
            if (!txindex.vSpent[prevout.n].IsNull())
            {
                printf("WARNING: ConnectInputs() : %s double-spend attempt at %s\n",
                       GetHash().ToString().substr(0,10).c_str(), txindex.vSpent[prevout.n].ToString().c_str());
                return DoS(100, error("ConnectInputs() : %s prev tx already used at %s", GetHash().ToString().substr(0,10).c_str(), txindex.vSpent[prevout.n].ToString().c_str()));
            }

            {
            // Skip ECDSA signature verification when connecting blocks (fBlock=true)
            // before the last blockchain checkpoint. This is safe because block merkle hashes are
            // still computed and checked, and any change will be caught at the next checkpoint.
            if (!(fBlock && (nBestHeight < Checkpoints::GetTotalBlocksEstimate())))
            {
                // Verify signature
                if (!VerifySignature(txPrev, *this, i, flags, 0))
                {
                    if (flags & STANDARD_NOT_MANDATORY_VERIFY_FLAGS) {
                    // Check whether the failure was caused by a
                    // non-mandatory script verification check, such as
                    // non-null dummy arguments;
                    // if so, don't trigger DoS protection to
                    // avoid splitting the network between upgraded and
                    // non-upgraded nodes.
                    if (VerifySignature(txPrev, *this, i, flags & ~STANDARD_NOT_MANDATORY_VERIFY_FLAGS, 0))
                        return error("ConnectInputs() : %s non-mandatory VerifySignature failed", GetHash().ToString().c_str());
                    }
                    // Failures of other flags indicate a transaction that is
                    // invalid in new blocks, e.g. a invalid P2SH. We DoS ban
                    // such nodes as they are not following the protocol. That
                    // said during an upgrade careful thought should be taken
                    // as to the correct behavior - we may want to continue
                    // peering with non-upgraded nodes even after a soft-fork
                    // super-majority vote has passed.
                    return DoS(100,error("ConnectInputs() : %s VerifySignature failed", GetHash().ToString().substr(0,10).c_str()));
                }
            }
            }
            // Mark outpoints as spent
            txindex.vSpent[prevout.n] = posThisTx;

            // Write back
            if (fBlock || fMiner)
            {
                mapTestPool[prevout.hash] = txindex;
            }
            //Push txPrev and txindex to vTxPrev and VTxIndex
            // vTxPrev.push_back(txPrev);
            // vTxindex.push_back(txindex);
        }
        //vector<nameTempProxy>& vName
        //If it can't connect inputs return false to the Name DB
        // if (!hooks->ConnectInputs(txdb, mapTestPool, *this, posThisTx, pindexBlock, fBlock, fMiner, flags, vName)) {
        //     return false;
        // }

        if (nVersion == ANON_TXN_VERSION)
        {
            if (pindexBlock && pindexBlock->nHeight >= FORK_HEIGHT_RINGSIG_DEPRECATION)
                return DoS(100, error("ConnectInputs() : ring signature transactions deprecated after height %d", FORK_HEIGHT_RINGSIG_DEPRECATION));

            int64_t nSumAnon;
            bool fInvalid;
            if (!CheckAnonInputs(txdb, nSumAnon, fInvalid, true))
            {
                //if (fInvalid)
                DoS(100, error("ConnectInputs() : CheckAnonInputs found invalid tx %s", GetHash().ToString().substr(0,10).c_str()));
            };

            nValueIn += nSumAnon;
        };

        if (IsShielded())
        {
            if (pindexBlock && pindexBlock->nHeight < FORK_HEIGHT_SHIELDED)
                return DoS(100, error("ConnectInputs() : shielded tx before activation height %d", FORK_HEIGHT_SHIELDED));

            for (const CShieldedSpendDescription& spend : vShieldedSpend)
            {
                CShieldedNullifierSpent nfs;
                if (txdb.ReadShieldedNullifier(spend.nullifier, nfs))
                    return DoS(100, error("ConnectInputs() : shielded nullifier %s already spent in tx %s",
                                          spend.nullifier.ToString().substr(0,10).c_str(),
                                          nfs.txnHash.ToString().substr(0,10).c_str()));

                if (!txdb.ReadShieldedAnchor(spend.anchor))
                    return DoS(100, error("ConnectInputs() : shielded anchor %s not found",
                                          spend.anchor.ToString().substr(0,10).c_str()));
                int nAnchorHeight = 0;
                if (pindexBlock && txdb.ReadShieldedAnchorHeight(spend.anchor, nAnchorHeight))
                {
                    if (pindexBlock->nHeight - nAnchorHeight < MIN_SHIELDED_SPEND_DEPTH)
                        return DoS(100, error("ConnectInputs() : shielded anchor %s too recent (height=%d, block=%d, need %d)",
                                              spend.anchor.ToString().substr(0,10).c_str(),
                                              nAnchorHeight, pindexBlock->nHeight, MIN_SHIELDED_SPEND_DEPTH));
                }
            }

            if (nValueBalance > 0)
            {
                if (nValueIn > MAX_MONEY - nValueBalance)
                    return DoS(100, error("ConnectInputs() : nValueIn overflow with shielded balance"));
                nValueIn += nValueBalance;
            }
            if (nValueBalance == std::numeric_limits<int64_t>::min())
                return DoS(100, error("ConnectInputs() : nValueBalance is INT64_MIN"));
            int64_t nShieldedAbsorbed = (nValueBalance < 0) ? (-nValueBalance) : 0;

            if (GetValueOut() > MAX_MONEY - nShieldedAbsorbed)
                return DoS(100, error("ConnectInputs() : GetValueOut + nShieldedAbsorbed overflow"));

            // NullStake coinstakes are exempt: the reward enters the shielded pool
            // from block subsidy, not from transparent inputs. The reward amount
            // is validated separately in ConnectBlock() against GetProofOfStakeReward().
            if ((nVersion != SHIELDED_TX_VERSION_NULLSTAKE && nVersion != SHIELDED_TX_VERSION_NULLSTAKE_V2 && nVersion != SHIELDED_TX_VERSION_NULLSTAKE_COLD) || !IsCoinStake())
            {
                if (nValueIn < GetValueOut() + nShieldedAbsorbed)
                    return DoS(100, error("ConnectInputs() : %s shielded value balance failed (in=%" PRId64 " out=%" PRId64 " shielded=%" PRId64 ")",
                                          GetHash().ToString().substr(0,10).c_str(),
                                          nValueIn, GetValueOut(), nShieldedAbsorbed));
            }

            // use DoS(100) to ban peers sending shielded tx when ZK unavailable
            if (!CZKContext::IsInitialized())
                return DoS(100, error("ConnectInputs() : ZK context not initialized, cannot validate shielded tx"));

            {
                uint256 sighash = GetBindingSigHash();

                // DSP mode flags (default to fully private for v2000)
                bool fHideAmount   = IsDSP() ? DSP_HideAmount(nPrivacyMode)   : true;
                bool fHideSender   = IsDSP() ? DSP_HideSender(nPrivacyMode)   : true;

                int nBlockHeight = pindexBlock ? pindexBlock->nHeight : nBestHeight;

                // Post-fork enforcement: after FCMP fork, reject old tx versions with shielded spends
                if (nBlockHeight >= FORK_HEIGHT_FCMP_VALIDATION && !vShieldedSpend.empty()
                    && nVersion < SHIELDED_TX_VERSION_FCMP)
                {
                    return DoS(100, error("ConnectInputs() : tx version %d with shielded spends rejected after FCMP fork (need version >= %d)",
                                          nVersion, SHIELDED_TX_VERSION_FCMP));
                }

                CCurveTreeNode ciRootNode;
                uint256 hashExpectedFCMPRoot = 0;
                if (nVersion >= SHIELDED_TX_VERSION_FCMP && nBlockHeight >= FORK_HEIGHT_FCMP_VALIDATION
                    && !vShieldedSpend.empty())
                {
                    std::string strFCMPError;
                    if (!LoadFCMPValidationRoot(txdb, nBlockHeight, ciRootNode, hashExpectedFCMPRoot, strFCMPError))
                        return DoS(100, error("ConnectInputs() : %s", strFCMPError.c_str()));
                    if (!CheckFCMPSpendRoots(*this, nBlockHeight, hashExpectedFCMPRoot, strFCMPError))
                        return DoS(100, error("ConnectInputs() : %s", strFCMPError.c_str()));
                }

                for (size_t i = 0; i < vShieldedSpend.size(); i++)
                {
                    if (fHideAmount)
                    {
                        if (!VerifyBulletproofRangeProof(vShieldedSpend[i].cv, vShieldedSpend[i].rangeProof))
                            return DoS(100, error("ConnectInputs() : shielded spend %d range proof failed", (int)i));
                    }
                    else
                    {
                        if (!VerifyPedersenCommitment(vShieldedSpend[i].cv,
                                                       vShieldedSpend[i].nPlaintextValue,
                                                       vShieldedSpend[i].vchPlaintextBlind))
                            return DoS(100, error("ConnectInputs() : DSP spend %d commitment opening proof failed", (int)i));
                    }

                    if (vShieldedSpend[i].vchSpendAuthSig.empty() || vShieldedSpend[i].vchRk.empty())
                        return DoS(100, error("ConnectInputs() : shielded spend %d missing spend auth sig or rk", (int)i));

                    if (!VerifySpendAuthSignature(vShieldedSpend[i].vchRk, sighash, vShieldedSpend[i].vchSpendAuthSig))
                        return DoS(100, error("ConnectInputs() : shielded spend %d spend auth sig failed", (int)i));

                    if (fHideSender)
                    {
                        if (vShieldedSpend[i].vchLelantusProof.empty() || vShieldedSpend[i].vAnonSet.empty())
                            return DoS(100, error("ConnectInputs() : shielded spend %d missing mandatory Lelantus proof", (int)i));

                        if ((int)vShieldedSpend[i].vAnonSet.size() < LELANTUS_MIN_SET_SIZE)
                            return DoS(100, error("ConnectInputs() : shielded spend %d anonymity set size %d below minimum %d",
                                                  (int)i, (int)vShieldedSpend[i].vAnonSet.size(), LELANTUS_MIN_SET_SIZE));

                        {
                            {
                                std::set<std::vector<unsigned char>> setChainCommitments;
                                uint64_t nCommitCount = 0;
                                txdb.ReadShieldedCommitmentCount(nCommitCount);
                                for (uint64_t ci = 0; ci < nCommitCount; ci++)
                                {
                                    CPedersenCommitment chainCommit;
                                    if (txdb.ReadShieldedCommitment(ci, chainCommit))
                                        setChainCommitments.insert(chainCommit.vchCommitment);
                                }

                                for (size_t j = 0; j < vShieldedSpend[i].vAnonSet.size(); j++)
                                {
                                    if (setChainCommitments.find(vShieldedSpend[i].vAnonSet[j].vchCommitment) == setChainCommitments.end())
                                        return DoS(100, error("ConnectInputs() : shielded spend %d anonymity set commitment %d not in chain state", (int)i, (int)j));
                                }
                            }

                            CAnonymitySet anonSet;
                            anonSet.vCommitments = vShieldedSpend[i].vAnonSet;
                            CLelantusProof proof;
                            proof.vchProof = vShieldedSpend[i].vchLelantusProof;
                            proof.serialNumber = vShieldedSpend[i].lelantusSerial;

                            if (!VerifyLelantusProof(anonSet, proof, vShieldedSpend[i].cv))
                                return DoS(100, error("ConnectInputs() : shielded spend %d Lelantus proof failed", (int)i));
                        }
                    }

                    // FCMP++ proof: required after FORK_HEIGHT_FCMP_VALIDATION for FCMP tx versions
                    if (!fSkipFCMP && nVersion >= SHIELDED_TX_VERSION_FCMP && nBlockHeight >= FORK_HEIGHT_FCMP_VALIDATION)
                    {
                        if (vShieldedSpend[i].fcmpProof.IsNull())
                            return DoS(100, error("ConnectInputs() : shielded spend %d missing FCMP++ proof (required post-fork)", (int)i));

                        if (!VerifyFCMPProof(ciRootNode, vShieldedSpend[i].fcmpProof, vShieldedSpend[i].cv))
                            return DoS(100, error("ConnectInputs() : shielded spend %d FCMP++ proof failed", (int)i));
                    }
                }
                for (size_t i = 0; i < vShieldedOutput.size(); i++)
                {
                    if (fHideAmount)
                    {
                        if (!VerifyBulletproofRangeProof(vShieldedOutput[i].cv, vShieldedOutput[i].rangeProof))
                            return DoS(100, error("ConnectInputs() : shielded output %d range proof failed", (int)i));
                    }
                    else
                    {
                        if (!VerifyPedersenCommitment(vShieldedOutput[i].cv,
                                                       vShieldedOutput[i].nPlaintextValue,
                                                       vShieldedOutput[i].vchPlaintextBlind))
                            return DoS(100, error("ConnectInputs() : DSP output %d commitment opening proof failed", (int)i));
                    }
                }

                if (!fHideAmount)
                {
                    int64_t nPlainIn = 0, nPlainOut = 0;
                    for (size_t i = 0; i < vShieldedSpend.size(); i++)
                        nPlainIn += vShieldedSpend[i].nPlaintextValue;
                    for (size_t i = 0; i < vShieldedOutput.size(); i++)
                        nPlainOut += vShieldedOutput[i].nPlaintextValue;
                    if (nPlainIn - nPlainOut != nValueBalance)
                        return DoS(100, error("ConnectInputs() : DSP plaintext value balance mismatch"));
                }

                if (bindingSig.IsNull())
                    return DoS(100, error("ConnectInputs() : shielded tx missing mandatory binding signature"));

                {
                    std::vector<CPedersenCommitment> vInCommits, vOutCommits;
                    for (size_t i = 0; i < vShieldedSpend.size(); i++)
                        vInCommits.push_back(vShieldedSpend[i].cv);
                    for (size_t i = 0; i < vShieldedOutput.size(); i++)
                        vOutCommits.push_back(vShieldedOutput[i].cv);

                    if (!VerifyBindingSignature(vInCommits, vOutCommits, nValueBalance, sighash, bindingSig.bindingSig))
                        return DoS(100, error("ConnectInputs() : shielded binding signature verification failed"));
                }
            }
        };

        if (!IsCoinStake())
        {
            int64_t nEffectiveOut = GetValueOut();
            if (IsShielded() && nValueBalance < 0)
                nEffectiveOut += (-nValueBalance); // shielded value absorbed from transparent

            if (nValueIn < nEffectiveOut)
                return DoS(100, error("ConnectInputs() : %s value in < value out", GetHash().ToString().substr(0,10).c_str()));

            // Tally transaction fees
            int64_t nTxFee = nValueIn - nEffectiveOut;
            if (nTxFee < 0)
                return DoS(100, error("ConnectInputs() : %s nTxFee < 0", GetHash().ToString().substr(0,10).c_str()));

            // enforce transaction fees for every block
            if (nTxFee < GetMinFee())
                return fBlock? DoS(100, error("ConnectInputs() : %s not paying required fee=%s, paid=%s", GetHash().ToString().substr(0,10).c_str(), FormatMoney(GetMinFee()).c_str(), FormatMoney(nTxFee).c_str())) : false;

            nFees += nTxFee;
            if (!MoneyRange(nFees))
                return DoS(100, error("ConnectInputs() : nFees out of range"));
        }
    }

    return true;
}

bool CBlock::DisconnectBlock(CTxDB& txdb, CBlockIndex* pindex, bool fWriteNames)
{
    // Disconnect in reverse order. HREG reverse-state replay must happen
    // immediately after each tx's inputs are disconnected, while earlier same-
    // block parent tx indices still exist in txdb.
    for (int i = vtx.size()-1; i >= 0; i--)
    {
        if (!vtx[i].DisconnectInputs(txdb))
            return false;
        if (hreg::IsHRegRecognitionActive(pindex->nHeight))
        {
            CTransaction tx = vtx[i];
            bool fInvalid = false;
            MapPrevTx mapInputs;
            std::map<uint256, CTxIndex> mapEmpty;
            if (!tx.FetchInputs(txdb, mapEmpty, true, false, mapInputs, fInvalid))
                return error("DisconnectBlock() : HREG FetchInputs failed");
            std::string strHRegError;
            if (!hreg::DisconnectHRegTx(tx, mapInputs, pindex->nHeight, strHRegError))
                return error("DisconnectBlock() : HREG disconnect invalid: %s", strHRegError.c_str());
        }
    }

    if (pindex->nHeight >= FORK_HEIGHT_DAG)
    {
        std::vector<CFinalityTallyCertificate> vFinalityCerts = ExtractFinalityTallyCertificatesFromBlock(*this);
        if (!vFinalityCerts.empty() &&
            !g_finalityTracker.DisconnectBlockTallyCertificates(txdb, pindex->GetBlockHash(), vFinalityCerts))
            return error("DisconnectBlock() : DisconnectBlockTallyCertificates failed");

        std::vector<CFinalityTallyShare> vFinalityShares = ExtractFinalityTallySharesFromBlock(*this);
        if (!vFinalityShares.empty() &&
            !g_finalityTracker.DisconnectBlockTallyShares(txdb, pindex->GetBlockHash(), vFinalityShares))
            return error("DisconnectBlock() : DisconnectBlockTallyShares failed");

        std::vector<CFinalityVote> vFinalityVotes = ExtractFinalityVotesFromBlock(*this);
        if (!vFinalityVotes.empty() &&
            !g_finalityTracker.DisconnectBlockVotes(txdb, pindex->GetBlockHash(), vFinalityVotes))
            return error("DisconnectBlock() : DisconnectBlockVotes failed");
    }

    if (pindex->nHeight >= FORK_HEIGHT_SHIELDED)
    {
        for (const CTransaction& tx : vtx)
        {
            if (!tx.IsShielded())
                continue;

            for (const CShieldedSpendDescription& spend : tx.vShieldedSpend)
            {
                txdb.EraseShieldedNullifier(spend.nullifier);
            }

            int64_t nShieldedPool = 0;
            txdb.ReadShieldedPoolValue(nShieldedPool);
            nShieldedPool += tx.nValueBalance; // reverse the subtraction done in ConnectBlock
            txdb.WriteShieldedPoolValue(nShieldedPool);
            nShieldedPoolValue = nShieldedPool;
        }

        CIncrementalMerkleTree currentTree;
        if (txdb.ReadShieldedTree(currentTree))
        {
            uint256 currentRoot = currentTree.Root();
            txdb.EraseShieldedAnchor(currentRoot);
        }

        CIncrementalMerkleTree prevTree;
        if (txdb.ReadShieldedTreeAtBlock(pindex->GetBlockHash(), prevTree))
        {
            txdb.WriteShieldedTree(prevTree);

            txdb.WriteShieldedCommitmentCount(prevTree.Size());
        }

        if (pindex->nHeight >= FORK_HEIGHT_FCMP &&
            pindex->nHeight < FORK_HEIGHT_EPOCH_ROOT_FCMP)
        {
            CCurveTree restoredCurveTree;
            if (txdb.ReadCurveTreeAtBlock(pindex->GetBlockHash(), restoredCurveTree))
            {
                txdb.WriteCurveTree(restoredCurveTree);
            }
            else
            {
                // fallback: O(n) rebuild
                printf("DisconnectBlock() : WARNING - Curve Tree snapshot not found for block %s, falling back to O(n) rebuild\n",
                       pindex->GetBlockHash().ToString().substr(0,16).c_str());
                uint64_t nRestoredCount = prevTree.Size();
                for (uint64_t i = 0; i < nRestoredCount; i++)
                {
                    CPedersenCommitment commit;
                    if (txdb.ReadShieldedCommitment(i, commit))
                        restoredCurveTree.InsertLeaf(commit);
                }
                txdb.WriteCurveTree(restoredCurveTree);
            }
            txdb.EraseCurveTreeAtBlock(pindex->GetBlockHash());
        }
    }

    // Update block index on disk without changing it in memory.
    // The memory index structure will be changed after the db commits.
    if (pindex->pprev)
    {
        CDiskBlockIndex blockindexPrev(pindex->pprev);
        blockindexPrev.hashNext = 0;
        if (!txdb.WriteBlockIndex(blockindexPrev))
            return error("DisconnectBlock() : WriteBlockIndex failed");
    }

    // innova: undo name transactions in reverse order
    for (int i = vtx.size() - 1; i >= 0; i--)
        hooks->DisconnectInputs(vtx[i]);

    // ppcoin: clean up wallet after disconnecting coinstake
    for (CTransaction& tx : vtx)
        SyncWithWallets(tx, this, false, false);

    return true;
}

bool static BuildAddrIndex(const CScript &script, std::vector<uint160>& addrIds)
{
    CScript::const_iterator pc = script.begin();
    CScript::const_iterator pend = script.end();
    std::vector<unsigned char> data;
    opcodetype opcode;
    bool fHaveData = false;
    while (pc < pend) {
        script.GetOp(pc, opcode, data);
        if (0 <= opcode && opcode <= OP_PUSHDATA4 && data.size() >= 8) { // data element
            uint160 addrid = 0;
            if (data.size() <= 20) {
                memcpy(&addrid, &data[0], data.size());
            } else {
                addrid = Hash160(data);
            }
            addrIds.push_back(addrid);
            fHaveData = true;
        }
    }
    if (!fHaveData) {
        uint160 addrid = Hash160(script);
	addrIds.push_back(addrid);
        return true;
    }
    else
    {
	if(addrIds.size() > 0)
	    return true;
	else
  	    return false;
    }
}

bool FindTransactionsByDestination(const CTxDestination &dest, std::vector<uint256> &vtxhash) {
    uint160 addrid = 0;
    const CKeyID *pkeyid = boost::get<CKeyID>(&dest);
    if (pkeyid)
        addrid = static_cast<uint160>(*pkeyid);
    if (!addrid) {
        const CScriptID *pscriptid = boost::get<CScriptID>(&dest);
        if (pscriptid)
            addrid = static_cast<uint160>(*pscriptid);
    }
    if (!addrid)
    {
        printf("FindTransactionsByDestination(): Couldn't parse dest into addrid\n");
        return false;
    }

    LOCK(cs_main);
    CTxDB txdb("r");
    if(!txdb.ReadAddrIndex(addrid, vtxhash))
    {
	printf("FindTransactionsByDestination(): txdb.ReadAddrIndex failed\n");
	return false;
    }
    return true;
}

void CBlock::RebuildAddressIndex(CTxDB& txdb)
{
    for (CTransaction& tx : vtx)
    {
        uint256 hashTx = tx.GetHash();
	// inputs
	if(!tx.IsCoinBase())
	{
            MapPrevTx mapInputs;
	    map<uint256, CTxIndex> mapQueuedChangesT;
	    bool fInvalid;
            if (!tx.FetchInputs(txdb, mapQueuedChangesT, true, false, mapInputs, fInvalid))
                return;

	    MapPrevTx::const_iterator mi;
	    for(MapPrevTx::const_iterator mi = mapInputs.begin(); mi != mapInputs.end(); ++mi)
	    {
		    for (const CTxOut &atxout : (*mi).second.second.vout)
		    {
			std::vector<uint160> addrIds;
			if(BuildAddrIndex(atxout.scriptPubKey, addrIds))
			{
                    for (uint160 addrId : addrIds)
		            {
			            if(!txdb.WriteAddrIndex(addrId, hashTx))
				            printf("RebuildAddressIndex(): txins WriteAddrIndex failed addrId: %s txhash: %s\n", addrId.ToString().c_str(), hashTx.ToString().c_str());
                    }
			}
		    }
	    }

        }
	// outputs
	for (const CTxOut &atxout : tx.vout) {
	    std::vector<uint160> addrIds;
        if(BuildAddrIndex(atxout.scriptPubKey, addrIds))
	    {
		for (uint160 addrId : addrIds)
		{
		    if(!txdb.WriteAddrIndex(addrId, hashTx))
		        printf("RebuildAddressIndex(): txouts WriteAddrIndex failed addrId: %s txhash: %s\n", addrId.ToString().c_str(), hashTx.ToString().c_str());
        }
	    }
	}
    }
}

static int64_t nTimeVerify = 0;
static int64_t nTimeConnect = 0;
static int64_t nTimeIndex = 0;
static int64_t nTimeCallbacks = 0;
static int64_t nTimeTotal = 0;

// Seed deterministic unspendable commitments at fork height for Lelantus anonymity set.
// Each seed: blind_i = SHA256("Innova_Genesis_Seed_" || i), cv_i = blind_i * G (zero value),
// cmu_i = SHA256("Innova_Genesis_Seed_CMU_" || i). Unspendable: no spending key, no nullifier derivation.
bool SeedGenesisCommitments(CTxDB& txdb, CIncrementalMerkleTree& shieldedTree, CCurveTree* pCurveTree)
{
    for (int i = 0; i < LELANTUS_GENESIS_SEED_COUNT; i++)
    {
        // Deterministic blinding factor
        CHashWriter ssBlind(SER_GETHASH, 0);
        ssBlind << std::string("Innova_Genesis_Seed_");
        ssBlind << i;
        uint256 blindHash = ssBlind.GetHash();
        std::vector<unsigned char> vchBlind(blindHash.begin(), blindHash.begin() + 32);

        CPedersenCommitment cv;
        if (!CreateBlindCommitment(vchBlind, cv))
            return error("SeedGenesisCommitments() : CreateBlindCommitment failed for seed %d", i);

        // Deterministic note commitment (Merkle leaf)
        CHashWriter ssCmu(SER_GETHASH, 0);
        ssCmu << std::string("Innova_Genesis_Seed_CMU_");
        ssCmu << i;
        uint256 cmu = ssCmu.GetHash();

        // Append to Merkle tree and write to commitment DB
        shieldedTree.Append(cmu);
        uint64_t nCommitIdx = shieldedTree.Size() - 1;
        if (!txdb.WriteShieldedCommitment(nCommitIdx, cv))
            return error("SeedGenesisCommitments() : WriteShieldedCommitment failed for seed %d", i);

        if (pCurveTree)
            pCurveTree->InsertLeaf(cv);
    }

    if (fDebug)
        printf("SeedGenesisCommitments() : seeded %d genesis commitments for Lelantus anonymity set\n",
               LELANTUS_GENESIS_SEED_COUNT);
    return true;
}

// ---------------------------------------------------------------------------
// Collateralnode payee/rank enforcement guard.
//
// Restores the !IsInitialBlockDownload() clause of the ORIGINAL upstream CN
// payment guard that was dropped during "Rebrand Phase Two" (commit c66803e).
// The pre-rebrand parent (ca0809a, main.cpp:2712) guarded the whole CN payment
// block with "... && (pindex->nHeight < pindexBest->nHeight+5) &&
// !IsInitialBlockDownload() && <name>Payments == true".
//
// Why this matters: the payee validation depends on the node's in-memory
// collateralnode list (vecCollateralnodes), which is only populated/synced via
// peer "iseg"/dseg messages and is NON-authoritative during initial block
// download. When the payee is not found in the (unsynced/empty) list, the code
// falls back to FindCNPayment(), an unbounded genesis-ward scan of every block
// and vout looking for the CN collateral. During IBD the local chain is also
// incomplete, so that scan is both pathologically slow (~200s of cs_main) and
// unreliable (it never finds the collateral -> false DoS-rejection of an
// otherwise-valid block). This wedges the node and starves every RPC.
//
// We therefore DEFER CN payee enforcement until the CN state is authoritative.
// "Authoritative" means BOTH (1) not in initial block download AND (2) the local
// in-memory collateralnode list is at least as complete as the network-announced
// median count (mnCount) -- i.e. it can reliably contain the block's payee. A
// marginal list (non-empty but partial) still triggers the expensive FindCNPayment
// fallback, so IBD alone is insufficient. All other consensus checks -- including
// the deterministic coinbase reward / CN-amount cap in ConnectBlock -- still run.
// Once CN state is authoritative, the EXACT existing validation runs unchanged.
// ---------------------------------------------------------------------------
static std::atomic<int64_t> nCNPaymentsDeferred{0};
// Diagnostic-only CN guard outcome telemetry (NO consensus/lock/validation change).
static std::atomic<int64_t> nCNValidationGuardPassed{0};
static std::atomic<int64_t> nCNValidationPayeeFound{0};
static std::atomic<int64_t> nCNValidationPayeeMissing{0};
static std::atomic<int64_t> nCNValidationFindCNPaymentEntered{0};
static std::atomic<int> nCNLastGuardReason{0};     // 0=none 1=justcheck 2=disabled 3=old 4=ibd 5=thin 6=passed
static std::atomic<int64_t> nCNLastGuardHeight{0};

bool ShouldValidateCollateralnodePayments(CBlockIndex* pindex, bool fJustCheck,
                                         bool fCollateralnodePaymentsEnabled)
{
    if (pindex)
        nCNLastGuardHeight.store(pindex->nHeight, std::memory_order_relaxed);
    nCNLastGuardReason.store(0, std::memory_order_relaxed);
    if (fJustCheck)
    {
        nCNLastGuardReason.store(1, std::memory_order_relaxed);
        return false;
    }
    if (!fCollateralnodePaymentsEnabled)
    {
        nCNLastGuardReason.store(2, std::memory_order_relaxed);
        return false;
    }
    if (!(pindex->GetBlockTime() > GetTime() - 20 * nCoinbaseMaturity))
    {
        nCNLastGuardReason.store(3, std::memory_order_relaxed);
        return false;
    }
    if (IsInitialBlockDownload())
    {
        nCNPaymentsDeferred.fetch_add(1, std::memory_order_relaxed);
        nCNLastGuardReason.store(4, std::memory_order_relaxed);
        if (fDebug)
            printf("CNPAY_DEFERRED height=%d reason=IBD\n", pindex->nHeight);
        return false;
    }
    // Being past IBD alone is NOT sufficient: a partially-synced collateralnode
    // list can still miss a block's real payee. When the payee is not in the
    // local list, the code falls back to FindCNPayment(), an unbounded
    // genesis-ward scan of every block/vout (~200s of cs_main) which then
    // false-DoS-rejects an otherwise-valid block. So the list is authoritative
    // for enforcement only once it is at least as complete as the network-
    // announced median collateralnode count (mnCount) -- i.e. once it can
    // reliably contain the block's payee. If we cannot confirm that (unknown
    // median, stale/thin list, or the lock is busy), we defer.
    unsigned int nNetMedian = 0;
    size_t nLocalCount = 0;
    {
        TRY_LOCK(cs_collateralnodes, lockCN);
        if (lockCN)
        {
            nNetMedian = mnCount;
            nLocalCount = vecCollateralnodes.size();
        }
        // If the lock could not be taken, treat as non-authoritative (defer).
    }
    if (nNetMedian > 0 && nLocalCount >= (size_t)nNetMedian)
    {
        nCNValidationGuardPassed.fetch_add(1, std::memory_order_relaxed);
        nCNLastGuardReason.store(6, std::memory_order_relaxed);
        return true; // authoritative: run exact existing validation unchanged
    }
    nCNPaymentsDeferred.fetch_add(1, std::memory_order_relaxed);
    nCNLastGuardReason.store(5, std::memory_order_relaxed);
    if (fDebug)
        printf("CNPAY_DEFERRED height=%d reason=list-not-authoritative local=%u median=%u\n",
               pindex->nHeight, (unsigned)nLocalCount, nNetMedian);
    return false;
}

int64_t GetCNPaymentsDeferredCount()
{
    return nCNPaymentsDeferred.load(std::memory_order_relaxed);
}
void ResetCNPaymentsDeferredCountForTesting()
{
    nCNPaymentsDeferred.store(0, std::memory_order_relaxed);
}
int64_t GetCNValidationGuardPassedCount()
{
    return nCNValidationGuardPassed.load(std::memory_order_relaxed);
}
int64_t GetCNValidationPayeeFoundCount()
{
    return nCNValidationPayeeFound.load(std::memory_order_relaxed);
}
int64_t GetCNValidationPayeeMissingCount()
{
    return nCNValidationPayeeMissing.load(std::memory_order_relaxed);
}
int64_t GetCNValidationFindCNPaymentEnteredCount()
{
    return nCNValidationFindCNPaymentEntered.load(std::memory_order_relaxed);
}
int GetCNValidationLastGuardReason()
{
    return nCNLastGuardReason.load(std::memory_order_relaxed);
}
int64_t GetCNValidationLastGuardHeight()
{
    return nCNLastGuardHeight.load(std::memory_order_relaxed);
}
void ResetCNValidationCountersForTesting()
{
    nCNValidationGuardPassed.store(0, std::memory_order_relaxed);
    nCNValidationPayeeFound.store(0, std::memory_order_relaxed);
    nCNValidationPayeeMissing.store(0, std::memory_order_relaxed);
    nCNValidationFindCNPaymentEntered.store(0, std::memory_order_relaxed);
    nCNLastGuardReason.store(0, std::memory_order_relaxed);
    nCNLastGuardHeight.store(0, std::memory_order_relaxed);
}

bool CBlock::ConnectBlock(CTxDB& txdb, CBlockIndex* pindex, bool fJustCheck, bool fWriteNames)
{
    ibdactivepath::ActivePathTimer ibdConnectTimer(
        ibdactivepath::GetCounters().connectblock_us_total,
        ibdactivepath::GetCounters().connectblock_us_max,
        ibdactivepath::GetCounters().connectblock_count,
        "connectblock", pindex->nHeight);
    int64_t nConnectBlockStart = GetTimeMillis();
    int64_t nConnectCheckStart = GetTimeMillis();

    // Check it again in case a previous version let a bad block in, but skip BlockSig checking
    if (!CheckBlock(!fJustCheck, !fJustCheck, false))
        return false;
    int64_t nConnectCheckMs = GetTimeMillis() - nConnectCheckStart;

    if (pindex->nHeight >= FORK_HEIGHT_DAG && IsProofOfStake())
        return DoS(100, error("ConnectBlock() : proof-of-stake blocks are not allowed after DAG fork"));

    std::vector<CFinalityVote> vFinalityVotes = ExtractFinalityVotesFromBlock(*this);
    if (!vFinalityVotes.empty() && (pindex->nHeight < FORK_HEIGHT_DAG || IsProofOfStake()))
        return DoS(100, error("ConnectBlock() : finality votes are only valid in post-DAG proof-of-work blocks"));
    std::vector<CFinalityTallyShare> vFinalityShares = ExtractFinalityTallySharesFromBlock(*this);
    if (!vFinalityShares.empty() && (pindex->nHeight < FORK_HEIGHT_DAG || IsProofOfStake()))
        return DoS(100, error("ConnectBlock() : finality tally shares are only valid in post-DAG proof-of-work blocks"));
    std::vector<CFinalityTallyCertificate> vFinalityCerts = ExtractFinalityTallyCertificatesFromBlock(*this);
    if (!vFinalityCerts.empty() && (pindex->nHeight < FORK_HEIGHT_DAG || IsProofOfStake()))
        return DoS(100, error("ConnectBlock() : finality tally certificates are only valid in post-DAG proof-of-work blocks"));

    // strict script verification post-fork
    unsigned int flags = SCRIPT_VERIFY_NONE;

    if (pindex->nHeight == 2080000 && GetHash() == uint256("0x000000001f9f67efdef5c02fc3da51f308011443c9e5dae6a79a11dba88525e8"))
        return DoS(100, error("ConnectBlock() : reject block from bad chain"));

    // Strict script verification after fork height
    if (pindex->nHeight >= FORK_HEIGHT_TIGHTER_DRIFT)
    {
        flags = MANDATORY_SCRIPT_VERIFY_FLAGS |
                SCRIPT_VERIFY_STRICTENC |
                SCRIPT_VERIFY_CHECKLOCKTIMEVERIFY;
    }

    //// issue here: it doesn't know the version
    unsigned int nTxPos;
    if (fJustCheck)
        // FetchInputs treats CDiskTxPos(1,1,1) as a special "refer to memorypool" indicator
        // Since we're just checking the block and not actually connecting it, it might not (and probably shouldn't) be on the disk to get the transaction from
        nTxPos = 1;
    else
        nTxPos = pindex->nBlockPos + ::GetSerializeSize(CBlock(), SER_DISK, CLIENT_VERSION) - (2 * GetSizeOfCompactSize(0)) + GetSizeOfCompactSize(vtx.size());

    map<uint256, CTxIndex> mapQueuedChanges;
    int64_t nFees = 0;
    int64_t nValueIn = 0;
    int64_t nValueOut = 0;
    int64_t nAmountBurned = 0;
    int64_t nStakeReward = 0;
    unsigned int nSigOps = 0;

    //DiskTxPos pos(pindex->GetBlockPos(), GetSizeOfCompactSize(vtx.size()));
    CDiskTxPos pos(pindex->nFile, pindex->nBlockPos, nTxPos);
    std::vector<std::pair<uint256, CDiskTxPos> > vPos;
    vPos.reserve(vtx.size());

    std::vector<CAmount> vFees (vtx.size(), 0);

    bool fFCMPBatchVerified = false;
    if (pindex->nHeight >= FORK_HEIGHT_FCMP_VALIDATION)
    {
        std::vector<CFCMPProof> vBlockProofs;
        std::vector<CPedersenCommitment> vBlockCommitments;
        CCurveTreeNode fcmpRootNode;
        uint256 hashExpectedFCMPRoot = 0;
        bool fHaveFCMPRoot = false;

        for (unsigned int i = 1; i < vtx.size(); i++)
        {
            if (vtx[i].nVersion >= SHIELDED_TX_VERSION_FCMP)
            {
                if (vtx[i].vShieldedSpend.size() > 1000)
                    return DoS(100, error("ConnectBlock() : tx %d has too many shielded spends (%u)", i, (unsigned int)vtx[i].vShieldedSpend.size()));

                for (const auto& spend : vtx[i].vShieldedSpend)
                {
                    if (spend.fcmpProof.IsNull())
                        return DoS(100, error("ConnectBlock() : tx %d spend missing FCMP++ proof", i));
                    if (!fHaveFCMPRoot)
                    {
                        std::string strFCMPError;
                        if (!LoadFCMPValidationRoot(txdb, pindex->nHeight, fcmpRootNode, hashExpectedFCMPRoot, strFCMPError))
                            return DoS(100, error("ConnectBlock() : %s", strFCMPError.c_str()));
                        fHaveFCMPRoot = true;
                    }
                    if (pindex->nHeight >= FORK_HEIGHT_EPOCH_ROOT_FCMP && spend.curveTreeRoot != hashExpectedFCMPRoot)
                        return DoS(100, error("ConnectBlock() : tx %d spend FCMP root does not match finalized epoch root", i));
                    vBlockProofs.push_back(spend.fcmpProof);
                    vBlockCommitments.push_back(spend.cv);
                }
            }
        }

        if (!vBlockProofs.empty())
        {
            if (!BatchVerifyFCMPProofs(fcmpRootNode, vBlockProofs, vBlockCommitments))
                return DoS(100, error("ConnectBlock() : batch FCMP++ proof verification failed"));

            fFCMPBatchVerified = true;

            if (fDebug)
                printf("ConnectBlock() : batch verified %d FCMP++ proofs\n", (int)vBlockProofs.size());
        }
    }

    std::set<uint256> setBlockNullifiers;

    // IDAG: Collect spent outputs from DAG sibling blocks (lower hash = canonical earlier)
    // Transactions conflicting with already-spent outputs from siblings are skipped
    std::set<COutPoint> setDAGSpentOutputs;
    bool fDAGActive = (pindex->nHeight >= FORK_HEIGHT_DAG);
    if (fDAGActive && pindex->phashBlock)
    {
        std::set<uint256> siblings = g_dagManager.GetDAGSiblingBlocks(pindex->GetBlockHash());
        CBlockDAGData currentDagData;
        bool fHaveCurrentDAGOrder = g_dagManager.GetDAGData(pindex->GetBlockHash(), currentDagData) &&
                                    currentDagData.nDAGOrder >= 0;
        for (const uint256& hashSibling : siblings)
        {
            bool fSiblingPrecedes = (hashSibling < pindex->GetBlockHash());
            CBlockDAGData siblingDagData;
            if (fHaveCurrentDAGOrder && g_dagManager.GetDAGData(hashSibling, siblingDagData) &&
                siblingDagData.nDAGOrder >= 0)
                fSiblingPrecedes = siblingDagData.nDAGOrder < currentDagData.nDAGOrder;
            if (!fSiblingPrecedes)
                continue;

            // Load sibling block and collect its spent outputs
            std::map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(hashSibling);
            if (mi == mapBlockIndex.end())
                continue;
            CBlock sibBlock;
            if (!sibBlock.ReadFromDisk(mi->second))
                continue;

            for (const CTransaction& sibTx : sibBlock.vtx)
            {
                if (sibTx.IsCoinBase() || sibTx.IsCoinStake())
                    continue;
                for (const CTxIn& txin : sibTx.vin)
                    setDAGSpentOutputs.insert(txin.prevout);
            }
        }
    }

    int64_t nTransparentValidateMicros = 0;
    int64_t nShieldedValidateMicros = 0;
    int64_t nPrivateStakeValidateMicros = 0;
    int64_t nAnonValidateMicros = 0;
    unsigned int nTransparentValidateCount = 0;
    unsigned int nShieldedValidateCount = 0;
    unsigned int nPrivateStakeValidateCount = 0;
    unsigned int nAnonValidateCount = 0;

    const bool fHRegActive = hreg::IsHRegRecognitionActive(pindex->nHeight);
    hreg::RegistryState hregShadowState;
    if (fHRegActive)
        hregShadowState = hreg::g_registry;

    for (CTransaction& tx : vtx)
    {
        //const CTransaction &tx = vtx[i];
        int64_t nTxValidateStart = GetTimeMicros();
        uint256 hashTx = tx.GetHash();

        // Do not allow blocks that contain transactions which 'overwrite' older transactions,
        // unless those are already completely spent.
        // If such overwrites are allowed, coinbases and transactions depending upon those
        // can be duplicated to remove the ability to spend the first instance -- even after
        // being sent to another address.
        // See BIP30 and http://r6.ca/blog/20120206T005236Z.html for more information.
        // This logic is not necessary for memory pool transactions, as AcceptToMemoryPool
        // already refuses previously-known transaction ids entirely.
        // This rule was originally applied all blocks whose timestamp was after March 15, 2012, 0:00 UTC.
        // Now that the whole chain is irreversibly beyond that time it is applied to all blocks except the
        // two in the chain that violate it. This prevents exploiting the issue against nodes in their
        // initial block download.
        CTxIndex txindexOld;
        if (txdb.ReadTxIndex(hashTx, txindexOld)) {
            for (CDiskTxPos &pos : txindexOld.vSpent)
                if (pos.IsNull())
                    return false;
        }

        nSigOps += tx.GetLegacySigOpCount();
        if (nSigOps > MAX_BLOCK_SIGOPS)
            return DoS(100, error("ConnectBlock() : too many sigops"));

        CDiskTxPos posThisTx(pindex->nFile, pindex->nBlockPos, nTxPos);
        if (!fJustCheck)
            nTxPos += ::GetSerializeSize(tx, SER_DISK, CLIENT_VERSION);

        MapPrevTx mapInputs;
        if (tx.IsCoinBase())
        {
            int64_t nCoinbaseValue;
            try {
                nCoinbaseValue = tx.GetValueOut();
            } catch (const std::runtime_error& e) {
                return DoS(100, error("ConnectBlock() : coinbase GetValueOut overflow: %s", e.what()));
            }
            nValueOut += nCoinbaseValue;
        }
        else
        {
            // IDAG Phase 2: Skip transactions whose inputs conflict with DAG siblings
            if (fDAGActive && !tx.IsCoinStake())
            {
                bool fConflict = false;
                for (const CTxIn& txin : tx.vin)
                {
                    if (setDAGSpentOutputs.count(txin.prevout))
                    {
                        fConflict = true;
                        break;
                    }
                }
                if (fConflict)
                {
                    if (fDebug)
                        printf("ConnectBlock() : DAG conflict skip tx %s (inputs spent by sibling)\n",
                               hashTx.ToString().substr(0, 20).c_str());
                    // Skip this tx but don't fail the block
                    nTxPos += ::GetSerializeSize(tx, SER_DISK, CLIENT_VERSION);
                    pos.nTxPos += ::GetSerializeSize(tx, SER_DISK, CLIENT_VERSION);
                    continue;
                }
            }

            bool fInvalid;
            if (!tx.FetchInputs(txdb, mapQueuedChanges, true, false, mapInputs, fInvalid))
                return false;

            for (const CShieldedSpendDescription& spend : tx.vShieldedSpend)
            {
                if (!setBlockNullifiers.insert(spend.nullifier).second)
                    return DoS(100, error("ConnectBlock() : duplicate nullifier %s across txs in block",
                                          spend.nullifier.ToString().substr(0,10).c_str()));
            }

            // Add in sigops done by pay-to-script-hash inputs;
            // this is to prevent a "rogue miner" from creating
            // an incredibly-expensive-to-validate block.
            nSigOps += tx.GetP2SHSigOpCount(mapInputs);
            if (nSigOps > MAX_BLOCK_SIGOPS)
                return DoS(100, error("ConnectBlock() : too many sigops"));

            int64_t nTxValueIn = tx.GetValueIn(mapInputs);
            int64_t nTxValueOut = tx.GetValueOut();

            if (tx.nVersion == ANON_TXN_VERSION)
            {
                // reject ring sig txs in blocks after deprecation fork
                if (pindex->nHeight >= FORK_HEIGHT_RINGSIG_DEPRECATION)
                    return DoS(100, error("ConnectBlock() : ring signature transactions deprecated after height %d", FORK_HEIGHT_RINGSIG_DEPRECATION));

                int64_t nSumAnon;
                if (!tx.CheckAnonInputs(txdb, nSumAnon, fInvalid, true))
                {
                    if (fInvalid)
                        return error("ConnectBlock() : CheckAnonInputs found invalid tx %s", tx.GetHash().ToString().substr(0,10).c_str());
                    return false;
                };

                nTxValueIn += nSumAnon;
            };

            if (tx.IsShielded())
            {
                if (tx.nValueBalance > 0)
                {
                    if (tx.nValueBalance > std::numeric_limits<int64_t>::max() - nTxValueIn)
                        return DoS(100, error("ConnectBlock() : shielded value balance overflow (unshield)"));
                    nTxValueIn += tx.nValueBalance;
                }
                else if (tx.nValueBalance < 0)
                {
                    if (tx.nValueBalance == std::numeric_limits<int64_t>::min())
                        return DoS(100, error("ConnectBlock() : shielded value balance INT64_MIN"));
                    int64_t nAbsBalance = -tx.nValueBalance;
                    if (nAbsBalance > std::numeric_limits<int64_t>::max() - nTxValueOut)
                        return DoS(100, error("ConnectBlock() : shielded value balance overflow (shield)"));
                    nTxValueOut += nAbsBalance;
                }
            }

            if (nTxValueIn > std::numeric_limits<int64_t>::max() - nValueIn)
                return DoS(100, error("ConnectBlock() : block value-in overflow"));
            if (nTxValueOut > std::numeric_limits<int64_t>::max() - nValueOut)
                return DoS(100, error("ConnectBlock() : block value-out overflow"));
            nValueIn += nTxValueIn;
            nValueOut += nTxValueOut;
            for (const CTxOut& out : tx.vout) {
              if(out.scriptPubKey.IsUnspendable())
                nAmountBurned += out.nValue;
            }
            if (!tx.IsCoinStake()) {
                nFees += nTxValueIn - nTxValueOut;
            }
            if (tx.IsCoinStake())
                nStakeReward = nTxValueOut - nTxValueIn;

            if (!tx.ConnectInputs(txdb, mapInputs, mapQueuedChanges, posThisTx, pindex, true, false, flags, true, fFCMPBatchVerified))
                return false;

            if (fHRegActive)
            {
                std::string strHRegError;
                if (!hregShadowState.ApplyConnectedTx(tx, mapInputs, pindex->nHeight, strHRegError))
                    return DoS(100, error("ConnectBlock() : HREG registration invalid: %s", strHRegError.c_str()));
            }

            int64_t nTxValidateMicros = GetTimeMicros() - nTxValidateStart;
            if (tx.IsCoinStake() && (tx.nVersion == SHIELDED_TX_VERSION_NULLSTAKE ||
                                     tx.nVersion == SHIELDED_TX_VERSION_NULLSTAKE_V2 ||
                                     tx.nVersion == SHIELDED_TX_VERSION_NULLSTAKE_COLD))
            {
                nPrivateStakeValidateMicros += nTxValidateMicros;
                nPrivateStakeValidateCount++;
            }
            else if (tx.IsShielded())
            {
                nShieldedValidateMicros += nTxValidateMicros;
                nShieldedValidateCount++;
            }
            else if (tx.nVersion == ANON_TXN_VERSION)
            {
                nAnonValidateMicros += nTxValidateMicros;
                nAnonValidateCount++;
            }
            else
            {
                nTransparentValidateMicros += nTxValidateMicros;
                nTransparentValidateCount++;
            }
        }

        mapQueuedChanges[hashTx] = CTxIndex(posThisTx, tx.vout.size());

        vPos.push_back(std::make_pair(tx.GetHash(), pos));
        // pos.nTxOffset += ::GetSerializeSize(tx, SER_DISK, CLIENT_VERSION);
        pos.nTxPos += ::GetSerializeSize(tx, SER_DISK, CLIENT_VERSION);
    }

    //int64_t nTime1 = GetTimeMicros(); nTimeConnect += nTime1 - nTimeStart;
    //LogPrint("bench", "      - Connect %u transactions: %.2fms (%.3fms/tx, %.3fms/txin) [%.2fs]\n", (unsigned)vtx.size(), 0.001 * (nTime1 - nTimeStart), 0.001 * (nTime1 - nTimeStart) / vtx.size(), nInputs <= 1 ? 0 : 0.001 * (nTime1 - nTimeStart) / (nInputs-1), nTimeConnect * 0.000001);

    // if (!control.Wait())
    //     return state.DoS(100, false);
    // int64_t nTime2 = GetTimeMicros(); nTimeVerify += nTime2 - nTimeStart;
    // LogPrint("bench", "    - Verify %u txins: %.2fms (%.3fms/txin) [%.2fs]\n", nInputs - 1, 0.001 * (nTime2 - nTimeStart), nInputs <= 1 ? 0 : 0.001 * (nTime2 - nTimeStart) / (nInputs-1), nTimeVerify * 0.000001);


    int64_t nFinalityRewardOut = 0;
    if (!vFinalityVotes.empty())
    {
        if (!CheckFinalityStakeProofsNotSpentInBlock(*this, vFinalityVotes))
            return DoS(100, error("ConnectBlock() : finality stake proof spent in including block"));
        if (!CheckFinalityVoteRewardOutputs(*this, vFinalityVotes, nFinalityRewardOut))
            return DoS(100, error("ConnectBlock() : finality vote reward outputs invalid"));
        for (const CFinalityVote& vote : vFinalityVotes)
        {
            std::string strVoteError;
            if (!g_finalityTracker.CheckVote(vote, txdb, &strVoteError))
                return DoS(100, error("ConnectBlock() : finality vote invalid: %s", strVoteError.c_str()));
        }
    }
    for (const CFinalityTallyCertificate& cert : vFinalityCerts)
    {
        std::string strCertError;
        if (!cert.IsValidBasic(&strCertError))
            return DoS(100, error("ConnectBlock() : finality tally certificate invalid: %s", strCertError.c_str()));
        if (GetEpochForHeight(cert.nHeight) != cert.nEpoch ||
            GetEpochBoundaryHeight(cert.nEpoch, cert.nHeight) != cert.nHeight)
            return DoS(100, error("ConnectBlock() : finality tally certificate has wrong epoch boundary"));
    }
    for (const CFinalityTallyShare& share : vFinalityShares)
    {
        std::string strShareError;
        if (!g_finalityTracker.CheckTallyShare(share, &strShareError, &vFinalityVotes))
            return DoS(100, error("ConnectBlock() : finality tally share invalid: %s", strShareError.c_str()));
    }

    if (IsProofOfWork())
    {
        // Historical compatibility: the original code used pindexBest->nHeight
        // (which equals pindex->nHeight - 1 during sequential block connection)
        // instead of the block's own height. The entire reward schedule was built
        // with this off-by-one behavior. Preserve it for historical blocks.
        int nRewardHeight = pindex->nHeight;
        if (pindex->nHeight < FORK_HEIGHT_TIGHTER_DRIFT && pindex->nHeight > 0)
            nRewardHeight = pindex->nHeight - 1;
        int64_t nReward = GetProofOfWorkReward(nRewardHeight, nFees);

        // Adaptive block size penalty (post-DAG): reduce allowed reward for oversized blocks
        nReward = ApplyBlockSizePenalty(nReward, *this, pindex->pprev);

        // Check coinbase reward
        if (nReward > MAX_MONEY - nFinalityRewardOut)
            return DoS(50, error("ConnectBlock() : finality reward overflow"));
        int64_t nAllowedCoinbase = nReward + nFinalityRewardOut;

        if (vtx[0].GetValueOut() > nAllowedCoinbase)
            return DoS(50, error("ConnectBlock() : coinbase reward exceeded (actual=%" PRId64" vs calculated=%" PRId64")",
                   vtx[0].GetValueOut(),
                   nAllowedCoinbase));
    }
    if (IsProofOfStake())
    {
        if (nStakeReward == 0 && !vtx[1].IsCoinStake())
            return DoS(100, error("ConnectBlock() : PoS block but vtx[1] is not coinstake"));

        if (vtx[1].nVersion == SHIELDED_TX_VERSION_NULLSTAKE_V2)
        {
            if (pindex->nHeight < FORK_HEIGHT_NULLSTAKE_V2)
                return DoS(100, error("ConnectBlock() : NullStake V2 coinstake before fork height"));

            if (vtx[1].nullstakeProofV2.IsNull())
                return DoS(100, error("ConnectBlock() : NullStake V2 kernel proof missing"));

            if (vtx[1].vShieldedSpend.empty())
                return DoS(100, error("ConnectBlock() : NullStake V2 coinstake has no shielded spends"));

            if (vtx[1].nullstakeProofV2.nTimeTx != nTime)
                return DoS(100, error("ConnectBlock() : NullStake V2 nTimeTx %" PRId64 " != block time %" PRId64,
                                       (int64_t)vtx[1].nullstakeProofV2.nTimeTx, (int64_t)nTime));

            if (pindex->pprev)
            {
                if (vtx[1].nullstakeProofV2.nStakeModifier != pindex->pprev->nStakeModifier)
                    return DoS(100, error("ConnectBlock() : NullStake V2 stake modifier mismatch (proof=0x%016" PRIx64 " chain=0x%016" PRIx64 ")",
                                           vtx[1].nullstakeProofV2.nStakeModifier, pindex->pprev->nStakeModifier));
            }

            if (!VerifyNullStakeKernelProofV2(vtx[1].nullstakeProofV2,
                                              vtx[1].vShieldedSpend[0].cv,
                                              nBits))
                return DoS(100, error("ConnectBlock() : NullStake V2 kernel proof invalid"));

            CCurveTree nullstakeTree;
            if (!txdb.ReadCurveTree(nullstakeTree))
                return DoS(100, error("ConnectBlock() : failed to read curve tree for NullStake V2"));
            nullstakeTree.RebuildParentNodes();

            CCurveTreeNode nullstakeRoot = nullstakeTree.GetRootNode();
            if (vtx[1].vShieldedSpend[0].fcmpProof.IsNull())
                return DoS(100, error("ConnectBlock() : NullStake V2 stake FCMP proof missing"));
            if (!VerifyFCMPProof(nullstakeRoot, vtx[1].vShieldedSpend[0].fcmpProof,
                                  vtx[1].vShieldedSpend[0].cv))
                return DoS(100, error("ConnectBlock() : NullStake V2 stake FCMP proof invalid"));

            uint64_t nCoinAge = 1;  // Minimum coin-day for V2
            int64_t nCalculatedStakeReward = ApplyBlockSizePenalty(GetProofOfStakeReward(nCoinAge, nFees), *this, pindex->pprev);
            if (nStakeReward > nCalculatedStakeReward)
                return DoS(100, error("ConnectBlock() : NullStake V2 coinstake pays too much(actual=%" PRId64" vs calculated=%" PRId64")", nStakeReward, nCalculatedStakeReward));
        }
        else if (vtx[1].nVersion == SHIELDED_TX_VERSION_NULLSTAKE_COLD)
        {
            if (pindex->nHeight < FORK_HEIGHT_NULLSTAKE_V3)
                return DoS(100, error("ConnectBlock() : NullStake V3 cold stake coinstake before fork height"));

            if (vtx[1].nullstakeProofV3.IsNull())
                return DoS(100, error("ConnectBlock() : NullStake V3 kernel proof missing"));

            if (vtx[1].vShieldedSpend.empty())
                return DoS(100, error("ConnectBlock() : NullStake V3 coinstake has no shielded spends"));

            if (vtx[1].nullstakeProofV3.acProof.GetProofSize() > BPAC_V3_MAX_PROOF_SIZE)
                return DoS(100, error("ConnectBlock() : NullStake V3 proof exceeds size limit (%u > %u)",
                                       (unsigned int)vtx[1].nullstakeProofV3.acProof.GetProofSize(),
                                       (unsigned int)BPAC_V3_MAX_PROOF_SIZE));

            if (vtx[1].nullstakeProofV3.nTimeTx != nTime)
                return DoS(100, error("ConnectBlock() : NullStake V3 nTimeTx %" PRId64 " != block time %" PRId64,
                                       (int64_t)vtx[1].nullstakeProofV3.nTimeTx, (int64_t)nTime));

            {
                uint256 zeroHash;
                memset(zeroHash.begin(), 0, 32);
                if (vtx[1].nullstakeProofV3.delegationHash == zeroHash)
                    return DoS(100, error("ConnectBlock() : NullStake V3 delegation hash is zero"));
            }

            if (vtx[1].nullstakeProofV3.vchPkStake.size() != 33)
                return DoS(100, error("ConnectBlock() : NullStake V3 pk_stake invalid size"));
            if (vtx[1].nullstakeProofV3.vchPkOwner.size() != 33)
                return DoS(100, error("ConnectBlock() : NullStake V3 pk_owner invalid size"));

            if (pindex->pprev)
            {
                if (vtx[1].nullstakeProofV3.nStakeModifier != pindex->pprev->nStakeModifier)
                    return DoS(100, error("ConnectBlock() : NullStake V3 stake modifier mismatch (proof=0x%016" PRIx64 " chain=0x%016" PRIx64 ")",
                                           vtx[1].nullstakeProofV3.nStakeModifier, pindex->pprev->nStakeModifier));
            }

            if (!VerifyNullStakeKernelProofV3(vtx[1].nullstakeProofV3,
                                              vtx[1].vShieldedSpend[0].cv,
                                              nBits))
                return DoS(100, error("ConnectBlock() : NullStake V3 kernel proof invalid"));

            CCurveTree nullstakeV3Tree;
            if (!txdb.ReadCurveTree(nullstakeV3Tree))
                return DoS(100, error("ConnectBlock() : failed to read curve tree for NullStake V3"));
            nullstakeV3Tree.RebuildParentNodes();

            CCurveTreeNode nullstakeV3Root = nullstakeV3Tree.GetRootNode();
            if (vtx[1].vShieldedSpend[0].fcmpProof.IsNull())
                return DoS(100, error("ConnectBlock() : NullStake V3 stake FCMP proof missing"));
            if (!VerifyFCMPProof(nullstakeV3Root, vtx[1].vShieldedSpend[0].fcmpProof,
                                  vtx[1].vShieldedSpend[0].cv))
                return DoS(100, error("ConnectBlock() : NullStake V3 stake FCMP proof invalid"));

            // V3 reward: same conservative approach as V2
            uint64_t nCoinAge = 1;
            int64_t nCalculatedStakeReward = ApplyBlockSizePenalty(GetProofOfStakeReward(nCoinAge, nFees), *this, pindex->pprev);
            if (nStakeReward > nCalculatedStakeReward)
                return DoS(100, error("ConnectBlock() : NullStake V3 coinstake pays too much(actual=%" PRId64" vs calculated=%" PRId64")", nStakeReward, nCalculatedStakeReward));
        }
        else if (vtx[1].nVersion == SHIELDED_TX_VERSION_NULLSTAKE)
        {
            if (pindex->nHeight < FORK_HEIGHT_NULLSTAKE)
                return DoS(100, error("ConnectBlock() : NullStake coinstake before fork height"));

            if (vtx[1].nullstakeProof.IsNull())
                return DoS(100, error("ConnectBlock() : NullStake kernel proof missing"));

            if (vtx[1].vShieldedSpend.empty())
                return DoS(100, error("ConnectBlock() : NullStake coinstake has no shielded spends"));

            if (vtx[1].nullstakeProof.nTimeTx != nTime)
                return DoS(100, error("ConnectBlock() : NullStake nTimeTx %" PRId64 " != block time %" PRId64,
                                       (int64_t)vtx[1].nullstakeProof.nTimeTx, (int64_t)nTime));

            if (vtx[1].nullstakeProof.nBlockTimeFrom >= vtx[1].nullstakeProof.nTimeTx)
                return DoS(100, error("ConnectBlock() : NullStake nBlockTimeFrom >= nTimeTx"));

            {
                int64_t nStakeAge = (int64_t)vtx[1].nullstakeProof.nTimeTx - (int64_t)vtx[1].nullstakeProof.nBlockTimeFrom;
                if (nStakeAge < nStakeMinAge)
                    return DoS(100, error("ConnectBlock() : NullStake stake age %" PRId64 " < minimum %" PRId64, nStakeAge, (int64_t)nStakeMinAge));
                if (nStakeAge > 365 * 24 * 60 * 60)
                    return DoS(50, error("ConnectBlock() : NullStake stake age %" PRId64 " exceeds 1 year", nStakeAge));
            }

            {
                bool fFoundBlockTime = false;
                CBlockIndex* pBlockFrom = NULL;
                CBlockIndex* pWalk = pindex->pprev;
                // increase lookback to cover nStakeMaxAge (90 days)
                // With 15s blocks: 90 days = 518,400 blocks. Use 600,000 for margin.
                // Was 1000 which only covered ~4.2 hours -- far less than 10-hour nStakeMinAge
                for (int i = 0; i < 600000 && pWalk != NULL; i++, pWalk = pWalk->pprev)
                {
                    if ((int64_t)pWalk->nTime == (int64_t)vtx[1].nullstakeProof.nBlockTimeFrom)
                    {
                        fFoundBlockTime = true;
                        pBlockFrom = pWalk;
                        break;
                    }
                }
                if (!fFoundBlockTime || pBlockFrom == NULL)
                    return DoS(100, error("ConnectBlock() : NullStake nBlockTimeFrom does not match any recent block"));

                // verify stake modifier matches chain state
                uint64_t nExpectedStakeModifier = 0;
                int nStakeModifierHeight = 0;
                int64_t nStakeModifierTime = 0;
                if (!GetKernelStakeModifier(pBlockFrom->GetBlockHash(), pindex->pprev, nExpectedStakeModifier,
                                            nStakeModifierHeight, nStakeModifierTime, false))
                    return DoS(100, error("ConnectBlock() : Failed to get stake modifier for NullStake proof"));

                if (vtx[1].nullstakeProof.nStakeModifier != nExpectedStakeModifier)
                    return DoS(100, error("ConnectBlock() : NullStake stake modifier mismatch (proof=0x%016" PRIx64 " chain=0x%016" PRIx64 ")",
                                          vtx[1].nullstakeProof.nStakeModifier, nExpectedStakeModifier));
            }

            int64_t nWeight = GetWeight((int64_t)vtx[1].nullstakeProof.nBlockTimeFrom,
                                         (int64_t)vtx[1].nullstakeProof.nTimeTx);

            if (!VerifyNullStakeKernelProof(vtx[1].nullstakeProof,
                                            vtx[1].vShieldedSpend[0].cv,
                                            nBits, nWeight))
                return DoS(100, error("ConnectBlock() : NullStake kernel proof invalid"));

            CCurveTree nullstakeTree;
            if (!txdb.ReadCurveTree(nullstakeTree))
                return DoS(100, error("ConnectBlock() : failed to read curve tree for NullStake"));
            nullstakeTree.RebuildParentNodes();

            CCurveTreeNode nullstakeRoot = nullstakeTree.GetRootNode();
            if (vtx[1].vShieldedSpend[0].fcmpProof.IsNull())
                return DoS(100, error("ConnectBlock() : NullStake stake FCMP proof missing"));
            if (!VerifyFCMPProof(nullstakeRoot, vtx[1].vShieldedSpend[0].fcmpProof,
                                  vtx[1].vShieldedSpend[0].cv))
                return DoS(100, error("ConnectBlock() : NullStake stake FCMP proof invalid"));

            uint64_t nCoinAge = nWeight > 0 ? (uint64_t)nWeight : 1;
            int64_t nCalculatedStakeReward = ApplyBlockSizePenalty(GetProofOfStakeReward(nCoinAge, nFees), *this, pindex->pprev);
            if (nStakeReward > nCalculatedStakeReward)
                return DoS(100, error("ConnectBlock() : NullStake coinstake pays too much(actual=%" PRId64" vs calculated=%" PRId64")", nStakeReward, nCalculatedStakeReward));
        }
        else
        {
            uint64_t nCoinAge;
            if (!vtx[1].GetCoinAge(txdb, nCoinAge))
                return error("ConnectBlock() : %s unable to get coin age for coinstake", vtx[1].GetHash().ToString().substr(0,10).c_str());

            int64_t nCalculatedStakeReward = ApplyBlockSizePenalty(GetProofOfStakeReward(nCoinAge, nFees), *this, pindex->pprev);

            if (nStakeReward > nCalculatedStakeReward)
                return DoS(100, error("ConnectBlock() : coinstake pays too much(actual=%" PRId64" vs calculated=%" PRId64")", nStakeReward, nCalculatedStakeReward));
        }

        // Reject shielded coinstake unless NullStake version is allowed
        if (pindex->nHeight >= FORK_HEIGHT_SHIELDED)
        {
            if (vtx[1].IsShielded())
            {
                // PRIV-AUDIT-3: After V2 fork, reject V1 proofs (they leak UTXO identity)
                bool fNullStakeAllowed = (vtx[1].nVersion == SHIELDED_TX_VERSION_NULLSTAKE && pindex->nHeight >= FORK_HEIGHT_NULLSTAKE && pindex->nHeight < FORK_HEIGHT_NULLSTAKE_V2)
                    || (vtx[1].nVersion == SHIELDED_TX_VERSION_NULLSTAKE_V2 && pindex->nHeight >= FORK_HEIGHT_NULLSTAKE_V2)
                    || (vtx[1].nVersion == SHIELDED_TX_VERSION_NULLSTAKE_COLD && pindex->nHeight >= FORK_HEIGHT_NULLSTAKE_V3);
                if (!fNullStakeAllowed)
                    return DoS(100, error("ConnectBlock() : shielded transaction cannot be coinstake (Layer 2)"));
            }

            // Reject coinstake inputs from shielded transactions
            if (vtx[1].nVersion != SHIELDED_TX_VERSION_NULLSTAKE && vtx[1].nVersion != SHIELDED_TX_VERSION_NULLSTAKE_V2 && vtx[1].nVersion != SHIELDED_TX_VERSION_NULLSTAKE_COLD)
            {
                MapPrevTx mapShieldedCheck;
                bool fShieldedInvalid = false;
                if (vtx[1].FetchInputs(txdb, mapQueuedChanges, true, false, mapShieldedCheck, fShieldedInvalid))
                {
                    for (unsigned int j = 0; j < vtx[1].vin.size(); j++)
                    {
                        const COutPoint& prevout = vtx[1].vin[j].prevout;
                        if (mapShieldedCheck.count(prevout.hash))
                        {
                            const CTransaction& txPrev = mapShieldedCheck[prevout.hash].second;
                            if (txPrev.IsShielded())
                                return DoS(100, error("ConnectBlock() : coinstake input from shielded transaction (Layer 3)"));
                        }
                    }
                }
            }
        }

        if (pindex->nHeight >= FORK_HEIGHT_COLD_STAKING)
        {
            CTransaction& coinstake = vtx[1];

            bool fHasColdStakeInput = false;
            CScript coldStakeScript;
            int64_t nP2CSInputValue = 0;

            MapPrevTx mapColdInputs;
            bool fInvalid = false;
            if (coinstake.FetchInputs(txdb, mapQueuedChanges, true, false, mapColdInputs, fInvalid))
            {
                for (unsigned int j = 0; j < coinstake.vin.size(); j++)
                {
                    const COutPoint& prevout = coinstake.vin[j].prevout;
                    if (mapColdInputs.count(prevout.hash))
                    {
                        const CTransaction& txPrev = mapColdInputs[prevout.hash].second;
                        if (prevout.n < txPrev.vout.size())
                        {
                            const CScript& prevScript = txPrev.vout[prevout.n].scriptPubKey;
                            if (IsPayToColdStaking(prevScript))
                            {
                                if (!fHasColdStakeInput)
                                {
                                    fHasColdStakeInput = true;
                                    coldStakeScript = prevScript;
                                }
                                else if (prevScript != coldStakeScript)
                                {
                                    return DoS(100, error("ConnectBlock() : cold stake inputs use different P2CS scripts"));
                                }
                                if (txPrev.vout[prevout.n].nValue < 0 || nP2CSInputValue + txPrev.vout[prevout.n].nValue < nP2CSInputValue)
                                    return DoS(100, error("ConnectBlock() : cold stake input value overflow"));
                                nP2CSInputValue += txPrev.vout[prevout.n].nValue;
                            }
                        }
                    }
                }
            }

            if (fHasColdStakeInput)
            {
                int64_t nP2CSOutputValue = 0;

                for (unsigned int i = 1; i < coinstake.vout.size(); i++)
                {
                    if (coinstake.vout[i].IsEmpty())
                        continue;

                    if (coinstake.vout[i].scriptPubKey == coldStakeScript)
                    {
                        if (coinstake.vout[i].nValue < 0 || nP2CSOutputValue + coinstake.vout[i].nValue < nP2CSOutputValue)
                            return DoS(100, error("ConnectBlock() : cold stake output value overflow"));
                        nP2CSOutputValue += coinstake.vout[i].nValue;
                        continue;
                    }

                    if (i == coinstake.vout.size() - 1 && !IsPayToColdStaking(coinstake.vout[i].scriptPubKey))
                    {
                        int64_t nCNPayment = coinstake.vout[i].nValue;
                        if (nCNPayment < 0 || !MoneyRange(nCNPayment))
                            return DoS(100, error("ConnectBlock() : cold stake CN payment out of range"));
                        if (nP2CSOutputValue > MAX_MONEY - nCNPayment)
                            return DoS(100, error("ConnectBlock() : cold stake total output overflow"));
                        int64_t nTotalOutput = nP2CSOutputValue + nCNPayment;
                        if (nTotalOutput < nP2CSInputValue)
                            return DoS(100, error("ConnectBlock() : cold stake output less than input"));
                        int64_t nReward = nTotalOutput - nP2CSInputValue;
                        if (!MoneyRange(nReward))
                            return DoS(100, error("ConnectBlock() : cold stake reward out of range"));

                        if (nReward > 0 && nCNPayment > 0 && (nCNPayment / 3 > nReward / 10 + 1))
                            return DoS(100, error("ConnectBlock() : cold stake CN payment %" PRId64 " exceeds 30%% of reward %" PRId64,
                                                  nCNPayment, nReward));

                        // Verify CN payment goes to a legitimate collateralnode (post-fork only)
                        if (nCNPayment > 0 && pindex->nHeight >= FORK_HEIGHT_CN_PAYMENT_VALIDATION)
                        {
                            CScript expectedPayee;
                            bool fValidCNPayee = false;

                            if (collateralnodePayments.GetBlockPayee(pindex->nHeight, expectedPayee))
                            {
                                fValidCNPayee = (coinstake.vout[i].scriptPubKey == expectedPayee);
                            }

                            // Fallback: check against active collateralnodes
                            if (!fValidCNPayee && vecCollateralnodes.size() > 0)
                            {
                                LOCK(cs_collateralnodes);
                                for (CCollateralNode& mn : vecCollateralnodes)
                                {
                                    if (mn.IsEnabled())
                                    {
                                        CScript mnPayee;
                                        mnPayee.SetDestination(mn.pubkey.GetID());
                                        if (coinstake.vout[i].scriptPubKey == mnPayee)
                                        {
                                            fValidCNPayee = true;
                                            break;
                                        }
                                    }
                                }
                            }

                            if (!fValidCNPayee)
                                return DoS(100, error("ConnectBlock() : cold stake CN payment to invalid payee (not a registered collateralnode)"));
                        }
                        continue;
                    }

                    return DoS(100, error("ConnectBlock() : cold stake output %u does not match P2CS input script", i));
                }

                if (nP2CSOutputValue < nP2CSInputValue)
                    return DoS(100, error("ConnectBlock() : cold stake P2CS output value (%" PRId64 ") less than input value (%" PRId64 ")",
                                          nP2CSOutputValue, nP2CSInputValue));
            }
        }
        else
        {
            const CTransaction& coinstake = vtx[1];
            for (unsigned int i = 0; i < coinstake.vout.size(); i++)
            {
                if (IsPayToColdStaking(coinstake.vout[i].scriptPubKey))
                    return DoS(100, error("ConnectBlock() : cold staking output not allowed before fork height %d", FORK_HEIGHT_COLD_STAKING));
            }
        }
    }

    bool CollateralnodePayments = false;
    bool fIsInitialDownload = IsInitialBlockDownload();

    if (fTestNet) {
        if (pindex->nHeight > BLOCK_START_COLLATERALNODE_PAYMENTS_TESTNET){
            CollateralnodePayments = true;
            if(fDebug) { printf("CheckBlock() : Collateralnode payments enabled\n"); }
        }else{
            CollateralnodePayments = false;
            if(fDebug) { printf("CheckBlock() : Collateralnode payments disabled\n"); }
        }
    } else {
        if (pindex->nHeight > BLOCK_START_COLLATERALNODE_PAYMENTS && pindex->nHeight > 2085000){
            CollateralnodePayments = true;
            if(fDebug) { printf("CheckBlock() : Collateralnode payments enabled\n"); }
        }else{
            CollateralnodePayments = false;
            if(fDebug) { printf("CheckBlock() : Collateralnode payments disabled\n"); }
        }
    }

    if(ShouldValidateCollateralnodePayments(pindex, fJustCheck, CollateralnodePayments == true))
    {
        LOCK2(cs_main, mempool.cs);

        CScript burnPayee;
        CBitcoinAddress burnDestination;
        burnDestination.SetString(fTestNet ? "8TestXXXXXXXXXXXXXXXXXXXXXXXXbCvpq" : "INNXXXXXXXXXXXXXXXXXXXXXXXXXZeeDTw");
        burnPayee = GetScriptForDestination(burnDestination.Get());

        // Network-aware CN enforcement height
        int nCNEnforcementHeight = fTestNet ? MN_ENFORCEMENT_ACTIVE_HEIGHT_TESTNET : MN_ENFORCEMENT_ACTIVE_HEIGHT;

        if(IsProofOfStake() && pindexBest != NULL){
            // (reward goes entirely to shielded pool, no MN payment)
            if (vtx[1].nVersion == SHIELDED_TX_VERSION_NULLSTAKE || vtx[1].nVersion == SHIELDED_TX_VERSION_NULLSTAKE_V2 || vtx[1].nVersion == SHIELDED_TX_VERSION_NULLSTAKE_COLD)
            {
                // NullStake/V3: no collateralnode payments
            }
            else if(pindexBest->GetBlockHash() == hashPrevBlock){

                // make sure the ranks are updated to prev block
                GetCollateralnodeRanks(pindexBest);
                // Calculate Coin Age for Collateralnode Reward Calculation
                uint64_t nCoinAge;
                if (!vtx[1].GetCoinAge(txdb, nCoinAge))
                    return error("CheckBlock-POS : %s unable to get coin age for coinstake, Can't Calculate Collateralnode Reward\n", vtx[1].GetHash().ToString().substr(0,10).c_str());
                int64_t nCalculatedStakeReward = ApplyBlockSizePenalty(GetProofOfStakeReward(nCoinAge, nFees), *this, pindex->pprev);

                // Calculate expected collateralnodePaymentAmmount
                int64_t collateralnodePaymentAmount = GetCollateralnodePayment(pindex->nHeight, nCalculatedStakeReward);

                // If we don't already have its previous block, skip collateralnode payment step
                if (pindex != NULL)
                {
                    bool foundPaymentAmount = false;
                    bool foundPayee = false;
                    bool paymentOK = false;

                    CScript payee;
                    if(fDebug) { printf("CheckBlock-POS() : Using collateralnode payments for block %ld\n", pindex->nHeight); }

                    // Check transaction for payee and if contains collateralnode reward payment
                    if(fDebug) { printf("CheckBlock-POS(): Transaction 1 Size : %i\n", vtx[1].vout.size()); }
                    if(fDebug) { printf("CheckBlock-POS() : Expected Collateralnode reward of: %ld\n", collateralnodePaymentAmount); }
                    for (unsigned int i = 0; i < vtx[1].vout.size(); i++) {
                        if(fDebug) { printf("CheckBlock-POS() : Payment vout number: %i , Amount: %ld\n",i, vtx[1].vout[i].nValue); }
                        if(vtx[1].vout[i].nValue == collateralnodePaymentAmount )
                        {
                            foundPaymentAmount = true;
                            payee = vtx[1].vout[i].scriptPubKey;
                            CScript pubScript;

                            if (pubScript == payee) {
                                printf("CheckBlock-POS() : Found collateralnode payment: %s INN to anonymous payee.\n", FormatMoney(vtx[1].vout[i].nValue).c_str());
                                foundPayee = true;
                            } else if (payee == burnPayee) {
                                printf("CheckBlock-POS() : Found collateralnode payment: %s INN to burn address.\n", FormatMoney(vtx[1].vout[i].nValue).c_str());
                                foundPayee = true;
                            } else {
                                CTxDestination mnDest;
                                ExtractDestination(vtx[1].vout[i].scriptPubKey, mnDest);
                                CBitcoinAddress mnAddress(mnDest);
                                if (fDebug) printf("CheckBlock-POS() : Found collateralnode payment: %s INN to %s.\n",FormatMoney(vtx[1].vout[i].nValue).c_str(), mnAddress.ToString().c_str());
                                for (CCollateralNode& mn : vecCollateralnodes)
                                {
                                    pubScript = GetScriptForDestination(mn.pubkey.GetID());
                                    CTxDestination address1;
                                    ExtractDestination(pubScript, address1);
                                    CBitcoinAddress address2(address1);

                                    if (vtx[1].vout[i].scriptPubKey == pubScript)
                                    {
                                        int64_t value = vtx[1].vout[i].nValue;
                                        if (fDebug) printf("CheckBlock-POS() : Collateralnode PoS payee found at block %d: %s who got paid %s INN rate:%" PRId64" rank:%d lastpaid:%d\n", pindex->nHeight, address2.ToString().c_str(), FormatMoney(value).c_str(), mn.payRate, mn.nRank, mn.nBlockLastPaid);

                                        if (!fIsInitialDownload) {
                                            if (!CheckPoSCNPayment(pindex, vtx[1].vout[i].nValue, mn)) // CheckPoSCNPayment()
                                            {
                                                if (pindex->nHeight >= nCNEnforcementHeight) {
                                                    printf("CheckBlock-POS() : Out-of-cycle CollateralNode payment detected, rejecting block. rank:%d value:%s avg:%s payRate:%s payCount:%d\n",mn.nRank,FormatMoney(mn.payValue).c_str(),FormatMoney(nAverageCNIncome).c_str(),FormatMoney(mn.payRate).c_str(), mn.payCount);
                                                } else {
                                                    printf("CheckBlock-POS(): This collateralnode payment is too aggressive and will be accepted after block %d\n", nCNEnforcementHeight);
                                                }
                                                //break;
                                            } else {
                                                if (fDebug) printf("CheckBlock-POS() : Payment meets rate requirement: payee has earnt %s against average %s\n",FormatMoney(mn.payValue).c_str(),FormatMoney(nAverageCNIncome).c_str());
                                            }
                                        } else {
                                            if (fDebug) printf("CheckBlock-POS() : Wallet currently in startup mode, ignoring rate requirements.");
                                        }
                                        // add mn payment data
                                        mn.nBlockLastPaid = pindex->nHeight;
                                        CCollateralNPayData data;
                                        data.height = pindex->nHeight;
                                        data.amount = value;
                                        data.hash = pindex->GetBlockHash();
                                        mn.payData.push_back(data);
                                        mn.SetPayRate(pindex->nHeight);
                                        nCNValidationPayeeFound.fetch_add(1, std::memory_order_relaxed);
                                        foundPayee = true;
                                        paymentOK = true;
                                        break;
                                    }
                                }
                                // if payee not found in mn list, check if the pubkey holds a 5K transaction
                                if (!foundPayee) {
                                    nCNValidationPayeeMissing.fetch_add(1, std::memory_order_relaxed);
                                    nCNValidationFindCNPaymentEntered.fetch_add(1, std::memory_order_relaxed);
                                    if (FindCNPayment(payee, pindex)) {
                                        if (fDebug) printf("CheckBlock-POS() : WARNING: Payee was not found in MN list, but confirmed to hold collateral.\n");
                                        nCNValidationPayeeFound.fetch_add(1, std::memory_order_relaxed);
                                        foundPayee = true;
                                    }
                                }
                            }
                        }
                    }



                    if (!foundPayee) {
                        if (pindex->nHeight >= nCNEnforcementHeight) {
                                    LOCK(cs_vNodes);
                                    for (CNode* pnode : vNodes)
                                    {
                                        if (pnode->nVersion >= colLateralPool.PROTOCOL_VERSION) {
                                                printf("Asking for Collateralnode list from %s\n",pnode->addr.ToStringIPPort().c_str());
                                                pnode->PushMessage("iseg", CTxIn()); //request full mn list
                                                pnode->nLastDseg = GetTime();
                                        }
                                    }
                            if (SyncTraceEnabled())
                                printf("SYNC_EVENT time_us=%lld event=CHECKBLOCK_POS_PAYEE_REJECT hash=%s height=%d\n",
                                       (long long)GetTimeMicros(),
                                       GetHash().ToString().c_str(),
                                       pindex ? pindex->nHeight : -1);
                            return error("CheckBlock-POS() : Did not find this payee in the collateralnode list. Requesting list update and rejecting block.");
                        } else {
                            if (fDebug) printf("WARNING: Did not find this payee in the collateralnode list, this block will not be accepted after block %d\n", nCNEnforcementHeight);
                            foundPayee = true;
                        }
                    } else if (paymentOK) {
                        if (pindex->nHeight >= nCNEnforcementHeight) {
                            if (fDebug) printf("CheckBlock-POS() : This payment has been determined as legitimate, and will be allowed.\n");
                        } else {
                            if (fDebug) printf("CheckBlock-POS() : This payment has been determined as legitimate, and will be allowed after block %d.\n", nCNEnforcementHeight);
                        }
                    }

                    if(!(foundPaymentAmount && foundPayee)) {
                        CTxDestination address1;
                        ExtractDestination(payee, address1);
                        CBitcoinAddress address2(address1);
                        if(fDebug) { printf("CheckBlock-POS() : Couldn't find collateralnode payment(%d|%ld) or payee(%d|%s) nHeight %d. \n", foundPaymentAmount, collateralnodePaymentAmount, foundPayee, address2.ToString().c_str(), pindex->nHeight+1); }
                        return DoS(100, error("CheckBlock-POS() : Couldn't find collateralnode payment or payee"));
                    } else {
                        if(fDebug) { printf("CheckBlock-POS() : Found collateralnode payment %d\n", pindex->nHeight+1); }
                    }
                } else {
                    if(fDebug) { printf("CheckBlock-POS() : Is initial download, skipping collateralnode payment check %ld\n", pindexBest->nHeight+1); }
                }
            } else {
                if(fDebug) { printf("CheckBlock-POS() : Skipping collateralnode payment check - nHeight %ld Hash %s\n", pindex->nHeight, GetHash().ToString().c_str()); }
            }
        }else if(IsProofOfWork() && pindexBest != NULL){
            if(pindexBest->GetBlockHash() == hashPrevBlock){

                int64_t collateralnodePaymentAmount = GetCollateralnodePayment(pindex->nHeight, vtx[0].GetValueOut());

                // If we don't already have its previous block, skip collateralnode payment step
                if (pindex != NULL)
                {
                    bool foundPaymentAmount = false;
                    bool foundPayee = false;
                    bool paymentOK = true;
                    CScript payee;

                    {
                        // Collateral nodes can be added, removed, or updated by the
                        // networking thread while a block is being connected. Keep
                        // ranking, payee validation, and payment accounting on one
                        // stable view of the list.
                        LOCK(cs_collateralnodes);

                        // make sure the ranks are updated
                        GetCollateralnodeRanks(pindexBest);

                        if(fDebug) { printf("CheckBlock-POW() : Using non-specific collateralnode payments %ld\n", pindex->nHeight); }

                    // Check transaction for payee and if contains collateralnode reward payment
                    if (fDebug) { printf("CheckBlock-POW(): Transaction 0 Size : %i\n", vtx[0].vout.size()); }
                    if (fDebug) { printf("CheckBlock-POW() : Expected Collateralnode reward of: %ld\n", collateralnodePaymentAmount); }
                    for (unsigned int i = 0; i < vtx[0].vout.size(); i++) {
                        if(fDebug) { printf("CheckBlock-POW() : Payment vout number: %i , Amount: %lld\n",i, vtx[0].vout[i].nValue); }
                        if(vtx[0].vout[i].nValue == collateralnodePaymentAmount )
                        {
                            CTxDestination mnDest;
                            payee = vtx[0].vout[i].scriptPubKey;
                            ExtractDestination(payee, mnDest);
                            CBitcoinAddress mnAddress(mnDest);
                            if (fDebug) printf("CheckBlock-POW() : Found collateralnode payment: %s INN to %s.\n",FormatMoney(vtx[0].vout[i].nValue).c_str(), mnAddress.ToString().c_str());

                            foundPaymentAmount = true;

                            CScript pubScript;

                            for (CCollateralNode& mn : vecCollateralnodes)
                            {
                                pubScript = GetScriptForDestination(mn.pubkey.GetID());
                                CTxDestination address1;
                                ExtractDestination(pubScript, address1);
                                CBitcoinAddress address2(address1);

                                if (payee == pubScript)
                                {
                                    if (fDebug) printf("CheckBlock-POW() : Collateralnode PoW payee found at block %d: %s who got paid %s INN rate:%" PRId64" rank:%d lastpaid:%d\n", pindex->nHeight, address2.ToString().c_str(), FormatMoney(vtx[0].vout[i].nValue).c_str(), FormatMoney(mn.payRate).c_str(), mn.nRank, mn.nBlockLastPaid);
                                    if (!fIsInitialDownload) {
                                        if (!CheckCNPayment(pindex, vtx[0].vout[i].nValue, mn)) // if MN is being paid and it's bottom 50% ranked, don't let it be paid.
                                        {
                                            if (pindex->nHeight >= nCNEnforcementHeight)
                                            {
                                                printf("CheckBlock-POW() : Collateralnode overpayment detected, rejecting block. rank:%d value:%s avg:%s payRate:%s payCount:%d\n",mn.nRank,FormatMoney(mn.payValue).c_str(),FormatMoney(nAverageCNIncome).c_str(),FormatMoney(mn.payRate).c_str(), mn.payCount);
                                            } else {
                                                printf("WARNING: This collateralnode payment is too aggressive and will not be accepted after block %d\n", nCNEnforcementHeight);
                                            }
                                            //break;
                                        } else {
                                            if (fDebug) printf("CheckBlock-POW() : Payment meets rate requirement: payee has earnt %s against average %s\n",FormatMoney(mn.payValue).c_str(),FormatMoney(nAverageCNIncome).c_str());
                                        }
                                    } else {
                                        if (fDebug) printf("CheckBlock-POW() : Wallet currently in startup mode, ignoring rate requirements.");
                                    }

                                    mn.nBlockLastPaid = pindex->nHeight;
                                    CCollateralNPayData data;
                                    data.height = pindex->nHeight;
                                    data.amount = vtx[0].vout[i].nValue;
                                    data.hash = pindex->GetBlockHash();
                                    mn.payData.push_back(data);
                                    mn.SetPayRate(pindex->nHeight);
                                    nCNValidationPayeeFound.fetch_add(1, std::memory_order_relaxed);
                                    foundPayee = true;
                                    paymentOK = true;
                                    break;
                                } else if (payee == burnPayee) {
                                    printf("CheckBlock-POW() : Found collateralnode payment: %s INN to burn address.\n", FormatMoney(vtx[0].vout[i].nValue).c_str());
                                    nCNValidationPayeeFound.fetch_add(1, std::memory_order_relaxed);
                                    foundPayee = true;
                                }
                            }

                            // if payee not found in mn list, check if the pubkey holds a 5K transaction
                            if (!foundPayee) {
                                nCNValidationPayeeMissing.fetch_add(1, std::memory_order_relaxed);
                                nCNValidationFindCNPaymentEntered.fetch_add(1, std::memory_order_relaxed);
                                if (FindCNPayment(payee, pindex)) {
                                    if (fDebug) printf("CheckBlock-POW() : WARNING: Payee was not found in MN list, but confirmed to hold collateral.\n");
                                    nCNValidationPayeeFound.fetch_add(1, std::memory_order_relaxed);
                                    foundPayee = true;
                                }
                            }
                        }
                    }
                    }

                    if (!foundPayee) {
                        if (pindex->nHeight >= nCNEnforcementHeight) {
                                LOCK(cs_vNodes);
                                for (CNode* pnode : vNodes)
                                {
                                    if (pnode->nVersion >= colLateralPool.PROTOCOL_VERSION) {
                                            printf("Asking for Collateralnode list from %s\n",pnode->addr.ToStringIPPort().c_str());
                                            pnode->PushMessage("iseg", CTxIn()); //request full mn list
                                            pnode->nLastDseg = GetTime();
                                    }
                                }
                                if (SyncTraceEnabled())
                                    printf("SYNC_EVENT time_us=%lld event=CHECKBLOCK_POW_PAYEE_REJECT hash=%s height=%d\n",
                                           (long long)GetTimeMicros(),
                                           GetHash().ToString().c_str(),
                                           pindex ? pindex->nHeight : -1);
                                return error("CheckBlock-POW() : Did not find this payee in the collateralnode list, rejecting block.");
                        } else {
                            if (fDebug) printf("WARNING: Did not find this payee in  the collateralnode list, this block will not be accepted after block %d\n", nCNEnforcementHeight);
                            foundPayee = true;
                        }
                    } else if (paymentOK) {
                        if (pindex->nHeight >= nCNEnforcementHeight) {
                            if (fDebug) printf("CheckBlock-POW() : This payment has been determined as legitimate, and will be allowed.\n");
                        } else {
                            if (fDebug) printf("CheckBlock-POW() : This payment has been determined as legitimate, and will be allowed after block %d.\n", nCNEnforcementHeight);
                        }
                    }

                    if(fDebug) {printf("CheckBlock-POW(): foundPaymentAmount= %i ; foundPayee = %i\n", foundPaymentAmount, foundPayee); }
                    if(!(foundPaymentAmount && foundPayee)) {
                        CScript payee;
                        CTxDestination address1;
                        ExtractDestination(payee, address1);
                        CBitcoinAddress address2(address1);
                        if(fDebug) { printf("CheckBlock-POW() : Couldn't find collateralnode payment(%d|%ld) or payee(%d|%s) nHeight %d. \n", foundPaymentAmount, collateralnodePaymentAmount, foundPayee, address2.ToString().c_str(), pindex->nHeight+1); }
                        return DoS(100, error("CheckBlock-POW() : Couldn't find collateralnode payment or payee"));
                    } else {
                        if(fDebug) { printf("CheckBlock-POW() : Found collateralnode payment %d\n", pindex->nHeight+1); }
                    }
                } else {
                    if(fDebug) { printf("CheckBlock-POW() : Is initial download, skipping collateralnode payment check %d\n", pindex->nHeight+1); }
                }
            } else {
                if(fDebug) { printf("CheckBlock-POW() : Skipping collateralnode payment check - nHeight %d Hash %s\n", pindex->nHeight+1, GetHash().ToString().c_str()); }
            }
        }

         else {
            if(fDebug) { printf("CheckBlock() : pindex is null, skipping collateralnode payment check\n"); }
        }
    } else {
        if(fDebug) {
                printf("CheckBlock() : skipping collateralnode payment checks\n");
        }
    }

    if (pindex->nHeight >= FORK_HEIGHT_SHIELDED && !fJustCheck)
    {
        CIncrementalMerkleTree shieldedTree;
        if (pindex->pprev)
            txdb.ReadShieldedTree(shieldedTree); // OK if not found (empty tree)

        txdb.WriteShieldedTreeAtBlock(pindex->GetBlockHash(), shieldedTree);

        CCurveTree curveTree;
        bool fMutableCurveTree = (pindex->nHeight >= FORK_HEIGHT_FCMP &&
                                  pindex->nHeight < FORK_HEIGHT_EPOCH_ROOT_FCMP);
        if (fMutableCurveTree && pindex->pprev)
            txdb.ReadCurveTree(curveTree); // OK if not found (empty)

        if (fMutableCurveTree)
            txdb.WriteCurveTreeAtBlock(pindex->GetBlockHash(), curveTree);

        // Seed genesis commitments at the fork activation block
        // These provide the initial Lelantus anonymity set (16 unspendable decoys)
        if (pindex->nHeight == FORK_HEIGHT_SHIELDED)
        {
            CCurveTree* pCurveTreePtr = fMutableCurveTree ? &curveTree : nullptr;
            if (!SeedGenesisCommitments(txdb, shieldedTree, pCurveTreePtr))
                return error("ConnectBlock() : SeedGenesisCommitments failed");
        }

        int64_t nShieldedPool = 0;
        txdb.ReadShieldedPoolValue(nShieldedPool); // OK if not found (zero)

        // Catch cross-tx nullifier duplicates within this block
        std::set<uint256> setBlockNullifiers;

        for (const CTransaction& tx : vtx)
        {
            if (!tx.IsShielded())
                continue;

            for (unsigned int i = 0; i < tx.vShieldedSpend.size(); i++)
            {
                if (!setBlockNullifiers.insert(tx.vShieldedSpend[i].nullifier).second)
                    return error("ConnectBlock() : duplicate nullifier %s across transactions in block",
                                 tx.vShieldedSpend[i].nullifier.ToString().substr(0,10).c_str());

                CShieldedNullifierSpent nfs;
                nfs.txnHash = tx.GetHash();
                nfs.nIndex = i;
                if (!txdb.WriteShieldedNullifier(tx.vShieldedSpend[i].nullifier, nfs))
                    return error("ConnectBlock() : WriteShieldedNullifier failed");
            }

            // Append note commitments to the Merkle tree and commitment index
            for (const CShieldedOutputDescription& output : tx.vShieldedOutput)
            {
                shieldedTree.Append(output.cmu);

                // Index Pedersen commitment for Lelantus anonymity set construction
                uint64_t nCommitIdx = shieldedTree.Size() - 1;
                if (!txdb.WriteShieldedCommitment(nCommitIdx, output.cv))
                    return error("ConnectBlock() : WriteShieldedCommitment failed");

                if (!txdb.WriteShieldedCommitmentHeight(nCommitIdx, pindex->nHeight))
                    return error("ConnectBlock() : WriteShieldedCommitmentHeight failed");
                // Reverse index for spend validation
                if (!txdb.WriteShieldedCommitmentIndex(output.cv.vchCommitment, nCommitIdx))
                    return error("ConnectBlock() : WriteShieldedCommitmentIndex failed");

                if (fMutableCurveTree)
                    curveTree.InsertLeaf(output.cv);
            }

            nShieldedPool -= tx.nValueBalance;

            if (nShieldedPool < 0)
                return error("ConnectBlock() : shielded pool would go negative (%" PRId64 "), inflation detected", nShieldedPool);
        }

        if (!txdb.WriteShieldedTree(shieldedTree))
            return error("ConnectBlock() : WriteShieldedTree failed");
        if (!txdb.WriteShieldedCommitmentCount(shieldedTree.Size()))
            return error("ConnectBlock() : WriteShieldedCommitmentCount failed");

        if (fMutableCurveTree)
        {
            if (!txdb.WriteCurveTree(curveTree))
                return error("ConnectBlock() : WriteCurveTree failed");
        }

        uint256 treeRoot = shieldedTree.Root();
        if (!txdb.WriteShieldedAnchor(treeRoot))
            return error("ConnectBlock() : WriteShieldedAnchor failed");
        // Only write anchor height for NEW anchors (don't reset age each block)
        {
            int nExistingHeight = 0;
            if (!txdb.ReadShieldedAnchorHeight(treeRoot, nExistingHeight))
            {
                if (!txdb.WriteShieldedAnchorHeight(treeRoot, pindex->nHeight))
                    return error("ConnectBlock() : WriteShieldedAnchorHeight failed");
            }
        }

        if (!txdb.WriteShieldedPoolValue(nShieldedPool))
            return error("ConnectBlock() : WriteShieldedPoolValue failed");

        nShieldedPoolValue = nShieldedPool;

        if (fDebug)
            printf("ConnectBlock() : shielded tree root=%s, pool=%" PRId64 "\n",
                   treeRoot.ToString().substr(0,10).c_str(), nShieldedPool);
    }

    // ppcoin: track money supply and mint amount info
    pindex->nMint = nValueOut - nValueIn + nFees;
    pindex->nMoneySupply = (pindex->pprev? pindex->pprev->nMoneySupply : 0) + nValueOut - nValueIn;
    pindex->nMoneySupply -= nAmountBurned;
    if (pindex->nMoneySupply < 0)
        return error("ConnectBlock() : negative money supply at height %d", pindex->nHeight);

    if (!fJustCheck && !vFinalityVotes.empty())
    {
        if (!g_finalityTracker.ConnectBlockVotes(txdb, pindex->GetBlockHash(), vFinalityVotes))
            return error("ConnectBlock() : ConnectBlockVotes failed");
    }
    if (!fJustCheck && !vFinalityShares.empty())
    {
        if (!g_finalityTracker.ConnectBlockTallyShares(txdb, pindex->GetBlockHash(), vFinalityShares))
            return error("ConnectBlock() : ConnectBlockTallyShares failed");
    }
    if (!fJustCheck && !vFinalityCerts.empty())
    {
        if (!g_finalityTracker.ConnectBlockTallyCertificates(txdb, pindex->GetBlockHash(), vFinalityCerts))
            return error("ConnectBlock() : ConnectBlockTallyCertificates failed");
    }

    // innova: collect valid name tx
    // NOTE: tx.UpdateCoins should not affect this loop, probably...
    // vector<nameTempProxy> vName;
    // for (unsigned int i=0; i<vtx.size(); i++)
    // {
    //     if (fDebug) printf("ConnectBlock() for Name Index\n");
    //     const CTransaction &tx = vtx[i];
    //     //if (!tx.IsCoinBase()) //|| !tx.IsCoinStake()
    //     //hooks->CheckInputs(tx, pindex, vName, vPos[i].second, vFees[i]); // collect valid name tx to vName
    //     // hooks->CheckInputs(txdb, mapTestPool, tx, vPos[i].second, pindexBlock)
    // }

    if (!txdb.WriteBlockIndex(CDiskBlockIndex(pindex)))
        return error("Connect() : WriteBlockIndex for pindex failed");

    if (fJustCheck)
    {
        if (fDebug && GetBoolArg("-showtimers", false))
            printf("ConnectBlock: height=%d justcheck total=%" PRId64"ms check=%" PRId64"ms tx_transparent=%u/%" PRId64"us tx_shielded=%u/%" PRId64"us tx_anon=%u/%" PRId64"us tx_privstake=%u/%" PRId64"us\n",
                   pindex->nHeight, GetTimeMillis() - nConnectBlockStart, nConnectCheckMs,
                   nTransparentValidateCount, nTransparentValidateMicros,
                   nShieldedValidateCount, nShieldedValidateMicros,
                   nAnonValidateCount, nAnonValidateMicros,
                   nPrivateStakeValidateCount, nPrivateStakeValidateMicros);
        return true;
    }

    // Write queued txindex changes
    for (map<uint256, CTxIndex>::iterator mi = mapQueuedChanges.begin(); mi != mapQueuedChanges.end(); ++mi)
    {
        if (!txdb.UpdateTxIndex((*mi).first, (*mi).second))
            return error("ConnectBlock() : UpdateTxIndex failed");
    }
    if(GetBoolArg("-addrindex", false))
    {
        // Write Address Index
        for (CTransaction& tx : vtx)
        {
            uint256 hashTx = tx.GetHash();
        // inputs
        if(!tx.IsCoinBase())
        {
                MapPrevTx mapInputs;
            map<uint256, CTxIndex> mapQueuedChangesT;
            bool fInvalid;
                if (!tx.FetchInputs(txdb, mapQueuedChangesT, true, false, mapInputs, fInvalid))
                    return false;

            MapPrevTx::const_iterator mi;
            for(MapPrevTx::const_iterator mi = mapInputs.begin(); mi != mapInputs.end(); ++mi)
            {
                for (const CTxOut &atxout : (*mi).second.second.vout)
                {
                std::vector<uint160> addrIds;
                if(BuildAddrIndex(atxout.scriptPubKey, addrIds))
                {
                        for (uint160 addrId : addrIds)
                        {
                        if(!txdb.WriteAddrIndex(addrId, hashTx))
                            printf("ConnectBlock(): txins WriteAddrIndex failed addrId: %s txhash: %s\n", addrId.ToString().c_str(), hashTx.ToString().c_str());
                        }
                }
                }
            }

            }

        // outputs
        for (const CTxOut &atxout : tx.vout) {
            std::vector<uint160> addrIds;
                if(BuildAddrIndex(atxout.scriptPubKey, addrIds))
            {
            for (uint160 addrId : addrIds)
            {
                if(!txdb.WriteAddrIndex(addrId, hashTx))
                    printf("ConnectBlock(): txouts WriteAddrIndex failed addrId: %s txhash: %s\n", addrId.ToString().c_str(), hashTx.ToString().c_str());
                    }
            }
        }
        }
    }

    // Update block index on disk without changing it in memory.
    // The memory index structure will be changed after the db commits.
    if (pindex->pprev)
    {
        CDiskBlockIndex blockindexPrev(pindex->pprev);
        blockindexPrev.hashNext = pindex->GetBlockHash();
        if (!txdb.WriteBlockIndex(blockindexPrev))
            return error("ConnectBlock() : WriteBlockIndex failed");
    }

    // Check Name Release Height to Connect Blocks
    if (pindex->nHeight >= RELEASE_HEIGHT) {
        // add names to innovanamesindex.dat
        hooks->ConnectBlock(txdb, pindex);
    }

    if (fHRegActive)
        hreg::g_registry = hregShadowState;

    // Watch for transactions paying to me
    for (CTransaction& tx : vtx)
        SyncWithWallets(tx, this, true);

    // update the UI about the new block
    uiInterface.NotifyRanksUpdated();

    if (fDebug && GetBoolArg("-showtimers", false))
        printf("ConnectBlock: height=%d total=%" PRId64"ms check=%" PRId64"ms tx_transparent=%u/%" PRId64"us tx_shielded=%u/%" PRId64"us tx_anon=%u/%" PRId64"us tx_privstake=%u/%" PRId64"us\n",
               pindex->nHeight, GetTimeMillis() - nConnectBlockStart, nConnectCheckMs,
               nTransparentValidateCount, nTransparentValidateMicros,
               nShieldedValidateCount, nShieldedValidateMicros,
               nAnonValidateCount, nAnonValidateMicros,
               nPrivateStakeValidateCount, nPrivateStakeValidateMicros);

    return true;
}

bool static Reorganize(CTxDB& txdb, CBlockIndex* pindexNew)
{
    printf("REORGANIZE\n");

    // Operator-invalidation gate (defense in depth; SetBestChain precedes every
    // Reorganize call, but guard here too for direct callers).
    if (IsBlockOperatorInvalid(pindexNew))
        return error("Reorganize() : block %s (or an ancestor) is invalidated by the operator",
                     pindexNew->GetBlockHash().ToString().substr(0, 20).c_str());

    {
        int nFinalHeight = g_finalityTracker.GetFinalizedHeight();
        if (nFinalHeight > 0 && pindexBest && pindexBest->nHeight >= FORK_HEIGHT_FINALITY)
        {
            CBlockIndex* pCheck = pindexBest;
            CBlockIndex* pLonger = pindexNew;
            while (pCheck != pLonger)
            {
                while (pLonger && pLonger->nHeight > pCheck->nHeight)
                    pLonger = pLonger->pprev;
                if (pCheck == pLonger)
                    break;
                if (pCheck)
                    pCheck = pCheck->pprev;
            }
            if (pCheck && pCheck->nHeight < nFinalHeight)
            {
                return error("Reorganize() : rejected - fork point height %d is below finalized height %d",
                             pCheck->nHeight, nFinalHeight);
            }
        }
    }

    // Find the fork
    CBlockIndex* pfork = pindexBest;
    CBlockIndex* plonger = pindexNew;
    while (pfork != plonger)
    {
        while (plonger->nHeight > pfork->nHeight)
            if (!(plonger = plonger->pprev))
                return error("Reorganize() : plonger->pprev is null");
        if (pfork == plonger)
            break;
        if (!(pfork = pfork->pprev))
            return error("Reorganize() : pfork->pprev is null");
    }

    // List of what to disconnect
    vector<CBlockIndex*> vDisconnect;
    for (CBlockIndex* pindex = pindexBest; pindex != pfork; pindex = pindex->pprev)
        vDisconnect.push_back(pindex);

    // List of what to connect
    vector<CBlockIndex*> vConnect;
    for (CBlockIndex* pindex = pindexNew; pindex != pfork; pindex = pindex->pprev)
        vConnect.push_back(pindex);
    reverse(vConnect.begin(), vConnect.end());

    printf("REORGANIZE: Disconnect %" PRIszu" blocks; %s..%s\n", vDisconnect.size(), pfork->GetBlockHash().ToString().substr(0,20).c_str(), pindexBest->GetBlockHash().ToString().substr(0,20).c_str());
    printf("REORGANIZE: Connect %" PRIszu" blocks; %s..%s\n", vConnect.size(), pfork->GetBlockHash().ToString().substr(0,20).c_str(), pindexNew->GetBlockHash().ToString().substr(0,20).c_str());

    if (SyncTraceEnabled())
    {
        std::vector<uint256> vDisconnectHashes;
        vDisconnectHashes.reserve(vDisconnect.size());
        for (CBlockIndex* pindex : vDisconnect)
            vDisconnectHashes.push_back(pindex->GetBlockHash());
        std::vector<uint256> vConnectHashes;
        vConnectHashes.reserve(vConnect.size());
        for (CBlockIndex* pindex : vConnect)
            vConnectHashes.push_back(pindex->GetBlockHash());
        BlockRequestTraceReorg(
            g_nBlockTraceSourcePeer,
            pfork->GetBlockHash(), pfork->nHeight,
            pindexBest->GetBlockHash(), pindexBest->nHeight,
            pindexNew->GetBlockHash(), pindexNew->nHeight,
            vDisconnectHashes, vConnectHashes,
            CBigNum(pindexBest->nChainTrust).ToString(),
            CBigNum(pindexNew->nChainTrust).ToString());
    }


    // Disconnect shorter branch
    list<CTransaction> vResurrect;
    for (CBlockIndex* pindex : vDisconnect)
    {
        CBlock block;
        if (!block.ReadFromDisk(pindex))
            return error("Reorganize() : ReadFromDisk for disconnect failed");
        if (!block.DisconnectBlock(txdb, pindex))
            return error("Reorganize() : DisconnectBlock %s failed", pindex->GetBlockHash().ToString().substr(0,20).c_str());

        // Queue memory transactions to resurrect.
        // We only do this for blocks after the last checkpoint (reorganisation before that
        // point should only happen with -reindex/-loadblock, or a misbehaving peer.
        BOOST_REVERSE_FOREACH(const CTransaction& tx, block.vtx)
            if (!(tx.IsCoinBase() || tx.IsCoinStake()) && pindex->nHeight > Checkpoints::GetTotalBlocksEstimate())
                vResurrect.push_front(tx);
    }

    // Connect longer branch
    vector<CTransaction> vDelete;
    for (unsigned int i = 0; i < vConnect.size(); i++)
    {
        CBlockIndex* pindex = vConnect[i];
        CBlock block;
        if (!block.ReadFromDisk(pindex))
            return error("Reorganize() : ReadFromDisk for connect failed");

        if (!IsInitialBlockDownload()) GetCollateralnodeRanks(pindex); // recalculate ranks for the this block hash if required

        if (!block.ConnectBlock(txdb, pindex))
        {
            // Invalid block
            return error("Reorganize() : ConnectBlock %s failed", pindex->GetBlockHash().ToString().substr(0,20).c_str());
        }

        // Queue memory transactions to delete
        for (const CTransaction& tx : block.vtx)
            vDelete.push_back(tx);
    }
    if (!txdb.WriteHashBestChain(pindexNew->GetBlockHash()))
        return error("Reorganize() : WriteHashBestChain failed");

    // Make sure it's successfully written to disk before changing memory structure
    if (!txdb.TxnCommit())
        return error("Reorganize() : TxnCommit failed");

    // Clear setStakeSeen so disconnected stakes don't block the new branch
    for (CBlockIndex* pindex : vDisconnect)
    {
        if (pindex->IsProofOfStake())
            setStakeSeen.erase(make_pair(pindex->prevoutStake, pindex->nStakeTime));
    }

    // IDAG: Clean up DAG data for disconnected blocks
    // Phase 1: Batch all LevelDB erasures atomically
    {
        CTxDB txdbDAGClean;
        txdbDAGClean.TxnBegin();
        for (auto rit = vDisconnect.rbegin(); rit != vDisconnect.rend(); ++rit)
        {
            CBlockIndex* pindex = *rit;
            if (pindex->nHeight >= FORK_HEIGHT_DAG && pindex->phashBlock)
                txdbDAGClean.EraseDAGLinks(pindex->GetBlockHash());
        }
        txdbDAGClean.TxnCommit();
    }
    // Phase 2: Memory cleanup after LevelDB commit (reverse order: children first)
    bool fDAGReorg = false;
    for (auto rit = vDisconnect.rbegin(); rit != vDisconnect.rend(); ++rit)
    {
        CBlockIndex* pindex = *rit;
        if (pindex->nHeight >= FORK_HEIGHT_DAG && pindex->phashBlock)
        {
            g_dagManager.RemoveBlockDAGData(pindex->GetBlockHash());
            fDAGReorg = true;
        }
    }
    // Re-color DAG blocks above fork point to ensure consistency with fresh-synced nodes
    if (fDAGReorg && pfork)
        g_dagManager.RebuildDAGOrderIncremental(pfork->nHeight);

    // Disconnect shorter branch
    for (CBlockIndex* pindex : vDisconnect)
        if (pindex->pprev)
            pindex->pprev->pnext = NULL;

    // Connect longer branch
    for (CBlockIndex* pindex : vConnect)
        if (pindex->pprev)
            pindex->pprev->pnext = pindex;

    // Resurrect memory transactions, re-validate shielded anchors
    for (CTransaction& tx : vResurrect)
    {
        if (tx.IsShielded())
        {
            bool fValidAnchors = true;
            for (const CShieldedSpendDescription& spend : tx.vShieldedSpend)
            {
                if (!txdb.ReadShieldedAnchor(spend.anchor))
                {
                    fValidAnchors = false;
                    if (fDebug)
                        printf("Reorganize() : dropping shielded tx %s - anchor %s no longer valid\n",
                               tx.GetHash().ToString().substr(0,10).c_str(),
                               spend.anchor.ToString().substr(0,10).c_str());
                    break;
                }
            }
            if (!fValidAnchors)
                continue; // Don't resurrect this tx - anchors are invalid
        }
        tx.AcceptToMemoryPool(txdb);
    }

    // Delete redundant memory transactions that are in the connected branch
    for (CTransaction& tx : vDelete) {
        mempool.remove(tx);
        mempool.removeConflicts(tx);
    }

    CollateralNReorgBlock = true;
    printf("REORGANIZE: done\n");

    return true;
}


// Called from inside SetBestChain: attaches a block to the new best chain being built
bool CBlock::SetBestChainInner(CTxDB& txdb, CBlockIndex *pindexNew)
{
    uint256 hash = GetHash();

    // Operator-invalidation gate (defense in depth; the primary gate is in
    // SetBestChain, which precedes every SetBestChainInner call).
    if (IsBlockOperatorInvalid(pindexNew))
        return error("SetBestChainInner() : block %s (or an ancestor) is invalidated by the operator",
                     pindexNew->GetBlockHash().ToString().substr(0, 20).c_str());

    // Adding to current best branch
    if (!ConnectBlock(txdb, pindexNew) || !txdb.WriteHashBestChain(hash))
    {
        txdb.TxnAbort();
        InvalidChainFound(pindexNew);
        return false;
    }
    {
        ibdactivepath::ActivePathTimer ibdChainStateCommitTimer(
            ibdactivepath::GetCounters().chainstate_commit_us_total,
            ibdactivepath::GetCounters().chainstate_commit_us_max,
            ibdactivepath::GetCounters().chainstate_commit_count,
            "chainstate_commit", pindexNew->nHeight);
        if (!txdb.TxnCommit())
            return error("SetBestChain() : TxnCommit failed");
    }

    if (pindexNew->pprev)
        pindexNew->pprev->pnext = pindexNew;

    // Delete redundant memory transactions
    for (CTransaction& tx : vtx)
        mempool.remove(tx);

    // IDAG Phase 3: Remove txs from DAG sibling blocks
    if (pindexNew->nHeight >= FORK_HEIGHT_DAG)
    {
        std::set<uint256> siblings = g_dagManager.GetDAGSiblingBlocks(hash);
        for (const uint256& hashSibling : siblings)
            mempool.RemoveDAGConflicts(hashSibling);
    }

    return true;
}

bool CBlock::SetBestChain(CTxDB& txdb, CBlockIndex* pindexNew)
{
    ibdactivepath::ActivePathTimer ibdSetBestChainTimer(
        ibdactivepath::GetCounters().setbestchain_us_total,
        ibdactivepath::GetCounters().setbestchain_us_max,
        ibdactivepath::GetCounters().setbestchain_count,
        "setbestchain", pindexNew->nHeight);
    // Operator-invalidation gate: never activate a chain that descends from an
    // operator-invalidated block, regardless of how it reached SetBestChain.
    if (IsBlockOperatorInvalid(pindexNew))
        return error("SetBestChain() : block %s (or an ancestor) is invalidated by the operator",
                     pindexNew->GetBlockHash().ToString().substr(0, 20).c_str());
    uint256 hash = GetHash();
    ibdblocklatency::RecordSetBestChainBegin(hash);
    const uint256 hashOldBest = hashBestChain;
    const int nOldBestHeight = nBestHeight;
    const bool fReorgPath = (hashPrevBlock != hashOldBest);
    if (!txdb.TxnBegin())
        return error("SetBestChain() : TxnBegin failed");

    if (pindexGenesisBlock == NULL && hash == GetGenesisBlockHash())
    {
        txdb.WriteHashBestChain(hash);
        if (!txdb.TxnCommit())
            return error("SetBestChain() : TxnCommit failed");
        pindexGenesisBlock = pindexNew;
    }
    else if (hashPrevBlock == hashBestChain)
    {
        if (!SetBestChainInner(txdb, pindexNew))
            return error("SetBestChain() : SetBestChainInner failed");
    }
    else
    {
        {
            int nFinalHeight = g_finalityTracker.GetFinalizedHeight();
            if (nFinalHeight > 0 && pindexBest && pindexBest->nHeight >= FORK_HEIGHT_FINALITY)
            {
                CBlockIndex* pWalk = pindexNew;
                while (pWalk && pWalk->nHeight > nBestHeight)
                    pWalk = pWalk->pprev;
                CBlockIndex* pOld = pindexBest;
                while (pOld && pWalk && pOld != pWalk)
                {
                    if (pOld->nHeight > pWalk->nHeight)
                        pOld = pOld->pprev;
                    else if (pWalk->nHeight > pOld->nHeight)
                        pWalk = pWalk->pprev;
                    else
                    {
                        pOld = pOld->pprev;
                        pWalk = pWalk->pprev;
                    }
                }
                if (pOld && pOld->nHeight < nFinalHeight)
                {
                    txdb.TxnAbort();
                    return error("SetBestChain() : rejected reorg - fork below finalized height %d", nFinalHeight);
                }
            }
        }

        // the first block in the new chain that will cause it to become the new best chain
        CBlockIndex *pindexIntermediate = pindexNew;

        // list of blocks that need to be connected afterwards
        std::vector<CBlockIndex*> vpindexSecondary;

        // Reorganize is costly in terms of db load, as it works in a single db transaction.
        // Try to limit how much needs to be done inside
        while (pindexIntermediate->pprev && pindexIntermediate->pprev->nChainTrust > pindexBest->nChainTrust)
        {
            vpindexSecondary.push_back(pindexIntermediate);
            pindexIntermediate = pindexIntermediate->pprev;
        }

        if (!vpindexSecondary.empty())
            printf("Postponing %" PRIszu" reconnects\n", vpindexSecondary.size());

        // Switch to new best branch
        if (!Reorganize(txdb, pindexIntermediate))
        {
            txdb.TxnAbort();
            InvalidChainFound(pindexNew);
            return error("SetBestChain() : Reorganize failed");
        }

        // Connect further blocks
        BOOST_REVERSE_FOREACH(CBlockIndex *pindex, vpindexSecondary)
        {
            CBlock block;
            if (!block.ReadFromDisk(pindex))
            {
                printf("SetBestChain() : ReadFromDisk failed\n");
                break;
            }
            if (!txdb.TxnBegin()) {
                printf("SetBestChain() : TxnBegin 2 failed\n");
                break;
            }
            // errors now are not fatal, we still did a reorganisation to a new chain in a valid way
            if (!block.SetBestChainInner(txdb, pindex))
                break;
        }


    }

    // Update best block in wallet (so we can detect restored wallets)
    bool fIsInitialDownload = IsInitialBlockDownload();
    if (!fIsInitialDownload)
    {
        const CBlockLocator locator(pindexNew);
        ibdactivepath::ActivePathTimer ibdWalletCallbackTimer(
            ibdactivepath::GetCounters().wallet_callback_us_total,
            ibdactivepath::GetCounters().wallet_callback_us_max,
            ibdactivepath::GetCounters().wallet_callback_count,
            "wallet_callback", pindexNew->nHeight);
        ::SetBestChain(locator);
    }

    if (IbdHeadersControlPlaneEnabled())
        TraceIbdHeadersObserverEvent("connect", NULL, hash, pindexNew->nHeight);
    // New best block
    hashBestChain = hash;
    pindexBest = pindexNew;
    pblockindexFBBHLast = NULL;
    nBestHeight = pindexBest->nHeight;
    nBestChainTrust = pindexNew->nChainTrust;
    nTimeBestReceived = GetTime();
    if (IbdHeadersControlPlaneEnabled())
        g_ibdHeadersObserver.UpdateAnchor(hashBestChain, nBestHeight);
    mempool.AddTransactionsUpdated(1);
    // A new active tip invalidates the locator context of any pending
    // frontier getblocks response; refuse the exemption rather than admit a
    // block whose connection point has moved.
    InvalidateFrontierOnTipChange();
    ibdmetrics::Get().setbestchain_commits.fetch_add(1, std::memory_order_relaxed);

    uint256 nBestBlockTrust = (pindexBest->nHeight != 0 && pindexBest->pprev != NULL) ? (pindexBest->nChainTrust - pindexBest->pprev->nChainTrust) : pindexBest->nChainTrust;

    printf("SetBestChain: new best=%s  height=%d  trust=%s  blocktrust=%" PRId64"  date=%s\n",
      hashBestChain.ToString().substr(0,20).c_str(), nBestHeight,
      CBigNum(nBestChainTrust).ToString().c_str(),
      nBestBlockTrust.Get64(),
      DateTimeStrFormat("%x %H:%M:%S", pindexBest->GetBlockTime()).c_str());
    if (SyncTraceEnabled())
        BlockRequestTraceSetBestChainCommit(
            g_nBlockTraceSourcePeer, hashOldBest, nOldBestHeight,
            hashBestChain, nBestHeight, fReorgPath,
            (int64_t)pindexBest->GetBlockTime());

    nTimeBestReceived = GetTime();

    // Check the version of the last 100 blocks to see if we need to upgrade:
    if (!fIsInitialDownload)
    {
        int nUpgraded = 0;
        const CBlockIndex* pindex = pindexBest;
        for (int i = 0; i < 100 && pindex != NULL; i++)
        {
            if (pindex->nVersion > CBlock::CURRENT_VERSION)
                ++nUpgraded;
            pindex = pindex->pprev;
        }
        if (nUpgraded > 0)
            printf("SetBestChain: %d of last 100 blocks above version %d\n", nUpgraded, CBlock::CURRENT_VERSION);
        if (nUpgraded > 100/2)
            // strMiscWarning is read by GetWarnings(), called by Qt and the JSON-RPC code to warn the user:
            strMiscWarning = _("Warning: This version is obsolete, upgrade required!");
    }

    std::string strCmd = GetArg("-blocknotify", "");

    if (!fIsInitialDownload && !strCmd.empty())
    {
        boost::replace_all(strCmd, "%s", hashBestChain.GetHex());
        boost::thread t(runCommand, strCmd); // thread runs free
    }

    ibdblocklatency::RecordBlockConnected(GetHash(), nBestHeight);

    return true;
}

// ppcoin: total coin age spent in transaction, in the unit of coin-days.
// Only those coins meeting minimum age requirement counts. As those
// transactions not in main chain are not currently indexed so we
// might not find out about their coin age. Older transactions are
// guaranteed to be in main chain by sync-checkpoint. This rule is
// introduced to help nodes establish a consistent view of the coin
// age (trust score) of competing branches.
bool CTransaction::GetCoinAge(CTxDB& txdb, uint64_t& nCoinAge) const
{
    CBigNum bnCentSecond = 0;  // coin age in the unit of cent-seconds
    nCoinAge = 0;

    if (IsCoinBase())
        return true;

    for (const CTxIn& txin : vin)
    {
        // First try finding the previous transaction in database
        CTransaction txPrev;
        CTxIndex txindex;
        if (!txPrev.ReadFromDisk(txdb, txin.prevout, txindex))
            continue;  // previous transaction not in main chain
        if (nTime < txPrev.nTime)
            return false;  // Transaction timestamp violation

        // Read block header
        CBlock block;
        if (!block.ReadFromDisk(txindex.pos.nFile, txindex.pos.nBlockPos, false))
            return false; // unable to read block of previous transaction
        if (block.GetBlockTime() + nStakeMinAge > nTime)
            continue; // only count coins meeting min age requirement

        int64_t nValueIn = txPrev.vout[txin.prevout.n].nValue;
        // Cap coin age to 1 year (post-fork only)
        int64_t nTimeDiff = nTime - txPrev.nTime;
        if (nBestHeight >= FORK_HEIGHT_TIGHTER_DRIFT && nTimeDiff > 365 * 24 * 60 * 60)
            nTimeDiff = 365 * 24 * 60 * 60;
        bnCentSecond += CBigNum(nValueIn) * nTimeDiff / CENT;

        if (fDebug && GetBoolArg("-printcoinage"))
            printf("coin age nValueIn=%" PRId64" nTimeDiff=%d bnCentSecond=%s\n", nValueIn, nTime - txPrev.nTime, bnCentSecond.ToString().c_str());
    }

    CBigNum bnCoinDay = bnCentSecond * CENT / COIN / (24 * 60 * 60);
    if (fDebug && GetBoolArg("-printcoinage"))
        printf("coin age bnCoinDay=%s\n", bnCoinDay.ToString().c_str());
    nCoinAge = bnCoinDay.getuint64();
    return true;
}

// ppcoin: total coin age spent in block, in the unit of coin-days.
bool CBlock::GetCoinAge(uint64_t& nCoinAge) const
{
    nCoinAge = 0;

    CTxDB txdb("r");
    for (const CTransaction& tx : vtx)
    {
        uint64_t nTxCoinAge;
        if (tx.GetCoinAge(txdb, nTxCoinAge))
            nCoinAge += nTxCoinAge;
        else
            return false;
    }

    if (nCoinAge == 0) // block coin age minimum 1 coin-day
        nCoinAge = 1;
    if (fDebug && GetBoolArg("-printcoinage"))
        printf("block coin age total nCoinDays=%" PRId64"\n", nCoinAge);
    return true;
}

bool CBlock::AddToBlockIndex(unsigned int nFile, unsigned int nBlockPos, const uint256& hashProof)
{
    ibdactivepath::ActivePathTimer ibdAddToBlockIndexTimer(
        ibdactivepath::GetCounters().addtoblockindex_us_total,
        ibdactivepath::GetCounters().addtoblockindex_us_max,
        ibdactivepath::GetCounters().addtoblockindex_count,
        "addtoblockindex", nBestHeight + 1);
    int64_t nAddStart = GetTimeMillis();
    int64_t nDAGInitMs = 0;
    int64_t nDAGColorMs = 0;
    int64_t nDAGWriteMs = 0;

    // Check for duplicate
    uint256 hash = GetHash();
    ibdblocklatency::RecordAddToBlockIndexBegin(hash);
    if (mapBlockIndex.count(hash))
        return error("AddToBlockIndex() : %s already exists", hash.ToString().substr(0,20).c_str());

    // Construct new block index object
    CBlockIndex* pindexNew = new CBlockIndex(nFile, nBlockPos, *this);
    if (!pindexNew)
        return error("AddToBlockIndex() : new CBlockIndex failed");
    pindexNew->phashBlock = &hash;
    map<uint256, CBlockIndex*>::iterator miPrev = mapBlockIndex.find(hashPrevBlock);
    if (miPrev != mapBlockIndex.end())
    {
        pindexNew->pprev = (*miPrev).second;
        if (pindexNew->pprev->nHeight < 0)
            return error("AddToBlockIndex() : pprev has invalid height %d", pindexNew->pprev->nHeight);
        pindexNew->nHeight = pindexNew->pprev->nHeight + 1;
    }

    if (pindexNew->nHeight >= FORK_HEIGHT_DAG && pindexNew->IsProofOfStake())
        return error("AddToBlockIndex() : proof-of-stake block at post-DAG height %d", pindexNew->nHeight);

    // ppcoin: compute chain trust score
    pindexNew->nChainTrust = (pindexNew->pprev ? pindexNew->pprev->nChainTrust : 0) + pindexNew->GetBlockTrust();

    // ppcoin: compute stake entropy bit for stake modifier
    if (!pindexNew->SetStakeEntropyBit(GetStakeEntropyBit()))
        return error("AddToBlockIndex() : SetStakeEntropyBit() failed");

    // Record proof hash value
    pindexNew->hashProof = hashProof;

    // ppcoin: compute stake modifier
    uint64_t nStakeModifier = 0;
    bool fGeneratedStakeModifier = false;
    if (!ComputeNextStakeModifier(pindexNew->pprev, nStakeModifier, fGeneratedStakeModifier))
        return error("AddToBlockIndex() : ComputeNextStakeModifier() failed");
    pindexNew->SetStakeModifier(nStakeModifier, fGeneratedStakeModifier);
    // in-memory chain-own memo: block time of the last generated modifier active
    // at this index (for O(1) next-block recovery under -stakemodifieropt).
    pindexNew->nStakeModifierTime = fGeneratedStakeModifier
        ? pindexNew->GetBlockTime()
        : (pindexNew->pprev ? pindexNew->pprev->nStakeModifierTime : 0);
    pindexNew->nStakeModifierChecksum = GetStakeModifierChecksum(pindexNew);
    if (!CheckStakeModifierCheckpoints(pindexNew->nHeight, pindexNew->nStakeModifierChecksum))
        return error("AddToBlockIndex() : Rejected by stake modifier checkpoint height=%d, modifier=0x%016" PRIx64, pindexNew->nHeight, nStakeModifier);

    // Add to mapBlockIndex
    map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.insert(make_pair(hash, pindexNew)).first;
    if (pindexNew->IsProofOfStake())
        setStakeSeen.insert(make_pair(pindexNew->prevoutStake, pindexNew->nStakeTime));
    pindexNew->phashBlock = &((*mi).first);
    pindexNew->BuildSkip();

    // Write to disk block index
    CTxDB txdb;
    if (!txdb.TxnBegin())
        return false;
    {
        CSyncLockPhase phase("ProcessMessage(block)", "blockindex_db");
        txdb.WriteBlockIndex(CDiskBlockIndex(pindexNew));
    }
    {
        CSyncLockPhase phase("ProcessMessage(block)", "blockindex_db_commit");
        ibdactivepath::ActivePathTimer ibdBlockIndexCommitTimer(
            ibdactivepath::GetCounters().blockindex_commit_us_total,
            ibdactivepath::GetCounters().blockindex_commit_us_max,
            ibdactivepath::GetCounters().blockindex_commit_count,
            "blockindex_commit", pindexNew->nHeight);
        if (!txdb.TxnCommit())
            return false;
    }

    bool fDAGDataInitialized = false;
    std::vector<uint256> vDAGParents;

    // IDAG Phase 2: Initialize DAG data for post-fork blocks
    if (pindexNew->nHeight >= FORK_HEIGHT_DAG && pindexNew->IsProofOfWork())
    {
        // Extract DAG parents from coinbase OP_RETURN
        for (unsigned int i = 0; i < vtx[0].vout.size(); i++)
        {
            vDAGParents = ExtractDAGParents(vtx[0].vout[i].scriptPubKey);
            if (!vDAGParents.empty())
                break;
        }

        if (!vDAGParents.empty())
        {
            int64_t nDAGTimer = GetTimeMillis();
            g_dagManager.InitBlockDAGData(pindexNew, vDAGParents);
            fDAGDataInitialized = true;
            nDAGInitMs = GetTimeMillis() - nDAGTimer;

            // IDAG Phase 4: Fork-gate between GHOSTDAG and DAGKNIGHT coloring
            nDAGTimer = GetTimeMillis();
            if (pindexNew->nHeight >= FORK_HEIGHT_DAGKNIGHT)
                g_dagManager.ColorBlockDAGKnight(pindexNew);
            else
                g_dagManager.ColorBlock(pindexNew);
            nDAGColorMs = GetTimeMillis() - nDAGTimer;

            nDAGTimer = GetTimeMillis();
            CTxDB txdbDAG;
            if (txdbDAG.TxnBegin())
            {
                g_dagManager.WriteDAGLinks(txdbDAG, hash);
                // Also update parent entries (new child link)
                for (const uint256& hashParent : vDAGParents)
                    g_dagManager.WriteDAGLinks(txdbDAG, hashParent);
                {
                    ibdactivepath::ActivePathTimer ibdDAGCommitTimer(
                        ibdactivepath::GetCounters().dag_epoch_commit_us_total,
                        ibdactivepath::GetCounters().dag_epoch_commit_us_max,
                        ibdactivepath::GetCounters().dag_epoch_commit_count,
                        "dag_commit", pindexNew->nHeight);
                    txdbDAG.TxnCommit();
                }
            }
            nDAGWriteMs = GetTimeMillis() - nDAGTimer;

            // Use DAG score for best-chain comparison
            uint256 nDAGScore = g_dagManager.ComputeDAGScore(pindexNew);
            pindexNew->nChainTrust = nDAGScore;

            // IDAG Phase 3: Remove DAG sibling txs from mempool
            std::set<uint256> siblings = g_dagManager.GetDAGSiblingBlocks(hash);
            for (const uint256& hashSibling : siblings)
                mempool.RemoveDAGConflicts(hashSibling);

            // IDAG Phase 3: Epoch state computation + pruning at epoch boundaries
            if (pindexNew->nHeight > 0)
            {
                int nCurrentEpoch = GetEpochForHeight(pindexNew->nHeight);
                int nPreviousEpoch = GetEpochForHeight(pindexNew->nHeight - 1);
                if (nCurrentEpoch > nPreviousEpoch)
                {
                    int nCompletedEpoch = nPreviousEpoch;
                    int nEpochStart = GetEpochBoundaryHeight(nCompletedEpoch, pindexNew->nHeight);
                    int nEpochEnd = GetEpochBoundaryHeight(nCompletedEpoch + 1, pindexNew->nHeight) - 1;
                    int nEpochInterval = (nEpochEnd >= nEpochStart) ? (nEpochEnd - nEpochStart + 1) : GetEpochInterval(nEpochStart);

                    g_dagManager.ComputeEpochState(nCompletedEpoch, nEpochInterval);

                    CTxDB txdbEpoch;
                    if (txdbEpoch.TxnBegin())
                    {
                        g_dagManager.WriteEpochState(txdbEpoch, nCompletedEpoch);
                        {
                            ibdactivepath::ActivePathTimer ibdEpochCommitTimer(
                                ibdactivepath::GetCounters().dag_epoch_commit_us_total,
                                ibdactivepath::GetCounters().dag_epoch_commit_us_max,
                                ibdactivepath::GetCounters().dag_epoch_commit_count,
                                "epoch_commit", pindexNew->nHeight);
                            txdbEpoch.TxnCommit();
                        }
                    }

                    // Prune old DAG data periodically
                    CTxDB txdbPrune;
                    g_dagManager.PruneDAGData(txdbPrune, pindexNew->nHeight);
                }
            }
        }
    }

    LOCK(cs_main);

    // New best
    if (pindexNew->nChainTrust > nBestChainTrust)
    {
        CSyncLockPhase phase("ProcessMessage(block)", "set_best_chain");
        if (!SetBestChain(txdb, pindexNew))
        {
            if (fDAGDataInitialized)
            {
                g_dagManager.RemoveBlockDAGData(hash);

                CTxDB txdbDAGClean;
                if (txdbDAGClean.TxnBegin())
                {
                    txdbDAGClean.EraseDAGLinks(hash);
                    for (const uint256& hashParent : vDAGParents)
                    {
                        if (g_dagManager.HasDAGData(hashParent))
                            g_dagManager.WriteDAGLinks(txdbDAGClean, hashParent);
                    }
                    txdbDAGClean.TxnCommit();
                }
            }

            CTxDB txdbIndexClean;
            txdbIndexClean.EraseBlockIndex(hash);
            mapBlockIndex.erase(hash);
            if (pindexNew->IsProofOfStake())
                setStakeSeen.erase(make_pair(pindexNew->prevoutStake, pindexNew->nStakeTime));
            delete pindexNew;
            ibdblocklatency::RecordBlockTerminal(hash, ibdblocklatency::OUTCOME_REJECTED);
            return false;
        }
    }
    else
        ibdblocklatency::RecordBlockAcceptedSide(hash, pindexNew->nHeight);

    if (pindexNew == pindexBest)
    {
        // Notify UI to display prev block's coinbase if it was ours
        static uint256 hashPrevBestCoinBase;
        UpdatedTransaction(hashPrevBestCoinBase);
        hashPrevBestCoinBase = vtx[0].GetHash();
    }

    {
        static int64_t nLastNotifyTime = 0;
        static int nLastNotifyHeight = 0;
        int64_t nNow = GetTimeMillis();
        int nHeight = pindexNew->nHeight;
        bool fNotify = !IsInitialBlockDownload()
                       || (nNow - nLastNotifyTime > 2000)   // at least every 2 seconds
                       || (nHeight - nLastNotifyHeight >= 500); // or every 500 blocks
        if (fNotify)
        {
            CSyncLockPhase phase("ProcessMessage(block)", "wallet_ui_notification");
            uiInterface.NotifyBlocksChanged(nHeight, GetNumBlocksOfPeers());
            nLastNotifyTime = nNow;
            nLastNotifyHeight = nHeight;
        }
    }

    if (fDebug && GetBoolArg("-showtimers", false))
        printf("AddToBlockIndex: height=%d total=%" PRId64"ms dag_init=%" PRId64"ms dag_color=%" PRId64"ms dag_write=%" PRId64"ms\n",
               pindexNew->nHeight, GetTimeMillis() - nAddStart, nDAGInitMs, nDAGColorMs, nDAGWriteMs);

    return true;
}




bool CBlock::CheckBlock(bool fCheckPOW, bool fCheckMerkleRoot, bool fCheckSig, const char** ppszRejectReason) const
{
    if (ppszRejectReason)
        *ppszRejectReason = "CHECKBLOCK_OTHER";
    // These are checks that are independent of context
    // that can be verified before saving an orphan block.

    // Size limits (ceiling as sanity check; height-aware limit enforced in AcceptBlock)
    if (vtx.empty() || vtx.size() > ADAPTIVE_BLOCK_CEILING || ::GetSerializeSize(*this, SER_NETWORK, PROTOCOL_VERSION) > ADAPTIVE_BLOCK_CEILING)
        { if (ppszRejectReason) *ppszRejectReason = "CHECKBLOCK_SIZE"; return DoS(100, error("CheckBlock() : size limits failed")); }

    // Check proof of work matches claimed amount
    if (fCheckPOW && IsProofOfWork() && !CheckProofOfWork(GetPoWHash(), nBits))
        { if (ppszRejectReason) *ppszRejectReason = "CHECKBLOCK_OTHER"; return DoS(50, error("CheckBlock() : proof of work failed")); }

    // Check timestamp
    if (GetBlockTime() > FutureDrift(GetAdjustedTime()))
        { if (ppszRejectReason) *ppszRejectReason = "CHECKBLOCK_FUTURE_TIMESTAMP"; return error("CheckBlock() : block timestamp too far in the future"); }

    // First transaction must be coinbase, the rest must not be
    if (vtx.empty() || !vtx[0].IsCoinBase())
        { if (ppszRejectReason) *ppszRejectReason = "CHECKBLOCK_TX_EMPTY"; return DoS(100, error("CheckBlock() : first tx is not coinbase")); }
    for (unsigned int i = 1; i < vtx.size(); i++)
        if (vtx[i].IsCoinBase())
            { if (ppszRejectReason) *ppszRejectReason = "CHECKBLOCK_POS_STRUCTURE"; return DoS(100, error("CheckBlock() : more than one coinbase")); }

    // Check coinbase timestamp
    if (GetBlockTime() > FutureDrift((int64_t)vtx[0].nTime))
        { if (ppszRejectReason) *ppszRejectReason = "CHECKBLOCK_FUTURE_TIMESTAMP"; return DoS(50, error("CheckBlock() : coinbase timestamp is too early")); }

    if (IsProofOfStake())
    {
        // Coinbase output should be empty if proof-of-stake block
        // Post-DAG: allow additional zero-value OP_RETURN outputs for DAG parent commitment
        if (!vtx[0].vout[0].IsEmpty())
            { if (ppszRejectReason) *ppszRejectReason = "CHECKBLOCK_POS_STRUCTURE"; return DoS(100, error("CheckBlock() : coinbase vout[0] not empty for proof-of-stake block")); }
        // Cap extra outputs (1 empty + up to 2 OP_RETURN for DAG/data)
        if (vtx[0].vout.size() > 3)
            { if (ppszRejectReason) *ppszRejectReason = "CHECKBLOCK_POS_STRUCTURE"; return DoS(100, error("CheckBlock() : too many coinbase outputs (%d) for proof-of-stake block", (int)vtx[0].vout.size())); }
        if (vtx[0].vout.size() > 1)
        {
            for (unsigned int i = 1; i < vtx[0].vout.size(); i++)
            {
                if (vtx[0].vout[i].nValue != 0)
                    { if (ppszRejectReason) *ppszRejectReason = "CHECKBLOCK_POS_STRUCTURE"; return DoS(100, error("CheckBlock() : non-zero coinbase output[%d] in proof-of-stake block", i)); }
                // Must be OP_RETURN (DAG commitment or similar data-carrying output)
                if (vtx[0].vout[i].scriptPubKey.size() < 1 || vtx[0].vout[i].scriptPubKey[0] != OP_RETURN)
                    { if (ppszRejectReason) *ppszRejectReason = "CHECKBLOCK_POS_STRUCTURE"; return DoS(100, error("CheckBlock() : non-OP_RETURN extra coinbase output[%d] in proof-of-stake block", i)); }
            }
        }

        // Second transaction must be coinstake, the rest must not be
        if (vtx.empty() || !vtx[1].IsCoinStake())
            { if (ppszRejectReason) *ppszRejectReason = "CHECKBLOCK_POS_STRUCTURE"; return DoS(100, error("CheckBlock() : second tx is not coinstake")); }
        for (unsigned int i = 2; i < vtx.size(); i++)
            if (vtx[i].IsCoinStake())
                { if (ppszRejectReason) *ppszRejectReason = "CHECKBLOCK_POS_STRUCTURE"; return DoS(100, error("CheckBlock() : more than one coinstake")); }

		// Check coinstake timestamp
		if (!CheckCoinStakeTimestamp(GetBlockTime(), (int64_t)vtx[1].nTime))
			{ if (ppszRejectReason) *ppszRejectReason = "CHECKBLOCK_POS_STRUCTURE"; return DoS(50, error("CheckBlock() : coinstake timestamp violation nTimeBlock=%" PRId64" nTimeTx=%u", GetBlockTime(), vtx[1].nTime)); }

		// Check proof-of-stake block signature
		if (fCheckSig && !CheckBlockSignature())
            { if (ppszRejectReason) *ppszRejectReason = "CHECKBLOCK_SIGNATURE"; return DoS(100, error("CheckBlock() : bad proof-of-stake block signature")); }
	}

    // Check transactions
    for (const CTransaction& tx : vtx)
    {
        if (!tx.CheckTransaction())
            { if (ppszRejectReason) *ppszRejectReason = "CHECKBLOCK_TX_INVALID"; return DoS(tx.nDoS, error("CheckBlock() : CheckTransaction failed")); }

        // ppcoin: check transaction timestamp
        if (GetBlockTime() < (int64_t)tx.nTime)
            { if (ppszRejectReason) *ppszRejectReason = "CHECKBLOCK_FUTURE_TIMESTAMP"; return DoS(50, error("CheckBlock() : block timestamp earlier than transaction timestamp")); }
    }

    // Check for duplicate txids. This is caught by ConnectInputs(),
    // but catching it earlier avoids a potential DoS attack:
    set<uint256> uniqueTx;
    for (const CTransaction& tx : vtx)
    {
        uniqueTx.insert(tx.GetHash());
    }
    if (uniqueTx.size() != vtx.size())
        { if (ppszRejectReason) *ppszRejectReason = "CHECKBLOCK_TX_INVALID"; return DoS(100, error("CheckBlock() : duplicate transaction")); }

    unsigned int nSigOps = 0;
    for (const CTransaction& tx : vtx)
    {
        nSigOps += tx.GetLegacySigOpCount();
    }
    if (nSigOps > MAX_BLOCK_SIGOPS_ADAPTIVE)
        { if (ppszRejectReason) *ppszRejectReason = "CHECKBLOCK_TX_INVALID"; return DoS(100, error("CheckBlock() : out-of-bounds SigOpCount")); }

    // Check merkle root
    if (fCheckMerkleRoot && hashMerkleRoot != BuildMerkleTree())
        { if (ppszRejectReason) *ppszRejectReason = "CHECKBLOCK_MERKLE_ROOT"; return DoS(100, error("CheckBlock() : hashMerkleRoot mismatch")); }


    return true;
}

bool CBlock::AcceptBlock()
{
    AssertLockHeld(cs_main);
    ibdactivepath::ActivePathTimer ibdAcceptBlockTimer(
        ibdactivepath::GetCounters().acceptblock_us_total,
        ibdactivepath::GetCounters().acceptblock_us_max,
        ibdactivepath::GetCounters().acceptblock_count,
        "acceptblock", nBestHeight + 1);
    int64_t nAcceptStart = GetTimeMillis();

    if (nVersion > CURRENT_VERSION)
    {
        TraceAcceptBlockReject(*this, nBestHeight + 1,
                               ABREJECT_UNKNOWN_BLOCK_VERSION);
        return DoS(100, error("AcceptBlock() : reject unknown block version %d", nVersion));
    }

    // Check for duplicate
    uint256 hash = GetHash();
    if (SyncTraceEnabled())
        printf("SYNC_EVENT time_us=%lld event=ACCEPT_BLOCK_BEGIN hash=%s local_height=%d\n",
               (long long)GetTimeMicros(),
               hash.ToString().c_str(), nBestHeight);
    ibdblocklatency::RecordAcceptBlockBegin(hash);
    if (mapBlockIndex.count(hash))
    {
        TraceAcceptBlockReject(*this, nBestHeight + 1, ABREJECT_DUPLICATE);
        return error("AcceptBlock() : block already in mapBlockIndex");
    }

    // Get prev block index
    map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(hashPrevBlock);
    if (mi == mapBlockIndex.end())
    {
        TraceAcceptBlockReject(*this, nBestHeight + 1, ABREJECT_PREV_NOT_FOUND);
        return DoS(10, error("AcceptBlock() : prev block not found"));
    }
    CBlockIndex* pindexPrev = (*mi).second;
    int nHeight = pindexPrev->nHeight+1;

    // Operator-invalidation gate: reject the block (and any descendant of an
    // operator-invalidated block) without peer punishment. This is not a
    // consensus failure, so DoS()/InvalidChainFound are deliberately avoided.
    if (setInvalidBlockHash.count(hash))
    {
        TraceAcceptBlockReject(*this, nHeight, ABREJECT_OPERATOR_INVALIDATED);
        return error("AcceptBlock() : block %s is invalidated by the operator",
                     hash.ToString().substr(0, 20).c_str());
    }
    if (IsBlockOperatorInvalid(pindexPrev))
    {
        TraceAcceptBlockReject(*this, nHeight,
                               ABREJECT_PREV_OPERATOR_INVALIDATED);
        return error("AcceptBlock() : previous block %s (or an ancestor) is invalidated by the operator",
                     hashPrevBlock.ToString().substr(0, 20).c_str());
    }

    if (nHeight >= FORK_HEIGHT_DAG && IsProofOfStake())
    {
        TraceAcceptBlockReject(*this, nHeight, ABREJECT_POS_AFTER_DAG);
        return DoS(100, error("AcceptBlock() : proof-of-stake blocks are not allowed after DAG fork height %d", FORK_HEIGHT_DAG));
    }

    // Block size enforcement (height-aware)
    {
        unsigned int nBlockBytes = ::GetSerializeSize(*this, SER_NETWORK, PROTOCOL_VERSION);
        if (nHeight < FORK_HEIGHT_DAG)
        {
            // Pre-fork: strict 1MB limit (matches old wallet consensus)
            if (nBlockBytes > MAX_BLOCK_SIZE_LEGACY)
            {
                TraceAcceptBlockReject(*this, nHeight, ABREJECT_BLOCK_SIZE);
                return DoS(100, error("AcceptBlock() : block size %u exceeds legacy limit %u at height %d",
                                      nBlockBytes, MAX_BLOCK_SIZE_LEGACY, nHeight));
            }
        }
        else
        {
            // Post-fork: adaptive limit
            unsigned int nAdaptiveLimit = GetAdaptiveBlockSizeLimit(pindexPrev);
            if (nBlockBytes > nAdaptiveLimit)
            {
                TraceAcceptBlockReject(*this, nHeight, ABREJECT_BLOCK_SIZE);
                return DoS(50, error("AcceptBlock() : block size %u exceeds adaptive limit %u at height %d",
                                      nBlockBytes, nAdaptiveLimit, nHeight));
            }
        }
    }

    // Check proof-of-work or proof-of-stake
    unsigned int nComputedBits = GetNextTargetRequired(pindexPrev, IsProofOfStake());
    if (nBits != nComputedBits)
    {
        TraceAcceptBlockReject(*this, nHeight, ABREJECT_INCORRECT_BITS);
        return DoS(100, error("AcceptBlock() : incorrect %s", IsProofOfWork() ? "proof-of-work" : "proof-of-stake"));
    }

    if (GetBlockTime() <= pindexPrev->GetPastTimeLimit() || FutureDrift(GetBlockTime(), nHeight) < pindexPrev->GetBlockTime())
    {
        TraceAcceptBlockReject(*this, nHeight, ABREJECT_TIMESTAMP_TOO_EARLY);
        return error("AcceptBlock() : block's timestamp is too early");
    }

    // Check that all transactions are finalized
    for (const CTransaction& tx : vtx)
        //if (!tx.IsFinal(nHeight, GetBlockTime()))
		  if (!tx.IsFinal(nHeight, GetBlockTime()))
        {
            TraceAcceptBlockReject(*this, nHeight, ABREJECT_NON_FINAL_TX);
            return DoS(10, error("AcceptBlock() : contains a non-final transaction"));
        }

    // Check that the block chain matches the known block chain up to a checkpoint
    if (!Checkpoints::CheckHardened(nHeight, hash))
    {
        TraceAcceptBlockReject(*this, nHeight, ABREJECT_HARDENED_CHECKPOINT);
        return DoS(100, error("AcceptBlock() : rejected by hardened checkpoint lock-in at %d", nHeight));
    }

    uint256 hashProof;
    // Verify hash target and signature of coinstake tx
    if (IsProofOfStake())
    {
        CSyncLockPhase phase("ProcessMessage(block)", "pos_validation");
        uint256 targetProofOfStake;
        //if (!CheckProofOfStake(pindexPrev, vtx[1], nBits, hashProof, targetProofOfStake))
		if (!CheckProofOfStake(pindexPrev, vtx[1], nBits, hashProof, targetProofOfStake))
        {
            // Only penalize outside IBD (PoS verification needs UTXOs)
            if (!IsInitialBlockDownload())
            {
                TraceAcceptBlockReject(*this, nHeight, ABREJECT_CHECK_POS_FAILED);
                return DoS(50, error("AcceptBlock() : check proof-of-stake failed for block %s (peer penalized)",
                                     hash.ToString().c_str()));
            }
			printf("WARNING: AcceptBlock(): check proof-of-stake failed for block %s (IBD - no penalty)\n", hash.ToString().c_str());
            TraceAcceptBlockReject(*this, nHeight, ABREJECT_CHECK_POS_FAILED);
			return false;
        }
    }
    // PoW is checked in CheckBlock()
    if (IsProofOfWork())
    {
        CSyncLockPhase phase("ProcessMessage(block)", "pow_validation");
        hashProof = GetPoWHash();
    }

    // Reject ring signature transactions after deprecation height
    if (nHeight >= FORK_HEIGHT_RINGSIG_DEPRECATION)
    {
        for (unsigned int i = 0; i < vtx.size(); i++)
        {
            if (vtx[i].nVersion == ANON_TXN_VERSION)
            {
                TraceAcceptBlockReject(*this, nHeight,
                                       ABREJECT_RINGSIG_DEPRECATION);
                return DoS(100, error("AcceptBlock() : ring signature transaction (ANON_TXN_VERSION) in block at height %d after deprecation height %d",
                                       nHeight, FORK_HEIGHT_RINGSIG_DEPRECATION));
            }
        }
    }

    bool cpSatisfies = Checkpoints::CheckSync(hash, pindexPrev);

    // Check that the block satisfies synchronized checkpoint
    if (CheckpointsMode == Checkpoints::STRICT && !cpSatisfies)
    {
        TraceAcceptBlockReject(*this, nHeight, ABREJECT_SYNC_CHECKPOINT);
        return error("AcceptBlock() : rejected by synchronized checkpoint");
    }

    if (CheckpointsMode == Checkpoints::ADVISORY && !cpSatisfies)
        strMiscWarning = _("WARNING: syncronized checkpoint violation detected, but skipped!");

    // Enforce rule that the coinbase starts with serialized block height
    CScript expect = CScript() << nHeight;
    if (vtx[0].vin[0].scriptSig.size() < expect.size() ||
        !std::equal(expect.begin(), expect.end(), vtx[0].vin[0].scriptSig.begin()))
    {
        TraceAcceptBlockReject(*this, nHeight, ABREJECT_COINBASE_HEIGHT);
        return DoS(100, error("AcceptBlock() : block height mismatch in coinbase"));
    }

    // IDAG Phase 2: Validate DAG parent commitment in coinbase OP_RETURN
    if (nHeight >= FORK_HEIGHT_DAG)
    {
        // Search coinbase outputs for DAG parent commitment
        std::vector<uint256> vDAGParents;
        for (unsigned int i = 0; i < vtx[0].vout.size(); i++)
        {
            vDAGParents = ExtractDAGParents(vtx[0].vout[i].scriptPubKey);
            if (!vDAGParents.empty())
                break;
        }

        if (vDAGParents.empty())
        {
            TraceAcceptBlockReject(*this, nHeight, ABREJECT_DAG_PARENT);
            return DoS(100, error("AcceptBlock() : post-DAG-fork block missing DAG parent commitment"));
        }

        if (vDAGParents.size() > (unsigned int)MAX_DAG_PARENTS)
        {
            TraceAcceptBlockReject(*this, nHeight, ABREJECT_DAG_PARENT);
            return DoS(100, error("AcceptBlock() : too many DAG parents (%d > %d)", (int)vDAGParents.size(), MAX_DAG_PARENTS));
        }

        // Primary parent (index 0) must match hashPrevBlock
        if (vDAGParents[0] != hashPrevBlock)
        {
            TraceAcceptBlockReject(*this, nHeight, ABREJECT_DAG_PARENT);
            return DoS(100, error("AcceptBlock() : DAG primary parent %s != hashPrevBlock %s",
                                   vDAGParents[0].ToString().substr(0, 20).c_str(),
                                   hashPrevBlock.ToString().substr(0, 20).c_str()));
        }

        // Validate merge parents
        for (unsigned int i = 1; i < vDAGParents.size(); i++)
        {
            // No self-reference
            if (vDAGParents[i] == hash)
            {
                TraceAcceptBlockReject(*this, nHeight, ABREJECT_DAG_PARENT);
                return DoS(100, error("AcceptBlock() : DAG parent[%d] is self-reference", i));
            }

            // Must exist in block index
            if (!mapBlockIndex.count(vDAGParents[i]))
            {
                if (IsInitialBlockDownload())
                {
                    if (fDebug)
                        printf("AcceptBlock() : DAG merge parent[%d] %s not found during IBD, deferring validation\n",
                               i, vDAGParents[i].ToString().substr(0, 20).c_str());
                    continue;
                }
                TraceAcceptBlockReject(*this, nHeight, ABREJECT_DAG_PARENT);
                return DoS(10, error("AcceptBlock() : DAG merge parent[%d] %s not found",
                                      i, vDAGParents[i].ToString().substr(0, 20).c_str()));
            }

            // Merge parent must have lower height
            CBlockIndex* pMergeParent = mapBlockIndex[vDAGParents[i]];
            if (pMergeParent->nHeight >= nHeight)
            {
                TraceAcceptBlockReject(*this, nHeight, ABREJECT_DAG_PARENT);
                return DoS(100, error("AcceptBlock() : DAG merge parent[%d] height %d >= block height %d",
                                       i, pMergeParent->nHeight, nHeight));
            }
            if (pMergeParent->nHeight >= FORK_HEIGHT_DAG && pMergeParent->IsProofOfStake())
            {
                TraceAcceptBlockReject(*this, nHeight, ABREJECT_DAG_PARENT);
                return DoS(100, error("AcceptBlock() : DAG merge parent[%d] is proof-of-stake", i));
            }

            // Merge parent within DAG_MERGE_DEPTH of primary parent
            if (pindexPrev->nHeight - pMergeParent->nHeight > DAG_MERGE_DEPTH)
            {
                TraceAcceptBlockReject(*this, nHeight, ABREJECT_DAG_PARENT);
                return DoS(50, error("AcceptBlock() : DAG merge parent[%d] too deep (%d below primary)",
                                      i, pindexPrev->nHeight - pMergeParent->nHeight));
            }

            // No duplicate parents
            for (unsigned int j = 0; j < i; j++)
            {
                if (vDAGParents[j] == vDAGParents[i])
                {
                    TraceAcceptBlockReject(*this, nHeight, ABREJECT_DAG_PARENT);
                    return DoS(100, error("AcceptBlock() : duplicate DAG parent at index %d and %d", j, i));
                }
            }
        }
    }

    // Write block to history file
    if (!CheckDiskSpace(::GetSerializeSize(*this, SER_DISK, CLIENT_VERSION)))
    {
        TraceAcceptBlockReject(*this, nHeight, ABREJECT_DISK_SPACE);
        return error("AcceptBlock() : out of disk space");
    }
    unsigned int nFile = -1;
    unsigned int nBlockPos = 0;
    int64_t nWriteDiskStart = GetTimeMillis();
    ibdactivepath::ActivePathTimer ibdRawBlockWriteTimer(
        ibdactivepath::GetCounters().raw_block_write_us_total,
        ibdactivepath::GetCounters().raw_block_write_us_max,
        ibdactivepath::GetCounters().raw_block_write_count,
        "raw_block_write", nBestHeight + 1);
    {
        CSyncLockPhase phase("ProcessMessage(block)", "write_to_disk");
        if (!WriteToDisk(nFile, nBlockPos))
        {
            TraceAcceptBlockReject(*this, nHeight, ABREJECT_WRITE_TO_DISK);
            return error("AcceptBlock() : WriteToDisk failed");
        }
    }
    int64_t nWriteDiskMs = GetTimeMillis() - nWriteDiskStart;
    int64_t nAddIndexStart = GetTimeMillis();
    {
        CSyncLockPhase phase("ProcessMessage(block)", "add_to_block_index");
        if (!AddToBlockIndex(nFile, nBlockPos, hashProof))
        {
            TraceAcceptBlockReject(*this, nHeight, ABREJECT_ADD_TO_BLOCK_INDEX);
            return error("AcceptBlock() : AddToBlockIndex failed");
        }
    }
    int64_t nAddIndexMs = GetTimeMillis() - nAddIndexStart;

    // Relay inventory, but don't relay old inventory during initial block download
    int nBlockEstimate = Checkpoints::GetTotalBlocksEstimate();
    if (hashBestChain == hash)
    {
        LOCK(cs_vNodes);
        for (CNode* pnode : vNodes)
        {
            int nPeerHeight = pnode->nBestKnownHeight >= 0 ? pnode->nBestKnownHeight : pnode->nChainHeight;
            if (nBestHeight > (nPeerHeight != -1 ? nPeerHeight - 2000 : nBlockEstimate))
                pnode->PushInventory(CInv(MSG_BLOCK, hash));
        }
    }

    // ppcoin: check pending sync-checkpoint
    Checkpoints::AcceptPendingSyncCheckpoint();

    if (fDebug && GetBoolArg("-showtimers", false))
        printf("AcceptBlock: height=%d total=%" PRId64"ms write_disk=%" PRId64"ms add_index=%" PRId64"ms\n",
               nHeight, GetTimeMillis() - nAcceptStart, nWriteDiskMs, nAddIndexMs);

    ClearRejectedBlockForSync(hash);
    return true;
}

uint256 CBlockIndex::GetBlockTrust() const
{
    CBigNum bnTarget;
    bnTarget.SetCompact(nBits);

    if (bnTarget <= 0)
        return 0;

    if (nHeight >= FORK_HEIGHT_DAG && IsProofOfStake())
        return 0;

    if (nHeight >= FORK_HEIGHT_POEM)
        return GetBlockEntropy((IsProofOfStake() && nHeight < FORK_HEIGHT_DAG) ? hashProof : *phashBlock);

    return ((CBigNum(1)<<256) / (bnTarget+1)).getuint256();
}

// -----------------------------------------------------------------------------
// A.10.1f — First production BlockIndexHotOwner consumer.
//
// Block-trust metadata derivation (CBlockIndex::GetBlockTrust) driven through
// the HotOwner pin contract: logical hash -> authority -> hot materialization ->
// pin -> real consumer logic -> release. After the handle scope closes, the hot
// object is eviction-eligible again (PinCount(hash)==0 unless anchored).
//
// Sparse-hot safe: GetBlockTrust() reads only nBits / nHeight / nFlags /
// hashProof / *phashBlock — no pprev/pnext/pskip, no ancestry walk.
//
// Fail-closed: ok=false (nTrust=0) on ANY pin/materialization failure, so a
// HotOwner failure is never silently masked by a legacy fallback.
//
// Lock contract: production callers already hold cs_main. This function does
// not acquire cs_main; BlockIndexHotOwner is a LEAF (never takes cs_main), so
// cs_main -> owner-internal-lock ordering holds with no new inversion.
// -----------------------------------------------------------------------------
BlockIndexHotDerivedTrust GetBlockTrustViaHotOwner(BlockIndexHotOwner& owner,
                                                   const uint256& hash)
{
    BlockIndexHotDerivedTrust out;
    const BlockIndexLogicalId id(hash);
    BlockIndexHotHandle handle;
    const BlockIndexHotStatus st = owner.Pin(id, &handle);
    if (st != BlockIndexHotStatus::OK || !handle.IsValid())
        return out;                      // fail-closed: materialization/pin failure
    CBlockIndex* pindex = handle.Get();
    if (!pindex)
        return out;                      // fail-closed: no resident object
    out.nTrust = pindex->GetBlockTrust(); // REAL production consumer logic
    out.ok = true;
    // handle released here -> PinCount(hash)==0, object eviction-eligible again
    return out;
}

bool CBlockIndex::IsSuperMajority(int minVersion, const CBlockIndex* pstart, unsigned int nRequired, unsigned int nToCheck)
{
    unsigned int nFound = 0;
    for (unsigned int i = 0; i < nToCheck && nFound < nRequired && pstart != NULL; i++)
    {
        if (pstart->nVersion >= minVersion)
            ++nFound;
        pstart = pstart->pprev;
    }
    return (nFound >= nRequired);
}

// -----------------------------------------------------------------------------
// Operator block invalidation (invalidateblock / reconsiderblock).
//
// Semantics:
//  * setInvalidBlockHash holds ONLY explicitly invalidated hashes. A block is
//    treated as invalid if it or any ancestor is in the set, so descendants of
//    an entry are implicitly invalid.
//  * The set is persisted (txdb, both backends) BEFORE any active-chain
//    rollback. If a crash separates the two, startup re-runs the rollback (see
//    RecoverFromInvalidatedBestChain) so the database never settles with an
//    active tip descending from an operator-invalidated block.
//  * Operator invalidation is not a consensus failure: it never raises
//    nBestInvalidTrust and never triggers peer punishment.
// -----------------------------------------------------------------------------

bool IsBlockOperatorInvalid(const CBlockIndex* pindex)
{
    if (pindex == NULL || setInvalidBlockHash.empty())
        return false;
    for (const CBlockIndex* p = pindex; p != NULL; p = p->pprev)
        if (setInvalidBlockHash.count(*p->phashBlock))
            return true;
    return false;
}

static bool PersistInvalidBlockSet()
{
    CTxDB txdb;
    if (!txdb.TxnBegin())
        return false;
    txdb.WriteInvalidBlockSet(setInvalidBlockHash);
    return txdb.TxnCommit();
}

static bool IsAncestorOfBest(const CBlockIndex* pindex)
{
    for (const CBlockIndex* p = pindexBest; p != NULL; p = p->pprev)
        if (p == pindex)
            return true;
    return false;
}

// Roll the active chain back so its tip becomes pindexTarget (an ancestor of
// the current best). Reuses SetBestChain/Reorganize, so finality, DAG and
// stake-seen cleanup are handled by the existing machinery. The wallet and
// collateral callbacks inside that machinery are safe with no wallet /
// collateral nodes registered (startup healing).
static bool RollbackActiveChainTo(CBlockIndex* pindexTarget)
{
    AssertLockHeld(cs_main);
    if (pindexBest == NULL)
        return false;
    if (pindexTarget == NULL || pindexTarget == pindexBest)
        return true;

    CBlock block;
    if (!block.ReadFromDisk(pindexTarget))
        return error("RollbackActiveChainTo() : ReadFromDisk failed");
    CTxDB txdb;
    if (!block.SetBestChain(txdb, pindexTarget))
        return error("RollbackActiveChainTo() : SetBestChain failed");
    return true;
}

// Activate the best eligible chain tip (the ActivateBestChain equivalent in
// this trust-based client). Only leaf tips are considered so an internal
// ancestor can never suppress a better descendant. Candidates must not be
// operator-invalid, must have block data on disk, and must have a fork point at
// or above the finalized height.
static bool ActivateBestEligibleChain()
{
    AssertLockHeld(cs_main);
    if (pindexBest == NULL || mapBlockIndex.empty())
        return true;

    std::set<uint256> setReferenced;
    for (const PAIRTYPE(const uint256, CBlockIndex*)& item : mapBlockIndex)
        if (item.second->pprev != NULL)
            setReferenced.insert(*item.second->pprev->phashBlock);

    const int nFinalHeight = g_finalityTracker.GetFinalizedHeight();
    CBlockIndex* pindexCandidate = NULL;
    for (const PAIRTYPE(const uint256, CBlockIndex*)& item : mapBlockIndex)
    {
        CBlockIndex* pindex = item.second;
        if (setReferenced.count(*pindex->phashBlock))
            continue; // not a tip
        if (IsBlockOperatorInvalid(pindex))
            continue;
        if (pindex->nChainTrust <= nBestChainTrust)
            continue;
        CBlock block;
        if (!block.ReadFromDisk(pindex))
            continue; // incomplete / header-only index entry
        if (nFinalHeight > 0 && pindexBest->nHeight >= FORK_HEIGHT_FINALITY)
        {
            CBlockIndex* pFork = pindex;
            CBlockIndex* pOther = pindexBest;
            while (pFork != pOther)
            {
                while (pFork != NULL && pFork->nHeight > pOther->nHeight)
                    pFork = pFork->pprev;
                if (pFork == pOther)
                    break;
                if (pOther != NULL)
                    pOther = pOther->pprev;
            }
            if (pFork == NULL || pFork->nHeight < nFinalHeight)
                continue; // would be rejected by SetBestChain anyway
        }
        if (pindexCandidate == NULL ||
            pindex->nChainTrust > pindexCandidate->nChainTrust)
            pindexCandidate = pindex;
    }

    if (pindexCandidate == NULL)
        return true;

    CBlock block;
    if (!block.ReadFromDisk(pindexCandidate))
        return true;
    CTxDB txdb;
    if (!block.SetBestChain(txdb, pindexCandidate))
        return error("ActivateBestEligibleChain() : SetBestChain failed");

    ShadowCompareCandidateSelection();
    return true;
}

bool InvalidateBlock(const uint256& hash, std::string& strError)
{
    LOCK(cs_main);

    std::map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(hash);
    if (mi == mapBlockIndex.end())
    {
        strError = "Block not found";
        return false;
    }
    CBlockIndex* pindex = mi->second;
    if (pindex->nHeight == 0)
    {
        strError = "The genesis block cannot be invalidated";
        return false;
    }
    if (fImporting || fReindex)
    {
        strError = "Cannot invalidate while importing or reindexing";
        return false;
    }
    if (setInvalidBlockHash.count(hash))
        return true; // idempotent: already explicitly invalidated

    const bool fOnBest = IsAncestorOfBest(pindex);

    // Prevalidate finality before any persistent mutation so the operator gets
    // a deterministic error and the chain state is untouched.
    if (fOnBest)
    {
        CBlockIndex* pTarget = pindex->pprev;
        const int nFinalHeight = g_finalityTracker.GetFinalizedHeight();
        if (nFinalHeight > 0 && pTarget != NULL && pTarget->nHeight < nFinalHeight)
        {
            strError = strprintf(
                "Cannot invalidate block at height %d: its parent height %d is "
                "below the finalized height %d",
                pindex->nHeight, pTarget->nHeight, nFinalHeight);
            return false;
        }
    }

    // Persist FIRST. A crash here leaves the set written and the best chain
    // un-rolled-back; startup healing completes the rollback.
    setInvalidBlockHash.insert(hash);
    if (!PersistInvalidBlockSet())
    {
        setInvalidBlockHash.erase(hash);
        strError = "Failed to persist block invalidation";
        return false;
    }

    if (fOnBest)
    {
        CBlockIndex* pTarget = pindex->pprev;
        if (!RollbackActiveChainTo(pTarget))
            printf("InvalidateBlock: rollback for %s failed; restart will heal the chain\n",
                   hash.ToString().substr(0, 20).c_str());
    }
    ActivateBestEligibleChain();

    if (pindexBest)
        uiInterface.NotifyBlocksChanged(pindexBest->nHeight, GetNumBlocksOfPeers());

    printf("InvalidateBlock: invalidated %s (height %d)\n",
           hash.ToString().substr(0, 20).c_str(), pindex->nHeight);
    return true;
}

bool ReconsiderBlock(const uint256& hash, std::string& strError)
{
    LOCK(cs_main);

    std::map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(hash);
    if (mi == mapBlockIndex.end())
    {
        strError = "Block not found";
        return false;
    }
    if (fImporting || fReindex)
    {
        strError = "Cannot reconsider while importing or reindexing";
        return false;
    }
    if (!setInvalidBlockHash.count(hash))
        return true; // idempotent: not explicitly invalidated

    // Remove exactly this hash. Other explicit invalidations (ancestors or
    // descendants) are untouched.
    setInvalidBlockHash.erase(hash);
    if (!PersistInvalidBlockSet())
    {
        setInvalidBlockHash.insert(hash);
        strError = "Failed to persist block reconsideration";
        return false;
    }

    ActivateBestEligibleChain();

    if (pindexBest)
        uiInterface.NotifyBlocksChanged(pindexBest->nHeight, GetNumBlocksOfPeers());

    printf("ReconsiderBlock: reconsidered %s\n", hash.ToString().substr(0, 20).c_str());
    return true;
}

// Startup healing: if the stored hashBestChain descends from an
// operator-invalidated block (a crash between persisting the invalid set and
// rolling back the active chain), roll the chain back to the highest valid
// ancestor before the node starts operating. Called from CTxDB::LoadBlockIndex
// in both backends, after setInvalidBlockHash is loaded and after the naive
// pindexBest reconstruction.
bool RecoverFromInvalidatedBestChain()
{
    // cs_main is recursive; this may be called from inside or outside an
    // existing cs_main hold (the -loadblockindextest diagnostic path calls
    // CTxDB::LoadBlockIndex without holding it).
    LOCK(cs_main);
    if (setInvalidBlockHash.empty() || pindexBest == NULL)
        return true;

    // Find the highest valid ancestor of the best chain: every block above it
    // is operator-invalid (invalidity is inherited, so the whole descendant
    // region of an entry must be disconnected).
    CBlockIndex* pHeal = NULL;
    for (CBlockIndex* p = pindexBest; p != NULL; p = p->pprev)
    {
        if (!IsBlockOperatorInvalid(p))
        {
            pHeal = p;
            break;
        }
    }
    if (pHeal == NULL)
        return error("RecoverFromInvalidatedBestChain() : no valid ancestor");
    if (pHeal == pindexBest)
        return true; // best chain has no operator-invalidated block

    printf("RecoverFromInvalidatedBestChain: rolling best chain back from %s (height %d) to %s (height %d)\n",
           pindexBest->GetBlockHash().ToString().substr(0, 20).c_str(), pindexBest->nHeight,
           pHeal->GetBlockHash().ToString().substr(0, 20).c_str(), pHeal->nHeight);

    return RollbackActiveChainTo(pHeal);
}

bool ProcessBlock(CNode* pfrom, CBlock* pblock)
{
    AssertLockHeld(cs_main);

    ibdactivepath::ActivePathTimer ibdProcessBlockTimer(
        ibdactivepath::GetCounters().processblock_us_total,
        ibdactivepath::GetCounters().processblock_us_max,
        ibdactivepath::GetCounters().processblock_count,
        "processblock", nBestHeight);
    g_nBlockTraceSourcePeer = pfrom ? pfrom->GetId() : -1;
    int64_t nStartTime = GetTimeMillis();
    // Check for duplicate
    uint256 hash = pblock->GetHash();
    if (SyncTraceEnabled())
        printf("SYNC_EVENT time_us=%lld event=PROCESS_BLOCK_BEGIN hash=%s peer=%d local_height=%d\n",
               (long long)GetTimeMicros(),
               hash.ToString().c_str(),
               pfrom ? pfrom->GetId() : -1,
               nBestHeight);
    ibdblocklatency::RecordProcessBlockBegin(hash);
    if (pfrom != NULL && pindexBest != NULL && pindexBest->GetBlockTime() < GetTime() - 300 && fDebug)
        printf("sync: ProcessBlock %s from %s (height %d)\n", hash.ToString().substr(0,20).c_str(), pfrom->addrName.c_str(), nBestHeight);
    if (mapBlockIndex.count(hash)) {
        TraceProcessBlockReject(pfrom, pblock, PBREJECT_DUPLICATE_INDEXED);
        ibdblocklatency::RecordBlockTerminal(hash, ibdblocklatency::OUTCOME_ALREADY_HAVE);
        return error("ProcessBlock() : already have block %d %s", mapBlockIndex[hash]->nHeight, hash.ToString().substr(0,20).c_str());
    }
    if (mapOrphanBlocks.count(hash)) {
        TraceProcessBlockReject(pfrom, pblock, PBREJECT_DUPLICATE_ORPHAN);
        ibdblocklatency::RecordBlockTerminal(hash, ibdblocklatency::OUTCOME_ALREADY_HAVE);
        return error("ProcessBlock() : already have block (orphan) %s", hash.ToString().substr(0,20).c_str());
    }

    // Operator-invalidation gate: reject before any orphan admission or
    // consensus work. This is not a consensus failure - no peer punishment and
    // no InvalidChainFound (nBestInvalidTrust) update.
    if (setInvalidBlockHash.count(hash)) {
        TraceProcessBlockReject(pfrom, pblock, PBREJECT_OPERATOR_INVALIDATED);
        ibdblocklatency::RecordBlockTerminal(hash, ibdblocklatency::OUTCOME_REJECTED);
        return error("ProcessBlock() : block %s is invalidated by the operator",
                     hash.ToString().substr(0,20).c_str());
    }

    // ppcoin: check proof-of-stake
    // Limited duplicity on stake: prevents block flood attack
    // Duplicate stake allowed only when there is orphan child block
    if (pblock->IsProofOfStake() && setStakeSeen.count(pblock->GetProofOfStake()) && !mapOrphanBlocksByPrev.count(hash) && !Checkpoints::WantedByPendingSyncCheckpoint(hash)) {
        TraceProcessBlockReject(pfrom, pblock, PBREJECT_DUPLICATE_INDEXED_STAKE);
        ibdblocklatency::RecordBlockTerminal(hash, ibdblocklatency::OUTCOME_REJECTED);
        return error("ProcessBlock() : duplicate proof-of-stake (%s, %d) for block %s", pblock->GetProofOfStake().first.ToString().c_str(), pblock->GetProofOfStake().second, hash.ToString().c_str());
    }

    if (pblock->IsProofOfStake() && mapBlockIndex.count(pblock->hashPrevBlock))
    {
        CBlockIndex* pindexPrev = mapBlockIndex[pblock->hashPrevBlock];
        if (pindexPrev && pindexPrev->nHeight + 1 >= FORK_HEIGHT_DAG)
        {
            if (pfrom)
                pfrom->Misbehaving(100);
            TraceProcessBlockReject(pfrom, pblock, PBREJECT_POS_AFTER_DAG);
            ibdblocklatency::RecordBlockTerminal(hash, ibdblocklatency::OUTCOME_REJECTED);
            return error("ProcessBlock() : proof-of-stake block after DAG fork");
        }
    }

    // Preliminary checks
    int64_t nCheckStart = GetTimeMillis();
    const char* pszCheckBlockReason = NULL;
    {
        CSyncLockPhase phase("ProcessMessage(block)", "block_precheck");
        if (!pblock->CheckBlock(true, true, true, &pszCheckBlockReason)) {
        TraceProcessBlockReject(pfrom, pblock, PBREJECT_CHECKBLOCK_FALSE, pszCheckBlockReason);
        ibdblocklatency::RecordBlockTerminal(hash, ibdblocklatency::OUTCOME_REJECTED);
            return error("ProcessBlock() : CheckBlock FAILED");
        }
    }
    int64_t nCheckMs = GetTimeMillis() - nCheckStart;

    CBlockIndex* pcheckpoint = Checkpoints::GetLastSyncCheckpoint();
    if (pcheckpoint && pblock->hashPrevBlock != hashBestChain && !Checkpoints::WantedByPendingSyncCheckpoint(hash))
    {
        // Extra checks to prevent "fill up memory by spamming with bogus blocks"
        int64_t deltaTime = pblock->GetBlockTime() - pcheckpoint->nTime;
        CBigNum bnNewBlock;
        bnNewBlock.SetCompact(pblock->nBits);
        CBigNum bnRequired;

        if (pblock->IsProofOfStake())
            bnRequired.SetCompact(ComputeMinStake(GetLastBlockIndex(pcheckpoint, true)->nBits, deltaTime, pblock->nTime));
        else
            bnRequired.SetCompact(ComputeMinWork(GetLastBlockIndex(pcheckpoint, false)->nBits, deltaTime));

        if (bnNewBlock > bnRequired)
        {
            if (pfrom)
                pfrom->Misbehaving(100);
            std::string strWeakExtra = strprintf("block_trust=%s required_trust=%s checkpoint_height=%d checkpoint_hash=%s local_best_height=%d local_best_hash=%s", bnNewBlock.ToString().c_str(), bnRequired.ToString().c_str(), pcheckpoint ? pcheckpoint->nHeight : -1, pcheckpoint ? pcheckpoint->GetBlockHash().ToString().c_str() : uint256(0).ToString().c_str(), pindexBest ? pindexBest->nHeight : -1, pindexBest ? pindexBest->GetBlockHash().ToString().c_str() : uint256(0).ToString().c_str());
            TraceProcessBlockReject(pfrom, pblock, PBREJECT_WEAK_CHECKPOINT, NULL, strWeakExtra);
            ibdblocklatency::RecordBlockTerminal(hash, ibdblocklatency::OUTCOME_REJECTED);
            return error("ProcessBlock() : block with too little %s", pblock->IsProofOfStake()? "proof-of-stake" : "proof-of-work");
        }
    }

    // Innova: ask for pending sync-checkpoint if any
    if (!IsInitialBlockDownload()){

        Checkpoints::AskForPendingSyncCheckpoint(pfrom);

        CScript payee;

        if (!fImporting && !fReindex && pindexBest->nHeight > Checkpoints::GetTotalBlocksEstimate()){
            if(collateralnodePayments.GetBlockPayee(pindexBest->nHeight, payee)){
                // MAYBE NEEDS TO BE REWORKED
                //UPDATE COLLATERALNODE LAST PAID TIME
                // CCollateralnode* pmn = mnodeman.Find(vin);
                // if(pmn != NULL) {
                //     pmn->nLastPaid = GetAdjustedTime();
                // }

                printf("ProcessBlock() : Got BlockPayee for block : - %d\n", pindexBest->nHeight);
            }

            colLateralPool.CheckTimeout();
            colLateralPool.NewBlock();
            collateralnodePayments.ProcessBlock((pindexBest->nHeight)+10);

        }

    }


    // If don't already have its previous block, shunt it off to holding area until we get it
    if (!mapBlockIndex.count(pblock->hashPrevBlock)) //pblock->hashPrevBlock != 0 &&
    {
        if (fDebug)
            printf("ProcessBlock: ORPHAN BLOCK hash=%s prev=%s coinbase_height=%d peer_orphans=%d global_orphans=%zu\n",
                   hash.ToString().c_str(),
                   pblock->hashPrevBlock.ToString().c_str(),
                   DecodeCoinbaseHeightForTrace(*pblock),
                   pfrom ? GetPeerOrphanCount(pfrom->GetId()) : -1,
                   mapOrphanBlocks.size());
            //LogPrintf("ProcessBlock: ORPHAN BLOCK %lu, prev=%s\n", (unsigned long)mapOrphanBlocks.size(), pblock->hashPrevBlock.ToString());

        PruneOrphanBlocks();

        if (pfrom) {
            int nOrphansFromPeer = 0;
            if (PeerOrphanStorageLimitExceeded(pfrom->GetId(),
                                               &nOrphansFromPeer)) {
                if (!IbdHeaderSchedulerSelectActive())
                    pfrom->PushGetBlocks(
                        pindexBest, uint256(0),
                        ibdmetrics::GETBLOCKS_SOURCE_ORPHAN_LIMIT);

                if (IsInitialBlockDownload()) {
                    // EXPTRACE HOOK: IBD orphan-limit rejection (parent
                    // unknown by definition on this path).
                    ibdexptrace::NoteOrphanLimitReject(
                        pfrom->GetId(), nOrphansFromPeer,
                        (int)mapOrphanBlocks.size(), false);
                    TraceProcessBlockReject(pfrom, pblock, PBREJECT_ORPHAN_LIMIT_IBD);
                    ibdblocklatency::RecordBlockTerminal(hash, ibdblocklatency::OUTCOME_REJECTED);
                    return error("ProcessBlock() : peer %d exceeded orphan limit (IBD, no penalty)", pfrom->GetId());
                }
                // EXPTRACE HOOK: non-IBD orphan-limit rejection.
                ibdexptrace::NoteOrphanLimitReject(
                    pfrom->GetId(), nOrphansFromPeer,
                    (int)mapOrphanBlocks.size(), false);
                pfrom->Misbehaving(1);
                TraceProcessBlockReject(pfrom, pblock, PBREJECT_ORPHAN_LIMIT_NORMAL);
                ibdblocklatency::RecordBlockTerminal(hash, ibdblocklatency::OUTCOME_REJECTED);
                return error("ProcessBlock() : peer %d exceeded orphan limit", pfrom->GetId());
            }
        }

        // ppcoin: check proof-of-stake
        if (pblock->IsProofOfStake())
        {
            // Limited duplicity on stake: prevents block flood attack
            // Duplicate stake allowed only when there is orphan child block
            if (setStakeSeenOrphan.count(pblock->GetProofOfStake()) && !mapOrphanBlocksByPrev.count(hash) && !Checkpoints::WantedByPendingSyncCheckpoint(hash)) {
                TraceProcessBlockReject(pfrom, pblock, PBREJECT_DUPLICATE_STAKE_ORPHAN);
                ibdblocklatency::RecordBlockTerminal(hash, ibdblocklatency::OUTCOME_REJECTED);
                return error("ProcessBlock() : duplicate proof-of-stake (%s, %d) for orphan block %s", pblock->GetProofOfStake().first.ToString().c_str(), pblock->GetProofOfStake().second, hash.ToString().c_str());
            }
            else
                setStakeSeenOrphan.insert(pblock->GetProofOfStake());
        }
        CBlock* pblock2 = new CBlock(*pblock);
        mapOrphanBlocks.insert(make_pair(hash, pblock2));
        mapOrphanBlocksByPrev.insert(make_pair(pblock2->hashPrevBlock, pblock2));
        ibdblocklatency::RecordBlockOrphaned(hash);

        if (pfrom) {
            mapOrphanBlocksByNode[hash] = pfrom->GetId();
            mapOrphanCountByNode[pfrom->GetId()]++;
            BlockRequestTraceOrphanWatermark(
                pfrom->GetId(), mapOrphanCountByNode[pfrom->GetId()], "add");
            // EXPTRACE HOOK: orphan added to the per-peer holding area.
            ibdexptrace::NoteOrphanAdd(
                pfrom->GetId(), mapOrphanCountByNode[pfrom->GetId()],
                (int)mapOrphanBlocks.size());
        }

        // Ask this guy to fill in what we're missing
        if (pfrom)
        {
            // Parallel IBD delivery commonly creates temporary orphans whose
            // parent is already in the advertised batch.
            if (!IsInitialBlockDownload())
                pfrom->PushGetBlocks(
                    pindexBest, GetOrphanRoot(pblock2),
                    ibdmetrics::GETBLOCKS_SOURCE_INV_CONTINUATION);
            // ppcoin: getblocks may not obtain the ancestor block rejected
            // earlier by duplicate-stake check so we ask for it again directly
            uint256 hashWanted = WantedByOrphan(pblock2);
            BlockRequestTraceUpdateBlockContextLocked(hashWanted, hashWanted, hash);
            pfrom->AskFor(
                CInv(MSG_BLOCK, hashWanted),
                BLOCKREQ_SOURCE_ORPHAN);
            if (SyncTraceEnabled())
            {
                const bool fAdmitted = pfrom->IsBlockAskForQueued(hashWanted);
                NodeId nOwnerPeer = -1;
                BlockRequestOwnerState ownerState = BLOCK_REQUEST_OWNER_QUEUED;
                const bool fOwnerClaimed =
                    GetBlockRequestOwner(hashWanted, &nOwnerPeer, &ownerState) &&
                    nOwnerPeer == pfrom->GetId();
                BlockRequestTraceMissingParentRequest(
                    pfrom, hash, pblock->hashPrevBlock, hashWanted,
                    fAdmitted, fOwnerClaimed,
                    GetPeerOrphanCount(pfrom->GetId()),
                    mapOrphanBlocks.size());
            }
        }
        return true;
    }

    // Store to disk
    int64_t nAcceptStart = GetTimeMillis();
    bool fAcceptBlock = false;
    {
        CSyncLockPhase phase("ProcessMessage(block)", "acceptblock");
        fAcceptBlock = pblock->AcceptBlock();
    }
    if (!fAcceptBlock) {
        TraceProcessBlockReject(pfrom, pblock, PBREJECT_ACCEPTBLOCK_FALSE);
        ibdblocklatency::RecordBlockTerminal(hash, ibdblocklatency::OUTCOME_REJECTED);
        return error("ProcessBlock() : AcceptBlock FAILED");
    }
    int64_t nAcceptMs = GetTimeMillis() - nAcceptStart;

    // Recursively process any orphan blocks that depended on this one
    {
    CSyncLockPhase orphanPhase("ProcessMessage(block)", "orphan_processing");
    vector<uint256> vWorkQueue;
    vWorkQueue.push_back(hash);
    for (unsigned int i = 0; i < vWorkQueue.size(); i++)
    {
        uint256 hashPrev = vWorkQueue[i];
        for (multimap<uint256, CBlock*>::iterator mi = mapOrphanBlocksByPrev.lower_bound(hashPrev);
             mi != mapOrphanBlocksByPrev.upper_bound(hashPrev);
             ++mi)
        {
            CBlock* pblockOrphan = (*mi).second;
            uint256 orphanHash = pblockOrphan->GetHash();
            if (pblockOrphan->AcceptBlock())
            {
                vWorkQueue.push_back(orphanHash);
                RetryOrphanLimitRejectedOnParentConnect(orphanHash, pfrom);
            }
            mapOrphanBlocks.erase(orphanHash);
            // Release the stake marker only when no other stored orphan still
            // references the kernel (duplicate stakes are allowed on the
            // orphan path while an orphan child depends on the block).
            if (pblockOrphan->IsProofOfStake())
                EraseStakeSeenOrphanIfUnreferenced(pblockOrphan->GetProofOfStake());

            map<uint256, NodeId>::iterator nodeIt = mapOrphanBlocksByNode.find(orphanHash);
            if (nodeIt != mapOrphanBlocksByNode.end()) {
                mapOrphanCountByNode[nodeIt->second]--;
                BlockRequestTraceOrphanWatermark(
                    nodeIt->second, mapOrphanCountByNode[nodeIt->second], "remove");
                mapOrphanBlocksByNode.erase(nodeIt);
            }

            delete pblockOrphan;
        }
        mapOrphanBlocksByPrev.erase(hashPrev);
    }
    }

    if (fDebug && GetBoolArg("-showtimers", false)) {
        printf("ProcessBlock: ACCEPTED total=%" PRId64"ms check=%" PRId64"ms accept=%" PRId64"ms\n",
               GetTimeMillis() - nStartTime, nCheckMs, nAcceptMs);
    } else {
        if (fDebug) printf("ProcessBlock: ACCEPTED\n");
    }

    // ppcoin: if responsible for sync-checkpoint send it
    if (pfrom && !CSyncCheckpoint::strMasterPrivKey.empty())
        Checkpoints::SendSyncCheckpoint(Checkpoints::AutoSelectSyncCheckpoint()->GetBlockHash());

    return true;
}

// novacoin: attempt to generate suitable proof-of-stake
bool CBlock::SignBlock(CWallet& wallet, int64_t nFees)
{
    // if we are trying to sign
    //    something except proof-of-stake block template
    if (!vtx[0].vout[0].IsEmpty())
        return false;

    // if we are trying to sign
    //    a complete proof-of-stake block
    if (IsProofOfStake())
        return true;

    // nLastCoinStakeSearchTime = GetAdjustedTime(); // startup timestamp
    // nLastCoinStakeSearchTime = pindexBest->GetBlockTime(); // time of the last block in our index

    CKey key;
    CTransaction txCoinStake; // make a new transaction.
    int64_t nSearchTime = txCoinStake.nTime; // search to current time

    if (fDebug && GetBoolArg("-printcoinstake")) printf ("searchtime %ld to %ld \n",nSearchTime,nLastCoinStakeSearchTime);
    if (nSearchTime > nLastCoinStakeSearchTime)
    {
        if (fDebug && GetBoolArg("-printcoinstake")) printf ("nSearchTime %ld > nLastCoinStakeSearchTime %ld\n",nSearchTime,nLastCoinStakeSearchTime);
        if (wallet.CreateCoinStake(wallet, nBits, nSearchTime-nLastCoinStakeSearchTime, nFees, txCoinStake, key))
        {
            if (fDebug && GetBoolArg("-printcoinstake")) printf ("CreateCoinStake succeeded \n");
            if (txCoinStake.nTime >= max(pindexBest->GetPastTimeLimit()+1, PastDrift(pindexBest->GetBlockTime(), pindexBest->nHeight + 1)))
            {
                if (fDebug && GetBoolArg("-printcoinstake")) printf ("txCoinStake.nTime >= max(pindexBest->GetPastTimeLimit()+1, PastDrift(pindexBest->GetBlockTime()))");
                // make sure coinstake would meet timestamp protocol
                //    as it would be the same as the block timestamp
                vtx[0].nTime = nTime = txCoinStake.nTime;
                nTime = max(pindexBest->GetPastTimeLimit()+1, GetMaxTransactionTime());
                nTime = max(GetBlockTime(), PastDrift(pindexBest->GetBlockTime(), pindexBest->nHeight + 1));

                // we have to make sure that we have no future timestamps in
                //    our transactions set
                for (vector<CTransaction>::iterator it = vtx.begin(); it != vtx.end();)
                    if (it->nTime > nTime) { it = vtx.erase(it); } else { ++it; }

                vtx.insert(vtx.begin() + 1, txCoinStake);
                hashMerkleRoot = BuildMerkleTree();

                // append a signature to our block
                return key.Sign(GetHash(), vchBlockSig);
            }
        }
        nLastCoinStakeSearchInterval = nSearchTime - nLastCoinStakeSearchTime;
        nLastCoinStakeSearchTime = nSearchTime;
        if (fDebug && GetBoolArg("-printcoinstake")) printf ("CreateCoinStake failed at %ld. Try again in %ld\n",nLastCoinStakeSearchTime,nLastCoinStakeSearchInterval);
    }

    return false;
}

bool CBlock::CheckBlockSignature() const
{
    if (IsProofOfWork())
        return vchBlockSig.empty();

    // NullStake V1/V2: verify block signature against rk from the first shielded spend
    if (vtx[1].nVersion == SHIELDED_TX_VERSION_NULLSTAKE || vtx[1].nVersion == SHIELDED_TX_VERSION_NULLSTAKE_V2)
    {
        if (vchBlockSig.empty())
            return false;

        if (vtx[1].vShieldedSpend.empty() || vtx[1].vShieldedSpend[0].vchRk.empty())
            return false;

        if (vtx[1].vShieldedSpend[0].vchRk.size() != 33 && vtx[1].vShieldedSpend[0].vchRk.size() != 65)
            return false;

        CPubKey rkPubKey(vtx[1].vShieldedSpend[0].vchRk);
        if (!rkPubKey.IsValid() || !rkPubKey.IsFullyValid())
            return false;

        return rkPubKey.Verify(GetHash(), vchBlockSig);
    }

    // NullStake V3 (Private Cold Staking): verify block signature against pk_stake
    if (vtx[1].nVersion == SHIELDED_TX_VERSION_NULLSTAKE_COLD)
    {
        if (vchBlockSig.empty())
            return false;

        if (vtx[1].nullstakeProofV3.vchPkStake.size() != 33)
            return false;

        CPubKey pkStake(vtx[1].nullstakeProofV3.vchPkStake);
        if (!pkStake.IsValid() || !pkStake.IsFullyValid())
            return false;

        return pkStake.Verify(GetHash(), vchBlockSig);
    }

    vector<valtype> vSolutions;
    txnouttype whichType;

    const CTxOut& txout = vtx[1].vout[1];

    if (!Solver(txout.scriptPubKey, whichType, vSolutions))
        return false;

    if (whichType == TX_PUBKEY)
    {
        valtype& vchPubKey = vSolutions[0];
        return CPubKey(vchPubKey).Verify(GetHash(), vchBlockSig);
    }

    if (whichType == TX_COLDSTAKE)
    {
        const CScript& scriptSig = vtx[1].vin[0].scriptSig;
        CScript::const_iterator pc = scriptSig.begin();
        opcodetype opcode;
        valtype vchSig, vchFlag, vchPubKey;

        if (!scriptSig.GetOp(pc, opcode, vchSig))
            return false;
        if (!scriptSig.GetOp(pc, opcode, vchFlag))
            return false;
        if (!scriptSig.GetOp(pc, opcode, vchPubKey))
            return false;

        CPubKey pubkey(vchPubKey);
        if (!pubkey.IsValid())
            return false;

        CKeyID stakerKeyID = CKeyID(uint160(vSolutions[0]));
        if (pubkey.GetID() != stakerKeyID)
            return false;

        return pubkey.Verify(GetHash(), vchBlockSig);
    }

    return false;
}

bool CheckDiskSpace(uint64_t nAdditionalBytes)
{
    uint64_t nFreeBytesAvailable = fs::space(GetDataDir()).available;

    // Check for nMinDiskSpace bytes
    if (nFreeBytesAvailable < nMinDiskSpace + nAdditionalBytes)
    {
        fShutdown = true;
        string strMessage = _("Warning: Disk space is low!");
        strMiscWarning = strMessage;
        printf("*** %s\n", strMessage.c_str());
        uiInterface.ThreadSafeMessageBox(strMessage, "Innova", CClientUIInterface::OK | CClientUIInterface::ICON_EXCLAMATION | CClientUIInterface::MODAL);
        StartShutdown();
        return false;
    }
    return true;
}

static unsigned int nCurrentBlockFile = 1;

static fs::path BlockFilePath(unsigned int nFile)
{
    string strBlockFn = strprintf("blk%04u.dat", nFile);
    return GetDataDir() / strBlockFn;
}

FILE* OpenBlockFile(unsigned int nFile, unsigned int nBlockPos, const char* pszMode)
{
    if ((nFile < 1) || (nFile == (unsigned int) -1))
        return NULL;
    FILE* file = fopen(BlockFilePath(nFile).string().c_str(), pszMode);
    if (!file)
        return NULL;
    if (nBlockPos != 0 && !strchr(pszMode, 'a') && !strchr(pszMode, 'w'))
    {
        if (fseek(file, nBlockPos, SEEK_SET) != 0)
        {
            fclose(file);
            return NULL;
        }
    }
    return file;
}

FILE* AppendBlockFile(unsigned int& nFileRet)
{
    nFileRet = 0;
    while (true)
    {
        FILE* file = OpenBlockFile(nCurrentBlockFile, 0, "ab");
        if (!file)
            return NULL;
        if (fseek(file, 0, SEEK_END) != 0)
            return NULL;
        // FAT32 file size max 4GB, fseek and ftell max 2GB, so we must stay under 2GB
        if (ftell(file) < (long)(0x7F000000 - MAX_SIZE))
        {
            nFileRet = nCurrentBlockFile;
            return file;
        }
        fclose(file);
        nCurrentBlockFile++;
    }
}

bool RebuildMainChainForwardLinks()
{
    for (PAIRTYPE(const uint256, CBlockIndex*)& item : mapBlockIndex)
        item.second->pnext = NULL;

    if (pindexBest == NULL)
        return pindexGenesisBlock == NULL;

    CBlockIndex* pindex = pindexBest;
    int nLinks = 0;
    while (pindex->pprev)
    {
        if (pindex->pprev->nHeight >= pindex->nHeight)
            return error("RebuildMainChainForwardLinks() : invalid height link %d -> %d",
                         pindex->pprev->nHeight, pindex->nHeight);
        pindex->pprev->pnext = pindex;
        pindex = pindex->pprev;
        ++nLinks;
    }

    if (pindex != pindexGenesisBlock)
        return error("RebuildMainChainForwardLinks() : best chain does not end at genesis");

    printf("Rebuilt %d main-chain forward links\n", nLinks);
    return true;
}

bool LoadBlockIndex(bool fAllowNew)
{
    LOCK(cs_main);

    if (fRegTest)
    {
        pchMessageStart[0] = 0xfa;
        pchMessageStart[1] = 0xbf;
        pchMessageStart[2] = 0xb5;
        pchMessageStart[3] = 0xda;

        bnProofOfWorkLimit = CBigNum(~uint256(0) >> 1);
        nStakeMinAge = 0;
        nCoinbaseMaturity = 1;
        nTargetSpacing = 1;
    }
    else if (fTestNet)
    {
        pchMessageStart[0] = 0x9b;
        pchMessageStart[1] = 0x1d;
        pchMessageStart[2] = 0xfc;
        pchMessageStart[3] = 0x26;

        bnProofOfWorkLimit = bnProofOfWorkLimitTestNet; // 16 bits PoW target limit for testnet
        bnProofOfStakeLimit = bnProofOfStakeLimitTestNet; // much easier PoS for testnet
        nStakeMinAge = 1 * 60; // test net min age is 1 minute
        nCoinbaseMaturity = 15; // test maturity is 15 blocks
    };

    //
    // Load block index
    //
    CTxDB txdb("cr+");
    if (!txdb.LoadBlockIndex())
        return false;
    if (!pwalletMain->CacheAnonStats())
        printf("CacheAnonStats() failed.\n");

    //
    // Init with genesis block
    //
    if (mapBlockIndex.empty())
    {
        if (!fAllowNew)
            return false;

        if(fRegTest)
        {
            const char* pszTimestampRegTest = "Innova RegTest Mode";
            CTransaction txNewRegTest;

            txNewRegTest.nTime = 1296688602;
            txNewRegTest.vin.resize(1);
            txNewRegTest.vout.resize(1);
            txNewRegTest.vin[0].scriptSig = CScript() << 0 << CBigNum(42) << vector<unsigned char>((const unsigned char*)pszTimestampRegTest, (const unsigned char*)pszTimestampRegTest + strlen(pszTimestampRegTest));
            txNewRegTest.vout[0].SetEmpty();

            CBlock blockRegTest;
            blockRegTest.vtx.push_back(txNewRegTest);
            blockRegTest.hashPrevBlock = 0;
            blockRegTest.hashMerkleRoot = blockRegTest.BuildMerkleTree();
            blockRegTest.nTime    = 1296688602;
            blockRegTest.nVersion = 1;
            blockRegTest.nBits    = bnProofOfWorkLimit.GetCompact();
            blockRegTest.nNonce   = 2;

            printf("RegTest blockRegTest.GetHash() == %s\n", blockRegTest.GetHash().ToString().c_str());
            printf("RegTest blockRegTest.hashMerkleRoot == %s\n", blockRegTest.hashMerkleRoot.ToString().c_str());
            printf("RegTest blockRegTest.nBits = 0x%08x\n", blockRegTest.nBits);

            unsigned int nFile;
            unsigned int nBlockPos;
            if (!blockRegTest.WriteToDisk(nFile, nBlockPos))
                return error("RegTestLoadBlockIndex() : writing genesis block to disk failed");

            uint256 hashRegTestGenesis = blockRegTest.GetHash();
            if (!blockRegTest.AddToBlockIndex(nFile, nBlockPos, hashRegTestGenesis))
                return error("RegTestLoadBlockIndex() : genesis block not accepted");

            if (!Checkpoints::WriteSyncCheckpoint(hashRegTestGenesis))
                return error("RegTestLoadBlockIndex() : failed to init sync checkpoint");

            printf("RegTest genesis block initialized: %s\n", hashRegTestGenesis.ToString().c_str());
        }
        else if(fTestNet)
        {
            const char* pszTimestampTestNet = "Innova Public IDAG Hidden Finality Testnet | May 26 2026 | Epoch-Root FCMP";
            CTransaction txNewTestNet;

            txNewTestNet.nTime = 1779753600;
            txNewTestNet.vin.resize(1);
            txNewTestNet.vout.resize(1);
            txNewTestNet.vin[0].scriptSig = CScript() << 0 << CBigNum(42) << vector<unsigned char>((const unsigned char*)pszTimestampTestNet, (const unsigned char*)pszTimestampTestNet + strlen(pszTimestampTestNet));
            txNewTestNet.vout[0].SetEmpty();

            CBlock blocktest;
            blocktest.vtx.push_back(txNewTestNet);
            blocktest.hashPrevBlock = 0;
            blocktest.hashMerkleRoot = blocktest.BuildMerkleTree();
            blocktest.nTime    = 1779753600;
            blocktest.nVersion = 1;
            blocktest.nBits    = bnProofOfWorkLimit.GetCompact();
            blocktest.nNonce   = 127761;

            if (false && (blocktest.GetHash() != hashGenesisBlockTestNet))
            {
            // This will figure out a valid hash and Nonce if you're
            // creating a different genesis block:
                uint256 hashTarget = CBigNum().SetCompact(blocktest.nBits).getuint256();
                while (blocktest.GetHash() > hashTarget)
                {
                    ++blocktest.nNonce;
                    if (blocktest.nNonce == 0)
                    {
                        printf("NONCE WRAPPED, incrementing time");
                        ++blocktest.nTime;
                    }
                }
            }
            blocktest.print();
            printf("TestNet blocktest.GetHash() == %s\n", blocktest.GetHash().ToString().c_str());
            printf("TestNet blocktest.hashMerkleRoot == %s\n", blocktest.hashMerkleRoot.ToString().c_str());
            printf("TestNet blocktest.nTime = %u \n", blocktest.nTime);
            printf("TestNet blocktest.nNonce = %u \n", blocktest.nNonce);


            //// debug print
            if (blocktest.hashMerkleRoot != uint256("0xa18a28c4cde90e5c637c63715018a466511dd22eccd8f371512eec3074b1b19d"))
                return error("TestNetLoadBlockIndex() : invalid testnet genesis merkle root %s", blocktest.hashMerkleRoot.ToString().c_str());
            blocktest.print();
            if (blocktest.GetHash() != hashGenesisBlockTestNet)
                return error("TestNetLoadBlockIndex() : invalid testnet genesis hash %s", blocktest.GetHash().ToString().c_str());
            if (!blocktest.CheckBlock())
                return error("TestNetLoadBlockIndex() : testnet genesis block validation failed");

            // -- debug print
            if (fDebugChain)
            {
                printf("Initialised Innova TestNet genesis block:\n");
                blocktest.print();
            };

            // Start new block file
            unsigned int nFile;
            unsigned int nBlockPos;
            if (!blocktest.WriteToDisk(nFile, nBlockPos))
                return error("TestNetLoadBlockIndex() : writing genesis block to disk failed");
            if (!blocktest.AddToBlockIndex(nFile, nBlockPos, hashGenesisBlockTestNet))
                return error("TestNetLoadBlockIndex() : Testnet genesis block not accepted");

            // ppcoin: initialize synchronized checkpoint
            if (!Checkpoints::WriteSyncCheckpoint(hashGenesisBlockTestNet))
                return error("TestNetLoadBlockIndex() : failed to init sync checkpoint");

        } else {

            const char* pszTimestamp = "Innova Blockchain starts on 12/10/2019";
            CTransaction txNew;
            txNew.nTime = 1576002227;
            txNew.vin.resize(1);
            txNew.vout.resize(1);
            txNew.vin[0].scriptSig = CScript() << 0 << CBigNum(42) << vector<unsigned char>((const unsigned char*)pszTimestamp, (const unsigned char*)pszTimestamp + strlen(pszTimestamp));
            txNew.vout[0].SetEmpty();

            CBlock block;
            block.vtx.push_back(txNew);
            block.hashPrevBlock = 0;
            block.hashMerkleRoot = block.BuildMerkleTree();
            block.nTime    = 1576002227;
            block.nVersion = 1;
            block.nBits    = bnProofOfWorkLimit.GetCompact();
            block.nNonce   = 253080;

            if (false && (block.GetHash() != hashGenesisBlock)) {
            // This will figure out a valid hash and Nonce if you're
            // creating a different genesis block:
                uint256 hashTarget = CBigNum().SetCompact(block.nBits).getuint256();
                while (block.GetHash() > hashTarget)
                {
                    ++block.nNonce;
                    if (block.nNonce == 0)
                    {
                        printf("NONCE WRAPPED, incrementing time");
                        ++block.nTime;
                    }
                }
            }
            block.print();
            printf("block.GetHash() == %s\n", block.GetHash().ToString().c_str());
            printf("block.hashMerkleRoot == %s\n", block.hashMerkleRoot.ToString().c_str());
            printf("block.nTime = %u \n", block.nTime);
            printf("block.nNonce = %u \n", block.nNonce);


            //// debug print
            assert(block.hashMerkleRoot == uint256("0x7fe3177ea86b03a9c8773b32a3db36f32f4011bec4a0724032c36bc1c9d569a0"));
            block.print();
            assert(block.GetHash() == hashGenesisBlock);
            assert(block.CheckBlock());

            // -- debug print
            if (fDebugChain)
            {
                printf("Initialised genesis block:\n");
                block.print();
            };

            // Start new block file
            unsigned int nFile;
            unsigned int nBlockPos;
            if (!block.WriteToDisk(nFile, nBlockPos))
                return error("LoadBlockIndex() : writing genesis block to disk failed");
            if (!block.AddToBlockIndex(nFile, nBlockPos, hashGenesisBlock))
                return error("LoadBlockIndex() : genesis block not accepted");

            // ppcoin: initialize synchronized checkpoint
            if (!Checkpoints::WriteSyncCheckpoint(hashGenesisBlock))
                return error("LoadBlockIndex() : failed to init sync checkpoint");
        }
    }

    string strPubKey = "";

    // if checkpoint master key changed must reset sync-checkpoint
    if (!txdb.ReadCheckpointPubKey(strPubKey) || strPubKey != CSyncCheckpoint::strMasterPubKey)
    {
        // write checkpoint master key to db
        txdb.TxnBegin();
        if (!txdb.WriteCheckpointPubKey(CSyncCheckpoint::strMasterPubKey))
            return error("LoadBlockIndex() : failed to write new checkpoint master key to db");
        if (!txdb.TxnCommit())
            return error("LoadBlockIndex() : failed to commit new checkpoint master key to db");
        if ((!fTestNet) && (!fRegTest) && !Checkpoints::ResetSyncCheckpoint())
            return error("LoadBlockIndex() : failed to reset sync-checkpoint");
    }

    std::string strHRegRebuildError;
    if (!hreg::RebuildHRegStateFromActiveChain(strHRegRebuildError))
        return error("LoadBlockIndex() : HREG rebuild failed: %s", strHRegRebuildError.c_str());

    return true;
}



void PrintBlockTree()
{
    AssertLockHeld(cs_main);
    // pre-compute tree structure
    map<CBlockIndex*, vector<CBlockIndex*> > mapNext;
    for (map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.begin(); mi != mapBlockIndex.end(); ++mi)
    {
        CBlockIndex* pindex = (*mi).second;
        mapNext[pindex->pprev].push_back(pindex);
        // test
        //while (rand() % 3 == 0)
        //    mapNext[pindex->pprev].push_back(pindex);
    }

    vector<pair<int, CBlockIndex*> > vStack;
    vStack.push_back(make_pair(0, pindexGenesisBlock));

    int nPrevCol = 0;
    while (!vStack.empty())
    {
        int nCol = vStack.back().first;
        CBlockIndex* pindex = vStack.back().second;
        vStack.pop_back();

        // print split or gap
        if (nCol > nPrevCol)
        {
            for (int i = 0; i < nCol-1; i++)
                printf("| ");
            printf("|\\\n");
        }
        else if (nCol < nPrevCol)
        {
            for (int i = 0; i < nCol; i++)
                printf("| ");
            printf("|\n");
       }
        nPrevCol = nCol;

        // print columns
        for (int i = 0; i < nCol; i++)
            printf("| ");

        // print item
        CBlock block;
        block.ReadFromDisk(pindex);
        printf("%d (%u,%u) %s  %08x  %s  mint %7s  tx %" PRIszu"",
            pindex->nHeight,
            pindex->nFile,
            pindex->nBlockPos,
            block.GetHash().ToString().c_str(),
            block.nBits,
            DateTimeStrFormat("%x %H:%M:%S", block.GetBlockTime()).c_str(),
            FormatMoney(pindex->nMint).c_str(),
            block.vtx.size());

        //PrintWallets(block);

        // put the main time-chain first
        vector<CBlockIndex*>& vNext = mapNext[pindex];
        for (unsigned int i = 0; i < vNext.size(); i++)
        {
            if (vNext[i]->pnext)
            {
                swap(vNext[0], vNext[i]);
                break;
            }
        }

        // iterate children
        for (unsigned int i = 0; i < vNext.size(); i++)
            vStack.push_back(make_pair(nCol+i, vNext[i]));
    }
}

bool LoadExternalBlockFile(FILE* fileIn)
{
    int64_t nStart = GetTimeMillis();

    int nLoaded = 0;
    {
        LOCK(cs_main);
        try {
            CAutoFile blkdat(fileIn, SER_DISK, CLIENT_VERSION);
            unsigned int nPos = 0;
            while (nPos != (unsigned int)-1 && blkdat.good() && !fRequestShutdown)
            {
                unsigned char pchData[65536];
                do {
                    fseek(blkdat, nPos, SEEK_SET);
                    int nRead = fread(pchData, 1, sizeof(pchData), blkdat);
                    if (nRead <= 8)
                    {
                        nPos = (unsigned int)-1;
                        break;
                    }
                    void* nFind = memchr(pchData, pchMessageStart[0], nRead+1-sizeof(pchMessageStart));
                    if (nFind)
                    {
                        if (memcmp(nFind, pchMessageStart, sizeof(pchMessageStart))==0)
                        {
                            nPos += ((unsigned char*)nFind - pchData) + sizeof(pchMessageStart);
                            break;
                        }
                        nPos += ((unsigned char*)nFind - pchData) + 1;
                    }
                    else
                        nPos += sizeof(pchData) - sizeof(pchMessageStart) + 1;
                } while(!fRequestShutdown);
                if (nPos == (unsigned int)-1)
                    break;
                fseek(blkdat, nPos, SEEK_SET);
                unsigned int nSize;
                blkdat >> nSize;
                if (nSize > 0 && nSize <= MAX_BLOCK_SIZE)
                {
                    CBlock block;
                    blkdat >> block;
                    if (ProcessBlock(NULL,&block))
                    {
                        nLoaded++;
                        nPos += 4 + nSize;
                    }
                }
            }
        }
        catch (std::exception &e) {
            printf("%s() : Deserialize or I/O error caught during load: %s\n",
                   __PRETTY_FUNCTION__, e.what());
        }
    }
    printf("Loaded %i blocks from external file in %" PRId64"ms\n", nLoaded, GetTimeMillis() - nStart);
    return nLoaded > 0;
}

//////////////////////////////////////////////////////////////////////////////
//
// CAlert
//

extern map<uint256, CAlert> mapAlerts;
extern CCriticalSection cs_mapAlerts;

string GetWarnings(string strFor)
{
    int nPriority = 0;
    string strStatusBar;
    string strRPC;

    if (GetBoolArg("-testsafemode"))
        strRPC = "test";

    // Misc warnings like out of disk space and clock is wrong
    if (strMiscWarning != "")
    {
        nPriority = 1000;
        strStatusBar = strMiscWarning;
    }

    // if detected invalid checkpoint enter safe mode
    if (Checkpoints::hashInvalidCheckpoint != 0)
    {
        nPriority = 3000;
        strStatusBar = strRPC = _("WARNING: Invalid checkpoint found! Displayed transactions may not be correct! You may need to upgrade, or notify developers.");
    }

    // Alerts
    {
        LOCK(cs_mapAlerts);
        for (PAIRTYPE(const uint256, CAlert)& item : mapAlerts)
        {
            const CAlert& alert = item.second;
            if (alert.AppliesToMe() && alert.nPriority > nPriority)
            {
                nPriority = alert.nPriority;
                strStatusBar = alert.strStatusBar;
                if (nPriority > 1000)
                    strRPC = strStatusBar;
            }
        }
    }

    if (strFor == "statusbar")
        return strStatusBar;
    else if (strFor == "rpc")
        return strRPC;
    assert(!"GetWarnings() : invalid parameter");
    return "error";
}








//////////////////////////////////////////////////////////////////////////////
//
// Messages
//


bool static AlreadyHave(CTxDB& txdb, const CInv& inv)
{
    switch (inv.type)
    {
    case MSG_TX:
        {
        bool txInMap = false;
        txInMap = mempool.exists(inv.hash);
        return txInMap ||
               mapOrphanTransactions.count(inv.hash) ||
               txdb.ContainsTx(inv.hash);
        }

    case MSG_BLOCK:
        return mapBlockIndex.count(inv.hash) ||
               mapOrphanBlocks.count(inv.hash);
    case MSG_SPORK:
        return mapSporks.count(inv.hash);
    case MSG_COLLATERALNODE_WINNER:
        return mapSeenCollateralnodeVotes.count(inv.hash);
    }
    // Don't know what it is, just say we already got one
    return true;
}

// --- Bounded getdata block-serve budget ---
// Defaults to 1, which reproduces exact legacy behavior (at most one MSG_BLOCK
// served per ProcessGetData invocation).  A larger value drains a multi-block
// getdata burst across fewer msghand passes while keeping cs_main hold and the
// per-peer send buffer bounded.  Configured once in AppInit2, then immutable.
static int nGetDataBlockBudget = 1;
static const int GETDATA_BLOCK_BUDGET_MAX = 64;

void InitGetDataBlockBudget(int n)
{
    nGetDataBlockBudget = (n < 1) ? 1 : (n > GETDATA_BLOCK_BUDGET_MAX ? GETDATA_BLOCK_BUDGET_MAX : n);
}
int GetDataBlockBudget() { return nGetDataBlockBudget; }

// Lightweight diagnostic counters (single-writer: msghand thread only).
// DataBlockBudgetCounters struct is declared in net.h.
static uint64_t gDbCalls = 0, gDbBlockServed = 0, gDbBudgetHit = 0;
static uint64_t gDbSendBufferBreak = 0, gDbQueueRemaining = 0, gDbMaxServedPerCall = 0;

DataBlockBudgetCounters GetDataBlockBudgetCounters()
{
    DataBlockBudgetCounters c;
    c.calls = gDbCalls;
    c.blockServed = gDbBlockServed;
    c.budgetHit = gDbBudgetHit;
    c.sendBufferBreak = gDbSendBufferBreak;
    c.queueRemaining = gDbQueueRemaining;
    c.maxServedPerCall = gDbMaxServedPerCall;
    return c;
}
void ResetDataBlockBudgetCountersForTesting()
{
    gDbCalls = 0; gDbBlockServed = 0; gDbBudgetHit = 0;
    gDbSendBufferBreak = 0; gDbQueueRemaining = 0; gDbMaxServedPerCall = 0;
}

void static ProcessGetData(CNode* pfrom)
{
    if (fDebugNet)
      printf("ProcessGetData\n");

    std::deque<CInv>::iterator it = pfrom->vRecvGetData.begin();

    vector<CInv> vNotFound;

    LOCK(cs_main);

    ++gDbCalls;
    int nBlocksThisCall = 0;
    bool fSendBufferBreakThisCall = false;

    while (it != pfrom->vRecvGetData.end()) {
        // Don't bother if send buffer is too full to respond anyway
        if (pfrom->nSendSize >= SendBufferSize())
        {
            fSendBufferBreakThisCall = true;
            break;
        }

        if (fShutdown)
            return;

        const CInv &inv = *it;
        {
            boost::this_thread::interruption_point();
            it++;

            if (inv.type == MSG_BLOCK || inv.type == MSG_FILTERED_BLOCK)
            {
                bool send = false;
                // Send block from disk
                map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(inv.hash);
                if (mi != mapBlockIndex.end())
                {
                    pfrom->getBlocksServer.NoteBlockGetData(
                        inv.hash, mi->second->nHeight, GetTimeMillis());
                    GetBlocksServedInvNoteGetData(
                        pfrom->getBlocksServedInv, inv.hash,
                        mi->second->nHeight, GetTimeMicros());
                    // Real consumption immediately defeats any IP-keyed
                    // reconnect debt: a consuming peer must not leak
                    // zero-consumption debt into future reconnects.
                    GetBlocksServedInvReconnectClearedByConsumption(
                        static_cast<const CNetAddr&>(pfrom->addr),
                        GetTimeMicros());
                    send = true;
                    CBlock block;
                    block.ReadFromDisk((*mi).second);
                    if (inv.type == MSG_FILTERED_BLOCK)
                    {
                        LOCK(pfrom->cs_filter);
                        if (pfrom->pfilter)
                        {
                            CMerkleBlock merkleBlock(block, *pfrom->pfilter);
                            pfrom->PushMessage("merkleblock", merkleBlock);
                            typedef std::pair<unsigned int, uint256> PairType;
                            for (const PairType& pair : merkleBlock.vMatchedTxn)
                                if (!pfrom->setInventoryKnown.count(CInv(MSG_TX, pair.second)))
                                    pfrom->PushMessage("tx", block.vtx[pair.first]);
                        }
                    }
                    else
                    {
                        pfrom->PushMessage("block", block);
                        ++gDbBlockServed;
                    }

                    // Trigger them to send a getblocks request for the next batch of inventory
                    if (inv.hash == pfrom->hashContinue)
                    {
                        // Bypass PushInventory, this must send even if redundant,
                        // and we want it right after the last block so they don't
                        // wait for other stuff first.
                        vector<CInv> vInv;
                        vInv.push_back(CInv(MSG_BLOCK, hashBestChain));
                        pfrom->PushMessage("inv", vInv);
                        pfrom->hashContinue = 0;
                    }
                }
                // disconnect node in case we have reached the outbound limit for serving historical blocks
                static const int nOneWeek = 7 * 24 * 60 * 60; // assume > 1 week = historical
                if (send && CNode::OutboundTargetReached(true) &&
                (
                    ((pindexBest != NULL) &&
                    (pindexBest->GetBlockTime() - mi->second->GetBlockTime() > nOneWeek)) ||
                    inv.type == MSG_BLOCK
                    ) && !pfrom->fWhitelisted)
                {
                    printf("net historical block serving limit reached, disconnected peer=%d\n", pfrom->GetId());

                    //disconnect node
                    pfrom->fDisconnect = true;
                    send = false;
                }

            }
            else if (inv.IsKnownType())
            {
                // Send stream from relay memory
                bool pushed = false;
                /*{
                    LOCK(cs_mapRelay);
                    map<CInv, CDataStream>::iterator mi = mapRelay.find(inv);
                    if (mi != mapRelay.end()) {
                        pfrom->PushMessage(inv.GetCommand(), (*mi).second);
                        pushed = true;
                    }
                }*/
                if (!pushed && inv.type == MSG_TX) {
                    if(mapCollateralNBroadcastTxes.count(inv.hash)){
                        CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
                        ss.reserve(1000);
                        ss <<
                            mapCollateralNBroadcastTxes[inv.hash].tx <<
                            mapCollateralNBroadcastTxes[inv.hash].vin <<
                            mapCollateralNBroadcastTxes[inv.hash].vchSig <<
                            mapCollateralNBroadcastTxes[inv.hash].sigTime;

                        pfrom->PushMessage("dstx", ss);
                        pushed = true;
                    } else {
                        CTransaction tx;
                        if (mempool.lookup(inv.hash, tx)) {
                            CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
                            ss.reserve(1000);
                            ss << tx;
                            pfrom->PushMessage("tx", ss);
                            pushed = true;
                        }
                    }
                }
                if (!pushed && inv.type == MSG_SPORK) {
                    if(mapSporks.count(inv.hash)){
                        CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
                        ss.reserve(1000);
                        ss << mapSporks[inv.hash];
                        pfrom->PushMessage("spork", ss);
                        pushed = true;
                    }
                }
                if (!pushed && inv.type == MSG_COLLATERALNODE_WINNER) {
                    if(mapSeenCollateralnodeVotes.count(inv.hash)){
                        CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
                        int a = 0;
                        ss.reserve(1000);
                        ss << mapSeenCollateralnodeVotes[inv.hash] << a;
                        pfrom->PushMessage("mnw", ss);
                        pushed = true;
                    }
                }
                if (!pushed) {
                    vNotFound.push_back(inv);
                }
            }

            // Track requests for our stuff.
            g_signals.Inventory(inv.hash);

            // Bounded multi-block budget: MSG_BLOCK advances the per-pass counter
            // exactly where legacy advanced the single-block cap.  Legacy
            // (budget==1) breaks on the first MSG_BLOCK; a larger budget serves up
            // to nGetDataBlockBudget blocks per invocation.
            if (inv.type == MSG_BLOCK /* || inv.type == MSG_FILTERED_BLOCK */)
            {
                ++nBlocksThisCall;
                if (nBlocksThisCall >= nGetDataBlockBudget)
                {
                    ++gDbBudgetHit;
                    break;
                }
            }
        }
    }

    pfrom->vRecvGetData.erase(pfrom->vRecvGetData.begin(), it);

    gDbQueueRemaining = (uint64_t)pfrom->vRecvGetData.size();
    if ((uint64_t)nBlocksThisCall > gDbMaxServedPerCall)
        gDbMaxServedPerCall = (uint64_t)nBlocksThisCall;
    if (fSendBufferBreakThisCall)
        ++gDbSendBufferBreak;

    if (!vNotFound.empty()) {
        // Let the peer know that we didn't find what it asked for, so it doesn't
        // have to wait around forever. Currently only SPV clients actually care
        // about this message: it's needed when they are recursively walking the
        // dependencies of relevant unconfirmed transactions. SPV clients want to
        // do that because they want to know about (and store and rebroadcast and
        // risk analyze) the dependencies of transactions relevant to them, without
        // having to download the entire memory pool.
        pfrom->PushMessage("notfound", vNotFound);
    }
}

// Test hook: expose the file-local ProcessGetData to the unit-test harness
// (mirrors the ibdforensic::*ForTesting pattern; no production callers).
void ProcessGetDataForTesting(CNode* pfrom) { ProcessGetData(pfrom); }

// The message start string is designed to be unlikely to occur in normal data.
// The characters are rarely used upper ASCII, not valid as UTF-8, and produce
// a large 4-byte int at any alignment.
unsigned char pchMessageStart[4] = { 0xfa, 0xf4, 0x3f, 0xb7 };

// Serving-side getheaders dedup toggle.  Initialized once in AppInit2 before
// networking threads start, then immutable.
static bool fHeadersServedDedupEnabled = false;

void InitHeadersServedDedup(bool fEnabled)
{
    fHeadersServedDedupEnabled = fEnabled;
}

bool HeadersServedDedupEnabled()
{
    return fHeadersServedDedupEnabled;
}

uint256 HeaderServedDedupFingerprint(const CBlockLocator& locator,
                                     const uint256& hashStop,
                                     const uint256& tipHash)
{
    CHashWriter ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << locator;
    ss << hashStop;
    ss << tipHash;
    return ss.GetHash();
}

bool HeaderServedDedupEntryFresh(const CNode::CHeadersServedDedupEntry& entry,
                                 int64_t nNowUs)
{
    int64_t nAgeUs = nNowUs - entry.nServedUs;
    return nAgeUs >= 0 && nAgeUs < HEADER_SERVED_DEDUP_TTL * 1000000;
}

void HeaderServedDedupUpsert(std::map<uint256, CNode::CHeadersServedDedupEntry>& map,
                             const uint256& fp, int64_t nNowUs,
                             uint32_t nHeadersCount, uint64_t nBytes)
{
    // Purge expired entries.
    for (std::map<uint256, CNode::CHeadersServedDedupEntry>::iterator it = map.begin();
         it != map.end();)
    {
        if (nNowUs - it->second.nServedUs >= HEADER_SERVED_DEDUP_TTL * 1000000)
            map.erase(it++);
        else
            ++it;
    }
    // Enforce the hard cap: evict the oldest entry when full and fp is new.
    if (map.count(fp) == 0 && map.size() >= HEADER_SERVED_DEDUP_CAP)
    {
        std::map<uint256, CNode::CHeadersServedDedupEntry>::iterator itOldest = map.begin();
        for (std::map<uint256, CNode::CHeadersServedDedupEntry>::iterator it = map.begin();
             it != map.end(); ++it)
        {
            if (it->second.nServedUs < itOldest->second.nServedUs)
                itOldest = it;
        }
        map.erase(itOldest);
    }
    CNode::CHeadersServedDedupEntry& entry = map[fp];
    entry.nServedUs = nNowUs;
    entry.nRepeat += 1;
    entry.nHeadersCount = nHeadersCount;
    entry.nBytes = nBytes;
}

// ---------------------------------------------------------------------------
// Serving-side getblocks -> inv ZERO-CONSUMPTION suppression.
//
// Mechanism (separate from policy): per-peer served-inv window accounting.
// Each served getblocks inv reply accumulates items/bytes into the current
// window and its (firstHash, lastHash, min/max height) footprint into a
// bounded recent-served set.  A getdata whose block matches a recently served
// window counts as consumption.  A strict-inbound, non-whitelisted peer that
// repeats an overlapping range at an unchanged chain tip with zero consumption
// is, once the streak threshold is met, no longer served that inv: the reply
// is dropped without writing anything.  All state is guarded by cs_main, which
// both the getblocks handler and the getdata-serving path hold.
// ---------------------------------------------------------------------------
static bool fGetBlocksServedInvZeroEnabled = false;
static GetBlocksServedInvZeroConfig gGetBlocksServedInvZeroConfig;

GetBlocksServedInvZeroConfig::GetBlocksServedInvZeroConfig()
    : nGraceS(GETBLOCKS_SERVED_INV_GRACE_S),
      nMinItems(GETBLOCKS_SERVED_INV_MIN_ITEMS),
      nInitialStreak(GETBLOCKS_SERVED_INV_INITIAL_STREAK),
      nReentryStreak(GETBLOCKS_SERVED_INV_REENTRY_STREAK),
      nRecentWindowCap(GETBLOCKS_SERVED_INV_RECENT_WINDOW_CAP),
      nWindowExpiryS(GETBLOCKS_SERVED_INV_WINDOW_EXPIRY_S)
{
}

void InitGetBlocksServedInvZero(bool fEnabled)
{
    fGetBlocksServedInvZeroEnabled = fEnabled;
    GetBlocksServedInvZeroConfig cfg;
    cfg.nGraceS = std::max<int64_t>(
        1, GetArg("-getblocksservedinvgrace",
                  (int64_t)GETBLOCKS_SERVED_INV_GRACE_S));
    cfg.nMinItems = (uint64_t)std::max<int64_t>(
        0, GetArg("-getblocksservedinvminitems",
                  (int64_t)GETBLOCKS_SERVED_INV_MIN_ITEMS));
    cfg.nInitialStreak = (unsigned int)std::max<int64_t>(
        1, GetArg("-getblocksservedinvstreak",
                  (int64_t)GETBLOCKS_SERVED_INV_INITIAL_STREAK));
    cfg.nReentryStreak = (unsigned int)std::max<int64_t>(
        1, GetArg("-getblocksservedinvreentry",
                  (int64_t)GETBLOCKS_SERVED_INV_REENTRY_STREAK));
    cfg.nRecentWindowCap = (size_t)std::max<int64_t>(
        1, GetArg("-getblocksservedinvwindow",
                  (int64_t)GETBLOCKS_SERVED_INV_RECENT_WINDOW_CAP));
    cfg.nWindowExpiryS = std::max<int64_t>(
        1, GetArg("-getblocksservedinvexpiry",
                  (int64_t)GETBLOCKS_SERVED_INV_WINDOW_EXPIRY_S));
    SetGetBlocksServedInvZeroConfig(cfg);
}

bool GetBlocksServedInvZeroEnabled()
{
    return fGetBlocksServedInvZeroEnabled;
}

const GetBlocksServedInvZeroConfig& GetBlocksServedInvConfig()
{
    return gGetBlocksServedInvZeroConfig;
}

void SetGetBlocksServedInvZeroConfig(const GetBlocksServedInvZeroConfig& cfg)
{
    gGetBlocksServedInvZeroConfig = cfg;
}

GetBlocksServedInvDecision::GetBlocksServedInvDecision()
    : fSuppress(false),
      fQualify(false),
      fDisarm(false),
      nItemsAvoided(0),
      nBytesAvoided(0)
{
}

static bool GbServedInvRangesOverlap(int nAMin, int nAMax,
                                     int nBMin, int nBMax)
{
    if (nAMin < 0 || nAMax < 0 || nAMin > nAMax ||
        nBMin < 0 || nBMax < 0 || nBMin > nBMax)
        return false;
    return nAMin <= nBMax && nBMin <= nAMax;
}

bool GetBlocksServedInvWindowOverlaps(
    const CNode::CGetBlocksServedInvState& state,
    const CGetBlocksRequestInfo& request,
    int64_t nNowUs)
{
    if (!state.fGbHaveWindow || request.nPredictedResponseCount == 0)
        return false;
    const int nPredMin = request.nResolvedHeight + 1;
    const int nPredMax = request.nResolvedHeight +
        static_cast<int>(request.nPredictedResponseCount);
    const int64_t nExpiryUs =
        GetBlocksServedInvConfig().nWindowExpiryS * 1000000;
    for (size_t i = 0; i < state.vGbServedWindows.size(); ++i)
    {
        const CNode::CGetBlocksServedInvWindow& entry =
            state.vGbServedWindows[i];
        if (entry.nServedUs == 0 || nNowUs - entry.nServedUs >= nExpiryUs)
            continue;
        if (GbServedInvRangesOverlap(nPredMin, nPredMax,
                                     entry.nMinHeight, entry.nMaxHeight))
            return true;
    }
    return false;
}

static void GetBlocksServedInvResetWindow(
    CNode::CGetBlocksServedInvState& state, int64_t nNowUs)
{
    state.nGbServedInvWindowStartUs = nNowUs;
    state.nGbServedInvItems = 0;
    state.nGbServedInvBytes = 0;
    state.nGbGetDataMatches = 0;
}

GetBlocksServedInvDecision GetBlocksServedInvEvaluate(
    CNode::CGetBlocksServedInvState& state,
    const CGetBlocksRequestInfo& request,
    bool fStrictInbound,
    int64_t nNowUs,
    uint64_t nPredictedItems)
{
    GetBlocksServedInvDecision decision;
    decision.nItemsAvoided = nPredictedItems;
    decision.nBytesAvoided = nPredictedItems * 36;

    if (!GetBlocksServedInvZeroEnabled() || !fStrictInbound)
        return decision;
    if (!state.fGbHaveWindow)
        return decision;

    const GetBlocksServedInvZeroConfig& cfg = GetBlocksServedInvConfig();

    // Disarm: the response legitimately changed (tip advance or reorg/fork).
    if (request.hashChainTip != state.hashGbLastResponseChainTip)
    {
        GetBlocksServedInvResetWindow(state, nNowUs);
        state.nGbZeroConsumeStreak = 0;
        state.fGbSuppressInv = false;
        state.vGbServedWindows.clear();
        decision.fDisarm = true;
        return decision;
    }

    // Disarm: a genuinely-new (non-overlapping) range the peer is exploring
    // is still served.  No suppression until re-evidenced on this range.
    if (!GetBlocksServedInvWindowOverlaps(state, request, nNowUs))
    {
        GetBlocksServedInvResetWindow(state, nNowUs);
        state.nGbZeroConsumeStreak = 0;
        state.fGbSuppressInv = false;
        decision.fDisarm = true;
        return decision;
    }

    if (nNowUs - state.nGbServedInvWindowStartUs < cfg.nGraceS * 1000000)
        return decision;
    if (state.nGbServedInvItems < cfg.nMinItems)
        return decision;

    // Zero consumption is the load-bearing conservative signal: a
    // low-but-nonzero consumer (e.g. 0.75% of the window) is NEVER suppressed
    // by this first policy.
    if (state.nGbGetDataMatches > 0)
    {
        ibdmetrics::Get().getblocks_low_nonzero_windows.fetch_add(
            1, std::memory_order_relaxed);
        return decision;
    }

    decision.fQualify = true;
    ibdmetrics::Get().getblocks_zero_consume_windows.fetch_add(
        1, std::memory_order_relaxed);
    const unsigned int nRequiredStreak = state.fGbPriorZeroConsume
        ? cfg.nReentryStreak
        : cfg.nInitialStreak;
    if (state.nGbZeroConsumeStreak + 1 >= nRequiredStreak)
    {
        decision.fSuppress = true;
        if (state.fGbPriorZeroConsume)
        {
            ibdmetrics::Get().getblocks_reentry_ae.fetch_add(
                1, std::memory_order_relaxed);
        }
    }
    return decision;
}

void GetBlocksServedInvRecordItem(CNode::CGetBlocksServedInvState& state,
                                  int64_t nNowUs)
{
    if (!GetBlocksServedInvZeroEnabled())
        return;
    if (state.nGbServedInvWindowStartUs == 0)
        state.nGbServedInvWindowStartUs = nNowUs;
    state.nGbServedInvItems++;
    state.nGbServedInvBytes += 36;
    ibdmetrics::Get().getblocks_served_inv_items.fetch_add(
        1, std::memory_order_relaxed);
    ibdmetrics::Get().getblocks_served_inv_bytes.fetch_add(
        36, std::memory_order_relaxed);
}

void GetBlocksServedInvRecordResponse(
    CNode::CGetBlocksServedInvState& state,
    const CGetBlocksResponseInfo& response,
    const uint256& hashChainTip,
    int64_t nNowUs)
{
    if (!GetBlocksServedInvZeroEnabled())
        return;
    state.fGbHaveWindow = true;
    state.hashGbLastResponseChainTip = hashChainTip;

    const GetBlocksServedInvZeroConfig& cfg = GetBlocksServedInvConfig();
    const int64_t nExpiryUs = cfg.nWindowExpiryS * 1000000;

    // Purge expired windows.
    for (std::vector<CNode::CGetBlocksServedInvWindow>::iterator it =
             state.vGbServedWindows.begin();
         it != state.vGbServedWindows.end();)
    {
        if (it->nServedUs == 0 || nNowUs - it->nServedUs >= nExpiryUs)
            it = state.vGbServedWindows.erase(it);
        else
            ++it;
    }

    CNode::CGetBlocksServedInvWindow entry;
    entry.hashFirst = response.hashFirst;
    entry.hashLast = response.hashLast;
    entry.nMinHeight = response.nMinHeight;
    entry.nMaxHeight = response.nMaxHeight;
    entry.nServedUs = nNowUs;
    state.vGbServedWindows.push_back(entry);
    while (state.vGbServedWindows.size() > cfg.nRecentWindowCap)
        state.vGbServedWindows.erase(state.vGbServedWindows.begin());
}

bool GetBlocksServedInvNoteGetData(
    CNode::CGetBlocksServedInvState& state,
    const uint256& hashBlock, int nHeight, int64_t nNowUs)
{
    if (!GetBlocksServedInvZeroEnabled() || !state.fGbHaveWindow)
        return false;

    const int64_t nExpiryUs =
        GetBlocksServedInvConfig().nWindowExpiryS * 1000000;

    bool fMatch = false;
    for (size_t i = 0; i < state.vGbServedWindows.size(); ++i)
    {
        const CNode::CGetBlocksServedInvWindow& entry =
            state.vGbServedWindows[i];
        if (entry.nServedUs == 0 || nNowUs - entry.nServedUs >= nExpiryUs)
            continue;
        const bool fHashMatch =
            hashBlock == entry.hashFirst || hashBlock == entry.hashLast;
        const bool fHeightMatch =
            nHeight >= 0 &&
            entry.nMinHeight >= 0 &&
            entry.nMaxHeight >= entry.nMinHeight &&
            nHeight >= entry.nMinHeight &&
            nHeight <= entry.nMaxHeight;
        if (fHashMatch || fHeightMatch)
        {
            fMatch = true;
            break;
        }
    }
    if (!fMatch)
        return false;

    state.nGbGetDataMatches++;
    ibdmetrics::Get().getblocks_consumption_getdata_matches.fetch_add(
        1, std::memory_order_relaxed);
    if (state.fGbSuppressInv)
    {
        state.fGbSuppressInv = false;
        ibdmetrics::Get().getblocks_release_latch_events.fetch_add(
            1, std::memory_order_relaxed);
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Reconnect-persistent zero-consumption debt.
//
// A bounded, short-TTL, CNetAddr-keyed map that carries a tiny "debt" from a
// zero-consumption peer's previous connection into a reconnecting one, so a
// same-peer reconnect does not trivially reset all evidence.  Only real
// ZERO-consumption evidence is ever written; a peer that consumes (any
// matches>0) is never written and its existing debt is cleared by consumption.
// Bootstrap-safety invariant: priming a new connection never starts it
// suppressed (fSuppressInv stays false) and re-stamps a FRESH window, so the
// GRACE window always grants a genuine serving opportunity before any
// suppression could fire.  This map is a leaf (its mutex is never acquired
// while holding cs_main/cs_vNodes or any served-dedup lock).
// ---------------------------------------------------------------------------
namespace
{
struct CGetBlocksServedInvReconnectStore
{
    std::map<CNetAddr, CGetBlocksServedInvReconnectDebtEntry> m;
    CCriticalSection mtx; // leaf lock (matches LOCK()/CCriticalBlock)
};
CGetBlocksServedInvReconnectStore g_reconnectDebt;
} // namespace

void GetBlocksServedInvReconnectWrite(const CNode* pnode, int64_t nNowUs)
{
    if (!pnode || !GetBlocksServedInvZeroEnabled())
        return;
    GetBlocksServedInvReconnectWriteState(
        pnode->getBlocksServedInv,
        static_cast<const CNetAddr&>(pnode->addr), nNowUs);
}

void GetBlocksServedInvReconnectWriteState(
    const CNode::CGetBlocksServedInvState& s, const CNetAddr& addr,
    int64_t nNowUs)
{
    if (!GetBlocksServedInvZeroEnabled())
        return;
    const GetBlocksServedInvZeroConfig& cfg = GetBlocksServedInvConfig();
    // Write debt only on real zero-consumption evidence.
    bool fZero = s.fGbSuppressInv ||
                 (s.fGbHaveWindow && s.nGbGetDataMatches == 0 &&
                  s.nGbServedInvItems >= cfg.nMinItems &&
                  s.fGbPriorZeroConsume);
    if (!fZero)
        return;
    uint64_t nDebtItems = s.nGbServedInvItems;
    uint64_t nCap = std::max<uint64_t>(cfg.nMinItems, (uint64_t)4096);
    if (nDebtItems > nCap)
        nDebtItems = nCap;
    unsigned int nStreak = s.nGbZeroConsumeStreak;
    if (nStreak < 1)
        nStreak = 1;
    LOCK(g_reconnectDebt.mtx);
    {
        for (auto it = g_reconnectDebt.m.begin();
             it != g_reconnectDebt.m.end();)
        {
            if (nNowUs - it->second.nLastSeenUs >
                GETBLOCKS_SERVED_INV_RECONNECT_DEBT_TTL_US)
                it = g_reconnectDebt.m.erase(it);
            else
                ++it;
        }
    }
    if (g_reconnectDebt.m.find(addr) == g_reconnectDebt.m.end() &&
        g_reconnectDebt.m.size() >= GETBLOCKS_SERVED_INV_RECONNECT_DEBT_CAP)
    {
        auto oldest = g_reconnectDebt.m.begin();
        for (auto it = g_reconnectDebt.m.begin();
             it != g_reconnectDebt.m.end(); ++it)
            if (it->second.nLastSeenUs < oldest->second.nLastSeenUs)
                oldest = it;
        g_reconnectDebt.m.erase(oldest);
        ibdmetrics::Get().getblocks_reconnect_debt_evicted.fetch_add(
            1, std::memory_order_relaxed);
    }
    CGetBlocksServedInvReconnectDebtEntry& e = g_reconnectDebt.m[addr];
    e.nDebtItems = nDebtItems;
    e.nDebtStreak = nStreak;
    e.nLastSeenUs = nNowUs;
    ibdmetrics::Get().getblocks_reconnect_debt_entries.store(
        (int64_t)g_reconnectDebt.m.size(), std::memory_order_relaxed);
}

void GetBlocksServedInvReconnectPrime(
    CNode::CGetBlocksServedInvState& state, const CNetAddr& addr,
    int64_t nNowUs, bool& fPrimed)
{
    fPrimed = false;
    if (!GetBlocksServedInvZeroEnabled())
        return;
    const GetBlocksServedInvZeroConfig& cfg = GetBlocksServedInvConfig();
    LOCK(g_reconnectDebt.mtx);
    {
        for (auto it = g_reconnectDebt.m.begin();
             it != g_reconnectDebt.m.end();)
        {
            if (nNowUs - it->second.nLastSeenUs >
                GETBLOCKS_SERVED_INV_RECONNECT_DEBT_TTL_US)
                it = g_reconnectDebt.m.erase(it);
            else
                ++it;
        }
    }
    auto it = g_reconnectDebt.m.find(addr);
    if (it == g_reconnectDebt.m.end())
    {
        ibdmetrics::Get().getblocks_reconnect_debt_entries.store(
            (int64_t)g_reconnectDebt.m.size(), std::memory_order_relaxed);
        return;
    }
    // Bootstrap safety: never start suppressed.  Prime items toward MIN_ITEMS
    // and set the re-entry history marker so re-qualification is FASTER, but
    // stamp a FRESH window: GRACE must elapse (and, on this connection, the
    // peer must re-demonstrate zero consumption) before suppression can fire,
    // so the first INV -> getdata -> block round trip is always granted.
    state.nGbServedInvWindowStartUs = nNowUs;
    state.nGbServedInvItems =
        std::min<uint64_t>(it->second.nDebtItems, cfg.nMinItems);
    state.fGbPriorZeroConsume = true;
    state.fGbSuppressInv = false;
    state.nGbGetDataMatches = 0;
    state.nGbZeroConsumeStreak = 0;
    fPrimed = true;
    ibdmetrics::Get().getblocks_reconnect_debt_transferred.fetch_add(
        1, std::memory_order_relaxed);
}

void GetBlocksServedInvReconnectClearedByConsumption(const CNetAddr& addr,
                                                     int64_t nNowUs)
{
    (void)nNowUs;
    if (!GetBlocksServedInvZeroEnabled())
        return;
    LOCK(g_reconnectDebt.mtx);
    auto it = g_reconnectDebt.m.find(addr);
    if (it == g_reconnectDebt.m.end())
        return;
    g_reconnectDebt.m.erase(it);
    ibdmetrics::Get().getblocks_reconnect_debt_entries.store(
        (int64_t)g_reconnectDebt.m.size(), std::memory_order_relaxed);
    ibdmetrics::Get().getblocks_reconnect_debt_cleared_by_consumption
        .fetch_add(1, std::memory_order_relaxed);
}

size_t GetBlocksServedInvReconnectSize()
{
    LOCK(g_reconnectDebt.mtx);
    return g_reconnectDebt.m.size();
}

void GetBlocksServedInvReconnectClearAll()
{
    LOCK(g_reconnectDebt.mtx);
    g_reconnectDebt.m.clear();
    ibdmetrics::Get().getblocks_reconnect_debt_entries.store(
        0, std::memory_order_relaxed);
}

bool static ProcessMessage(CNode* pfrom, string strCommand, CDataStream& vRecv, int64_t nTimeReceived)
{
    static map<CService, CPubKey> mapReuseKey;
    RandAddSeedPerfmon();
    if (fDebugNet)
        printf("received: %s (%" PRIszu" bytes)\n", strCommand.c_str(), vRecv.size());
    if (mapArgs.count("-dropmessagestest") && GetRand(atoi(mapArgs["-dropmessagestest"])) == 0)
    {
        printf("dropmessagestest DROPPING RECV MESSAGE\n");
        return true;
    }

    if (strCommand == "version")
    {
        // Each connection can only send one version message
        if (pfrom->nVersion != 0)
        {
            pfrom->Misbehaving(1);
            return false;
        }

        int64_t nTime;
        CAddress addrMe;
        CAddress addrFrom;
        uint64_t nNonce = 1;
        vRecv >> pfrom->nVersion >> pfrom->nServices >> nTime >> addrMe;

        // Old Node Versioning with Block Height Code
        bool oldVersion = false;

        if (pfrom->nVersion < MIN_PEER_PROTO_VERSION)
            oldVersion = true;

        /*
        if (pfrom->nVersion < PROTO_VERSION)
        {
            // disconnect from peers older than this proto version
            printf("partner %s using obsolete version %i; disconnecting\n", pfrom->addr.ToString().c_str(), pfrom->nVersion);
            pfrom->fDisconnect = true;
            return false;
        }*/

        if (pfrom->nVersion == 10300)
            pfrom->nVersion = 300;
        if (!vRecv.empty())
            vRecv >> addrFrom >> nNonce;
        if (!vRecv.empty())
        {
            vRecv >> pfrom->strSubVer;
            if (pfrom->strSubVer.size() > 256)
                pfrom->strSubVer.resize(256);
        }
        if (!vRecv.empty())
        {
            vRecv >> pfrom->nChainHeight;
            pfrom->UpdateBestKnownBlock(pfrom->nChainHeight, uint256(0));
        }

        // Disconnect if the peer's subversion is < /Innovai:3.3.9.14/
        // Leaving this out for now until new update is out for a bit
        // if (pfrom->strSubVer != "/Innovai:3.3.9.14/")
        //     oldVersion = true;

        // print the current pfrom->strSubVer
        printf("ProcessMessage(): peer=%s using SubVer=%s, oldVersion=%s\n", pfrom->addr.ToString().c_str(), pfrom->strSubVer.c_str(), oldVersion ? "true" : "false");

        if (oldVersion == true)
        {
          printf("Partner %s using obsolete version %i; DISCONNECTING\n", pfrom->addr.ToString().c_str(), pfrom->nVersion);
          pfrom->fDisconnect = true;
          if (pfrom->fColLateralMaster)
              printf("Masternode hosting node version was obsolete. This masternode should be removed from the list\n");
          return false;
        }

        // if (pfrom->nSendBytes >= 1000000) // New arg flag per peer 1MB 1000000 bytes
        // {
        //     printf("data sent by peer = %i, disconnecting\n", pfrom->nSendBytes);
        //     printf("disconnecting node from max outbound per peer target: %s\n", pfrom->addr.ToString().c_str());
        //     pfrom->fDisconnect = true;
        //     return false;
        // }

        if (pfrom->fInbound && addrMe.IsRoutable())
        {
            pfrom->addrLocal = addrMe;
            SeenLocal(addrMe);
        }

        // Disconnect if we connected to ourself
        if (nNonce == nLocalHostNonce && nNonce > 1)
        {
            printf("connected to self at %s, disconnecting\n", pfrom->addr.ToString().c_str());
            pfrom->fDisconnect = true;
            return true;
        }

        // record my external IP reported by peer
        if (addrFrom.IsRoutable() && addrMe.IsRoutable())
            addrSeenByPeer = addrMe;


        pfrom->fClient = !(pfrom->nServices & NODE_NETWORK);

        if (GetBoolArg("-synctime", true))
            AddTimeData(pfrom->addr, nTime);

        // Change version
        pfrom->PushMessage("verack");
        pfrom->ssSend.SetVersion(min(pfrom->nVersion, PROTOCOL_VERSION));

        if (!pfrom->fInbound)
        {
            // Advertise our address
            if (!fNoListen && !IsInitialBlockDownload())
            {
                CAddress addr = GetLocalAddress(&pfrom->addr);
                if (addr.IsRoutable())
                    pfrom->PushAddress(addr);
            }

            // Get recent addresses
            if (pfrom->fOneShot || pfrom->nVersion >= CADDR_TIME_VERSION || addrman.size() < 1000)
            {
                pfrom->PushMessage("getaddr");
                pfrom->fGetAddr = true;
            }
            addrman.Good(pfrom->addr);
        } else {
            if (((CNetAddr)pfrom->addr) == (CNetAddr)addrFrom)
            {
                addrman.Add(addrFrom, addrFrom);
                addrman.Good(addrFrom);
            }
        }

        // Ask every node for the collateralnode list straight away
        pfrom->PushMessage("iseg", CTxIn());

        // Full nodes use the historical getblocks/inv path. Header-first
        // synchronization is reserved for SPV mode, where headers are indexed.
        if (!fSPVMode && !IbdHeaderSchedulerSelectActive() &&
            !pfrom->fClient && !pfrom->fOneShot &&
            (pfrom->nBestKnownHeight < 0 ||
             pfrom->nBestKnownHeight > (nBestHeight - 144)) &&
            IsBlockSyncPeerVersion(pfrom->nVersion))
        {
            pfrom->PushGetBlocks(
                pindexBest, uint256(0), ibdmetrics::GETBLOCKS_SOURCE_VERSION);
        }

        // Relay alerts
        {
            LOCK(cs_mapAlerts);
            for (PAIRTYPE(const uint256, CAlert)& item : mapAlerts)
                item.second.RelayTo(pfrom);
        }

        // Relay sync-checkpoint
        {
            LOCK(Checkpoints::cs_hashSyncCheckpoint);
            if (!Checkpoints::checkpointMessage.IsNull())
                Checkpoints::checkpointMessage.RelayTo(pfrom);
        }

        pfrom->fSuccessfullyConnected = true;
        pfrom->fRelayTxes = true;

        printf("receive version message: version %d, blocks=%d, us=%s, them=%s, peer=%s\n", pfrom->nVersion, pfrom->nChainHeight, addrMe.ToString().c_str(), addrFrom.ToString().c_str(), pfrom->addr.ToString().c_str());

        cPeerBlockCounts.input(pfrom->nBestKnownHeight >= 0 ? pfrom->nBestKnownHeight : pfrom->nChainHeight);

        // ppcoin: ask for pending sync-checkpoint if any
        if (!IsInitialBlockDownload())
            Checkpoints::AskForPendingSyncCheckpoint(pfrom);
    }


    else if (pfrom->nVersion == 0)
    {
        // Must have a version message before anything else, as it is sent as soon as the socket opens
        pfrom->Misbehaving(1);
        if (fDebug) printf("net: received an out-of-sequence %s from peer at %s\n", strCommand.c_str(), pfrom->addr.ToString().c_str());
        if (pfrom->nMisbehavior > 10 || pfrom->nTimeConnected < GetTime() - 10)
            pfrom->fDisconnect = true; // Disconnect them so we can reconnect and try for another version message
        return false;
    }


    else if (strCommand == "verack")
    {
        pfrom->SetRecvVersion(min(pfrom->nVersion, PROTOCOL_VERSION));
        printf("net: received verack from peer version %d (recvVersion: %d) at %s\n", pfrom->nVersion, pfrom->nRecvVersion, pfrom->addr.ToString().c_str());

        if (fSPVMode)
        {
            pfrom->PushMessage("sendheaders");
            printf("SPV: Requesting headers from peer %s\n", pfrom->addr.ToString().c_str());
            pfrom->PushGetHeaders(CBlockLocator(pindexBest), uint256(0), "spv-request");
        }
        else if (IbdHeadersControlPlaneEnabled())
        {
            CBlockLocator observerLocator;
            bool requestObserver = false;
            { LOCK(cs_main); requestObserver = PrepareIbdHeadersObserverRequest(pfrom, observerLocator); }
            if (requestObserver)
                pfrom->PushGetHeaders(observerLocator, uint256(0), "ibd-observe-initial");
        }
    }


    else if (strCommand == "sendheaders")
    {
        LOCK(cs_main);
        pfrom->fPreferHeaders = true;
        if (fDebug)
            printf("peer=%s enabled headers-first announcements\n", pfrom->addr.ToString().c_str());
    }


    else if (strCommand == "addr")
    {
        vector<CAddress> vAddr;
        vRecv >> vAddr;

        // Don't want addr from older versions unless seeding
        if (pfrom->nVersion < CADDR_TIME_VERSION && addrman.size() > 1000)
            return true;
        if (vAddr.size() > 1000)
        {
            pfrom->Misbehaving(20);
            return error("message addr size() = %" PRIszu"", vAddr.size());
        }

        // Store the new addresses
        vector<CAddress> vAddrOk;
        int64_t nNow = GetAdjustedTime();
        int64_t nSince = nNow - 10 * 60;
        for (CAddress& addr : vAddr)
        {
            if (fShutdown)
                return true;
            if (addr.nTime <= 100000000 || addr.nTime > nNow + 10 * 60)
                addr.nTime = nNow - 5 * 24 * 60 * 60;
            pfrom->AddAddressKnown(addr);
            bool fReachable = IsReachable(addr);
            if (addr.nTime > nSince && !pfrom->fGetAddr && vAddr.size() <= 10 && addr.IsRoutable())
            {
                // Relay to a limited number of other nodes
                {
                    LOCK(cs_vNodes);
                    // Use deterministic randomness to send to the same nodes for 24 hours
                    // at a time so the setAddrKnowns of the chosen nodes prevent repeats
                    static uint256 hashSalt;
                    if (hashSalt == 0)
                        hashSalt = GetRandHash();
                    uint64_t hashAddr = addr.GetHash();
                    uint256 hashRand = hashSalt ^ (hashAddr<<32) ^ ((GetTime()+hashAddr)/(24*60*60));
                    hashRand = Hash(BEGIN(hashRand), END(hashRand));
                    multimap<uint256, CNode*> mapMix;
                    for (CNode* pnode : vNodes)
                    {
                        if (pnode->nVersion < CADDR_TIME_VERSION)
                            continue;
                        unsigned int nPointer;
                        memcpy(&nPointer, &pnode, sizeof(nPointer));
                        uint256 hashKey = hashRand ^ nPointer;
                        hashKey = Hash(BEGIN(hashKey), END(hashKey));
                        mapMix.insert(make_pair(hashKey, pnode));
                    }
                    int nRelayNodes = fReachable ? 2 : 1; // limited relaying of addresses outside our network(s)
                    for (multimap<uint256, CNode*>::iterator mi = mapMix.begin(); mi != mapMix.end() && nRelayNodes-- > 0; ++mi)
                        ((*mi).second)->PushAddress(addr);
                }
            }
            // Do not store addresses outside our network
            if (fReachable)
                vAddrOk.push_back(addr);
        }
        addrman.Add(vAddrOk, pfrom->addr, 2 * 60 * 60);
        if (vAddr.size() < 1000)
            pfrom->fGetAddr = false;
        if (pfrom->fOneShot) {
            printf("DEBUG-DISCONNECT fOneShot peer=%s\n", pfrom->addr.ToString().c_str());
            pfrom->fDisconnect = true;
        }
    }

    else if (strCommand == "inv")
    {
        vector<CInv> vInv;
        vRecv >> vInv;
        if (vInv.size() > MAX_INV_SZ)
        {
            pfrom->Misbehaving(20);
            return error("message inv size() = %" PRIszu"", vInv.size());
        }

        if (!pfrom->fWhitelisted)
        {
            int64_t nNow = GetTime();
            if (nNow - pfrom->nInvWindowStart >= (int64_t)INV_RATE_LIMIT_WINDOW)
            {
                pfrom->nInvCount = 0;
                pfrom->nInvWindowStart = nNow;
            }
            pfrom->nInvCount += vInv.size();

            bool fSyncing = IsInitialBlockDownload() ||
                            (pindexBest != NULL && pindexBest->GetBlockTime() < GetTime() - 300);
            if (pfrom->nInvCount > INV_RATE_LIMIT_ITEMS && !fSyncing)
            {
                pfrom->Misbehaving(25);
                if (fDebug)
                    printf("inv rate limit exceeded: peer=%s count=%" PRIu64" in %" PRId64"s\n",
                           pfrom->addr.ToString().c_str(), pfrom->nInvCount,
                           nNow - pfrom->nInvWindowStart + INV_RATE_LIMIT_WINDOW);
            }
        }

        unsigned int nLastBlock = (unsigned int)(-1);
        int nBlockCount = 0;
        for (unsigned int nInv = 0; nInv < vInv.size(); nInv++) {
            if (vInv[nInv].type == MSG_BLOCK)
                nBlockCount++;
            if (vInv[vInv.size() - 1 - nInv].type == MSG_BLOCK && nLastBlock == (unsigned int)(-1)) {
                nLastBlock = vInv.size() - 1 - nInv;
            }
        }

        if (nBlockCount > 0)
        {
            pfrom->nBlocksReceivedInBatch = 0;
            pfrom->nExpectedBatchSize = nBlockCount;
            pfrom->fPrefetchSent = false;
            if (nLastBlock != (unsigned int)(-1))
                pfrom->hashLastBlockInBatch = vInv[nLastBlock].hash;
            if (fDebug)
                printf("Prefetch: New batch of %d blocks, last=%s\n", nBlockCount,
                       pfrom->hashLastBlockInBatch.ToString().substr(0,20).c_str());
        }

        LOCK(cs_main);
        CTxDB txdb("r");
        RecoveryResponseObservation recoveryObservation;
        const bool fObserveRecovery =
            pfrom->HasActiveRecoveryResponseWindow();
        if (fObserveRecovery)
            recoveryObservation.total_inv = vInv.size();
        const bool fGetBlocksResponse =
            pfrom->HasOutstandingGetBlocks();
        int64_t nGetBlocksResponseBlockInv = 0;
        int64_t nGetBlocksResponseUnknown = 0;
        if (fGetBlocksResponse)
        {
            ibdmetrics::Get().getblocks_response_inv_messages.fetch_add(
                1, std::memory_order_relaxed);
            ibdmetrics::RecordZeroLatency(ibdmetrics::ZERO_LATENCY_INV);
        }

        // Any inv message closes the active single-flight cycle.  The cycle is
        // consumed BEFORE the per-inv work so a continuation getblocks pushed
        // inside the loop below queues behind the freed slot instead of
        // joining a backlog behind an already-closed outstanding request.
        // The response counts (nGetBlocksResponseBlockInv/Unknown) below are
        // attributed to this message as "inv while a cycle was active" — the
        // wire protocol carries no request id, so the correlation is
        // heuristic, not exact.
        if (fGetBlocksResponse)
            pfrom->ConsumeGetBlocksResponse();

        // Consume the pending frontier getblocks expectation.  If this inv
        // message is a response to a getblocks that was requested with the
        // current active-tip locator, its first unknown block inv is the IBD
        // frontier candidate and is offered the single-slot admission
        // exemption (see FrontierCandidateCanAdmit).
        const bool fFrontierResponse = pfrom->fFrontierResponsePending;
        if (fFrontierResponse)
        {
            ibdmetrics::Get().frontier_response_consumed.fetch_add(
                1, std::memory_order_relaxed);
            ibdmetrics::FrontierResponsePendingAdd(-1);
        }
        pfrom->fFrontierResponsePending = false;
        bool fFrontierSlotOffered = false;

        // EXPTRACE HOOK: classify the full inv message before per-inv work.
        {
            std::vector<uint256> vBlockHashes;
            vBlockHashes.reserve(nBlockCount);
            for (unsigned int nInv = 0; nInv < vInv.size(); nInv++)
                if (vInv[nInv].type == MSG_BLOCK)
                    vBlockHashes.push_back(vInv[nInv].hash);
            ibdexptrace::NoteInv(
                pfrom->GetId(), nBlockCount, fGetBlocksResponse,
                fFrontierResponse, nBestHeight, pfrom->nBestKnownHeight,
                vBlockHashes, pfrom->hashLastBlockInBatch);
        }

        for (unsigned int nInv = 0; nInv < vInv.size(); nInv++)
        {
            const CInv &inv = vInv[nInv];

            if (fShutdown)
                return true;

            boost::this_thread::interruption_point();
            pfrom->AddInventoryKnown(inv);

            bool fAlreadyHave = AlreadyHave(txdb, inv);
            if (fGetBlocksResponse && inv.type == MSG_BLOCK)
                ++nGetBlocksResponseBlockInv;
            if (fObserveRecovery && inv.type == MSG_BLOCK)
            {
                ++recoveryObservation.block_inv;
                if (recoveryObservation.first_block_hash == uint256(0))
                    recoveryObservation.first_block_hash = inv.hash;
                std::map<uint256, CBlockIndex*>::iterator miRecovery =
                    mapBlockIndex.find(inv.hash);
                if (!fAlreadyHave)
                {
                    ++recoveryObservation.unknown_blocks;
                    if (recoveryObservation.first_unknown_block_hash == uint256(0))
                        recoveryObservation.first_unknown_block_hash = inv.hash;
                }
                else if (mapOrphanBlocks.count(inv.hash))
                    ++recoveryObservation.known_orphan_blocks;
                else if (miRecovery != mapBlockIndex.end() &&
                         miRecovery->second->IsInMainChain())
                    ++recoveryObservation.known_active_blocks;
                else if (miRecovery != mapBlockIndex.end())
                    ++recoveryObservation.known_nonactive_indexed_blocks;
                else
                    ++recoveryObservation.unknown_blocks;
            }
            if (inv.type == MSG_BLOCK)
            {
                std::map<uint256, CBlockIndex*>::iterator miKnown = mapBlockIndex.find(inv.hash);
                if (miKnown != mapBlockIndex.end())
                    pfrom->UpdateBestKnownBlock(miKnown->second->nHeight, inv.hash);
            }
            if (fDebugNet)
                printf("  got inventory: %s  %s\n", inv.ToString().c_str(), fAlreadyHave ? "have" : "new");
            if (inv.type == MSG_BLOCK && pindexBest != NULL && pindexBest->GetBlockTime() < GetTime() - 300 && fDebug)
                printf("sync inv: %s %s from %s solicited=%d\n", inv.ToString().c_str(), fAlreadyHave ? "HAVE" : "NEW", pfrom->addrName.c_str(), fGetBlocksResponse ? 1 : 0);

            if (!fAlreadyHave) {
                if (inv.type == MSG_BLOCK)
                {
                    if (fGetBlocksResponse)
                    {
                        ++nGetBlocksResponseUnknown;
                        ibdmetrics::RecordZeroLatency(
                            ibdmetrics::ZERO_LATENCY_UNKNOWN_INV);
                        if (ibdmetrics::Get().global_active_current.load(
                                std::memory_order_relaxed) == 0)
                            ibdmetrics::Get().inv_unknown_during_zero_global.fetch_add(
                                1, std::memory_order_relaxed);
                    }
                    const bool fFrontierCandidate =
                        fFrontierResponse && !fFrontierSlotOffered;
                    fFrontierSlotOffered =
                        fFrontierSlotOffered || fFrontierCandidate;
                    if (IbdHeaderSchedulerSelectActive())
                    {
                        RecordIbdHeaderSchedulerInvAvailability(pfrom, inv.hash);
                        const CIbdHeadersObserver::Classification c =
                            g_ibdHeadersObserver.Classify(inv.hash, -1);
                        if (c == CIbdHeadersObserver::IN_PREDICTED_WINDOW)
                            ++g_ibdHeaderSchedulerState.invInsideWindow;
                        else if (c == CIbdHeadersObserver::BEFORE_WINDOW)
                            ++g_ibdHeaderSchedulerState.invBeforeWindow;
                        else if (c == CIbdHeadersObserver::AFTER_WINDOW)
                            ++g_ibdHeaderSchedulerState.invAfterWindow;
                        else if (c == CIbdHeadersObserver::OFF_ACTIVE_BRANCH)
                            ++g_ibdHeaderSchedulerState.invOffBranch;
                        else
                            ++g_ibdHeaderSchedulerState.invUnknown;
                        ++g_ibdHeaderSchedulerState.invPrevented;
                        printf("IBD_HEADERS_SCHED event=inv peer=%d hash=%s class=%s active_admission=prevented frontier_candidate=%d\n",
                               pfrom->GetId(), inv.hash.ToString().c_str(),
                               CIbdHeadersObserver::ClassificationName(c),
                               fFrontierCandidate ? 1 : 0);
                        if (c == CIbdHeadersObserver::UNKNOWN_TO_GRAPH &&
                            !pfrom->getHeadersSync.IsInFlight())
                        {
                            CBlockLocator observerLocator;
                            if (PrepareIbdHeadersObserverRequest(pfrom, observerLocator))
                                pfrom->PushHeadersContinuation(observerLocator, uint256(0),
                                                      "ibd-select-inv");
                        }
                        RequestBlockPipelineWake(WAKE_CAUSE_OTHER);
                    }
                    else
                        TryAdmitBlockInvOrDefer(pfrom, inv, fFrontierCandidate);
                }
                else
                    pfrom->AskFor(inv, BLOCKREQ_SOURCE_INV);
            }
            else if (inv.type == MSG_BLOCK && mapOrphanBlocks.count(inv.hash)) {
                CBlock* pblockOrphan = mapOrphanBlocks[inv.hash];
                if (IsInitialBlockDownload())
                {
                    uint256 hashWanted = WantedByOrphan(pblockOrphan);
                    BlockRequestTraceUpdateBlockContextLocked(hashWanted, hashWanted, inv.hash);
                    pfrom->AskFor(
                        CInv(MSG_BLOCK, hashWanted),
                        BLOCKREQ_SOURCE_ORPHAN);
                }
                else if (!IbdHeaderSchedulerSelectActive())
                    pfrom->PushGetBlocks(
                        pindexBest, GetOrphanRoot(pblockOrphan),
                        ibdmetrics::GETBLOCKS_SOURCE_INV_CONTINUATION);
            } else if (nInv == nLastBlock) {
                // In case we are on a very long side-chain, it is possible that we already have
                // the last block in an inv bundle sent in response to getblocks. Try to detect
                // this situation. Do not paginate our main chain from a peer that cannot advance
                // it, but retain the historical side-chain continuation path.
                std::map<uint256, CBlockIndex*>::iterator miLast = mapBlockIndex.find(inv.hash);
                if (!IbdHeaderSchedulerSelectActive() &&
                    miLast != mapBlockIndex.end() &&
                    pfrom->ShouldContinueKnownBlockInventory(
                        nBestHeight, miLast->second->IsInMainChain()))
                {
                    pfrom->PushGetBlocks(
                        miLast->second, uint256(0),
                        ibdmetrics::GETBLOCKS_SOURCE_INV_CONTINUATION);
                    if (fDebugNet)
                        printf("force request: %s\n", inv.ToString().c_str());
                }
            }

            // Don't bother if send buffer is too full to respond anyway
            if (pfrom->nSendSize >= SendBufferSize()) {
                pfrom->Misbehaving(50);
                return error("send buffer size() = %" PRIszu"", pfrom->nSendSize);
            }

            // Track requests for our stuff
            g_signals.Inventory(inv.hash);
        }

        if (fGetBlocksResponse)
        {
            ibdmetrics::Get().getblocks_response_block_inv_count.fetch_add(
                nGetBlocksResponseBlockInv, std::memory_order_relaxed);
            ibdmetrics::Get().getblocks_response_unknown_count.fetch_add(
                nGetBlocksResponseUnknown, std::memory_order_relaxed);
            ibdactivepath::RecordGetBlocksResponse(
                nGetBlocksResponseBlockInv, nGetBlocksResponseUnknown);
            if (nGetBlocksResponseUnknown > 0)
                ibdactivepath::RecordUsefulResponseEnd();
            if (nGetBlocksResponseUnknown == 0)
            {
                ibdmetrics::Get().getblocks_response_zero_unknown.fetch_add(
                    1, std::memory_order_relaxed);
                ibdmetrics::Get().getblocks_response_inv_zero_unknown.fetch_add(
                    1, std::memory_order_relaxed);
            }
        }

        if (fObserveRecovery)
        {
            RecoveryResponseResult completed;
            if (pfrom->ObserveRecoveryResponseInv(
                    GetTimeMicros(), recoveryObservation, completed) &&
                BlockRequestTraceEnabled())
            {
                printf("%s\n", FormatRecoveryResponseSummary(
                    pfrom->GetId(), completed).c_str());
            }
        }

    }


    else if (strCommand == "getdata")
    {
        vector<CInv> vInv;
        vRecv >> vInv;
        if (vInv.size() > MAX_INV_SZ)
        {
            pfrom->Misbehaving(20);
            return error("message getdata size() = %" PRIszu"", vInv.size());
        }

        pfrom->vRecvGetData.insert(pfrom->vRecvGetData.end(), vInv.begin(), vInv.end());
        ProcessGetData(pfrom);
    }


    else if (strCommand == "getblocks")
    {
        CBlockLocator locator;
        uint256 hashStop;
        vRecv >> locator >> hashStop;

        LOCK(cs_main);

        CBlockIndex* pindexLocator = locator.GetBlockIndex();
        CBlockIndex* pindexStop = NULL;
        std::map<uint256, CBlockIndex*>::iterator miStop =
            mapBlockIndex.find(hashStop);
        if (miStop != mapBlockIndex.end() && miStop->second->IsInMainChain())
            pindexStop = miStop->second;

        CGetBlocksRequestInfo request;
        uint256 hashLocatorTail;
        locator.GetHashes(request.hashLocatorTip, hashLocatorTail);
        request.nResolvedHeight =
            pindexLocator ? pindexLocator->nHeight : -1;
        request.hashStop = hashStop;
        request.nStopHeight = pindexStop ? pindexStop->nHeight : -1;
        request.hashChainTip = hashBestChain;
        request.nRequestTimeMillis = GetTimeMillis();

        CBlockIndex* pindexFirst =
            pindexLocator ? pindexLocator->pnext : NULL;
        int nPredictedMainItems = pindexFirst && pindexBest
            ? std::min(1000, std::max(
                  0, pindexBest->nHeight - request.nResolvedHeight))
            : 0;
        bool fPredictedTipInventory = false;
        if (pindexStop &&
            pindexStop->nHeight > request.nResolvedHeight &&
            pindexStop->nHeight - request.nResolvedHeight <= 1000)
        {
            nPredictedMainItems = std::max(
                0, pindexStop->nHeight - request.nResolvedHeight - 1);
            fPredictedTipInventory =
                hashStop != hashBestChain && pindexBest &&
                pindexStop->GetBlockTime() + nStakeMinAge >
                    pindexBest->GetBlockTime();
        }

        CBlockIndex* pindexPredictedLast = pindexFirst;
        for (int i = 1; pindexPredictedLast &&
             i < nPredictedMainItems; ++i)
        {
            pindexPredictedLast = pindexPredictedLast->pnext;
        }
        request.nPredictedResponseCount =
            nPredictedMainItems + (fPredictedTipInventory ? 1 : 0);
        if (nPredictedMainItems > 0 && pindexFirst)
        {
            request.hashPredictedFirst = pindexFirst->GetBlockHash();
            request.hashPredictedLast =
                pindexPredictedLast->GetBlockHash();
        }
        if (fPredictedTipInventory)
        {
            if (nPredictedMainItems == 0)
                request.hashPredictedFirst = hashBestChain;
            request.hashPredictedLast = hashBestChain;
        }

        const bool fStrictInbound =
            pfrom->fInbound && !pfrom->fWhitelisted;
        const bool fDiagnostic =
            BlockRequestTraceEnabled() ||
            GetBoolArg("-getblocksdiag", false);
        CGetBlocksServerDecision decision =
            pfrom->getBlocksServer.Evaluate(request, fStrictInbound);
        const bool fRepeatedRange =
            !decision.fProgress &&
            (decision.fIdenticalRequest || decision.fSameResponse);
        const uint64_t nAbuseCount = std::max<uint64_t>(
            pfrom->getBlocksServer.nConsecutiveNonProgressingRequests,
            pfrom->getBlocksServer.nRequestsRateLimited);
        const bool fLogAbuse =
            fDiagnostic ||
            (BlockRequestTraceEnabled() && ShouldLogGetBlocksAbuse(nAbuseCount));

        if (fDiagnostic)
            LogGetBlocksServerEvent(
                "GETBLOCKS_REQUEST", pfrom, request, NULL, decision);
        if (fRepeatedRange && fLogAbuse)
            LogGetBlocksServerEvent(
                "GETBLOCKS_REPEAT", pfrom, request, NULL, decision);

        if (decision.fPenalize)
            pfrom->Misbehaving(decision.nPenalty);

        if (decision.action != GETBLOCKS_SERVER_ALLOW)
        {
            if (decision.action == GETBLOCKS_SERVER_RATE_LIMIT)
                ibdforensic::CountGetBlocksRateLimitInbound();
            if (decision.action == GETBLOCKS_SERVER_SUPPRESS &&
                fLogAbuse)
            {
                LogGetBlocksServerEvent(
                    "GETBLOCKS_SUPPRESS", pfrom, request, NULL,
                    decision);
            }
            else if (decision.action == GETBLOCKS_SERVER_RATE_LIMIT &&
                     fLogAbuse)
            {
                LogGetBlocksServerEvent(
                    "GETBLOCKS_RATE_LIMIT", pfrom, request, NULL,
                    decision);
            }
            else if (decision.action == GETBLOCKS_SERVER_DISCONNECT)
            {
                LogGetBlocksServerEvent(
                    "GETBLOCKS_DISCONNECT", pfrom, request, NULL,
                    decision);
                pfrom->fDisconnect = true;
            }
            return true;
        }

        // Zero-consumption served-inv suppression (only when
        // -getblocksservedinvzero is enabled): a strict-inbound peer that
        // repeats an overlapping range at an unchanged chain tip with zero
        // consumption (no getdata matching the served inv) stops being served
        // that inv.  Consumption via getdata is the binding signal; this never
        // affects getdata -> block serving, never penalizes, never disconnects.
        if (GetBlocksServedInvZeroEnabled())
        {
            // Reconnect-persistent zero-consumption debt: prime a NEW
            // connection's per-CNode state from the bounded, short-TTL, IP-keyed
            // debt (if any) so a reconnecting zero-consumption peer re-qualifies
            // faster.  Never starts suppressed (bootstrap safety: GRACE still
            // grants a serving opportunity before any suppression can fire).
            if (!pfrom->getBlocksServedInv.fGbHaveWindow)
            {
                bool fPrimed = false;
                GetBlocksServedInvReconnectPrime(
                    pfrom->getBlocksServedInv,
                    static_cast<const CNetAddr&>(pfrom->addr),
                    GetTimeMicros(), fPrimed);
            }
            GetBlocksServedInvDecision servedInvDecision =
                GetBlocksServedInvEvaluate(
                    pfrom->getBlocksServedInv, request, fStrictInbound,
                    GetTimeMicros(), request.nPredictedResponseCount);
            if (servedInvDecision.fSuppress)
            {
                CNode::CGetBlocksServedInvState& servedInv =
                    pfrom->getBlocksServedInv;
                servedInv.nGbZeroConsumeStreak++;
                servedInv.fGbSuppressInv = true;
                servedInv.fGbPriorZeroConsume = true;
                ibdmetrics::Get().getblocks_suppressed_inv_replies.fetch_add(
                    1, std::memory_order_relaxed);
                ibdmetrics::Get().getblocks_suppressed_inv_items.fetch_add(
                    servedInvDecision.nItemsAvoided, std::memory_order_relaxed);
                ibdmetrics::Get().getblocks_suppressed_inv_bytes_avoided.fetch_add(
                    servedInvDecision.nBytesAvoided,
                    std::memory_order_relaxed);
                if (BlockRequestTraceEnabled())
                {
                    printf("GETBLOCKS_SERVED_INV_SUPPRESS: peer=%s tip=%s streak=%u window_items=%llu window_matches=%llu window_age_ms=%lld items_avoided=%llu bytes_avoided=%llu\n",
                           pfrom->addr.ToString().c_str(),
                           request.hashChainTip.ToString().substr(0, 16).c_str(),
                           (unsigned int)servedInv.nGbZeroConsumeStreak,
                           (unsigned long long)servedInv.nGbServedInvItems,
                           (unsigned long long)servedInv.nGbGetDataMatches,
                           (long long)((GetTimeMicros() -
                                        servedInv.nGbServedInvWindowStartUs) / 1000),
                           (unsigned long long)servedInvDecision.nItemsAvoided,
                           (unsigned long long)servedInvDecision.nBytesAvoided);
                }
                return true;
            }
            if (servedInvDecision.fQualify)
                pfrom->getBlocksServedInv.nGbZeroConsumeStreak++;
        }

        CGetBlocksResponseInfo response;
        CBlockIndex* pindex = pindexFirst;
        int nLimit = 1000;
        if (fDebugNet)
            printf("getblocks %d to %s limit %d\n",
                   (pindex ? pindex->nHeight : -1),
                   hashStop.ToString().substr(0,20).c_str(), nLimit);
        for (; pindex; pindex = pindex->pnext)
        {
            const uint256 hashBlock = pindex->GetBlockHash();
            if (hashBlock == hashStop)
            {
                if (fDebugNet)
                    printf("  getblocks stopping at %d %s\n",
                           pindex->nHeight,
                           hashBlock.ToString().substr(0,20).c_str());
                // ppcoin: tell downloading node about the latest block if it's
                // without risk being rejected due to stake connection check
                if (hashStop != hashBestChain &&
                    pindex->GetBlockTime() + nStakeMinAge >
                        pindexBest->GetBlockTime() &&
                    pfrom->PushGetBlocksInventory(
                        CInv(MSG_BLOCK, hashBestChain)))
                {
                    response.Add(hashBestChain, pindexBest->nHeight);
                    GetBlocksServedInvRecordItem(pfrom->getBlocksServedInv,
                                                 GetTimeMicros());
                }
                break;
            }

            if (pfrom->PushGetBlocksInventory(
                    CInv(MSG_BLOCK, hashBlock)))
            {
                response.Add(hashBlock, pindex->nHeight);
                GetBlocksServedInvRecordItem(pfrom->getBlocksServedInv,
                                             GetTimeMicros());
            }
            if (--nLimit <= 0)
            {
                // When this block is requested, we'll send an inv that'll make them
                // getblocks the next batch of inventory.
                if (fDebugNet)
                    printf("  getblocks stopping at limit %d %s\n",
                           pindex->nHeight,
                           hashBlock.ToString().substr(0,20).c_str());
                pfrom->hashContinue = hashBlock;
                break;
            }
        }

        pfrom->getBlocksServer.RecordResponse(request, response);
        GetBlocksServedInvRecordResponse(
            pfrom->getBlocksServedInv, response, request.hashChainTip,
            GetTimeMicros());
        if (fDiagnostic || (fRepeatedRange && fLogAbuse))
        {
            LogGetBlocksServerEvent(
                "GETBLOCKS_RESPONSE", pfrom, request, &response,
                decision);
        }
    }
    else if (strCommand == "checkpoint")
    {
        CSyncCheckpoint checkpoint;
        vRecv >> checkpoint;

        if (checkpoint.ProcessSyncCheckpoint(pfrom))
        {
            // Relay
            pfrom->hashCheckpointKnown = checkpoint.hashCheckpoint;
            LOCK(cs_vNodes);
            for (CNode* pnode : vNodes)
                checkpoint.RelayTo(pnode);
        }
    }

    else if (strCommand == "getheaders")
    {
        CBlockLocator locator;
        uint256 hashStop;
        vRecv >> locator >> hashStop;

        uint256 hashLocatorFirst = 0;
        uint256 hashLocatorLast = 0;
        bool fHasLocatorHashes = locator.GetHashes(hashLocatorFirst, hashLocatorLast);
        size_t nLocatorSize = locator.Size();

        uint64_t nGetHeadersDiagCount = 0;
        bool fLogGetHeaders = false;
        {
            LOCK(cs_getHeadersDiag);
            uint64_t& nCount = mapGetHeadersSendDiagCount[pfrom->GetId()];
            nCount++;
            nGetHeadersDiagCount = nCount;
            fLogGetHeaders = (nCount <= 5 || (nCount % 25) == 0);
        }

        LOCK(cs_main);

        // Serving-side dedup (only when -headersservededup is enabled):
        // fingerprint the FULL (locator, hashStop) request PLUS the active
        // chain tip hash, so that identical requests under an unchanged tip
        // collapse while any tip change (advance or reorg) forces a fresh
        // serve.  Computed under cs_main: pindexBest is only valid here.
        const bool fHeadersDedupActive = HeadersServedDedupEnabled();
        uint256 headersDedupFp;
        if (fHeadersDedupActive)
            headersDedupFp = HeaderServedDedupFingerprint(
                locator, hashStop,
                pindexBest ? pindexBest->GetBlockHash() : uint256(0));

        CBlockIndex* pindexFork = NULL;
        CBlockIndex* pindex = NULL;
        if (locator.IsNull())
        {
            // If locator is null, return the hashStop block
            std::map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(hashStop);
            if (mi == mapBlockIndex.end())
                return true;
            pindexFork = (*mi).second;
            pindex = pindexFork;
        }
        else
        {
            // Find the last block the caller has in the main chain
            pindexFork = locator.GetBlockIndex();
            if (pindexFork)
                pindex = pindexFork->pnext;
        }

        if (BlockRequestTraceEnabled() && fLogGetHeaders)
        {
            printf("GETHEADERS_RECV: peer=%s diag=%llu locator_size=%zu locator_first=%s locator_last=%s hashStop=%s fork_height=%d start_height=%d local_height=%d best_header=%d peer_bestknownheight=%d peer_startingheight=%d\n",
                   pfrom->addr.ToString().c_str(),
                   (unsigned long long)nGetHeadersDiagCount,
                   nLocatorSize,
                   fHasLocatorHashes ? hashLocatorFirst.ToString().c_str() : "none",
                   fHasLocatorHashes ? hashLocatorLast.ToString().c_str() : "none",
                   hashStop.ToString().c_str(),
                   pindexFork ? pindexFork->nHeight : -1,
                   pindex ? pindex->nHeight : -1,
                   nBestHeight,
                   pindexBest ? pindexBest->nHeight : -1,
                   pfrom->nBestKnownHeight,
                   pfrom->nChainHeight);
        }

        // Suppress a re-ask of the identical request within the TTL.  Nothing
        // is written to the socket and parsing/peer state stays intact.
        if (fHeadersDedupActive)
        {
            uint32_t nPrevRepeat = 0;
            uint32_t nPrevCount = 0;
            uint64_t nPrevBytes = 0;
            int64_t nPrevUs = 0;
            bool fSuppress = false;
            {
                LOCK(pfrom->cs_mapHeadersServedDedup);
                std::map<uint256, CNode::CHeadersServedDedupEntry>::const_iterator itDedup =
                    pfrom->mapHeadersServedDedup.find(headersDedupFp);
                if (itDedup != pfrom->mapHeadersServedDedup.end() &&
                    HeaderServedDedupEntryFresh(itDedup->second, GetTimeMicros()))
                {
                    fSuppress = true;
                    nPrevRepeat = itDedup->second.nRepeat;
                    nPrevCount = itDedup->second.nHeadersCount;
                    nPrevBytes = itDedup->second.nBytes;
                    nPrevUs = itDedup->second.nServedUs;
                }
            }
            if (fSuppress)
            {
                if (BlockRequestTraceEnabled() && fLogGetHeaders)
                {
                    printf("GETHEADERS_DEDUP: peer=%s fp=%s repeat=%u age=%lld suppressed_count=%u suppressed_bytes=%llu\n",
                           pfrom->addr.ToString().c_str(),
                           headersDedupFp.ToString().substr(0, 16).c_str(),
                           (unsigned int)(nPrevRepeat + 1),
                           (long long)((GetTimeMicros() - nPrevUs) / 1000),
                           nPrevCount,
                           (unsigned long long)nPrevBytes);
                }
                return true;
            }
        }

        std::vector<CBlock> vHeaders;
        int nLimit = 2000;
        CBlockIndex* pindexFirstSent = NULL;
        CBlockIndex* pindexLastSent = NULL;

        for (; pindex; pindex = pindex->pnext)
        {
            if (!pindexFirstSent)
                pindexFirstSent = pindex;
            pindexLastSent = pindex;
            vHeaders.push_back(pindex->GetBlockHeader());
            if (--nLimit <= 0 || pindex->GetBlockHash() == hashStop)
                break;
        }

        unsigned int nHeadersBytes = ::GetSerializeSize(vHeaders, SER_NETWORK, PROTOCOL_VERSION);
        if (fHeadersDedupActive)
        {
            LOCK(pfrom->cs_mapHeadersServedDedup);
            HeaderServedDedupUpsert(pfrom->mapHeadersServedDedup, headersDedupFp,
                                    GetTimeMicros(), (uint32_t)vHeaders.size(),
                                    nHeadersBytes);
        }
        if (BlockRequestTraceEnabled() && fLogGetHeaders)
        {
            printf("GETHEADERS_SEND: peer=%s diag=%llu headers=%zu bytes=%u first_hash=%s first_height=%d last_hash=%s last_height=%d fork_height=%d hashStop=%s\n",
                   pfrom->addr.ToString().c_str(),
                   (unsigned long long)nGetHeadersDiagCount,
                   vHeaders.size(),
                   nHeadersBytes,
                   pindexFirstSent ? pindexFirstSent->GetBlockHash().ToString().c_str() : "none",
                   pindexFirstSent ? pindexFirstSent->nHeight : -1,
                   pindexLastSent ? pindexLastSent->GetBlockHash().ToString().c_str() : "none",
                   pindexLastSent ? pindexLastSent->nHeight : -1,
                   pindexFork ? pindexFork->nHeight : -1,
                   hashStop.ToString().c_str());
        }

        // A direct getheaders response is solicited. The wire protocol does not
        // expose whether the requester is in IBD, so bounded responder priority
        // safely applies to every direct response (including SPV requesters).
        pfrom->fNextHeadersResponsePriority = true;
        pfrom->PushMessage("headers", vHeaders);
    }

    else if (strCommand == "headers")
    {
        std::vector<CBlock> vHeaders;
        vRecv >> vHeaders;

        if (vHeaders.size() > 2000)
        {
            pfrom->Misbehaving(20);
            return error("headers message size > 2000");
        }

        unsigned int nHeadersBytes = ::GetSerializeSize(vHeaders, SER_NETWORK, PROTOCOL_VERSION);
        uint256 hashFirstHeader = vHeaders.empty() ? uint256(0) : vHeaders.front().GetHash();
        uint256 hashLastHeader = vHeaders.empty() ? uint256(0) : vHeaders.back().GetHash();

        uint64_t nHeadersDiagCount = 0;
        {
            LOCK(cs_getHeadersDiag);
            uint64_t& nCount = mapHeadersRecvDiagCount[pfrom->GetId()];
            nCount++;
            nHeadersDiagCount = nCount;
        }

        const int64_t nHeadersCsMainWaitBegin = GetTimeMicros();
        LOCK(cs_main);
        const int64_t nHeadersCsMainAcquired = GetTimeMicros();
        if (IbdHeadersControlPlaneEnabled())
            printf("IBD_HEADER_DISPATCH event=cs_main_acquired peer=%lld wait_begin_us=%lld acquired_us=%lld wait_us=%lld\n",
               (long long)pfrom->GetId(),
               (long long)nHeadersCsMainWaitBegin,
               (long long)nHeadersCsMainAcquired,
               (long long)(nHeadersCsMainAcquired - nHeadersCsMainWaitBegin));

        RecordGetHeadersResponse(pfrom, vHeaders.size(), nHeadersBytes);

        CIbdHeadersObserver::HeaderResult observerResult;
        if (IbdHeadersControlPlaneEnabled() && !fSPVMode && IsInitialBlockDownload() && pindexBest)
        {
            g_ibdHeadersObserver.SetEnabled(true);
            g_ibdHeadersObserver.UpdateAnchor(pindexBest->GetBlockHash(), pindexBest->nHeight);
            std::vector<std::pair<uint256, uint256> > observedHeaders;
            observedHeaders.reserve(vHeaders.size());
            for (std::vector<CBlock>::const_iterator it = vHeaders.begin();
                 it != vHeaders.end(); ++it)
                observedHeaders.push_back(std::make_pair(it->GetHash(), it->hashPrevBlock));
            observerResult = g_ibdHeadersObserver.ObserveHeaders(
                pfrom->GetId(), observedHeaders);
            const CIbdHeaderNode* graphTip = g_ibdHeadersObserver.Graph().ActiveTip();
            const std::vector<uint256> predicted = g_ibdHeadersObserver.PredictedWindow();
            printf("IBD_HEADERS_OBSERVE event=headers peer=%lld expected=%d count=%zu accepted=%llu duplicates=%llu disconnected=%llu quarantined=%llu window_size=%zu first=%s last=%s graph_tip_height=%d graph_ahead=%d continue=%d\n",
                   (long long)pfrom->GetId(), observerResult.expectedResponse ? 1 : 0,
                   vHeaders.size(),
                   (unsigned long long)g_ibdHeadersObserver.Stats().accepted,
                   (unsigned long long)g_ibdHeadersObserver.Stats().duplicates,
                   (unsigned long long)g_ibdHeadersObserver.Stats().disconnected,
                   (unsigned long long)g_ibdHeadersObserver.Stats().quarantined,
                   predicted.size(),
                   predicted.empty() ? "none" : predicted.front().ToString().c_str(),
                   predicted.empty() ? "none" : predicted.back().ToString().c_str(),
                   graphTip ? graphTip->height : -1,
                   graphTip ? graphTip->height - g_ibdHeadersObserver.Graph().AnchorHeight() : -1,
                   observerResult.continueHeaders ? 1 : 0);
            printf("IBD_HEADER_DISPATCH event=graph_insert_complete peer=%lld complete_us=%lld count=%zu graph_tip_height=%d mark_active_calls=%llu mark_active_touched=%llu mark_active_touched_total=%llu\n",
                   (long long)pfrom->GetId(), (long long)GetTimeMicros(),
                   vHeaders.size(), graphTip ? graphTip->height : -1,
                   (unsigned long long)g_ibdHeadersObserver.Graph().MarkActivePathCalls(),
                   (unsigned long long)g_ibdHeadersObserver.Graph().LastMarkActivePathTouched(),
                   (unsigned long long)g_ibdHeadersObserver.Graph().MarkActivePathTouchedTotal());
            if (observerResult.expectedResponse)
            {
                if (observerResult.continueHeaders &&
                    !observerResult.continuationLocator.empty())
                {
                    g_ibdHeadersObserver.MarkHeaderRequest(pfrom->GetId());
                    pfrom->PushHeadersContinuation(
                        CBlockLocator(observerResult.continuationLocator),
                        uint256(0), "ibd-observe-continue");
                }
                return true;
            }
        }

        const int nFullTipBefore = pindexBest ? pindexBest->nHeight : -1;
        const int nBestHeaderBefore = fSPVMode && pindexBest ? pindexBest->nHeight : -1;
        int nNewHeaders = 0;
        int nAlreadyKnownHeaders = 0;
        int nConnectedHeaders = 0;
        int nOrphanHeaders = 0;
        int nFirstHeightKnown = -1;
        int nLastHeightKnown = -1;
        bool fContinuousRange = true;

        if (!vHeaders.empty())
        {
            std::map<uint256, CBlockIndex*>::const_iterator miFirst = mapBlockIndex.find(hashFirstHeader);
            if (miFirst != mapBlockIndex.end())
                nFirstHeightKnown = miFirst->second->nHeight;
            else
            {
                std::map<uint256, CBlockIndex*>::const_iterator miPrev = mapBlockIndex.find(vHeaders.front().hashPrevBlock);
                if (miPrev != mapBlockIndex.end())
                    nFirstHeightKnown = miPrev->second->nHeight + 1;
            }

            uint256 hashPrevious = hashFirstHeader;
            for (size_t i = 0; i < vHeaders.size(); ++i)
            {
                const CBlock& header = vHeaders[i];
                const uint256 hash = (i == 0) ? hashFirstHeader : header.GetHash();
                if (i > 0 && header.hashPrevBlock != hashPrevious)
                    fContinuousRange = false;
                hashPrevious = hash;

                if (mapBlockIndex.count(hash))
                    nAlreadyKnownHeaders++;
                else
                {
                    nNewHeaders++;
                }
            }

            std::map<uint256, CBlockIndex*>::const_iterator miLast = mapBlockIndex.find(hashLastHeader);
            if (miLast != mapBlockIndex.end())
                nLastHeightKnown = miLast->second->nHeight;
            else if (fContinuousRange && nFirstHeightKnown >= 0)
                nLastHeightKnown = nFirstHeightKnown + (int)vHeaders.size() - 1;
        }

        if (vHeaders.empty())
        {
            if (BlockRequestTraceEnabled())
            {
                printf("HEADERS_TRACE: peer=%s peer_id=%lld response_sequence=%llu headers_count=0 headers_bytes=%u first_hash=none first_height_known=-1 last_hash=none last_height_known=-1 new_headers_count=0 already_known_count=0 connected_count=0 orphan_count=0 best_header_before=%d best_header_after=%d full_tip_before=%d full_tip_after=%d range_contiguous=1 next_getheaders_reason=empty-response\n",
                       pfrom->addrName.c_str(),
                       (long long)pfrom->GetId(),
                       (unsigned long long)nHeadersDiagCount,
                       nHeadersBytes,
                       nBestHeaderBefore,
                       nBestHeaderBefore,
                       nFullTipBefore,
                       nFullTipBefore);
            }
            return true;
        }

        CBlockIndex* pindexLast = NULL;
        std::vector<CInv> vGetData;
        for (const CBlock& header : vHeaders)
        {
            uint256 hash = header.GetHash();

            if (mapBlockIndex.count(hash))
            {
                pindexLast = mapBlockIndex[hash];
                pfrom->UpdateBestKnownBlock(pindexLast->nHeight, hash);
                continue;
            }

            if (mapBlockIndex.count(header.hashPrevBlock) == 0)
            {
                if (fDebug) printf("Header %s has unknown parent %s, waiting for in-flight blocks\n",
                       hash.ToString().substr(0,20).c_str(),
                       header.hashPrevBlock.ToString().substr(0,20).c_str());
                if (!fSPVMode && !IbdHeaderSchedulerSelectActive())
                    pfrom->PushGetBlocks(
                        pindexBest, uint256(0),
                        ibdmetrics::GETBLOCKS_SOURCE_HEADERS);
                break;
            }

            CBlockIndex* pindexPrev = mapBlockIndex[header.hashPrevBlock];
            pfrom->UpdateBestKnownBlock(pindexPrev->nHeight + 1, hash);

            // PoS blocks have nNonce==0 in legacy headers, but post-DAG all
            // headers must be valid PoW headers.
            if (pindexPrev->nHeight + 1 >= FORK_HEIGHT_DAG || header.nNonce != 0)
            {
                if (!CheckProofOfWork(hash, header.nBits))
                {
                    pfrom->Misbehaving(100);
                    return error("header %s has invalid proof of work", hash.ToString().c_str());
                }
            }

            if (header.GetBlockTime() > FutureDrift(GetAdjustedTime()))
            {
                pfrom->Misbehaving(10);
                return error("header %s timestamp too far in future", hash.ToString().c_str());
            }

            nConnectedHeaders++;

            if (fSPVMode)
            {
                CBlockIndex* pindexNew = new CBlockIndex();
                pindexNew->phashBlock = &(mapBlockIndex.insert(make_pair(hash, pindexNew)).first->first);
                pindexNew->pprev = pindexPrev;
                pindexNew->nHeight = pindexPrev->nHeight + 1;
                pindexNew->nVersion = header.nVersion;
                pindexNew->hashMerkleRoot = header.hashMerkleRoot;
                pindexNew->nTime = header.nTime;
                pindexNew->nBits = header.nBits;
                pindexNew->nNonce = header.nNonce;
                pindexNew->nFile = 0;
                pindexNew->nBlockPos = 0;
                pindexNew->nChainTrust = pindexPrev->nChainTrust + pindexNew->GetBlockTrust();

                if (pindexNew->nChainTrust > nBestChainTrust)
                {
                    pindexPrev->pnext = pindexNew;
                    pindexBest = pindexNew;
                    hashBestChain = hash;
                    nBestHeight = pindexNew->nHeight;
                    nBestChainTrust = pindexNew->nChainTrust;
                    nTimeBestReceived = GetTime();
                }

                pindexLast = pindexNew;
                if (fDebugNet && pindexNew->nHeight % 1000 == 0)
                    printf("SPV: Processed header at height %d\n", pindexNew->nHeight);
            }
            else
            {
                // Request full block data, skip if already in-flight
                if (pfrom->IsBlockInFlight(hash))
                {
                    if (fDebug)
                        printf("Header block %s already in-flight, skipping\n",
                               hash.ToString().substr(0,20).c_str());
                    pindexLast = pindexPrev;
                    continue;
                }
                if (fDebug)
                    printf("Header announced block %s (parent height %d), requesting full block\n",
                           hash.ToString().substr(0,20).c_str(), pindexPrev->nHeight);
                vGetData.push_back(CInv(MSG_BLOCK, hash));
                pindexLast = pindexPrev;
            }
        }

        nOrphanHeaders = std::max(0, nNewHeaders - nConnectedHeaders);
        const bool fContinueHeaders =
            fSPVMode && nConnectedHeaders > 0 && pindexLast && vHeaders.size() >= 2000;
        const int nFullTipAfter = pindexBest ? pindexBest->nHeight : -1;
        const int nBestHeaderAfter = fSPVMode && pindexBest ? pindexBest->nHeight : -1;

        if (BlockRequestTraceEnabled())
        {
            printf("HEADERS_TRACE: peer=%s peer_id=%lld response_sequence=%llu headers_count=%zu headers_bytes=%u first_hash=%s first_height_known=%d last_hash=%s last_height_known=%d new_headers_count=%d already_known_count=%d connected_count=%d orphan_count=%d best_header_before=%d best_header_after=%d full_tip_before=%d full_tip_after=%d range_contiguous=%d next_getheaders_reason=%s\n",
                   pfrom->addrName.c_str(),
                   (long long)pfrom->GetId(),
                   (unsigned long long)nHeadersDiagCount,
                   vHeaders.size(),
                   nHeadersBytes,
                   hashFirstHeader.ToString().c_str(),
                   nFirstHeightKnown,
                   hashLastHeader.ToString().c_str(),
                   nLastHeightKnown,
                   nNewHeaders,
                   nAlreadyKnownHeaders,
                   nConnectedHeaders,
                   nOrphanHeaders,
                   nBestHeaderBefore,
                   nBestHeaderAfter,
                   nFullTipBefore,
                   nFullTipAfter,
                   fContinuousRange ? 1 : 0,
                   fContinueHeaders ? "full-batch" : "none");
        }

        // In full node mode, request full blocks for announced headers
        if (!fSPVMode && !IbdHeaderSchedulerSelectActive() && !vGetData.empty())
        {
            vector<CInv> vOwnedGetData;
            for (const CInv& inv : vGetData)
            {
                NodeId nOwnerPeer = -1;
                BlockRequestOwnerState ownerState = BLOCK_REQUEST_OWNER_QUEUED;
                bool fHasOwner = GetBlockRequestOwner(
                    inv.hash, &nOwnerPeer, &ownerState);
                if ((fHasOwner &&
                     (nOwnerPeer != pfrom->GetId() ||
                      ownerState == BLOCK_REQUEST_OWNER_IN_FLIGHT)) ||
                    (!fHasOwner &&
                     !TryAssignBlockRequestOwner(
                         inv.hash, pfrom->GetId(),
                         BLOCKREQ_SOURCE_HEADERS_DIRECT,
                         &nOwnerPeer, &ownerState)))
                {
                    if (BlockRequestTraceEnabled())
                        BlockRequestTraceGetDataSkip(
                            pfrom, inv.hash, nOwnerPeer,
                            BlockRequestOwnerStateName(ownerState));
                    continue;
                }
                vOwnedGetData.push_back(inv);
                pfrom->MarkBlockInFlight(inv.hash);
                if (BlockRequestTraceEnabled())
                    BlockRequestTraceInFlightMark(pfrom, inv.hash, false);
            }
            vGetData.swap(vOwnedGetData);
            if (IbdHeadersControlPlaneEnabled())
                for (const CInv& inv : vGetData)
                    TraceIbdHeadersObserverEvent("request", pfrom, inv.hash, -1);
            if (fDebug)
                printf("Requesting %u full blocks from peer %s via getdata\n",
                       (unsigned int)vGetData.size(),
                       pfrom->addr.ToString().c_str());
            if (BlockRequestTraceEnabled())
            {
                for (const CInv& inv : vGetData)
                {
                    std::map<uint256, CBlockIndex*>::const_iterator miTrace =
                        mapBlockIndex.find(inv.hash);
                    uint256 hashParent = uint256(0);
                    if (miTrace != mapBlockIndex.end() && miTrace->second->pprev)
                        hashParent = miTrace->second->pprev->GetBlockHash();
                    BlockRequestTraceUpdateBlockContextLocked(
                        inv.hash, hashParent, uint256(0));
                    BlockRequestTraceGetDataSend(
                        pfrom, inv.hash, BLOCKREQ_SOURCE_HEADERS_DIRECT,
                        miTrace != mapBlockIndex.end() ? 1 : 0,
                        true, miTrace != mapBlockIndex.end(), false, false,
                        -1, -1);
                }
            }
            if (!vGetData.empty())
            {
                ibdforensic::RecordGetDataBatch(
                    pfrom->GetId(), BlockHashesOfGetData(vGetData),
                    GetTimeMicros(), pfrom->nSendSize,
                    pfrom->hashLastBlockInBatch, pfrom->nExpectedBatchSize);
                pfrom->nLastGetDataTime = GetTime();
                pfrom->PushBlockGetData(vGetData);
            }
        }

        // SPV continuation is allowed only after this response advanced the
        // indexed header tip, so the next locator is necessarily different.
        if (fContinueHeaders)
            pfrom->PushHeadersContinuation(CBlockLocator(pindexLast), uint256(0), "headers-continue");

        if (fSPVMode && pindexLast && pindexLast == pindexBest)
        {
            printf("SPV: Headers synced to height %d, ready to request transactions\n", nBestHeight);
        }
    }

    else if (strCommand == "tx")
    {
        vector<uint256> vWorkQueue;
        vector<uint256> vEraseQueue;
        CTxDB txdb("r");
        CTransaction tx;
        vRecv >> tx;

        CInv inv(MSG_TX, tx.GetHash());
        pfrom->AddInventoryKnown(inv);

        bool fMissingInputs = false;
        if (tx.AcceptToMemoryPool(txdb, &fMissingInputs))
        {
            SyncWithWallets(tx, NULL, true);
            RelayTransaction(tx, inv.hash);
            {
                LOCK(cs_mapAlreadyAskedFor);
                mapAlreadyAskedFor.erase(inv);
            }
            vWorkQueue.push_back(inv.hash);
            vEraseQueue.push_back(inv.hash);

            // Recursively process any orphan transactions that depended on this one
            for (unsigned int i = 0; i < vWorkQueue.size(); i++)
            {
                uint256 hashPrev = vWorkQueue[i];
                for (set<uint256>::iterator mi = mapOrphanTransactionsByPrev[hashPrev].begin();
                     mi != mapOrphanTransactionsByPrev[hashPrev].end();
                     ++mi)
                {
                    const uint256& orphanTxHash = *mi;
                    CTransaction& orphanTx = mapOrphanTransactions[orphanTxHash];
                    bool fMissingInputs2 = false;

                    if (orphanTx.AcceptToMemoryPool(txdb, &fMissingInputs2))
                    {
                        printf("   accepted orphan tx %s\n", orphanTxHash.ToString().substr(0,10).c_str());
                        SyncWithWallets(orphanTx, NULL, true);
                        RelayTransaction(orphanTx, orphanTxHash);
                        {
                            LOCK(cs_mapAlreadyAskedFor);
                            mapAlreadyAskedFor.erase(CInv(MSG_TX, orphanTxHash));
                        }
                        vWorkQueue.push_back(orphanTxHash);
                        vEraseQueue.push_back(orphanTxHash);
                    }
                    else if (!fMissingInputs2)
                    {
                        // invalid orphan
                        vEraseQueue.push_back(orphanTxHash);
                        printf("   removed invalid orphan tx %s\n", orphanTxHash.ToString().substr(0,10).c_str());
                    }
                }
            }

            for (uint256 hash : vEraseQueue)
                EraseOrphanTx(hash);
        }
        else if (fMissingInputs)
        {
            AddOrphanTx(tx);

            // DoS prevention: do not allow mapOrphanTransactions to grow unbounded
            //unsigned int nEvicted = LimitOrphanTxSize(MAX_ORPHAN_TRANSACTIONS);
            unsigned int nMaxOrphanTx = (unsigned int)std::max((int64_t)0, GetArg("-maxorphantx", DEFAULT_MAX_ORPHAN_TRANSACTIONS));
            unsigned int nEvicted = LimitOrphanTxSize(nMaxOrphanTx);

            if (nEvicted > 0)
                printf("mapOrphan overflow, removed %u tx\n", nEvicted);
        }
        if (tx.nDoS) pfrom->Misbehaving(tx.nDoS);
    }


    else if (strCommand == "block")
    {
        const uint64_t nBlockPayloadBytes = vRecv.size();
        CBlock block;
        uint256 hashBlock;
        {
            CSyncLockPhase phase("ProcessMessage(block)", "deserialize");
            vRecv >> block;
            hashBlock = block.GetHash();
        }

        if (fDebugNet) printf("received block %s\n", hashBlock.ToString().substr(0,20).c_str());
        // block.print();

        CInv inv(MSG_BLOCK, hashBlock);
        pfrom->AddInventoryKnown(inv);

        const bool fTraceBlockRequest = BlockRequestTraceEnabled();
        bool fSenderInFlightBefore =
            pfrom->setBlocksInFlight.count(hashBlock) != 0;
        int64_t nSenderInFlightAge = -1;
        if (fTraceBlockRequest)
        {
            std::map<uint256, int64_t>::const_iterator miInFlight =
                pfrom->mapBlockInFlightSince.find(hashBlock);
            if (miInFlight != pfrom->mapBlockInFlightSince.end())
                nSenderInFlightAge =
                    std::max<int64_t>(0, GetTime() - miInFlight->second);
        }
        pfrom->ClearBlockInFlight(hashBlock);
        ibdforensic::RecordReceived(
            pfrom->GetId(), hashBlock, GetTimeMicros(),
            nTimeReceived);
        // ClearBlockInFlight already releases this peer's owner slot.
        // The remaining owner slots (if any) are cleared only after the block
        // passes validation, so an invalid first copy does not kill a healthy
        // backup (Stage 5 multi-owner transport).
        ibdblocklatency::RecordBlockReceived(
            hashBlock, pfrom->GetId(), (int64_t)nBlockPayloadBytes,
            nTimeReceived, pfrom->nPingUsecTime);

        CSyncLockDiagnostics blockLockDiagnostics(
            "ProcessMessage(block)", "cs_main");
        int64_t nIBDCsMainWaitStart =
            ibdactivepath::IBDActivePathTraceEnabled()
                ? ibdactivepath::MonotonicMicros() : 0;
        LOCK(cs_main);
        if (nIBDCsMainWaitStart)
            ibdactivepath::RecordCSMainWait(
                ibdactivepath::MonotonicMicros() - nIBDCsMainWaitStart);
        blockLockDiagnostics.Acquired();
        bool fKnownBefore = mapBlockIndex.count(hashBlock) != 0;
        if (IbdHeadersControlPlaneEnabled())
        {
            int observedHeight = -1;
            std::map<uint256, CBlockIndex*>::const_iterator observedIndex =
                mapBlockIndex.find(hashBlock);
            if (observedIndex != mapBlockIndex.end())
                observedHeight = observedIndex->second->nHeight;
            TraceIbdHeadersObserverEvent("receive", pfrom, hashBlock, observedHeight);
        }
        bool fOrphanBefore = mapOrphanBlocks.count(hashBlock) != 0;
        bool fMissingPrevBefore = mapBlockIndex.count(block.hashPrevBlock) == 0;
        bool fWouldHitIbdOrphanLimit = false;
        if (pfrom && fMissingPrevBefore && IsInitialBlockDownload())
        {
            int nOrphansFromPeerBefore = 0;
            fWouldHitIbdOrphanLimit = PeerOrphanStorageLimitExceeded(
                pfrom->GetId(), &nOrphansFromPeerBefore);
        }

        if (fTraceBlockRequest)
        {
            BlockRequestTraceUpdateBlockContextLocked(
                hashBlock, block.hashPrevBlock, uint256(0));
            BlockRequestTraceBlockReceive(
                pfrom, hashBlock, fKnownBefore,
                fSenderInFlightBefore, nSenderInFlightAge);
            BlockRequestTraceInFlightClear(
                pfrom, hashBlock, "receive",
                nSenderInFlightAge, fKnownBefore);
            if (fMissingPrevBefore)
            {
                const int64_t nLastAcceptedAge =
                    nTimeBestReceived > 0
                        ? std::max<int64_t>(0, GetTime() - nTimeBestReceived)
                        : -1;
                NodeId nPrevOwnerPeer = -1;
                BlockRequestOwnerState prevOwnerState =
                    BLOCK_REQUEST_OWNER_QUEUED;
                const bool fPrevOwnerKnown =
                    GetBlockRequestOwner(block.hashPrevBlock,
                                         &nPrevOwnerPeer, &prevOwnerState);
                BlockRequestTraceContinuityBreak(
                    pfrom, hashBlock, block.hashPrevBlock,
                    nBestHeight, hashBestChain,
                    nLastAcceptedAge,
                    pfrom->nBestKnownHeight,
                    pfrom->hashBestKnownBlock,
                    mapOrphanBlocks.count(block.hashPrevBlock) != 0,
                    pfrom->setBlocksInFlight.count(block.hashPrevBlock) != 0,
                    pfrom->IsBlockAskForQueued(block.hashPrevBlock),
                    fPrevOwnerKnown && nPrevOwnerPeer == pfrom->GetId(),
                    nPrevOwnerPeer,
                    BlockRequestOwnerStateName(prevOwnerState),
                    GetPeerOrphanCount(pfrom->GetId()),
                    mapOrphanBlocks.size(),
                    TipAncestorOfPeerBestKnown(pfrom->hashBestKnownBlock));
            }
        }
        ibdmetrics::Get().block_receive_total.fetch_add(
            1, std::memory_order_relaxed);

        // A.9a.3h: a staking fetch may target an active cold source whose
        // historical parent/index is intentionally non-resident. Validate and
        // store those bytes as MATERIALIZATION only; this never supplies chain
        // authority. Normal connectable blocks publish their accepted position
        // below instead of being written twice.
        bool fStoredStakingMaterialization = false;
        unsigned int nStakingFile = 0;
        unsigned int nStakingBlockPos = 0;
        const bool fPendingStakingMaterialization =
            fHybridSPV && pwalletMain &&
            pwalletMain->IsStakingMaterializationPending(hashBlock);
        if (fPendingStakingMaterialization)
        {
            std::map<uint256, CBlockIndex*>::const_iterator known =
                mapBlockIndex.find(hashBlock);
            if (known != mapBlockIndex.end() && known->second && known->second->nFile > 0)
            {
                nStakingFile = known->second->nFile;
                nStakingBlockPos = known->second->nBlockPos;
                fStoredStakingMaterialization = true;
            }
            else if (mapBlockIndex.count(block.hashPrevBlock) == 0 &&
                     block.CheckBlock() &&
                     block.WriteToDisk(nStakingFile, nStakingBlockPos))
            {
                fStoredStakingMaterialization = true;
            }
        }

        bool fAccepted = fStoredStakingMaterialization
            ? true
            : ProcessBlock(pfrom, &block);
        if (fStoredStakingMaterialization)
            pwalletMain->PublishStakingMaterialization(
                hashBlock, nStakingFile, nStakingBlockPos);
        if (fAccepted && fPendingStakingMaterialization)
        {
            std::map<uint256, CBlockIndex*>::const_iterator accepted =
                mapBlockIndex.find(hashBlock);
            if (accepted != mapBlockIndex.end() && accepted->second &&
                accepted->second->nFile > 0)
                pwalletMain->PublishStakingMaterialization(
                    hashBlock, accepted->second->nFile,
                    accepted->second->nBlockPos);
        }
        bool fEffRetryRecorded = false;
        if (fAccepted)
        {
            ClearRejectedBlockForSync(hashBlock);
            RetryOrphanLimitRejectedOnParentConnect(hashBlock, pfrom);
            pfrom->nLastBlockRecv = GetTime();
            std::map<uint256, CBlockIndex*>::iterator miAccepted =
                mapBlockIndex.find(hashBlock);
            if (miAccepted != mapBlockIndex.end())
            {
                pfrom->UpdateBestKnownBlock(
                    miAccepted->second->nHeight, hashBlock);
                if (miAccepted->second->IsInMainChain())
                {
                    ibdmetrics::Get().block_result_accepted_active.fetch_add(
                        1, std::memory_order_relaxed);
                    // EXPTRACE HOOK: block connected to the active chain.
                    ibdexptrace::NoteBlockConnected(hashBlock);
                }
                else if (mapOrphanBlocks.count(hashBlock) != 0)
                    ibdmetrics::Get().block_result_orphan_new.fetch_add(
                        1, std::memory_order_relaxed);
            }
            if (pfrom->nBestKnownHeight > pfrom->nChainHeight)
                pfrom->nChainHeight = pfrom->nBestKnownHeight;
        }
        else
        {
            // EXPTRACE HOOK: block received but not accepted (rejected,
            // duplicate, orphan-limit).  Non-main-chain acceptance is the
            // caller's choice; only hard non-acceptance is counted here.
            ibdexptrace::NoteBlockNotConnected(hashBlock);
        }
        // A delivered block response is no longer an outstanding global
        // anti-duplicate request, even when validation classified it as
        // already-known or orphaned.
        if (fAccepted || fKnownBefore || fOrphanBefore)
        {
            // Logical request satisfied: clear any remaining backup owner slots
            // without arming recovery.  This also lets EraseAlreadyAskedFor
            // remove the global anti-duplicate entry.
            SatisfyBlockLogicalRequest(hashBlock);
            EraseAlreadyAskedForIfUnowned(inv);
        }
        else if (!fKnownBefore && !fOrphanBefore && block.nDoS == 0 &&
                 mapBlockIndex.count(hashBlock) == 0 &&
                 mapOrphanBlocks.count(hashBlock) == 0)
        {
            const bool fOrphanLimitRetrySuppressed =
                fWouldHitIbdOrphanLimit && IsInitialBlockDownload();
            RecordRejectedBlockForSync(
                hashBlock, !fOrphanLimitRetrySuppressed);
            fEffRetryRecorded = !fOrphanLimitRetrySuppressed;
            const int64_t nRejectedUntil = GetTimeMicros() +
                (fOrphanLimitRetrySuppressed
                    ? ORPHAN_LIMIT_REJECT_RETRY_COOLDOWN_US
                    : ALREADY_ASKED_FOR_NEGATIVE_COOLDOWN_US);
            if (fOrphanLimitRetrySuppressed)
                RecordOrphanLimitRejectedBlock(
                    pfrom->GetId(), inv, nRejectedUntil, block.hashPrevBlock);
            // The 120s orphan-limit protection is peer-local only (see
            // RecordOrphanLimitRejectedBlock).  The peer-agnostic map carries
            // only the ordinary short negative cooldown so a different peer is
            // never delayed by the saturating peer's cooldown.
            RecordRejectedBlockGlobalNegativeCooldown(inv);
        }

        if (block.nDoS)
            pfrom->Misbehaving(block.nDoS);

        BlockRequestTraceResult traceResult = BLOCKREQ_RESULT_UNKNOWN;
        if (fTraceBlockRequest)
        {
            std::map<uint256, CBlockIndex*>::const_iterator miAfter =
                mapBlockIndex.find(hashBlock);
            bool fIndexedAfter = miAfter != mapBlockIndex.end();
            bool fOrphanAfter = mapOrphanBlocks.count(hashBlock) != 0;
            bool fActiveChainAfter =
                fIndexedAfter && miAfter->second->IsInMainChain();
            bool fBestChainAfter =
                fIndexedAfter && miAfter->second == pindexBest;
            int nHeightAfter =
                fIndexedAfter ? miAfter->second->nHeight : -1;

            if (fKnownBefore)
                traceResult = BLOCKREQ_RESULT_ALREADY_KNOWN;
            else if (fOrphanBefore)
                traceResult = BLOCKREQ_RESULT_ORPHAN_DUPLICATE;
            else if (fAccepted && fIndexedAfter)
                traceResult = fActiveChainAfter
                    ? BLOCKREQ_RESULT_ACCEPTED_ACTIVE
                    : BLOCKREQ_RESULT_ACCEPTED_INDEXED;
            else if (fAccepted && fOrphanAfter)
                traceResult = BLOCKREQ_RESULT_ORPHAN_NEW;
            else if (fAccepted)
                traceResult = BLOCKREQ_RESULT_TRUE_UNINDEXED;
            else if (fWouldHitIbdOrphanLimit)
                traceResult = BLOCKREQ_RESULT_ORPHAN_LIMIT_IBD;
            else if (block.nDoS == 0 && !fMissingPrevBefore)
                traceResult = BLOCKREQ_RESULT_ACCEPT_FAILED;
            else
                traceResult = BLOCKREQ_RESULT_REJECTED;
            BlockRequestTraceBlockResult(
                pfrom, hashBlock, traceResult, fAccepted,
                fIndexedAfter, fActiveChainAfter,
                fBestChainAfter, nHeightAfter);
            BlockRequestTraceMissingParentResolved(
                pfrom, hashBlock, fAccepted, nHeightAfter);
        }

        if (IBDEfficiencyTraceEnabled())
        {
            std::map<uint256, CBlockIndex*>::const_iterator miEff =
                mapBlockIndex.find(hashBlock);
            bool fIndexedAfter = miEff != mapBlockIndex.end();
            bool fOrphanAfter = mapOrphanBlocks.count(hashBlock) != 0;
            bool fActiveChainAfter =
                fIndexedAfter && miEff->second->IsInMainChain();

            bool fEffAcceptedActive = fAccepted && fActiveChainAfter;
            bool fEffAcceptedSide = fAccepted && fIndexedAfter && !fActiveChainAfter;
            bool fEffOrphanNew = fAccepted && fOrphanAfter;
            bool fEffRejected = !fAccepted;

            bool fEffUnique = !fKnownBefore && !fOrphanBefore;
            bool fEffDuplicateIndexed = fKnownBefore;
            bool fEffDuplicateOrphan = fOrphanBefore;

            IBDEfficiencyRecordBlock(nBlockPayloadBytes,
                                    fSenderInFlightBefore,
                                    fEffUnique,
                                    fEffDuplicateIndexed,
                                    fEffDuplicateOrphan,
                                    fEffAcceptedActive,
                                    fEffAcceptedSide,
                                    fEffOrphanNew,
                                    fEffRejected,
                                    fEffRetryRecorded);
            IBDEfficiencyMaybeSummary(GetTimeMicros());
        }

        // Chain sync continues via headers batch completion and stall recovery.

        if (IsInitialBlockDownload() && pfrom->nExpectedBatchSize > 0)
        {
            pfrom->nBlocksReceivedInBatch++;

            int nPrefetchThreshold = (pfrom->nExpectedBatchSize * 3) / 4;

            if (!IbdHeaderSchedulerSelectActive() &&
                !pfrom->fPrefetchSent && pfrom->nBlocksReceivedInBatch >= nPrefetchThreshold)
            {
                if (pfrom->hashLastBlockInBatch != 0 && mapBlockIndex.count(pfrom->hashLastBlockInBatch))
                {
                    CBlockIndex* pindexLast = mapBlockIndex[pfrom->hashLastBlockInBatch];
                    bool fPrefetchSentBefore = pfrom->fPrefetchSent;
                    const bool fQueued = pfrom->PushGetBlocks(
                        pindexLast, uint256(0), ibdmetrics::GETBLOCKS_SOURCE_PREFETCH);
                    pfrom->fPrefetchSent = true;
                    if (BlockRequestTraceEnabled() && fQueued)
                    {
                        BlockRequestTraceGetBlocksTrigger(
                            pfrom, "batch75", hashBlock,
                            pfrom->nBlocksReceivedInBatch,
                            pfrom->nExpectedBatchSize,
                            fPrefetchSentBefore,
                            pfrom->hashLastBlockInBatch,
                            traceResult,
                            pindexLast->GetBlockHash(),
                            pindexLast->nHeight,
                            uint256(0));
                    }
                    if (fDebug)
                        printf("Prefetch: Requesting next batch at %d/%d blocks (from height %d)\n",
                               pfrom->nBlocksReceivedInBatch, pfrom->nExpectedBatchSize, pindexLast->nHeight);
                }
                else if (pindexBest)
                {
                    bool fPrefetchSentBefore = pfrom->fPrefetchSent;
                    const bool fQueued = pfrom->PushGetBlocks(
                        pindexBest, uint256(0), ibdmetrics::GETBLOCKS_SOURCE_PREFETCH);
                    pfrom->fPrefetchSent = true;
                    if (BlockRequestTraceEnabled() && fQueued)
                    {
                        BlockRequestTraceGetBlocksTrigger(
                            pfrom, "batch75", hashBlock,
                            pfrom->nBlocksReceivedInBatch,
                            pfrom->nExpectedBatchSize,
                            fPrefetchSentBefore,
                            pfrom->hashLastBlockInBatch,
                            traceResult,
                            pindexBest->GetBlockHash(),
                            pindexBest->nHeight,
                            uint256(0));
                    }
                    if (fDebug)
                        printf("Prefetch: Requesting next batch at %d/%d blocks (fallback from best height %d)\n",
                               pfrom->nBlocksReceivedInBatch, pfrom->nExpectedBatchSize, pindexBest->nHeight);
                }
            }
        }

        if (pindexBest && pfrom->fSuccessfullyConnected && IsInitialBlockDownload())
        {
            const int64_t nNow = GetTime();
            const bool fPeerAhead = (pfrom->nBestKnownHeight > nBestHeight);
            const bool fPipelineDrained = (pfrom->setBlocksInFlight.size() <= 1 && pfrom->getBlocksIndex.empty());
            const bool fCooldownExpired = (pfrom->nLastGetBlocksTime == 0 || (nNow - pfrom->nLastGetBlocksTime) >= 10);
            ibdmetrics::Get().pipeline_drained_checks.fetch_add(
                1, std::memory_order_relaxed);
            if (!fPeerAhead)
                ibdmetrics::Get().pipeline_drained_skip_not_ahead.fetch_add(
                    1, std::memory_order_relaxed);
            else if (!fCooldownExpired)
                ibdmetrics::Get().pipeline_drained_skip_getblocks_10s_cooldown.fetch_add(
                    1, std::memory_order_relaxed);
            else if (!fPipelineDrained)
                ibdmetrics::Get().pipeline_drained_skip_other_condition.fetch_add(
                    1, std::memory_order_relaxed);
            if (!IbdHeaderSchedulerSelectActive() &&
                fPeerAhead && fPipelineDrained && fCooldownExpired)
            {
                if (fDebugNet)
                {
                    printf("GETBLOCKS_CONTINUE: peer=%s reason=empty-inflight local_height=%d peer_height=%d blocks_in_flight=%zu queued_getblocks=%zu last_getblocks_age=%lld hashContinue=%s\n",
                           pfrom->addr.ToString().c_str(),
                           nBestHeight,
                           pfrom->nBestKnownHeight,
                           pfrom->setBlocksInFlight.size(),
                           pfrom->getBlocksIndex.size(),
                           (long long)(pfrom->nLastGetBlocksTime == 0 ? -1 : (nNow - pfrom->nLastGetBlocksTime)),
                           pfrom->hashContinue.ToString().c_str());
                }
                bool fPrefetchSentBefore = pfrom->fPrefetchSent;
                const bool fQueued = pfrom->PushGetBlocks(
                    pindexBest, uint256(0),
                    ibdmetrics::GETBLOCKS_SOURCE_CONTINUATION);
                if (fQueued)
                    ibdmetrics::Get().pipeline_drained_getblocks_queued.fetch_add(
                        1, std::memory_order_relaxed);
                if (BlockRequestTraceEnabled() && fQueued)
                {
                    BlockRequestTraceGetBlocksTrigger(
                        pfrom, "pipeline-drained", hashBlock,
                        pfrom->nBlocksReceivedInBatch,
                        pfrom->nExpectedBatchSize,
                        fPrefetchSentBefore,
                        pfrom->hashLastBlockInBatch,
                        traceResult,
                        pindexBest->GetBlockHash(),
                        pindexBest->nHeight,
                        uint256(0));
                }
            }
        }
    }


    else if (strCommand == "getaddr")
    {
        // Don't return addresses older than nCutOff timestamp
        int64_t nCutOff = GetTime() - (nNodeLifespan * 24 * 60 * 60);
        pfrom->vAddrToSend.clear();
        vector<CAddress> vAddr = addrman.GetAddr();
        for (const CAddress &addr : vAddr)
            if(addr.nTime > nCutOff)
                pfrom->PushAddress(addr);
    }


    else if (strCommand == "mempool")
    {
        std::vector<uint256> vtxid;
        mempool.queryHashes(vtxid);
        vector<CInv> vInv;
        for (unsigned int i = 0; i < vtxid.size(); i++) {
            CInv inv(MSG_TX, vtxid[i]);
            vInv.push_back(inv);
            if (i == (MAX_INV_SZ - 1))
                    break;
        }
        if (vInv.size() > 0)
            pfrom->PushMessage("inv", vInv);
    }


    else if (strCommand == "checkorder")
    {
        uint256 hashReply;
        vRecv >> hashReply;

        if (!GetBoolArg("-allowreceivebyip"))
        {
            pfrom->PushMessage("reply", hashReply, (int)2, string(""));
            return true;
        }

        CWalletTx order;
        vRecv >> order;

        /// we have a chance to check the order here

        // Keep giving the same key to the same ip until they use it
        if (!mapReuseKey.count(pfrom->addr))
            pwalletMain->GetKeyFromPool(mapReuseKey[pfrom->addr], true);

        // Send back approval of order and pubkey to use
        CScript scriptPubKey;
        scriptPubKey << mapReuseKey[pfrom->addr] << OP_CHECKSIG;
        pfrom->PushMessage("reply", hashReply, (int)0, scriptPubKey);
    }


    else if (strCommand == "reply")
    {
        uint256 hashReply;
        vRecv >> hashReply;

        CRequestTracker tracker;
        {
            LOCK(pfrom->cs_mapRequests);
            map<uint256, CRequestTracker>::iterator mi = pfrom->mapRequests.find(hashReply);
            if (mi != pfrom->mapRequests.end())
            {
                tracker = (*mi).second;
                pfrom->mapRequests.erase(mi);
            }
        }
        if (!tracker.IsNull())
            tracker.fn(tracker.param1, vRecv);
    }


    else if (strCommand == "ping")
    {
        if (pfrom->nVersion > BIP0031_VERSION)
        {
            uint64_t nonce = 0;
            vRecv >> nonce;
            // Echo the message back with the nonce. This allows for two useful features:
            //
            // 1) A remote node can quickly check if the connection is operational
            // 2) Remote nodes can measure the latency of the network thread. If this node
            //    is overloaded it won't respond to pings quickly and the remote node can
            //    avoid sending us more work, like chain download requests.
            //
            // The nonce stops the remote getting confused between different pings: without
            // it, if the remote node sends a ping once per second and this node takes 5
            // seconds to respond to each, the 5th ping the remote sends would appear to
            // return very quickly.
            pfrom->PushMessage("pong", nonce);
        }
    }


    else if (strCommand == "pong")
    {
        int64_t pingUsecEnd = nTimeReceived;
        uint64_t nonce = 0;
        size_t nAvail = vRecv.in_avail();
        bool bPingFinished = false;
        std::string sProblem;
        int nPongResult = PONG_RESULT_UNSOLICITED;

        if (nAvail >= sizeof(nonce)) {
            vRecv >> nonce;

            // Only process pong message if there is an outstanding ping (old ping without nonce should never pong)
            if (pfrom->nPingNonceSent != 0) {
                if (nonce == pfrom->nPingNonceSent) {
                    // Matching pong received, this ping is no longer outstanding
                    bPingFinished = true;
                    nPongResult = PONG_RESULT_MATCHED;
                    int64_t pingUsecTime = pingUsecEnd - pfrom->nPingUsecStart;
                    if (pingUsecTime > 0) {
                        // Successful ping time measurement, replace previous
                        pfrom->nPingUsecTime = pingUsecTime;
                        if (fDebug) { printf("Ping time for peer %s: %.1f msec\n", pfrom->addr.ToString().c_str(), ((double)pfrom->nPingUsecTime) / 1000.0); }
                    } else {
                        // This should never happen
                        sProblem = "Timing mishap";
                    }
                } else {
                    // Nonce mismatches are normal when pings are overlapping
                    sProblem = "Nonce mismatch";
                    nPongResult = PONG_RESULT_WRONG_NONCE;
                    if (nonce == 0) {
                        // This is most likely a bug in another implementation somewhere, cancel this ping
                        bPingFinished = true;
                        sProblem = "Nonce zero";
                        nPongResult = PONG_RESULT_NONCE_ZERO;
                    }
                }
            } else {
                sProblem = "Unsolicited pong without ping";
            }
        } else {
            // This is most likely a bug in another implementation somewhere, cancel this ping
            bPingFinished = true;
            sProblem = "Short payload";
            nPongResult = PONG_RESULT_SHORT_PAYLOAD;
        }

        if (!(sProblem.empty())) {
            printf("pong %s %s: %s, %" PRIx64" expected, %" PRIx64" received, %zu bytes\n"
                , pfrom->addr.ToString().c_str()
                , pfrom->strSubVer.c_str()
                , sProblem.c_str()
                , pfrom->nPingNonceSent
                , nonce
                , nAvail);
        }
        if (PingLifecycleTraceEnabled())
            PingLifecycleTracePongResult(pfrom, nonce, nPongResult);
        if (bPingFinished) {
            pfrom->nPingNonceSent = 0;
        }
    }


    else if (strCommand == "alert")
    {
        CAlert alert;
        vRecv >> alert;

        uint256 alertHash = alert.GetHash();
        if (pfrom->setKnown.count(alertHash) == 0)
        {
            if (alert.ProcessAlert())
            {
                // Relay
                pfrom->setKnown.insert(alertHash);
                {
                    LOCK(cs_vNodes);
                    for (CNode* pnode : vNodes)
                        alert.RelayTo(pnode);
                }
            }
            else {
                // Small DoS penalty so peers that send us lots of
                // duplicate/expired/invalid-signature/whatever alerts
                // eventually get banned.
                // This isn't a Misbehaving(100) (immediate ban) because the
                // peer might be an older or different implementation with
                // a different signature key, etc.
                pfrom->Misbehaving(10);
            }
        }
    }


    else if (strCommand == "filterload")
    {
        if (vRecv.size() > MAX_BLOOM_FILTER_SIZE + 100)  // +100 for serialization overhead
        {
            pfrom->Misbehaving(100);
            return false;
        }
        CBloomFilter filter;
        vRecv >> filter;

        if (!filter.IsWithinSizeConstraints())
        {
            pfrom->Misbehaving(100);
        }
        else
        {
            LOCK(pfrom->cs_filter);
            delete pfrom->pfilter;
            pfrom->pfilter = new CBloomFilter(filter);
            pfrom->pfilter->UpdateEmptyFull();
        }
        pfrom->fRelayTxes = true;
    }


    else if (strCommand == "filteradd")
    {
        std::vector<unsigned char> vData;
        vRecv >> vData;

        if (vData.size() > MAX_SCRIPT_ELEMENT_SIZE)
        {
            pfrom->Misbehaving(100);
        }
        else
        {
            LOCK(pfrom->cs_filter);
            if (pfrom->pfilter)
            {
                pfrom->pfilter->insert(vData);
            }
            else
            {
                pfrom->Misbehaving(100);
            }
        }
    }


    else if (strCommand == "filterclear")
    {
        LOCK(pfrom->cs_filter);
        delete pfrom->pfilter;
        pfrom->pfilter = new CBloomFilter();
        pfrom->fRelayTxes = true;
    }


    else if (strCommand == "merkleblock")
    {
        CMerkleBlock merkleBlock;
        vRecv >> merkleBlock;
        std::vector<uint256> vMatch;
        if (merkleBlock.txn.ExtractMatches(vMatch) != merkleBlock.header.hashMerkleRoot)
        {
            pfrom->Misbehaving(100);
            return error("merkleblock: Invalid merkle root");
        }

        if (fDebug)
            printf("SPV: Received merkleblock with %u matched transactions\n", (unsigned int)vMatch.size());

        if (fHybridSPV && pwalletMain)
        {
            uint256 hashBlock = Tribus(BEGIN(merkleBlock.header.nVersion), END(merkleBlock.header.nNonce));

            // Global order: cs_main -> cs_wallet -> cs_spvutxos.  Keep current
            // authority stable through publication; UpdateSPVUtxo rechecks under
            // the same cs_main transaction and never acquires cs_main itself.
            LOCK2(cs_main, pwalletMain->cs_wallet);
            int nHeight = 0;
            ColdHotSeamSnapshot source;
            std::string sourceError;
            const ColdHotSeamResult authority =
                GetStakingSourceAuthority(hashBlock, &source, &sourceError);
            if (authority != COLD_HOT_SEAM_OK)
            {
                if (fDebug)
                    printf("SPV: Ignoring merkleblock without current authority: %s\n", hashBlock.ToString().c_str());
            }
            else
            {
                nHeight = source.snapshot.height;
                CPartialMerkleTree txnCopy = merkleBlock.txn;
                std::vector<uint256> vMatchCopy;
                txnCopy.ExtractMatches(vMatchCopy);

                for (const uint256& txhash : vMatch)
                {
                    int nTxIndex = -1;
                    for (int i = 0; i < (int)vMatchCopy.size(); i++)
                    {
                        if (vMatchCopy[i] == txhash)
                        {
                            nTxIndex = i;
                            break;
                        }
                    }

                    std::map<uint256, CWalletTx>::iterator wit = pwalletMain->mapWallet.find(txhash);
                    if (wit != pwalletMain->mapWallet.end())
                    {
                        const CWalletTx& wtx = wit->second;
                        for (unsigned int n = 0; n < wtx.vout.size(); n++)
                        {
                            if (pwalletMain->IsMine(wtx.vout[n]))
                            {
                                COutPoint outpoint(txhash, n);
                                SPVUtxo utxo(txhash, n, wtx.vout[n].nValue,
                                             nHeight, hashBlock, wtx.nTime,
                                             wtx.vout[n].scriptPubKey);
                                utxo.hashMerkleRoot = merkleBlock.header.hashMerkleRoot;
                                utxo.nTxIndex = nTxIndex;
                                utxo.fHaveBlock = true;
                                utxo.fVerified = (nTxIndex >= 0 && nHeight > 0);
                                pwalletMain->UpdateSPVUtxo(outpoint, utxo);
                            }
                        }
                    }
                }
            }
        }
    }


    else
    {
        //ProcessMessageCollateralN(pfrom, strCommand, vRecv);
        ProcessMessageCollateralnode(pfrom, strCommand, vRecv);
        ProcessMessageNullSend(pfrom, strCommand, vRecv);
        ProcessMessageFinality(pfrom, strCommand, vRecv);
        //ProcessSpork(pfrom, strCommand, vRecv);

        // Ignore unknown commands for extensibility
    }


    // Update the last seen time for this node's address
    if (pfrom->fNetworkNode)
        if (strCommand == "version" || strCommand == "addr" || strCommand == "inv" || strCommand == "getdata" || strCommand == "ping")
            AddressCurrentlyConnected(pfrom->addr);


    return true;
}

void (*g_processMessagesPostExtractHook)() = NULL;

static const unsigned int IBD_REQUESTED_BLOCK_BURST_LIMIT = 32;

static bool IsRequestedBlockMessage(CNode* pfrom, const CNetMessage& message)
{
    if (pfrom == NULL || !message.complete() || !message.hdr.IsValid() ||
        message.hdr.GetCommand() != "block")
        return false;

    try
    {
        CDataStream blockStream(message.vRecv.begin(),
                                message.vRecv.begin() + message.hdr.nMessageSize,
                                SER_NETWORK, pfrom->nRecvVersion);
        CBlock block;
        blockStream >> block;
        NodeId ownerPeer = -1;
        BlockRequestOwnerState ownerState = BLOCK_REQUEST_OWNER_QUEUED;
        if (!GetBlockRequestOwnerDetails(block.GetHash(), &ownerPeer,
                                         &ownerState, NULL) ||
            ownerPeer != pfrom->GetId())
            return false;
        return ownerState == BLOCK_REQUEST_OWNER_IN_FLIGHT ||
               pfrom->setBlocksInFlight.count(block.GetHash()) != 0;
    }
    catch (const std::exception&)
    {
        return false;
    }
}

static size_t FindRequestedBlockMessage(CNode* pfrom, size_t nStart,
                                        size_t nMaxMessages)
{
    if (pfrom == NULL || pfrom->vRecvMsg.empty())
        return std::numeric_limits<size_t>::max();

    const size_t nQueueSize = pfrom->vRecvMsg.size();
    if (nStart >= nQueueSize)
        nStart = 0;
    const size_t nCount = std::min(nMaxMessages, nQueueSize - nStart);
    for (size_t j = 0; j < nCount; ++j)
    {
        const size_t i = nStart + j;
        const CNetMessage& candidate = pfrom->vRecvMsg[i];
        if (!candidate.complete())
            break;
        if (IsRequestedBlockMessage(pfrom, candidate))
            return i;
    }
    return std::numeric_limits<size_t>::max();
}

// requires LOCK(cs_vRecvMsg)
bool ProcessMessages(CNode* pfrom, CCriticalBlock& recvLock,
                     unsigned int nBlockBurstDepth)
{
    //if (fDebug)
    //    printf("ProcessMessages(%zu messages)\n", pfrom->vRecvMsg.size());

    //
    // Message format
    //  (4) message start
    //  (12) command
    //  (4) size
    //  (4) checksum
    //  (x) data
    //
    bool fOk = true;

    size_t nPriorityIndex = std::numeric_limits<size_t>::max();
    size_t nPriorityScannedBytes = 0;
    size_t nPriorityScannedMessages = 0;
    size_t nRequestedBlockIndex = std::numeric_limits<size_t>::max();
    uint8_t nPriorityClass = 0;
    if (IbdHeadersControlPlaneEnabled() && !fSPVMode &&
        (nBlockBurstDepth > 0 || !pfrom->fIbdHeaderPriorityNeedsFifo) &&
        !pfrom->vRecvMsg.empty())
    {
        const size_t nMaxMessages = 256;
        const size_t nMaxBytes = 16 * 1024 * 1024;
        const size_t nQueueSize = pfrom->vRecvMsg.size();
        const size_t nStart = pfrom->nIbdPriorityScanOffset < nQueueSize ? pfrom->nIbdPriorityScanOffset : 0;
        size_t nHeaderIndex = std::numeric_limits<size_t>::max();
        size_t nBlockIndex = std::numeric_limits<size_t>::max();
        const size_t nScanCount = std::min(nMaxMessages, nQueueSize - nStart);
        for (size_t j = 0; j < nScanCount; ++j)
        {
            const size_t i = nStart + j;
            CNetMessage& candidate = pfrom->vRecvMsg[i];
            if (!candidate.complete()) break;
            if (candidate.hdr.nMessageSize > nMaxBytes - nPriorityScannedBytes) break;
            ++nPriorityScannedMessages;
            nPriorityScannedBytes += candidate.hdr.nMessageSize;
            if (!candidate.hdr.IsValid()) continue;
            uint256 candidateHash = Hash(candidate.vRecv.begin(), candidate.vRecv.begin() + candidate.hdr.nMessageSize);
            unsigned int candidateChecksum = 0;
            memcpy(&candidateChecksum, &candidateHash, sizeof(candidateChecksum));
            if (candidateChecksum != candidate.hdr.nChecksum) continue;
            const std::string command = candidate.hdr.GetCommand();
            if (command == "headers" && nHeaderIndex == std::numeric_limits<size_t>::max()) nHeaderIndex = i;
            else if (command == "block")
            {
                if (nBlockIndex == std::numeric_limits<size_t>::max()) nBlockIndex = i;
                if (IsRequestedBlockMessage(pfrom, candidate) && nRequestedBlockIndex == std::numeric_limits<size_t>::max()) nRequestedBlockIndex = i;
            }
        }
        const size_t nPreferredBlock = nRequestedBlockIndex;
        if (nBlockBurstDepth > 0 && nPreferredBlock != std::numeric_limits<size_t>::max())
        { nPriorityIndex = nPreferredBlock; nPriorityClass = 1; }
        else if (nPreferredBlock != std::numeric_limits<size_t>::max() && (nHeaderIndex == std::numeric_limits<size_t>::max() || pfrom->nIbdPriorityLastClass != 1))
        { nPriorityIndex = nPreferredBlock; nPriorityClass = 1; }
        else if (nHeaderIndex != std::numeric_limits<size_t>::max())
        { nPriorityIndex = nHeaderIndex; nPriorityClass = 2; }
        else if (nBlockIndex != std::numeric_limits<size_t>::max())
        { nPriorityIndex = nBlockIndex; nPriorityClass = 1; }
        if (nPriorityScannedMessages != 0) pfrom->nIbdPriorityScanOffset = (nStart + nPriorityScannedMessages) % nQueueSize;
    }
    const bool fPrioritySelected =
        nPriorityIndex != std::numeric_limits<size_t>::max();
    const bool fSelectedRequestedBlock =
        fPrioritySelected && nPriorityClass == 1 &&
        nPriorityIndex == nRequestedBlockIndex;
    if (fPrioritySelected) {
        pfrom->fIbdHeaderPriorityNeedsFifo = true;
        pfrom->nIbdPriorityLastClass = nPriorityClass;
    }

    if (!fPrioritySelected && !pfrom->vRecvGetData.empty())
        ProcessGetData(pfrom);

    // this maintains the order of responses
    if (!fPrioritySelected && !pfrom->vRecvGetData.empty()) return fOk;

    std::unique_ptr<CNetMessage> pExtracted;
    int64_t nCompleteWaiting = 0;
    std::deque<CNetMessage>::iterator it = pfrom->vRecvMsg.begin();
    if (fPrioritySelected)
        it += nPriorityIndex;
    while (!pfrom->fDisconnect && it != pfrom->vRecvMsg.end()) {
        // Don't bother if send buffer is too full to respond anyway
        if (pfrom->nSendSize >= SendBufferSize())
            break;

        // get next message
        CNetMessage& queuedMsg = *it;

        //if (fDebug)
        //    printf("ProcessMessages(message %u msgsz, %zu bytes, complete:%s)\n",
        //            queuedMsg.hdr.nMessageSize, queuedMsg.vRecv.size(),
        //            queuedMsg.complete() ? "Y" : "N");

        // end, if an incomplete message is found
        if (!queuedMsg.complete())
            break;

        if (!fPrioritySelected && pfrom->fIbdHeaderPriorityNeedsFifo)
            pfrom->fIbdHeaderPriorityNeedsFifo = false;
        // Move and erase exactly one complete frame while the receive-queue
        // lock is held. No queue reference survives recvLock.Unlock().
        for (std::deque<CNetMessage>::const_iterator itWait = it + 1;
             itWait != pfrom->vRecvMsg.end(); ++itWait)
            if (itWait->complete())
                ++nCompleteWaiting;
        pExtracted.reset(new CNetMessage(std::move(queuedMsg)));
        pfrom->vRecvMsg.erase(it);
        if (nPriorityIndex != std::numeric_limits<size_t>::max() && pfrom->nIbdPriorityScanOffset > nPriorityIndex)
            --pfrom->nIbdPriorityScanOffset;
        if (pfrom->nIbdPriorityScanOffset >= pfrom->vRecvMsg.size())
            pfrom->nIbdPriorityScanOffset = 0;
        recvLock.Unlock();
        if (g_processMessagesPostExtractHook)
            g_processMessagesPostExtractHook();
        CNetMessage& msg = *pExtracted;

        // Scan for message start
        if (memcmp(msg.hdr.pchMessageStart, pchMessageStart, sizeof(pchMessageStart)) != 0) {
            printf("\n\nPROCESSMESSAGE: INVALID MESSAGESTART\n\n");
            fOk = false;
            break;
        }

        // Read header
        CMessageHeader& hdr = msg.hdr;
        if (!hdr.IsValid())
        {
            printf("\n\nPROCESSMESSAGE: ERRORS IN HEADER %s\n\n\n", hdr.GetCommand().c_str());
            break;
        }
        string strCommand = hdr.GetCommand();

        // Message size
        unsigned int nMessageSize = hdr.nMessageSize;

        // Checksum
        CDataStream& vRecv = msg.vRecv;
        uint256 hash = Hash(vRecv.begin(), vRecv.begin() + nMessageSize);
        unsigned int nChecksum = 0;
        memcpy(&nChecksum, &hash, sizeof(nChecksum));
        if (nChecksum != hdr.nChecksum)
        {
            printf("ProcessMessages(%s, %u bytes) : CHECKSUM ERROR nChecksum=%08x hdr.nChecksum=%08x\n",
               strCommand.c_str(), nMessageSize, nChecksum, hdr.nChecksum);
            break;
        }

        RecordP2PMessageStat(pfrom, strCommand, nMessageSize, true);

        // Process message
        bool fRet = false;
        try
        {
            if (ibdactivepath::IBDActivePathTraceEnabled() &&
                strCommand == "block")
            {
                ibdactivepath::RecordBlockDispatchDelay(
                    std::max<int64_t>(0, GetTimeMicros() - msg.nTime),
                    nCompleteWaiting);
            }
            if (PingLifecycleTraceEnabled() && strCommand == "pong")
                PingLifecycleTracePongProcessBegin(pfrom, msg.nTime);
            if (IbdHeadersControlPlaneEnabled() && strCommand == "headers")
                printf("IBD_HEADER_DISPATCH event=dispatch_begin peer=%lld frame_us=%lld dispatch_us=%lld priority=%d\n",
                       (long long)pfrom->GetId(), (long long)msg.nTime,
                       (long long)GetTimeMicros(), fPrioritySelected ? 1 : 0);
            fRet = ProcessMessage(pfrom, strCommand, vRecv, msg.nTime);
            boost::this_thread::interruption_point();
        }
        catch (std::ios_base::failure& e)
        {
            if (strstr(e.what(), "end of data"))
            {
				if(fDebug)
					// Allow exceptions from under-length message on vRecv
					printf("ProcessMessages(%s, %u bytes) : Exception '%s' caught, normally caused by a message being shorter than its stated length\n", strCommand.c_str(), nMessageSize, e.what());
            }
            else if (strstr(e.what(), "size too large"))
            {
                printf("ProcessMessages(%s, %u bytes) : Oversized data from peer=%s - '%s'\n",
                       strCommand.c_str(), nMessageSize, pfrom->addr.ToString().c_str(), e.what());
                Misbehaving(pfrom->GetId(), 50);  // Severe penalty for oversized messages
            }
            else if (strstr(e.what(), "non-canonical"))
            {
                printf("ProcessMessages(%s, %u bytes) : Non-canonical encoding from peer=%s - '%s'\n",
                       strCommand.c_str(), nMessageSize, pfrom->addr.ToString().c_str(), e.what());
                Misbehaving(pfrom->GetId(), 20);  // Penalty for non-canonical encoding
            }
            else
            {
                PrintExceptionContinue(&e, "ProcessMessages()");
            }
        }
        catch (boost::thread_interrupted) {
            throw;
        }
        catch (std::exception& e) {
            PrintExceptionContinue(&e, "ProcessMessages()");
        } catch (...) {
            PrintExceptionContinue(NULL, "ProcessMessages()");
        }

        if (!fRet)
            printf("ProcessMessage(%s, %u bytes) FAILED\n", strCommand.c_str(), nMessageSize);

        if (strCommand == "block" &&
            (fSelectedRequestedBlock ||
             IsRequestedBlockMessage(pfrom, msg)) &&
            nBlockBurstDepth + 1 < IBD_REQUESTED_BLOCK_BURST_LIMIT)
        {
            TRY_LOCK(pfrom->cs_vRecvMsg, burstLock);
            if (burstLock)
            {
                const size_t nNext = FindRequestedBlockMessage(
                    pfrom, pfrom->nIbdPriorityScanOffset, 256);
                if (nNext != std::numeric_limits<size_t>::max())
                    return ProcessMessages(pfrom, burstLock,
                                           nBlockBurstDepth + 1);
            }
        }
        break;
    }

    return fOk;
}


bool SendMessages(CNode* pto, bool fSendTrickle,
                  const std::vector<CNode*>& vNodesCopy)
{
    RecoveryResponseResult recoveryResult;
    if (pto->ExpireRecoveryResponseWindow(GetTimeMicros(), recoveryResult) &&
        BlockRequestTraceEnabled())
        printf("%s\n", FormatRecoveryResponseSummary(
            pto->GetId(), recoveryResult).c_str());
    if (pto->ExpireGetBlocksOutstanding() && BlockRequestTraceEnabled())
        printf("BLOCKREQTRACE time_us=%lld event=GETBLOCKS_OUTSTANDING_TIMEOUT peer=%d\n",
               (long long)GetTimeMicros(), pto->GetId());
    if (pto->nVersion == 0)
        return true;

    bool pingSend = false;
    if (pto->fPingQueued) {
        pingSend = true;
    }
    if (pto->nPingNonceSent == 0 && pto->nPingUsecStart + PING_INTERVAL * 1000000 < GetTimeMicros()) {
        pingSend = true;
    }
    if (pingSend) {
        uint64_t nonce = 0;
        while (nonce == 0) {
            RAND_bytes((unsigned char*)&nonce, sizeof(nonce));
        }
        if (PingLifecycleTraceEnabled()) {
            if (pto->nPingNonceSent != 0)
                PingLifecycleTraceReplaced(pto, pto->nPingNonceSent);
            PingLifecycleTraceScheduled(pto, nonce);
        }
        pto->fPingQueued = false;
        pto->nPingUsecStart = GetTimeMicros();
        pto->nPingQueuedUsec = GetTimeMicros();
        if (pto->nVersion > BIP0031_VERSION) {
            pto->nPingNonceSent = nonce;
            pto->PushMessage("ping", nonce);
        } else {
            pto->nPingNonceSent = 0;
            pto->PushMessage("ping");
        }
        if (PingLifecycleTraceEnabled())
            PingLifecycleTraceQueuedSend(pto, nonce);
    }



    //
    // getblocks: handled OUTSIDE cs_main to prevent IBD stall when GUI
    // thread holds cs_main (Qt refreshWallet LOCK2).  CBlockLocator
    // construction only reads pprev pointers — same pattern used by the
    // sync-stall-recovery path above.
    //
    if (pto->fStartSync && !fImporting && !fReindex) {
        pto->fStartSync = false;
        if (!fSPVMode && !IbdHeaderSchedulerSelectActive() &&
            pto->CanAdvanceBlockSync(nBestHeight))
        {
            if (!pto->fInitialSyncRequestSent &&
                pto->PushGetBlocks(
                    pindexBest, uint256(0), ibdmetrics::GETBLOCKS_SOURCE_INITIAL))
                pto->fInitialSyncRequestPending = true;
        }
    }

    {
        // Single-flight getblocks flush: send at most one request per peer per
        // SendMessages pass, and only when no outstanding cycle is active.
        // The single-flight gate lives here (not in PushGetBlocks) so an
        // inv-continuation or recovery intent is never silently dropped while
        // a cycle is active — it coalesces into the bounded pending slot and
        // flushes once the outstanding cycle closes or expires.
        if (!pto->HasOutstandingGetBlocks() &&
            !pto->getBlocksIndex.empty())
        {
            CBlockIndex* pindexBegin = pto->getBlocksIndex[0];
            const uint256 hashStop = pto->getBlocksHash[0];
            const uint64_t nRecoveryId =
                pto->getBlocksRecoveryIds.size() > 0
                    ? pto->getBlocksRecoveryIds[0] : 0;
            const ibdmetrics::GetBlocksSource getBlocksSource =
                pto->getBlocksSources.size() > 0
                    ? pto->getBlocksSources[0]
                    : ibdmetrics::GETBLOCKS_SOURCE_OTHER;
            if (fDebugNet)
                printf("Pushing getblocks %s to %s\n\n",
                       pindexBegin->ToString().c_str(),
                       hashStop.ToString().c_str());
            if (fDebug)
                printf("GETBLOCKS_SEND peer=%d locator_height=%d locator_hash=%s stop=%s source=%s local_best_height=%d\n",
                       pto->GetId(),
                       pindexBegin ? pindexBegin->nHeight : -1,
                       pindexBegin ? pindexBegin->GetBlockHash().ToString().c_str() : "null",
                       hashStop.ToString().c_str(),
                       ibdexptrace::GetBlocksSourceName((int)getBlocksSource),
                       nBestHeight);
            RecoveryTraceSend(pto, nRecoveryId, pindexBegin, hashStop, 1);
            pto->PushMessage("getblocks",
                             CBlockLocator(pindexBegin), hashStop);
            ibdmetrics::RecordGetBlocksWireSent(getBlocksSource);
            ibdactivepath::RecordGetBlocksWireSent();
            pto->SetOutstandingGetBlocks(getBlocksSource,
                                         pindexBegin, hashStop);
            ibdmetrics::GetBlocksOutstandingAdd(1);
            // Arm the frontier response expectation when the flushed request
            // used the current active-tip locator (index == best and no stop
            // hash).  Its first unknown block inv may be admitted past a zero
            // deferred budget so the connectable frontier block can be
            // requested; the exemption is invalidated if the tip advances.
            if (pindexBegin == pindexBest && hashStop == uint256(0))
            {
                if (!pto->fFrontierResponsePending)
                    ibdmetrics::FrontierResponsePendingAdd(1);
                pto->fFrontierResponsePending = true;
                ibdmetrics::Get().frontier_response_armed.fetch_add(
                    1, std::memory_order_relaxed);
                pto->nFrontierLocatorHeight =
                    pindexBest ? pindexBest->nHeight : -1;
            }
            if (nRecoveryId == 0 && pto->fInitialSyncRequestPending)
            {
                pto->fInitialSyncRequestPending = false;
                pto->fInitialSyncRequestSent = true;
            }
            // This is the narrowest common point at which ANY block-sync
            // getblocks has actually been committed for transmission: every
            // source (initial sync, INV/orphan continuation, checkpoints,
            // wallet rescan) funnels through PushGetBlocks and is flushed
            // here.  Arm stalled-sync recovery exactly once per ordinary
            // (non-recovery) request to a peer that can advance block sync.
            RecordOrdinaryGetBlocksCommitted(
                GetTime(), nRecoveryId,
                pto->CanAdvanceBlockSync(nBestHeight));

            ibdmetrics::GetBlocksQueuedAdd(-1, true);
            pto->getBlocksIndex.clear();
            pto->getBlocksHash.clear();
            pto->getBlocksRecoveryIds.clear();
            pto->getBlocksSources.clear();
        }
    }

    {
        CSyncLockDiagnostics sendLockDiagnostics(
            "SendMessages", "cs_main");
        ibdmetrics::Get().refill_sendmessages_passes.fetch_add(
            1, std::memory_order_relaxed);
        TRY_LOCK(cs_main, lockMain);
        if (lockMain)
        {
            sendLockDiagnostics.Acquired();
            {
                CSyncLockPhase phase("SendMessages", "ibd_refill_scheduler");
                if (IbdHeaderSchedulerSelectActive())
                    RefillOrderedHeaderBlockRequests(pto, vNodesCopy);
                else
                    RefillDeferredBlockRequests(pto, vNodesCopy);
            }

        if (fSPVMode && pto->getHeadersSync.IsTimedOut(GetTime()))
            pto->PushGetHeaders(CBlockLocator(pindexBest), uint256(0), "timeout-retry");

        if (IbdHeadersControlPlaneEnabled() && !fSPVMode &&
            pto->getHeadersSync.IsTimedOut(GetTime()))
        {
            CBlockLocator observerLocator;
            if (PrepareIbdHeadersObserverRequest(pto, observerLocator))
                pto->PushGetHeaders(observerLocator, uint256(0), "ibd-observe-timeout");
        }

        {
            CSyncLockPhase phase("SendMessages", "dandelion");
        if (dandelionState.IsEnabled())
        {
            std::vector<int> vPeerIds;
            {
                TRY_LOCK(cs_vNodes, lockNodes);
                if (lockNodes)
                {
                    for (CNode* pnode : vNodes)
                        vPeerIds.push_back(pnode->GetId());
                }
            }
            if (!vPeerIds.empty())
                dandelionRouter.UpdateEpoch(GetTime(), vPeerIds);

            std::vector<uint256> vFluff = dandelionState.CheckStemTimeouts(GetTime());
            for (const uint256& txHash : vFluff)
            {
                LOCK(cs_mapRelay);
                CInv inv(MSG_TX, txHash);
                if (mapRelay.count(inv))
                    RelayInventory(inv);
            }
        }

        }
        // Resend wallet transactions that haven't gotten in a block yet
        // Except during reindex, importing and IBD, when old wallet
        // transactions become unconfirmed and spams other nodes.
        if (!fReindex && !IsInitialBlockDownload())
        {
            CSyncLockPhase phase("SendMessages", "wallet_resend");
            ResendWalletTransactions();
        }

        // Address refresh broadcast
        {
            CSyncLockPhase phase("SendMessages", "address_refresh");
            static int64_t nLastRebroadcast;
        if (!IsInitialBlockDownload() && (GetTime() - nLastRebroadcast > 24 * 60 * 60))
        {
            {
                TRY_LOCK(cs_vNodes, lockNodes);
                if (lockNodes)
                {
                    for (CNode* pnode : vNodes)
                    {
                        // Periodically clear setAddrKnown to allow refresh broadcasts
                        if (nLastRebroadcast)
                            pnode->setAddrKnown.clear();

                        // Rebroadcast our address
                        if (!fNoListen)
                        {
                            CAddress addr = GetLocalAddress(&pnode->addr);
                            if (addr.IsRoutable())
                                pnode->PushAddress(addr);
                        }
                    }
                    nLastRebroadcast = GetTime();
                }
            }
        }
        }

        //
        // Message: addr
        //
        {
            CSyncLockPhase phase("SendMessages", "addr_message");
        if (fSendTrickle)
        {
            vector<CAddress> vAddr;
            vAddr.reserve(pto->vAddrToSend.size());
            for (const CAddress& addr : pto->vAddrToSend)
            {
                // returns true if wasn't already contained in the set
                if (pto->setAddrKnown.insert(addr).second)
                {
                    vAddr.push_back(addr);
                    // receiver rejects addr messages larger than 1000
                    if (vAddr.size() >= 1000)
                    {
                        pto->PushMessage("addr", vAddr);
                        vAddr.clear();
                    }
                }
            }
            pto->vAddrToSend.clear();
            if (!vAddr.empty())
                pto->PushMessage("addr", vAddr);
        }
        }

        // A getblocks reply is protocol inventory, even when the peer prefers
        // headers for unsolicited block announcements.
        {
            CSyncLockPhase phase("SendMessages", "getblocks_inventory");
        vector<CInv> vGetBlocksInv;
        {
            LOCK(pto->cs_inventory);
            vGetBlocksInv.swap(pto->vGetBlocksInventoryToSend);
            for (const CInv& inv : vGetBlocksInv)
                pto->setInventoryKnown.insert(inv);
        }
        if (!vGetBlocksInv.empty())
        {
            vector<CInv> vResponse;
            vResponse.reserve(std::min((size_t)1000, vGetBlocksInv.size()));
            for (const CInv& inv : vGetBlocksInv)
            {
                vResponse.push_back(inv);
                if (vResponse.size() >= 1000)
                {
                    pto->PushMessage("inv", vResponse);
                    vResponse.clear();
                }
            }
            if (!vResponse.empty())
                pto->PushMessage("inv", vResponse);
        }
        }

        //
        // Message: inventory
        //
        {
            CSyncLockPhase phase("SendMessages", "inventory_construction");
        vector<CInv> vInv;
        vector<CInv> vInvWait;
        vector<CBlock> vBlockHeaders;
        {
            LOCK(pto->cs_inventory);
            vInv.reserve(pto->vInventoryToSend.size());
            vInvWait.reserve(pto->vInventoryToSend.size());
            for (const CInv& inv : pto->vInventoryToSend)
            {
                if (pto->setInventoryKnown.count(inv))
                    continue;

                // trickle out tx inv to protect privacy
                if (inv.type == MSG_TX && !fSendTrickle)
                {
                    // 1/4 of tx invs blast to all immediately
                    static uint256 hashSalt;
                    if (hashSalt == 0)
                        hashSalt = GetRandHash();
                    uint256 hashRand = inv.hash ^ hashSalt;
                    hashRand = Hash(BEGIN(hashRand), END(hashRand));
                    bool fTrickleWait = ((hashRand & 3) != 0);

                    // always trickle our own transactions
                    if (!fTrickleWait)
                    {
                        CWalletTx wtx;
                        if (GetTransaction(inv.hash, wtx))
                            if (wtx.fFromMe)
                                fTrickleWait = true;
                    }

                    if (fTrickleWait)
                    {
                        vInvWait.push_back(inv);
                        continue;
                    }
                }

                // returns true if wasn't already contained in the set
                if (pto->setInventoryKnown.insert(inv).second)
                {
                    if (inv.type == MSG_BLOCK && pto->fPreferHeaders)
                    {
                        map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(inv.hash);
                        if (mi != mapBlockIndex.end())
                        {
                            CBlockIndex* pindex = (*mi).second;
                            vBlockHeaders.push_back(pindex->GetBlockHeader());
                        }
                    }
                    else
                    {
                        vInv.push_back(inv);
                    }
                    if (vInv.size() >= 1000)
                    {
                        pto->PushMessage("inv", vInv);
                        vInv.clear();
                    }
                }
            }
            pto->vInventoryToSend = vInvWait;
        }
        if (!vInv.empty())
            pto->PushMessage("inv", vInv);
        if (!vBlockHeaders.empty())
            pto->PushMessage("headers", vBlockHeaders);
        }

        // getdata moved outside cs_main (below) for IBD reliability

    }
        else
        {
            ibdmetrics::Get().refill_skipped_cs_main_trylock_failed.fetch_add(
                1, std::memory_order_relaxed);
        }
    }
    //
    // getdata: flush pending requests outside cs_main.
    // Uses its own TRY_LOCK for AlreadyHave; if cs_main is unavailable
    // the request is sent anyway (duplicate receipt is harmless).
    //
    {
        vector<CInv> vGetData;
        int64_t nNow = GetTime() * 1000000;
        const size_t nMaxBlocksInFlightPerPeer =
            (size_t)GetMaxActiveBlockRequestsPerPeer();
        pto->ExpireBlockInFlight();
        const bool fIBDPassHadDue =
            !pto->mapAskFor.empty() &&
            (*pto->mapAskFor.begin()).first <= nNow;
        const bool fIBDPassHadFreeCapacity =
            pto->setBlocksInFlight.size() < nMaxBlocksInFlightPerPeer;
        int64_t nIBDPassSent = 0;
        while (!pto->mapAskFor.empty() && (*pto->mapAskFor.begin()).first <= nNow)
        {
            CInv inv = (*pto->mapAskFor.begin()).second;
            bool fSkip = false;
            bool fBlockRequest = (inv.type == MSG_BLOCK || inv.type == MSG_FILTERED_BLOCK);
            const bool fTraceBlockRequest =
                inv.type == MSG_BLOCK && BlockRequestTraceEnabled();
            bool fSamePeerInFlight = false;
            bool fCsMainCheckPerformed = false;
            bool fCsMainCheckResult = false;
            int nKnownInBlockIndex = -1;
            if (fBlockRequest)
            {
                fSamePeerInFlight = pto->IsBlockInFlight(inv.hash);
                if (fSamePeerInFlight)
                {
                    if (fTraceBlockRequest)
                    {
                        BlockRequestTraceAskRemoved(
                            pto, inv.hash,
                            "same-peer-inflight", -1);
                    }
                    pto->EraseAskForEntry(
                        pto->mapAskFor.begin(), false,
                        ibdmetrics::ACTIVE_DECREMENT_ASKFOR_REMOVED_OWNER_CONFLICT);
                    EraseAlreadyAskedForIfUnowned(inv);
                    continue;
                }
                if (pto->setBlocksInFlight.size() >= nMaxBlocksInFlightPerPeer)
                {
                    break;
                }
                NodeId nOwnerPeer = -1;
                BlockRequestOwnerState ownerState = BLOCK_REQUEST_OWNER_QUEUED;
                const bool fIsOwner = GetBlockRequestOwnerForPeer(
                    inv.hash, pto->GetId(), &ownerState);
                if (!fIsOwner)
                {
                    const size_t nOwnerCount = GetBlockRequestOwnerCount(inv.hash);
                    if (nOwnerCount == 0)
                    {
                        // Legacy / manually queued entries may not have an
                        // owner slot yet; claim one atomically before sending.
                        if (!TryAssignBlockRequestOwner(
                                inv.hash, pto->GetId(), BLOCKREQ_SOURCE_ASKFOR,
                                &nOwnerPeer, &ownerState))
                        {
                            if (fTraceBlockRequest)
                                BlockRequestTraceGetDataSkip(
                                    pto, inv.hash, nOwnerPeer,
                                    BlockRequestOwnerStateName(ownerState));
                            pto->EraseAskForEntry(
                                pto->mapAskFor.begin(), true,
                                ibdmetrics::ACTIVE_DECREMENT_ASKFOR_REMOVED_OWNER_CONFLICT);
                            continue;
                        }
                    }
                    else
                    {
                        GetBlockRequestOwner(inv.hash, &nOwnerPeer, NULL);
                        if (fTraceBlockRequest)
                            BlockRequestTraceGetDataSkip(
                                pto, inv.hash, nOwnerPeer,
                                BlockRequestOwnerStateName(ownerState));
                        RecordAlternateBlockAnnouncer(inv.hash, pto->GetId());
                        pto->EraseAskForEntry(
                            pto->mapAskFor.begin(), true,
                            ibdmetrics::ACTIVE_DECREMENT_ASKFOR_REMOVED_OWNER_CONFLICT);
                        continue;
                    }
                }
                else if (ownerState == BLOCK_REQUEST_OWNER_IN_FLIGHT)
                {
                    if (fTraceBlockRequest)
                        BlockRequestTraceGetDataSkip(
                            pto, inv.hash, pto->GetId(),
                            BlockRequestOwnerStateName(ownerState));
                    pto->EraseAskForEntry(
                        pto->mapAskFor.begin(), false,
                        ibdmetrics::ACTIVE_DECREMENT_ASKFOR_REMOVED_OWNER_CONFLICT);
                    continue;
                }
            }
            {
                CSyncLockPhase phase("SendMessages", "alreadyhave_txdb");
                TRY_LOCK(cs_main, lockMain);
                if (lockMain)
                {
                    fCsMainCheckPerformed = true;
                    if (fTraceBlockRequest)
                    {
                        std::map<uint256, CBlockIndex*>::const_iterator miTrace =
                            mapBlockIndex.find(inv.hash);
                        nKnownInBlockIndex = miTrace != mapBlockIndex.end() ? 1 : 0;
                        uint256 hashParent = uint256(0);
                        if (miTrace != mapBlockIndex.end() && miTrace->second->pprev)
                            hashParent = miTrace->second->pprev->GetBlockHash();
                        else if (mapOrphanBlocks.count(inv.hash) != 0)
                            hashParent = mapOrphanBlocks[inv.hash]->hashPrevBlock;
                        BlockRequestTraceUpdateBlockContextLocked(
                            inv.hash, hashParent, uint256(0));
                    }
                    CTxDB txdb("r");
                    fSkip = AlreadyHave(txdb, inv);
                    fCsMainCheckResult = fSkip;
                }
            }
            if (!fSkip)
            {
                if (fBlockRequest)
                {
                    NodeId nOwnerPeer = -1;
                    BlockRequestOwnerState ownerState = BLOCK_REQUEST_OWNER_QUEUED;
                    const int nDesiredCopies =
                        std::max(1, GetBlockRequestOwnerMaxCopies(inv.hash));
                    if (!TryAssignBlockRequestOwner(
                            inv.hash, pto->GetId(), BLOCKREQ_SOURCE_ASKFOR,
                            &nOwnerPeer, &ownerState, nDesiredCopies))
                    {
                        if (fTraceBlockRequest)
                            BlockRequestTraceGetDataSkip(
                                pto, inv.hash, nOwnerPeer,
                                BlockRequestOwnerStateName(ownerState));
                        pto->EraseAskForEntry(
                            pto->mapAskFor.begin(), true,
                            ibdmetrics::ACTIVE_DECREMENT_ASKFOR_REMOVED_OWNER_CONFLICT);
                        EraseAlreadyAskedForIfUnowned(inv);
                        continue;
                    }
                }
                if (fDebugNet)
                    printf("sending getdata: %s\n", inv.ToString().c_str());
                vGetData.push_back(inv);
                if (fBlockRequest)
                {
                    ibdactivepath::RecordAskForToGetData(
                        std::max<int64_t>(0, nNow - (*pto->mapAskFor.begin()).first));
                    ++nIBDPassSent;
                    pto->MarkBlockInFlight(inv.hash);
                    ibdactivepath::RecordBlockRequestSent(inv.hash);
                    ibdblocklatency::RecordGetDataSent(inv.hash, pto->GetId());
                    if (IbdHeadersControlPlaneEnabled())
                        TraceIbdHeadersObserverEvent("request", pto, inv.hash, -1);
                    if (fTraceBlockRequest)
                        BlockRequestTraceInFlightMark(pto, inv.hash, true);
                }
                if (vGetData.size() >= 1000)
                {
                    ibdforensic::RecordGetDataBatch(
                        pto->GetId(), BlockHashesOfGetData(vGetData),
                        GetTimeMicros(), pto->nSendSize,
                        pto->hashLastBlockInBatch, pto->nExpectedBatchSize);
                    pto->nLastGetDataTime = GetTime();
                    pto->PushBlockGetData(vGetData);
                    vGetData.clear();
                }
            }
            int64_t nPreviousGlobalAskedTime = -1;
            {
                LOCK(cs_mapAlreadyAskedFor);
                if (fTraceBlockRequest)
                {
                    std::map<CInv, int64_t>::const_iterator miPrevious =
                        mapAlreadyAskedFor.find(inv);
                    if (miPrevious != mapAlreadyAskedFor.end())
                        nPreviousGlobalAskedTime = miPrevious->second;
                }
                if (mapAlreadyAskedFor.size() < MAX_ALREADY_ASKED_FOR_SIZE ||
                    mapAlreadyAskedFor.count(inv) != 0)
                    mapAlreadyAskedFor[inv] = nNow;
            }
            if (!fSkip && fTraceBlockRequest)
            {
                BlockRequestTraceGetDataSend(
                    pto, inv.hash, BLOCKREQ_SOURCE_ASKFOR,
                    nKnownInBlockIndex,
                    fCsMainCheckPerformed,
                    fCsMainCheckResult,
                    fSamePeerInFlight,
                    true,
                    nPreviousGlobalAskedTime,
                    nNow);
            }
            else if (fSkip && fTraceBlockRequest)
            {
                BlockRequestTraceAskRemoved(
                    pto, inv.hash, "already-have",
                    nKnownInBlockIndex);
            }
            if (fSkip)
            {
                pto->EraseAskForEntry(
                    pto->mapAskFor.begin(), true,
                    ibdmetrics::ACTIVE_DECREMENT_ASKFOR_REMOVED_ALREADY_HAVE);
                EraseAlreadyAskedForIfUnowned(inv);
            }
            else
            {
                pto->EraseAskForEntry(
                    pto->mapAskFor.begin(), false,
                    ibdmetrics::ACTIVE_DECREMENT_ASKFOR_SENT_TRANSITION);
            }
        }
        {
            int nStopReason = ibdactivepath::GETDATA_STOP_OTHER;
            if (pto->mapAskFor.empty())
                nStopReason = ibdactivepath::GETDATA_STOP_EMPTY;
            else if ((*pto->mapAskFor.begin()).first > nNow)
                nStopReason = ibdactivepath::GETDATA_STOP_NO_DUE;
            else
                nStopReason = ibdactivepath::GETDATA_STOP_INFLIGHT_CAP;
            ibdactivepath::RecordGetDataPass(
                nIBDPassSent, fIBDPassHadDue, fIBDPassHadFreeCapacity,
                nStopReason);
        }
        if (!vGetData.empty())
        {
            ibdforensic::RecordGetDataBatch(
                pto->GetId(), BlockHashesOfGetData(vGetData),
                GetTimeMicros(), pto->nSendSize,
                pto->hashLastBlockInBatch, pto->nExpectedBatchSize);
            pto->nLastGetDataTime = GetTime();
            pto->PushBlockGetData(vGetData);
        }
    }

    return true;
}

int64_t GetCollateralnodePayment(int nHeight, int64_t blockValue)
{
    if (blockValue <= 0)
        return 0;
    return (blockValue / 100) * 65 + ((blockValue % 100) * 65) / 100;
}
