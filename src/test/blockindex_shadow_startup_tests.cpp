#include <boost/test/unit_test.hpp>
#include "../blockindex_shadow_runtime.h"
#include "../blockindex_generation_lifecycle.h"
#include "../blockindex_generation_builder.h"
#include "../blockindex_shadow_startup.h"
#include "../fixed_blockindex_store.h"
#include "../blockindex_hashindex.h"
#include "../blockindex_activeindex.h"
#include "../main.h"
#include "../util.h"

#include <boost/filesystem.hpp>

#include <stdio.h>
#include <string>
#include <vector>

namespace {

static boost::filesystem::path UniqueRoot(const std::string& tag)
{
    boost::filesystem::path model =
        boost::filesystem::path("innova-blockindex-shadow-") /
        boost::filesystem::path(tag + "-%%%%-%%%%-%%%%");
    boost::filesystem::path dir = boost::filesystem::temp_directory_path() /
        boost::filesystem::unique_path(model);
    boost::filesystem::create_directories(dir);
    return dir;
}

static BlockIndexRecord MakeRecord(uint64_t nonceBase, int32_t height, const uint256& hashPrev)
{
    BlockIndexRecord rec;
    rec.hash = uint256(nonceBase + 1);
    rec.hashPrev = hashPrev;
    rec.height = height;
    rec.nFile = (uint32_t)(10 + nonceBase);
    rec.nBlockPos = (uint32_t)(20 + nonceBase);
    rec.nFlags = 0;
    rec.nVersion = 7;
    rec.nTime = (uint32_t)(1700000000U + (unsigned int)height);
    rec.nBits = 0x1d00ffffU;
    rec.nNonce = (uint32_t)(9000 + nonceBase);
    rec.nMint = 100 + (int64_t)nonceBase;
    rec.nMoneySupply = 500 + (int64_t)(nonceBase * 3);
    rec.nStakeModifier = 0x1234000000000000ULL + nonceBase;
    rec.nStakeTime = 0;
    rec.hashProof = uint256(nonceBase + 2000);
    rec.hashMerkleRoot = uint256(nonceBase + 3000);
    return rec;
}

static BlockIndexGenerationSource BuildSyntheticSource(int tipHeight, int sideCount,
                                                       uint64_t activeBase, uint64_t sideBase)
{
    BlockIndexGenerationSource src;
    std::vector<uint256> activeHash;
    for (int h = 0; h <= tipHeight; ++h)
    {
        uint256 hprev = (h == 0) ? uint256(0) : activeHash[h - 1];
        BlockIndexRecord rec = MakeRecord(activeBase + (uint64_t)h, h, hprev);
        rec.hash = uint256(activeBase + (uint64_t)h);
        activeHash.push_back(rec.hash);
        BlockIndexGenerationSourceRecord s; s.hash = rec.hash; s.record = rec;
        src.records.push_back(s);
    }
    for (int k = 0; k < sideCount; ++k)
    {
        int forkH = k % (tipHeight + 1);
        uint256 hprev = activeHash[forkH];
        int hgt = forkH + 1;
        BlockIndexRecord rec = MakeRecord(sideBase + (uint64_t)k, hgt, hprev);
        rec.hash = uint256(sideBase + (uint64_t)k);
        BlockIndexGenerationSourceRecord s; s.hash = rec.hash; s.record = rec;
        src.records.push_back(s);
    }
    src.hashBestChain = activeHash[tipHeight];
    src.foundBestChain = true;
    return src;
}

// A mock legacy oracle. It models the authoritative legacy chain:
//  - activeChain[0..legacyTip] built from `activeBase` (so a generation built
//    from the same base shares the overlapping prefix's hashes/fields exactly).
//  - byHash indexed from an optional full source (active + side records) so
//    bounded hash samples that resolve side records find them, matching the real
//    daemon's mapBlockIndex.
class MockLegacyOracle : public BlockIndexShadowLegacyOracle
{
public:
    int legacyTip;
    std::vector<BlockIndexRecord> activeChain;   // 0..legacyTip
    std::map<uint256, BlockIndexRecord> byHash;  // every known record
    bool omitAtShadowTip;                        // simulate missing legacy block at a height

