// Stage C — focused tests for the production Block Index Manager.
//
// Oracle: a real, independently-built V2 generation + the existing
// BlockIndexV2Reader (NOT the manager compared to itself). The manager is
// bound to that reader and its by-value results are checked against the
// generation's known contents.

#include <boost/test/unit_test.hpp>
#include "../blockindex_manager.h"
#include "../blockindex_v2_reader.h"
#include "../blockindex_generation_builder.h"
#include "../blockindex_generation_lifecycle.h"
#include "../blockindex_authoritative_startup.h" // g_fAuthoritativeStartup
#include "../innovarpc.h"                          // getblockchaininfo/getstakemodifiercheckpoints

#include <boost/filesystem.hpp>
#include <cstdio>
#include <string>

namespace {
static boost::filesystem::path MgrUniqueRoot()
{
    boost::filesystem::path p = boost::filesystem::temp_directory_path() /
        boost::filesystem::unique_path("innova-mgr-%%%%-%%%%");
    boost::filesystem::create_directories(p);
    return p;
}
static BlockIndexRecord MgrRec(uint64_t n, int h, uint256 prev)
{
    BlockIndexRecord r; r.hash = uint256(n); r.hashPrev = prev; r.height = h;
    r.nVersion = 1; r.nTime = 1000 + h; r.nBits = 0x1d00ffff; r.hashProof = uint256(n + 100);
    return r;
}
static BlockIndexGenerationSource MgrSource()
{
    BlockIndexGenerationSource s; uint256 prev(0);
    for (int h = 0; h < 8; ++h)
    {
        BlockIndexRecord r = MgrRec(100 + h, h, prev);
        BlockIndexGenerationSourceRecord q; q.hash = r.hash; q.record = r; s.records.push_back(q);
        prev = r.hash;
    }
    BlockIndexRecord side = MgrRec(999, 3, uint256(102));
    BlockIndexGenerationSourceRecord q; q.hash = side.hash; q.record = side; s.records.push_back(q);
    s.hashBestChain = prev; s.foundBestChain = true;
    return s;
}
static void MgrBuildSelected(const boost::filesystem::path& root)
{
    BlockIndexGenerationBuilder b; BlockIndexGenerationStats st; std::string e;
    BOOST_REQUIRE_MESSAGE(b.Build(MgrSource(), (root / "blockindex-gen-000001").string(), 1, &st, &e), e);
    b.Close();
    BOOST_REQUIRE_MESSAGE(BlockIndexGenerationManager::SelectGeneration(root.string(), 1, &e) == BLOCK_INDEX_LIFECYCLE_OK, e);
}
static void MgrOpen(const boost::filesystem::path& root, BlockIndexV2Reader* r)
{
    BlockIndexV2ReaderOptions o; std::string e;
    BOOST_REQUIRE_MESSAGE(r->Open(root.string(), o, &e), e);
}
// The manager must run in READER mode for these hermetic tests; other suites in
// the same process may leave the authoritative flag set. Save/restore it.
struct MgrReaderMode
{
    bool saved;
    MgrReaderMode() : saved(g_fAuthoritativeStartup) { g_fAuthoritativeStartup = false; }
    ~MgrReaderMode() { g_fAuthoritativeStartup = saved; }
};
} // namespace

BOOST_AUTO_TEST_SUITE(blockindex_manager_tests)

BOOST_AUTO_TEST_CASE(unbound_manager_is_not_available_and_fails_closed)
{
    MgrReaderMode mode;
    BlockIndexManager mgr;
    std::string e;
    BOOST_CHECK(!mgr.IsReaderBound());
    BOOST_CHECK(!mgr.IsAvailable(&e));
    BOOST_CHECK_EQUAL(mgr.Generation(), 0U);
    BOOST_CHECK_EQUAL(mgr.TotalRecordCount(), 0U);
    BlockIndexSnapshot s;
    BOOST_CHECK_EQUAL(mgr.LookupByHash(uint256(100), &s, &e), BLOCK_INDEX_MANAGER_NOT_OPEN);
    BOOST_CHECK_EQUAL(mgr.GetActiveByHeight(0, &s, &e), BLOCK_INDEX_MANAGER_NOT_OPEN);
    BOOST_CHECK(mgr.MakeView(&e).IsValid() == false);
}

