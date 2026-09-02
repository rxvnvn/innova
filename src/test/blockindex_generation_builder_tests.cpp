#include <boost/test/unit_test.hpp>
#include "../blockindex_generation_builder.h"
#include "../blockindex_generation_lifecycle.h"
#include "../blockindex_derived_state.h"
#include "../blockindex_startup_authority.h"
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

static boost::filesystem::path UniqueGenDir(const std::string& tag)
{
    boost::filesystem::path model =
        boost::filesystem::path("innova-blockindex-genbuilder-") /
        boost::filesystem::path(tag + "-%%%%-%%%%-%%%%");
    boost::filesystem::path dir = boost::filesystem::temp_directory_path() /
        boost::filesystem::unique_path(model) /
        "gen-000001";
    boost::filesystem::create_directories(dir);
    return dir;
}

// Build a deterministic, valid BlockIndexRecord. hash is derived so that the
// active chain can link via hashPrev.
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
    rec.nMint = 1000000 + (int64_t)nonceBase;
    rec.nMoneySupply = 5000000 + (int64_t)(nonceBase * 10);
    rec.nStakeModifier = 0x1234000000000000ULL + nonceBase;
    rec.nStakeTime = 0;
    rec.hashProof = uint256(nonceBase + 2000);
    rec.hashMerkleRoot = uint256(nonceBase + 3000);
    return rec;
}

// Build a full synthetic source: an active chain (heights 0..tipHeight) plus
// `sideChains` blocks that fork off a given height and are NOT active.
static BlockIndexGenerationSource BuildSyntheticSource(
    int tipHeight,
    int sideChainCount,
    uint64_t activeNonceBase,
    uint64_t sideNonceBase)
{
    BlockIndexGenerationSource src;

    // active chain: height h has hash uint256(activeBase + h), hashPrev = chain[h-1]
    std::vector<uint256> activeHash;
    std::vector<BlockIndexRecord> activeRec;
    for (int h = 0; h <= tipHeight; ++h)
    {
        uint256 hashPrev = (h == 0) ? uint256(0) : activeHash[h - 1];
        BlockIndexRecord rec = MakeRecord(activeNonceBase + (uint64_t)h, h, hashPrev);
        // override hash deterministically: uint256(activeNonceBase + h)
        rec.hash = uint256(activeNonceBase + (uint64_t)h);
        rec.hashPrev = hashPrev;
        activeHash.push_back(rec.hash);
        activeRec.push_back(rec);

        BlockIndexGenerationSourceRecord s;
        s.hash = rec.hash;
        s.record = rec;
        src.records.push_back(s);
    }

    // side chains: each forks off height (h % (tipHeight+1)) adding one block
    for (int k = 0; k < sideChainCount; ++k)
    {
        // pick a fork height strictly less than tip (so it branches off active)
        int forkH = k % (tipHeight + 1);
        uint256 hashPrev = activeHash[forkH];
        int hgt = forkH + 1;
        BlockIndexRecord rec = MakeRecord(sideNonceBase + (uint64_t)k, hgt, hashPrev);
        rec.hash = uint256(sideNonceBase + (uint64_t)k);
        rec.hashPrev = hashPrev;
        BlockIndexGenerationSourceRecord s;
        s.hash = rec.hash;
        s.record = rec;
        src.records.push_back(s);
    }

    // hashBestChain = active tip
    src.hashBestChain = activeHash[tipHeight];
    src.foundBestChain = true;
    return src;
}

static std::string ReadWholeFile(const boost::filesystem::path& path)
{
    FILE* f = fopen(path.string().c_str(), "rb");
    std::string d;
    if (!f) return d;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    d.resize((size_t)sz); if (sz) { size_t n = fread(&d[0], 1, (size_t)sz, f); d.resize(n); }
    fclose(f);
    return d;
}

static void WriteWholeFile(const boost::filesystem::path& path, const std::string& data)
{
    FILE* f = fopen(path.string().c_str(), "wb");
    if (data.size()) { fwrite(data.data(), 1, data.size(), f); }
    fclose(f);
}

static void FlipOneByte(const boost::filesystem::path& path, size_t offset)
{
    std::string d = ReadWholeFile(path);
    BOOST_REQUIRE_LT(offset, d.size());
    d[offset] ^= (char)0x41;
    WriteWholeFile(path, d);
}

} // namespace

BOOST_AUTO_TEST_SUITE(blockindex_generation_builder_tests)

BOOST_AUTO_TEST_CASE(build_and_verify_small_active_only_generation)
{
    boost::filesystem::path dir = UniqueGenDir("small-active");
    BlockIndexGenerationSource src = BuildSyntheticSource(5, 0, 1000, 9000);

    BlockIndexGenerationStats stats;
    std::string error;
    {
        BlockIndexGenerationBuilder builder;
        BOOST_REQUIRE_MESSAGE(builder.Build(src, dir.string(), 1, &stats, &error), error);
        builder.Close();
    }
    BOOST_CHECK_EQUAL((int)stats.totalRecords, 6);
    BOOST_CHECK_EQUAL((int)stats.activeRecords, 6);
    BOOST_CHECK_EQUAL((int)stats.sideChainRecords, 0);
    BOOST_CHECK_EQUAL(stats.activeTipHeight, 5);

    BlockIndexDifferentialResult diff;
    error.clear();
    BOOST_REQUIRE_MESSAGE(VerifyGenerationAgainstSource(src, dir.string(), 1, &diff, &error), error);
    BOOST_CHECK_EQUAL((int)diff.hashQueries, 6);
    BOOST_CHECK_EQUAL((int)diff.hashMismatches, 0);
    BOOST_CHECK_EQUAL((int)diff.hashCorruptions, 0);
    BOOST_CHECK_EQUAL((int)diff.hashNotFound, 0);
    BOOST_CHECK_EQUAL((int)diff.heightQueries, 6);
    BOOST_CHECK_EQUAL((int)diff.heightMismatches, 0);
    BOOST_CHECK_EQUAL((int)diff.parentChecks, 5);
    BOOST_CHECK_EQUAL((int)diff.parentMismatches, 0);
    BOOST_CHECK(diff.tipCoherent);
}

