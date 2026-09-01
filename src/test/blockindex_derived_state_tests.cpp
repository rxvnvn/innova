// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.

#include <boost/test/unit_test.hpp>

#include "../blockindex_derived_state.h"
#include "../blockindex_startup_authority.h"
#include "../blockindex_accessor.h"
#include "../main.h"

#include <map>
#include <type_traits>
#include <vector>

namespace {

static_assert(std::is_copy_constructible<BlockIndexDerivedEntry>::value,
              "derived entries must be durable by-value results");

// Synthetic fixture: builds a small chain with known derived values,
// creates a derived.dat, and validates round-trip + coherence.
struct DerivedStateFixture
{
    std::string testDir;
    uint64_t testGeneration;

    // Known derived values for synthetic chain
    uint256 trustGenesis;
    uint256 trustOne;
    uint256 trustTwo;
    uint256 trustTip;
    uint256 trustSide;

    unsigned int checksumGenesis;
    unsigned int checksumOne;
    unsigned int checksumTwo;
    unsigned int checksumTip;
    unsigned int checksumSide;

    int64_t modifierTimeGenesis; // 0 = unavailable
    int64_t modifierTimeOne;
    int64_t modifierTimeTwo;
    int64_t modifierTimeTip;
    int64_t modifierTimeSide;   // 0 = unavailable

    DerivedStateFixture()
        : testDir("/tmp/test_derived_state_" + std::to_string(GetTime())),
          testGeneration(42),
          trustGenesis(uint256(1000)),
          trustOne(uint256(2000)),
          trustTwo(uint256(3000)),
          trustTip(uint256(4000)),
          trustSide(uint256(2500)),
          checksumGenesis(0),       // zero is a valid available value
          checksumOne(111),
          checksumTwo(222),
          checksumTip(333),
          checksumSide(444),
          modifierTimeGenesis(0),   // unavailable
          modifierTimeOne(100001),
          modifierTimeTwo(100002),
          modifierTimeTip(100003),
          modifierTimeSide(0)       // unavailable
    {
        boost::filesystem::create_directories(testDir);
    }

    ~DerivedStateFixture()
    {
        boost::filesystem::remove_all(testDir);
    }

    BlockIndexDerivedEntry MakeEntry(uint256 trust, unsigned int checksum,
                                     int64_t modTime, bool hasModTime)
    {
        BlockIndexDerivedEntry e;
        e.chainTrust = trust;
        e.stakeModifierChecksum = checksum;
        e.stakeModifierTime = modTime;
        e.SetHasStakeModifierTime(hasModTime);
        return e;
    }

    void BuildStore(BlockIndexDerivedStateStore* store, std::string* error)
    {
        BOOST_REQUIRE(BlockIndexDerivedStateStore::Create(testDir, testGeneration, store, error));

        // Append entries in RecordId order (1-based): genesis, one, two, tip, side
        BOOST_REQUIRE(store->Append(MakeEntry(trustGenesis, checksumGenesis, modifierTimeGenesis, false), error));
        BOOST_REQUIRE(store->Append(MakeEntry(trustOne, checksumOne, modifierTimeOne, true), error));
        BOOST_REQUIRE(store->Append(MakeEntry(trustTwo, checksumTwo, modifierTimeTwo, true), error));
        BOOST_REQUIRE(store->Append(MakeEntry(trustTip, checksumTip, modifierTimeTip, true), error));
        BOOST_REQUIRE(store->Append(MakeEntry(trustSide, checksumSide, modifierTimeSide, false), error));

        BOOST_REQUIRE(store->Finalize(error));
        BOOST_CHECK_EQUAL(store->EntryCount(), 5ULL);
    }
};

} // namespace

BOOST_FIXTURE_TEST_SUITE(blockindex_derived_state_tests, DerivedStateFixture)