BOOST_AUTO_TEST_CASE(reader_lookup_by_hash_and_active_height_are_equivalent)
{
    MgrReaderMode mode;
    boost::filesystem::path root = MgrUniqueRoot(); MgrBuildSelected(root);
    BlockIndexV2Reader r; MgrOpen(root, &r);
    BlockIndexManager mgr; mgr.BindReader(&r);
    std::string e; BlockIndexSnapshot s;

    BOOST_CHECK(mgr.IsAvailable(&e));
    BOOST_CHECK_EQUAL(mgr.Generation(), 1U);
    BOOST_CHECK_EQUAL(mgr.TotalRecordCount(), 9U);

    BOOST_CHECK_EQUAL(mgr.LookupByHash(uint256(105), &s, &e), BLOCK_INDEX_MANAGER_OK);
    BOOST_CHECK_EQUAL(s.height, 5); BOOST_CHECK(s.fInMainChain); BOOST_CHECK(s.hash == uint256(105));

    BOOST_CHECK_EQUAL(mgr.GetActiveByHeight(5, &s, &e), BLOCK_INDEX_MANAGER_OK);
    BOOST_CHECK(s.hash == uint256(105)); BOOST_CHECK(s.fInMainChain);

    // Independent oracle: the raw reader must agree.
    BlockIndexSnapshot raw;
    BOOST_CHECK_EQUAL(r.LookupByHash(uint256(105), &raw, &e), BLOCK_INDEX_V2_READ_FOUND);
    BOOST_CHECK(raw.height == s.height && raw.hash == s.hash);
}

BOOST_AUTO_TEST_CASE(parent_and_ancestor_are_by_value)
{
    MgrReaderMode mode;
    boost::filesystem::path root = MgrUniqueRoot(); MgrBuildSelected(root);
    BlockIndexV2Reader r; MgrOpen(root, &r);
    BlockIndexManager mgr; mgr.BindReader(&r);
    std::string e; BlockIndexSnapshot s;

    BOOST_CHECK_EQUAL(mgr.GetParent(uint256(105), &s, &e), BLOCK_INDEX_MANAGER_OK);
    BOOST_CHECK(s.hash == uint256(104) && s.height == 4);

    BOOST_CHECK_EQUAL(mgr.GetAncestor(uint256(107), 2, &s, &e), BLOCK_INDEX_MANAGER_OK);
    BOOST_CHECK(s.hash == uint256(102) && s.height == 2);

    // Genesis has no persisted parent: genuine absence, not a failure.
    BOOST_CHECK_EQUAL(mgr.GetParent(uint256(100), &s, &e), BLOCK_INDEX_MANAGER_NOT_FOUND);
}

BOOST_AUTO_TEST_CASE(tip_contains_and_tipis)
{
    MgrReaderMode mode;
    boost::filesystem::path root = MgrUniqueRoot(); MgrBuildSelected(root);
    BlockIndexV2Reader r; MgrOpen(root, &r);
    BlockIndexManager mgr; mgr.BindReader(&r);
    std::string e; BlockIndexSnapshot s;

    BOOST_CHECK_EQUAL(mgr.GetTip(&s, &e), BLOCK_INDEX_MANAGER_OK);
    BOOST_CHECK(s.hash == uint256(107) && s.height == 7);

    uint256 h;
    BOOST_CHECK_EQUAL(mgr.BestTipHash(&h, &e), BLOCK_INDEX_MANAGER_OK);
    BOOST_CHECK(h == uint256(107));
    int ht = -1;
    BOOST_CHECK_EQUAL(mgr.ActiveTipHeight(&ht, &e), BLOCK_INDEX_MANAGER_OK);
    BOOST_CHECK_EQUAL(ht, 7);

    bool b = false;
    BOOST_CHECK_EQUAL(mgr.Contains(uint256(105), &b, &e), BLOCK_INDEX_MANAGER_OK);
    BOOST_CHECK(b);
    BOOST_CHECK_EQUAL(mgr.Contains(uint256(0xdead), &b, &e), BLOCK_INDEX_MANAGER_OK);
    BOOST_CHECK(!b);
    BOOST_CHECK_EQUAL(mgr.TipIs(uint256(107), &b, &e), BLOCK_INDEX_MANAGER_OK);
    BOOST_CHECK(b);
    BOOST_CHECK_EQUAL(mgr.TipIs(uint256(105), &b, &e), BLOCK_INDEX_MANAGER_OK);
    BOOST_CHECK(!b);

    // A side (non-active) block is present but not the tip.
    BOOST_CHECK_EQUAL(mgr.Contains(uint256(999), &b, &e), BLOCK_INDEX_MANAGER_OK);
    BOOST_CHECK(b);
}

