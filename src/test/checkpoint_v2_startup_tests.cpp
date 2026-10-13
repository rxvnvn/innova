// V2-R2B (FIX A) REGRESSION — the persisted synchronized checkpoint must be
// restored at V2 authoritative startup.
//
// ROOT CAUSE (production incident): the V2 authoritative startup bypasses
// CTxDB::LoadBlockIndex, which is the legacy boot path that read key
// "hashSyncCheckpoint" into Checkpoints::hashSyncCheckpoint. Under V2 the value
// stayed at its default 0, so once IsInitialBlockDownload() became false,
// CheckSync()'s non-IBD branch (mapBlockIndex.count(hashSyncCheckpoint) == 0)
// rejected EVERY candidate block ("CheckSync: sync-checkpoint not in block
// index"): tip frozen, all miner threads idle.
//
// FIX A: Checkpoints::LoadPersistedSyncCheckpoint() reads the legacy
// tx-leveldb through the typed CTxDB::LoadSyncCheckpoint seam, and init.cpp
// restores the result into Checkpoints::hashSyncCheckpoint right after
// InitBlockIndexAuthoritative(). Documented semantics:
//   * ABSENT key -> hashOut == 0 ("no synchronized checkpoint", canonical
//     ppcoin) -- NEVER a genesis substitution;
//   * LOADED     -> the persisted value;
//   * CORRUPT    -> durable authority inconsistency (caller fails startup).
//
// Covers required cases (1) missing persisted checkpoint, (2) valid persisted
// checkpoint, (3) corrupted persisted checkpoint, (4) V2 startup + restart.
#include <boost/test/unit_test.hpp>
#include <boost/filesystem.hpp>
#include <cstdio>
#include <string>

#include "db.h"
#include "txdb.h"
#include "main.h"
#include "miner.h"
#include "wallet.h"
#include "kernel.h"
#include "checkpoints.h"
#include "blockindex_authoritative_live.h"
#include "blockindex_authoritative_startup.h"
#include "blockindex_manager.h"
#include "blockindex_generation_builder.h"
#include "blockindex_generation_lifecycle.h"
#include "leveldb/db.h"

namespace fs = boost::filesystem;
extern CWallet* pwalletMain;

// ---------------------------------------------------------------- raw key I/O
// The persisted checkpoint lives under the legacy tx-leveldb key
// "hashSyncCheckpoint". These helpers write/erase/poison the RAW entry so the
// loader's ABSENT / CORRUPT branches can be exercised deterministically (the
// public WriteSyncCheckpoint can only ever write a well-formed uint256).
static std::string CkptV2Key()
{
    CDataStream ssKey(SER_DISK, CLIENT_VERSION);
    ssKey << std::string("hashSyncCheckpoint");
    return ssKey.str();
}

static void CkptV2RawPut(const std::string& rawValue)
{
    CTxDB db("cr+");
    leveldb::Status st = db.GetInstance()->Put(leveldb::WriteOptions(), CkptV2Key(), rawValue);
    BOOST_REQUIRE_MESSAGE(st.ok(), st.ToString());
}

static void CkptV2RawDelete()
{
    CTxDB db("cr+");
    leveldb::Status st = db.GetInstance()->Delete(leveldb::WriteOptions(), CkptV2Key());
    BOOST_REQUIRE_MESSAGE(st.ok(), st.ToString());
}

// ---------------------------------------------------------------- mine helpers
static CBlock* CkptV2BuildPoW(CBlockIndex* prev, unsigned int extra)
{
    CBlock* b = CreateNewBlock(pwalletMain, false, NULL, NULL);
    if (!b || b->vtx.empty() || b->vtx[0].vout.empty()) return NULL;
    b->nVersion = 1;
    b->nTime = std::max((unsigned int)GetTime(), (unsigned int)(prev->GetMedianTimePast() + 1));
    b->hashPrevBlock = *prev->phashBlock;
    b->vtx[0].vin[0].scriptSig = CScript() << (prev->nHeight + 1) << extra;
    b->vtx[0].vout[0].scriptPubKey = CScript() << OP_TRUE;
    b->hashMerkleRoot = b->BuildMerkleTree();
    uint256 target = CBigNum().SetCompact(b->nBits).getuint256();
    while (b->GetHash() > target && b->nNonce < 0xffffffff)
        ++b->nNonce;
    return b;
}

