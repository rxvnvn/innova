// Copyright (c) 2026 The Innova developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "ibdforensic.h"

#include <errno.h>
#include <stdio.h>

#include <algorithm>
#include <cstring>
#include <deque>

#include "sync.h"
#include "util.h"
#include "net.h"

// See ibdforensic.h for the contract.  Implementation notes:
//   - g_mutex guards every piece of shared state below.  It is a leaf lock:
//     it is never held while acquiring any other lock, so it can be taken
//     from inside cs_main / cs_vSend / cs_vNodes without creating a cycle.
//   - When disabled, every record function returns before touching g_mutex
//     or any container, so disabled runs have no allocation and no locking.
//   - A hash is recorded exactly once (its first request).  A re-request is
//     only possible after the scheduler released ownership (timeout, receive,
//     disconnect, or owner conflict), so seeing a hash twice is always a
//     re-request signal, not a duplicate record.

namespace ibdforensic {

namespace {

volatile bool g_enabled = false;
std::string g_path;
CCriticalSection g_mutex;

std::vector<BatchRecord> g_batches;
std::map<uint256, BatchEntry> g_entries;
uint64_t g_nextBatchId = 0;
uint64_t g_nextGenId = 0;
uint64_t g_unsolicitedReceipts = 0;
GetBlocksRateCounters g_rate;

// Per-hash generation ledger.  Kept separate from g_entries because a
// generation opens in MarkBlockInFlight, which for the trailing getdata batch
// (and the headers-direct path) runs before RecordGetDataBatch creates the
// canonical BatchEntry.  A hash may have several generations in open order.
struct HashGenerations
{
    std::vector<GenerationRecord> gens;
};

std::map<uint256, HashGenerations> g_generations;

// Per-peer genuine delivery progress: peer -> wall-clock framing-complete time
// of its most recent requested block receipt.  Read-only observation state; the
// scheduler does not consult it (it becomes the input to experiment A later).
std::map<int, int64_t> g_peerLastReceiveUs;

// Batch ids recorded by RecordGetDataBatch but not yet stamped with a first
// socket send.  Both this queue and the peer's getdata send queue are FIFO and
// were produced in the same order, so the oldest un-stamped batch corresponds
// to the first "getdata" message that actually starts sending.
std::deque<uint64_t> g_unstampedBatches;

int SeqBucket(uint32_t nHashes, uint32_t seq)
{
    if (nHashes <= 1)
        return 0;
    int idx = (int)((double)seq * 10.0 / (double)(nHashes - 1));
    if (idx > 9)
        idx = 9;
    return idx;
}

// A response is truncated at the getblocks 1000-block limit (and therefore
// has a continuation marker) only when the announced batch is exactly the
// server-side cap.  This is the downloader-side proxy for the remote's
// hashContinue marker (which the server stores on its CNode for us).
bool HasContinuationMarker(int nExpectedBatchSize)
{
    return nExpectedBatchSize >= 1000;
}

// All helpers below require g_mutex.  g_generations maps hash -> generations;
// an entry is always looked up via the map's operator[] so a generation can be
// opened before the canonical BatchEntry exists.

// Close the active generation for *hash* as *pszReason* and return its gen id
// (0 when none is active).
uint64_t CloseActiveGenerationLocked(const uint256& hash, int64_t nNowUs,
                                     const char* pszReason)
{
    std::vector<GenerationRecord>& gens = g_generations[hash].gens;
    if (gens.empty())
        return 0;
    GenerationRecord& last = gens.back();
    if (last.releaseUs != 0)
        return 0;
    last.releaseUs = nNowUs;
    last.reason = pszReason;
    return last.genId;
}

// The generation that delivered a receipt at *nDispatchUs*: the closed
// generation whose [mark, release] window contains the dispatch time, else the
// most recent generation.  Deterministic: iterated newest-to-oldest so the
// window closest to the receipt wins; an always-active ledger falls back to the
// most recent generation.
uint64_t GenerationAtOrAfterLocked(const std::vector<GenerationRecord>& gens,
                                   int64_t nDispatchUs)
{
    if (gens.empty())
        return 0;
    for (size_t i = gens.size(); i-- > 0;)
    {
        const GenerationRecord& g = gens[i];
        if (g.releaseUs != 0 && g.markUs <= nDispatchUs &&
            nDispatchUs <= g.releaseUs)
            return g.genId;
    }
    return gens.back().genId;
}

// The mark time of the generation that delivered the receipt (see
// genIdAtReceipt), falling back to the canonical mark when no generation is
// attributed (legacy dumps / hashes recorded before the ledger existed).
int64_t DeliveringMarkUsLocked(const BatchEntry& e)
{
    if (e.genIdAtReceipt == 0)
        return e.markTimeUs;
    std::map<uint256, HashGenerations>::const_iterator gi =
        g_generations.find(e.hash);
    if (gi == g_generations.end())
        return e.markTimeUs;
    const std::vector<GenerationRecord>& gens = gi->second.gens;
    for (size_t i = 0; i < gens.size(); ++i)
        if (gens[i].genId == e.genIdAtReceipt)
            return gens[i].markUs;
    return e.markTimeUs;
}

// The generation shown on a legacy per-hash row: the delivering generation when
// known, else the most recent generation for the hash.
const GenerationRecord* LegacyRowGenerationLocked(const BatchEntry& e)
{
    std::map<uint256, HashGenerations>::const_iterator gi =
        g_generations.find(e.hash);
    if (gi == g_generations.end() || gi->second.gens.empty())
        return NULL;
    const std::vector<GenerationRecord>& gens = gi->second.gens;
    if (e.genIdAtReceipt != 0)
    {
        for (size_t i = 0; i < gens.size(); ++i)
            if (gens[i].genId == e.genIdAtReceipt)
                return &gens[i];
    }
    return &gens.back();
}

} // namespace

void SetEnabled(bool fEnabled, const std::string& strPath)
{
    LOCK(g_mutex);
    g_enabled = fEnabled;
    g_path = strPath;

    if (!fEnabled || strPath.empty())
        return;

    // Fail fast: create/truncate the output file now so a bad path or missing
    // permission surfaces at startup (debug.log / stderr) instead of silently
    // at shutdown, and so the file exists even if zero events are recorded.
    // Dump() re-opens it at shutdown and rewrites it.
    FILE* f = fopen(strPath.c_str(), "w");
    if (f == NULL)
    {
        const std::string msg = strprintf(
            "IBDFORENSIC: cannot create dump file '%s': %s (will retry at "
            "shutdown)",
            strPath.c_str(), strerror(errno));
        LogPrintf("%s\n", msg);
        fprintf(stderr, "%s\n", msg.c_str());
    }
    else
    {
        fclose(f);
    }
}

bool IsEnabled()
{
    return g_enabled;
}

void ResetForTesting()
{
    LOCK(g_mutex);
    g_batches.clear();
    g_entries.clear();
    g_unsolicitedReceipts = 0;
    g_rate = GetBlocksRateCounters();
    g_nextBatchId = 0;
    g_nextGenId = 0;
    g_peerLastReceiveUs.clear();
    g_unstampedBatches.clear();
    g_generations.clear();
    g_path.clear();
    g_enabled = true;
}

void RecordGetDataBatch(int peer, const std::vector<uint256>& vBlockHashes,
                        int64_t nNowUs, size_t nSendBufferBytes,
                        const uint256& hashLastBlockInBatch,
                        int nExpectedBatchSize)
{
    if (!g_enabled)
        return;
    if (vBlockHashes.empty())
        return;

    const bool fContinuation = HasContinuationMarker(nExpectedBatchSize);

    LOCK(g_mutex);
    BatchRecord rec;
    rec.peer = peer;
    rec.batchId = g_nextBatchId++;
    rec.sendTimeUs = nNowUs;
    rec.sendBufferBytes = nSendBufferBytes;
    rec.nHashes = (uint32_t)vBlockHashes.size();
    rec.hashes = vBlockHashes;
    rec.firstSocketSendUs = 0;
    rec.nsendFirstSend = 0;

    // The getdata message that carries this batch is enqueued right after this
    // call (PushMessage), in the same FIFO order as every other batch.  The
    // first "getdata" first-send stamps the oldest un-stamped batch.
    g_unstampedBatches.push_back(rec.batchId);

    for (uint32_t seq = 0; seq < rec.nHashes; ++seq)
    {
        const uint256& hash = vBlockHashes[seq];
        rec.hashes[seq] = hash;

        std::map<uint256, BatchEntry>::iterator it = g_entries.find(hash);
        if (it == g_entries.end())
        {
            BatchEntry e;
            e.hash = hash;
            e.batchId = rec.batchId;
            e.seq = seq;
            e.wasHashContinue =
                fContinuation && (hash == hashLastBlockInBatch);
            e.markTimeUs = nNowUs;
            e.requestPeer = peer;
            g_entries.insert(std::make_pair(hash, e));
        }
        else
        {
            // Re-request of a released hash.  Keep the canonical (first)
            // entry; record only the first re-request details.
            BatchEntry& e = it->second;
            e.reRequested = true;
            if (e.reRequestPeer == -1)
            {
                e.reRequestTimeUs = nNowUs;
                e.reRequestPeer = peer;
                if (e.requestPeer != peer)
                    e.reRequestedOtherPeer = true;
            }
        }
    }

    g_batches.push_back(rec);
}

void RecordReceived(int peer, const uint256& hash, int64_t nDispatchUs,
                    int64_t nFramingCompleteUs)
{
    if (!g_enabled)
        return;
    LOCK(g_mutex);
    std::map<uint256, BatchEntry>::iterator it = g_entries.find(hash);
    if (it == g_entries.end())
    {
        ++g_unsolicitedReceipts;
        return;
    }
    BatchEntry& e = it->second;
    if (e.recvTimeUs != 0)
        return;
    e.recvTimeUs = nDispatchUs;
    e.recvFramingCompleteUs = nFramingCompleteUs;
    if (e.timeoutTimeUs != 0 && nDispatchUs >= e.timeoutTimeUs)
        e.receivedAfterTimeout = true;

    // Delivery progress for the peer, measured before this receipt so it stays
    // "last genuine receive strictly before this release".
    std::map<int, int64_t>::const_iterator pi =
        g_peerLastReceiveUs.find(peer);
    const int64_t nProgressBefore =
        pi != g_peerLastReceiveUs.end() ? pi->second : 0;

    // Receipt attribution: close the active generation as "receive"; otherwise
    // use the generation whose [mark, release] window contains the dispatch
    // time, else the most recent generation.
    const uint64_t genId = CloseActiveGenerationLocked(hash, nDispatchUs,
                                                       "receive");
    if (genId != 0)
    {
        e.genIdAtReceipt = genId;
        e.progressLastUs = nProgressBefore;
    }
    else
    {
        std::map<uint256, HashGenerations>::const_iterator gi =
            g_generations.find(hash);
        if (gi != g_generations.end())
            e.genIdAtReceipt =
                GenerationAtOrAfterLocked(gi->second.gens, nDispatchUs);
    }

    g_peerLastReceiveUs[peer] = nFramingCompleteUs;
}

void RecordGenerationStart(int peer, const uint256& hash, int64_t nNowUs,
                           int announcePeer, bool diversified)
{
    if (!g_enabled)
        return;
    LOCK(g_mutex);
    std::vector<GenerationRecord>& gens = g_generations[hash].gens;
    // The in-flight admission gate already prevents a second MarkBlockInFlight
    // while the hash is in flight; this guard keeps the ledger consistent even
    // if a caller bypasses it.
    if (!gens.empty() && gens.back().releaseUs == 0)
        return;
    GenerationRecord g;
    g.genId = ++g_nextGenId;
    g.peer = peer;
    g.markUs = nNowUs;
    g.releaseUs = 0;
    g.announcePeer = announcePeer;
    g.diversified = diversified;
    gens.push_back(g);
}

void RecordGenerationEnd(const uint256& hash, int64_t nNowUs,
                         const char* pszReason)
{
    if (!g_enabled)
        return;
    LOCK(g_mutex);
    std::map<uint256, HashGenerations>::iterator gi = g_generations.find(hash);
    if (gi == g_generations.end())
        return;
    std::vector<GenerationRecord>& gens = gi->second.gens;
    if (gens.empty())
        return;
    GenerationRecord& last = gens.back();
    if (last.releaseUs != 0)
        return; // idempotent: a hash can be released by more than one path
    last.releaseUs = nNowUs;
    last.reason = pszReason;

    // Snapshot the requesting peer's genuine progress at release (excludes any
    // receipt that closes this same generation, which is recorded afterwards on
    // the receive path).
    std::map<int, int64_t>::const_iterator pi =
        g_peerLastReceiveUs.find(last.peer);
    if (pi != g_peerLastReceiveUs.end())
    {
        std::map<uint256, BatchEntry>::iterator ei = g_entries.find(hash);
        if (ei != g_entries.end())
            ei->second.progressLastUs = pi->second;
    }
}

void RecordSocketSend(const char* pszCommand, int64_t nNowUs,
                      size_t nSendSizeAtFirstSend)
{
    if (!g_enabled)
        return;
    if (pszCommand == NULL || strcmp(pszCommand, "getdata") != 0)
        return;
    LOCK(g_mutex);
    if (g_unstampedBatches.empty())
        return;
    const uint64_t batchId = g_unstampedBatches.front();
    g_unstampedBatches.pop_front();
    if (batchId >= g_batches.size())
        return; // FIFO invariant violated; leave the batch unstamped
    BatchRecord& b = g_batches[batchId];
    b.firstSocketSendUs = nNowUs;
    b.nsendFirstSend = nSendSizeAtFirstSend;
}

void RecordExpired(int peer, const uint256& hash, int64_t nNowUs,
                   int64_t nHeadAgeUs)
{
    if (!g_enabled)
        return;
    LOCK(g_mutex);
    std::map<uint256, BatchEntry>::iterator it = g_entries.find(hash);
    if (it == g_entries.end())
        return;
    BatchEntry& e = it->second;
    if (e.timeoutTimeUs == 0)
    {
        e.timeoutTimeUs = nNowUs;
        e.headAgeAtExpiryUs = nHeadAgeUs;
    }
}

void CountGetBlocksRateLimitInbound()
{
    if (!g_enabled)
        return;
    LOCK(g_mutex);
    ++g_rate.inboundRateLimited;
}

void CountGetBlocksRateLimitOutboundDedup()
{
    if (!g_enabled)
        return;
    LOCK(g_mutex);
    ++g_rate.outboundDedupSkipped;
}

void CountGetBlocksRateLimitOutboundWakeCooldown()
{
    if (!g_enabled)
        return;
    LOCK(g_mutex);
    ++g_rate.outboundWakeCooldown;
}

void CountGetBlocksOutstandingNoResponse(uint64_t nOutstanding)
{
    if (!g_enabled)
        return;
    LOCK(g_mutex);
    g_rate.outstandingNoResponse += nOutstanding;
}

std::string FormatSummary()
{
    struct Bucket
    {
        uint64_t count;
        uint64_t timeoutCount;
        int64_t sumUs;
        int64_t minUs;
        int64_t maxUs;
        std::vector<int64_t> lat;
        Bucket() : count(0), timeoutCount(0), sumUs(0),
                   minUs(INT64_MAX), maxUs(0) {}
    };
    Bucket buckets[10];

    uint64_t nClean = 0;

    uint64_t nTimeouts = 0;
    uint64_t nNeverReceived = 0;
    uint64_t nAfterTimeout = 0;
    uint64_t nAfterTimeoutReRequested = 0;
    uint64_t nAfterTimeoutReRequestedOther = 0;
    uint64_t nAfterTimeoutReRequestedBeforeRecv = 0;

    uint64_t nHashContinue = 0;
    uint64_t nHashContinueTimedOut = 0;
    uint64_t nHashContinueAfterTimeout = 0;
    uint64_t nHashContinueNeverReceived = 0;

    uint64_t nBatchesWithHashContinue = 0;
    uint64_t nBatchesWithHashContinueTimedOut = 0;

    uint64_t nGenerationsTotal = 0;
    uint64_t nGenerationsActive = 0;
    uint64_t nGenReasons[5] = {0, 0, 0, 0, 0};
    // index: 0 receive, 1 timeout, 2 queue-removal, 3 clear, 4 disconnect

    {
        LOCK(g_mutex);

        for (std::map<uint256, BatchEntry>::const_iterator it =
                 g_entries.begin();
             it != g_entries.end(); ++it)
        {
            const BatchEntry& e = it->second;
            const uint64_t nHashes = e.batchId < g_batches.size()
                                         ? g_batches[e.batchId].nHashes
                                         : 0;
            const int idx = SeqBucket((uint32_t)nHashes, e.seq);

            if (e.timeoutTimeUs != 0)
            {
                ++nTimeouts;
                ++buckets[idx].timeoutCount;
                if (e.recvTimeUs == 0)
                    ++nNeverReceived;
            }

            if (e.recvTimeUs != 0 && e.timeoutTimeUs == 0)
            {
                // Clean arrival: no timeout involved, useful for latency-by-
                // position analysis.  Latency is measured against the mark of
                // the generation that delivered the block (so a re-requested
                // hash is charged to its delivering request, not the first).
                const int64_t lat =
                    e.recvTimeUs - DeliveringMarkUsLocked(e);
                ++nClean;
                ++buckets[idx].count;
                buckets[idx].sumUs += lat;
                buckets[idx].lat.push_back(lat);
                if (lat < buckets[idx].minUs)
                    buckets[idx].minUs = lat;
                if (lat > buckets[idx].maxUs)
                    buckets[idx].maxUs = lat;
            }

            if (e.receivedAfterTimeout)
            {
                ++nAfterTimeout;
                if (e.reRequested)
                    ++nAfterTimeoutReRequested;
                if (e.reRequestedOtherPeer)
                    ++nAfterTimeoutReRequestedOther;
                if (e.reRequested && e.reRequestTimeUs != 0 &&
                    e.reRequestTimeUs <= e.recvTimeUs)
                    ++nAfterTimeoutReRequestedBeforeRecv;
            }

            if (e.wasHashContinue)
            {
                ++nHashContinue;
                if (e.timeoutTimeUs != 0)
                {
                    ++nHashContinueTimedOut;
                    if (e.recvTimeUs == 0)
                        ++nHashContinueNeverReceived;
                    else if (e.receivedAfterTimeout)
                        ++nHashContinueAfterTimeout;
                }
            }
        }

        for (std::vector<BatchRecord>::const_iterator it = g_batches.begin();
             it != g_batches.end(); ++it)
        {
            for (std::vector<uint256>::const_iterator h = it->hashes.begin();
                 h != it->hashes.end(); ++h)
            {
                std::map<uint256, BatchEntry>::const_iterator e =
                    g_entries.find(*h);
                if (e != g_entries.end() && e->second.wasHashContinue)
                {
                    ++nBatchesWithHashContinue;
                    if (e->second.timeoutTimeUs != 0)
                        ++nBatchesWithHashContinueTimedOut;
                    break;
                }
            }
        }

        for (std::map<uint256, HashGenerations>::const_iterator gi =
                 g_generations.begin();
             gi != g_generations.end(); ++gi)
        {
            const std::vector<GenerationRecord>& gens = gi->second.gens;
            nGenerationsTotal += (uint64_t)gens.size();
            for (size_t i = 0; i < gens.size(); ++i)
            {
                const GenerationRecord& g = gens[i];
                if (g.releaseUs == 0)
                {
                    ++nGenerationsActive;
                    continue;
                }
                if (g.reason == "receive")
                    ++nGenReasons[0];
                else if (g.reason == "timeout")
                    ++nGenReasons[1];
                else if (g.reason == "queue-removal")
                    ++nGenReasons[2];
                else if (g.reason == "clear")
                    ++nGenReasons[3];
                else if (g.reason == "disconnect")
                    ++nGenReasons[4];
            }
        }
    }

    // Real least-squares slope (computed over clean arrivals only, using
    // absolute seq as x).  Recompute inside the lock-free section over the
    // canonical entries' clean latencies.
    long double sx = 0, sy = 0, sxx = 0, sxy = 0;
    long double n = 0;
    {
        LOCK(g_mutex);
        for (std::map<uint256, BatchEntry>::const_iterator it =
                 g_entries.begin();
             it != g_entries.end(); ++it)
        {
            const BatchEntry& e = it->second;
            if (e.recvTimeUs != 0 && e.timeoutTimeUs == 0)
            {
                const long double x = (long double)e.seq;
                const long double y =
                    (long double)(e.recvTimeUs - DeliveringMarkUsLocked(e));
                sx += x;
                sy += y;
                sxx += x * x;
                sxy += x * y;
                n += 1;
            }
        }
    }
    long double slope = 0;
    if (n > 1 && n * sxx - sx * sx != 0)
        slope = (n * sxy - sx * sy) / (n * sxx - sx * sx);

    std::string out;
    char buf[256];
    out += "IBDFORENSIC SUMMARY\n";
    snprintf(buf, sizeof(buf),
             "batches=%llu canonical_entries=%llu unsolicited_receipts=%llu "
             "clean_arrivals=%llu\n",
             (unsigned long long)g_batches.size(),
             (unsigned long long)g_entries.size(),
             (unsigned long long)g_unsolicitedReceipts,
             (unsigned long long)nClean);
    out += buf;

    out += "latency_by_position (clean arrivals, no timeout): bucket "
           "[0=head..9=tail] count mean_us p50_us p95_us max_us\n";
    for (int i = 0; i < 10; ++i)
    {
        const Bucket& b = buckets[i];
        if (b.count == 0 && b.timeoutCount == 0)
            continue;
        int64_t meanUs = 0, p50 = 0, p95 = 0, maxUs = 0;
        if (b.count > 0)
        {
            std::vector<int64_t> lat = b.lat;
            std::sort(lat.begin(), lat.end());
            meanUs = b.sumUs / (int64_t)b.count;
            p50 = lat[lat.size() / 2];
            size_t idx95 = (size_t)(lat.size() * 0.95);
            if (idx95 >= lat.size())
                idx95 = lat.size() - 1;
            p95 = lat[idx95];
            maxUs = b.maxUs;
        }
        snprintf(buf, sizeof(buf),
                 "  bucket=%d count=%llu mean_us=%lld p50_us=%lld p95_us=%lld "
                 "max_us=%lld timeouts_in_bucket=%llu\n",
                 i, (unsigned long long)b.count, (long long)meanUs,
                 (long long)p50, (long long)p95, (long long)maxUs,
                 (unsigned long long)b.timeoutCount);
        out += buf;
    }
    snprintf(buf, sizeof(buf),
             "latency_slope_us_per_seq=%f (least squares, clean arrivals)\n",
             (double)slope);
    out += buf;

    snprintf(buf, sizeof(buf),
             "timeouts_total=%llu never_received_total=%llu\n",
             (unsigned long long)nTimeouts,
             (unsigned long long)nNeverReceived);
    out += buf;

    snprintf(buf, sizeof(buf),
             "generations_total=%llu generations_active=%llu "
             "closed_receive=%llu closed_timeout=%llu "
             "closed_queue_removal=%llu closed_clear=%llu "
             "closed_disconnect=%llu\n",
             (unsigned long long)nGenerationsTotal,
             (unsigned long long)nGenerationsActive,
             (unsigned long long)nGenReasons[0],
             (unsigned long long)nGenReasons[1],
             (unsigned long long)nGenReasons[2],
             (unsigned long long)nGenReasons[3],
             (unsigned long long)nGenReasons[4]);
    out += buf;

    snprintf(buf, sizeof(buf),
             "received_after_timeout=%llu of_those_rerequested=%llu "
             "of_those_rerequested_other_peer=%llu "
             "of_those_rerequested_before_receipt=%llu\n",
             (unsigned long long)nAfterTimeout,
             (unsigned long long)nAfterTimeoutReRequested,
             (unsigned long long)nAfterTimeoutReRequestedOther,
             (unsigned long long)nAfterTimeoutReRequestedBeforeRecv);
    out += buf;

    snprintf(buf, sizeof(buf),
             "hashcontinue_entries=%llu timed_out=%llu "
             "received_after_timeout=%llu never_received=%llu\n",
             (unsigned long long)nHashContinue,
             (unsigned long long)nHashContinueTimedOut,
             (unsigned long long)nHashContinueAfterTimeout,
             (unsigned long long)nHashContinueNeverReceived);
    out += buf;

    snprintf(buf, sizeof(buf),
             "batches_with_hashcontinue=%llu "
             "batches_with_hashcontinue_timed_out=%llu\n",
             (unsigned long long)nBatchesWithHashContinue,
             (unsigned long long)nBatchesWithHashContinueTimedOut);
    out += buf;

    const GetBlocksRateCounters rate = RateCounters();
    snprintf(buf, sizeof(buf),
             "getblocks_rate_limited_inbound=%llu "
             "getblocks_outbound_dedup_skipped=%llu "
             "getblocks_outbound_wake_cooldown=%llu "
             "getblocks_outstanding_no_response=%llu\n",
             (unsigned long long)rate.inboundRateLimited,
             (unsigned long long)rate.outboundDedupSkipped,
             (unsigned long long)rate.outboundWakeCooldown,
             (unsigned long long)rate.outstandingNoResponse);
    out += buf;

    return out;
}

bool Dump()
{
    const std::string summary = FormatSummary();

    // Make the shutdown dump observable in debug.log (daemon stdout is /dev/null).
    LogPrintf("%s", summary.c_str());
    printf("%s", summary.c_str());

    if (g_path.empty())
        return true;

    FILE* f = fopen(g_path.c_str(), "w");
    if (f == NULL)
    {
        const std::string msg = strprintf(
            "IBDFORENSIC: could not open dump file '%s': %s",
            g_path.c_str(), strerror(errno));
        LogPrintf("%s\n", msg);
        fprintf(stderr, "%s\n", msg.c_str());
        return false;
    }

    fprintf(f, "%s\n", summary.c_str());
    fprintf(f,
            "# peer,batch_id,seq,n_hashes,hash,was_hashcontinue,"
            "mark_time_us,recv_time_us,timeout_time_us,"
            "received_after_timeout,rerequested,"
            "rerequested_other_peer,rerequest_peer,"
            "rerequest_time_us,send_buffer_bytes,generation_id,"
            "generation_mark_us,generation_release_us,"
            "generation_release_reason,enqueue_time_us,"
            "first_socket_send_us,nsend_first_send,progress_last_us,"
            "head_age_at_expiry_us,recv_framing_complete_us\n");
    {
        LOCK(g_mutex);
        for (std::map<uint256, BatchEntry>::const_iterator it =
                 g_entries.begin();
             it != g_entries.end(); ++it)
        {
            const BatchEntry& e = it->second;
            const GenerationRecord* pg = LegacyRowGenerationLocked(e);
            const uint64_t nGenId = pg != NULL ? pg->genId : 0;
            const int64_t nGenMark = pg != NULL ? pg->markUs : 0;
            const int64_t nGenRelease = pg != NULL ? pg->releaseUs : 0;
            const char* pszGenReason = pg != NULL ? pg->reason.c_str() : "";
            const BatchRecord* pb = e.batchId < g_batches.size()
                                        ? &g_batches[e.batchId]
                                        : NULL;
            fprintf(f, "%d,%llu,%u,%u,%s,%d,%lld,%lld,%lld,%d,%d,%d,%d,"
                       "%lld,%zu,%llu,%lld,%lld,%s,%lld,%lld,%zu,%lld,%lld,"
                       "%lld\n",
                    e.requestPeer, (unsigned long long)e.batchId, e.seq,
                    pb != NULL ? pb->nHashes : 0,
                    e.hash.ToString().c_str(), e.wasHashContinue ? 1 : 0,
                    (long long)e.markTimeUs, (long long)e.recvTimeUs,
                    (long long)e.timeoutTimeUs,
                    e.receivedAfterTimeout ? 1 : 0, e.reRequested ? 1 : 0,
                    e.reRequestedOtherPeer ? 1 : 0, e.reRequestPeer,
                    (long long)e.reRequestTimeUs,
                    pb != NULL ? pb->sendBufferBytes : (size_t)0,
                    (unsigned long long)nGenId, (long long)nGenMark,
                    (long long)nGenRelease, pszGenReason,
                    pb != NULL ? (long long)pb->sendTimeUs : (long long)0,
                    pb != NULL ? (long long)pb->firstSocketSendUs : (long long)0,
                    pb != NULL ? pb->nsendFirstSend : (size_t)0,
                    (long long)e.progressLastUs,
                    (long long)e.headAgeAtExpiryUs,
                    (long long)e.recvFramingCompleteUs);
        }
    }

    fprintf(f, "#generations\n");
    const bool fDiversificationAttribution =
        IsFutureSupplyDiversificationEnabled();
    if (fDiversificationAttribution)
        fprintf(f,
                "# generation_id,batch_id,hash,peer,mark_us,release_us,"
                "reason,announce_peer,diversified\n");
    else
        fprintf(f,
                "# generation_id,batch_id,hash,peer,mark_us,release_us,"
                "reason\n");
    {
        LOCK(g_mutex);
        for (std::map<uint256, HashGenerations>::const_iterator gi =
                 g_generations.begin();
             gi != g_generations.end(); ++gi)
        {
            const std::vector<GenerationRecord>& gens = gi->second.gens;
            std::map<uint256, BatchEntry>::const_iterator ei =
                g_entries.find(gi->first);
            const uint64_t nBatchId =
                ei != g_entries.end() ? ei->second.batchId : 0;
            for (size_t i = 0; i < gens.size(); ++i)
            {
                const GenerationRecord& g = gens[i];
                if (fDiversificationAttribution)
                {
                    fprintf(f, "%llu,%llu,%s,%d,%lld,%lld,%s,%d,%d\n",
                            (unsigned long long)g.genId,
                            (unsigned long long)nBatchId,
                            gi->first.ToString().c_str(), g.peer,
                            (long long)g.markUs, (long long)g.releaseUs,
                            g.reason.c_str(), g.announcePeer,
                            g.diversified ? 1 : 0);
                }
                else
                {
                    fprintf(f, "%llu,%llu,%s,%d,%lld,%lld,%s\n",
                            (unsigned long long)g.genId,
                            (unsigned long long)nBatchId,
                            gi->first.ToString().c_str(), g.peer,
                            (long long)g.markUs, (long long)g.releaseUs,
                            g.reason.c_str());
                }
            }
        }
    }

    if (fclose(f) != 0)
    {
        const std::string msg = strprintf(
            "IBDFORENSIC: could not flush/close dump file '%s': %s",
            g_path.c_str(), strerror(errno));
        LogPrintf("%s\n", msg);
        fprintf(stderr, "%s\n", msg.c_str());
        return false;
    }

    LogPrintf("IBDFORENSIC: wrote dump file '%s' (%u batches, %llu canonical "
              "entries)\n",
              g_path.c_str(), (unsigned)g_batches.size(),
              (unsigned long long)g_entries.size());
    return true;
}

size_t BatchCount()
{
    LOCK(g_mutex);
    return g_batches.size();
}

size_t GenerationCount()
{
    LOCK(g_mutex);
    size_t n = 0;
    for (std::map<uint256, HashGenerations>::const_iterator gi =
             g_generations.begin();
         gi != g_generations.end(); ++gi)
        n += gi->second.gens.size();
    return n;
}

size_t EntryCount()
{
    LOCK(g_mutex);
    return g_entries.size();
}

size_t UnsolicitedReceiptCount()
{
    LOCK(g_mutex);
    return g_unsolicitedReceipts;
}

GetBlocksRateCounters RateCounters()
{
    LOCK(g_mutex);
    return g_rate;
}

const std::vector<BatchRecord>& BatchesForTesting()
{
    LOCK(g_mutex);
    return g_batches;
}

const std::map<uint256, BatchEntry>& EntriesForTesting()
{
    LOCK(g_mutex);
    return g_entries;
}

// Flattened copy of the generation ledger for tests: hash -> generations in
// open order.  A snapshot, not a reference, so it stays valid after the lock
// is released.
std::map<uint256, std::vector<GenerationRecord> > GenerationsForTesting()
{
    LOCK(g_mutex);
    std::map<uint256, std::vector<GenerationRecord> > out;
    for (std::map<uint256, HashGenerations>::const_iterator gi =
             g_generations.begin();
         gi != g_generations.end(); ++gi)
        out[gi->first] = gi->second.gens;
    return out;
}

} // namespace ibdforensic