BOOST_AUTO_TEST_CASE(missing_block_is_not_found_never_authority_failure)
{
    MgrReaderMode mode;
    boost::filesystem::path root = MgrUniqueRoot(); MgrBuildSelected(root);
    BlockIndexV2Reader r; MgrOpen(root, &r);
    BlockIndexManager mgr; mgr.BindReader(&r);
    std::string e; BlockIndexSnapshot s; s.found = true;
    BOOST_CHECK_EQUAL(mgr.LookupByHash(uint256(0xdeadbeef), &s, &e), BLOCK_INDEX_MANAGER_NOT_FOUND);
    BOOST_CHECK(!s.found);
}

// Non-regression for the genesis stake-modifier-checksum materialization defect
// (genesis 0x0e00670b / h100000 checkpoint 0xcf12d0aa): a generation that cannot
// authoritatively supply derived state MUST fail closed / report availability
// false, and the manager MUST NEVER substitute zero for a missing checksum.
BOOST_AUTO_TEST_CASE(derived_consensus_fails_closed_no_zero_substitution)
{
    MgrReaderMode mode;
    boost::filesystem::path root = MgrUniqueRoot(); MgrBuildSelected(root);
    BlockIndexV2Reader r; MgrOpen(root, &r);
    BlockIndexManager mgr; mgr.BindReader(&r);
    std::string e; BlockIndexDerivedConsensus dc;

    BlockIndexManagerStatus st = mgr.GetDerivedConsensus(uint256(105), &dc, &e);
    // Either the generation is not derived-capable (UNSUPPORTED) or it is, in
    // which case absent fields stay unavailable. In NO case is a zero/default
    // checksum reported as available.
    BOOST_CHECK(st == BLOCK_INDEX_MANAGER_UNSUPPORTED || st == BLOCK_INDEX_MANAGER_OK);
    BOOST_CHECK(!dc.hasStakeModifierChecksum);
    BOOST_CHECK(!dc.hasBlockSize);
}

BOOST_AUTO_TEST_CASE(view_binds_generation_and_tip_and_detects_invalid)
{
    MgrReaderMode mode;
    boost::filesystem::path root = MgrUniqueRoot(); MgrBuildSelected(root);
    BlockIndexV2Reader r; MgrOpen(root, &r);
    BlockIndexManager mgr; mgr.BindReader(&r);
    std::string e;

    BlockIndexManager::View v = mgr.MakeView(&e);
    BOOST_CHECK(v.IsValid());
    BOOST_CHECK_EQUAL(v.Generation(), 1U);
    BOOST_CHECK(v.TipHash() == uint256(107));
    BOOST_CHECK_EQUAL(v.TipHeight(), 7);
    BOOST_CHECK_EQUAL(v.CheckCoherent(&e), BLOCK_INDEX_MANAGER_OK);

    BlockIndexManager::View none;
    std::string e2;
    BOOST_CHECK_EQUAL(none.CheckCoherent(&e2), BLOCK_INDEX_MANAGER_NOT_OPEN);
}

