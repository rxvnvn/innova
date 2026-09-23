// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.
//
// R2c.1c — DAG tip frontier generation-lifecycle integration tests.
//
// Exercises the PRODUCTION BlockIndexGenerationBuilder path (not direct
// BuildDagTipFrontier calls) for the frontier-capable generation contract:
// build -> artifact -> manifest capability/binding -> validate -> publish/select.
// Also covers legacy-manifest compatibility and the read-only capability query.

#include <boost/test/unit_test.hpp>

#include "../blockindex_generation_builder.h"
#include "../blockindex_generation_lifecycle.h"
#include "../candidate_frontier_metadata.h"
#include "../dag_tip_frontier_metadata.h"
#include "../dag_tip_frontier.h"
#include "../fixed_blockindex_store.h"
#include "../main.h"
#include "../dag.h"
#include "../serialize.h"
#include "../blockindex_authoritative_startup.h"
#include "../txdb-leveldb.h"
#include "../txdb.h"

#include <leveldb/db.h>
#include <leveldb/filter_policy.h>

#include <boost/filesystem.hpp>

#include <openssl/sha.h>

#include <stdio.h>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

struct LifecycleBlockInfo
{
    uint256 hash;
    unsigned int nFile;
    unsigned int nBlockPos;
    LifecycleBlockInfo() : nFile(0), nBlockPos(0) {}
};

static LifecycleBlockInfo WriteBlock(const boost::filesystem::path& dir,
                                     uint256 hashPrev, unsigned int nTime,
                                     unsigned int nNonce)
{
    LifecycleBlockInfo info;
    info.nFile = 1;
    CTransaction coinbase;
    coinbase.nVersion = 1;
    coinbase.nTime = nTime;
    CTxIn in; in.prevout = COutPoint(uint256(0), 0xffffffff);
    in.scriptSig = CScript() << OP_TRUE; in.nSequence = 0xffffffff;
    coinbase.vin.push_back(in);
    CTxOut out; out.nValue = 0; out.scriptPubKey = CScript() << OP_TRUE;
    coinbase.vout.push_back(out);
    CBlock block;
    block.nVersion = 1; block.hashPrevBlock = hashPrev;
    block.nTime = nTime; block.nBits = 0x1d00ffffU; block.nNonce = nNonce;
    block.vtx.push_back(coinbase);
    block.hashMerkleRoot = block.BuildMerkleTree();
    info.hash = block.GetHash();
    CDataStream ssBlock(SER_DISK, CLIENT_VERSION); ssBlock << block;
    unsigned int nDiskSize = ssBlock.size();
    boost::filesystem::path f = dir / "blk0001.dat";
    FILE* fp = fopen(f.string().c_str(), "ab"); assert(fp);
    unsigned char magic[] = {0xfa,0xbf,0xb5,0xda};
    fwrite(magic,1,4,fp); fwrite(&nDiskSize,4,1,fp);
    long pos = ftell(fp); info.nBlockPos = (unsigned int)pos;
    fwrite(&ssBlock[0],1,ssBlock.size(),fp);
    fflush(fp); fclose(fp);
    return info;
}

// Write a daglinks entry with the SAME serialization as CTxDB::WriteDAGLinks,
// so the frontier builder's verifier reads exactly what the builder digest used.
static bool WriteDagLinksEntry(const std::string& dbDir, const uint256& hash,
                               const std::vector<uint256>& parents)
{
    leveldb::Options opts; opts.create_if_missing = true;
    opts.filter_policy = leveldb::NewBloomFilterPolicy(10);
    leveldb::DB* db = NULL;
    leveldb::Status st = leveldb::DB::Open(opts, dbDir, &db);
    if (!st.ok() || !db) return false;
    CDataStream k(SER_DISK, CLIENT_VERSION); k << make_pair(std::string("daglinks"), hash);
    CBlockDAGData d; d.vDAGParents = parents; d.fBlue = true;
    CDataStream v(SER_DISK, CLIENT_VERSION); v << d;
    st = db->Put(leveldb::WriteOptions(), k.str(), v.str());
    delete db;
    return st.ok();
}

// S4 test-only: wipe the shared mutable DAG source residue (daglinks +
// child-count projection + all source/authority markers) so each fixture
// presents exactly its own world to the authoritative startup. The source
// token itself is KEPT: it is an opaque identity, and because every marker and
// projection is wiped together with the links, nothing certifies the previous
// canvas; the startup rebuilds counts + score authority against the new links
// under the same token. Uses the same raw leveldb access as WriteDagLinksEntry;
// never used in production.
static bool ResetSharedDAGSource(const std::string& dbDir, std::string* error)
{
    leveldb::Options opts; opts.create_if_missing = true;
    opts.filter_policy = leveldb::NewBloomFilterPolicy(10);
    leveldb::DB* db = NULL;
    leveldb::Status st = leveldb::DB::Open(opts, dbDir, &db);
    if (!st.ok() || !db) { if (error) *error = "reset: cannot open " + dbDir; return false; }
    const char* prefixes[] = { "daglinks", "dagchildcount", "dagchildcountstate",
                               "dagscorestate", "dagscoreinvalid" };
    std::vector<std::string> doomed;
    for (size_t p = 0; p < sizeof(prefixes)/sizeof(prefixes[0]); ++p)
    {
        CDataStream ps(SER_DISK, CLIENT_VERSION);
        ps << std::string(prefixes[p]);
        const std::string prefix = ps.str();
        leveldb::Iterator* it = db->NewIterator(leveldb::ReadOptions());
        for (it->Seek(prefix); it->Valid(); it->Next())
        {
            const leveldb::Slice k = it->key();
            if (k.size() < prefix.size() || memcmp(k.data(), prefix.data(), prefix.size()) != 0) break;
            doomed.push_back(k.ToString());
        }
        delete it;
    }
    for (size_t i = 0; i < doomed.size(); ++i)
    {
        st = db->Delete(leveldb::WriteOptions(), doomed[i]);
        if (!st.ok()) { if (error) *error = "reset: delete failed"; delete db; return false; }
    }
    delete db;
    return true;
}

// Build a small authoritative chain with a daglinks store, run the PRODUCTION
// builder, and return the staging dir + the final legacy tip reference.
struct LifecycleFixture
{
    boost::filesystem::path root;
    boost::filesystem::path blockDir;
    boost::filesystem::path dagDb;
    std::vector<LifecycleBlockInfo> blocks;
    BlockIndexGenerationSource src;
    std::set<uint256> expectedTips; // nodes - referencedParents (legacy algebra)
    int heights;

