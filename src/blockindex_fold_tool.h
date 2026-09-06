// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.

#ifndef INNOVA_BLOCKINDEX_FOLD_TOOL_H
#define INNOVA_BLOCKINDEX_FOLD_TOOL_H

#include "blockindex_tip.h"
#include "blockindex_generation_lifecycle.h"
#include "fixed_blockindex_store.h"
#include "blockindex_derived_state.h"
#include "blockindex_activeindex.h"

#include <cstdint>
#include <string>
#include <vector>

// P6 — streaming online fold / compaction of the mutable tip into a new
// immutable generation.
//
// Immutable base (CURRENT=gen-A, tip S) + mutable tip (blockindex_tip, S+1..L)
// must, on fold, become a single new immutable generation gen-A+1 whose
// committed chain reaches the fold boundary F (>= S), with the mutable tip
// truncated back to F. Blocks <= F become immutable V2 history.
//
// Design (bounded RAM, no O(N) full-history rebuild, no consensus change):
//   1. Open the CURRENT base generation (records.dat + active.dat + derived.dat).
//   2. Stream-copy base records/active/derived into a NEW staging generation
//      build-{F+1}.tmp via the same FixedBlockIndexStore/ActiveIndex/
//      DerivedStateStore append primitives (batched, bounded RAM). The base
//      portion is copied byte-identical (records); the tip's fold-prefix
//      (blocks S+1..F, active) is appended on top with RecordIds continuing past
//      the base recordCount. Nothing is recomputed from scratch; no O(N) memory.
//   3. Write MANIFEST COMPLETE for the new generation (committedTip = F hash,
//      recordCount = base count + fold-prefix length).
//   4. PublishGeneration (build.tmp -> gen-{F+1}) then ValidateGeneration then
//      SelectGeneration (atomic CURRENT flip).
//   5. Truncate the mutable tip to F (tip re-anchor: baseGeneration-any, its
//      active tail [F, L] retained). Fail-closed on any mismatch -> no CURRENT
//      flip, tip untouched.
//
// Crash recovery: current flip is atomic (SelectGeneration); a crash before
// flip leaves the old CURRENT + tip intact; a crash after flip but before tip
// truncate leaves gen-F+1 selected with tip still holding the folded prefix --
// which the next restart reconciles (truncate to F). Lifrecycle Publish/
// Validate/Select already provide the crash-consistency.

struct BlockIndexFoldConfig
{
    std::string v2Root;
    uint64_t    numberOfGen;         // next generation id (current + 1)
    int32_t     foldHeight;          // F: fold blocks <= F into the immut. gen
    std::string error;               // captured errors
};

struct BlockIndexFoldResult
{
    bool ok;
    uint64_t baseRecordCount;
    uint64_t foldRecordCount;     // records in the fold-prefix appended
    int32_t  baseTipHeight;
    int32_t  foldHeight;
    uint64_t newGeneration;       // gen id selected after fold
    std::string error;

    BlockIndexFoldResult()
        : ok(false), baseRecordCount(0), foldRecordCount(0),
          baseTipHeight(-1), foldHeight(-1), newGeneration(0) {}
};

class BlockIndexFoldTool
{
public:
    // Fold the mutable tip up to foldHeight into new gen newGen.
    // Read base from <v2Root>/gen-{newGen-1} (the CURRENT immutable base).
    // Reads tip from <v2Root>/blockindex_tip.
    // On success: selects gen-newGen (CURRENT flipped) and truncates the tip to
    // foldHeight. All bounded RAM (streamed appends, no O(N) resident history).
    static BlockIndexFoldResult Fold(const std::string& v2Root,
                                     uint64_t newGen,
                                     int32_t foldHeight,
                                     std::string* error);
};

#endif // INNOVA_BLOCKINDEX_FOLD_TOOL_H