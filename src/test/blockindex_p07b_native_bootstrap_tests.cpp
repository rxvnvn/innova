// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.

// PM1-P0-07b (C2) focused fixtures: daemon-native Block Index V2 bootstrap and
// bounded legacy (txleveldb) migration under the OWNER-ACCEPTED MIXED-STORE
// boundary (no *.old rename; txleveldb stays an active runtime database).
//
// Fixtures:
//   c2_a  EMPTY_NEW genesis bootstrap
//   c2_a  genesis semantic parity vs the REAL legacy resident oracle
//   c2_c  legacy-only bounded migration (L2)
//   c2_c  crash before CURRENT -> deterministic retry (L3)
//   c2_c  corrupt new authority -> FAIL-CLOSED, never legacy fallback (L6)
//   c2_c  mixed-store continuity: shared txleveldb still operational (L5)
//   c2_f  source/builder failure -> no CURRENT, no authority transfer

#include <boost/test/unit_test.hpp>

#include "main.h"
#include "txdb.h"
#include "blockindex_native_bootstrap.h"
#include "blockindex_startup_ownership.h"
#include "blockindex_generation_builder.h"
#include "blockindex_generation_lifecycle.h"
#include "blockindex_startup_authority.h"

#include <leveldb/db.h>
#include <leveldb/filter_policy.h>
#include <boost/filesystem.hpp>

#include <cstdio>
#include <string>
#include <vector>

namespace fs = boost::filesystem;

extern void* GetGlobalTxdbPtrForTest();

static const char* kCurrentName = "blockindex-current";

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

static fs::path C2TempDir(const char* tag)
{
    fs::path p = fs::temp_directory_path() / fs::unique_path(std::string(tag) + "-%%%%-%%%%");
    fs::create_directories(p);
    return p;
}

static void C2CloseLegacyGlobal()
{
    if (GetGlobalTxdbPtrForTest() != NULL)
    {
        CTxDB db("r");
        db.Close();
    }
}

// Deterministic synthetic block; mirrors the proven R2D/R3F writer.
struct C2BlockInfo { uint256 hash; unsigned int nFile, nBlockPos; };

static C2BlockInfo C2WriteBlock(const fs::path& dir, uint256 prev, unsigned int nTime,
                                unsigned int nBits, unsigned int nNonce, unsigned int nFile,
                                CBlock* outBlock = NULL)
{
    C2BlockInfo info; info.nFile = nFile;
    CTransaction coinbase; coinbase.nVersion = 1; coinbase.nTime = nTime;
    CTxIn input; input.prevout = COutPoint(uint256(0), 0xffffffff);
    input.scriptSig = CScript() << OP_TRUE; input.nSequence = 0xffffffff;
    coinbase.vin.push_back(input);
    CTxOut output; output.nValue = 0; output.scriptPubKey = CScript() << OP_TRUE;
    coinbase.vout.push_back(output);
    CBlock block; block.nVersion = 1; block.hashPrevBlock = prev;
    block.nTime = nTime; block.nBits = nBits; block.nNonce = nNonce;
    block.vtx.push_back(coinbase); block.hashMerkleRoot = block.BuildMerkleTree();
    info.hash = block.GetHash();
    if (outBlock) *outBlock = block;
    CDataStream ss(SER_DISK, CLIENT_VERSION); ss << block;
    char name[32]; snprintf(name, sizeof(name), "blk%04u.dat", nFile);
    FILE* fp = fopen((dir / name).string().c_str(), "ab"); BOOST_REQUIRE(fp != NULL);
    unsigned char magic[] = {0xfa, 0xbf, 0xb5, 0xda};
    fwrite(magic, 1, 4, fp);
    unsigned int ns = ss.size();
    fwrite(&ns, 4, 1, fp);
    long pos = ftell(fp); info.nBlockPos = (unsigned int)pos;
    fwrite(&ss[0], 1, ss.size(), fp); fflush(fp); fclose(fp);
    return info;
}