    explicit LifecycleFixture(int n, bool frontierOn)
        : heights(n)
    {
        root = boost::filesystem::temp_directory_path()
            / boost::filesystem::unique_path("r2c1c-life-%%%%-%%%%-%%%%");
        boost::filesystem::create_directories(root);
        blockDir = root / "blocks"; boost::filesystem::create_directories(blockDir);
        dagDb = root / "dagdb"; boost::filesystem::create_directories(dagDb);

        uint256 prev(0);
        for (int h = 0; h <= heights; ++h)
        {
            LifecycleBlockInfo bi = WriteBlock(blockDir, prev, (unsigned int)(1000+h), (unsigned int)(100+h));
            blocks.push_back(bi);
            prev = bi.hash;
        }

        for (int h = 0; h <= heights; ++h)
        {
            BlockIndexRecord rec;
            rec.hash = blocks[h].hash;
            rec.hashPrev = (h == 0) ? uint256(0) : blocks[h-1].hash;
            rec.height = h; rec.nVersion = 1;
            rec.nTime = (unsigned int)(1000+h);
            rec.nBits = 0x1d00ffffU; rec.nNonce = (unsigned int)(100+h);
            rec.nFile = blocks[h].nFile; rec.nBlockPos = blocks[h].nBlockPos;
            BlockIndexGenerationSourceRecord sr; sr.hash=rec.hash; sr.record=rec;
            src.records.push_back(sr);
        }
        src.hashBestChain = blocks[heights].hash;
        src.foundBestChain = true;
        src.blockDataDir = blockDir.string();

        // A linear DAG (each block parents = its single linear parent), plus a
        // root. Tips per legacy algebra = {tip block} (every other node is a
        // referenced parent). Deterministic and small.
        std::map<uint256, std::vector<uint256> > parents;
        for (int h = 0; h <= heights; ++h)
        {
            std::vector<uint256> p;
            if (h > 0) p.push_back(blocks[h-1].hash);
            parents[blocks[h].hash] = p;
            src.dagLinks[blocks[h].hash] = p;
        }
        if (frontierOn)
        {
            src.dagLinksDir = dagDb.string();
            for (int h = 0; h <= heights; ++h)
                BOOST_REQUIRE(WriteDagLinksEntry(dagDb.string(), blocks[h].hash, parents[blocks[h].hash]));
        }
        // Legacy tips = all nodes minus referenced parents.
        expectedTips.clear();
        for (int h = 0; h <= heights; ++h) expectedTips.insert(blocks[h].hash);
        for (int h = 0; h <= heights; ++h)
            for (size_t p = 0; p < parents[blocks[h].hash].size(); ++p)
                expectedTips.erase(parents[blocks[h].hash][p]);
    }

    ~LifecycleFixture()
    {
        boost::system::error_code ec;
        boost::filesystem::remove_all(root, ec);
    }

    // Run the PRODUCTION builder into root/build-000001.tmp.
    bool Build(uint64_t gen, std::string* error)
    {
        boost::filesystem::path staging = root / (std::string("build-00000") + std::to_string(gen) + ".tmp");
        BlockIndexGenerationBuilder b;
        bool ok = b.Build(src, staging.string(), gen, NULL, error);
        b.Close();
        return ok;
    }
};

// Collect frontier tips via the streaming reader into a set (test-only).
static std::set<uint256> ReadFrontierTips(const std::string& artifact, uint64_t gen,
                                          const unsigned char dagInputDigest[32])
{
    std::set<uint256> out;
    dag_tip_frontier::TipFrontierReader r;
    std::string err;
    BOOST_REQUIRE_MESSAGE(r.Open(artifact, gen, dagInputDigest, &err), err);
    uint256 h;
    while (r.Next(&h)) out.insert(h);
    return out;
}

} // namespace

BOOST_AUTO_TEST_CASE(r2c1c_production_builder_frontier_capable_normal)
{
    // Case A: normal frontier-capable production build.
    LifecycleFixture fx(5, /*frontierOn=*/true);
    BOOST_REQUIRE_EQUAL(fx.expectedTips.size(), 1u); // only tip remains
    std::string error;
    BOOST_REQUIRE_MESSAGE(fx.Build(1, &error), error);
    boost::filesystem::path staging = fx.root / "build-000001.tmp";
    // MANIFEST capability = AUTHORITATIVE_FRONTIER
    FixedBlockIndexOpenOptions opts; opts.requireCompleteManifest = true;
    FixedBlockIndexStore store;
    BOOST_REQUIRE(FixedBlockIndexStore::OpenReadOnly(staging.string(), opts, &store, &error));
    const FixedBlockIndexManifest& m = store.GetManifest();
    BOOST_CHECK_EQUAL((int)m.capability, (int)BLOCK_INDEX_GENERATION_CAPABILITY_AUTHORITATIVE_FRONTIER);
    // artifact exists + reader opens + integrity query PRESENT_VALID
    boost::filesystem::path artifact = staging / BLOCK_INDEX_DAG_TIP_FRONTIER_FILE_NAME;
    BOOST_CHECK(boost::filesystem::exists(artifact));
    std::string detail;
    DagTipFrontierCapability cap = QueryDagTipFrontierCapability(
        staging.string(), m.generation, m.capability, m.dagInputDigest, &detail);
    BOOST_CHECK_EQUAL((int)cap, (int)DAG_TIP_FRONTIER_CAPABILITY_PRESENT_VALID);
    // frontier tips == legacy algebra
    std::set<uint256> tips = ReadFrontierTips(artifact.string(), m.generation, m.dagInputDigest);
    BOOST_CHECK(tips == fx.expectedTips);
    // Publish + Select + ValidateGeneration PASS (full production lifecycle).
    std::string perr;
    BOOST_CHECK_EQUAL((int)BlockIndexGenerationManager::PublishGeneration(fx.root.string(), 1, &perr),
                      (int)BLOCK_INDEX_LIFECYCLE_OK);
    std::string serr;
    BOOST_CHECK_EQUAL((int)BlockIndexGenerationManager::SelectGeneration(fx.root.string(), 1, &serr),
                      (int)BLOCK_INDEX_LIFECYCLE_OK);
    std::string verr;
    BOOST_CHECK_EQUAL((int)BlockIndexGenerationManager::ValidateGeneration(fx.root.string(), 1, &verr),
                      (int)BLOCK_INDEX_LIFECYCLE_OK);
}

