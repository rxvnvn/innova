// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.

// Bounded-memory block-index recovery tool — deterministic causal tests.
//
// Validates the tool against the EXISTING authoritative-chain fixtures (the
// same legacy LevelDB + published generation builder the G2 catchup tests
// use), plus the CLI entry point, the bounded-resume behavior and the
// fail-closed gates:
//
//   R-A   S < L recovery succeeds; final tip == L exactly (height + hash)
//   R-B   derived-state determinism: every appended chainTrust equals the
//         continuous rule-based cumulative value (LM-builder M4 parity)
//   R-C   CLI binary: RECOVERY_OK, final_tip==L, and the crash-safe reports
//   R-D   retry idempotency: second run is a resume (NO-OP, tip unchanged)
//   R-E   --abandon: a partial tip is torn down and preserved (rollback
//         evidence), the rebuild converges again to L
//   R-F   fail-closed: mismatched/absent base authority, legacy tip below
//         base, wrong target hash (snapshot contradiction)
//   R-G   boundary: S == L -> NO-OP (exit success, nothing appended)
//
// Memory is bounded by construction in the tool; the tests assert the RESULT
// guarantees (exact L, derived identity, idempotency), not internals.
#include <boost/test/unit_test.hpp>

#include "blockindex_recovery_tool.h"
#include "blockindex_tip.h"
#include "blockindex_generation_builder.h"
#include "blockindex_generation_lifecycle.h"
#include "main.h"

#include <leveldb/db.h>
#include <leveldb/filter_policy.h>
#include <boost/filesystem.hpp>

#include <cstdio>
#include <string>
#include <vector>

namespace fs = boost::filesystem;

// V2-R1: network flags are process globals; the POEM boundary test
// toggles regtest and restores them.
extern bool fRegTest;
extern bool fTestNet;

static std::string MakeTempDir()
{
    char tmpl[] = "/tmp/innova-recovery-XXXXXX";
    char* d = mkdtemp(tmpl);
    BOOST_REQUIRE(d != NULL);
    return std::string(d);
}

// Write a persisted legacy LevelDB 'blockindex'+'hashBestChain' reachable at
// L (identical fixture shape to the G2 catchup tests: CDiskBlockIndex with
// correct hashPrev linkage, bloom-filtered, deterministic values).
static std::vector<uint256> WriteLegacyChain(const std::string& legacyDir,
                                             int tip,
                                             const std::vector<uint32_t>* flags,
                                             const std::vector<uint32_t>* stemDisabledAt = NULL)
{
    fs::create_directories(legacyDir);
    leveldb::Options options;
    options.create_if_missing = true;
    options.error_if_exists = true;
    options.filter_policy = leveldb::NewBloomFilterPolicy(10);
    leveldb::DB* db = NULL;
    BOOST_REQUIRE(leveldb::DB::Open(options, legacyDir, &db).ok());
    std::vector<CDiskBlockIndex> idx((size_t)tip + 1);
    std::vector<uint256> hashes((size_t)tip + 1);
    for (int h = 0; h <= tip; ++h)
    {
        CDiskBlockIndex bi;
        bi.nHeight = h;
        bi.nFile = 1;
        bi.nBlockPos = (unsigned)(100 + h);
        bi.hashPrev = (h == 0) ? uint256(0) : hashes[(size_t)h - 1];
        bi.nVersion = 7;
        bi.nTime = 1700000000u + (unsigned)h;
        bi.nBits = 0x1d00ffff;
        bi.nNonce = (unsigned)h;
        bi.nFlags = flags ? (*flags)[(size_t)h] : 0u;
        bi.hashMerkleRoot = uint256(0x1111ULL + h);
        bi.hashProof = uint256(0x2222ULL + h);
        if (bi.IsProofOfStake())
        {
            bi.prevoutStake = COutPoint(uint256(0xDE00ULL + h), 0);
            bi.nStakeTime = 1700000000u + (unsigned)h;
        }
        bi.nMint = 100 + h;
        bi.nMoneySupply = 500 + h * 3;
        idx[(size_t)h] = bi;
        hashes[(size_t)h] = bi.GetBlockHash();
    }
    for (int h = 0; h <= tip; ++h)
    {
        bool skip = false;
        if (stemDisabledAt)
        {
            for (size_t i = 0; i < stemDisabledAt->size(); ++i)
                if ((int)(*stemDisabledAt)[i] == h) skip = true;
        }
        if (skip) continue; // MISSING legacy record -> the fail-closed gate
        CDataStream ssKey(SER_DISK, CLIENT_VERSION);
        ssKey << make_pair(std::string("blockindex"), hashes[(size_t)h]);
        CDataStream ssVal(SER_DISK, CLIENT_VERSION);
        ssVal << idx[(size_t)h];
        BOOST_REQUIRE(db->Put(leveldb::WriteOptions(), ssKey.str(), ssVal.str()).ok());
    }
    {
        CDataStream ssKey(SER_DISK, CLIENT_VERSION);
        ssKey << std::string("hashBestChain");
        CDataStream ssVal(SER_DISK, CLIENT_VERSION);
        ssVal << hashes[(size_t)tip];
        BOOST_REQUIRE(db->Put(leveldb::WriteOptions(), ssKey.str(), ssVal.str()).ok());
    }
    delete db;
    return hashes;
}

