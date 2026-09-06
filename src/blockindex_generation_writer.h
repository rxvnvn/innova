// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.

#ifndef INNOVA_BLOCKINDEX_GENERATION_WRITER_H
#define INNOVA_BLOCKINDEX_GENERATION_WRITER_H

#include "fixed_blockindex_store.h"
#include "blockindex_hashindex.h"
#include "blockindex_activeindex.h"
#include "blockindex_derived_state.h"
#include "blockindex_generation_lifecycle.h"

#include <cstdint>
#include <string>
#include <vector>

// Common bounded-memory generation construction path (M1).
//
// ONE construction path shared by:
//   1. V1 -> V2 migration (stream from legacy txleveldb)
//   2. V2 online fold/compaction (copy base gen + append fold prefix)
//
// The writer manages the four generation stores (records.dat / hashindex /
// active.dat / derived.dat) and their content binding + MANIFEST + lifecycle.
// It keeps memory bounded (O(batch)) by buffering appends and flushing them via
// the store AppendBatch primitives (single-fsync batches, byte-identical to
// per-record appends). No O(N) resident graph / map is required.
//
// Both the migration builder and the fold tool drive this writer; neither keeps
// its own second implementation of the record/active/derived write path.

class BlockIndexGenerationWriter
{
public:
    BlockIndexGenerationWriter();
    ~BlockIndexGenerationWriter();

    // Create a fresh target generation directory (build-<gen>.tmp by convention,
    // caller supplies the staging path). Idempotent-clean (removes a stale tmp).
    bool OpenTarget(const std::string& stagingDir, uint64_t generation,
                    std::string* error);

    // ---- append (buffered, bounded) ----
    // Append one record in RecordId order. Returns the assigned RecordId.
    bool AppendRecord(const BlockIndexRecord& rec, BlockIndexId* outId,
                      std::string* error);
    // Append one derived entry (RecordId order, matches AppendRecord sequence).
    bool AppendDerived(const BlockIndexDerivedEntry& entry, std::string* error);
    // Append one active member (height -> RecordId). Heights must be appended
    // in ascending dense order 0..tip.
    bool AppendActive(BlockIndexId id, int32_t height, std::string* error);
    // Flush any buffered batches (records/derived/active) to disk.
    bool Flush(std::string* error);

    // ---- hash-index management ----
    // Put hash -> RecordId into the hashindex (batch-buffered too).
    bool PutHashIndex(const uint256& hash, BlockIndexId id, std::string* error);

    // ---- finalize ----
    // Set the derived content binding (generation root or shadow binding), then
    // Finalize derived.dat and write the MANIFEST as COMPLETE with the given
    // committed tip.
    bool Finalize(uint64_t generation,
                  const uint256& committedTipHash,
                  BlockIndexId committedTipId,
                  int32_t committedTipHeight,
                  uint64_t recordCount,
                  uint32_t generationCapability,
                  std::string* error);

    // Compute the generation root from files (records/active/hashindex/derived)
    // for AUTHORITATIVE content binding. Uses RecomputeGenerationRootFromFiles.
    static bool ComputeGenerationRootFromFiles(const std::string& stagingDir,
                                               uint64_t generation,
                                               unsigned char root[32],
                                               std::string* error);

    // ---- lifecycle ----
    // Publish (rename build-N.tmp -> gen-N), validate, select (atomic CURRENT)
    // under <root>. This is the crash-safe commit: old CURRENT untouched until
    // SelectGeneration succeeds.
    static bool ValidatePublishSelect(const std::string& root,
                                      uint64_t generation,
                                      std::string* error);

    // Close/destroy writer handles (release FILE* + LevelDB LOCK) so a
    // downstream PublishGeneration can re-open read-only.
    void Close();

    // diagnostics
    uint64_t RecordCount() const;      // records appended (committed to write)
    bool IsOpen() const;

private:
    FixedBlockIndexStore store_;
    BlockIndexHashIndex hashIndex_;
    BlockIndexActiveIndex active_;
    BlockIndexDerivedStateStore derived_;
    std::vector<BlockIndexRecord> recordBatch_;
    std::vector<BlockIndexDerivedEntry> derivedBatch_;
    std::vector<BlockIndexId> activeBatch_;
    std::string stagingDir_;
    uint64_t generation_;
    bool open_;

    static const size_t kBatchEntries = 65536; // bounded batch (~15 MB max)
};

#endif // INNOVA_BLOCKINDEX_GENERATION_WRITER_H