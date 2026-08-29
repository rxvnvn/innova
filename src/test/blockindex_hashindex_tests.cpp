#include <boost/test/unit_test.hpp>
#include "../blockindex_hashindex.h"
#include "../fixed_blockindex_store.h"
#include "../blockindex_accessor.h"
#include "../main.h"
#include "../util.h"

#include <boost/filesystem.hpp>
#include <leveldb/db.h>

#include <fstream>
#include <limits>
#include <set>
#include <string>
#include <vector>

namespace {

static boost::filesystem::path UniqueGenDir(const std::string& tag)
{
    boost::filesystem::path model =
        boost::filesystem::path("innova-blockindex-hashindex-") /
        boost::filesystem::path(tag + "-%%%%-%%%%-%%%%");
    boost::filesystem::path dir = boost::filesystem::temp_directory_path() /
        boost::filesystem::unique_path(model) /
        "gen-000001";
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
    data[offset] ^= (char)0x41;
    WriteWholeFile(path, data);
}

static BlockIndexRecord MakeRecord(uint64_t nonceBase, int32_t height, bool fPos)
{
    BlockIndexRecord rec;
    rec.hash = uint256(nonceBase + 1);
    rec.hashPrev = height == 0 ? uint256(0) : uint256(nonceBase);
    rec.height = height;
    rec.nFile = (uint32_t)(10 + nonceBase);
    rec.nBlockPos = (uint32_t)(20 + nonceBase);
    rec.nFlags = fPos ? CBlockIndex::BLOCK_PROOF_OF_STAKE : 0;
    rec.nVersion = 7;
    rec.nTime = (uint32_t)(1700000000U + (unsigned int)height);
    rec.nBits = (uint32_t)(0x1d00ffffU - (unsigned int)(nonceBase & 0xff));
    rec.nNonce = (uint32_t)(9000 + nonceBase);
    rec.nMint = 1000000 + (int64_t)nonceBase;
    rec.nMoneySupply = 5000000 + (int64_t)(nonceBase * 10);
    rec.nStakeModifier = 0x1234000000000000ULL + nonceBase;
    rec.prevoutStake = fPos ? COutPoint(uint256(nonceBase + 1000), (unsigned int)(nonceBase % 13)) : COutPoint();
    rec.nStakeTime = fPos ? (uint32_t)(1700002000U + (unsigned int)height) : 0;
    rec.hashProof = uint256(nonceBase + 2000);
    rec.hashMerkleRoot = uint256(nonceBase + 3000);
    return rec;
}

static void ExpectRecordsEqual(const BlockIndexRecord& a, const BlockIndexRecord& b)
{
    BOOST_CHECK_EQUAL(a.hash.ToString(), b.hash.ToString());
    BOOST_CHECK_EQUAL(a.hashPrev.ToString(), b.hashPrev.ToString());
    BOOST_CHECK_EQUAL(a.hashMerkleRoot.ToString(), b.hashMerkleRoot.ToString());
    BOOST_CHECK_EQUAL(a.hashProof.ToString(), b.hashProof.ToString());
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
}

static BlockIndexRecord RecordFromIndex(const CBlockIndex* pindex)
{
    BlockIndexRecord rec;
    rec.hash = pindex->GetBlockHash();
    rec.hashPrev = pindex->pprev ? pindex->pprev->GetBlockHash() : uint256(0);
    rec.hashMerkleRoot = pindex->hashMerkleRoot;
    rec.hashProof = pindex->hashProof;
    rec.height = pindex->nHeight;
    rec.nFile = pindex->nFile;
    rec.nBlockPos = pindex->nBlockPos;
    rec.nFlags = pindex->nFlags;
    rec.nVersion = pindex->nVersion;
    rec.nTime = pindex->nTime;
    rec.nBits = pindex->nBits;
    rec.nNonce = pindex->nNonce;
    rec.nMint = pindex->nMint;
    rec.nMoneySupply = pindex->nMoneySupply;
    rec.nStakeModifier = pindex->nStakeModifier;
    rec.prevoutStake = pindex->prevoutStake;
    rec.nStakeTime = pindex->nStakeTime;
    return rec;
}

static FixedBlockIndexStore CreateStore(const boost::filesystem::path& dir, uint64_t generation)
{
    std::string error;
    FixedBlockIndexStore store;
    BOOST_REQUIRE(FixedBlockIndexStore::Create(dir.string(), generation, &store, &error));
    BOOST_REQUIRE_MESSAGE(error.empty(), error);
    return store;
}

static BlockIndexHashIndex CreateHashIndex(const boost::filesystem::path& dir, uint64_t generation)
{
    std::string error;
    BlockIndexHashIndex index;
    BOOST_REQUIRE(BlockIndexHashIndex::Create(dir.string(), generation, &index, &error));
    BOOST_REQUIRE_MESSAGE(error.empty(), error);
    return index;
}

static FixedBlockIndexShadowLookup OpenShadowLookup(const boost::filesystem::path& dir)
{
    std::string error;
    FixedBlockIndexShadowLookup lookup;
    BOOST_REQUIRE_MESSAGE(FixedBlockIndexShadowLookup::Open(dir.string(), &lookup, &error), error);
    BOOST_REQUIRE_MESSAGE(error.empty(), error);
    return lookup;
}

} // namespace

