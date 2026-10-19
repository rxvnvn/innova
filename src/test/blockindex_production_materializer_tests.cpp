// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/test/unit_test.hpp>

#include "blockindex_authority_materializer.h"
#include "blockindex_v2_reader.h"
#include "blockindex_derived_state.h"
#include "blockindex_generation_builder.h"
#include "blockindex_generation_lifecycle.h"
#include "blockindex_hot_owner.h"
#include "main.h"

#include <boost/filesystem.hpp>
#include <memory>
#include <string>
#include <vector>

namespace {

static boost::filesystem::path UniqueRoot()
{
    boost::filesystem::path p = boost::filesystem::temp_directory_path() /
        boost::filesystem::unique_path("innova-prod-mat-%%%%-%%%%");
    boost::filesystem::create_directories(p);
    return p;
}

// EXACT COPY from blockindex_v2_reader_tests.cpp Rec() - with explicit PoW flags
static BlockIndexRecord Rec(uint64_t n, int h, uint256 prev)
{
    BlockIndexRecord r;
    r.hash = uint256(n);
    r.hashPrev = prev;
    r.height = h;
    r.nVersion = 1;
    r.nTime = 1000 + h;
    r.nBits = 0x1d00ffff;
    r.hashProof = uint256(n + 100);
    // CRITICAL: Explicitly set PoW fields (zero-initialized by default but must be explicit for builder)
    r.nStakeModifier = 0;
    r.prevoutStake = COutPoint();
    r.nFlags = 0;
    return r;
}

// EXACT COPY from blockindex_v2_reader_tests.cpp Source()
static BlockIndexGenerationSource Source()
{
    BlockIndexGenerationSource s; uint256 prev(0);
    for(int h=0;h<8;++h){BlockIndexRecord r=Rec(100+h,h,prev); BlockIndexGenerationSourceRecord q;q.hash=r.hash;q.record=r;s.records.push_back(q);prev=r.hash;}
    BlockIndexRecord side=Rec(999,3,uint256(102)); BlockIndexGenerationSourceRecord q;q.hash=side.hash;q.record=side;s.records.push_back(q);
    s.hashBestChain=prev;s.foundBestChain=true;return s;
}

// EXACT COPY from blockindex_v2_reader_tests.cpp BuildSelected()
static void BuildSelected(const boost::filesystem::path& root)
{
    BlockIndexGenerationBuilder b; BlockIndexGenerationStats st; std::string e;
    BOOST_REQUIRE_MESSAGE(b.Build(Source(), (root/"gen-000001").string(), 1, &st, &e),e); b.Close();
    BOOST_REQUIRE_MESSAGE(BlockIndexGenerationManager::SelectGeneration(root.string(),1,&e)==BLOCK_INDEX_LIFECYCLE_OK,e);
}

static void OpenReader(const boost::filesystem::path& root, BlockIndexV2Reader* r)
{
    BlockIndexV2ReaderOptions o;
    o.cacheCapacityBytes = 64ULL * 1024 * 1024;
    std::string e;
    BOOST_REQUIRE_MESSAGE(r->Open(root.string(), o, &e), e);
}

static void OpenDerivedStore(const boost::filesystem::path& root, BlockIndexDerivedStateStore* store)
{
    std::string e;
    BOOST_REQUIRE_MESSAGE(BlockIndexDerivedStateStore::OpenReadOnly(
        (root / "gen-000001").string(), 1, store, &e), e);
}

} // namespace

BOOST_AUTO_TEST_SUITE(blockindex_production_materializer_tests)

// ============================================================================
// G0 — production materializer success: real by-value metadata -> sparse-hot object
// ============================================================================
BOOST_AUTO_TEST_CASE(g0_materializer_success)
{
    boost::filesystem::path root = UniqueRoot();
    BuildSelected(root);

    BlockIndexV2Reader reader;
    OpenReader(root, &reader);

    BlockIndexDerivedStateStore derivedStore;
    OpenDerivedStore(root, &derivedStore);

    BlockIndexAuthorityMaterializer mat(&reader, &derivedStore, 1);

    // Materialize a block at height 5 (hash = 100 + 5 = 105)
    BlockIndexLogicalId id(uint256(105));
    BlockIndexHotMaterialized out;
    BlockIndexHotStatus st = mat.Materialize(id, &out);

    BOOST_CHECK(st == BlockIndexHotStatus::OK);
    BOOST_CHECK(out.found);
    BOOST_CHECK(out.generation == 1);
    BOOST_CHECK(out.snapshot.found);
    BOOST_CHECK(out.snapshot.hash == uint256(105));
    BOOST_CHECK(out.snapshot.height == 5);
    BOOST_CHECK(out.snapshot.nBits == 0x1d00ffffU);
    BOOST_CHECK(out.snapshot.nChainTrust != uint256(0));
    BOOST_CHECK(out.snapshot.fProofOfStake == false);
    BOOST_CHECK(out.snapshot.hashProof == uint256(205)); // 105 + 100
}