// ============================================================================
// Stage D additions — derived nSize completion, proof-type walk, view coherence.
// ============================================================================
namespace {
struct DSize { uint256 hash; unsigned int nFile; unsigned int nBlockPos; unsigned int nSize; };

// Write a structurally-valid block to blk0001.dat with the builder's expected
// [magic(4)][size(4)][serialized block] framing (same on-disk shape the builder
// consumes for exact nSize).
static DSize MgrWriteBlock(const boost::filesystem::path& dir, uint256 prev,
                           unsigned int t, unsigned int bits, unsigned int nonce)
{
    DSize info; info.nFile = 1;
    CTransaction cb; cb.nVersion = 1; cb.nTime = t;
    CTxIn in; in.prevout = COutPoint(uint256(0), 0xffffffff);
    in.scriptSig = CScript() << OP_TRUE; in.nSequence = 0xffffffff; cb.vin.push_back(in);
    CTxOut out; out.nValue = 0; out.scriptPubKey = CScript() << OP_TRUE; cb.vout.push_back(out);
    CBlock block; block.nVersion = 1; block.hashPrevBlock = prev; block.nTime = t;
    block.nBits = bits; block.nNonce = nonce; block.vtx.push_back(cb);
    block.hashMerkleRoot = block.BuildMerkleTree();
    info.hash = block.GetHash();
    info.nSize = ::GetSerializeSize(block, SER_NETWORK, PROTOCOL_VERSION);
    CDataStream ss(SER_DISK, CLIENT_VERSION); ss << block; unsigned int disk = ss.size();
    boost::filesystem::path f = dir / "blk0001.dat";
    FILE* fp = fopen(f.string().c_str(), "ab"); BOOST_REQUIRE(fp);
    unsigned char magic[] = {0xfa, 0xbf, 0xb5, 0xda};
    fwrite(magic, 1, 4, fp); fwrite(&disk, 4, 1, fp); long pos = ftell(fp);
    info.nBlockPos = (unsigned int)pos;
    fwrite(&ss[0], 1, ss.size(), fp); fclose(fp);
    return info;
}

// Build generation `gen` with `count` active blocks. When withBlocks, real
// blk0001.dat bytes are written (nSize present); posHeight marks one block PoS.
static unsigned int g_dSizePresent = 0;
static void MgrBuildGen(const boost::filesystem::path& root, uint64_t gen, int count,
                        int posHeight, bool withBlocks)
{
    BlockIndexGenerationSource src; uint256 prev(0); std::vector<DSize> bs;
    boost::filesystem::path bdir = root / "blocks";
    if (withBlocks) boost::filesystem::create_directories(bdir);
    for (int h = 0; h < count; ++h)
    {
        DSize d; if (withBlocks) d = MgrWriteBlock(bdir, prev, 1000 + h, 0x1d00ffff, 100 + h);
        else { d.hash = uint256(100 + h); d.nFile = 0; d.nBlockPos = 0; d.nSize = 0; }
        bs.push_back(d); prev = d.hash;
    }
    for (int h = 0; h < count; ++h)
    {
        BlockIndexRecord r; r.hash = bs[h].hash;
        r.hashPrev = (h == 0) ? uint256(0) : bs[h-1].hash; r.height = h;
        r.nVersion = 1; r.nTime = 1000 + h; r.nBits = 0x1d00ffff; r.nNonce = 100 + h;
        r.nFile = bs[h].nFile; r.nBlockPos = bs[h].nBlockPos;
        if (h == posHeight) { r.nFlags = CBlockIndex::BLOCK_PROOF_OF_STAKE; r.prevoutStake = COutPoint(uint256(0xabcdef), 0); r.nStakeTime = 1000 + h; }
        BlockIndexGenerationSourceRecord q; q.hash = r.hash; q.record = r; src.records.push_back(q);
    }
    src.hashBestChain = prev; src.foundBestChain = true;
    if (withBlocks) src.blockDataDir = bdir.string();
    if (withBlocks && posHeight >= 0 && posHeight < count) g_dSizePresent = bs[posHeight].nSize;
    BlockIndexGenerationBuilder b; BlockIndexGenerationStats st; std::string e;
    char gd[64]; snprintf(gd, sizeof(gd), "blockindex-gen-%06llu", (unsigned long long)gen);
    BOOST_REQUIRE_MESSAGE(b.Build(src, (root / gd).string(), gen, &st, &e), e);
    b.Close();
    BOOST_REQUIRE_MESSAGE(BlockIndexGenerationManager::SelectGeneration(root.string(), gen, &e) == BLOCK_INDEX_LIFECYCLE_OK, e);
}
} // namespace

BOOST_AUTO_TEST_CASE(d_blocksize_absent_is_unavailable_not_zero)
{
    MgrReaderMode mode;
    boost::filesystem::path root = MgrUniqueRoot(); MgrBuildSelected(root);
    BlockIndexV2Reader r; MgrOpen(root, &r);
    BlockIndexManager mgr; mgr.BindReader(&r);
    std::string e;
    unsigned int sz = 12345; bool has = true;
    BOOST_CHECK_EQUAL(r.GetBlockSize(1, &sz, &has, &e), BLOCK_INDEX_V2_READ_FOUND);
    BOOST_CHECK(!has);          // unavailable ...
    BOOST_CHECK_EQUAL(sz, 0U);  // ... and only then is the value zero
    BlockIndexDerivedConsensus dc;
    BlockIndexManagerStatus st = mgr.GetDerivedConsensus(uint256(100), &dc, &e);
    BOOST_CHECK(st == BLOCK_INDEX_MANAGER_UNSUPPORTED || st == BLOCK_INDEX_MANAGER_OK);
    BOOST_CHECK(!dc.hasBlockSize); // zero value never means "available"
}

