// Copyright (c) 2026 The Innova developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "pinglifecycletrace.h"

#include "net.h"
#include "util.h"

#include <stdint.h>
#include <string>
#include <vector>

namespace {

// Observation-only state.  The enabled flag is initialized once in AppInit2
// before networking threads start and is then immutable, matching the
// blockrequesttrace pattern.  Counters are guarded because they are updated
// from both the socket and message-handler threads.
CCriticalSection cs_pingLifecycle;
bool fPingLifecycleEnabled = false;
int64_t nLastPingLifecycle1sEmit = 0;

struct PingLifecycleCounters
{
    uint64_t pingsScheduled;
    uint64_t pingsReplaced;
    uint64_t pingsQueued;
    uint64_t pingsSent;         // socket_write_complete (fully flushed)
    uint64_t pongsFrameComplete;
    uint64_t pongsProcessed;
    uint64_t pongsMatched;
    uint64_t pongsUnmatched;    // wrong nonce, zero nonce, unsolicited, short
    uint64_t peerDisconnects;
    uint64_t pingQueueToSendCount;
    uint64_t pingQueueToSendSumUs;
    uint64_t pingQueueToSendMaxUs;
    uint64_t pongFrameToProcessCount;
    uint64_t pongFrameToProcessSumUs;
    uint64_t pongFrameToProcessMaxUs;

    PingLifecycleCounters()
        : pingsScheduled(0),
          pingsReplaced(0),
          pingsQueued(0),
          pingsSent(0),
          pongsFrameComplete(0),
          pongsProcessed(0),
          pongsMatched(0),
          pongsUnmatched(0),
          peerDisconnects(0),
          pingQueueToSendCount(0),
          pingQueueToSendSumUs(0),
          pingQueueToSendMaxUs(0),
          pongFrameToProcessCount(0),
          pongFrameToProcessSumUs(0),
          pongFrameToProcessMaxUs(0)
    {
    }
};

PingLifecycleCounters g_counters;

const char* PingLifecyclePongResultName(int nResult)
{
    switch (nResult)
    {
    case PONG_RESULT_MATCHED:
        return "matched";
    case PONG_RESULT_WRONG_NONCE:
        return "wrong_nonce";
    case PONG_RESULT_NONCE_ZERO:
        return "nonce_zero";
    case PONG_RESULT_UNSOLICITED:
        return "unsolicited";
    case PONG_RESULT_SHORT_PAYLOAD:
        return "short_payload";
    default:
        return "unknown";
    }
}

// Emit one additive PING_LIFECYCLE line.  Peer fields are read best-effort
// without acquiring peer locks; this is observation-only diagnostics that must
// never alter production behaviour.  recv_buffer_bytes is only meaningful when
// the caller holds cs_vRecvMsg (pong frame/process events); otherwise 0.
void PingLifecycleEmit(CNode* pnode, const char* pszEvent, uint64_t nonce,
                       int64_t nSinceScheduledUs, int64_t nSinceQueuedUs,
                       unsigned int nRecvBufferBytes)
{
    printf("PING_LIFECYCLE time_us=%lld nodeid=%d addr=%s subver=%s nonce=%llu "
           "event=%s since_scheduled_us=%lld since_queued_us=%lld "
           "recv_queue_depth=%zu send_buffer_bytes=%zu recv_buffer_bytes=%u "
           "blocks_inflight=%zu ask_queue=%zu lastsend=%lld lastrecv=%lld\n",
           (long long)GetTimeMicros(),
           pnode->GetId(),
           pnode->addr.ToString().c_str(),
           pnode->strSubVer.c_str(),
           (unsigned long long)nonce,
           pszEvent,
           (long long)nSinceScheduledUs,
           (long long)nSinceQueuedUs,
           pnode->vRecvMsg.size(),
           pnode->nSendSize,
           nRecvBufferBytes,
           pnode->setBlocksInFlight.size(),
           pnode->mapAskFor.size(),
           (long long)pnode->nLastSend,
           (long long)pnode->nLastRecv);
}

// Sum of buffered receive message payloads (caller holds cs_vRecvMsg).
unsigned int PingLifecycleRecvBufferBytes(CNode* pnode)
{
    unsigned int total = 0;
    for (std::deque<CNetMessage>::const_iterator it = pnode->vRecvMsg.begin();
         it != pnode->vRecvMsg.end(); ++it)
        total += (unsigned int)it->vRecv.size() + 24;
    return total;
}

} // namespace

bool InitPingLifecycleTrace(bool fEnabled)
{
    LOCK(cs_pingLifecycle);
    fPingLifecycleEnabled = fEnabled;
    g_counters = PingLifecycleCounters();
    nLastPingLifecycle1sEmit = 0;
    if (fEnabled)
    {
        printf("PING_LIFECYCLE time_us=%lld nodeid=-1 addr=none subver=none "
               "nonce=0 event=START since_scheduled_us=0 since_queued_us=0 "
               "recv_queue_depth=0 send_buffer_bytes=0 recv_buffer_bytes=0 "
               "blocks_inflight=0 ask_queue=0 lastsend=0 lastrecv=0\n",
               (long long)GetTimeMicros());
    }
    return true;
}

