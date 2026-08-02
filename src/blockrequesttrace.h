// Copyright (c) 2026 The Innova developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef INNOVA_BLOCKREQUESTTRACE_H
#define INNOVA_BLOCKREQUESTTRACE_H

#include "uint256.h"

#include <stdint.h>
#include <string>
#include <vector>

class CNode;

enum BlockRequestTraceSource
{
    BLOCKREQ_SOURCE_OTHER = 0,
    BLOCKREQ_SOURCE_ASKFOR,
    BLOCKREQ_SOURCE_INV,
    BLOCKREQ_SOURCE_HEADERS_DIRECT,
    BLOCKREQ_SOURCE_ORPHAN,
    BLOCKREQ_SOURCE_CHECKPOINT,
    BLOCKREQ_SOURCE_REJECT_RECOVERY,
    BLOCKREQ_SOURCE_ORPHAN_LIMIT_RETRY
};

enum BlockRequestTraceResult
{
    BLOCKREQ_RESULT_UNKNOWN = 0,
    BLOCKREQ_RESULT_ACCEPTED_ACTIVE,
    BLOCKREQ_RESULT_ACCEPTED_INDEXED,
    BLOCKREQ_RESULT_ORPHAN_NEW,
    BLOCKREQ_RESULT_ALREADY_KNOWN,
    BLOCKREQ_RESULT_ORPHAN_DUPLICATE,
    BLOCKREQ_RESULT_REJECTED,
    BLOCKREQ_RESULT_ORPHAN_LIMIT_IBD,
    BLOCKREQ_RESULT_ACCEPT_FAILED,
    BLOCKREQ_RESULT_TRUE_UNINDEXED
};

bool InitBlockRequestTrace(bool fEnabled, const std::string& strHashFilter);
bool BlockRequestTraceEnabled();

void BlockRequestTraceSetBlockContext(const uint256& hash,
                                      const uint256& parentHash,
                                      const char* pszParentStatus,
                                      int nActiveHeight,
                                      int nBlockIndexHeight,
                                      bool fBodyKnown,
                                      const uint256& orphanChildHash);
void BlockRequestTraceAskSchedule(CNode* pnode, const uint256& hash,
                                  BlockRequestTraceSource source,
                                  int64_t nScheduledTime,
                                  int64_t nPreviousGlobalTime,
                                  bool fSamePeerInFlight);
void BlockRequestTraceAskSkip(CNode* pnode, const uint256& hash,
                              BlockRequestTraceSource source,
                              const char* pszReason,
                              int ownerPeer = -1,
                              const char* pszOwnerState = "none");
void BlockRequestTraceAskSkipOrphanPressure(CNode* pnode, const uint256& hash,
                                            BlockRequestTraceSource source,
                                            int nOrphanCountPeer,
                                            int nQueuedBlockRequests,
                                            int nSentBlockRequests,
                                            int nProjectedPressure,
                                            int nPressureBudget,
                                            int nHardLimit,
                                            int ownerPeer = -1,
                                            const char* pszOwnerState = "none");
void BlockRequestTraceAskRemoved(CNode* pnode, const uint256& hash,
                                 const char* pszReason,
                                 int nKnownInBlockIndex);
void BlockRequestTraceGetDataSkip(CNode* pnode, const uint256& hash,
                                  int ownerPeer,
                                  const char* pszOwnerState);
void BlockRequestTraceGetDataSend(CNode* pnode, const uint256& hash,
                                  BlockRequestTraceSource path,
                                  int nKnownInBlockIndex,
                                  bool fCsMainCheckPerformed,
                                  bool fCsMainCheckResult,
                                  bool fSamePeerInFlight,
                                  bool fMapAskForPresent,
                                  int64_t nPreviousGlobalAskedTime,
                                  int64_t nWrittenGlobalAskedTime);
void BlockRequestTraceInFlightMark(CNode* pnode, const uint256& hash,
                                   bool fConsumesQueuedEntry);
void BlockRequestTraceBlockReceive(CNode* pnode, const uint256& hash,
                                   bool fKnownBefore,
                                   bool fSenderInFlightBefore,
                                   int64_t nSenderInFlightAge);
void BlockRequestTraceBlockResult(CNode* pnode, const uint256& hash,
                                  BlockRequestTraceResult result,
                                  bool fProcessBlockResult,
                                  bool fIndexedAfter,
                                  bool fActiveChainAfter,
                                  bool fBestChainAfter,
                                  int nHeightAfter);
void BlockRequestTraceInFlightClear(CNode* pnode, const uint256& hash,
                                    const char* pszReason,
                                    int64_t nAge,
                                    bool fKnownInBlockIndex);
void BlockRequestTraceInFlightExpire(CNode* pnode, const uint256& hash,
                                     int64_t nAge);
void BlockRequestTracePeerClosed(CNode* pnode);
void BlockRequestTraceOwnerAssign(const uint256& hash, int peer,
                                  const char* pszState,
                                  BlockRequestTraceSource source);
void BlockRequestTraceOwnerRelease(const uint256& hash, int peer,
                                   const char* pszState,
                                   const char* pszReason);

void BlockRequestTraceGetBlocksQueued(CNode* pnode,
                                      const uint256& hashBegin,
                                      int nBeginHeight,
                                      const uint256& hashStop);
