// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.

#include "blockindex_candidate_startup_builder.h"
#include "main.h"

#include <stdint.h>
#include <set>
#include <string>

BlockIndexCandidateStartupBuilder::BlockIndexCandidateStartupBuilder()
{
}

bool BlockIndexCandidateStartupBuilder::Build(const BlockIndexV2Reader& reader,
                                              const BlockIndexDerivedStateStore& derived,
                                              int forkHeightDAG,
                                              SnapshotCandidateFrontierStore* store,
                                              std::string* error) const
{
    if (!store)
        return false;
    if (error) error->clear();

    if (!reader.IsOpen() || !derived.IsOpen())
    {
        if (error) *error = "candidate builder: reader/derived not open";
        return false;
    }
    if (reader.Generation() != derived.Generation())
    {
        if (error) *error = "candidate builder: reader/derived generation mismatch";
        return false;
    }
    if (reader.RecordCount() != derived.EntryCount())
    {
        if (error) *error = "candidate builder: record/derived count mismatch";
        return false;
    }

    const uint64_t count = reader.RecordCount();
    (void)forkHeightDAG; // era-independent tip/tracking semantics

    // Phase 1: scan ALL records by value (active + side branches), collect
    // parent hashes. Transient O(N) compact scalars (hashPrev set) - reported.
    const BlockIndexId bestId = reader.GetTip().id;
    const uint256 bestTipHash = reader.GetTip().hash;

    // setReferenced: every hash that is some block's parent.
    std::set<uint256> referenced;
    // hash -> RecordId (for fork + trust resolution).
    std::map<BlockIndexId, BlockIndexSnapshot> byId;

    for (BlockIndexId id = 1; id <= count; ++id)
    {
        BlockIndexSnapshot snap;
        std::string rerr;
        BlockIndexV2ReadStatus st = reader.GetRecordById(id, &snap, &rerr);
        if (st != BLOCK_INDEX_V2_READ_FOUND)
        {
            if (error) *error = "candidate builder: corrupt record id=" + std::to_string((uint64_t)id) + ": " + rerr;
            return false;
        }
        byId[id] = snap;
        if (!(snap.hashPrev == uint256(0)))
            referenced.insert(snap.hashPrev);
    }

    // Phase 2: tips = blocks not referenced as any parent.
    SnapshotCandidateFrontierStore out;
    out.hasBest = false;

    for (const auto& kv : byId)
    {
        const BlockIndexSnapshot& snap = kv.second;
        if (referenced.count(snap.hash))
            continue; // not a tip

        // trust from derived store
        BlockIndexDerivedEntry de;
        std::string derr;
        BlockIndexDerivedLookupStatus dst = derived.Read(kv.first, &de, &derr);
        if (dst != BLOCK_INDEX_DERIVED_LOOKUP_FOUND)
        {
            if (error) *error = "candidate builder: missing derived trust id=" + std::to_string((uint64_t)kv.first);
            return false;
        }

        // fork/ancestry is captured by the by-value store's `parent` field;
        // the evaluator reproduces fork semantics via GetParent walks. No
        // historical pprev topology is required (M-GATE-3/6).

        out.AddBlock(snap.hash, snap.hashPrev, de.chainTrust, snap.height);
        out.tipHashes.push_back(snap.hash);

        if (setInvalidBlockHash.count(snap.hash))
            out.operatorInvalid.insert(snap.hash);
        if (de.flags & BLOCK_INDEX_DERIVED_FLAG_HAS_BLOCK_SIZE)
            out.hasData.insert(snap.hash);
    }

    // Best tip + best trust (nBestChainTrust threshold).
    if (bestId != 0 && byId.count(bestId))
    {
        BlockIndexDerivedEntry de;
        std::string derr;
        if (derived.Read(bestId, &de, &derr) == BLOCK_INDEX_DERIVED_LOOKUP_FOUND)
            out.SetBest(bestTipHash, de.chainTrust);
        else
            out.SetBest(bestTipHash, uint256(0));
    }

    *store = out;
    return true;
}