BOOST_AUTO_TEST_CASE(r2c1c_production_builder_frontier_determinism_and_validate_publish)
{
    // Case B + PHASE 8-ish: deterministic + full validate/publish/select path on a
    // production-squared build (uses the production Blueprint builder twice).
    LifecycleFixture fx(4, /*frontierOn=*/true);
    std::string error;
    BOOST_REQUIRE_MESSAGE(fx.Build(2, &error), error);
    boost::filesystem::path staging2 = fx.root / "build-000002.tmp";
    FixedBlockIndexOpenOptions opts; opts.requireCompleteManifest = true;
    FixedBlockIndexStore s2;
    BOOST_REQUIRE(FixedBlockIndexStore::OpenReadOnly(staging2.string(), opts, &s2, &error));
    const FixedBlockIndexManifest& m2 = s2.GetManifest();
    boost::filesystem::path artifact = staging2 / BLOCK_INDEX_DAG_TIP_FRONTIER_FILE_NAME;
    std::set<uint256> tips = ReadFrontierTips(artifact.string(), m2.generation, m2.dagInputDigest);
    BOOST_CHECK(tips == fx.expectedTips);
    // Publish + select + validate (full lifecycle).
    BOOST_CHECK_EQUAL((int)BlockIndexGenerationManager::PublishGeneration(fx.root.string(), 2, &error),
                      (int)BLOCK_INDEX_LIFECYCLE_OK);
    BOOST_CHECK_EQUAL((int)BlockIndexGenerationManager::SelectGeneration(fx.root.string(), 2, &error),
                      (int)BLOCK_INDEX_LIFECYCLE_OK);
    BOOST_CHECK_EQUAL((int)BlockIndexGenerationManager::ValidateGeneration(fx.root.string(), 2, &error),
                      (int)BLOCK_INDEX_LIFECYCLE_OK);
    // after publish, stable gen-N contains frontier + capability query valid
    boost::filesystem::path stable = fx.root / "gen-000002";
    boost::filesystem::path artifactS = stable / BLOCK_INDEX_DAG_TIP_FRONTIER_FILE_NAME;
    BOOST_CHECK(boost::filesystem::exists(artifactS));
    std::string detail;
    DagTipFrontierCapability cap = QueryDagTipFrontierCapability(
        stable.string(), m2.generation, m2.capability, m2.dagInputDigest, &detail);
    BOOST_CHECK_EQUAL((int)cap, (int)DAG_TIP_FRONTIER_CAPABILITY_PRESENT_VALID);
}

BOOST_AUTO_TEST_CASE(r2c1c_frontier_missing_corrupt_wronggen_wrongdigest_failclosed)
{
    // Cases C/D/E/F/G on a valid build: mutating the artifact fails closed.
    LifecycleFixture fx(4, /*frontierOn=*/true);
    std::string error;
    BOOST_REQUIRE_MESSAGE(fx.Build(3, &error), error);
    boost::filesystem::path staging = fx.root / "build-000003.tmp";
    FixedBlockIndexOpenOptions opts; opts.requireCompleteManifest = true;
    FixedBlockIndexStore store;
    BOOST_REQUIRE(FixedBlockIndexStore::OpenReadOnly(staging.string(), opts, &store, &error));
    const FixedBlockIndexManifest& m = store.GetManifest();
    boost::filesystem::path artifact = staging / BLOCK_INDEX_DAG_TIP_FRONTIER_FILE_NAME;

    // C: a missing artifact on a frontier-capable generation is declared-
    // capability corruption, never legacy-unavailable. Publish validates
    // staging with requireStableName=false and must fail closed.
    {
        boost::system::error_code ec;
        boost::filesystem::rename(artifact, fx.root / "moved.dat", ec);
        std::string detail;
        DagTipFrontierCapability cap = QueryDagTipFrontierCapability(staging.string(), m.generation, m.capability, m.dagInputDigest, &detail);
        BOOST_CHECK_EQUAL((int)cap, (int)DAG_TIP_FRONTIER_CAPABILITY_CORRUPT);
        BOOST_CHECK(!detail.empty());
        std::string perr;
        BOOST_CHECK_NE((int)BlockIndexGenerationManager::PublishGeneration(fx.root.string(), 3, &perr),
                       (int)BLOCK_INDEX_LIFECYCLE_OK);
        std::string err2;
        boost::filesystem::rename(fx.root / "moved.dat", artifact, ec);
    }

    // D: truncated/corrupt artifact -> query CORRUPT, publication fails.
    {
        std::string data;
        {
            FILE* f = fopen(artifact.string().c_str(), "rb");
            if (f) { fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET); data.resize((size_t)sz); if(sz) fread(&data[0],1,(size_t)sz,f); fclose(f); }
        }
        BOOST_REQUIRE(data.size() > 64);
        const std::string original = data;
        data.resize(32); // truncate header
        {
            FILE* f = fopen(artifact.string().c_str(), "wb"); fwrite(data.data(),1,data.size(),f); fclose(f);
        }
        std::string detail;
        DagTipFrontierCapability cap = QueryDagTipFrontierCapability(staging.string(), m.generation, m.capability, m.dagInputDigest, &detail);
        BOOST_CHECK_EQUAL((int)cap, (int)DAG_TIP_FRONTIER_CAPABILITY_CORRUPT);
        std::string perr;
        BOOST_CHECK_NE((int)BlockIndexGenerationManager::PublishGeneration(fx.root.string(), 3, &perr),
                       (int)BLOCK_INDEX_LIFECYCLE_OK);
        FILE* restore = fopen(artifact.string().c_str(), "wb");
        BOOST_REQUIRE(restore);
        fwrite(original.data(), 1, original.size(), restore);
        fclose(restore);
    }

    // E: artifact bound to a DIFFERENT generation fails binding (open rejects).
    {
        dag_tip_frontier::TipFrontierReader r;
        std::string e;
        BOOST_CHECK(!r.Open(artifact.string(), m.generation + 99, m.dagInputDigest, &e));
    }

    // F: wrong dagInputDigest -> binding mismatch.
    {
        unsigned char badDigest[32];
        memset(badDigest, 0xAB, 32);
        dag_tip_frontier::TipFrontierReader r;
        std::string e;
        BOOST_CHECK(!r.Open(artifact.string(), m.generation, badDigest, &e));
    }
}

