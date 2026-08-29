#include <boost/test/unit_test.hpp>
#include "../fixed_blockindex_store.h"
#include "../blockindex_accessor.h"
#include "../main.h"
#include "../util.h"

#include <boost/filesystem.hpp>
#include <stdint.h>
#include <stdio.h>

#include <fstream>
#include <limits>
#include <set>
#include <string>
#include <vector>

namespace {

static boost::filesystem::path UniqueStoreDir(const std::string& tag)
{
    boost::filesystem::path dir = boost::filesystem::temp_directory_path() /
        boost::filesystem::unique_path("innova-blockindex-" + tag + "-%%%%-%%%%-%%%%");
    boost::filesystem::create_directories(dir);
    return dir;
}

static std::string ReadWholeFile(const boost::filesystem::path& path)
{
    std::ifstream in(path.string().c_str(), std::ios::in | std::ios::binary);
    std::string data;
    in.seekg(0, std::ios::end);
    data.resize((size_t)in.tellg());
    in.seekg(0, std::ios::beg);
    if (!data.empty())
        in.read(&data[0], data.size());
    return data;
}

static void WriteWholeFile(const boost::filesystem::path& path, const std::string& data)
{
    std::ofstream out(path.string().c_str(), std::ios::out | std::ios::binary | std::ios::trunc);
    BOOST_REQUIRE(out.is_open());
    if (!data.empty())
        out.write(data.data(), data.size());
    out.close();
    BOOST_REQUIRE(out.good());
}

static void FlipOneByte(const boost::filesystem::path& path, size_t offset)
{
    std::string data = ReadWholeFile(path);
    BOOST_REQUIRE_LT(offset, data.size());
    data[offset] ^= (char)0x5a;
    WriteWholeFile(path, data);
}

static void TruncateFileTo(const boost::filesystem::path& path, size_t size)
{
    FILE* f = fopen(path.string().c_str(), "r+b");
    BOOST_REQUIRE(f != NULL);
    BOOST_REQUIRE(ftruncate(fileno(f), (off_t)size) == 0);
    fclose(f);
}

static BlockIndexRecord MakeRecord(uint64_t nonceBase, int32_t height, bool fPos)
{
    BlockIndexRecord rec;
    rec.hash = uint256(nonceBase + 1);
    rec.hashPrev = height == 0 ? uint256(0) : uint256(nonceBase);
    rec.height = height;
    rec.nFile = (uint32_t)(100 + nonceBase);
    rec.nBlockPos = (uint32_t)(200 + nonceBase);
    rec.nFlags = fPos ? CBlockIndex::BLOCK_PROOF_OF_STAKE : 0;
    rec.nVersion = 7;
    rec.nTime = (uint32_t)(1700000000U + (unsigned int)height);
    rec.nBits = (uint32_t)(0x1d00ffffU - (unsigned int)(nonceBase & 0xff));
    rec.nNonce = (uint32_t)(9000 + nonceBase);
    rec.nMint = 1000000 + (int64_t)nonceBase;
    rec.nMoneySupply = 5000000 + (int64_t)(nonceBase * 10);
    rec.nStakeModifier = 0xabcdef0000000000ULL + nonceBase;
    rec.prevoutStake = fPos ? COutPoint(uint256(nonceBase + 1000), (unsigned int)(nonceBase % 11)) : COutPoint();
    rec.nStakeTime = fPos ? (uint32_t)(1700001000U + (unsigned int)height) : 0;
    rec.hashProof = uint256(nonceBase + 2000);
    rec.hashMerkleRoot = uint256(nonceBase + 3000);
    return rec;
}

static void ExpectRecordsEqual(const BlockIndexRecord& a, const BlockIndexRecord& b)
{
    BOOST_CHECK_EQUAL(a.hash.ToString(), b.hash.ToString());
    BOOST_CHECK_EQUAL(a.hashPrev.ToString(), b.hashPrev.ToString());
    BOOST_CHECK_EQUAL(a.height, b.height);
    BOOST_CHECK_EQUAL(a.nFile, b.nFile);
    BOOST_CHECK_EQUAL(a.nBlockPos, b.nBlockPos);
    BOOST_CHECK_EQUAL(a.nFlags, b.nFlags);
    BOOST_CHECK_EQUAL(a.nVersion, b.nVersion);
    BOOST_CHECK_EQUAL(a.nTime, b.nTime);
    BOOST_CHECK_EQUAL(a.nBits, b.nBits);
    BOOST_CHECK_EQUAL(a.nNonce, b.nNonce);
    BOOST_CHECK_EQUAL(a.nMint, b.nMint);
    BOOST_CHECK_EQUAL(a.nMoneySupply, b.nMoneySupply);
    BOOST_CHECK_EQUAL(a.nStakeModifier, b.nStakeModifier);
    BOOST_CHECK_EQUAL(a.prevoutStake.ToString(), b.prevoutStake.ToString());
    BOOST_CHECK_EQUAL(a.nStakeTime, b.nStakeTime);
    BOOST_CHECK_EQUAL(a.hashProof.ToString(), b.hashProof.ToString());
    BOOST_CHECK_EQUAL(a.hashMerkleRoot.ToString(), b.hashMerkleRoot.ToString());
}

static BlockIndexRecord RecordFromSnapshot(const BlockIndexSnapshot& snap)
{
    BlockIndexRecord rec;
    rec.hash = snap.hash;
    rec.hashPrev = snap.hashPrev;
    rec.height = snap.height;
    rec.nFile = snap.nFile;
    rec.nBlockPos = snap.nBlockPos;
    rec.nFlags = snap.nFlags;
    rec.nVersion = snap.nVersion;
    rec.nTime = snap.nTime;
    rec.nBits = snap.nBits;
    rec.nNonce = snap.nNonce;
    rec.nMint = snap.nMint;
    rec.nMoneySupply = snap.nMoneySupply;
    rec.nStakeModifier = snap.nStakeModifier;
    rec.prevoutStake = snap.prevoutStake;
    rec.nStakeTime = snap.nStakeTime;
    rec.hashProof = snap.hashProof;
    rec.hashMerkleRoot = uint256(0);
    return rec;
}

static void ExpectRecordMatchesIndex(const BlockIndexRecord& rec, const CBlockIndex* pindex)
{
    BOOST_REQUIRE(pindex != NULL);
    BOOST_CHECK_EQUAL(rec.hash.ToString(), pindex->GetBlockHash().ToString());
    BOOST_CHECK_EQUAL(rec.hashPrev.ToString(), pindex->pprev ? pindex->pprev->GetBlockHash().ToString() : uint256(0).ToString());
    BOOST_CHECK_EQUAL(rec.height, pindex->nHeight);
    BOOST_CHECK_EQUAL(rec.nFile, pindex->nFile);
    BOOST_CHECK_EQUAL(rec.nBlockPos, pindex->nBlockPos);
    BOOST_CHECK_EQUAL(rec.nFlags, pindex->nFlags);
    BOOST_CHECK_EQUAL(rec.nVersion, pindex->nVersion);
    BOOST_CHECK_EQUAL(rec.nTime, pindex->nTime);
    BOOST_CHECK_EQUAL(rec.nBits, pindex->nBits);
    BOOST_CHECK_EQUAL(rec.nNonce, pindex->nNonce);
    BOOST_CHECK_EQUAL(rec.nMint, pindex->nMint);
    BOOST_CHECK_EQUAL(rec.nMoneySupply, pindex->nMoneySupply);
    BOOST_CHECK_EQUAL(rec.nStakeModifier, pindex->nStakeModifier);
    BOOST_CHECK_EQUAL(rec.prevoutStake.ToString(), pindex->prevoutStake.ToString());
    BOOST_CHECK_EQUAL(rec.nStakeTime, pindex->nStakeTime);
    BOOST_CHECK_EQUAL(rec.hashProof.ToString(), pindex->hashProof.ToString());
    BOOST_CHECK_EQUAL(rec.hashMerkleRoot.ToString(), pindex->hashMerkleRoot.ToString());
}

static FixedBlockIndexStore CreateStore(const boost::filesystem::path& dir, uint64_t generation)
{
    std::string error;
    FixedBlockIndexStore store;
    BOOST_REQUIRE(FixedBlockIndexStore::Create(dir.string(), generation, &store, &error));
    BOOST_REQUIRE_MESSAGE(error.empty(), error);
    return store;
}

static FixedBlockIndexStore OpenStoreReadOnly(const boost::filesystem::path& dir, bool requireComplete)
{
    std::string error;
    FixedBlockIndexStore store;
    FixedBlockIndexOpenOptions options;
    options.requireCompleteManifest = requireComplete;
    BOOST_REQUIRE(FixedBlockIndexStore::OpenReadOnly(dir.string(), options, &store, &error));
    BOOST_REQUIRE_MESSAGE(error.empty(), error);
    return store;
}

} // namespace

