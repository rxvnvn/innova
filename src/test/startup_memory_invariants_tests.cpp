// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.
//
// ENGINEERING INVARIANTS — startup memory / I/O gates (fast tier).
//
// These cases pin the BEHAVIOUR that the startup-memory repairs rely on, not a
// particular source line. Each one fails if a future change reintroduces the
// defect it was written for:
//
//   BOOT-001  the generation is read ONE time to establish record integrity, and
//             any per-record startup projection is collected on that same pass.
//             Here: the reader's integrity-scan visitor must reproduce the
//             setStakeSeen builder's output EXACTLY (same predicate, same
//             records), the scan must be reported as having run, and the
//             verify-once memo must report the fallback honestly on a re-open.
//   MEM-002   setStakeSeen is handed over, never copied: the startup path swaps
//             the built set in. Proven here by the equivalence of the single
//             collected set with the streaming builder oracle (the copy itself
//             is a peak-RSS effect measured by the heavy gate).
//   MEM-003   an emergency derived rebuild materializes its entries ON DISK in a
//             scratch directory (bounded resident state), never inside the
//             immutable generation, and still serves EXACT parity values.

#include <boost/test/unit_test.hpp>
#include <boost/filesystem.hpp>

#include "main.h"
#include "util.h"
#include "blockindex_v2_reader.h"
#include "blockindex_generation_lifecycle.h"
#include "blockindex_generation_builder.h"
#include "blockindex_derived_state.h"
#include "blockindex_stake_seen_builder.h"
#include "fixed_blockindex_store.h"

#include <cstdio>
#include <set>
#include <string>
#include <vector>

namespace bfs = boost::filesystem;

namespace {

// A small AUTHORITATIVE generation whose records carry the PoS flag, the
// prevoutStake and the nStakeTime that setStakeSeen is built from.
struct SmiFixture
{
    bfs::path root;
    std::vector<uint256> hashes;
    std::vector<BlockIndexDerivedEntry> builderDerived;
    int tip;
    std::string derivedPath;
    std::vector<unsigned char> derivedBytes;
    bool frSaved, ftSaved;

    explicit SmiFixture(int t) : tip(t)
    {
        frSaved = ::fRegTest; ftSaved = ::fTestNet;
        ::fRegTest = false; ::fTestNet = false; // mainnet params: POEM inert
        root = bfs::temp_directory_path() / bfs::unique_path("smi-gen-%%%%-%%%%");
        bfs::create_directories(root / "blocks");

        std::vector<BlockIndexRecord> recs;
        uint256 prev(0);
        for (int h = 0; h <= tip; ++h)
        {
            CBlock blk;
            blk.nVersion = 1;
            blk.hashPrevBlock = prev;
            blk.nTime = 1700000000u + 37u * (unsigned)h + 11u;
            blk.nBits = 0x1d00ffffU;
            blk.nNonce = (unsigned)h;
            CTransaction coin;
            coin.nVersion = 1;
            coin.nTime = blk.nTime;
            CTxIn in;
            in.prevout = COutPoint(uint256(0), 0xffffffff);
            in.scriptSig = CScript() << OP_TRUE;
            in.nSequence = 0xffffffff;
            coin.vin.push_back(in);
            CTxOut xo;
            xo.nValue = 0;
            xo.scriptPubKey = CScript() << OP_TRUE;
            coin.vout.push_back(xo);
            blk.vtx.push_back(coin);
            blk.hashMerkleRoot = blk.BuildMerkleTree();
            CDataStream ss(SER_DISK, CLIENT_VERSION);
            ss << blk;
            const std::string blkPath = (root / "blocks" / "blk0001.dat").string();
            FILE* bf = fopen(blkPath.c_str(), "ab");
            BOOST_REQUIRE(bf != NULL);
            unsigned char magic[] = {0xfa, 0xbf, 0xb5, 0xda};
            fwrite(magic, 1, 4, bf);
            unsigned ns = ss.size();
            fwrite(&ns, 4, 1, bf);
            long pos = ftell(bf);
            fwrite(&ss[0], 1, ss.size(), bf);
            fflush(bf);
            fclose(bf);

            BlockIndexRecord rec;
            rec.hash = blk.GetHash();
            rec.hashPrev = prev;
            rec.height = h;
            rec.nVersion = 1;
            rec.nTime = blk.nTime;
            rec.nBits = 0x1d00ffffU;
            rec.nNonce = (unsigned)h;
            rec.nFile = 1;
            rec.nBlockPos = (unsigned)pos;
            rec.nFlags = CBlockIndex::BLOCK_PROOF_OF_STAKE; // every record is PoS
            rec.prevoutStake = COutPoint(blk.GetHash(), (unsigned)h);
            rec.nStakeTime = blk.nTime - 60u;
            rec.nMoneySupply = 0;
            recs.push_back(rec);
            hashes.push_back(rec.hash);
            prev = rec.hash;
        }

        BlockIndexGenerationSource src;
        for (int h = 0; h <= tip; ++h)
        {
            BlockIndexGenerationSourceRecord sr;
            sr.hash = recs[h].hash;
            sr.record = recs[h];
            src.records.push_back(sr);
        }
        src.hashBestChain = hashes[tip];
        src.foundBestChain = true;
        src.blockDataDir = (root / "blocks").string();

        BlockIndexGenerationBuilder b;
        BlockIndexGenerationStats st;
        std::string error;
        BOOST_REQUIRE_MESSAGE(b.Build(src, (root / "build-000001.tmp").string(), 1, &st, &error), error);
        b.Close();
        BOOST_REQUIRE_MESSAGE(
            BlockIndexGenerationManager::PublishGeneration(root.string(), 1, &error) ==
                (int)BLOCK_INDEX_LIFECYCLE_OK, error);
        BOOST_REQUIRE_MESSAGE(
            BlockIndexGenerationManager::SelectGeneration(root.string(), 1, &error) ==
                (int)BLOCK_INDEX_LIFECYCLE_OK, error);

        const bfs::path gd = root / BlockIndexGenerationManager::GenerationName(1);
        derivedPath = (gd / BLOCK_INDEX_DERIVED_FILE_NAME).string();
        FILE* df = fopen(derivedPath.c_str(), "rb");
        BOOST_REQUIRE(df != NULL);
        fseek(df, 0, SEEK_END);
        const long sz = ftell(df);
        fseek(df, 0, SEEK_SET);
        derivedBytes.assign((size_t)sz, 0);
        BOOST_REQUIRE(fread(&derivedBytes[0], 1, derivedBytes.size(), df) == derivedBytes.size());
        fclose(df);
    }