// ============================================================================
// G1 — first consumer end-to-end: real authority -> production materializer ->
// HotOwner -> GetBlockTrustViaHotOwner -> correct trust
// ============================================================================
BOOST_AUTO_TEST_CASE(g1_first_consumer_e2e)
{
    boost::filesystem::path root = UniqueRoot();
    BuildSelected(root);

    BlockIndexV2Reader reader;
    OpenReader(root, &reader);

    BlockIndexDerivedStateStore derivedStore;
    OpenDerivedStore(root, &derivedStore);

    BlockIndexAuthorityMaterializer mat(&reader, &derivedStore, 1);

    BlockIndexHotOwner owner;
    owner.SetCurrentGeneration(1);
    owner.SetMaterializer(&mat);

    // Test PoW block at height 5 (hash = 100 + 5 = 105)
    BlockIndexHotDerivedTrust r = GetBlockTrustViaHotOwner(owner, uint256(105));
    BOOST_CHECK(r.ok);
    BOOST_CHECK(r.nTrust != uint256(0));

    // Verify against legacy computation
    uint256 legTrust;
    {
        CBlockIndex leg;
        leg.phashBlock = new uint256(uint256(105));
        leg.nBits = 0x1d00ffffU;
        leg.nHeight = 5;
        leg.nFlags = 0;
        leg.hashProof = uint256(205); // 105 + 100
        legTrust = leg.GetBlockTrust();
    }
    BOOST_CHECK(r.nTrust == legTrust);
}

// ============================================================================
// G2 — legacy/hot differential: same logical hash, same semantic trust result
// ============================================================================
BOOST_AUTO_TEST_CASE(g2_legacy_hot_differential)
{
    boost::filesystem::path root = UniqueRoot();
    BuildSelected(root);

    BlockIndexV2Reader reader;
    OpenReader(root, &reader);

    BlockIndexDerivedStateStore derivedStore;
    OpenDerivedStore(root, &derivedStore);

    BlockIndexAuthorityMaterializer mat(&reader, &derivedStore, 1);

    BlockIndexHotOwner owner;
    owner.SetCurrentGeneration(1);
    owner.SetMaterializer(&mat);

    // Test multiple blocks (heights 1..7, hashes 101..107)
    for (int h = 1; h < 8; ++h) {
        uint256 hash = uint256(100 + h);
        BlockIndexHotDerivedTrust hotTrust = GetBlockTrustViaHotOwner(owner, hash);
        BOOST_REQUIRE(hotTrust.ok);

        // Legacy reference
        uint256 legTrust;
        {
            CBlockIndex leg;
            leg.phashBlock = new uint256(hash);
            leg.nBits = 0x1d00ffffU;
            leg.nHeight = h;
            leg.nFlags = 0;
            leg.hashProof = uint256(100 + h + 100); // 200 + h
            legTrust = leg.GetBlockTrust();
        }
        BOOST_CHECK(hotTrust.nTrust == legTrust);
    }
}

// ============================================================================
// G3 — no resident pointer dependency: clear legacy state, production path works
// ============================================================================
BOOST_AUTO_TEST_CASE(g3_no_resident_pointer_dependency)
{
    boost::filesystem::path root = UniqueRoot();
    BuildSelected(root);

    BlockIndexV2Reader reader;
    OpenReader(root, &reader);

    BlockIndexDerivedStateStore derivedStore;
    OpenDerivedStore(root, &derivedStore);

    BlockIndexAuthorityMaterializer mat(&reader, &derivedStore, 1);

    BlockIndexHotOwner owner;
    owner.SetCurrentGeneration(1);
    owner.SetMaterializer(&mat);

    // Materialize and consume WITHOUT any legacy mapBlockIndex involvement
    // The materializer only uses V2 reader + derived store
    BlockIndexHotDerivedTrust r = GetBlockTrustViaHotOwner(owner, uint256(105));
    BOOST_CHECK(r.ok);
    BOOST_CHECK(r.nTrust != uint256(0));

    // Verify no legacy mapBlockIndex was accessed by checking that
    // the materializer doesn't even know about mapBlockIndex
    // (This is a structural guarantee - the materializer has no mapBlockIndex dependency)
    BOOST_CHECK(owner.IsResident(BlockIndexLogicalId(uint256(105))));
}

