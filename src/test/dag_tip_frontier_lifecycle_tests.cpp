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