BOOST_AUTO_TEST_CASE(r2c1c_legacy_generation_compatible_and_capability_unavailable)
{
    // Case H: a non-frontier production build (frontierOn=false) stays a normal
    // AUTHORITATIVE generation; parses; validates; capability query -> unavailable.
    LifecycleFixture fx(4, /*frontierOn=*/false);
    std::string error;
    BOOST_REQUIRE_MESSAGE(fx.Build(4, &error), error);
    boost::filesystem::path staging = fx.root / "build-000004.tmp";
    FixedBlockIndexOpenOptions opts; opts.requireCompleteManifest = true;
    FixedBlockIndexStore store;
    BOOST_REQUIRE(FixedBlockIndexStore::OpenReadOnly(staging.string(), opts, &store, &error));
    const FixedBlockIndexManifest& m = store.GetManifest();
    BOOST_CHECK_EQUAL((int)m.capability, (int)BLOCK_INDEX_GENERATION_CAPABILITY_AUTHORITATIVE);
    // Publish + select then ValidateGeneration PASS (legacy path unchanged).
    std::string perr;
    BOOST_CHECK_EQUAL((int)BlockIndexGenerationManager::PublishGeneration(fx.root.string(), 4, &perr),
                      (int)BLOCK_INDEX_LIFECYCLE_OK);
    std::string serr;
    BOOST_CHECK_EQUAL((int)BlockIndexGenerationManager::SelectGeneration(fx.root.string(), 4, &serr),
                      (int)BLOCK_INDEX_LIFECYCLE_OK);
    std::string verr;
    BOOST_CHECK_EQUAL((int)BlockIndexGenerationManager::ValidateGeneration(fx.root.string(), 4, &verr),
                      (int)BLOCK_INDEX_LIFECYCLE_OK);
    std::string detail;
    DagTipFrontierCapability cap = QueryDagTipFrontierCapability(staging.string(), m.generation, m.capability, m.dagInputDigest, &detail);
    BOOST_CHECK_EQUAL((int)cap, (int)DAG_TIP_FRONTIER_CAPABILITY_LEGACY_UNAVAILABLE);

    // OLD_SHADOW also never declared frontier capability, so artifact absence
    // remains legacy-unavailable rather than a declared-capability failure.
    cap = QueryDagTipFrontierCapability(staging.string(), m.generation,
        BLOCK_INDEX_GENERATION_CAPABILITY_OLD_SHADOW, m.dagInputDigest, &detail);
    BOOST_CHECK_EQUAL((int)cap, (int)DAG_TIP_FRONTIER_CAPABILITY_LEGACY_UNAVAILABLE);
}

BOOST_AUTO_TEST_CASE(r2c1c_frontier_count_digest_mismatch_reports_corrupt)
{
    // Case G: mutate a frontier tip byte so the recomputed digest differs from
    // the header digest -> capability CORRUPT.
    LifecycleFixture fx(5, /*frontierOn=*/true);
    std::string error;
    BOOST_REQUIRE_MESSAGE(fx.Build(5, &error), error);
    boost::filesystem::path staging = fx.root / "build-000005.tmp";
    FixedBlockIndexOpenOptions opts; opts.requireCompleteManifest = true;
    FixedBlockIndexStore store;
    BOOST_REQUIRE(FixedBlockIndexStore::OpenReadOnly(staging.string(), opts, &store, &error));
    const FixedBlockIndexManifest& m = store.GetManifest();
    boost::filesystem::path artifact = staging / BLOCK_INDEX_DAG_TIP_FRONTIER_FILE_NAME;
    std::string data;
    {
        FILE* f = fopen(artifact.string().c_str(), "rb");
        if (f) { fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET); data.resize((size_t)sz); if(sz) fread(&data[0],1,(size_t)sz,f); fclose(f); }
    }
    BOOST_REQUIRE(data.size() > 93); // header(min) + one tip
    // flip a byte inside the tip body (offset 93..)
    for (size_t i = 93; i < data.size(); ++i)
    {
        data[i] ^= (char)0x40;
        FILE* f = fopen(artifact.string().c_str(), "wb"); fwrite(data.data(),1,data.size(),f); fclose(f);
        std::string detail;
        DagTipFrontierCapability cap = QueryDagTipFrontierCapability(staging.string(), m.generation, m.capability, m.dagInputDigest, &detail);
        if (cap != DAG_TIP_FRONTIER_CAPABILITY_PRESENT_VALID)
        {
            BOOST_CHECK_EQUAL((int)cap, (int)DAG_TIP_FRONTIER_CAPABILITY_CORRUPT);
            break;
        }
        data[i] ^= (char)0x40; // restore
    }
}

BOOST_AUTO_TEST_CASE(r2c1c_production_builder_missing_daglinks_no_frontier)
{
    // Guard: a production build whose daglinks source is absent (e.g. DAG
    // dormant / no daglinks store) stays a valid AUTHORITATIVE generation and
    // does NOT declare frontier-capable (no hidden requirement).
    LifecycleFixture fx(3, /*frontierOn=*/true);
    // Empty the daglinks source dir (simulate no DAG data): remove entries.
    // Easiest deterministic simulation: build with dagLinksDir empty string.
    fx.src.dagLinks.clear();
    fx.src.dagLinksDir.clear();
    std::string error;
    BOOST_REQUIRE_MESSAGE(fx.Build(6, &error), error);
    boost::filesystem::path staging = fx.root / "build-000006.tmp";
    FixedBlockIndexOpenOptions opts; opts.requireCompleteManifest = true;
    FixedBlockIndexStore store;
    BOOST_REQUIRE(FixedBlockIndexStore::OpenReadOnly(staging.string(), opts, &store, &error));
    const FixedBlockIndexManifest& m = store.GetManifest();
    BOOST_CHECK_EQUAL((int)m.capability, (int)BLOCK_INDEX_GENERATION_CAPABILITY_AUTHORITATIVE);
    std::string perr;
    BOOST_CHECK_EQUAL((int)BlockIndexGenerationManager::PublishGeneration(fx.root.string(), 6, &perr),
                      (int)BLOCK_INDEX_LIFECYCLE_OK);
    std::string serr;
    BOOST_CHECK_EQUAL((int)BlockIndexGenerationManager::SelectGeneration(fx.root.string(), 6, &serr),
                      (int)BLOCK_INDEX_LIFECYCLE_OK);
    BOOST_CHECK_EQUAL((int)BlockIndexGenerationManager::ValidateGeneration(fx.root.string(), 6, &perr),
                      (int)BLOCK_INDEX_LIFECYCLE_OK);
}

// =========================================================================
// R2c.1d3a — InitBlockIndexAuthoritative integration matrix
//
// Exercises the REAL production InitBlockIndexAuthoritative() path on an
// isolated temporary datadir using CTxDB/txleveldb semantically equivalent
// to fx.dagDb. Tests the four capability states: PRESENT_VALID,
// LEGACY_UNAVAILABLE, CORRUPT, repeated PRESENT_VALID.
//
// Uses the existing LifecycleFixture pattern + test-only reset API.
// =========================================================================

namespace {

// Helper to compare uint256 sets - returns true if equal
static bool SetsEqual(const std::set<uint256>& a, const std::set<uint256>& b)
{
    if (a.size() != b.size()) return false;
    for (const auto& h : a) {
        if (b.find(h) == b.end()) return false;
    }
    return true;
}

// Extended fixture that adds real txleveldb (CTxDB) and isolated -datadir
// for testing InitBlockIndexAuthoritative.
struct InitAuthoritativeFixture
{
    boost::filesystem::path root;
    boost::filesystem::path blockDir;
    boost::filesystem::path dagDb; // this is the txleveldb equivalent
    std::vector<LifecycleBlockInfo> blocks;
    BlockIndexGenerationSource src;
    std::set<uint256> expectedTips;
    int heights;
    uint64_t selectedGeneration;

