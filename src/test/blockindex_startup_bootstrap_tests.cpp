// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.

// A.10.1i - Causal verification for the authoritative-startup bootstrap owner.

#include <boost/test/unit_test.hpp>

#include "../blockindex_startup_bootstrap.h"
#include "../blockindex_startup_authority.h"
#include "../blockindex_startup_seam.h"
#include "../blockindex_generation_builder.h"
#include "../blockindex_generation_lifecycle.h"
#include "../fixed_blockindex_store.h"
#include "../main.h"

#include <boost/filesystem.hpp>

#include <assert.h>
#include <stdio.h>
#include <string>
#include <vector>

namespace {

struct SyntheticBlockInfo
{
    uint256 hash;
    unsigned int nFile = 0;
    unsigned int nBlockPos = 0;
    unsigned int nSize = 0;
};

static SyntheticBlockInfo WriteSyntheticBlock(
    const boost::filesystem::path& dir,
    uint256 hashPrevBlock, unsigned int nTime, unsigned int nBits, unsigned int nNonce)
{
    SyntheticBlockInfo info;
    info.nFile = 1;

    CTransaction coinbase;
    coinbase.nVersion = 1;
    coinbase.nTime = nTime;
    CTxIn input;
    input.prevout = COutPoint(uint256(0), 0xffffffff);
    input.scriptSig = CScript() << OP_TRUE;
    input.nSequence = 0xffffffff;
    coinbase.vin.push_back(input);
    CTxOut output;
    output.nValue = 0;
    output.scriptPubKey = CScript() << OP_TRUE;
    coinbase.vout.push_back(output);

    CBlock block;
    block.nVersion = 1;
    block.hashPrevBlock = hashPrevBlock;
    block.nTime = nTime;
    block.nBits = nBits;
    block.nNonce = nNonce;
    block.vtx.push_back(coinbase);
    block.hashMerkleRoot = block.BuildMerkleTree();

    info.hash = block.GetHash();
    info.nSize = ::GetSerializeSize(block, SER_NETWORK, PROTOCOL_VERSION);

    CDataStream ssBlock(SER_DISK, CLIENT_VERSION);
    ssBlock << block;
    unsigned int nDiskSize = ssBlock.size();

    boost::filesystem::path blockFile = dir / "blk0001.dat";
    FILE* f = fopen(blockFile.string().c_str(), "ab");
    assert(f);

    unsigned char magic[] = {0xfa, 0xbf, 0xb5, 0xda};
    fwrite(magic, 1, 4, f);
    fwrite(&nDiskSize, 4, 1, f);
    long pos = ftell(f);
    info.nBlockPos = (unsigned int)pos;
    fwrite(&ssBlock[0], 1, ssBlock.size(), f);
    fflush(f);
    fclose(f);

    return info;
}

struct BootstrapFixture
{
    boost::filesystem::path root;
    boost::filesystem::path blockDir;
    SyntheticBlockInfo tip;
    int tipHeight;

