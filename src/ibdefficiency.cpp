// Copyright (c) 2026 The Innova developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "ibdefficiency.h"
#include "main.h"
#include "sync.h"
#include "util.h"

#include <inttypes.h>
#include <stdio.h>

static bool fIBDEfficiencyTraceEnabled = false;
static IBDEfficiencyCounters ibdEfficiencyTotal;
static IBDEfficiencyCounters ibdEfficiencyDelta;
static int64_t nIBDEfficiencyLastSummary = 0;
static int nIBDEfficiencyLastHeight = 0;

IBDEfficiencyCounters::IBDEfficiencyCounters()
    : received_requested(0),
      received_unsolicited(0),
      received_unique(0),
      received_duplicate_indexed(0),
      received_duplicate_orphan(0),
      received_accepted_active(0),
      received_accepted_side(0),
      received_orphan_new(0),
      received_rejected(0),
      received_retry_recorded(0),
      bytes_requested(0),
      bytes_unsolicited(0),
      bytes_unique(0),
      bytes_duplicate(0),
      bytes_accepted_active(0),
      bytes_accepted_side(0),
      bytes_orphan_new(0),
      bytes_rejected(0)
{
}

void IBDEfficiencyCounters::RecordBlock(uint64_t nBlockSize,
                                        bool fRequested,
                                        bool fUnique,
                                        bool fDuplicateIndexed,
                                        bool fDuplicateOrphan,
                                        bool fAcceptedActive,
                                        bool fAcceptedSide,
                                        bool fOrphanNew,
                                        bool fRejected,
                                        bool fRetryRecorded)
{
    if (fRequested)
        received_requested.fetch_add(1, std::memory_order_relaxed);
    else
        received_unsolicited.fetch_add(1, std::memory_order_relaxed);

    if (fUnique)
        received_unique.fetch_add(1, std::memory_order_relaxed);
    else if (fDuplicateIndexed)
        received_duplicate_indexed.fetch_add(1, std::memory_order_relaxed);
    else if (fDuplicateOrphan)
        received_duplicate_orphan.fetch_add(1, std::memory_order_relaxed);

    if (fAcceptedActive)
        received_accepted_active.fetch_add(1, std::memory_order_relaxed);
    else if (fAcceptedSide)
        received_accepted_side.fetch_add(1, std::memory_order_relaxed);
    else if (fOrphanNew)
        received_orphan_new.fetch_add(1, std::memory_order_relaxed);
    else if (fRejected)
        received_rejected.fetch_add(1, std::memory_order_relaxed);

    if (fRetryRecorded)
        received_retry_recorded.fetch_add(1, std::memory_order_relaxed);

    if (fRequested)
        bytes_requested.fetch_add(nBlockSize, std::memory_order_relaxed);
    else
        bytes_unsolicited.fetch_add(nBlockSize, std::memory_order_relaxed);

    if (fUnique)
        bytes_unique.fetch_add(nBlockSize, std::memory_order_relaxed);
    else
        bytes_duplicate.fetch_add(nBlockSize, std::memory_order_relaxed);

    if (fAcceptedActive)
        bytes_accepted_active.fetch_add(nBlockSize, std::memory_order_relaxed);
    else if (fAcceptedSide)
        bytes_accepted_side.fetch_add(nBlockSize, std::memory_order_relaxed);
    else if (fOrphanNew)
        bytes_orphan_new.fetch_add(nBlockSize, std::memory_order_relaxed);
    else if (fRejected)
        bytes_rejected.fetch_add(nBlockSize, std::memory_order_relaxed);
}

static void PrintRate(const char* label, uint64_t count, int64_t nElapsedSec)
{
    if (nElapsedSec > 0)
        LogPrintf("IBDEFFICIENCY %s %" PRIu64 " (%.1f/s)\n",
               label, count, (double)count / nElapsedSec);
    else
        LogPrintf("IBDEFFICIENCY %s %" PRIu64 "\n", label, count);
}

static void PrintBytes(const char* label, uint64_t bytes, int64_t nElapsedSec)
{
    double mb = bytes / (1024.0 * 1024.0);
    if (nElapsedSec > 0)
        LogPrintf("IBDEFFICIENCY %s %.1fMB (%.1fMB/s)\n",
               label, mb, mb / nElapsedSec);
    else
        LogPrintf("IBDEFFICIENCY %s %.1fMB\n", label, mb);
}