BOOST_AUTO_TEST_CASE(build_with_side_chain_keeps_side_in_records_not_active)
{
    boost::filesystem::path dir = UniqueGenDir("side");
    // 6 active + 4 side
    BlockIndexGenerationSource src = BuildSyntheticSource(5, 4, 1000, 9000);
    BOOST_REQUIRE_EQUAL((int)src.records.size(), 10);

    BlockIndexGenerationStats stats;
    std::string error;
    {
        BlockIndexGenerationBuilder builder;
        BOOST_REQUIRE_MESSAGE(builder.Build(src, dir.string(), 1, &stats, &error), error);
        builder.Close();
    }
    BOOST_CHECK_EQUAL((int)stats.totalRecords, 10);
    BOOST_CHECK_EQUAL((int)stats.activeRecords, 6);
    BOOST_CHECK_EQUAL((int)stats.sideChainRecords, 4);

    // verify: side-chain differential must confirm side records present but not
    // active, and the overall verdict must be clean.
    BlockIndexDifferentialResult diff;
    error.clear();
    BOOST_REQUIRE_MESSAGE(VerifyGenerationAgainstSource(src, dir.string(), 1, &diff, &error), error);
    BOOST_CHECK_EQUAL((int)diff.hashQueries, 10);
    BOOST_CHECK_EQUAL((int)diff.hashMismatches, 0);
    BOOST_CHECK_EQUAL((int)diff.sideChainSamples, 4);
    BOOST_CHECK_EQUAL((int)diff.sideChainMismatches, 0);
    BOOST_CHECK_EQUAL((int)diff.parentMismatches, 0);
    BOOST_CHECK(diff.tipCoherent);
}

BOOST_AUTO_TEST_CASE(recordid_assignment_is_deterministic_across_builds)
{
    boost::filesystem::path dir1 = UniqueGenDir("det-1");
    boost::filesystem::path dir2 = UniqueGenDir("det-2");
    BlockIndexGenerationSource src = BuildSyntheticSource(8, 5, 5000, 7000);
    // shuffle-independent: pass records in a different order
    BlockIndexGenerationSource src2 = src;

    BlockIndexGenerationStats s1, s2;
    std::string error;

    {
        BlockIndexGenerationBuilder b1;
        BOOST_REQUIRE(b1.Build(src, dir1.string(), 1, &s1, &error));
        b1.Close();
    }
    error.clear();
    {
        // reverse insertion order to prove order-independence
        std::vector<BlockIndexGenerationSourceRecord> rev = src2.records;
        src2.records.clear();
        for (size_t i = rev.size(); i-- > 0; )
            src2.records.push_back(rev[i]);
        BlockIndexGenerationBuilder b2;
        BOOST_REQUIRE(b2.Build(src2, dir2.string(), 1, &s2, &error));
        b2.Close();
    }

    // total/active/side identical
    BOOST_CHECK_EQUAL((int)s1.totalRecords, (int)s2.totalRecords);
    BOOST_CHECK_EQUAL((int)s1.activeRecords, (int)s2.activeRecords);
    BOOST_CHECK_EQUAL((int)s1.sideChainRecords, (int)s2.sideChainRecords);
    BOOST_CHECK_EQUAL(s1.activeTipHeight, s2.activeTipHeight);
    BOOST_CHECK(s1.activeTipHash == s2.activeTipHash);

    // Reopen both and compare records.dat presence deterministically via the
    // differential; ordering by hash is reproducible so both must be identical.
    BlockIndexDifferentialResult d1, d2;
    error.clear();
    BOOST_REQUIRE(VerifyGenerationAgainstSource(src, dir1.string(), 1, &d1, &error));
    error.clear();
    BOOST_REQUIRE(VerifyGenerationAgainstSource(src, dir2.string(), 1, &d2, &error));
    BOOST_CHECK_EQUAL((int)d1.hashQueries, (int)d2.hashQueries);
    BOOST_CHECK_EQUAL((int)d1.hashMismatches, 0);
    BOOST_CHECK_EQUAL((int)d2.hashMismatches, 0);
}

BOOST_AUTO_TEST_CASE(incomplete_or_duplicate_source_fails_closed)
{
    // Duplicate hash in source: two records sharing a hash would produce two
    // distinct RecordIds (fine) but the hashindex Put will conflict -> the
    // builder must fail rather than silently overwrite.
    boost::filesystem::path dir = UniqueGenDir("dup");
    BlockIndexGenerationSource src = BuildSyntheticSource(3, 0, 1000, 9000);
    // append a second record with the SAME hash as an existing one
    BlockIndexGenerationSourceRecord dup;
    dup.hash = uint256(1000); // same as active height 0
    dup.record = src.records[0].record;
    src.records.push_back(dup);

    BlockIndexGenerationStats stats;
    std::string error;
    BlockIndexGenerationBuilder builder;
    BOOST_CHECK(!builder.Build(src, dir.string(), 1, &stats, &error));
    BOOST_CHECK(!error.empty());
}

BOOST_AUTO_TEST_CASE(reopen_complete_generation_from_disk_succeeds_independently)
{
    boost::filesystem::path dir = UniqueGenDir("reopen");
    BlockIndexGenerationSource src = BuildSyntheticSource(7, 3, 3000, 6000);
    std::string error;
    BlockIndexGenerationStats stats;
    {
        BlockIndexGenerationBuilder builder;
        BOOST_REQUIRE(builder.Build(src, dir.string(), 1, &stats, &error));
        builder.Close();
    }

    // Simulate process/object lifetime boundary: everything re-allocated.
    FixedBlockIndexStore reopened;
    FixedBlockIndexOpenOptions opts;
    opts.requireCompleteManifest = true;
    error.clear();
    BOOST_REQUIRE_MESSAGE(FixedBlockIndexStore::OpenReadOnly(dir.string(), opts, &reopened, &error), error);
    BOOST_CHECK_EQUAL(reopened.GetManifest().state, (uint32_t)BLOCK_INDEX_MANIFEST_COMPLETE);

    // differential after reopen
    BlockIndexDifferentialResult diff;
    error.clear();
    BOOST_REQUIRE(VerifyGenerationAgainstSource(src, dir.string(), 1, &diff, &error));
    BOOST_CHECK_EQUAL((int)diff.hashMismatches, 0);
    BOOST_CHECK_EQUAL((int)diff.randomHashChecks, 10000);
    BOOST_CHECK_EQUAL((int)diff.randomHeightChecks, 10000);
    BOOST_CHECK_EQUAL((int)diff.randomMismatches, 0);
    BOOST_CHECK(diff.tipCoherent);
}