    void CorruptDerivedBody()
    {
        std::vector<unsigned char> bad = derivedBytes;
        BOOST_REQUIRE(bad.size() > BLOCK_INDEX_DERIVED_HEADER_SIZE_V2 + 10);
        bad[BLOCK_INDEX_DERIVED_HEADER_SIZE_V2 + 5] ^= 0xFF;
        FILE* f = fopen(derivedPath.c_str(), "wb");
        BOOST_REQUIRE(f != NULL);
        BOOST_REQUIRE(fwrite(&bad[0], 1, bad.size(), f) == bad.size());
        fclose(f);
    }

    std::string GenerationDir() const
    {
        return (root / BlockIndexGenerationManager::GenerationName(1)).string();
    }

    ~SmiFixture()
    {
        ::fRegTest = frSaved; ::fTestNet = ftSaved;
        boost::system::error_code ec;
        bfs::remove_all(root, ec);
    }
};

typedef std::set<std::pair<COutPoint, unsigned int> > StakeSeenSet;

void CollectStakeSeenFromReader(const BlockIndexV2Reader& reader, StakeSeenSet* out)
{
    BOOST_REQUIRE(reader.ForEachRecordSequential(
        [out](BlockIndexId, const BlockIndexRecord& rec) -> bool
        {
            if (rec.nFlags & CBlockIndex::BLOCK_PROOF_OF_STAKE)
                out->insert(std::make_pair(rec.prevoutStake, rec.nStakeTime));
            return true;
        }, NULL));
}

} // namespace

BOOST_AUTO_TEST_SUITE(startup_memory_invariants_tests)