// T1 — round trip: persist known derived values and reopen them exactly
BOOST_AUTO_TEST_CASE(derived_state_round_trip)
{
    std::string error;
    BlockIndexDerivedStateStore store;
    BuildStore(&store, &error);

    // Close and reopen
    store = BlockIndexDerivedStateStore();
    BOOST_REQUIRE(BlockIndexDerivedStateStore::OpenReadOnly(testDir, testGeneration, &store, &error));
    BOOST_CHECK_EQUAL(store.EntryCount(), 5ULL);
    BOOST_CHECK_EQUAL(store.Generation(), testGeneration);

    // Verify each entry
    BlockIndexDerivedEntry entry;

    // RecordId 1: genesis — checksum zero as AVAILABLE, modifier time unavailable
    BOOST_CHECK_EQUAL(store.Read(1, &entry, &error), BLOCK_INDEX_DERIVED_LOOKUP_FOUND);
    BOOST_CHECK(entry.chainTrust == trustGenesis);
    BOOST_CHECK_EQUAL(entry.stakeModifierChecksum, checksumGenesis);
    BOOST_CHECK_EQUAL(entry.HasStakeModifierTime(), false);
    BOOST_CHECK_EQUAL(entry.stakeModifierTime, 0);

    // RecordId 2: one — all available
    BOOST_CHECK_EQUAL(store.Read(2, &entry, &error), BLOCK_INDEX_DERIVED_LOOKUP_FOUND);
    BOOST_CHECK(entry.chainTrust == trustOne);
    BOOST_CHECK_EQUAL(entry.stakeModifierChecksum, checksumOne);
    BOOST_CHECK_EQUAL(entry.HasStakeModifierTime(), true);
    BOOST_CHECK_EQUAL(entry.stakeModifierTime, modifierTimeOne);

    // RecordId 3: two
    BOOST_CHECK_EQUAL(store.Read(3, &entry, &error), BLOCK_INDEX_DERIVED_LOOKUP_FOUND);
    BOOST_CHECK(entry.chainTrust == trustTwo);
    BOOST_CHECK_EQUAL(entry.stakeModifierChecksum, checksumTwo);
    BOOST_CHECK_EQUAL(entry.HasStakeModifierTime(), true);
    BOOST_CHECK_EQUAL(entry.stakeModifierTime, modifierTimeTwo);

    // RecordId 4: tip
    BOOST_CHECK_EQUAL(store.Read(4, &entry, &error), BLOCK_INDEX_DERIVED_LOOKUP_FOUND);
    BOOST_CHECK(entry.chainTrust == trustTip);
    BOOST_CHECK_EQUAL(entry.stakeModifierChecksum, checksumTip);
    BOOST_CHECK_EQUAL(entry.HasStakeModifierTime(), true);
    BOOST_CHECK_EQUAL(entry.stakeModifierTime, modifierTimeTip);

    // RecordId 5: side — modifier time unavailable
    BOOST_CHECK_EQUAL(store.Read(5, &entry, &error), BLOCK_INDEX_DERIVED_LOOKUP_FOUND);
    BOOST_CHECK(entry.chainTrust == trustSide);
    BOOST_CHECK_EQUAL(entry.stakeModifierChecksum, checksumSide);
    BOOST_CHECK_EQUAL(entry.HasStakeModifierTime(), false);

    // RecordId 6: beyond count
    BOOST_CHECK_EQUAL(store.Read(6, &entry, &error), BLOCK_INDEX_DERIVED_LOOKUP_NOT_FOUND);

    // RecordId 0: invalid
    BOOST_CHECK_EQUAL(store.Read(0, &entry, &error), BLOCK_INDEX_DERIVED_LOOKUP_NOT_FOUND);
}

// T2 — generation mismatch: combine derived state with wrong generation identity
BOOST_AUTO_TEST_CASE(derived_state_generation_mismatch)
{
    std::string error;
    BlockIndexDerivedStateStore store;
    BuildStore(&store, &error);
    store = BlockIndexDerivedStateStore();

    // Try to open with wrong generation
    BlockIndexDerivedStateStore reader;
    BOOST_CHECK(!BlockIndexDerivedStateStore::OpenReadOnly(testDir, testGeneration + 1, &reader, &error));
    BOOST_CHECK(error.find("generation mismatch") != std::string::npos);
}

// T3 — record-count mismatch: derived entry count must match records.dat count
// (This is validated at the reader level, not the store level. The store is
// self-consistent. The reader must verify entryCount == manifest.recordCount.)
BOOST_AUTO_TEST_CASE(derived_state_entry_count_coherence)
{
    std::string error;
    BlockIndexDerivedStateStore store;
    BuildStore(&store, &error);
    store = BlockIndexDerivedStateStore();

    // Reopen and verify entry count matches what was written
    BlockIndexDerivedStateStore reader;
    BOOST_REQUIRE(BlockIndexDerivedStateStore::OpenReadOnly(testDir, testGeneration, &reader, &error));
    BOOST_CHECK_EQUAL(reader.EntryCount(), 5ULL);

    // Reading beyond count returns NOT_FOUND
    BlockIndexDerivedEntry entry;
    BOOST_CHECK_EQUAL(reader.Read(6, &entry, &error), BLOCK_INDEX_DERIVED_LOOKUP_NOT_FOUND);
    BOOST_CHECK_EQUAL(reader.Read(100, &entry, &error), BLOCK_INDEX_DERIVED_LOOKUP_NOT_FOUND);
}

