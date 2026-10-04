// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.
//
// ===========================================================================
// PM1-P0-05 / R3G — THE ONE authoritative derived-state replay primitive.
// ===========================================================================
//
// Extracted VERBATIM from the G2 catch-up tool (blockindex_catchup_tool.cpp
// `ComputeDerived`, itself byte-for-byte the LM builder's M4 compute). Every
// derived-state writer path (LM builder, legacy catch-up, and the reader's
// corruption rebuild below) now shares THIS primitive, so a corruption rebuild
// cannot diverge from a builder-written derived.dat.
//
// SEMANTIC CORE (identical inputs -> identical outputs):
//   chainTrust            = parentDerived.chainTrust
//                           + GetAuthoritativeBlockTrustValue(rec.nBits,
//                               rec.height, fPos, rec.hashProof, rec.hash)
//                           (fPos from rec.nFlags; PM1-P0-08 rule)
//   stakeModifierChecksum = Hash(parentChecksum || nFlags || proof ||
//                               nStakeModifier) >> (256 - 32), where
//                           proof = PoS ? rec.hashProof : 0 and the parent
//                           term exists only when rec.hashPrev != 0.
//   stakeModifierTime     = BLOCK_STAKE_MODIFIER ? rec.nTime : (hasParent &&
//                           parent has memo ? parent memo : unavailable)
//   nSize                 = exact GetSerializeSize(CBlock @ rec.nFile /
//                           rec.nBlockPos, SER_NETWORK, PROTOCOL_VERSION)
//                           when the block file is readable AND the block
//                           hash identity matches; otherwise unavailable + 0.
//
// BY-VALUE ONLY: no CBlockIndex*, no mapBlockIndex, no pindexBest, no
// derived.dat reads. O(1) per call apart from the optional bounded block-file
// read for nSize. Zero derived-state reads: the corrupted file is never a
// rebuild input.
//
// STREAMED / BOUNDED: the drivers below hold only ONE derived entry in flight
// beyond the caller-owned output map. They never materialize a CBlockIndex
// per block and never populate mapBlockIndex (PM1-P0-06 stays open).
//
// AVAILABILITY-FIELD CONTRACT (blocks the default-entry trap):
// chainTrust and stakeModifierChecksum are ALWAYS written (magnitude 0 is a
// legal value and is never suppressed by the encoder). stakeModifierTime and
// nSize are availability-governed: each ComputeDerivedFromParent call clears
// them and sets them only via a replay-selected path, so a caller that starts
// from a default-constructed entry cannot leak layout noise into the payload.
#ifndef INNOVA_BLOCKINDEX_DERIVED_REPLAY_H
#define INNOVA_BLOCKINDEX_DERIVED_REPLAY_H

#include "fixed_blockindex_store.h"     // BlockIndexRecord, FixedBlockIndexStore, FixedBlockIndexManifest
#include "blockindex_activeindex.h"     // BlockIndexActiveIndex
#include "blockindex_derived_state.h"   // BlockIndexDerivedEntry, BlockIndexId

#include <stdint.h>
#include <map>
#include <string>

// THE semantic core (moved from blockindex_catchup_tool.cpp; the catch-up
// tool now calls this exact function).
//
//   rec           the record being replayed; its parent's derived entry is
//                 supplied separately
//   parentDerived the parent record's derived entry (a default-constructed
//                 entry seeds a branch root / genesis replay)
//   blockDataDir  directory containing blk%04u.dat; empty disables the nSize
//                 lookup (entries then carry nSize 0 / hasBlockSize false)
//   out           the derived entry for THIS record; always fully written
void ComputeDerivedFromParent(const BlockIndexRecord& rec,
                              const BlockIndexDerivedEntry& parentDerived,
                              const std::string& blockDataDir,
                              BlockIndexDerivedEntry* out);

enum BlockIndexDerivedReplayStatus
{
    BLOCK_INDEX_DERIVED_REPLAY_OK = 0,
    BLOCK_INDEX_DERIVED_REPLAY_IO_ERROR = 2,   // any store/active read I/O failure
    BLOCK_INDEX_DERIVED_REPLAY_CORRUPT = 3,    // decode/integrity/topology failure
    BLOCK_INDEX_DERIVED_REPLAY_INTERNAL = 4,   // impl precondition violated (e.g. bad handle)
};

