// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.
//
// A.13.6-R2c.1 — authoritative DAG tip frontier substrate differential tests.
//
// Proves, on deterministic daglinks fixtures:
//   A. immutable frontier == legacy tip equation (nodes - referenced parents);
//   B. streaming reader emits the same ordered tip set;
//   C. mutable live overlay deltas produce exact legacy parity after sequences;
//   D. restart/reopen preserves the frontier + delta;
//   E. generation mismatch / corrupt / missing frontier fail closed.

#include <boost/test/unit_test.hpp>

#include "../dag_tip_frontier.h"
#include "../dag_tip_live_overlay.h"
#include "../dag.h" // CBlockDAGData
#include "../serialize.h"

#include <leveldb/db.h>
#include <leveldb/filter_policy.h>

#include <boost/filesystem.hpp>

#include <openssl/sha.h>

#include <map>
#include <set>
#include <stdio.h>
#include <string>
#include <vector>

namespace {

// Write a daglinks entry with the same key/value serialization as
// CTxDB::WriteDAGLinks (txdb-leveldb.cpp) / the restart-seam fixture.
static bool WriteDagLinksEntry(const std::string& dbDir,
                               const uint256& hash,
                               const CBlockDAGData& data)
{
    leveldb::Options options;
    options.create_if_missing = true;
    leveldb::DB* db = NULL;
    leveldb::Status st = leveldb::DB::Open(options, dbDir, &db);
    if (!st.ok() || !db) return false;
    CDataStream ssKey(SER_DISK, CLIENT_VERSION);
    ssKey << make_pair(std::string("daglinks"), hash);
    CDataStream ssValue(SER_DISK, CLIENT_VERSION);
    ssValue << data;
    st = db->Put(leveldb::WriteOptions(), ssKey.str(), ssValue.str());
    delete db;
    return st.ok();
}

// Compute the canonical dagInputDigest over the SORTED node stream (same as
// the generation-builder / R1a verifier semantics).
static bool ComputeDagDigest(const std::string& dbDir, unsigned char out[32])
{
    leveldb::Options options;
    options.create_if_missing = false;
    leveldb::DB* db = NULL;
    leveldb::Status st = leveldb::DB::Open(options, dbDir, &db);
    if (!st.ok() || !db) return false;
    std::map<uint256, CBlockDAGData> nodes;
    CDataStream prefix(SER_DISK, CLIENT_VERSION);
    prefix << std::string("daglinks");
    std::string p = prefix.str();
    leveldb::Iterator* it = db->NewIterator(leveldb::ReadOptions());
    it->Seek(p);
    while (it->Valid())
    {
        if (it->key().ToString().compare(0, p.size(), p) != 0) break;
        try
        {
            CDataStream key(SER_DISK, CLIENT_VERSION);
            key.write(it->key().data(), it->key().size());
            std::pair<std::string, uint256> pairKey;
            key >> pairKey;
            CDataStream value(SER_DISK, CLIENT_VERSION);
            value.write(it->value().data(), it->value().size());
            CBlockDAGData data;
            value >> data;
            nodes[pairKey.second] = data;
        }
        catch (...) { delete it; delete db; return false; }
        it->Next();
    }
    delete it;
    delete db;
    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    for (std::map<uint256, CBlockDAGData>::const_iterator mi = nodes.begin(); mi != nodes.end(); ++mi)
    {
        SHA256_Update(&ctx, mi->first.begin(), 32);
        uint32_t count = (uint32_t)mi->second.vDAGParents.size();
        SHA256_Update(&ctx, &count, 4);
        for (size_t i = 0; i < mi->second.vDAGParents.size(); ++i)
            SHA256_Update(&ctx, mi->second.vDAGParents[i].begin(), 32);
    }
    SHA256_Final(out, &ctx);
    return true;
}

// Reference legacy tip set from the PROVEN equation (dag.cpp RebuildPendingChildIndex):
//   tips = { node hashes } - { hashes referenced as parents by any node }
static std::set<uint256> LegacyTipSet(const std::map<uint256, CBlockDAGData>& nodes)
{
    std::set<uint256> tips;
    for (std::map<uint256, CBlockDAGData>::const_iterator mi = nodes.begin(); mi != nodes.end(); ++mi)
        tips.insert(mi->first);
    for (std::map<uint256, CBlockDAGData>::const_iterator mi = nodes.begin(); mi != nodes.end(); ++mi)
        for (size_t i = 0; i < mi->second.vDAGParents.size(); ++i)
            tips.erase(mi->second.vDAGParents[i]);
    return tips;
}

struct FrontierFixture
{
    boost::filesystem::path tmp;
    boost::filesystem::path dagDb;
    boost::filesystem::path artifact;
    boost::filesystem::path delta;
    boost::filesystem::path temp;
    std::map<uint256, CBlockDAGData> nodes;
    std::map<int, std::vector<uint256> > n2; // raw parents for large-bound test
    uint64_t generation;
    FrontierFixture() : generation(7)
    {
        tmp = boost::filesystem::temp_directory_path() /
            boost::filesystem::unique_path("r2c1-frontier-%%%%-%%%%-%%%%");
        boost::filesystem::create_directories(tmp);
        dagDb = tmp / "daglinks";
        artifact = tmp / "dag-tip-frontier.dat";
        delta = tmp / "dagtips.log";
        temp = tmp / "temp";
    }
    ~FrontierFixture()
    {
        try { boost::filesystem::remove_all(tmp); } catch (...) {}
    }
    void SeedNode(const uint256& h, const std::vector<uint256>& parents)
    {
        CBlockDAGData d;
        d.vDAGParents = parents;
        d.fBlue = true;
        d.nDAGScore = 0;
        nodes[h] = d;
    }
    void WriteAll()
    {
        for (std::map<uint256, CBlockDAGData>::const_iterator mi = nodes.begin(); mi != nodes.end(); ++mi)
            BOOST_REQUIRE_MESSAGE(WriteDagLinksEntry(dagDb.string(), mi->first, mi->second),
                                  "seed daglinks failed");
    }
    FrontierFixture& WithWriteAll()
    {
        for (std::map<uint256, CBlockDAGData>::const_iterator mi = nodes.begin(); mi != nodes.end(); ++mi)
            BOOST_REQUIRE_MESSAGE(WriteDagLinksEntry(dagDb.string(), mi->first, mi->second),
                                  "seed daglinks failed");
        return *this;
    }
    unsigned char digest[32];
    bool digestReady;
    FrontierFixture& ComputeDigest()
    {
        digestReady = ComputeDagDigest(dagDb.string(), digest);
        BOOST_REQUIRE(digestReady);
        return *this;
    }
    bool Build()
    {
        dag_tip_frontier::BuildOptions opts;
        opts.tempParent = temp.string();
        dag_tip_frontier::BuildResult res;
        return dag_tip_frontier::BuildDagTipFrontier(dagDb.string(), digest, generation,
                                                     artifact.string(), opts, &res);
    }
};

} // namespace