    explicit BootstrapFixture(int heights)
        : tipHeight(heights)
    {
        root = boost::filesystem::temp_directory_path() /
            boost::filesystem::unique_path("innova-blockindex-bootstrap-%%%%-%%%%-%%%%");
        boost::filesystem::create_directories(root);
        blockDir = root / "blocks";
        boost::filesystem::create_directories(blockDir);

        std::vector<SyntheticBlockInfo> blocks;
        uint256 prev(0);
        for (int h = 0; h <= tipHeight; ++h)
        {
            SyntheticBlockInfo bi = WriteSyntheticBlock(
                blockDir, prev, (unsigned int)(1000 + h), 0x1d00ffffU, (unsigned int)(100 + h));
            blocks.push_back(bi);
            prev = bi.hash;
        }
        tip = blocks[tipHeight];

        BlockIndexGenerationSource src;
        for (int h = 0; h <= tipHeight; ++h)
        {
            BlockIndexRecord rec;
            rec.hash = blocks[h].hash;
            rec.hashPrev = (h == 0) ? uint256(0) : blocks[h - 1].hash;
            rec.height = h;
            rec.nVersion = 1;
            rec.nTime = (unsigned int)(1000 + h);
            rec.nBits = 0x1d00ffffU;
            rec.nNonce = (unsigned int)(100 + h);
            rec.nFile = blocks[h].nFile;
            rec.nBlockPos = blocks[h].nBlockPos;
            BlockIndexGenerationSourceRecord sr;
            sr.hash = rec.hash;
            sr.record = rec;
            src.records.push_back(sr);
        }
        src.hashBestChain = tip.hash;
        src.foundBestChain = true;
        src.blockDataDir = blockDir.string();

        boost::filesystem::path staging = root / "build-000001.tmp";
        std::string error;
        {
            BlockIndexGenerationBuilder b;
            BOOST_REQUIRE_MESSAGE(b.Build(src, staging.string(), 1, NULL, &error), error);
            b.Close();
        }
        BOOST_REQUIRE_MESSAGE(
            BlockIndexGenerationManager::PublishGeneration(root.string(), 1, &error) ==
                (int)BLOCK_INDEX_LIFECYCLE_OK,
            error);
        BOOST_REQUIRE_MESSAGE(
            BlockIndexGenerationManager::SelectGeneration(root.string(), 1, &error) ==
                (int)BLOCK_INDEX_LIFECYCLE_OK,
            error);
    }

    ~BootstrapFixture()
    {
        boost::system::error_code ec;
        boost::filesystem::remove_all(root, ec);
    }

    bool ManifestIsAuthoritative() const
    {
        std::string error;
        FixedBlockIndexOpenOptions opts;
        opts.requireCompleteManifest = true;
        FixedBlockIndexStore store;
        if (!FixedBlockIndexStore::OpenReadOnly((root / "gen-000001").string(), opts, &store, &error))
            return false;
        return store.GetManifest().capability ==
            (uint32_t)BLOCK_INDEX_GENERATION_CAPABILITY_AUTHORITATIVE;
    }
};

// Open bt on the fixture's authoritative generation in place.
BlockIndexStartupStatus OpenBootstrap(const BootstrapFixture& fx,
                                      BlockIndexStartupBootstrap& bt,
                                      std::string& error)
{
    BlockIndexV2ReaderOptions o;
    error.clear();
    return bt.Open(fx.root.string(), o, &error);
}

} // namespace

BOOST_AUTO_TEST_SUITE(blockindex_startup_bootstrap_tests)

// I0 - construct from an AUTHORITATIVE generation.
BOOST_AUTO_TEST_CASE(i0_construct_from_authorative)
{
    BootstrapFixture fx(3);
    BOOST_REQUIRE_MESSAGE(fx.ManifestIsAuthoritative(), "fixture must be AUTHORITATIVE");
    std::string error;
    BlockIndexStartupBootstrap bt;
    BOOST_REQUIRE_MESSAGE(OpenBootstrap(fx, bt, error) == BLOCK_INDEX_STARTUP_OK, error);
    BOOST_CHECK(bt.IsOpen());
}

// I1 - best-tip logical identity is exact.
BOOST_AUTO_TEST_CASE(i1_best_tip_identity_exact)
{
    BootstrapFixture fx(3);
    std::string error;
    BlockIndexStartupBootstrap bt;
    BOOST_REQUIRE_MESSAGE(OpenBootstrap(fx, bt, error) == BLOCK_INDEX_STARTUP_OK, error);
    BOOST_CHECK(bt.BestTipId() == BlockIndexLogicalId(fx.tip.hash));
    BlockIndexStartupAuthorityIdentity id = bt.AuthorityIdentity();
    BOOST_CHECK(id.kind == BLOCK_INDEX_STARTUP_AUTHORITY_V2);
    BOOST_CHECK(id.generationQualified);
    BOOST_CHECK(id.generation == bt.Generation());
}