// ACTIVE-chain driver: deterministically regenerates the derived entries for
// the contiguous ACTIVE chain parent -> committed tip, streamed by height.
//
//   store        open read-only records store of the SAME generation
//   active       open read-only active index (same generation; NOT made
//                writable by this driver)
//   manifest     that store's manifest (committed tip / record count)
//   blockDataDir blk directory; empty disables nSize materialization
//   parentId     RecordId of an ALREADY-DERIVED record on the active chain
//                (its derived entry must equal this record's OWN derived
//                semantics, not the seed of its child). Records are never
//                read for any other purpose; NO derived.dat access.
//   seedDerived  that parent's OWN derived entry
//   outEntries   caller-owned map keyed by RecordId; on success receives the
//                rebuilt entry for every RecordId strictly between parentId
//                and the committed active tip. (Generation immutability: the
//                caller decides whether/where these live; this driver only
//                fills the caller's structures.)
//   outReplayed  number of entries written by THIS call (optional)
//
// Fail closed on: the seed parent record unreadable/corrupt; any active
// height in (parent, tip] unreadable, corrupt, or height/linkage
// inconsistent (the active index's own O(1) adjacency check). On failure
// nothing was appended to *outEntries beyond what earlier calls put there.
BlockIndexDerivedReplayStatus ReplayDerivedActiveChain(
    const FixedBlockIndexStore& store,
    const BlockIndexActiveIndex& active,
    const FixedBlockIndexManifest& manifest,
    const std::string& blockDataDir,
    BlockIndexId parentId,
    const BlockIndexDerivedEntry& seedDerived,
    std::map<BlockIndexId, BlockIndexDerivedEntry>* outEntries,
    uint64_t* outReplayed,
    std::string* error);

// SIDE-branch closure driver (deterministic DFS): complements the active
// driver by regenerating derived entries for every remaining record in the
// generation (off-active-path branches and their own branch roots).
//
// For each record id in [1, manifest.recordCount] not yet present in
// *outEntries: walk its hashPrev ancestry THROUGH THE STORE (per-hash point
// reads, O(depth) per chain start, O(records) total) until a record IS
// resolved, then derive DOWN the stack with ComputeDerivedFromParent, writing
// each new entry into *outEntries. A record with hashPrev == 0 that is
// unresolved is a BRANCH ROOT and seeds from a default-constructed derived
// entry (the exact parentless shape the builder uses).
//
// Fail closed on: any store read failure; an ancestry step whose claimed
// parent hash has NO matching record in the store (disconnected topology —
// the same A.10.1b-fix3 contract the builder enforces); a cycle (a record is
// its own ancestor). This is the exact shape where authoritative RECORD
// corruption must fail closed: nothing derived from it is manufactured.
BlockIndexDerivedReplayStatus ReplayDerivedSideClosure(
    const FixedBlockIndexStore& store,
    const FixedBlockIndexManifest& manifest,
    const std::string& blockDataDir,
    std::map<BlockIndexId, BlockIndexDerivedEntry>* outEntries,
    uint64_t* outReplayed,
    std::string* error);

// Reader rebuild composition: the full deterministic rebuild for a
// PRESENT-BUT-CORRUPT derived companion.
//
//   store / active / manifest  the generation's authoritative inputs (the
//                              reader has already opened and tip-validated
//                              them before calling)
//   blockDataDir               blk directory; empty disables nSize lookup
//   outEntries                 receives the rebuilt entry for EVERY committed
//                              record (RecordId-keyed); REBUILD-ONLY output
//                              consumed in memory by the reader — never
//                              written to the sealed generation
//   status                     replay status on failure (optional)
//
// Sequence: PASS 1 = active chain (ReplayDerivedActiveChain, planted on the
// synthetic null genesis parent); PASS 2 = side-branch closure
// (ReplayDerivedSideClosure) filling every remaining record. Coverage proof:
// outEntries->size() == manifest.recordCount is the LAST.success gate.
//
// Fail closed on any authoritative-input failure (store read failure, active
// entry corruption, linkage mismatch, duplicate record hash, claimed parent
// absent from the store, or any coverage shortfall). On failure *outEntries
// publishes NOTHING from that attempt.
bool RebuildAllDerivedFromAuthoritative(
    const FixedBlockIndexStore& store,
    const BlockIndexActiveIndex& active,
    const FixedBlockIndexManifest& manifest,
    const std::string& blockDataDir,
    std::map<BlockIndexId, BlockIndexDerivedEntry>* outEntries,
    BlockIndexDerivedReplayStatus* status,
    std::string* error);

#endif // INNOVA_BLOCKINDEX_DERIVED_REPLAY_H
