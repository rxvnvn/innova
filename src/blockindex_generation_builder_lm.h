// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.

#ifndef INNOVA_BLOCKINDEX_GENERATION_BUILDER_LM_H
#define INNOVA_BLOCKINDEX_GENERATION_BUILDER_LM_H

#include "blockindex_generation_writer.h"
#include "blockindex_generation_lifecycle.h"
#include "fixed_blockindex_store.h"

#include <cstdint>
#include <string>
#include <vector>

// Low-memory V1->V2 migration builder (M2-M6).
//
// Rebuilds an immutable V2 generation from a static legacy txleveldb snapshot
// with peak memory bounded INDEPENDENTLY of historical block count N. Replaces
// the current builder's approach (materialize all source records in RAM + copy
// into `ordered` + three hash maps + full DAG CBlockIndex graph) with a
// streamed, shared-writer pipeline:
//
//   legacy txleveldb iterator
//     -> decode CDiskBlockIndex one at a time -> BlockIndexGenerationWriter
//        (records.dat + hashindex, batched bounded-RAM)
//   active chain: tip->hashPrev walk via a temp hash->id index (LevelDB)
//     -> active.dat ascending (streamed)
//   derived state: height-ordered computation against the temp index
//     -> derived.dat (streamed)
//   DAG scores: height-ordered incremental coloring keeping only the
//     DAG_MERGE_DEPTH window + a transient CBlockIndex per block (NO N-object
//     graph) -> exact same ColorBlock/ColorBlockDAGKnight scores
//   digests: file-based (streamed stores) -> generation root -> MANIFEST
//   lifecycle: Publish -> Validate -> atomic Select (old CURRENT untouched)
//
// Byte/logical parity with the existing builder is the acceptance gate; the old
// builder is NOT retired until this proves parity on a controlled dataset.

class BlockIndexGenerationBuilderLM
{
public:
    BlockIndexGenerationBuilderLM();
    ~BlockIndexGenerationBuilderLM();

    // Build a COMPLETE generation at stagingDir (build-<gen>.tmp) from a
    // legacy txleveldb snapshot dir. Writer handles records/active/derived/
    // hashindex/manifest. capability flags whether block data (nSize) was
    // available for AUTHORITATIVE.
    //   snapshotLevelDbDir : the txleveldb snapshot (read-only).
    //   blockDataDir       : optional dir containing blk*.dat for exact nSize
    //                        (empty => OLD_SHADOW capability).
    // Returns false + error on any failure (partial staging is crash-unsafe but
    // life-cycle keeps CURRENT intact). On success *outStagingDir holds the
    // completed staging path (caller publishes via lifecycle or
    // ValidatePublishSelect).
    bool Build(const std::string& snapshotLevelDbDir,
               const std::string& blockDataDir,
               uint64_t generation,
               const std::string& stagingDir,
               std::string* error);

private:
    BlockIndexGenerationWriter writer_;
};

#endif // INNOVA_BLOCKINDEX_GENERATION_BUILDER_LM_H