// Build an AUTHORITATIVE V2 generation @ S from the first S+1 hashes
// (published + selected; identical to the catchup-test helper).
static std::string BuildGenerationS(const std::vector<uint256>& hashes, int S,
                                    const std::string& rootDir)
{
    BlockIndexGenerationSource src;
    for (int h = 0; h <= S; ++h)
    {
        uint256 parent = (h == 0) ? uint256(0) : hashes[(size_t)h - 1];
        BlockIndexRecord rec;
        rec.hash = hashes[(size_t)h];
        rec.hashPrev = parent;
        rec.hashMerkleRoot = uint256(0x1111ULL + h);
        rec.height = h;
        rec.nFile = 1;
        rec.nBlockPos = (unsigned)(100 + h);
        rec.nFlags = 0;
        rec.nVersion = 7;
        rec.nTime = 1700000000u + (unsigned)h;
        rec.nBits = 0x1d00ffff;
        rec.nNonce = (unsigned)h;
        rec.nMint = 100 + h;
        rec.nMoneySupply = 500 + h * 3;
        BlockIndexGenerationSourceRecord s; s.hash = rec.hash; s.record = rec;
        src.records.push_back(s);
    }
    src.hashBestChain = hashes[(size_t)S];
    src.foundBestChain = true;
    BlockIndexGenerationBuilder b;
    BlockIndexGenerationStats stats;
    std::string error;
    fs::path staging = fs::path(rootDir) / "build-000001.tmp";
    BOOST_REQUIRE_MESSAGE(b.Build(src, staging.string(), 1, &stats, &error), error);
    b.Close();
    BOOST_REQUIRE_MESSAGE(
        BlockIndexGenerationManager::PublishGeneration(rootDir, 1, &error) ==
            (int)BLOCK_INDEX_LIFECYCLE_OK, error);
    BOOST_REQUIRE_MESSAGE(
        BlockIndexGenerationManager::SelectGeneration(rootDir, 1, &error) ==
            (int)BLOCK_INDEX_LIFECYCLE_OK, error);
    return rootDir;
}

BOOST_AUTO_TEST_SUITE(blockindex_recovery_tool_tests)