// Build a REAL synthetic legacy-only datadir: blk00001.dat with heights 0..S and
// a genuine <dir>/txleveldb LevelDB holding CDiskBlockIndex records + hashBestChain
// (exactly the key/value encoding the shared AddToBlockIndex/SetBestChain path uses).
// When fWriteBestChain is false the "hashBestChain" key is omitted (bad source).
// When fWriteBlockFiles is false the blk files are omitted (nSize unavailable).
static std::vector<uint256> C2BuildSyntheticLegacy(const fs::path& dir, int S,
                                                   bool fWriteBestChain = true,
                                                   bool fWriteBlockFiles = true)
{
    fs::create_directories(dir);
    std::vector<uint256> hashes;
    std::vector<C2BlockInfo> infos;
    std::vector<CBlock> blocks;
    uint256 prev(0);
    for (int h = 0; h <= S; ++h)
    {
        CBlock block;
        C2BlockInfo b;
        if (fWriteBlockFiles)
        {
            b = C2WriteBlock(dir, prev, 1700000000u + (unsigned)h, 0x1d00ffffU, (unsigned)h, 1, &block);
        }
        else
        {
            CTransaction coinbase; coinbase.nVersion = 1; coinbase.nTime = 1700000000u + (unsigned)h;
            CTxIn input; input.prevout = COutPoint(uint256(0), 0xffffffff);
            input.scriptSig = CScript() << OP_TRUE; input.nSequence = 0xffffffff;
            coinbase.vin.push_back(input);
            CTxOut output; output.nValue = 0; output.scriptPubKey = CScript() << OP_TRUE;
            coinbase.vout.push_back(output);
            block.nVersion = 1; block.hashPrevBlock = prev;
            block.nTime = 1700000000u + (unsigned)h; block.nBits = 0x1d00ffffU;
            block.nNonce = (unsigned)h; block.vtx.push_back(coinbase);
            block.hashMerkleRoot = block.BuildMerkleTree();
            b.hash = block.GetHash(); b.nFile = 1; b.nBlockPos = 8 + 5 + h;
        }
        hashes.push_back(b.hash); infos.push_back(b); blocks.push_back(block); prev = b.hash;
    }

    leveldb::Options options;
    options.create_if_missing = true;
    options.filter_policy = leveldb::NewBloomFilterPolicy(10);
    leveldb::DB* db = NULL;
    BOOST_REQUIRE(leveldb::DB::Open(options, (dir / "txleveldb").string(), &db).ok());
    for (size_t h = 0; h < infos.size(); ++h)
    {
        CDiskBlockIndex bi;
        bi.nHeight = (int)h;
        bi.nFile = infos[h].nFile;
        bi.nBlockPos = infos[h].nBlockPos;
        bi.hashPrev = blocks[h].hashPrevBlock;
        bi.nVersion = blocks[h].nVersion;
        bi.nTime = blocks[h].nTime;
        bi.nBits = blocks[h].nBits;
        bi.nNonce = blocks[h].nNonce;
        bi.nFlags = 0;
        bi.hashMerkleRoot = blocks[h].hashMerkleRoot;
        bi.hashProof = uint256(0);
        bi.nMint = 0; bi.nMoneySupply = 0;
        CDataStream ssKey(SER_DISK, CLIENT_VERSION);
        ssKey << make_pair(std::string("blockindex"), bi.GetBlockHash());
        CDataStream ssVal(SER_DISK, CLIENT_VERSION);
        ssVal << bi;
        BOOST_REQUIRE(db->Put(leveldb::WriteOptions(), ssKey.str(), ssVal.str()).ok());
    }
    if (fWriteBestChain)
    {
        CDataStream k(SER_DISK, CLIENT_VERSION); k << std::string("hashBestChain");
        CDataStream v(SER_DISK, CLIENT_VERSION); v << hashes.back();
        BOOST_REQUIRE(db->Put(leveldb::WriteOptions(), k.str(), v.str()).ok());
    }
    // A non-block-index namespace that must survive migration (mixed-store proof).
    {
        CDataStream k(SER_DISK, CLIENT_VERSION); k << std::string("version");
        CDataStream v(SER_DISK, CLIENT_VERSION); v << (uint32_t)12345;
        BOOST_REQUIRE(db->Put(leveldb::WriteOptions(), k.str(), v.str()).ok());
    }
    delete db;
    return hashes;
}

