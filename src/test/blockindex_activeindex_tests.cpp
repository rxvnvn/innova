#include <boost/test/unit_test.hpp>
#include "../blockindex_activeindex.h"
#include "../fixed_blockindex_store.h"
#include "../blockindex_hashindex.h"
#include "../blockindex_accessor.h"
#include "../main.h"
#include "../util.h"

#include <boost/filesystem.hpp>
#include <leveldb/db.h>

#include <fstream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace {

static boost::filesystem::path UniqueGenDir(const std::string& tag)
{
    boost::filesystem::path model =
        boost::filesystem::path("innova-blockindex-activeindex-") /
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

static BlockIndexActiveIndex CreateActive(const boost::filesystem::path& dir, uint64_t generation)
{
    std::string error;
    BlockIndexActiveIndex act;
    BOOST_REQUIRE(BlockIndexActiveIndex::Create(dir.string(), generation, &act, &error));
    BOOST_REQUIRE_MESSAGE(error.empty(), error);
    return act;
}

static BlockIndexHashIndex CreateHashIndex(const boost::filesystem::path& dir, uint64_t generation)
{
    std::string error;
    BlockIndexHashIndex index;
    BOOST_REQUIRE_MESSAGE(BlockIndexHashIndex::Create(dir.string(), generation, &index, &error), error);
    BOOST_REQUIRE_MESSAGE(error.empty(), error);
    return index;
}

static BlockIndexHashIndex OpenHashIndex(const boost::filesystem::path& dir, uint64_t generation)
{
    std::string error;
    BlockIndexHashIndex index;
    BOOST_REQUIRE_MESSAGE(BlockIndexHashIndex::Open(dir.string(), generation, &index, &error), error);
    BOOST_REQUIRE_MESSAGE(error.empty(), error);
    return index;
}

static FixedBlockIndexShadowActiveLookup OpenShadowActive(const boost::filesystem::path& dir)
{
    std::string error;
    FixedBlockIndexShadowActiveLookup lookup;
    BOOST_REQUIRE_MESSAGE(FixedBlockIndexShadowActiveLookup::Open(dir.string(), &lookup, &error), error);
    BOOST_REQUIRE_MESSAGE(error.empty(), error);
    return lookup;
}

// Finalize a writable store as COMPLETE with the given committed boundary.
static void FinalizeManifest(FixedBlockIndexStore* store, uint64_t recordCount,
                             BlockIndexId tipId, int32_t tipHeight, const uint256& tipHash)
{
    std::string error;
    FixedBlockIndexManifest m = store->GetManifest();
    m.state = BLOCK_INDEX_MANIFEST_COMPLETE;
    m.recordCount = recordCount;
    m.committedTipId = tipId;
    m.committedTipHeight = tipHeight;
    m.committedTipHash = tipHash;
    BOOST_REQUIRE(store->WriteManifest(m, &error));
    BOOST_REQUIRE_MESSAGE(error.empty(), error);
}

static void AppendBlock(FixedBlockIndexStore* store, const BlockIndexRecord& rec,
                        std::vector<std::pair<int32_t, BlockIndexId> >* chain)
{
    std::string error;
    BlockIndexId id = BLOCK_INDEX_ID_INVALID;
    BOOST_REQUIRE(store->Append(rec, &id, &error));
    BOOST_REQUIRE_MESSAGE(error.empty(), error);
    chain->push_back(std::make_pair(rec.height, id));
}

} // namespace

BOOST_AUTO_TEST_SUITE(blockindex_activeindex_tests)

BOOST_AUTO_TEST_CASE(active_entry_codec_is_exact_little_endian_and_rejects_invalid)
{
    std::string value;
    std::string error;
    BOOST_REQUIRE(EncodeBlockIndexActiveEntry(0x0102030405060708ULL, &value, &error));
    BOOST_CHECK_EQUAL((int)value.size(), (int)BLOCK_INDEX_ACTIVE_ENTRY_SIZE_V1);
    BOOST_CHECK_EQUAL((unsigned char)value[0], 0x08);
    BOOST_CHECK_EQUAL((unsigned char)value[7], 0x01);

    BlockIndexId decoded = 0;
    BOOST_REQUIRE(DecodeBlockIndexActiveEntry(value.data(), value.size(), &decoded, &error));
    BOOST_CHECK_EQUAL(decoded, 0x0102030405060708ULL);

    // determinism
    std::string value2;
    BOOST_REQUIRE(EncodeBlockIndexActiveEntry(0x0102030405060708ULL, &value2, &error));
    BOOST_CHECK_EQUAL_COLLECTIONS(value.begin(), value.end(), value2.begin(), value2.end());

    // fail-closed
    error.clear();
    BOOST_CHECK(!DecodeBlockIndexActiveEntry("1234567", 7, &decoded, &error));
    BOOST_CHECK(!error.empty());
    error.clear();
    const std::string zeroValue(8, '\0');
    BOOST_CHECK(!DecodeBlockIndexActiveEntry(zeroValue.data(), zeroValue.size(), &decoded, &error));
    BOOST_CHECK(!error.empty());
    error.clear();
    BOOST_CHECK(!EncodeBlockIndexActiveEntry(BLOCK_INDEX_ID_INVALID, &value, &error));
    BOOST_CHECK(!error.empty());
}