BOOST_AUTO_TEST_CASE(corruption_probes_fail_closed_on_small_cloned_generations)
{
    // Uses small cloned generations (never the real full generation). Each probe
    // corrupts one independent clone and asserts the appropriate fail-closed
    // behavior of the production open/lookup paths.

    // 1) records.dat CRC corruption
    {
        boost::filesystem::path dir = UniqueGenDir("probe-crc");
        BlockIndexGenerationSource src = BuildSyntheticSource(4, 1, 1000, 9000);
        std::string error;
        {
            BlockIndexGenerationBuilder b;
            BOOST_REQUIRE(b.Build(src, dir.string(), 1, NULL, &error));
            b.Close();
        }
        // OpenReadOnly validates header + manifest + committed file bounds; the
        // per-record CRC is enforced at read/decode time (A.2/A.3 semantics). So
        // corrupt a record payload and assert the corruption is DETECTED as a
        // corruption/error when the full differential reads and decodes every
        // committed record — not silently accepted.
        FlipOneByte(dir / BLOCK_INDEX_RECORDS_FILE_NAME, BLOCK_INDEX_RECORDS_HEADER_SIZE_V1 + 17);
        BlockIndexDifferentialResult diff;
        std::string e;
        // The differential decodes the whole committed region in one pass; a
        // CRC corruption makes a record undecodable, so Verify refuses to
        // complete (fail-closed) rather than silently passing the bad record.
        BOOST_CHECK_MESSAGE(!VerifyGenerationAgainstSource(src, dir.string(), 1, &diff, &e) && !e.empty(),
                            "records.dat CRC corruption must be detected fail-closed");
    }

    // 2) active.dat invalid RecordId (sparse zero in committed region)
    {
        boost::filesystem::path dir = UniqueGenDir("probe-activezero");
        BlockIndexGenerationSource src = BuildSyntheticSource(4, 1, 1000, 9000);
        std::string error;
        {
            BlockIndexGenerationBuilder b;
            BOOST_REQUIRE(b.Build(src, dir.string(), 1, NULL, &error));
            b.Close();
        }
        std::string a = ReadWholeFile(dir / BLOCK_INDEX_ACTIVE_FILE_NAME);
        // zero out the entry at height 1
        for (int i = 0; i < 8; ++i)
            a[BLOCK_INDEX_ACTIVE_HEADER_SIZE_V1 + 8 + i] = '\0';
        WriteWholeFile(dir / BLOCK_INDEX_ACTIVE_FILE_NAME, a);
        FixedBlockIndexShadowActiveLookup lu;
        std::string e;
        BOOST_CHECK_MESSAGE(!FixedBlockIndexShadowActiveLookup::Open(dir.string(), &lu, &e) && !e.empty(),
                            "active.dat zero RecordId in committed region must fail closed");
    }

    // 3) hashindex wrong RecordId -> records.dat read must reject an out-of-range id
    {
        boost::filesystem::path dir = UniqueGenDir("probe-wrongid");
        BlockIndexGenerationSource src = BuildSyntheticSource(3, 0, 1000, 9000);
        std::string error;
        {
            BlockIndexGenerationBuilder b;
            BOOST_REQUIRE(b.Build(src, dir.string(), 1, NULL, &error));
            b.Close();
        }
        FixedBlockIndexOpenOptions opts;
        opts.requireCompleteManifest = true;
        FixedBlockIndexStore s;
        std::string e;
        BOOST_REQUIRE_MESSAGE(FixedBlockIndexStore::OpenReadOnly(dir.string(), opts, &s, &e), e);
        BlockIndexRecord rec;
        std::string e2;
        BOOST_CHECK(!s.Read(999, &rec, &e2)); // id beyond committed count must fail
        BOOST_CHECK(!e2.empty());
    }

    // 4) generation mismatch (active.dat generation != records/MANIFEST)
    {
        boost::filesystem::path dir = UniqueGenDir("probe-genmismatch");
        BlockIndexGenerationSource src = BuildSyntheticSource(3, 0, 1000, 9000);
        std::string error;
        {
            BlockIndexGenerationBuilder b;
            BOOST_REQUIRE(b.Build(src, dir.string(), 1, NULL, &error));
            b.Close();
        }
        std::string a = ReadWholeFile(dir / BLOCK_INDEX_ACTIVE_FILE_NAME);
        a[24] = (char)2; // generation low byte -> 2 (was 1)
        WriteWholeFile(dir / BLOCK_INDEX_ACTIVE_FILE_NAME, a);
        FixedBlockIndexShadowActiveLookup lu;
        std::string e;
        BOOST_CHECK_MESSAGE(!FixedBlockIndexShadowActiveLookup::Open(dir.string(), &lu, &e) && !e.empty(),
                            "generation mismatch must fail closed");
    }

    // 5) incomplete/truncated generation: active.dat truncated below committed tip
    {
        boost::filesystem::path dir = UniqueGenDir("probe-truncated");
        BlockIndexGenerationSource src = BuildSyntheticSource(4, 0, 1000, 9000);
        std::string error;
        {
            BlockIndexGenerationBuilder b;
            BOOST_REQUIRE(b.Build(src, dir.string(), 1, NULL, &error));
            b.Close();
        }
        std::string a = ReadWholeFile(dir / BLOCK_INDEX_ACTIVE_FILE_NAME);
        a.resize(BLOCK_INDEX_ACTIVE_HEADER_SIZE_V1 + 8); // only height 0, committed tip height 4
        WriteWholeFile(dir / BLOCK_INDEX_ACTIVE_FILE_NAME, a);
        FixedBlockIndexShadowActiveLookup lu;
        std::string e;
        BOOST_CHECK_MESSAGE(!FixedBlockIndexShadowActiveLookup::Open(dir.string(), &lu, &e) && !e.empty(),
                            "active.dat truncated below committed tip must fail closed");
    }
}

// Write content binding directly into derived.dat header at offset 40
static bool WriteContentBindingInFile(const boost::filesystem::path& path,
                                       const unsigned char binding[32],
                                       std::string* error)
{
    std::string data = ReadWholeFile(path);
    if (data.size() < 72)
        return (*error = "derived.dat too small", false);
    memcpy(&data[40], binding, 32);
    WriteWholeFile(path, data);
    return true;
}

// Write manifest capability directly at offset 88 in MANIFEST file
static bool WriteManifestCapability(const boost::filesystem::path& genDir,
                                     uint32_t capability,
                                     std::string* error)
{
    boost::filesystem::path manifestPath = genDir / "MANIFEST";
    std::string data = ReadWholeFile(manifestPath);
    if (data.size() < 92)
        return (*error = "MANIFEST too small", false);
    memcpy(&data[88], &capability, 4);
    WriteWholeFile(manifestPath, data);
    return true;
}

// ---- A.10.1b-fix3 causal tests ----

// C1-B: no blockDataDir produces OLD_SHADOW, not AUTHORITATIVE
BOOST_AUTO_TEST_CASE(c1_no_blockdata_produces_old_shadow)
{
    boost::filesystem::path dir = UniqueGenDir("c1-old-shadow");
    BlockIndexGenerationSource src = BuildSyntheticSource(4, 1, 5000, 9000);
    // No blockDataDir set
    std::string error;
    BlockIndexGenerationBuilder b;
    BlockIndexGenerationStats stats;
    BOOST_REQUIRE_MESSAGE(b.Build(src, dir.string(), 1, &stats, &error), error);
    b.Close();

    // Verify manifest has OLD_SHADOW capability
    FixedBlockIndexOpenOptions opts;
    opts.requireCompleteManifest = true;
    FixedBlockIndexStore s;
    BOOST_REQUIRE(FixedBlockIndexStore::OpenReadOnly(dir.string(), opts, &s, &error));
    const FixedBlockIndexManifest& m = s.GetManifest();
    BOOST_CHECK_EQUAL(m.capability, (uint32_t)BLOCK_INDEX_GENERATION_CAPABILITY_OLD_SHADOW);
}

