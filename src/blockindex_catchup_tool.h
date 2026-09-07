// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.

#ifndef INNOVA_BLOCKINDEX_CATCHUP_TOOL_H
#define INNOVA_BLOCKINDEX_CATCHUP_TOOL_H

#include "blockindex_tip.h"
#include "blockindex_v2_reader.h"

#include <cstdint>
#include <string>

// G2 — production S->L catch-up: bridge a validated immutable V2 generation @ S
// to the later persisted LEGACY_RESIDENT state @ L.
//
//   immutable V2 generation @ S
//       +  persisted legacy LevelDB state @ L  (consistent, read-only copy)
//       -> blockindex_tip containing S+1..L (active + sides)
//       -> authoritative restart lands at exact L
//
// This consumes REAL persisted legacy block-index state (CTxDB LevelDB keys
// "blockindex"+hash -> CDiskBlockIndex, "hashBestChain" -> tip hash), NOT
// synthetic in-process objects. It reuses the low-memory builder's exact
// CDiskBlockIndex->BlockIndexRecord decode + the M4 derived-state formulas
// (chainTrust = parent+blockTrust, stake checksum, memo, nSize) so the tip
// authority is byte/logical-identical to an offline rebuild. Memory is bounded
// independently of the delta (height-ordered streaming + a bounded parent-derived
// in-RAM cache for the delta; base S's derived state is read by value from the
// generation reader + derived store). Idempotent (BlockIndexTipAuthority::AppendBatch
// skips duplicate hashes); crash-safe (tip.meta is the single commit point,
// uncommitted tails truncated on open); fail-closed on base/gen mismatch.
//
// The caller supplies an explicit consistent copy of the legacy store (never the
// concurrently-mutating live datadir). For the production runbook this is a short
// final consistency stop -> copied LevelDB.

class BlockIndexCatchupResult
{
public:
    bool ok;                 // true on success
    std::string error;       // failure reason (ok==false)
    int32_t  baseTipHeight;  // S (from V2 authority)
    uint256  baseTipHash;
    int32_t  targetHeight;   // L (from legacy hashBestChain)
    uint256  targetHash;
    uint64_t appendedRecords; // count of S+1..L recorded (active + side)
    int32_t  finalTipHeight; // effective tip after catch-up (== L on success)
    uint256  finalTipHash;
    std::string errorClearOnOk;

    BlockIndexCatchupResult()
        : ok(false), baseTipHeight(-1), baseTipHash(0), targetHeight(-1),
          targetHash(0), appendedRecords(0), finalTipHeight(-1) {}
};

// Run the catch-up. Parameters:
//   v2Root            lifecycle root with the selected/validated AUTHORITATIVE
//                     generation (CURRENT). The base tip S is derived from it.
//   legacyLevelDb     path to a CONSISTENT COPY of the legacy txleveldb dir
//                     (blockindex + hashBestChain keys), reaching active tip L.
//   blockDataDir      dir with blk*.dat for exact nSize (may be empty -> nSize=0).
//   horizon           residency horizon (residency-only; not used by this tool's
//                     persisted authority but reserved for the live tail).
//   out               result.
// Returns false + out.error on ANY failure (fail-closed), leaves any partially
// built tip recoverable by re-running (idempotent).
bool RunBlockIndexCatchup(const std::string& v2Root,
                          const std::string& legacyLevelDb,
                          const std::string& blockDataDir,
                          int livetailHorizon,
                          BlockIndexCatchupResult* out);

#endif // INNOVA_BLOCKINDEX_CATCHUP_TOOL_H