BOOST_AUTO_TEST_SUITE(fixed_blockindex_store_tests)

BOOST_AUTO_TEST_CASE(record_layout_constants_are_exact)
{
    BOOST_CHECK_EQUAL(BLOCK_INDEX_FORMAT_VERSION, 1U);
    BOOST_CHECK_EQUAL(BLOCK_INDEX_RECORD_VERSION, 1U);
    BOOST_CHECK_EQUAL(BLOCK_INDEX_RECORD_SIZE_V1, 228U);
    BOOST_CHECK_EQUAL(BLOCK_INDEX_RECORDS_HEADER_SIZE_V1, 40U);
    BOOST_CHECK_EQUAL(BLOCK_INDEX_MANIFEST_SIZE_V1, 88U);
}

BOOST_AUTO_TEST_CASE(codec_roundtrip_is_deterministic_and_exact_size)
{
    BlockIndexRecord rec = MakeRecord(11, 7, true);
    std::vector<unsigned char> a;
    std::vector<unsigned char> b;
    std::string error;
    BOOST_REQUIRE(EncodeBlockIndexRecordV1(rec, &a, &error));
    BOOST_REQUIRE_MESSAGE(error.empty(), error);
    BOOST_REQUIRE(EncodeBlockIndexRecordV1(rec, &b, &error));
    BOOST_REQUIRE_MESSAGE(error.empty(), error);
    BOOST_CHECK_EQUAL(a.size(), BLOCK_INDEX_RECORD_SIZE_V1);
    BOOST_CHECK_EQUAL_COLLECTIONS(a.begin(), a.end(), b.begin(), b.end());

    BlockIndexRecord decoded;
    BOOST_REQUIRE(DecodeBlockIndexRecordV1(a.data(), a.size(), &decoded, &error));
    BOOST_REQUIRE_MESSAGE(error.empty(), error);
    ExpectRecordsEqual(rec, decoded);
}

