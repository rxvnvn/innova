// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.
//
// A.13.6-R2c.1 — authoritative, generation-bound DAG tip frontier substrate.
//
// Phase-1 proven semantic: for a complete daglinks store, the legacy DAG tip
// set equals  { daglinks node hashes }  minus  { hashes referenced as parents
// by any known DAG node }.  This is exactly what CDAGManager::
// RebuildPendingChildIndex (src/dag.cpp) reconstructs on load from persisted
// vDAGParents alone; persisted vDAGChildren is ignored and rebuilt.
//
// This header provides:
//   - an immutable generation-bound "dag-tip-frontier" artifact that stores the
//     deterministic, sorted tip-hash seed; built with bounded external-sort RAM;
//   - a streaming reader (never materializes the full tip set);
//   - a mutable live overlay that mirrors the ongoing legacy tip-set mutations
//     with bounded RAM residency and a generation/restart contract.
//
// This slice does NOT migrate any consumer (SelectBestDAGTip remains
// untouched, legacy setDAGTips remains authoritative).

#ifndef INNOVA_DAG_TIP_FRONTIER_H
#define INNOVA_DAG_TIP_FRONTIER_H

#include <stdint.h>
#include <string>
#include <vector>

#include "uint256.h"  // innnova uint256

// ---------------------------------------------------------------------------
// Immutable generation tip-frontier artifact
// ---------------------------------------------------------------------------

// Deterministic on-disk form:
//   magic  "INNOVADGTF1"  (12 bytes)
//   version          u8  == 1
//   generation       u64 LE
//   dagInputDigest   32 bytes (binding to the DAG source digest in the V2
//                              generation MANIFEST)
//   frontierDigest   32 bytes (SHA256 over the sorted concatenated tip hashes)
//   tipCount         u64 LE
//   tip hashes        tipCount * 32 bytes, ascending uint256 order (duplicate
//                     free, deterministic)
//
// The tip hashes are stored sorted so the reader can stream them with a
// bounded window and the digest is independently verifiable over the stream.