BOOST_AUTO_TEST_CASE(create_writes_exact_versioned_dense_header_and_binds_generation)
{
    boost::filesystem::path dir = UniqueGenDir("hdrcodec");
    BlockIndexActiveIndex act = CreateActive(dir, 5);
    std::string error;
    BOOST_CHECK_EQUAL(act.PhysicalHeight(), (int64_t)-1);
    BOOST_CHECK_EQUAL(act.Generation(), 5ULL);
    BOOST_CHECK_EQUAL(act.IsLogicalReadOnly(), false);
    BOOST_CHECK(act.IsOpen());

    boost::filesystem::path file = dir / BLOCK_INDEX_ACTIVE_FILE_NAME;
    BOOST_CHECK_EQUAL(boost::filesystem::file_size(file),
                      (uint64_t)BLOCK_INDEX_ACTIVE_HEADER_SIZE_V1);
    std::string bytes = ReadWholeFile(file);
    BOOST_CHECK_EQUAL((int)bytes.size(), (int)BLOCK_INDEX_ACTIVE_HEADER_SIZE_V1);
    BOOST_CHECK_EQUAL(std::string(bytes.data(), 6), std::string("INNBAC"));
    BOOST_CHECK_EQUAL((unsigned char)bytes[6], 'T');
    BOOST_CHECK_EQUAL((unsigned char)bytes[7], '1');

    // reopen read-only with the same generation succeeds; wrong generation fails.
    BlockIndexActiveIndex reopened;
    BOOST_REQUIRE(BlockIndexActiveIndex::Open(dir.string(), 5, &reopened, &error));
    BOOST_CHECK(error.empty());
    BOOST_CHECK_EQUAL(reopened.PhysicalHeight(), (int64_t)-1);
    BOOST_CHECK_EQUAL(reopened.IsLogicalReadOnly(), true);

    error.clear();
    BlockIndexActiveIndex bad;
    BOOST_CHECK(!BlockIndexActiveIndex::Open(dir.string(), 6, &bad, &error));
    BOOST_CHECK(!error.empty());
}

BOOST_AUTO_TEST_CASE(append_is_dense_contiguous_and_bounds_enforced)
{
    boost::filesystem::path dir = UniqueGenDir("dense");
    BlockIndexActiveIndex act = CreateActive(dir, 1);
    std::string error;

    BOOST_REQUIRE(act.Append(1, 0, &error));
    BOOST_CHECK(error.empty());
    BOOST_REQUIRE(act.Append(2, 1, &error));
    BOOST_CHECK(error.empty());
    BOOST_CHECK_EQUAL(act.PhysicalHeight(), (int64_t)1);

    // non-contiguous append rejected (no sparse mappings)
    error.clear();
    BOOST_CHECK(!act.Append(3, 3, &error));
    BOOST_CHECK(!error.empty());

    // RecordId 0 rejected
    error.clear();
    BOOST_CHECK(!act.Append(BLOCK_INDEX_ID_INVALID, 2, &error));
    BOOST_CHECK(!error.empty());

    // correct next height accepted
    BOOST_REQUIRE(act.Append(3, 2, &error));
    BOOST_CHECK(error.empty());
    BOOST_CHECK_EQUAL(boost::filesystem::file_size(dir / BLOCK_INDEX_ACTIVE_FILE_NAME),
                      (uint64_t)BLOCK_INDEX_ACTIVE_HEADER_SIZE_V1 + 3 * BLOCK_INDEX_ACTIVE_ENTRY_SIZE_V1);

    BlockIndexId id = BLOCK_INDEX_ID_INVALID;
    BOOST_CHECK(act.ReadEntry(0, &id, &error));
    BOOST_CHECK_EQUAL(id, 1ULL);
    BOOST_CHECK(act.ReadEntry(1, &id, &error));
    BOOST_CHECK_EQUAL(id, 2ULL);
    BOOST_CHECK(act.ReadEntry(2, &id, &error));
    BOOST_CHECK_EQUAL(id, 3ULL);
    error.clear();
    BOOST_CHECK(!act.ReadEntry(3, &id, &error));
    BOOST_CHECK(!error.empty());
}