BOOST_AUTO_TEST_SUITE(blockindex_hashindex_tests)

BOOST_AUTO_TEST_CASE(hash_key_and_recordid_value_codecs_are_exact_and_deterministic)
{
    std::string key;
    std::string value;
    std::string error;
    const uint256 hash(0x1234);
    BOOST_REQUIRE(EncodeBlockIndexHashKey(hash, &key, &error));
    BOOST_REQUIRE(EncodeBlockIndexRecordIdValue(0x0102030405060708ULL, &value, &error));
    BOOST_CHECK_EQUAL((int)key.size(), (int)BLOCK_INDEX_HASH_KEY_SIZE);
    BOOST_CHECK_EQUAL((unsigned char)key[0], (unsigned char)BLOCK_INDEX_HASH_KEY_PREFIX);
    BOOST_CHECK_EQUAL((int)value.size(), (int)BLOCK_INDEX_HASH_VALUE_SIZE);
    BOOST_CHECK_EQUAL((unsigned char)value[0], 0x08);
    BOOST_CHECK_EQUAL((unsigned char)value[7], 0x01);

    std::string key2;
    std::string value2;
    BOOST_REQUIRE(EncodeBlockIndexHashKey(hash, &key2, &error));
    BOOST_REQUIRE(EncodeBlockIndexRecordIdValue(0x0102030405060708ULL, &value2, &error));
    BOOST_CHECK_EQUAL_COLLECTIONS(key.begin(), key.end(), key2.begin(), key2.end());
    BOOST_CHECK_EQUAL_COLLECTIONS(value.begin(), value.end(), value2.begin(), value2.end());

    BlockIndexId decoded = 0;
    BOOST_REQUIRE(DecodeBlockIndexRecordIdValue(value.data(), value.size(), &decoded, &error));
    BOOST_CHECK_EQUAL(decoded, 0x0102030405060708ULL);
}

BOOST_AUTO_TEST_CASE(malformed_value_length_and_zero_recordid_fail_closed)
{
    BlockIndexId decoded = 0;
    std::string error;
    BOOST_CHECK(!DecodeBlockIndexRecordIdValue("1234567", 7, &decoded, &error));
    BOOST_CHECK(!error.empty());
    error.clear();
    const std::string zeroValue(8, '\0');
    BOOST_CHECK(!DecodeBlockIndexRecordIdValue(zeroValue.data(), zeroValue.size(), &decoded, &error));
    BOOST_CHECK(!error.empty());
}