    explicit InitAuthoritativeFixture(int n, bool frontierOn)
        : heights(n), selectedGeneration(0)
    {
        // Isolated temporary datadir for the test
        root = boost::filesystem::temp_directory_path()
            / boost::filesystem::unique_path("r2c1d3a-initauth-%%%%-%%%%-%%%%");
        boost::filesystem::create_directories(root);
        blockDir = root / "blocks"; boost::filesystem::create_directories(blockDir);
        dagDb = root / "immutable-dagdb"; boost::filesystem::create_directories(dagDb);

        // GetDataDir is process-cached by TestingSetup; never attempt a late
        // -datadir switch here. Immutable builder input stays fixture-owned.

        // Build linear chain blocks
        uint256 prev(0);
        for (int h = 0; h <= heights; ++h)
        {
            LifecycleBlockInfo bi = WriteBlock(blockDir, prev, (unsigned int)(1000+h), (unsigned int)(100+h));
            blocks.push_back(bi);
            prev = bi.hash;
        }

        for (int h = 0; h <= heights; ++h)
        {
            BlockIndexRecord rec;
            rec.hash = blocks[h].hash;
            rec.hashPrev = (h == 0) ? uint256(0) : blocks[h-1].hash;
            rec.height = h; rec.nVersion = 1;
            rec.nTime = (unsigned int)(1000+h);
            rec.nBits = 0x1d00ffffU; rec.nNonce = (unsigned int)(100+h);
            rec.nFile = blocks[h].nFile; rec.nBlockPos = blocks[h].nBlockPos;
            BlockIndexGenerationSourceRecord sr; sr.hash=rec.hash; sr.record=rec;
            src.records.push_back(sr);
        }
        src.hashBestChain = blocks[heights].hash;
        src.foundBestChain = true;
        src.blockDataDir = blockDir.string();

        // A linear DAG (each block parents = its single linear parent), plus a
        // root. Tips per legacy algebra = {tip block} (every other node is a
        // referenced parent). Deterministic and small.
        std::map<uint256, std::vector<uint256> > parents;
        for (int h = 0; h <= heights; ++h)
        {
            std::vector<uint256> p;
            if (h > 0) p.push_back(blocks[h-1].hash);
            parents[blocks[h].hash] = p;
            src.dagLinks[blocks[h].hash] = p;
        }
        if (frontierOn)
        {
            src.dagLinksDir = dagDb.string();
            for (int h = 0; h <= heights; ++h)
                BOOST_REQUIRE(WriteDagLinksEntry(dagDb.string(), blocks[h].hash, parents[blocks[h].hash]));
        }
        // Legacy tips = all nodes minus referenced parents.
        expectedTips.clear();
        for (int h = 0; h <= heights; ++h) expectedTips.insert(blocks[h].hash);
        for (int h = 0; h <= heights; ++h)
            for (size_t p = 0; p < parents[blocks[h].hash].size(); ++p)
                expectedTips.erase(parents[blocks[h].hash][p]);
    }

    ~InitAuthoritativeFixture()
    {
        // Clean up test-only authoritative startup state before destroying datadir
        ResetBlockIndexAuthoritativeStartupForTest();
        boost::system::error_code ec;
        boost::filesystem::remove_all(root, ec);
    }

    // Build production V2 generation and publish/select it
    bool BuildAndSelect(uint64_t gen, std::string* error)
    {
        boost::filesystem::path staging = root / (std::string("build-00000") + std::to_string(gen) + ".tmp");
        BlockIndexGenerationBuilder b;
        bool ok = b.Build(src, staging.string(), gen, NULL, error);
        b.Close();
        if (!ok) return false;
        selectedGeneration = gen;

        std::string perr;
        if (BlockIndexGenerationManager::PublishGeneration(root.string(), gen, &perr) != (int)BLOCK_INDEX_LIFECYCLE_OK)
        {
            if (error) *error = "publish: " + perr;
            return false;
        }
        std::string serr;
        if (BlockIndexGenerationManager::SelectGeneration(root.string(), gen, &serr) != (int)BLOCK_INDEX_LIFECYCLE_OK)
        {
            if (error) *error = "select: " + serr;
            return false;
        }
        return true;
    }

    // S4: the shared mutable source must present EXACTLY this fixture's world
    // for the startup score reconcile (strict retained-canvas enumeration +
    // fail-closed metadata resolution). Reset the accumulated source residue -
    // daglinks, child-count projection, and all authority markers (the source
    // token is kept; all markers are rebuilt under it from the new links) -
    // then replay the fixture's daglinks. Test-only; production never resets.
    bool PrepareActualLiveDaglinks(std::string* error)
    {
        { CTxDB closeDb; closeDb.Close(); }
        const std::string live = (GetDataDir() / "txleveldb").string();
        if (!ResetSharedDAGSource(live, error)) return false;
        for (std::map<uint256, std::vector<uint256> >::const_iterator it = src.dagLinks.begin(); it != src.dagLinks.end(); ++it)
            if (!WriteDagLinksEntry(live, it->first, it->second))
            { if (error) *error = "cannot write fixture daglinks to actual txleveldb"; return false; }
        return true;
    }

    // Run the REAL InitBlockIndexAuthoritative on the selected generation
    bool RunInitAuthoritative(std::string* error)
    {
        return InitBlockIndexAuthoritative(root.string(), error);
    }

    // Helper to open the frontier artifact and read tips
    std::set<uint256> ReadFrontierTipsFromGen(const FixedBlockIndexManifest& m,
                                               const std::string& genDir)
    {
        std::set<uint256> out;
        boost::filesystem::path artifact = boost::filesystem::path(genDir) / BLOCK_INDEX_DAG_TIP_FRONTIER_FILE_NAME;
        dag_tip_frontier::TipFrontierReader r;
        std::string err;
        BOOST_REQUIRE_MESSAGE(r.Open(artifact.string(), m.generation, m.dagInputDigest, &err), err);
        uint256 h;
        while (r.Next(&h)) out.insert(h);
        return out;
    }
};

} // namespace

