#include <boost/test/unit_test.hpp>

#include "blockindex_generation_builder_lm.h"
#include "blockindex_generation_builder.h"
#include "blockindex_generation_writer.h"
#include "blockindex_generation_lifecycle.h"
#include "blockindex_live_acceptance.h"
#include "blockindex_authoritative_restart.h"
#include "blockindex_authoritative_startup.h"
#include "blockindex_v2_reader.h"
#include "candidate_frontier_metadata.h"
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

// ---- V2-R1 (PM1-P0-08 / PM1-P0-10) focused regression fixtures ----
extern bool fRegTest;
extern bool fTestNet;
extern bool g_lmV2R1ForceDerivedLookupError;
extern bool g_lmV2R1ForceDerivedPutFailure;
extern bool g_cfmV2R1ForceMarkerLookupError;

// RAII restorer so an aborted (BOOST_REQUIRE) case cannot leave a seam armed.
struct V2R1SeamGuard
{
    bool* p;
    bool saved;
    explicit V2R1SeamGuard(bool* flag) : p(flag), saved(*flag) {}
    ~V2R1SeamGuard() { *p = saved; }
};

struct LmChainBlock { uint256 hash; CDiskBlockIndex disk; };

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

// ---- V2-R1 focused helpers ----

static leveldb::DB* OpenSnapshot(const std::string& snapDir)
{
    fs::create_directories(snapDir);
    leveldb::Options options;
    options.create_if_missing = true;
    options.error_if_exists = true;
    options.filter_policy = leveldb::NewBloomFilterPolicy(10);
    leveldb::DB* db = NULL;
    BOOST_REQUIRE(leveldb::DB::Open(options, snapDir, &db).ok());
    return db;
}

// Build a deterministic single chain 0..tip with per-height nFlags, writing
// CDiskBlockIndex records + hashBestChain into the snapshot DB. When
// posNullPrevout is true, a PoS-flagged block is written WITHOUT a stake source
// (nFlags PoS but NULL prevoutStake) -> an ambiguous classification.
static std::vector<LmChainBlock> MakeFlagsChain(leveldb::DB* db, int tip,
                                                const std::vector<uint32_t>& flagsByHeight,
                                                bool posNullPrevout)
{
    std::vector<LmChainBlock> chain(tip + 1);
    for (int h = 0; h <= tip; ++h)
    {
        CDiskBlockIndex bi;
        bi.nHeight = h;
        bi.nFile = 1;
        bi.nBlockPos = (unsigned)(100 + h);
        bi.hashPrev = (h == 0) ? uint256(0) : chain[h - 1].hash;
        bi.nVersion = 7;
        bi.nTime = 1700000000u + (unsigned)h;
        bi.nBits = 0x1d00ffff;
        bi.nNonce = (unsigned)h;
        bi.nFlags = flagsByHeight[h];
        bi.hashMerkleRoot = uint256(0x1111ULL + h);
        bi.hashProof = uint256(0x2222ULL + h);
        if (bi.IsProofOfStake())
        {
            bi.nStakeTime = 1700000000u + (unsigned)h;
            if (!posNullPrevout)
                bi.prevoutStake = COutPoint(uint256(0xDE00ULL + h), 0);
        }
        bi.nMint = 100 + h;
        bi.nMoneySupply = 500 + h * 3;
        chain[h].hash = bi.GetBlockHash();
        chain[h].disk = bi;
        WriteSnapshotBlockIndex(db, bi);
    }
    {
        CDataStream ssKey(SER_DISK, CLIENT_VERSION);
        ssKey << std::string("hashBestChain");
        CDataStream ssVal(SER_DISK, CLIENT_VERSION);
        ssVal << chain[tip].hash;
        BOOST_REQUIRE(db->Put(leveldb::WriteOptions(), ssKey.str(), ssVal.str()).ok());
    }
    return chain;
}

