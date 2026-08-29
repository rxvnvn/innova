#include <boost/test/unit_test.hpp>
#include "../blockindex_generation_lifecycle.h"
#include "../blockindex_generation_builder.h"
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
        boost::filesystem::path("innova-blockindex-lifecycle-") /
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

// Build a small COMPLETE generation directly at dir/gen-<n> (or build-<n>.tmp).
static void BuildGenerationInto(const std::string& dirPath, uint64_t gen,
                                const std::string& entryName, int tipHeight, int sideCount,
                                uint64_t activeBase, uint64_t sideBase)
{
    BlockIndexGenerationSource src = BuildSyntheticSource(tipHeight, sideCount, activeBase, sideBase);
    std::string error;
    BlockIndexGenerationBuilder builder;
    // Build into a temp then move to the target entry name.
    boost::filesystem::path tmp = boost::filesystem::temp_directory_path() /
        boost::filesystem::unique_path("innova-lifecycle-build-%%%%-%%%%");
    BlockIndexGenerationStats stats;
    bool ok = builder.Build(src, tmp.string(), gen, &stats, &error);
    builder.Close();
    BOOST_REQUIRE_MESSAGE(ok, error);
    boost::filesystem::path target = boost::filesystem::path(dirPath) / entryName;
    boost::filesystem::create_directories(boost::filesystem::path(dirPath));
    BOOST_REQUIRE(!boost::filesystem::exists(target));
    boost::filesystem::rename(tmp, target);
}

// Build a valid COMPLETE staging generation build-<n>.tmp under root.
static void BuildStaging(const std::string& root, uint64_t gen)
{
    BuildGenerationInto(root, gen, BlockIndexGenerationManager::StagingName(gen),
                        6, 2, 10000 * (gen + 1), 20000 * (gen + 1));
}

} // namespace

BOOST_AUTO_TEST_SUITE(blockindex_generation_lifecycle_tests)

BOOST_AUTO_TEST_CASE(current_codec_exact_bytes_roundtrip_and_fail_closed)
{
    std::string enc;
    std::string error;
    BlockIndexCurrentRecord rec;
    rec.generation = 0x0102030405060708ULL;
    BOOST_REQUIRE(EncodeBlockIndexCurrentRecord(rec, &enc, &error));
    BOOST_CHECK_EQUAL((int)enc.size(), (int)BLOCK_INDEX_CURRENT_SIZE_V1);
    // magic
    BOOST_CHECK_EQUAL(std::string(enc.data(), 6), std::string("INNBCU"));
    BOOST_CHECK_EQUAL((unsigned char)enc[6], 'R');
    BOOST_CHECK_EQUAL((unsigned char)enc[7], '1');
    // generation LE at offset 16
    BOOST_CHECK_EQUAL((unsigned char)enc[16], 0x08);
    BOOST_CHECK_EQUAL((unsigned char)enc[23], 0x01);

    // determinism
    std::string enc2;
    BOOST_REQUIRE(EncodeBlockIndexCurrentRecord(rec, &enc2, &error));
    BOOST_CHECK_EQUAL_COLLECTIONS(enc.begin(), enc.end(), enc2.begin(), enc2.end());

    // roundtrip
    BlockIndexCurrentRecord out;
    BOOST_REQUIRE(DecodeBlockIndexCurrentRecord(enc.data(), enc.size(), &out, &error));
    BOOST_CHECK_EQUAL(out.generation, rec.generation);
    BOOST_CHECK_EQUAL(out.formatVersion, BLOCK_INDEX_CURRENT_FORMAT_VERSION);

    // generation 0 invalid at encode and decode
    error.clear();
    BlockIndexCurrentRecord z; z.generation = 0;
    BOOST_CHECK(!EncodeBlockIndexCurrentRecord(z, &enc, &error));
    BOOST_CHECK(!error.empty());

    // bad magic
    error.clear();
    std::string bad = enc;
    bad[0] = 'X';
    BlockIndexCurrentRecord o2;
    BOOST_CHECK(!DecodeBlockIndexCurrentRecord(bad.data(), bad.size(), &o2, &error));
    BOOST_CHECK(!error.empty());

    // unsupported version: flip format_version to 99
    error.clear();
    std::string v2 = enc;
    v2[8] = 99;
    BlockIndexCurrentRecord o3;
    BOOST_CHECK(!DecodeBlockIndexCurrentRecord(v2.data(), v2.size(), &o3, &error));
    BOOST_CHECK(!error.empty());

    // truncated
    error.clear();
    BOOST_CHECK(!DecodeBlockIndexCurrentRecord(enc.data(), enc.size() - 1, &o3, &error));
    BOOST_CHECK(!error.empty());

    // checksum corruption
    error.clear();
    std::string c = enc;
    c[16] ^= (char)0x40; // alter generation, breaking checksum
    BlockIndexCurrentRecord o4;
    BOOST_CHECK(!DecodeBlockIndexCurrentRecord(c.data(), c.size(), &o4, &error));
    BOOST_CHECK(!error.empty());

    // trailing garbage not applicable to fixed-size; extra length rejected
    error.clear();
    std::string extra = enc + "x";
    BlockIndexCurrentRecord o5;
    BOOST_CHECK(!DecodeBlockIndexCurrentRecord(extra.data(), extra.size(), &o5, &error));
    BOOST_CHECK(!error.empty());
}

