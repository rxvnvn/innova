#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MODULE DAG SourceState Bootstrap
#include <boost/test/unit_test.hpp>

#include "../txdb.h"
#include "../dag.h"
#include "../dag_tips_delta.h"
#include "../util.h"
#include "../wallet.h"
#include "../ui_interface.h"
#include "../checkpoints.h"
#include <boost/filesystem.hpp>
#include <leveldb/db.h>
#include <sys/wait.h>
#include <unistd.h>

CWallet* pwalletMain = NULL;
CClientUIInterface uiInterface;
bool fConfChange = false;
bool fEnforceCanonical = true;
bool fUseFastIndex = true;
unsigned int nDerivationMethodIndex = 0;
unsigned int nMinerSleep = 5000;
unsigned int nNodeLifespan = 7;
enum Checkpoints::CPMode CheckpointsMode = Checkpoints::STRICT;
extern bool fPrintToConsole;
extern void noui_connect();
extern bool g_testFailDAGSourceStateBootstrapMint;
extern bool g_testFailDAGSourceStateBootstrapTxnBegin;
extern bool g_testFailDAGSourceStateBootstrapTxnCommit;
extern bool g_testFailDAGScoreRevocation;
void Shutdown(void*) { exit(0); }
void StartShutdown() { exit(0); }

namespace {
namespace fs = boost::filesystem;

struct IsolatedTxDB
{
    fs::path root;
    static fs::path ProcessRoot()
    {
        static const fs::path p = fs::temp_directory_path() /
            fs::unique_path("innova-dagsource-%%%%-%%%%-%%%%");
        return p;
    }
    IsolatedTxDB()
    {
        root = ProcessRoot();
        fs::create_directories(root);
        // GetDataDir is process-cached: install the process-unique root before
        // constructing any CTxDB, then reset only its disposable source DB.
        mapArgs["-datadir"] = root.string();
        CTxDB old;
        old.Close();
        fs::remove_all(root / "txleveldb");
        fPrintToDebugger = true;
        noui_connect();
    }
    ~IsolatedTxDB()
    {
        CTxDB db;
        db.Close();
    }
    void Close() { CTxDB db; db.Close(); }
    void Reopen() { CTxDB db("c"); (void)db; }
};

static void PutCorruptToken()
{
    CTxDB db;
    CDataStream key(SER_DISK, CLIENT_VERSION);
    key << make_pair(std::string("dagsourcestate"), uint8_t(0));
    BOOST_REQUIRE(db.GetInstance()->Put(leveldb::WriteOptions(), key.str(), "x").ok());
}

static void PutRepresentativeDaglink()
{
    CTxDB db;
    CBlockDAGData d;
    d.vDAGParents.push_back(uint256(9));
    BOOST_REQUIRE(db.TxnBegin());
    BOOST_REQUIRE(db.WriteDAGLinks(uint256(10), d));
    BOOST_REQUIRE(db.TxnCommit());
}
}

BOOST_AUTO_TEST_CASE(source_semantic_frontier_envelope_and_abort)
{
    IsolatedTxDB fx; CTxDB db; std::string error;
    const uint256 p(1001), c(1002), t1(2001);
    BOOST_REQUIRE(db.BootstrapDAGSourceStateId(&error));
    BOOST_REQUIRE(db.EnsureDAGChildCountIndex(&error));
    BOOST_REQUIRE(db.TxnBegin());
    BOOST_REQUIRE(db.WriteDAGLinks(p, CBlockDAGData()));
    BOOST_REQUIRE(db.TxnCommit());
    uint256 t0; BOOST_REQUIRE(db.ReadDAGSourceStateId(t0));
    struct Capture {
        std::vector<DagTipCommittedDeltaEvent> events;
        static void Observe(const DagTipCommittedDeltaEvent& e, void* p) { static_cast<Capture*>(p)->events.push_back(e); }
        ~Capture() { SetDagTipCommittedDeltaObserver(NULL, NULL); }
    } capture;
    SetDagTipCommittedDeltaObserver(&Capture::Observe, &capture);
    BOOST_REQUIRE(BeginDagTipDeltaTransaction(DAG_TIP_DELTA_ADD_TO_BLOCK_INDEX));
    BOOST_REQUIRE(db.TxnBegin()); CBlockDAGData child; child.vDAGParents.push_back(p);
    BOOST_REQUIRE(db.WriteDAGLinks(c, child));
    BOOST_REQUIRE(db.WriteDAGSourceStateId(t1));
    BOOST_REQUIRE(db.TxnCommit()); SetDagTipDeltaFinalSourceStateId(t1);
    CommitDagTipDeltaTransaction();
    BOOST_REQUIRE_EQUAL(capture.events.size(), 4U);
    BOOST_CHECK(capture.events[0].hasInitialSourceStateId);
    BOOST_CHECK(capture.events[0].initialSourceStateId == t0);
    std::set<uint256> tips; tips.insert(p);
    for (const auto& e : capture.events) if (e.kind == DagTipCommittedDeltaEvent::RECORD) {
        if (e.record.op == DagTipDeltaRecord::TIP_ADD) tips.insert(e.record.hash); else tips.erase(e.record.hash);
    }
    BOOST_CHECK(tips == std::set<uint256>({c}));
    BOOST_CHECK(capture.events.back().finalSourceStateId == t1);
    capture.events.clear();
    BOOST_REQUIRE(BeginDagTipDeltaTransaction(DAG_TIP_DELTA_ADD_TO_BLOCK_INDEX));
    BOOST_REQUIRE(db.TxnBegin()); BOOST_REQUIRE(db.EraseDAGLinks(c));
    BOOST_REQUIRE(db.TxnAbort());
    CommitDagTipDeltaTransaction(); // even an erroneous caller cannot publish aborted edits
    BOOST_CHECK(capture.events.empty());
}