// Rule-based expected cumulative chainTrust (SUM of the surviving rule 0..h).
static uint256 ExpectedCumulative(const std::vector<LmChainBlock>& chain,
                                  const std::vector<uint32_t>& flags, int h)
{
    uint256 cum = 0;
    for (int k = 0; k <= h; ++k)
    {
        const bool fPos = (flags[k] & CBlockIndex::BLOCK_PROOF_OF_STAKE) != 0;
        cum = cum + GetAuthoritativeBlockTrustValue(chain[k].disk.nBits, k, fPos,
                                                    chain[k].disk.hashProof, chain[k].hash);
    }
    return cum;
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

// =====================================================================
// V2-R1 (PM1-P0-08) — migration trust parity across every writer path.
// =====================================================================

// T1/T2: mainnet pre-POEM reciprocal, including a pre-POEM admissible PoS.
BOOST_AUTO_TEST_CASE(v2r1_mainnet_pre_poem_trust_parity)
{
    bool frSaved = fRegTest, ftSaved = fTestNet;
    fRegTest = false; fTestNet = false; // mainnet: POEM/DAG inert
    const std::string dir = MakeTempDir();
    const std::string snapDir = dir + "/snapshot";
    leveldb::DB* db = OpenSnapshot(snapDir);
    const int tip = 3;
    std::vector<uint32_t> flags(tip + 1, 0u);
    flags[1] |= CBlockIndex::BLOCK_PROOF_OF_STAKE; // pre-POEM PoS (reciprocal branch)
    std::vector<LmChainBlock> chain = MakeFlagsChain(db, tip, flags, false);
    delete db;

    BlockIndexGenerationBuilderLM lm; std::string lerr;
    BOOST_REQUIRE_MESSAGE(lm.Build(snapDir, "", 1, dir + "/build-000001.tmp", &lerr), lerr);

    std::string perr;
    BOOST_REQUIRE_MESSAGE(BlockIndexGenerationWriter::ValidatePublishSelect(dir, 1, &perr), perr);
    BlockIndexV2Reader reader; BlockIndexV2ReaderOptions ropts; std::string rerr;
    BOOST_REQUIRE_MESSAGE(reader.Open(dir, ropts, &rerr), rerr);

    for (int h = 0; h <= tip; ++h)
    {
        const bool fPos = (flags[h] & CBlockIndex::BLOCK_PROOF_OF_STAKE) != 0;
        const uint256 btRule = GetAuthoritativeBlockTrustValue(chain[h].disk.nBits, h, fPos,
                                                               chain[h].disk.hashProof, chain[h].hash);
        CBlockIndex ci; ci.phashBlock = &chain[h].hash; ci.nHeight = h;
        ci.nBits = chain[h].disk.nBits; ci.nFlags = chain[h].disk.nFlags; ci.hashProof = chain[h].disk.hashProof;
        BOOST_CHECK_MESSAGE(ci.GetBlockTrust() == btRule, "legacy member != rule at h=" << h);
        BlockIndexSnapshot vs; vs.nBits = chain[h].disk.nBits; vs.height = h;
        vs.fProofOfStake = fPos; vs.hashProof = chain[h].disk.hashProof; vs.hash = chain[h].hash;
        BOOST_CHECK_MESSAGE(GetAuthoritativeBlockTrust(vs) == btRule, "by-value != rule at h=" << h);
        BlockIndexSnapshot snap; std::string gerr;
        BOOST_REQUIRE_EQUAL(reader.GetActiveByHeight(h, &snap, &gerr), BLOCK_INDEX_V2_READ_FOUND);
        BOOST_CHECK_MESSAGE(snap.nChainTrust == ExpectedCumulative(chain, flags, h), "LM chainTrust mismatch at h=" << h);
    }
    fRegTest = frSaved; fTestNet = ftSaved;
}

// T3-T8: regtest POEM(9)/DAG(11) boundary. PoW at 0-8 and 11-12; admissible PoS
// in the POEM window (9,10) -> entropy(hashProof); post-POEM PoW -> entropy(hash).
BOOST_AUTO_TEST_CASE(v2r1_poem_dag_boundary_trust_parity)
{
    bool frSaved = fRegTest, ftSaved = fTestNet;
    fRegTest = true; fTestNet = false; // regtest: POEM=9, DAG=11
    BOOST_REQUIRE_EQUAL(GetForkHeightPoem(), 9);
    BOOST_REQUIRE_EQUAL(GetForkHeightDAG(), 11);

    const std::string dir = MakeTempDir();
    const std::string snapDir = dir + "/snapshot";
    leveldb::DB* db = OpenSnapshot(snapDir);
    const int tip = 12;
    std::vector<uint32_t> flags(tip + 1, 0u);
    flags[9]  |= CBlockIndex::BLOCK_PROOF_OF_STAKE;
    flags[10] |= CBlockIndex::BLOCK_PROOF_OF_STAKE;
    std::vector<LmChainBlock> chain = MakeFlagsChain(db, tip, flags, false);
    delete db;

    BlockIndexGenerationBuilderLM lm; std::string lerr;
    BOOST_REQUIRE_MESSAGE(lm.Build(snapDir, "", 1, dir + "/build-000001.tmp", &lerr), lerr);
    std::string perr;
    BOOST_REQUIRE_MESSAGE(BlockIndexGenerationWriter::ValidatePublishSelect(dir, 1, &perr), perr);
    BlockIndexV2Reader reader; BlockIndexV2ReaderOptions ropts; std::string rerr;
    BOOST_REQUIRE_MESSAGE(reader.Open(dir, ropts, &rerr), rerr);

    for (int h = 0; h <= tip; ++h)
    {
        const bool fPos = (flags[h] & CBlockIndex::BLOCK_PROOF_OF_STAKE) != 0;
        const uint256 btRule = GetAuthoritativeBlockTrustValue(chain[h].disk.nBits, h, fPos,
                                                               chain[h].disk.hashProof, chain[h].hash);
        CBlockIndex ci; ci.phashBlock = &chain[h].hash; ci.nHeight = h;
        ci.nBits = chain[h].disk.nBits; ci.nFlags = chain[h].disk.nFlags; ci.hashProof = chain[h].disk.hashProof;
        BOOST_CHECK_MESSAGE(ci.GetBlockTrust() == btRule, "legacy member != rule at h=" << h);
        BlockIndexSnapshot vs; vs.nBits = chain[h].disk.nBits; vs.height = h;
        vs.fProofOfStake = fPos; vs.hashProof = chain[h].disk.hashProof; vs.hash = chain[h].hash;
        BOOST_CHECK_MESSAGE(GetAuthoritativeBlockTrust(vs) == btRule, "by-value != rule at h=" << h);
        BlockIndexSnapshot snap; std::string gerr;
        BOOST_REQUIRE_EQUAL(reader.GetActiveByHeight(h, &snap, &gerr), BLOCK_INDEX_V2_READ_FOUND);
        BOOST_CHECK_MESSAGE(snap.nChainTrust == ExpectedCumulative(chain, flags, h), "LM chainTrust mismatch at h=" << h);
    }
    // Post-DAG PoS rule branch is zero (network rule; not a manufactured input).
    BOOST_CHECK(GetAuthoritativeBlockTrustValue(0x1d00ffff, 11, true, uint256(1), uint256(2)) == uint256(0));
    fRegTest = frSaved; fTestNet = ftSaved;
}

// T9: ambiguous PoS classification (nFlags PoS but NULL prevoutStake) must FAIL
// migration rather than persist an ambiguous trust.
BOOST_AUTO_TEST_CASE(v2r1_malformed_pos_classification_rejected)
{
    bool frSaved = fRegTest, ftSaved = fTestNet;
    fRegTest = true; fTestNet = false;
    const std::string dir = MakeTempDir();
    const std::string snapDir = dir + "/snapshot";
    leveldb::DB* db = OpenSnapshot(snapDir);
    const int tip = 3;
    std::vector<uint32_t> flags(tip + 1, 0u);
    flags[2] |= CBlockIndex::BLOCK_PROOF_OF_STAKE; // PoS flag, NULL stake source
    MakeFlagsChain(db, tip, flags, /*posNullPrevout=*/true);
    delete db;

    BlockIndexGenerationBuilderLM lm; std::string lerr;
    bool ok = lm.Build(snapDir, "", 1, dir + "/build-000001.tmp", &lerr);
    BOOST_CHECK_MESSAGE(!ok, "ambiguous PoS classification must FAIL migration");
    BOOST_CHECK(lerr.find("ambiguous PoS classification") != std::string::npos);
    BOOST_CHECK(!fs::exists(fs::path(dir) / "CURRENT"));
    BOOST_CHECK(!fs::exists(fs::path(dir) / "gen-000001"));
    fRegTest = frSaved; fTestNet = ftSaved;
}

// =====================================================================
// V2-R1 (PM1-P0-10) — fail-closed LM input handling.
// =====================================================================

// M1/M6: disconnected side parent (absent from source) must FAIL closed,
// matching the regular builder's rejection semantics.
BOOST_AUTO_TEST_CASE(v2r1_disconnected_side_parent_fails_closed)
{
    bool frSaved = fRegTest, ftSaved = fTestNet;
    fRegTest = true; fTestNet = false;
    const std::string dir = MakeTempDir();
    const std::string snapDir = dir + "/snapshot";
    leveldb::DB* db = OpenSnapshot(snapDir);
    const int tip = 5;
    std::vector<uint32_t> flags(tip + 1, 0u);
    MakeFlagsChain(db, tip, flags, false); // valid active chain 0..5
    {
        CDiskBlockIndex bi;
        bi.nHeight = 6; bi.nFile = 1; bi.nBlockPos = 200;
        bi.hashPrev = uint256(0xBEEF0001ULL); // absent from the source
        bi.nVersion = 7; bi.nTime = 1700000006u; bi.nBits = 0x1d00ffff; bi.nNonce = 999;
        bi.nFlags = 0; bi.hashMerkleRoot = uint256(0x55ULL); bi.hashProof = uint256(0x66ULL);
        WriteSnapshotBlockIndex(db, bi);
    }
    delete db;

    BlockIndexGenerationBuilderLM lm; std::string lerr;
    bool ok = lm.Build(snapDir, "", 1, dir + "/build-000001.tmp", &lerr);
    BOOST_CHECK_MESSAGE(!ok, "disconnected side parent must FAIL migration");
    BOOST_CHECK(lerr.find("disconnected parent") != std::string::npos);
    BOOST_CHECK(!fs::exists(fs::path(dir) / "CURRENT"));
    fRegTest = frSaved; fTestNet = ftSaved;
}

// M2: a local parent-index Get ERROR (injected) must FAIL closed, never be
// treated as NOT_FOUND.
BOOST_AUTO_TEST_CASE(v2r1_parent_lookup_error_fails_closed)
{
    bool frSaved = fRegTest, ftSaved = fTestNet;
    fRegTest = true; fTestNet = false;
    V2R1SeamGuard sg(&g_lmV2R1ForceDerivedLookupError);
    const std::string dir = MakeTempDir();
    const std::string snapDir = dir + "/snapshot";
    leveldb::DB* db = OpenSnapshot(snapDir);
    const int tip = 3;
    std::vector<uint32_t> flags(tip + 1, 0u);
    MakeFlagsChain(db, tip, flags, false);
    delete db;

    g_lmV2R1ForceDerivedLookupError = true;
    BlockIndexGenerationBuilderLM lm; std::string lerr;
    bool ok = lm.Build(snapDir, "", 1, dir + "/build-000001.tmp", &lerr);
    g_lmV2R1ForceDerivedLookupError = false;
    BOOST_CHECK_MESSAGE(!ok, "injected parent-lookup ERROR must FAIL CLOSED");
    BOOST_CHECK(lerr.find("parent derived lookup failed") != std::string::npos);
    BOOST_CHECK(!fs::exists(fs::path(dir) / "CURRENT"));
    fRegTest = frSaved; fTestNet = ftSaved;
}

// M3: a derived-index Put failure (injected) must FAIL closed before COMPLETE.
BOOST_AUTO_TEST_CASE(v2r1_derived_index_put_failure_fails_closed)
{
    bool frSaved = fRegTest, ftSaved = fTestNet;
    fRegTest = true; fTestNet = false;
    V2R1SeamGuard sg(&g_lmV2R1ForceDerivedPutFailure);
    const std::string dir = MakeTempDir();
    const std::string snapDir = dir + "/snapshot";
    leveldb::DB* db = OpenSnapshot(snapDir);
    const int tip = 3;
    std::vector<uint32_t> flags(tip + 1, 0u);
    MakeFlagsChain(db, tip, flags, false);
    delete db;

    g_lmV2R1ForceDerivedPutFailure = true;
    BlockIndexGenerationBuilderLM lm; std::string lerr;
    bool ok = lm.Build(snapDir, "", 1, dir + "/build-000001.tmp", &lerr);
    g_lmV2R1ForceDerivedPutFailure = false;
    BOOST_CHECK_MESSAGE(!ok, "injected deridx Put failure must FAIL CLOSED");
    BOOST_CHECK(lerr.find("deridx write failed") != std::string::npos);
    BOOST_CHECK(!fs::exists(fs::path(dir) / "CURRENT"));
    fRegTest = frSaved; fTestNet = ftSaved;
}

// M4: candidate parent-marker NOT_FOUND where absence is allowed -> the tip (no
// child) is the single leaf; M5: a marker local ERROR (injected) must FAIL closed.
BOOST_AUTO_TEST_CASE(v2r1_candidate_marker_notfound_ok_error_fails_closed)
{
    bool frSaved = fRegTest, ftSaved = fTestNet;
    fRegTest = true; fTestNet = false;
    const int tip = 2;
    std::vector<uint32_t> flags(tip + 1, 0u);

    // M4: NOT_FOUND where absence is semantically allowed -> the tip (no child)
    // is the single leaf. We exercise the production candidate-leaf function
    // directly: the LM OLD_SHADOW fixture does not reach it via writer Finalize
    // (leaves are materialized on the AUTHORITATIVE path only), but the same
    // function is what the authoritative writer + lifecycle validation consume.
    {
        const std::string dir = MakeTempDir();
        const std::string snapDir = dir + "/snapshot";
        leveldb::DB* db = OpenSnapshot(snapDir);
        std::vector<LmChainBlock> chain = MakeFlagsChain(db, tip, flags, false);
        delete db;
        const std::string stage = dir + "/build-000001.tmp";
        BlockIndexGenerationBuilderLM lm; std::string lerr;
        BOOST_REQUIRE_MESSAGE(lm.Build(snapDir, "", 1, stage, &lerr), lerr);
        BOOST_REQUIRE(!fs::exists(fs::path(stage) / "candidate-leaves.dat"));
        std::string merr;
        BOOST_REQUIRE_MESSAGE(EnsureCandidateLeafMetadata(stage, 1, &merr), merr);
        std::vector<uint256> leaves; std::string rerr;
        BOOST_REQUIRE_MESSAGE(ReadCandidateLeafMetadata(stage, 1, &leaves, &rerr), rerr);
        BOOST_CHECK_EQUAL(leaves.size(), 1u);
        BOOST_CHECK(leaves.size() == 1 && leaves[0] == chain[tip].hash);
    }

    // M5: a local parent-marker lookup ERROR (injected) must FAIL CLOSED: no leaf
    // sidecar is published.
    {
        V2R1SeamGuard sg(&g_cfmV2R1ForceMarkerLookupError);
        const std::string dir = MakeTempDir();
        const std::string snapDir = dir + "/snapshot";
        leveldb::DB* db = OpenSnapshot(snapDir);
        MakeFlagsChain(db, tip, flags, false);
        delete db;
        const std::string stage = dir + "/build-000001.tmp";
        BlockIndexGenerationBuilderLM lm; std::string lerr;
        BOOST_REQUIRE_MESSAGE(lm.Build(snapDir, "", 1, stage, &lerr), lerr);
        g_cfmV2R1ForceMarkerLookupError = true;
        std::string merr;
        bool ok = EnsureCandidateLeafMetadata(stage, 1, &merr);
        g_cfmV2R1ForceMarkerLookupError = false;
        BOOST_CHECK_MESSAGE(!ok, "candidate marker lookup ERROR must FAIL CLOSED");
        BOOST_CHECK(merr.find("candidate leaves marker lookup failed") != std::string::npos);
        BOOST_CHECK(!fs::exists(fs::path(stage) / "candidate-leaves.dat"));
        BOOST_CHECK(!fs::exists(fs::path(dir) / "CURRENT"));
    }
    fRegTest = frSaved; fTestNet = ftSaved;
}

// Section 17: regular builder vs LM builder byte parity for the same fixture
// (identical numeric generation namespace).
BOOST_AUTO_TEST_CASE(v2r1_regular_vs_lm_byte_parity)
{
    bool frSaved = fRegTest, ftSaved = fTestNet;
    fRegTest = true; fTestNet = false;
    const std::string dir = MakeTempDir();
    const std::string snapDir = dir + "/snapshot";
    leveldb::DB* db = OpenSnapshot(snapDir);
    const int tip = 12;
    std::vector<uint32_t> flags(tip + 1, 0u);
    flags[9] |= CBlockIndex::BLOCK_PROOF_OF_STAKE;
    flags[10] |= CBlockIndex::BLOCK_PROOF_OF_STAKE;
    MakeFlagsChain(db, tip, flags, false);
    delete db;

    const std::string regRoot = dir + "/reg"; fs::create_directories(regRoot);
    BlockIndexGenerationSource src; std::string serr;
    BOOST_REQUIRE_MESSAGE(ReadLegacyBlockIndexSource(snapDir, &src, &serr), serr);
    BlockIndexGenerationBuilder rb; BlockIndexGenerationStats stats; std::string berr;
    const std::string regStage = regRoot + "/build-000001.tmp";
    BOOST_REQUIRE_MESSAGE(rb.Build(src, regStage, 1, &stats, &berr), berr);
    rb.Close();

    const std::string lmRoot = dir + "/lm"; fs::create_directories(lmRoot);
    const std::string lmStage = lmRoot + "/build-000001.tmp";
    BlockIndexGenerationBuilderLM lm; std::string lerr;
    BOOST_REQUIRE_MESSAGE(lm.Build(snapDir, "", 1, lmStage, &lerr), lerr);

    // Format-critical component files must be byte-identical for the same fixture
    // and the same numeric generation namespace (both OLD_SHADOW here).
    const char* files[] = {"records.dat","active.dat","derived.dat"};
    for (size_t i = 0; i < sizeof(files)/sizeof(files[0]); ++i)
    {
        std::string a = ReadFileBytes(regStage + "/" + files[i]);
        std::string b = ReadFileBytes(lmStage + "/" + files[i]);
        BOOST_CHECK_MESSAGE(!a.empty() && a == b,
            "regular vs LM byte mismatch for " << files[i]
            << " (reg " << a.size() << " vs lm " << b.size() << ")");
    }
    // MANIFEST: the OLD_SHADOW generation root/content-binding lives in the first
    // 92 bytes and must be identical. The trailing 32-byte dagInputDigest field is
    // informational for OLD_SHADOW (the writer persists the passed DAG digest only
    // on the AUTHORITATIVE path); the regular builder writes SHA256(empty)
    // unconditionally while the LM OLD_SHADOW manifest keeps zeros. That field is
    // NOT part of the OLD_SHADOW root, so we assert the root region and record the
    // shadow-only informational divergence (out of V2-R1 scope: not trust/fail-open).
    {
        std::string mr = ReadFileBytes(regStage + "/MANIFEST");
        std::string ml = ReadFileBytes(lmStage + "/MANIFEST");
        BOOST_REQUIRE_EQUAL(mr.size(), 124u);
        BOOST_REQUIRE_EQUAL(ml.size(), 124u);
        BOOST_CHECK_MESSAGE(mr.substr(0, 92) == ml.substr(0, 92),
            "MANIFEST root/metadata region mismatch (reg vs lm)");
        // Deterministic record of the shadow-only informational divergence:
        const std::string zeros(32, '\0');
        BOOST_CHECK_MESSAGE(ml.substr(92) == zeros,
            "LM OLD_SHADOW MANIFEST dagInputDigest tail expected zeros");
        BOOST_CHECK_MESSAGE(mr.substr(92) != zeros,
            "regular OLD_SHADOW MANIFEST writes SHA256(empty) dagInputDigest (non-zero)");
    }
    fRegTest = frSaved; fTestNet = ftSaved;
}

BOOST_AUTO_TEST_SUITE_END()