BOOST_AUTO_TEST_CASE(d_blocksize_present_available_and_values_match)
{
    MgrReaderMode mode;
    boost::filesystem::path root = MgrUniqueRoot(); MgrBuildGen(root, 1, 8, 5, true);
    BlockIndexV2Reader r; MgrOpen(root, &r);
    BlockIndexManager mgr; mgr.BindReader(&r);
    std::string e; BlockIndexSnapshot s;
    BOOST_REQUIRE_EQUAL(mgr.GetActiveByHeight(5, &s, &e), BLOCK_INDEX_MANAGER_OK);
    BOOST_CHECK(s.hasBlockSize);
    BOOST_CHECK(s.nSize > 0);
    BOOST_CHECK_EQUAL(s.nSize, g_dSizePresent);
    // Manager derived view agrees with the reader accessor and the snapshot.
    BlockIndexDerivedConsensus dc;
    BOOST_CHECK_EQUAL(mgr.GetDerivedConsensus(s.hash, &dc, &e), BLOCK_INDEX_MANAGER_OK);
    BOOST_CHECK(dc.hasBlockSize);
    BOOST_CHECK_EQUAL(dc.blockSize, s.nSize);
    unsigned int sz = 0; bool has = false;
    BOOST_CHECK_EQUAL(r.GetBlockSize(s.id, &sz, &has, &e), BLOCK_INDEX_V2_READ_FOUND);
    BOOST_CHECK(has); BOOST_CHECK_EQUAL(sz, s.nSize);
}

BOOST_AUTO_TEST_CASE(d_blocksize_restart_reopen_equivalence)
{
    MgrReaderMode mode;
    boost::filesystem::path root = MgrUniqueRoot(); MgrBuildGen(root, 1, 8, 5, true);
    unsigned int first = 0;
    {
        BlockIndexV2Reader r; MgrOpen(root, &r);
        BlockIndexManager mgr; mgr.BindReader(&r);
        std::string e; BlockIndexSnapshot s;
        BOOST_REQUIRE_EQUAL(mgr.GetActiveByHeight(5, &s, &e), BLOCK_INDEX_MANAGER_OK);
        first = s.nSize; BOOST_CHECK(s.hasBlockSize);
        r.Close();
    }
    {
        BlockIndexV2Reader r2; MgrOpen(root, &r2);
        BlockIndexManager mgr2; mgr2.BindReader(&r2);
        std::string e; BlockIndexSnapshot s;
        BOOST_REQUIRE_EQUAL(mgr2.GetActiveByHeight(5, &s, &e), BLOCK_INDEX_MANAGER_OK);
        BOOST_CHECK(s.hasBlockSize);
        BOOST_CHECK_EQUAL(s.nSize, first);
    }
}

BOOST_AUTO_TEST_CASE(d_last_block_index_proof_type_walk)
{
    MgrReaderMode mode;
    boost::filesystem::path root = MgrUniqueRoot(); MgrBuildGen(root, 1, 8, 5, true);
    BlockIndexV2Reader r; MgrOpen(root, &r);
    BlockIndexManager mgr; mgr.BindReader(&r);
    std::string e;
    BlockIndexSnapshot tip;
    BOOST_REQUIRE_EQUAL(mgr.GetTip(&tip, &e), BLOCK_INDEX_MANAGER_OK);
    BOOST_CHECK_EQUAL(tip.height, 7);

    BlockIndexSnapshot powB, posB;
    BOOST_REQUIRE_EQUAL(mgr.GetLastBlockIndexByProofType(tip.hash, false, &powB, &e), BLOCK_INDEX_MANAGER_OK);
    BOOST_CHECK_EQUAL(powB.height, 7); BOOST_CHECK(!powB.fProofOfStake);       // tip is PoW
    BOOST_REQUIRE_EQUAL(mgr.GetLastBlockIndexByProofType(tip.hash, true, &posB, &e), BLOCK_INDEX_MANAGER_OK);
    BOOST_CHECK_EQUAL(posB.height, 5); BOOST_CHECK(posB.fProofOfStake);        // nearest PoS

    // Independent oracle: a raw reader backward walk must agree on the height.
    BlockIndexSnapshot w = tip; int found = -1;
    while (true) { if (w.fProofOfStake) { found = w.height; break; } if (!w.hasParent) break;
                   BOOST_REQUIRE_EQUAL(r.GetParent(w.id, &w, &e), BLOCK_INDEX_V2_READ_FOUND); }
    BOOST_CHECK_EQUAL(found, posB.height);
}

