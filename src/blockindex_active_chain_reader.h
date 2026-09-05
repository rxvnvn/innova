// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.

// A.10.1k/D-prereq - By-value active-chain reader.
//
// Iterates the active chain by HEIGHT using by-value storage, so that
// historical block-bytes consumers (hreg rebuild, wallet rescan) can run
// WITHOUT the resident mapBlockIndex / pnext CBlockIndex pointer graph.
//
// Key primitives (source-proven):
//   - FixedBlockIndexShadowActiveLookup::LookupByHeight -> by-value
//     BlockIndexRecord{hash, height, nFile, nBlockPos, nTime, ...}
//   - CBlock::ReadFromDisk(nFile, nBlockPos) reads a full block by coordinates
//     WITHOUT a CBlockIndex* (main.h:1585).
//
// The reader holds NO resident CBlockIndex and NO O(N) cache: it resolves one
// active height at a time from active.dat/records.dat (O(1) per height, O(N)
// total disk work) and lets the caller read the block bytes by coordinates.
//
// When a by-value generation lookup is NOT open (legacy default startup), the
// caller falls back to the legacy resident path unchanged. This seam is
// additive: production default remains LEGACY_RESIDENT.

#ifndef INNOVA_BLOCKINDEX_ACTIVE_CHAIN_READER_H
#define INNOVA_BLOCKINDEX_ACTIVE_CHAIN_READER_H

#include "fixed_blockindex_store.h"
#include "blockindex_activeindex.h"

#include <stdint.h>
#include <string>

// By-value description of one active-chain block at a given height.
struct BlockIndexActiveBlock
{
    uint256    hash;
    int        height;
    unsigned int nFile;
    unsigned int nBlockPos;
    unsigned int nTime;

    BlockIndexActiveBlock() : hash(0), height(-1), nFile(0), nBlockPos(0), nTime(0) {}
};

// By-value reader over the active chain of a selected generation. No resident
// CBlockIndex, no pnext/p prev/pskip topology, no O(N) cache.
class BlockIndexActiveChainReader
{
public:
    BlockIndexActiveChainReader();

    // Open the by-value active-chain source from a generation directory
    // (gen-%06llu). Mutually consistent with records.dat + active.dat + MANIFEST.
    bool Open(const std::string& generationDir, uint64_t generation, std::string* error);
    void Close();
    bool IsOpen() const;

    // Highest committed active height (inclusive), or -1 if empty.
    int64_t GetActiveHeight() const;

    // Resolve one active height to by-value block description. Found=false if
    // height >= ActiveHeight or corrupt.
    bool LookupByHeight(int32_t height, BlockIndexActiveBlock* out, std::string* error) const;

private:
    FixedBlockIndexShadowActiveLookup lookup_;
    bool open_;
};

#endif // INNOVA_BLOCKINDEX_ACTIVE_CHAIN_READER_H