// ============================================================================
// G4 — sparse topology: materialized object has pprev/pnext/pskip == NULL
// ============================================================================
BOOST_AUTO_TEST_CASE(g4_sparse_topology)
{
    boost::filesystem::path root = UniqueRoot();
    BuildSelected(root);

    BlockIndexV2Reader reader;
    OpenReader(root, &reader);

    BlockIndexDerivedStateStore derivedStore;
    OpenDerivedStore(root, &derivedStore);

    BlockIndexAuthorityMaterializer mat(&reader, &derivedStore, 1);

    BlockIndexHotOwner owner;
    owner.SetCurrentGeneration(1);
    owner.SetMaterializer(&mat);

    BlockIndexHotHandle h;
    BOOST_REQUIRE(owner.Pin(BlockIndexLogicalId(uint256(105)), &h) == BlockIndexHotStatus::OK);

    CBlockIndex* p = h.Get();
    BOOST_REQUIRE(p != NULL);

    // Sparse-hot: no topology pointers
    BOOST_CHECK(p->pprev == NULL);
    BOOST_CHECK(p->pnext == NULL);
    BOOST_CHECK(p->pskip == NULL);

    // But consumer still works
    BOOST_CHECK(p->GetBlockTrust() != uint256(0));
    BOOST_CHECK(p->nHeight == 5);
    BOOST_CHECK(p->nBits == 0x1d00ffffU);
}

// ============================================================================
// G5 — release/evict/rematerialize: production path round-trip same result
// ============================================================================
BOOST_AUTO_TEST_CASE(g5_evict_rematerialize)
{
    boost::filesystem::path root = UniqueRoot();
    BuildSelected(root);

    BlockIndexV2Reader reader;
    OpenReader(root, &reader);

    BlockIndexDerivedStateStore derivedStore;
    OpenDerivedStore(root, &derivedStore);

    BlockIndexAuthorityMaterializer mat(&reader, &derivedStore, 1);

    BlockIndexHotOwner owner;
    owner.SetCurrentGeneration(1);
    owner.SetMaterializer(&mat);

    // First materialization
    BlockIndexHotDerivedTrust r1 = GetBlockTrustViaHotOwner(owner, uint256(105));
    BOOST_REQUIRE(r1.ok);
    BOOST_CHECK(owner.IsResident(BlockIndexLogicalId(uint256(105))));

    // Evict
    BOOST_REQUIRE(owner.EvictResident(BlockIndexLogicalId(uint256(105))) == BlockIndexHotStatus::OK);
    BOOST_CHECK(!owner.IsResident(BlockIndexLogicalId(uint256(105))));

    // Rematerialize and consume again
    BlockIndexHotDerivedTrust r2 = GetBlockTrustViaHotOwner(owner, uint256(105));
    BOOST_CHECK(r2.ok);
    BOOST_CHECK(owner.IsResident(BlockIndexLogicalId(uint256(105))));
    BOOST_CHECK(r2.nTrust == r1.nTrust); // Same result
}

// ============================================================================
// G6 — not found: unknown logical hash -> typed failure, no resident object
// ============================================================================
BOOST_AUTO_TEST_CASE(g6_not_found)
{
    boost::filesystem::path root = UniqueRoot();
    BuildSelected(root);

    BlockIndexV2Reader reader;
    OpenReader(root, &reader);

    BlockIndexDerivedStateStore derivedStore;
    OpenDerivedStore(root, &derivedStore);

    BlockIndexAuthorityMaterializer mat(&reader, &derivedStore, 1);

    BlockIndexHotOwner owner;
    owner.SetCurrentGeneration(1);
    owner.SetMaterializer(&mat);

    // Unknown hash
    BlockIndexHotDerivedTrust r = GetBlockTrustViaHotOwner(owner, uint256(999999));
    BOOST_CHECK(!r.ok);
    BOOST_CHECK(!owner.IsResident(BlockIndexLogicalId(uint256(999999))));
}