// I2 - best-tip materializes into HotOwner (resident, pinned).
BOOST_AUTO_TEST_CASE(i2_best_tip_materializes)
{
    BootstrapFixture fx(3);
    std::string error;
    BlockIndexStartupBootstrap bt;
    BOOST_REQUIRE_MESSAGE(OpenBootstrap(fx, bt, error) == BLOCK_INDEX_STARTUP_OK, error);
    BOOST_REQUIRE(bt.IsOpen());
    BOOST_CHECK(bt.BestTipObject() != NULL);
    BOOST_CHECK_EQUAL(bt.BestTipObject()->nHeight, fx.tipHeight);
    BOOST_CHECK(bt.Owner().IsResident(bt.BestTipId()));
    BOOST_CHECK(bt.Owner().IsPinned(bt.BestTipId()));
}

// I3 - best-tip remains pinned/anchored for the bootstrap lifetime.
BOOST_AUTO_TEST_CASE(i3_best_tip_anchored)
{
    BootstrapFixture fx(3);
    std::string error;
    BlockIndexStartupBootstrap bt;
    BOOST_REQUIRE_MESSAGE(OpenBootstrap(fx, bt, error) == BLOCK_INDEX_STARTUP_OK, error);
    BOOST_REQUIRE(bt.IsOpen());
    std::vector<BlockIndexLogicalId> eligible = bt.OwnerPtr()->EvictEligible();
    bool found = false;
    for (size_t i = 0; i < eligible.size(); ++i)
        if (eligible[i] == bt.BestTipId()) found = true;
    BOOST_CHECK_MESSAGE(!found, "best tip must never be eviction-eligible (anchor)");
    BOOST_CHECK(bt.OwnerPtr()->EvictResident(bt.BestTipId()) == BlockIndexHotStatus::EVICTION_BLOCKED);
}

// I4 - sparse topology: pprev/pnext/pskip are NOT fabricated for the best tip.
BOOST_AUTO_TEST_CASE(i4_sparse_topology)
{
    BootstrapFixture fx(3);
    std::string error;
    BlockIndexStartupBootstrap bt;
    BOOST_REQUIRE_MESSAGE(OpenBootstrap(fx, bt, error) == BLOCK_INDEX_STARTUP_OK, error);
    CBlockIndex* tip = bt.BestTipObject();
    BOOST_REQUIRE(tip != NULL);
    BOOST_CHECK(tip->pprev == NULL);
    BOOST_CHECK(tip->pnext == NULL);
    BOOST_CHECK(tip->pskip == NULL);
}

// I5 - derived metadata parity (height; active projection).
BOOST_AUTO_TEST_CASE(i5_derived_meta)
{
    BootstrapFixture fx(3);
    std::string error;
    BlockIndexStartupBootstrap bt;
    BOOST_REQUIRE_MESSAGE(OpenBootstrap(fx, bt, error) == BLOCK_INDEX_STARTUP_OK, error);
    BlockIndexStartupResult rec = bt.GetBestTipRecord();
    BOOST_REQUIRE(rec.HasRecord());
    BOOST_CHECK_EQUAL(rec.record.height, fx.tipHeight);
    BOOST_CHECK(rec.record.active);
    BOOST_CHECK(rec.record.logicalId == bt.BestTipId());
}

// I6 - generation coherence: authority/reader/derived/owner all bind same gen.
BOOST_AUTO_TEST_CASE(i6_generation_coherence)
{
    BootstrapFixture fx(3);
    std::string error;
    BlockIndexStartupBootstrap bt;
    BOOST_REQUIRE_MESSAGE(OpenBootstrap(fx, bt, error) == BLOCK_INDEX_STARTUP_OK, error);
    const uint64_t gen = bt.Generation();
    BOOST_CHECK_EQUAL(gen, bt.AuthorityIdentity().generation);
    BOOST_CHECK_EQUAL(gen, bt.Materializer().Generation());
    BOOST_CHECK_EQUAL(gen, bt.DerivedStorePtr()->Generation());
    BOOST_CHECK_EQUAL(gen, bt.Owner().CurrentGeneration());
}

