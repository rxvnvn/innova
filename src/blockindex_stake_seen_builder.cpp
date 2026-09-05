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

    const uint64_t count = reader.RecordCount();
    for (BlockIndexId id = 1; id <= count; ++id)
    {
        BlockIndexSnapshot snap;
        std::string rerr;
        BlockIndexV2ReadStatus st = reader.GetRecordById(id, &snap, &rerr);
        if (st != BLOCK_INDEX_V2_READ_FOUND)
        {
            if (error) *error = "stake_seen builder: corrupt record id=" +
                std::to_string((uint64_t)id) + ": " + rerr;
            return false;
        }
        if (!(snap.nFlags & CBlockIndex::BLOCK_PROOF_OF_STAKE))
            continue;
        out->insert(std::make_pair(snap.prevoutStake, snap.nStakeTime));
    }
    return true;
}