BOOST_AUTO_TEST_CASE(source_semantic_replacement_cardinality_and_retention_matrix)
{
    IsolatedTxDB fx; CTxDB db; std::string error;
    BOOST_REQUIRE(db.BootstrapDAGSourceStateId(&error)); BOOST_REQUIRE(db.EnsureDAGChildCountIndex(&error));
    struct Capture {
        std::vector<DagTipCommittedDeltaEvent> events;
        static void Observe(const DagTipCommittedDeltaEvent& e,void* p){static_cast<Capture*>(p)->events.push_back(e);}
        ~Capture(){SetDagTipCommittedDeltaObserver(NULL,NULL);SetDagTipDeltaRamCapacityForTest(256);}
    } capture;
    SetDagTipCommittedDeltaObserver(&Capture::Observe,&capture); SetDagTipDeltaRamCapacityForTest(1);
    std::set<uint256> replay; uint64_t sequence=8000;
    auto mutate=[&](uint256 hash,std::vector<uint256> parents,bool erase){
        capture.events.clear(); uint256 before; BOOST_REQUIRE(db.ReadDAGSourceStateId(before));
        BOOST_REQUIRE(BeginDagTipDeltaTransaction(DAG_TIP_DELTA_ADD_TO_BLOCK_INDEX));
        BOOST_REQUIRE(db.TxnBegin());
        if(erase) BOOST_REQUIRE(db.EraseDAGLinks(hash));
        else {CBlockDAGData data;data.vDAGParents=parents;BOOST_REQUIRE(db.WriteDAGLinks(hash,data));}
        const uint256 token(++sequence); BOOST_REQUIRE(db.WriteDAGSourceStateId(token));
        BOOST_REQUIRE(db.TxnCommit());SetDagTipDeltaFinalSourceStateId(token);CommitDagTipDeltaTransaction();
        BOOST_REQUIRE(capture.events.size()>=2U);
        BOOST_CHECK(capture.events.front().initialSourceStateId==before);
        for(const auto& e:capture.events) if(e.kind==DagTipCommittedDeltaEvent::RECORD){
            if(e.record.op==DagTipDeltaRecord::TIP_ADD) BOOST_CHECK(replay.insert(e.record.hash).second);
            else BOOST_CHECK_EQUAL(replay.erase(e.record.hash),1U);
        }
        std::map<uint256,CBlockDAGData> nodes;BOOST_REQUIRE(db.IterateDAGLinks(nodes));
        std::set<uint256> expected;for(const auto& n:nodes)expected.insert(n.first);
        for(const auto& n:nodes)for(const auto& p:n.second.vDAGParents)expected.erase(p);
        BOOST_CHECK(replay==expected);
        BOOST_CHECK(capture.events.back().finalSourceStateId==token);
        BOOST_CHECK_EQUAL(capture.events.back().expectedRecordCount,capture.events.size()-2);
    };
    const uint256 p(1),q(2),c(3),d(4),late(5);
    mutate(p,{},false); mutate(q,{},false);
    mutate(c,{p,p},false); // duplicate-normalized 0->1
    mutate(d,{p},false);   // 1->2 no parent transition
    mutate(c,{q,q},false); // P 2->1 none; Q 0->1 remove
    mutate(d,{},true);     // P 1->0 add
    mutate(q,{},true);     // removed nonfrontier Q is NOT added
    mutate(c,{},true);     // unretained Q count 1->0 does NOT add Q
    mutate(c,{late},false);mutate(late,{},false); // late vertex already has a child: no TIP_ADD
    mutate(c,{},true);mutate(late,{},true);
    mutate(p,{p},false);   // self edge cannot create a tip
    mutate(p,{},false);    // self edge removed: tip returns
    mutate(p,{},true);
}

BOOST_AUTO_TEST_CASE(child_count_rebuild_projects_unique_child_to_parent_edges)
{
    IsolatedTxDB fx;
    const uint256 parent(9), child(10);
    CTxDB db;
    CBlockDAGData p;
    CBlockDAGData c;
    c.vDAGParents.push_back(parent);
    c.vDAGParents.push_back(parent); // duplicate parent is one logical edge.
    BOOST_REQUIRE(db.TxnBegin());
    BOOST_REQUIRE(db.WriteDAGLinks(parent, p));
    BOOST_REQUIRE(db.WriteDAGLinks(child, c));
    uint256 token;
    BOOST_REQUIRE(db.MintDAGSourceStateId(token));
    BOOST_REQUIRE(db.WriteDAGSourceStateId(token));
    BOOST_REQUIRE(db.TxnCommit());

    std::string error;
    BOOST_REQUIRE_MESSAGE(db.EnsureDAGChildCountIndex(&error), error);
    uint64_t count = 0;
    bool present = false;
    BOOST_REQUIRE(db.ReadDAGChildCount(parent, &count, &present));
    BOOST_CHECK(present);
    BOOST_CHECK_EQUAL(count, 1u);
}