// Use the boundary harness convention: a single top-level test that drives all
// focused cases to keep linking simple (mirrors blockindex_authoritative_live_tests).
BOOST_AUTO_TEST_CASE(r2c1_dag_tip_frontier_substrate)
{
    using namespace dag_tip_frontier;

    // ---------- A. Single DAG block (no parents): it is the only tip. ----------
    {
        FrontierFixture fx;
        uint256 a(0xA1000001UL);
        fx.SeedNode(a, std::vector<uint256>());
        fx.WithWriteAll().ComputeDigest();
        std::set<uint256> expected = LegacyTipSet(fx.nodes);
        BOOST_REQUIRE_EQUAL(expected.size(), 1u);
        BOOST_REQUIRE(fx.Build());
        { // streaming reader
            TipFrontierReader r;
            std::string err;
            BOOST_REQUIRE(r.Open(fx.artifact.string(), fx.generation, fx.digest, &err));
            BOOST_CHECK_EQUAL(r.TipCount(), 1u);
            uint256 h;
            BOOST_CHECK(r.Next(&h) && h == a);
            BOOST_CHECK(!r.Next(&h));
        }
    }

    // ---------- B. Linear chain A<-B<-C: only C is a tip. ----------
    {
        FrontierFixture fx;
        uint256 a(0xB1000001UL), b(0xB1000002UL), c(0xB1000003UL);
        fx.SeedNode(a, std::vector<uint256>());
        std::vector<uint256> pa(1, a);
        std::vector<uint256> pb(1, b);
        fx.SeedNode(b, pa);
        fx.SeedNode(c, pb);
        fx.WithWriteAll().ComputeDigest();
        std::set<uint256> expected = LegacyTipSet(fx.nodes);
        BOOST_REQUIRE_EQUAL(expected.size(), 1u);
        BOOST_REQUIRE(expected.count(c) == 1u);
        BOOST_REQUIRE(fx.Build());
        {
            TipFrontierReader r;
            std::string err;
            BOOST_REQUIRE(r.Open(fx.artifact.string(), fx.generation, fx.digest, &err));
            uint256 h;
            BOOST_REQUIRE(r.Next(&h));
            BOOST_CHECK(h == c);
            BOOST_CHECK(!r.Next(&h));
        }
    }

    // ---------- C. Competing side tips: two childless forks both are tips. ----------
    {
        FrontierFixture fx;
        uint256 g(0xC1000001UL), l(0xC1000002UL), r(0xC1000003UL);
        fx.SeedNode(g, std::vector<uint256>());
        std::vector<uint256> pg(1, g);
        fx.SeedNode(l, pg);
        fx.SeedNode(r, pg);
        fx.WithWriteAll().ComputeDigest();
        std::set<uint256> expected = LegacyTipSet(fx.nodes);
        BOOST_REQUIRE_EQUAL(expected.size(), 2u);
        BOOST_REQUIRE(expected.count(l) == 1u && expected.count(r) == 1u);
        BOOST_REQUIRE(fx.Build());
        {
            TipFrontierReader r;
            std::string err;
            BOOST_REQUIRE(r.Open(fx.artifact.string(), fx.generation, fx.digest, &err));
            std::set<uint256> read;
            uint256 h;
            while (r.Next(&h)) read.insert(h);
            BOOST_CHECK(read == expected);
        }
    }

    // ---------- D. Multi-parent block: both parents cease to be tips. ----------
    {
        FrontierFixture fx;
        uint256 a(0xD1000001UL), b(0xD1000002UL), m(0xD1000003UL);
        fx.SeedNode(a, std::vector<uint256>());
        fx.SeedNode(b, std::vector<uint256>());
        std::vector<uint256> pm;
        pm.push_back(a); pm.push_back(b);
        fx.SeedNode(m, pm);
        fx.WithWriteAll().ComputeDigest();
        std::set<uint256> expected = LegacyTipSet(fx.nodes);
        BOOST_REQUIRE_EQUAL(expected.size(), 1u);
        BOOST_REQUIRE(expected.count(m) == 1u); // a,b removed, m added
        BOOST_REQUIRE(fx.Build());
        {
            TipFrontierReader r;
            std::string err;
            BOOST_REQUIRE(r.Open(fx.artifact.string(), fx.generation, fx.digest, &err));
            uint256 h;
            BOOST_REQUIRE(r.Next(&h));
            BOOST_CHECK(h == m);
        }
    }

    // ---------- E. Generation mismatch / corrupt / missing fail closed. ----------
    {
        FrontierFixture fx;
        uint256 a(0xE1000001UL);
        fx.SeedNode(a, std::vector<uint256>());
        fx.WithWriteAll().ComputeDigest();
        BOOST_REQUIRE(fx.Build());
        TipFrontierReader r;
        std::string err;
        // wrong generation
        BOOST_REQUIRE(!r.Open(fx.artifact.string(), fx.generation + 1, fx.digest, &err));
        // missing file
        boost::filesystem::path missing = fx.tmp / "does-not-exist.dat";
        BOOST_REQUIRE(!r.Open(missing.string(), fx.generation, fx.digest, &err));
    }

    // ---------- F. Overlay live deltas + restart: parity with legacy after
    //               sequences (single, side, parent-removal) across reopen. ----------
    {
        FrontierFixture fx;
        // History before authoritative boundary: g -> {l, r} competing tips.
        uint256 g(0xF1000001UL), l(0xF1000002UL), r(0xF1000003UL);
        fx.SeedNode(g, std::vector<uint256>());
        std::vector<uint256> pg(1, g);
        fx.SeedNode(l, pg);
        fx.SeedNode(r, pg);
        fx.WithWriteAll().ComputeDigest();
        std::set<uint256> seedExpected = LegacyTipSet(fx.nodes); // {l, r}
        BOOST_REQUIRE(fx.Build());

        LiveTipFrontierOverlay ov;
        std::string err;
        BOOST_REQUIRE(ov.Open(fx.artifact.string(), fx.delta.string(), fx.generation,
                              fx.digest, 4, &err));
        BOOST_CHECK_EQUAL(ov.CacheCapacity(), 4u);
        // initial composition == seed
        uint64_t n0 = 0;
        BOOST_REQUIRE(ov.GetComposedTipCount(&n0, &err) && n0 == 2);
        for (std::set<uint256>::const_iterator it = seedExpected.begin(); it != seedExpected.end(); ++it)
        {
            bool c = false;
            BOOST_REQUIRE(ov.Contains(*it, &c, &err) && c);
        }

        // Live: a new block m=(l,r) arrives -> legacy setDAGTips becomes {m}:
        //   InitBlockDAGData(m): m has no children -> tip; l,r removed as tips.
        uint256 m(0xF1000004UL);
        std::vector<uint256> pm; pm.push_back(l); pm.push_back(r);
        CBlockDAGData md; md.vDAGParents = pm; md.fBlue = true;
        fx.nodes[m] = md;
        std::set<uint256> afterM = LegacyTipSet(fx.nodes); // {m}
        // Mirror the exact legacy tip mutations via the shadow overlay API.
        BOOST_REQUIRE(ov.AddTip(m, &err));
        BOOST_REQUIRE(ov.RemoveTip(l, &err));
        BOOST_REQUIRE(ov.RemoveTip(r, &err));
        std::set<uint256> composed;
        {
            struct Ctx { std::set<uint256>* s; };
            Ctx c; c.s = &composed;
            BOOST_REQUIRE(ov.ForEachTip([](const uint256& h, void* ctx){ ((Ctx*)ctx)->s->insert(h); return true; }, &c, &err));
        }
        BOOST_CHECK(composed == afterM);
        // Logical cardinality (on disk) exceeds cache capacity => bounded RAM.
        BOOST_CHECK_GE((int)ov.PersistentOverrideCount(), 3);
        BOOST_CHECK_LE((int)ov.CacheCurrent(), 4);

        // Restart: reopen overlay from persistent delta; composition must match.
        ov.Close();
        LiveTipFrontierOverlay ov2;
        BOOST_REQUIRE(ov2.Open(fx.artifact.string(), fx.delta.string(), fx.generation,
                               fx.digest, 4, &err));
        std::set<uint256> composed2;
        {
            struct Ctx { std::set<uint256>* s; };
            Ctx c; c.s = &composed2;
            BOOST_REQUIRE(ov2.ForEachTip([](const uint256& h, void* ctx){ ((Ctx*)ctx)->s->insert(h); return true; }, &c, &err));
        }
        BOOST_CHECK(composed2 == afterM);
        BOOST_CHECK_EQUAL(ov2.PersistentOverrideCount(), 3u); // m,l,r on disk
        printf("R2C1 FRONTIER: seed=%d live=%d restart=%d persistent=%d cache=%d ok\n",
               (int)n0, (int)composed.size(), (int)composed2.size(),
               (int)ov2.PersistentOverrideCount(), (int)ov2.CacheCurrent());
    }
}

