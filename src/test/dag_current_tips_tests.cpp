#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MODULE Current DAG Tips
#include <boost/test/unit_test.hpp>

#include "../dag.h"
#include "../dag_tip_frontier.h"
#include "../serialize.h"

#include <boost/filesystem.hpp>
#include <leveldb/db.h>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <vector>

namespace {
namespace fs = boost::filesystem;

static bool PutDagLink(const std::string& dir, const uint256& hash,
                       const std::vector<uint256>& parents)
{
    leveldb::Options options;
    options.create_if_missing = true;
    leveldb::DB* db = NULL;
    if (!leveldb::DB::Open(options, dir, &db).ok()) return false;
    CBlockDAGData data;
    data.vDAGParents = parents;
    CDataStream key(SER_DISK, CLIENT_VERSION);
    CDataStream value(SER_DISK, CLIENT_VERSION);
    key << make_pair(std::string("daglinks"), hash);
    value << data;
    const bool ok = db->Put(leveldb::WriteOptions(), key.str(), value.str()).ok();
    delete db;
    return ok;
}

static std::vector<uint256> ReadRawTips(const fs::path& path)
{
    std::ifstream input(path.string().c_str(), std::ios::binary);
    BOOST_REQUIRE(input.good());
    std::vector<uint256> result;
    for (;;)
    {
        unsigned char raw[32];
        input.read((char*)raw, sizeof(raw));
        if (input.gcount() == 0) break;
        BOOST_REQUIRE_EQUAL(input.gcount(), (std::streamsize)sizeof(raw));
        uint256 value;
        memcpy(value.begin(), raw, sizeof(raw));
        result.push_back(value);
    }
    return result;
}

static std::vector<unsigned char> ReadBytes(const fs::path& path)
{
    std::ifstream input(path.string().c_str(), std::ios::binary);
    return std::vector<unsigned char>((std::istreambuf_iterator<char>(input)),
                                      std::istreambuf_iterator<char>());
}

static dag_tip_frontier::BuildOptions TightOptions(const fs::path& root)
{
    dag_tip_frontier::BuildOptions options;
    options.tempParent = root.string();
    options.chunkBytes = 64;
    options.maxRecordsPerChunk = 2;
    options.maxOpenRuns = 2;
    return options;
}
}

BOOST_AUTO_TEST_CASE(current_source_exact_mutable_multirun_deterministic_raw_stream)
{
    const fs::path root = fs::temp_directory_path() / fs::unique_path("dag-current-%%%%-%%%%");
    fs::create_directories(root);
    const fs::path db = root / "db";
    const fs::path firstOut = root / "first.raw";
    const fs::path secondOut = root / "second.raw";
    const uint256 genesis(1), left(2), right(3), merged(4);

    BOOST_REQUIRE(PutDagLink(db.string(), genesis, std::vector<uint256>()));
    BOOST_REQUIRE(PutDagLink(db.string(), left, std::vector<uint256>(1, genesis)));
    BOOST_REQUIRE(PutDagLink(db.string(), right, std::vector<uint256>(1, genesis)));

    dag_tip_frontier::CurrentDagTipDerivationResult first;
    BOOST_REQUIRE(dag_tip_frontier::DeriveCurrentDagTipsBounded(
        db.string(), firstOut.string(), TightOptions(root), &first));
    BOOST_CHECK_EQUAL(first.status, dag_tip_frontier::DAG_CURRENT_TIPS_DERIVATION_OK);
    BOOST_CHECK_EQUAL(first.tipCount, 2U);
    BOOST_CHECK_GT(first.runCount, 1U);
    BOOST_CHECK_LE(first.peakChunkRecords, 4U);
    BOOST_CHECK_EQUAL(fs::file_size(firstOut), 64U);
    std::vector<uint256> expectedFirst;
    expectedFirst.push_back(left); expectedFirst.push_back(right);
    std::sort(expectedFirst.begin(), expectedFirst.end());
    BOOST_CHECK(ReadRawTips(firstOut) == expectedFirst);

    BOOST_REQUIRE(PutDagLink(db.string(), merged, std::vector<uint256>({left, right})));
    dag_tip_frontier::CurrentDagTipDerivationResult mutated, repeat;
    BOOST_REQUIRE(dag_tip_frontier::DeriveCurrentDagTipsBounded(
        db.string(), firstOut.string(), TightOptions(root), &mutated));
    BOOST_REQUIRE(dag_tip_frontier::DeriveCurrentDagTipsBounded(
        db.string(), secondOut.string(), TightOptions(root), &repeat));
    BOOST_CHECK_EQUAL(mutated.status, dag_tip_frontier::DAG_CURRENT_TIPS_DERIVATION_OK);
    BOOST_CHECK_EQUAL(mutated.tipCount, 1U);
    BOOST_CHECK(ReadRawTips(firstOut) == std::vector<uint256>(1, merged));
    BOOST_CHECK(ReadBytes(firstOut) == ReadBytes(secondOut));

    fs::remove_all(root);
}

BOOST_AUTO_TEST_CASE(current_source_unavailable_and_output_failure_are_typed_and_not_published)
{
    const fs::path root = fs::temp_directory_path() / fs::unique_path("dag-current-failure-%%%%-%%%%");
    fs::create_directories(root);
    dag_tip_frontier::CurrentDagTipDerivationResult unavailable;
    const fs::path absentOutput = root / "absent.raw";
    BOOST_CHECK(!dag_tip_frontier::DeriveCurrentDagTipsBounded(
        (root / "missing-db").string(), absentOutput.string(), TightOptions(root), &unavailable));
    BOOST_CHECK_EQUAL(unavailable.status,
                      dag_tip_frontier::DAG_CURRENT_TIPS_DERIVATION_SOURCE_UNAVAILABLE);
    BOOST_CHECK(!fs::exists(absentOutput));

    const fs::path db = root / "db";
    BOOST_REQUIRE(PutDagLink(db.string(), uint256(1), std::vector<uint256>()));
    const fs::path nonDirectory = root / "not-a-directory";
    { std::ofstream marker(nonDirectory.string().c_str()); marker << "x"; }
    const fs::path blockedOutput = nonDirectory / "output.raw";
    dag_tip_frontier::CurrentDagTipDerivationResult publication;
    BOOST_CHECK(!dag_tip_frontier::DeriveCurrentDagTipsBounded(
        db.string(), blockedOutput.string(), TightOptions(root), &publication));
    BOOST_CHECK_EQUAL(publication.status,
                      dag_tip_frontier::DAG_CURRENT_TIPS_DERIVATION_OUTPUT_PUBLICATION_FAILURE);
    BOOST_CHECK(!fs::exists(blockedOutput));

    fs::remove_all(root);
}