    MockLegacyOracle() : legacyTip(-1), omitAtShadowTip(false) {}

    // Build active chain 0..n from activeBase. Index active records by hash.
    void BuildActive(int n, uint64_t activeBase)
    {
        activeChain.clear();
        std::vector<uint256> hashes;
        for (int h = 0; h <= n; ++h)
        {
            uint256 hprev = (h == 0) ? uint256(0) : hashes[h - 1];
            BlockIndexRecord rec = MakeRecord(activeBase + (uint64_t)h, h, hprev);
            rec.hash = uint256(activeBase + (uint64_t)h);
            hashes.push_back(rec.hash);
            activeChain.push_back(rec);
            byHash[rec.hash] = rec;
        }
        legacyTip = n;
    }

    // Seed byHash with side-chain (non-active) source records so hash samples
    // resolve them (mirrors mapBlockIndex containing forks).
    void AddSideRecords(const BlockIndexGenerationSource& src)
    {
        for (size_t i = 0; i < src.records.size(); ++i)
            byHash[src.records[i].hash] = src.records[i].record;
    }

    virtual int GetLegacyTipHeight() { return legacyTip; }

    virtual bool GetLegacyActiveRecord(int height, BlockIndexRecord* out)
    {
        if (height < 0 || height > legacyTip) return false;
        if (omitAtShadowTip) return false;
        *out = activeChain[height];
        return true;
    }

    virtual bool GetLegacyRecordByHash(const uint256& hash, BlockIndexRecord* out)
    {
        std::map<uint256, BlockIndexRecord>::iterator it = byHash.find(hash);
        if (it == byHash.end()) return false;
        *out = it->second;
        return true;
    }
};

// Build a generation into root and return its source (used to seed the mock
// byHash identically).
static BlockIndexGenerationSource BuildShadowGeneration(const std::string& root, uint64_t gen,
                                                        int tipHeight, int sideCount,
                                                        uint64_t activeBase, uint64_t sideBase)
{
    BlockIndexGenerationSource src = BuildSyntheticSource(tipHeight, sideCount, activeBase, sideBase);
    std::string error;
    boost::filesystem::path tmp = boost::filesystem::temp_directory_path() /
        boost::filesystem::unique_path("innova-shadow-build-%%%%-%%%%");
    BlockIndexGenerationBuilder builder;
    BlockIndexGenerationStats stats;
    BOOST_REQUIRE_MESSAGE(builder.Build(src, tmp.string(), gen, &stats, &error), error);
    builder.Close();
    boost::filesystem::create_directories(boost::filesystem::path(root));
    boost::filesystem::path staging = boost::filesystem::path(root) /
        BlockIndexGenerationManager::StagingName(gen);
    boost::filesystem::rename(tmp, staging);
    BOOST_REQUIRE_EQUAL(BlockIndexGenerationManager::PublishGeneration(root, gen, &error),
                        (int)BLOCK_INDEX_LIFECYCLE_OK);
    error.clear();
    BOOST_REQUIRE_EQUAL(BlockIndexGenerationManager::SelectGeneration(root, gen, &error),
                        (int)BLOCK_INDEX_LIFECYCLE_OK);
    return src;
}

} // namespace

BOOST_AUTO_TEST_SUITE(blockindex_shadow_startup_tests)

BOOST_AUTO_TEST_CASE(shadow_disabled_when_empty_root)
{
    boost::filesystem::path root = UniqueRoot("disabled");
    BlockIndexShadowOpenConfig cfg;
    cfg.root = root.string();
    MockLegacyOracle legacy;
    legacy.BuildActive(150, 1000);
    BlockIndexV2ShadowRuntime runtime;
    std::string error;
    const BlockIndexV2ShadowState& st = runtime.OpenShadow(cfg, &legacy, &error);
    BOOST_CHECK_EQUAL(st.compatibility, (int)BLOCK_INDEX_SHADOW_COMPAT_NOT_PUBLISHED);
    BOOST_CHECK(!st.structuralValidationOk);
}

