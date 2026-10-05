#ifndef INNOVA_BLOCKINDEX_STARTUP_OWNERSHIP_H
#define INNOVA_BLOCKINDEX_STARTUP_OWNERSHIP_H

#include <stdint.h>
#include <string>

#include "blockindex_generation_lifecycle.h"

// PM1-P0-07 / P0-07b: the SINGLE explicit production startup block-index
// OWNERSHIP decision.
//
// Normal production block-index authority is V2 ONLY. Legacy persistence may
// survive only as MIGRATION SOURCE / COMPATIBILITY MIRROR / DIAGNOSTIC INPUT.
//
// The durable ownership commit point is the EXISTING BlockIndexGenerationManager
// CURRENT marker (published by SelectGeneration). No new durable marker exists:
//   CURRENT absent  -> V2 ownership NOT published (legacy may still be source)
//   CURRENT valid   -> V2 owns authority
//   CURRENT corrupt -> fail-closed corruption classification
//
// This classifier only READS durable state; it never mutates/creates storage.

enum BlockIndexStartupOwnershipState
{
    BLOCK_INDEX_STARTUP_OWNERSHIP_EMPTY_NEW = 0,                // no V2, no legacy
    BLOCK_INDEX_STARTUP_OWNERSHIP_LEGACY_MIGRATION_REQUIRED = 1, // no V2, legacy present -> migrate
    BLOCK_INDEX_STARTUP_OWNERSHIP_V2_AUTHORITATIVE = 2,          // CURRENT valid -> V2 owns
    BLOCK_INDEX_STARTUP_OWNERSHIP_V2_RECOVERY_REQUIRED = 3,      // V2 published but recoverable defect
    BLOCK_INDEX_STARTUP_OWNERSHIP_FATAL_CORRUPTION = 4           // V2 present-but-corrupt/incompatible -> fail closed
};

const char* BlockIndexStartupOwnershipStateName(BlockIndexStartupOwnershipState state);

struct BlockIndexStartupOwnershipDecision
{
    BlockIndexStartupOwnershipState state;

    // V2 signals
    BlockIndexLifecycleStatus v2CurrentStatus; // ReadCurrent() result
    uint64_t v2Generation;                     // selected generation if status OK
    bool v2Valid;                              // CURRENT valid + selected generation validated
    bool unpublishedArtifacts;                 // generation artifacts exist but CURRENT absent

    // legacy signals
    bool legacyPresent;   // a legacy block index store exists under dataDir
    bool legacyReadable;  // legacy store is present (readability probed non-destructively)

    std::string detail;

    BlockIndexStartupOwnershipDecision()
        : state(BLOCK_INDEX_STARTUP_OWNERSHIP_FATAL_CORRUPTION),
          v2CurrentStatus(BLOCK_INDEX_LIFECYCLE_NOT_PUBLISHED),
          v2Generation(0),
          v2Valid(false),
          unpublishedArtifacts(false),
          legacyPresent(false),
          legacyReadable(false)
    {
    }
};

// One explicit ownership decision before legacy can become authority.
// v2Root : the generation root directory (holds CURRENT + gen-N).
// dataDir: the node datadir (legacy block index lives under dataDir/txleveldb).
// Returns true when a definitive classification was produced (out->state is
// then authoritative); false only on a hard probe error (out updated, error set).
bool ClassifyBlockIndexStartupOwnership(const std::string& v2Root,
                                        const std::string& dataDir,
                                        BlockIndexStartupOwnershipDecision* out,
                                        std::string* error);

// PM1-P0-07b: owner-approved CANONICAL production V2 root.
//
//   <effective network datadir>   (mainnet ~/.innova; regtest ~/.innova/regtest)
//
// The authoritative block-index state lives DIRECTLY under the effective network
// datadir; the C1 lifecycle files (blockindex-current, blockindex-gen-%06llu,
// blockindex-build-%06llu.tmp) are already namespace-safe. There is NO permanent
// blockindex-v2/ or blockindex/ container. Callers pass the effective datadir
// (GetDataDir(), which is network-specific) as dataDir.
//
// This ONE root is used for: startup ownership classification, generation
// manager/CURRENT discovery, native fresh/legacy bootstrap output, generation
// publication and authoritative V2 startup. The -blockindexv2authoritative
// flag remains an explicit diagnostic/developer OVERRIDE only; normal
// production correctness must not depend on it.
//
// Precedence helper: returns the override when non-empty, else the canonical
// default. Callers should use this single resolver so every subsystem agrees.
std::string ResolveBlockIndexV2Root(const std::string& overrideRoot,
                                    const std::string& dataDir);

// The canonical default root alone (no override).
std::string GetDefaultBlockIndexV2Root(const std::string& dataDir);

#endif