void IBDEfficiencyCounters::PrintSummary(const char* pszLabel,
                                         int nHeightStart,
                                         int nHeightEnd,
                                         int64_t nElapsedSec) const
{
    uint64_t nRequested = received_requested.load(std::memory_order_relaxed);
    uint64_t nUnsolicited = received_unsolicited.load(std::memory_order_relaxed);
    uint64_t nTotal = nRequested + nUnsolicited;

    uint64_t nUnique = received_unique.load(std::memory_order_relaxed);
    uint64_t nDupIdx = received_duplicate_indexed.load(std::memory_order_relaxed);
    uint64_t nDupOrph = received_duplicate_orphan.load(std::memory_order_relaxed);

    uint64_t nAccActive = received_accepted_active.load(std::memory_order_relaxed);
    uint64_t nAccSide = received_accepted_side.load(std::memory_order_relaxed);
    uint64_t nOrphanNew = received_orphan_new.load(std::memory_order_relaxed);
    uint64_t nRejected = received_rejected.load(std::memory_order_relaxed);
    uint64_t nRetry = received_retry_recorded.load(std::memory_order_relaxed);

    int nHeightDelta = nHeightEnd - nHeightStart;

    LogPrintf("IBDEFFICIENCY %s heights=%d..%d (delta=%d) elapsed=%" PRId64 "s\n",
           pszLabel, nHeightStart, nHeightEnd, nHeightDelta, nElapsedSec);

    LogPrintf("IBDEFFICIENCY --- origin ---\n");
    PrintRate("blocks_requested", nRequested, nElapsedSec);
    PrintRate("blocks_unsolicited", nUnsolicited, nElapsedSec);
    LogPrintf("IBDEFFICIENCY requested_ratio %.1f%%\n",
           nTotal > 0 ? 100.0 * nRequested / nTotal : 0.0);
    PrintBytes("bytes_requested", bytes_requested.load(std::memory_order_relaxed), nElapsedSec);
    PrintBytes("bytes_unsolicited", bytes_unsolicited.load(std::memory_order_relaxed), nElapsedSec);

    LogPrintf("IBDEFFICIENCY --- novelty ---\n");
    PrintRate("blocks_unique", nUnique, nElapsedSec);
    PrintRate("blocks_duplicate_indexed", nDupIdx, nElapsedSec);
    PrintRate("blocks_duplicate_orphan", nDupOrph, nElapsedSec);
    LogPrintf("IBDEFFICIENCY unique_ratio %.1f%%\n",
           nTotal > 0 ? 100.0 * nUnique / nTotal : 0.0);
    PrintBytes("bytes_unique", bytes_unique.load(std::memory_order_relaxed), nElapsedSec);
    PrintBytes("bytes_duplicate", bytes_duplicate.load(std::memory_order_relaxed), nElapsedSec);

    LogPrintf("IBDEFFICIENCY --- outcome ---\n");
    PrintRate("blocks_accepted_active", nAccActive, nElapsedSec);
    PrintRate("blocks_accepted_side", nAccSide, nElapsedSec);
    PrintRate("blocks_orphan_new", nOrphanNew, nElapsedSec);
    PrintRate("blocks_rejected", nRejected, nElapsedSec);
    PrintRate("blocks_retry_recorded", nRetry, nElapsedSec);
    uint64_t nAccepted = nAccActive + nAccSide;
    LogPrintf("IBDEFFICIENCY active_ratio %.1f%% (of accepted)\n",
           nAccepted > 0 ? 100.0 * nAccActive / nAccepted : 0.0);
    LogPrintf("IBDEFFICIENCY efficiency %.1f%% (accepted_active / total)\n",
           nTotal > 0 ? 100.0 * nAccActive / nTotal : 0.0);
    LogPrintf("IBDEFFICIENCY blocks_per_height %.1f\n",
           nHeightDelta > 0 ? (double)nTotal / nHeightDelta : 0.0);

    PrintBytes("bytes_accepted_active", bytes_accepted_active.load(std::memory_order_relaxed), nElapsedSec);
    PrintBytes("bytes_accepted_side", bytes_accepted_side.load(std::memory_order_relaxed), nElapsedSec);
    PrintBytes("bytes_orphan_new", bytes_orphan_new.load(std::memory_order_relaxed), nElapsedSec);
    PrintBytes("bytes_rejected", bytes_rejected.load(std::memory_order_relaxed), nElapsedSec);
}

