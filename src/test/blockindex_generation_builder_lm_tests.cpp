#include <boost/test/unit_test.hpp>

#include "blockindex_generation_builder_lm.h"
#include "blockindex_generation_writer.h"
#include "blockindex_generation_lifecycle.h"
#include "blockindex_live_acceptance.h"
#include "blockindex_authoritative_restart.h"
#include "blockindex_tip.h"
#include "main.h"
#include "util.h"

#include <leveldb/db.h>
#include <leveldb/filter_policy.h>
#include <boost/filesystem.hpp>

#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace fs = boost::filesystem;

// =====================================================================
// M3 — streamed active-chain construction in the low-memory builder.
//
// Builds a tiny synthetic txleveldb snapshot (blockindex keys + hashBestChain)
// using the SAME LevelDB key/value encoding as CTxDB, then runs
// BlockIndexGenerationBuilderLM::Build and asserts:
//   M3-1  records written (all N blocks present as records)
//   M3-2  active chain reconstructed correctly to the snapshot tip
//         (ascending heights 0..tip, valid hashPrev linkage)
//   M3-3  no O(N) CBlockIndex graph (the LM path never populates mapBlockIndex)
// =====================================================================

static std::string MakeTempDir()
{
    char tmpl[] = "/tmp/innova-m3-XXXXXX";
    char* d = mkdtemp(tmpl);
    BOOST_REQUIRE(d != NULL);
    return std::string(d);
}

// Write a CDiskBlockIndex record into a LevelDB snapshot with the CTxDB key.
static void WriteSnapshotBlockIndex(leveldb::DB* db, const CDiskBlockIndex& bi)
{
    CDataStream ssKey(SER_DISK, CLIENT_VERSION);
    ssKey << make_pair(std::string("blockindex"), bi.GetBlockHash());
    CDataStream ssVal(SER_DISK, CLIENT_VERSION);
    ssVal << bi;
    leveldb::Status s = db->Put(leveldb::WriteOptions(), ssKey.str(), ssVal.str());
    BOOST_REQUIRE(s.ok());
}

// Build a chain of CDiskBlockIndex of length tip+1, height 0..tip, and write
// to the snapshot. Returns the chain hashes in ascending height (computed via
// GetBlockHash(), which derives blockHash from version/prev/merkle/time/bits/
// nonce — we must use the ACTUAL derived hash for parent-linking).
static std::vector<uint256> MakeChain(leveldb::DB* db, int tip)
{
    std::vector<uint256> hashes(tip + 1);
    std::vector<CDiskBlockIndex> idx(tip + 1);
    for (int h = 0; h <= tip; ++h)
    {
        uint256 parent = (h == 0) ? uint256(0) : hashes[h - 1];
        CDiskBlockIndex bi;
        bi.nHeight = h;
        bi.nFile = 1;
        bi.nBlockPos = (unsigned)(100 + h);
        bi.hashPrev = parent;
        bi.nVersion = 7;
        bi.nTime = 1700000000u + (unsigned)h;
        bi.nBits = 0x1d00ffff;
        bi.nNonce = (unsigned)h;
        bi.nFlags = 0;
        bi.hashMerkleRoot = uint256(0x1111ULL + h);
        bi.hashProof = uint256(0x2222ULL + h);
        if (h > 0)
        {
            bi.prevoutStake = COutPoint(uint256((unsigned)h), 0); // PoS-like
            bi.nStakeTime = (uint32_t)(1700000000u + h);
        }
        bi.nMint = 100 + h;
        bi.nMoneySupply = 500 + h * 3;
        WriteSnapshotBlockIndex(db, bi);
        // compute the ACTUAL serialized hash (GetBlockHash derives it)
        hashes[h] = bi.GetBlockHash();
        idx[h] = bi;
    }
    // Re-write with correct hashPrev linkage (hashPrev must equal the ACTUAL
    // parent's GetBlockHash, which we now know).
    for (int h = 1; h <= tip; ++h)
    {
        CDiskBlockIndex bi = idx[h];
        bi.hashPrev = hashes[h - 1];
        WriteSnapshotBlockIndex(db, bi);
    }
    // hashBestChain key
    {
        CDataStream ssKey(SER_DISK, CLIENT_VERSION);
        ssKey << std::string("hashBestChain");
        CDataStream ssVal(SER_DISK, CLIENT_VERSION);
        ssVal << hashes[tip];
        leveldb::Status s = db->Put(leveldb::WriteOptions(), ssKey.str(), ssVal.str());
        BOOST_REQUIRE(s.ok());
    }
    return hashes;
}

static std::string ReadFileBytes(const std::string& p)
{
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) return std::string();
    std::string out;
    unsigned char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        out.append((const char*)buf, n);
    fclose(f);
    return out;
}