BOOST_AUTO_TEST_CASE(store_rejects_manifest_building_when_complete_required_and_accepts_complete)
{
    boost::filesystem::path dir = UniqueStoreDir("manifest-state");
    FixedBlockIndexStore store = CreateStore(dir, 1);
    BlockIndexRecord rec = MakeRecord(1, 0, false);
    BlockIndexId id = BLOCK_INDEX_ID_INVALID;
    std::string error;
    BOOST_REQUIRE(store.Append(rec, &id, &error));
    BOOST_REQUIRE(id != BLOCK_INDEX_ID_INVALID);

    FixedBlockIndexManifest manifest = store.GetManifest();
    manifest.state = BLOCK_INDEX_MANIFEST_BUILDING;
    manifest.recordCount = 0;
    manifest.committedTipId = BLOCK_INDEX_ID_INVALID;
    manifest.committedTipHeight = -1;
    manifest.committedTipHash = uint256(0);
    BOOST_REQUIRE(store.WriteManifest(manifest, &error));

    FixedBlockIndexStore ro;
    FixedBlockIndexOpenOptions strictOptions;
    strictOptions.requireCompleteManifest = true;
    error.clear();
    BOOST_CHECK(!FixedBlockIndexStore::OpenReadOnly(dir.string(), strictOptions, &ro, &error));
    BOOST_CHECK(!error.empty());

    manifest.state = BLOCK_INDEX_MANIFEST_COMPLETE;
    manifest.recordCount = 1;
    manifest.committedTipId = id;
    manifest.committedTipHeight = rec.height;
    manifest.committedTipHash = rec.hash;
    BOOST_REQUIRE(store.WriteManifest(manifest, &error));

    error.clear();
    BOOST_REQUIRE(FixedBlockIndexStore::OpenReadOnly(dir.string(), strictOptions, &ro, &error));
    BOOST_CHECK_EQUAL(ro.CommittedRecordCount(), 1U);
}