// I7 - generation mismatch fails closed.
// The best tip is already anchored/resident, so re-pin returns OK (resident).
// GENERATION_MISMATCH is observed on a NEW materialization when the owner's
// generation differs from the materializer's bound generation.
BOOST_AUTO_TEST_CASE(i7_generation_mismatch_fails_closed)
{
    BootstrapFixture fx(3);
    std::string error;
    BlockIndexStartupBootstrap bt;
    BOOST_REQUIRE_MESSAGE(OpenBootstrap(fx, bt, error) == BLOCK_INDEX_STARTUP_OK, error);
    // Re-pin of the already-resident anchor is OK (resident) - not a mismatch.
    BlockIndexHotHandle h0;
    BOOST_CHECK(bt.OwnerPtr()->Pin(bt.BestTipId(), &h0) == BlockIndexHotStatus::OK);
    // A NEW materialization under a stale owner generation must fail closed.
    BlockIndexStartupResult parent = bt.Authority().GetParent(bt.BestTipId());
    BOOST_REQUIRE(parent.HasRecord());
    bt.OwnerPtr()->SetCurrentGeneration(bt.Generation() + 1);
    BlockIndexHotHandle h;
    BlockIndexHotStatus st = bt.OwnerPtr()->Pin(parent.record.logicalId, &h);
    BOOST_CHECK(st == BlockIndexHotStatus::GENERATION_MISMATCH);
}

// I8 - missing/absent best-tip materialization fails closed (no legacy fallback).
BOOST_AUTO_TEST_CASE(i8_missing_best_tip_fails_closed)
{
    BootstrapFixture fx(3);
    std::string error;
    BlockIndexStartupBootstrap bt;
    BOOST_REQUIRE_MESSAGE(OpenBootstrap(fx, bt, error) == BLOCK_INDEX_STARTUP_OK, error);
    BlockIndexHotHandle h;
    BlockIndexHotStatus st = bt.OwnerPtr()->Pin(BlockIndexLogicalId(uint256(0xDEAD)), &h);
    BOOST_CHECK(st != BlockIndexHotStatus::OK);
}

// I9 - bounded residency: resident HotOwner count is O(anchors+pins), not O(N).
BOOST_AUTO_TEST_CASE(i9_bounded_residency)
{
    BootstrapFixture fx(40);
    std::string error;
    BlockIndexStartupBootstrap bt;
    BOOST_REQUIRE_MESSAGE(OpenBootstrap(fx, bt, error) == BLOCK_INDEX_STARTUP_OK, error);
    size_t resident = bt.Owner().ResidentCount();
    BOOST_CHECK_MESSAGE(resident <= 3, "resident must be O(anchors+pins), got " << resident);
}

// I10 - pin lifetime causal proof: eviction blocked while pinned, eligible after.
BOOST_AUTO_TEST_CASE(i10_pin_lifetime_causal)
{
    BootstrapFixture fx(4);
    std::string error;
    BlockIndexStartupBootstrap bt;
    BOOST_REQUIRE_MESSAGE(OpenBootstrap(fx, bt, error) == BLOCK_INDEX_STARTUP_OK, error);

    BlockIndexStartupResult parentRec = bt.Authority().GetParent(bt.BestTipId());
    BOOST_REQUIRE(parentRec.HasRecord());
    BlockIndexHotHandle h;
    BOOST_REQUIRE(bt.OwnerPtr()->Pin(parentRec.record.logicalId, &h) == BlockIndexHotStatus::OK);
    BOOST_CHECK(bt.OwnerPtr()->EvictResident(parentRec.record.logicalId) ==
                BlockIndexHotStatus::EVICTION_BLOCKED);
    h.Reset();
    BlockIndexHotStatus evictSt = bt.OwnerPtr()->EvictResident(parentRec.record.logicalId);
    BOOST_CHECK(evictSt == BlockIndexHotStatus::OK);
}