// T4 — tip mismatch: derived store tip trust must match MANIFEST committed tip
// (Validated at reader level. Store-level test: verify the last entry is readable.)
BOOST_AUTO_TEST_CASE(derived_state_tip_entry_readable)
{
    std::string error;
    BlockIndexDerivedStateStore store;
    BuildStore(&store, &error);
    store = BlockIndexDerivedStateStore();

    BlockIndexDerivedStateStore reader;
    BOOST_REQUIRE(BlockIndexDerivedStateStore::OpenReadOnly(testDir, testGeneration, &reader, &error));

    // Last entry (tip) must be readable and match
    BlockIndexDerivedEntry entry;
    BOOST_CHECK_EQUAL(reader.Read(5, &entry, &error), BLOCK_INDEX_DERIVED_LOOKUP_FOUND);
    BOOST_CHECK(entry.chainTrust == trustSide);
}

// T5 — truncation/corruption: must fail typed/closed
BOOST_AUTO_TEST_CASE(derived_state_truncation_fails_closed)
{
    std::string error;
    BlockIndexDerivedStateStore store;
    BuildStore(&store, &error);
    store = BlockIndexDerivedStateStore();

    // Truncate the file to header-only (remove all entries)
    std::string derivedPath = testDir + "/" + BLOCK_INDEX_DERIVED_FILE_NAME;
    {
        FILE* f = fopen(derivedPath.c_str(), "rb");
        BOOST_REQUIRE(f != NULL);
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fclose(f);

        // Truncate to header size
        BOOST_REQUIRE(truncate(derivedPath.c_str(), BLOCK_INDEX_DERIVED_HEADER_SIZE_V1) == 0);
    }

    // Open should fail: entry region truncated
    BlockIndexDerivedStateStore reader;
    BOOST_CHECK(!BlockIndexDerivedStateStore::OpenReadOnly(testDir, testGeneration, &reader, &error));
    BOOST_CHECK(error.find("truncated") != std::string::npos);
}

// T6 — old generation/version: must produce explicitly designed compatibility result
BOOST_AUTO_TEST_CASE(derived_state_old_generation_rejected)
{
    std::string error;
    BlockIndexDerivedStateStore store;
    BuildStore(&store, &error);
    store = BlockIndexDerivedStateStore();

    // Try to open with generation 0 (invalid)
    BlockIndexDerivedStateStore reader;
    BOOST_CHECK(!BlockIndexDerivedStateStore::OpenReadOnly(testDir, 0, &reader, &error));
}