BOOST_AUTO_TEST_CASE(d_view_detects_generation_change)
{
    MgrReaderMode mode;
    boost::filesystem::path root = MgrUniqueRoot();
    MgrBuildGen(root, 1, 8, 5, true);
    BlockIndexV2Reader r1; MgrOpen(root, &r1);
    BlockIndexManager mgr; mgr.BindReader(&r1);
    std::string e;
    BlockIndexManager::View v = mgr.MakeView(&e);
    BOOST_REQUIRE(v.IsValid());
    BOOST_CHECK_EQUAL(v.Generation(), 1U);
    BOOST_CHECK_EQUAL(v.CheckCoherent(&e), BLOCK_INDEX_MANAGER_OK);
    r1.Close();

    // Publish + select a second generation, rebind to its reader.
    MgrBuildGen(root, 2, 9, 6, true);
    BlockIndexV2Reader r2; MgrOpen(root, &r2);
    mgr.BindReader(&r2);
    BOOST_CHECK_EQUAL(mgr.Generation(), 2U);
    // The old view must fail closed rather than silently mix chain views.
    BOOST_CHECK_EQUAL(v.CheckCoherent(&e), BLOCK_INDEX_MANAGER_AUTHORITY_FAILURE);
}

BOOST_AUTO_TEST_CASE(d_difficulty_path_is_pointer_free_no_mapblockindex)
{
    // Deletion gate for the authoritative difficulty read path: the manager must
    // supply the PoW/PoS difficulty INPUTS (proof-type ancestors) with the legacy
    // global mapBlockIndex EMPTY and pindexBest NULL. If this ever requires the
    // pointer graph, the migration is not real.
    MgrReaderMode mode;
    ClearBlockIndexAccessorState();
    std::map<uint256, CBlockIndex*> saved; saved.swap(mapBlockIndex);
    CBlockIndex* savedBest = pindexBest; pindexBest = NULL;
    {
        boost::filesystem::path root = MgrUniqueRoot(); MgrBuildGen(root, 1, 8, 5, true);
        BlockIndexV2Reader r; MgrOpen(root, &r);
        BlockIndexManager mgr; mgr.BindReader(&r);
        std::string e; uint256 tip; BlockIndexSnapshot powB, posB;
        BOOST_REQUIRE_EQUAL(mgr.BestTipHash(&tip, &e), BLOCK_INDEX_MANAGER_OK);
        BOOST_REQUIRE_EQUAL(mgr.GetLastBlockIndexByProofType(tip, false, &powB, &e), BLOCK_INDEX_MANAGER_OK);
        BOOST_REQUIRE_EQUAL(mgr.GetLastBlockIndexByProofType(tip, true, &posB, &e), BLOCK_INDEX_MANAGER_OK);
        BOOST_CHECK_EQUAL(powB.height, 7);
        BOOST_CHECK_EQUAL(posB.height, 5);
        BOOST_CHECK(mapBlockIndex.empty()); // never consulted
        r.Close();
    }
    pindexBest = savedBest; mapBlockIndex.swap(saved);
}

BOOST_AUTO_TEST_CASE(f_get_locator_hashes_is_pointer_free_no_mapblockindex)
{
    // Deletion gate for L6c: the getblocks locator must be built from a begin
    // HASH through the manager's by-value ancestry with the legacy global
    // mapBlockIndex EMPTY and pindexBest NULL. If locator construction ever
    // requires the historical pointer graph, the migration is not real.
    MgrReaderMode mode;
    ClearBlockIndexAccessorState();
    std::map<uint256, CBlockIndex*> saved; saved.swap(mapBlockIndex);
    CBlockIndex* savedBest = pindexBest; pindexBest = NULL;
    {
        boost::filesystem::path root = MgrUniqueRoot(); MgrBuildGen(root, 1, 8, 5, true);
        BlockIndexV2Reader r; MgrOpen(root, &r);
        BlockIndexManager mgr; mgr.BindReader(&r);
        std::string e; uint256 tip;
        BOOST_REQUIRE_EQUAL(mgr.BestTipHash(&tip, &e), BLOCK_INDEX_MANAGER_OK);

        std::vector<uint256> vHave;
        BOOST_REQUIRE_EQUAL(mgr.GetLocatorHashes(tip, &vHave, &e), BLOCK_INDEX_MANAGER_OK);
        // Legacy CBlockLocator(tip) layout: ancestors (10-then-doubling) + genesis.
        BOOST_REQUIRE(vHave.size() >= 2);
        BOOST_CHECK(vHave.front() == tip);
        BOOST_CHECK(vHave.back() == GetGenesisBlockHash());

        // uint256(0) reproduces the retired NULL begin: genesis-only locator.
        std::vector<uint256> vNull;
        BOOST_REQUIRE_EQUAL(mgr.GetLocatorHashes(uint256(0), &vNull, &e), BLOCK_INDEX_MANAGER_OK);
        BOOST_REQUIRE_EQUAL(vNull.size(), 1U);
        BOOST_CHECK(vNull[0] == GetGenesisBlockHash());

        // Unresolvable begin fails closed (never emits a wrong locator).
        std::vector<uint256> vBad;
        BOOST_CHECK_EQUAL(mgr.GetLocatorHashes(uint256(424242), &vBad, &e),
                          BLOCK_INDEX_MANAGER_NOT_FOUND);

        BOOST_CHECK(mapBlockIndex.empty()); // never consulted
        r.Close();
    }
    pindexBest = savedBest; mapBlockIndex.swap(saved);
}