static bool C2BuildPublishSelect(const fs::path& root, const BlockIndexGenerationSource& src,
                                 uint64_t gen, std::string* error)
{
    BlockIndexGenerationBuilder builder;
    if (!builder.Build(src, BlockIndexGenerationManager::StagingPath(root.string(), gen),
                       gen, NULL, error))
    {
        builder.Close();
        return false;
    }
    builder.Close();
    if (BlockIndexGenerationManager::PublishGeneration(root.string(), gen, error) != BLOCK_INDEX_LIFECYCLE_OK)
        return false;
    if (BlockIndexGenerationManager::SelectGeneration(root.string(), gen, error) != BLOCK_INDEX_LIFECYCLE_OK)
        return false;
    return true;
}

static bool C2OpenGeneration(const fs::path& root, V2BlockIndexStartupAuthority* auth, std::string* error)
{
    return auth->Open(root.string(), error) == BLOCK_INDEX_STARTUP_OK;
}

BOOST_AUTO_TEST_SUITE(blockindex_p07b_native_bootstrap_tests)

// ---------------------------------------------------------------------------
// C2-A: EMPTY_NEW genesis bootstrap through the REAL shared builder
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(c2_a_empty_new_genesis_bootstrap)
{
    C2CloseLegacyGlobal();
    const fs::path v2root = C2TempDir("c2a-empty");

    std::string err;
    BOOST_REQUIRE_MESSAGE(
        PrepareNativeBlockIndexGeneration(v2root.string(), GetDataDir().string(),
                                          BLOCK_INDEX_STARTUP_OWNERSHIP_EMPTY_NEW, &err) ==
            BLOCK_INDEX_NATIVE_PREPARE_OK, err);

    // Durable logical CURRENT -> generation 1; transient staging removed.
    uint64_t gen = 0;
    BOOST_REQUIRE(BlockIndexGenerationManager::OpenCurrent(v2root.string(), &gen, &err) ==
                  BLOCK_INDEX_LIFECYCLE_OK);
    BOOST_REQUIRE_EQUAL(gen, (uint64_t)1);
    BOOST_REQUIRE(fs::exists(v2root / kCurrentName));
    BOOST_REQUIRE(fs::is_directory(fs::path(BlockIndexGenerationManager::GenerationPath(v2root.string(), 1))));
    BOOST_REQUIRE(!fs::exists(fs::path(BlockIndexGenerationManager::StagingPath(v2root.string(), 1))));
    BOOST_REQUIRE(!fs::exists(fs::path((v2root / "blockindex-migsrc.tmp").string())));

    // height-0 new-format authority.
    V2BlockIndexStartupAuthority auth;
    BOOST_REQUIRE_MESSAGE(C2OpenGeneration(v2root, &auth, &err), err);
    BOOST_REQUIRE(auth.IsAuthoritativeCapable());
    BlockIndexStartupResult gen0 = auth.GetActiveByHeight(0);
    BOOST_REQUIRE(gen0.HasRecord());
    BOOST_REQUIRE_EQUAL(gen0.record.height, 0);
    BOOST_REQUIRE(gen0.record.active);
    BOOST_REQUIRE(!gen0.record.hasParent);
    BOOST_REQUIRE(gen0.record.logicalId.GetHash() == GetGenesisBlockHash());
    BOOST_REQUIRE(gen0.record.derived.hasChainTrust);
    BOOST_REQUIRE(gen0.record.derived.hasBlockSize);
    BlockIndexStartupResult tip = auth.GetTip();
    BOOST_REQUIRE(tip.HasRecord());
    BOOST_REQUIRE_EQUAL(tip.record.height, 0);
    auth.Close();

    fs::remove_all(v2root);
}