BOOST_AUTO_TEST_CASE(truncate_and_reappend_isolates_only_active_tail)
{
    boost::filesystem::path dir = UniqueGenDir("reorg-tail");
    BlockIndexActiveIndex act = CreateActive(dir, 1);
    std::string error;
    BOOST_REQUIRE(act.Append(1, 0, &error));
    BOOST_REQUIRE(act.Append(2, 1, &error));
    BOOST_REQUIRE(act.Append(3, 2, &error));
    BOOST_REQUIRE(act.Append(4, 3, &error));
    BOOST_REQUIRE(act.Append(5, 4, &error));
    BOOST_REQUIRE(act.Append(6, 5, &error));
    BOOST_CHECK_EQUAL(act.PhysicalHeight(), (int64_t)5);

    // reorg at height 2: keep entries 0..2, drop the rest.
    BOOST_REQUIRE(act.TruncateTo(2, &error));
    BOOST_CHECK(error.empty());
    BOOST_CHECK_EQUAL(act.PhysicalHeight(), (int64_t)2);
    BOOST_CHECK_EQUAL(boost::filesystem::file_size(dir / BLOCK_INDEX_ACTIVE_FILE_NAME),
                      (uint64_t)BLOCK_INDEX_ACTIVE_HEADER_SIZE_V1 + 3 * BLOCK_INDEX_ACTIVE_ENTRY_SIZE_V1);
    BlockIndexId id = BLOCK_INDEX_ID_INVALID;
    BOOST_CHECK(act.ReadEntry(0, &id, &error));
    BOOST_CHECK_EQUAL(id, 1ULL);
    BOOST_CHECK(act.ReadEntry(2, &id, &error));
    BOOST_CHECK_EQUAL(id, 3ULL);
    error.clear();
    BOOST_CHECK(!act.ReadEntry(3, &id, &error));
    BOOST_CHECK(!error.empty());

    // replace the tail with a fresh branch
    BOOST_REQUIRE(act.Append(20, 3, &error));
    BOOST_REQUIRE(act.Append(21, 4, &error));
    BOOST_CHECK_EQUAL(act.PhysicalHeight(), (int64_t)4);
    BOOST_CHECK(act.ReadEntry(0, &id, &error));
    BOOST_CHECK_EQUAL(id, 1ULL);
    BOOST_CHECK(act.ReadEntry(3, &id, &error));
    BOOST_CHECK_EQUAL(id, 20ULL);
    BOOST_CHECK(act.ReadEntry(4, &id, &error));
    BOOST_CHECK_EQUAL(id, 21ULL);

    // truncate to empty
    BOOST_REQUIRE(act.TruncateTo(-1, &error));
    BOOST_CHECK_EQUAL(act.PhysicalHeight(), (int64_t)-1);
    BOOST_CHECK_EQUAL(boost::filesystem::file_size(dir / BLOCK_INDEX_ACTIVE_FILE_NAME),
                      (uint64_t)BLOCK_INDEX_ACTIVE_HEADER_SIZE_V1);
    error.clear();
    BOOST_CHECK(!act.ReadEntry(0, &id, &error));

    // truncate above current physical height rejected
    error.clear();
    BOOST_CHECK(!act.TruncateTo(5, &error));
    BOOST_CHECK(!error.empty());
}

BOOST_AUTO_TEST_CASE(open_is_read_only_and_rejects_corrupt_header_dense_violation_and_wrong_generation)
{
    boost::filesystem::path dir = UniqueGenDir("open-neg");
    BlockIndexActiveIndex act = CreateActive(dir, 1);
    std::string error;
    BlockIndexActiveIndex reopened;

    // read-only reopen
    BOOST_REQUIRE(BlockIndexActiveIndex::Open(dir.string(), 1, &reopened, &error));
    BOOST_CHECK(reopened.IsLogicalReadOnly());
    BOOST_CHECK(!reopened.Append(9, 0, &error));
    BOOST_CHECK(!error.empty());

    // corrupt magic
    dir = UniqueGenDir("open-corrupt");
    CreateActive(dir, 1);
    FlipOneByte(dir / BLOCK_INDEX_ACTIVE_FILE_NAME, 0);
    error.clear();
    BOOST_CHECK(!BlockIndexActiveIndex::Open(dir.string(), 1, &reopened, &error));
    BOOST_CHECK(!error.empty());

    // dense-multiple violation (append a partial trailing byte)
    dir = UniqueGenDir("open-dense");
    act = CreateActive(dir, 1);
    BOOST_REQUIRE(act.Append(1, 0, &error));
    std::string bytes = ReadWholeFile(dir / BLOCK_INDEX_ACTIVE_FILE_NAME);
    bytes.push_back('\x00');
    WriteWholeFile(dir / BLOCK_INDEX_ACTIVE_FILE_NAME, bytes);
    error.clear();
    BOOST_CHECK(!BlockIndexActiveIndex::Open(dir.string(), 1, &reopened, &error));
    BOOST_CHECK(!error.empty());

    // wrong generation
    dir = UniqueGenDir("open-gen");
    CreateActive(dir, 2);
    error.clear();
    BOOST_CHECK(!BlockIndexActiveIndex::Open(dir.string(), 1, &reopened, &error));
    BOOST_CHECK(!error.empty());
}

