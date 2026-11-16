// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.

#include "blockindex_candidate_startup_builder.h"
#include "candidate_frontier_metadata.h"
#include "main.h"

#include <stdint.h>
#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <string>

#include <boost/filesystem.hpp>
#include <leveldb/cache.h>
#include <leveldb/db.h>
#include <leveldb/filter_policy.h>
#include <unistd.h>
#include <leveldb/write_batch.h>

BlockIndexCandidateStartupBuilder::BlockIndexCandidateStartupBuilder()
{
}

bool BlockIndexCandidateStartupBuilder::Build(const BlockIndexV2Reader& reader,
                                              BlockIndexScanProjection projection,
                                              const BlockIndexDerivedStateStore& derived,
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
        const size_t leafCount = persistedLeaves.size();

        // BOOT-004: the reader's open-time integrity scans already decode EVERY
        // record and EVERY derived entry of this generation. When they collected
        // exactly this leaf set, the frontier is assembled from those projections
        // and the builder performs NO random I/O. `projection` was handed over by
        // the reader (one-shot) and is consumed here.
        //
        // The previously used per-leaf probes cost 3.06 read syscalls and
        // 6.85 ms per leaf on a cold 8.24M generation (measured: 93 746
        // syscalls / 209.9 s for the hashindex+record probes, plus a duplicate
        // derived read of 31 023 syscalls / 39.2 s). Those probes answered a
        // question the integrity scans had just answered for free.
        //
        // The lookup path below is kept as the honest FALLBACK (verify-once memo
        // short-circuited the scan, no derived pass, or a generation without
        // derived data). Both paths produce the same store: results are placed
        // at their ORIGINAL leaf index, so the frontier order and content are
        // independent of RecordId ordering and of which path ran.
        // A projection that does not EXACTLY answer this leaf set of THIS
        // generation is not usable: fall back to lookups (never treat an absent
        // projection as an empty frontier).
        const bool usedScan = projection.Complete(leafCount, reader.Generation());
        const std::vector<BlockIndexScanRecordMatch>& recMatches = projection.records;
        const std::vector<BlockIndexScanDerivedMatch>& derMatches = projection.derived;

        if (usedScan)
        {
            std::vector<int32_t> heights(leafCount, 0);
            std::vector<uint256> hashesPrev(leafCount);
            std::vector<uint256> trust(leafCount);
            std::vector<uint32_t> flagsOf(leafCount, 0);
            std::vector<unsigned char> filled(leafCount, 0);
            std::map<BlockIndexId, size_t> idToLeaf;
            for (size_t mi = 0; mi < recMatches.size(); ++mi)
            {
                const BlockIndexScanRecordMatch& m = recMatches[mi];
                std::vector<uint256>::const_iterator it =
                    std::lower_bound(persistedLeaves.begin(), persistedLeaves.end(), m.record.hash);
                if (it == persistedLeaves.end() || *it != m.record.hash)
                {
                    if (error) *error = "candidate builder: scanned record absent from the persisted leaf set (id=" +
                                        std::to_string((uint64_t)m.id) + ")";
                    return false;
                }
                const size_t li = (size_t)(it - persistedLeaves.begin());
                heights[li] = m.record.height;
                hashesPrev[li] = m.record.hashPrev;
                filled[li] = 1;
                idToLeaf[m.id] = li;
            }
            for (size_t mi = 0; mi < derMatches.size(); ++mi)
            {
                std::map<BlockIndexId, size_t>::const_iterator it = idToLeaf.find(derMatches[mi].id);
                if (it == idToLeaf.end())
                {
                    if (error) *error = "candidate builder: derived match without a scanned record (id=" +
                                        std::to_string((uint64_t)derMatches[mi].id) + ")";
                    return false;
                }
                trust[it->second] = derMatches[mi].entry.chainTrust;
                flagsOf[it->second] = derMatches[mi].entry.flags;
            }
            for (size_t i = 0; i < leafCount; ++i)
            {
                if (!filled[i])
                {
                    if (error) *error = "candidate builder: persisted leaf was not found by the integrity scan (" +
                                        persistedLeaves[i].GetHex() + ")";
                    return false;
                }
                out.AddBlock(persistedLeaves[i], hashesPrev[i], trust[i], heights[i]);
                out.tipHashes.push_back(persistedLeaves[i]);
                if (setInvalidBlockHash.count(persistedLeaves[i]))
                    out.operatorInvalid.insert(persistedLeaves[i]);
                if (flagsOf[i] & BLOCK_INDEX_DERIVED_FLAG_HAS_BLOCK_SIZE)
                    out.hasData.insert(persistedLeaves[i]);
            }
        }
        else
        {
            // BOOT-003: issue the per-leaf lookups in hashindex KEY ORDER.
            //
            // The hashindex key is 'h' || hash and RecordIds are hash-sorted, so
            // ascending leaf hashes turn the probes into an ascending key/id
            // walk. NOTHING semantic changes: results are placed back at their
            // original leaf index.
            std::vector<uint32_t> order(persistedLeaves.size());
            for (uint32_t i = 0; i < (uint32_t)order.size(); ++i)
                order[i] = i;
            std::sort(order.begin(), order.end(),
                      [&persistedLeaves](uint32_t a, uint32_t b)
                      {
                          return persistedLeaves[a] < persistedLeaves[b];
                      });
            std::vector<BlockIndexSnapshot> snaps(persistedLeaves.size());
            std::vector<BlockIndexDerivedEntry> des(persistedLeaves.size());
            for (size_t oi = 0; oi < order.size(); ++oi)
            {
                const uint32_t i = order[oi];
                std::string rerr;
                if (reader.LookupByHash(persistedLeaves[i], &snaps[i], &rerr) != BLOCK_INDEX_V2_READ_FOUND)
                {
                    if (error) *error = "candidate builder: persisted leaf lookup failed: " + rerr;
                    return false;
                }
                std::string derr;
                if (derived.Read(snaps[i].id, &des[i], &derr) != BLOCK_INDEX_DERIVED_LOOKUP_FOUND)
                {
                    if (error) *error = "candidate builder: persisted leaf derived lookup failed: " + derr;
                    return false;
                }
            }
            for (size_t i = 0; i < persistedLeaves.size(); ++i)
            {
                const BlockIndexSnapshot& snap = snaps[i];
                const BlockIndexDerivedEntry& de = des[i];
                out.AddBlock(snap.hash, snap.hashPrev, de.chainTrust, snap.height);
                out.tipHashes.push_back(snap.hash);
                if (setInvalidBlockHash.count(snap.hash)) out.operatorInvalid.insert(snap.hash);
                if (de.flags & BLOCK_INDEX_DERIVED_FLAG_HAS_BLOCK_SIZE) out.hasData.insert(snap.hash);
            }
        }

        // Execution proof for the gate: which source served the frontier. A
        // silent revert to per-leaf probing is visible in the boot log itself.
        printf("BLOCKINDEX_CANDIDATE_FRONTIER leaf_source=%s leaves=%zu\n",
               usedScan ? "integrity_scan" : "hash_lookup", leafCount);
        fflush(stdout);

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

    printf("BLOCKINDEX_CANDIDATE_FRONTIER leaf_source=parent_marker leaves=%zu\n",
           out.tipHashes.size());
    fflush(stdout);

    delete marker;
    marker = NULL;
    fs::remove_all(tmpDir, ec);
    *store = out;
    return true;
}