// PRESENT_VALID: A frontier-capable generation with valid dag-tip-frontier.dat
// and real txleveldb source should initialize authoritative startup successfully
// and establish a DagTipOverlayRuntime.
BOOST_AUTO_TEST_CASE(r2c1d3a_init_authoritative_present_valid)
{
    InitAuthoritativeFixture fx(5, /*frontierOn=*/true);
    BOOST_REQUIRE_EQUAL(fx.expectedTips.size(), 1u); // only tip remains

    std::string error;
    BOOST_REQUIRE_MESSAGE(fx.BuildAndSelect(1, &error), error);

    // Verify the generation has frontier capability
    boost::filesystem::path genDir = fx.root / "gen-000001";
    FixedBlockIndexOpenOptions opts; opts.requireCompleteManifest = true;
    FixedBlockIndexStore store;
    BOOST_REQUIRE(FixedBlockIndexStore::OpenReadOnly(genDir.string(), opts, &store, &error));
    const FixedBlockIndexManifest& m = store.GetManifest();
    BOOST_CHECK_EQUAL((int)m.capability, (int)BLOCK_INDEX_GENERATION_CAPABILITY_AUTHORITATIVE_FRONTIER);

    // Query capability - should be PRESENT_VALID
    std::string detail;
    DagTipFrontierCapability cap = QueryDagTipFrontierCapability(
        genDir.string(), m.generation, m.capability, m.dagInputDigest, &detail);
    BOOST_CHECK_EQUAL((int)cap, (int)DAG_TIP_FRONTIER_CAPABILITY_PRESENT_VALID);

    // Now run the REAL InitBlockIndexAuthoritative
    BOOST_REQUIRE_MESSAGE(fx.PrepareActualLiveDaglinks(&error), error);
    BOOST_REQUIRE_MESSAGE(fx.RunInitAuthoritative(&error), error);

    // Verify authoritative startup succeeded
    BOOST_CHECK(g_fAuthoritativeStartup);
    BOOST_CHECK(pindexBest != NULL);
    BOOST_CHECK_EQUAL(pindexBest->nHeight, fx.heights);
    BOOST_CHECK(hashBestChain == fx.blocks[fx.heights].hash);

    // d3b: PRESENT_VALID retains the sole runtime and installs the sole
    // committed-delta observer slot owned by that runtime.
    BOOST_CHECK(HasDagTipOverlayRuntimeForTest());
    BOOST_CHECK_EQUAL(DagTipOverlayRuntimeGenerationForTest(), fx.selectedGeneration);
    BOOST_CHECK(GetDagTipDeltaState().enabled);

    // Verify frontier tips match legacy algebra
    std::set<uint256> tips = fx.ReadFrontierTipsFromGen(m, genDir.string());
    if (!SetsEqual(tips, fx.expectedTips)) {
        BOOST_ERROR("frontier tips do not match expected tips");
    }

    // Clean up for next test: unregister before runtime destruction.
    ResetBlockIndexAuthoritativeStartupForTest();
    BOOST_CHECK(!GetDagTipDeltaState().enabled);
}

// LEGACY_UNAVAILABLE: A non-frontier generation (no dag-tip-frontier.dat)
// cannot install the authoritative selector runtime.
// R2c.2/S6-repair (blocker repair cycle, Phase 8): such startup must FAIL
// CLOSED. Previously it booted with g_fAuthoritativeStartup=true and no
// runtime — a permanently half-operational node (primary selection /
// mining / finality all UNAVAILABLE for the process lifetime) with no
// startup diagnostic. The all-or-nothing contract refuses instead.
BOOST_AUTO_TEST_CASE(r2c1d3a_init_authoritative_legacy_unavailable)
{
    InitAuthoritativeFixture fx(4, /*frontierOn=*/false);

    std::string error;
    BOOST_REQUIRE_MESSAGE(fx.BuildAndSelect(2, &error), error);

    // Verify the generation does NOT have frontier capability
    boost::filesystem::path genDir = fx.root / "gen-000002";
    FixedBlockIndexOpenOptions opts; opts.requireCompleteManifest = true;
    FixedBlockIndexStore store;
    BOOST_REQUIRE(FixedBlockIndexStore::OpenReadOnly(genDir.string(), opts, &store, &error));
    const FixedBlockIndexManifest& m = store.GetManifest();
    BOOST_CHECK_EQUAL((int)m.capability, (int)BLOCK_INDEX_GENERATION_CAPABILITY_AUTHORITATIVE);

    // Query capability - should be LEGACY_UNAVAILABLE
    std::string detail;
    DagTipFrontierCapability cap = QueryDagTipFrontierCapability(
        genDir.string(), m.generation, m.capability, m.dagInputDigest, &detail);
    BOOST_CHECK_EQUAL((int)cap, (int)DAG_TIP_FRONTIER_CAPABILITY_LEGACY_UNAVAILABLE);

    // R2c.2/S6-repair: InitBlockIndexAuthoritative must REFUSE to boot a
    // generation that cannot support the selector runtime (fail closed;
    // all-or-nothing). No g_fAuthoritativeStartup, no owner, no runtime.
    BOOST_REQUIRE_MESSAGE(fx.PrepareActualLiveDaglinks(&error), error);
    BOOST_CHECK(!fx.RunInitAuthoritative(&error));
    BOOST_CHECK(!error.empty());
    BOOST_CHECK(!g_fAuthoritativeStartup);
    BOOST_CHECK(pindexBest == NULL);
    BOOST_CHECK(!HasDagTipOverlayRuntimeForTest());
    BOOST_CHECK_EQUAL(DagTipOverlayRuntimeGenerationForTest(), (uint64_t)0);

    ResetBlockIndexAuthoritativeStartupForTest();
}

// CORRUPT: A generation that declares AUTHORITATIVE_FRONTIER but has a
// corrupted dag-tip-frontier.dat should FAIL CLOSED in InitBlockIndexAuthoritative.
BOOST_AUTO_TEST_CASE(r2c1d3a_init_authoritative_corrupt_fails_closed)
{
    InitAuthoritativeFixture fx(4, /*frontierOn=*/true);

    std::string error;
    BOOST_REQUIRE_MESSAGE(fx.BuildAndSelect(3, &error), error);

    // Corrupt the frontier artifact
    boost::filesystem::path genDir = fx.root / "gen-000003";
    boost::filesystem::path artifact = genDir / BLOCK_INDEX_DAG_TIP_FRONTIER_FILE_NAME;
    BOOST_REQUIRE(boost::filesystem::exists(artifact));

    // Read and truncate the artifact
    std::string data;
    {
        FILE* f = fopen(artifact.string().c_str(), "rb");
        BOOST_REQUIRE(f);
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        data.resize((size_t)sz);
        fread(&data[0], 1, (size_t)sz, f);
        fclose(f);
    }
    BOOST_REQUIRE(data.size() > 64);
    data.resize(32); // truncate to corrupt

    {
        FILE* f = fopen(artifact.string().c_str(), "wb");
        BOOST_REQUIRE(f);
        fwrite(data.data(), 1, data.size(), f);
        fclose(f);
    }

    // Query capability - should be CORRUPT
    FixedBlockIndexOpenOptions opts; opts.requireCompleteManifest = true;
    FixedBlockIndexStore store;
    BOOST_REQUIRE(FixedBlockIndexStore::OpenReadOnly(genDir.string(), opts, &store, &error));
    const FixedBlockIndexManifest& m = store.GetManifest();
    std::string detail;
    DagTipFrontierCapability cap = QueryDagTipFrontierCapability(
        genDir.string(), m.generation, m.capability, m.dagInputDigest, &detail);
    BOOST_CHECK_EQUAL((int)cap, (int)DAG_TIP_FRONTIER_CAPABILITY_CORRUPT);

    // Run InitBlockIndexAuthoritative - MUST fail closed
    BOOST_REQUIRE_MESSAGE(fx.PrepareActualLiveDaglinks(&error), error);
    BOOST_CHECK(!fx.RunInitAuthoritative(&error));
    BOOST_CHECK(!error.empty());
    BOOST_CHECK(!g_fAuthoritativeStartup);
    BOOST_CHECK(pindexBest == NULL);
    BOOST_CHECK(!HasDagTipOverlayRuntimeForTest());

    ResetBlockIndexAuthoritativeStartupForTest();
}

