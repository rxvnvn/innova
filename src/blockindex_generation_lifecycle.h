#ifndef INNOVA_BLOCKINDEX_GENERATION_LIFECYCLE_H
#define INNOVA_BLOCKINDEX_GENERATION_LIFECYCLE_H

#include "fixed_blockindex_store.h"
#include "blockindex_activeindex.h"

#include <stdint.h>
#include <string>

// Phase A.6: Block Index V2 generation lifecycle + atomic CURRENT publication.
//
// Model:
//   build-N.tmp  --PublishGeneration(N)-->  gen-N  --SelectGeneration(N)-->  CURRENT=A
//
// A COMPLETE generation is NOT authoritative merely because it exists.
// Publication and selection are separate operations. CURRENT is the sole
// explicit selector. No directory scan silently replaces CURRENT semantics.
// Legacy block index remains authoritative.

// ---- CURRENT V1 persistent format ----
// Fixed-size, little-endian, ABI-independent, independently versioned from
// records.dat/MANIFEST/active.dat/hashindex.
//
// Layout (28 bytes total):
//   [0]  / 8  / magic            "INNBCUR1"
//   [8]  / 4  / format_version   uint32 LE = 1
//   [12] / 4  / schema_version   uint32 LE = 1
//   [16] / 8  / generation       uint64 LE
//   [24] / 4  / checksum         CRC32 over bytes [0..24)
//
// The stable generation directory name (gen-%06llu) is strictly derived from
// `generation`, so it is not stored redundantly. generation 0 is forbidden.

static const uint32_t BLOCK_INDEX_CURRENT_FORMAT_VERSION = 1;
static const uint32_t BLOCK_INDEX_CURRENT_SCHEMA_VERSION = 1;
static const uint32_t BLOCK_INDEX_CURRENT_SIZE_V1 = 28;
static const char* const BLOCK_INDEX_CURRENT_FILE_NAME = "CURRENT";

struct BlockIndexCurrentRecord
{
    uint32_t formatVersion;
    uint32_t schemaVersion;
    uint64_t generation;

    BlockIndexCurrentRecord()
        : formatVersion(BLOCK_INDEX_CURRENT_FORMAT_VERSION),
          schemaVersion(BLOCK_INDEX_CURRENT_SCHEMA_VERSION),
          generation(0)
    {
    }
};

bool EncodeBlockIndexCurrentRecord(const BlockIndexCurrentRecord& record, std::string* out, std::string* error);
bool DecodeBlockIndexCurrentRecord(const char* data, size_t size, BlockIndexCurrentRecord* out, std::string* error);

// ---- lifecycle status ----
enum BlockIndexLifecycleStatus
{
    BLOCK_INDEX_LIFECYCLE_OK = 1,            // operation succeeded
    BLOCK_INDEX_LIFECYCLE_NOT_PUBLISHED = 2, // CURRENT absent -> no published generation
    BLOCK_INDEX_LIFECYCLE_CORRUPT = 3,       // CURRENT present but malformed
    BLOCK_INDEX_LIFECYCLE_MISSING_GENERATION = 4, // CURRENT points to a missing gen-N
    BLOCK_INDEX_LIFECYCLE_ERROR = 5,         // generic operational error
};

// ---------------------------------------------------------------------------
// Manager. All paths are generation-root relative and stay under `root`.
// ---------------------------------------------------------------------------
class BlockIndexGenerationManager
{
public:
    // Stable path for a generation id within root (gen-%06llu).
    static std::string GenerationName(uint64_t generation);
    static std::string GenerationPath(const std::string& root, uint64_t generation);
    // Staging path within root (build-%06llu.tmp).
    static std::string StagingName(uint64_t generation);
    static std::string StagingPath(const std::string& root, uint64_t generation);

    // Structural validation of a stable gen-N (also used on its staging clone).
    // Proves: dirname name matches, MANIFEST COMPLETE + generation, records/active
    // generation binding, committed tip coherence, component existence, read-only
    // reopen of readers, hashindex generation. Does NOT run the 8M differential.
    static BlockIndexLifecycleStatus ValidateGeneration(const std::string& root,
                                                        uint64_t generation,
                                                        std::string* error);

    // Publish: build-N.tmp -> gen-N (durable rename + reopen validation). Does
    // NOT change CURRENT. Fails if gen-N already exists (protects rollback).
    static BlockIndexLifecycleStatus PublishGeneration(const std::string& root,
                                                       uint64_t generation,
                                                       std::string* error);

    // Select: atomically write CURRENT = N, only after ValidateGeneration(N)
    // passes. Rollback = SelectGeneration(oldN) with the same validation.
    static BlockIndexLifecycleStatus SelectGeneration(const std::string& root,
                                                      uint64_t generation,
                                                      std::string* error);

    // Read CURRENT. Returns OK + out->generation, NOT_PUBLISHED (absent),
    // CORRUPT (malformed), MISSING_GENERATION (selects absent gen-N).
    static BlockIndexLifecycleStatus ReadCurrent(const std::string& root,
                                                 BlockIndexCurrentRecord* out,
                                                 std::string* error);

    // Open the generation selected by CURRENT: ReadCurrent + validate selected gen.
    static BlockIndexLifecycleStatus OpenCurrent(const std::string& root,
                                                 uint64_t* selectedGeneration,
                                                 std::string* error);

private:
    // Structural validation against a concrete generation directory (used for
    // both build-N.tmp and gen-N by the public wrappers).
    static BlockIndexLifecycleStatus ValidateGenerationDir(const std::string& generationDir,
                                                           uint64_t expectedGeneration,
                                                           bool requireStableName,
                                                           uint64_t generation,
                                                           std::string* error);
};

#endif