// I11 - default startup selector remains LEGACY_RESIDENT (no cutover).
BOOST_AUTO_TEST_CASE(i11_legacy_default_unchanged)
{
    BOOST_CHECK_EQUAL((int)BlockIndexStartupFactory::DefaultMode(),
                      (int)BlockIndexStartupMode::LEGACY_RESIDENT);
}

// I12 - CAUSAL no-mapBlockIndex proof: best-tip materializes while the hash is
// absent from mapBlockIndex and map remains absent.
BOOST_AUTO_TEST_CASE(i12_no_map_dependency_causal)
{
    BootstrapFixture fx(3);
    std::string error;
    // Pre-condition: the bootstrapped hash is absent from the resident map.
    BOOST_CHECK(mapBlockIndex.find(fx.tip.hash) == mapBlockIndex.end());
    BlockIndexStartupBootstrap bt;
    BOOST_REQUIRE_MESSAGE(OpenBootstrap(fx, bt, error) == BLOCK_INDEX_STARTUP_OK, error);
    BOOST_REQUIRE(bt.BestTipObject() != NULL);
    // Post-condition: best-tip materialized WITHOUT a mapBlockIndex entry.
    BOOST_CHECK(mapBlockIndex.find(fx.tip.hash) == mapBlockIndex.end());
}



// J0 - genesis anchor (A.10.1j): bootstrap materializes/pins exact genesis from
// the same generation.
BOOST_AUTO_TEST_CASE(j0_genesis_anchor)
{
    BootstrapFixture fx(3);
    std::string error;
    BlockIndexStartupBootstrap bt;
    BOOST_REQUIRE_MESSAGE(OpenBootstrap(fx, bt, error) == BLOCK_INDEX_STARTUP_OK, error);
    BOOST_REQUIRE(bt.IsOpen());
    BOOST_REQUIRE(bt.GenesisObject() != NULL);
    BOOST_CHECK_EQUAL(bt.GenesisObject()->nHeight, 0);
    BOOST_CHECK(bt.Owner().IsResident(bt.GenesisId()));
    BOOST_CHECK(bt.Owner().IsPinned(bt.GenesisId()));
}

// J1 - genesis lifetime: eviction blocked while anchored; the anchor persists
// for the bootstrap lifetime.
BOOST_AUTO_TEST_CASE(j1_genesis_lifetime)
{
    BootstrapFixture fx(3);
    std::string error;
    BlockIndexStartupBootstrap bt;
    BOOST_REQUIRE_MESSAGE(OpenBootstrap(fx, bt, error) == BLOCK_INDEX_STARTUP_OK, error);
    std::vector<BlockIndexLogicalId> eligible = bt.OwnerPtr()->EvictEligible();
    bool found = false;
    for (size_t i = 0; i < eligible.size(); ++i)
        if (eligible[i] == bt.GenesisId()) found = true;
    BOOST_CHECK_MESSAGE(!found, "genesis must never be eviction-eligible (anchor)");
    BOOST_CHECK(bt.OwnerPtr()->EvictResident(bt.GenesisId()) == BlockIndexHotStatus::EVICTION_BLOCKED);
}

// J2 - sparse genesis: no historical pprev/pnext/pskip required to anchor.
BOOST_AUTO_TEST_CASE(j2_sparse_genesis)
{
    BootstrapFixture fx(3);
    std::string error;
    BlockIndexStartupBootstrap bt;
    BOOST_REQUIRE_MESSAGE(OpenBootstrap(fx, bt, error) == BLOCK_INDEX_STARTUP_OK, error);
    CBlockIndex* g = bt.GenesisObject();
    BOOST_REQUIRE(g != NULL);
    BOOST_CHECK(g->pprev == NULL);
    BOOST_CHECK(g->pnext == NULL);
    BOOST_CHECK(g->pskip == NULL);
}

BOOST_AUTO_TEST_SUITE_END()