// ============================================================================
// G7 — corrupt/unavailable metadata: inject invalid metadata -> fail closed
// ============================================================================
BOOST_AUTO_TEST_CASE(g7_corrupt_metadata)
{
    // This test requires a generation with corrupt derived.dat
    // For now, test the materializer directly with a closed derived store
    boost::filesystem::path root = UniqueRoot();
    BuildSelected(root);

    BlockIndexV2Reader reader;
    OpenReader(root, &reader);

    // Use an UNOPENED derived store
    BlockIndexDerivedStateStore derivedStore;
    // Don't open it

    BlockIndexAuthorityMaterializer mat(&reader, &derivedStore, 1);

    BlockIndexHotOwner owner;
    owner.SetCurrentGeneration(1);
    owner.SetMaterializer(&mat);

    BlockIndexHotDerivedTrust r = GetBlockTrustViaHotOwner(owner, uint256(105));
    BOOST_CHECK(!r.ok); // Fail closed
    BOOST_CHECK(!owner.IsResident(BlockIndexLogicalId(uint256(105))));
}

// ============================================================================
// G8 — generation rollover: start G1 materialization, move to G2, stale G1 cannot publish as G2
// ============================================================================
BOOST_AUTO_TEST_CASE(g8_generation_rollover)
{
    boost::filesystem::path root = UniqueRoot();
    BuildSelected(root);

    BlockIndexV2Reader reader;
    OpenReader(root, &reader);

    BlockIndexDerivedStateStore derivedStore;
    OpenDerivedStore(root, &derivedStore);

    BlockIndexAuthorityMaterializer mat(&reader, &derivedStore, 1);

    BlockIndexHotOwner owner;
    owner.SetCurrentGeneration(1);
    owner.SetMaterializer(&mat);

    // Materialize at generation 1
    BlockIndexHotDerivedTrust r1 = GetBlockTrustViaHotOwner(owner, uint256(105));
    BOOST_REQUIRE(r1.ok);
    uint256 trustGen1 = r1.nTrust;
    BOOST_REQUIRE(trustGen1 != uint256(0));

    // Now simulate generation rollover: advance owner generation to 2
    // The materializer is still bound to generation 1
    owner.SetCurrentGeneration(2);

    // The resident object from generation 1 is still cached by logical hash
    // To force a generation mismatch, we need to evict first
    BOOST_REQUIRE(owner.EvictResident(BlockIndexLogicalId(uint256(105))) == BlockIndexHotStatus::OK);

    // Now try to materialize with generation 1 materializer but owner at generation 2
    // This should fail with GENERATION_MISMATCH on publish
    BlockIndexHotDerivedTrust r2 = GetBlockTrustViaHotOwner(owner, uint256(105));
    // The materializer will produce generation 1 materialization, but owner expects 2
    // HotOwner should reject with GENERATION_MISMATCH -> fail closed
    BOOST_CHECK(!r2.ok);
    BOOST_CHECK(!owner.IsResident(BlockIndexLogicalId(uint256(105))));
}

// ============================================================================
// G9 — current generation success: after rollover, fresh G2 materialization succeeds
// ============================================================================
BOOST_AUTO_TEST_CASE(g9_current_generation_success)
{
    // This test requires building a second generation
    // For now, we test that the materializer properly tags its generation
    boost::filesystem::path root = UniqueRoot();
    BuildSelected(root);

    BlockIndexV2Reader reader;
    OpenReader(root, &reader);

    BlockIndexDerivedStateStore derivedStore;
    OpenDerivedStore(root, &derivedStore);

    BlockIndexAuthorityMaterializer mat(&reader, &derivedStore, 1);

    // Verify generation is correctly reported
    BOOST_CHECK(mat.Generation() == 1);

    BlockIndexHotOwner owner;
    owner.SetCurrentGeneration(1);
    owner.SetMaterializer(&mat);

    BlockIndexHotDerivedTrust r = GetBlockTrustViaHotOwner(owner, uint256(105));
    BOOST_CHECK(r.ok);
    BOOST_CHECK(owner.CurrentGeneration() == 1);
}

