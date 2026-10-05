#include <boost/test/unit_test.hpp>
#include <boost/filesystem.hpp>

#include <fstream>
#include <string>

#include "blockindex_startup_ownership.h"

namespace fs = boost::filesystem;

namespace {

struct P07TempRoot
{
    fs::path path;
    P07TempRoot()
    {
        path = fs::temp_directory_path() / fs::unique_path("innova-p07own-%%%%-%%%%");
        fs::create_directories(path);
    }
    ~P07TempRoot()
    {
        boost::system::error_code ec;
        fs::remove_all(path, ec);
    }
    std::string str() const { return path.string(); }
};

void WriteBytes(const std::string& file, const std::string& bytes)
{
    fs::create_directories(fs::path(file).parent_path());
    std::ofstream o(file.c_str(), std::ios::binary);
    o.write(bytes.data(), (std::streamsize)bytes.size());
}

void WriteValidCurrent(const std::string& v2Root, uint64_t generation)
{
    BlockIndexCurrentRecord rec;
    rec.generation = generation;
    std::string enc, err;
    BOOST_REQUIRE(EncodeBlockIndexCurrentRecord(rec, &enc, &err));
    WriteBytes((fs::path(v2Root) / BLOCK_INDEX_CURRENT_FILE_NAME).string(), enc);
}

} // namespace

BOOST_AUTO_TEST_SUITE(blockindex_p07_startup_ownership_tests)

// P07-01: neither legacy nor V2 -> EMPTY_NEW.
BOOST_AUTO_TEST_CASE(p07_01_neither_present_is_empty_new)
{
    P07TempRoot t;
    fs::path v2Root = t.path / "bzindexv2";
    fs::path dataDir = t.path / "data"; // no txleveldb
    fs::create_directories(dataDir);

    BlockIndexStartupOwnershipDecision d;
    std::string err;
    BOOST_REQUIRE(ClassifyBlockIndexStartupOwnership(v2Root.string(), dataDir.string(), &d, &err));
    BOOST_CHECK_EQUAL(d.state, BLOCK_INDEX_STARTUP_OWNERSHIP_EMPTY_NEW);
    BOOST_CHECK(!d.legacyPresent);
    BOOST_CHECK(!d.v2Valid);
    BOOST_CHECK_EQUAL(d.v2CurrentStatus, BLOCK_INDEX_LIFECYCLE_NOT_PUBLISHED);
}

// P07-02: valid legacy only -> LEGACY_MIGRATION_REQUIRED.
BOOST_AUTO_TEST_CASE(p07_02_legacy_only_requires_migration)
{
    P07TempRoot t;
    fs::path v2Root = t.path / "bzindexv2";
    fs::path dataDir = t.path / "data";
    fs::create_directories(dataDir / "txleveldb");

    BlockIndexStartupOwnershipDecision d;
    std::string err;
    BOOST_REQUIRE(ClassifyBlockIndexStartupOwnership(v2Root.string(), dataDir.string(), &d, &err));
    BOOST_CHECK_EQUAL(d.state, BLOCK_INDEX_STARTUP_OWNERSHIP_LEGACY_MIGRATION_REQUIRED);
    BOOST_CHECK(d.legacyPresent);
    BOOST_CHECK(!d.v2Valid);
}

// P07-10a: CURRENT corrupt -> FATAL_CORRUPTION (never treated as V2 absence).
BOOST_AUTO_TEST_CASE(p07_10a_corrupt_current_is_fatal)
{
    P07TempRoot t;
    fs::path v2Root = t.path / "bzindexv2";
    fs::path dataDir = t.path / "data";
    fs::create_directories(dataDir / "txleveldb"); // valid legacy present too
    WriteBytes((v2Root / BLOCK_INDEX_CURRENT_FILE_NAME).string(), std::string("NOT-A-VALID-CURRENT-RECORD"));

    BlockIndexStartupOwnershipDecision d;
    std::string err;
    BOOST_REQUIRE(ClassifyBlockIndexStartupOwnership(v2Root.string(), dataDir.string(), &d, &err));
    BOOST_CHECK_EQUAL(d.state, BLOCK_INDEX_STARTUP_OWNERSHIP_FATAL_CORRUPTION);
    BOOST_CHECK_EQUAL(d.v2CurrentStatus, BLOCK_INDEX_LIFECYCLE_CORRUPT);
}