BOOST_AUTO_TEST_CASE(child_count_replacement_and_abort_are_batch_atomic)
{
    IsolatedTxDB fx;
    const uint256 p(1), q(2), r(3), s(4), child(10);
    CTxDB db;
    CBlockDAGData d; d.vDAGParents.push_back(p); d.vDAGParents.push_back(q);
    uint256 t0;
    BOOST_REQUIRE(db.MintDAGSourceStateId(t0));
    BOOST_REQUIRE(db.TxnBegin());
    BOOST_REQUIRE(db.WriteDAGLinks(child, d));
    BOOST_REQUIRE(db.WriteDAGSourceStateId(t0));
    BOOST_REQUIRE(db.TxnCommit());
    std::string error; BOOST_REQUIRE(db.EnsureDAGChildCountIndex(&error));
    BOOST_REQUIRE_MESSAGE(db.IsDAGChildCountIndexHealthy(&error), error);

    CBlockDAGData replacement; replacement.vDAGParents.push_back(p); replacement.vDAGParents.push_back(r); replacement.vDAGParents.push_back(r);
    uint256 t1; BOOST_REQUIRE(db.MintDAGSourceStateId(t1));
    BOOST_REQUIRE(db.TxnBegin());
    BOOST_REQUIRE(db.WriteDAGLinks(child, replacement)); // stages Q--, R++.
    BOOST_REQUIRE(db.WriteDAGSourceStateId(t1));
    BOOST_REQUIRE(db.TxnCommit());
    uint64_t n=0; bool present=false;
    BOOST_REQUIRE(db.ReadDAGChildCount(p,&n,&present)); BOOST_CHECK(present); BOOST_CHECK_EQUAL(n,1u);
    BOOST_REQUIRE(db.ReadDAGChildCount(q,&n,&present)); BOOST_CHECK(!present); BOOST_CHECK_EQUAL(n,0u);
    BOOST_REQUIRE(db.ReadDAGChildCount(r,&n,&present)); BOOST_CHECK(present); BOOST_CHECK_EQUAL(n,1u);
    BOOST_REQUIRE_MESSAGE(db.IsDAGChildCountIndexHealthy(&error), error);

    CBlockDAGData aborted; aborted.vDAGParents.push_back(s);
    uint256 t2; BOOST_REQUIRE(db.MintDAGSourceStateId(t2));
    BOOST_REQUIRE(db.TxnBegin());
    BOOST_REQUIRE(db.WriteDAGLinks(child, aborted)); // stages P--, R--, S++.
    BOOST_REQUIRE(db.WriteDAGSourceStateId(t2));
    BOOST_REQUIRE(db.TxnAbort());
    CBlockDAGData got; BOOST_REQUIRE(db.ReadDAGLinks(child, got));
    BOOST_CHECK(got.vDAGParents == replacement.vDAGParents);
    uint256 after; BOOST_REQUIRE(db.ReadDAGSourceStateId(after)); BOOST_CHECK(after == t1);
    BOOST_REQUIRE(db.ReadDAGChildCount(p,&n,&present)); BOOST_CHECK(present); BOOST_CHECK_EQUAL(n,1u);
    BOOST_REQUIRE(db.ReadDAGChildCount(r,&n,&present)); BOOST_CHECK(present); BOOST_CHECK_EQUAL(n,1u);
    BOOST_REQUIRE(db.ReadDAGChildCount(s,&n,&present)); BOOST_CHECK(!present); BOOST_CHECK_EQUAL(n,0u);
    BOOST_REQUIRE_MESSAGE(db.IsDAGChildCountIndexHealthy(&error), error);
}

BOOST_AUTO_TEST_CASE(child_count_stale_projection_must_not_be_recertified_by_mutation)
{
    IsolatedTxDB fx;
    CTxDB db;
    const uint256 p(91), c(92), q(93), next(94);
    CBlockDAGData parent, child;
    child.vDAGParents.push_back(p);
    BOOST_REQUIRE(db.TxnBegin());
    BOOST_REQUIRE(db.WriteDAGLinks(p, parent));
    BOOST_REQUIRE(db.WriteDAGLinks(c, child));
    BOOST_REQUIRE(db.WriteDAGSourceStateId(uint256(101)));
    BOOST_REQUIRE(db.TxnCommit());
    std::string error;
    BOOST_REQUIRE(db.EnsureDAGChildCountIndex(&error));
    BOOST_REQUIRE(db.IsDAGChildCountIndexHealthy(&error));

    // Emulate persisted stale projection: forward edge remains, its reverse
    // count is missing, and the marker truthfully identifies an older token.
    CDataStream countKey(SER_DISK, CLIENT_VERSION), markerKey(SER_DISK, CLIENT_VERSION), stale(SER_DISK, CLIENT_VERSION);
    countKey << make_pair(std::string("dagchildcount"), p);
    markerKey << make_pair(std::string("dagchildcountstate"), uint8_t(0));
    stale << make_pair(uint32_t(1), uint256(100));
    BOOST_REQUIRE(db.GetInstance()->Delete(leveldb::WriteOptions(), countKey.str()).ok());
    BOOST_REQUIRE(db.GetInstance()->Put(leveldb::WriteOptions(), markerKey.str(), stale.str()).ok());
    fx.Close(); fx.Reopen();
    CTxDB reopened;
    BOOST_REQUIRE(!reopened.IsDAGChildCountIndexHealthy(&error));
    CBlockDAGData retained;
    BOOST_REQUIRE(reopened.ReadDAGLinks(c, retained));
    BOOST_REQUIRE(retained.vDAGParents == child.vDAGParents);

    // A normal unrelated mutation must not certify this stale projection.
    CBlockDAGData unrelated; unrelated.vDAGParents.push_back(q);
    BOOST_REQUIRE(reopened.TxnBegin());
    const bool linksOK = reopened.WriteDAGLinks(next, unrelated);
    const bool tokenOK = linksOK && reopened.WriteDAGSourceStateId(uint256(102));
    if (tokenOK) BOOST_REQUIRE(reopened.TxnCommit());
    else BOOST_REQUIRE(reopened.TxnAbort());
    uint64_t count = 999; bool present = true;
    BOOST_REQUIRE(reopened.ReadDAGChildCount(p, &count, &present));
    BOOST_REQUIRE(reopened.ReadDAGLinks(c, retained));
    const bool healthy = reopened.IsDAGChildCountIndexHealthy(&error);
    BOOST_TEST_MESSAGE("stale projection: linksOK=" << linksOK << " tokenOK=" << tokenOK
        << " healthy=" << healthy << " retained_C_to_P=" << (retained.vDAGParents == child.vDAGParents)
        << " count_P=" << count << " present=" << present);
    BOOST_CHECK_MESSAGE(!healthy, "ordinary mutation recertified stale counts without rebuild");
}