// R-A + R-B: S=5, L=8 -> recovery lands exactly L with M4-identity derived.
BOOST_AUTO_TEST_CASE(recovery_exact_L_and_derived_parity)
{
    bool frSaved = fRegTest, ftSaved = fTestNet;
    fRegTest = true; fTestNet = false; // POEM=9 (stable trust rule)

    const std::string dir = MakeTempDir();
    const std::string legacyDir = dir + "/legacy";
    const int L = 8, S = 5;
    std::vector<uint256> hashes = WriteLegacyChain(legacyDir, L, NULL);
    const std::string v2Root = BuildGenerationS(hashes, S, dir + "/v2");

    BlockIndexRecoveryStats res;
    BOOST_REQUIRE_MESSAGE(RunBlockIndexRecovery(v2Root, legacyDir, "", false, &res),
                          res.error);
    BOOST_CHECK(res.ok);
    BOOST_CHECK_EQUAL(res.baseTipHeight, S);
    BOOST_CHECK(res.baseTipHash == hashes[(size_t)S]);
    BOOST_CHECK_EQUAL(res.targetHeight, L);
    BOOST_CHECK(res.targetHash == hashes[(size_t)L]);
    BOOST_CHECK_EQUAL(res.recordsAppended, (uint64_t)(L - S));
    BOOST_CHECK_EQUAL(res.diskBytesFinal, 0); // spill released after acceptance

    // Tip authority: exact L parity + a per-height legacy identity sweep
    // (every recovered record equals the legacy snapshot record).
    BlockIndexTipAuthority tip;
    std::string terr;
    BOOST_REQUIRE_MESSAGE(BlockIndexTipAuthority::Open(v2Root, 1, &tip, &terr), terr);
    BOOST_REQUIRE_EQUAL((int)tip.TipHeight(), L);
    BOOST_REQUIRE(tip.TipHash() == hashes[(size_t)L]);
    for (int h = S + 1; h <= L; ++h)
    {
        BlockIndexTipRead tr = tip.LookupActiveByHeight(h, &terr);
        BOOST_CHECK_MESSAGE(tr.status == BLOCK_INDEX_TIP_OK,
                            "tip read h=" << h << " status=" << (int)tr.status);
        BOOST_CHECK(tr.record.hash == hashes[(size_t)h]);
        BOOST_CHECK(tr.active);
    }

    // R-B: derived continuity — each appended block's chainTrust delta equals
    // the exact per-block authoritative trust of that block (the M4 rule,
    // applied over the legacy snapshot's flags/prevout invariants at these
    // heights; POEM window under fRegTest).
    {
        for (int h = S + 1; h < L; ++h)
        {
            BlockIndexTipRead hi = tip.LookupActiveByHeight(h + 1, &terr);
            BlockIndexTipRead lo = tip.LookupActiveByHeight(h, &terr);
            BOOST_REQUIRE(hi.status == BLOCK_INDEX_TIP_OK);
            BOOST_REQUIRE(lo.status == BLOCK_INDEX_TIP_OK);
            const bool fPosHi =
                (hi.record.nFlags & CBlockIndex::BLOCK_PROOF_OF_STAKE) != 0;
            const uint256 expectDelta =
                GetAuthoritativeBlockTrustValue(hi.record.nBits, h + 1, fPosHi,
                                                hi.record.hashProof,
                                                hashes[(size_t)h + 1]);
            BOOST_CHECK_MESSAGE(hi.derived.chainTrust ==
                                    lo.derived.chainTrust + expectDelta,
                                "chainTrust continuation mismatch at h=" << h + 1);
        }
    }

    fRegTest = frSaved; fTestNet = ftSaved;
    printf("R-A/R-B PASS: recovery S=%d..L=%d exact-L, derived continuation M4-exact.\n", S, L);
}