BOOST_AUTO_TEST_CASE(exact_tip_compatibility)
{
    boost::filesystem::path root = UniqueRoot("exact");
    BlockIndexGenerationSource src = BuildShadowGeneration(root.string(), 1, 100, 2, 1000, 9000);
    MockLegacyOracle legacy;
    legacy.BuildActive(100, 1000); // legacy tip == shadow tip; same base => identical prefix
    legacy.AddSideRecords(src);
    BlockIndexV2ShadowRuntime runtime;
    std::string error;
    BlockIndexShadowOpenConfig cfg; cfg.root = root.string();
    const BlockIndexV2ShadowState& st = runtime.OpenShadow(cfg, &legacy, &error);
    BOOST_CHECK(st.structuralValidationOk);
    BOOST_CHECK_EQUAL(st.compatibility, (int)BLOCK_INDEX_SHADOW_COMPAT_EXACT);
    BOOST_CHECK(st.tipOnLegacyActiveChain);
    BOOST_CHECK_EQUAL(st.sampleMismatches, 0ULL);
    BOOST_CHECK_EQUAL(st.authoritative, false);
}

BOOST_AUTO_TEST_CASE(ancestor_compatibility_legacy_ahead)
{
    boost::filesystem::path root = UniqueRoot("ancestor");
    BlockIndexGenerationSource src = BuildShadowGeneration(root.string(), 1, 100, 2, 1000, 9000);
    MockLegacyOracle legacy;
    legacy.BuildActive(150, 1000); // legacy tip 150, overlapping 0..100 identical
    legacy.AddSideRecords(src);
    BlockIndexV2ShadowRuntime runtime;
    std::string error;
    BlockIndexShadowOpenConfig cfg; cfg.root = root.string();
    const BlockIndexV2ShadowState& st = runtime.OpenShadow(cfg, &legacy, &error);
    BOOST_CHECK(st.structuralValidationOk);
    BOOST_CHECK_EQUAL(st.compatibility, (int)BLOCK_INDEX_SHADOW_COMPAT_ANCESTOR);
    BOOST_CHECK(st.tipOnLegacyActiveChain);
    BOOST_CHECK_EQUAL(st.heightLag, 50);
    BOOST_CHECK_EQUAL(st.sampleMismatches, 0ULL);
}

BOOST_AUTO_TEST_CASE(shadow_ahead_fails)
{
    boost::filesystem::path root = UniqueRoot("ahead");
    BuildShadowGeneration(root.string(), 1, 100, 2, 1000, 9000);
    MockLegacyOracle legacy;
    legacy.BuildActive(50, 1000); // legacy tip 50 < shadow tip 100
    BlockIndexV2ShadowRuntime runtime;
    std::string error;
    BlockIndexShadowOpenConfig cfg; cfg.root = root.string();
    const BlockIndexV2ShadowState& st = runtime.OpenShadow(cfg, &legacy, &error);
    BOOST_CHECK_EQUAL(st.compatibility, (int)BLOCK_INDEX_SHADOW_COMPAT_AHEAD);
    BOOST_CHECK(!st.tipOnLegacyActiveChain);
}

BOOST_AUTO_TEST_CASE(shadow_diverged_at_same_historical_height_fails)
{
    boost::filesystem::path root = UniqueRoot("diverged");
    // generation active base = 1000 (tip hash = uint256(1100)).
    BuildShadowGeneration(root.string(), 1, 100, 2, 1000, 9000);
    MockLegacyOracle legacy;
    // legacy uses a DIFFERENT base so the legacy active block at height 100 has a
    // different hash than the shadow committed tip (uint256(1100)) -> DIVERGED.
    legacy.BuildActive(150, 7777);
    BlockIndexV2ShadowRuntime runtime;
    std::string error;
    BlockIndexShadowOpenConfig cfg; cfg.root = root.string();
    const BlockIndexV2ShadowState& st = runtime.OpenShadow(cfg, &legacy, &error);
    BOOST_CHECK_EQUAL(st.compatibility, (int)BLOCK_INDEX_SHADOW_COMPAT_DIVERGED);
    BOOST_CHECK(!st.tipOnLegacyActiveChain);
}