// BOOT-001 / MEM-002: the integrity-scan visitor must produce EXACTLY the
// setStakeSeen that the streaming builder produces, on the SAME single pass.
BOOST_AUTO_TEST_CASE(boot001_scan_visitor_equals_stake_seen_builder)
{
    SmiFixture fx(9);
    BOOST_REQUIRE_EQUAL(fx.hashes.size(), (size_t)(fx.tip + 1));

    BlockIndexV2ReaderOptions opts;
    StakeSeenSet collected;
    opts.integrityScanVisitor = [&collected](BlockIndexId, const BlockIndexRecord& rec)
    {
        if (rec.nFlags & CBlockIndex::BLOCK_PROOF_OF_STAKE)
            collected.insert(std::make_pair(rec.prevoutStake, rec.nStakeTime));
    };

    const uint64_t scansBefore = BlockIndexV2ReaderIntegrityScansForTesting();
    BlockIndexV2Reader reader;
    std::string err;
    BOOST_REQUIRE_MESSAGE(reader.Open(fx.root.string(), opts, &err), err);

    // The scan really ran with the visitor installed (not memo-short-circuited).
    BOOST_CHECK_EQUAL(BlockIndexV2ReaderIntegrityScansForTesting(), scansBefore + 1);
    BOOST_CHECK_MESSAGE(reader.IntegrityScanRanOnOpen(),
                        "BOOT-001: visitor installed but scan reported as not run");

    // Oracle: the A.10.1o streaming builder over the same generation.
    StakeSeenSet oracle;
    BlockIndexStakeSeenBuilder sb;
    std::string berr;
    BOOST_REQUIRE_MESSAGE(sb.Build(reader, &oracle, &berr), berr);

    BOOST_CHECK_EQUAL(oracle.size(), (size_t)(fx.tip + 1));
    BOOST_CHECK_MESSAGE(collected == oracle,
                        "BOOT-001: scan-collected set differs from the builder oracle ("
                        << collected.size() << " vs " << oracle.size() << ")");

    // A second open inside one process is memo-short-circuited: the visitor is
    // NOT run and the reader reports that honestly, so the startup path falls
    // back to the explicit rebuild instead of serving a half-collected set.
    // (The first reader must be closed first: the generation's hashindex is a
    // LevelDB directory with a process-level LOCK, exactly as in production.)
    reader.Close();
    StakeSeenSet second;
    BlockIndexV2ReaderOptions opts2;
    opts2.integrityScanVisitor = [&second](BlockIndexId, const BlockIndexRecord& rec)
    {
        if (rec.nFlags & CBlockIndex::BLOCK_PROOF_OF_STAKE)
            second.insert(std::make_pair(rec.prevoutStake, rec.nStakeTime));
    };
    BlockIndexV2Reader reader2;
    BOOST_REQUIRE_MESSAGE(reader2.Open(fx.root.string(), opts2, &err), err);
    BOOST_CHECK(!reader2.IntegrityScanRanOnOpen());
    BOOST_CHECK(second.empty());
    BOOST_CHECK_EQUAL(BlockIndexV2ReaderIntegrityScansForTesting(), scansBefore + 1);
    reader2.Close();
}

// BOOT-001: a corrupt record still fails the open closed WITH a visitor
// installed (the visitor can never turn a failing scan into an OK open).
BOOST_AUTO_TEST_CASE(boot001_corrupt_record_still_fails_closed_with_visitor)
{
    SmiFixture fx(4);
    // Truncate the committed record region by one byte.
    const std::string recPath = (bfs::path(fx.GenerationDir()) / BLOCK_INDEX_RECORDS_FILE_NAME).string();
    const uintmax_t before = bfs::file_size(recPath);
    bfs::resize_file(recPath, before - 1);

    StakeSeenSet collected;
    BlockIndexV2ReaderOptions opts;
    opts.integrityScanVisitor = [&collected](BlockIndexId, const BlockIndexRecord& rec)
    {
        if (rec.nFlags & CBlockIndex::BLOCK_PROOF_OF_STAKE)
            collected.insert(std::make_pair(rec.prevoutStake, rec.nStakeTime));
    };
    BlockIndexV2Reader reader;
    std::string err;
    BOOST_CHECK(!reader.Open(fx.root.string(), opts, &err));
    BOOST_CHECK(!err.empty());
    BOOST_CHECK(!reader.IntegrityScanRanOnOpen());
}

