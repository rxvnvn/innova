// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.

// A.10.1o - By-value setStakeSeen builder.
//
// Reproduces the legacy setStakeSeen population that CTxDB::LoadBlockIndex
// performs during its full scan (txdb-leveldb.cpp:1045-1047):
//
//   for each blockindex record:
//     if (pindexNew->IsProofOfStake())            // nFlags & BLOCK_PROOF_OF_STAKE
//         setStakeSeen.insert(make_pair(prevoutStake, nStakeTime));
//
// without constructing the historical mapBlockIndex / CBlockIndex resident
// graph. The needed fields (prevoutStake, nStakeTime) are persisted in the V2
// BlockIndexRecord / BlockIndexSnapshot (fixed_blockindex_store.h:30,43;
// blockindex_accessor.h:42,43; blockindex_v2_reader.cpp:46-48), so a full
// by-value record scan (RecordCount + GetRecordById(1..N)) exactly reproduces
// the legacy universe (active + side PoS records) with NO block-by-bytes read
// and NO CBlockIndex residency.
//
// setStakeSeen is std::set<std::pair<COutPoint, unsigned int>> (main.h:366);
// std::set semantics: unique key, insertion order-independent (sorted set).
//
// This phase provides ONLY the by-value builder (additive substrate for future
// authoritative startup D). It does NOT change runtime setStakeSeen semantics
// and does NOT remove the legacy construction. Default stays LEGACY_RESIDENT.

#ifndef INNOVA_BLOCKINDEX_STAKE_SEEN_BUILDER_H
#define INNOVA_BLOCKINDEX_STAKE_SEEN_BUILDER_H

#include "blockindex_v2_reader.h"

#include <set>
#include <stdint.h>
#include <string>

// By-value startup setStakeSeen builder. Reads authoritative V2 records and
// builds the exact legacy stake-seen set WITHOUT mapBlockIndex/CBlockIndex.
class BlockIndexStakeSeenBuilder
{
public:
    BlockIndexStakeSeenBuilder();

    // Populate `out` with the exact legacy setStakeSeen contents for the
    // selected generation's full record set. Iterates all records by value;
    // for each PoS record (nFlags & BLOCK_PROOF_OF_STAKE) inserts
    // make_pair(prevoutStake, nStakeTime). Fails closed on read/generation
    // error; never falls back to legacy LoadBlockIndex.
    bool Build(const BlockIndexV2Reader& reader,
               std::set<std::pair<COutPoint, unsigned int> >* out,
               std::string* error) const;
};

#endif // INNOVA_BLOCKINDEX_STAKE_SEEN_BUILDER_H