// ---------------------------------------------------------------------------
// C2-A: genesis semantic parity vs the REAL legacy resident oracle
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(c2_a_genesis_semantic_parity)
{
    C2CloseLegacyGlobal();
    const fs::path v2root = C2TempDir("c2a-parity");

    BlockIndexGenerationSource src;
    std::string err;
    BOOST_REQUIRE_MESSAGE(CreateGenesisBlockIndexSource(GetDataDir().string(), &src, &err), err);
    BOOST_REQUIRE_EQUAL(src.records.size(), (size_t)1);
    BOOST_REQUIRE(src.foundBestChain);
    const BlockIndexRecord& rec = src.records[0].record;
    const uint256 ghash = src.records[0].hash;

    // Legacy resident oracle: the real CBlockIndex the legacy path materialises.
    BOOST_REQUIRE(ghash == GetGenesisBlockHash());
    std::map<uint256, CBlockIndex*>::iterator it = mapBlockIndex.find(ghash);
    BOOST_REQUIRE(it != mapBlockIndex.end());
    const CBlockIndex* oracle = it->second;

    BOOST_REQUIRE(rec.hash == oracle->GetBlockHash());
    BOOST_REQUIRE(rec.hashPrev == uint256(0));
    BOOST_REQUIRE(oracle->pprev == NULL); // canonical parentless representation
    BOOST_REQUIRE_EQUAL(rec.height, oracle->nHeight);
    BOOST_REQUIRE_EQUAL(rec.height, 0);
    BOOST_REQUIRE_EQUAL(rec.nVersion, oracle->nVersion);
    BOOST_REQUIRE(rec.hashMerkleRoot == oracle->hashMerkleRoot);
    BOOST_REQUIRE_EQUAL(rec.nTime, oracle->nTime);
    BOOST_REQUIRE_EQUAL(rec.nBits, oracle->nBits);
    BOOST_REQUIRE_EQUAL(rec.nNonce, oracle->nNonce);
    BOOST_REQUIRE_EQUAL(rec.nFlags, oracle->nFlags);
    BOOST_REQUIRE(rec.hashProof == oracle->hashProof);
    BOOST_REQUIRE_EQUAL(rec.nStakeModifier, oracle->nStakeModifier);
    BOOST_REQUIRE_EQUAL(rec.nStakeTime, oracle->nStakeTime);
    BOOST_REQUIRE(rec.prevoutStake == oracle->prevoutStake);
    BOOST_REQUIRE_EQUAL(rec.nMint, oracle->nMint);
    BOOST_REQUIRE_EQUAL(rec.nMoneySupply, oracle->nMoneySupply);

    // Genesis disk binding: the record's (nFile,nBlockPos) resolves to the block.
    {
        char name[32]; snprintf(name, sizeof(name), "blk%04u.dat", rec.nFile);
        FILE* f = fopen((GetDataDir() / name).string().c_str(), "rb");
        BOOST_REQUIRE(f != NULL);
        BOOST_REQUIRE(fseek(f, (long)rec.nBlockPos, SEEK_SET) == 0);
        CBlock blk;
        { CAutoFile filein(f, SER_DISK, CLIENT_VERSION); filein >> blk; }
        BOOST_REQUIRE(blk.GetHash() == ghash);
    }

    // Derived-entry parity through the REAL shared builder.
    BOOST_REQUIRE_MESSAGE(C2BuildPublishSelect(v2root, src, 1, &err), err);
    V2BlockIndexStartupAuthority auth;
    BOOST_REQUIRE_MESSAGE(C2OpenGeneration(v2root, &auth, &err), err);
    BlockIndexStartupResult gen0 = auth.GetActiveByHeight(0);
    BOOST_REQUIRE(gen0.HasRecord());
    BOOST_REQUIRE(gen0.record.derived.hasChainTrust);
    BOOST_REQUIRE(gen0.record.derived.chainTrust == oracle->nChainTrust);
    BOOST_REQUIRE(gen0.record.derived.hasStakeModifierChecksum);
    BOOST_REQUIRE_EQUAL(gen0.record.derived.stakeModifierChecksum, oracle->nStakeModifierChecksum);
    BOOST_REQUIRE(gen0.record.derived.hasStakeModifierTime);
    BOOST_REQUIRE_EQUAL(gen0.record.derived.stakeModifierTime, oracle->nStakeModifierTime);
    BOOST_REQUIRE(gen0.record.derived.hasBlockSize);
    BOOST_REQUIRE(gen0.record.derived.blockSize > 0);
    auth.Close();

    fs::remove_all(v2root);
}