BOOST_AUTO_TEST_CASE(child_count_complete_replacement_matrix)
{
    const std::vector<std::vector<uint256> > before = {{uint256(1),uint256(2)}, {uint256(1),uint256(2)}, {uint256(1)}, {uint256(1),uint256(2)}};
    const std::vector<std::vector<uint256> > after = {{uint256(1),uint256(2)}, {uint256(2)}, {uint256(1),uint256(2)}, {uint256(3),uint256(4)}};
    for (size_t i=0; i<before.size(); ++i) {
        IsolatedTxDB fx; CTxDB db; CBlockDAGData d; d.vDAGParents=before[i];
        BOOST_REQUIRE(db.TxnBegin()); BOOST_REQUIRE(db.WriteDAGLinks(uint256(10),d));
        BOOST_REQUIRE(db.WriteDAGSourceStateId(uint256(101))); BOOST_REQUIRE(db.TxnCommit());
        std::string error; BOOST_REQUIRE(db.EnsureDAGChildCountIndex(&error));
        d.vDAGParents=after[i];
        BOOST_REQUIRE(db.TxnBegin()); BOOST_REQUIRE(db.WriteDAGLinks(uint256(10),d));
        BOOST_REQUIRE(db.WriteDAGSourceStateId(uint256(102))); BOOST_REQUIRE(db.TxnCommit());
        fx.Close(); fx.Reopen(); CTxDB reopened;
        CBlockDAGData got; BOOST_REQUIRE(reopened.ReadDAGLinks(uint256(10),got));
        BOOST_CHECK(got.vDAGParents==after[i]);
        for (unsigned j=1; j<=4; ++j) {
            uint64_t count=999; bool present=false;
            const bool expected=std::find(after[i].begin(),after[i].end(),uint256(j))!=after[i].end();
            BOOST_REQUIRE(reopened.ReadDAGChildCount(uint256(j),&count,&present));
            BOOST_CHECK_EQUAL(count,expected?1U:0U); BOOST_CHECK_EQUAL(present,expected);
        }
        BOOST_REQUIRE(reopened.IsDAGChildCountIndexHealthy(&error));
    }
}

BOOST_AUTO_TEST_CASE(child_count_marker_corruption_matrix)
{
    for (unsigned mode=0; mode<5; ++mode) {
        IsolatedTxDB fx; CTxDB db; std::string error;
        BOOST_REQUIRE(db.BootstrapDAGSourceStateId(&error));
        BOOST_REQUIRE(db.EnsureDAGChildCountIndex(&error));
        CDataStream key(SER_DISK,CLIENT_VERSION), value(SER_DISK,CLIENT_VERSION);
        key << make_pair(std::string("dagchildcountstate"),uint8_t(0));
        if (mode==0) BOOST_REQUIRE(db.GetInstance()->Delete(leveldb::WriteOptions(),key.str()).ok());
        else {
            uint256 source; BOOST_REQUIRE(db.ReadDAGSourceStateId(source));
            if(mode==1) value << make_pair(uint32_t(2),source);
            if(mode==2) value << make_pair(uint32_t(1),uint256(0));
            if(mode==3) value.write("x",1);
            // mode 4 is empty/truncated marker.
            BOOST_REQUIRE(db.GetInstance()->Put(leveldb::WriteOptions(),key.str(),value.str()).ok());
        }
        fx.Close(); fx.Reopen(); CTxDB reopened;
        BOOST_CHECK(!reopened.IsDAGChildCountIndexHealthy(&error)); BOOST_CHECK(!error.empty());
        BOOST_REQUIRE(reopened.TxnBegin());
        const bool ok=reopened.WriteDAGSourceStateId(uint256(111));
        if(mode!=0) BOOST_CHECK(!ok); // missing marker preserves legacy bootstrap semantics
        BOOST_REQUIRE(reopened.TxnAbort());
        BOOST_CHECK(!reopened.IsDAGChildCountIndexHealthy(&error));
    }
}

BOOST_AUTO_TEST_CASE(child_count_raw_corruption_rejects_mutation)
{
    for(unsigned mode=0;mode<6;++mode) {
        IsolatedTxDB fx; CTxDB db; CBlockDAGData d; d.vDAGParents.push_back(uint256(1));
        BOOST_REQUIRE(db.TxnBegin()); BOOST_REQUIRE(db.WriteDAGLinks(uint256(10),d));
        BOOST_REQUIRE(db.WriteDAGSourceStateId(uint256(101))); BOOST_REQUIRE(db.TxnCommit());
        std::string error; BOOST_REQUIRE(db.EnsureDAGChildCountIndex(&error));
        CDataStream key(SER_DISK,CLIENT_VERSION), value(SER_DISK,CLIENT_VERSION);
        key << make_pair(std::string("dagchildcount"),uint256(1));
        if(mode==0) value.write("x",1);
        if(mode==1) value << uint64_t(0);
        if(mode==2) value << uint64_t(UINT64_MAX);
        if(mode==4) { value << uint64_t(1); value.write("x",1); }
        // mode 5: empty/truncated count.
        if(mode==3) BOOST_REQUIRE(db.GetInstance()->Delete(leveldb::WriteOptions(),key.str()).ok());
        else BOOST_REQUIRE(db.GetInstance()->Put(leveldb::WriteOptions(),key.str(),value.str()).ok());
        BOOST_REQUIRE(db.TxnBegin());
        if(mode==3) BOOST_CHECK(!db.EraseDAGLinks(uint256(10)));
        else BOOST_CHECK(!db.WriteDAGLinks(uint256(11),d));
        BOOST_REQUIRE(db.TxnAbort());
        CBlockDAGData retained; BOOST_REQUIRE(db.ReadDAGLinks(uint256(10),retained));
        BOOST_CHECK(!db.ReadDAGLinks(uint256(11),retained));
    }
}