// ============================================================================
// R1 slice-1 — best-tip pointer decoupling DELETION GATE.
//
// The migrated RPC consumers (getblockchaininfo money supply,
// getstakemodifiercheckpoints best-tip height, getinfo money supply, gettxout
// bestblock/confirmations) must serve the ACTIVE TIP by value from the
// authoritative block-index manager with the LEGACY GLOBALS REMOVED:
// pindexBest == NULL and mapBlockIndex EMPTY. Before the migration these
// consumers dereferenced the historical pindexBest CBlockIndex object and would
// fault (NULL dereference) in exactly this environment.
// ============================================================================
namespace {
// Build generation `gen` (count active blocks, one PoS at posHeight) with a
// KNOWN non-zero money supply on every record; select it; report the tip.
static void MgrBuildTipMetaGen(const boost::filesystem::path& root, uint64_t gen,
                               int count, int posHeight, int64_t supplyBase,
                               uint256* outTipHash, int* outTipHeight)
{
    BlockIndexGenerationSource src; uint256 prev(0);
    for (int h = 0; h < count; ++h)
    {
        BlockIndexRecord r; r.hash = uint256(300 + h); r.hashPrev = prev; r.height = h;
        r.nVersion = 1; r.nTime = 1000 + h; r.nBits = 0x1d00ffff; r.nNonce = 100 + h;
        r.nMoneySupply = supplyBase + h;
        if (h == posHeight) { r.nFlags = CBlockIndex::BLOCK_PROOF_OF_STAKE;
                              r.prevoutStake = COutPoint(uint256(0xabcdef), 0); r.nStakeTime = 1000 + h; }
        BlockIndexGenerationSourceRecord q; q.hash = r.hash; q.record = r; src.records.push_back(q);
        prev = r.hash;
    }
    src.hashBestChain = prev; src.foundBestChain = true;
    BlockIndexGenerationBuilder b; BlockIndexGenerationStats st; std::string e;
    char gd[64]; snprintf(gd, sizeof(gd), "blockindex-gen-%06llu", (unsigned long long)gen);
    BOOST_REQUIRE_MESSAGE(b.Build(src, (root / gd).string(), gen, &st, &e), e);
    b.Close();
    BOOST_REQUIRE_MESSAGE(BlockIndexGenerationManager::SelectGeneration(root.string(), gen, &e) == BLOCK_INDEX_LIFECYCLE_OK, e);
    *outTipHash = prev; *outTipHeight = count - 1;
}
// Remove the legacy pointer globals for the duration of the gate, restoring them
// (even on an assertion-throw) so the suite stays reusable.
struct R1LegacyGlobalsGuard
{
    std::map<uint256, CBlockIndex*> savedIdx;
    CBlockIndex* savedBest;
    R1LegacyGlobalsGuard() : savedBest(pindexBest)
    {
        ClearBlockIndexAccessorState();
        savedIdx.swap(mapBlockIndex);
        pindexBest = NULL;
    }
    ~R1LegacyGlobalsGuard() { pindexBest = savedBest; mapBlockIndex.swap(savedIdx); }
};
// Save/restore the process-singleton manager binding.
struct R1ManagerBindingGuard
{
    BlockIndexManager& mgr;
    const BlockIndexV2Reader* saved;
    R1ManagerBindingGuard() : mgr(GetBlockIndexManager()), saved(mgr.BoundReader()) {}
    ~R1ManagerBindingGuard() { mgr.BindReader(saved); }
};
} // namespace

