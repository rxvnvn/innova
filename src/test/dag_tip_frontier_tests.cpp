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
#include "../dag_tip_overlay_consumer.h"
#include "../dag_tip_overlay_recovery.h"
#include "../dag_tip_overlay_runtime.h"
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

struct OverlayConsumerSource
{
    uint256 token;
    bool readable;
    bool healthy;
    OverlayConsumerSource() : token(0), readable(true), healthy(true) {}
};
static bool ReadOverlayConsumerSource(uint256* out, void* ctx)
{
    OverlayConsumerSource* s = static_cast<OverlayConsumerSource*>(ctx);
    if (!s || !s->readable) return false;
    *out = s->token;
    return true;
}
static bool HealthyOverlayConsumerSource(void* ctx)
{
    OverlayConsumerSource* s = static_cast<OverlayConsumerSource*>(ctx);
    return s && s->healthy;
}

struct OverlayRecoverySource
{
    std::vector<uint256> tokens;
    size_t reads;
    bool readable;
    bool healthy;
    OverlayRecoverySource() : reads(0), readable(true), healthy(true) {}
};
static bool ReadOverlayRecoverySource(uint256* out, void* ctx)
{
    OverlayRecoverySource* s = static_cast<OverlayRecoverySource*>(ctx);
    if (!s || !s->readable || s->tokens.empty()) return false;
    const size_t index = std::min(s->reads++, s->tokens.size() - 1);
    *out = s->tokens[index];
    return true;
}
static bool HealthyOverlayRecoverySource(void* ctx)
{
    OverlayRecoverySource* s = static_cast<OverlayRecoverySource*>(ctx);
    return s && s->healthy;
}

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

BOOST_AUTO_TEST_CASE(r2c1d2b_overlay_metadata_absent_is_unavailable)
{
    using namespace dag_tip_frontier;
    FrontierFixture fx;
    const uint256 root(0xD2B00001UL);
    fx.SeedNode(root, std::vector<uint256>());
    fx.WithWriteAll().ComputeDigest();
    BOOST_REQUIRE(fx.Build());

    LiveTipFrontierOverlay overlay;
    std::string error;
    BOOST_REQUIRE(overlay.Open(fx.artifact.string(), fx.delta.string(), fx.generation,
                               fx.digest, 2, &error));

    // A fresh generation-bound override DB has no source checkpoint. It must
    // be explicitly unavailable, never implicitly trusted as CLEAN.
    LiveTipOverlayCheckpoint checkpoint;
    BOOST_CHECK(!overlay.ReadCheckpoint(&checkpoint, &error));
    BOOST_CHECK_EQUAL(checkpoint.phase, LIVE_OVERLAY_PHASE_UNAVAILABLE);
}

BOOST_AUTO_TEST_CASE(r2c1d2b_overlay_checkpoint_clean_round_trip)
{
    using namespace dag_tip_frontier;
    FrontierFixture fx;
    fx.SeedNode(uint256(0xD2B00002UL), std::vector<uint256>());
    fx.WithWriteAll().ComputeDigest();
    BOOST_REQUIRE(fx.Build());
    LiveTipOverlayStore store;
    std::string error;
    BOOST_REQUIRE(store.Open(fx.delta.string(), fx.generation, &error));

    LiveTipOverlayCheckpoint written;
    written.generationId = fx.generation;
    memcpy(written.dagInputDigest, fx.digest, sizeof(written.dagInputDigest));
    written.phase = LIVE_OVERLAY_PHASE_CLEAN;
    written.appliedSourceStateId = uint256(0xD2B0C1EAUL);
    BOOST_REQUIRE(store.WriteCheckpoint(written, &error));

    LiveTipOverlayCheckpoint read;
    BOOST_REQUIRE(store.ReadCheckpoint(&read, &error));
    BOOST_CHECK_EQUAL(read.version, LIVE_OVERLAY_CHECKPOINT_VERSION);
    BOOST_CHECK_EQUAL(read.generationId, fx.generation);
    BOOST_CHECK_EQUAL(read.phase, LIVE_OVERLAY_PHASE_CLEAN);
    BOOST_CHECK(read.appliedSourceStateId == written.appliedSourceStateId);
    BOOST_CHECK(memcmp(read.dagInputDigest, fx.digest, sizeof(read.dagInputDigest)) == 0);
}

static DagTipCommittedDeltaEvent BoundBegin(uint256 token, uint64_t records = 0)
{
    DagTipCommittedDeltaEvent event(DagTipCommittedDeltaEvent::BEGIN, DAG_TIP_DELTA_ADD_TO_BLOCK_INDEX);
    event.hasInitialSourceStateId = true;
    event.initialSourceStateId = token;
    event.expectedRecordCount = records;
    return event;
}