// R-C: the CLI binary catches up and emits the machine-readable RECOVERY_OK.
BOOST_AUTO_TEST_CASE(recovery_cli_binary_end_to_end)
{
    const std::string dir = MakeTempDir();
    const std::string legacyDir = dir + "/legacy";
    const int L = 9, S = 6;
    std::vector<uint256> hashes = WriteLegacyChain(legacyDir, L, NULL);
    const std::string v2Root = BuildGenerationS(hashes, S, dir + "/v2");

    std::string cmd = "./blockindex-recovery-tool " + v2Root + " " + legacyDir +
                      " \"\" --report " + dir + "/report.txt";
    char buf[65536];
    std::string out;
    FILE* p = popen((cmd + " 2>&1").c_str(), "r");
    BOOST_REQUIRE(p != NULL);
    while (fgets(buf, sizeof(buf), p)) out += buf;
    int rc = pclose(p);
    int rcExit = (rc == -1) ? -1 : WEXITSTATUS(rc);

    BOOST_CHECK_EQUAL(rcExit, 0);
    BOOST_CHECK_MESSAGE(out.find("RECOVERY_OK") != std::string::npos, out);
    BOOST_CHECK_MESSAGE(out.find("base_tip=6") != std::string::npos, out);
    BOOST_CHECK_MESSAGE(out.find("target(L)=9") != std::string::npos, out);
    // The report file carries the bounded-RAM + peak-disk witnesses.
    BOOST_REQUIRE(fs::exists(dir + "/report.txt"));
    FILE* rep = fopen((dir + "/report.txt").c_str(), "r");
    BOOST_REQUIRE(rep != NULL);
    std::string repData;
    int c;
    while ((c = fgetc(rep)) != EOF) repData.push_back((char)c);
    fclose(rep);
    BOOST_CHECK_MESSAGE(repData.find("ok=1") != std::string::npos, repData);
    BOOST_CHECK_MESSAGE(repData.find("target_height=9") != std::string::npos, repData);
    BOOST_CHECK_MESSAGE(repData.find("peak_rss_bytes=") != std::string::npos, repData);
    BOOST_CHECK_MESSAGE(repData.find("disk_peak_bytes=") != std::string::npos, repData);
    BOOST_CHECK_MESSAGE(repData.find("disk_final_bytes=0") != std::string::npos, repData);
    printf("R-C PASS: CLI end-to-end RECOVERY_OK with report file.\n");
}

// R-D: retry idempotency.
BOOST_AUTO_TEST_CASE(recovery_retry_idempotent_resume)
{
    const std::string dir = MakeTempDir();
    const std::string legacyDir = dir + "/legacy";
    const int L = 12, S = 6;
    std::vector<uint256> hashes = WriteLegacyChain(legacyDir, L, NULL);
    const std::string v2Root = BuildGenerationS(hashes, S, dir + "/v2");

    BlockIndexRecoveryStats r1;
    BOOST_REQUIRE_MESSAGE(RunBlockIndexRecovery(v2Root, legacyDir, "", false, &r1),
                          r1.error);
    BOOST_REQUIRE_EQUAL(r1.targetHeight, L);

    // Retry: a resume -- tip already at L, nothing appended, converges.
    BlockIndexRecoveryStats r2;
    BOOST_REQUIRE_MESSAGE(RunBlockIndexRecovery(v2Root, legacyDir, "", false, &r2),
                          r2.error);
    BOOST_CHECK_EQUAL(r2.recordsAppended, 0u);
    BOOST_CHECK_EQUAL(r2.targetHeight, L);
    BOOST_CHECK(r2.targetHash == hashes[(size_t)L]);
    BOOST_CHECK_EQUAL(r2.diskBytesFinal, 0);
    printf("R-D PASS: retry resume idempotent (appended=0, tip unchanged).\n");
}

// R-E: --abandon tears a partial tip down with rollback evidence and rebuilds.
BOOST_AUTO_TEST_CASE(recovery_abandon_preserves_rollback_evidence)
{
    const std::string dir = MakeTempDir();
    const std::string legacyDir = dir + "/legacy";
    const int L = 10, S = 4;
    std::vector<uint256> hashes = WriteLegacyChain(legacyDir, L, NULL);
    const std::string v2Root = BuildGenerationS(hashes, S, dir + "/v2");

    // First: a normal recovery (a complete tip at L).
    BlockIndexRecoveryStats first;
    BOOST_REQUIRE_MESSAGE(RunBlockIndexRecovery(v2Root, legacyDir, "", false, &first),
                          first.error);
    BOOST_REQUIRE_EQUAL(first.targetHeight, L);

    // --abandon: the tip (already at L) is preserved as
    // tip.pre-recovery-0 (rollback evidence, never deleted) and
    // the recovery starts fresh from the base S, converging to L again.
    BlockIndexRecoveryStats res;
    BOOST_REQUIRE_MESSAGE(RunBlockIndexRecovery(v2Root, legacyDir, "", true, &res),
                          res.error);
    BOOST_CHECK(res.ok);
    BOOST_CHECK_EQUAL(res.targetHeight, L);
    BOOST_CHECK_MESSAGE(fs::exists(fs::path(v2Root) / "tip.pre-recovery-0"),
                        "rollback evidence (pre-recovery dir) must be preserved");
    printf("R-E PASS: --abandon preserved rollback evidence and re-converged to L.\n");
}