BOOST_AUTO_TEST_CASE(g_r1_tip_metadata_is_pointer_free_no_mapblockindex)
{
    MgrReaderMode mode;                 // g_fAuthoritativeStartup=false -> bound-reader backend
    R1LegacyGlobalsGuard legacy;        // pindexBest==NULL, mapBlockIndex empty
    R1ManagerBindingGuard binding;      // restores the singleton binding afterwards
    BlockIndexManager& mgr = binding.mgr;

    boost::filesystem::path root = MgrUniqueRoot();
    uint256 tip1; int ht1 = -1;
    MgrBuildTipMetaGen(root, 1, 8, 5, 5000, &tip1, &ht1);   // tip height 7, PoS at 5
    BlockIndexV2Reader r; MgrOpen(root, &r);
    mgr.BindReader(&r);

    // Independent oracle: the bound V2 authority agrees on tip identity/height/membership.
    std::string e;
    uint256 mh; int mht = -1; bool isTip = false;
    BOOST_REQUIRE_EQUAL(mgr.BestTipHash(&mh, &e), BLOCK_INDEX_MANAGER_OK);
    BOOST_CHECK(mh == tip1);
    BOOST_REQUIRE_EQUAL(mgr.ActiveTipHeight(&mht, &e), BLOCK_INDEX_MANAGER_OK);
    BOOST_CHECK_EQUAL(mht, ht1);
    BOOST_REQUIRE_EQUAL(mgr.TipIs(tip1, &isTip, &e), BLOCK_INDEX_MANAGER_OK);
    BOOST_CHECK(isTip);

    // getblockchaininfo: money supply comes from the tip BY VALUE (no pindexBest,
    // no mapBlockIndex). A CPI/pointer consumer would fault here.
    const json_spirit::Array params;
    json_spirit::Object bci = getblockchaininfo(params, false).get_obj();
    BlockIndexSnapshot tipSnap;
    BOOST_REQUIRE_EQUAL(mgr.GetTip(&tipSnap, &e), BLOCK_INDEX_MANAGER_OK);
    BOOST_CHECK(tipSnap.hash == tip1);
    BOOST_CHECK(tipSnap.height == ht1);
    BOOST_CHECK(find_value(bci, "moneysupply") == ValueFromAmount(tipSnap.nMoneySupply));
    BOOST_CHECK(find_value(bci, "difficulty").get_obj().size() == 2U); // ManagerDifficultyPair succeeded

    // getstakemodifiercheckpoints: best-tip height from the manager (startheight
    // above the tip so no legacy FindBlockByHeight walk is entered).
    json_spirit::Array sp;
    sp.push_back(json_spirit::Value(1000000)); sp.push_back(json_spirit::Value(1000));
    json_spirit::Object smc = getstakemodifiercheckpoints(sp, false).get_obj();
    BOOST_CHECK_EQUAL(find_value(smc, "end_height").get_int(), ht1);

    BOOST_CHECK(mapBlockIndex.empty());   // the pointer graph was never consulted

    // ---- tip change: a second generation / new tip is reflected by value ----
    r.Close();
    uint256 tip2; int ht2 = -1;
    MgrBuildTipMetaGen(root, 2, 9, 6, 9000, &tip2, &ht2);   // tip height 8, PoS at 6
    BOOST_CHECK(tip2 != tip1);
    BlockIndexV2Reader r2; MgrOpen(root, &r2);
    mgr.BindReader(&r2);
    BOOST_REQUIRE_EQUAL(mgr.ActiveTipHeight(&mht, &e), BLOCK_INDEX_MANAGER_OK);
    BOOST_CHECK_EQUAL(mht, ht2);
    json_spirit::Object bci2 = getblockchaininfo(params, false).get_obj();
    BlockIndexSnapshot tipSnap2;
    BOOST_REQUIRE_EQUAL(mgr.GetTip(&tipSnap2, &e), BLOCK_INDEX_MANAGER_OK);
    BOOST_CHECK(tipSnap2.hash == tip2);
    BOOST_CHECK(find_value(bci2, "moneysupply") == ValueFromAmount(tipSnap2.nMoneySupply));
    BOOST_CHECK(!(find_value(bci2, "moneysupply") == find_value(bci, "moneysupply")));
    json_spirit::Object smc2 = getstakemodifiercheckpoints(sp, false).get_obj();
    BOOST_CHECK_EQUAL(find_value(smc2, "end_height").get_int(), ht2);
    r2.Close();

    // ---- authority absent: the by-value tip read fails closed (never a silent
    //      tip, never a fallback to a NULL historical pointer) ----
    mgr.BindReader(NULL);
    BlockIndexSnapshot tipAbsent;
    BOOST_CHECK(!mgr.IsAvailable(&e));
    BOOST_CHECK(mgr.GetTip(&tipAbsent, &e) != BLOCK_INDEX_MANAGER_OK);
}

BOOST_AUTO_TEST_SUITE_END()