namespace dag_tip_frontier {

static const char* const FRONTIER_MAGIC = "INNOVADGTF1";
static const unsigned int FRONTIER_VERSION = 1;
static const unsigned int FRONTIER_HEADER_MIN_SIZE = 12 + 1 + 8 + 32 + 32 + 8; // 93

struct BuildOptions
{
    size_t chunkBytes;
    size_t maxRecordsPerChunk;
    size_t maxOpenRuns;
    std::string tempParent;
    BuildOptions()
        : chunkBytes(1024 * 1024), maxRecordsPerChunk(4096), maxOpenRuns(8),
          tempParent() {}
};

enum BuildStatus
{
    DAG_TIP_FRONTIER_BUILD_OK = 0,
    DAG_TIP_FRONTIER_BUILD_SOURCE_UNAVAILABLE,
    DAG_TIP_FRONTIER_BUILD_SOURCE_CORRUPT,
    DAG_TIP_FRONTIER_BUILD_DECODE_FAILURE,
    DAG_TIP_FRONTIER_BUILD_TEMP_IO_FAILURE,
    DAG_TIP_FRONTIER_BUILD_AUTHORITY_FAILURE
};

struct BuildResult
{
    BuildStatus status;
    std::string error;
    uint64_t nodesProcessed;
    uint64_t parentRefsProcessed;
    uint64_t frontierTipCount;
    uint64_t artifactBytes;
    uint64_t temporaryBytesWritten;
    uint64_t peakChunkBytes;
    uint64_t peakChunkRecords;
    uint64_t runCount;
    uint64_t mergePasses;
    BuildResult()
        : status(DAG_TIP_FRONTIER_BUILD_AUTHORITY_FAILURE),
          nodesProcessed(0), parentRefsProcessed(0), frontierTipCount(0),
          artifactBytes(0), temporaryBytesWritten(0), peakChunkBytes(0),
          peakChunkRecords(0), runCount(0), mergePasses(0) {}
};

// Build the immutable tip-frontier artifact for the selected generation.
//
//   dagLinksDir   : the generation's DAG source LevelDB directory (the same
//                   directory the DagSourceBindingVerifier / DagLogicalAuthority
//                   open, containing "daglinks"-prefixed keys).
//   expectedDigest: the generation's dagInputDigest read from the V2 MANIFEST;
//                   the builder verifies the source stream against it so the
//                   artifact is bound to the exact generation.
//   generation    : the generation id from the V2 MANIFEST.
//   outPath       : artifact destination (e.g. <genDir>/dag-tip-frontier.dat).
bool BuildDagTipFrontier(const std::string& dagLinksDir,
                         const unsigned char expectedDigest[32],
                         uint64_t generation,
                         const std::string& outPath,
                         const BuildOptions& options,
                         BuildResult* result);

// Current-source-only bounded derivation. Writes a raw, headerless sequence of
// sorted uint256 tip hashes to caller-owned outputPath. It has no generation,
// immutable digest, SourceStateId, or overlay semantics.
enum CurrentDagTipDerivationStatus
{
    DAG_CURRENT_TIPS_DERIVATION_INTERNAL_FAILURE = 0,
    DAG_CURRENT_TIPS_DERIVATION_OK,
    DAG_CURRENT_TIPS_DERIVATION_INVALID_OPTIONS,
    DAG_CURRENT_TIPS_DERIVATION_SOURCE_UNAVAILABLE,
    DAG_CURRENT_TIPS_DERIVATION_SOURCE_CORRUPT,
    DAG_CURRENT_TIPS_DERIVATION_DECODE_FAILURE,
    DAG_CURRENT_TIPS_DERIVATION_TEMP_IO_FAILURE,
    DAG_CURRENT_TIPS_DERIVATION_OUTPUT_PUBLICATION_FAILURE
};

struct CurrentDagTipDerivationResult
{
    CurrentDagTipDerivationStatus status;
    std::string error;
    uint64_t nodesProcessed, parentRefsProcessed, tipCount;
    uint64_t temporaryBytesWritten, peakChunkBytes, peakChunkRecords;
    uint64_t runCount, mergePasses;
    CurrentDagTipDerivationResult() : status(DAG_CURRENT_TIPS_DERIVATION_INTERNAL_FAILURE), nodesProcessed(0), parentRefsProcessed(0), tipCount(0), temporaryBytesWritten(0), peakChunkBytes(0), peakChunkRecords(0), runCount(0), mergePasses(0) {}
};
bool DeriveCurrentDagTipsBounded(const std::string& dagLinksDir,
                                 const std::string& outputPath,
                                 const BuildOptions& options,
                                 CurrentDagTipDerivationResult* result);

// Read the artifact header back (for verification tooling). Not required for
// streaming reads.
bool ReadDagTipFrontierHeader(const std::string& path,
                              uint64_t* generation,
                              unsigned char dagInputDigest[32],
                              unsigned char frontierDigest[32],
                              uint64_t* tipCount,
                              std::string* error);

// Streaming iteration over an existing frontier artifact. The cursor holds a
// small bounded read window and never materializes the full tip set.
class TipFrontierReader
{
public:
    TipFrontierReader();
    ~TipFrontierReader();
    // open(expectGeneration) binds the artifact to a generation; mismatch,
    // corrupt header, or digest failure is a typed failure.
    bool Open(const std::string& path, uint64_t expectGeneration,
              const unsigned char expectDagInputDigest[32], std::string* error);
    void Close();
    bool IsOpen() const;
    uint64_t Generation() const;
    uint64_t TipCount() const;
    bool GetFrontierDigest(unsigned char out[32]) const;
    // The dagInputDigest the artifact was bound to at open (for root folding).
    bool GetBoundDagInputDigest(unsigned char out[32]) const;
    // true = next tip hash produced; false = exhausted or not open.
    bool Next(uint256* out);
    // Bounded contains() via linear scan only if genuinely useful (see report).
    bool Contains(const uint256& hash);
    void ResetStream();
private:
    struct Impl;
    Impl* impl_;
};

// Typed outcomes shared by the mutable overlay.
enum FrontierReadStatus
{
    DAG_TIP_FRONTIER_OK = 0,
    DAG_TIP_FRONTIER_NOT_FOUND,
    DAG_TIP_FRONTIER_GENERATION_MISMATCH,
    DAG_TIP_FRONTIER_CORRUPT,
    DAG_TIP_FRONTIER_AUTHORITY_FAILURE
};

} // namespace dag_tip_frontier

#endif // INNOVA_DAG_TIP_FRONTIER_H