BOOST_AUTO_TEST_CASE(create_put_lookup_absent_and_duplicate_semantics_work)
{
    boost::filesystem::path dir = UniqueGenDir("basic");
    BlockIndexHashIndex index = CreateHashIndex(dir, 1);
    std::string error;
    BlockIndexId id = BLOCK_INDEX_ID_INVALID;

    BOOST_CHECK_EQUAL(index.Lookup(uint256(999), &id, &error), BLOCK_INDEX_HASH_LOOKUP_NOT_FOUND);
    BOOST_CHECK(error.empty());

    BOOST_REQUIRE(index.Put(uint256(10), 7, &error));
    BOOST_CHECK_EQUAL(index.Lookup(uint256(10), &id, &error), BLOCK_INDEX_HASH_LOOKUP_FOUND);
    BOOST_CHECK_EQUAL(id, 7ULL);

    error.clear();
    BOOST_REQUIRE(index.Put(uint256(10), 7, &error));
    BOOST_CHECK(error.empty());

    error.clear();
    BOOST_CHECK(!index.Put(uint256(10), 8, &error));
    BOOST_CHECK(!error.empty());
}

BOOST_AUTO_TEST_CASE(schema_and_generation_mismatch_fail_closed)
{
    boost::filesystem::path dir = UniqueGenDir("mismatch");
    FixedBlockIndexStore store = CreateStore(dir, 1);
    BlockIndexHashIndex index = CreateHashIndex(dir, 1);
    std::string error;
    BlockIndexId id = BLOCK_INDEX_ID_INVALID;
    BlockIndexRecord rec = MakeRecord(1, 0, false);
    BOOST_REQUIRE(store.Append(rec, &id, &error));
    BOOST_REQUIRE(index.Put(rec.hash, id, &error));
    FixedBlockIndexManifest manifest = store.GetManifest();
    manifest.state = BLOCK_INDEX_MANIFEST_COMPLETE;
    manifest.recordCount = 1;
    manifest.committedTipId = id;
    manifest.committedTipHeight = rec.height;
    manifest.committedTipHash = rec.hash;
    BOOST_REQUIRE(store.WriteManifest(manifest, &error));

    index.Close();
    index = BlockIndexHashIndex();
    BOOST_REQUIRE(leveldb::DestroyDB((dir / BLOCK_INDEX_HASHINDEX_DIR_NAME).string(), leveldb::Options()).ok());
    leveldb::Options options;
    options.create_if_missing = true;
    leveldb::DB* db = NULL;
    BOOST_REQUIRE(leveldb::DB::Open(options, (dir / BLOCK_INDEX_HASHINDEX_DIR_NAME).string(), &db).ok());
    delete db;

    BlockIndexHashIndex reopened;
    error.clear();
    BOOST_CHECK(!BlockIndexHashIndex::Open(dir.string(), 1, &reopened, &error));
    BOOST_CHECK(!error.empty());

    BOOST_REQUIRE(leveldb::DestroyDB((dir / BLOCK_INDEX_HASHINDEX_DIR_NAME).string(), leveldb::Options()).ok());
    index = CreateHashIndex(dir, 2);
    error.clear();
    FixedBlockIndexShadowLookup lookup;
    BOOST_CHECK(!FixedBlockIndexShadowLookup::Open(dir.string(), &lookup, &error));
    BOOST_CHECK(!error.empty());
}