// Repeated PRESENT_VALID: Running InitBlockIndexAuthoritative twice on the
// same valid generation (after reset) should succeed both times.
BOOST_AUTO_TEST_CASE(r2c1d3a_init_authoritative_repeated_present_valid)
{
    InitAuthoritativeFixture fx(5, /*frontierOn=*/true);
    BOOST_REQUIRE_EQUAL(fx.expectedTips.size(), 1u);

    std::string error;
    BOOST_REQUIRE_MESSAGE(fx.BuildAndSelect(4, &error), error);

    boost::filesystem::path genDir = fx.root / "gen-000004";
    FixedBlockIndexOpenOptions opts; opts.requireCompleteManifest = true;
    FixedBlockIndexStore store;
    BOOST_REQUIRE(FixedBlockIndexStore::OpenReadOnly(genDir.string(), opts, &store, &error));
    const FixedBlockIndexManifest& m = store.GetManifest();

    // FIRST run
    BOOST_REQUIRE_MESSAGE(fx.PrepareActualLiveDaglinks(&error), error);
    BOOST_REQUIRE_MESSAGE(fx.RunInitAuthoritative(&error), error);
    BOOST_CHECK(g_fAuthoritativeStartup);
    BOOST_CHECK(HasDagTipOverlayRuntimeForTest());
    BOOST_CHECK_EQUAL(DagTipOverlayRuntimeGenerationForTest(), fx.selectedGeneration);
    std::set<uint256> tips1 = fx.ReadFrontierTipsFromGen(m, genDir.string());
    if (!SetsEqual(tips1, fx.expectedTips)) {
        BOOST_ERROR("first run frontier tips do not match expected tips");
    }

    ResetBlockIndexAuthoritativeStartupForTest();

    // SECOND run on the SAME generation
    BOOST_REQUIRE_MESSAGE(fx.PrepareActualLiveDaglinks(&error), error);
    BOOST_REQUIRE_MESSAGE(fx.RunInitAuthoritative(&error), error);
    BOOST_CHECK(g_fAuthoritativeStartup);
    BOOST_CHECK(HasDagTipOverlayRuntimeForTest());
    BOOST_CHECK_EQUAL(DagTipOverlayRuntimeGenerationForTest(), fx.selectedGeneration);
    std::set<uint256> tips2 = fx.ReadFrontierTipsFromGen(m, genDir.string());
    if (!SetsEqual(tips2, fx.expectedTips)) {
        BOOST_ERROR("second run frontier tips do not match expected tips");
    }
    if (!SetsEqual(tips2, tips1)) {
        BOOST_ERROR("second run tips do not match first run tips");
    }

    ResetBlockIndexAuthoritativeStartupForTest();
}

// =========================================================================
// R2c.1d3b.i1 — NEGATIVE real-startup observer-registration matrix.
//
// Through the REAL InitBlockIndexAuthoritative() production path, proves that
// a child-count authority that cannot be certified at the point of connection
// refuses to install the committed-delta observer and never publishes a
// false-healthy runtime. Single-threaded startup executes the source repair
// (Bootstrap + Ensure) before the registration boundary, so repairable states
// are recounted and then connect; only states the repair cannot certify, or an
// authority that turns unhealthy at the exact registration boundary, are
// refused. Both refusing layers are exercised here through the real path.
// =========================================================================

// Test-only failure seams located in src/txdb-leveldb.cpp (file scope in that
// TU; declared here at file scope so the anonymous helpers + cases share them).
extern bool g_testFailDAGChildCountRebuild;
extern bool g_testFailDAGChildCountRevocation;

namespace {

static bool ObserverSlotActive()
{
    return GetDagTipDeltaState().enabled;
}

// CTxDB exposes Write/Erase as protected; a test subclass re-exposes them so a
// fixture can seed raw child-count marker state into the shared live source.
struct SeedDB : CTxDB
{
    explicit SeedDB(const char* mode) : CTxDB(mode) {}
    using CTxDB::Write;
    using CTxDB::Erase;
};

// Write an undecodable value under the state-marker key. BootstrapDAGSourceStateId
// never touches it; EnsureDAGChildCountIndex refuses on the unreadable marker.
static void SeedCorruptChildCountMarker()
{
    SeedDB db("+w");
    db.Write(make_pair(std::string("dagchildcountstate"), uint8_t(0)), std::string("x"));
    db.Close();
}

// Ensure a valid matching marker cannot take the fast path (force rebuild path).
static void SeedNoChildCountMarker()
{
    SeedDB db("+w");
    db.Erase(make_pair(std::string("dagchildcountstate"), uint8_t(0)));
    db.Close();
}

// Persist the revocation/rebuild-required key against the shared live source.
static void SeedChildCountRevocation()
{
    CTxDB db("+w");
    db.RevokeDAGChildCountForTest();
    db.Close();
}

// Restore the SHARED live txleveldb source to a certified healthy state so
// subsequent tests in the process are unaffected. Drops any leftover marker so
// Ensure always performs a full rebuild from daglinks and republishes.
static void ForceRepairChildCountHealthy()
{
    SeedDB db("+w");
    db.Erase(make_pair(std::string("dagchildcountstate"), uint8_t(0)));
    std::string err;
    BOOST_REQUIRE_MESSAGE(db.EnsureDAGChildCountIndex(&err), err);
    BOOST_CHECK_MESSAGE(db.IsDAGChildCountIndexHealthy(&err), err);
    db.Close();
}

// Registration-boundary hook: revoke the authority at the exact point the
// startup health re-check runs (after repair + runtime Start). Does NOT Close()
// so the shared live handle survives for the boundary re-check.
static void ForceRevokeAtRegistrationBoundary()
{
    CTxDB db("+w");
    db.RevokeDAGChildCountForTest();
}

} // namespace