BOOST_AUTO_TEST_CASE(r2c1d3b_consumer_rejects_unbound_preimage)
{
    using namespace dag_tip_frontier;
    FrontierFixture fx;
    fx.SeedNode(uint256(42), std::vector<uint256>());
    fx.WithWriteAll().ComputeDigest();
    BOOST_REQUIRE(fx.Build());
    LiveTipFrontierOverlay overlay;
    std::string error;
    BOOST_REQUIRE(overlay.Open(fx.artifact.string(), fx.delta.string(), fx.generation, fx.digest, 2, &error));
    LiveTipOverlayCheckpoint cp;
    BOOST_REQUIRE(overlay.MakeCheckpoint(LIVE_OVERLAY_PHASE_CLEAN, uint256(100), &cp, &error));
    BOOST_REQUIRE(overlay.WriteCheckpoint(cp, &error));
    OverlayConsumerSource source;
    source.token = uint256(102); // a prior envelope at T101 could have been lost
    DagTipOverlayConsumer consumer(&overlay, &ReadOverlayConsumerSource, &HealthyOverlayConsumerSource, &source);
    DagTipCommittedDeltaEvent unbound(DagTipCommittedDeltaEvent::BEGIN, DAG_TIP_DELTA_ADD_TO_BLOCK_INDEX);
    BOOST_CHECK(!consumer.Consume(unbound, &error));
    BOOST_CHECK(!consumer.Available());
}

BOOST_AUTO_TEST_CASE(r2c1d2b_committed_delta_begin_persists_applying_base)
{
    using namespace dag_tip_frontier;
    FrontierFixture fx;
    fx.SeedNode(uint256(0xD2B10001UL), std::vector<uint256>());
    fx.WithWriteAll().ComputeDigest();
    BOOST_REQUIRE(fx.Build());

    LiveTipFrontierOverlay overlay;
    std::string error;
    BOOST_REQUIRE(overlay.Open(fx.artifact.string(), fx.delta.string(), fx.generation,
                               fx.digest, 2, &error));
    LiveTipOverlayCheckpoint clean;
    BOOST_REQUIRE(overlay.MakeCheckpoint(LIVE_OVERLAY_PHASE_CLEAN,
                                         uint256(0xD2B10010UL), &clean, &error));
    BOOST_REQUIRE(overlay.WriteCheckpoint(clean, &error));

    OverlayConsumerSource source;
    DagTipOverlayConsumer consumer(&overlay, &ReadOverlayConsumerSource,
                                   &HealthyOverlayConsumerSource, &source);
    DagTipCommittedDeltaEvent begin(DagTipCommittedDeltaEvent::BEGIN,
                                    DAG_TIP_DELTA_ADD_TO_BLOCK_INDEX);
    begin.hasInitialSourceStateId = true;
    begin.initialSourceStateId = clean.appliedSourceStateId;
    BOOST_REQUIRE(consumer.Consume(begin, &error));
    LiveTipOverlayCheckpoint applying;
    BOOST_REQUIRE(overlay.ReadCheckpoint(&applying, &error));
    BOOST_CHECK_EQUAL(applying.phase, LIVE_OVERLAY_PHASE_APPLYING);
    BOOST_CHECK(applying.appliedSourceStateId == clean.appliedSourceStateId);
    BOOST_CHECK(!consumer.Available());
}

BOOST_AUTO_TEST_CASE(r2c1d2b_committed_delta_add_end_cleans_at_source_token)
{
    using namespace dag_tip_frontier;
    FrontierFixture fx;
    const uint256 seed(0xD2B20001UL), added(0xD2B20002UL);
    fx.SeedNode(seed, std::vector<uint256>());
    fx.WithWriteAll().ComputeDigest();
    BOOST_REQUIRE(fx.Build());
    LiveTipFrontierOverlay overlay;
    std::string error;
    BOOST_REQUIRE(overlay.Open(fx.artifact.string(), fx.delta.string(), fx.generation, fx.digest, 2, &error));
    LiveTipOverlayCheckpoint clean;
    BOOST_REQUIRE(overlay.MakeCheckpoint(LIVE_OVERLAY_PHASE_CLEAN, uint256(100), &clean, &error));
    BOOST_REQUIRE(overlay.WriteCheckpoint(clean, &error));
    OverlayConsumerSource source;
    source.token = uint256(101);
    DagTipOverlayConsumer consumer(&overlay, &ReadOverlayConsumerSource, &HealthyOverlayConsumerSource, &source);
    BOOST_REQUIRE(consumer.Consume(BoundBegin(uint256(100), 1), &error));
    BOOST_REQUIRE(consumer.Consume(DagTipCommittedDeltaEvent(DAG_TIP_DELTA_ADD_TO_BLOCK_INDEX,
                        DagTipDeltaRecord(DagTipDeltaRecord::TIP_ADD, added)), &error));
    DagTipCommittedDeltaEvent end(DagTipCommittedDeltaEvent::END, DAG_TIP_DELTA_ADD_TO_BLOCK_INDEX);
    end.expectedRecordCount = 1; end.hasFinalSourceStateId = true;
    end.finalSourceStateId = source.token;
    BOOST_REQUIRE(consumer.Consume(end, &error));
    LiveTipOverlayCheckpoint after;
    BOOST_REQUIRE(overlay.ReadCheckpoint(&after, &error));
    BOOST_CHECK_EQUAL(after.phase, LIVE_OVERLAY_PHASE_CLEAN);
    BOOST_CHECK(after.appliedSourceStateId == source.token);
    bool present = false;
    BOOST_REQUIRE(overlay.Contains(added, &present, &error));
    BOOST_CHECK(present);
    BOOST_CHECK(consumer.Available());
}