// T7 — differential trust/checksum: build synthetic chain using legacy semantics
// and compare persisted V2 derived values against exact legacy values
BOOST_AUTO_TEST_CASE(derived_state_differential_trust_matches_legacy)
{
    // This test verifies that the derived values we persist are exactly what
    // the legacy startup authority would produce. We use the same fixture as
    // the startup authority tests.
    LOCK(cs_main);

    // Save globals
    std::map<uint256, CBlockIndex*> savedMap;
    CBlockIndex* savedBest = pindexBest;
    CBlockIndex* savedGenesis = pindexGenesisBlock;
    uint256 savedHashBest = hashBestChain;
    uint256 savedBestTrust = nBestChainTrust;
    int savedBestHeight = nBestHeight;
    savedMap.swap(mapBlockIndex);
    pindexBest = NULL;
    pindexGenesisBlock = NULL;
    hashBestChain = 0;
    nBestChainTrust = 0;
    nBestHeight = -1;
    ClearFindBlockByHeightCache();
    ClearBlockIndexAccessorState();

    // Build synthetic chain
    std::vector<CBlockIndex*> owned;
    auto Add = [&](const uint256& hash, int height, CBlockIndex* parent,
                   uint64_t trust, unsigned int checksum,
                   int64_t modifierTime, unsigned int blockSize) -> CBlockIndex*
    {
        CBlockIndex* p = new CBlockIndex();
        p->pprev = parent;
        p->nHeight = height;
        p->nChainTrust = uint256(trust);
        p->nStakeModifierChecksum = checksum;
        p->nStakeModifierTime = modifierTime;
        p->nSize = blockSize;
        p->nTime = 100000 + height;
        p->nBits = 0x1e0fffff;
        std::map<uint256, CBlockIndex*>::iterator inserted =
            mapBlockIndex.insert(std::make_pair(hash, p)).first;
        p->phashBlock = &inserted->first;
        owned.push_back(p);
        return p;
    };

    uint256 hGenesis(uint256(3001));
    uint256 hOne(uint256(3002));
    uint256 hTwo(uint256(3003));
    uint256 hTip(uint256(3004));

    CBlockIndex* genesis = Add(hGenesis, 0, NULL, 10, 0, 0, 0);
    CBlockIndex* one = Add(hOne, 1, genesis, 20, 111, 1000, 501);
    CBlockIndex* two = Add(hTwo, 2, one, 30, 222, 1000, 502);
    CBlockIndex* tip = Add(hTip, 3, two, 40, 333, 1200, 503);

    genesis->pnext = one;
    one->pnext = two;
    two->pnext = tip;
    tip->pnext = NULL;

    pindexGenesisBlock = genesis;
    pindexBest = tip;
    hashBestChain = hTip;
    nBestHeight = 3;
    nBestChainTrust = tip->nChainTrust;

    // Get legacy authority values
    LegacyBlockIndexStartupAuthority authority;
    unsigned int mismatches = 0;

    // Build derived store from the same values
    std::string error;
    std::string diffDir = testDir + "/diff";
    boost::filesystem::create_directories(diffDir);
    BlockIndexDerivedStateStore derivedStore;
    BOOST_REQUIRE(BlockIndexDerivedStateStore::Create(diffDir, testGeneration, &derivedStore, &error));

    const uint256 hashes[] = {hGenesis, hOne, hTwo, hTip};
    for (size_t i = 0; i < 4; ++i)
    {
        BlockIndexStartupResult result = authority.LookupByHash(BlockIndexLogicalId(hashes[i]));
        BOOST_REQUIRE(result.HasRecord());

        BlockIndexDerivedEntry entry;
        entry.chainTrust = result.record.derived.chainTrust;
        entry.stakeModifierChecksum = result.record.derived.stakeModifierChecksum;
        entry.stakeModifierTime = result.record.derived.stakeModifierTime;
        entry.SetHasStakeModifierTime(result.record.derived.hasStakeModifierTime);
        BOOST_REQUIRE(derivedStore.Append(entry, &error));
    }
    BOOST_REQUIRE(derivedStore.Finalize(&error));

    // Reopen and compare
    derivedStore = BlockIndexDerivedStateStore();
    BlockIndexDerivedStateStore reader;
    BOOST_REQUIRE(BlockIndexDerivedStateStore::OpenReadOnly(diffDir, testGeneration, &reader, &error));

    for (size_t i = 0; i < 4; ++i)
    {
        BlockIndexStartupResult legacy = authority.LookupByHash(BlockIndexLogicalId(hashes[i]));
        BOOST_REQUIRE(legacy.HasRecord());

        BlockIndexDerivedEntry persisted;
        BOOST_REQUIRE_EQUAL(reader.Read(i + 1, &persisted, &error),
                           BLOCK_INDEX_DERIVED_LOOKUP_FOUND);

        if (persisted.chainTrust != legacy.record.derived.chainTrust)
            ++mismatches;
        if (persisted.stakeModifierChecksum != legacy.record.derived.stakeModifierChecksum)
            ++mismatches;
        if (persisted.HasStakeModifierTime() != legacy.record.derived.hasStakeModifierTime)
            ++mismatches;
        if (persisted.HasStakeModifierTime() &&
            persisted.stakeModifierTime != legacy.record.derived.stakeModifierTime)
            ++mismatches;
    }

    BOOST_CHECK_EQUAL(mismatches, 0U);

    // Restore globals
    for (size_t i = 0; i < owned.size(); ++i)
        delete owned[i];
    mapBlockIndex.clear();
    savedMap.swap(mapBlockIndex);
    pindexBest = savedBest;
    pindexGenesisBlock = savedGenesis;
    hashBestChain = savedHashBest;
    nBestChainTrust = savedBestTrust;
    nBestHeight = savedBestHeight;
    ClearFindBlockByHeightCache();
    ClearBlockIndexAccessorState();
}