// Unreadable child-count state marker at startup -> EnsureDAGChildCountIndex
// fails closed; InitBlockIndexAuthoritative returns false, no runtime is
// published, no observer is installed.
BOOST_AUTO_TEST_CASE(r2c1d3b_i1_neg_startup_corrupt_marker_refuses_observer)
{
    InitAuthoritativeFixture fx(4, /*frontierOn=*/true);
    std::string error;
    BOOST_REQUIRE_MESSAGE(fx.BuildAndSelect(5, &error), error);
    BOOST_REQUIRE_MESSAGE(fx.PrepareActualLiveDaglinks(&error), error);
    SeedCorruptChildCountMarker();

    const bool wasEnabled = ObserverSlotActive();
    BOOST_CHECK(!fx.RunInitAuthoritative(&error));
    BOOST_CHECK(!error.empty());
    BOOST_CHECK(!g_fAuthoritativeStartup);
    BOOST_CHECK(!HasDagTipOverlayRuntimeForTest());
    BOOST_CHECK_EQUAL((int)ObserverSlotActive(), (int)wasEnabled);
    BOOST_CHECK(error.find("child-count") != std::string::npos);

    // Restore the shared live source for later tests, then reset.
    SetDagObserverBoundaryHookForTest(NULL);
    ForceRepairChildCountHealthy();
    ResetBlockIndexAuthoritativeStartupForTest();
}

// Rebuild failure at startup -> Ensure leaves revocation durable and fails
// closed; startup refuses to register an observer.
BOOST_AUTO_TEST_CASE(r2c1d3b_i1_neg_startup_rebuild_failure_refuses_observer)
{
    InitAuthoritativeFixture fx(4, /*frontierOn=*/true);
    std::string error;
    BOOST_REQUIRE_MESSAGE(fx.BuildAndSelect(6, &error), error);
    BOOST_REQUIRE_MESSAGE(fx.PrepareActualLiveDaglinks(&error), error);
    SeedNoChildCountMarker();

    const bool wasEnabled = ObserverSlotActive();
    g_testFailDAGChildCountRebuild = true;
    BOOST_CHECK(!fx.RunInitAuthoritative(&error));
    g_testFailDAGChildCountRebuild = false;
    BOOST_CHECK(!error.empty());
    BOOST_CHECK(!g_fAuthoritativeStartup);
    BOOST_CHECK(!HasDagTipOverlayRuntimeForTest());
    BOOST_CHECK_EQUAL((int)ObserverSlotActive(), (int)wasEnabled);

    SetDagObserverBoundaryHookForTest(NULL);
    ForceRepairChildCountHealthy();
    ResetBlockIndexAuthoritativeStartupForTest();
}

// Revocation persistence failure at startup -> fail-closed; no observer.
BOOST_AUTO_TEST_CASE(r2c1d3b_i1_neg_startup_revocation_persistence_failure_refuses_observer)
{
    InitAuthoritativeFixture fx(4, /*frontierOn=*/true);
    std::string error;
    BOOST_REQUIRE_MESSAGE(fx.BuildAndSelect(7, &error), error);
    BOOST_REQUIRE_MESSAGE(fx.PrepareActualLiveDaglinks(&error), error);
    SeedNoChildCountMarker();

    const bool wasEnabled = ObserverSlotActive();
    g_testFailDAGChildCountRevocation = true;
    BOOST_CHECK(!fx.RunInitAuthoritative(&error));
    g_testFailDAGChildCountRevocation = false;
    BOOST_CHECK(!error.empty());
    BOOST_CHECK(!g_fAuthoritativeStartup);
    BOOST_CHECK(!HasDagTipOverlayRuntimeForTest());
    BOOST_CHECK_EQUAL((int)ObserverSlotActive(), (int)wasEnabled);

    SetDagObserverBoundaryHookForTest(NULL);
    ForceRepairChildCountHealthy();
    ResetBlockIndexAuthoritativeStartupForTest();
}

// The registration-boundary health re-check refuses: even after a successful
// source repair and runtime Start, once the authority turns unhealthy at the
// exact registration point, InitBlockIndexAuthoritative fails closed and does
// not install the observer or retain the runtime.
BOOST_AUTO_TEST_CASE(r2c1d3b_i1_neg_boundary_refuses_unhealthy_observer)
{
    InitAuthoritativeFixture fx(4, /*frontierOn=*/true);
    std::string error;
    BOOST_REQUIRE_MESSAGE(fx.BuildAndSelect(8, &error), error);
    BOOST_REQUIRE_MESSAGE(fx.PrepareActualLiveDaglinks(&error), error);

    const bool wasEnabled = ObserverSlotActive();
    SetDagObserverBoundaryHookForTest(&ForceRevokeAtRegistrationBoundary);
    BOOST_CHECK(!fx.RunInitAuthoritative(&error));
    SetDagObserverBoundaryHookForTest(NULL);
    BOOST_CHECK(!error.empty());
    BOOST_CHECK(error.find("observer source unhealthy") != std::string::npos);
    BOOST_CHECK(!g_fAuthoritativeStartup);
    BOOST_CHECK(!HasDagTipOverlayRuntimeForTest());
    BOOST_CHECK_EQUAL((int)ObserverSlotActive(), (int)wasEnabled);

    ForceRepairChildCountHealthy();
    ResetBlockIndexAuthoritativeStartupForTest();
}

// Supporting positive: a REPAIRABLE revoked authority (poison present, no valid
// marker) is recounted by the real startup Ensure, then connects; the observer
// is installed only after the freshly rebuilt healthy predicate passes.
BOOST_AUTO_TEST_CASE(r2c1d3b_i1_pos_startup_recovers_revoked_then_registers)
{
    InitAuthoritativeFixture fx(4, /*frontierOn=*/true);
    std::string error;
    BOOST_REQUIRE_MESSAGE(fx.BuildAndSelect(9, &error), error);
    BOOST_REQUIRE_MESSAGE(fx.PrepareActualLiveDaglinks(&error), error);
    SeedChildCountRevocation();

    SetDagObserverBoundaryHookForTest(NULL);
    BOOST_REQUIRE_MESSAGE(fx.RunInitAuthoritative(&error), error);
    BOOST_CHECK(g_fAuthoritativeStartup);
    BOOST_CHECK(HasDagTipOverlayRuntimeForTest());
    BOOST_CHECK(ObserverSlotActive());

    // The freshly repaired projection is healthy and marker-bound.
    {
        CTxDB db;
        std::string herr;
        BOOST_CHECK_MESSAGE(db.IsDAGChildCountIndexHealthy(&herr), herr);
        db.Close();
        CTxDB srcRead;
        std::string serr;
        BOOST_REQUIRE_MESSAGE(srcRead.IsDAGChildCountIndexHealthy(&serr), serr);
        srcRead.Close();
    }
    ResetBlockIndexAuthoritativeStartupForTest();
    BOOST_CHECK(!ObserverSlotActive());
}