void IBDEfficiencyCounters::ResetDelta()
{
    received_requested.store(0, std::memory_order_relaxed);
    received_unsolicited.store(0, std::memory_order_relaxed);
    received_unique.store(0, std::memory_order_relaxed);
    received_duplicate_indexed.store(0, std::memory_order_relaxed);
    received_duplicate_orphan.store(0, std::memory_order_relaxed);
    received_accepted_active.store(0, std::memory_order_relaxed);
    received_accepted_side.store(0, std::memory_order_relaxed);
    received_orphan_new.store(0, std::memory_order_relaxed);
    received_rejected.store(0, std::memory_order_relaxed);
    received_retry_recorded.store(0, std::memory_order_relaxed);
    bytes_requested.store(0, std::memory_order_relaxed);
    bytes_unsolicited.store(0, std::memory_order_relaxed);
    bytes_unique.store(0, std::memory_order_relaxed);
    bytes_duplicate.store(0, std::memory_order_relaxed);
    bytes_accepted_active.store(0, std::memory_order_relaxed);
    bytes_accepted_side.store(0, std::memory_order_relaxed);
    bytes_orphan_new.store(0, std::memory_order_relaxed);
    bytes_rejected.store(0, std::memory_order_relaxed);
}

bool InitIBDEfficiencyTrace(bool fEnabled)
{
    fIBDEfficiencyTraceEnabled = fEnabled;
    if (fEnabled)
    {
        nIBDEfficiencyLastSummary = GetTimeMicros();
        nIBDEfficiencyLastHeight = nBestHeight;
        LogPrintf("IBDEFFICIENCY time_us=%lld event=START enabled=1\n",
               (long long)nIBDEfficiencyLastSummary);
    }
    return true;
}

bool IBDEfficiencyTraceEnabled()
{
    return fIBDEfficiencyTraceEnabled;
}

void IBDEfficiencyRecordBlock(uint64_t nBlockSize,
                              bool fRequested,
                              bool fUnique,
                              bool fDuplicateIndexed,
                              bool fDuplicateOrphan,
                              bool fAcceptedActive,
                              bool fAcceptedSide,
                              bool fOrphanNew,
                              bool fRejected,
                              bool fRetryRecorded)
{
    if (!fIBDEfficiencyTraceEnabled)
        return;

    ibdEfficiencyTotal.RecordBlock(nBlockSize,
                                  fRequested, fUnique,
                                  fDuplicateIndexed, fDuplicateOrphan,
                                  fAcceptedActive, fAcceptedSide,
                                  fOrphanNew, fRejected, fRetryRecorded);
    ibdEfficiencyDelta.RecordBlock(nBlockSize,
                                   fRequested, fUnique,
                                   fDuplicateIndexed, fDuplicateOrphan,
                                   fAcceptedActive, fAcceptedSide,
                                   fOrphanNew, fRejected, fRetryRecorded);
}

void IBDEfficiencyMaybeSummary(int64_t nNow)
{
    if (!fIBDEfficiencyTraceEnabled)
        return;

    if (nIBDEfficiencyLastSummary == 0)
    {
        nIBDEfficiencyLastSummary = nNow;
        nIBDEfficiencyLastHeight = nBestHeight;
        return;
    }

    int64_t nElapsed = nNow - nIBDEfficiencyLastSummary;
    if (nElapsed < 60000000LL)
        return;

    ibdEfficiencyDelta.PrintSummary("INTERVAL",
                                    nIBDEfficiencyLastHeight,
                                    nBestHeight,
                                    nElapsed / 1000000LL);

    nIBDEfficiencyLastSummary = nNow;
    nIBDEfficiencyLastHeight = nBestHeight;
    ibdEfficiencyDelta.ResetDelta();
}

void IBDEfficiencyShutdownSummary()
{
    if (!fIBDEfficiencyTraceEnabled)
        return;

    ibdEfficiencyTotal.PrintSummary("TOTAL", 0, nBestHeight, 0);
}