BOOST_AUTO_TEST_CASE(read_validates_invalid_id_past_end_and_overflow)
{
    boost::filesystem::path dir = UniqueStoreDir("bounds");
    FixedBlockIndexStore store = CreateStore(dir, 7);
    std::string error;
    BlockIndexId id1 = BLOCK_INDEX_ID_INVALID;
    BOOST_REQUIRE(store.Append(MakeRecord(1, 0, false), &id1, &error));

    FixedBlockIndexManifest manifest = store.GetManifest();
    manifest.state = BLOCK_INDEX_MANIFEST_COMPLETE;
    manifest.recordCount = 1;
    manifest.committedTipId = id1;
    manifest.committedTipHeight = 0;
    manifest.committedTipHash = MakeRecord(1, 0, false).hash;
    BOOST_REQUIRE(store.WriteManifest(manifest, &error));

    FixedBlockIndexStore ro = OpenStoreReadOnly(dir, true);
    BlockIndexRecord rec;
    error.clear();
    BOOST_CHECK(!ro.Read(BLOCK_INDEX_ID_INVALID, &rec, &error));
    BOOST_CHECK(!error.empty());
    error.clear();
    BOOST_CHECK(!ro.Read(id1 + 1, &rec, &error));
    BOOST_CHECK(!error.empty());
    error.clear();
    BOOST_CHECK(!ro.Read(std::numeric_limits<uint64_t>::max(), &rec, &error));
    BOOST_CHECK(!error.empty());
}

BOOST_AUTO_TEST_CASE(truncated_header_invalid_magic_versions_and_record_size_fail_closed)
{
    boost::filesystem::path dir = UniqueStoreDir("header");
    FixedBlockIndexStore store = CreateStore(dir, 5);
    std::string error;
    const boost::filesystem::path recordsPath = dir / BLOCK_INDEX_RECORDS_FILE_NAME;
    const boost::filesystem::path manifestPath = dir / BLOCK_INDEX_MANIFEST_FILE_NAME;

    std::string origRecords = ReadWholeFile(recordsPath);
    std::string origManifest = ReadWholeFile(manifestPath);

    TruncateFileTo(recordsPath, BLOCK_INDEX_RECORDS_HEADER_SIZE_V1 - 1);
    FixedBlockIndexStore ro;
    FixedBlockIndexOpenOptions opts;
    opts.requireCompleteManifest = false;
    error.clear();
    BOOST_CHECK(!FixedBlockIndexStore::OpenReadOnly(dir.string(), opts, &ro, &error));
    BOOST_CHECK(!error.empty());
    WriteWholeFile(recordsPath, origRecords);

    std::string broken = origRecords;
    broken[0] ^= 0x01;
    WriteWholeFile(recordsPath, broken);
    error.clear();
    BOOST_CHECK(!FixedBlockIndexStore::OpenReadOnly(dir.string(), opts, &ro, &error));
    BOOST_CHECK(!error.empty());
    WriteWholeFile(recordsPath, origRecords);

    std::string manifestBroken = origManifest;
    manifestBroken[8] = 2; // format_version little-endian low byte
    WriteWholeFile(manifestPath, manifestBroken);
    error.clear();
    BOOST_CHECK(!FixedBlockIndexStore::OpenReadOnly(dir.string(), opts, &ro, &error));
    BOOST_CHECK(!error.empty());
    WriteWholeFile(manifestPath, origManifest);

    broken = origRecords;
    broken[12] = 2; // record_version low byte
    WriteWholeFile(recordsPath, broken);
    error.clear();
    BOOST_CHECK(!FixedBlockIndexStore::OpenReadOnly(dir.string(), opts, &ro, &error));
    BOOST_CHECK(!error.empty());
    WriteWholeFile(recordsPath, origRecords);

    broken = origRecords;
    broken[20] ^= 0x01; // record_size low byte
    WriteWholeFile(recordsPath, broken);
    error.clear();
    BOOST_CHECK(!FixedBlockIndexStore::OpenReadOnly(dir.string(), opts, &ro, &error));
    BOOST_CHECK(!error.empty());
    WriteWholeFile(recordsPath, origRecords);
}