// ---------------------------------------------------------------------------
// C2-C / L2: legacy-only bounded migration -> durable CURRENT -> V2 authority
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(c2_c_legacy_only_migration)
{
    const fs::path dir = C2TempDir("c2c-legacy");
    std::vector<uint256> hashes = C2BuildSyntheticLegacy(dir, 3);
    const uint256 tip = hashes.back();
    const fs::path v2root = dir / "blockindex-v2";

    C2CloseLegacyGlobal();

    std::string err;
    BlockIndexStartupOwnershipDecision d;
    BOOST_REQUIRE_MESSAGE(ClassifyBlockIndexStartupOwnership(v2root.string(), dir.string(), &d, &err), err);
    BOOST_REQUIRE_EQUAL((int)d.state, (int)BLOCK_INDEX_STARTUP_OWNERSHIP_LEGACY_MIGRATION_REQUIRED);
    BOOST_REQUIRE(d.legacyPresent);

    BOOST_REQUIRE_MESSAGE(
        PrepareNativeBlockIndexGeneration(v2root.string(), dir.string(), d.state, &err) ==
            BLOCK_INDEX_NATIVE_PREPARE_OK, err);

    uint64_t gen = 0;
    BOOST_REQUIRE(BlockIndexGenerationManager::OpenCurrent(v2root.string(), &gen, &err) ==
                  BLOCK_INDEX_LIFECYCLE_OK);
    BOOST_REQUIRE_EQUAL(gen, (uint64_t)1);

    V2BlockIndexStartupAuthority auth;
    BOOST_REQUIRE_MESSAGE(C2OpenGeneration(v2root, &auth, &err), err);
    BOOST_REQUIRE(auth.IsAuthoritativeCapable());
    BOOST_REQUIRE_EQUAL(auth.GetTip().record.height, 3);
    BOOST_REQUIRE(auth.GetTip().record.logicalId.GetHash() == tip);
    BOOST_REQUIRE(auth.GetActiveByHeight(0).HasRecord());
    BOOST_REQUIRE(auth.GetActiveByHeight(3).record.logicalId.GetHash() == tip);
    auth.Close();

    // Post-migration: restart selects V2 authority; legacy source no longer required.
    BlockIndexStartupOwnershipDecision d2;
    BOOST_REQUIRE(ClassifyBlockIndexStartupOwnership(v2root.string(), dir.string(), &d2, &err));
    BOOST_REQUIRE_EQUAL((int)d2.state, (int)BLOCK_INDEX_STARTUP_OWNERSHIP_V2_AUTHORITATIVE);
    BOOST_REQUIRE(d2.v2Valid);

    // Owner boundary: the shared txleveldb is NOT renamed/split.
    BOOST_REQUIRE(fs::is_directory(dir / "txleveldb"));
    BOOST_REQUIRE(!fs::exists(dir / "txleveldb.old"));
    BOOST_REQUIRE(!fs::exists(fs::path((v2root / "blockindex-migsrc.tmp").string())));

    fs::remove_all(dir);
}

// ---------------------------------------------------------------------------
// L3: crash before CURRENT (CURRENT removed) -> deterministic retry succeeds
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(c2_l3_crash_before_current_retry)
{
    const fs::path dir = C2TempDir("c2c-l3");
    std::vector<uint256> hashes = C2BuildSyntheticLegacy(dir, 2);
    const uint256 tip = hashes.back();
    const fs::path v2root = dir / "blockindex-v2";
    C2CloseLegacyGlobal();

    std::string err;
    BlockIndexStartupOwnershipDecision d;
    BOOST_REQUIRE(ClassifyBlockIndexStartupOwnership(v2root.string(), dir.string(), &d, &err));
    BOOST_REQUIRE_EQUAL((int)d.state, (int)BLOCK_INDEX_STARTUP_OWNERSHIP_LEGACY_MIGRATION_REQUIRED);
    BOOST_REQUIRE_MESSAGE(
        PrepareNativeBlockIndexGeneration(v2root.string(), dir.string(), d.state, &err) ==
            BLOCK_INDEX_NATIVE_PREPARE_OK, err);

    // Simulate a crash BEFORE the CURRENT commit: drop the marker only.
    BOOST_REQUIRE(fs::remove(v2root / kCurrentName));
    BlockIndexStartupOwnershipDecision dCrash;
    BOOST_REQUIRE(ClassifyBlockIndexStartupOwnership(v2root.string(), dir.string(), &dCrash, &err));
    BOOST_REQUIRE_EQUAL((int)dCrash.state, (int)BLOCK_INDEX_STARTUP_OWNERSHIP_LEGACY_MIGRATION_REQUIRED);

    // The legacy source is untouched -> deterministic retry migrates again.
    BOOST_REQUIRE(fs::is_directory(dir / "txleveldb"));
    BOOST_REQUIRE_MESSAGE(
        PrepareNativeBlockIndexGeneration(v2root.string(), dir.string(), dCrash.state, &err) ==
            BLOCK_INDEX_NATIVE_PREPARE_OK, err);
    uint64_t gen = 0;
    BOOST_REQUIRE(BlockIndexGenerationManager::OpenCurrent(v2root.string(), &gen, &err) ==
                  BLOCK_INDEX_LIFECYCLE_OK);
    BOOST_REQUIRE_EQUAL(gen, (uint64_t)2);

    V2BlockIndexStartupAuthority auth;
    BOOST_REQUIRE_MESSAGE(C2OpenGeneration(v2root, &auth, &err), err);
    BOOST_REQUIRE(auth.GetTip().record.logicalId.GetHash() == tip);
    auth.Close();

    fs::remove_all(dir);
}