BOOST_AUTO_TEST_CASE(current_absent_and_corrupt_and_missing_generation)
{
    // absent
    {
        boost::filesystem::path root = UniqueRoot("absent");
        MockLegacyOracle legacy; legacy.BuildActive(10, 1000);
        BlockIndexV2ShadowRuntime runtime;
        std::string error;
        BlockIndexShadowOpenConfig cfg; cfg.root = root.string();
        const BlockIndexV2ShadowState& st = runtime.OpenShadow(cfg, &legacy, &error);
        BOOST_CHECK_EQUAL(st.compatibility, (int)BLOCK_INDEX_SHADOW_COMPAT_NOT_PUBLISHED);
    }
    // corrupt CURRENT
    {
        boost::filesystem::path root = UniqueRoot("currupt");
        BuildShadowGeneration(root.string(), 1, 10, 0, 1000, 9000);
        std::string cp = (boost::filesystem::path(root.string()) / BLOCK_INDEX_CURRENT_FILE_NAME).string();
        FILE* f = fopen(cp.c_str(), "r+b");
        BOOST_REQUIRE(f != NULL);
        fputc('X', f); fclose(f);
        MockLegacyOracle legacy; legacy.BuildActive(10, 1000);
        BlockIndexV2ShadowRuntime runtime;
        std::string error;
        BlockIndexShadowOpenConfig cfg; cfg.root = root.string();
        const BlockIndexV2ShadowState& st = runtime.OpenShadow(cfg, &legacy, &error);
        BOOST_CHECK_EQUAL(st.compatibility, (int)BLOCK_INDEX_SHADOW_COMPAT_CORRUPT);
    }
    // selected generation missing
    {
        boost::filesystem::path root = UniqueRoot("missing-gen");
        std::string enc;
        BlockIndexCurrentRecord r; r.generation = 99;
        std::string e;
        EncodeBlockIndexCurrentRecord(r, &enc, &e);
        boost::filesystem::create_directories(boost::filesystem::path(root.string()));
        FILE* f = fopen((root.string() + "/CURRENT.tmp").c_str(), "wb");
        fwrite(enc.data(), 1, enc.size(), f); fclose(f);
        rename((root.string() + "/CURRENT.tmp").c_str(), (root.string() + "/CURRENT").c_str());
        MockLegacyOracle legacy; legacy.BuildActive(10, 1000);
        BlockIndexV2ShadowRuntime runtime;
        std::string error;
        BlockIndexShadowOpenConfig cfg; cfg.root = root.string();
        const BlockIndexV2ShadowState& st = runtime.OpenShadow(cfg, &legacy, &error);
        BOOST_CHECK(!st.structuralValidationOk);
        BOOST_CHECK_EQUAL(st.generation, 99ULL);
    }
}

BOOST_AUTO_TEST_CASE(structural_generation_corruption_detected)
{
    boost::filesystem::path root = UniqueRoot("structcorrupt");
    BuildShadowGeneration(root.string(), 1, 20, 1, 1000, 9000);
    boost::filesystem::remove(boost::filesystem::path(root.string()) / "gen-000001" / BLOCK_INDEX_RECORDS_FILE_NAME);
    MockLegacyOracle legacy; legacy.BuildActive(20, 1000);
    BlockIndexV2ShadowRuntime runtime;
    std::string error;
    BlockIndexShadowOpenConfig cfg; cfg.root = root.string();
    const BlockIndexV2ShadowState& st = runtime.OpenShadow(cfg, &legacy, &error);
    BOOST_CHECK(!st.structuralValidationOk);
}