BOOST_AUTO_TEST_CASE(generation_naming_are_exact_and_large_ids_supported)
{
    BOOST_CHECK_EQUAL(BlockIndexGenerationManager::GenerationName(1), std::string("gen-000001"));
    BOOST_CHECK_EQUAL(BlockIndexGenerationManager::GenerationName(2), std::string("gen-000002"));
    BOOST_CHECK_EQUAL(BlockIndexGenerationManager::StagingName(1), std::string("build-000001.tmp"));
    BOOST_CHECK_EQUAL(BlockIndexGenerationManager::StagingName(2), std::string("build-000002.tmp"));
    // large ids
    BOOST_CHECK_EQUAL(BlockIndexGenerationManager::GenerationName(999999), std::string("gen-999999"));
    BOOST_CHECK_EQUAL(BlockIndexGenerationManager::GenerationName(1000000), std::string("gen-1000000"));
    BOOST_CHECK_EQUAL(BlockIndexGenerationManager::GenerationName(18446744073709551615ULL),
                      std::string("gen-18446744073709551615"));
    // path forms
    BOOST_CHECK_EQUAL(BlockIndexGenerationManager::GenerationPath("/r", 1), "/r/gen-000001");
    BOOST_CHECK_EQUAL(BlockIndexGenerationManager::StagingPath("/r", 1), "/r/build-000001.tmp");
}

BOOST_AUTO_TEST_CASE(publish_select_open_lifecycle_roundtrip)
{
    boost::filesystem::path root = UniqueRoot("roundtrip");
    BuildStaging(root.string(), 1);

    // CURRENT absent -> NOT_PUBLISHED
    BlockIndexCurrentRecord cur;
    std::string error;
    BOOST_CHECK_EQUAL(BlockIndexGenerationManager::ReadCurrent(root.string(), &cur, &error),
                      (int)BLOCK_INDEX_LIFECYCLE_NOT_PUBLISHED);

    // validate staging structurally (build-1.tmp is not a stable gen-1 path;
    // gen-000001 does not exist yet)
    BOOST_CHECK_NE(BlockIndexGenerationManager::ValidateGeneration(root.string(), 1, &error),
                   (int)BLOCK_INDEX_LIFECYCLE_OK);

    // publish build-1.tmp -> gen-1
    BOOST_CHECK_EQUAL(BlockIndexGenerationManager::PublishGeneration(root.string(), 1, &error),
                      (int)BLOCK_INDEX_LIFECYCLE_OK);
    BOOST_CHECK(boost::filesystem::exists(boost::filesystem::path(root.string()) / "gen-000001"));
    BOOST_CHECK(!boost::filesystem::exists(boost::filesystem::path(root.string()) / "build-000001.tmp"));

    // publication does NOT change CURRENT (absent still)
    BOOST_CHECK_EQUAL(BlockIndexGenerationManager::ReadCurrent(root.string(), &cur, &error),
                      (int)BLOCK_INDEX_LIFECYCLE_NOT_PUBLISHED);

    // validate stable generation
    BOOST_CHECK_EQUAL(BlockIndexGenerationManager::ValidateGeneration(root.string(), 1, &error),
                      (int)BLOCK_INDEX_LIFECYCLE_OK);

    // select generation 1 -> writes CURRENT
    BOOST_CHECK_EQUAL(BlockIndexGenerationManager::SelectGeneration(root.string(), 1, &error),
                      (int)BLOCK_INDEX_LIFECYCLE_OK);
    BOOST_CHECK(boost::filesystem::exists(boost::filesystem::path(root.string()) / "CURRENT"));

    // CURRENT now resolves to 1
    BOOST_CHECK_EQUAL(BlockIndexGenerationManager::ReadCurrent(root.string(), &cur, &error),
                      (int)BLOCK_INDEX_LIFECYCLE_OK);
    BOOST_CHECK_EQUAL(cur.generation, 1ULL);

    // OpenCurrent resolves 1
    uint64_t sel = 0;
    BOOST_CHECK_EQUAL(BlockIndexGenerationManager::OpenCurrent(root.string(), &sel, &error),
                      (int)BLOCK_INDEX_LIFECYCLE_OK);
    BOOST_CHECK_EQUAL(sel, 1ULL);
}

