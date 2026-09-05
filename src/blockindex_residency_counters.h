// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.

// A.10.1q / Activation Stage 1 - Block Index residency scalar counters.
//
// Minimal O(1) instrumentation to causally answer the real-chain A/B question:
// does historical block count dictate resident CBlockIndex RAM?
//
// Counters observe events at their NATURAL creation/call sites only. They never
// traverse history to count, never retain pointers, never affect consensus,
// serialization, or disk format.

#ifndef INNOVA_BLOCKINDEX_RESIDENCY_COUNTERS_H
#define INNOVA_BLOCKINDEX_RESIDENCY_COUNTERS_H

#include <stdint.h>
#include <string>

// Single writer at each natural site; declared volatile-free atomics.
extern volatile int64_t g_res_cblockindex_constructed;
extern volatile int64_t g_res_mapinserts;
extern volatile int64_t g_res_pprev_links;
extern volatile int64_t g_res_pnext_links;
extern volatile int64_t g_res_pskip_links;
extern volatile int64_t g_res_anchors;
extern volatile int64_t g_res_loadblockindex_calls;
extern volatile int64_t g_res_rebuildcandidates_calls;
extern volatile int64_t g_res_restoredagtrust_calls;
extern volatile int64_t g_res_legacyaccessor_fallbacks;

// Emit one compact key=value residency line for a given startup mode + HotOwner
// metrics. mode: "LEGACY_RESIDENT"|"BY_VALUE_AUTHORITATIVE"; generation 0 if none.
// hot_current/hot_peak = BlockIndexHotMetrics residentCount/peakResidentCount;
// pins_current/pins_peak = pinnedCount/pin peak.
void PrintBlockIndexResidency(const std::string& mode, int64_t generation,
                              const char* tag,
                              int64_t hot_current, int64_t hot_peak,
                              int64_t pins_current, int64_t pins_peak);

#endif // INNOVA_BLOCKINDEX_RESIDENCY_COUNTERS_H