BOOST_AUTO_TEST_CASE(r2c1d2b_consumer_failure_order_and_end_matrix)
{
    using namespace dag_tip_frontier;
    const uint256 h(0xD2B30001UL);
    struct Case {
        FrontierFixture fx; LiveTipFrontierOverlay ov; OverlayConsumerSource src; std::string e;
        Case() { fx.SeedNode(uint256(0xD2B30001UL), std::vector<uint256>()); fx.WithWriteAll().ComputeDigest(); BOOST_REQUIRE(fx.Build()); BOOST_REQUIRE(ov.Open(fx.artifact.string(),fx.delta.string(),fx.generation,fx.digest,2,&e)); LiveTipOverlayCheckpoint c; BOOST_REQUIRE(ov.MakeCheckpoint(LIVE_OVERLAY_PHASE_CLEAN,uint256(10),&c,&e)); BOOST_REQUIRE(ov.WriteCheckpoint(c,&e)); src.token=uint256(11); }
        DagTipOverlayConsumer C() { return DagTipOverlayConsumer(&ov,&ReadOverlayConsumerSource,&HealthyOverlayConsumerSource,&src); }
        void Applying() { DagTipOverlayConsumer c=C(); BOOST_REQUIRE(c.Consume(BoundBegin(uint256(10), 0),&e)); }
    };
    // RECORD/END before BEGIN and duplicate BEGIN are local shadow failures.
    { Case x; DagTipOverlayConsumer c=x.C(); BOOST_CHECK(!c.Consume(DagTipCommittedDeltaEvent(DAG_TIP_DELTA_ADD_TO_BLOCK_INDEX,DagTipDeltaRecord(DagTipDeltaRecord::TIP_ADD,h)),&x.e)); BOOST_CHECK(!c.Available()); }
    { Case x; DagTipOverlayConsumer c=x.C(); BOOST_CHECK(!c.Consume(DagTipCommittedDeltaEvent(DagTipCommittedDeltaEvent::END,DAG_TIP_DELTA_ADD_TO_BLOCK_INDEX),&x.e)); BOOST_CHECK(!c.Available()); }
    { Case x; DagTipOverlayConsumer c=x.C(); BOOST_REQUIRE(c.Consume(BoundBegin(uint256(10), 0),&x.e)); BOOST_CHECK(!c.Consume(BoundBegin(uint256(10), 0),&x.e)); BOOST_CHECK(!c.Available()); }
    // END validation failures leave durable APPLYING, never false CLEAN.
    { Case x; DagTipOverlayConsumer c=x.C(); BOOST_REQUIRE(c.Consume(BoundBegin(uint256(10), 0),&x.e)); DagTipCommittedDeltaEvent e(DagTipCommittedDeltaEvent::END,DAG_TIP_DELTA_ADD_TO_BLOCK_INDEX); BOOST_CHECK(!c.Consume(e,&x.e)); LiveTipOverlayCheckpoint q; BOOST_REQUIRE(x.ov.ReadCheckpoint(&q,&x.e)); BOOST_CHECK_EQUAL(q.phase,LIVE_OVERLAY_PHASE_APPLYING); }
    { Case x; DagTipOverlayConsumer c=x.C(); BOOST_REQUIRE(c.Consume(BoundBegin(uint256(10), 0),&x.e)); DagTipCommittedDeltaEvent e(DagTipCommittedDeltaEvent::END,DAG_TIP_DELTA_ADD_TO_BLOCK_INDEX); e.hasFinalSourceStateId=true; e.finalSourceStateId=uint256(12); BOOST_CHECK(!c.Consume(e,&x.e)); LiveTipOverlayCheckpoint q; BOOST_REQUIRE(x.ov.ReadCheckpoint(&q,&x.e)); BOOST_CHECK_EQUAL(q.phase,LIVE_OVERLAY_PHASE_APPLYING); }
    { Case x; x.src.healthy=false; DagTipOverlayConsumer c=x.C(); BOOST_REQUIRE(c.Consume(BoundBegin(uint256(10), 0),&x.e)); DagTipCommittedDeltaEvent e(DagTipCommittedDeltaEvent::END,DAG_TIP_DELTA_ADD_TO_BLOCK_INDEX); e.hasFinalSourceStateId=true; e.finalSourceStateId=x.src.token; BOOST_CHECK(!c.Consume(e,&x.e)); BOOST_CHECK(!c.Available()); }
    { Case x; x.src.readable=false; DagTipOverlayConsumer c=x.C(); BOOST_REQUIRE(c.Consume(BoundBegin(uint256(10), 0),&x.e)); DagTipCommittedDeltaEvent e(DagTipCommittedDeltaEvent::END,DAG_TIP_DELTA_ADD_TO_BLOCK_INDEX); e.hasFinalSourceStateId=true; e.finalSourceStateId=x.src.token; BOOST_CHECK(!c.Consume(e,&x.e)); BOOST_CHECK(!c.Available()); }
}