BOOST_AUTO_TEST_CASE(truncated_record_partial_tail_and_checksum_corruption_fail_closed)
{
    boost::filesystem::path dir = UniqueStoreDir("corruption");
    FixedBlockIndexStore store = CreateStore(dir, 9);
    std::string error;
    BlockIndexId id1 = BLOCK_INDEX_ID_INVALID;
    BlockIndexId id2 = BLOCK_INDEX_ID_INVALID;
    BlockIndexRecord rec1 = MakeRecord(1, 0, false);
    BlockIndexRecord rec2 = MakeRecord(2, 1, true);
    BOOST_REQUIRE(store.Append(rec1, &id1, &error));
    BOOST_REQUIRE(store.Append(rec2, &id2, &error));
    FixedBlockIndexManifest manifest = store.GetManifest();
    manifest.state = BLOCK_INDEX_MANIFEST_COMPLETE;
    manifest.recordCount = 2;
    manifest.committedTipId = id2;
    manifest.committedTipHeight = 1;
    manifest.committedTipHash = rec2.hash;
    BOOST_REQUIRE(store.WriteManifest(manifest, &error));

    const boost::filesystem::path recordsPath = dir / BLOCK_INDEX_RECORDS_FILE_NAME;
    FixedBlockIndexStore ro = OpenStoreReadOnly(dir, true);
    BlockIndexRecord out;
    BOOST_REQUIRE(ro.Read(id2, &out, &error));

    TruncateFileTo(recordsPath, BLOCK_INDEX_RECORDS_HEADER_SIZE_V1 + BLOCK_INDEX_RECORD_SIZE_V1 + 17);
    FixedBlockIndexStore roTrunc;
    FixedBlockIndexOpenOptions opts;
    opts.requireCompleteManifest = true;
    error.clear();
    BOOST_CHECK(!FixedBlockIndexStore::OpenReadOnly(dir.string(), opts, &roTrunc, &error));
    BOOST_CHECK(!error.empty());

    dir = UniqueStoreDir("tail");
    store = CreateStore(dir, 10);
    BOOST_REQUIRE(store.Append(rec1, &id1, &error));
    BOOST_REQUIRE(store.Append(rec2, &id2, &error));
    manifest = store.GetManifest();
    manifest.state = BLOCK_INDEX_MANIFEST_COMPLETE;
    manifest.recordCount = 1;
    manifest.committedTipId = id1;
    manifest.committedTipHeight = 0;
    manifest.committedTipHash = rec1.hash;
    BOOST_REQUIRE(store.WriteManifest(manifest, &error));
    ro = OpenStoreReadOnly(dir, true);
    BOOST_REQUIRE(ro.Read(id1, &out, &error));
    error.clear();
    BOOST_CHECK(!ro.Read(id2, &out, &error));
    BOOST_CHECK(!error.empty());

    dir = UniqueStoreDir("flip");
    store = CreateStore(dir, 11);
    BOOST_REQUIRE(store.Append(rec1, &id1, &error));
    manifest = store.GetManifest();
    manifest.state = BLOCK_INDEX_MANIFEST_COMPLETE;
    manifest.recordCount = 1;
    manifest.committedTipId = id1;
    manifest.committedTipHeight = 0;
    manifest.committedTipHash = rec1.hash;
    BOOST_REQUIRE(store.WriteManifest(manifest, &error));
    FlipOneByte(dir / BLOCK_INDEX_RECORDS_FILE_NAME, BLOCK_INDEX_RECORDS_HEADER_SIZE_V1 + 20);
    ro = OpenStoreReadOnly(dir, true);
    error.clear();
    BOOST_CHECK(!ro.Read(id1, &out, &error));
    BOOST_CHECK(!error.empty());
}

BOOST_AUTO_TEST_CASE(legacy_encode_decode_semantics_match_for_admitted_v1_fields)
{
    LegacyBlockIndexAccessor accessor;
    LOCK(cs_main);
    BOOST_REQUIRE(pindexBest != NULL);

    std::vector<int> heights;
    heights.push_back(0);
    heights.push_back(std::min(1, pindexBest->nHeight));
    heights.push_back(std::max(0, pindexBest->nHeight / 2));
    heights.push_back(std::max(0, pindexBest->nHeight - 1));
    heights.push_back(pindexBest->nHeight);
    std::sort(heights.begin(), heights.end());
    heights.erase(std::unique(heights.begin(), heights.end()), heights.end());

    for (size_t i = 0; i < heights.size(); ++i)
    {
        CBlockIndex* pindex = FindBlockByHeight(heights[i]);
        BOOST_REQUIRE(pindex != NULL);
        BlockIndexSnapshot snap = accessor.LookupByHash(pindex->GetBlockHash());
        BOOST_REQUIRE(snap.found);
        BlockIndexRecord rec = RecordFromSnapshot(snap);
        rec.hashMerkleRoot = pindex->hashMerkleRoot;

        std::vector<unsigned char> encoded;
        std::string error;
        BOOST_REQUIRE(EncodeBlockIndexRecordV1(rec, &encoded, &error));
        BlockIndexRecord decoded;
        BOOST_REQUIRE(DecodeBlockIndexRecordV1(encoded.data(), encoded.size(), &decoded, &error));
        ExpectRecordMatchesIndex(decoded, pindex);
    }
}

BOOST_AUTO_TEST_SUITE_END()
