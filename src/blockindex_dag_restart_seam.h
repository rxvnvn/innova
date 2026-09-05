// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.

// A.10.1j - Bounded by-value DAG restart seam.
//
// Goal: make DAG restart reconstruction (the nDAGScore -> nChainTrust restore
// that legacy CDAGManager::RestoreDAGTrustIntoChainTrust performs) reproducible
// from by-value metadata WITHOUT the all-history resident CBlockIndex graph.
//
// Legacy oracle (dag.cpp:958-982): for every block with
//   nHeight >= FORK_HEIGHT_DAG AND IsProofOfWork() AND mapDAGData[hash].nDAGScore != 0
// the daemon sets CBlockIndex::nChainTrust = nDAGScore. The function reads only
// hash / nHeight / nFlags(BLOCK_PROOF_OF_STAKE) / nDAGScore - all by-value
// fields. It does NOT follow pprev/pnext/pskip.
//
// DAG topology (vDAGParents) and canonical nDAGScore persist in the LevelDB
// "daglinks"/"dagscores" store (CTxDB::WriteDAGLinks, txdb-leveldb.cpp:423-426;
// IterateDAGLinks 702-745), independent of the V2 generation and of the resident
// mapBlockIndex. So the restore decision can be computed by scanning the
// daglinks store O(N) on disk AND the by-value authoritative metadata, without
// constructing historical CBlockIndex objects.
//
// This seam is a REPRESENTATION / DATA-SOURCE migration only. It does NOT:
//   - change DAG parent semantics, DAG score semantics, chain selection,
//     tie-break, checkpoints, staking, validity, or consensus rules;
//   - perform authoritative-startup cutover (D) or full HotOwner lifecycle (B);
//   - replace the resident mapDAGData (which remains the canonical O(N) runtime
//     DAG authority - disclosed, not disguised).
//
// Production default startup remains LEGACY_RESIDENT (init.cpp:1470 still calls
// legacy LoadBlockIndex()). This seam is the additive by-value substrate that D
// will select.
//
// Lock contract: this seam is leaf/stateless with respect to cs_main. It never
// mutates global CBlockIndex/mapBlockIndex/mapDAGData. It consumes a caller-held
// read snapshot. No new global locks; no async I/O.

#ifndef INNOVA_BLOCKINDEX_DAG_RESTART_SEAM_H
#define INNOVA_BLOCKINDEX_DAG_RESTART_SEAM_H

#include "blockindex_startup_authority.h"

#include <map>
#include <stdint.h>
#include <string>
#include <vector>

// A single post-DAG PoW block that legacy would restore: hash -> canonical
// DAG score (== restored nChainTrust).
struct DagRestartRestoreEntry
{
    uint256 hash;
    uint256 dagScore;      // canonical nDAGScore; == resulting nChainTrust
    int     height;
};

// By-value result of a DAG restart trust restore. No CBlockIndex*, no pointer
// topology; stable logical hashes only.
struct DagRestartResult
{
    std::vector<DagRestartRestoreEntry> restore;   // blocks legacy would restore
    uint64_t totalRestored;                        // size of restore
    bool     ok;
    std::string error;

    DagRestartResult() : totalRestored(0), ok(false) {}
};

// By-value DAG restart seam.
//
// Reproduces CDAGManager::RestoreDAGTrustIntoChainTrust's decision rule as a
// by-value mapping using:
//   1. the by-value startup authority (hash -> height / flags / PoW) for the
//      SELECTED authoritative generation;
//   2. the DAG canonical scores read O(N) from the LevelDB "daglinks" store.
//
// The seam NEVER dereferences mapBlockIndex / resident CBlockIndex. It never
// mutates global DAG consensus state. It is a pure read-side by-value
// reconstruction used as the correctness substrate for D (and for differential
// tests proving parity with the legacy oracle).
class BlockIndexDagRestartSeam
{
public:
    BlockIndexDagRestartSeam();

    // Compute the set of post-DAG PoW blocks whose nChainTrust would be
    // restored (== nDAGScore) under legacy semantics, from:
    //   - dagLinksDir  : LevelDB dir holding "daglinks"/"dagscores" (from
    //                    which canonical nDAGScore per hash is read);
    //   - authority    : open by-value startup authority for the SAME
    //                    generation (supplies height + PoW flag + reachable
    //                    historical hashes);
    //   - forkHeightDAG: FORK_HEIGHT_DAG.
    // Returns false (fail closed) on any read/corruption error; never silently
    // returns a partial/legacy result.
    bool ComputeRestore(const std::string& dagLinksDir,
                        const BlockIndexStartupAuthority& authority,
                        int forkHeightDAG,
                        DagRestartResult* out,
                        std::string* error) const;

private:
    // Read canonical nDAGScore per hash from the LevelDB daglinks store.
    // (Reuses the persisted "dagscores" semantics; NULL on not found.)
    bool ReadCanonicalScores(const std::string& dagLinksDir,
                             std::map<uint256, uint256>* scores,
                             std::string* error) const;
};

#endif // INNOVA_BLOCKINDEX_DAG_RESTART_SEAM_H