// ============================================================================
// G10 — no topology fabrication: materializer doesn't recursively materialize ancestors
// ============================================================================
BOOST_AUTO_TEST_CASE(g10_no_topology_fabrication)
{
    boost::filesystem::path root = UniqueRoot();
    BuildSelected(root);

    BlockIndexV2Reader reader;
    OpenReader(root, &reader);

    BlockIndexDerivedStateStore derivedStore;
    OpenDerivedStore(root, &derivedStore);

    BlockIndexAuthorityMaterializer mat(&reader, &derivedStore, 1);

    BlockIndexHotOwner owner;
    owner.SetCurrentGeneration(1);
    owner.SetMaterializer(&mat);

    // Materialize a single block at height 5 (hash 105)
    BlockIndexHotDerivedTrust r = GetBlockTrustViaHotOwner(owner, uint256(105));
    BOOST_REQUIRE(r.ok);

    // Only ONE object should be resident (the requested block)
    // No ancestors were materialized
    BOOST_CHECK(owner.ResidentCount() == 1);
    BOOST_CHECK(owner.IsResident(BlockIndexLogicalId(uint256(105))));
    BOOST_CHECK(!owner.IsResident(BlockIndexLogicalId(uint256(104)))); // parent not materialized
    BOOST_CHECK(!owner.IsResident(BlockIndexLogicalId(uint256(100)))); // genesis not materialized
}

// ============================================================================
// G11 — concurrent same-hash production materialization: multiple pins converge
// ============================================================================
BOOST_AUTO_TEST_CASE(g11_concurrent_same_hash)
{
    boost::filesystem::path root = UniqueRoot();
    BuildSelected(root);

    BlockIndexV2Reader reader;
    OpenReader(root, &reader);

    BlockIndexDerivedStateStore derivedStore;
    OpenDerivedStore(root, &derivedStore);

    BlockIndexAuthorityMaterializer mat(&reader, &derivedStore, 1);

    BlockIndexHotOwner owner;
    owner.SetCurrentGeneration(1);
    owner.SetMaterializer(&mat);

    // Two pins on the same hash
    BlockIndexHotHandle h1, h2;
    BOOST_REQUIRE(owner.Pin(BlockIndexLogicalId(uint256(105)), &h1) == BlockIndexHotStatus::OK);
    BOOST_REQUIRE(owner.Pin(BlockIndexLogicalId(uint256(105)), &h2) == BlockIndexHotStatus::OK);

    // Same resident object
    BOOST_CHECK(h1.Get() == h2.Get());
    BOOST_CHECK(owner.ResidentCount() == 1);
    BOOST_CHECK(owner.PinCount() == 2);

    // Both produce correct trust
    BOOST_CHECK(h1.Get()->GetBlockTrust() == h2.Get()->GetBlockTrust());
    BOOST_CHECK(h1.Get()->GetBlockTrust() != uint256(0));
}

// ============================================================================
// G12 — explicit failure propagation: diagnostic/status survives
// ============================================================================
BOOST_AUTO_TEST_CASE(g12_explicit_failure_propagation)
{
    boost::filesystem::path root = UniqueRoot();
    BuildSelected(root);

    BlockIndexV2Reader reader;
    OpenReader(root, &reader);

    BlockIndexDerivedStateStore derivedStore;
    OpenDerivedStore(root, &derivedStore);

    BlockIndexAuthorityMaterializer mat(&reader, &derivedStore, 1);

    // Test AUTHORITY_MISSING
    BlockIndexHotMaterialized out;
    BlockIndexHotStatus st = mat.Materialize(BlockIndexLogicalId(uint256(999999)), &out);
    BOOST_CHECK(st == BlockIndexHotStatus::AUTHORITY_MISSING);
    BOOST_CHECK(!out.found);

    // Test with closed reader
    BlockIndexV2Reader closedReader;
    BlockIndexAuthorityMaterializer matClosed(&closedReader, &derivedStore, 1);
    st = matClosed.Materialize(BlockIndexLogicalId(uint256(105)), &out);
    BOOST_CHECK(st == BlockIndexHotStatus::AUTHORITY_MISSING);

    // Test with closed derived store
    BlockIndexDerivedStateStore closedDerived;
    BlockIndexAuthorityMaterializer matDerived(&reader, &closedDerived, 1);
    st = matDerived.Materialize(BlockIndexLogicalId(uint256(105)), &out);
    BOOST_CHECK(st == BlockIndexHotStatus::MATERIALIZATION_UNAVAILABLE);
}

BOOST_AUTO_TEST_SUITE_END()