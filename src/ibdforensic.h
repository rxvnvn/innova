// Copyright (c) 2026 The Innova developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef INNOVA_IBDFORENSIC_H
#define INNOVA_IBDFORENSIC_H

#include <stdint.h>

#include <map>
#include <string>
#include <vector>

#include "uint256.h"

// Passive per-getdata-batch instrumentation for the IBD block-request
// scheduler.
//
// PURPOSE
//   Record, without changing any scheduler decision, everything needed to
//   answer the following questions about a real IBD run:
//     - does request latency grow with the position of a hash inside a
//       getdata batch?
//     - is the in-flight-timeout tail located at the end of the batch?
//     - how many blocks arrive after their in-flight request already timed
//       out?
//     - of those, how many had already been re-requested (to another peer)?
//     - how much duplicate traffic does the timeout tail generate?
//     - does a timed-out hashContinue block delay the advancement of the
//       download batch?
//     - how many outstanding getblocks are dropped without a response /
//       suppressed by rate limiting?
//
// INVARIANT
//   This module is observation-only.  None of its functions influence the
//   scheduler: they take no state from it, return nothing to it, and the
//   in-flight/timeout/cap decisions are untouched.  When disabled (the
//   default) every record function returns before touching any shared state,
//   so normal (non-forensic) runs are bit-identical in behaviour.
//
// DATA MODEL
//   A "batch" is one getdata message that carries at least one block request.
//   Every requested block hash has exactly one canonical BatchEntry (keyed by
//   hash) recording the first request plus the whole lifecycle (mark, timeout,
//   receipt, re-request).  A later re-request of the same hash (only possible
//   after the scheduler released ownership, e.g. after a timeout) updates the
//   canonical entry instead of creating a second one; the batch still records
//   the hash so the re-request's position in the new batch remains visible.
//
// TIMEBASE
//   All timestamps are wall-clock microseconds from GetTimeMicros(), the same
//   clock family the scheduler's timeout (GetTime() seconds) uses, so
//   "received after timeout" comparisons are consistent.

namespace ibdforensic {

struct BatchEntry
{
    uint256 hash;
    uint64_t batchId;          // batch the hash was first requested in
    uint32_t seq;              // block-relative ordinal inside that batch
    bool wasHashContinue;      // hash == peer continuation marker at send time
    int64_t markTimeUs;        // getdata push time (when in flight marked)
    int64_t recvTimeUs;        // block message received (0 = never)
    int64_t timeoutTimeUs;     // in-flight request expired (0 = never)
    bool receivedAfterTimeout; // receipt happened after the timeout fired
    bool reRequested;          // hash marked in flight again after release
    bool reRequestedOtherPeer; // that re-request went to a different peer
    int reRequestPeer;         // peer of the first re-request (-1 = none)
    int64_t reRequestTimeUs;   // time of the first re-request (0 = none)
    int requestPeer;           // peer this batch was requested from

    BatchEntry()
        : batchId(0), seq(0), wasHashContinue(false), markTimeUs(0),
          recvTimeUs(0), timeoutTimeUs(0), receivedAfterTimeout(false),
          reRequested(false), reRequestedOtherPeer(false), reRequestPeer(-1),
          reRequestTimeUs(0), requestPeer(-1)
    {
    }
};

struct BatchRecord
{
    int peer;
    uint64_t batchId;
    int64_t sendTimeUs;        // getdata push time
    size_t sendBufferBytes;    // peer send buffer size at push time
    uint32_t nHashes;          // number of block hashes in the getdata message
    std::vector<uint256> hashes; // block hashes in wire order

    BatchRecord()
        : peer(-1), batchId(0), sendTimeUs(0), sendBufferBytes(0), nHashes(0)
    {
    }
};

struct GetBlocksRateCounters
{
    uint64_t inboundRateLimited;      // we rate-limited a peer's getblocks
    uint64_t outboundDedupSkipped;    // we skipped a getblocks send (5s dedup)
    uint64_t outboundWakeCooldown;    // wake-driven getblocks hit its cooldown
    uint64_t outstandingNoResponse;   // outstanding getblocks dropped w/o reply

    GetBlocksRateCounters()
        : inboundRateLimited(0), outboundDedupSkipped(0),
          outboundWakeCooldown(0), outstandingNoResponse(0)
    {
    }
};

// Enable/disable recording and choose the optional CSV dump path.
// Call once during startup, before any record function can be reached.
// When enabled with a non-empty path the file is created immediately (fail
// fast): a missing parent directory or missing permission surfaces right away
// via LogPrintf/stderr instead of silently at shutdown, and the file exists
// even if no event is ever recorded.  Disabling never erases already-recorded
// counters; Dump() still writes whatever was collected.
void SetEnabled(bool fEnabled, const std::string& strPath);
bool IsEnabled();

// Test support: clear all recorded state.
void ResetForTesting();

// Called once per getdata message that contains at least one block request,
// immediately before it is written to the wire.  vBlockHashes must contain
// only the block-request hashes of the message, in wire order.  The scheduler
// passes pre-filtered hashes so this module has no dependency on the inv-type
// enum.  nExpectedBatchSize is the announced size of the current getblocks
// batch (used to decide whether the batch was truncated at the 1000-block
// getblocks limit and therefore has a real continuation marker).
void RecordGetDataBatch(int peer, const std::vector<uint256>& vBlockHashes,
                        int64_t nNowUs, size_t nSendBufferBytes,
                        const uint256& hashLastBlockInBatch,
                        int nExpectedBatchSize);

// A block message arrived (called on the block receive path).
void RecordReceived(int peer, const uint256& hash, int64_t nNowUs);

// An in-flight request expired (called from ExpireBlockInFlight).
void RecordExpired(int peer, const uint256& hash, int64_t nNowUs);

// getblocks RATE_LIMIT / no-response events.
void CountGetBlocksRateLimitInbound();
void CountGetBlocksRateLimitOutboundDedup();
void CountGetBlocksRateLimitOutboundWakeCooldown();
void CountGetBlocksOutstandingNoResponse(uint64_t nOutstanding);

// Human-readable aggregate summary answering the questions in the file
// comment.  Does not depend on enabled state.
std::string FormatSummary();

// Write the per-batch/per-hash CSV dump to the configured path (no-op when no
// path was configured), then print the summary to stdout.  The summary is
// also written to debug.log so a daemon shutdown is observable there.
// Returns false (and logs the reason via LogPrintf/stderr) when the dump file
// could not be opened or written; the recorded data is kept regardless.
bool Dump();

// Test accessors.
size_t BatchCount();
size_t EntryCount();
size_t UnsolicitedReceiptCount();
GetBlocksRateCounters RateCounters();
const std::vector<BatchRecord>& BatchesForTesting();
const std::map<uint256, BatchEntry>& EntriesForTesting();

} // namespace ibdforensic

#endif // INNOVA_IBDFORENSIC_H