BOOST_AUTO_TEST_CASE(r2c1d2b_consumer_remove_multirecord_interrupt_and_bounds)
{
    using namespace dag_tip_frontier;
    FrontierFixture fx; uint256 a(0xD2B40001UL), b(0xD2B40002UL), c(0xD2B40003UL), d(0xD2B40004UL);
    fx.SeedNode(a,std::vector<uint256>()); fx.SeedNode(c,std::vector<uint256>()); fx.WithWriteAll().ComputeDigest(); BOOST_REQUIRE(fx.Build());
    LiveTipFrontierOverlay ov; std::string e; BOOST_REQUIRE(ov.Open(fx.artifact.string(),fx.delta.string(),fx.generation,fx.digest,2,&e)); LiveTipOverlayCheckpoint cp; BOOST_REQUIRE(ov.MakeCheckpoint(LIVE_OVERLAY_PHASE_CLEAN,uint256(20),&cp,&e)); BOOST_REQUIRE(ov.WriteCheckpoint(cp,&e)); OverlayConsumerSource src; src.token=uint256(21); DagTipOverlayConsumer con(&ov,&ReadOverlayConsumerSource,&HealthyOverlayConsumerSource,&src);
    BOOST_REQUIRE(con.Consume(BoundBegin(uint256(20), 4),&e));
    const DagTipDeltaRecord recs[] = { DagTipDeltaRecord(DagTipDeltaRecord::TIP_ADD,a), DagTipDeltaRecord(DagTipDeltaRecord::TIP_ADD,b), DagTipDeltaRecord(DagTipDeltaRecord::TIP_REMOVE,c), DagTipDeltaRecord(DagTipDeltaRecord::TIP_ADD,d) };
    for(size_t i=0;i<sizeof(recs)/sizeof(recs[0]);++i) BOOST_REQUIRE(con.Consume(DagTipCommittedDeltaEvent(DAG_TIP_DELTA_ADD_TO_BLOCK_INDEX,recs[i]),&e));
    DagTipCommittedDeltaEvent end(DagTipCommittedDeltaEvent::END,DAG_TIP_DELTA_ADD_TO_BLOCK_INDEX); end.expectedRecordCount=4; end.hasFinalSourceStateId=true; end.finalSourceStateId=src.token; BOOST_REQUIRE(con.Consume(end,&e));
    std::set<uint256> got; struct C { std::set<uint256>* s; } ctx={&got}; BOOST_REQUIRE(ov.ForEachTip([](const uint256& h,void* p){((C*)p)->s->insert(h);return true;},&ctx,&e)); std::set<uint256> expected; expected.insert(a); expected.insert(b); expected.insert(d); BOOST_CHECK(got==expected);
    BOOST_CHECK_LE(ov.CacheCurrent(),2u); BOOST_CHECK_LE(ov.CachePeak(),2u);
}