BOOST_AUTO_TEST_CASE(shadow_lookup_returns_records_and_enforces_committed_boundary)
{
    boost::filesystem::path dir = UniqueGenDir("lookup");
    FixedBlockIndexStore store = CreateStore(dir, 1);
    std::string error;
    BlockIndexActiveIndex act = CreateActive(dir, 1);

    std::vector<std::pair<int32_t, BlockIndexId> > chain;
    BlockIndexRecord r0 = MakeRecord(1000, 0, false);
    BlockIndexRecord r1 = MakeRecord(1001, 1, false);
    BlockIndexRecord r2 = MakeRecord(1002, 2, false);
    AppendBlock(&store, r0, &chain);
    AppendBlock(&store, r1, &chain);
    AppendBlock(&store, r2, &chain);
    // map active heights to the records
    BOOST_REQUIRE(act.Append(chain[0].second, 0, &error));
    BOOST_REQUIRE(act.Append(chain[1].second, 1, &error));
    BOOST_REQUIRE(act.Append(chain[2].second, 2, &error));
    FinalizeManifest(&store, 3, chain[2].second, 2, r2.hash);
    act = BlockIndexActiveIndex(); // drop writable handle, keyed on disk state

    FixedBlockIndexShadowActiveLookup lookup = OpenShadowActive(dir);
    BlockIndexRecord out;
    BlockIndexId id = BLOCK_INDEX_ID_INVALID;
    BOOST_CHECK_EQUAL(lookup.LookupByHeight(0, &out, &id, &error), BLOCK_INDEX_ACTIVE_LOOKUP_FOUND);
    BOOST_CHECK(error.empty());
    ExpectRecordsEqual(out, r0);
    BOOST_CHECK_EQUAL(id, chain[0].second);
    error.clear();
    BOOST_CHECK_EQUAL(lookup.LookupByHeight(1, &out, &id, &error), BLOCK_INDEX_ACTIVE_LOOKUP_FOUND);
    ExpectRecordsEqual(out, r1);
    error.clear();
    BOOST_CHECK_EQUAL(lookup.LookupByHeight(2, &out, &id, &error), BLOCK_INDEX_ACTIVE_LOOKUP_FOUND);
    ExpectRecordsEqual(out, r2);

    // above committed active tip -> NOT_FOUND
    error.clear();
    BOOST_CHECK_EQUAL(lookup.LookupByHeight(3, &out, &id, &error), BLOCK_INDEX_ACTIVE_LOOKUP_NOT_FOUND);
    BOOST_CHECK(error.empty());
    error.clear();
    BOOST_CHECK_EQUAL(lookup.LookupByHeight(100, &out, &id, &error), BLOCK_INDEX_ACTIVE_LOOKUP_NOT_FOUND);
    BOOST_CHECK(error.empty());

    // negative height -> ERROR
    error.clear();
    BOOST_CHECK_EQUAL(lookup.LookupByHeight(-1, &out, &id, &error), BLOCK_INDEX_ACTIVE_LOOKUP_ERROR);
    BOOST_CHECK(!error.empty());
}

BOOST_AUTO_TEST_CASE(shadow_lookup_cross_checks_record_height_against_requested)
{
    boost::filesystem::path dir = UniqueGenDir("crosscheck");
    FixedBlockIndexStore store = CreateStore(dir, 1);
    std::string error;
    BlockIndexActiveIndex act = CreateActive(dir, 1);

    // store record for height 1 but mis-index it at active height 0
    BlockIndexRecord r1 = MakeRecord(5000, 1, false);
    BlockIndexId id = BLOCK_INDEX_ID_INVALID;
    BOOST_REQUIRE(store.Append(r1, &id, &error));
    BOOST_REQUIRE(act.Append(id, 0, &error));
    FinalizeManifest(&store, 1, id, 0, r1.hash);
    act = BlockIndexActiveIndex();

    FixedBlockIndexShadowActiveLookup lookup = OpenShadowActive(dir);
    error.clear();
    BlockIndexRecord out;
    BlockIndexId outId = BLOCK_INDEX_ID_INVALID;
    BOOST_CHECK_EQUAL(lookup.LookupByHeight(0, &out, &outId, &error), BLOCK_INDEX_ACTIVE_LOOKUP_ERROR);
    BOOST_CHECK(!error.empty());
}