BOOST_AUTO_TEST_CASE(publish_rejects_building_corrupt_id_mismatch_and_missing_component)
{
    boost::filesystem::path root = UniqueRoot("reject");

    // 1) BUILDING manifest generation cannot... actually builder finalizes COMPLETE,
    // so to get a BUILDING we craft one: build staging then rewrite MANIFEST to BUILDING.
    BuildStaging(root.string(), 1);
    // corrupt MANIFEST state to BUILDING manually
    {
        std::string mp = (boost::filesystem::path(root.string()) / "build-000001.tmp" / BLOCK_INDEX_MANIFEST_FILE_NAME).string();
        unsigned char d[88];
        FILE* g = fopen(mp.c_str(), "rb");
        BOOST_REQUIRE(g != NULL);
        BOOST_REQUIRE(fread(d, 1, 88, g) == 88);
        fclose(g);
        // state field at offset 52 (uint32 LE)
        d[52] = BLOCK_INDEX_MANIFEST_BUILDING;
        FILE* f = fopen(mp.c_str(), "r+b");
        BOOST_REQUIRE(f != NULL);
        BOOST_REQUIRE(fwrite(d, 1, 88, f) == 88);
        fclose(f);
    }
    std::string error;
    BOOST_CHECK_EQUAL(BlockIndexGenerationManager::PublishGeneration(root.string(), 1, &error),
                      (int)BLOCK_INDEX_LIFECYCLE_ERROR);
    BOOST_CHECK(!error.empty());
    BOOST_CHECK(!boost::filesystem::exists(boost::filesystem::path(root.string()) / "gen-000001"));

    // 2) generation-id mismatch: build as gen 2 but publish as gen 1
    boost::filesystem::path root2 = UniqueRoot("reject-id");
    BuildGenerationInto(root2.string(), 2, BlockIndexGenerationManager::StagingName(1),
                        6, 2, 1000, 2000); // staging name build-000001.tmp but MANIFEST generation 2
    error.clear();
    BOOST_CHECK_EQUAL(BlockIndexGenerationManager::PublishGeneration(root2.string(), 1, &error),
                      (int)BLOCK_INDEX_LIFECYCLE_ERROR);
    BOOST_CHECK(!error.empty());

    // 3) missing required component (delete records.dat)
    boost::filesystem::path root3 = UniqueRoot("reject-missing");
    BuildStaging(root3.string(), 1);
    boost::filesystem::remove(boost::filesystem::path(root3.string()) / "build-000001.tmp" / BLOCK_INDEX_RECORDS_FILE_NAME);
    error.clear();
    BOOST_CHECK_EQUAL(BlockIndexGenerationManager::PublishGeneration(root3.string(), 1, &error),
                      (int)BLOCK_INDEX_LIFECYCLE_ERROR);
    BOOST_CHECK(!error.empty());
}

