// Copyright (c) 2026 The Innova developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef INNOVA_IBDBLOCKLATENCY_H
#define INNOVA_IBDBLOCKLATENCY_H

#include <stdint.h>

#include <string>
#include <vector>

#include "uint256.h"

// Runtime-only per-block GETDATA -> CONNECT latency decomposition.
//
// Disabled by default (-ibdblocklatency=0).  When enabled it records, per
// sampled block, wall-clock microseconds at every stage of the request
// lifecycle and correlates them with the requesting peer, receiving peer,
// block size, connected height, peer ping (RTT) when available, and the
// global inflight/queued/deferred pressure at receipt:
//
//   T0 AskFor enqueued
//   T1 getdata dispatched
//   T2 block message fully received on the wire (msg.nTime)
//   T3 ProcessBlock entered
//   T4 AcceptBlock entered
//   T5 AddToBlockIndex entered
//   T6 SetBestChain entered
//   T7 block connected
//
// Intervals emitted: T1-T0, T2-T1, T3-T2, T4-T3, T5-T4, T6-T5, T7-T6, TOTAL.
// Aggregated as mean/median/p95/max over a bounded ring of connected-active
// samples, printed once per second (IBD_BLOCKLAT_1S).  With
// -ibdblocklatencycsv=<path> the CSV is opened once at Init with fully
// buffered stdio (setvbuf, no per-row fflush) and each terminal row is
// streamed immediately as its outcome is recorded, so no rows are ever
// dropped; the file is flushed and closed at Shutdown.
//
// This module is instrumentation only: it changes no scheduling, ownership,
// diversification, limits, sleeps, timeouts, or IBD behavior.  All hooks are
// no-ops when disabled.
//
// Threading: every hook is called from the message-handler thread (plus the
// socket-handler thread for the timeout/disconnect terminal hooks).  The
// record map and aggregation ring are guarded by a leaf lock that is never
// held while any other lock is acquired.

namespace ibdblocklatency {

enum
{
    BLOCKLAT_INTERVAL_ASKFOR_TO_GETDATA = 0,
    BLOCKLAT_INTERVAL_GETDATA_TO_RECEIVE,
    BLOCKLAT_INTERVAL_RECEIVE_TO_PROCESS,
    BLOCKLAT_INTERVAL_PROCESS_TO_ACCEPT,
    BLOCKLAT_INTERVAL_ACCEPT_TO_INDEX,
    BLOCKLAT_INTERVAL_INDEX_TO_BEST,
    BLOCKLAT_INTERVAL_BEST_TO_CONNECT,
    BLOCKLAT_INTERVAL_TOTAL,
    BLOCKLAT_NUM_INTERVALS
};

// Terminal fates of a tracked request.  orphan/unsolicited are not terminal
// fates: an orphaned block can still connect later (marked fOrphaned), and an
// unsolicited receive is captured by requestPeer == -1.  Terminal rows carry
// one of the *_TERMINAL outcomes below; the per-outcome lifetime counters
// additionally track orphaned/unsolicited events.
enum
{
    OUTCOME_CONNECTED_ACTIVE = 0,  // block became the active tip (T7)
    OUTCOME_ACCEPTED_SIDE,         // added to index but lower trust, never connected
    OUTCOME_ALREADY_HAVE,          // duplicate in index/orphan table
    OUTCOME_REJECTED,              // validation/limit reject
    OUTCOME_TIMEOUT,               // inflight request expired without delivery
    OUTCOME_DISCONNECT,            // peer dropped while request outstanding
    OUTCOME_INCOMPLETE_EVICTED,    // stale/never-completed record reclaimed
    OUTCOME_COUNT
};

const char* OutcomeName(int outcome);

struct BlockLatencySample
{
    uint256 hash;
    int outcome;          // OUTCOME_*
    int fOrphaned;        // 1 = spent time in the orphan table before terminal
    int requestPeer;      // peer that asked (T0), -1 = unsolicited
    int receivePeer;      // peer that delivered (T2), -1 = never received
    int64_t blockSize;    // payload bytes at T2, 0 = never received
    int64_t height;       // connected height (T7), -1 = never connected
    int64_t pingMs;       // receive peer ping (RTT) at T2, -1 = unknown
    int64_t requestPeerPressure;  // request peer active pressure at T0
    int64_t globalInflight;       // global inflight at T2
    int64_t globalQueued;         // global queued at T2
    int64_t globalDeferred;       // global deferred at T2
    int64_t intervalUs[BLOCKLAT_NUM_INTERVALS];  // -1 = unavailable
};

void SetEnabled(bool fEnabled, const std::string& strCsvPath);
bool Enabled();

// Test-only accessors (no-ops when disabled, matching the enabled-gate of the
// production hooks).
void ResetForTesting();
size_t SampleCountForTesting();
const std::vector<BlockLatencySample>& SamplesForTesting();
void FlushCsvForTesting();
int64_t ReceivedForTesting();
int64_t UnsolicitedForTesting();
int64_t ProcessedForTesting();
int64_t ConnectedForTesting();
int64_t OrphanedForTesting();
int64_t OutcomeCountForTesting(int outcome);

// T0 / T1 / T2 / T3..T7 lifecycle hooks.
void RecordAskForEnqueue(const uint256& hash, int peer, int64_t peerPressure);
void RecordGetDataSent(const uint256& hash, int peer);
void RecordBlockReceived(const uint256& hash, int receivePeer,
                         int64_t nBlockSize, int64_t nTimeReceivedUs,
                         int64_t nPingUsec);
void RecordProcessBlockBegin(const uint256& hash);
void RecordAcceptBlockBegin(const uint256& hash);
void RecordAddToBlockIndexBegin(const uint256& hash);
void RecordSetBestChainBegin(const uint256& hash);
void RecordBlockConnected(const uint256& hash, int64_t nConnectedHeight);

// Terminal hooks.  RecordBlockOrphaned is NOT terminal: it flags the record so
// a later connect/eviction reflects the orphan stage.  The others emit a
// terminal row (partial intervals with -1 for stages never reached) and drop
// the record.  All are no-ops when the hash has no tracked record.
void RecordBlockOrphaned(const uint256& hash);
void RecordBlockAcceptedSide(const uint256& hash, int64_t nBlockHeight);
void RecordBlockTerminal(const uint256& hash, int outcome,
                         int64_t nHeight = -1);

// One aggregated IBD_BLOCKLAT_1S line per second.  Called from the
// message-handler thread.
void EmitIBDBlockLatency1s();

// Shutdown dump: per-block CSV (when a path was configured) plus an
// aggregated latency summary to stdout.
void Dump();

} // namespace ibdblocklatency

#endif // INNOVA_IBDBLOCKLATENCY_H
