// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.

#ifndef INNOVA_BLOCKINDEX_RECOVERY_TOOL_H
#define INNOVA_BLOCKINDEX_RECOVERY_TOOL_H

#include "fixed_blockindex_store.h"
#include "blockindex_derived_state.h"

#include <stdint.h>
#include <string>

// ---------------------------------------------------------------------------
// Bounded-memory Block Index V2 recovery tool (library core).
//
// Converts the VALIDATED Stage-B streaming prototype into the production
// recovery path for a legacy chain history S..L (the genesis-recovery case
// is S == -1: an empty base generation + the full chain above it):
//
//   CONSISTENT IMMUTABLE legacy LevelDB snapshot       (read-only open)
//        |  backward ancestry walk L -> S+1            O(1) RAM per step
//        v
//   <v2root>/blockindex_tip/recovery-path.tmp          on-disk SPILL,
//        |  (records, ascending, fixed 228 B entries)  delta * 228 B disk
//        v
//   forward streaming pass: ComputeDerivedFromParent   (the ONE shared
//        |  M4 primitive) + BlockIndexTipAuthority::AppendBatch every
//        |  BATCH_RECORDS records (the existing commit protocol:
//        |  tail append + fsync + side index + tip.meta single commit)
//        v
//   <v2root>/blockindex_tip/  -- full provenance history, tip == L
//
// Memory contract (replaces the Stage-A OOM: activePath ~1.86 GiB vector +
// DerivedCache ~1.0 GiB map):
//   * NO full-chain in-memory vector: records live in the on-disk path file
//     (the tip-records.dat-equivalent form); ONE record is in flight.
//   * NO unbounded derived map: the derived chain needs ONLY the parent's
//     derived entry, which for a linear chain is the previous step's
//     output (O(1) resident) -- exactly the Stage-B observation.
//   * NO O(N^2): the walk reads each record exactly once; the forward pass
//     streams the path file exactly once.
//   * The tip authority's bounded windows are the only inside-the-tip cache
//     (65536-entry ring + 2 MiB LRU, compile-time bounded).
// Disk: recovery-path.tmp (delta * 228 B, deleted after final acceptance)
//       + the real tip-records/-derived/-active/-hashindex stores
//       (delta * 292 B + LevelDB), which ARE the deliverable.
//
// Crash safety / idempotency:
//   * tip.meta remains THE single commit point (one per batch) -- an
//     interrupted forward pass leaves at most the pre-existing committed
//     tip plus an uncommitted tail, truncated deterministically at Open.
//   * recovery.meta (CoW tmp+fsync+rename, single 512-byte fixed record)
//     is the derived resume bookkeeping (never authoritative: tip.meta is).
//     Crash cases: mid-backward-walk -> path file incomplete -> rebuilt
//     deterministically; mid-forward-pass -> tip.meta height K resumes the
//     forward pass from slot K-S-1 with an O(1) link validation.
//   * Retry always converges to tip == L; a mismatched resume (base gen,
//     target, linkage) fails closed with the tip untouched.
//   * Validation BEFORE the final acceptance: the run reopens the tip
//     authority (Open revalidates the full streaming content digest chains
//     O(N) time / O(1) RAM) and requires reader.PeekTip() == L, then (and
//     only then) deletes the spill + writes recovery.meta = ACCEPTED.
//     ROLLBACK EVIDENCE (recovery.meta + path file when abandoned) is
//     preserved until the operator deletes the recovery directory.
// ---------------------------------------------------------------------------

struct BlockIndexRecoveryStats
{
    bool ok;                          // false + error on any failure
    std::string error;                // failure reason (ok == false)

    // Inputs
    uint64_t legacyRecordsTotal;      // records seen in the snapshot scan
    int32_t  baseTipHeight;           // S (base generation committed tip)
    uint256  baseTipHash;             // S hash
    int32_t  targetHeight;            // L (from legacy hashBestChain)
    uint256  targetHash;              // L hash

    // Progress
    uint64_t recordsAppended;         // records appended THIS run
    uint64_t recordsResumed;          // records skipped (already committed)
    uint32_t batch;                   // AppendBatch leaf calls that committed
    uint32_t fullBatches;             // full BATCH_RECORDS batches committed

    // Derived identity
    uint256  lastChainTrust;          // tip.chainTrust after the run (M4)

    // Bounded-memory witnesses
    int64_t  peakRssBytes;            // /proc/self/status VmHWM after the run
    uint64_t ringResidentMax;         // max pending-window residency observed
    uint64_t ringCap;                 // window cap (constant 65536)
    uint64_t lruBytesCap;             // window LRU byte cap (constant 2 MiB)
    uint64_t derivedRingBytesCap;     // derived-ring byte cap (budget based)
    int64_t  diskBytesPeak;           // peak extra disk of the spill itself
    int64_t  diskBytesFinal;          // extra disk AFTER acceptance (0)

    BlockIndexRecoveryStats()
        : ok(false), legacyRecordsTotal(0), baseTipHeight(-1), baseTipHash(0),
          targetHeight(-1), targetHash(0), recordsAppended(0),
          recordsResumed(0), batch(0), fullBatches(0), lastChainTrust(0),
          peakRssBytes(0), ringResidentMax(0), ringCap(0), lruBytesCap(0),
          derivedRingBytesCap(0), diskBytesPeak(0), diskBytesFinal(0) {}
};

// Run the recovery. Parameters:
//   v2Root            lifecycle root with the CURRENT-selected (validated)
//                     base generation. The base tip S is derived from it.
//                     S == -1 (empty generation) = genesis recovery.
//   legacyLevelDb     CONSISTENT READ-ONLY COPY of the legacy txleveldb
//                     (blockindex + hashBestChain), reaching tip L.
//   blockDataDir      dir with blk%04u.dat for exact nSize (may be empty ->
//                     nSize 0 / unavailable). Optional per-record data.
//   abandon           true = discard a PARTIAL recovery under this root and
//                     rebuild the tip from scratch (the pre-recovery tip dir
//                     is preserved as blockindex_tip.pre-recovery-N/).
//   out               result.
// The run is idempotent: re-running is a resume, never a duplicate commit.
bool RunBlockIndexRecovery(const std::string& v2Root,
                           const std::string& legacyLevelDb,
                           const std::string& blockDataDir,
                           bool abandon,
                           BlockIndexRecoveryStats* out);

#endif // INNOVA_BLOCKINDEX_RECOVERY_TOOL_H