// MEM-003: the emergency derived rebuild is materialized ON DISK in a scratch
// directory, never inside the immutable generation, and serves exact parity.
BOOST_AUTO_TEST_CASE(mem003_bounded_rebuild_on_disk_parity_and_immutability)
{
    SmiFixture fx(7);

    // Intact baseline (sealed companion).
    std::vector<uint256> baseline;
    {
        BlockIndexV2Reader reader;
        std::string err;
        BOOST_REQUIRE_MESSAGE(reader.Open(fx.root.string(), BlockIndexV2ReaderOptions(), &err), err);
        for (size_t i = 0; i < fx.hashes.size(); ++i)
        {
            BlockIndexSnapshot snap;
            BOOST_REQUIRE(reader.LookupByHash(fx.hashes[i], &snap, &err) == BLOCK_INDEX_V2_READ_FOUND);
            baseline.push_back(snap.nChainTrust);
        }
        reader.Close();
    }

    // Generation directory inventory before the rebuild: the rebuild must not
    // add, remove or rewrite a single file inside it.
    std::set<std::string> genBefore;
    for (bfs::directory_iterator it(fx.GenerationDir()), end; it != end; ++it)
        genBefore.insert(it->path().filename().string());
    const std::vector<unsigned char> derivedBefore = fx.derivedBytes;

    fx.CorruptDerivedBody();
    const std::string scratch = (fx.root / ".derived-rebuild").string();
    BOOST_REQUIRE(!bfs::exists(scratch));

    {
        BlockIndexV2Reader reader;
        std::string err;
        BOOST_REQUIRE_MESSAGE(reader.Open(fx.root.string(), BlockIndexV2ReaderOptions(), &err), err);
        BOOST_REQUIRE_MESSAGE(bfs::exists(scratch), "MEM-003: rebuild target not created on disk");
        BOOST_REQUIRE(bfs::exists(bfs::path(scratch) / BLOCK_INDEX_DERIVED_FILE_NAME));

        // Served values: exact parity with the sealed companion's values.
        for (size_t i = 0; i < fx.hashes.size(); ++i)
        {
            BlockIndexSnapshot snap;
            BOOST_REQUIRE(reader.LookupByHash(fx.hashes[i], &snap, &err) == BLOCK_INDEX_V2_READ_FOUND);
            BOOST_CHECK_MESSAGE(snap.nChainTrust == baseline[i],
                                "MEM-003: rebuilt trust differs at index " << i);
        }

        // The rebuild target is a FORMAT-VALID derived.dat: it re-opens and
        // CRC-verifies through the normal read-only path.
        BlockIndexDerivedStateStore rebuilt;
        BOOST_REQUIRE_MESSAGE(
            BlockIndexDerivedStateStore::OpenReadOnly(scratch, 1, &rebuilt, &err), err);
        BOOST_CHECK_EQUAL(rebuilt.EntryCount(), (uint64_t)fx.hashes.size());
        BOOST_REQUIRE_MESSAGE(rebuilt.VerifyIntegrity(&err), err);

        reader.Close();
        BOOST_CHECK_MESSAGE(!bfs::exists(scratch), "MEM-003: scratch dir not removed on close");
    }

    // The immutable generation is untouched: same files, same bytes.
    std::set<std::string> genAfter;
    for (bfs::directory_iterator it(fx.GenerationDir()), end; it != end; ++it)
        genAfter.insert(it->path().filename().string());
    BOOST_CHECK(genBefore == genAfter);
    FILE* df = fopen(fx.derivedPath.c_str(), "rb");
    BOOST_REQUIRE(df != NULL);
    std::vector<unsigned char> derivedAfter(derivedBefore.size(), 0);
    BOOST_REQUIRE(fread(&derivedAfter[0], 1, derivedAfter.size(), df) == derivedAfter.size());
    fclose(df);
    // The generation still carries the CORRUPTED bytes we wrote (the rebuild
    // never "repairs" a sealed file), i.e. the rebuild is a serving overlay.
    BOOST_CHECK(derivedAfter != derivedBefore);
}

// MEM-003: a scratch directory left behind by a crash is discarded, never
// reused, and the rebuild still succeeds.
BOOST_AUTO_TEST_CASE(mem003_stale_scratch_is_discarded)
{
    SmiFixture fx(5);
    fx.CorruptDerivedBody();

    const bfs::path scratch = fx.root / ".derived-rebuild";
    bfs::create_directories(scratch);
    {
        FILE* f = fopen((scratch / BLOCK_INDEX_DERIVED_FILE_NAME).string().c_str(), "wb");
        BOOST_REQUIRE(f != NULL);
        const char junk[] = "stale-partial-rebuild";
        BOOST_REQUIRE(fwrite(junk, 1, sizeof junk, f) == sizeof junk);
        fclose(f);
    }

    BlockIndexV2Reader reader;
    std::string err;
    BOOST_REQUIRE_MESSAGE(reader.Open(fx.root.string(), BlockIndexV2ReaderOptions(), &err), err);
    BlockIndexDerivedStateStore rebuilt;
    BOOST_REQUIRE_MESSAGE(
        BlockIndexDerivedStateStore::OpenReadOnly(scratch.string(), 1, &rebuilt, &err), err);
    BOOST_CHECK_EQUAL(rebuilt.EntryCount(), (uint64_t)fx.hashes.size());
    reader.Close();
}

BOOST_AUTO_TEST_SUITE_END()