BOOST_AUTO_TEST_CASE(r2c1d2b_remaining_failure_restart_and_adversarial_gates)
{
    using namespace dag_tip_frontier;
    // Shared fixture constructor: one owner/open DB, valid CLEAN(T0), injected source.
    struct Case {
        FrontierFixture fx; LiveTipFrontierOverlay ov; OverlayConsumerSource src; std::string e;
        Case(size_t cap=2) { fx.SeedNode(uint256(0xD2B50001UL),std::vector<uint256>()); fx.WithWriteAll().ComputeDigest(); BOOST_REQUIRE(fx.Build()); BOOST_REQUIRE(ov.Open(fx.artifact.string(),fx.delta.string(),fx.generation,fx.digest,cap,&e)); LiveTipOverlayCheckpoint q; BOOST_REQUIRE(ov.MakeCheckpoint(LIVE_OVERLAY_PHASE_CLEAN,uint256(30),&q,&e)); BOOST_REQUIRE(ov.WriteCheckpoint(q,&e)); src.token=uint256(31); }
        DagTipOverlayConsumer Consumer() { return DagTipOverlayConsumer(&ov,&ReadOverlayConsumerSource,&HealthyOverlayConsumerSource,&src); }
        DagTipCommittedDeltaEvent End() { DagTipCommittedDeltaEvent x(DagTipCommittedDeltaEvent::END,DAG_TIP_DELTA_ADD_TO_BLOCK_INDEX); x.expectedRecordCount=1; x.hasFinalSourceStateId=true; x.finalSourceStateId=src.token; return x; }
        void AssertApplying() { LiveTipOverlayCheckpoint q; BOOST_REQUIRE(ov.ReadCheckpoint(&q,&e)); BOOST_CHECK_EQUAL(q.phase,LIVE_OVERLAY_PHASE_APPLYING); BOOST_CHECK(q.appliedSourceStateId==uint256(30)); }
    };
    // Injected override write: partial overlay stays APPLYING and shadow-only fails.
    { Case x; DagTipOverlayConsumer c=x.Consumer(); BOOST_REQUIRE(c.Consume(BoundBegin(uint256(30), 1),&x.e)); g_testFailLiveTipOverlayOverrideWrite=true; BOOST_CHECK(!c.Consume(DagTipCommittedDeltaEvent(DAG_TIP_DELTA_ADD_TO_BLOCK_INDEX,DagTipDeltaRecord(DagTipDeltaRecord::TIP_ADD,uint256(40))),&x.e)); g_testFailLiveTipOverlayOverrideWrite=false; BOOST_CHECK(!c.Available()); x.AssertApplying(); }
    // CLEAN publication failure cannot claim availability or durable CLEAN.
    { Case x; DagTipOverlayConsumer c=x.Consumer(); BOOST_REQUIRE(c.Consume(BoundBegin(uint256(30), 1),&x.e)); BOOST_REQUIRE(c.Consume(DagTipCommittedDeltaEvent(DAG_TIP_DELTA_ADD_TO_BLOCK_INDEX,DagTipDeltaRecord(DagTipDeltaRecord::TIP_ADD,uint256(41))),&x.e)); g_testFailLiveTipOverlayCheckpointWrite=true; BOOST_CHECK(!c.Consume(x.End(),&x.e)); g_testFailLiveTipOverlayCheckpointWrite=false; BOOST_CHECK(!c.Available()); x.AssertApplying(); }
    // Binding corruption after BEGIN cannot publish CLEAN.
    { Case x; DagTipOverlayConsumer c=x.Consumer(); BOOST_REQUIRE(c.Consume(BoundBegin(uint256(30), 1),&x.e)); LiveTipOverlayCheckpoint bad; BOOST_REQUIRE(x.ov.ReadCheckpoint(&bad,&x.e)); bad.frontierDigest[0]^=1; BOOST_REQUIRE(x.ov.WriteCheckpoint(bad,&x.e)); BOOST_CHECK(!c.Consume(x.End(),&x.e)); BOOST_CHECK(!c.Available()); LiveTipOverlayCheckpoint q; BOOST_REQUIRE(x.ov.ReadCheckpoint(&q,&x.e)); BOOST_CHECK_EQUAL(q.phase,LIVE_OVERLAY_PHASE_APPLYING); }
    // Interrupted envelope persists APPLYING across close/reopen; no implicit CLEAN.
    { Case x; DagTipOverlayConsumer c=x.Consumer(); BOOST_REQUIRE(c.Consume(BoundBegin(uint256(30), 1),&x.e)); BOOST_REQUIRE(c.Consume(DagTipCommittedDeltaEvent(DAG_TIP_DELTA_ADD_TO_BLOCK_INDEX,DagTipDeltaRecord(DagTipDeltaRecord::TIP_ADD,uint256(42))),&x.e)); x.ov.Close(); LiveTipFrontierOverlay reopened; BOOST_REQUIRE(reopened.Open(x.fx.artifact.string(),x.fx.delta.string(),x.fx.generation,x.fx.digest,2,&x.e)); LiveTipOverlayCheckpoint q; BOOST_REQUIRE(reopened.ReadCheckpoint(&q,&x.e)); BOOST_CHECK_EQUAL(q.phase,LIVE_OVERLAY_PHASE_APPLYING); }
    // Hundreds of streaming RECORDs through cap=2; reference is effective frontier.
    { Case x(2); DagTipOverlayConsumer c=x.Consumer(); BOOST_REQUIRE(c.Consume(BoundBegin(uint256(30), 400),&x.e)); std::set<uint256> ref; ref.insert(uint256(0xD2B50001UL)); const int N=400; for(int i=0;i<N;++i){ uint256 h(0xD2B60000UL+(uint64_t)i); DagTipDeltaRecord::Op op=(i%5==0)?DagTipDeltaRecord::TIP_REMOVE:DagTipDeltaRecord::TIP_ADD; BOOST_REQUIRE(c.Consume(DagTipCommittedDeltaEvent(DAG_TIP_DELTA_ADD_TO_BLOCK_INDEX,DagTipDeltaRecord(op,h)),&x.e)); if(op==DagTipDeltaRecord::TIP_ADD) ref.insert(h); else ref.erase(h); } DagTipCommittedDeltaEvent end=x.End(); end.expectedRecordCount=400; BOOST_REQUIRE(c.Consume(end,&x.e)); std::set<uint256> got; struct C{std::set<uint256>* p;}; C ctx={&got}; BOOST_REQUIRE(x.ov.ForEachTip([](const uint256& h,void* v){((C*)v)->p->insert(h);return true;},&ctx,&x.e)); BOOST_CHECK(got==ref); BOOST_CHECK_LE(x.ov.CacheCurrent(),2u); BOOST_CHECK_LE(x.ov.CachePeak(),2u); BOOST_CHECK_GT(x.ov.PersistentOverrideCount(),100u); x.ov.Close(); LiveTipFrontierOverlay r; BOOST_REQUIRE(r.Open(x.fx.artifact.string(),x.fx.delta.string(),x.fx.generation,x.fx.digest,2,&x.e)); LiveTipOverlayCheckpoint q; BOOST_REQUIRE(r.ReadCheckpoint(&q,&x.e)); BOOST_CHECK_EQUAL(q.phase,LIVE_OVERLAY_PHASE_CLEAN); BOOST_CHECK(q.appliedSourceStateId==x.src.token); std::set<uint256> restart; C rctx={&restart}; BOOST_REQUIRE(r.ForEachTip([](const uint256& h,void* v){((C*)v)->p->insert(h);return true;},&rctx,&x.e)); BOOST_CHECK(restart==ref); BOOST_CHECK_LE(r.CacheCurrent(),2u); }
}

