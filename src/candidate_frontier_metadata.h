#ifndef INNOVA_CANDIDATE_FRONTIER_METADATA_H
#define INNOVA_CANDIDATE_FRONTIER_METADATA_H

#include <stdint.h>
#include <string>
#include <vector>
#include "uint256.h"

static const char* const BLOCK_INDEX_CANDIDATE_LEAVES_FILE_NAME = "candidate-leaves.dat";

bool EnsureCandidateLeafMetadata(const std::string& generationDir,
                                 uint64_t generation,
                                 std::string* error);
bool ReadCandidateLeafMetadata(const std::string& generationDir,
                               uint64_t generation,
                               std::vector<uint256>* leaves,
                               std::string* error);

// Canonical binding consumed by both generation publication and validation.
bool ComputeCandidateLeavesBinding(const std::string& generationDir,
                                   uint64_t generation,
                                   unsigned char out[32],
                                   std::string* error);

bool MixCandidateLeavesIntoDagDigest(const unsigned char dagInputDigest[32],
                                     const unsigned char candidateLeavesBinding[32],
                                     unsigned char mixedDigest[32]);

#endif