BOOST_AUTO_TEST_CASE(end_to_end_lookup_absent_uncommitted_tail_hash_mismatch_and_crc_corruption_fail_closed)
{
    boost::filesystem::path dir = UniqueGenDir("e2e-neg");
    FixedBlockIndexStore store = CreateStore(dir, 1);
    BlockIndexHashIndex index = CreateHashIndex(dir, 1);
    std::string error;
    BlockIndexId id1 = BLOCK_INDEX_ID_INVALID;
    BlockIndexId id2 = BLOCK_INDEX_ID_INVALID;
    BlockIndexRecord rec1 = MakeRecord(1, 0, false);
    BlockIndexRecord rec2 = MakeRecord(2, 1, true);
    BOOST_REQUIRE(store.Append(rec1, &id1, &error));
    BOOST_REQUIRE(store.Append(rec2, &id2, &error));
    BOOST_REQUIRE(index.Put(rec1.hash, id1, &error));
    BOOST_REQUIRE(index.Put(rec2.hash, id2, &error));
    FixedBlockIndexManifest manifest = store.GetManifest();
    manifest.state = BLOCK_INDEX_MANIFEST_COMPLETE;
    manifest.recordCount = 1;
    manifest.committedTipId = id1;
    manifest.committedTipHeight = rec1.height;
    manifest.committedTipHash = rec1.hash;
    BOOST_REQUIRE(store.WriteManifest(manifest, &error));
    index.Close();
    index = BlockIndexHashIndex();

    FixedBlockIndexShadowLookup lookup = OpenShadowLookup(dir);
    BlockIndexRecord out;
    BlockIndexId id = BLOCK_INDEX_ID_INVALID;
    BOOST_CHECK_EQUAL(lookup.LookupByHash(uint256(123456), &out, &id, &error), BLOCK_INDEX_HASH_LOOKUP_NOT_FOUND);
    BOOST_CHECK(error.empty());

    error.clear();
    BOOST_CHECK_EQUAL(lookup.LookupByHash(rec2.hash, &out, &id, &error), BLOCK_INDEX_HASH_LOOKUP_ERROR);
    BOOST_CHECK(!error.empty());

    dir = UniqueGenDir("hash-mismatch");
    store = CreateStore(dir, 1);
    index = CreateHashIndex(dir, 1);
    BOOST_REQUIRE(store.Append(rec1, &id1, &error));
    BOOST_REQUIRE(index.Put(uint256(9999), id1, &error));
    manifest = store.GetManifest();
    manifest.state = BLOCK_INDEX_MANIFEST_COMPLETE;
    manifest.recordCount = 1;
    manifest.committedTipId = id1;
    manifest.committedTipHeight = rec1.height;
    manifest.committedTipHash = rec1.hash;
    BOOST_REQUIRE(store.WriteManifest(manifest, &error));
    index.Close();
    index = BlockIndexHashIndex();
    lookup = OpenShadowLookup(dir);
    error.clear();
    BOOST_CHECK_EQUAL(lookup.LookupByHash(uint256(9999), &out, &id, &error), BLOCK_INDEX_HASH_LOOKUP_ERROR);
    BOOST_CHECK(!error.empty());

    dir = UniqueGenDir("crc");
    store = CreateStore(dir, 1);
    index = CreateHashIndex(dir, 1);
    BOOST_REQUIRE(store.Append(rec1, &id1, &error));
    BOOST_REQUIRE(index.Put(rec1.hash, id1, &error));
    manifest = store.GetManifest();
    manifest.state = BLOCK_INDEX_MANIFEST_COMPLETE;
    manifest.recordCount = 1;
    manifest.committedTipId = id1;
    manifest.committedTipHeight = rec1.height;
    manifest.committedTipHash = rec1.hash;
    BOOST_REQUIRE(store.WriteManifest(manifest, &error));
    index.Close();
    index = BlockIndexHashIndex();
    FlipOneByte(dir / BLOCK_INDEX_RECORDS_FILE_NAME, BLOCK_INDEX_RECORDS_HEADER_SIZE_V1 + 17);
    lookup = OpenShadowLookup(dir);
    error.clear();
    BOOST_CHECK_EQUAL(lookup.LookupByHash(rec1.hash, &out, &id, &error), BLOCK_INDEX_HASH_LOOKUP_ERROR);
    BOOST_CHECK(!error.empty());
}