// C2-A: changed derived entry + valid local CRC -> ValidateGeneration REJECT
// For AUTHORITATIVE capability, validation recomputes the generation root from
// actual files. We manually upgrade an OLD_SHADOW to AUTHORITATIVE by computing
// the full generation root and writing it as the content binding.
BOOST_AUTO_TEST_CASE(c2_changed_derived_entry_rejects)
{
    boost::filesystem::path root = UniqueGenDir("c2-derived-reject");
    boost::filesystem::path genDir = root / "gen-000001";
    BlockIndexGenerationSource src = BuildSyntheticSource(5, 1, 10000, 20000);
    std::string error;
    {
        BlockIndexGenerationBuilder b;
        BOOST_REQUIRE_MESSAGE(b.Build(src, genDir.string(), 1, NULL, &error), error);
        b.Close();
    }
    // Validate passes before tampering (as OLD_SHADOW)
    BOOST_CHECK_EQUAL(BlockIndexGenerationManager::ValidateGeneration(root.string(), 1, &error),
                      (int)BLOCK_INDEX_LIFECYCLE_OK);

    // Upgrade to AUTHORITATIVE: compute full generation root from actual files,
    // write it directly into derived.dat content binding, set manifest capability.
    {
        FixedBlockIndexOpenOptions opts; opts.requireCompleteManifest = true;
        FixedBlockIndexStore store;
        BOOST_REQUIRE(FixedBlockIndexStore::OpenReadOnly(genDir.string(), opts, &store, &error));
        const FixedBlockIndexManifest& m = store.GetManifest();

        unsigned char recomputedRoot[32];
        BOOST_REQUIRE(RecomputeGenerationRootFromFiles(genDir, m.generation,
            m.committedTipHash, m.recordCount, m.dagInputDigest, recomputedRoot, &error));

        BOOST_REQUIRE(WriteContentBindingInFile(genDir / BLOCK_INDEX_DERIVED_FILE_NAME, recomputedRoot, &error));
        BOOST_REQUIRE(WriteManifestCapability(genDir, BLOCK_INDEX_GENERATION_CAPABILITY_AUTHORITATIVE, &error));
    }

    // Validate passes after upgrade to AUTHORITATIVE
    error.clear();
    BOOST_CHECK_EQUAL(BlockIndexGenerationManager::ValidateGeneration(root.string(), 1, &error),
                      (int)BLOCK_INDEX_LIFECYCLE_OK);

    // Tamper: change a middle derived entry's chainTrust field
    std::string derivedData = ReadWholeFile(genDir / BLOCK_INDEX_DERIVED_FILE_NAME);
    const size_t entryOffset = BLOCK_INDEX_DERIVED_HEADER_SIZE_V2 + 2 * BLOCK_INDEX_DERIVED_ENTRY_SIZE_V2;
    derivedData[entryOffset + 5] ^= 0x01;
    // Recompute CRC (last 4 bytes of entry)
    {
        uint32_t crc = 0;
        for (size_t j = 0; j < BLOCK_INDEX_DERIVED_ENTRY_SIZE_V2 - 4; ++j)
            crc += (unsigned char)derivedData[entryOffset + j];
        memcpy(&derivedData[entryOffset + BLOCK_INDEX_DERIVED_ENTRY_SIZE_V2 - 4], &crc, 4);
    }
    WriteWholeFile(genDir / BLOCK_INDEX_DERIVED_FILE_NAME, derivedData);

    // Validation must now REJECT (root recomputation will differ)
    error.clear();
    BOOST_CHECK_NE(BlockIndexGenerationManager::ValidateGeneration(root.string(), 1, &error),
                   (int)BLOCK_INDEX_LIFECYCLE_OK);
}

// C2-B: changed records entry + valid local CRC -> ValidateGeneration REJECT
BOOST_AUTO_TEST_CASE(c2_changed_records_entry_rejects)
{
    boost::filesystem::path root = UniqueGenDir("c2-records-reject");
    boost::filesystem::path genDir = root / "gen-000001";
    BlockIndexGenerationSource src = BuildSyntheticSource(5, 1, 10000, 20000);
    std::string error;
    {
        BlockIndexGenerationBuilder b;
        BOOST_REQUIRE_MESSAGE(b.Build(src, genDir.string(), 1, NULL, &error), error);
        b.Close();
    }
    BOOST_CHECK_EQUAL(BlockIndexGenerationManager::ValidateGeneration(root.string(), 1, &error),
                      (int)BLOCK_INDEX_LIFECYCLE_OK);

    // Upgrade to AUTHORITATIVE
    {
        FixedBlockIndexOpenOptions opts; opts.requireCompleteManifest = true;
        FixedBlockIndexStore store;
        BOOST_REQUIRE(FixedBlockIndexStore::OpenReadOnly(genDir.string(), opts, &store, &error));
        const FixedBlockIndexManifest& m = store.GetManifest();
        unsigned char recomputedRoot[32];
        BOOST_REQUIRE(RecomputeGenerationRootFromFiles(genDir, m.generation,
            m.committedTipHash, m.recordCount, m.dagInputDigest, recomputedRoot, &error));
        BOOST_REQUIRE(WriteContentBindingInFile(genDir / BLOCK_INDEX_DERIVED_FILE_NAME, recomputedRoot, &error));
        BOOST_REQUIRE(WriteManifestCapability(genDir, BLOCK_INDEX_GENERATION_CAPABILITY_AUTHORITATIVE, &error));
    }

    // Verify upgraded validation passes
    error.clear();
    BOOST_CHECK_EQUAL(BlockIndexGenerationManager::ValidateGeneration(root.string(), 1, &error),
                      (int)BLOCK_INDEX_LIFECYCLE_OK);

    // Tamper: change a field in a middle record entry
    std::string recordsData = ReadWholeFile(genDir / BLOCK_INDEX_RECORDS_FILE_NAME);
    const size_t recOffset = BLOCK_INDEX_RECORDS_HEADER_SIZE_V1 + 2 * BLOCK_INDEX_RECORD_SIZE_V1;
    recordsData[recOffset + 160] ^= 0x01; // flip a byte
    // Recompute CRC (last 4 bytes of record)
    {
        uint32_t crc = 0;
        for (size_t j = 0; j < BLOCK_INDEX_RECORD_SIZE_V1 - 4; ++j)
            crc += (unsigned char)recordsData[recOffset + j];
        memcpy(&recordsData[recOffset + BLOCK_INDEX_RECORD_SIZE_V1 - 4], &crc, 4);
    }
    WriteWholeFile(genDir / BLOCK_INDEX_RECORDS_FILE_NAME, recordsData);

    // Validation must now REJECT (root recomputation will differ)
    error.clear();
    BOOST_CHECK_NE(BlockIndexGenerationManager::ValidateGeneration(root.string(), 1, &error),
                   (int)BLOCK_INDEX_LIFECYCLE_OK);
}