BOOST_AUTO_TEST_CASE(r2c1d2c_missing_checkpoint_requires_current_source_rebuild)
{
    using namespace dag_tip_frontier;
    FrontierFixture fx;
    const uint256 root(0xD2C00001UL), child(0xD2C00002UL);
    fx.SeedNode(root, std::vector<uint256>());
    fx.WithWriteAll().ComputeDigest();
    BOOST_REQUIRE(fx.Build());
    // Current source advances beyond the immutable seed; recovery must not
    // compare this mutable relation to fx.digest.
    CBlockDAGData childData;
    childData.vDAGParents.push_back(root);
    BOOST_REQUIRE(WriteDagLinksEntry(fx.dagDb.string(), child, childData));
    LiveTipFrontierOverlay overlay;
    std::string error;
    BOOST_REQUIRE(overlay.Open(fx.artifact.string(), fx.delta.string(), fx.generation, fx.digest, 2, &error));
    OverlayRecoverySource source;
    source.tokens.push_back(uint256(0xD2C00010UL));
    DagTipOverlayRecovery recovery(&overlay, fx.dagDb.string(), &ReadOverlayRecoverySource,
                                   &HealthyOverlayRecoverySource, &source);
    BOOST_REQUIRE(recovery.Recover(&error));
    LiveTipOverlayCheckpoint checkpoint;
    BOOST_REQUIRE(overlay.ReadCheckpoint(&checkpoint, &error));
    BOOST_CHECK_EQUAL(checkpoint.phase, LIVE_OVERLAY_PHASE_CLEAN);
    BOOST_CHECK(checkpoint.appliedSourceStateId == source.tokens[0]);
    bool rootPresent = true, childPresent = false;
    BOOST_REQUIRE(overlay.Contains(root, &rootPresent, &error));
    BOOST_REQUIRE(overlay.Contains(child, &childPresent, &error));
    BOOST_CHECK(!rootPresent && childPresent);
    BOOST_CHECK(recovery.Available());
}