BOOST_AUTO_TEST_CASE(shadow_open_fails_closed_on_sparse_zero_and_over_boundary_entries)
{
    // sparse zero active entry within committed region
    boost::filesystem::path dir = UniqueGenDir("sparse-zero");
    FixedBlockIndexStore store = CreateStore(dir, 1);
    std::string error;
    BlockIndexActiveIndex act = CreateActive(dir, 1);
    BlockIndexRecord r0 = MakeRecord(6000, 0, false);
    BlockIndexRecord r1 = MakeRecord(6001, 1, false);
    BlockIndexId id0 = BLOCK_INDEX_ID_INVALID;
    BlockIndexId id1 = BLOCK_INDEX_ID_INVALID;
    BOOST_REQUIRE(store.Append(r0, &id0, &error));
    BOOST_REQUIRE(store.Append(r1, &id1, &error));
    BOOST_REQUIRE(act.Append(id0, 0, &error));
    BOOST_REQUIRE(act.Append(id1, 1, &error));
    FinalizeManifest(&store, 2, id0, 1, r0.hash);

    // corrupt the entry at height 1 to a zero RecordId
    std::string bytes = ReadWholeFile(dir / BLOCK_INDEX_ACTIVE_FILE_NAME);
    BOOST_REQUIRE_LE(BLOCK_INDEX_ACTIVE_HEADER_SIZE_V1 + 1 * BLOCK_INDEX_ACTIVE_ENTRY_SIZE_V1 + 8,
                     (size_t)bytes.size());
    for (size_t i = 0; i < 8; ++i)
        bytes[BLOCK_INDEX_ACTIVE_HEADER_SIZE_V1 + 1 * BLOCK_INDEX_ACTIVE_ENTRY_SIZE_V1 + i] = '\0';
    WriteWholeFile(dir / BLOCK_INDEX_ACTIVE_FILE_NAME, bytes);
    act = BlockIndexActiveIndex();

    error.clear();
    FixedBlockIndexShadowActiveLookup lookup;
    BOOST_CHECK(!FixedBlockIndexShadowActiveLookup::Open(dir.string(), &lookup, &error));
    BOOST_CHECK(!error.empty());

    // active entry exceeding the committed record count
    dir = UniqueGenDir("overcount");
    FixedBlockIndexStore store2 = CreateStore(dir, 1);
    BlockIndexActiveIndex act2 = CreateActive(dir, 1);
    BOOST_REQUIRE(store2.Append(r0, &id0, &error));
    BOOST_REQUIRE(store2.Append(r1, &id1, &error));
    BOOST_REQUIRE(act2.Append(id0, 0, &error));
    BOOST_REQUIRE(act2.Append(id1, 1, &error));
    // commit only one record but keep active height 1 -> id1 (recordCount 1)
    FinalizeManifest(&store2, 1, id0, 1, r0.hash);
    act2 = BlockIndexActiveIndex();

    error.clear();
    BOOST_CHECK(!FixedBlockIndexShadowActiveLookup::Open(dir.string(), &lookup, &error));
    BOOST_CHECK(!error.empty());
}

BOOST_AUTO_TEST_CASE(shadow_open_enforces_manifest_generation_and_committed_tip_coherence)
{
    // generation mismatch: store gen 1, active.dat gen 2
    boost::filesystem::path dir = UniqueGenDir("gen-mismatch");
    FixedBlockIndexStore store = CreateStore(dir, 1);
    std::string error;
    BlockIndexActiveIndex act = CreateActive(dir, 2);
    BlockIndexRecord r0 = MakeRecord(7000, 0, false);
    BlockIndexId id0 = BLOCK_INDEX_ID_INVALID;
    BOOST_REQUIRE(store.Append(r0, &id0, &error));
    FinalizeManifest(&store, 1, id0, 0, r0.hash);
    act = BlockIndexActiveIndex();
    error.clear();
    FixedBlockIndexShadowActiveLookup lookup;
    BOOST_CHECK(!FixedBlockIndexShadowActiveLookup::Open(dir.string(), &lookup, &error));
    BOOST_CHECK(!error.empty());

    // committed-tip incoherence: entry at tip height != MANIFEST.committedTipId
    dir = UniqueGenDir("tip-coherence");
    FixedBlockIndexStore store2 = CreateStore(dir, 1);
    BlockIndexActiveIndex act2 = CreateActive(dir, 1);
    BlockIndexRecord rA = MakeRecord(7100, 0, false);
    BlockIndexRecord rB = MakeRecord(7101, 1, false);
    BlockIndexId idA = BLOCK_INDEX_ID_INVALID;
    BlockIndexId idB = BLOCK_INDEX_ID_INVALID;
    BOOST_REQUIRE(store2.Append(rA, &idA, &error));
    BOOST_REQUIRE(store2.Append(rB, &idB, &error));
    BOOST_REQUIRE(act2.Append(idA, 0, &error));
    BOOST_REQUIRE(act2.Append(idB, 1, &error));
    // manifest says tip is height 1 -> idA (wrong: active has idB at height 1)
    FinalizeManifest(&store2, 2, idA, 1, rB.hash);
    act2 = BlockIndexActiveIndex();
    error.clear();
    BOOST_CHECK(!FixedBlockIndexShadowActiveLookup::Open(dir.string(), &lookup, &error));
    BOOST_CHECK(!error.empty());

    // active.dat truncated below committed tip
    dir = UniqueGenDir("truncated");
    FixedBlockIndexStore store3 = CreateStore(dir, 1);
    BlockIndexActiveIndex act3 = CreateActive(dir, 1);
    BlockIndexRecord rT = MakeRecord(7200, 0, false);
    BlockIndexId idT = BLOCK_INDEX_ID_INVALID;
    BOOST_REQUIRE(store3.Append(rT, &idT, &error));
    BOOST_REQUIRE(act3.Append(idT, 0, &error));
    FinalizeManifest(&store3, 1, idT, 2, rT.hash); // committed tip height 2 but active only has 0
    act3 = BlockIndexActiveIndex();
    error.clear();
    BOOST_CHECK(!FixedBlockIndexShadowActiveLookup::Open(dir.string(), &lookup, &error));
    BOOST_CHECK(!error.empty());
}