// ---------------------------------------------------------------------------
// L6: corrupt new authority with valid legacy present -> FAIL-CLOSED
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(c2_l6_corrupt_authority_fails_closed)
{
    const fs::path dir = C2TempDir("c2c-l6");
    std::vector<uint256> hashes = C2BuildSyntheticLegacy(dir, 2);
    (void)hashes;
    const fs::path v2root = dir / "blockindex-v2";
    C2CloseLegacyGlobal();

    std::string err;
    BOOST_REQUIRE_MESSAGE(
        PrepareNativeBlockIndexGeneration(v2root.string(), dir.string(),
                                          BLOCK_INDEX_STARTUP_OWNERSHIP_LEGACY_MIGRATION_REQUIRED, &err) ==
            BLOCK_INDEX_NATIVE_PREPARE_OK, err);
    BOOST_REQUIRE(fs::exists(v2root / kCurrentName));

    // Corrupt the authority marker while a perfectly valid legacy store exists.
    {
        FILE* f = fopen((v2root / kCurrentName).string().c_str(), "wb");
        BOOST_REQUIRE(f != NULL);
        unsigned char junk[28]; for (int i = 0; i < 28; ++i) junk[i] = 0xFF;
        fwrite(junk, 1, sizeof(junk), f); fflush(f); fclose(f);
    }

    BlockIndexStartupOwnershipDecision d;
    BOOST_REQUIRE(ClassifyBlockIndexStartupOwnership(v2root.string(), dir.string(), &d, &err));
    // Present-but-corrupt authority must NOT be reclassified as a legacy migration.
    BOOST_REQUIRE_EQUAL((int)d.state, (int)BLOCK_INDEX_STARTUP_OWNERSHIP_FATAL_CORRUPTION);

    V2BlockIndexStartupAuthority auth;
    BOOST_REQUIRE(auth.Open(v2root.string(), &err) != BLOCK_INDEX_STARTUP_OK);
    BOOST_REQUIRE(!auth.IsOpen());

    // Prepare refuses a fatal state (no silent re-migration / legacy fallback).
    BOOST_REQUIRE(PrepareNativeBlockIndexGeneration(v2root.string(), dir.string(), d.state, &err) ==
                  BLOCK_INDEX_NATIVE_PREPARE_FAILED);

    fs::remove_all(dir);
}