BOOST_AUTO_TEST_CASE(child_count_detected_corruption_must_disable_health)
{
    IsolatedTxDB fx; CTxDB db; CBlockDAGData child;
    child.vDAGParents.push_back(uint256(1));
    BOOST_REQUIRE(db.TxnBegin());
    BOOST_REQUIRE(db.WriteDAGLinks(uint256(10), child));
    BOOST_REQUIRE(db.WriteDAGSourceStateId(uint256(101)));
    BOOST_REQUIRE(db.TxnCommit());
    std::string error; BOOST_REQUIRE(db.EnsureDAGChildCountIndex(&error));
    CDataStream key(SER_DISK, CLIENT_VERSION);
    key << make_pair(std::string("dagchildcount"), uint256(1));
    BOOST_REQUIRE(db.GetInstance()->Put(leveldb::WriteOptions(), key.str(), "x").ok());
    uint64_t count=0; bool present=false;
    BOOST_REQUIRE(!db.ReadDAGChildCount(uint256(1), &count, &present));
    const bool healthyAfterDetection=db.IsDAGChildCountIndexHealthy(&error);
    const bool ensureOK=db.EnsureDAGChildCountIndex(&error);
    const bool countReadableAfterEnsure=db.ReadDAGChildCount(uint256(1), &count, &present);
    BOOST_TEST_MESSAGE("detected corruption: health=" << healthyAfterDetection
        << " ensure=" << ensureOK << " count_readable_after_ensure=" << countReadableAfterEnsure);
    BOOST_CHECK_MESSAGE(!healthyAfterDetection, "known corrupt count remains certified healthy");
    BOOST_CHECK_MESSAGE(!ensureOK || countReadableAfterEnsure, "Ensure reports success without repairing known corruption");
}

BOOST_AUTO_TEST_CASE(child_count_revocation_abort_reopen_rebuild_parity)
{
    IsolatedTxDB fx; CTxDB db;
    // P1=zero children; P2=one; P3=two; child 10 has two
    // unique parents and a duplicate input reference.
    BOOST_REQUIRE(db.TxnBegin());
    for(unsigned i=1;i<=3;++i) { CBlockDAGData d; BOOST_REQUIRE(db.WriteDAGLinks(uint256(i),d)); }
    CBlockDAGData c; c.vDAGParents={uint256(2),uint256(3),uint256(3)};
    BOOST_REQUIRE(db.WriteDAGLinks(uint256(10),c));
    c.vDAGParents={uint256(3)}; BOOST_REQUIRE(db.WriteDAGLinks(uint256(11),c));
    BOOST_REQUIRE(db.WriteDAGSourceStateId(uint256(123))); BOOST_REQUIRE(db.TxnCommit());
    std::string error; BOOST_REQUIRE(db.EnsureDAGChildCountIndex(&error));
    CDataStream key(SER_DISK,CLIENT_VERSION), junkKey(SER_DISK,CLIENT_VERSION);
    key << make_pair(std::string("dagchildcount"),uint256(3));
    junkKey << make_pair(std::string("dagchildcount"),uint256(999));
    BOOST_REQUIRE(db.GetInstance()->Put(leveldb::WriteOptions(),key.str(),"x").ok());
    BOOST_REQUIRE(db.GetInstance()->Put(leveldb::WriteOptions(),junkKey.str(),"stale").ok());
    BOOST_REQUIRE(db.TxnBegin());
    uint64_t count=0; bool present=false;
    BOOST_REQUIRE(!db.ReadDAGChildCount(uint256(3),&count,&present));
    BOOST_REQUIRE(!db.IsDAGChildCountIndexHealthy(&error));
    BOOST_REQUIRE(!db.WriteDAGLinks(uint256(12),c));
    BOOST_REQUIRE(!db.WriteDAGSourceStateId(uint256(124)));
    BOOST_REQUIRE(!db.EnsureDAGChildCountIndex(&error)); // no rebuilding in transaction
    BOOST_REQUIRE(db.TxnAbort());
    BOOST_REQUIRE(!db.IsDAGChildCountIndexHealthy(&error));
    fx.Close(); fx.Reopen(); CTxDB reopened;
    BOOST_REQUIRE(!reopened.IsDAGChildCountIndexHealthy(&error));
    extern bool g_testFailDAGChildCountRebuild;
    g_testFailDAGChildCountRebuild=true;
    const bool rebuilt=reopened.EnsureDAGChildCountIndex(&error);
    g_testFailDAGChildCountRebuild=false;
    BOOST_REQUIRE(!rebuilt);
    fx.Close(); fx.Reopen(); CTxDB repaired;
    BOOST_REQUIRE(!repaired.IsDAGChildCountIndexHealthy(&error));
    BOOST_REQUIRE(repaired.EnsureDAGChildCountIndex(&error));
    fx.Close(); fx.Reopen(); CTxDB finalDb;
    BOOST_REQUIRE(finalDb.IsDAGChildCountIndexHealthy(&error));
    for(unsigned i=1;i<=3;++i) {
        BOOST_REQUIRE(finalDb.ReadDAGChildCount(uint256(i),&count,&present));
        BOOST_CHECK_EQUAL(count,i-1); BOOST_CHECK_EQUAL(present,i!=1);
    }
    BOOST_REQUIRE(finalDb.ReadDAGChildCount(uint256(999),&count,&present));
    BOOST_CHECK(!present); BOOST_CHECK_EQUAL(count,0U);
    uint256 source; BOOST_REQUIRE(finalDb.ReadDAGSourceStateId(source)); BOOST_CHECK(source==uint256(123));
}