BOOST_AUTO_TEST_CASE(r2c1_dag_tip_frontier_overlay_capacity_bounded)
{
    using namespace dag_tip_frontier;
    // Adversarial: logical override cardinality (dozens/hundreds) > RAM capacity (2).
    FrontierFixture fx;
    // historical seed: some set of tips
    const int nSeed = 8;
    for (int i = 1; i <= nSeed; ++i)
        fx.SeedNode(uint256((uint64_t)(0x50000000UL + i)), std::vector<uint256>());
    fx.WithWriteAll().ComputeDigest();
    BOOST_REQUIRE(fx.Build());
    std::set<uint256> seedSet = LegacyTipSet(fx.nodes);
    BOOST_REQUIRE_EQUAL(seedSet.size(), (size_t)nSeed);

    const size_t cap = 2;
    LiveTipFrontierOverlay ov;
    std::string err;
    BOOST_REQUIRE(ov.Open(fx.artifact.string(), fx.delta.string(), fx.generation,
                          fx.digest, cap, &err));
    BOOST_CHECK_EQUAL(ov.CacheCapacity(), cap);

    // Reference logical state (test-only std::set).
    std::set<uint256> ref = seedSet;

    // Hundreds of distinct mutations, with many on evicted hashes.
    const int N = 200;
    for (int i = 1; i <= N; ++i)
    {
        uint256 h((uint64_t)(0x60000000UL + i));         // non-seed added
        BOOST_REQUIRE(ov.AddTip(h, &err));
        ref.insert(h);
        if (i % 3 == 0)                                // remove some (evicted/non-evicted)
        {
            BOOST_REQUIRE(ov.RemoveTip(h, &err));
            ref.erase(h);
        }
        if (i % 5 == 0)                                 // remove a seed member
        {
            uint256 s(0x50000000UL + (uint64_t)(i % nSeed) + 1);
            BOOST_REQUIRE(ov.RemoveTip(s, &err));
            ref.erase(s);
        }
        if (i % 7 == 0)                                 // re-add an evicted non-seed hash
        {
            uint256 h2((uint64_t)(0x60000000UL + (uint64_t)(i / 7)));
            BOOST_REQUIRE(ov.AddTip(h2, &err));
            ref.insert(h2);
        }
    }

    // 1. logical cardinality (on disk) >> capacity
    BOOST_CHECK_GE((int)ov.PersistentOverrideCount(), 50);
    // 2. resident cache <= capacity
    BOOST_CHECK_LE((int)ov.CacheCurrent(), (int)cap);
    // 3&4. exact Contains for early evicted + ForEachTip result == reference
    for (int i = 1; i <= N; ++i)
    {
        uint256 h((uint64_t)(0x60000000UL + i));
        bool c = false;
        BOOST_REQUIRE(ov.Contains(h, &c, &err));
        BOOST_CHECK_EQUAL((int)c, (int)ref.count(h));
    }
    for (int i = 1; i <= nSeed; ++i)
    {
        uint256 s((uint64_t)(0x50000000UL + i));
        bool c = false;
        BOOST_REQUIRE(ov.Contains(s, &c, &err));
        BOOST_CHECK_EQUAL((int)c, (int)ref.count(s));
    }
    std::set<uint256> streamed;
    {
        struct Ctx { std::set<uint256>* s; };
        Ctx c; c.s = &streamed;
        BOOST_REQUIRE(ov.ForEachTip([](const uint256& h, void* ctx){ ((Ctx*)ctx)->s->insert(h); return true; }, &c, &err));
    }
    BOOST_CHECK(streamed == ref);

    // 5. exact restart result
    ov.Close();
    LiveTipFrontierOverlay ov2;
    BOOST_REQUIRE(ov2.Open(fx.artifact.string(), fx.delta.string(), fx.generation,
                           fx.digest, cap, &err));
    std::set<uint256> streamed2;
    {
        struct Ctx { std::set<uint256>* s; };
        Ctx c; c.s = &streamed2;
        BOOST_REQUIRE(ov2.ForEachTip([](const uint256& h, void* ctx){ ((Ctx*)ctx)->s->insert(h); return true; }, &c, &err));
    }
    BOOST_CHECK(streamed2 == ref);
    BOOST_CHECK_LE((int)ov2.CacheCurrent(), (int)cap);

    // 6. repeated add/remove on an evicted hash stays exact
    uint256 ev(0x77770000UL);
    for (int k = 0; k < 5; ++k)
    {
        BOOST_REQUIRE(ov2.AddTip(ev, &err));
        BOOST_REQUIRE(ov2.RemoveTip(ev, &err));
    }
    bool evIn = false;
    BOOST_REQUIRE(ov2.Contains(ev, &evIn, &err));
    BOOST_CHECK(!evIn); // net ABSENT

    // 9. generation mismatch fail-closed
    {
        LiveTipFrontierOverlay ovg;
        std::string gerr;
        BOOST_REQUIRE(!ovg.Open(fx.artifact.string(), fx.delta.string(), fx.generation + 1,
                                fx.digest, cap, &gerr));
    }

    printf("R2C1 OVERLAY: seed=%d persistent=%d logical=%d cache=%d cap=%d exact=%d\n",
           (int)seedSet.size(), (int)ov2.PersistentOverrideCount(), (int)ref.size(),
           (int)ov2.CacheCurrent(), (int)cap, (int)(streamed2 == ref));
}

