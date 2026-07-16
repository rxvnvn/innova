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
    BLOCKREQ_SOURCE_REJECT_RECOVERY
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
    BLOCKREQ_RESULT_TRUE_UNINDEXED
};

bool InitBlockRequestTrace(bool fEnabled, const std::string& strHashFilter);
bool BlockRequestTraceEnabled();

void BlockRequestTraceAskSchedule(CNode* pnode, const uint256& hash,
                                  BlockRequestTraceSource source,
                                  int64_t nScheduledTime,
                                  int64_t nPreviousGlobalTime,
                                  bool fSamePeerInFlight);
void BlockRequestTraceAskRemoved(CNode* pnode, const uint256& hash,
                                 const char* pszReason,
                                 int nKnownInBlockIndex);
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

#endif // INNOVA_BLOCKREQUESTTRACE_H