BOOST_AUTO_TEST_CASE(side_chain_records_absent_from_active_but_preserved_and_reorg_isolates_tail)
{
    boost::filesystem::path dir = UniqueGenDir("reorg");
    FixedBlockIndexStore store = CreateStore(dir, 1);
    std::string error;
    BlockIndexActiveIndex act = CreateActive(dir, 1);

    // main chain A, heights 0..6
    std::vector<std::pair<int32_t, BlockIndexId> > chainA;
    for (int h = 0; h <= 6; ++h)
        AppendBlock(&store, MakeRecord(1000 + h, h, false), &chainA);
    // side chain B shares fork at height 2 (A_2), heights 3..5
    std::vector<std::pair<int32_t, BlockIndexId> > chainB;
    for (int h = 3; h <= 5; ++h)
        AppendBlock(&store, MakeRecord(2000 + h, h, false), &chainB);

    // active index maps ALL of A (dense 0..6)
    for (size_t i = 0; i < chainA.size(); ++i)
        BOOST_REQUIRE(act.Append(chainA[i].second, chainA[i].first, &error));

    // preservation index: both chains recorded in hashindex
    BlockIndexHashIndex hashIndex = CreateHashIndex(dir, 1);
    for (size_t i = 0; i < chainA.size(); ++i)
        BOOST_REQUIRE(hashIndex.Put(MakeRecord(1000 + chainA[i].first, chainA[i].first, false).hash, chainA[i].second, &error));
    for (size_t i = 0; i < chainB.size(); ++i)
        BOOST_REQUIRE(hashIndex.Put(MakeRecord(2000 + chainB[i].first, chainB[i].first, false).hash, chainB[i].second, &error));

    const BlockIndexId tipA = chainA.back().second;
    const uint256 tipHashA = MakeRecord(1000 + 6, 6, false).hash;
    FinalizeManifest(&store, chainA.size() + chainB.size(), tipA, 6, tipHashA);
    hashIndex.Close();
    act = BlockIndexActiveIndex();

    // ---- initial active chain: every height serves the A record ----
    {
        FixedBlockIndexShadowActiveLookup lookup = OpenShadowActive(dir);
        BlockIndexRecord out;
        BlockIndexId id = BLOCK_INDEX_ID_INVALID;
        for (int h = 0; h <= 6; ++h)
        {
            error.clear();
            BOOST_CHECK_EQUAL(lookup.LookupByHeight(h, &out, &id, &error), BLOCK_INDEX_ACTIVE_LOOKUP_FOUND);
            BOOST_CHECK(error.empty());
            ExpectRecordsEqual(out, MakeRecord(1000 + h, h, false));
        }
        // the side chain is present in records.dat/hashindex but NOT active:
        error.clear();
        BOOST_CHECK_EQUAL(lookup.LookupByHeight(3, &out, &id, &error), BLOCK_INDEX_ACTIVE_LOOKUP_FOUND);
        BOOST_CHECK_EQUAL(id, chainA[3].second); // A, not B
    }

    // side-chain records remain readable through records.dat + hashindex (preserved)
    {
        FixedBlockIndexStore storeRO;
        FixedBlockIndexOpenOptions opts;
        opts.requireCompleteManifest = true;
        BOOST_REQUIRE(FixedBlockIndexStore::OpenReadOnly(dir.string(), opts, &storeRO, &error));
        BlockIndexRecord outTmp;
        for (size_t i = 0; i < chainB.size(); ++i)
        {
            error.clear();
            BOOST_REQUIRE(storeRO.Read(chainB[i].second, &outTmp, &error));
            BOOST_CHECK_EQUAL(outTmp.height, chainB[i].first);
        }
        BlockIndexHashIndex hi = OpenHashIndex(dir, 1);
        for (size_t i = 0; i < chainB.size(); ++i)
        {
            error.clear();
            BlockIndexId hid = BLOCK_INDEX_ID_INVALID;
            BOOST_CHECK_EQUAL(hi.Lookup(MakeRecord(2000 + chainB[i].first, chainB[i].first, false).hash, &hid, &error), BLOCK_INDEX_HASH_LOOKUP_FOUND);
            BOOST_CHECK_EQUAL(hid, chainB[i].second);
        }
        hi.Close();
    }

    // ---- reorg to B: truncate to fork height 2, replace tail 3..5 with B ----
    {
        act = BlockIndexActiveIndex();
        BOOST_REQUIRE(BlockIndexActiveIndex::OpenWritable(dir.string(), 1, &act, &error));
        BOOST_REQUIRE(act.TruncateTo(2, &error));
        for (size_t i = 0; i < chainB.size(); ++i)
            BOOST_REQUIRE(act.Append(chainB[i].second, chainB[i].first, &error));
        FinalizeManifest(&store, chainA.size() + chainB.size(), chainB.back().second, 5, MakeRecord(2005, 5, false).hash);
        act = BlockIndexActiveIndex();
    }

    {
        FixedBlockIndexShadowActiveLookup lookup = OpenShadowActive(dir);
        BlockIndexRecord out;
        BlockIndexId id = BLOCK_INDEX_ID_INVALID;
        // common ancestor region still the A chain
        for (int h = 0; h <= 2; ++h)
        {
            error.clear();
            BOOST_CHECK_EQUAL(lookup.LookupByHeight(h, &out, &id, &error), BLOCK_INDEX_ACTIVE_LOOKUP_FOUND);
            ExpectRecordsEqual(out, MakeRecord(1000 + h, h, false));
        }
        // new active tail now serves B
        for (int h = 3; h <= 5; ++h)
        {
            error.clear();
            BOOST_CHECK_EQUAL(lookup.LookupByHeight(h, &out, &id, &error), BLOCK_INDEX_ACTIVE_LOOKUP_FOUND);
            ExpectRecordsEqual(out, MakeRecord(2000 + h, h, false));
        }
        // A_3..A_6 are now orphaned: above new committed tip 5 -> NOT_FOUND via active,
        // but still preserved in records.dat / hashindex.
        error.clear();
        BOOST_CHECK_EQUAL(lookup.LookupByHeight(6, &out, &id, &error), BLOCK_INDEX_ACTIVE_LOOKUP_NOT_FOUND);
        BOOST_CHECK(error.empty());
    }
    {
        BlockIndexHashIndex hi = OpenHashIndex(dir, 1);
        for (size_t i = 3; i <= 6; ++i)
        {
            error.clear();
            BlockIndexId hid = BLOCK_INDEX_ID_INVALID;
            BOOST_CHECK_EQUAL(hi.Lookup(MakeRecord(1000 + i, i, false).hash, &hid, &error), BLOCK_INDEX_HASH_LOOKUP_FOUND);
            BOOST_CHECK_EQUAL(hid, chainA[i].second);
        }
        hi.Close();
    }
}