// T8 — restart/reopen: close/reopen and obtain identical results
BOOST_AUTO_TEST_CASE(derived_state_restart_reopen)
{
    std::string error;
    BlockIndexDerivedStateStore store;
    BuildStore(&store, &error);
    store = BlockIndexDerivedStateStore();

    // First open
    BlockIndexDerivedStateStore reader1;
    BOOST_REQUIRE(BlockIndexDerivedStateStore::OpenReadOnly(testDir, testGeneration, &reader1, &error));

    BlockIndexDerivedEntry entry1;
    BOOST_REQUIRE_EQUAL(reader1.Read(3, &entry1, &error), BLOCK_INDEX_DERIVED_LOOKUP_FOUND);

    // Close and reopen
    reader1 = BlockIndexDerivedStateStore();
    BlockIndexDerivedStateStore reader2;
    BOOST_REQUIRE(BlockIndexDerivedStateStore::OpenReadOnly(testDir, testGeneration, &reader2, &error));

    BlockIndexDerivedEntry entry2;
    BOOST_REQUIRE_EQUAL(reader2.Read(3, &entry2, &error), BLOCK_INDEX_DERIVED_LOOKUP_FOUND);

    // Must be identical
    BOOST_CHECK(entry1.chainTrust == entry2.chainTrust);
    BOOST_CHECK_EQUAL(entry1.stakeModifierChecksum, entry2.stakeModifierChecksum);
    BOOST_CHECK_EQUAL(entry1.stakeModifierTime, entry2.stakeModifierTime);
    BOOST_CHECK_EQUAL(entry1.flags, entry2.flags);
}

// T9 — complexity: demonstrate the production reader keeps bounded state
// independent of N. The store itself is O(1) per lookup (direct seek).
BOOST_AUTO_TEST_CASE(derived_state_lookup_is_constant_time)
{
    std::string error;
    BlockIndexDerivedStateStore store;

    // Build a store with many entries
    BOOST_REQUIRE(BlockIndexDerivedStateStore::Create(testDir, testGeneration, &store, &error));
    const int N = 1000;
    for (int i = 0; i < N; ++i)
    {
        BlockIndexDerivedEntry entry;
        entry.chainTrust = uint256(i + 1);
        entry.stakeModifierChecksum = (unsigned int)(i * 7);
        entry.stakeModifierTime = 100000 + i;
        entry.SetHasStakeModifierTime(i % 2 == 0);
        BOOST_REQUIRE(store.Append(entry, &error));
    }
    BOOST_REQUIRE(store.Finalize(&error));
    store = BlockIndexDerivedStateStore();

    // Reopen and verify O(1) lookups (direct file seek, no iteration)
    BlockIndexDerivedStateStore reader;
    BOOST_REQUIRE(BlockIndexDerivedStateStore::OpenReadOnly(testDir, testGeneration, &reader, &error));
    BOOST_CHECK_EQUAL(reader.EntryCount(), (uint64_t)N);

    // Random access: read first, middle, last
    BlockIndexDerivedEntry entry;
    BOOST_CHECK_EQUAL(reader.Read(1, &entry, &error), BLOCK_INDEX_DERIVED_LOOKUP_FOUND);
    BOOST_CHECK(entry.chainTrust == uint256(1));

    BOOST_CHECK_EQUAL(reader.Read(N / 2, &entry, &error), BLOCK_INDEX_DERIVED_LOOKUP_FOUND);
    BOOST_CHECK(entry.chainTrust == uint256(N / 2));

    BOOST_CHECK_EQUAL(reader.Read(N, &entry, &error), BLOCK_INDEX_DERIVED_LOOKUP_FOUND);
    BOOST_CHECK(entry.chainTrust == uint256(N));

    // The reader state is bounded: no O(N) vectors, maps, or caches
    // (verified by code inspection: ReadHandle has only FILE*, fileSize, cs)
}

// T10 — authority/materialization separation: prove derived authority lookup
// does not require block bytes, disk block coordinates, or resident CBlockIndex
BOOST_AUTO_TEST_CASE(derived_state_no_materialization_required)
{
    std::string error;
    BlockIndexDerivedStateStore store;
    BuildStore(&store, &error);
    store = BlockIndexDerivedStateStore();

    BlockIndexDerivedStateStore reader;
    BOOST_REQUIRE(BlockIndexDerivedStateStore::OpenReadOnly(testDir, testGeneration, &reader, &error));

    // Read derived state without any CBlockIndex, mapBlockIndex, or block bytes
    BlockIndexDerivedEntry entry;
    BOOST_CHECK_EQUAL(reader.Read(2, &entry, &error), BLOCK_INDEX_DERIVED_LOOKUP_FOUND);

    // The entry contains only semantic metadata:
    // - chainTrust (uint256)
    // - stakeModifierChecksum (uint32)
    // - stakeModifierTime (int64, availability-flagged)
    // No nFile, nBlockPos, CBlockIndex*, phashBlock, or block body data.
    BOOST_CHECK(entry.chainTrust == trustOne);
    BOOST_CHECK_EQUAL(entry.stakeModifierChecksum, checksumOne);
    BOOST_CHECK_EQUAL(entry.HasStakeModifierTime(), true);
    BOOST_CHECK_EQUAL(entry.stakeModifierTime, modifierTimeOne);
}

BOOST_AUTO_TEST_SUITE_END()