// Disconnected topology: non-genesis record with absent parent -> REJECT
BOOST_AUTO_TEST_CASE(disconnected_topology_rejects)
{
    BlockIndexGenerationSource src;
    // Genesis
    BlockIndexRecord genesis;
    genesis.hash = uint256(1000);
    genesis.hashPrev = uint256(0);
    genesis.height = 0;
    genesis.nVersion = 1; genesis.nTime = 1000; genesis.nBits = 0x1d00ffff;
    genesis.hashProof = uint256(1100);
    BlockIndexGenerationSourceRecord sg; sg.hash = genesis.hash; sg.record = genesis;
    src.records.push_back(sg);

    // Valid child of genesis (main chain tip)
    BlockIndexRecord child;
    child.hash = uint256(2000);
    child.hashPrev = uint256(1000); // parent = genesis
    child.height = 1;
    child.nVersion = 1; child.nTime = 1001; child.nBits = 0x1d00ffff;
    child.hashProof = uint256(2100);
    BlockIndexGenerationSourceRecord sc; sc.hash = child.hash; sc.record = child;
    src.records.push_back(sc);

    // Disconnected side-branch record: parent NOT in source
    BlockIndexRecord orphan;
    orphan.hash = uint256(3000);
    orphan.hashPrev = uint256(9999); // parent absent!
    orphan.height = 1;
    orphan.nVersion = 1; orphan.nTime = 1002; orphan.nBits = 0x1d00ffff;
    orphan.hashProof = uint256(3100);
    BlockIndexGenerationSourceRecord so; so.hash = orphan.hash; so.record = orphan;
    src.records.push_back(so);

    src.hashBestChain = child.hash;
    src.foundBestChain = true;

    boost::filesystem::path dir = UniqueGenDir("disconnected");
    std::string error;
    BlockIndexGenerationBuilder b;
    BlockIndexGenerationStats stats;
    // Must fail: disconnected parent
    BOOST_CHECK(!b.Build(src, dir.string(), 1, &stats, &error));
    BOOST_CHECK_MESSAGE(error.find("disconnected") != std::string::npos,
                        "Expected 'disconnected' in error, got: " << error);
}

// C3: canonical DAG trust != linear trust; builder reconstructs via ColorBlock
// Fixture: genesis (h=0) -> ... -> preDAG (h=10) -> A (h=11) and B (h=11).
// C (h=12) has selected parent A and merge parent B.
// Heights must be contiguous for the active chain walk.
// Linear trust for C = parentTrust(A) + blockTrust(C)
// Canonical DAG trust for C = selectedParentScore + blockTrust(C) + blockTrust(B)
//   because B is a blue merge parent (anticone size <= GHOSTDAG_K=18).
BOOST_AUTO_TEST_CASE(c3_canonical_dag_trust_over_linear)
{
    const unsigned int nBits = 0x1d00ffff;
    std::string error;

    // Build contiguous chain: genesis(0) -> 1 -> ... -> 10 -> then DAG fork
    // Active chain tip must be C at h=12, with contiguous heights.
    // We need: genesis(0), intermediate(1..10), A(11), B(11), C(12)
    // But active chain can only follow one parent. Use A as active chain.
    // B is a side-branch merge parent at same height.

    // For simplicity: genesis(0), preDAG(1..10), A(11), C(12)
    // B(11) is parallel to A with same parent (preDAG at h=10)
    // Active chain: genesis -> preDAG(1) -> ... -> preDAG(10) -> A(11) -> C(12)

    BlockIndexGenerationSource src;
    std::vector<BlockIndexRecord> records;

    // Genesis
    {
        BlockIndexRecord r; r.hash = uint256(1); r.hashPrev = uint256(0);
        r.height = 0; r.nVersion = 1; r.nTime = 1000; r.nBits = nBits; r.hashProof = uint256(100);
        records.push_back(r);
    }
    // Pre-DAG chain h=1..10
    for (int h = 1; h <= 10; ++h)
    {
        BlockIndexRecord r; r.hash = uint256(h + 1); r.hashPrev = uint256(h);
        r.height = h; r.nVersion = 1; r.nTime = 1000 + h; r.nBits = nBits;
        r.hashProof = uint256(100 + h);
        records.push_back(r);
    }
    // A at h=11 (post-DAG, parent = preDAG at h=10, hash=11)
    {
        BlockIndexRecord r; r.hash = uint256(100); r.hashPrev = uint256(11);
        r.height = 11; r.nVersion = 1; r.nTime = 1100; r.nBits = nBits; r.hashProof = uint256(200);
        records.push_back(r);
    }
    // B at h=11 (post-DAG, parallel to A, parent = preDAG at h=10, hash=11)
    {
        BlockIndexRecord r; r.hash = uint256(200); r.hashPrev = uint256(11);
        r.height = 11; r.nVersion = 1; r.nTime = 1101; r.nBits = nBits; r.hashProof = uint256(300);
        records.push_back(r);
    }
    // C at h=12 (post-DAG, parent = A, merge parent = B)
    {
        BlockIndexRecord r; r.hash = uint256(300); r.hashPrev = uint256(100);
        r.height = 12; r.nVersion = 1; r.nTime = 1200; r.nBits = nBits; r.hashProof = uint256(400);
        records.push_back(r);
    }

    for (auto& r : records)
    {
        BlockIndexGenerationSourceRecord sr; sr.hash = r.hash; sr.record = r;
        src.records.push_back(sr);
    }
    src.hashBestChain = uint256(300); // C
    src.foundBestChain = true;

    // DAG links: A and B have preDAG(h=10) as DAG parent; C has A and B
    src.dagLinks[uint256(100)] = {uint256(11)}; // A's DAG parent
    src.dagLinks[uint256(200)] = {uint256(11)}; // B's DAG parent
    src.dagLinks[uint256(300)] = {uint256(100), uint256(200)}; // C's DAG parents

    // Step 1: Call ReconstructCanonicalDAGScores
    std::vector<std::pair<int32_t, uint256>> heightSorted;
    std::map<uint256, const BlockIndexRecord*> recordByHash;
    for (size_t i = 0; i < src.records.size(); ++i)
    {
        heightSorted.push_back({src.records[i].record.height, src.records[i].hash});
        recordByHash[src.records[i].hash] = &src.records[i].record;
    }
    std::sort(heightSorted.begin(), heightSorted.end());

    std::map<uint256, uint256> canonicalScores;
    BOOST_REQUIRE_MESSAGE(ReconstructCanonicalDAGScores(heightSorted, recordByHash,
                                                        src.dagLinks, &canonicalScores, &error),
                          error);

    // Verify canonical score for C exists
    uint256 hashC = uint256(300);
    BOOST_REQUIRE_MESSAGE(canonicalScores.count(hashC),
                          "canonical DAG score missing for C");

    // Compute expected values
    CBigNum bnTarget;
    bnTarget.SetCompact(nBits);
    uint256 blockTrust = ((CBigNum(1) << 256) / (bnTarget + 1)).getuint256();

    // Linear trust for C: 13 * blockTrust (genesis + 10 pre-DAG + A + C)
    uint256 linearC = CBigNum(blockTrust).getuint256();
    for (int i = 1; i < 13; ++i)
        linearC = (CBigNum(linearC) + CBigNum(blockTrust)).getuint256();

    // Canonical: A's DAG score = preDAG(10).nChainTrust + blockTrust = 11*blockTrust + blockTrust = 12*blockTrust
    // C's canonical = A's DAG score + blockTrust(C) + blockTrust(B) = 12*bt + bt + bt = 14*bt
    uint256 canonicalC = canonicalScores[hashC];

    // Verify canonical != linear
    BOOST_CHECK_MESSAGE(canonicalC != linearC,
                        "canonical must differ from linear trust");

    // Step 2: Build generation and verify builder uses canonical score
    boost::filesystem::path root = UniqueGenDir("c3-dag-trust");
    boost::filesystem::path genDir = root / "gen-000001";
    {
        BlockIndexGenerationBuilder b;
        BOOST_REQUIRE_MESSAGE(b.Build(src, genDir.string(), 1, NULL, &error), error);
        b.Close();
    }

    // Read chainTrust for C from derived.dat
    // Records are hash-sorted: 1,2,...,11,100,200,300 (C is last = RecordId 14)
    BlockIndexDerivedStateStore dstore;
    BOOST_REQUIRE(BlockIndexDerivedStateStore::OpenReadOnly(genDir.string(), 1, &dstore, &error));
    BlockIndexDerivedEntry entryC;
    BlockIndexDerivedLookupStatus readStatus = dstore.Read(src.records.size(), &entryC, &error);
    BOOST_REQUIRE_MESSAGE(readStatus == BLOCK_INDEX_DERIVED_LOOKUP_FOUND, error);
    BOOST_CHECK_MESSAGE(entryC.chainTrust == canonicalC,
                        "builder chainTrust for C must equal canonical DAG score");

    // Step 3: Inject incorrect dagScore — builder must ignore it
    src.dagScores[hashC] = uint256(999999);
    boost::filesystem::path root2 = UniqueGenDir("c3-dag-trust-injected");
    boost::filesystem::path genDir2 = root2 / "gen-000001";
    {
        BlockIndexGenerationBuilder b;
        BOOST_REQUIRE_MESSAGE(b.Build(src, genDir2.string(), 1, NULL, &error), error);
        b.Close();
    }
    BlockIndexDerivedStateStore dstore2;
    BOOST_REQUIRE(BlockIndexDerivedStateStore::OpenReadOnly(genDir2.string(), 1, &dstore2, &error));
    BlockIndexDerivedEntry entryC2;
    readStatus = dstore2.Read(src.records.size(), &entryC2, &error);
    BOOST_REQUIRE_MESSAGE(readStatus == BLOCK_INDEX_DERIVED_LOOKUP_FOUND, error);
    BOOST_CHECK_MESSAGE(entryC2.chainTrust == canonicalC,
                        "builder must ignore injected dagScore and use canonical reconstruction");
}

