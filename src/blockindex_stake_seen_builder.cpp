// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.

#include "blockindex_stake_seen_builder.h"
#include "main.h"

#include <string>

BlockIndexStakeSeenBuilder::BlockIndexStakeSeenBuilder()
{
}

bool BlockIndexStakeSeenBuilder::Build(const BlockIndexV2Reader& reader,
                                       std::set<std::pair<COutPoint, unsigned int> >* out,
                                       std::string* error) const
{
    if (!out)
        return false;
    out->clear();
    if (error) error->clear();

    if (!reader.IsOpen())
    {
        if (error) *error = "stake_seen builder: reader not open";
        return false;
    }

    // T2 (Cohort T): this is a known full-generation sequential traversal. The
    // legacy loop called GetRecordById per record, paying the reader lock, an
    // LRU lookup/miss/insert (which the one-shot scan never reuses) and
    // active-membership work - none of which this predicate consumes. Stream
    // records instead. Semantics are identical: the loop inserted
    // (prevoutStake, nStakeTime) for every record carrying the PoS flag,
    // SnapshotFromRecord copies nFlags/prevoutStake/nStakeTime verbatim, the
    // legacy path never consulted active membership, and insertion into a
    // std::set makes RecordId order irrelevant.
    bool ok = reader.ForEachRecordSequential(
        [out](BlockIndexId, const BlockIndexRecord& rec) -> bool
        {
            if (rec.nFlags & CBlockIndex::BLOCK_PROOF_OF_STAKE)
                out->insert(std::make_pair(rec.prevoutStake, rec.nStakeTime));
            return true;
        }, error);
    if (!ok)
    {
        if (error && error->empty())
            *error = "stake_seen builder: sequential scan of records.dat failed";
        return false;
    }
    return true;
}