void BlockRequestTraceStallRecovery(CNode* pnode,
                                    int nLocalHeight,
                                    int nPeerHeight,
                                    int64_t nLastBlockAge,
                                    const uint256& hashBegin,
                                    int nBeginHeight,
                                    const uint256& hashStop,
                                    const std::vector<uint256>& vErasedHashes);
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
                                       const uint256& hashStop);

// Continuity / divergence diagnosis instrumentation. All events below are
// emitted on the SYNC_EVENT stream, which is enabled whenever
// blockrequesttrace=1 is active. Event state is passed as values so tracing
// never acquires cs_main, cs_vNodes, or a peer lock in reverse order.

// Enrich the per-tip-advance SETBESTCHAIN_COMMIT event with the previous tip,
// the source peer, whether the block was requested, and its first request/send
// pipeline origin. Called on every active-chain tip change.
void BlockRequestTraceSetBestChainCommit(int peer,
                                         const uint256& hashOldTip,
                                         int nOldHeight,
                                         const uint256& hashNewTip,
                                         int nNewHeight,
                                         bool fReorg,
                                         int64_t nBlockTime);

// Emit a REORG event describing an active-chain switch: fork point,
// disconnected/connected branches, chain-trust comparison, and the peer that
// supplied the winning candidate.
void BlockRequestTraceReorg(int peer,
                            const uint256& hashFork, int nForkHeight,
                            const uint256& hashOldBest, int nOldHeight,
                            const uint256& hashNewBest, int nNewHeight,
                            const std::vector<uint256>& vDisconnect,
                            const std::vector<uint256>& vConnect,
                            const std::string& strOldTrust,
                            const std::string& strNewTrust);

// Emit the one-shot FIRST_CONTINUITY_BREAK event. Fires only once per process
// at the earliest block receive whose parent is absent locally while no block
// has connected for the configured interval. Returns true when emitted now.
bool BlockRequestTraceContinuityBreak(CNode* pnode,
                                      const uint256& hashBlock,
                                      const uint256& hashPrev,
                                      int nLocalHeight,
                                      const uint256& hashLocalTip,
                                      int64_t nLastAcceptedAgeSeconds,
                                      int nPeerBestHeight,
                                      const uint256& hashPeerBest,
                                      bool fPrevInOrphans,
                                      bool fPrevInFlight,
                                      bool fPrevQueued,
                                      bool fPrevOwnerClaimed,
                                      int nPrevOwnerPeer,
                                      const char* pszPrevOwnerState,
                                      int nOrphanCountPeer,
                                      size_t nOrphanCountGlobal,
                                      int nTipAncestorOfPeerBest);

// Reset the one-shot continuity-break gate. Called when the trace is (re)enabled.
void BlockRequestTraceContinuityBreakReset();

// Emit a MISSING_PARENT_REQUEST at orphan-insertion time: the orphan block,
// the selected missing ancestor, whether AskFor admitted it, and whether this
// peer claimed ownership. The wanted hash is tracked so that its eventual
// arrival can be reported by BlockRequestTraceMissingParentResolved.
void BlockRequestTraceMissingParentRequest(CNode* pnode,
                                           const uint256& orphanHash,
                                           const uint256& orphanPrev,
                                           const uint256& hashWanted,
                                           bool fAdmitted,
                                           bool fOwnerClaimed,
                                           int nOrphanCountPeer,
                                           size_t nOrphanCountGlobal);

// Emit a MISSING_PARENT_RESOLVED when a previously-wanted missing parent
// arrives and is processed. Includes whether a getdata was sent and the final
// ProcessBlock outcome (read from the trace registry). Returns true when the
// block matched a pending missing-parent request.
bool BlockRequestTraceMissingParentResolved(CNode* pnode,
                                            const uint256& hashBlock,
                                            bool fAccepted,
                                            int nHeightAfter);

// Per-peer watermark events when orphan or deferred block-inv counts cross
// 64/128/256/512/700/750. pszAction is "add" or "remove". Returns true when a
// watermark event was emitted for this transition.
bool BlockRequestTraceOrphanWatermark(int peer, int nCount, const char* pszAction);
bool BlockRequestTraceDeferredWatermark(int peer, int nCount, const char* pszAction);

enum ProcessBlockRejectReason
{
    PBREJECT_DUPLICATE_INDEXED = 0,
    PBREJECT_DUPLICATE_ORPHAN,
    PBREJECT_DUPLICATE_INDEXED_STAKE,
    PBREJECT_POS_AFTER_DAG,
    PBREJECT_CHECKBLOCK_FALSE,
    PBREJECT_WEAK_CHECKPOINT,
    PBREJECT_ORPHAN_LIMIT_IBD,
    PBREJECT_ORPHAN_LIMIT_NORMAL,
    PBREJECT_DUPLICATE_STAKE_ORPHAN,
    PBREJECT_ACCEPTBLOCK_FALSE,
    PBREJECT_UNKNOWN_FALSE
};

bool InitProcessBlockRejectTrace(bool fEnabled);
bool ProcessBlockRejectTraceEnabled();
const char* ProcessBlockRejectReasonName(ProcessBlockRejectReason reason);
std::string ProcessBlockRejectTraceLastReason(const uint256& hash);

#endif // INNOVA_BLOCKREQUESTTRACE_H
