#include <boost/test/unit_test.hpp>
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

BOOST_AUTO_TEST_SUITE_END()