bool PingLifecycleTraceEnabled()
{
    return fPingLifecycleEnabled;
}

void PingLifecycleTraceReplaced(CNode* pnode, uint64_t nOldNonce)
{
    if (!fPingLifecycleEnabled)
        return;
    {
        LOCK(cs_pingLifecycle);
        g_counters.pingsReplaced++;
    }
    const int64_t nNow = GetTimeMicros();
    PingLifecycleEmit(pnode, "ping_replaced", nOldNonce,
                      nNow - pnode->nPingUsecStart, 0, 0);
}

void PingLifecycleTraceScheduled(CNode* pnode, uint64_t nonce)
{
    if (!fPingLifecycleEnabled)
        return;
    {
        LOCK(cs_pingLifecycle);
        g_counters.pingsScheduled++;
    }
    const int64_t nNow = GetTimeMicros();
    PingLifecycleEmit(pnode, "scheduled", nonce,
                      nNow - pnode->nPingUsecStart, 0, 0);
}

void PingLifecycleTraceQueuedSend(CNode* pnode, uint64_t nonce)
{
    if (!fPingLifecycleEnabled)
        return;
    {
        LOCK(cs_pingLifecycle);
        g_counters.pingsQueued++;
    }
    const int64_t nNow = GetTimeMicros();
    PingLifecycleEmit(pnode, "queued_send", nonce,
                      nNow - pnode->nPingUsecStart,
                      nNow - pnode->nPingQueuedUsec, 0);
}

void PingLifecycleTraceSocketWriteBegin(CNode* pnode)
{
    if (!fPingLifecycleEnabled)
        return;
    const int64_t nNow = GetTimeMicros();
    {
        LOCK(cs_pingLifecycle);
        g_counters.pingQueueToSendCount++;
        g_counters.pingQueueToSendSumUs +=
            std::max<int64_t>(0, nNow - pnode->nPingQueuedUsec);
        if (nNow - pnode->nPingQueuedUsec > g_counters.pingQueueToSendMaxUs)
            g_counters.pingQueueToSendMaxUs = nNow - pnode->nPingQueuedUsec;
    }
    PingLifecycleEmit(pnode, "socket_write_begin", pnode->nPingNonceSent,
                      nNow - pnode->nPingUsecStart,
                      nNow - pnode->nPingQueuedUsec, 0);
}

void PingLifecycleTraceSocketWriteComplete(CNode* pnode)
{
    if (!fPingLifecycleEnabled)
        return;
    {
        LOCK(cs_pingLifecycle);
        g_counters.pingsSent++;
    }
    const int64_t nNow = GetTimeMicros();
    PingLifecycleEmit(pnode, "socket_write_complete", pnode->nPingNonceSent,
                      nNow - pnode->nPingUsecStart,
                      nNow - pnode->nPingQueuedUsec, 0);
}

void PingLifecycleTracePongFrameComplete(CNode* pnode)
{
    if (!fPingLifecycleEnabled)
        return;
    {
        LOCK(cs_pingLifecycle);
        g_counters.pongsFrameComplete++;
    }
    const int64_t nNow = GetTimeMicros();
    // Caller (SocketHandler) holds cs_vRecvMsg, so the queue is consistent.
    PingLifecycleEmit(pnode, "pong_frame_complete", pnode->nPingNonceSent,
                      nNow - pnode->nPingUsecStart,
                      nNow - pnode->nPingQueuedUsec,
                      PingLifecycleRecvBufferBytes(pnode));
}

void PingLifecycleTracePongProcessBegin(CNode* pnode, int64_t nFrameCompleteUs)
{
    if (!fPingLifecycleEnabled)
        return;
    const int64_t nNow = GetTimeMicros();
    const int64_t nFrameToProcess = std::max<int64_t>(0, nNow - nFrameCompleteUs);
    {
        LOCK(cs_pingLifecycle);
        g_counters.pongsProcessed++;
        g_counters.pongFrameToProcessCount++;
        g_counters.pongFrameToProcessSumUs += nFrameToProcess;
        if (nFrameToProcess > g_counters.pongFrameToProcessMaxUs)
            g_counters.pongFrameToProcessMaxUs = nFrameToProcess;
    }
    // Caller (ProcessMessages) holds cs_vRecvMsg, so the queue is consistent.
    PingLifecycleEmit(pnode, "pong_process_begin", pnode->nPingNonceSent,
                      nNow - pnode->nPingUsecStart,
                      nNow - pnode->nPingQueuedUsec,
                      PingLifecycleRecvBufferBytes(pnode));
}

