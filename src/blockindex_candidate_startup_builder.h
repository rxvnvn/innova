// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.

// A.10.1m - By-value candidate startup builder.
//
// Replaces the startup candidate-tip reconstruction dependency on the
// historical mapBlockIndex / pprev graph (legacy RebuildCandidateTips,
// candidate_frontier.cpp:217-279) with a PURE BY-VALUE builder that populates
// the EXISTING by-value CandidateFrontierStore (SnapshotCandidateFrontierStore)
// from a selected authoritative V2 generation.
//
// Legacy semantics reproduced (oracle):
//   - a block is a candidate TIP iff its hash is NOT referenced as any other
//     block's parent (setReferenced built from all hashPrev);
//   - fork-point = LCA(tip, pindexBest) by pprev walk  -> reproduced by-value
//     via reader.FindFork(tipId, bestId);
//   - nChainTrust copied verbatim (from derived store);
//   - fValid = operator-invalid check (setInvalidBlockHash);
//   - fHasData = block bytes available;
//   - tie/winner left to EvaluateCandidateFrontierByValue (strict '>' on trust,
//     hash-height ordering), NOT decided here.
//
// NO CBlockIndex*, NO mapBlockIndex, NO pprev/pskip/pnext, NO O(N) historical
// pointer topology. Transient parent-hash set is O(N) COMPACT SCALARS (justified
// and reported). Output CandidateFrontierStore is O(F) - the existing by-value
// authority, not a duplication.
//
// This phase does NOT perform D cutover, does NOT flip startup default, and
// does NOT remove/change legacy RebuildCandidateTips (still used by
// LEGACY_RESIDENT).

#ifndef INNOVA_BLOCKINDEX_CANDIDATE_STARTUP_BUILDER_H
#define INNOVA_BLOCKINDEX_CANDIDATE_STARTUP_BUILDER_H

#include "candidate_frontier.h"
#include "blockindex_v2_reader.h"
#include "blockindex_derived_state.h"

#include <stdint.h>
#include <string>

class BlockIndexCandidateStartupBuilder
{
public:
    BlockIndexCandidateStartupBuilder();

    // Build the by-value candidate frontier store from a selected generation's
    // reader + derived store. `reader`/`derived` must be open and generation-
    // coherent (A.10.1i bootstrap style). `forkHeightDAG` = FORK_HEIGHT_DAG.
    // On success fills `store` (existing concrete by-value frontier store) and
    // returns true. Fails closed on any read/corruption/generation error; never
    // falls back to legacy RebuildCandidateTips.
    bool Build(const BlockIndexV2Reader& reader,
               const BlockIndexDerivedStateStore& derived,
               int forkHeightDAG,
               SnapshotCandidateFrontierStore* store,
               std::string* error) const;
};

#endif // INNOVA_BLOCKINDEX_CANDIDATE_STARTUP_BUILDER_H