BOOST_AUTO_TEST_CASE(real_legacy_to_hashindex_to_record_shadow_lookup_matches_admitted_fields_and_reopens)
{
    boost::filesystem::path dir = UniqueGenDir("legacy-diff");
    FixedBlockIndexStore store = CreateStore(dir, 1);
    std::string error;
    LegacyBlockIndexAccessor accessor;

    std::vector<const CBlockIndex*> chosen;
    {
        LOCK(cs_main);
        BOOST_REQUIRE(pindexBest != NULL);
        chosen.push_back(pindexGenesisBlock);
        chosen.push_back(FindBlockByHeight(std::min(1, pindexBest->nHeight)));
        chosen.push_back(FindBlockByHeight(std::max(0, pindexBest->nHeight / 2)));
        chosen.push_back(FindBlockByHeight(std::max(0, pindexBest->nHeight - 1)));
        chosen.push_back(pindexBest);
    }

    std::vector<const CBlockIndex*> uniqueChosen;
    std::set<std::string> seenHashes;
    for (size_t i = 0; i < chosen.size(); ++i)
    {
        BOOST_REQUIRE(chosen[i] != NULL);
        const std::string hash = chosen[i]->GetBlockHash().ToString();
        if (seenHashes.insert(hash).second)
            uniqueChosen.push_back(chosen[i]);
    }

    std::vector<uint256> hashes;
    BlockIndexId lastId = BLOCK_INDEX_ID_INVALID;
    {
        BlockIndexHashIndex index = CreateHashIndex(dir, 1);
        for (size_t i = 0; i < uniqueChosen.size(); ++i)
        {
            BlockIndexRecord rec = RecordFromIndex(uniqueChosen[i]);
            BlockIndexId id = BLOCK_INDEX_ID_INVALID;
            BOOST_REQUIRE(store.Append(rec, &id, &error));
            BOOST_REQUIRE(index.Put(rec.hash, id, &error));
            hashes.push_back(rec.hash);
            lastId = id;
        }

        FixedBlockIndexManifest manifest = store.GetManifest();
        manifest.state = BLOCK_INDEX_MANIFEST_COMPLETE;
        manifest.recordCount = lastId;
        manifest.committedTipId = lastId;
        manifest.committedTipHeight = uniqueChosen.back()->nHeight;
        manifest.committedTipHash = hashes.back();
        BOOST_REQUIRE(store.WriteManifest(manifest, &error));
    }

    {
        FixedBlockIndexShadowLookup lookup = OpenShadowLookup(dir);
        for (size_t i = 0; i < hashes.size(); ++i)
        {
            BlockIndexRecord out;
            BlockIndexId id = BLOCK_INDEX_ID_INVALID;
            error.clear();
            BOOST_CHECK_EQUAL(lookup.LookupByHash(hashes[i], &out, &id, &error), BLOCK_INDEX_HASH_LOOKUP_FOUND);
            BOOST_CHECK(error.empty());
            ExpectRecordsEqual(out, RecordFromIndex(uniqueChosen[i]));

            BlockIndexSnapshot snap;
            {
                LOCK(cs_main);
                snap = accessor.LookupByHash(hashes[i]);
                BOOST_REQUIRE(snap.found);
            }
            BOOST_CHECK_EQUAL(out.hash.ToString(), snap.hash.ToString());
            BOOST_CHECK_EQUAL(out.hashPrev.ToString(), snap.hashPrev.ToString());
            BOOST_CHECK_EQUAL(out.height, snap.height);
            BOOST_CHECK_EQUAL(out.nFile, snap.nFile);
            BOOST_CHECK_EQUAL(out.nBlockPos, snap.nBlockPos);
            BOOST_CHECK_EQUAL(out.nFlags, snap.nFlags);
            BOOST_CHECK_EQUAL(out.nVersion, snap.nVersion);
            BOOST_CHECK_EQUAL(out.nTime, snap.nTime);
            BOOST_CHECK_EQUAL(out.nBits, snap.nBits);
            BOOST_CHECK_EQUAL(out.nNonce, snap.nNonce);
            BOOST_CHECK_EQUAL(out.nMint, snap.nMint);
            BOOST_CHECK_EQUAL(out.nMoneySupply, snap.nMoneySupply);
            BOOST_CHECK_EQUAL(out.nStakeModifier, snap.nStakeModifier);
            BOOST_CHECK_EQUAL(out.prevoutStake.ToString(), snap.prevoutStake.ToString());
            BOOST_CHECK_EQUAL(out.nStakeTime, snap.nStakeTime);
            BOOST_CHECK_EQUAL(out.hashProof.ToString(), snap.hashProof.ToString());
        }
    }

    FixedBlockIndexShadowLookup lookup = OpenShadowLookup(dir);
    for (size_t i = 0; i < hashes.size(); ++i)
    {
        BlockIndexRecord out;
        BlockIndexId id = BLOCK_INDEX_ID_INVALID;
        error.clear();
        BOOST_CHECK_EQUAL(lookup.LookupByHash(hashes[i], &out, &id, &error), BLOCK_INDEX_HASH_LOOKUP_FOUND);
        BOOST_CHECK(error.empty());
    }
}

BOOST_AUTO_TEST_SUITE_END()