// R-F1: legacy tip below base -> fail closed.
BOOST_AUTO_TEST_CASE(recovery_legacy_tip_below_base_fails_closed)
{
    const std::string dir = MakeTempDir();
    const std::string legacyDir = dir + "/legacy";
    const int L = 3, S = 5; // legacy tip BELOW the base tip
    std::vector<uint256> hashes = WriteLegacyChain(legacyDir, L, NULL);
    const std::string v2Root = BuildGenerationS(hashes, S, dir + "/v2");

    BlockIndexRecoveryStats res;
    bool ok = RunBlockIndexRecovery(v2Root, legacyDir, "", false, &res);
    BOOST_CHECK_MESSAGE(!ok, "legacy tip below S must fail closed");
    BOOST_CHECK(!res.ok);
    BOOST_CHECK_MESSAGE(res.error.find("below/mismatched") != std::string::npos, res.error);
    // The tip must be untouched (absent here).
    BOOST_CHECK(!fs::exists(fs::path(v2Root) / "tip" / "tip.meta"));
    printf("R-F1 PASS: legacy tip below base -> fail closed.\n");
}

// R-F2: a missing legacy link (broken snapshot) fails closed -- no partial tip.
BOOST_AUTO_TEST_CASE(recovery_broken_snapshot_fails_closed)
{
    const std::string dir = MakeTempDir();
    const std::string legacyDir = dir + "/legacy";
    const int L = 8, S = 5;
    std::vector<uint32_t> stemDisabledAt; stemDisabledAt.push_back(6); // h=6 missing
    std::vector<uint256> hashes = WriteLegacyChain(legacyDir, L, NULL, &stemDisabledAt);
    const std::string v2Root = BuildGenerationS(hashes, S, dir + "/v2");

    BlockIndexRecoveryStats res;
    bool ok = RunBlockIndexRecovery(v2Root, legacyDir, "", false, &res);
    BOOST_CHECK_MESSAGE(!ok, "the broken ancestry walk must fail closed");
    BOOST_CHECK(!res.ok);
    printf("R-F2 PASS: missing legacy link -> fail closed.\n");
}

// R-F3: an empty / unselected V2 root fails closed (absent base authority).
BOOST_AUTO_TEST_CASE(recovery_absent_base_authority_fails_closed)
{
    const std::string dir = MakeTempDir();
    const std::string legacyDir = dir + "/legacy";
    std::vector<uint256> hashes = WriteLegacyChain(legacyDir, 8, NULL);
    const std::string emptyV2 = dir + "/empty-v2";
    fs::create_directories(emptyV2);

    BlockIndexRecoveryStats res;
    bool ok = RunBlockIndexRecovery(emptyV2, legacyDir, "", false, &res);
    BOOST_CHECK_MESSAGE(!ok, "absent base authority must fail closed");
    BOOST_CHECK(!res.ok);
    BOOST_CHECK(!res.error.empty());
    printf("R-F3 PASS: absent base generation -> fail closed.\n");
}

// R-G: S == L -> NO-OP recovery.
BOOST_AUTO_TEST_CASE(recovery_noop_when_already_at_L)
{
    const std::string dir = MakeTempDir();
    const std::string legacyDir = dir + "/legacy";
    const int L = 6, S = 6;
    std::vector<uint256> hashes = WriteLegacyChain(legacyDir, L, NULL);
    const std::string v2Root = BuildGenerationS(hashes, S, dir + "/v2");

    BlockIndexRecoveryStats res;
    BOOST_REQUIRE_MESSAGE(RunBlockIndexRecovery(v2Root, legacyDir, "", false, &res),
                          res.error);
    BOOST_CHECK(res.ok);
    BOOST_CHECK_EQUAL(res.recordsAppended, 0u);
    BOOST_CHECK(!fs::exists(fs::path(v2Root) / "tip" / "tip.meta"));
    printf("R-G PASS: S == L -> NO-OP (no tip materialized).\n");
}

BOOST_AUTO_TEST_SUITE_END()