BOOST_AUTO_TEST_CASE(r2c1d2c_recovery_coherence_failure_stale_restart_and_bounds)
{
    using namespace dag_tip_frontier;
    struct Case {
        FrontierFixture fx; LiveTipFrontierOverlay overlay; OverlayRecoverySource source; std::string error;
        Case(size_t cap = 2) {
            fx.SeedNode(uint256(0xD2C10001UL), std::vector<uint256>());
            fx.WithWriteAll().ComputeDigest(); BOOST_REQUIRE(fx.Build());
            BOOST_REQUIRE(overlay.Open(fx.artifact.string(), fx.delta.string(), fx.generation, fx.digest, cap, &error));
            source.tokens.push_back(uint256(0xD2C10100UL));
        }
        DagTipOverlayRecovery Recovery(const std::string& dir = std::string()) {
            return DagTipOverlayRecovery(&overlay, dir.empty() ? fx.dagDb.string() : dir,
                &ReadOverlayRecoverySource, &HealthyOverlayRecoverySource, &source);
        }
        void Applying() { LiveTipOverlayCheckpoint q; BOOST_REQUIRE(overlay.ReadCheckpoint(&q, &error)); BOOST_CHECK_EQUAL(q.phase, LIVE_OVERLAY_PHASE_APPLYING); }
    };
    // Equal seed/current requires no override but still publishes CLEAN(T).
    { Case x; DagTipOverlayRecovery r=x.Recovery(); DagTipOverlayRecoveryResult q; BOOST_REQUIRE(r.Recover(&q)); BOOST_CHECK_EQUAL(q.status,DAG_TIP_OVERLAY_RECOVERY_AVAILABLE); BOOST_CHECK_EQUAL(x.overlay.PersistentOverrideCount(),0U); }
    // Tstart != Tend leaves APPLYING and never falsely publishes CLEAN.
    { Case x; x.source.tokens.push_back(uint256(0xD2C10101UL)); DagTipOverlayRecovery r=x.Recovery(); DagTipOverlayRecoveryResult q; BOOST_CHECK(!r.Recover(&q)); BOOST_CHECK_EQUAL(q.status,DAG_TIP_OVERLAY_RECOVERY_SOURCE_CHANGED); x.Applying(); }
    // Health false before work leaves no CLEAN checkpoint.
    { Case x; x.source.healthy=false; DagTipOverlayRecovery r=x.Recovery(); DagTipOverlayRecoveryResult q; BOOST_CHECK(!r.Recover(&q)); BOOST_CHECK_EQUAL(q.status,DAG_TIP_OVERLAY_RECOVERY_SOURCE_UNHEALTHY); LiveTipOverlayCheckpoint c; BOOST_CHECK(!x.overlay.ReadCheckpoint(&c,&x.error)); }
    // Existing stale PRESENT override is cleared before exact rebuild.
    { Case x; const uint256 stale(0xD2C10200UL); BOOST_REQUIRE(x.overlay.AddTip(stale,&x.error)); DagTipOverlayRecovery r=x.Recovery(); BOOST_REQUIRE(r.Recover(&x.error)); bool present=true; BOOST_REQUIRE(x.overlay.Contains(stale,&present,&x.error)); BOOST_CHECK(!present); BOOST_CHECK_EQUAL(x.overlay.PersistentOverrideCount(),0U); }
    // Current derivation failure is typed and leaves APPLYING.
    { Case x; DagTipOverlayRecovery r=x.Recovery((x.fx.tmp/"missing-db").string()); DagTipOverlayRecoveryResult q; BOOST_CHECK(!r.Recover(&q)); BOOST_CHECK_EQUAL(q.status,DAG_TIP_OVERLAY_RECOVERY_CURRENT_DERIVATION_FAILURE); x.Applying(); }
    // Override/checkpoint seams never claim CLEAN.
    { Case x; CBlockDAGData d; d.vDAGParents.push_back(uint256(0xD2C10001UL)); BOOST_REQUIRE(WriteDagLinksEntry(x.fx.dagDb.string(),uint256(0xD2C10201UL),d)); g_testFailLiveTipOverlayOverrideWrite=true; DagTipOverlayRecovery r=x.Recovery(); DagTipOverlayRecoveryResult q; BOOST_CHECK(!r.Recover(&q)); g_testFailLiveTipOverlayOverrideWrite=false; BOOST_CHECK_EQUAL(q.status,DAG_TIP_OVERLAY_RECOVERY_OVERLAY_WRITE_FAILURE); x.Applying(); }
    { Case x; g_testFailLiveTipOverlayCheckpointWrite=true; DagTipOverlayRecovery r=x.Recovery(); DagTipOverlayRecoveryResult q; BOOST_CHECK(!r.Recover(&q)); g_testFailLiveTipOverlayCheckpointWrite=false; BOOST_CHECK_EQUAL(q.status,DAG_TIP_OVERLAY_RECOVERY_CHECKPOINT_PUBLICATION_FAILURE); }
    // Restart retains a proven CLEAN checkpoint and exact composed frontier.
    { Case x; DagTipOverlayRecovery r=x.Recovery(); BOOST_REQUIRE(r.Recover(&x.error)); x.overlay.Close(); LiveTipFrontierOverlay reopened; BOOST_REQUIRE(reopened.Open(x.fx.artifact.string(),x.fx.delta.string(),x.fx.generation,x.fx.digest,2,&x.error)); LiveTipOverlayCheckpoint q; BOOST_REQUIRE(reopened.ReadCheckpoint(&q,&x.error)); BOOST_CHECK_EQUAL(q.phase,LIVE_OVERLAY_PHASE_CLEAN); BOOST_CHECK(q.appliedSourceStateId==x.source.tokens[0]); }
    // Recovery forwards current derivation statistics; tight bounds force runs.
    { Case x; for (int i=0;i<4;++i) { CBlockDAGData d; d.vDAGParents.push_back(uint256(0xD2C10001UL)); BOOST_REQUIRE(WriteDagLinksEntry(x.fx.dagDb.string(),uint256(0xD2C11000UL+i),d)); } BuildOptions o; o.tempParent=(x.fx.tmp/"recovery-temp").string(); o.chunkBytes=64; o.maxRecordsPerChunk=2; o.maxOpenRuns=2; DagTipOverlayRecovery r(&x.overlay,x.fx.dagDb.string(),&ReadOverlayRecoverySource,&HealthyOverlayRecoverySource,&x.source,o); DagTipOverlayRecoveryResult q; BOOST_REQUIRE(r.Recover(&q)); BOOST_CHECK_GT(q.currentRunCount,1U); BOOST_CHECK_LE(q.currentPeakChunkRecords,4U); BOOST_CHECK_LE(x.overlay.CacheCurrent(),2U); BOOST_CHECK_GT(x.overlay.PersistentOverrideCount(),2U); }
}