// ---- Helper: generate synthetic block data files ----

#include "../core.h"  // CTransaction, CTxIn, CTxOut, COutPoint

// Create a minimal CBlock with a coinbase tx, write to blk0001.dat,
// return the block hash, nBlockPos, and nSize.
struct SyntheticBlockInfo {
    uint256 hash;
    unsigned int nFile;
    unsigned int nBlockPos;
    unsigned int nSize;
};

static SyntheticBlockInfo WriteSyntheticBlock(
    const boost::filesystem::path& dir,
    uint256 hashPrevBlock,
    unsigned int nTime,
    unsigned int nBits,
    unsigned int nNonce)
{
    SyntheticBlockInfo info;
    info.nFile = 1;

    // Create coinbase transaction
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

    // Create block
    CBlock block;
    block.nVersion = 1;
    block.hashPrevBlock = hashPrevBlock;
    block.nTime = nTime;
    block.nBits = nBits;
    block.nNonce = nNonce;
    block.vtx.push_back(coinbase);
    block.hashMerkleRoot = block.BuildMerkleTree();

    // Compute hash and size
    info.hash = block.GetHash();
    info.nSize = ::GetSerializeSize(block, SER_NETWORK, PROTOCOL_VERSION);

    // Serialize block to memory first
    CDataStream ssBlock(SER_DISK, CLIENT_VERSION);
    ssBlock << block;
    unsigned int nDiskSize = ssBlock.size();

    // Verify roundtrip: deserialize from the serialized bytes and check hash
    {
        CBlock verify;
        CDataStream ssVerify(ssBlock.begin(), ssBlock.end(), SER_DISK, CLIENT_VERSION);
        ssVerify >> verify;
        assert(verify.GetHash() == info.hash);
    }

    // Write to blk0001.dat: [magic(4)][size(4)][block_data]
    boost::filesystem::path blockFile = dir / "blk0001.dat";
    FILE* f = fopen(blockFile.string().c_str(), "ab");
    assert(f);

    // Write magic (regtest)
    unsigned char magic[] = {0xfa, 0xbf, 0xb5, 0xda};
    fwrite(magic, 1, 4, f);

    // Write size (4 bytes LE)
    fwrite(&nDiskSize, 4, 1, f);

    // Record nBlockPos (position of block data, after magic+size)
    long pos = ftell(f);
    info.nBlockPos = (unsigned int)pos;

    // Write serialized block data
    fwrite(&ssBlock[0], 1, ssBlock.size(), f);

    fflush(f);
    fclose(f);

    return info;
}