BOOST_AUTO_TEST_CASE(current_absent_corrupt_and_missing_generation_semantics)
{
    boost::filesystem::path root = UniqueRoot("curr-semantics");
    BuildStaging(root.string(), 1);
    std::string error;

    // publish + select 1
    BOOST_CHECK_EQUAL(BlockIndexGenerationManager::PublishGeneration(root.string(), 1, &error),
                      (int)BLOCK_INDEX_LIFECYCLE_OK);
    error.clear();
    BOOST_CHECK_EQUAL(BlockIndexGenerationManager::SelectGeneration(root.string(), 1, &error),
                      (int)BLOCK_INDEX_LIFECYCLE_OK);
    error.clear();

    // corrupt CURRENT -> CORRUPT (distinct from absent)
    {
        std::string cp = (boost::filesystem::path(root.string()) / BLOCK_INDEX_CURRENT_FILE_NAME).string();
        FILE* g = fopen(cp.c_str(), "rb");
        BOOST_REQUIRE(g != NULL);
        unsigned char d[28];
        BOOST_REQUIRE(fread(d, 1, 28, g) == 28);
        fclose(g);
        d[0] ^= (char)0xFF;
        FILE* f = fopen(cp.c_str(), "r+b");
        BOOST_REQUIRE(f != NULL);
        fwrite(d, 1, 28, f);
        fclose(f);
    }
    BlockIndexCurrentRecord cur;
    error.clear();
    BOOST_CHECK_EQUAL(BlockIndexGenerationManager::ReadCurrent(root.string(), &cur, &error),
                      (int)BLOCK_INDEX_LIFECYCLE_CORRUPT);
    // must NOT auto-select any generation
    uint64_t sel = 999;
    error.clear();
    BOOST_CHECK_EQUAL(BlockIndexGenerationManager::OpenCurrent(root.string(), &sel, &error),
                      (int)BLOCK_INDEX_LIFECYCLE_CORRUPT);

    // delete CURRENT -> absent/NOT_PUBLISHED
    boost::filesystem::remove(boost::filesystem::path(root.string()) / BLOCK_INDEX_CURRENT_FILE_NAME);
    error.clear();
    BOOST_CHECK_EQUAL(BlockIndexGenerationManager::ReadCurrent(root.string(), &cur, &error),
                      (int)BLOCK_INDEX_LIFECYCLE_NOT_PUBLISHED);

    // CURRENT points to a missing generation -> MISSING_GENERATION (no fallback)
    {
        std::string err2;
        BlockIndexGenerationManager::SelectGeneration(root.string(), 1, &err2); // gen-1 exists
        err2.clear();
        // craft CURRENT -> 99 whose gen-000099 doesn't exist
        std::string enc;
        BlockIndexCurrentRecord r; r.generation = 99;
        EncodeBlockIndexCurrentRecord(r, &enc, &err2);
        // write CURRENT.tmp then rename over (use the durable writer semantics)
        FILE* f = fopen((root.string() + "/CURRENT.tmp").c_str(), "wb");
        fwrite(enc.data(), 1, enc.size(), f); fclose(f);
        rename((root.string() + "/CURRENT.tmp").c_str(), (root.string() + "/CURRENT").c_str());
    }
    error.clear();
    sel = 0;
    BOOST_CHECK_EQUAL(BlockIndexGenerationManager::OpenCurrent(root.string(), &sel, &error),
                      (int)BLOCK_INDEX_LIFECYCLE_MISSING_GENERATION);
    // ReadCurrent still decodes 99 (selection metadata intact) but open fails on missing
    error.clear();
    BOOST_CHECK_EQUAL(BlockIndexGenerationManager::ReadCurrent(root.string(), &cur, &error),
                      (int)BLOCK_INDEX_LIFECYCLE_OK);
    BOOST_CHECK_EQUAL(cur.generation, 99ULL);
}