void PingLifecycleTracePongResult(CNode* pnode, uint64_t nonce, int nResult)
{
    if (!fPingLifecycleEnabled)
        return;
    {
        LOCK(cs_pingLifecycle);
        if (nResult == PONG_RESULT_MATCHED)
            g_counters.pongsMatched++;
        else
            g_counters.pongsUnmatched++;
    }
    const int64_t nNow = GetTimeMicros();
    PingLifecycleEmit(pnode, PingLifecyclePongResultName(nResult), nonce,
                      nNow - pnode->nPingUsecStart,
                      nNow - pnode->nPingQueuedUsec, 0);
}

void PingLifecycleTracePeerClosed(CNode* pnode)
{
    if (!fPingLifecycleEnabled)
        return;
    {
        LOCK(cs_pingLifecycle);
        g_counters.peerDisconnects++;
    }
    const int64_t nNow = GetTimeMicros();
    PingLifecycleEmit(pnode, "peer_disconnect", pnode->nPingNonceSent,
                      pnode->nPingNonceSent && pnode->nPingUsecStart
                          ? nNow - pnode->nPingUsecStart : 0,
                      pnode->nPingQueuedUsec
                          ? nNow - pnode->nPingQueuedUsec : 0,
                      0);
}

void PingLifecycleTraceEmit1s(const std::vector<CNode*>& vNodesCopy)
{
    if (!fPingLifecycleEnabled)
        return;
    const int64_t nNow = GetTimeMicros();
    LOCK(cs_pingLifecycle);
    if (nNow - nLastPingLifecycle1sEmit < 1000000)
        return;
    nLastPingLifecycle1sEmit = nNow;

    int nOutstanding = 0;
    int64_t nMaxAgeMs = 0;
    for (std::vector<CNode*>::const_iterator it = vNodesCopy.begin();
         it != vNodesCopy.end(); ++it)
    {
        const CNode* pnode = *it;
        if (pnode->nPingNonceSent != 0 && pnode->nPingUsecStart != 0)
        {
            ++nOutstanding;
            const int64_t nAgeMs = (nNow - pnode->nPingUsecStart) / 1000;
            if (nAgeMs > nMaxAgeMs)
                nMaxAgeMs = nAgeMs;
        }
    }

    printf("PING_LIFECYCLE_1S time_us=%lld pings_scheduled=%llu "
           "pings_replaced=%llu pings_queued=%llu pings_sent=%llu "
           "pongs_frame_complete=%llu pongs_processed=%llu pongs_matched=%llu "
           "pongs_unmatched=%llu ping_outstanding_current=%d "
           "ping_outstanding_max_age_ms=%lld "
           "pong_frame_to_process_avg_us=%llu pong_frame_to_process_max_us=%llu "
           "ping_queue_to_send_avg_us=%llu ping_queue_to_send_max_us=%llu\n",
           (long long)nNow,
           (unsigned long long)g_counters.pingsScheduled,
           (unsigned long long)g_counters.pingsReplaced,
           (unsigned long long)g_counters.pingsQueued,
           (unsigned long long)g_counters.pingsSent,
           (unsigned long long)g_counters.pongsFrameComplete,
           (unsigned long long)g_counters.pongsProcessed,
           (unsigned long long)g_counters.pongsMatched,
           (unsigned long long)g_counters.pongsUnmatched,
           nOutstanding,
           (long long)nMaxAgeMs,
           (unsigned long long)(g_counters.pongFrameToProcessCount
               ? g_counters.pongFrameToProcessSumUs / g_counters.pongFrameToProcessCount : 0),
           (unsigned long long)g_counters.pongFrameToProcessMaxUs,
           (unsigned long long)(g_counters.pingQueueToSendCount
               ? g_counters.pingQueueToSendSumUs / g_counters.pingQueueToSendCount : 0),
           (unsigned long long)g_counters.pingQueueToSendMaxUs);
}

PingLifecycleCountersSnapshot PingLifecycleCountersForTesting()
{
    PingLifecycleCountersSnapshot snap;
    LOCK(cs_pingLifecycle);
    snap.pingsScheduled = g_counters.pingsScheduled;
    snap.pingsReplaced = g_counters.pingsReplaced;
    snap.pingsQueued = g_counters.pingsQueued;
    snap.pingsSent = g_counters.pingsSent;
    snap.pongsFrameComplete = g_counters.pongsFrameComplete;
    snap.pongsProcessed = g_counters.pongsProcessed;
    snap.pongsMatched = g_counters.pongsMatched;
    snap.pongsUnmatched = g_counters.pongsUnmatched;
    snap.peerDisconnects = g_counters.peerDisconnects;
    snap.pingQueueToSendCount = g_counters.pingQueueToSendCount;
    snap.pingQueueToSendSumUs = g_counters.pingQueueToSendSumUs;
    snap.pingQueueToSendMaxUs = g_counters.pingQueueToSendMaxUs;
    snap.pongFrameToProcessCount = g_counters.pongFrameToProcessCount;
    snap.pongFrameToProcessSumUs = g_counters.pongFrameToProcessSumUs;
    snap.pongFrameToProcessMaxUs = g_counters.pongFrameToProcessMaxUs;
    return snap;
}