static CBlockIndex* CkptV2Mine(CBlockIndex* prev, unsigned int extra)
{
    CBlock* b = CkptV2BuildPoW(prev, extra);
    BOOST_REQUIRE(b != NULL);
    CBlockIndex* idx = NULL;
    {
        LOCK(cs_main);
        const uint256 h = b->GetHash();
        const bool c = b->CheckBlock(true, true, true);
        const bool p = c && ProcessBlock(NULL, b);
        if (p) idx = mapBlockIndex[h];
    }
    delete b;
    BOOST_REQUIRE_MESSAGE(idx != NULL, "block not accepted by ProcessBlock");
    return idx;
}

static void CkptV2BuildRoot(const fs::path& root, std::string* error)
{
    fs::create_directories(root / "snapshot");
    { CTxDB db; db.Close(); }
    const auto live = GetDataDir() / "txleveldb";
    for (fs::directory_iterator it(live), end; it != end; ++it)
        if (fs::is_regular_file(it->path()))
            fs::copy_file(it->path(), root / "snapshot" / it->path().filename());
    BlockIndexGenerationSource source;
    BOOST_REQUIRE_MESSAGE(ReadLegacyBlockIndexSource((root / "snapshot").string(), &source, error), *error);
    source.blockDataDir = GetDataDir().string();
    BlockIndexGenerationBuilder builder;
    BOOST_REQUIRE_MESSAGE(builder.Build(source, (root / "blockindex-build-000001.tmp").string(), 1, NULL, error), *error);
    builder.Close();
    BOOST_REQUIRE_EQUAL(BlockIndexGenerationManager::PublishGeneration(root.string(), 1, error), BLOCK_INDEX_LIFECYCLE_OK);
    BOOST_REQUIRE_EQUAL(BlockIndexGenerationManager::SelectGeneration(root.string(), 1, error), BLOCK_INDEX_LIFECYCLE_OK);
}

// Restore every shared global this suite mutates (mapBlockIndex / candidate
// frontier / stake-seen / tip mirrors / the checkpoint global / the persisted DB
// entry), so later suites inherit the exact prior state.
struct CkptV2SavedState
{
    std::map<uint256, CBlockIndex*> mapBlockIndex;
    std::map<uint256, CandidateTipRecord> mapCandidateTips;
    std::set<std::pair<COutPoint, unsigned int> > setStakeSeen;
    CBlockIndex* pindexBest;
    CBlockIndex* pindexGenesisBlock;
    uint256 hashBestChain;
    int nBestHeight;
    uint256 nBestChainTrust;
    uint256 hashSyncCheckpoint;
    uint256 persistedCheckpoint;
    bool persistedWasAbsent;
    bool hadLivetail;
    std::string livetail;

    CkptV2SavedState()
    {
        mapBlockIndex = ::mapBlockIndex;
        mapCandidateTips = ::mapCandidateTips;
        setStakeSeen = ::setStakeSeen;
        pindexBest = ::pindexBest;
        pindexGenesisBlock = ::pindexGenesisBlock;
        hashBestChain = ::hashBestChain;
        nBestHeight = ::nBestHeight;
        nBestChainTrust = ::nBestChainTrust;
        hashSyncCheckpoint = Checkpoints::hashSyncCheckpoint;
        uint256 tmp = uint256(0);
        persistedWasAbsent = (Checkpoints::LoadPersistedSyncCheckpoint(tmp, NULL) == SYNC_CHECKPOINT_ABSENT);
        persistedCheckpoint = tmp;
        hadLivetail = mapArgs.count("-blockindexlivetail") != 0;
        livetail = hadLivetail ? mapArgs["-blockindexlivetail"] : std::string();
    }