BOOST_AUTO_TEST_CASE(shadow_lookup_never_serves_uncommitted_active_tail)
{
    boost::filesystem::path dir = UniqueGenDir("uncommitted-tail");
    FixedBlockIndexStore store = CreateStore(dir, 1);
    std::string error;
    BlockIndexActiveIndex act = CreateActive(dir, 1);

    // records for heights 0..3, but commit only 0..2 (recordCount 4 keeps all
    // ids valid; committed active tip is height 2). active.dat physically holds
    // an uncommitted tail entry at height 3.
    std::vector<std::pair<int32_t, BlockIndexId> > chain;
    BlockIndexRecord r[4];
    for (int h = 0; h <= 3; ++h)
    {
        r[h] = MakeRecord(4000 + h, h, false);
        AppendBlock(&store, r[h], &chain);
        BOOST_REQUIRE(act.Append(chain[h].second, h, &error));
    }
    FinalizeManifest(&store, 4, chain[2].second, 2, r[2].hash);
    act = BlockIndexActiveIndex();

    FixedBlockIndexShadowActiveLookup lookup = OpenShadowActive(dir);
    BlockIndexRecord out;
    BlockIndexId id = BLOCK_INDEX_ID_INVALID;
    error.clear();
    BOOST_CHECK_EQUAL(lookup.LookupByHeight(0, &out, &id, &error), BLOCK_INDEX_ACTIVE_LOOKUP_FOUND);
    BOOST_CHECK_EQUAL(lookup.LookupByHeight(2, &out, &id, &error), BLOCK_INDEX_ACTIVE_LOOKUP_FOUND);
    ExpectRecordsEqual(out, r[2]);
    // height 3 is physically present in active.dat but NOT committed: NOT_FOUND.
    error.clear();
    BOOST_CHECK_EQUAL(lookup.LookupByHeight(3, &out, &id, &error), BLOCK_INDEX_ACTIVE_LOOKUP_NOT_FOUND);
    BOOST_CHECK(error.empty());
    error.clear();
    BOOST_CHECK_EQUAL(lookup.LookupByHeight(4, &out, &id, &error), BLOCK_INDEX_ACTIVE_LOOKUP_NOT_FOUND);
    BOOST_CHECK(error.empty());
}

