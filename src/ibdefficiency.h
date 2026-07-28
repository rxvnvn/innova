// Copyright (c) 2026 The Innova developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef INNOVA_IBDEFFICIENCY_H
#define INNOVA_IBDEFFICIENCY_H

#include <stdint.h>

#include <atomic>

class CNode;

struct IBDEfficiencyCounters
{
    // Count counters: origin dimension (each block counted in exactly one)
    std::atomic<uint64_t> received_requested;
    std::atomic<uint64_t> received_unsolicited;

    // Count counters: novelty dimension (each block counted in exactly one)
    std::atomic<uint64_t> received_unique;
    std::atomic<uint64_t> received_duplicate_indexed;
    std::atomic<uint64_t> received_duplicate_orphan;

    // Count counters: outcome dimension (each block counted in exactly one)
    std::atomic<uint64_t> received_accepted_active;
    std::atomic<uint64_t> received_accepted_side;
    std::atomic<uint64_t> received_orphan_new;
    std::atomic<uint64_t> received_rejected;

    // Count counter: retry record (subset of rejected)
    std::atomic<uint64_t> received_retry_recorded;

    // Byte counters: origin dimension
    std::atomic<uint64_t> bytes_requested;
    std::atomic<uint64_t> bytes_unsolicited;

    // Byte counters: novelty dimension
    std::atomic<uint64_t> bytes_unique;
    std::atomic<uint64_t> bytes_duplicate;

    // Byte counters: outcome dimension
    std::atomic<uint64_t> bytes_accepted_active;
    std::atomic<uint64_t> bytes_accepted_side;
    std::atomic<uint64_t> bytes_orphan_new;
    std::atomic<uint64_t> bytes_rejected;

    IBDEfficiencyCounters();

    void RecordBlock(uint64_t nBlockSize,
                     bool fRequested,
                     bool fUnique,
                     bool fDuplicateIndexed,
                     bool fDuplicateOrphan,
                     bool fAcceptedActive,
                     bool fAcceptedSide,
                     bool fOrphanNew,
                     bool fRejected,
                     bool fRetryRecorded);

    void PrintSummary(const char* pszLabel,
                      int nHeightStart,
                      int nHeightEnd,
                      int64_t nElapsedSec) const;

    void ResetDelta();
};

bool InitIBDEfficiencyTrace(bool fEnabled);
bool IBDEfficiencyTraceEnabled();
void IBDEfficiencyRecordBlock(uint64_t nBlockSize,
                              bool fRequested,
                              bool fUnique,
                              bool fDuplicateIndexed,
                              bool fDuplicateOrphan,
                              bool fAcceptedActive,
                              bool fAcceptedSide,
                              bool fOrphanNew,
                              bool fRejected,
                              bool fRetryRecorded);
void IBDEfficiencyMaybeSummary(int64_t nNow);
void IBDEfficiencyShutdownSummary();

#endif // INNOVA_IBDEFFICIENCY_H