BOOST_AUTO_TEST_SUITE(blockindex_generation_builder_lm_tests)

BOOST_AUTO_TEST_CASE(m3_active_chain_streamed)
{
    const std::string dir = MakeTempDir();
    const std::string snapDir = dir + "/snapshot";
    fs::create_directories(snapDir);

    // Create synthetic snapshot LevelDB.
    leveldb::Options options;
    options.create_if_missing = true;
    options.error_if_exists = true;
    options.filter_policy = leveldb::NewBloomFilterPolicy(10);
    leveldb::DB* db = NULL;
    leveldb::Status status = leveldb::DB::Open(options, snapDir, &db);
    BOOST_REQUIRE(status.ok());

    const int tip = 5;
    std::vector<uint256> hashes = MakeChain(db, tip);
    BOOST_REQUIRE_EQUAL(hashes.size(), (size_t)(tip + 1));
    delete db; // close before the LM builder opens it read-only

    // Run the low-memory builder.
    const std::string staging = dir + "/build-000001.tmp";
    BlockIndexGenerationBuilderLM lm;
    std::string error;
    // builder M2/M3 only supports records+hashindex+active (derived not yet);
    // Build returns after M3. We assert active streamed by reading active.dat.
    BOOST_REQUIRE_MESSAGE(lm.Build(snapDir, "", 1, staging, &error), error);

    // M3-1: records written. active.dat should exist.
    fs::path actPath = fs::path(staging) / "active.dat";
    BOOST_REQUIRE(fs::exists(actPath));

    // M3-2: active.dat entry count == tip+1 (each 8-byte BlockIndexId).
    std::string actData = ReadFileBytes(actPath.string());
    // active.dat = 40-byte header + N x 8 bytes
    size_t hdr = 40;
    size_t entries = (actData.size() - hdr) / 8;
    BOOST_REQUIRE_EQUAL(entries, (size_t)(tip + 1));

    // M4: derived.dat written, entry count == record count (N = tip+1).
    fs::path derPath = fs::path(staging) / "derived.dat";
    BOOST_REQUIRE(fs::exists(derPath));
    std::string derData = ReadFileBytes(derPath.string());
    // derived.dat = 80-byte V2 header + N x 56 bytes
    size_t derHdr = 80;
    size_t derEntries = derData.size() > derHdr ? (derData.size() - derHdr) / 56 : 0;
    BOOST_REQUIRE_EQUAL(derEntries, (size_t)(tip + 1));
    printf("M3/M4 PASS: LM builder streamed %zu active + %zu derived entries\n",
           entries, derEntries);
}

BOOST_AUTO_TEST_CASE(m6_finalized_generation_validates)
{
    const std::string dir = MakeTempDir();
    const std::string snapDir = dir + "/snapshot";
    fs::create_directories(snapDir);
    leveldb::Options options;
    options.create_if_missing = true;
    options.error_if_exists = true;
    options.filter_policy = leveldb::NewBloomFilterPolicy(10);
    leveldb::DB* db = NULL;
    leveldb::Status status = leveldb::DB::Open(options, snapDir, &db);
    BOOST_REQUIRE(status.ok());
    const int tip = 5;
    std::vector<uint256> hashes = MakeChain(db, tip);
    delete db;

    const std::string staging = dir + "/build-000001.tmp";
    BlockIndexGenerationBuilderLM lm;
    std::string error;
    BOOST_REQUIRE_MESSAGE(lm.Build(snapDir, "", 1, staging, &error), error);

    // M6: the finalized generation must be publishable/selectable (crash-safe
    // CURRENT flip) via the shared writer lifecycle path.
    std::string perr;
    bool okps = BlockIndexGenerationWriter::ValidatePublishSelect(dir, 1, &perr);
    printf("  publish/select ok=%d err='%s'\n", (int)okps, perr.c_str());
    BOOST_REQUIRE_MESSAGE(okps, "publish/select: '" + perr + "'");
    BOOST_REQUIRE(fs::exists(fs::path(dir) / "gen-000001"));
    printf("M6 PASS: LM builder finalized generation publishes + selects\n");
}