BOOST_AUTO_TEST_CASE(rollback_selection_switches_between_generations)
{
    boost::filesystem::path root = UniqueRoot("rollback");
    std::string error;
    BuildStaging(root.string(), 1);
    BuildStaging(root.string(), 2);
    BOOST_CHECK_EQUAL(BlockIndexGenerationManager::PublishGeneration(root.string(), 1, &error), (int)BLOCK_INDEX_LIFECYCLE_OK);
    error.clear();
    BOOST_CHECK_EQUAL(BlockIndexGenerationManager::PublishGeneration(root.string(), 2, &error), (int)BLOCK_INDEX_LIFECYCLE_OK);
    error.clear();

    uint64_t sel = 0;
    BOOST_CHECK_EQUAL(BlockIndexGenerationManager::SelectGeneration(root.string(), 1, &error), (int)BLOCK_INDEX_LIFECYCLE_OK);
    error.clear();
    BOOST_CHECK_EQUAL(BlockIndexGenerationManager::OpenCurrent(root.string(), &sel, &error), (int)BLOCK_INDEX_LIFECYCLE_OK);
    BOOST_CHECK_EQUAL(sel, 1ULL);

    BOOST_CHECK_EQUAL(BlockIndexGenerationManager::SelectGeneration(root.string(), 2, &error), (int)BLOCK_INDEX_LIFECYCLE_OK);
    error.clear();
    BOOST_CHECK_EQUAL(BlockIndexGenerationManager::OpenCurrent(root.string(), &sel, &error), (int)BLOCK_INDEX_LIFECYCLE_OK);
    BOOST_CHECK_EQUAL(sel, 2ULL);

    // rollback
    BOOST_CHECK_EQUAL(BlockIndexGenerationManager::SelectGeneration(root.string(), 1, &error), (int)BLOCK_INDEX_LIFECYCLE_OK);
    error.clear();
    BOOST_CHECK_EQUAL(BlockIndexGenerationManager::OpenCurrent(root.string(), &sel, &error), (int)BLOCK_INDEX_LIFECYCLE_OK);
    BOOST_CHECK_EQUAL(sel, 1ULL);

    // neither generation modified by selection
    BOOST_CHECK(boost::filesystem::exists(boost::filesystem::path(root.string()) / "gen-000002"));
    BlockIndexCurrentRecord cur;
    BOOST_CHECK_EQUAL(BlockIndexGenerationManager::ReadCurrent(root.string(), &cur, &error), (int)BLOCK_INDEX_LIFECYCLE_OK);
    BOOST_CHECK_EQUAL(cur.generation, 1ULL);
}

BOOST_AUTO_TEST_CASE(no_auto_discovery_and_orphan_and_build_ignored)
{
    boost::filesystem::path root = UniqueRoot("nodiscovery");
    std::string error;
    // place several stable gen dirs + a higher orphan + a build tmp
    BuildGenerationInto(root.string(), 1, "gen-000001", 3, 1, 1000, 2000);
    BuildGenerationInto(root.string(), 2, "gen-000002", 3, 1, 3000, 4000);
    BuildGenerationInto(root.string(), 999999, "gen-999999", 3, 1, 5000, 6000);
    BuildStaging(root.string(), 3); // build-000003.tmp

    // CURRENT -> gen-000001
    BOOST_CHECK_EQUAL(BlockIndexGenerationManager::SelectGeneration(root.string(), 1, &error), (int)BLOCK_INDEX_LIFECYCLE_OK);
    error.clear();
    uint64_t sel = 0;
    BOOST_CHECK_EQUAL(BlockIndexGenerationManager::OpenCurrent(root.string(), &sel, &error), (int)BLOCK_INDEX_LIFECYCLE_OK);
    BOOST_CHECK_EQUAL(sel, 1ULL); // must open gen-1, NOT 999999 or 2 or build-3

    // corrupt/delete CURRENT: must NOT auto-select any other generation
    boost::filesystem::remove(boost::filesystem::path(root.string()) / BLOCK_INDEX_CURRENT_FILE_NAME);
    error.clear();
    sel = 999;
    BOOST_CHECK_EQUAL(BlockIndexGenerationManager::OpenCurrent(root.string(), &sel, &error),
                      (int)BLOCK_INDEX_LIFECYCLE_NOT_PUBLISHED);
    // build-3 must never be selected even if it is COMPLETE
    BOOST_REQUIRE(boost::filesystem::exists(boost::filesystem::path(root.string()) / "build-000003.tmp"));
}

BOOST_AUTO_TEST_SUITE_END()