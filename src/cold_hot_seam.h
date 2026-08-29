// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef INNOVA_COLD_HOT_SEAM_H
#define INNOVA_COLD_HOT_SEAM_H

#include "blockindex_accessor.h"
#include "blockindex_v2_reader.h"
#include "main.h"

/**
 * Cold/hot seam navigator — bridges pointer-free cold historical
 * BlockIndexV2Reader with hot live BlockIndexAccessor for forward
 * active-chain traversal.
 *
 * Cold domain = heights ≤ reader committedTipHeight (frozen generation).
 * Hot domain  = heights > reader tip, using live LegacyBlockIndexAccessor.
 *
 * Invariants:
 * 1. Seam check: cold at seam height == hot at same height.
 * 2. Extension-only assumption: cold prefix is an ancestor of the live chain.
 * 3. Stale generation: detected via CurrentSelectionChanged() + hash check.
 * 4. Reorg through/below seam: detected via hash mismatch at seam height.
 */
class ColdHotSeamNavigator
{
public:
    ColdHotSeamNavigator();

    /** Open the cold side with a V2 reader.  Hot side uses the global
     *  LegacyBlockIndexAccessor.  Returns false if cold reader fails. */
    bool Open(const std::string& v2Root, const BlockIndexV2ReaderOptions& options,
              std::string* error);

    /** Close cold reader. */
    void Close();

    /** Check if the cold generation is still valid relative to the live chain.
     *  Returns false if the generation is stale or the seam hash mismatches. */
    bool VerifySeam(std::string* error) const;

    /** Get the next active block after the given BlockIndexId.
     *  If the block is in the cold domain and below the seam, returns the
     *  cold successor.  If at the seam, verifies the seam and returns the
     *  hot successor.  If in the hot domain, returns the hot successor. */
    BlockIndexSnapshot GetNextActive(BlockIndexId id) const;

    /** Get a BlockIndexSnapshot by hash, checking cold first then hot. */
    BlockIndexSnapshot LookupByHash(const uint256& hash) const;

    /** Get parent of a block (cold reader first, then hot). */
    BlockIndexSnapshot GetParent(BlockIndexId id) const;

    /** Get ancestor at a target height. */
    BlockIndexSnapshot GetAncestor(BlockIndexId id, int targetHeight) const;

    /** Get the current cold generation tip. */
    BlockIndexSnapshot GetColdTip() const;

    /** Get the current hot tip. */
    BlockIndexSnapshot GetHotTip() const;

    /** Is the given height in the cold domain? */
    bool IsColdDomain(int height) const;

    /** Is the navigator open and valid? */
    bool IsOpen() const;

private:
    BlockIndexV2Reader coldReader;
    LegacyBlockIndexAccessor hotAccessor;
    bool open;
};

#endif // INNOVA_COLD_HOT_SEAM_H