// ---------------------------------------------------------------------------
// L5: mixed-store continuity - shared txleveldb stays an operational database
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(c2_l5_mixed_store_continuity)
{
    const fs::path dir = C2TempDir("c2c-l5");
    C2BuildSyntheticLegacy(dir, 2);
    const fs::path v2root = dir / "blockindex-v2";
    C2CloseLegacyGlobal();

    std::string err;
    BOOST_REQUIRE_MESSAGE(
        PrepareNativeBlockIndexGeneration(v2root.string(), dir.string(),
                                          BLOCK_INDEX_STARTUP_OWNERSHIP_LEGACY_MIGRATION_REQUIRED, &err) ==
            BLOCK_INDEX_NATIVE_PREPARE_OK, err);

    // The SAME txleveldb path is still present and openable after V2 activation.
    BOOST_REQUIRE(fs::is_directory(dir / "txleveldb"));
    leveldb::Options options;
    options.create_if_missing = false;
    leveldb::DB* db = NULL;
    BOOST_REQUIRE_MESSAGE(leveldb::DB::Open(options, (dir / "txleveldb").string(), &db).ok(),
                          "shared txleveldb must remain openable after migration");

    // Required non-block-index namespace survived.
    {
        CDataStream k(SER_DISK, CLIENT_VERSION); k << std::string("version");
        std::string v;
        BOOST_REQUIRE(db->Get(leveldb::ReadOptions(), k.str(), &v).ok());
        CDataStream ss(v.data(), v.data() + v.size(), SER_DISK, CLIENT_VERSION);
        uint32_t ver = 0; ss >> ver;
        BOOST_REQUIRE_EQUAL(ver, (uint32_t)12345);
    }

    // Legacy block-index keys remain as PHYSICAL, non-authoritative residue.
    {
        CDataStream start(SER_DISK, CLIENT_VERSION);
        start << make_pair(std::string("blockindex"), uint256(0));
        leveldb::Iterator* it = db->NewIterator(leveldb::ReadOptions());
        it->Seek(start.str());
        int n = 0;
        while (it->Valid())
        {
            CDataStream sk(SER_DISK, CLIENT_VERSION);
            sk.write(it->key().data(), it->key().size());
            std::string t; sk >> t;
            if (t != "blockindex") break;
            ++n; it->Next();
        }
        delete it;
        BOOST_REQUIRE(n > 0);
    }
    delete db;

    BOOST_REQUIRE(!fs::exists(dir / "txleveldb.old"));
    fs::remove_all(dir);
}

// ---------------------------------------------------------------------------
// C2 failure: bad migration source -> no CURRENT, no authority transfer
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(c2_failure_no_current_on_bad_source)
{
    // (a) legacy present but no hashBestChain -> builder cannot produce authority.
    {
        const fs::path dir = C2TempDir("c2f-nobest");
        C2BuildSyntheticLegacy(dir, 2, /*fWriteBestChain=*/false);
        const fs::path v2root = dir / "blockindex-v2";
        C2CloseLegacyGlobal();
        std::string err;
        BOOST_REQUIRE(PrepareNativeBlockIndexGeneration(
                          v2root.string(), dir.string(),
                          BLOCK_INDEX_STARTUP_OWNERSHIP_LEGACY_MIGRATION_REQUIRED, &err) ==
                      BLOCK_INDEX_NATIVE_PREPARE_FAILED);
        BOOST_REQUIRE(!fs::exists(v2root / kCurrentName));
        fs::remove_all(dir);
    }
    // (b) records present but block data absent -> nSize unavailable -> fails closed.
    {
        const fs::path dir = C2TempDir("c2f-noblk");
        std::vector<uint256> hashes = C2BuildSyntheticLegacy(dir, 2, true, /*fWriteBlockFiles=*/false);
        (void)hashes;
        const fs::path v2root = dir / "blockindex-v2";
        C2CloseLegacyGlobal();
        std::string err;
        BOOST_REQUIRE(PrepareNativeBlockIndexGeneration(
                          v2root.string(), dir.string(),
                          BLOCK_INDEX_STARTUP_OWNERSHIP_LEGACY_MIGRATION_REQUIRED, &err) ==
                      BLOCK_INDEX_NATIVE_PREPARE_FAILED);
        BOOST_REQUIRE(!fs::exists(v2root / kCurrentName));
        fs::remove_all(dir);
    }
    // (c) EMPTY_NEW into an unwritable root -> fails closed, no CURRENT.
    {
        const fs::path dir = C2TempDir("c2f-unwritable");
        const fs::path notADir = dir / "afile";
        { FILE* f = fopen(notADir.string().c_str(), "wb"); BOOST_REQUIRE(f != NULL); fclose(f); }
        std::string err;
        BOOST_REQUIRE(PrepareNativeBlockIndexGeneration(
                          (notADir / "child").string(), dir.string(),
                          BLOCK_INDEX_STARTUP_OWNERSHIP_EMPTY_NEW, &err) ==
                      BLOCK_INDEX_NATIVE_PREPARE_FAILED);
        fs::remove_all(dir);
    }
}

BOOST_AUTO_TEST_SUITE_END()