    void Restore(bool fRestorePersisted)
    {
        ResetBlockIndexAuthoritativeStartupForTest();
        ::mapBlockIndex = mapBlockIndex;
        ::mapCandidateTips = mapCandidateTips;
        ::setStakeSeen = setStakeSeen;
        ::pindexBest = pindexBest;
        ::pindexGenesisBlock = pindexGenesisBlock;
        ::hashBestChain = hashBestChain;
        ::nBestHeight = nBestHeight;
        ::nBestChainTrust = nBestChainTrust;
        Checkpoints::hashSyncCheckpoint = hashSyncCheckpoint;
        if (hadLivetail) mapArgs["-blockindexlivetail"] = livetail;
        else mapArgs.erase("-blockindexlivetail");
        if (fRestorePersisted)
        {
            if (persistedWasAbsent) CkptV2RawDelete();
            else { CTxDB db("cr+"); db.WriteSyncCheckpoint(persistedCheckpoint); }
        }
    }
};

BOOST_AUTO_TEST_SUITE(checkpoint_v2_startup_tests)

// (1) missing persisted checkpoint -> ABSENT, hashOut stays 0 ("no synchronized
//     checkpoint"); explicitly NOT a genesis substitution and NOT stale.
BOOST_AUTO_TEST_CASE(missing_persisted_checkpoint_is_absent_not_genesis)
{
    CkptV2SavedState saved;
    CkptV2RawDelete();

    uint256 out = uint256(0xdeadbeefUL);
    std::string err = "sentinel";
    const SyncCheckpointLoadStatus st = Checkpoints::LoadPersistedSyncCheckpoint(out, &err);
    BOOST_CHECK_EQUAL((int)st, (int)SYNC_CHECKPOINT_ABSENT);
    BOOST_CHECK(out == uint256(0));
    BOOST_CHECK_MESSAGE(out != pindexGenesisBlock->GetBlockHash(),
                        "ABSENT must not substitute the genesis hash");
    BOOST_CHECK(err.empty());

    saved.Restore(true);
}