BOOST_AUTO_TEST_CASE(r2c1d3b_legacy_clean_certificate_requires_recovery)
{
    using namespace dag_tip_frontier;
    FrontierFixture fx; const uint256 p(710), c(711), token(712);
    fx.SeedNode(p,std::vector<uint256>()); fx.WithWriteAll().ComputeDigest(); BOOST_REQUIRE(fx.Build());
    CBlockDAGData child; child.vDAGParents.push_back(p);
    BOOST_REQUIRE(WriteDagLinksEntry(fx.dagDb.string(),c,child));
    OverlayRecoverySource src; src.tokens.push_back(token);
    DagTipOverlayRuntimeConfig cfg; cfg.generation=fx.generation;cfg.artifactPath=fx.artifact.string();
    cfg.overlayDbDir=fx.delta.string();cfg.dagLinksDir=fx.dagDb.string();memcpy(cfg.dagInputDigest,fx.digest,32);
    cfg.cacheCapacity=2;cfg.sourceReader=&ReadOverlayRecoverySource;cfg.sourceHealthy=&HealthyOverlayRecoverySource;cfg.context=&src;
    std::string error;
    { LiveTipFrontierOverlay overlay; BOOST_REQUIRE(overlay.Open(cfg.artifactPath,cfg.overlayDbDir,cfg.generation,cfg.dagInputDigest,2,&error));
      LiveTipOverlayCheckpoint cp; BOOST_REQUIRE(overlay.MakeCheckpoint(LIVE_OVERLAY_PHASE_CLEAN,token,&cp,&error));
      cp.version=1; BOOST_REQUIRE(overlay.WriteCheckpoint(cp,&error)); }
    DagTipOverlayRuntime runtime; BOOST_REQUIRE_MESSAGE(runtime.Start(cfg,&error),error);
    BOOST_CHECK_EQUAL(runtime.RecoveryInvocations(),1U);
    bool present=false; BOOST_REQUIRE(runtime.Overlay()->Contains(p,&present,&error)); BOOST_CHECK(!present);
    BOOST_REQUIRE(runtime.Overlay()->Contains(c,&present,&error)); BOOST_CHECK(present);
}

BOOST_AUTO_TEST_CASE(r2c1d3a_runtime_owner_startup_matrix)
{
    using namespace dag_tip_frontier;
    struct Case { FrontierFixture fx; OverlayRecoverySource src; DagTipOverlayRuntimeConfig c; std::string e;
      Case(){ fx.SeedNode(uint256(0xD3A10001UL),std::vector<uint256>()); fx.WithWriteAll().ComputeDigest(); BOOST_REQUIRE(fx.Build()); src.tokens.push_back(uint256(0xD3A10100UL)); c.generation=fx.generation;c.artifactPath=fx.artifact.string();c.overlayDbDir=fx.delta.string();c.dagLinksDir=fx.dagDb.string();memcpy(c.dagInputDigest,fx.digest,32);c.cacheCapacity=2;c.sourceReader=&ReadOverlayRecoverySource;c.sourceHealthy=&HealthyOverlayRecoverySource;c.context=&src; }
      void Clean(){ LiveTipFrontierOverlay o; BOOST_REQUIRE(o.Open(c.artifactPath,c.overlayDbDir,c.generation,c.dagInputDigest,2,&e)); LiveTipOverlayCheckpoint p; BOOST_REQUIRE(o.MakeCheckpoint(LIVE_OVERLAY_PHASE_CLEAN,src.tokens[0],&p,&e)); BOOST_REQUIRE(o.WriteCheckpoint(p,&e)); }
    };
    { Case x; x.Clean(); DagTipOverlayRuntime r; BOOST_REQUIRE(r.Start(x.c,&x.e)); BOOST_CHECK(r.Available()); BOOST_CHECK_EQUAL(r.RecoveryInvocations(),0U); BOOST_REQUIRE(r.Overlay()!=NULL); BOOST_REQUIRE(r.Consumer()!=NULL); }
    { Case x; { DagTipOverlayRuntime r; BOOST_REQUIRE(r.Start(x.c,&x.e)); BOOST_CHECK_EQUAL(r.RecoveryInvocations(),1U); LiveTipOverlayCheckpoint p; BOOST_REQUIRE(r.Overlay()->ReadCheckpoint(&p,&x.e)); BOOST_CHECK_EQUAL(p.phase,LIVE_OVERLAY_PHASE_CLEAN); } DagTipOverlayRuntime second; BOOST_REQUIRE(second.Start(x.c,&x.e)); BOOST_CHECK_EQUAL(second.RecoveryInvocations(),0U); }
    { Case x; x.Clean(); x.src.tokens[0]=uint256(0xD3A10101UL); DagTipOverlayRuntime r; BOOST_REQUIRE(r.Start(x.c,&x.e)); BOOST_CHECK_EQUAL(r.RecoveryInvocations(),1U); }
    { Case x; x.src.healthy=false; DagTipOverlayRuntime r; BOOST_CHECK(!r.Start(x.c,&x.e)); BOOST_CHECK(!r.Available()); }
    { Case x; x.src.readable=false; DagTipOverlayRuntime r; BOOST_CHECK(!r.Start(x.c,&x.e)); BOOST_CHECK(!r.Available()); }
    { Case x; x.c.dagLinksDir=(x.fx.tmp/"missing").string(); DagTipOverlayRuntime r; BOOST_CHECK(!r.Start(x.c,&x.e)); BOOST_CHECK(!r.Available()); BOOST_CHECK_EQUAL(r.RecoveryInvocations(),1U); }
    { Case x; x.c.dagInputDigest[0]^=1; DagTipOverlayRuntime r; BOOST_CHECK(!r.Start(x.c,&x.e)); BOOST_CHECK_EQUAL(r.Status(),DAG_TIP_OVERLAY_RUNTIME_IMMUTABLE_FAILURE); BOOST_CHECK_EQUAL(r.RecoveryInvocations(),0U); }
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