// P07-10b: CURRENT valid but referenced generation missing -> FATAL (no downgrade).
BOOST_AUTO_TEST_CASE(p07_10b_current_missing_generation_is_fatal)
{
    P07TempRoot t;
    fs::path v2Root = t.path / "bzindexv2";
    fs::path dataDir = t.path / "data";
    fs::create_directories(dataDir / "txleveldb");
    fs::create_directories(v2Root);
    WriteValidCurrent(v2Root.string(), 1); // gen-000001 absent

    BlockIndexStartupOwnershipDecision d;
    std::string err;
    BOOST_REQUIRE(ClassifyBlockIndexStartupOwnership(v2Root.string(), dataDir.string(), &d, &err));
    BOOST_CHECK_EQUAL(d.state, BLOCK_INDEX_STARTUP_OWNERSHIP_FATAL_CORRUPTION);
    BOOST_CHECK_EQUAL(d.v2CurrentStatus, BLOCK_INDEX_LIFECYCLE_OK); // CURRENT parses
}

// P07-10c: unpublished generation artifacts, no legacy -> EMPTY_NEW (+artifact flag).
BOOST_AUTO_TEST_CASE(p07_10c_unpublished_artifacts_no_legacy_is_empty_new)
{
    P07TempRoot t;
    fs::path v2Root = t.path / "bzindexv2";
    fs::path dataDir = t.path / "data";
    fs::create_directories(dataDir);              // no txleveldb
    fs::create_directories(v2Root / "blockindex-build-000002.tmp"); // interrupted build artifact

    BlockIndexStartupOwnershipDecision d;
    std::string err;
    BOOST_REQUIRE(ClassifyBlockIndexStartupOwnership(v2Root.string(), dataDir.string(), &d, &err));
    BOOST_CHECK_EQUAL(d.state, BLOCK_INDEX_STARTUP_OWNERSHIP_EMPTY_NEW);
    BOOST_CHECK(d.unpublishedArtifacts);
}

// P07-10d: unpublished generation artifacts + legacy -> migration retry.
BOOST_AUTO_TEST_CASE(p07_10d_unpublished_artifacts_with_legacy_retries_migration)
{
    P07TempRoot t;
    fs::path v2Root = t.path / "bzindexv2";
    fs::path dataDir = t.path / "data";
    fs::create_directories(dataDir / "txleveldb");
    fs::create_directories(v2Root / "blockindex-build-000002.tmp");

    BlockIndexStartupOwnershipDecision d;
    std::string err;
    BOOST_REQUIRE(ClassifyBlockIndexStartupOwnership(v2Root.string(), dataDir.string(), &d, &err));
    BOOST_CHECK_EQUAL(d.state, BLOCK_INDEX_STARTUP_OWNERSHIP_LEGACY_MIGRATION_REQUIRED);
    BOOST_CHECK(d.unpublishedArtifacts);
    BOOST_CHECK(d.legacyPresent);
}

// P07-11: canonical production V2 root = the effective network datadir ITSELF
// (NO blockindex-v2/ container); explicit diagnostic override still wins.
BOOST_AUTO_TEST_CASE(p07_11_canonical_default_root_is_effective_datadir)
{
    P07TempRoot t;
    // GetDataDir() is network-specific: for regtest it resolves <base>/regtest.
    const std::string effectiveDatadir = (t.path / "regtest").string();
    fs::create_directories(effectiveDatadir);

    // (a) default root == effective network datadir (NOT <datadir>/blockindex-v2).
    BOOST_CHECK_EQUAL(GetDefaultBlockIndexV2Root(effectiveDatadir), effectiveDatadir);
    BOOST_CHECK(GetDefaultBlockIndexV2Root(effectiveDatadir) !=
                (fs::path(effectiveDatadir) / "blockindex-v2").string());

    // (b) resolver: no override -> effective datadir; explicit override wins.
    BOOST_CHECK_EQUAL(ResolveBlockIndexV2Root("", effectiveDatadir), effectiveDatadir);
    BOOST_CHECK_EQUAL(ResolveBlockIndexV2Root("/custom/root", effectiveDatadir), "/custom/root");

    // (c) the C1 physical lifecycle names appear DIRECTLY under that root, with
    //     NO blockindex-v2/ and NO blockindex/ container.
    const std::string root = ResolveBlockIndexV2Root("", effectiveDatadir);
    WriteValidCurrent(root, 1);
    fs::create_directories(fs::path(root) / "blockindex-gen-000001");
    fs::create_directories(fs::path(root) / "blockindex-build-000002.tmp");
    BOOST_CHECK(fs::exists(fs::path(root) / "blockindex-current"));
    BOOST_CHECK(fs::exists(fs::path(root) / "blockindex-gen-000001"));
    BOOST_CHECK(fs::exists(fs::path(root) / "blockindex-build-000002.tmp"));
    BOOST_CHECK(!fs::exists(fs::path(root) / "blockindex-v2"));
    BOOST_CHECK(!fs::exists(fs::path(root) / "blockindex"));
}

BOOST_AUTO_TEST_SUITE_END()