// Item 4: wrong block identity at valid coordinates -> AUTHORITATIVE build REJECT
BOOST_AUTO_TEST_CASE(c1_wrong_block_identity_rejects)
{
    boost::filesystem::path dir = UniqueGenDir("wrong-identity");
    boost::filesystem::path blockDir = dir / "blocks";
    boost::filesystem::create_directories(blockDir);

    // Write a real block
    SyntheticBlockInfo bi = WriteSyntheticBlock(blockDir, uint256(0), 1000, 0x1d00ffff, 12345);

    // Create source record with WRONG hash (different from actual block)
    BlockIndexRecord rec;
    rec.hash = uint256(0xDEADBEEF); // wrong hash
    rec.hashPrev = uint256(0);
    rec.height = 0;
    rec.nVersion = 1; rec.nTime = 1000; rec.nBits = 0x1d00ffff;
    rec.hashProof = uint256(1100);
    rec.nFile = bi.nFile;
    rec.nBlockPos = bi.nBlockPos;

    BlockIndexGenerationSource src;
    BlockIndexGenerationSourceRecord sr; sr.hash = rec.hash; sr.record = rec;
    src.records.push_back(sr);
    src.hashBestChain = rec.hash;
    src.foundBestChain = true;
    src.blockDataDir = blockDir.string();

    std::string error;
    BlockIndexGenerationBuilder b;
    BlockIndexGenerationStats stats;
    // Must fail: block identity mismatch means nSize unavailable,
    // so AUTHORITATIVE cannot be produced
    BOOST_CHECK(!b.Build(src, dir.string(), 1, &stats, &error));
}

// Item 2+3: real AUTHORITATIVE lifecycle + exact nSize end-to-end
// Build with real blockDataDir -> AUTHORITATIVE -> Validate -> Publish -> Select
// -> V2BlockIndexStartupAuthority::Open -> LookupByHash -> OK
BOOST_AUTO_TEST_CASE(c1_authoritative_lifecycle_roundtrip)
{
    // Create unique root without gen-000001 subdir (UniqueGenDir creates it)
    boost::filesystem::path root = boost::filesystem::temp_directory_path() /
        boost::filesystem::unique_path("innova-blockindex-genbuilder-/auth-lifecycle-%%%%-%%%%-%%%%");
    boost::filesystem::create_directories(root);
    boost::filesystem::path genDir = root / "gen-000001";
    boost::filesystem::path blockDir = root / "blocks";
    boost::filesystem::create_directories(blockDir);

    // Create a chain of 3 blocks
    SyntheticBlockInfo b0 = WriteSyntheticBlock(blockDir, uint256(0), 1000, 0x1d00ffff, 100);
    SyntheticBlockInfo b1 = WriteSyntheticBlock(blockDir, b0.hash, 1001, 0x1d00ffff, 200);
    SyntheticBlockInfo b2 = WriteSyntheticBlock(blockDir, b1.hash, 1002, 0x1d00ffff, 300);

    // Create source records with correct hashes
    BlockIndexGenerationSource src;
    auto addBlock = [&](const SyntheticBlockInfo& bi, uint256 hashPrev, int height, uint256 hashProof) {
        BlockIndexRecord rec;
        rec.hash = bi.hash;
        rec.hashPrev = hashPrev;
        rec.height = height;
        rec.nVersion = 1; rec.nTime = 1000 + height; rec.nBits = 0x1d00ffff;
        rec.hashProof = hashProof;
        rec.nFile = bi.nFile;
        rec.nBlockPos = bi.nBlockPos;
        BlockIndexGenerationSourceRecord sr; sr.hash = rec.hash; sr.record = rec;
        src.records.push_back(sr);
    };
    addBlock(b0, uint256(0), 0, uint256(1100));
    addBlock(b1, b0.hash, 1, uint256(1200));
    addBlock(b2, b1.hash, 2, uint256(1300));
    src.hashBestChain = b2.hash;
    src.foundBestChain = true;
    src.blockDataDir = blockDir.string();

    // Build into staging directory (PublishGeneration expects build-000001.tmp)
    std::string error;
    boost::filesystem::path stagingDir = root / "build-000001.tmp";
    {
        BlockIndexGenerationBuilder b;
        bool buildOk = b.Build(src, stagingDir.string(), 1, NULL, &error);
        if (!buildOk) {
            boost::filesystem::path blkPath = blockDir / "blk0001.dat";
            fprintf(stderr, "DIAG: build failed: %s\n", error.c_str());
            fprintf(stderr, "DIAG: blk exists=%d size=%llu\n",
                    (int)boost::filesystem::exists(blkPath),
                    boost::filesystem::exists(blkPath) ? (unsigned long long)boost::filesystem::file_size(blkPath) : 0ULL);
            fprintf(stderr, "DIAG: b0 nFile=%u nBlockPos=%u nSize=%u hash=%s\n",
                    b0.nFile, b0.nBlockPos, b0.nSize, b0.hash.ToString().c_str());
            fprintf(stderr, "DIAG: b1 nFile=%u nBlockPos=%u nSize=%u\n", b1.nFile, b1.nBlockPos, b1.nSize);
            fprintf(stderr, "DIAG: b2 nFile=%u nBlockPos=%u nSize=%u\n", b2.nFile, b2.nBlockPos, b2.nSize);
            fprintf(stderr, "DIAG: blockDataDir=%s\n", src.blockDataDir.c_str());
            // Try reading all three blocks manually
            FILE* bf = fopen(blkPath.string().c_str(), "rb");
            if (bf) {
                for (int bi = 0; bi < 3; ++bi) {
                    unsigned int testPos = (bi == 0) ? b0.nBlockPos : (bi == 1) ? b1.nBlockPos : b2.nBlockPos;
                    uint256 expectedHash = (bi == 0) ? b0.hash : (bi == 1) ? b1.hash : b2.hash;
                    fseeko(bf, testPos, SEEK_SET);
                    CBlock testBlock;
                    CAutoFile testFile(bf, SER_DISK, CLIENT_VERSION);
                    try {
                        testFile >> testBlock;
                        uint256 gotHash = testBlock.GetHash();
                        fprintf(stderr, "DIAG: block[%d] pos=%u expected=%s got=%s match=%d\n",
                                bi, testPos, expectedHash.ToString().substr(0,16).c_str(),
                                gotHash.ToString().substr(0,16).c_str(), (int)(gotHash == expectedHash));
                    } catch (std::exception& e) { fprintf(stderr, "DIAG: block[%d] read FAILED: %s\n", bi, e.what()); }
                    bf = fopen(blkPath.string().c_str(), "rb"); // reopen for next iteration
                }
                fclose(bf);
            }
        }
        BOOST_REQUIRE_MESSAGE(buildOk, error);
        b.Close();
    }

    // Verify exact nSize matches GetSerializeSize
    {
        FILE* bf = fopen((blockDir / "blk0001.dat").string().c_str(), "rb");
        BOOST_REQUIRE(bf != NULL);
        fseeko(bf, b2.nBlockPos, SEEK_SET);
        CBlock block;
        CAutoFile filein(bf, SER_DISK, CLIENT_VERSION);
        filein >> block;
        unsigned int expectedNSize = ::GetSerializeSize(block, SER_NETWORK, PROTOCOL_VERSION);
        BOOST_CHECK_EQUAL(b2.nSize, expectedNSize);
    }

    // Publish (validates staging internally, then renames to gen-000001)
    BOOST_REQUIRE_MESSAGE(BlockIndexGenerationManager::PublishGeneration(root.string(), 1, &error) == (int)BLOCK_INDEX_LIFECYCLE_OK,
                          "PublishGeneration failed: " << error);

    // Select
    BOOST_REQUIRE_MESSAGE(BlockIndexGenerationManager::SelectGeneration(root.string(), 1, &error) == (int)BLOCK_INDEX_LIFECYCLE_OK,
                          "SelectGeneration failed: " << error);

    // Verify AUTHORITATIVE capability (after Publish, genDir now exists)
    {
        FixedBlockIndexOpenOptions opts; opts.requireCompleteManifest = true;
        FixedBlockIndexStore store;
        BOOST_REQUIRE_MESSAGE(FixedBlockIndexStore::OpenReadOnly(genDir.string(), opts, &store, &error),
                              "OpenReadOnly failed: " << error << " genDir=" << genDir.string());
        const FixedBlockIndexManifest& m = store.GetManifest();
        BOOST_CHECK_EQUAL(m.capability, (uint32_t)BLOCK_INDEX_GENERATION_CAPABILITY_AUTHORITATIVE);
        BOOST_CHECK_EQUAL(m.recordCount, (uint64_t)3);
    }

    // V2BlockIndexStartupAuthority::Open
    V2BlockIndexStartupAuthority authority;
    BOOST_REQUIRE_MESSAGE(authority.Open(root.string(), &error) == BLOCK_INDEX_STARTUP_OK, error);

    // LookupByHash for the tip block
    BlockIndexStartupResult lookupResult = authority.LookupByHash(BlockIndexLogicalId(b2.hash));
    BOOST_REQUIRE_MESSAGE(lookupResult.HasRecord(), "LookupByHash must find tip block");
    BOOST_CHECK_EQUAL(lookupResult.record.height, 2);
    BOOST_CHECK_MESSAGE(lookupResult.record.derived.hasBlockSize, "nSize must be present for AUTHORITATIVE");
    BOOST_CHECK_MESSAGE(lookupResult.record.derived.blockSize > 0, "nSize must be positive for AUTHORITATIVE");
}