BOOST_AUTO_TEST_CASE(deterministic_samples_zero_mismatch_and_injected_mismatch_detected)
{
    // Clean sample: 0 mismatch (sampleHeightChecks >= heightSamples because
    // boundary checks add a few extra active-height checks).
    {
        boost::filesystem::path root = UniqueRoot("clean-sample");
        BlockIndexGenerationSource src = BuildShadowGeneration(root.string(), 1, 60, 3, 1000, 9000);
        MockLegacyOracle legacy; legacy.BuildActive(60, 1000); legacy.AddSideRecords(src);
        BlockIndexV2ShadowRuntime runtime;
        std::string error;
        BlockIndexShadowOpenConfig cfg; cfg.root = root.string();
        const BlockIndexV2ShadowState& st = runtime.OpenShadow(cfg, &legacy, &error);
        BOOST_CHECK(st.structuralValidationOk);
        BOOST_CHECK_EQUAL(st.compatibility, (int)BLOCK_INDEX_SHADOW_COMPAT_EXACT);
        BOOST_CHECK_EQUAL(st.sampleHashChecks, cfg.hashSamples);
        BOOST_CHECK_GE(st.sampleHeightChecks, cfg.heightSamples); // + boundary checks
        BOOST_CHECK_EQUAL(st.sampleMismatches, 0ULL);
    }
    // Injected mismatch: legacy shares active prefix but a middle active record
    // differs from the V2 generation -> sampled mismatches detected.
    {
        boost::filesystem::path root = UniqueRoot("inject-mismatch");
        BlockIndexGenerationSource src = BuildShadowGeneration(root.string(), 1, 40, 2, 1000, 9000);
        MockLegacyOracle legacy; legacy.BuildActive(40, 1000); legacy.AddSideRecords(src);
        BOOST_REQUIRE((int)legacy.activeChain.size() > 20);
        BlockIndexRecord rec = legacy.activeChain[20];
        rec.hashMerkleRoot = uint256(0x9999);
        rec.nTime = 1; // corrupt an admitted V1 field
        legacy.activeChain[20] = rec;
        BlockIndexV2ShadowRuntime runtime;
        std::string error;
        BlockIndexShadowOpenConfig cfg; cfg.root = root.string();
        const BlockIndexV2ShadowState& st = runtime.OpenShadow(cfg, &legacy, &error);
        BOOST_CHECK_EQUAL(st.compatibility, (int)BLOCK_INDEX_SHADOW_COMPAT_EXACT); // tip still matches
        BOOST_CHECK_GT(st.sampleMismatches, 0ULL); // corruption detected in samples
    }
}

BOOST_AUTO_TEST_CASE(non_strict_failure_keeps_shadow_errored_but_loggable)
{
    boost::filesystem::path root = UniqueRoot("nonstrict");
    MockLegacyOracle legacy; legacy.BuildActive(5, 1000);
    BlockIndexV2ShadowRuntime runtime;
    std::string error;
    BlockIndexShadowOpenConfig cfg; cfg.root = root.string();
    bool threw = false;
    try { runtime.OpenShadow(cfg, &legacy, &error); }
    catch (...) { threw = true; }
    BOOST_CHECK(!threw); // runtime never throws
    const BlockIndexV2ShadowState& st = runtime.GetState();
    BOOST_CHECK(!st.structuralValidationOk); // NOT a partial READY state
}

BOOST_AUTO_TEST_CASE(diagnostics_contain_authoritative_false)
{
    boost::filesystem::path root = UniqueRoot("diag");
    BuildShadowGeneration(root.string(), 1, 10, 0, 1000, 9000);
    MockLegacyOracle legacy; legacy.BuildActive(10, 1000);
    BlockIndexV2ShadowRuntime runtime;
    std::string error;
    BlockIndexShadowOpenConfig cfg; cfg.root = root.string();
    const BlockIndexV2ShadowState& st = runtime.OpenShadow(cfg, &legacy, &error);
    BOOST_CHECK_EQUAL(st.authoritative, false);
    BOOST_CHECK(st.enabled);
    BOOST_CHECK_EQUAL(st.generation, 1ULL);
}

BOOST_AUTO_TEST_SUITE_END()