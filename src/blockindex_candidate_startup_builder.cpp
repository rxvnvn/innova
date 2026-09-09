// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.

#include "blockindex_candidate_startup_builder.h"
#include "candidate_frontier_metadata.h"
#include "main.h"

#include <stdint.h>
#include <set>
#include <string>

#include <boost/filesystem.hpp>
#include <leveldb/cache.h>
#include <leveldb/db.h>
#include <leveldb/filter_policy.h>
#include <unistd.h>

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

    const BlockIndexSnapshot bestTip = reader.GetTip();
    const BlockIndexId bestId = bestTip.id;
    const uint256 bestTipHash = bestTip.hash;

    // New generations carry the immutable static leaf frontier. This path is
    // O(frontier) at startup; old generations fall through to the bounded
    // external parent-marker compatibility path below.
    std::vector<uint256> persistedLeaves;
    std::string leafError;
    if (ReadCandidateLeafMetadata(reader.GenerationPath(), reader.Generation(),
                                   &persistedLeaves, &leafError))
    {
        SnapshotCandidateFrontierStore out;
        for (size_t i = 0; i < persistedLeaves.size(); ++i)
        {
            BlockIndexSnapshot snap;
            std::string rerr;
            if (reader.LookupByHash(persistedLeaves[i], &snap, &rerr) != BLOCK_INDEX_V2_READ_FOUND)
            {
                if (error) *error = "candidate builder: persisted leaf lookup failed: " + rerr;
                return false;
            }
            BlockIndexDerivedEntry de;
            std::string derr;
            if (derived.Read(snap.id, &de, &derr) != BLOCK_INDEX_DERIVED_LOOKUP_FOUND)
            {
                if (error) *error = "candidate builder: persisted leaf derived lookup failed: " + derr;
                return false;
            }
            out.AddBlock(snap.hash, snap.hashPrev, de.chainTrust, snap.height);
            out.tipHashes.push_back(snap.hash);
            if (setInvalidBlockHash.count(snap.hash)) out.operatorInvalid.insert(snap.hash);
            if (de.flags & BLOCK_INDEX_DERIVED_FLAG_HAS_BLOCK_SIZE) out.hasData.insert(snap.hash);
        }
        if (bestId != 0)
        {
            BlockIndexDerivedEntry de; std::string derr;
            if (derived.Read(bestId, &de, &derr) != BLOCK_INDEX_DERIVED_LOOKUP_FOUND)
            {
                if (error) *error = "candidate builder: best-tip derived lookup failed: " + derr;
                return false;
            }
            out.SetBest(bestTipHash, de.chainTrust);
        }
        *store = out;
        return true;
    }

    // generation. Keep that relation in a bounded-cache temporary LevelDB,
    // rather than retaining N snapshots and N parent hashes in RAM.
    namespace fs = boost::filesystem;
    boost::system::error_code ec;
    const fs::path tmpDir = fs::temp_directory_path(ec) /
        fs::unique_path("innova-candidate-leaves-%%%%-%%%%", ec);
    if (ec || !fs::create_directories(tmpDir, ec))
    {
        if (error) *error = "candidate builder: create external marker failed";
        return false;
    }
    const fs::path markerPath = tmpDir / "parents";
    leveldb::Options options;
    options.create_if_missing = true;
    options.error_if_exists = true;
    options.filter_policy = leveldb::NewBloomFilterPolicy(10);
    options.block_cache = leveldb::NewLRUCache(512 * 1024);
    options.write_buffer_size = 1 * 1024 * 1024;
    options.max_open_files = 64;
    leveldb::DB* marker = NULL;
    leveldb::Status openStatus = leveldb::DB::Open(options, markerPath.string(), &marker);
    if (!openStatus.ok())
    {
        fs::remove_all(tmpDir, ec);
        if (error) *error = "candidate builder: external marker open failed: " + openStatus.ToString();
        return false;
    }
    const auto fail = [&](const std::string& message) -> bool {
        delete marker;
        marker = NULL;
        fs::remove_all(tmpDir, ec);
        if (error) *error = message;
        return false;
    };

    leveldb::WriteBatch batch;
    unsigned int batchCount = 0;
    for (BlockIndexId id = 1; id <= count; ++id)
    {
        BlockIndexSnapshot snap;
        std::string rerr;
        if (reader.GetRecordById(id, &snap, &rerr) != BLOCK_INDEX_V2_READ_FOUND)
            return fail("candidate builder: corrupt record id=" +
                        std::to_string((uint64_t)id) + ": " + rerr);
        if (snap.hashPrev == uint256(0))
            continue;
        const leveldb::Slice key((const char*)snap.hashPrev.begin(), 32);
        batch.Put(key, leveldb::Slice());
        if (++batchCount >= 4096)
        {
            leveldb::Status st = marker->Write(leveldb::WriteOptions(), &batch);
            if (!st.ok())
                return fail("candidate builder: external marker write failed: " + st.ToString());
            batch.Clear();
            batchCount = 0;
        }
    }
    if (batchCount != 0)
    {
        leveldb::Status st = marker->Write(leveldb::WriteOptions(), &batch);
        if (!st.ok())
            return fail("candidate builder: external marker final write failed: " + st.ToString());
    }

    SnapshotCandidateFrontierStore out;
    out.hasBest = false;
    for (BlockIndexId id = 1; id <= count; ++id)
    {
        BlockIndexSnapshot snap;
        std::string rerr;
        if (reader.GetRecordById(id, &snap, &rerr) != BLOCK_INDEX_V2_READ_FOUND)
            return fail("candidate builder: corrupt record id=" +
                        std::to_string((uint64_t)id) + ": " + rerr);
        const leveldb::Slice key((const char*)snap.hash.begin(), 32);
        std::string ignored;
        if (marker->Get(leveldb::ReadOptions(), key, &ignored).ok())
            continue; // referenced by another immutable record

        BlockIndexDerivedEntry de;
        std::string derr;
        if (derived.Read(id, &de, &derr) != BLOCK_INDEX_DERIVED_LOOKUP_FOUND)
            return fail("candidate builder: missing derived trust id=" +
                        std::to_string((uint64_t)id) + ": " + derr);
        out.AddBlock(snap.hash, snap.hashPrev, de.chainTrust, snap.height);
        out.tipHashes.push_back(snap.hash);
        if (setInvalidBlockHash.count(snap.hash))
            out.operatorInvalid.insert(snap.hash);
        if (de.flags & BLOCK_INDEX_DERIVED_FLAG_HAS_BLOCK_SIZE)
            out.hasData.insert(snap.hash);
    }

    if (bestId != 0)
    {
        BlockIndexDerivedEntry de;
        std::string derr;
        if (derived.Read(bestId, &de, &derr) != BLOCK_INDEX_DERIVED_LOOKUP_FOUND)
            return fail("candidate builder: missing best-tip derived trust: " + derr);
        out.SetBest(bestTipHash, de.chainTrust);
    }

    delete marker;
    marker = NULL;
    fs::remove_all(tmpDir, ec);
    *store = out;
    return true;
}