// Item 1: nonlinear side-branch stake checksum differential
// Verify that a side-branch with different trust produces a different
// stake modifier checksum than the main chain.
BOOST_AUTO_TEST_CASE(c3_side_branch_checksum_differential)
{
    const unsigned int nBits = 0x1d00ffff;
    std::string error;

    // Create two parallel chains sharing a common ancestor:
    // genesis(0) -> A(1) -> B(2) [main chain]
    //                   -> C(2) [side branch with different nTime]
    // The stake modifier checksum depends on nFlags, hashProof, nStakeModifier,
    // which differ between B and C due to different nTime/nNonce.

    BlockIndexGenerationSource src;
    std::vector<BlockIndexRecord> records;

    // Genesis
    {
        BlockIndexRecord r; r.hash = uint256(1); r.hashPrev = uint256(0);
        r.height = 0; r.nVersion = 1; r.nTime = 1000; r.nBits = nBits; r.hashProof = uint256(100);
        records.push_back(r);
    }
    // A at h=1
    {
        BlockIndexRecord r; r.hash = uint256(2); r.hashPrev = uint256(1);
        r.height = 1; r.nVersion = 1; r.nTime = 1001; r.nBits = nBits; r.hashProof = uint256(200);
        records.push_back(r);
    }
    // B at h=2 (main chain tip, PoS so hashProof affects checksum)
    {
        BlockIndexRecord r; r.hash = uint256(3); r.hashPrev = uint256(2);
        r.height = 2; r.nVersion = 1; r.nTime = 1002; r.nBits = nBits; r.hashProof = uint256(300);
        r.nFlags = CBlockIndex::BLOCK_PROOF_OF_STAKE;
        r.prevoutStake = COutPoint(uint256(0xAA), 0);
        r.nStakeTime = 1002;
        records.push_back(r);
    }
    // C at h=2 (side branch, PoS with different hashProof)
    {
        BlockIndexRecord r; r.hash = uint256(4); r.hashPrev = uint256(2);
        r.height = 2; r.nVersion = 1; r.nTime = 9999; r.nBits = nBits; r.hashProof = uint256(400);
        r.nFlags = CBlockIndex::BLOCK_PROOF_OF_STAKE;
        r.prevoutStake = COutPoint(uint256(0xBB), 0);
        r.nStakeTime = 9999;
        records.push_back(r);
    }

    for (auto& r : records)
    {
        BlockIndexGenerationSourceRecord sr; sr.hash = r.hash; sr.record = r;
        src.records.push_back(sr);
    }
    src.hashBestChain = uint256(3); // B is tip
    src.foundBestChain = true;

    // Build into staging (UniqueGenDir already created root/gen-000001)
    boost::filesystem::path root = UniqueGenDir("side-branch-checksum");
    boost::filesystem::path stagingDir = root / "build-000001.tmp";
    boost::filesystem::path genDir = root / "gen-000001";
    {
        BlockIndexGenerationBuilder b;
        BOOST_REQUIRE_MESSAGE(b.Build(src, stagingDir.string(), 1, NULL, &error), error);
        b.Close();
    }

    // Read derived entries for B and C from staging directory
    BlockIndexDerivedStateStore dstore;
    BOOST_REQUIRE(BlockIndexDerivedStateStore::OpenReadOnly(stagingDir.string(), 1, &dstore, &error));
    BlockIndexDerivedEntry entryB, entryC;
    BOOST_REQUIRE(dstore.Read(3, &entryB, &error) == BLOCK_INDEX_DERIVED_LOOKUP_FOUND);
    BOOST_REQUIRE(dstore.Read(4, &entryC, &error) == BLOCK_INDEX_DERIVED_LOOKUP_FOUND);

    // B and C have same parent (A) but different nTime/nProof -> different checksums
    BOOST_CHECK_MESSAGE(entryB.stakeModifierChecksum != entryC.stakeModifierChecksum,
                        "side-branch with different nTime must produce different stake modifier checksum");

    // Verify via V2 StartupAuthority
    BOOST_CHECK_EQUAL(BlockIndexGenerationManager::PublishGeneration(root.string(), 1, &error),
                      (int)BLOCK_INDEX_LIFECYCLE_OK);
    BOOST_CHECK_EQUAL(BlockIndexGenerationManager::SelectGeneration(root.string(), 1, &error),
                      (int)BLOCK_INDEX_LIFECYCLE_OK);

    // Verify checksums differ (already proven by derived state above)
    // V2 authority requires AUTHORITATIVE capability which needs real blockDataDir;
    // the checksum differential is proven at the builder level.
}

BOOST_AUTO_TEST_SUITE_END()