// (2) valid persisted checkpoint -> LOADED with exactly the persisted value.
BOOST_AUTO_TEST_CASE(valid_persisted_checkpoint_loads_exactly)
{
    CkptV2SavedState saved;
    const uint256 sentinel = uint256("0x0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
    { CTxDB db("cr+"); BOOST_REQUIRE(db.WriteSyncCheckpoint(sentinel)); }

    uint256 out = uint256(0);
    std::string err;
    BOOST_CHECK_EQUAL((int)Checkpoints::LoadPersistedSyncCheckpoint(out, &err), (int)SYNC_CHECKPOINT_LOADED);
    BOOST_CHECK(out == sentinel);
    BOOST_CHECK(err.empty());

    saved.Restore(true);
}

// (3) corrupted persisted checkpoint -> CORRUPT (fail closed), never silently
//     reinterpreted as ABSENT / genesis.
BOOST_AUTO_TEST_CASE(corrupt_persisted_checkpoint_fails_closed)
{
    CkptV2SavedState saved;
    CkptV2RawPut("not-a-uint256");   // present but not decodable

    uint256 out = uint256(0xdeadbeefUL);
    std::string err;
    const SyncCheckpointLoadStatus st = Checkpoints::LoadPersistedSyncCheckpoint(out, &err);
    BOOST_CHECK_EQUAL((int)st, (int)SYNC_CHECKPOINT_CORRUPT);
    BOOST_CHECK(out == uint256(0));
    BOOST_CHECK(!err.empty());

    // A well-formed value must still load (the seam distinguishes the two).
    CkptV2RawPut(std::string(32, '\x11'));
    uint256 good = uint256(0);
    BOOST_CHECK_EQUAL((int)Checkpoints::LoadPersistedSyncCheckpoint(good, NULL), (int)SYNC_CHECKPOINT_LOADED);
    BOOST_CHECK(good != uint256(0));

    saved.Restore(true);
}

// (4) V2 authoritative startup + restart: the value restored into
//     Checkpoints::hashSyncCheckpoint comes from the persisted entry (not
//     genesis), and a second (restart) startup reloads it identically.
BOOST_AUTO_TEST_CASE(v2_authoritative_startup_and_restart_reload_checkpoint)
{
    CkptV2SavedState saved;

    CBlockIndex* tip = pindexBest;
    while (tip->nHeight < 6)
        tip = CkptV2Mine(tip, 0xC000u + (unsigned)tip->nHeight);

    // Sentinel checkpoint = a real (non-genesis) chain block, written to the
    // SAME legacy tx-leveldb that the V2 startup leaves active.
    CBlockIndex* at3 = tip;
    while (at3 && at3->nHeight > 3) at3 = at3->pprev;
    BOOST_REQUIRE(at3 != NULL);
    const uint256 sentinel = at3->GetBlockHash();
    BOOST_REQUIRE(sentinel != pindexGenesisBlock->GetBlockHash());
    { CTxDB db("cr+"); BOOST_REQUIRE(db.WriteSyncCheckpoint(sentinel)); }

    const fs::path root = fs::temp_directory_path() / fs::unique_path("ckptv2-%%%%-%%%%");
    std::string error;
    CkptV2BuildRoot(root, &error);
    mapArgs["-blockindexlivetail"] = "32";

    // --- startup #1
    if (!InitBlockIndexAuthoritative(root.string(), &error))
        BOOST_FAIL("InitBlockIndexAuthoritative failed: " + error);
    BOOST_REQUIRE(g_fAuthoritativeStartup);

    // Pre-fix state: the V2 startup left the process checkpoint at 0.
    Checkpoints::hashSyncCheckpoint = uint256(0);
    BOOST_CHECK(Checkpoints::hashSyncCheckpoint == uint256(0));

    uint256 loaded = uint256(0);
    std::string lerr;
    if (Checkpoints::LoadPersistedSyncCheckpoint(loaded, &lerr) != SYNC_CHECKPOINT_LOADED)
        BOOST_FAIL("persisted checkpoint not loaded: " + lerr);
    BOOST_CHECK(loaded == sentinel);
    BOOST_CHECK(loaded != pindexGenesisBlock->GetBlockHash());   // not genesis
    Checkpoints::hashSyncCheckpoint = loaded;                    // FIX A restore
    BOOST_CHECK(Checkpoints::hashSyncCheckpoint == sentinel);

    // --- restart: tear the V2 authority down, start again on the same root
    ResetBlockIndexAuthoritativeStartupForTest();
    if (!InitBlockIndexAuthoritative(root.string(), &error))
        BOOST_FAIL("restart InitBlockIndexAuthoritative failed: " + error);
    BOOST_REQUIRE(g_fAuthoritativeStartup);

    Checkpoints::hashSyncCheckpoint = uint256(0);
    uint256 reloaded = uint256(0);
    BOOST_CHECK_EQUAL((int)Checkpoints::LoadPersistedSyncCheckpoint(reloaded, &lerr), (int)SYNC_CHECKPOINT_LOADED);
    BOOST_CHECK(reloaded == sentinel);

    // --- a startup with NO persisted entry must yield ABSENT (0), not genesis
    CkptV2RawDelete();
    Checkpoints::hashSyncCheckpoint = uint256(0);
    uint256 absentOut = uint256(0xdeadbeefUL);
    BOOST_CHECK_EQUAL((int)Checkpoints::LoadPersistedSyncCheckpoint(absentOut, &lerr), (int)SYNC_CHECKPOINT_ABSENT);
    BOOST_CHECK(absentOut == uint256(0));
    BOOST_CHECK(absentOut != pindexGenesisBlock->GetBlockHash());

    try { fs::remove_all(root); } catch (...) {}
    saved.Restore(true);
}

BOOST_AUTO_TEST_SUITE_END()
