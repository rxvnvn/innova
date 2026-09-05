// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.

#include "blockindex_residency_counters.h"

#include "main.h"

#include <stdio.h>

volatile int64_t g_res_cblockindex_constructed = 0;
volatile int64_t g_res_mapinserts = 0;
volatile int64_t g_res_pprev_links = 0;
volatile int64_t g_res_pnext_links = 0;
volatile int64_t g_res_pskip_links = 0;
volatile int64_t g_res_anchors = 0;
volatile int64_t g_res_loadblockindex_calls = 0;
volatile int64_t g_res_rebuildcandidates_calls = 0;
volatile int64_t g_res_restoredagtrust_calls = 0;
volatile int64_t g_res_legacyaccessor_fallbacks = 0;

void PrintBlockIndexResidency(const std::string& mode, int64_t generation,
                              const char* tag,
                              int64_t hot_current, int64_t hot_peak,
                              int64_t pins_current, int64_t pins_peak)
{
    printf("BLOCKINDEX_RESIDENCY %s mode=%s generation=%lld "
           "map_entries=%lld cblockindex_constructed=%lld mapinserts=%lld "
           "pprev_links=%lld pnext_links=%lld pskip_links=%lld anchors=%lld "
           "hot_current=%lld hot_peak=%lld pins_current=%lld pins_peak=%lld "
           "loadblockindex_calls=%lld rebuildcandidates_calls=%lld "
           "restoredagtrust_calls=%lld legacy_accessor_fallbacks=%lld\n",
           tag ? tag : "",
           mode.c_str(), (long long)generation,
           (long long)mapBlockIndex.size(),
           (long long)g_res_cblockindex_constructed,
           (long long)g_res_mapinserts,
           (long long)g_res_pprev_links,
           (long long)g_res_pnext_links,
           (long long)g_res_pskip_links,
           (long long)g_res_anchors,
           (long long)hot_current, (long long)hot_peak,
           (long long)pins_current, (long long)pins_peak,
           (long long)g_res_loadblockindex_calls,
           (long long)g_res_rebuildcandidates_calls,
           (long long)g_res_restoredagtrust_calls,
           (long long)g_res_legacyaccessor_fallbacks);
    fflush(stdout);
}