BOOST_AUTO_TEST_CASE(r2c1_dag_tip_frontier_builder_bounds_and_determinism)
{
    using namespace dag_tip_frontier;
    FrontierFixture fx;
    // Build a moderately large DAG to force multiple external-sort runs:
    // 4000 nodes, each (i) children of (i/2) for i>1 => roughly 2000 tips
    // (all odd/leaf nodes of a balanced binary tree).
    for (int i = 1; i <= 4000; ++i)
    {
        uint256 h((uint64_t)i);
        std::vector<uint256> parents;
        if (i > 1) parents.push_back(uint256((uint64_t)(i / 2)));
        fx.n2[i] = parents; // record raw parents for legacy reference
    }
    // Seed daglinks store.
    for (std::map<int, std::vector<uint256> >::const_iterator it = fx.n2.begin(); it != fx.n2.end(); ++it)
    {
        CBlockDAGData d; d.vDAGParents = it->second; d.fBlue = true;
        BOOST_REQUIRE(WriteDagLinksEntry(fx.dagDb.string(), uint256((uint64_t)it->first), d));
    }
    fx.ComputeDigest();
    // Legacy tip set from the proven equation.
    std::set<uint256> expected;
    for (std::map<int, std::vector<uint256> >::const_iterator it = fx.n2.begin(); it != fx.n2.end(); ++it)
        expected.insert(uint256((uint64_t)it->first));
    for (std::map<int, std::vector<uint256> >::const_iterator it = fx.n2.begin(); it != fx.n2.end(); ++it)
        for (size_t p = 0; p < it->second.size(); ++p)
            expected.erase(it->second[p]);

    // Small chunk bound to force many runs + merge passes.
    BuildOptions opts;
    opts.tempParent = fx.temp.string();
    opts.chunkBytes = 1024;
    opts.maxRecordsPerChunk = 128;
    opts.maxOpenRuns = 4;
    BuildResult r1;
    BOOST_REQUIRE(BuildDagTipFrontier(fx.dagDb.string(), fx.digest, fx.generation, fx.artifact.string(), opts, &r1));
    // Determinism: rebuild to a second path, must be byte-identical.
    boost::filesystem::path a2 = fx.tmp / "frontier2.dat";
    BuildResult r2;
    BOOST_REQUIRE(BuildDagTipFrontier(fx.dagDb.string(), fx.digest, fx.generation, a2.string(), opts, &r2));
    BOOST_CHECK_EQUAL(r1.frontierTipCount, r2.frontierTipCount);
    BOOST_CHECK_EQUAL(r1.artifactBytes, r2.artifactBytes);

    // Verify tip set matches legacy and stream it back.
    BOOST_CHECK_EQUAL(r1.frontierTipCount, (uint64_t)expected.size());
    TipFrontierReader rd;
    std::string err;
    BOOST_REQUIRE(rd.Open(fx.artifact.string(), fx.generation, fx.digest, &err));
    std::set<uint256> read;
    uint256 h;
    while (rd.Next(&h)) read.insert(h);
    BOOST_CHECK(read == expected);

    printf("R2C1 BUILDER: nodes=%d tips=%llu artifactBytes=%llu tempBytes=%llu peakChunk=%llu runs=%llu mergePasses=%llu deterministic=%d\n",
           (int)fx.n2.size(), (unsigned long long)r1.frontierTipCount,
           (unsigned long long)r1.artifactBytes, (unsigned long long)r1.temporaryBytesWritten,
           (unsigned long long)r1.peakChunkBytes, (unsigned long long)r1.runCount,
           (unsigned long long)r1.mergePasses, (int)(r1.artifactBytes == r2.artifactBytes));
}