BOOST_AUTO_TEST_CASE(child_count_revocation_persistence_failure_is_process_fail_closed)
{
    IsolatedTxDB fx; CTxDB db; std::string error;
    BOOST_REQUIRE(db.BootstrapDAGSourceStateId(&error)); BOOST_REQUIRE(db.EnsureDAGChildCountIndex(&error));
    CDataStream key(SER_DISK,CLIENT_VERSION); key << make_pair(std::string("dagchildcount"),uint256(1));
    BOOST_REQUIRE(db.GetInstance()->Put(leveldb::WriteOptions(),key.str(),"x").ok());
    extern bool g_testFailDAGChildCountRevocation;
    g_testFailDAGChildCountRevocation=true;
    uint64_t count=0; bool present=false;
    const bool readOK=db.ReadDAGChildCount(uint256(1),&count,&present);
    const bool health=db.IsDAGChildCountIndexHealthy(&error);
    const bool repair=db.EnsureDAGChildCountIndex(&error);
    g_testFailDAGChildCountRevocation=false;
    BOOST_CHECK(!readOK); BOOST_CHECK(!health); BOOST_CHECK(!repair);
    fx.Close(); fx.Reopen(); CTxDB repaired;
    BOOST_REQUIRE(!repaired.IsDAGChildCountIndexHealthy(&error));
    // Storage repaired: only a full successful rebuild releases fallback latch.
    BOOST_REQUIRE(repaired.EnsureDAGChildCountIndex(&error));
    BOOST_REQUIRE(repaired.IsDAGChildCountIndexHealthy(&error));
}

BOOST_AUTO_TEST_CASE(child_count_revocation_survives_detector_process_exit)
{
    IsolatedTxDB fx; CTxDB db; std::string error;
    BOOST_REQUIRE(db.BootstrapDAGSourceStateId(&error));
    BOOST_REQUIRE(db.EnsureDAGChildCountIndex(&error));
    CDataStream key(SER_DISK,CLIENT_VERSION);
    key << make_pair(std::string("dagchildcount"),uint256(1));
    BOOST_REQUIRE(db.GetInstance()->Put(leveldb::WriteOptions(),key.str(),"x").ok());
    fx.Close(); // no live LevelDB handle/threads are inherited by detector
    const pid_t pid=fork(); BOOST_REQUIRE(pid>=0);
    if(pid==0) {
        CTxDB detector;
        uint64_t count=0; bool present=false;
        const bool rejected=!detector.ReadDAGChildCount(uint256(1),&count,&present);
        // Simulate detector process exit without orderly database shutdown.
        _exit(rejected?0:1);
    }
    int status=0; BOOST_REQUIRE(waitpid(pid,&status,0)==pid);
    BOOST_REQUIRE(WIFEXITED(status)); BOOST_REQUIRE_EQUAL(WEXITSTATUS(status),0);
    CTxDB verifier;
    BOOST_REQUIRE(!verifier.IsDAGChildCountIndexHealthy(&error));
    BOOST_REQUIRE(verifier.EnsureDAGChildCountIndex(&error));
    BOOST_REQUIRE(verifier.IsDAGChildCountIndexHealthy(&error));
}

BOOST_AUTO_TEST_CASE(tokenless_legacy_mints_and_close_reopen_preserves)
{
    IsolatedTxDB fx;
    PutRepresentativeDaglink();
    CTxDB db;
    BOOST_CHECK(!db.HasDAGSourceStateId());
    uint256 absent;
    BOOST_CHECK(!db.ReadDAGSourceStateId(absent));
    std::string error;
    BOOST_REQUIRE(db.BootstrapDAGSourceStateId(&error));
    uint256 first;
    BOOST_REQUIRE(db.ReadDAGSourceStateId(first));
    BOOST_CHECK(first != uint256(0));
    std::map<uint256, CBlockDAGData> links;
    BOOST_REQUIRE(db.IterateDAGLinks(links));
    BOOST_REQUIRE(links.count(uint256(10)) == 1);
    BOOST_CHECK(links[uint256(10)].vDAGParents == std::vector<uint256>(1, uint256(9)));
    BOOST_CHECK(!GetDagTipDeltaState().active);
    fx.Close();
    fx.Reopen();
    CTxDB reopened;
    std::string again;
    BOOST_REQUIRE(reopened.BootstrapDAGSourceStateId(&again));
    uint256 second;
    BOOST_REQUIRE(reopened.ReadDAGSourceStateId(second));
    BOOST_CHECK(second == first);
}

BOOST_AUTO_TEST_CASE(empty_source_gets_persistent_token)
{
    IsolatedTxDB fx;
    CTxDB db;
    std::string error;
    BOOST_REQUIRE(db.BootstrapDAGSourceStateId(&error));
    uint256 first;
    BOOST_REQUIRE(db.ReadDAGSourceStateId(first));
    BOOST_CHECK(first != uint256(0));
    fx.Close(); fx.Reopen();
    CTxDB reopened; uint256 second;
    BOOST_REQUIRE(reopened.BootstrapDAGSourceStateId(&error));
    BOOST_REQUIRE(reopened.ReadDAGSourceStateId(second));
    BOOST_CHECK(second == first);
}

BOOST_AUTO_TEST_CASE(corrupt_token_fails_closed_and_is_not_replaced)
{
    IsolatedTxDB fx;
    PutRepresentativeDaglink();
    PutCorruptToken();
    CTxDB db;
    uint256 ignored;
    BOOST_CHECK(db.HasDAGSourceStateId());
    BOOST_CHECK(!db.ReadDAGSourceStateId(ignored));
    std::string error;
    BOOST_CHECK(!db.BootstrapDAGSourceStateId(&error));
    BOOST_CHECK(!error.empty());
    BOOST_CHECK(db.HasDAGSourceStateId());
    BOOST_CHECK(!db.ReadDAGSourceStateId(ignored));
    fx.Close(); fx.Reopen();
    CTxDB reopened;
    BOOST_CHECK(!reopened.ReadDAGSourceStateId(ignored));
}

