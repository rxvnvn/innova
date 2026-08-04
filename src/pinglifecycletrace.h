// Copyright (c) 2026 The Innova developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef INNOVA_PINGLIFECYCLETRACE_H
#define INNOVA_PINGLIFECYCLETRACE_H

#include <stdint.h>
#include <vector>

class CNode;

// Diagnostic-only ping/pong lifecycle tracing, enabled with
// -pinglifecycletrace=1 (default 0).  All hooks are pure observation: they
// never modify scheduler, timeouts, ownership, or peer selection, and they
// emit additive PING_LIFECYCLE lines through the existing debug logger (no
// file I/O in the hot path).  The goal is to attribute the pingtime/pingwait
// gap observed during IBD to one of four models:
//   A remote/network delay   (ping left quickly, pong frame late)
//   B local recv starvation  (pong frame complete, ProcessMessage late)
//   C local send starvation  (ping queued, but stuck in the send buffer)
//   D accounting bug         (pong processed, outstanding state not cleared)

bool InitPingLifecycleTrace(bool fEnabled);
bool PingLifecycleTraceEnabled();

// A ping was about to replace an already-outstanding ping (nonce overwritten).
void PingLifecycleTraceReplaced(CNode* pnode, uint64_t nOldNonce);
// A ping was created in SendMessages (nPingUsecStart just stamped).
void PingLifecycleTraceScheduled(CNode* pnode, uint64_t nonce);
// The ping message has been appended to vSendMsg (entered the send buffer).
void PingLifecycleTraceQueuedSend(CNode* pnode, uint64_t nonce);
// The first byte of the ping message was written by send().
void PingLifecycleTraceSocketWriteBegin(CNode* pnode);
// The whole ping message has been flushed from vSendMsg.
void PingLifecycleTraceSocketWriteComplete(CNode* pnode);
// The full "pong" frame arrived and was timestamped in ReceiveMsgBytes.
void PingLifecycleTracePongFrameComplete(CNode* pnode);
// ProcessMessages is about to dispatch the "pong" message.
// nFrameCompleteUs is the message receipt time (msg.nTime).
void PingLifecycleTracePongProcessBegin(CNode* pnode, int64_t nFrameCompleteUs);
// ProcessMessage("pong") classified the reply.
void PingLifecycleTracePongResult(CNode* pnode, uint64_t nonce, int nResult);
// The CNode is being destroyed (peer closed).
void PingLifecycleTracePeerClosed(CNode* pnode);

// One-second aggregate emitted from ThreadMessageHandler.
void PingLifecycleTraceEmit1s(const std::vector<CNode*>& vNodesCopy);

enum PingLifecyclePongResult
{
    PONG_RESULT_MATCHED = 0,
    PONG_RESULT_WRONG_NONCE,
    PONG_RESULT_NONCE_ZERO,
    PONG_RESULT_UNSOLICITED,
    PONG_RESULT_SHORT_PAYLOAD
};

// Test-only accessors used by pinglifecycle_tests to verify the recorded
// semantics (counters only; production paths are exercised unmodified).
struct PingLifecycleCountersSnapshot
{
    uint64_t pingsScheduled;
    uint64_t pingsReplaced;
    uint64_t pingsQueued;
    uint64_t pingsSent;
    uint64_t pongsFrameComplete;
    uint64_t pongsProcessed;
    uint64_t pongsMatched;
    uint64_t pongsUnmatched;
    uint64_t peerDisconnects;
    uint64_t pingQueueToSendCount;
    uint64_t pingQueueToSendSumUs;
    uint64_t pingQueueToSendMaxUs;
    uint64_t pongFrameToProcessCount;
    uint64_t pongFrameToProcessSumUs;
    uint64_t pongFrameToProcessMaxUs;
};
PingLifecycleCountersSnapshot PingLifecycleCountersForTesting();

#endif // INNOVA_PINGLIFECYCLETRACE_H