BOOST_AUTO_TEST_CASE(real_legacy_active_chain_differential_matches_direct_legacy)
{
    boost::filesystem::path dir = UniqueGenDir("legacy-diff");
    FixedBlockIndexStore store = CreateStore(dir, 1);
    std::string error;
    BlockIndexActiveIndex act = CreateActive(dir, 1);
    LegacyBlockIndexAccessor accessor;

    // Walk the CURRENT real legacy active chain (order-independent: it is
    // whatever the fixture/prior suites left; deterministic at read time).
    std::vector<std::pair<int32_t, BlockIndexId> > activeChain;
    {
        LOCK(cs_main);
        BOOST_REQUIRE(pindexBest != NULL);
        const int tipHeight = pindexBest->nHeight;
        for (int h = 0; h <= tipHeight; ++h)
        {
            CBlockIndex* pindex = FindBlockByHeight(h);
            BOOST_REQUIRE(pindex != NULL);
            BlockIndexRecord rec = RecordFromIndex(pindex);
            BlockIndexId id = BLOCK_INDEX_ID_INVALID;
            BOOST_REQUIRE(store.Append(rec, &id, &error));
            activeChain.push_back(std::make_pair(h, id));
        }
    }
    for (size_t i = 0; i < activeChain.size(); ++i)
        BOOST_REQUIRE(act.Append(activeChain[i].second, activeChain[i].first, &error));

    uint256 tipHash;
    {
        LOCK(cs_main);
        tipHash = pindexBest->GetBlockHash();
    }
    FinalizeManifest(&store, activeChain.size(), activeChain.back().second,
                     activeChain.back().first, tipHash);
    act = BlockIndexActiveIndex();

    FixedBlockIndexShadowActiveLookup lookup = OpenShadowActive(dir);
    for (size_t i = 0; i < activeChain.size(); ++i)
    {
        const int h = activeChain[i].first;
        CBlockIndex* pindex = NULL;
        {
            LOCK(cs_main);
            pindex = FindBlockByHeight(h);
            BOOST_REQUIRE(pindex != NULL);
        }
        const BlockIndexRecord expected = RecordFromIndex(pindex);
        BlockIndexRecord out;
        BlockIndexId outId = BLOCK_INDEX_ID_INVALID;
        error.clear();
        BOOST_CHECK_EQUAL(lookup.LookupByHeight(h, &out, &outId, &error), BLOCK_INDEX_ACTIVE_LOOKUP_FOUND);
        BOOST_CHECK(error.empty());
        ExpectRecordsEqual(out, expected);
        BOOST_CHECK_EQUAL(outId, activeChain[i].second);

        // legacy-vs-shadow active-height equivalence
        BlockIndexSnapshot snap;
        {
            LOCK(cs_main);
            snap = accessor.GetActiveByHeight(h);
            BOOST_REQUIRE(snap.found);
        }
        BOOST_CHECK_EQUAL(out.hash.ToString(), snap.hash.ToString());
        BOOST_CHECK_EQUAL(out.height, snap.height);
    }

    // Boundary: below committed tip is NOT_FOUND (never serves uncommitted / absent
    // heights), negative is ERROR.
    const int tipHeight = activeChain.back().first;
    BlockIndexRecord out2;
    BlockIndexId outId2 = BLOCK_INDEX_ID_INVALID;
    error.clear();
    BOOST_CHECK_EQUAL(lookup.LookupByHeight(tipHeight + 1, &out2, &outId2, &error),
                      BLOCK_INDEX_ACTIVE_LOOKUP_NOT_FOUND);
    BOOST_CHECK(error.empty());
    error.clear();
    BOOST_CHECK_EQUAL(lookup.LookupByHeight(1000000, &out2, &outId2, &error),
                      BLOCK_INDEX_ACTIVE_LOOKUP_NOT_FOUND);
    BOOST_CHECK(error.empty());
    error.clear();
    BOOST_CHECK_EQUAL(lookup.LookupByHeight(-1, &out2, &outId2, &error),
                      BLOCK_INDEX_ACTIVE_LOOKUP_ERROR);
    BOOST_CHECK(!error.empty());
}

BOOST_AUTO_TEST_SUITE_END()