BOOST_AUTO_TEST_CASE(legacy_loadblockindex_propagates_bootstrap_failure)
{
    IsolatedTxDB fx;
    g_testFailDAGSourceStateBootstrapMint = true;
    CTxDB db;
    BOOST_CHECK(!db.LoadBlockIndex());
    BOOST_CHECK(!db.HasDAGSourceStateId());
    g_testFailDAGSourceStateBootstrapMint = false;
    BOOST_REQUIRE(db.LoadBlockIndex());
    uint256 token;
    BOOST_REQUIRE(db.ReadDAGSourceStateId(token));
    BOOST_CHECK(token != uint256(0));
}

BOOST_AUTO_TEST_CASE(bootstrap_failure_seams_fail_closed_and_retry)
{
    bool* seams[] = { &g_testFailDAGSourceStateBootstrapMint,
                      &g_testFailDAGSourceStateBootstrapTxnBegin,
                      &g_testFailDAGSourceStateBootstrapTxnCommit };
    for (size_t i = 0; i < sizeof(seams)/sizeof(seams[0]); ++i)
    {
        IsolatedTxDB fx;
        PutRepresentativeDaglink();
        *seams[i] = true;
        CTxDB db;
        std::string error;
        BOOST_CHECK(!db.BootstrapDAGSourceStateId(&error));
        BOOST_CHECK(!error.empty());
        BOOST_CHECK(!db.HasDAGSourceStateId());
        BOOST_CHECK(!GetDagTipDeltaState().active);
        *seams[i] = false;
        BOOST_REQUIRE(db.BootstrapDAGSourceStateId(&error));
        uint256 token;
        BOOST_REQUIRE(db.ReadDAGSourceStateId(token));
        BOOST_CHECK(token != uint256(0));
        fx.Close(); fx.Reopen();
        CTxDB reopened; uint256 same;
        BOOST_REQUIRE(reopened.ReadDAGSourceStateId(same));
        BOOST_CHECK(same == token);
    }
    g_testFailDAGSourceStateBootstrapMint = false;
    g_testFailDAGSourceStateBootstrapTxnBegin = false;
    g_testFailDAGSourceStateBootstrapTxnCommit = false;
}

// Edge: trailing junk bytes on the state marker must NOT mask a token mismatch.
// The marker payload is a (version, SourceStateId) pair; a trailing byte is
// ignored by the pair decoder, but it cannot turn a WRONG token into a matching
// one. An intact matching pair stays healthy (the count projection integrity is
// governed by the strict count reader, covered separately by modes 4/5); a
// non-matching token with identical trailing junk must remain unhealthy.
BOOST_AUTO_TEST_CASE(child_count_marker_trailing_bytes_do_not_mask_token_mismatch)
{
    IsolatedTxDB fx; CTxDB db; std::string error;
    BOOST_REQUIRE(db.BootstrapDAGSourceStateId(&error));
    BOOST_REQUIRE(db.EnsureDAGChildCountIndex(&error));
    uint256 source; BOOST_REQUIRE(db.ReadDAGSourceStateId(source));

    CDataStream key(SER_DISK, CLIENT_VERSION);
    key << make_pair(std::string("dagchildcountstate"), uint8_t(0));

    // Intact matching pair followed by one trailing junk byte.
    CDataStream trailing(SER_DISK, CLIENT_VERSION);
    trailing << make_pair(uint32_t(1), source);
    trailing.write("j", 1);
    BOOST_REQUIRE(db.GetInstance()->Put(leveldb::WriteOptions(), key.str(), trailing.str()).ok());
    fx.Close(); fx.Reopen(); CTxDB reopened;
    BOOST_REQUIRE(reopened.IsDAGChildCountIndexHealthy(&error));

    // A WRONG token with identical trailing junk must NOT become healthy.
    fx.Close(); fx.Reopen(); CTxDB wrong;
    CDataStream wrongVal(SER_DISK, CLIENT_VERSION);
    wrongVal << make_pair(uint32_t(1), uint256(0));
    wrongVal.write("j", 1);
    BOOST_REQUIRE(wrong.GetInstance()->Put(leveldb::WriteOptions(), key.str(), wrongVal.str()).ok());
    fx.Close(); fx.Reopen(); CTxDB wrongRe;
    BOOST_CHECK(!wrongRe.IsDAGChildCountIndexHealthy(&error));
    BOOST_CHECK(!error.empty());
}

// R2c.2s score-authority certificate lifecycle (disk layer). The recolor engine
// itself (full GHOSTDAG/DAGKNIGHT coloring over retained daglinks) is e2e-proven;
// here we pin the durable certificate: publish, health, revocation, marker
// corruption, stale-token refusal, abort/reopen persistence and fail-closed
// revocation-write failure.
BOOST_AUTO_TEST_CASE(score_authority_certificate_lifecycle)
{
    IsolatedTxDB fx; CTxDB db; std::string error;
    BOOST_REQUIRE(db.BootstrapDAGSourceStateId(&error));
    BOOST_REQUIRE(db.EnsureDAGChildCountIndex(&error));

    // No certificate published yet -> unhealthy (marker missing).
    BOOST_CHECK(!db.IsDAGScoreAuthorityHealthy(&error));
    BOOST_CHECK(!error.empty());

    // Publish binds to current source.
    BOOST_REQUIRE(db.PublishDAGScoreCertificateAtomic(&error));
    fx.Close(); fx.Reopen(); CTxDB reopened;
    BOOST_REQUIRE(reopened.IsDAGScoreAuthorityHealthy(&error));

    // Revocation poison overrides a valid matching marker and survives reopen.
    BOOST_REQUIRE(reopened.RevokeDAGScoreAuthorityForTest());
    fx.Close(); fx.Reopen(); CTxDB revoked;
    BOOST_CHECK(!revoked.IsDAGScoreAuthorityHealthy(&error));
    BOOST_CHECK(!error.empty());
}

