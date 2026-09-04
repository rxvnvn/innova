// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef INNOVA_BLOCKINDEX_AUTHORITY_MATERIALIZER_H
#define INNOVA_BLOCKINDEX_AUTHORITY_MATERIALIZER_H

#include "blockindex_v2_reader.h"
#include "blockindex_derived_state.h"
#include "blockindex_hot_owner.h"
#include "blockindex_navigation.h"

#include <stdint.h>

/**
 * Production BlockIndexHotMaterializer backed by real V2 generation metadata.
 *
 * This materializer reconstructs a sparse-hot CBlockIndex from authoritative
 * by-value V2 storage (records.dat + derived.dat) WITHOUT depending on any
 * resident historical CBlockIndex* or mapBlockIndex pointer topology.
 *
 * Architecture:
 *   logical hash
 *       ↓
 *   BlockIndexV2Reader (hashindex → RecordId → record)
 *       ↓
 *   BlockIndexDerivedStateStore (RecordId → chainTrust, blockSize, stake modifier)
 *       ↓
 *   BlockIndexSnapshot (by-value layer-1 metadata)
 *       ↓
 *   BlockIndexHotMaterialized
 *       ↓
 *   BlockIndexHotOwner → BlockIndexHotHandle → production consumer
 *
 * Invariants:
 * - No mapBlockIndex lookup
 * - No CBlockIndex* pointer dependency
 * - No recursive ancestry materialization
 * - No silent fallback to legacy
 * - Materializer I/O occurs OUTSIDE HotOwner lock (split-lock preserved)
 * - Generation is explicit and bound to the opened V2 generation
 */
class BlockIndexAuthorityMaterializer : public BlockIndexHotMaterializer
{
public:
    /**
     * Construct from generation-bound V2 components.
     *
     * @param reader        Open BlockIndexV2Reader (must be open, generation-bound)
     * @param derivedStore  Open BlockIndexDerivedStateStore (same generation as reader)
     * @param generation    Generation number (must match reader.Generation() and derivedStore.Generation())
     *
     * All references are non-owning and must outlive this materializer.
     * The materializer adds O(1) state — no resident object cache, no pin table.
     */
    BlockIndexAuthorityMaterializer(const BlockIndexV2Reader* reader,
                                    const BlockIndexDerivedStateStore* derivedStore,
                                    uint64_t generation);

    virtual ~BlockIndexAuthorityMaterializer() {}

    /**
     * Resolve a logical block hash into by-value index metadata.
     *
     * This method runs OUTSIDE the HotOwner lock (A.10.1d/e split-lock).
     * It performs V2 storage reads (reader cache + derived store) to construct
     * a complete BlockIndexSnapshot and derived fields.
     *
     * @param id   Logical block identity (stable hash)
     * @param out  Materialized result on success
     * @return     Typed status — preserves failure class for diagnosis
     */
    virtual BlockIndexHotStatus Materialize(const BlockIndexLogicalId& id,
                                            BlockIndexHotMaterialized* out) const override;

    /**
     * Generation this materializer is bound to.
     * Must match HotOwner.SetCurrentGeneration() for publish to succeed.
     */
    uint64_t Generation() const { return generation_; }

private:
    const BlockIndexV2Reader* reader_;
    const BlockIndexDerivedStateStore* derivedStore_;
    uint64_t generation_;

    // Map V2 read status to HotOwner materialization status
    static BlockIndexHotStatus MapV2ReadStatus(BlockIndexV2ReadStatus status);
    // Map derived lookup status to HotOwner materialization status
    static BlockIndexHotStatus MapDerivedStatus(BlockIndexDerivedLookupStatus status);

    // Convert V2 record + derived entry to BlockIndexSnapshot
    bool PopulateSnapshot(BlockIndexId recordId,
                          const BlockIndexDerivedEntry& derivedEntry,
                          BlockIndexSnapshot& snapshot) const;
};

#endif // INNOVA_BLOCKINDEX_AUTHORITY_MATERIALIZER_H