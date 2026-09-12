// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.
//
// R2c.1c — DAG tip frontier generation-lifecycle metadata.
//
// Binds the immutable dag-tip-frontier.dat artifact into the generation-root
// contract in the same domain-separated style as candidate-leaves, so a
// frontier-capable generation's root commits to its frontier and validation can
// recompute it independently, while legacy generations (no frontier file) keep
// an unchanged root.

#ifndef INNOVA_DAG_TIP_FRONTIER_METADATA_H
#define INNOVA_DAG_TIP_FRONTIER_METADATA_H

#include <stdint.h>
#include <string>

#include "uint256.h"

static const char* const BLOCK_INDEX_DAG_TIP_FRONTIER_FILE_NAME = "dag-tip-frontier.dat";

// Build dag-tip-frontier.dat inside generationDir from the daglinks LevelDB
// source, binding it to generationId + dagInputDigest. Returns false (with a
// typed error) on any authoritative deficiency so publication can abort.
bool EnsureDagTipFrontierMetadata(const std::string& dagLinksDir,
                                  const std::string& generationDir,
                                  uint64_t generationId,
                                  const unsigned char dagInputDigest[32],
                                  std::string* error);

// Canonical 32-byte binding of a present frontier artifact for root folding.
// Reads the artifact header and binds (generation || dagInputDigest ||
// frontierDigest || tipCount). Returns false when the artifact is absent or
// unreadable so validation can fail closed for a required capability.
bool ComputeDagTipFrontierBinding(const std::string& generationDir,
                                  uint64_t generationId,
                                  const unsigned char dagInputDigest[32],
                                  unsigned char out[32],
                                  std::string* error);

// Domain-separated mix: binding = SHA256(0x86 || dagInputDigest || frontierBinding).
bool MixDagTipFrontierIntoDigest(const unsigned char dagInputDigest[32],
                                 const unsigned char frontierBinding[32],
                                 unsigned char mixedDigest[32]);

// Typed frontier capability query for the future consumer.
enum DagTipFrontierCapability
{
    DAG_TIP_FRONTIER_CAPABILITY_PRESENT_VALID = 0,  // artifact present + bound + integrity passes
    DAG_TIP_FRONTIER_CAPABILITY_LEGACY_UNAVAILABLE, // legacy generation never had a frontier
    DAG_TIP_FRONTIER_CAPABILITY_CORRUPT,            // declared but corrupt/mismatched
    DAG_TIP_FRONTIER_CAPABILITY_AUTHORITY_FAILURE   // I/O / generation mismatch / authority error
};

// Read-only capability query. `declaredGenerationCapability` is the manifest
// capability, not an inference from artifact presence: only a generation that
// explicitly declares AUTHORITATIVE_FRONTIER may require this artifact.
DagTipFrontierCapability QueryDagTipFrontierCapability(
    const std::string& generationDir, uint64_t expectedGeneration,
    uint32_t declaredGenerationCapability,
    const unsigned char expectedDagInputDigest[32], std::string* detail);

#endif // INNOVA_DAG_TIP_FRONTIER_METADATA_H