BOOST_AUTO_TEST_CASE(score_authority_marker_corruption_matrix)
{
    for (unsigned mode=0; mode<5; ++mode) {
        IsolatedTxDB fx; CTxDB db; std::string error;
        BOOST_REQUIRE(db.BootstrapDAGSourceStateId(&error));
        BOOST_REQUIRE(db.EnsureDAGChildCountIndex(&error));
        uint256 source; BOOST_REQUIRE(db.ReadDAGSourceStateId(source));
        BOOST_REQUIRE(db.PublishDAGScoreCertificateAtomic(&error));

        CDataStream key(SER_DISK, CLIENT_VERSION);
        key << make_pair(std::string("dagscorestate"), uint8_t(0));
        CDataStream value(SER_DISK, CLIENT_VERSION);
        if (mode==0) BOOST_REQUIRE(db.GetInstance()->Delete(leveldb::WriteOptions(), key.str()).ok());
        else {
            if(mode==1) value << make_pair(uint32_t(2), source);
            if(mode==2) value << make_pair(uint32_t(1), uint256(0));
            if(mode==3) value.write("x",1);
            // mode 4 is empty/truncated marker.
            BOOST_REQUIRE(db.GetInstance()->Put(leveldb::WriteOptions(), key.str(), value.str()).ok());
        }
        fx.Close(); fx.Reopen(); CTxDB reopened;
        BOOST_CHECK(!reopened.IsDAGScoreAuthorityHealthy(&error));
        BOOST_CHECK(!error.empty());
        // A corrupt/missing marker must not be silently healed by a mutation.
        BOOST_REQUIRE(reopened.TxnBegin());
        BOOST_REQUIRE(reopened.WriteDAGSourceStateId(source));
        BOOST_REQUIRE(reopened.TxnCommit());
        BOOST_CHECK(!reopened.IsDAGScoreAuthorityHealthy(&error));
    }
}

BOOST_AUTO_TEST_CASE(score_authority_stale_token_not_recertified)
{
    // A certificate bound to an OLD token must remain unhealthy after the source
    // advances; ordinary mutation must not accidentally re-certify stale scores.
    IsolatedTxDB fx; CTxDB db; std::string error;
    BOOST_REQUIRE(db.BootstrapDAGSourceStateId(&error));
    BOOST_REQUIRE(db.EnsureDAGChildCountIndex(&error));
    uint256 source; BOOST_REQUIRE(db.ReadDAGSourceStateId(source));
    BOOST_REQUIRE(db.PublishDAGScoreCertificateAtomic(&error));

    // Advance the source token (mutation) without re-publishing score certificate.
    BOOST_REQUIRE(db.TxnBegin());
    BOOST_REQUIRE(db.WriteDAGSourceStateId(uint256(999)));
    BOOST_REQUIRE(db.TxnCommit());
    BOOST_CHECK(!db.IsDAGScoreAuthorityHealthy(&error)); // token mismatch
    // Re-publish bound to the NEW token is the only way to become healthy.
    BOOST_REQUIRE(db.PublishDAGScoreCertificateAtomic(&error));
    BOOST_CHECK(db.IsDAGScoreAuthorityHealthy(&error));
}

BOOST_AUTO_TEST_CASE(score_authority_revocation_persistence_failure_fail_closed)
{
    IsolatedTxDB fx; CTxDB db; std::string error;
    BOOST_REQUIRE(db.BootstrapDAGSourceStateId(&error));
    BOOST_REQUIRE(db.EnsureDAGChildCountIndex(&error));
    BOOST_REQUIRE(db.PublishDAGScoreCertificateAtomic(&error));
    g_testFailDAGScoreRevocation = true;
    const bool revokedOk = db.RevokeDAGScoreAuthorityForTest();
    g_testFailDAGScoreRevocation = false;
    BOOST_CHECK(!revokedOk);
    BOOST_CHECK(!db.IsDAGScoreAuthorityHealthy(&error)); // process-local fail-closed
    BOOST_CHECK(!error.empty());
}

BOOST_AUTO_TEST_CASE(score_authority_abort_reopen_publish)
{
    // Publish must be durable/sync and not tied to an active transaction abort.
    IsolatedTxDB fx; CTxDB db; std::string error;
    BOOST_REQUIRE(db.BootstrapDAGSourceStateId(&error));
    BOOST_REQUIRE(db.EnsureDAGChildCountIndex(&error));
    BOOST_REQUIRE(db.PublishDAGScoreCertificateAtomic(&error));
    // An unrelated abort must not remove the already-durable certificate.
    BOOST_REQUIRE(db.TxnBegin()); BOOST_REQUIRE(db.TxnAbort());
    fx.Close(); fx.Reopen(); CTxDB reopened;
    BOOST_CHECK(reopened.IsDAGScoreAuthorityHealthy(&error));
    // Revoke restores unhealthy and survives close/reopen.
    BOOST_REQUIRE(reopened.RevokeDAGScoreAuthorityForTest());
    fx.Close(); fx.Reopen(); CTxDB reopened2;
    BOOST_CHECK(!reopened2.IsDAGScoreAuthorityHealthy(&error));
    // Re-publish after revoke heals again (full lifecycle cycle).
    BOOST_REQUIRE(reopened2.PublishDAGScoreCertificateAtomic(&error));
    fx.Close(); fx.Reopen(); CTxDB reopened3;
    BOOST_CHECK(reopened3.IsDAGScoreAuthorityHealthy(&error));
}