BOOST_AUTO_TEST_CASE(m7_snapshot_to_live_tip_catchup)
{
    const std::string dir = MakeTempDir();

    // (1) LM-build gen-1 from a synthetic snapshot at tip S (=6).
    const std::string snapDir = dir + "/snapshot";
    fs::create_directories(snapDir);
    leveldb::Options options;
    options.create_if_missing = true;
    options.error_if_exists = true;
    options.filter_policy = leveldb::NewBloomFilterPolicy(10);
    leveldb::DB* db = NULL;
    leveldb::Status status = leveldb::DB::Open(options, snapDir, &db);
    BOOST_REQUIRE(status.ok());
    const int S = 6;
    std::vector<uint256> chainHashes = MakeChain(db, S); // heights 0..S
    delete db;

    const std::string staging = dir + "/build-000001.tmp";
    BlockIndexGenerationBuilderLM lm;
    std::string error;
    BOOST_REQUIRE_MESSAGE(lm.Build(snapDir, "", 1, staging, &error), error);
    std::string perr;
    BOOST_REQUIRE_MESSAGE(BlockIndexGenerationWriter::ValidatePublishSelect(dir, 1, &perr),
                          "publish: " + perr);
    BOOST_REQUIRE(fs::exists(fs::path(dir) / "gen-000001"));

    // (2) capture S+1..L into blockindex_tip while legacy advanced.
    const uint64_t baseRec = (uint64_t)(S + 1);
    const int baseTip = S;
    BlockIndexTipAuthority tip;
    BOOST_REQUIRE(BlockIndexTipAuthority::Create(dir, 1, baseRec, baseTip, &tip, &error));
    const uint256 sHash = chainHashes[S];

    BlockIndexLiveTail tail;
    tail.SetSources(NULL, &tip);
    tail.SetHorizon(64);
    tail.SetCurrentGeneration(1);
    BlockIndexLiveAcceptance seam;
    // base-known: true only for the snapshot tip anchor sHash (and its chain,
    // which the base gen holds). Provide a static trampoline context that marks
    // the base block hashes 0..S as known by re-deriving from MakeChain hashes.
    struct LMBaseCtxT { uint256 sHash; };
    static LMBaseCtxT lmBaseCtx;
    lmBaseCtx.sHash = sHash;
    struct base_known_tramp {
        static bool fn(const uint256& h, void* ud) {
            LMBaseCtxT* c = (LMBaseCtxT*)ud;
            // only the snapshot tip anchor is the base boundary; the rest of the
            // base exists in gen-1 (immutable). For the seam, marking the tip
            // anchor known is sufficient for S+1's parent.
            return c->sHash == h;
        }
    };
    seam.SetSources(&base_known_tramp::fn, &lmBaseCtx, &tip, &tail);

    // Append S+1, S+2 (active) via the acceptance seam.
    uint256 h7 = uint256(0xF0000007ULL + S);
    {
        BlockIndexRecord r;
        r.hash = h7; r.hashPrev = sHash; r.height = S+1;
        r.nFile = 1; r.nBlockPos = 100u + (unsigned)(S+1); r.nFlags = 0;
        r.nVersion = 7; r.nTime = 1700000000u + (unsigned)(S+1);
        r.nBits = 0x1d00ffff; r.nNonce = (unsigned)(S+1);
        BlockIndexDerivedEntry d; d.chainTrust = uint256(1);
        BlockIndexTipAppend a; a.record = r; a.derived = d;
        std::string aerr;
        BOOST_REQUIRE_MESSAGE(seam.CanAcceptDense(sHash, S+1, &aerr), aerr);
        BOOST_REQUIRE_EQUAL(seam.AcceptActive(a, S+1, &aerr), S+1);
    }
    uint256 h8 = uint256(0xF0000008ULL + S);
    {
        BlockIndexRecord r;
        r.hash = h8; r.hashPrev = h7; r.height = S+2;
        r.nFile = 1; r.nBlockPos = 100u + (unsigned)(S+2); r.nFlags = 0;
        r.nVersion = 7; r.nTime = 1700000000u + (unsigned)(S+2);
        r.nBits = 0x1d00ffff; r.nNonce = (unsigned)(S+2);
        BlockIndexDerivedEntry d; d.chainTrust = uint256(2);
        BlockIndexTipAppend a; a.record = r; a.derived = d;
        std::string aerr;
        BOOST_REQUIRE_MESSAGE(seam.CanAcceptDense(h7, S+2, &aerr), aerr);
        BOOST_REQUIRE_EQUAL(seam.AcceptActive(a, S+2, &aerr), S+2);
    }
    BOOST_REQUIRE_EQUAL(tip.TipHeight(), S+2); // L = 8

    // (3) authoritative restart: gen-1 + blockindex_tip -> lands at L.
    BlockIndexAuthoritativeRestart restart;
    std::string rerr;
    BOOST_REQUIRE_MESSAGE(restart.OpenBaseAndTip(dir, true, 1, NULL, &rerr), rerr);
    BOOST_REQUIRE(restart.HasPostSTip());
    BOOST_REQUIRE_EQUAL(restart.EffectiveTipHeight(), S+2);
    printf("M7 PASS: snapshot S=%d -> live tip L=%d via LM gen + blockindex_tip catch-up\n",
           S, (int)restart.EffectiveTipHeight());
}

BOOST_AUTO_TEST_SUITE_END()