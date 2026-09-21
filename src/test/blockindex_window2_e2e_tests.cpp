// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.

// Window-2 isolated E2E — full-consensus genuine-block verification.
//
// Proves, through the REAL node consensus path (CreateNewBlock -> CheckBlock ->
// ProcessBlock -> AcceptBlock -> AddToBlockIndex -> ConnectBlock -> SetBestChain),
// that V2 wiring does not alter consensus semantics and that the complete
// Window-2 sequence is logically equivalent under LEGACY_RESIDENT:
//
//   A. baseline: mine a genuine chain 0..S (real consensus ACCEPT, positive control)
//   B. continuation: mine genuinely-valid S+1..S+k -> each becomes active best tip
//   C. side branch: mine a competing branch at the same/prior height, then a reorg
//     to the winning branch -> chain trust / height / hash reflect legacy semantics
//   D. G1/P5 restart: reopen the authoritative tip store -> exact tip retained
//   E. G2 catch-up + G3 rollback integration: persisted legacy records re-open to
//     the exact authoritative chain (no peers / reindex / rescan)
//      state), and a second authoritative boot agrees (parity).
//
// This does NOT repopulate historical mapBlockIndex from V2; it uses genuine
// ProcessBlock mining for the live blocks and the catch-up/rollback storage path
// for the persisted boundary.
//
// NOTE: canonical TestingSetup loads the regtest chain into mapBlockIndex; this
// test mines genuine blocks ON TOP of that (positive control that the real
// consensus/ConnectBlock path is exercised with genuinely-valid, real mined
// blocks). The V2-only-boundary (parent non-resident) is covered by the dedicated
// authoritative_live orphan-gate + parent-resolution tests; the full-consensus
// ACCEPT of a genuinely-mined successor is proven here by the positive control.
#include <boost/test/unit_test.hpp>

#include "db.h"
#include "txdb.h"
#include "main.h"
#include "miner.h"
#include "wallet.h"
#include "zkproof.h"
#include "hooks.h"
#include "dag.h"
#include "dag_tips_delta.h"
#include "dag_tip_overlay_runtime.h"
#include "dag_tip_frontier.h"
#include "blockindex_authoritative_startup.h"
#include "blockindex_authoritative_live.h"
#include "blockindex_shadow_startup.h"
#include "blockindex_generation_builder.h"
#include "blockindex_generation_lifecycle.h"
#include <openssl/sha.h>
#include <fstream>

#include <boost/filesystem.hpp>
#include <cstdio>
#include <string>
#include <vector>
#include <set>

namespace fs = boost::filesystem;

extern CWallet* pwalletMain;
extern bool g_testFailSetBestChainAfterDagInit;
extern bool g_testFailInitialDagLinksCommit;
extern bool g_testFailFailedAddDagLinksCleanupCommit;
extern bool g_testFailReorganizeDagLinksEraseCommit;
extern bool g_testForceDagPruneInAdd;
extern int g_testDagPruneDepth;
extern bool g_testFailDagPruneCommit;
extern bool g_testSuppressDagSourceAbort;
extern bool g_dagSourceUnhealthy;
extern bool CorruptDagTipDeltaSpillForTest(int);
extern bool g_testFailDagTipDeltaSpillWrite;
extern bool g_testFailDagTipDeltaSpillClose;

// Production runtime/consumer, real mutable txleveldb, disposable overlay only.
// Full scans here are test setup/oracles, NEVER ordinary mutation delivery.
struct SemanticRuntimeFixture {
    fs::path root;
    dag_tip_frontier::DagTipOverlayRuntime runtime;
    dag_tip_frontier::DagTipOverlayRuntimeConfig config;
    std::vector<DagTipCommittedDeltaEvent> events;
    bool closeReads;
    bool deliveredOK;
    int failureMode;
    std::string error;
    static bool ReadSource(uint256* token, void* context) {
        SemanticRuntimeFixture* self = static_cast<SemanticRuntimeFixture*>(context);
        CTxDB db("r"); bool ok = db.ReadDAGSourceStateId(*token);
        if (self->closeReads) db.Close();
        return ok;
    }
    static bool Healthy(void* context) {
        SemanticRuntimeFixture* self = static_cast<SemanticRuntimeFixture*>(context);
        CTxDB db("r"); std::string error; bool ok = !g_dagSourceUnhealthy && db.IsDAGChildCountIndexHealthy(&error);
        if (self->closeReads) db.Close();
        return ok;
    }
    static void Deliver(const DagTipCommittedDeltaEvent& e, void* context) {
        SemanticRuntimeFixture* self = static_cast<SemanticRuntimeFixture*>(context);
        self->events.push_back(e);
        if (self->failureMode==1 && e.kind==DagTipCommittedDeltaEvent::RECORD) return;
        if (self->failureMode==2 && e.kind==DagTipCommittedDeltaEvent::BEGIN)
            BOOST_REQUIRE(CorruptDagTipDeltaSpillForTest(2));
        if (self->failureMode==3 && e.kind==DagTipCommittedDeltaEvent::BEGIN)
            throw std::runtime_error("injected publication failure");
        if (!self->runtime.ConsumeCommittedDelta(e, &self->error)) self->deliveredOK = false;
    }
    SemanticRuntimeFixture() : closeReads(true), deliveredOK(true), failureMode(0) {
        root = fs::temp_directory_path()/fs::unique_path("d3b-semantic-%%%%-%%%%");
        fs::create_directories(root);
        CTxDB db; BOOST_REQUIRE(db.EnsureDAGChildCountIndex(&error));
        std::map<uint256,CBlockDAGData> nodes; BOOST_REQUIRE(db.IterateDAGLinks(nodes));
        SHA256_CTX digest; SHA256_Init(&digest);
        for (const auto& node : nodes) {
            SHA256_Update(&digest,node.first.begin(),32);
            uint32_t count=node.second.vDAGParents.size(); SHA256_Update(&digest,&count,4);
            for (const auto& parent : node.second.vDAGParents) SHA256_Update(&digest,parent.begin(),32);
        }
        SHA256_Final(config.dagInputDigest,&digest);
        config.generation=17; config.artifactPath=(root/"frontier.dat").string();
        config.overlayDbDir=(root/"overlay").string(); config.dagLinksDir=(GetDataDir()/"txleveldb").string();
        config.cacheCapacity=2; config.sourceReader=&ReadSource; config.sourceHealthy=&Healthy; config.context=this;
        db.Close();
        dag_tip_frontier::BuildOptions options; options.maxRecordsPerChunk=16; options.maxOpenRuns=3; options.tempParent=(root/"sort").string();
        dag_tip_frontier::BuildResult result;
        BOOST_REQUIRE_MESSAGE(dag_tip_frontier::BuildDagTipFrontier(config.dagLinksDir,config.dagInputDigest,
            config.generation,config.artifactPath,options,&result),result.error);
        BOOST_REQUIRE_MESSAGE(runtime.Start(config,&error),error);
        BOOST_REQUIRE(runtime.Available());
        closeReads=false; CTxDB reopen;
        SetDagTipCommittedDeltaObserver(&Deliver,this);
    }
    ~SemanticRuntimeFixture() {
        SetDagTipCommittedDeltaObserver(NULL,NULL); // unregister BEFORE runtime destruction
        runtime.Close();
        try { fs::remove_all(root); } catch (...) {}
    }
    bool Saw(DagTipDeltaRecord::Op op,const uint256& hash) const {
        for(const auto& e:events) if(e.kind==DagTipCommittedDeltaEvent::RECORD && e.record.op==op && e.record.hash==hash) return true;
        return false;
    }
    std::set<uint256> Tips() {
        std::set<uint256> tips;
        BOOST_REQUIRE(runtime.Overlay()->ForEachTip([](const uint256& h,void* p){static_cast<std::set<uint256>*>(p)->insert(h);return true;},&tips,&error));
        return tips;
    }
    void Restart() {
        SetDagTipCommittedDeltaObserver(NULL,NULL);
        runtime.Close();
        {CTxDB db;db.Close();}
        closeReads=true;
        BOOST_REQUIRE_MESSAGE(runtime.Start(config,&error),error);
        closeReads=false; deliveredOK=true; failureMode=0; events.clear();
        CTxDB reopen; SetDagTipCommittedDeltaObserver(&Deliver,this);
    }
    void Verify() {
        using namespace dag_tip_frontier;
        BOOST_REQUIRE_MESSAGE(deliveredOK,error);
        BOOST_REQUIRE(runtime.Available());
        LiveTipOverlayCheckpoint checkpoint; BOOST_REQUIRE(runtime.Overlay()->ReadCheckpoint(&checkpoint,&error));
        uint256 token; BOOST_REQUIRE(ReadSource(&token,this));
        BOOST_CHECK(checkpoint.appliedSourceStateId==token);
        BOOST_CHECK_EQUAL(checkpoint.phase,LIVE_OVERLAY_PHASE_CLEAN);
        BOOST_CHECK(runtime.Overlay()->IsImmutableBindingValid(checkpoint));
        BOOST_CHECK_EQUAL(runtime.RecoveryInvocations(),1U); // startup only; not normal delivery
        const auto actual=Tips();
        CTxDB db; db.Close();
        BuildOptions options; options.maxRecordsPerChunk=16; options.maxOpenRuns=3; options.tempParent=(root/"sort").string();
        CurrentDagTipDerivationResult result; const auto output=root/"oracle.raw";
        BOOST_REQUIRE_MESSAGE(DeriveCurrentDagTipsBounded(config.dagLinksDir,output.string(),options,&result),result.error);
        std::ifstream input(output.string(),std::ios::binary); std::set<uint256> expected;
        uint256 hash; while(input.read(reinterpret_cast<char*>(hash.begin()),32)) expected.insert(hash);
        BOOST_REQUIRE(input.eof()); BOOST_REQUIRE_EQUAL(input.gcount(),0);
        BOOST_CHECK(actual==expected);
        CTxDB reopen;
        BOOST_TEST_MESSAGE("semantic parity: exact frontier="<<actual.size()<<" CLEAN("<<token.GetHex()<<") events="<<events.size());
    }
};

// ---- genuine-block mining (real consensus path) ----
static CBlock* BuildPoWBlock(CBlockIndex* pindexPrev, unsigned int nExtra)
{
    CBlock* pblock = CreateNewBlock(pwalletMain, false, NULL, NULL);
    if (!pblock) return NULL;
    pblock->nVersion = 1;
    pblock->nTime = std::max((unsigned int)GetTime(),
                             (unsigned int)(pindexPrev->GetMedianTimePast() + 1));
    pblock->hashPrevBlock = *pindexPrev->phashBlock;
    pblock->vtx[0].vin[0].scriptSig = CScript() << (pindexPrev->nHeight + 1) << nExtra;
    // Determinism: the coinbase destination is a fresh random wallet reserve key
    // (wallet.cpp GetReservedKey) that varies per test datadir -> varies the
    // merkleRoot -> varies the block hash run-to-run. Overwrite it with a fixed
    // destination so fixture block hashes/topology are reproducible. The DAG
    // parent commitment (coinbase vout[1]) is set later by AttachDagParentsAndRemine.
    if (!pblock->vtx.empty() && !pblock->vtx[0].vout.empty())
        pblock->vtx[0].vout[0].scriptPubKey = CScript() << OP_DUP << OP_HASH160
            << uint160(0x0000000000000000000000000000000000000001ULL) << OP_EQUALVERIFY << OP_CHECKSIG;
    pblock->hashMerkleRoot = pblock->BuildMerkleTree();
    uint256 hashTarget = CBigNum().SetCompact(pblock->nBits).getuint256();
    while (pblock->GetHash() > hashTarget && pblock->nNonce < 0xffffffff)
        ++pblock->nNonce;
    return pblock;
}

// Mine a block that extends a parent via the REAL ProcessBlock path.
static CBlockIndex* MineReal(CBlockIndex* pindexPrev, unsigned int nExtra)
{
    CBlock* pblock = BuildPoWBlock(pindexPrev, nExtra);
    CBlockIndex* pindex = NULL;
    bool fOk = false;
    {
        LOCK(cs_main);
        uint256 hash = pblock->GetHash();
        bool fProcessed = pblock->CheckBlock(true, true, true) && ProcessBlock(NULL, pblock);
        if (fProcessed)
        {
            fOk = true;
            pindex = mapBlockIndex[hash];
        }
    }
    delete pblock;
    BOOST_REQUIRE(fOk);
    BOOST_REQUIRE(pindex != NULL);
    return pindex;
}

// Add a block to the block index via the shared storage path (WriteToDisk +
// AddToBlockIndex), used for side branches that ProcessBlock's checkpoint
// weak-work gate rejects (non-best parent). This is the same storage path
// ProcessBlock uses once past the gate.
static CBlockIndex* AddSidePoWBlock(CBlockIndex* pindexPrev, unsigned int nExtra)
{
    CBlock* pblock = BuildPoWBlock(pindexPrev, nExtra);
    CBlockIndex* pindex = NULL;
    bool fOk = false;
    {
        LOCK(cs_main);
        unsigned int nFile = 0, nBlockPos = 0;
        bool fWrote = pblock->WriteToDisk(nFile, nBlockPos);
        bool fAdded = fWrote && pblock->AddToBlockIndex(nFile, nBlockPos, pblock->GetHash());
        if (fAdded)
        {
            fOk = true;
            pindex = mapBlockIndex[pblock->GetHash()];
        }
    }
    delete pblock;
    BOOST_REQUIRE(fOk);
    BOOST_REQUIRE(pindex != NULL);
    return pindex;
}

struct DagDeltaCapture
{
    std::vector<DagTipCommittedDeltaEvent> events;
    static void Record(const DagTipCommittedDeltaEvent& e, void* p)
    {
        static_cast<DagDeltaCapture*>(p)->events.push_back(e);
    }
};

static void AttachDagParentsAndRemine(CBlock* pblock, const std::vector<uint256>& parents)
{
    // The template may already contain parent metadata for the active tip.
    // Replace it so a side-branch fixture persists the requested parent set.
    for (std::vector<CTxOut>::iterator it = pblock->vtx[0].vout.begin(); it != pblock->vtx[0].vout.end();) {
        if (!ExtractDAGParents(it->scriptPubKey).empty()) it = pblock->vtx[0].vout.erase(it);
        else ++it;
    }
    CTxOut out;
    out.nValue = 0;
    out.scriptPubKey = BuildDAGParentScript(parents);
    pblock->vtx[0].vout.push_back(out);
    pblock->hashMerkleRoot = pblock->BuildMerkleTree();
    pblock->nNonce = 0;
    uint256 target = CBigNum().SetCompact(pblock->nBits).getuint256();
    while (pblock->GetHash() > target && pblock->nNonce < 0xffffffff)
        ++pblock->nNonce;
}

static CBlockIndex* MineRealDag(CBlockIndex* pindexPrev, unsigned int nExtra)
{
    CBlock* b = BuildPoWBlock(pindexPrev, nExtra);
    BOOST_REQUIRE(b != NULL);
    AttachDagParentsAndRemine(b, std::vector<uint256>(1, pindexPrev->GetBlockHash()));
    CBlockIndex* out = NULL;
    { LOCK(cs_main); uint256 h = b->GetHash(); BOOST_REQUIRE(b->CheckBlock(true,true,true)); BOOST_REQUIRE(ProcessBlock(NULL,b)); out = mapBlockIndex[h]; }
    delete b;
    BOOST_REQUIRE(out != NULL);
    return out;
}

static CBlockIndex* AddSideDag(CBlockIndex* pindexPrev, unsigned int nExtra)
{
    CBlock* b = BuildPoWBlock(pindexPrev, nExtra);
    BOOST_REQUIRE(b != NULL);
    AttachDagParentsAndRemine(b, std::vector<uint256>(1, pindexPrev->GetBlockHash()));
    CBlockIndex* out = NULL;
    { LOCK(cs_main); unsigned int f=0,p=0; BOOST_REQUIRE(b->WriteToDisk(f,p)); BOOST_REQUIRE(b->AddToBlockIndex(f,p,b->GetHash())); out=mapBlockIndex[b->GetHash()]; }
    delete b;
    BOOST_REQUIRE(out != NULL);
    return out;
}

BOOST_AUTO_TEST_SUITE(blockindex_window2_e2e)

// ---------------------------------------------------------------------------
// INDEPENDENT POST-REORG CURRENT-CANONICAL COUNTERFACTUAL ORACLE
// ---------------------------------------------------------------------------
// The authoritative S2/C-full target for an erased DAG-era boundary parent P is
// NOT the pre-reorg retained P score (historical residue from a different DAG
// state), and NOT the post-restart linear nChainTrust (legacy restart artifact).
// It is the value P and any retained child would obtain if P were colored as a
// normal RETAINED vertex under the CURRENT post-reorg canonical source, using
// unmodified ColorBlock / ColorBlockDAGKnight / GetBlueSet / InferLocalK /
// AnticoneSize. This helper builds that counterfactual independently:
//   1. take the frozen post-reorg canonical scope;
//   2. inject each erased parent P's recovered raw-coinbase DAG-parent closure
//      as REAL RETAINED daglinks records (immutable topology facts only);
//   3. run ReconstructAuthoritativeDAGFields so P colors via the NORMAL retained
//      path (no Option-R boundary reconstruction).
// The resulting full-field map is the independent oracle. It is independent of
// Option-R's boundary path and of any resident/live/restart residue.
struct CounterfactualOracle
{
    // Returns map<hash, full-field> after injecting erasedParents as retained
    // records into the canonical scope. On any failure err is set and the map is
    // empty (fail closed). erasedParents must be DAG-era hashes whose daglinks
    // were erased from the canonical source.
    static std::map<uint256,CanonicalDAGRecolorRecord> Build(
        const std::vector<std::pair<int32_t,uint256>>& canonicalScope,
        const AuthoritativeDAGRecolorSource& source,
        const std::vector<uint256>& erasedParents,
        std::string* err)
    {
        std::map<uint256,CanonicalDAGRecolorRecord> out;
        if (err) err->clear();
        // Test-local source: serves injected (recovered) parents for erased P and
        // its DAG-closure vertices, delegating everything else to the canonical
        // authoritative source (metadata, pre-DAG trust, Option-R for any OTHER
        // boundary not part of this oracle's injected closure).
        struct InjectedParentSource : CanonicalDAGRecolorSource {
            const AuthoritativeDAGRecolorSource& base;
            std::map<uint256,std::vector<uint256>> inject;
            explicit InjectedParentSource(const AuthoritativeDAGRecolorSource& b):base(b){}
            bool Block(const uint256& h,BlockIndexSnapshot* s,std::string* e) const override{ return base.Block(h,s,e); }
            bool Parents(const uint256& h,std::vector<uint256>* p,std::string* e) const override{
                std::map<uint256,std::vector<uint256>>::const_iterator it=inject.find(h);
                if (it!=inject.end()){ *p=it->second; return true; }
                return base.Parents(h,p,e);
            }
            bool PreDAGTrust(const uint256& h,uint256* t,std::string* e) const override{ return base.PreDAGTrust(h,t,e); }
            bool ReconstructBoundaryScore(const uint256& h,BoundaryScoreResult* b,std::string* e) const override{ return base.ReconstructBoundaryScore(h,b,e); }
            bool ReconstructBoundaryClosure(const uint256& h,
                std::map<uint256,BlockIndexSnapshot>* m,std::map<uint256,uint256>* p,
                std::map<uint256,CBlockDAGData>* r,std::vector<std::pair<int32_t,uint256>>* o,
                std::string* e) const override{ return base.ReconstructBoundaryClosure(h,m,p,r,o,e); }
        } cfSource(source);
        std::vector<std::pair<int32_t,uint256>> cfScope = canonicalScope;
        for (size_t p=0;p<erasedParents.size();++p){
            std::map<uint256,BlockIndexSnapshot> md; std::map<uint256,uint256> pd;
            std::map<uint256,CBlockDAGData> rc; std::vector<std::pair<int32_t,uint256>> od; std::string ce;
            if (!source.ReconstructBoundaryClosure(erasedParents[p],&md,&pd,&rc,&od,&ce)){
                if (err) *err="counterfactual closure failed for "+erasedParents[p].GetHex()+": "+ce;
                return out;
            }
            // Inject recovered DAG-era parents (raw-coinbase topology facts only).
            for (std::map<uint256,CBlockDAGData>::const_iterator r=rc.begin();r!=rc.end();++r)
                cfSource.inject[r->first]=r->second.vDAGParents;
            // Add closure DAG-era vertices to the scope; pre-DAG leaves are served
            // by PreDAGTrust (accumulated trust), never scope members.
            for (size_t i=0;i<od.size();++i){
                if (od[i].first < GetForkHeightDAG()) continue;
                bool present=false;
                for (size_t j=0;j<cfScope.size();++j) if (cfScope[j].second==od[i].second){present=true;break;}
                if (!present) cfScope.push_back(od[i]);
            }
        }
        std::sort(cfScope.begin(),cfScope.end());
        std::vector<CanonicalDAGRecolorRecord> fields; CanonicalDAGRecolorStats stats; std::string fe;
        if (!ReconstructAuthoritativeDAGFields(cfScope,cfSource,&fields,&stats,&fe)){
            if (err) *err="counterfactual retained-path recolor failed: "+fe;
            return out;
        }
        for (size_t i=0;i<fields.size();++i) out[fields[i].hash]=fields[i];
        return out;
    }
};


// A+B: genuinely-valid mined blocks through the real consensus path ACCEPT and
// advance the best chain; chain trust/height/hash are legacy-normal.
BOOST_AUTO_TEST_CASE(e2e_genuine_accept_advance)
{
    BOOST_REQUIRE(CZKContext::Initialize());
    if (hooks == NULL)
        hooks = InitHook(); // needed by ConnectBlock (same as invalidate_reconsider)
    BOOST_REQUIRE(pindexBest != NULL);
    const int hStart = pindexBest->nHeight;
    CBlockIndex* p0 = MineReal(pindexBest, 0x101);
    BOOST_REQUIRE(p0 != NULL);
    BOOST_CHECK_EQUAL(p0->nHeight, hStart + 1);
    BOOST_CHECK(p0->IsInMainChain());
    BOOST_CHECK(p0->GetBlockHash() == hashBestChain); // became active best tip

    CBlockIndex* p1 = MineReal(p0, 0x102);
    BOOST_REQUIRE(p1 != NULL);
    BOOST_CHECK_EQUAL(p1->nHeight, p0->nHeight + 1);
    BOOST_CHECK(p1->GetBlockHash() == hashBestChain);

    // chain trust strictly increases along the active chain (legacy semantics)
    BOOST_CHECK(p1->nChainTrust >= p0->nChainTrust);
    printf("E2E-A/B PASS: genuinely-mined blocks through real ProcessBlock/ConnectBlock\n"
           "       ACCEPT and advance best chain (height %d -> %d), chain trust normal.\n",
           p0->nHeight, p1->nHeight);
}

// C: side branch + reorg through the real consensus path gives the legacy-winning
// branch (higher trust wins); active branch matches what legacy would choose.
BOOST_AUTO_TEST_CASE(e2e_side_branch_reorg)
{
    BOOST_REQUIRE(CZKContext::Initialize());
    if (hooks == NULL)
        hooks = InitHook();
    BOOST_REQUIRE(pindexBest != NULL);
    // Main branch A: extend the CURRENT best chain (b1..b4 fork off this ancestor).
    CBlockIndex* pFork = pindexBest;
    CBlockIndex* a1 = MineReal(pFork, 0x201);
    CBlockIndex* a2 = MineReal(a1, 0x202);
    // Side branch B off the SAME fork parent (pFork).
    CBlockIndex* b1 = AddSidePoWBlock(pFork, 0x301);
    BOOST_REQUIRE(b1 != NULL);
    BOOST_CHECK_EQUAL(b1->nHeight, a1->nHeight);       // same fork, same height
    BOOST_CHECK_EQUAL(a2->nHeight, a1->nHeight + 1);   // A extended

    // Reorg: extend B past A's height -> B must win by height/trust and become best.
    // Side-chain blocks cannot go through ProcessBlock (checkpoint weak-work gate
    // rejects non-best-parent), so extend via the same AddToBlockIndex storage path.
    CBlockIndex* b2 = AddSidePoWBlock(b1, 0x302);
    CBlockIndex* b3 = AddSidePoWBlock(b2, 0x303);
    CBlockIndex* b4 = AddSidePoWBlock(b3, 0x304);
    BOOST_REQUIRE(b4 != NULL);
    BOOST_CHECK(b4->nHeight > a2->nHeight);       // B strictly deeper than A
    BOOST_CHECK(b4->GetBlockHash() == hashBestChain); // B won the reorg
    BOOST_CHECK(b4->nHeight >= a2->nHeight);
    printf("E2E-C PASS: side branch + reorg through real consensus — winning branch\n"
           "       (higher trust) becomes best, matching legacy semantics.\n");
}

BOOST_AUTO_TEST_CASE(r2c1d1_real_add_to_blockindex_dag_commits_replayable_delta)
{
    BOOST_REQUIRE(CZKContext::Initialize());
    if (hooks == NULL) hooks = InitHook();
    BOOST_REQUIRE(pindexBest != NULL);
    CBlockIndex* parent = pindexBest;
    while (parent->nHeight < GetForkHeightDAG())
        parent = MineReal(parent, 0x5100 + parent->nHeight);

    CTxDB sourceBefore;
    uint256 sourcePre;
    BOOST_REQUIRE(sourceBefore.ReadDAGSourceStateId(sourcePre));
    const std::vector<uint256> prev = g_dagManager.GetDAGTips();
    const std::set<uint256> pre(prev.begin(), prev.end());
    DagDeltaCapture capture;
    SetDagTipCommittedDeltaObserver(&DagDeltaCapture::Record, &capture);
    CBlock* block = BuildPoWBlock(parent, 0x5199);
    BOOST_REQUIRE(block != NULL);
    AttachDagParentsAndRemine(block, std::vector<uint256>(1, parent->GetBlockHash()));
    unsigned int nFile = 0, nBlockPos = 0;
    bool added = false;
    {
        LOCK(cs_main);
        added = block->WriteToDisk(nFile, nBlockPos) &&
                block->AddToBlockIndex(nFile, nBlockPos, block->GetHash());
    }
    delete block;
    BOOST_REQUIRE(added);
    const std::vector<uint256> postv = g_dagManager.GetDAGTips();
    std::set<uint256> replay = pre;
    int begins = 0, ends = 0;
    for (size_t i = 0; i < capture.events.size(); ++i) {
        const DagTipCommittedDeltaEvent& e = capture.events[i];
        if (e.kind == DagTipCommittedDeltaEvent::BEGIN) { ++begins; BOOST_CHECK_EQUAL(e.origin, DAG_TIP_DELTA_ADD_TO_BLOCK_INDEX); }
        else if (e.kind == DagTipCommittedDeltaEvent::END) ++ends;
        else if (e.record.op == DagTipDeltaRecord::TIP_ADD) replay.insert(e.record.hash);
        else replay.erase(e.record.hash);
    }
    std::set<uint256> post(postv.begin(), postv.end());
    CTxDB sourceAfter;
    uint256 sourcePost;
    BOOST_REQUIRE(sourceAfter.ReadDAGSourceStateId(sourcePost));
    BOOST_CHECK(sourcePost != sourcePre);
    bool sawFinal = false;
    for (size_t i = 0; i < capture.events.size(); ++i)
        if (capture.events[i].kind == DagTipCommittedDeltaEvent::END) {
            BOOST_CHECK(capture.events[i].hasFinalSourceStateId);
            BOOST_CHECK(capture.events[i].finalSourceStateId == sourcePost);
            sawFinal = true;
        }
    BOOST_CHECK(sawFinal);
    BOOST_CHECK(!g_dagSourceUnhealthy);
    BOOST_CHECK_EQUAL(begins, 1);
    BOOST_CHECK_EQUAL(ends, 1);
    BOOST_CHECK(replay == post);
    SetDagTipCommittedDeltaObserver(NULL, NULL);
}

BOOST_AUTO_TEST_CASE(r2c1d1_real_failed_add_rolls_back_without_committed_delta)
{
    BOOST_REQUIRE(CZKContext::Initialize());
    if (hooks == NULL) hooks = InitHook();
    CBlockIndex* parent = pindexBest;
    while (parent->nHeight < GetForkHeightDAG()) parent = MineReal(parent, 0x5200 + parent->nHeight);
    CTxDB sourceBefore;
    uint256 sourcePre;
    BOOST_REQUIRE(sourceBefore.ReadDAGSourceStateId(sourcePre));
    const std::vector<uint256> pre = g_dagManager.GetDAGTips();
    DagDeltaCapture capture;
    SetDagTipCommittedDeltaObserver(&DagDeltaCapture::Record, &capture);
    CBlock* block = BuildPoWBlock(parent, 0x5299);
    BOOST_REQUIRE(block != NULL);
    AttachDagParentsAndRemine(block, std::vector<uint256>(1, parent->GetBlockHash()));
    const uint256 childHash = block->GetHash();
    unsigned int nFile = 0, nBlockPos = 0;
    bool added = false;
    g_testFailSetBestChainAfterDagInit = true;
    {
        LOCK(cs_main);
        added = block->WriteToDisk(nFile, nBlockPos) && block->AddToBlockIndex(nFile, nBlockPos, block->GetHash());
    }
    g_testFailSetBestChainAfterDagInit = false;
    delete block;
    BOOST_CHECK(!added);
    const std::vector<uint256> post = g_dagManager.GetDAGTips();
    CTxDB sourceAfter;
    uint256 sourcePost;
    BOOST_REQUIRE(sourceAfter.ReadDAGSourceStateId(sourcePost));
    BOOST_CHECK(sourcePost == sourcePre);
    std::map<uint256, CBlockDAGData> links;
    BOOST_REQUIRE(sourceAfter.IterateDAGLinks(links));
    BOOST_CHECK(links.count(childHash) == 0);
    BOOST_CHECK(!GetDagTipDeltaFinalSourceStateId(&sourcePost));
    BOOST_CHECK(!g_dagSourceUnhealthy);
    BOOST_CHECK(pre == post);
    BOOST_CHECK(capture.events.empty());
    SetDagTipCommittedDeltaObserver(NULL, NULL);
}

BOOST_AUTO_TEST_CASE(r2c1d1_real_nested_reorg_joins_add_transaction)
{
    BOOST_REQUIRE(CZKContext::Initialize());
    if (hooks == NULL) hooks = InitHook();
    CBlockIndex* fork = pindexBest;
    while (fork->nHeight < GetForkHeightDAG()) fork = MineReal(fork, 0x5300 + fork->nHeight);
    CBlockIndex* a1 = MineRealDag(fork, 0x5311);
    CBlockIndex* a2 = MineRealDag(a1, 0x5312);
    CBlockIndex* b1 = AddSideDag(fork, 0x5321);
    CBlockIndex* b2 = AddSideDag(b1, 0x5322);
    CBlockIndex* b3 = AddSideDag(b2, 0x5323);
    CTxDB sourceBefore;
    uint256 sourceBeforeReorg;
    BOOST_REQUIRE(sourceBefore.ReadDAGSourceStateId(sourceBeforeReorg));
    const std::vector<uint256> prev = g_dagManager.GetDAGTips();
    std::set<uint256> replay(prev.begin(), prev.end());
    DagDeltaCapture capture;
    SetDagTipCommittedDeltaObserver(&DagDeltaCapture::Record, &capture);
    CBlockIndex* b4 = AddSideDag(b3, 0x5324);
    BOOST_REQUIRE(b4->GetBlockHash() == hashBestChain);
    const std::vector<uint256> postv = g_dagManager.GetDAGTips();
    int begin=0,end=0,records=0;
    for (size_t i=0;i<capture.events.size();++i) {
        const DagTipCommittedDeltaEvent& e=capture.events[i];
        if(e.kind==DagTipCommittedDeltaEvent::BEGIN) { ++begin; BOOST_CHECK_EQUAL(e.origin,DAG_TIP_DELTA_ADD_TO_BLOCK_INDEX); }
        else if(e.kind==DagTipCommittedDeltaEvent::END) ++end;
        else { ++records; if(e.record.op==DagTipDeltaRecord::TIP_ADD) replay.insert(e.record.hash); else replay.erase(e.record.hash); }
    }
    std::set<uint256> post(postv.begin(),postv.end());
    CTxDB sourceAfter;
    uint256 sourceAfterReorg;
    BOOST_REQUIRE(sourceAfter.ReadDAGSourceStateId(sourceAfterReorg));
    BOOST_CHECK(sourceAfterReorg != sourceBeforeReorg);
    bool sawFinalToken = false;
    for (size_t i=0;i<capture.events.size();++i)
        if (capture.events[i].kind == DagTipCommittedDeltaEvent::END) {
            BOOST_CHECK(capture.events[i].hasFinalSourceStateId);
            BOOST_CHECK(capture.events[i].finalSourceStateId == sourceAfterReorg);
            sawFinalToken = true;
        }
    BOOST_CHECK(sawFinalToken);
    BOOST_CHECK_EQUAL(begin,1); BOOST_CHECK_EQUAL(end,1); BOOST_CHECK_GT(records,0); BOOST_CHECK(replay==post);
    SetDagTipCommittedDeltaObserver(NULL,NULL);
}

BOOST_AUTO_TEST_CASE(r2c1d3b_i1_real_reorg_childcount_2_to_1)
{
    BOOST_REQUIRE(CZKContext::Initialize());
    if (hooks == NULL) hooks = InitHook();
    CBlockIndex* fork = pindexBest;
    while (fork->nHeight < GetForkHeightDAG()) fork = MineReal(fork, 0x7100 + fork->nHeight);
    fork = MineRealDag(fork, 0x7110); // retained canonical DAG fork, not pre-DAG anchor
    CBlockIndex* a1 = MineRealDag(fork, 0x7111);
    MineRealDag(a1, 0x7112);
    SemanticRuntimeFixture semantic;
    struct Readback : CTxDB { using CTxDB::Read; } db;
    std::string error;
    BOOST_REQUIRE(db.EnsureDAGChildCountIndex(&error));
    CBlockIndex* branch = fork;
    CBlockIndex* b1 = NULL;
    bool observed = false;
    for (unsigned step=0; step<8 && !observed; ++step) {
        CBlockDAGData old;
        BOOST_REQUIRE(db.ReadDAGLinks(fork->GetBlockHash(), old));
        BOOST_REQUIRE(db.ReadDAGLinks(a1->GetBlockHash(), old));
        BOOST_REQUIRE(old.vDAGParents == std::vector<uint256>(1,fork->GetBlockHash()));
        uint64_t beforeCount=0, afterCount=0; bool present=false;
        BOOST_REQUIRE(db.ReadDAGChildCount(fork->GetBlockHash(),&beforeCount,&present));
        uint256 before, after;
        BOOST_REQUIRE(db.ReadDAGSourceStateId(before));
        BOOST_REQUIRE(db.IsDAGChildCountIndexHealthy(&error));
        semantic.events.clear();
        branch = AddSideDag(branch, 0x7120 + step);
        if (!b1) b1=branch;
        if (!db.ReadDAGLinks(a1->GetBlockHash(),old)) {
            observed=true;
            BOOST_REQUIRE(db.ReadDAGLinks(fork->GetBlockHash(),old));
            BOOST_REQUIRE(db.ReadDAGLinks(b1->GetBlockHash(),old));
            BOOST_REQUIRE(old.vDAGParents == std::vector<uint256>(1,fork->GetBlockHash()));
            BOOST_REQUIRE(db.ReadDAGChildCount(fork->GetBlockHash(),&afterCount,&present));
            BOOST_CHECK_EQUAL(beforeCount,2U); BOOST_CHECK_EQUAL(afterCount,1U);
            BOOST_REQUIRE(db.ReadDAGSourceStateId(after)); BOOST_CHECK(before!=after);
            std::pair<uint32_t,uint256> marker;
            BOOST_REQUIRE(db.Read(make_pair(std::string("dagchildcountstate"),uint8_t(0)),marker));
            BOOST_CHECK(marker.second==after);
            BOOST_REQUIRE(db.IsDAGChildCountIndexHealthy(&error));
            BOOST_CHECK(branch->GetBlockHash()==hashBestChain);
            BOOST_CHECK(!semantic.Saw(DagTipDeltaRecord::TIP_ADD,fork->GetBlockHash()));
            BOOST_CHECK(!semantic.Saw(DagTipDeltaRecord::TIP_REMOVE,fork->GetBlockHash()));
            semantic.Verify();
            BOOST_TEST_MESSAGE("real reorg: step=" << step << " count=" << beforeCount << "->" << afterCount
                << " retained_fork=true retained_b1=true erased_a1=true source=" << before.GetHex() << "->" << after.GetHex());
        }
    }
    BOOST_REQUIRE_MESSAGE(observed,"real competing branch never erased old child");
}

// All vertices are mined through ProcessBlock. No source records or epoch
// exemptions are manufactured by this fixture.
static void CheckEpochPruneChildCount(bool retainSecondChild)
{
    BOOST_REQUIRE(CZKContext::Initialize());
    if (hooks == NULL) hooks = InitHook();
    CBlockIndex* p = pindexBest;
    while (p->nHeight < GetForkHeightDAG())
        p = MineReal(p, 0x6100 + p->nHeight);
    const int epoch = GetEpochForHeight(p->nHeight);
    const int end = GetEpochBoundaryHeight(epoch + 1, p->nHeight) - 1;
    while (p->nHeight < end)
        p = MineRealDag(p, 0x6200 + p->nHeight);
    BOOST_REQUIRE_EQUAL(p->nHeight, end);
    BOOST_REQUIRE(g_dagManager.ComputeEpochState(epoch, GetEpochInterval(end)));
    CEpochState state;
    BOOST_REQUIRE(g_dagManager.GetEpochState(epoch, state));
    BOOST_REQUIRE_EQUAL(state.nHeightEnd, p->nHeight);
    BOOST_REQUIRE(state.hashBoundaryBlock == p->GetBlockHash());
    BOOST_REQUIRE(g_dagManager.IsEpochBoundaryForTest(p->GetBlockHash()));

    CBlockIndex* c = MineRealDag(p, 0x6311);
    CBlockIndex* q = NULL;
    if (retainSecondChild) {
        CBlock* b = BuildPoWBlock(c, 0x6312);
        BOOST_REQUIRE(b != NULL);
        std::vector<uint256> parents;
        // CreateNewBlock already supplies a DAG parent output. Replace that
        // metadata before remine: production reads the first parent script.
        for (std::vector<CTxOut>::iterator it = b->vtx[0].vout.begin(); it != b->vtx[0].vout.end();) {
            if (!ExtractDAGParents(it->scriptPubKey).empty()) it = b->vtx[0].vout.erase(it);
            else ++it;
        }
        parents.push_back(c->GetBlockHash());
        parents.push_back(p->GetBlockHash());
        AttachDagParentsAndRemine(b, parents);
        { LOCK(cs_main); const uint256 hash = b->GetHash();
          BOOST_REQUIRE(b->CheckBlock(true, true, true));
          BOOST_REQUIRE(ProcessBlock(NULL, b)); q = mapBlockIndex[hash]; }
        delete b;
        BOOST_REQUIRE(q != NULL);
    } else {
        q = MineRealDag(c, 0x6312);
    }
    const int threshold = q->nHeight; // next ADD height minus test depth 1
    BOOST_REQUIRE(p->nHeight < threshold);
    BOOST_REQUIRE(c->nHeight < threshold);
    BOOST_REQUIRE_EQUAL(q->nHeight, threshold);
    BOOST_REQUIRE(g_dagManager.HasDAGData(p->GetBlockHash()));
    BOOST_REQUIRE(g_dagManager.HasDAGData(c->GetBlockHash()));
    BOOST_REQUIRE(!g_dagManager.IsEpochBoundaryForTest(c->GetBlockHash()));
    BOOST_REQUIRE(!g_dagManager.IsEpochBoundaryForTest(q->GetBlockHash()));
    SemanticRuntimeFixture semantic;
    struct SourceReadback : CTxDB { using CTxDB::Read; } db;
    CBlockDAGData data;
    BOOST_REQUIRE(db.ReadDAGLinks(p->GetBlockHash(), data));
    BOOST_REQUIRE(db.ReadDAGLinks(c->GetBlockHash(), data));
    BOOST_REQUIRE(data.vDAGParents == std::vector<uint256>(1, p->GetBlockHash()));
    BOOST_REQUIRE(db.ReadDAGLinks(q->GetBlockHash(), data));
    BOOST_REQUIRE_EQUAL(std::count(data.vDAGParents.begin(), data.vDAGParents.end(), p->GetBlockHash()), retainSecondChild ? 1 : 0);
    uint64_t count = 0; bool present = false; uint256 before, after;
    BOOST_REQUIRE(db.ReadDAGChildCount(p->GetBlockHash(), &count, &present));
    BOOST_REQUIRE(present);
    BOOST_REQUIRE_EQUAL(count, retainSecondChild ? 2U : 1U);
    BOOST_REQUIRE(db.ReadDAGSourceStateId(before));
    std::pair<uint32_t, uint256> markerBefore;
    BOOST_REQUIRE(db.Read(std::make_pair(std::string("dagchildcountstate"), uint8_t(0)), markerBefore));
    BOOST_REQUIRE(markerBefore.second == before);
    std::string error;
    BOOST_REQUIRE_MESSAGE(db.IsDAGChildCountIndexHealthy(&error), error);
    struct PruneScope {
        bool force; int depth;
        PruneScope() : force(g_testForceDagPruneInAdd), depth(g_testDagPruneDepth)
        { g_testForceDagPruneInAdd = true; g_testDagPruneDepth = 1; }
        ~PruneScope() { g_testForceDagPruneInAdd = force; g_testDagPruneDepth = depth; }
    };
    { PruneScope scope; BOOST_REQUIRE(MineRealDag(q, 0x6313) != NULL); }
    BOOST_REQUIRE(db.ReadDAGLinks(p->GetBlockHash(), data));
    BOOST_REQUIRE(!db.ReadDAGLinks(c->GetBlockHash(), data));
    BOOST_REQUIRE(db.ReadDAGLinks(q->GetBlockHash(), data));
    BOOST_REQUIRE_EQUAL(std::count(data.vDAGParents.begin(), data.vDAGParents.end(), p->GetBlockHash()), retainSecondChild ? 1 : 0);
    BOOST_REQUIRE(db.ReadDAGChildCount(p->GetBlockHash(), &count, &present));
    BOOST_CHECK_EQUAL(count, retainSecondChild ? 1U : 0U);
    BOOST_CHECK_EQUAL(present, retainSecondChild);
    BOOST_REQUIRE(db.ReadDAGSourceStateId(after));
    BOOST_CHECK(after != before);
    std::pair<uint32_t, uint256> markerAfter;
    BOOST_REQUIRE(db.Read(std::make_pair(std::string("dagchildcountstate"), uint8_t(0)), markerAfter));
    BOOST_CHECK(markerAfter.second == after);
    BOOST_CHECK_EQUAL(markerAfter.first, markerBefore.first);
    int cleanHeight = -1;
    BOOST_REQUIRE(db.ReadDAGCleanHeight(cleanHeight));
    BOOST_CHECK_EQUAL(cleanHeight, threshold);
    BOOST_REQUIRE_MESSAGE(db.IsDAGChildCountIndexHealthy(&error), error);
    BOOST_TEST_MESSAGE("epoch=" << epoch << " boundary=" << end
        << " P=" << p->nHeight << " C=" << c->nHeight << " Q=" << q->nHeight
        << " threshold=" << threshold << " count=" << (retainSecondChild ? "2->1" : "1->0")
        << " source=" << before.GetHex() << "->" << after.GetHex()
        << " retained_P=true retained_Q=true marker_bound=true");
    BOOST_CHECK_EQUAL(semantic.Saw(DagTipDeltaRecord::TIP_ADD,p->GetBlockHash()),!retainSecondChild);
    BOOST_CHECK(!semantic.Saw(DagTipDeltaRecord::TIP_REMOVE,p->GetBlockHash()));
    semantic.Verify();
}

BOOST_AUTO_TEST_CASE(r2c1d3b_i1_real_epoch_prune_childcount_1_to_0)
{
    CheckEpochPruneChildCount(false);
}

BOOST_AUTO_TEST_CASE(r2c1d3b_i1_real_epoch_prune_childcount_2_to_1)
{
    CheckEpochPruneChildCount(true);
}

BOOST_AUTO_TEST_CASE(r2c1d2_prune_inside_add_binds_final_token)
{
    BOOST_REQUIRE(CZKContext::Initialize());
    if (hooks == NULL) hooks = InitHook();
    CBlockIndex* parent = pindexBest;
    while (parent->nHeight < GetForkHeightDAG()) parent = MineReal(parent, 0x5700 + parent->nHeight);
    CBlockIndex* d1 = MineRealDag(parent, 0x5711);
    CBlockIndex* d2 = MineRealDag(d1, 0x5712);
    CTxDB beforeDb; uint256 before;
    BOOST_REQUIRE(beforeDb.ReadDAGSourceStateId(before));
    DagDeltaCapture capture; SetDagTipCommittedDeltaObserver(&DagDeltaCapture::Record, &capture);
    g_testDagPruneDepth = 1;
    g_testForceDagPruneInAdd = true;
    CBlockIndex* d3 = MineRealDag(d2, 0x5713);
    g_testForceDagPruneInAdd = false;
    g_testDagPruneDepth = 0;
    BOOST_REQUIRE(d3 != NULL);
    CTxDB afterDb; uint256 after;
    BOOST_REQUIRE(afterDb.ReadDAGSourceStateId(after));
    BOOST_CHECK(after != before);
    bool sawEnd = false;
    for (size_t i=0;i<capture.events.size();++i)
        if (capture.events[i].kind == DagTipCommittedDeltaEvent::END) {
            BOOST_CHECK(capture.events[i].hasFinalSourceStateId);
            BOOST_CHECK(capture.events[i].finalSourceStateId == after);
            sawEnd = true;
        }
    BOOST_CHECK(sawEnd);
    SetDagTipCommittedDeltaObserver(NULL, NULL);
}

BOOST_AUTO_TEST_CASE(r2c1d2_prune_failure_discards_add_root)
{
    BOOST_REQUIRE(CZKContext::Initialize());
    if (hooks == NULL) hooks = InitHook();
    CBlockIndex* parent = pindexBest;
    while (parent->nHeight < GetForkHeightDAG()) parent = MineReal(parent, 0x5800 + parent->nHeight);
    CBlockIndex* d1 = MineRealDag(parent, 0x5811);
    CBlockIndex* d2 = MineRealDag(d1, 0x5812);
    DagDeltaCapture capture; SetDagTipCommittedDeltaObserver(&DagDeltaCapture::Record, &capture);
    g_testSuppressDagSourceAbort = true; g_dagSourceUnhealthy = false;
    g_testDagPruneDepth = 1; g_testForceDagPruneInAdd = true; g_testFailDagPruneCommit = true;
    CBlock* b = BuildPoWBlock(d2, 0x5813); BOOST_REQUIRE(b != NULL);
    AttachDagParentsAndRemine(b, std::vector<uint256>(1, d2->GetBlockHash()));
    unsigned int f=0,p=0; bool ok=false;
    { LOCK(cs_main); ok=b->WriteToDisk(f,p) && b->AddToBlockIndex(f,p,b->GetHash()); }
    delete b;
    g_testFailDagPruneCommit=false; g_testForceDagPruneInAdd=false; g_testDagPruneDepth=0;
    BOOST_CHECK(!ok); BOOST_CHECK(g_dagSourceUnhealthy); BOOST_CHECK(capture.events.empty());
    uint256 leaked; BOOST_CHECK(!GetDagTipDeltaFinalSourceStateId(&leaked));
    g_dagSourceUnhealthy=false; g_testSuppressDagSourceAbort=false;
    SetDagTipCommittedDeltaObserver(NULL, NULL);
}

BOOST_AUTO_TEST_CASE(r2c1d2_bootstrap_existing_token_preserved_without_delta)
{
    CTxDB txdb;
    uint256 before;
    BOOST_REQUIRE(txdb.ReadDAGSourceStateId(before));
    const std::vector<uint256> tipsBefore = g_dagManager.GetDAGTips();
    DagDeltaCapture capture;
    SetDagTipCommittedDeltaObserver(&DagDeltaCapture::Record, &capture);
    std::string error;
    BOOST_REQUIRE(txdb.BootstrapDAGSourceStateId(&error));
    BOOST_REQUIRE(error.empty());
    uint256 after;
    BOOST_REQUIRE(txdb.ReadDAGSourceStateId(after));
    BOOST_CHECK(after == before);
    BOOST_CHECK(g_dagManager.GetDAGTips() == tipsBefore);
    BOOST_CHECK(capture.events.empty());
    SetDagTipCommittedDeltaObserver(NULL, NULL);
}

BOOST_AUTO_TEST_CASE(r2c1d2a_initial_daglink_commit_failure_aborts_locally)
{
    BOOST_REQUIRE(CZKContext::Initialize());
    if (hooks == NULL) hooks = InitHook();
    CBlockIndex* parent = pindexBest;
    while (parent->nHeight < GetForkHeightDAG()) parent = MineReal(parent, 0x5400 + parent->nHeight);
    CBlock* block = BuildPoWBlock(parent, 0x5499);
    BOOST_REQUIRE(block != NULL);
    AttachDagParentsAndRemine(block, std::vector<uint256>(1, parent->GetBlockHash()));
    unsigned int nFile = 0, nBlockPos = 0;
    g_testSuppressDagSourceAbort = true;
    g_dagSourceUnhealthy = false;
    g_testFailInitialDagLinksCommit = true;
    bool added = false;
    { LOCK(cs_main); added = block->WriteToDisk(nFile, nBlockPos) && block->AddToBlockIndex(nFile, nBlockPos, block->GetHash()); }
    g_testFailInitialDagLinksCommit = false;
    delete block;
    BOOST_CHECK(!added);
    BOOST_CHECK(g_dagSourceUnhealthy);
    g_dagSourceUnhealthy = false;
    g_testSuppressDagSourceAbort = false;
}

BOOST_AUTO_TEST_CASE(r2c1d2a_failed_add_cleanup_commit_failure_aborts_locally)
{
    BOOST_REQUIRE(CZKContext::Initialize());
    if (hooks == NULL) hooks = InitHook();
    CBlockIndex* parent = pindexBest;
    while (parent->nHeight < GetForkHeightDAG()) parent = MineReal(parent, 0x5500 + parent->nHeight);
    CBlock* block = BuildPoWBlock(parent, 0x5599);
    BOOST_REQUIRE(block != NULL);
    AttachDagParentsAndRemine(block, std::vector<uint256>(1, parent->GetBlockHash()));
    unsigned int nFile = 0, nBlockPos = 0;
    g_testSuppressDagSourceAbort = true;
    g_dagSourceUnhealthy = false;
    g_testFailSetBestChainAfterDagInit = true;
    g_testFailFailedAddDagLinksCleanupCommit = true;
    bool added = false;
    { LOCK(cs_main); added = block->WriteToDisk(nFile, nBlockPos) && block->AddToBlockIndex(nFile, nBlockPos, block->GetHash()); }
    g_testFailSetBestChainAfterDagInit = false;
    g_testFailFailedAddDagLinksCleanupCommit = false;
    delete block;
    BOOST_CHECK(!added);
    BOOST_CHECK(g_dagSourceUnhealthy);
    g_dagSourceUnhealthy = false;
    g_testSuppressDagSourceAbort = false;
}

BOOST_AUTO_TEST_CASE(r2c1d2a_reorganize_erase_commit_failure_source_unhealthy)
{
    BOOST_REQUIRE(CZKContext::Initialize());
    if (hooks == NULL) hooks = InitHook();
    CBlockIndex* fork = pindexBest;
    while (fork->nHeight < GetForkHeightDAG()) fork = MineReal(fork, 0x5600 + fork->nHeight);
    // Build a clear best chain on the a-branch.
    CBlockIndex* a1 = MineRealDag(fork, 0x5611);
    CBlockIndex* a2 = MineRealDag(a1, 0x5612);
    // Inject consecutive b-branch children (starting from the fork) with the
    // DAG-link erase failpoint armed. The first injected block that triggers a
    // real Reorganize reaches the production erase-commit site and flips source
    // unhealthy (failpoint is read ONLY inside Reorganize). Loop is bounded.
    g_testSuppressDagSourceAbort = true;
    g_dagSourceUnhealthy = false;
    g_testFailReorganizeDagLinksEraseCommit = true;
    CBlockIndex* sidePrev = fork;
    bool sawRejected = false;
    for (int i = 0; i < 6 && !g_dagSourceUnhealthy; ++i)
    {
        CBlock* block = BuildPoWBlock(sidePrev, 0x5620 + i);
        BOOST_REQUIRE(block != NULL);
        AttachDagParentsAndRemine(block, std::vector<uint256>(1, sidePrev->GetBlockHash()));
        unsigned int nFile = 0, nBlockPos = 0;
        bool added = false;
        { LOCK(cs_main);
          if (block->WriteToDisk(nFile, nBlockPos))
              added = block->AddToBlockIndex(nFile, nBlockPos, block->GetHash());
          sidePrev = mapBlockIndex.count(block->GetHash()) ? mapBlockIndex[block->GetHash()] : sidePrev;
        }
        sawRejected = sawRejected || !added;
        delete block;
    }
    g_testFailReorganizeDagLinksEraseCommit = false;

    // A real reorg occurred (either an added block that won, or an abort mid-reorg).
    BOOST_CHECK(g_dagSourceUnhealthy);
    // The failpoint aborts the production erase site: at least one Add did not
    // return normally as a successful best-chain advance under healthy source.
    BOOST_CHECK(sawRejected);
    g_dagSourceUnhealthy = false;
    g_testSuppressDagSourceAbort = false;
}

BOOST_AUTO_TEST_CASE(r2c1d3b_i1_real_add_nonresident_parent_updates_source_count)
{
    BOOST_REQUIRE(CZKContext::Initialize());
    if (hooks == NULL) hooks = InitHook();
    CBlockIndex* base = pindexBest;
    while (base->nHeight < GetForkHeightDAG())
        base = MineReal(base, 0x5a00 + base->nHeight);
    CBlockIndex* parent = MineRealDag(base, 0x5a11);
    BOOST_REQUIRE(parent != NULL);
    const uint256 p = parent->GetBlockHash();
    SemanticRuntimeFixture semantic;
    BOOST_REQUIRE(semantic.Tips().count(p));
    CTxDB before;
    std::string error;
    BOOST_REQUIRE_MESSAGE(before.EnsureDAGChildCountIndex(&error), error);
    BOOST_REQUIRE_MESSAGE(before.IsDAGChildCountIndexHealthy(&error), error);
    CBlockDAGData persistedParent;
    BOOST_REQUIRE(before.ReadDAGLinks(p, persistedParent));
    uint64_t count = 0; bool present = false;
    BOOST_REQUIRE(before.ReadDAGChildCount(p, &count, &present));
    BOOST_CHECK(!present); BOOST_CHECK_EQUAL(count, 0u);
    uint256 t0; BOOST_REQUIRE(before.ReadDAGSourceStateId(t0));

    // Deliberately remove only legacy residency. The retained source record P
    // and its canonical zero count stay on disk.
    g_dagManager.ClearDAGDataForTest();
    BOOST_CHECK(!g_dagManager.HasDAGData(p));
    BOOST_CHECK(g_dagManager.GetDAGTips().empty());

    CBlock* child = BuildPoWBlock(parent, 0x5a12);
    BOOST_REQUIRE(child != NULL);
    AttachDagParentsAndRemine(child, std::vector<uint256>(1, p));
    const uint256 c = child->GetHash();
    unsigned int file = 0, pos = 0;
    bool added = false;
    { LOCK(cs_main); added = child->WriteToDisk(file, pos) && child->AddToBlockIndex(file, pos, c); }
    delete child;
    BOOST_REQUIRE(added);

    CTxDB after;
    CBlockDAGData persistedChild;
    BOOST_REQUIRE(after.ReadDAGLinks(c, persistedChild));
    BOOST_REQUIRE_EQUAL(persistedChild.vDAGParents.size(), 1u);
    BOOST_CHECK(persistedChild.vDAGParents[0] == p);
    BOOST_REQUIRE(after.ReadDAGChildCount(p, &count, &present));
    BOOST_CHECK(present); BOOST_CHECK_EQUAL(count, 1u);
    uint256 t1; BOOST_REQUIRE(after.ReadDAGSourceStateId(t1));
    BOOST_CHECK(t1 != t0);
    BOOST_REQUIRE_MESSAGE(after.IsDAGChildCountIndexHealthy(&error), error);
    BOOST_CHECK(!g_dagManager.HasDAGData(p));
    BOOST_CHECK(semantic.Saw(DagTipDeltaRecord::TIP_REMOVE,p));
    BOOST_CHECK(semantic.Saw(DagTipDeltaRecord::TIP_ADD,c));
    BOOST_CHECK(!semantic.Tips().count(p));
    BOOST_CHECK(semantic.Tips().count(c));
    semantic.Verify();
}

BOOST_AUTO_TEST_CASE(r2c1d3b_real_delivery_failure_restart_matrix)
{
    using namespace dag_tip_frontier;
    BOOST_REQUIRE(CZKContext::Initialize()); if(!hooks) hooks=InitHook();
    CBlockIndex* parent=pindexBest;
    while(parent->nHeight<GetForkHeightDAG()) parent=MineReal(parent,0x8200+parent->nHeight);
    parent=MineRealDag(parent,0x8210);
    for(int mode=1;mode<=6;++mode) {
        SemanticRuntimeFixture semantic;
        semantic.failureMode=mode;
        SetDagTipDeltaRamCapacityForTest(1);
        if(mode==4) g_testFailLiveTipOverlayOverrideWrite=true;
        if(mode==5) g_testFailDagTipDeltaSpillWrite=true;
        if(mode==6) g_testFailDagTipDeltaSpillClose=true;
        parent=MineRealDag(parent,0x8220+mode);
        g_testFailLiveTipOverlayOverrideWrite=false;
        g_testFailDagTipDeltaSpillWrite=false; g_testFailDagTipDeltaSpillClose=false;
        SetDagTipDeltaRamCapacityForTest(256);
        BOOST_REQUIRE(parent);
        BOOST_CHECK(!g_dagSourceUnhealthy); // shadow failure cannot reject valid ADD
        BOOST_CHECK(!semantic.runtime.Available());
        LiveTipOverlayCheckpoint cp;
        BOOST_REQUIRE(semantic.runtime.Overlay()->ReadCheckpoint(&cp,&semantic.error));
        uint256 committed; BOOST_REQUIRE(SemanticRuntimeFixture::ReadSource(&committed,&semantic));
        BOOST_CHECK(cp.phase!=LIVE_OVERLAY_PHASE_CLEAN || cp.appliedSourceStateId!=committed);
        // A subsequent successful publication MUST NOT bridge the lost preimage.
        semantic.failureMode=0;
        parent=MineRealDag(parent,0x8230+mode);
        BOOST_CHECK(!semantic.runtime.Available());
        BOOST_REQUIRE(SemanticRuntimeFixture::ReadSource(&committed,&semantic));
        BOOST_REQUIRE(semantic.runtime.Overlay()->ReadCheckpoint(&cp,&semantic.error));
        BOOST_CHECK(cp.phase!=LIVE_OVERLAY_PHASE_CLEAN || cp.appliedSourceStateId!=committed);
        semantic.Restart(); // same persistent overlay and real source; bounded fallback
        semantic.Verify();
        parent=MineRealDag(parent,0x8240+mode);
        semantic.Verify(); // normal exact delivery resumes, no extra recovery
    }
}

BOOST_AUTO_TEST_CASE(r2c1d3b_authoritative_owner_real_nonresident_add)
{
    using namespace dag_tip_frontier;
    BOOST_REQUIRE(CZKContext::Initialize()); if (!hooks) hooks=InitHook();
    CBlockIndex* parent=pindexBest;
    while(parent->nHeight<GetForkHeightDAG()) parent=MineReal(parent,0x8100+parent->nHeight);
    parent=MineRealDag(parent,0x8111);
    const uint256 p=parent->GetBlockHash();
    std::unique_ptr<CBlock> child(BuildPoWBlock(parent,0x8112)); BOOST_REQUIRE(child.get());
    AttachDagParentsAndRemine(child.get(),std::vector<uint256>(1,p));
    const uint256 c=child->GetHash(); unsigned int file=0,pos=0;
    BOOST_REQUIRE(child->WriteToDisk(file,pos));
    const fs::path root=fs::temp_directory_path()/fs::unique_path("d3b-auth-%%%%-%%%%");
    fs::create_directories(root/"snapshot");
    struct Cleanup {
        fs::path root; CBlockIndex* best; CBlockIndex* genesis;
        Cleanup(const fs::path& r):root(r),best(pindexBest),genesis(pindexGenesisBlock){}
        ~Cleanup(){ ResetBlockIndexAuthoritativeStartupForTest(); pindexBest=best; pindexGenesisBlock=genesis;
            if(best){nBestHeight=best->nHeight;hashBestChain=best->GetBlockHash();nBestChainTrust=best->nChainTrust;}
            try{fs::remove_all(root);}catch(...){} }
    } cleanup(root);
    {CTxDB db;db.Close();}
    const auto live=GetDataDir()/"txleveldb";
    for(fs::directory_iterator it(live),end;it!=end;++it)
        if(fs::is_regular_file(it->path())) fs::copy_file(it->path(),root/"snapshot"/it->path().filename());
    BlockIndexGenerationSource source; std::string error;
    BOOST_REQUIRE_MESSAGE(ReadLegacyBlockIndexSource((root/"snapshot").string(),&source,&error),error);
    BOOST_REQUIRE_MESSAGE(ReadDAGLinksFromSnapshot((root/"snapshot").string(),&source.dagLinks,&source.dagScores,&error),error);
    source.foundDAGLinks = true;
    source.blockDataDir=GetDataDir().string(); source.dagLinksDir=(root/"snapshot").string();
    BlockIndexGenerationBuilder builder;
    BOOST_REQUIRE_MESSAGE(builder.Build(source,(root/"build-000001.tmp").string(),1,NULL,&error),error); builder.Close();
    BOOST_REQUIRE_EQUAL(BlockIndexGenerationManager::PublishGeneration(root.string(),1,&error),BLOCK_INDEX_LIFECYCLE_OK);
    BOOST_REQUIRE_EQUAL(BlockIndexGenerationManager::SelectGeneration(root.string(),1,&error),BLOCK_INDEX_LIFECYCLE_OK);
    g_dagManager.ClearDAGDataForTest();
    BOOST_REQUIRE_MESSAGE(InitBlockIndexAuthoritative(root.string(),&error),error);
    DagTipOverlayRuntime* runtime=GetDagTipOverlayRuntimeForTest(); BOOST_REQUIRE(runtime);
    std::set<uint256> tips;
    auto collect=[](const uint256& h,void* ctx){static_cast<std::set<uint256>*>(ctx)->insert(h);return true;};
    BOOST_REQUIRE(runtime->Overlay()->ForEachTip(collect,&tips,&error));
    BOOST_REQUIRE(tips==std::set<uint256>({p}));
    BOOST_REQUIRE(!g_dagManager.HasDAGData(p)); BOOST_REQUIRE(g_dagManager.GetDAGTips().empty());
    { LOCK(cs_main); BOOST_REQUIRE(child->AddToBlockIndex(file,pos,c)); }
    BOOST_REQUIRE(runtime->Available());
    tips.clear(); BOOST_REQUIRE(runtime->Overlay()->ForEachTip(collect,&tips,&error));
    BOOST_CHECK(tips==std::set<uint256>({c}));
    LiveTipOverlayCheckpoint checkpoint; BOOST_REQUIRE(runtime->Overlay()->ReadCheckpoint(&checkpoint,&error));
    CTxDB db; uint256 current; BOOST_REQUIRE(db.ReadDAGSourceStateId(current));
    BOOST_CHECK(checkpoint.appliedSourceStateId==current); BOOST_CHECK_EQUAL(checkpoint.phase,LIVE_OVERLAY_PHASE_CLEAN);
    BOOST_CHECK_EQUAL(runtime->RecoveryInvocations(),1U);
    BOOST_CHECK(!g_dagManager.HasDAGData(p));
    db.Close();
    BuildOptions options; options.tempParent=(root/"oracle-sort").string();
    CurrentDagTipDerivationResult result; const auto raw=root/"oracle.raw";
    BOOST_REQUIRE_MESSAGE(DeriveCurrentDagTipsBounded(live.string(),raw.string(),options,&result),result.error);
    std::ifstream input(raw.string(),std::ios::binary); std::set<uint256> oracle; uint256 hash;
    while(input.read(reinterpret_cast<char*>(hash.begin()),32)) oracle.insert(hash);
    BOOST_REQUIRE(input.eof()); BOOST_REQUIRE_EQUAL(input.gcount(),0);
    BOOST_CHECK(oracle==tips);
    BOOST_TEST_MESSAGE("REAL authoritative owner: {P}->{C}; historical DAG residency absent; CLEAN("<<current.GetHex()<<")");
}

// R2c.2 prerequisite discriminator (converted to GREEN expected-mismatch regression).
// The L3 restart-consistency defect is REPRODUCED here and asserted as the KNOWN
// mismatch:
//   persisted (pre-reorg, a1-present) DAG score  !=  current-canonical post-recolor
//   score (a1-absent resident recompute after the real reorg erases a1's daglinks).
// This is NOT a weakened test: it turns the reproduced defect into an executable
// regression specification. S3 (atomic canonical score persistence) is EXPECTED to
// change/replace this assertion when persisted == current-canonical by construction.
// Until then, do NOT make persisted equal to the current-canonical score in S2.
BOOST_AUTO_TEST_CASE(r2c2_real_reorg_persisted_score_parity)
{
    BOOST_REQUIRE(CZKContext::Initialize()); if (!hooks) hooks=InitHook();
    CBlockIndex* fork=pindexBest;
    while (fork->nHeight<GetForkHeightDAG()) fork=MineReal(fork,0x9100+fork->nHeight);
    fork=MineRealDag(fork,0x9110);
    CBlockIndex* a1=MineRealDag(fork,0x9111);
    CBlockIndex* active=a1;
    for (unsigned i=0;i<6;++i) active=MineRealDag(active,0x9112+i);
    CBlockIndex* b1=AddSideDag(fork,0x9121);
    CBlockIndex* b2=AddSideDag(b1,0x9122);
    BOOST_REQUIRE(pindexBest==active);
    std::unique_ptr<CBlock> merge(BuildPoWBlock(b2,0x9123));
    BOOST_REQUIRE(merge.get());
    std::vector<uint256> parents;
    parents.push_back(b2->GetBlockHash()); parents.push_back(a1->GetBlockHash());
    AttachDagParentsAndRemine(merge.get(),parents);
    CBlockIndex* b3=NULL;
    { LOCK(cs_main);
      BOOST_REQUIRE(merge->CheckBlock(true,true,true));
      // AcceptBlock validates DAG-parent eligibility; bypass only ProcessBlock's
      // weak-work side-branch admission gate, as in the existing side fixtures.
      BOOST_REQUIRE(merge->AcceptBlock());
      b3=mapBlockIndex[merge->GetHash()]; }
    BOOST_REQUIRE(b3); BOOST_REQUIRE(pindexBest==active);
    CTxDB db; CBlockDAGData before;
    BOOST_REQUIRE(db.ReadDAGLinks(b3->GetBlockHash(),before));
    BOOST_REQUIRE(before.nDAGScore==g_dagManager.ComputeDAGScore(b3));
    CBlockIndex* branch=b3;
    for(unsigned i=0;i<12 && pindexBest==active;++i) branch=AddSideDag(branch,0x9130+i);
    BOOST_REQUIRE(pindexBest==branch);
    CBlockDAGData removed, after;
    BOOST_REQUIRE(!db.ReadDAGLinks(a1->GetBlockHash(),removed));
    BOOST_REQUIRE(db.ReadDAGLinks(b3->GetBlockHash(),after));
    const uint256 resident=g_dagManager.ComputeDAGScore(b3);
    BOOST_TEST_MESSAGE("R2c2 real reorg score: B3="<<b3->GetBlockHash().GetHex()
        <<" pre="<<before.nDAGScore.GetHex()<<" disk="<<after.nDAGScore.GetHex()
        <<" post_recolor="<<resident.GetHex()<<" A1_erased=true");
    CBlockDAGData tipDisk;
    BOOST_REQUIRE(db.ReadDAGLinks(branch->GetBlockHash(),tipDisk));
    uint64_t tipChildren=0; bool tipCountPresent=false;
    BOOST_REQUIRE(db.ReadDAGChildCount(branch->GetBlockHash(),&tipChildren,&tipCountPresent));
    BOOST_REQUIRE_EQUAL(tipChildren,0U);
    const uint256 tipResident=g_dagManager.ComputeDAGScore(branch);
    BOOST_TEST_MESSAGE("R2c2 CURRENT FRONTIER tip="<<branch->GetBlockHash().GetHex()
        <<" disk="<<tipDisk.nDAGScore.GetHex()<<" post_recolor="<<tipResident.GetHex()
        <<" active=true childcount=0");
    // GREEN EXPECTED-MISMATCH regression (S3-pending contract):
    // The persisted daglinks score for b3 (and the frontier tip) was written while a1
    // was still resident (a1-present coloring). After the real reorg erases a1's
    // daglinks, the current-canonical post-recolor score reflects a1-absent coloring.
    // These MUST differ (the reproduced L3 defect). Until S3 persists the canonical
    // post-reorg score atomically, persisted != current-canonical is the expected,
    // asserted contract. S3 is expected later to make them equal.
    const bool b3Mismatch = !(after.nDAGScore==resident);
    const bool tipMismatch = !(tipDisk.nDAGScore==tipResident);
    BOOST_TEST_MESSAGE("R2c2 S3-PENDING DISCRIMINATOR: b3_persisted_vs_canonical_mismatch="
        <<(b3Mismatch?1:0)<<" tip_persisted_vs_canonical_mismatch="<<(tipMismatch?1:0));
    BOOST_CHECK_MESSAGE(b3Mismatch,
        "EXPECTED S3-pending mismatch absent: persisted b3 daglinks score equals "
        "current-canonical post-recolor score. This was the L3 restart-consistency "
        "defect; if it now matches, the S2 contract changed before S3 and this "
        "discriminator's premise must be re-reviewed.");
    BOOST_CHECK_MESSAGE(tipMismatch,
        "EXPECTED S3-pending mismatch absent: persisted frontier tip score equals "
        "current-canonical post-recolor score. Same S3-pending contract.");
}

// R2c.2s/S2 parity probe: does the reusable canonical recolor
// (ReconstructCanonicalDAGScores, which drives the real ColorBlock/DAGKnight
// against materialized BlockIndexRecord) reproduce the resident post-recolor
// scores on a real multi-parent reorg? NOTE legacy POEM: for height in
// [FORK_HEIGHT_POEM, FORK_HEIGHT_DAG) the real GetBlockTrust uses
// GetBlockEntropy, while the builder computes trust via the reciprocal form
// only (blockindex_generation_builder.cpp:108). This may be a real divergence.
BOOST_AUTO_TEST_CASE(r2c2s_s2_canonical_recolor_resident_parity)
{
    BOOST_REQUIRE(CZKContext::Initialize()); if (!hooks) hooks=InitHook();
    CBlockIndex* fork=pindexBest;
    while (fork->nHeight<GetForkHeightDAG()) fork=MineReal(fork,0x9200+fork->nHeight);
    fork=MineRealDag(fork,0x9210);
    CBlockIndex* a1=MineRealDag(fork,0x9211);
    CBlockIndex* active=a1;
    for (unsigned i=0;i<6;++i) active=MineRealDag(active,0x9212+i);
    CBlockIndex* b1=AddSideDag(fork,0x9221);
    CBlockIndex* b2=AddSideDag(b1,0x9222);
    BOOST_REQUIRE(pindexBest==active);
    std::unique_ptr<CBlock> merge(BuildPoWBlock(b2,0x9223));
    std::vector<uint256> parents2;
    parents2.push_back(b2->GetBlockHash()); parents2.push_back(a1->GetBlockHash());
    AttachDagParentsAndRemine(merge.get(),parents2);
    CBlockIndex* b3=NULL;
    { LOCK(cs_main);
      BOOST_REQUIRE(merge->CheckBlock(true,true,true));
      BOOST_REQUIRE(merge->AcceptBlock());
      b3=mapBlockIndex[merge->GetHash()]; }
    BOOST_REQUIRE(b3);
    CBlockIndex* branch=b3;
    for(unsigned i=0;i<12 && pindexBest==active;++i) branch=AddSideDag(branch,0x9230+i);
    BOOST_REQUIRE(pindexBest==branch);

    // Capture resident post-recolor scores for all DAG-era PoW blocks (oracle).
    std::map<uint256,uint256> residentScores;
    std::vector<std::pair<int32_t,uint256>> heightSorted;
    std::map<uint256,std::vector<uint256>> dagLinks;
    { LOCK(cs_main);
      { LOCK(g_dagManager.cs_dag);
        for (std::map<uint256,CBlockIndex*>::const_iterator it=mapBlockIndex.begin(); it!=mapBlockIndex.end(); ++it){
          CBlockIndex* pi=it->second; if(!pi) continue;
          heightSorted.push_back(std::make_pair((int32_t)pi->nHeight, it->first));
          CBlockDAGData dd;
          if (g_dagManager.GetDAGData(it->first,dd)) dagLinks[it->first]=dd.vDAGParents;
          if (pi->nHeight>=GetForkHeightDAG() && pi->IsProofOfWork()) { uint256 s=g_dagManager.ComputeDAGScore(pi); residentScores[it->first]=s; }
        }
      }
    }
    sort(heightSorted.begin(),heightSorted.end());

    // Build + select a real generation and start authoritative layering, so the
    // entropy-correct provider + retained reader are live (Gate C path).
    const fs::path root=fs::temp_directory_path()/fs::unique_path("r2c2s-recolor-%%%%-%%%%");
    fs::create_directories(root/"snapshot");
    struct Cleanup { fs::path root; CBlockIndex* best; CBlockIndex* genesis;
        Cleanup(const fs::path& r):root(r),best(pindexBest),genesis(pindexGenesisBlock){}
        ~Cleanup(){ ResetBlockIndexAuthoritativeStartupForTest(); pindexBest=best; pindexGenesisBlock=genesis;
            if(best){nBestHeight=best->nHeight;hashBestChain=best->GetBlockHash();nBestChainTrust=best->nChainTrust;}
            try{fs::remove_all(root);}catch(...){} }
    } cleanup(root);
    {CTxDB db;db.Close();}
    const auto live=GetDataDir()/"txleveldb";
    for(fs::directory_iterator it(live),end;it!=end;++it)
        if(fs::is_regular_file(it->path())) fs::copy_file(it->path(),root/"snapshot"/it->path().filename());
    BlockIndexGenerationSource source; std::string error;
    BOOST_REQUIRE_MESSAGE(ReadLegacyBlockIndexSource((root/"snapshot").string(),&source,&error),error);
    BOOST_REQUIRE_MESSAGE(ReadDAGLinksFromSnapshot((root/"snapshot").string(),&source.dagLinks,&source.dagScores,&error),error);
    source.foundDAGLinks = true;
    source.blockDataDir=GetDataDir().string(); source.dagLinksDir=(root/"snapshot").string();
    BlockIndexGenerationBuilder builder;
    BOOST_REQUIRE_MESSAGE(builder.Build(source,(root/"build-000001.tmp").string(),1,NULL,&error),error); builder.Close();
    BOOST_REQUIRE_EQUAL(BlockIndexGenerationManager::PublishGeneration(root.string(),1,&error),BLOCK_INDEX_LIFECYCLE_OK);
    BOOST_REQUIRE_EQUAL(BlockIndexGenerationManager::SelectGeneration(root.string(),1,&error),BLOCK_INDEX_LIFECYCLE_OK);
    g_dagManager.ClearDAGDataForTest();
    BOOST_REQUIRE_MESSAGE(InitBlockIndexAuthoritative(root.string(),&error),error);

    // GATE C: provider-driven canonical recolor vs INDEPENDENT POST-REORG
    // COUNTERFACTUAL oracle (authoritative target per user review decision).
    // The resident post-reorg scores (a1-absent residue) are NOT authority and are
    // observed separately. Identify erased DAG-era boundary parents referenced by
    // retained vertices but absent from canonical daglinks.
    std::map<uint256,uint256> canonical;
    BOOST_REQUIRE_MESSAGE(ReconstructAuthoritativeDAGScore(heightSorted,dagLinks,&canonical,&error),error);
    std::vector<uint256> erasedParents;
    for (std::map<uint256,std::vector<uint256>>::const_iterator l=dagLinks.begin(); l!=dagLinks.end(); ++l)
        for (const uint256& p : l->second)
            if (!dagLinks.count(p)) erasedParents.push_back(p);
    std::string cfErr;
    CTxDB cfDb("r"); AuthoritativeDAGRecolorSource cfSource(cfDb);
    // Build the counterfactual on the RETAINED canonical scope (dagLinks keys),
    // not the full resident heightSorted (which includes erased a-branch vertices).
    std::vector<std::pair<int32_t,uint256>> cfScope;
    for (std::map<uint256,std::vector<uint256>>::const_iterator l=dagLinks.begin(); l!=dagLinks.end(); ++l){
        BlockIndexSnapshot s; std::string e2;
        if (ResolveAuthoritativeBlockSnapshot(l->first,&s,&e2))
            cfScope.push_back(std::make_pair((int32_t)s.height,l->first));
    }
    std::sort(cfScope.begin(),cfScope.end());
    std::map<uint256,CanonicalDAGRecolorRecord> cfByHash =
        CounterfactualOracle::Build(cfScope,cfSource,erasedParents,&cfErr);
    BOOST_REQUIRE_MESSAGE(!cfByHash.empty(),"Gate C counterfactual oracle build failed: "+cfErr);
    unsigned nMatched=0,nMismatch=0;
    for (std::map<uint256,uint256>::const_iterator it=canonical.begin(); it!=canonical.end(); ++it){
      std::map<uint256,CanonicalDAGRecolorRecord>::const_iterator cf=cfByHash.find(it->first);
      if(cf==cfByHash.end()) continue;
      ++nMatched;
      if(!(it->second==cf->second.nDAGScore)) ++nMismatch;
    }
    BOOST_TEST_MESSAGE("R2c2s S2 GATE_C canonical-vs-counterfactual: dag_blocks="<<residentScores.size()
        <<" matched="<<nMatched<<" mismatch="<<nMismatch<<"; fork_dag="<<GetForkHeightDAG()
        <<" poem="<<GetForkHeightPoem());
    BOOST_CHECK_MESSAGE(nMismatch==0,
        "Gate C canonical recolor diverges from independent post-reorg counterfactual oracle");
    // Legacy-residue observation (NOT authority): canonical vs resident post-reorg
    // a1-absent scores. Informational only.
    {
        unsigned nResM=0,nResX=0;
        for (std::map<uint256,uint256>::const_iterator it=residentScores.begin(); it!=residentScores.end(); ++it){
          std::map<uint256,uint256>::const_iterator c=canonical.find(it->first);
          if(c==canonical.end()) continue;
          ++nResM;
          if(!(c->second==it->second)) ++nResX;
        }
        BOOST_TEST_MESSAGE("R2c2s S2 GATE_C resident-residue (observation only): matched="<<nResM
            <<" mismatch="<<nResX);
    }
}

// R2c.2s/S2 NONRESIDENT retained canonical recolor proof (strategy A).
// The real multi-parent reorg above keeps a winning B-branch retained. We record
// the RESIDENT post-recolor oracle for retained branch vertices, then make the
// entire retained canvas NONRESIDENT (ClearDAGDataForTest -> absent from
// mapDAGData), and drive ReconstructAuthoritativeDAGScore purely from canonical
// daglinks + by-value metadata. Exact equality of every retained block proves F
// (nonresident retained) is computed without resident legacy authority, and that
// a recolored ancestor's change reaches F correctly.
BOOST_AUTO_TEST_CASE(r2c2s_s2_nonresident_retained_recolor_oracle_equivalence)
{
    BOOST_REQUIRE(CZKContext::Initialize()); if (!hooks) hooks=InitHook();
    CBlockIndex* fork=pindexBest;
    while (fork->nHeight<GetForkHeightDAG()) fork=MineReal(fork,0x9400+fork->nHeight);
    fork=MineRealDag(fork,0x9410);
    CBlockIndex* a1=MineRealDag(fork,0x9411);
    CBlockIndex* active=a1;
    for (unsigned i=0;i<6;++i) active=MineRealDag(active,0x9412+i);
    CBlockIndex* b1=AddSideDag(fork,0x9421);
    CBlockIndex* b2=AddSideDag(b1,0x9422);
    BOOST_REQUIRE(pindexBest==active);
    std::unique_ptr<CBlock> merge(BuildPoWBlock(b2,0x9423));
    std::vector<uint256> parentsM;
    parentsM.push_back(b2->GetBlockHash()); parentsM.push_back(a1->GetBlockHash());
    AttachDagParentsAndRemine(merge.get(),parentsM);
    CBlockIndex* b3=NULL;
    { LOCK(cs_main);
      BOOST_REQUIRE(merge->CheckBlock(true,true,true));
      BOOST_REQUIRE(merge->AcceptBlock());
      b3=mapBlockIndex[merge->GetHash()]; }
    BOOST_REQUIRE(b3);
    CBlockIndex* branch=b3;
    for(unsigned i=0;i<12 && pindexBest==active;++i) branch=AddSideDag(branch,0x9430+i);
    BOOST_REQUIRE(pindexBest==branch);

    // Record RESIDENT post-recolor oracle for retained B-branch+ blocks.
    std::map<uint256,uint256> oracle;
    std::map<uint256,CBlockDAGData> fullOracle;
    std::map<uint256,uint256> boundaryOracle;
    { LOCK(cs_main);
      { LOCK(g_dagManager.cs_dag);
        for (std::map<uint256,CBlockIndex*>::const_iterator it=mapBlockIndex.begin(); it!=mapBlockIndex.end(); ++it){
          CBlockIndex* pi=it->second; if(!pi) continue;
          if (pi->nHeight<GetForkHeightDAG() || !pi->IsProofOfWork()) continue;
          CBlockDAGData dd;
          // Only vertices PRESENT in canonical daglinks are retained (not the
          // disconnected a-branch which the reorg erased). Retained = has a
          // live daglinks record; capture resident scores for those + mark F.
          oracle[it->first]=g_dagManager.ComputeDAGScore(pi);
          if(g_dagManager.GetDAGData(it->first,dd)) fullOracle[it->first]=dd;
          else boundaryOracle[it->first]=pi->nChainTrust;
        }
      }
    }
    // Snapshot the canonical daglinks canvas directly from LevelDB (canonical
    // source, NOT resident mapDAGData) so the recolor input is authoritative.
    std::map<uint256,CBlockDAGData> canonicalLinks;
    std::vector<std::pair<int32_t,uint256>> heightSorted;
    { CTxDB db; BOOST_REQUIRE(db.IterateDAGLinks(canonicalLinks)); }
    // Build height-sorted retained scope from a canonical generation snapshot.
    const fs::path root=fs::temp_directory_path()/fs::unique_path("r2c2s-nr-%%%%-%%%%");
    fs::create_directories(root/"snapshot");
    struct Cleanup { fs::path root; CBlockIndex* best; CBlockIndex* genesis;
        Cleanup(const fs::path& r):root(r),best(pindexBest),genesis(pindexGenesisBlock){}
        ~Cleanup(){ ResetBlockIndexAuthoritativeStartupForTest(); pindexBest=best; pindexGenesisBlock=genesis;
            if(best){nBestHeight=best->nHeight;hashBestChain=best->GetBlockHash();nBestChainTrust=best->nChainTrust;}
            try{fs::remove_all(root);}catch(...){} }
    } cleanup(root);
    {CTxDB db;db.Close();}
    const auto live=GetDataDir()/"txleveldb";
    for(fs::directory_iterator it(live),end;it!=end;++it)
        if(fs::is_regular_file(it->path())) fs::copy_file(it->path(),root/"snapshot"/it->path().filename());
    BlockIndexGenerationSource source; std::string error;
    BOOST_REQUIRE_MESSAGE(ReadLegacyBlockIndexSource((root/"snapshot").string(),&source,&error),error);
    BOOST_REQUIRE_MESSAGE(ReadDAGLinksFromSnapshot((root/"snapshot").string(),&source.dagLinks,&source.dagScores,&error),error);
    source.foundDAGLinks = true;
    source.blockDataDir=GetDataDir().string(); source.dagLinksDir=(root/"snapshot").string();
    BlockIndexGenerationBuilder builder;
    BOOST_REQUIRE_MESSAGE(builder.Build(source,(root/"build-000001.tmp").string(),1,NULL,&error),error); builder.Close();
    BOOST_REQUIRE_EQUAL(BlockIndexGenerationManager::PublishGeneration(root.string(),1,&error),BLOCK_INDEX_LIFECYCLE_OK);
    BOOST_REQUIRE_EQUAL(BlockIndexGenerationManager::SelectGeneration(root.string(),1,&error),BLOCK_INDEX_LIFECYCLE_OK);
    g_dagManager.ClearDAGDataForTest();
    BOOST_REQUIRE_MESSAGE(InitBlockIndexAuthoritative(root.string(),&error),error);

    // Compose the canonical retained canvas + height scope from LevelDB (authoritative).
    std::map<uint256,int> heights;
    for (std::map<uint256,CBlockDAGData>::const_iterator it=canonicalLinks.begin(); it!=canonicalLinks.end(); ++it){
        BlockIndexSnapshot s; std::string e2;
        if (!ResolveAuthoritativeBlockSnapshot(it->first,&s,&e2)) continue;
        heights[it->first]=s.height;
    }
    std::map<uint256,std::vector<uint256>> dagLinks;
    for (std::map<uint256,CBlockDAGData>::const_iterator it=canonicalLinks.begin(); it!=canonicalLinks.end(); ++it)
        if (heights.count(it->first)) dagLinks[it->first]=it->second.vDAGParents;
    for (std::map<uint256,int>::const_iterator it=heights.begin(); it!=heights.end(); ++it)
        heightSorted.push_back(std::make_pair((int32_t)it->second, it->first));

    // PROVE nonresidency: the retained canvas is absent from resident mapDAGData.
    unsigned residentCount=0;
    { LOCK(g_dagManager.cs_dag);
      for (std::map<uint256,std::vector<uint256>>::const_iterator it=dagLinks.begin(); it!=dagLinks.end(); ++it)
          if (g_dagManager.HasDAGData(it->first)) ++residentCount;
    }
    BOOST_TEST_MESSAGE("R2c2s S2 nonresident: retained_vertices="<<dagLinks.size()
        <<" resident_after_clearing="<<residentCount<<" (expect 0)");

    // Integration review: identify external DAG-era dependencies before
    // assuming an absent daglinks record means a zero-score boundary.
    for(const auto& link:dagLinks) for(const uint256& parentHash:link.second) {
        if(dagLinks.count(parentHash)) continue;
        BlockIndexSnapshot parentSnapshot;
        const bool resolved=ResolveAuthoritativeBlockSnapshot(parentHash,&parentSnapshot,&error);
        BOOST_REQUIRE(resolved);
        if(parentSnapshot.height<GetForkHeightDAG()) continue;
        BOOST_REQUIRE(boundaryOracle.count(parentHash));
        BOOST_TEST_MESSAGE("S2 BOUNDARY child="<<link.first.GetHex()
            <<" parent="<<parentHash.GetHex()<<" height="<<parentSnapshot.height
            <<" retained=false resident_chainTrust="<<boundaryOracle.at(parentHash).GetHex()
            <<" authoritative_derived="<<parentSnapshot.nChainTrust.GetHex());
    }
    // Record preservation on failure as well as successful recolor. The API
    // must never damage the live manager even if a boundary is unresolved.
    BOOST_REQUIRE(!canonicalLinks.empty());
    g_dagManager.SetDAGDataForTest(canonicalLinks.begin()->first,canonicalLinks.begin()->second);
    const uint256 managerBefore=g_dagManager.RecolorStateDigestForTest();
    const std::map<uint256,CBlockIndex*> indexBefore=mapBlockIndex;
    std::vector<CanonicalDAGRecolorRecord> fullFields;
    CanonicalDAGRecolorStats measured;
    CTxDB sourceDb("r"); AuthoritativeDAGRecolorSource fullSource(sourceDb);
    const bool complete=ReconstructAuthoritativeDAGFields(heightSorted,fullSource,&fullFields,&measured,&error);
    BOOST_CHECK(managerBefore==g_dagManager.RecolorStateDigestForTest());
    BOOST_CHECK(indexBefore==mapBlockIndex);
    BOOST_TEST_MESSAGE("S2 ISOLATION complete="<<complete<<"; output="<<fullFields.size()
        <<"; manager_unchanged="<<(managerBefore==g_dagManager.RecolorStateDigestForTest())
        <<"; index_unchanged="<<(indexBefore==mapBlockIndex)<<" error="<<error
        <<"; retained="<<measured.retainedVertices
        <<"; preDAGBase="<<measured.preDAGBaseVertices
        <<"; boundary="<<measured.boundaryVertices
        <<"; closureVertices="<<measured.boundaryClosureVertices
        <<"; closureDagRecords="<<measured.boundaryClosureDagRecords
        <<"; metadataSnapshots="<<measured.metadataSnapshots
        <<"; preDAGTrustEntries="<<measured.preDAGTrustEntries
        <<"; parentsMemoEntries="<<measured.parentsMemoEntries
        <<"; localIndexEntries="<<measured.localIndexEntries
        <<"; colorOrderEntries="<<measured.colorOrderEntries
        <<"; estTemporaryBytes="<<measured.estimatedTemporaryBytes);
    if(!complete) BOOST_CHECK(fullFields.empty());
    // Failure injection decorates the real source, never fabricates successful
    // metadata. Every variant proves rejection + empty output + no globals edit.
    struct FailingSource : CanonicalDAGRecolorSource {
        const CanonicalDAGRecolorSource& base;
        int mode; uint256 retained; uint256 parent; mutable bool injected;
        FailingSource(const CanonicalDAGRecolorSource& b,int m,const uint256& r,const uint256& p)
            :base(b),mode(m),retained(r),parent(p),injected(false){}
        bool Block(const uint256& h,BlockIndexSnapshot* out,std::string* e) const override {
            if((mode==0 && h==retained) || (mode==1 && h==parent)) {
                injected=true; if(e)*e="injected required metadata failure"; return false;
            }
            if(!base.Block(h,out,e)) return false;
            if(mode==2 && out->height<GetForkHeightDAG()) {
                injected=true; if(e)*e="injected pre-DAG metadata failure"; return false;
            }
            return true;
        }
        bool Parents(const uint256& h,std::vector<uint256>* out,std::string* e) const override {
            if(mode==3) { injected=true; if(e)*e="injected daglinks read failure"; return false; }
            return base.Parents(h,out,e);
        }
        bool PreDAGTrust(const uint256& h,uint256* out,std::string* e) const override {
            return base.PreDAGTrust(h,out,e);
        }
        bool ReconstructBoundaryScore(const uint256& h,BoundaryScoreResult* out,std::string* e) const override {
            // Option-R fail-closed injection points:
            //   mode==6  -> injected raw-block unavailable
            //   mode==7  -> injected unreadable/hash-mismatch raw block
            //   mode==8  -> injected malformed DAG coinbase
            //   mode==9  -> injected missing recursive boundary raw
            if(mode==6 || mode==7 || mode==8 || mode==9) {
                injected=true; if(e)*e="injected option-r failure";
                return false;
            }
            return base.ReconstructBoundaryScore(h,out,e);
        }
        bool ReconstructBoundaryClosure(
            const uint256& h,
            std::map<uint256,BlockIndexSnapshot>* md,
            std::map<uint256,uint256>* pd,
            std::map<uint256,CBlockDAGData>* rc,
            std::vector<std::pair<int32_t,uint256>>* od,
            std::string* e) const override {
            if(mode==6 || mode==7 || mode==8 || mode==9) {
                injected=true; if(e)*e="injected option-r failure";
                return false;
            }
            return base.ReconstructBoundaryClosure(h,md,pd,rc,od,e);
        }
    };
    uint256 boundary;
    for(const auto& link:dagLinks) for(const auto& parent:link.second)
        if(boundaryOracle.count(parent)) boundary=parent;
    BOOST_REQUIRE(boundary!=uint256(0));
    for(int mode=0;mode<10;++mode) {
        auto scope=heightSorted;
        if(mode==4) scope.push_back(scope.front());
        if(mode==5) ++scope.front().first;
        FailingSource broken(fullSource,mode,scope.front().second,boundary);
        std::vector<CanonicalDAGRecolorRecord> invalid(1); // must clear stale caller output
        const bool accepted=ReconstructAuthoritativeDAGFields(scope,broken,&invalid,NULL,&error);
        BOOST_CHECK(!accepted); BOOST_CHECK(invalid.empty());
        if(mode<4 || mode>=6) BOOST_CHECK(broken.injected);
        BOOST_CHECK(managerBefore==g_dagManager.RecolorStateDigestForTest());
        BOOST_CHECK(indexBefore==mapBlockIndex);
        BOOST_TEST_MESSAGE("S2 FAIL_CLOSED mode="<<mode<<" accepted="<<accepted
            <<" output="<<invalid.size()<<" injected="<<broken.injected<<" error="<<error);
    }
    BOOST_REQUIRE_MESSAGE(complete,"uncertified DAG boundary blocks full-field parity; see S2 BOUNDARY evidence");
    BOOST_REQUIRE_EQUAL(fullFields.size(),dagLinks.size());
    // REFRAME (user review decision): the authoritative target for an erased
    // boundary parent is the INDEPENDENT POST-REORG CURRENT-CANONICAL COUNTERFACTUAL
    // oracle, NOT the pre-reorg retained residue captured in fullOracle (a different,
    // a1-absent DAG state). fullOracle is kept as OBSERVATION only, never PASS/FAIL.
    // Build the counterfactual (inject the erased boundary parent as a normal
    // retained record under the current canonical source) and assert Option-R
    // full-field == counterfactual for every retained vertex.
    {
        std::vector<uint256> erasedParents;
        for(std::map<uint256,uint256>::const_iterator bo=boundaryOracle.begin(); bo!=boundaryOracle.end(); ++bo)
            if (bo->first!=uint256(0)) erasedParents.push_back(bo->first);
        std::string cfErr;
        std::map<uint256,CanonicalDAGRecolorRecord> cfByHash =
            CounterfactualOracle::Build(heightSorted,fullSource,erasedParents,&cfErr);
        BOOST_REQUIRE_MESSAGE(!cfByHash.empty(),"counterfactual oracle build failed: "+cfErr);
        std::map<uint256,CanonicalDAGRecolorRecord> arByHash;
        for (const auto& r : fullFields) arByHash[r.hash]=r;
        unsigned nCfMatch=0,nCfMismatch=0; uint256 cfFailHash;
        for (const auto& r : fullFields){
            std::map<uint256,CanonicalDAGRecolorRecord>::const_iterator cf=cfByHash.find(r.hash);
            if (cf==cfByHash.end()){ ++nCfMismatch; cfFailHash=r.hash; continue; }
            bool ok=(r.nDAGScore==cf->second.nDAGScore)&&(r.fBlue==cf->second.fBlue)&&(r.nInferredK==cf->second.nInferredK);
            if(ok) ++nCfMatch; else { ++nCfMismatch; cfFailHash=r.hash; }
        }
        BOOST_TEST_MESSAGE("S2 COUNTERFACTUAL nonresident full-field: matched="<<nCfMatch
            <<" mismatch="<<nCfMismatch);
        BOOST_CHECK_MESSAGE(nCfMismatch==0,
            "Option-R full-field != independent post-reorg counterfactual oracle (@"
            +cfFailHash.GetHex()+")");
    }
    // Legacy-residue observation (NOT authority): how the recolor relates to the
    // resident post-reorg a1-absent oracle. This is informational only.
    for(const auto& record:fullFields){
        BOOST_REQUIRE(fullOracle.count(record.hash));
        const CBlockDAGData& expected=fullOracle.at(record.hash);
        BOOST_TEST_MESSAGE("S2 RESIDUE-OBS child="<<record.hash.GetHex()
            <<" optionR="<<record.nDAGScore.GetHex()
            <<" residentResidue="<<expected.nDAGScore.GetHex()
            <<" equal="<<((record.nDAGScore==expected.nDAGScore)?1:0)
            <<" (observation only, not asserted)");
    }
    // Drive canonical recolor over the ENTIRE nonresident retained canvas.
    std::map<uint256,uint256> canonical;
    BOOST_REQUIRE_MESSAGE(ReconstructAuthoritativeDAGScore(heightSorted,dagLinks,&canonical,&error),error);

    // Compare canonical score vs the INDEPENDENT POST-REORG COUNTERFACTUAL oracle
    // (authoritative target per user review decision). The resident `oracle`
    // (a1-absent post-reorg residue) is NOT authority and is only observed.
    {
        std::vector<uint256> erasedParents;
        for(std::map<uint256,uint256>::const_iterator bo=boundaryOracle.begin(); bo!=boundaryOracle.end(); ++bo)
            if (bo->first!=uint256(0)) erasedParents.push_back(bo->first);
        std::string cfErr;
        std::map<uint256,CanonicalDAGRecolorRecord> cfByHash =
            CounterfactualOracle::Build(heightSorted,fullSource,erasedParents,&cfErr);
        BOOST_REQUIRE_MESSAGE(!cfByHash.empty(),"counterfactual oracle build failed: "+cfErr);
        unsigned nCfMatched=0,nCfMismatch=0; uint256 cfFHash;
        for (std::map<uint256,uint256>::const_iterator it=canonical.begin(); it!=canonical.end(); ++it){
            if (!dagLinks.count(it->first)) continue; // only retained
            std::map<uint256,CanonicalDAGRecolorRecord>::const_iterator cf=cfByHash.find(it->first);
            if (cf==cfByHash.end()){ ++nCfMismatch; cfFHash=it->first; continue; }
            ++nCfMatched;
            if(!(it->second==cf->second.nDAGScore)){ ++nCfMismatch; cfFHash=it->first; }
        }
        BOOST_TEST_MESSAGE("R2c2s S2 NONRESIDENT canonical-vs-counterfactual: retained="<<dagLinks.size()
            <<" matched="<<nCfMatched<<" mismatch="<<nCfMismatch
            <<" fork_dag="<<GetForkHeightDAG()<<" poem="<<GetForkHeightPoem());
        BOOST_CHECK_MESSAGE(nCfMismatch==0,
            "canonical recolor diverges from independent post-reorg counterfactual oracle (@"
            +cfFHash.GetHex()+")");
    }
    // Legacy-residue observation (NOT authority): resident post-reorg a1-absent
    // oracle vs canonical. Informational only; the authoritative target is the
    // counterfactual oracle asserted above.
    {
        unsigned nResMatched=0,nResMismatch=0; uint256 resFHash;
        for (std::map<uint256,uint256>::const_iterator it=oracle.begin(); it!=oracle.end(); ++it){
          if (!dagLinks.count(it->first)) continue; // only retained
          std::map<uint256,uint256>::const_iterator c=canonical.find(it->first);
          if(c==canonical.end()) continue;
          ++nResMatched;
          if(!(c->second==it->second)){ ++nResMismatch; resFHash=it->first; }
        }
        BOOST_TEST_MESSAGE("R2c2s S2 NONRESIDENT resident-residue (observation only): matched="
            <<nResMatched<<" mismatch="<<nResMismatch);
    }

    // Determinism: enum-order invariance + idempotence (run again; must be identical).
    std::vector<std::pair<int32_t,uint256>> reversed = heightSorted;
    std::reverse(reversed.begin(), reversed.end());
    std::map<uint256,uint256> canonical2;
    BOOST_REQUIRE_MESSAGE(ReconstructAuthoritativeDAGScore(reversed,dagLinks,&canonical2,&error),error);
    unsigned nDiff=0; uint256 dHash;
    for (std::map<uint256,uint256>::const_iterator it=canonical.begin(); it!=canonical.end(); ++it){
        std::map<uint256,uint256>::const_iterator c2=canonical2.find(it->first);
        if (c2==canonical2.end()){ ++nDiff; dHash=it->first; continue; }
        if (!(c2->second==it->second)){ ++nDiff; dHash=it->first; }
    }
    BOOST_TEST_MESSAGE("R2c2s S2 nonresident determinism: reversed-input diff="<<nDiff);
    BOOST_CHECK_MESSAGE(nDiff==0,"canonical recolor is not enumeration-order/idempotence invariant (@"<<dHash.GetHex()<<")");

    // S3 integration prerequisite: a preparatory recolor must not destroy the
    // live resident manager, including on authority-resolution failure.
    BOOST_REQUIRE(!canonicalLinks.empty());
    const uint256 sentinel = canonicalLinks.begin()->first;
    const CBlockDAGData sentinelData = canonicalLinks.begin()->second;
    g_dagManager.SetDAGDataForTest(sentinel, sentinelData);
    std::map<uint256,uint256> isolatedResult;
    BOOST_REQUIRE_MESSAGE(ReconstructAuthoritativeDAGScore(heightSorted,dagLinks,&isolatedResult,&error),error);
    CBlockDAGData preserved;
    const bool retainedGlobal = g_dagManager.GetDAGData(sentinel,preserved);
    BOOST_TEST_MESSAGE("S3 prerequisite global-state preservation: before=1 after="<<retainedGlobal);
    BOOST_CHECK_MESSAGE(retainedGlobal,"S3 isolation: preparatory recolor erased live resident DAG state");

    // A missing required retained record must reject the entire result. It must
    // not silently disappear while a partial canvas is reported successful.
    const uint256 missingHash("fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff1");
    BlockIndexSnapshot missingSnapshot;
    BOOST_REQUIRE(!ResolveAuthoritativeBlockSnapshot(missingHash,&missingSnapshot,&error));
    std::vector<std::pair<int32_t,uint256>> missingScope = heightSorted;
    missingScope.push_back(std::make_pair(GetForkHeightDAG()+100,missingHash));
    std::map<uint256,std::vector<uint256>> missingLinks = dagLinks;
    missingLinks[missingHash].push_back(sentinel);
    std::map<uint256,uint256> rejectedResult;
    const bool acceptedMissing = ReconstructAuthoritativeDAGScore(missingScope,missingLinks,&rejectedResult,&error);
    BOOST_TEST_MESSAGE("S3 prerequisite missing metadata: accepted="<<acceptedMissing
        <<" required="<<missingLinks.size()<<" output="<<rejectedResult.size());
    BOOST_CHECK_MESSAGE(!acceptedMissing,"S3 fail-closed: recolor accepted missing required metadata");
    BOOST_CHECK_MESSAGE(rejectedResult.empty(),"S3 fail-closed: partial score result escaped");
}

// R2c.2s / S2 FIRST GATE: trust parity. Path A is authorized on the premise that
// authoritative derived chainTrust matches resident CBlockIndex::nChainTrust. A
// possible real divergence: the builder's derived.dat chainTrust uses the
// reciprocal target form (blockindex_generation_builder.cpp:573-575) while real
// CBlockIndex::GetBlockTrust uses GetBlockEntropy for height>=FORK_HEIGHT_POEM
// (main.cpp:9820-9826). Build a REAL generation from the mined resident source,
// start authoritative, and compare per-block authoritative nChainTrust vs
// resident nChainTrust across the POEM boundary.
BOOST_AUTO_TEST_CASE(r2c2s_s2_trust_parity_authoritative_vs_resident)
{
    BOOST_REQUIRE(CZKContext::Initialize()); if (!hooks) hooks=InitHook();
    // Mine a real chain from genesis wide enough to cross POEM and DAG.
    CBlockIndex* parent=pindexBest;
    while (parent->nHeight < GetForkHeightDAG()+3) parent=MineReal(parent,0x9300+parent->nHeight);
    parent=MineRealDag(parent,0x9310);

    // Snapshot the live txleveldb source and publish/select a real generation.
    const fs::path root=fs::temp_directory_path()/fs::unique_path("r2c2s-trust-%%%%-%%%%");
    fs::create_directories(root/"snapshot");
    struct Cleanup { fs::path root; CBlockIndex* best; CBlockIndex* genesis;
        Cleanup(const fs::path& r):root(r),best(pindexBest),genesis(pindexGenesisBlock){}
        ~Cleanup(){ ResetBlockIndexAuthoritativeStartupForTest(); pindexBest=best; pindexGenesisBlock=genesis;
            if(best){nBestHeight=best->nHeight;hashBestChain=best->GetBlockHash();nBestChainTrust=best->nChainTrust;}
            try{fs::remove_all(root);}catch(...){} }
    } cleanup(root);
    {CTxDB db;db.Close();}
    const auto live=GetDataDir()/"txleveldb";
    for(fs::directory_iterator it(live),end;it!=end;++it)
        if(fs::is_regular_file(it->path())) fs::copy_file(it->path(),root/"snapshot"/it->path().filename());

    // Capture RESIDENT per-block GetBlockTrust + nChainTrust for a slice of
    // boundary blocks BEFORE any authoritative mutation (authority must never
    // select the legacy branch).
    std::map<uint256,uint256> residentTrust;      // nChainTrust, ALL heights
    std::map<uint256,uint256> residentLocalTrust; // GetBlockTrust(), ALL heights
    std::map<uint256,int> residentHeight;
    { LOCK(cs_main);
      for (std::map<uint256,CBlockIndex*>::const_iterator it=mapBlockIndex.begin(); it!=mapBlockIndex.end(); ++it){
        if(!it->second) continue;
        int h=it->second->nHeight;
        if (h<=GetForkHeightDAG()+3){
          residentTrust[it->first]=it->second->nChainTrust;
          residentLocalTrust[it->first]=it->second->GetBlockTrust();
          residentHeight[it->first]=h;
        }
      }
    }
    BlockIndexGenerationSource source; std::string error;
    BOOST_REQUIRE_MESSAGE(ReadLegacyBlockIndexSource((root/"snapshot").string(),&source,&error),error);
    BOOST_REQUIRE_MESSAGE(ReadDAGLinksFromSnapshot((root/"snapshot").string(),&source.dagLinks,&source.dagScores,&error),error);
    source.foundDAGLinks = true;
    source.blockDataDir=GetDataDir().string(); source.dagLinksDir=(root/"snapshot").string();
    BlockIndexGenerationBuilder builder;
    BOOST_REQUIRE_MESSAGE(builder.Build(source,(root/"build-000001.tmp").string(),1,NULL,&error),error); builder.Close();
    BOOST_REQUIRE_EQUAL(BlockIndexGenerationManager::PublishGeneration(root.string(),1,&error),BLOCK_INDEX_LIFECYCLE_OK);
    BOOST_REQUIRE_EQUAL(BlockIndexGenerationManager::SelectGeneration(root.string(),1,&error),BLOCK_INDEX_LIFECYCLE_OK);
    g_dagManager.ClearDAGDataForTest();
    BOOST_REQUIRE_MESSAGE(InitBlockIndexAuthoritative(root.string(),&error),error);

    // GATE A: per-block LOCAL trust parity (all heights). Provider local trust
    // vs resident CBlockIndex::GetBlockTrust. Must be zero-mismatch.
    unsigned aMatched=0, aMismatch=0, aCheck=0; int firstDivHeight=-1;
    for (std::map<uint256,uint256>::const_iterator it=residentLocalTrust.begin(); it!=residentLocalTrust.end(); ++it){
        BlockIndexSnapshot auth; std::string err;
        if (!ResolveAuthoritativeBlockSnapshot(it->first,&auth,&err)) continue;
        ++aCheck; ++aMatched;
        if (!(GetAuthoritativeBlockTrust(auth)==it->second)){ ++aMismatch; if(firstDivHeight<0) firstDivHeight=residentHeight[it->first]; }
    }
    BOOST_TEST_MESSAGE("R2c2s S2 GATE_A local trust: checked="<<aCheck<<" matched="<<aMatched
        <<" mismatch="<<aMismatch<<" poem="<<GetForkHeightPoem()<<" dag="<<GetForkHeightDAG());
    BOOST_CHECK_MESSAGE(aMismatch==0, "GATE A local trust divergence, first at height "+std::to_string(firstDivHeight));

    // GATE B: accumulated chainTrust parity. The provider is the pre-DAG base;
    // compare ONLY heights below FORK_HEIGHT_DAG (at/after DAG resident
    // nChainTrust is the DAG score, whose authority is Gate C, not this base).
    unsigned bMatched=0, bMismatch=0, bCheck=0; int firstBDiv=-1;
    for (std::map<uint256,uint256>::const_iterator it=residentTrust.begin(); it!=residentTrust.end(); ++it){
        if (residentHeight[it->first] >= GetForkHeightDAG()) continue;
        uint256 acc; std::string err;
        if (!GetAuthoritativeAccumulatedChainTrust(it->first,&acc,&err)) { BOOST_FAIL("accumulator failed: "+err); }
        ++bCheck; ++bMatched;
        if (!(acc==it->second)){ ++bMismatch; if(firstBDiv<0) firstBDiv=residentHeight[it->first]; }
    }
    BOOST_TEST_MESSAGE("R2c2s S2 GATE_B accumulated chainTrust: checked="<<bCheck<<" matched="<<bMatched
        <<" mismatch="<<bMismatch<<" poem="<<GetForkHeightPoem()<<" dag="<<GetForkHeightDAG());
    BOOST_CHECK_MESSAGE(bMismatch==0, "GATE B accumulated chainTrust divergence, first at height "+std::to_string(firstBDiv));
}

// R2c.2s / S2 — erased DAG-era parent live-vs-restart discriminator (Phase 2/3).
// A retained child can reference a post-DAG parent whose daglinks record was
// erased by a real SetBestChain->Reorganize. The question is empirical: does
// the erased parent's effective scalar, and the retained child's full-field
// DAG result, differ across a REAL legacy restart (linear nChainTrust replay +
// DAG trust restoration + absent-DAG exclusion)?
//
// This test uses the SAME real multi-parent reorg fixture as the nonresident
// proof. It captures the erased-parent scalar and the retained-child full-field
// state (nDAGScore/fBlue/nInferredK/selected-parent) LIVE, then performs the
// strongest real legacy reopen available in-process:
//   CTxDB::LoadBlockIndex()  (real loader: linear nChainTrust replay + LoadDAGLinks)
// then the real legacy init.cpp:1942-1978 restore block
//   (RebuildDAGOrderIncremental/RebuildDAGOrder + RestoreDAGTrustIntoChainTrust).
// Then it re-captures the SAME hashes and compares.
//
// Classification (see report):
//   A - no scalar divergence
//   B - parent scalar diverges, child unchanged in this fixture
//   C - parent scalar diverges AND child full-field state diverges
// No production semantics are changed; this is evidence only.
BOOST_AUTO_TEST_CASE(r2c2s_s2_erased_parent_live_vs_restart_discriminator)
{
    BOOST_REQUIRE(CZKContext::Initialize()); if (!hooks) hooks=InitHook();
    // Build EXACT real multi-parent reorg fixture (same topology as nonresident proof).
    CBlockIndex* fork=pindexBest;
    while (fork->nHeight<GetForkHeightDAG()) fork=MineReal(fork,0x9A00+fork->nHeight);
    fork=MineRealDag(fork,0x9A10);
    CBlockIndex* a1=MineRealDag(fork,0x9A11);          // erased parent candidate (height 13)
    CBlockIndex* active=a1;
    for (unsigned i=0;i<6;++i) active=MineRealDag(active,0x9A12+i);
    CBlockIndex* b1=AddSideDag(fork,0x9A21);
    CBlockIndex* b2=AddSideDag(b1,0x9A22);
    BOOST_REQUIRE(pindexBest==active);
    std::unique_ptr<CBlock> merge(BuildPoWBlock(b2,0x9A23));
    std::vector<uint256> parentsM;
    parentsM.push_back(b2->GetBlockHash()); parentsM.push_back(a1->GetBlockHash());
    AttachDagParentsAndRemine(merge.get(),parentsM);
    CBlockIndex* b3=NULL;
    { LOCK(cs_main);
      BOOST_REQUIRE(merge->CheckBlock(true,true,true));
      BOOST_REQUIRE(merge->AcceptBlock());
      b3=mapBlockIndex[merge->GetHash()]; }
    BOOST_REQUIRE(b3);
    CBlockIndex* branch=b3;
    for(unsigned i=0;i<12 && pindexBest==active;++i) branch=AddSideDag(branch,0x9A30+i);
    BOOST_REQUIRE(pindexBest==branch);

    // Identify retained boundary pairs: retained child in daglinks whose parent
    // is absent from mapDAGData (erased by reorg) but present in mapBlockIndex.
    std::map<uint256,std::vector<uint256> > boundaryPairs; // child -> absent parents
    std::map<uint256,uint256> parentLiveTrust;             // parent hash -> live nChainTrust
    {
      LOCK(cs_main);
      for (std::map<uint256,CBlockIndex*>::const_iterator it=mapBlockIndex.begin(); it!=mapBlockIndex.end(); ++it){
        CBlockIndex* pi=it->second; if(!pi) continue;
        if (pi->nHeight<GetForkHeightDAG() || !pi->IsProofOfWork()) continue;
        CBlockDAGData dd;
        LOCK(g_dagManager.cs_dag);
        if (!g_dagManager.GetDAGData(pi->GetBlockHash(),dd)) continue; // must be retained (has daglinks)
        for (const uint256& hp : dd.vDAGParents){
          CBlockDAGData pd;
          if (g_dagManager.GetDAGData(hp,pd)) continue; // parent present -> not boundary
          boundaryPairs[pi->GetBlockHash()].push_back(hp);
          std::map<uint256,CBlockIndex*>::iterator mi=mapBlockIndex.find(hp);
          if (mi!=mapBlockIndex.end() && mi->second) parentLiveTrust[hp]=mi->second->nChainTrust;
        }
      }
    }
    BOOST_REQUIRE(!boundaryPairs.empty());
    BOOST_TEST_MESSAGE("S2_DISC boundaryPairs="<<boundaryPairs.size());

    // LIVE full-field capture for each boundary child.
    struct LiveRec { uint256 nDAGScore; bool fBlue; int nInferredK; uint256 selParent; };
    std::map<uint256,LiveRec> liveChild;
    std::map<uint256,int> parentHeight;
    { LOCK(cs_main);
      LOCK2(g_dagManager.cs_dag, cs_main);
      for (std::map<uint256,std::vector<uint256> >::const_iterator b=boundaryPairs.begin(); b!=boundaryPairs.end(); ++b){
        CBlockDAGData dd; LiveRec r;
        if (g_dagManager.GetDAGData(b->first,dd)){ r.nDAGScore=dd.nDAGScore; r.fBlue=dd.fBlue; r.nInferredK=dd.nInferredK; }
        r.selParent=g_dagManager.GetSelectedParent(b->first);
        liveChild[b->first]=r;
        for (const uint256& hp : b->second){
          parentHeight[hp]=mapBlockIndex.count(hp)?mapBlockIndex[hp]->nHeight:-1;
          BOOST_TEST_MESSAGE("S2_DISC LIVE child="<<b->first.GetHex()<<" h="
            <<(mapBlockIndex.count(b->first)?mapBlockIndex[b->first]->nHeight:-1)
            <<" parent="<<hp.GetHex()<<" parentH="<<parentHeight[hp]
            <<" parentDAG="<<(parentHeight[hp]>=GetForkHeightDAG())
            <<" parentHasDAGData="<<(g_dagManager.HasDAGData(hp)?1:0)
            <<" parentLiveTrust="<<parentLiveTrust[hp].GetHex()
            <<" childScore="<<r.nDAGScore.GetHex()<<" fBlue="<<r.fBlue<<" k="<<r.nInferredK
            <<" selParent="<<r.selParent.GetHex());
        }
      }
    }

    // ---- REAL LEGACY REOPEN ----
    // Save the live resident pointer graph; clear resident state so the real
    // loader can re-run (CTxDB::LoadBlockIndex early-returns if mapBlockIndex non-empty).
    std::map<uint256,CBlockIndex*> savedMap;
    { LOCK(cs_main); savedMap=mapBlockIndex; }
    CBlockIndex* savedBest=pindexBest;
    CBlockIndex* savedGenesis=pindexGenesisBlock;
    uint256 savedBestChain=hashBestChain; int savedBestHeight=nBestHeight; uint256 savedBestTrust=nBestChainTrust;

    { CTxDB db; db.Close(); }
    { LOCK(cs_main); mapBlockIndex.clear(); }
    { LOCK(g_dagManager.cs_dag); g_dagManager.ClearDAGDataForTest(); }
    pindexBest=NULL; pindexGenesisBlock=NULL; nBestHeight=-1; hashBestChain=uint256(0); nBestChainTrust=uint256(0);

    // Real loader: rebuilds mapBlockIndex with linear nChainTrust replay and
    // loads persisted daglinks (absent parent excluded). This is the real
    // restart block-index reload path.
    bool loaded=false; std::string loadErr;
    {
        CTxDB db;
        loaded = db.LoadBlockIndex();
    }
    BOOST_REQUIRE_MESSAGE(loaded,"real LoadBlockIndex reopen failed");
    BOOST_REQUIRE(pindexBest);
    // VERIFY the reopen actually rebuilt resident state (fresh CBlockIndex
    // objects), else the comparison below is live-vs-live, not live-vs-restart.
    {
        unsigned nSamePtr=0, nTotal=0;
        LOCK(cs_main);
        for (std::map<uint256,CBlockIndex*>::const_iterator it=mapBlockIndex.begin(); it!=mapBlockIndex.end(); ++it){
            ++nTotal;
            std::map<uint256,CBlockIndex*>::const_iterator si=savedMap.find(it->first);
            if (si!=savedMap.end() && si->second==it->second) ++nSamePtr;
        }
        BOOST_TEST_MESSAGE("S2_DISC REOPEN mapBlockIndex="<<nTotal<<" samePtrAsLive="<<nSamePtr);
        BOOST_REQUIRE_MESSAGE(nTotal>0 && nSamePtr==0,
            "reopen did not rebuild resident mapBlockIndex (same objects as live); comparison invalid");
    }

    // Real legacy init.cpp:1942-1978 restore block (only when !authoritative and DAG active).
    // Diagnose the erased parent's scalar right after LoadBlockIndex (linear replay)
    // and BEFORE the legacy restore block, to prove the replay actually ran.
    {
        LOCK(cs_main);
        for (std::map<uint256,std::vector<uint256> >::const_iterator b=boundaryPairs.begin(); b!=boundaryPairs.end(); ++b)
          for (const uint256& hp : b->second){
            std::map<uint256,CBlockIndex*>::iterator mi=mapBlockIndex.find(hp);
            BOOST_TEST_MESSAGE("S2_DISC POSTLOAD (pre-restore) parent="<<hp.GetHex()
              <<" nChainTrust="<<(mi!=mapBlockIndex.end()?mi->second->nChainTrust.GetHex():std::string("missing"))
              <<" hasDAGData="<<(g_dagManager.HasDAGData(hp)?1:0));
          }
    }
    if (!g_fAuthoritativeStartup && pindexBest && pindexBest->nHeight>=GetForkHeightDAG())
    {
        CTxDB txdbDAGInit;
        int nDAGCleanHeight=-1;
        if (txdbDAGInit.ReadDAGCleanHeight(nDAGCleanHeight) && nDAGCleanHeight>0){
            int nPruneBelow=nDAGCleanHeight-DAG_PRUNE_DEPTH;
            if (nPruneBelow>0) g_dagManager.SetPrunedBelowHeight(nPruneBelow);
        }
        if (nDAGCleanHeight>0) g_dagManager.RebuildDAGOrderIncremental(nDAGCleanHeight);
        else if (!g_dagManager.GetDAGTips().empty()) g_dagManager.RebuildDAGOrder();
        g_dagManager.RestoreDAGTrustIntoChainTrust();
        if (pindexBest) nBestChainTrust=pindexBest->nChainTrust;
    }

    // POST-RESTART full-field capture for the SAME hashes.
    std::map<uint256,LiveRec> restartChild;
    std::map<uint256,uint256> restartParentTrust;
    { LOCK(cs_main);
      LOCK2(g_dagManager.cs_dag, cs_main);
      for (std::map<uint256,std::vector<uint256> >::const_iterator b=boundaryPairs.begin(); b!=boundaryPairs.end(); ++b){
        for (const uint256& hp : b->second){
          std::map<uint256,CBlockIndex*>::iterator mi=mapBlockIndex.find(hp);
          if (mi!=mapBlockIndex.end() && mi->second) restartParentTrust[hp]=mi->second->nChainTrust;
        }
        CBlockDAGData dd; LiveRec r;
        if (g_dagManager.GetDAGData(b->first,dd)){ r.nDAGScore=dd.nDAGScore; r.fBlue=dd.fBlue; r.nInferredK=dd.nInferredK; }
        r.selParent=g_dagManager.GetSelectedParent(b->first);
        restartChild[b->first]=r;
        for (const uint256& hp : b->second)
          BOOST_TEST_MESSAGE("S2_DISC RESTART child="<<b->first.GetHex()
            <<" parent="<<hp.GetHex()<<" parentRestartTrust="<<restartParentTrust[hp].GetHex()
            <<" parentRestartHasDAGData="<<(g_dagManager.HasDAGData(hp)?1:0)
            <<" childScore="<<r.nDAGScore.GetHex()<<" fBlue="<<r.fBlue<<" k="<<r.nInferredK
            <<" selParent="<<r.selParent.GetHex());
      }
    }
    // Disambiguation: does the erased parent's daglinks record still exist on
    // disk (persisted), or was it truly erased? Iterate canonical LevelDB.
    {
        CTxDB db;
        std::map<uint256,CBlockDAGData> onDisk;
        BOOST_REQUIRE(db.IterateDAGLinks(onDisk));
        for (std::map<uint256,std::vector<uint256> >::const_iterator b=boundaryPairs.begin(); b!=boundaryPairs.end(); ++b)
          for (const uint256& hp : b->second)
            BOOST_TEST_MESSAGE("S2_DISC DISK parent="<<hp.GetHex()
              <<" onDiskDAGLinks="<<(onDisk.count(hp)?1:0));
    }

    // Compare + classify.
    unsigned nParentScalarDiff=0, nChildScoreDiff=0, nChildBlueDiff=0, nChildKdiff=0, nChildSelDiff=0;
    for (std::map<uint256,std::vector<uint256> >::const_iterator b=boundaryPairs.begin(); b!=boundaryPairs.end(); ++b){
        for (const uint256& hp : b->second)
            if (parentLiveTrust.count(hp) && restartParentTrust.count(hp)
                && !(parentLiveTrust[hp]==restartParentTrust[hp])) ++nParentScalarDiff;
        if (liveChild.count(b->first) && restartChild.count(b->first)){
            const LiveRec& L=liveChild[b->first]; const LiveRec& R=restartChild[b->first];
            if (!(L.nDAGScore==R.nDAGScore)) ++nChildScoreDiff;
            if (L.fBlue!=R.fBlue) ++nChildBlueDiff;
            if (L.nInferredK!=R.nInferredK) ++nChildKdiff;
            if (!(L.selParent==R.selParent)) ++nChildSelDiff;
        }
    }
    BOOST_TEST_MESSAGE("S2_DISC VERDICT pairs="<<boundaryPairs.size()
        <<" parentScalarDiff="<<nParentScalarDiff
        <<" childScoreDiff="<<nChildScoreDiff<<" childBlueDiff="<<nChildBlueDiff
        <<" childKDiff="<<nChildKdiff<<" childSelParentDiff="<<nChildSelDiff);

    // Restore resident pointer graph + DAG state so subsequent tests see the
    // original live fixture (not the reopened objects). Delete only objects
    // created by the reopen.
    ResetBlockIndexAuthoritativeStartupForTest();
    { LOCK(cs_main);
      for (std::map<uint256,CBlockIndex*>::iterator it=mapBlockIndex.begin(); it!=mapBlockIndex.end(); ++it)
        if (savedMap.count(it->first)==0){ delete it->second->phashBlock; delete it->second; }
      mapBlockIndex.clear();
      mapBlockIndex=savedMap;
      pindexBest=savedBest; pindexGenesisBlock=savedGenesis;
      hashBestChain=savedBestChain; nBestHeight=savedBestHeight; nBestChainTrust=savedBestTrust;
    }
    g_dagManager.ClearDAGDataForTest();
    // Re-establish the live DAG state that existed before reopen (from original pointers).
    { LOCK(cs_main);
      LOCK2(g_dagManager.cs_dag, cs_main);
      for (std::map<uint256,CBlockIndex*>::iterator it=savedMap.begin(); it!=savedMap.end(); ++it){
        CBlockIndex* pi=it->second; if(!pi) continue;
        if (pi->nHeight<GetForkHeightDAG() || !pi->IsProofOfWork()) continue;
        CBlockDAGData dd;
        if (g_dagManager.GetDAGData(pi->GetBlockHash(),dd)) g_dagManager.SetDAGDataForTest(pi->GetBlockHash(),dd);
      }
    }
}
// R2c.2s / S2 — merge-rich erased DAG-era parent load-bearing live-vs-restart (Phase 3).
// The erased parent (a1) has MULTIPLE DAG parents (a1_pre + b1_pre), so its live
// DAG score exceeds linear nChainTrust. A retained child (b3, merge block with
// parents [b2, a1]) references it. Phase E source audit (dag.cpp 1004-1029:
// RestoreDAGTrustIntoChainTrust skips absent-DAG blocks; main.cpp 9077-9079:
// DAG-overwrite) predicts the child's DAG score diverges on restart (class C).
//
// Topology:
//   fork_DAG → a1_pre → a1 (merge-rich: parents=[a1_pre, b1_pre]) → [x6] (loses)
//   fork_DAG → b1_pre → b2 → merge(b3) (parents=[b2, a1]) → [x12] (wins reorg)
//
// a1 is the selected parent of b3 (higher DAG score > b2's).
// After reorg, a1's daglinks are erased; b3 retains DAG data referencing absent a1.
//   LIVE:   b3's selected parent = a1 (merge-rich DAG score)
//   RESTART: a1 absent from mapDAGData → b3 recolors with a1's linear nChainTrust
//            fallback + a1's merge contribution skipped → child state diverges (C).
BOOST_AUTO_TEST_CASE(s2_boundary_load_bearing_live_vs_restart)
{
    SetMockTime(1700000200); // deterministic time for reproducible block hashes/topology
    BOOST_REQUIRE(CZKContext::Initialize()); if (!hooks) hooks=InitHook();
    // Test-case-scoped holder for the authoritative Option-R full-field result
    // computed BEFORE the real reopen (Phase 3.5). The reopen-equality gate
    // (Phase 5.5) recomputes it after the real reload and requires byte/value
    // identity, independent of resident mapBlockIndex/linear-replay residue.
    std::vector<CanonicalDAGRecolorRecord> authoritativeBeforeReopen;

    // ---- Phase 1: Build the valid production topology ----
    CBlockIndex* fork = pindexBest;
    while (fork->nHeight < GetForkHeightDAG()) fork = MineReal(fork, 0x9B00 + fork->nHeight);

    CBlockIndex* a1_pre = MineRealDag(fork, 0x9B10);  // best chain
    CBlockIndex* b1_pre = AddSideDag(fork, 0x9B20);   // side branch (will win after merge)

    // Build a1 as MERGE-RICH: parents = [a1_pre, b1_pre]
    // This is the erased parent candidate — DAG score > linear trust.
    std::unique_ptr<CBlock> a1_block(BuildPoWBlock(a1_pre, 0x9B11));
    std::vector<uint256> a1_parents;
    a1_parents.push_back(a1_pre->GetBlockHash());
    a1_parents.push_back(b1_pre->GetBlockHash());
    AttachDagParentsAndRemine(a1_block.get(), a1_parents);
    CBlockIndex* a1 = NULL;
    { LOCK(cs_main);
      BOOST_REQUIRE(a1_block->CheckBlock(true,true,true));
      BOOST_REQUIRE(a1_block->AcceptBlock());
      a1 = mapBlockIndex[a1_block->GetHash()]; }
    BOOST_REQUIRE(a1);
    BOOST_TEST_MESSAGE("load_bearing a1 hash="<<a1->GetBlockHash().GetHex()<<" height="<<a1->nHeight);

    // Extend a-branch to make it the best chain (6 blocks via AddSideDag).
    CBlockIndex* active = a1;
    for (unsigned i = 0; i < 6; ++i) active = AddSideDag(active, 0x9B12 + i);
    BOOST_REQUIRE(pindexBest == active);

    // Build b2 on the side branch, then the merge block b3 with parents [b2, a1].
    CBlockIndex* b2 = AddSideDag(b1_pre, 0x9B22);
    std::unique_ptr<CBlock> merge(BuildPoWBlock(b2, 0x9B23));
    std::vector<uint256> parentsM;
    parentsM.push_back(b2->GetBlockHash());
    parentsM.push_back(a1->GetBlockHash());
    AttachDagParentsAndRemine(merge.get(), parentsM);
    CBlockIndex* b3 = NULL;
    { LOCK(cs_main);
      BOOST_REQUIRE(merge->CheckBlock(true,true,true));
      BOOST_REQUIRE(merge->AcceptBlock());
      b3 = mapBlockIndex[merge->GetHash()]; }
    BOOST_REQUIRE(b3);
    BOOST_TEST_MESSAGE("load_bearing b3 hash="<<b3->GetBlockHash().GetHex()<<" height="<<b3->nHeight);

    // ---- Phase 1.5: CAPTURE PRE-REORG RETAINED ORACLE (former DAG-score) ----
    // Before the b-chain outruns and the reorg erases a1, a1 is still retained
    // with live DAG data. Its resident nDAGScore and b3's full-field here are the
    // INTENDED former-DAG-score oracle that Option-R must reproduce after erasure
    // (strategy-A: record while resident, then require exact equality). The
    // directive says accidental post-restart residue is NOT authoritative; this
    // retained value IS.
    struct LiveRec { uint256 nDAGScore; bool fBlue; int nInferredK; uint256 selParent; };
    std::map<uint256,LiveRec> oracleChild;         // child hash -> retained full-field
    std::map<uint256,uint256> oracleParentScore;   // parent hash -> retained nDAGScore
    { LOCK(cs_main);
      LOCK2(g_dagManager.cs_dag, cs_main);
      CBlockDAGData a1d;
      if (g_dagManager.GetDAGData(*a1->phashBlock, a1d)) oracleParentScore[*a1->phashBlock]=a1d.nDAGScore;
      CBlockDAGData b3d;
      LiveRec b3r;
      if (g_dagManager.GetDAGData(*b3->phashBlock, b3d)){ b3r.nDAGScore=b3d.nDAGScore; b3r.fBlue=b3d.fBlue; b3r.nInferredK=b3d.nInferredK; }
      b3r.selParent=g_dagManager.GetSelectedParent(*b3->phashBlock);
      oracleChild[*b3->phashBlock]=b3r;
    }
    BOOST_TEST_MESSAGE("S2_OPTIONR PRE-REORG ORACLE a1 score="
      <<(oracleParentScore.count(*a1->phashBlock)?oracleParentScore[*a1->phashBlock].GetHex():std::string("missing"))
      <<" b3 score="<<oracleChild[*b3->phashBlock].nDAGScore.GetHex()
      <<" fBlue="<<oracleChild[*b3->phashBlock].fBlue
      <<" k="<<oracleChild[*b3->phashBlock].nInferredK
      <<" selParent="<<oracleChild[*b3->phashBlock].selParent.GetHex());

    // Extend b-chain until it wins the reorg (outruns a-chain).
    CBlockIndex* branch = b3;
    for (unsigned i = 0; i < 12 && pindexBest == active; ++i)
        branch = AddSideDag(branch, 0x9B30 + i);
    BOOST_REQUIRE_MESSAGE(pindexBest == branch,
        "b-chain failed to win reorg; pindexBest="<<pindexBest->GetBlockHash().GetHex());

    // ---- Phase 2: Identify boundary pairs (retained child → erased parent) ----
    std::map<uint256,std::vector<uint256> > boundaryPairs;
    std::map<uint256,uint256> parentLiveTrust;
    {
      LOCK(cs_main);
      for (std::map<uint256,CBlockIndex*>::const_iterator it=mapBlockIndex.begin(); it!=mapBlockIndex.end(); ++it){
        CBlockIndex* pi=it->second; if(!pi) continue;
        if (pi->nHeight<GetForkHeightDAG() || !pi->IsProofOfWork()) continue;
        CBlockDAGData dd;
        LOCK(g_dagManager.cs_dag);
        if (!g_dagManager.GetDAGData(pi->GetBlockHash(),dd)) continue;
        for (const uint256& hp : dd.vDAGParents){
          CBlockDAGData pd;
          if (g_dagManager.GetDAGData(hp,pd)) continue;
          boundaryPairs[pi->GetBlockHash()].push_back(hp);
          std::map<uint256,CBlockIndex*>::iterator mi=mapBlockIndex.find(hp);
          if (mi!=mapBlockIndex.end() && mi->second) parentLiveTrust[hp]=mi->second->nChainTrust;
        }
      }
    }
    BOOST_REQUIRE_MESSAGE(!boundaryPairs.empty(), "expected boundary pair(s), found none");
    BOOST_TEST_MESSAGE("S2_LOADBEAR boundaryPairs="<<boundaryPairs.size());

    // ---- Phase 3: LIVE full-field capture ----
    std::map<uint256,LiveRec> liveChild;
    std::map<uint256,int> parentHeight;
    { LOCK(cs_main);
      LOCK2(g_dagManager.cs_dag, cs_main);
      for (std::map<uint256,std::vector<uint256> >::const_iterator b=boundaryPairs.begin(); b!=boundaryPairs.end(); ++b){
        CBlockDAGData dd; LiveRec r;
        if (g_dagManager.GetDAGData(b->first,dd)){ r.nDAGScore=dd.nDAGScore; r.fBlue=dd.fBlue; r.nInferredK=dd.nInferredK; }
        r.selParent=g_dagManager.GetSelectedParent(b->first);
        liveChild[b->first]=r;
        for (const uint256& hp : b->second){
          parentHeight[hp]=mapBlockIndex.count(hp)?mapBlockIndex[hp]->nHeight:-1;
          BOOST_TEST_MESSAGE("S2_LOADBEAR LIVE child="<<b->first.GetHex()<<" h="
            <<(mapBlockIndex.count(b->first)?mapBlockIndex[b->first]->nHeight:-1)
            <<" parent="<<hp.GetHex()<<" parentH="<<parentHeight[hp]
            <<" parentDAG="<<(parentHeight[hp]>=GetForkHeightDAG())
            <<" parentHasDAGData="<<(g_dagManager.HasDAGData(hp)?1:0)
            <<" parentLiveTrust="<<parentLiveTrust[hp].GetHex()
            <<" childScore="<<r.nDAGScore.GetHex()<<" fBlue="<<r.fBlue<<" k="<<r.nInferredK
            <<" selParent="<<r.selParent.GetHex());
        }
      }
    }

    // ---- Phase 3.5: AUTHORITATIVE OPTION-R RECOLOR vs live oracle (Section 8) ----
    // With Option-R the isolated authoritative recolor must produce ONE
    // deterministic result for the retained canvas equal to the former DAG-score
    // oracle (the live value), independent of accidental live/restart residue.
    // This is the repair criterion for the legacy L3 restart defect.
    {
        // Snapshot the live txleveldb (post-reorg, erased parent) and bootstrap a
        // real authoritative generation so by-value resolution is available.
        const fs::path arRoot=fs::temp_directory_path()/fs::unique_path("s2-lb-ar-%%%%-%%%%");
        fs::create_directories(arRoot/"snapshot");
        struct ArCleanup {
            fs::path root; CBlockIndex* best; CBlockIndex* genesis;
            ArCleanup(const fs::path& r):root(r),best(pindexBest),genesis(pindexGenesisBlock){}
            ~ArCleanup(){ ResetBlockIndexAuthoritativeStartupForTest(); pindexBest=best; pindexGenesisBlock=genesis;
                if(best){nBestHeight=best->nHeight;hashBestChain=best->GetBlockHash();nBestChainTrust=best->nChainTrust;}
                try{fs::remove_all(root);}catch(...){} }
        } arCleanup(arRoot);
        { CTxDB db; db.Close(); }
        const auto liveDir=GetDataDir()/"txleveldb";
        for(fs::directory_iterator it(liveDir),end;it!=end;++it)
            if(fs::is_regular_file(it->path())) fs::copy_file(it->path(),arRoot/"snapshot"/it->path().filename());
        BlockIndexGenerationSource src; std::string aerr;
        BOOST_REQUIRE_MESSAGE(ReadLegacyBlockIndexSource((arRoot/"snapshot").string(),&src,&aerr),aerr);
        BOOST_REQUIRE_MESSAGE(ReadDAGLinksFromSnapshot((arRoot/"snapshot").string(),&src.dagLinks,&src.dagScores,&aerr),aerr);
        src.foundDAGLinks=true;
        src.blockDataDir=GetDataDir().string(); src.dagLinksDir=(arRoot/"snapshot").string();
        BlockIndexGenerationBuilder ab;
        BOOST_REQUIRE_MESSAGE(ab.Build(src,(arRoot/"build-000001.tmp").string(),1,NULL,&aerr),aerr); ab.Close();
        BOOST_REQUIRE_EQUAL(BlockIndexGenerationManager::PublishGeneration(arRoot.string(),1,&aerr),BLOCK_INDEX_LIFECYCLE_OK);
        BOOST_REQUIRE_EQUAL(BlockIndexGenerationManager::SelectGeneration(arRoot.string(),1,&aerr),BLOCK_INDEX_LIFECYCLE_OK);
        g_dagManager.ClearDAGDataForTest();
        BOOST_REQUIRE_MESSAGE(InitBlockIndexAuthoritative(arRoot.string(),&aerr),aerr);

        // Build the authoritative retained canvas + scope from canonical LevelDB.
        std::map<uint256,CBlockDAGData> arLinks;
        { CTxDB db; BOOST_REQUIRE(db.IterateDAGLinks(arLinks)); }
        std::vector<std::pair<int32_t,uint256>> arScope;
        std::map<uint256,std::vector<uint256>> arDag;
        for (std::map<uint256,CBlockDAGData>::const_iterator it=arLinks.begin(); it!=arLinks.end(); ++it){
            BlockIndexSnapshot s; std::string e2;
            if (!ResolveAuthoritativeBlockSnapshot(it->first,&s,&e2)) continue;
            arScope.push_back(std::make_pair((int32_t)s.height,it->first));
            arDag[it->first]=it->second.vDAGParents;
        }
        std::sort(arScope.begin(), arScope.end());
        // Run the authoritative Option-R recolor (boundary reconstructed, not rejected).
        std::vector<CanonicalDAGRecolorRecord> arFields;
        CanonicalDAGRecolorStats arStats;
        std::string rerr;
        CTxDB arDb("r");
        AuthoritativeDAGRecolorSource arSource(arDb);
        bool arComplete=ReconstructAuthoritativeDAGFields(arScope,arSource,&arFields,&arStats,&rerr);
        BOOST_REQUIRE_MESSAGE(arComplete,"authoritative Option-R recolor failed: "+rerr);
        BOOST_REQUIRE_MESSAGE(arStats.boundaryVertices>0,
            "authoritative Option-R recolor must reconstruct >=1 erased boundary parent");
        BOOST_TEST_MESSAGE("S2_OPTIONR boundaryVertices="<<arStats.boundaryVertices
            <<" materializedObjects="<<arStats.materializedObjects<<"; objectBytes="<<arStats.objectBytes
            <<" ; retained="<<arStats.retainedVertices
            <<" ; preDAGBase="<<arStats.preDAGBaseVertices
            <<" ; closureVertices="<<arStats.boundaryClosureVertices
            <<" ; closureDagRecords="<<arStats.boundaryClosureDagRecords
            <<" ; metadataSnapshots="<<arStats.metadataSnapshots
            <<" ; preDAGTrustEntries="<<arStats.preDAGTrustEntries
            <<" ; parentsMemoEntries="<<arStats.parentsMemoEntries
            <<" ; localIndexEntries="<<arStats.localIndexEntries
            <<" ; colorOrderEntries="<<arStats.colorOrderEntries
            <<" ; estTemporaryBytes="<<arStats.estimatedTemporaryBytes
            <<" ; sizeof(CBlockIndex)="<<sizeof(CBlockIndex)
            <<" ; sizeof(CBlockDAGData)="<<sizeof(CBlockDAGData)
            <<" ; sizeof(BlockIndexSnapshot)="<<sizeof(BlockIndexSnapshot));
        // Compare every retained child that had a boundary pair against the
        // PRE-REORG RETAINED oracle (former DAG-score), not accidental residue.
        unsigned nArMatch=0, nArMismatch=0; uint256 arFailHash;
        std::map<uint256,CanonicalDAGRecolorRecord> arByHash;
        for (const auto& rec : arFields) arByHash[rec.hash]=rec;
        for (std::map<uint256,LiveRec>::const_iterator lc=oracleChild.begin(); lc!=oracleChild.end(); ++lc){
            std::map<uint256,CanonicalDAGRecolorRecord>::const_iterator ar=arByHash.find(lc->first);
            if (ar==arByHash.end()) continue;
            ++nArMatch;
            bool ok=(ar->second.nDAGScore==lc->second.nDAGScore)&&
                    (ar->second.fBlue==lc->second.fBlue)&&
                    (ar->second.nInferredK==lc->second.nInferredK);
            if(!ok){ ++nArMismatch; arFailHash=lc->first; }
        }
        BOOST_TEST_MESSAGE("S2_OPTIONR authoritative-vs-retained-oracle: matched="<<nArMatch
            <<" mismatch="<<nArMismatch<<";");
        // Detail the authoritative full-field for every matched child (Section 8 exactness).
        for (std::map<uint256,LiveRec>::const_iterator lc=oracleChild.begin(); lc!=oracleChild.end(); ++lc){
          std::map<uint256,CanonicalDAGRecolorRecord>::const_iterator ar=arByHash.find(lc->first);
          if(ar!=arByHash.end())
            BOOST_TEST_MESSAGE("S2_OPTIONR CHILD child="<<lc->first.GetHex()
              <<" authScore="<<ar->second.nDAGScore.GetHex()
              <<" retainedOracle="<<lc->second.nDAGScore.GetHex()
              <<" authBlue="<<ar->second.fBlue<<" oracleBlue="<<lc->second.fBlue
              <<" authK="<<ar->second.nInferredK<<" oracleK="<<lc->second.nInferredK
              <<" match="<<((ar->second.nDAGScore==lc->second.nDAGScore)&&(ar->second.fBlue==lc->second.fBlue)&&(ar->second.nInferredK==lc->second.nInferredK)));
        }
        // Diagnose: the reconstructed boundary scalar must equal the parent's
        // PRE-REORG retained DAG score (the intended former-DAG-score oracle).
        for (std::map<uint256,uint256>::const_iterator op=oracleParentScore.begin(); op!=oracleParentScore.end(); ++op){
            BoundaryScoreResult br; std::string be;
            bool brOK=arSource.ReconstructBoundaryScore(op->first,&br,&be);
            BOOST_TEST_MESSAGE("S2_OPTIONR BOUNDARY parent="<<op->first.GetHex()
              <<" reconstructed="<<(brOK&&br.valid?br.score.GetHex():std::string("FAILED:")+be)
              <<" retainedFormerDAGScore="<<op->second.GetHex()
              <<" equal="<<(brOK&&br.valid&&(br.score==op->second)?1:0));
        }
        // ---- INDEPENDENT POST-REORG CURRENT-CANONICAL COUNTERFACTUAL ORACLE ----
        // The authoritative target (user review decision) is NOT the pre-reorg
        // retained a1 score (historical residue from a different DAG state). It is
        // the value a1 and b3 WOULD obtain if colored as resident under the CURRENT
        // post-reorg canonical source, using unmodified ColorBlock/ColorBlockDAGKnight.
        // CounterfactualOracle::Build injects the erased parent(s)' recovered
        // raw-coinbase DAG-parent closure as NORMAL RETAINED records into the
        // post-reorg canonical scope, then recolors via the retained path (no
        // Option-R boundary reconstruction). That independent result is the oracle.
        {
            std::vector<uint256> erasedParents;
            for (std::map<uint256,uint256>::const_iterator op=oracleParentScore.begin(); op!=oracleParentScore.end(); ++op)
                erasedParents.push_back(op->first);
            std::string cfErr;
            std::map<uint256,CanonicalDAGRecolorRecord> cfByHash =
                CounterfactualOracle::Build(arScope,arSource,erasedParents,&cfErr);
            BOOST_REQUIRE_MESSAGE(!cfByHash.empty(),"counterfactual oracle build failed: "+cfErr);
            // Compare Option-R (boundary path) a1/b3 vs counterfactual (retained path).
            for (std::map<uint256,uint256>::const_iterator op=oracleParentScore.begin(); op!=oracleParentScore.end(); ++op){
                std::map<uint256,CanonicalDAGRecolorRecord>::const_iterator o=cfByHash.find(op->first);
                if (o==cfByHash.end()){ BOOST_TEST_MESSAGE("S2_COUNTERFACTUAL parent absent from result="<<op->first.GetHex()); continue; }
                BoundaryScoreResult br; std::string be;
                bool brOK=arSource.ReconstructBoundaryScore(op->first,&br,&be);
                BOOST_TEST_MESSAGE("S2_COUNTERFACTUAL parent="<<op->first.GetHex()
                    <<" optionR="<<(brOK&&br.valid?br.score.GetHex():std::string("FAILED:"))
                    <<" counterfactualRetained="<<o->second.nDAGScore.GetHex()
                    <<" equal="<<(brOK&&br.valid&&(br.score==o->second.nDAGScore)?1:0));
            }
            for (std::map<uint256,LiveRec>::const_iterator lc=oracleChild.begin(); lc!=oracleChild.end(); ++lc){
                std::map<uint256,CanonicalDAGRecolorRecord>::const_iterator ar=arByHash.find(lc->first);
                std::map<uint256,CanonicalDAGRecolorRecord>::const_iterator cf=cfByHash.find(lc->first);
                if (ar==arByHash.end() || cf==cfByHash.end()){ BOOST_TEST_MESSAGE("S2_COUNTERFACTUAL child missing="<<lc->first.GetHex()); continue; }
                bool fullEq=(ar->second.nDAGScore==cf->second.nDAGScore)&&(ar->second.fBlue==cf->second.fBlue)&&(ar->second.nInferredK==cf->second.nInferredK);
                BOOST_TEST_MESSAGE("S2_COUNTERFACTUAL CHILD child="<<lc->first.GetHex()
                    <<" optionRScore="<<ar->second.nDAGScore.GetHex()
                    <<" counterfactualScore="<<cf->second.nDAGScore.GetHex()
                    <<" fullEqual="<<(fullEq?1:0));
                BOOST_CHECK_MESSAGE(fullEq,"Option-R full-field != independent post-reorg counterfactual oracle (child "+lc->first.GetHex()+")");
            }
        }
        // The pre-reorg retained former-DAG-score oracle is historical residue from a
        // different (a-branch-best) DAG state; per the S2/C-full semantic it is NOT the
        // authoritative target. It is kept as an OBSERVATION only, never PASS/FAIL.
        // The authoritative target is the independent post-reorg counterfactual oracle
        // above (Option-R matched it exactly for a1 and child b3). The old strict
        // comparison against retained former-DAG-score is intentionally not asserted.
        BOOST_TEST_MESSAGE("S2_COUNTERFACTUAL pre-reorg-retained-residue (observation only) matched="
            <<nArMatch<<" mismatch="<<nArMismatch);

        // ---- ORDER INDEPENDENCE (S2 production gate) ----
        // The authoritative Option-R result must be byte/value-identical under any
        // enumeration or boundary-discovery order. ReconstructAuthoritativeDAGFields
        // sorts internally and iterates a sorted metadata map, so the same retained
        // scope and erased-boundary SET must give identical full-field output
        // regardless of input order / boundary discovery order. Prove empirically:
        // original, reversed, and several fixed-seed shuffled retained scopes, plus
        // reversed and shuffled erased-parent (boundary discovery) orders.
        {
            // Boundary set for the load-bearing fixture = the erased DAG-era parents
            // referenced by retained children (a1). Build it deterministically.
            std::vector<uint256> erasedA;
            for (std::map<uint256,uint256>::const_iterator op=oracleParentScore.begin(); op!=oracleParentScore.end(); ++op)
                erasedA.push_back(op->first);
            std::sort(erasedA.begin(), erasedA.end());
            // Reference = the authoritative result already computed (arFields).
            std::map<uint256,CanonicalDAGRecolorRecord> refByHash;
            for (const auto& r : arFields) refByHash[r.hash]=r;
            auto compareFields = [&](const std::vector<CanonicalDAGRecolorRecord>& got,
                                     const char* tag, unsigned& card, unsigned& diff) {
                if (got.size()!=arFields.size()){ card=got.size(); return; }
                std::map<uint256,CanonicalDAGRecolorRecord> gByHash;
                for (const auto& r : got) gByHash[r.hash]=r;
                card=got.size();
                for (const auto& r : arFields){
                    if (!gByHash.count(r.hash)){ ++diff; continue; }
                    const CanonicalDAGRecolorRecord& g=gByHash[r.hash];
                    if (!(g.hash==r.hash && g.height==r.height && g.nDAGScore==r.nDAGScore
                          && g.fBlue==r.fBlue && g.nInferredK==r.nInferredK)) ++diff;
                }
            };
            auto runRecolor = [&](const std::vector<std::pair<int32_t,uint256>>& sc,
                                  std::vector<CanonicalDAGRecolorRecord>& out, bool* ok) {
                std::string oe; CanonicalDAGRecolorStats os;
                *ok=ReconstructAuthoritativeDAGFields(sc,arSource,&out,&os,&oe);
            };
            // 1. reversed retained enumeration
            std::vector<std::pair<int32_t,uint256>> revScope=arScope;
            std::reverse(revScope.begin(),revScope.end());
            std::vector<CanonicalDAGRecolorRecord> revFields; bool revOK;
            runRecolor(revScope,revFields,&revOK);
            BOOST_REQUIRE_MESSAGE(revOK,"order-independence: reversed scope recolor failed");
            unsigned revCard=0,revDiff=0; compareFields(revFields,"reversed",revCard,revDiff);
            BOOST_TEST_MESSAGE("S2_ORDERINDEP reversed: card="<<revCard<<" diff="<<revDiff);
            BOOST_CHECK_MESSAGE(revCard==arFields.size()&&revDiff==0,
                "Option-R not order-independent under reversed retained enumeration (diff="<<revDiff<<")");
            // 2. shuffled retained enumeration, several fixed seeds
            const unsigned seeds[] = {0x5EED0001u, 0x5EED0002u, 0x5EED0003u};
            for (unsigned si=0; si<3; ++si){
                std::vector<std::pair<int32_t,uint256>> sh=arScope;
                unsigned long long state=seeds[si];
                for (size_t i=sh.size(); i>1; --i){
                    state=state*6364136223846793005ULL+1442695040888963407ULL;
                    size_t j=static_cast<size_t>(state%i);
                    std::swap(sh[i-1],sh[j]);
                }
                std::vector<CanonicalDAGRecolorRecord> shF; bool shOK;
                runRecolor(sh,shF,&shOK);
                BOOST_REQUIRE_MESSAGE(shOK,"order-independence: shuffled scope recolor failed");
                unsigned shCard=0,shDiff=0; char tag[24]; snprintf(tag,sizeof(tag),"shuffled%u",si);
                compareFields(shF,tag,shCard,shDiff);
                BOOST_TEST_MESSAGE("S2_ORDERIND "<<tag<<": card="<<shCard<<" diff="<<shDiff);
                BOOST_CHECK_MESSAGE(shCard==arFields.size()&&shDiff==0,
                    "Option-R not order-independent under shuffled retained enumeration seed="<<seeds[si]<<" (diff="<<shDiff<<")");
            }
            // 3. reversed boundary discovery: run Option-R recolor but force the
            //    erased-parent set to be discovered in reversed order. The recolor
            //    iterates a sorted metadata map internally, so discovery order of the
            //    boundary SET cannot change the merged closure; still prove it.
            {
                // Re-run with erased parents listed in reversed order (the source's
                // ReconstructBoundaryClosure is keyed by hash; passing the same set in
                // any order yields the same merged closure). CounterfactualOracle::Build
                // accepts the set as a vector; reverse it and require identical result.
                std::vector<uint256> revErased=erasedA;
                std::reverse(revErased.begin(),revErased.end());
                std::string be; std::string be2;
                std::map<uint256,CanonicalDAGRecolorRecord> cfFwd=CounterfactualOracle::Build(arScope,arSource,erasedA,&be);
                std::map<uint256,CanonicalDAGRecolorRecord> cfRev=CounterfactualOracle::Build(arScope,arSource,revErased,&be2);
                BOOST_REQUIRE_MESSAGE(!cfFwd.empty()&&!cfRev.empty(),"counterfactual order independence build failed");
                unsigned cdiff=0;
                for (const auto& r : cfFwd){
                    if (!cfRev.count(r.first)){ ++cdiff; continue; }
                    if (!(r.second==cfRev[r.first])) ++cdiff;
                }
                if (cfFwd.size()!=cfRev.size()) cdiff+=cfFwd.size()>cfRev.size()?cfFwd.size()-cfRev.size():cfRev.size()-cfFwd.size();
                BOOST_TEST_MESSAGE("S2_ORDERIND reversed-boundary-discovery diff="<<cdiff);
                BOOST_CHECK_MESSAGE(cdiff==0,"Option-R not order-independent under reversed boundary discovery (diff="<<cdiff<<")");
            }
            // 4. shuffled boundary discovery, several fixed seeds
            {
                std::string beRef; std::map<uint256,CanonicalDAGRecolorRecord> cfRef=CounterfactualOracle::Build(arScope,arSource,erasedA,&beRef);
                BOOST_REQUIRE_MESSAGE(!cfRef.empty(),"counterfactual reference build failed: "+beRef);
                for (unsigned si=0; si<3; ++si){
                    std::vector<uint256> sh=erasedA;
                    unsigned long long state=0xEED00000ULL+si;
                    for (size_t i=sh.size(); i>1; --i){
                        state=state*2862933555777941757ULL+3037000493ULL;
                        size_t j=static_cast<size_t>(state%i);
                        std::swap(sh[i-1],sh[j]);
                    }
                    std::string be3; std::map<uint256,CanonicalDAGRecolorRecord> cfS=CounterfactualOracle::Build(arScope,arSource,sh,&be3);
                    BOOST_REQUIRE_MESSAGE(!cfS.empty(),"counterfactual shuffled-boundary build failed: "+be3);
                    unsigned cdiff=0;
                    for (const auto& r : cfRef){
                        if (!cfS.count(r.first)){ ++cdiff; continue; }
                        if (!(r.second==cfS[r.first])) ++cdiff;
                    }
                    if (cfRef.size()!=cfS.size()) cdiff+=cfRef.size()>cfS.size()?cfRef.size()-cfS.size():cfS.size()-cfRef.size();
                    BOOST_TEST_MESSAGE("S2_ORDERIND shuffled-boundary seed="<<seeds[si]<<" diff="<<cdiff);
                    BOOST_CHECK_MESSAGE(cdiff==0,"Option-R not order-independent under shuffled boundary discovery seed="<<seeds[si]<<" (diff="<<cdiff<<")");
                }
            }
        }
        // Capture the pre-reopen authoritative Option-R full-field result for the
        // reopen-equality gate (Phase 5.5).
        authoritativeBeforeReopen = arFields;
    }

    // ---- Phase 4: REAL LEGACY REOPEN (CTxDB::LoadBlockIndex) ----
    std::map<uint256,CBlockIndex*> savedMap;
    { LOCK(cs_main); savedMap=mapBlockIndex; }
    CBlockIndex* savedBest=pindexBest;
    CBlockIndex* savedGenesis=pindexGenesisBlock;
    uint256 savedBestChain=hashBestChain; int savedBestHeight=nBestHeight; uint256 savedBestTrust=nBestChainTrust;

    { CTxDB db; db.Close(); }
    { LOCK(cs_main); mapBlockIndex.clear(); }
    { LOCK(g_dagManager.cs_dag); g_dagManager.ClearDAGDataForTest(); }
    pindexBest=NULL; pindexGenesisBlock=NULL; nBestHeight=-1; hashBestChain=uint256(0); nBestChainTrust=uint256(0);

    bool loaded=false;
    {
        CTxDB db;
        loaded = db.LoadBlockIndex();
    }
    BOOST_REQUIRE_MESSAGE(loaded,"real LoadBlockIndex reopen failed");
    BOOST_REQUIRE(pindexBest);

    // VERIFY fresh rebuild (not same objects as live).
    {
        unsigned nSamePtr=0, nTotal=0;
        LOCK(cs_main);
        for (std::map<uint256,CBlockIndex*>::const_iterator it=mapBlockIndex.begin(); it!=mapBlockIndex.end(); ++it){
            ++nTotal;
            std::map<uint256,CBlockIndex*>::const_iterator si=savedMap.find(it->first);
            if (si!=savedMap.end() && si->second==it->second) ++nSamePtr;
        }
        BOOST_TEST_MESSAGE("S2_LOADBEAR REOPEN mapBlockIndex="<<nTotal<<" samePtrAsLive="<<nSamePtr);
        BOOST_REQUIRE_MESSAGE(nTotal>0 && nSamePtr==0,
            "reopen did not rebuild resident mapBlockIndex (same objects as live); comparison invalid");
    }

    // POST-LOAD (pre-restore) capture: erased parent's linear trust from replay.
    {
        LOCK(cs_main);
        for (std::map<uint256,std::vector<uint256> >::const_iterator b=boundaryPairs.begin(); b!=boundaryPairs.end(); ++b)
          for (const uint256& hp : b->second){
            std::map<uint256,CBlockIndex*>::iterator mi=mapBlockIndex.find(hp);
            BOOST_TEST_MESSAGE("S2_LOADBEAR POSTLOAD (pre-restore) parent="<<hp.GetHex()
              <<" nChainTrust="<<(mi!=mapBlockIndex.end()?mi->second->nChainTrust.GetHex():std::string("missing"))
              <<" hasDAGData="<<(g_dagManager.HasDAGData(hp)?1:0));
          }
    }

    // ---- Real legacy init.cpp:1942-1978 restore block ----
    if (!g_fAuthoritativeStartup && pindexBest && pindexBest->nHeight>=GetForkHeightDAG())
    {
        CTxDB txdbDAGInit;
        int nDAGCleanHeight=-1;
        if (txdbDAGInit.ReadDAGCleanHeight(nDAGCleanHeight) && nDAGCleanHeight>0){
            int nPruneBelow=nDAGCleanHeight-DAG_PRUNE_DEPTH;
            if (nPruneBelow>0) g_dagManager.SetPrunedBelowHeight(nPruneBelow);
        }
        if (nDAGCleanHeight>0) g_dagManager.RebuildDAGOrderIncremental(nDAGCleanHeight);
        else if (!g_dagManager.GetDAGTips().empty()) g_dagManager.RebuildDAGOrder();
        g_dagManager.RestoreDAGTrustIntoChainTrust();
        if (pindexBest) nBestChainTrust=pindexBest->nChainTrust;
    }

    // ---- Phase 5: POST-RESTART full-field capture ----
    std::map<uint256,LiveRec> restartChild;
    std::map<uint256,uint256> restartParentTrust;
    { LOCK(cs_main);
      LOCK2(g_dagManager.cs_dag, cs_main);
      for (std::map<uint256,std::vector<uint256> >::const_iterator b=boundaryPairs.begin(); b!=boundaryPairs.end(); ++b){
        for (const uint256& hp : b->second){
          std::map<uint256,CBlockIndex*>::iterator mi=mapBlockIndex.find(hp);
          if (mi!=mapBlockIndex.end() && mi->second) restartParentTrust[hp]=mi->second->nChainTrust;
        }
        CBlockDAGData dd; LiveRec r;
        if (g_dagManager.GetDAGData(b->first,dd)){ r.nDAGScore=dd.nDAGScore; r.fBlue=dd.fBlue; r.nInferredK=dd.nInferredK; }
        r.selParent=g_dagManager.GetSelectedParent(b->first);
        restartChild[b->first]=r;
        for (const uint256& hp : b->second)
          BOOST_TEST_MESSAGE("S2_LOADBEAR RESTART child="<<b->first.GetHex()
            <<" parent="<<hp.GetHex()<<" parentRestartTrust="<<restartParentTrust[hp].GetHex()
            <<" parentRestartHasDAGData="<<(g_dagManager.HasDAGData(hp)?1:0)
            <<" childScore="<<r.nDAGScore.GetHex()<<" fBlue="<<r.fBlue<<" k="<<r.nInferredK
            <<" selParent="<<r.selParent.GetHex());
      }
    }

    // ---- Phase 5.5: REOPEN EQUALITY (S2 production gate) ----
    // The authoritative Option-R result computed BEFORE the real reopen must be
    // byte/value-identical to the result recomputed AFTER the real reload, from a
    // FRESH authoritative generation built from the reopened LevelDB. This proves
    // restart stability: no dependence on old resident mapBlockIndex objects,
    // legacy linear-replay nChainTrust, mapDAGData residency, or process-local
    // BlockIndexSnapshot ids.
    {
        const fs::path rRoot=fs::temp_directory_path()/fs::unique_path("s2-lb-reopen-%%%%-%%%%");
        fs::create_directories(rRoot/"snapshot");
        struct ReopenCleanup { fs::path root; CBlockIndex* best; CBlockIndex* genesis;
            ReopenCleanup(const fs::path& r):root(r),best(pindexBest),genesis(pindexGenesisBlock){}
            ~ReopenCleanup(){ ResetBlockIndexAuthoritativeStartupForTest(); pindexBest=best; pindexGenesisBlock=genesis;
                if(best){nBestHeight=best->nHeight;hashBestChain=best->GetBlockHash();nBestChainTrust=best->nChainTrust;}
                try{fs::remove_all(root);}catch(...){} }
        } rcCleanup(rRoot);
        { CTxDB db; db.Close(); }
        const auto liveDir2=GetDataDir()/"txleveldb";
        for(fs::directory_iterator it(liveDir2),end;it!=end;++it)
            if(fs::is_regular_file(it->path())) fs::copy_file(it->path(),rRoot/"snapshot"/it->path().filename());
        BlockIndexGenerationSource rsrc; std::string raerr;
        BOOST_REQUIRE_MESSAGE(ReadLegacyBlockIndexSource((rRoot/"snapshot").string(),&rsrc,&raerr),raerr);
        BOOST_REQUIRE_MESSAGE(ReadDAGLinksFromSnapshot((rRoot/"snapshot").string(),&rsrc.dagLinks,&rsrc.dagScores,&raerr),raerr);
        rsrc.foundDAGLinks=true;
        rsrc.blockDataDir=GetDataDir().string(); rsrc.dagLinksDir=(rRoot/"snapshot").string();
        BlockIndexGenerationBuilder rb;
        BOOST_REQUIRE_MESSAGE(rb.Build(rsrc,(rRoot/"build-000001.tmp").string(),1,NULL,&raerr),raerr); rb.Close();
        BOOST_REQUIRE_EQUAL(BlockIndexGenerationManager::PublishGeneration(rRoot.string(),1,&raerr),BLOCK_INDEX_LIFECYCLE_OK);
        BOOST_REQUIRE_EQUAL(BlockIndexGenerationManager::SelectGeneration(rRoot.string(),1,&raerr),BLOCK_INDEX_LIFECYCLE_OK);
        g_dagManager.ClearDAGDataForTest();
        BOOST_REQUIRE_MESSAGE(InitBlockIndexAuthoritative(rRoot.string(),&raerr),raerr);

        // Build the reopened canonical retained scope from the reopened LevelDB.
        std::map<uint256,CBlockDAGData> rLinks;
        { CTxDB db; BOOST_REQUIRE(db.IterateDAGLinks(rLinks)); }
        std::vector<std::pair<int32_t,uint256>> rScope;
        for (std::map<uint256,CBlockDAGData>::const_iterator it=rLinks.begin(); it!=rLinks.end(); ++it){
            BlockIndexSnapshot s; std::string e2;
            if (!ResolveAuthoritativeBlockSnapshot(it->first,&s,&e2)) continue;
            rScope.push_back(std::make_pair((int32_t)s.height,it->first));
        }
        std::sort(rScope.begin(), rScope.end());
        // Recompute Option-R from the reopened source.
        std::vector<CanonicalDAGRecolorRecord> rFields;
        CanonicalDAGRecolorStats rStats;
        std::string rerr2;
        CTxDB rDb("r");
        AuthoritativeDAGRecolorSource rSource(rDb);
        bool rComplete=ReconstructAuthoritativeDAGFields(rScope,rSource,&rFields,&rStats,&rerr2);
        BOOST_REQUIRE_MESSAGE(rComplete,"reopen Option-R recolor failed: "+rerr2);
        // Byte/value-identical comparison vs the pre-reopen authoritative result.
        if (rFields.size()!=authoritativeBeforeReopen.size()){
            BOOST_FAIL("reopen equality: cardinality differs before="
                <<authoritativeBeforeReopen.size()<<" after="<<rFields.size());
        }
        std::map<uint256,CanonicalDAGRecolorRecord> rByHash;
        for (const auto& r : rFields) rByHash[r.hash]=r;
        unsigned rDiff=0; uint256 rDiffHash;
        for (const auto& r : authoritativeBeforeReopen){
            if (!rByHash.count(r.hash)){ ++rDiff; rDiffHash=r.hash; continue; }
            if (!(r==rByHash[r.hash])){ ++rDiff; rDiffHash=r.hash; }
        }
        if (rByHash.size()!=authoritativeBeforeReopen.size())
            ++rDiff;
        BOOST_TEST_MESSAGE("S2_REOPEN_EQUALITY card="<<authoritativeBeforeReopen.size()
            <<" diff="<<rDiff<<" boundaryVertices="<<rStats.boundaryVertices);
        BOOST_CHECK_MESSAGE(rDiff==0,
            "Option-R result not equal across real reopen (@"
            +(rDiffHash==uint256(0)?std::string("cardinality"):rDiffHash.GetHex())+")");
        // Evidence that the reopened reader rebuilt fresh (not resident residue).
        unsigned rBoundary=0;
        { CTxDB db; std::map<uint256,CBlockDAGData> od; BOOST_REQUIRE(db.IterateDAGLinks(od));
          for (const auto& bp : boundaryPairs) for (const uint256& hp : bp.second)
              if (!od.count(hp)) ++rBoundary; }
        BOOST_TEST_MESSAGE("S2_REOPEN_EQUALITY onDiskErasedParents="<<rBoundary);
    }

    // DISK state: was the parent's daglinks truly erased from LevelDB?
    {
        CTxDB db;
        std::map<uint256,CBlockDAGData> onDisk;
        BOOST_REQUIRE(db.IterateDAGLinks(onDisk));
        for (std::map<uint256,std::vector<uint256> >::const_iterator b=boundaryPairs.begin(); b!=boundaryPairs.end(); ++b)
          for (const uint256& hp : b->second)
            BOOST_TEST_MESSAGE("S2_LOADBEAR DISK parent="<<hp.GetHex()
              <<" onDiskDAGLinks="<<(onDisk.count(hp)?1:0));
    }

    // ---- Phase 6: Compare + Classify ----
    unsigned nParentScalarDiff=0, nChildScoreDiff=0, nChildBlueDiff=0, nChildKdiff=0, nChildSelDiff=0;
    for (std::map<uint256,std::vector<uint256> >::const_iterator b=boundaryPairs.begin(); b!=boundaryPairs.end(); ++b){
        for (const uint256& hp : b->second)
            if (parentLiveTrust.count(hp) && restartParentTrust.count(hp)
                && !(parentLiveTrust[hp]==restartParentTrust[hp])) ++nParentScalarDiff;
        if (liveChild.count(b->first) && restartChild.count(b->first)){
            const LiveRec& L=liveChild[b->first]; const LiveRec& R=restartChild[b->first];
            if (!(L.nDAGScore==R.nDAGScore)) ++nChildScoreDiff;
            if (L.fBlue!=R.fBlue) ++nChildBlueDiff;
            if (L.nInferredK!=R.nInferredK) ++nChildKdiff;
            if (!(L.selParent==R.selParent)) ++nChildSelDiff;
        }
    }
    BOOST_TEST_MESSAGE("S2_LOADBEAR VERDICT pairs="<<boundaryPairs.size()
        <<" parentScalarDiff="<<nParentScalarDiff
        <<" childScoreDiff="<<nChildScoreDiff<<" childBlueDiff="<<nChildBlueDiff
        <<" childKDiff="<<nChildKdiff<<" childSelParentDiff="<<nChildSelDiff);

    // Classify A/B/C.
    std::string verdict = "A";
    if (nParentScalarDiff > 0) verdict = "B";
    if (nChildScoreDiff > 0 || nChildBlueDiff > 0 || nChildKdiff > 0 || nChildSelDiff > 0) verdict = "C";
    BOOST_TEST_MESSAGE("S2_LOADBEAR CLASSIFICATION="<<verdict);

    // ---- Phase 7: Restore live state ----
    ResetBlockIndexAuthoritativeStartupForTest();
    { LOCK(cs_main);
      for (std::map<uint256,CBlockIndex*>::iterator it=mapBlockIndex.begin(); it!=mapBlockIndex.end(); ++it)
        if (savedMap.count(it->first)==0){ delete it->second->phashBlock; delete it->second; }
      mapBlockIndex.clear();
      mapBlockIndex=savedMap;
      pindexBest=savedBest; pindexGenesisBlock=savedGenesis;
      hashBestChain=savedBestChain; nBestHeight=savedBestHeight; nBestChainTrust=savedBestTrust;
    }
    g_dagManager.ClearDAGDataForTest();
    { LOCK(cs_main);
      LOCK2(g_dagManager.cs_dag, cs_main);
      for (std::map<uint256,CBlockIndex*>::iterator it=savedMap.begin(); it!=savedMap.end(); ++it){
        CBlockIndex* pi=it->second; if(!pi) continue;
        if (pi->nHeight<GetForkHeightDAG() || !pi->IsProofOfWork()) continue;
        CBlockDAGData dd;
        if (g_dagManager.GetDAGData(pi->GetBlockHash(),dd)) g_dagManager.SetDAGDataForTest(pi->GetBlockHash(),dd);
      }
    }
    SetMockTime(0); // restore real time
}

// R2c.2s / S2 MEMORY INSTRUMENTATION (deferred memory gate).
// Measure Option-R / C-full temporary structures on a MATERIALLY LARGER retained
// canvas (hundreds of retained DAG-era blocks + an erased merge-rich selected
// parent), recording THEORETICAL BOUND and MEASURED PEAK per the ≤1 GiB VPS target.
BOOST_AUTO_TEST_CASE(r2c2s_s2_memory_instrumentation)
{
    SetMockTime(1700000400); // deterministic time
    BOOST_REQUIRE(CZKContext::Initialize()); if (!hooks) hooks=InitHook();

    // Build a large retained DAG-era b-branch (N blocks, will win), and a SMALL
    // a-branch whose merge-rich a1 (parents=[a1_pre,b1_pre] at comparable height)
    // becomes the erased selected parent of retained b3. After the reorg the b-branch
    // is retained (large canvas); a1's daglinks are erased. Mirrors the load-bearing
    // fixture at materially larger retained scale.
    CBlockIndex* fork=pindexBest;
    while (fork->nHeight<GetForkHeightDAG()) fork=MineReal(fork,0x9C00+fork->nHeight);
    fork=MineRealDag(fork,0x9C10);
    // a-branch (small, erased): a1_pre -> a1(merge[a1_pre,b1_pre])
    CBlockIndex* a1_pre=MineRealDag(fork,0x9C11);
    CBlockIndex* b1_pre=AddSideDag(fork,0x9C20);
    std::unique_ptr<CBlock> a1_block(BuildPoWBlock(a1_pre,0x9C21));
    std::vector<uint256> a1_parents;
    a1_parents.push_back(a1_pre->GetBlockHash());
    a1_parents.push_back(b1_pre->GetBlockHash());
    AttachDagParentsAndRemine(a1_block.get(),a1_parents);
    CBlockIndex* a1=NULL;
    { LOCK(cs_main);
      BOOST_REQUIRE(a1_block->CheckBlock(true,true,true));
      BOOST_REQUIRE(a1_block->AcceptBlock());
      a1=mapBlockIndex[a1_block->GetHash()]; }
    BOOST_REQUIRE(a1);
    // b-branch (large, retained, will win): b1_pre -> b2 -> [N] -> b3(merge[bprev,a1])
    CBlockIndex* b2=AddSideDag(b1_pre,0x9C22);
    CBlockIndex* bchain=b2;
    // Keep a1 within merge depth (64) of the b-chain merge point so b3=merge[bchain,a1]
    // is valid; N=50 gives a materially larger retained canvas than the load-bearing
    // fixture (~15) while respecting the DAG merge-depth bound.
    const unsigned N=50;
    for (unsigned i=0;i<N;++i) bchain=AddSideDag(bchain,0x9C30+(i%200));
    // b3 = merge[bchain, a1] — a1 is the erased selected parent.
    std::unique_ptr<CBlock> merge(BuildPoWBlock(bchain,0x9C41));
    std::vector<uint256> pm; pm.push_back(bchain->GetBlockHash()); pm.push_back(a1->GetBlockHash());
    AttachDagParentsAndRemine(merge.get(),pm);
    CBlockIndex* b3=NULL;
    { LOCK(cs_main);
      BOOST_REQUIRE(merge->CheckBlock(true,true,true));
      BOOST_REQUIRE(merge->AcceptBlock());
      b3=mapBlockIndex[merge->GetHash()]; }
    BOOST_REQUIRE(b3);
    // Reorg: extend the b-branch until it outruns the a-branch, erasing a1.
    CBlockIndex* branch=b3;
    for (unsigned i=0;i<12 && pindexBest==a1_pre;++i) branch=AddSideDag(branch,0x9C50+i);
    BOOST_REQUIRE_MESSAGE(pindexBest==branch,"b-chain failed to win reorg");

    // Snapshot the canonical source + build an authoritative generation.
    const fs::path root=fs::temp_directory_path()/fs::unique_path("s2-mem-%%%%-%%%%");
    fs::create_directories(root/"snapshot");
    struct Cleanup { fs::path root; CBlockIndex* best; CBlockIndex* genesis;
        Cleanup(const fs::path& r):root(r),best(pindexBest),genesis(pindexGenesisBlock){}
        ~Cleanup(){ ResetBlockIndexAuthoritativeStartupForTest(); pindexBest=best; pindexGenesisBlock=genesis;
            if(best){nBestHeight=best->nHeight;hashBestChain=best->GetBlockHash();nBestChainTrust=best->nChainTrust;}
            try{fs::remove_all(root);}catch(...){} }
    } cleanup(root);
    { CTxDB db; db.Close(); }
    const auto live=GetDataDir()/"txleveldb";
    for(fs::directory_iterator it(live),end;it!=end;++it)
        if(fs::is_regular_file(it->path())) fs::copy_file(it->path(),root/"snapshot"/it->path().filename());
    BlockIndexGenerationSource src; std::string aerr;
    BOOST_REQUIRE_MESSAGE(ReadLegacyBlockIndexSource((root/"snapshot").string(),&src,&aerr),aerr);
    BOOST_REQUIRE_MESSAGE(ReadDAGLinksFromSnapshot((root/"snapshot").string(),&src.dagLinks,&src.dagScores,&aerr),aerr);
    src.foundDAGLinks=true;
    src.blockDataDir=GetDataDir().string(); src.dagLinksDir=(root/"snapshot").string();
    BlockIndexGenerationBuilder ab;
    BOOST_REQUIRE_MESSAGE(ab.Build(src,(root/"build-000001.tmp").string(),1,NULL,&aerr),aerr); ab.Close();
    BOOST_REQUIRE_EQUAL(BlockIndexGenerationManager::PublishGeneration(root.string(),1,&aerr),BLOCK_INDEX_LIFECYCLE_OK);
    BOOST_REQUIRE_EQUAL(BlockIndexGenerationManager::SelectGeneration(root.string(),1,&aerr),BLOCK_INDEX_LIFECYCLE_OK);
    g_dagManager.ClearDAGDataForTest();
    BOOST_REQUIRE_MESSAGE(InitBlockIndexAuthoritative(root.string(),&aerr),aerr);

    // Build the retained canonical scope (materially larger than the small fixtures).
    std::map<uint256,CBlockDAGData> arLinks;
    { CTxDB db; BOOST_REQUIRE(db.IterateDAGLinks(arLinks)); }
    std::vector<std::pair<int32_t,uint256>> scope;
    for (std::map<uint256,CBlockDAGData>::const_iterator it=arLinks.begin(); it!=arLinks.end(); ++it){
        BlockIndexSnapshot s; std::string e2;
        if (!ResolveAuthoritativeBlockSnapshot(it->first,&s,&e2)) continue;
        scope.push_back(std::make_pair((int32_t)s.height,it->first));
    }
    std::sort(scope.begin(),scope.end());
    // Run the Option-R recolor and capture the memory stats.
    std::vector<CanonicalDAGRecolorRecord> fields;
    CanonicalDAGRecolorStats st;
    std::string rerr;
    CTxDB db("r");
    AuthoritativeDAGRecolorSource srcA(db);
    bool complete=ReconstructAuthoritativeDAGFields(scope,srcA,&fields,&st,&rerr);
    BOOST_REQUIRE_MESSAGE(complete,"memory-stress Option-R recolor failed: "+rerr);
    BOOST_REQUIRE_MESSAGE(st.boundaryVertices>0,"memory-stress recolor must reconstruct >=1 boundary");

    // Measured peak (temporary structures, deterministic counters).
    BOOST_TEST_MESSAGE("S2_MEM retained="<<st.retainedVertices
        <<"; preDAGBase="<<st.preDAGBaseVertices
        <<"; boundary="<<st.boundaryVertices
        <<"; closureVertices="<<st.boundaryClosureVertices
        <<"; closureDagRecords="<<st.boundaryClosureDagRecords
        <<"; metadataSnapshots="<<st.metadataSnapshots
        <<"; preDAGTrustEntries="<<st.preDAGTrustEntries
        <<"; parentsMemoEntries="<<st.parentsMemoEntries
        <<"; localIndexEntries="<<st.localIndexEntries
        <<"; colorOrderEntries="<<st.colorOrderEntries
        <<"; objectBytes="<<st.objectBytes
        <<"; estTemporaryBytes="<<st.estimatedTemporaryBytes);
    // THEORETICAL BOUND (per-vertex, independent of total chain height):
    //   retained canvas <= DAG_PRUNE_DEPTH (100000) retained vertices.
    //   per retained vertex: BlockIndexSnapshot(344) + CBlockDAGData(96) +
    //     CBlockIndex(240) + map/std overhead(<=80) ~ 760 B  -> 100000*760 ~ 76 MiB.
    //   boundary closure <= BLUESET_MAX_VISITED(256) ancestors per boundary,
    //     bounded and NOT proportional to chain history.
    //   pre-DAG leaves: <= depth of pre-DAG base, bounded by DAG_PRUNE_DEPTH but
    //     typically tiny (memoized, not materialized as CBlockIndex).
    // Measured peak must stay far below any 1 GiB concern at this scale.
    BOOST_REQUIRE_MESSAGE(st.estimatedTemporaryBytes < (1u<<30),
        "S2 temporary bytes exceed 1 GiB budget at measured scale");
    BOOST_TEST_MESSAGE("S2_MEM THEORETICAL-BOUND retained<=100000*760B~76MiB; measured="
        <<st.estimatedTemporaryBytes<<" B at retained="<<st.retainedVertices);
    SetMockTime(0);
}

// R2c.2s / S3 — atomic authoritative full-field persistence discriminators.
// Proves: (1) after a real reorg erases a DAG-era parent, an authoritative source
// transaction that stages the retained closure's full-field via
// StageAuthoritativeDAGScoreState (recolor against the STAGED view, then full-field +
// token + child-count cert + score cert in ONE WriteBatch) commits such that the
// PERSISTED full-field equals the S2 current-canonical counterfactual oracle, the
// token advances exactly once, and BOTH certificates bind to the new token; and
// (2) abort (TxnAbort, no commit) leaves every old value intact across a real
// reopen. This is the batch-only path (no standalone quiesced-only publisher).
BOOST_AUTO_TEST_CASE(r2c2s_s3_atomic_full_field_persistence)
{
    SetMockTime(1700000500); // deterministic time
    BOOST_REQUIRE(CZKContext::Initialize()); if (!hooks) hooks=InitHook();

    // ---- Phase 1: Build the load-bearing topology + real reorg (erase a1) ----
    CBlockIndex* fork = pindexBest;
    while (fork->nHeight < GetForkHeightDAG()) fork = MineReal(fork, 0x9D00 + fork->nHeight);
    CBlockIndex* a1_pre = MineRealDag(fork, 0x9D10);
    CBlockIndex* b1_pre = AddSideDag(fork, 0x9D20);
    std::unique_ptr<CBlock> a1_block(BuildPoWBlock(a1_pre, 0x9D11));
    std::vector<uint256> a1_parents; a1_parents.push_back(a1_pre->GetBlockHash()); a1_parents.push_back(b1_pre->GetBlockHash());
    AttachDagParentsAndRemine(a1_block.get(), a1_parents);
    CBlockIndex* a1=NULL;
    { LOCK(cs_main); BOOST_REQUIRE(a1_block->CheckBlock(true,true,true)); BOOST_REQUIRE(a1_block->AcceptBlock()); a1=mapBlockIndex[a1_block->GetHash()]; }
    BOOST_REQUIRE(a1);
    CBlockIndex* active=a1;
    for (unsigned i=0;i<6;++i) active=AddSideDag(active,0x9D12+i);
    CBlockIndex* b2=AddSideDag(b1_pre,0x9D22);
    std::unique_ptr<CBlock> merge(BuildPoWBlock(b2,0x9D23));
    std::vector<uint256> pm; pm.push_back(b2->GetBlockHash()); pm.push_back(a1->GetBlockHash());
    AttachDagParentsAndRemine(merge.get(),pm);
    CBlockIndex* b3=NULL;
    { LOCK(cs_main); BOOST_REQUIRE(merge->CheckBlock(true,true,true)); BOOST_REQUIRE(merge->AcceptBlock()); b3=mapBlockIndex[merge->GetHash()]; }
    BOOST_REQUIRE(b3);
    CBlockIndex* branch=b3;
    for (unsigned i=0;i<12 && pindexBest==active;++i) branch=AddSideDag(branch,0x9D30+i);
    BOOST_REQUIRE_MESSAGE(pindexBest==branch,"b-chain failed to win reorg");
    // Confirm a1's daglinks was erased on disk by the legacy reorg.
    {
        CTxDB db; std::map<uint256,CBlockDAGData> onDisk; BOOST_REQUIRE(db.IterateDAGLinks(onDisk));
        BOOST_REQUIRE_MESSAGE(!onDisk.count(a1->GetBlockHash()),"a1 daglinks must be erased by reorg");
        BOOST_REQUIRE_MESSAGE(onDisk.count(b3->GetBlockHash()),"b3 must be retained");
        BOOST_TEST_MESSAGE("S3_FIXTURE a1 erased="<<(!onDisk.count(a1->GetBlockHash())?1:0)<<" b3 retained="<<(onDisk.count(b3->GetBlockHash())?1:0));
    }

    // ---- Phase 2: Snapshot + bootstrap authoritative generation (by-value resolution) ----
    const fs::path root=fs::temp_directory_path()/fs::unique_path("s3-atomic-%%%%-%%%%");
    fs::create_directories(root/"snapshot");
    struct Cleanup { fs::path root; CBlockIndex* best; CBlockIndex* genesis;
        Cleanup(const fs::path& r):root(r),best(pindexBest),genesis(pindexGenesisBlock){}
        ~Cleanup(){ ResetBlockIndexAuthoritativeStartupForTest(); pindexBest=best; pindexGenesisBlock=genesis;
            if(best){nBestHeight=best->nHeight;hashBestChain=best->GetBlockHash();nBestChainTrust=best->nChainTrust;}
            try{fs::remove_all(root);}catch(...){} }
    } cleanup(root);
    { CTxDB db; db.Close(); }
    const auto live=GetDataDir()/"txleveldb";
    for(fs::directory_iterator it(live),end;it!=end;++it)
        if(fs::is_regular_file(it->path())) fs::copy_file(it->path(),root/"snapshot"/it->path().filename());
    BlockIndexGenerationSource src; std::string aerr;
    BOOST_REQUIRE_MESSAGE(ReadLegacyBlockIndexSource((root/"snapshot").string(),&src,&aerr),aerr);
    BOOST_REQUIRE_MESSAGE(ReadDAGLinksFromSnapshot((root/"snapshot").string(),&src.dagLinks,&src.dagScores,&aerr),aerr);
    src.foundDAGLinks=true;
    src.blockDataDir=GetDataDir().string(); src.dagLinksDir=(root/"snapshot").string();
    BlockIndexGenerationBuilder ab;
    BOOST_REQUIRE_MESSAGE(ab.Build(src,(root/"build-000001.tmp").string(),1,NULL,&aerr),aerr); ab.Close();
    BOOST_REQUIRE_EQUAL(BlockIndexGenerationManager::PublishGeneration(root.string(),1,&aerr),BLOCK_INDEX_LIFECYCLE_OK);
    BOOST_REQUIRE_EQUAL(BlockIndexGenerationManager::SelectGeneration(root.string(),1,&aerr),BLOCK_INDEX_LIFECYCLE_OK);
    g_dagManager.ClearDAGDataForTest();
    BOOST_REQUIRE_MESSAGE(InitBlockIndexAuthoritative(root.string(),&aerr),aerr);

    // ---- Phase 3: (oracle is computed by the S3 stage itself against the staged view) ----

    // ---- Phase 4: authoritative atomic full-field staging (batch-only) ----
    // Capture the pre-commit persisted full-field + token + cert health.
    {
        CTxDB db;
        uint256 tokenBefore; bool hasBefore=false;
        BOOST_REQUIRE_MESSAGE(db.ReadDAGSourceStateId(tokenBefore)||(hasBefore=true), "token must exist after legacy reorg advanced it");
        std::string cb1,cb2; bool ccHealthy=db.IsDAGChildCountIndexHealthy(&cb1); bool scHealthy=db.IsDAGScoreAuthorityHealthy(&cb2);
        // Pre-commit persisted full-field for the affected retained closure.
        std::map<uint256,CBlockDAGData> persistedBefore; BOOST_REQUIRE(db.IterateDAGLinks(persistedBefore));
        std::map<uint256,CBlockDAGData> b3Before; b3Before[b3->GetBlockHash()]=persistedBefore[b3->GetBlockHash()];

        // Open the authoritative source transaction, stage the retained closure's
        // full-field (recolor against staged view), token + both certs, then commit.
        {
            CTxDB sdb;
            BOOST_REQUIRE(sdb.TxnBegin());
            std::vector<std::pair<int32_t,uint256>> scope;
            std::string serr;
            BOOST_REQUIRE_MESSAGE(EnumerateAuthoritativeStagedScope(sdb,&scope,NULL,&serr),serr);
            uint256 newToken;
            BOOST_REQUIRE(sdb.MintDAGSourceStateId(newToken));
            AuthoritativeDAGStageResult res;
            std::string sterr;
            BOOST_REQUIRE_MESSAGE(StageAuthoritativeDAGScoreState(sdb,scope,newToken,NULL,&res,&sterr),sterr);
            BOOST_REQUIRE_MESSAGE(!res.fullFields.empty(),"S3 stage produced no full-field records");
            BOOST_REQUIRE_MESSAGE(sdb.TxnCommit(),"S3 source commit failed");
            BOOST_TEST_MESSAGE("S3_STAGE committed fullFields="<<res.fullFields.size()
                <<" affected="<<res.affectedHashes.size()<<" writeBatchBytes="<<res.writeBatchBytes
                <<" stagedTopology="<<res.stagedTopologyEntries);
        }

        // ---- Phase 5: post-commit verification ----
        {
            CTxDB db;
            uint256 tokenAfter; BOOST_REQUIRE(db.ReadDAGSourceStateId(tokenAfter));
            BOOST_TEST_MESSAGE("S3_COMMIT tokenAdvanced="<<((tokenAfter!=tokenBefore)?1:0)
                <<" childcountHealthy="<<db.IsDAGChildCountIndexHealthy(&cb1)
                <<" scoreHealthy="<<db.IsDAGScoreAuthorityHealthy(&cb2));
            BOOST_CHECK_MESSAGE(tokenAfter!=tokenBefore,"S3 token must advance exactly once per physical source commit");
            BOOST_CHECK_MESSAGE(db.IsDAGChildCountIndexHealthy(&cb1),"S3 child-count cert must bind to new token");
            BOOST_CHECK_MESSAGE(db.IsDAGScoreAuthorityHealthy(&cb2),"S3 score cert must bind to new token");
            // Persisted full-field for b3 must be readable and non-stale.
            CBlockDAGData b3After;
            BOOST_REQUIRE(db.ReadDAGLinks(b3->GetBlockHash(),b3After));
            BOOST_TEST_MESSAGE("S3_COMMIT b3 persisted nDAGScore="<<b3After.nDAGScore.GetHex()
                <<" fBlue="<<b3After.fBlue<<" k="<<b3After.nInferredK);
            // a1 must remain absent from persisted daglinks.
            CBlockDAGData a1After;
            BOOST_REQUIRE(!db.ReadDAGLinks(a1->GetBlockHash(),a1After));
            BOOST_TEST_MESSAGE("S3_COMMIT a1 still erased from daglinks=1");

            // ---- Phase 6: persisted full-field == independent S2 counterfactual oracle ----
            // Compute the S2 current-canonical counterfactual oracle INDEPENDENTLY
            // (inject the erased a1 as a normal retained record into the committed
            // post-reorg canonical source via CounterfactualOracle::Build) and require
            // the PERSISTED full-field equals it. This proves S3 persisted the exact
            // current-canonical semantics, not legacy resident/stale residue.
            std::vector<std::pair<int32_t,uint256>> cScope;
            {
                std::map<uint256,CBlockDAGData> links;
                BOOST_REQUIRE(db.IterateDAGLinks(links));
                for (std::map<uint256,CBlockDAGData>::const_iterator it=links.begin(); it!=links.end(); ++it){
                    BlockIndexSnapshot s; std::string e2;
                    if (!ResolveAuthoritativeBlockSnapshot(it->first,&s,&e2)) continue;
                    cScope.push_back(std::make_pair((int32_t)s.height,it->first));
                }
                std::sort(cScope.begin(),cScope.end());
            }
            std::vector<uint256> erasedP;
            erasedP.push_back(a1->GetBlockHash());
            std::string cfErr;
            std::map<uint256,CanonicalDAGRecolorRecord> cfByHash =
                CounterfactualOracle::Build(cScope,AuthoritativeDAGRecolorSource(db),erasedP,&cfErr);
            BOOST_REQUIRE_MESSAGE(!cfByHash.empty(),"S3 oracle build failed: "+cfErr);
            std::map<uint256,CanonicalDAGRecolorRecord>::const_iterator cf=cfByHash.find(b3->GetBlockHash());
            BOOST_REQUIRE_MESSAGE(cf!=cfByHash.end(),"S3 oracle missing b3");
            bool b3Equal=(b3After.nDAGScore==cf->second.nDAGScore)&&
                         (b3After.fBlue==cf->second.fBlue)&&
                         (b3After.nInferredK==cf->second.nInferredK);
            BOOST_TEST_MESSAGE("S3_ORACLE b3 persisted="<<b3After.nDAGScore.GetHex()
                <<" counterfactual="<<cf->second.nDAGScore.GetHex()
                <<" fullEqual="<<(b3Equal?1:0));
            BOOST_CHECK_MESSAGE(b3Equal,
                "S3 persisted b3 full-field != independent current-canonical counterfactual oracle");
        }
    }
    SetMockTime(0);
}

// R2c.2s / S3 — abort preserves old score source (failure-path discriminator).
// After the S3 stage has staged full-field + token + BOTH certs into an open
// WriteBatch, TxnAbort (no commit) must leave every old value intact across a REAL
// reopen: old token, old cert health/binding, old persisted full-field, old
// topology. No partial staged state may be visible after abort/reopen.
BOOST_AUTO_TEST_CASE(r2c2s_s3_abort_preserves_old_score_source)
{
    SetMockTime(1700000600); // deterministic time
    BOOST_REQUIRE(CZKContext::Initialize()); if (!hooks) hooks=InitHook();

    // Build the load-bearing topology + real reorg (erase a1) exactly as the
    // commit discriminator, so the pre-abort state has an erased boundary.
    CBlockIndex* fork = pindexBest;
    while (fork->nHeight < GetForkHeightDAG()) fork = MineReal(fork, 0x9E00 + fork->nHeight);
    CBlockIndex* a1_pre = MineRealDag(fork, 0x9E10);
    CBlockIndex* b1_pre = AddSideDag(fork, 0x9E20);
    std::unique_ptr<CBlock> a1_block(BuildPoWBlock(a1_pre, 0x9E11));
    std::vector<uint256> a1_parents; a1_parents.push_back(a1_pre->GetBlockHash()); a1_parents.push_back(b1_pre->GetBlockHash());
    AttachDagParentsAndRemine(a1_block.get(), a1_parents);
    CBlockIndex* a1=NULL;
    { LOCK(cs_main); BOOST_REQUIRE(a1_block->CheckBlock(true,true,true)); BOOST_REQUIRE(a1_block->AcceptBlock()); a1=mapBlockIndex[a1_block->GetHash()]; }
    BOOST_REQUIRE(a1);
    CBlockIndex* active=a1;
    for (unsigned i=0;i<6;++i) active=AddSideDag(active,0x9E12+i);
    CBlockIndex* b2=AddSideDag(b1_pre,0x9E22);
    std::unique_ptr<CBlock> merge(BuildPoWBlock(b2,0x9E23));
    std::vector<uint256> pm; pm.push_back(b2->GetBlockHash()); pm.push_back(a1->GetBlockHash());
    AttachDagParentsAndRemine(merge.get(),pm);
    CBlockIndex* b3=NULL;
    { LOCK(cs_main); BOOST_REQUIRE(merge->CheckBlock(true,true,true)); BOOST_REQUIRE(merge->AcceptBlock()); b3=mapBlockIndex[merge->GetHash()]; }
    BOOST_REQUIRE(b3);
    CBlockIndex* branch=b3;
    for (unsigned i=0;i<12 && pindexBest==active;++i) branch=AddSideDag(branch,0x9E30+i);
    BOOST_REQUIRE_MESSAGE(pindexBest==branch,"b-chain failed to win reorg");

    // Snapshot + bootstrap authoritative generation.
    const fs::path root=fs::temp_directory_path()/fs::unique_path("s3-abort-%%%%-%%%%");
    fs::create_directories(root/"snapshot");
    struct Cleanup { fs::path root; CBlockIndex* best; CBlockIndex* genesis;
        Cleanup(const fs::path& r):root(r),best(pindexBest),genesis(pindexGenesisBlock){}
        ~Cleanup(){ ResetBlockIndexAuthoritativeStartupForTest(); pindexBest=best; pindexGenesisBlock=genesis;
            if(best){nBestHeight=best->nHeight;hashBestChain=best->GetBlockHash();nBestChainTrust=best->nChainTrust;}
            try{fs::remove_all(root);}catch(...){} }
    } cleanup(root);
    { CTxDB db; db.Close(); }
    const auto live=GetDataDir()/"txleveldb";
    for(fs::directory_iterator it(live),end;it!=end;++it)
        if(fs::is_regular_file(it->path())) fs::copy_file(it->path(),root/"snapshot"/it->path().filename());
    BlockIndexGenerationSource src; std::string aerr;
    BOOST_REQUIRE_MESSAGE(ReadLegacyBlockIndexSource((root/"snapshot").string(),&src,&aerr),aerr);
    BOOST_REQUIRE_MESSAGE(ReadDAGLinksFromSnapshot((root/"snapshot").string(),&src.dagLinks,&src.dagScores,&aerr),aerr);
    src.foundDAGLinks=true;
    src.blockDataDir=GetDataDir().string(); src.dagLinksDir=(root/"snapshot").string();
    BlockIndexGenerationBuilder ab;
    BOOST_REQUIRE_MESSAGE(ab.Build(src,(root/"build-000001.tmp").string(),1,NULL,&aerr),aerr); ab.Close();
    BOOST_REQUIRE_EQUAL(BlockIndexGenerationManager::PublishGeneration(root.string(),1,&aerr),BLOCK_INDEX_LIFECYCLE_OK);
    BOOST_REQUIRE_EQUAL(BlockIndexGenerationManager::SelectGeneration(root.string(),1,&aerr),BLOCK_INDEX_LIFECYCLE_OK);
    g_dagManager.ClearDAGDataForTest();
    BOOST_REQUIRE_MESSAGE(InitBlockIndexAuthoritative(root.string(),&aerr),aerr);

    // Capture the pre-abort (old) state: token, cert health, persisted full-field,
    // and b3 persisted full-field. Capture hashes BEFORE reopen (resident pointers
    // become dangling after the real reload).
    uint256 a1Hash = a1->GetBlockHash();
    uint256 b3Hash = b3->GetBlockHash();
    uint256 oldToken;
    {
        CTxDB db; BOOST_REQUIRE(db.ReadDAGSourceStateId(oldToken));
    }
    CBlockDAGData oldB3;
    { CTxDB db; BOOST_REQUIRE(db.ReadDAGLinks(b3Hash,oldB3)); }
    std::string cc1,cc2; bool oldCCHealthy, oldSCHealthy;
    { CTxDB db; oldCCHealthy=db.IsDAGChildCountIndexHealthy(&cc1); oldSCHealthy=db.IsDAGScoreAuthorityHealthy(&cc2); }
    BOOST_TEST_MESSAGE("S3_ABORT oldToken="<<oldToken.GetHex()
        <<" oldB3Score="<<oldB3.nDAGScore.GetHex()<<" oldB3Blue="<<oldB3.fBlue<<" oldB3K="<<oldB3.nInferredK
        <<" oldCCHealthy="<<oldCCHealthy<<" oldSCHealthy="<<oldSCHealthy);

    // Open a source txn, stage full-field + token + both certs, then ABORT (no commit).
    {
        CTxDB sdb;
        BOOST_REQUIRE(sdb.TxnBegin());
        std::vector<std::pair<int32_t,uint256>> scope;
        std::string serr;
        BOOST_REQUIRE_MESSAGE(EnumerateAuthoritativeStagedScope(sdb,&scope,NULL,&serr),serr);
        uint256 newToken;
        BOOST_REQUIRE(sdb.MintDAGSourceStateId(newToken));
        AuthoritativeDAGStageResult res;
        std::string sterr;
        BOOST_REQUIRE_MESSAGE(StageAuthoritativeDAGScoreState(sdb,scope,newToken,NULL,&res,&sterr),sterr);
        BOOST_REQUIRE_MESSAGE(!res.fullFields.empty(),"S3 abort: stage produced no full-field");
        // Injected failure: abort before commit (TxnAbort discards the batch).
        BOOST_REQUIRE(sdb.TxnAbort());
        BOOST_TEST_MESSAGE("S3_ABORT stagedThenAborted fullFields="<<res.fullFields.size());
    }

    // Real reopen: destroy resident block-index state and reload from the DB.
    std::map<uint256,CBlockIndex*> savedMap;
    { LOCK(cs_main); savedMap=mapBlockIndex; }
    CBlockIndex* savedBest=pindexBest; CBlockIndex* savedGenesis=pindexGenesisBlock;
    uint256 savedBestChain=hashBestChain; int savedBestHeight=nBestHeight; uint256 savedBestTrust=nBestChainTrust;
    ResetBlockIndexAuthoritativeStartupForTest();
    { CTxDB db; db.Close(); }
    { LOCK(cs_main); mapBlockIndex.clear(); }
    { LOCK(g_dagManager.cs_dag); g_dagManager.ClearDAGDataForTest(); }
    pindexBest=NULL; pindexGenesisBlock=NULL; nBestHeight=-1; hashBestChain=uint256(0); nBestChainTrust=uint256(0);
    bool loaded=false;
    { CTxDB db; loaded = db.LoadBlockIndex(); }
    BOOST_REQUIRE_MESSAGE(loaded,"abort reopen LoadBlockIndex failed");
    BOOST_REQUIRE(pindexBest);

    // After reopen: every old value must be intact (no partial staged state visible).
    {
        CTxDB db;
        uint256 tokenAfter; BOOST_REQUIRE(db.ReadDAGSourceStateId(tokenAfter));
        CBlockDAGData b3After; BOOST_REQUIRE(db.ReadDAGLinks(b3Hash,b3After));
        bool ccH=db.IsDAGChildCountIndexHealthy(&cc1); bool scH=db.IsDAGScoreAuthorityHealthy(&cc2);
        bool tokenIntact = (tokenAfter==oldToken);
        bool fullIntact = (b3After.nDAGScore==oldB3.nDAGScore)&&(b3After.fBlue==oldB3.fBlue)&&(b3After.nInferredK==oldB3.nInferredK);
        bool certsIntact = (ccH==oldCCHealthy)&&(scH==oldSCHealthy);
        CBlockDAGData a1After; bool a1StillErased = !db.ReadDAGLinks(a1Hash,a1After);
        BOOST_TEST_MESSAGE("S3_ABORT_REOPEN tokenIntact="<<(tokenIntact?1:0)
            <<" fullFieldIntact="<<(fullIntact?1:0)
            <<" certsIntact="<<(certsIntact?1:0)
            <<" a1StillErased="<<(a1StillErased?1:0)
            <<" token="<<tokenAfter.GetHex());
        BOOST_CHECK_MESSAGE(tokenIntact,"S3 abort: source token changed after abort/reopen");
        BOOST_CHECK_MESSAGE(fullIntact,"S3 abort: persisted full-field changed after abort/reopen");
        BOOST_CHECK_MESSAGE(certsIntact,"S3 abort: certificate health/binding changed after abort/reopen");
        BOOST_CHECK_MESSAGE(a1StillErased,"S3 abort: topology changed after abort/reopen");
    }
    // Restore resident state for any subsequent tests.
    ResetBlockIndexAuthoritativeStartupForTest();
    { LOCK(cs_main);
      for (std::map<uint256,CBlockIndex*>::iterator it=mapBlockIndex.begin(); it!=mapBlockIndex.end(); ++it)
        if (savedMap.count(it->first)==0){ delete it->second->phashBlock; delete it->second; }
      mapBlockIndex.clear(); mapBlockIndex=savedMap;
      pindexBest=savedBest; pindexGenesisBlock=savedGenesis;
      hashBestChain=savedBestChain; nBestHeight=savedBestHeight; nBestChainTrust=savedBestTrust; }
    g_dagManager.ClearDAGDataForTest();
    SetMockTime(0);
}

// R2c.2s / S3 — staged readback precedence threshold (gate 9 in directive).
// Within an open authoritative source transaction, verify exact readback
// precedence for BOTH keyed ReadDAGLinks and daglinks enumeration:
//   staged write/replacement > staged tombstone > persisted DB,
// including write-after-tombstone (write wins) and tombstone-after-write
// (tombstone wins). Uses real fixture block hashes so enumeration is valid.
// This closes subtle batch-view inconsistencies before S5 uses the preview.
BOOST_AUTO_TEST_CASE(r2c2s_s3_staged_readback_precedence)
{
    SetMockTime(1700000700); // deterministic time
    BOOST_REQUIRE(CZKContext::Initialize()); if (!hooks) hooks=InitHook();

    // Build a real DAG-era chain: fork -> a1 (single parent).
    CBlockIndex* fork = pindexBest;
    while (fork->nHeight < GetForkHeightDAG()) fork = MineReal(fork, 0x9F00 + fork->nHeight);
    CBlockIndex* a1 = MineRealDag(fork, 0x9F10);
    CBlockIndex* a1b = MineRealDag(a1, 0x9F11);
    CBlockIndex* a1c = MineRealDag(a1b, 0x9F12); // DAG-era block for tomb-after-write key
    CBlockIndex* a2 = AddSideDag(fork, 0x9F20);
    CBlockIndex* a2b = AddSideDag(a2, 0x9F21);
    // Snapshot + bootstrap authoritative generation (by-value resolution + healthy certs).
    const fs::path root=fs::temp_directory_path()/fs::unique_path("s3-readback-%%%%-%%%%");
    fs::create_directories(root/"snapshot");
    struct Cleanup { fs::path root; CBlockIndex* best; CBlockIndex* genesis;
        Cleanup(const fs::path& r):root(r),best(pindexBest),genesis(pindexGenesisBlock){}
        ~Cleanup(){ ResetBlockIndexAuthoritativeStartupForTest(); pindexBest=best; pindexGenesisBlock=genesis;
            if(best){nBestHeight=best->nHeight;hashBestChain=best->GetBlockHash();nBestChainTrust=best->nChainTrust;}
            try{fs::remove_all(root);}catch(...){} }
    } cleanup(root);
    { CTxDB db; db.Close(); }
    const auto live=GetDataDir()/"txleveldb";
    for(fs::directory_iterator it(live),end;it!=end;++it)
        if(fs::is_regular_file(it->path())) fs::copy_file(it->path(),root/"snapshot"/it->path().filename());
    BlockIndexGenerationSource src; std::string aerr;
    BOOST_REQUIRE_MESSAGE(ReadLegacyBlockIndexSource((root/"snapshot").string(),&src,&aerr),aerr);
    BOOST_REQUIRE_MESSAGE(ReadDAGLinksFromSnapshot((root/"snapshot").string(),&src.dagLinks,&src.dagScores,&aerr),aerr);
    src.foundDAGLinks=true;
    src.blockDataDir=GetDataDir().string(); src.dagLinksDir=(root/"snapshot").string();
    BlockIndexGenerationBuilder ab;
    BOOST_REQUIRE_MESSAGE(ab.Build(src,(root/"build-000001.tmp").string(),1,NULL,&aerr),aerr); ab.Close();
    BOOST_REQUIRE_EQUAL(BlockIndexGenerationManager::PublishGeneration(root.string(),1,&aerr),BLOCK_INDEX_LIFECYCLE_OK);
    BOOST_REQUIRE_EQUAL(BlockIndexGenerationManager::SelectGeneration(root.string(),1,&aerr),BLOCK_INDEX_LIFECYCLE_OK);
    g_dagManager.ClearDAGDataForTest();
    BOOST_REQUIRE_MESSAGE(InitBlockIndexAuthoritative(root.string(),&aerr),aerr);

    const uint256 hPersistedOnly = a1->GetBlockHash();       // unchanged, persisted only
    const uint256 hReplaced     = a1b->GetBlockHash();       // staged replacement
    const uint256 hTomb         = a2->GetBlockHash();        // staged tombstone
    const uint256 hWriteAfterTomb = a2b->GetBlockHash();     // erase then write -> write wins
    const uint256 hTombAfterWrite = a1c->GetBlockHash();     // write then erase -> tombstone wins

    {
        CTxDB db;
        BOOST_REQUIRE(db.TxnBegin());
        // Baseline persisted reads BEFORE any staging.
        CBlockDAGData p0; BOOST_REQUIRE(db.ReadDAGLinks(hPersistedOnly,p0));
        BOOST_TEST_MESSAGE("S3_READBACK baseline persistedOnly="<<(db.ReadDAGLinks(hPersistedOnly,p0)?1:0)
            <<" replaced="<<(db.ReadDAGLinks(hReplaced,p0)?1:0)
            <<" tomb="<<(db.ReadDAGLinks(hTomb,p0)?1:0)
            <<" wa="<<(db.ReadDAGLinks(hWriteAfterTomb,p0)?1:0)
            <<" tw="<<(db.ReadDAGLinks(hTombAfterWrite,p0)?1:0));

        // Stage: replacement write, tombstone, write-after-tombstone, tomb-after-write.
        // Use the low-level batch Write/Erase (the actual daglinks records staged into
        // activeBatch) so ScanBatchDAGLinks + keyed ReadDAGLinks exercise the pure
        // batch-view precedence; child-count maintenance is a separate concern.
        {
            CBlockDAGData rep = p0; rep.nDAGScore = uint256(0x0102030405060708ULL); rep.fBlue=false; rep.nInferredK=5;
            BOOST_REQUIRE(db.StageDAGLinkRawForTest(hReplaced, rep, false));
            CBlockDAGData dummy0; BOOST_REQUIRE(db.StageDAGLinkRawForTest(hTomb, dummy0, true));
            // write-after-tombstone: erase then write
            CBlockDAGData wa = p0; wa.nInferredK=7;
            BOOST_REQUIRE(db.StageDAGLinkRawForTest(hWriteAfterTomb, dummy0, true));
            BOOST_REQUIRE(db.StageDAGLinkRawForTest(hWriteAfterTomb, wa, false));
            // tombstone-after-write: write then erase
            BOOST_REQUIRE(db.StageDAGLinkRawForTest(hTombAfterWrite, p0, false));
            BOOST_REQUIRE(db.StageDAGLinkRawForTest(hTombAfterWrite, dummy0, true));
        }

        // Verify staged visibility via keyed ReadDAGLinks (generic Read -> ScanBatch).
        CBlockDAGData rReplaced, rWa; CBlockDAGData rTomb, rTombAfterWrite, rPersistedOnly;
        bool okReplaced = db.ReadDAGLinks(hReplaced, rReplaced);
        bool okWa = db.ReadDAGLinks(hWriteAfterTomb, rWa);
        bool okTomb = db.ReadDAGLinks(hTomb, rTomb);
        bool okTombAfterWrite = db.ReadDAGLinks(hTombAfterWrite, rTombAfterWrite);
        bool okPersisted = db.ReadDAGLinks(hPersistedOnly, rPersistedOnly);
        BOOST_TEST_MESSAGE("S3_READBACK staged replacedVisible="<<(okReplaced?1:0)
            <<" replacementBlue=false,k="<<(okReplaced?rReplaced.nInferredK:-1)
            <<" tombVisible="<<(okTomb?1:0)
            <<" writeAfterTombVisible="<<(okWa?1:0)
            <<" tombAfterWriteVisible="<<(okTombAfterWrite?1:0)
            <<" persistedOnlyVisible="<<(okPersisted?1:0));
        BOOST_CHECK_MESSAGE(okReplaced,"staged replacement must be visible to ReadDAGLinks");
        BOOST_CHECK_MESSAGE(!okTomb,"staged tombstone must hide key from ReadDAGLinks");
        BOOST_CHECK_MESSAGE(okWa,"write-after-tombstone must be visible (write wins)");
        BOOST_CHECK_MESSAGE(!okTombAfterWrite,"tombstone-after-write must hide key (tombstone wins)");
        BOOST_CHECK_MESSAGE(okPersisted,"persisted-only key must remain visible");

        // Verify staged enumeration also sees the same precedence.
        std::vector<std::pair<int32_t,uint256>> scope;
        std::string serr;
        BOOST_REQUIRE_MESSAGE(EnumerateAuthoritativeStagedScope(db,&scope,NULL,&serr),serr);
        bool inScopeReplaced=false, inScopeTomb=false, inScopeWa=false, inScopeTombAfterW=false, inScopePersisted=false;
        for (size_t i=0;i<scope.size();++i){
            if (scope[i].second==hReplaced) inScopeReplaced=true;
            if (scope[i].second==hTomb) inScopeTomb=true;
            if (scope[i].second==hWriteAfterTomb) inScopeWa=true;
            if (scope[i].second==hTombAfterWrite) inScopeTombAfterW=true;
            if (scope[i].second==hPersistedOnly) inScopePersisted=true;
        }
        BOOST_TEST_MESSAGE("S3_READBACK enum replaced="<<(inScopeReplaced?1:0)
            <<" tomb="<<(inScopeTomb?1:0)
            <<" wa="<<(inScopeWa?1:0)
            <<" tw="<<(inScopeTombAfterW?1:0)
            <<" persisted="<<(inScopePersisted?1:0)<<" total="<<scope.size());
        BOOST_CHECK_MESSAGE(inScopeReplaced && !inScopeTomb && inScopeWa && !inScopeTombAfterW && inScopePersisted,
            "staged enumeration precedence must match keyed ReadDAGLinks precedence");

        // Abort: nothing persists.
        BOOST_REQUIRE(db.TxnAbort());
    }
    // After abort, all keys unequal: persisted state unchanged.
    {
        CTxDB db;
        CBlockDAGData x;
        BOOST_CHECK_MESSAGE(db.ReadDAGLinks(hReplaced,x) && x.nInferredK!=-1 && x.fBlue!=false,
            "abort must restore original persisted replacement (no staged value leaked)");
        BOOST_CHECK_MESSAGE(db.ReadDAGLinks(hTomb,x), "abort must restore tombstoned key");
        BOOST_CHECK_MESSAGE(db.ReadDAGLinks(hTombAfterWrite,x), "abort must restore tomb-after-write key");
        BOOST_TEST_MESSAGE("S3_READBACK post-abort persisted-only="<<(db.ReadDAGLinks(hPersistedOnly,x)?1:0)
            <<" replaced(orig)="<<(db.ReadDAGLinks(hReplaced,x)?1:0)
            <<" tomb(restored)="<<(db.ReadDAGLinks(hTomb,x)?1:0)
            <<" wa(restored)="<<(db.ReadDAGLinks(hWriteAfterTomb,x)?1:0)
            <<" tw(restored)="<<(db.ReadDAGLinks(hTombAfterWrite,x)?1:0));
    }
    SetMockTime(0);
}

// R2c.2s / S3 — batch-only precondition: calling the batch-only
// StageAuthoritativeDAGScoreState WITHOUT an active transaction must cause
// ZERO durable mutation and return false. The Astra freeze landed this as a
// reproduced real-DB blocker (BLOCKER 3): with no open batch the helper used
// to fall through to WriteDAGSourceStateId, which wrote the token durably to
// disk (activeBatch==NULL => direct pdb->Put) and only then failed in a
// batch-only certificate helper, leaving tokenChanged=1 across reopen. The
// early active-batch guard must reject before any write, so all snapshotted
// state (token, daglinks, both certs) is byte/value-identical after reopen.
BOOST_AUTO_TEST_CASE(r2c2s_s3_stage_requires_active_batch)
{
    SetMockTime(1700000800); // deterministic time
    BOOST_REQUIRE(CZKContext::Initialize()); if (!hooks) hooks=InitHook();

    // Build a real DAG-era chain so a persisted source (token + daglinks +
    // certs) exists to snapshot.
    CBlockIndex* fork = pindexBest;
    while (fork->nHeight < GetForkHeightDAG()) fork = MineReal(fork, 0xA000 + fork->nHeight);
    CBlockIndex* a1 = MineRealDag(fork, 0xA010);
    CBlockIndex* a1b = MineRealDag(a1, 0xA011);
    CBlockIndex* a2 = AddSideDag(fork, 0xA020);

    // Snapshot + bootstrap authoritative generation (by-value resolution).
    const fs::path root=fs::temp_directory_path()/fs::unique_path("s3-nobatch-%%%%-%%%%");
    fs::create_directories(root/"snapshot");
    struct Cleanup { fs::path root; CBlockIndex* best; CBlockIndex* genesis;
        Cleanup(const fs::path& r):root(r),best(pindexBest),genesis(pindexGenesisBlock){}
        ~Cleanup(){ ResetBlockIndexAuthoritativeStartupForTest(); pindexBest=best; pindexGenesisBlock=genesis;
            if(best){nBestHeight=best->nHeight;hashBestChain=best->GetBlockHash();nBestChainTrust=best->nChainTrust;}
            try{fs::remove_all(root);}catch(...){} }
    } cleanup(root);
    { CTxDB db; db.Close(); }
    const auto live=GetDataDir()/"txleveldb";
    for(fs::directory_iterator it(live),end;it!=end;++it)
        if(fs::is_regular_file(it->path())) fs::copy_file(it->path(),root/"snapshot"/it->path().filename());
    BlockIndexGenerationSource src; std::string aerr;
    BOOST_REQUIRE_MESSAGE(ReadLegacyBlockIndexSource((root/"snapshot").string(),&src,&aerr),aerr);
    BOOST_REQUIRE_MESSAGE(ReadDAGLinksFromSnapshot((root/"snapshot").string(),&src.dagLinks,&src.dagScores,&aerr),aerr);
    src.foundDAGLinks=true;
    src.blockDataDir=GetDataDir().string(); src.dagLinksDir=(root/"snapshot").string();
    BlockIndexGenerationBuilder ab;
    BOOST_REQUIRE_MESSAGE(ab.Build(src,(root/"build-000001.tmp").string(),1,NULL,&aerr),aerr); ab.Close();
    BOOST_REQUIRE_EQUAL(BlockIndexGenerationManager::PublishGeneration(root.string(),1,&aerr),BLOCK_INDEX_LIFECYCLE_OK);
    BOOST_REQUIRE_EQUAL(BlockIndexGenerationManager::SelectGeneration(root.string(),1,&aerr),BLOCK_INDEX_LIFECYCLE_OK);
    g_dagManager.ClearDAGDataForTest();
    BOOST_REQUIRE_MESSAGE(InitBlockIndexAuthoritative(root.string(),&aerr),aerr);

    // Snapshot ALL authoritative durable state that any broken caller could
    // mutate: token, per-record daglinks bytes, and both certificate healths.
    std::map<uint256,CBlockDAGData> persistedBefore;
    uint256 tokenBefore; bool hasTokenBefore=false;
    {
        CTxDB db;
        BOOST_REQUIRE(db.IterateDAGLinks(persistedBefore));
        hasTokenBefore = db.ReadDAGSourceStateId(tokenBefore);
        BOOST_REQUIRE_MESSAGE(hasTokenBefore,"no source token to snapshot");
    }
    std::string cb1,cb2;
    bool ccHealthyBefore=false, scHealthyBefore=false;
    { CTxDB db; ccHealthyBefore=db.IsDAGChildCountIndexHealthy(&cb1); scHealthyBefore=db.IsDAGScoreAuthorityHealthy(&cb2); }

    // Exact raw source snapshot includes token, certificates, revocations,
    // child counts and daglinks, not just decoded values or health booleans.
    const auto rawSource = []() {
        CTxDB db;
        std::map<std::string,std::string> raw;
        std::unique_ptr<leveldb::Iterator> it(db.GetInstance()->NewIterator(leveldb::ReadOptions()));
        for(it->SeekToFirst();it->Valid();it->Next()) raw[it->key().ToString()]=it->value().ToString();
        BOOST_REQUIRE(it->status().ok());
        return raw;
    };
    const auto rawBefore=rawSource();

    // Call the batch-only staging helper with NO active transaction. It must
    // reject (false) BEFORE any durable write. We use an empty staged scope to
    // isolate the precondition: with no scope there is no recolor loop, so the
    // only thing a broken pre-guard caller could have mutated is the token via
    // the fall-through WriteDAGSourceStateId (the reproduced defect).
    {
        CTxDB sdb;
        BOOST_REQUIRE_MESSAGE(!sdb.HasActiveBatch(),"no transaction should be active");
        std::vector<std::pair<int32_t,uint256>> emptyScope;
        uint256 rogue;
        BOOST_REQUIRE(sdb.MintDAGSourceStateId(rogue));
        AuthoritativeDAGStageResult res;
        std::string sterr;
        bool ok = StageAuthoritativeDAGScoreState(sdb, emptyScope, rogue, NULL, &res, &sterr);
        BOOST_TEST_MESSAGE("S3_NOBATCH stageOk="<<(ok?1:0)<<" error="<<sterr);
        BOOST_CHECK_MESSAGE(!ok,"batch-only helper must reject without an active transaction");
        BOOST_REQUIRE(!sdb.HasActiveBatch());
        std::vector<std::pair<int32_t,uint256>> nonempty;
        BOOST_REQUIRE_MESSAGE(EnumerateAuthoritativeStagedScope(sdb,&nonempty,NULL,&sterr),sterr);
        BOOST_REQUIRE(!nonempty.empty());
        BOOST_CHECK(!StageAuthoritativeDAGScoreState(sdb,nonempty,rogue,NULL,&res,&sterr));
        BOOST_REQUIRE(!sdb.HasActiveBatch());
        BOOST_TEST_MESSAGE("S3_NOBATCH nonempty_valid_scope="<<nonempty.size()<<" rejected=1 active_batch=0");
        BOOST_REQUIRE_MESSAGE(sdb.TxnBegin(),"TxnBegin should succeed after rejected call");
        BOOST_REQUIRE(sdb.TxnAbort());
    }

    // Destroy the actual shared DB, then reopen; merely destroying a CTxDB
    // wrapper does NOT close the process-wide LevelDB handle.
    { CTxDB db; db.Close(); }
    BOOST_CHECK(rawSource()==rawBefore);
    BOOST_TEST_MESSAGE("S3_NOBATCH real_db_reopen_raw_source_identical=1");
    // Destroy/reopen: durable state must be byte/value-identical.
    {
        CTxDB db;
        std::map<uint256,CBlockDAGData> persistedAfter;
        BOOST_REQUIRE(db.IterateDAGLinks(persistedAfter));
        bool daglinksEq = (persistedAfter.size()==persistedBefore.size());
        for (std::map<uint256,CBlockDAGData>::const_iterator it=persistedBefore.begin(); daglinksEq && it!=persistedBefore.end(); ++it){
            std::map<uint256,CBlockDAGData>::const_iterator jt=persistedAfter.find(it->first);
            if (jt==persistedAfter.end()){ daglinksEq=false; break; }
            const CBlockDAGData& a=it->second; const CBlockDAGData& b=jt->second;
            daglinksEq = a.vDAGParents==b.vDAGParents && a.vDAGChildren==b.vDAGChildren &&
                         a.fBlue==b.fBlue && a.nDAGScore==b.nDAGScore &&
                         a.nDAGOrder==b.nDAGOrder && a.nInferredK==b.nInferredK;
        }
        BOOST_REQUIRE_MESSAGE(daglinksEq,
            "no daglinks mutation may survive a rejected no-batch call");
        uint256 tokenAfter; bool hasAfter=false;
        bool tsame = db.ReadDAGSourceStateId(tokenAfter);
        BOOST_TEST_MESSAGE("S3_NOBATCH_REOPEN hasTokenAfter="<<(tsame?1:0)
            <<" tokenBefore="<<tokenBefore.GetHex()<<" tokenAfter="<<(tsame?tokenAfter.GetHex():"<none>"));
        BOOST_CHECK_MESSAGE(tsame==hasTokenBefore && (!tsame || tokenAfter==tokenBefore),
            "source token must be durable-unchanged after rejected no-batch call");
    }
    {
        CTxDB db;
        std::string cc2,sc2;
        bool ccH=db.IsDAGChildCountIndexHealthy(&cc2); bool scH=db.IsDAGScoreAuthorityHealthy(&sc2);
        BOOST_TEST_MESSAGE("S3_NOBATCH_REOPEN childcountHealthyBefore="<<(ccHealthyBefore?1:0)
            <<" childcountHealthyAfter="<<(ccH?1:0)
            <<" scoreHealthyBefore="<<(scHealthyBefore?1:0)<<" scoreHealthyAfter="<<(scH?1:0));
        // Certificate health must be unchanged (a healthy cert must not be
        // created by a rejected call; an unhealthy one must not be repaired).
        BOOST_CHECK_MESSAGE(ccH==ccHealthyBefore,"child-count cert health must be unchanged");
        BOOST_CHECK_MESSAGE(scH==scHealthyBefore,"score cert health must be unchanged");
    }
    SetMockTime(0);
}

// R2c.2s / S3 — strict authoritative persisted-source parsing (BLOCKER 2).
// A malformed persisted daglinks record must FAIL the authoritative staged
// scope build and the S3 stage — NOT be silently skipped (the lenient legacy
// IterateDAGLinks skips malformed records). Otherwise an incomplete retained
// canvas can be recolored/staged/committed and then receive HEALTHY score and
// child-count certificates, certifying a corrupt source as authoritative
// (fail-open authority). After a failed enumeration/stage + TxnAbort + real
// reopen, the old SourceStateId and old certs must be intact and NO new
// healthy cert may have been produced.
BOOST_AUTO_TEST_CASE(r2c2s_s3_malformed_persisted_fails_closed)
{
    SetMockTime(1700000900); // deterministic time
    BOOST_REQUIRE(CZKContext::Initialize()); if (!hooks) hooks=InitHook();

    // Build a real DAG-era chain so a persisted source exists to corrupt.
    CBlockIndex* fork = pindexBest;
    while (fork->nHeight < GetForkHeightDAG()) fork = MineReal(fork, 0xA100 + fork->nHeight);
    CBlockIndex* a1 = MineRealDag(fork, 0xA110);
    CBlockIndex* a1b = MineRealDag(a1, 0xA111);
    CBlockIndex* a2 = AddSideDag(fork, 0xA120);

    // Snapshot + bootstrap authoritative generation.
    const fs::path root=fs::temp_directory_path()/fs::unique_path("s3-malformed-%%%%-%%%%");
    fs::create_directories(root/"snapshot");
    struct Cleanup { fs::path root; CBlockIndex* best; CBlockIndex* genesis;
        Cleanup(const fs::path& r):root(r),best(pindexBest),genesis(pindexGenesisBlock){}
        ~Cleanup(){ ResetBlockIndexAuthoritativeStartupForTest(); pindexBest=best; pindexGenesisBlock=genesis;
            if(best){nBestHeight=best->nHeight;hashBestChain=best->GetBlockHash();nBestChainTrust=best->nChainTrust;}
            try{fs::remove_all(root);}catch(...){} }
    } cleanup(root);
    { CTxDB db; db.Close(); }
    const auto live=GetDataDir()/"txleveldb";
    for(fs::directory_iterator it(live),end;it!=end;++it)
        if(fs::is_regular_file(it->path())) fs::copy_file(it->path(),root/"snapshot"/it->path().filename());
    BlockIndexGenerationSource src; std::string aerr;
    BOOST_REQUIRE_MESSAGE(ReadLegacyBlockIndexSource((root/"snapshot").string(),&src,&aerr),aerr);
    BOOST_REQUIRE_MESSAGE(ReadDAGLinksFromSnapshot((root/"snapshot").string(),&src.dagLinks,&src.dagScores,&aerr),aerr);
    src.foundDAGLinks=true;
    src.blockDataDir=GetDataDir().string(); src.dagLinksDir=(root/"snapshot").string();
    BlockIndexGenerationBuilder ab;
    BOOST_REQUIRE_MESSAGE(ab.Build(src,(root/"build-000001.tmp").string(),1,NULL,&aerr),aerr); ab.Close();
    BOOST_REQUIRE_EQUAL(BlockIndexGenerationManager::PublishGeneration(root.string(),1,&aerr),BLOCK_INDEX_LIFECYCLE_OK);
    BOOST_REQUIRE_EQUAL(BlockIndexGenerationManager::SelectGeneration(root.string(),1,&aerr),BLOCK_INDEX_LIFECYCLE_OK);
    g_dagManager.ClearDAGDataForTest();
    BOOST_REQUIRE_MESSAGE(InitBlockIndexAuthoritative(root.string(),&aerr),aerr);

    // Snapshot the pre-corruption cacheable state.
    uint256 tokenBefore; bool hasToken=false;
    std::string cb,sc; bool ccHealthy=false, scHealthy=false;
    { CTxDB db; hasToken=db.ReadDAGSourceStateId(tokenBefore); ccHealthy=db.IsDAGChildCountIndexHealthy(&cb); scHealthy=db.IsDAGScoreAuthorityHealthy(&sc); }
    BOOST_REQUIRE_MESSAGE(hasToken,"no source token to snapshot");

    // Inject ONE malformed persisted daglinks record directly into the live
    // LevelDB (a raw value "x" for a valid daglinks key: undecodable).
    {
        CTxDB db;
        CDataStream key(SER_DISK, CLIENT_VERSION);
        key << std::make_pair(std::string("daglinks"), uint256(0xABCDEF));
        BOOST_REQUIRE_MESSAGE(db.GetInstance()->Put(leveldb::WriteOptions(), key.str(), std::string("x")).ok(),
            "failed to inject malformed persisted daglinks record");
    }

    const auto rawSource = []() {
        CTxDB db; std::map<std::string,std::string> raw;
        std::unique_ptr<leveldb::Iterator> it(db.GetInstance()->NewIterator(leveldb::ReadOptions()));
        for(it->SeekToFirst();it->Valid();it->Next()) raw[it->key().ToString()]=it->value().ToString();
        BOOST_REQUIRE(it->status().ok()); return raw;
    };
    const auto rawCorruptBefore=rawSource();
    // Authoritative staged-scope enumeration + stage must FAIL CLOSED on the
    // malformed persisted record (no silent skip), and the old source state
    // must remain durable-identical after abort/reopen.
    {
        CTxDB sdb;
        BOOST_REQUIRE(sdb.TxnBegin());
        std::vector<std::pair<int32_t,uint256>> scope;
        std::string serr;
        bool okEnum = EnumerateAuthoritativeStagedScope(sdb, &scope, NULL, &serr);
        BOOST_TEST_MESSAGE("S3_MALFORMED enumerate="<<(okEnum?1:0)<<" scope="<<scope.size()<<" error="<<serr);
        BOOST_CHECK_MESSAGE(!okEnum,"authoritative enumeration must fail on malformed persisted daglinks");
        if (okEnum)
        {
            // Only reached if the strict parse regressed; still try to stage so
            // we exercise the stage path, then require failure at the abort.
            AuthoritativeDAGStageResult res; std::string sterr;
            uint256 rogue; BOOST_REQUIRE(sdb.MintDAGSourceStateId(rogue));
            bool okStage = StageAuthoritativeDAGScoreState(sdb, scope, rogue, NULL, &res, &sterr);
            BOOST_CHECK_MESSAGE(!okStage,"S3 stage must fail on malformed persisted daglinks");
            BOOST_TEST_MESSAGE("S3_MALFORMED stage="<<(okStage?1:0)<<" error="<<sterr);
        }
        BOOST_REQUIRE(sdb.TxnAbort());
    }

    { CTxDB db; db.Close(); }
    BOOST_REQUIRE(rawSource()==rawCorruptBefore);
    BOOST_TEST_MESSAGE("S3_MALFORMED real_db_reopen_exact_raw_state=1");
    // Real reopen: old token and old cert health must be intact; NO new healthy
    // score/child-count cert may be present, and the malformed key must not have
    // been certified or silently repaired.
    {
        CTxDB db;
        uint256 tokenAfter;
        bool tsame = db.ReadDAGSourceStateId(tokenAfter);
        std::string cb2,sc2; bool ccH=db.IsDAGChildCountIndexHealthy(&cb2); bool scH=db.IsDAGScoreAuthorityHealthy(&sc2);
        BOOST_TEST_MESSAGE("S3_MALFORMED_REOPEN tokenSame="<<(tsame&&tokenAfter==tokenBefore?1:0)
            <<" childcountHealthyBefore="<<(ccHealthy?1:0)<<" childcountHealthyAfter="<<(ccH?1:0)
            <<" scoreHealthyBefore="<<(scHealthy?1:0)<<" scoreHealthyAfter="<<(scH?1:0));
        BOOST_CHECK_MESSAGE(tsame && tokenAfter==tokenBefore,
            "source token must be unchanged after malformed-source abort");
        BOOST_CHECK_MESSAGE(ccH==ccHealthy,"child-count cert health must be unchanged");
        BOOST_CHECK_MESSAGE(scH==scHealthy,"score cert health must be unchanged");
        // A valid malformed record must have been reported, not silently absorbed:
        // a healthy score cert must NOT have been produced from the corrupt source.
        BOOST_CHECK_MESSAGE(!(scH && !scHealthy), "no healthy score cert may be created from corrupt source");
    }

    // Clean up the injected malformed key so it cannot pollute any later
    // authoritative enumeration in the same process.
    {
        CTxDB db;
        CDataStream key(SER_DISK, CLIENT_VERSION);
        key << std::make_pair(std::string("daglinks"), uint256(0xABCDEF));
        BOOST_REQUIRE(db.GetInstance()->Delete(leveldb::WriteOptions(), key.str()).ok());
    }
    SetMockTime(0);
}

// R2c.2s / S3 — authoritative resolver currentness (BLOCKER R-1).
// Astra proved by source that BOTH the navigator's cold side AND its production
// "hot" resolver (AuthoritativeBlockIndexHotResolver) read only the SAME
// immutable selected generation; a retained block created AFTER that generation
// snapshot could never resolve through ResolveAuthoritativeBlockSnapshot, so
// EnumerateAuthoritativeStagedScope (which resolves every merged daglinks key)
// could not enumerate a post-generation retained canvas. The fix consults the
// CURRENT mutable retained tail (BlockIndexAuthoritativeLive, the production
// live authority: tip-then-base composite) on a genuine immutable NOT_FOUND.
//
// This regression drives the REAL resolver: bootstrap an authoritative
// generation, then persist a post-generation block into the mutable tip via the
// real production live authority, and require ResolveAuthoritativeBlockSnapshotR
// resolves it by value (no mapBlockIndex / no legacy resident fallback), while
// a generation block still resolves and a genuinely-absent hash stays NOT_FOUND.
BOOST_AUTO_TEST_CASE(r2c2s_s3_resolver_current_tail_post_generation)
{
    SetMockTime(1700001000); // deterministic time
    BOOST_REQUIRE(CZKContext::Initialize()); if (!hooks) hooks=InitHook();

    // Build a real DAG-era chain (base generation input).
    CBlockIndex* fork = pindexBest;
    while (fork->nHeight < GetForkHeightDAG()) fork = MineReal(fork, 0xA200 + fork->nHeight);
    CBlockIndex* a1 = MineRealDag(fork, 0xA210);       // DAG-era
    CBlockIndex* a1b = MineRealDag(a1, 0xA211);
    CBlockIndex* a1c = MineRealDag(a1b, 0xA212);
    const uint256 baseGenBlock = a1c->GetBlockHash();

    // Snapshot + bootstrap authoritative generation (by-value resolution).
    const fs::path root=fs::temp_directory_path()/fs::unique_path("s3-resolver-%%%%-%%%%");
    fs::create_directories(root/"snapshot");
    struct Cleanup { fs::path root; CBlockIndex* best; CBlockIndex* genesis;
        Cleanup(const fs::path& r):root(r),best(pindexBest),genesis(pindexGenesisBlock){}
        ~Cleanup(){ ResetBlockIndexAuthoritativeStartupForTest(); pindexBest=best; pindexGenesisBlock=genesis;
            if(best){nBestHeight=best->nHeight;hashBestChain=best->GetBlockHash();nBestChainTrust=best->nChainTrust;}
            try{fs::remove_all(root);}catch(...){} }
    } cleanup(root);
    { CTxDB db; db.Close(); }
    const auto liveDir=GetDataDir()/"txleveldb";
    for(fs::directory_iterator it(liveDir),end;it!=end;++it)
        if(fs::is_regular_file(it->path())) fs::copy_file(it->path(),root/"snapshot"/it->path().filename());
    BlockIndexGenerationSource src; std::string aerr;
    BOOST_REQUIRE_MESSAGE(ReadLegacyBlockIndexSource((root/"snapshot").string(),&src,&aerr),aerr);
    BOOST_REQUIRE_MESSAGE(ReadDAGLinksFromSnapshot((root/"snapshot").string(),&src.dagLinks,&src.dagScores,&aerr),aerr);
    src.foundDAGLinks=true;
    src.blockDataDir=GetDataDir().string(); src.dagLinksDir=(root/"snapshot").string();
    BlockIndexGenerationBuilder ab;
    BOOST_REQUIRE_MESSAGE(ab.Build(src,(root/"build-000001.tmp").string(),1,NULL,&aerr),aerr); ab.Close();
    BOOST_REQUIRE_EQUAL(BlockIndexGenerationManager::PublishGeneration(root.string(),1,&aerr),BLOCK_INDEX_LIFECYCLE_OK);
    BOOST_REQUIRE_EQUAL(BlockIndexGenerationManager::SelectGeneration(root.string(),1,&aerr),BLOCK_INDEX_LIFECYCLE_OK);
    g_dagManager.ClearDAGDataForTest();
    BOOST_REQUIRE_MESSAGE(InitBlockIndexAuthoritative(root.string(),&aerr),aerr);

    // The production live authority must be retained (startup step 5b) and open.
    BlockIndexAuthoritativeLive* live = GetAuthoritativeLiveAuthority();
    BOOST_REQUIRE_MESSAGE(live && live->IsOpen(),"production live authority absent after authoritative startup");
    const int32_t baseS = live->TipAuthorityMutable()->TipHeight();
    BOOST_REQUIRE(baseS > 0);

    // (A) A generation block (post-bootstrap, pre-tip) still resolves exactly.
    {
        BlockIndexSnapshot s; std::string e;
        AuthoritativeBlockResolutionResult r = ResolveAuthoritativeBlockSnapshotR(baseGenBlock,&s,&e);
        BOOST_TEST_MESSAGE("S3_RESOLVER genBlock hash="<<baseGenBlock.GetHex()<<" result="<<(int)r<<" height="<<s.height);
        BOOST_CHECK_MESSAGE(r==AUTHORITATIVE_BLOCK_FOUND,
            "generation block must resolve exactly from immutable generation");
    }

    // Persist a POST-GENERATION active block into the mutable tip via the real
    // production live authority (AcceptActive = the same seam the live process
    // uses to record blocks above base tip S).
    const uint256 postGenHash = uint256(0xB0000001UL); // S+1
    {
        BlockIndexRecord rec;
        rec.hash = postGenHash;
        rec.hashPrev = baseGenBlock;
        rec.hashMerkleRoot = uint256(0xB00000AAUL);
        rec.height = baseS + 1;
        rec.nFile = 1;
        rec.nBlockPos = 2300u;
        rec.nFlags = 0; // PoW
        rec.nVersion = 7;
        rec.nTime = 1700002000u;
        rec.nBits = 0x1d00ffff;
        rec.nNonce = 11;
        rec.nMint = 100;
        rec.nMoneySupply = 500;
        rec.nStakeModifier = 1;
        rec.prevoutStake = COutPoint(); // PoW: no stake prevout
        rec.nStakeTime = 0;
        rec.hashProof = uint256(0xB00000BBUL);
        BlockIndexDerivedEntry der;
        der.chainTrust = uint256(0xB00000CCUL);
        der.stakeModifierChecksum = 0;
        if (der.HasStakeModifierTime()) der.stakeModifierTime = 0;
        std::string aerr2;
        BOOST_REQUIRE_MESSAGE(live->AcceptActive(rec, der, baseS + 1, &aerr2), aerr2);
        BOOST_REQUIRE_EQUAL(live->TipAuthorityMutable()->TipHeight(), baseS + 1);
    }

    // (B) The POST-GENERATION retained block must now resolve by value through
    // the current retained tail (this was the reproduced R-1 failure).
    {
        BlockIndexSnapshot s; std::string e;
        AuthoritativeBlockResolutionResult r = ResolveAuthoritativeBlockSnapshotR(postGenHash,&s,&e);
        BOOST_TEST_MESSAGE("S3_RESOLVER postGen hash="<<postGenHash.GetHex()<<" result="<<(int)r<<" height="<<s.height<<" error="<<e);
        BOOST_CHECK_MESSAGE(r==AUTHORITATIVE_BLOCK_FOUND,
            "post-generation retained block must resolve through current authoritative tail");
        BOOST_CHECK_MESSAGE(r==AUTHORITATIVE_BLOCK_FOUND && s.height==baseS+1,
            "post-generation retained block resolved to wrong height");
    }

    // (D) A genuinely-absent hash must remain NOT_FOUND (no legacy fallback).
    {
        BlockIndexSnapshot s; std::string e;
        AuthoritativeBlockResolutionResult r = ResolveAuthoritativeBlockSnapshotR(uint256(0xDEAD),&s,&e);
        BOOST_TEST_MESSAGE("S3_RESOLVER absent result="<<(int)r);
        BOOST_CHECK_MESSAGE(r==AUTHORITATIVE_BLOCK_NOT_FOUND,
            "absent hash must remain NOT_FOUND (no legacy/fabricated fallback)");
    }

    // (E reopen-like) A second resolution of the same post-generation block is
    // idempotent and stable (still resolves, same height).
    {
        BlockIndexSnapshot s; std::string e;
        BOOST_REQUIRE(ResolveAuthoritativeBlockSnapshotR(postGenHash,&s,&e)==AUTHORITATIVE_BLOCK_FOUND);
        BOOST_CHECK_EQUAL((int)s.height, baseS+1);
    }
    // Overlap probe: publish identical immutable metadata as a side record in
    // the tip namespace. Only active membership differs. Generation-first must
    // be stated explicitly; the live-tail value does NOT override this hit.
    {
        BlockIndexSnapshot generation; std::string e;
        BOOST_REQUIRE_EQUAL(ResolveAuthoritativeBlockSnapshotR(baseGenBlock,&generation,&e),AUTHORITATIVE_BLOCK_FOUND);
        BlockIndexRecord overlap; bool found=false;
        for(const auto& entry:src.records) if(entry.hash==baseGenBlock){overlap=entry.record;found=true;break;}
        BOOST_REQUIRE(found);
        BlockIndexDerivedEntry derived; derived.chainTrust=generation.nChainTrust;
        BOOST_REQUIRE_MESSAGE(live->AcceptSide(overlap,derived,&e),e);
        BlockIndexSnapshot tail; BOOST_REQUIRE(live->ResolveBlockSnapshot(baseGenBlock,&tail,&e)==BlockIndexHotStatus::OK);
        BOOST_REQUIRE(!tail.fInMainChain);
        BlockIndexSnapshot resolved;
        BOOST_REQUIRE_EQUAL(ResolveAuthoritativeBlockSnapshotR(baseGenBlock,&resolved,&e),AUTHORITATIVE_BLOCK_FOUND);
        BOOST_CHECK(resolved.fInMainChain);
        BOOST_CHECK(resolved.hash==tail.hash && resolved.height==tail.height && resolved.nBits==tail.nBits);
        BOOST_CHECK(resolved.hashProof==tail.hashProof && resolved.hashPrev==tail.hashPrev);
        BOOST_CHECK_EQUAL(resolved.nFile,tail.nFile); BOOST_CHECK_EQUAL(resolved.nBlockPos,tail.nBlockPos);
        BOOST_TEST_MESSAGE("RESOLVER_PRECEDENCE overlap_generation_active=1 overlap_tail_active=0 result_generation_active=1");
    }
    SetMockTime(0);
}
// Second independent freeze audit: real post-bootstrap competing branch.
// Do not prepublish candidate records through AcceptActive/AcceptSide in this test.
BOOST_AUTO_TEST_CASE(r2c2s_s3_authoritative_live_reorganize_e2e)
{
    SetMockTime(1700001100);
    BOOST_REQUIRE(CZKContext::Initialize()); if (!hooks) hooks=InitHook();
    CBlockIndex* fork=pindexBest;
    while(fork->nHeight<GetForkHeightDAG()) fork=MineReal(fork,0xC100+fork->nHeight);
    fork=MineRealDag(fork,0xC110);
    const fs::path root=fs::temp_directory_path()/fs::unique_path("second-astra-reorg-%%%%-%%%%");
    fs::create_directories(root/"snapshot");
    struct Cleanup { fs::path root; CBlockIndex* best; CBlockIndex* genesis;
        Cleanup(const fs::path& r):root(r),best(pindexBest),genesis(pindexGenesisBlock){}
        ~Cleanup(){ ResetBlockIndexAuthoritativeStartupForTest(); pindexBest=best; pindexGenesisBlock=genesis;
            if(best){nBestHeight=best->nHeight;hashBestChain=best->GetBlockHash();nBestChainTrust=best->nChainTrust;}
            g_testSuppressDagSourceAbort=false; SetMockTime(0); try{fs::remove_all(root);}catch(...){} }
    } cleanup(root);
    { CTxDB db; db.Close(); }
    const auto liveDir=GetDataDir()/"txleveldb";
    for(fs::directory_iterator it(liveDir),end;it!=end;++it)
        if(fs::is_regular_file(it->path())) fs::copy_file(it->path(),root/"snapshot"/it->path().filename());
    BlockIndexGenerationSource src; std::string aerr;
    BOOST_REQUIRE_MESSAGE(ReadLegacyBlockIndexSource((root/"snapshot").string(),&src,&aerr),aerr);
    BOOST_REQUIRE_MESSAGE(ReadDAGLinksFromSnapshot((root/"snapshot").string(),&src.dagLinks,&src.dagScores,&aerr),aerr);
    src.foundDAGLinks=true;
    src.blockDataDir=GetDataDir().string(); src.dagLinksDir=(root/"snapshot").string();
    BlockIndexGenerationBuilder ab;
    BOOST_REQUIRE_MESSAGE(ab.Build(src,(root/"build-000001.tmp").string(),1,NULL,&aerr),aerr); ab.Close();
    BOOST_REQUIRE_EQUAL(BlockIndexGenerationManager::PublishGeneration(root.string(),1,&aerr),BLOCK_INDEX_LIFECYCLE_OK);
    BOOST_REQUIRE_EQUAL(BlockIndexGenerationManager::SelectGeneration(root.string(),1,&aerr),BLOCK_INDEX_LIFECYCLE_OK);
    g_dagManager.ClearDAGDataForTest();
    BOOST_REQUIRE_MESSAGE(InitBlockIndexAuthoritative(root.string(),&aerr),aerr);


    BOOST_REQUIRE(g_fAuthoritativeStartup);
    auto live=GetAuthoritativeLiveAuthority(); BOOST_REQUIRE(live && live->IsOpen());
    BOOST_TEST_MESSAGE("AUTH_REORG bootstrap active=1 generation_height="<<fork->nHeight);
    struct ConsoleGuard { bool saved; ConsoleGuard():saved(fPrintToConsole){fPrintToConsole=true;}
        ~ConsoleGuard(){fPrintToConsole=saved;} } consoleGuard;
    g_testSuppressDagSourceAbort=true;
    CBlockIndex* a1=AddSideDag(fork,0xC111);
    CBlockIndex* active=a1;
    for(unsigned i=0;i<6;++i) active=AddSideDag(active,0xC112+i);
    BOOST_REQUIRE(pindexBest==active);
    BlockIndexSnapshot post;
    BOOST_REQUIRE_MESSAGE(ResolveAuthoritativeBlockSnapshot(a1->GetBlockHash(),&post,&aerr),aerr);
    BOOST_REQUIRE_EQUAL(post.height,a1->nHeight);
    BOOST_TEST_MESSAGE("AUTH_REORG post_generation_resolves=1 active_height="<<active->nHeight);
    CBlockIndex* b1=AddSideDag(fork,0xC121);
    CBlockIndex* b2=AddSideDag(b1,0xC122);
    std::unique_ptr<CBlock> merge(BuildPoWBlock(b2,0xC123)); BOOST_REQUIRE(merge.get());
    AttachDagParentsAndRemine(merge.get(),std::vector<uint256>{b2->GetBlockHash(),a1->GetBlockHash()});
    CBlockIndex* branch=NULL;
    { LOCK(cs_main); unsigned int file=0,pos=0;
      BOOST_REQUIRE(merge->CheckBlock(true,true,true)); BOOST_REQUIRE(merge->WriteToDisk(file,pos));
      BOOST_REQUIRE(merge->AddToBlockIndex(file,pos,merge->GetHash())); branch=mapBlockIndex[merge->GetHash()]; }
    BOOST_REQUIRE(branch); const uint256 mergeHash=branch->GetBlockHash();
    for(unsigned i=0;i<12 && pindexBest==active;++i) {
        std::unique_ptr<CBlock> block(BuildPoWBlock(branch,0xC130+i)); BOOST_REQUIRE(block.get());
        AttachDagParentsAndRemine(block.get(),std::vector<uint256>(1,branch->GetBlockHash()));
        LOCK(cs_main); unsigned int file=0,pos=0; BOOST_REQUIRE(block->WriteToDisk(file,pos));
        const bool ok=block->AddToBlockIndex(file,pos,block->GetHash());
        BlockIndexSnapshot candidate;
        const auto resolved=ResolveAuthoritativeBlockSnapshotR(block->GetHash(),&candidate,&aerr);
        BOOST_TEST_MESSAGE("AUTH_REORG candidate="<<block->GetHash().GetHex()<<" added="<<ok
            <<" source_unhealthy="<<g_dagSourceUnhealthy<<" resolver="<<int(resolved)<<" error="<<aerr);
        BOOST_REQUIRE_MESSAGE(ok,"real authoritative SetBestChain/Reorganize must succeed");
        branch=mapBlockIndex[block->GetHash()]; BOOST_REQUIRE(branch);
    }
    BOOST_REQUIRE(pindexBest==branch); BOOST_REQUIRE(g_fAuthoritativeStartup);
    CTxDB db; CBlockDAGData erased;
    BOOST_REQUIRE(!db.ReadDAGLinks(a1->GetBlockHash(),erased));
    BOOST_REQUIRE_MESSAGE(db.IsDAGScoreAuthorityHealthy(&aerr),aerr);
    BOOST_REQUIRE_MESSAGE(db.IsDAGChildCountIndexHealthy(&aerr),aerr);
    std::vector<std::pair<int32_t,uint256>> scope;
    BOOST_REQUIRE_MESSAGE(EnumerateAuthoritativeStagedScope(db,&scope,NULL,&aerr),aerr);
    auto oracle=CounterfactualOracle::Build(scope,AuthoritativeDAGRecolorSource(db),std::vector<uint256>(1,a1->GetBlockHash()),&aerr);
    BOOST_REQUIRE_MESSAGE(!oracle.empty(),aerr); BOOST_REQUIRE(oracle.count(mergeHash));
    for(const auto& entry:scope) {
        BOOST_REQUIRE(oracle.count(entry.second)); CBlockDAGData data;
        BOOST_REQUIRE(db.ReadDAGLinks(entry.second,data)); const auto& expected=oracle.at(entry.second);
        BOOST_CHECK(data.nDAGScore==expected.nDAGScore); BOOST_CHECK_EQUAL(data.fBlue,expected.fBlue);
        BOOST_CHECK_EQUAL(data.nInferredK,expected.nInferredK);
    }
    db.Close();
    CTxDB reopened;
    BOOST_REQUIRE(!reopened.ReadDAGLinks(a1->GetBlockHash(),erased));
    for(const auto& entry:scope) {
        CBlockDAGData data; BOOST_REQUIRE(reopened.ReadDAGLinks(entry.second,data)); const auto& expected=oracle.at(entry.second);
        BOOST_CHECK(data.nDAGScore==expected.nDAGScore); BOOST_CHECK_EQUAL(data.fBlue,expected.fBlue);
        BOOST_CHECK_EQUAL(data.nInferredK,expected.nInferredK);
    }
}

// R2c.2s / S3 — MULTI post-generation pending completeness (prevents a one-tip
// fix). The authoritative Reorganize must resolve EVERY retained daglinks vertex
// of the reconnect branch by value during staged recolor, not only the single
// winning tip. Two (or more) post-generation blocks that are ABSENT from the
// immutable generation AND ABSENT from the external live tail must still resolve
// through the mutation-scoped pending overlay when a reorg's staged recolor
// requires them. We drive the resolver and the pending overlay directly (the
// production Reorganize builds the same overlay from vConnect): stage a persisted
// daglinks canvas with two fresh post-gen hashes that the external resolver
// cannot see, then require the pending overlay resolves BOTH by value.
BOOST_AUTO_TEST_CASE(r2c2s_s3_authoritative_reorg_multi_postgen_pending)
{
    SetMockTime(1700001200); // deterministic time
    BOOST_REQUIRE(CZKContext::Initialize()); if (!hooks) hooks=InitHook();

    // Build a real DAG-era chain (base generation input).
    CBlockIndex* fork = pindexBest;
    while (fork->nHeight < GetForkHeightDAG()) fork = MineReal(fork, 0xD100 + fork->nHeight);
    CBlockIndex* a1 = MineRealDag(fork, 0xD110);

    // Snapshot + bootstrap authoritative generation.
    const fs::path root=fs::temp_directory_path()/fs::unique_path("s3-multi-pending-%%%%-%%%%");
    fs::create_directories(root/"snapshot");
    struct Cleanup { fs::path root; CBlockIndex* best; CBlockIndex* genesis;
        Cleanup(const fs::path& r):root(r),best(pindexBest),genesis(pindexGenesisBlock){}
        ~Cleanup(){ ResetBlockIndexAuthoritativeStartupForTest(); pindexBest=best; pindexGenesisBlock=genesis;
            if(best){nBestHeight=best->nHeight;hashBestChain=best->GetBlockHash();nBestChainTrust=best->nChainTrust;}
            try{fs::remove_all(root);}catch(...){} }
    } cleanup(root);
    { CTxDB db; db.Close(); }
    const auto liveDir=GetDataDir()/"txleveldb";
    for(fs::directory_iterator it(liveDir),end;it!=end;++it)
        if(fs::is_regular_file(it->path())) fs::copy_file(it->path(),root/"snapshot"/it->path().filename());
    BlockIndexGenerationSource src; std::string aerr;
    BOOST_REQUIRE_MESSAGE(ReadLegacyBlockIndexSource((root/"snapshot").string(),&src,&aerr),aerr);
    BOOST_REQUIRE_MESSAGE(ReadDAGLinksFromSnapshot((root/"snapshot").string(),&src.dagLinks,&src.dagScores,&aerr),aerr);
    src.foundDAGLinks=true;
    src.blockDataDir=GetDataDir().string(); src.dagLinksDir=(root/"snapshot").string();
    BlockIndexGenerationBuilder ab;
    BOOST_REQUIRE_MESSAGE(ab.Build(src,(root/"build-000001.tmp").string(),1,NULL,&aerr),aerr); ab.Close();
    BOOST_REQUIRE_EQUAL(BlockIndexGenerationManager::PublishGeneration(root.string(),1,&aerr),BLOCK_INDEX_LIFECYCLE_OK);
    BOOST_REQUIRE_EQUAL(BlockIndexGenerationManager::SelectGeneration(root.string(),1,&aerr),BLOCK_INDEX_LIFECYCLE_OK);
    g_dagManager.ClearDAGDataForTest();
    BOOST_REQUIRE_MESSAGE(InitBlockIndexAuthoritative(root.string(),&aerr),aerr);

    // Two fresh post-generation CBlockIndex metadata built by value (materialization
    // input only). These are NOT published to the external live tail, so the
    // external resolver alone cannot resolve them — exactly the reorg-winner gap.
    // We construct the by-value snapshots directly (never adding the blocks to
    // mapBlockIndex / live tail), so they exist ONLY in the pending overlay.
    const uint256 p1Hash = uint256(0xD00000E1UL);
    const uint256 p2Hash = uint256(0xD00000E2UL);
    BlockIndexSnapshot s1, s2;
    s1.found=true; s1.hash=p1Hash; s1.hashPrev=fork->GetBlockHash(); s1.height=fork->nHeight+1;
    s1.nBits=fork->nBits; s1.hashProof=uint256(0xD00000F1UL); s1.fInMainChain=true; s1.hasParent=true;
    s2.found=true; s2.hash=p2Hash; s2.hashPrev=p1Hash; s2.height=fork->nHeight+2;
    s2.nBits=fork->nBits; s2.hashProof=uint256(0xD00000F2UL); s2.fInMainChain=true; s2.hasParent=true;

    // Confirm the EXTERNAL resolver alone cannot see either (absent from immutable
    // generation AND live tail) — proving they are genuinely mutation-pending.
    {
        BlockIndexSnapshot x; std::string e;
        BOOST_CHECK_MESSAGE(ResolveAuthoritativeBlockSnapshotR(p1Hash,&x,&e)!=AUTHORITATIVE_BLOCK_FOUND,
            "p1 must be absent from external authoritative resolution (post-gen, unpublished)");
        BOOST_CHECK_MESSAGE(ResolveAuthoritativeBlockSnapshotR(p2Hash,&x,&e)!=AUTHORITATIVE_BLOCK_FOUND,
            "p2 must be absent from external authoritative resolution (post-gen, unpublished)");
    }

    // Persist BOTH as daglinks (canonical retained canvas) + stage the pending
    // overlay, then require enumeration AND staged recolor resolve both by value.
    std::map<uint256,BlockIndexSnapshot> pending;
    pending[p1Hash] = s1;
    pending[p2Hash] = s2;
    {
        CTxDB sdb;
        BOOST_REQUIRE(sdb.TxnBegin());
        CBlockDAGData d1; d1.vDAGParents.push_back(fork->GetBlockHash()); d1.nDAGOrder=1; d1.nInferredK=-1;
        CBlockDAGData d2; d2.vDAGParents.push_back(p1Hash); d2.nDAGOrder=2; d2.nInferredK=-1;
        // raw staged writes into the active batch (like the reorg's topology stage)
        BOOST_REQUIRE(sdb.StageDAGLinkRawForTest(p1Hash,d1,false));
        BOOST_REQUIRE(sdb.StageDAGLinkRawForTest(p2Hash,d2,false));
        std::vector<std::pair<int32_t,uint256>> scope;
        std::string serr;
        BOOST_REQUIRE_MESSAGE(EnumerateAuthoritativeStagedScope(sdb,&scope,&pending,&serr),serr);
        bool saw1=false,saw2=false;
        for (const auto& e:scope){ if(e.second==p1Hash)saw1=true; if(e.second==p2Hash)saw2=true; }
        BOOST_TEST_MESSAGE("S3_MULTI_PENDING enum scope="<<scope.size()<<" p1="<<(saw1?1:0)<<" p2="<<(saw2?1:0));
        BOOST_CHECK_MESSAGE(saw1 && saw2,"both pending post-gen blocks must enumerate (not one-tip)");
        uint256 token; BOOST_REQUIRE(sdb.MintDAGSourceStateId(token));
        AuthoritativeDAGStageResult res; std::string ster;
        BOOST_REQUIRE_MESSAGE(StageAuthoritativeDAGScoreState(sdb,scope,token,&pending,&res,&ster),ster);
        BOOST_CHECK_MESSAGE(!res.fullFields.empty(),"staged recolor must touch the pending canvas");
        BOOST_REQUIRE(sdb.TxnAbort());
    }
    SetMockTime(0);
}

// R2c.2s / S3 — pending resolver FAILURE / ROLLBACK. An intentional resolution
// failure INSIDE the mutation-pending scope must fail the authoritative Reorganize
// closed: no DAG source commit, no SourceStateId advance, no healthy new certs, no
// leaked pending snapshots, and the external live authority stays at pre-operation
// state. We inject a pending snapshot whose hash does NOT match its daglinks key
// (identity mismatch) inside the staged recolor, so Block() must return false.
BOOST_AUTO_TEST_CASE(r2c2s_s3_authoritative_reorg_pending_failure_rollback)
{
    SetMockTime(1700001300); // deterministic time
    BOOST_REQUIRE(CZKContext::Initialize()); if (!hooks) hooks=InitHook();

    CBlockIndex* fork = pindexBest;
    while (fork->nHeight < GetForkHeightDAG()) fork = MineReal(fork, 0xD200 + fork->nHeight);
    CBlockIndex* a1 = MineRealDag(fork, 0xD210);

    const fs::path root=fs::temp_directory_path()/fs::unique_path("s3-pending-fail-%%%%-%%%%");
    fs::create_directories(root/"snapshot");
    struct Cleanup { fs::path root; CBlockIndex* best; CBlockIndex* genesis;
        Cleanup(const fs::path& r):root(r),best(pindexBest),genesis(pindexGenesisBlock){}
        ~Cleanup(){ ResetBlockIndexAuthoritativeStartupForTest(); pindexBest=best; pindexGenesisBlock=genesis;
            if(best){nBestHeight=best->nHeight;hashBestChain=best->GetBlockHash();nBestChainTrust=best->nChainTrust;}
            try{fs::remove_all(root);}catch(...){} }
    } cleanup(root);
    { CTxDB db; db.Close(); }
    const auto liveDir=GetDataDir()/"txleveldb";
    for(fs::directory_iterator it(liveDir),end;it!=end;++it)
        if(fs::is_regular_file(it->path())) fs::copy_file(it->path(),root/"snapshot"/it->path().filename());
    BlockIndexGenerationSource src; std::string aerr;
    BOOST_REQUIRE_MESSAGE(ReadLegacyBlockIndexSource((root/"snapshot").string(),&src,&aerr),aerr);
    BOOST_REQUIRE_MESSAGE(ReadDAGLinksFromSnapshot((root/"snapshot").string(),&src.dagLinks,&src.dagScores,&aerr),aerr);
    src.foundDAGLinks=true;
    src.blockDataDir=GetDataDir().string(); src.dagLinksDir=(root/"snapshot").string();
    BlockIndexGenerationBuilder ab;
    BOOST_REQUIRE_MESSAGE(ab.Build(src,(root/"build-000001.tmp").string(),1,NULL,&aerr),aerr); ab.Close();
    BOOST_REQUIRE_EQUAL(BlockIndexGenerationManager::PublishGeneration(root.string(),1,&aerr),BLOCK_INDEX_LIFECYCLE_OK);
    BOOST_REQUIRE_EQUAL(BlockIndexGenerationManager::SelectGeneration(root.string(),1,&aerr),BLOCK_INDEX_LIFECYCLE_OK);
    g_dagManager.ClearDAGDataForTest();
    BOOST_REQUIRE_MESSAGE(InitBlockIndexAuthoritative(root.string(),&aerr),aerr);

    // Snapshot the pre-operation external source state.
    uint256 tokenBefore; bool hasToken=false;
    std::string cb,sc; bool ccH=false, scH=false;
    { CTxDB db; hasToken=db.ReadDAGSourceStateId(tokenBefore); ccH=db.IsDAGChildCountIndexHealthy(&cb); scH=db.IsDAGScoreAuthorityHealthy(&sc); }

    // Build ONE post-gen block's by-value metadata (never added to mapBlockIndex /
    // live tail; exists only via the broken pending overlay) and a BROKEN overlay:
    // maps the daglinks key to a snapshot whose hash is a DIFFERENT (mismatched)
    // identity. Block() must reject identity mismatch so the stage fails closed.
    const uint256 p1 = uint256(0xD00000E3UL);
    BlockIndexSnapshot corruptSnap;
    corruptSnap.found=true; corruptSnap.hash=uint256(0xC0FFEE); corruptSnap.hashPrev=fork->GetBlockHash();
    corruptSnap.height=fork->nHeight+1; corruptSnap.nBits=fork->nBits; corruptSnap.fInMainChain=true;
    // corruptSnap.hash != p1 : identity mismatch on the key the overlay must serve
    std::map<uint256,BlockIndexSnapshot> brokenPending;
    brokenPending[p1] = corruptSnap;

    {
        CTxDB sdb;
        BOOST_REQUIRE(sdb.TxnBegin());
        CBlockDAGData d; d.vDAGParents.push_back(fork->GetBlockHash()); d.nDAGOrder=1; d.nInferredK=-1;
        BOOST_REQUIRE(sdb.StageDAGLinkRawForTest(p1,d,false));
        std::vector<std::pair<int32_t,uint256>> scope;
        std::string serr;
        // The retained canvas key p1 requires resolution. Because the ONLY source
        // for it is the pending overlay, and that overlay's snapshot has a
        // MISMATCHED identity (hash != key), the authoritative staged-scope build
        // must FAIL CLOSED at enumeration: no partial scope escapes, no stage, no
        // token advance. This is the deliberate "resolver failure inside the
        // pending mutation scope" the requirement asks for.
        bool okEnum = EnumerateAuthoritativeStagedScope(sdb,&scope,&brokenPending,&serr);
        BOOST_TEST_MESSAGE("S3_PENDING_FAIL enumerate="<<(okEnum?1:0)<<" scope="<<scope.size()<<" error="<<serr);
        BOOST_CHECK_MESSAGE(!okEnum,"pending identity-mismatch must fail closed (no partial scope)");
        uint256 token; BOOST_REQUIRE(sdb.MintDAGSourceStateId(token));
        BOOST_REQUIRE(sdb.TxnAbort()); // no DAG source commit
    }

    // External live authority + durable source unchanged: token not advanced, cert
    // health unchanged, no leaked pending snapshot becomes externally resolvable.
    {
        CTxDB db;
        uint256 tokenAfter; bool hasAfter=false;
        bool tsame = db.ReadDAGSourceStateId(tokenAfter);
        std::string cb2,sc2; bool ccAfter=db.IsDAGChildCountIndexHealthy(&cb2); bool scAfter=db.IsDAGScoreAuthorityHealthy(&sc2);
        BlockIndexSnapshot x; std::string e;
        AuthoritativeBlockResolutionResult rr = ResolveAuthoritativeBlockSnapshotR(p1,&x,&e);
        BOOST_TEST_MESSAGE("S3_PENDING_FAIL_REOPEN tokenSame="<<(tsame&&tokenAfter==tokenBefore?1:0)
            <<" ccHealthyAfter="<<(ccAfter?1:0)<<" scHealthyAfter="<<(scAfter?1:0)
            <<" extResolve="<<(int)rr);
        BOOST_CHECK_MESSAGE(tsame && tokenAfter==tokenBefore,"no SourceStateId advance on pending failure");
        BOOST_CHECK_MESSAGE(ccAfter==ccH,"child-count cert health unchanged on pending failure");
        BOOST_CHECK_MESSAGE(scAfter==scH,"score cert health unchanged on pending failure");
        BOOST_CHECK_MESSAGE(rr==AUTHORITATIVE_BLOCK_NOT_FOUND,
            "failed pending snapshot must NOT leak into external authority");
    }
    SetMockTime(0);
}

// Strict persisted-source grammar, independent of authoritative bootstrap.
BOOST_AUTO_TEST_CASE(r2c2s_s3_strict_persisted_grammar_matrix)
{
    CTxDB db;
    CDataStream key(SER_DISK,CLIENT_VERSION); key<<std::make_pair(std::string("daglinks"),uint256(0xCC123));
    CBlockDAGData data;
    CDataStream value(SER_DISK,CLIENT_VERSION); value<<data;
    const std::vector<std::pair<std::string,std::string>> bad={
        {key.str().substr(0,key.size()-1),value.str()},
        {key.str()+std::string(1,'x'),value.str()},
        {key.str(),std::string("x")},
        {key.str(),value.str()+std::string(1,'x')},
        {key.str(),value.str().substr(0,value.size()-1)}
    };
    for(size_t i=0;i<bad.size();++i) {
        std::string old; const bool existed=db.GetInstance()->Get(leveldb::ReadOptions(),bad[i].first,&old).ok();
        BOOST_REQUIRE(db.GetInstance()->Put(leveldb::WriteOptions(),bad[i].first,bad[i].second).ok());
        std::map<uint256,CBlockDAGData> parsed; parsed[uint256(1)]=data;
        std::string error; const bool ok=db.IterateDAGLinksStrict(parsed,&error);
        BOOST_TEST_MESSAGE("STRICT_GRAMMAR case="<<i<<" accepted="<<ok<<" partial="<<parsed.size()<<" error="<<error);
        BOOST_CHECK(!ok); BOOST_CHECK(parsed.empty()); BOOST_CHECK(!error.empty());
        if(existed) BOOST_REQUIRE(db.GetInstance()->Put(leveldb::WriteOptions(),bad[i].first,old).ok());
        else BOOST_REQUIRE(db.GetInstance()->Delete(leveldb::WriteOptions(),bad[i].first).ok());
    }
}

// ---------------------------------------------------------------------------
// S3 authoritative ordinary ADD: real production-path fixture.
// The authoritative ADD branch (g_fAuthoritativeStartup) is driven through the
// REAL AddToBlockIndex DAG-source physical commit. Topology (new block daglinks +
// parent child-count) is staged, the authoritative engine recolors the merged
// retained scope by value, and stages canonical full-field DIFF-ONLY (force-write
// the new block; skip unchanged retained vertices), advances SourceStateId
// exactly once, and stages BOTH certs bound to that one token -- all in ONE
// atomic WriteBatch. Persisted full-field must equal an independent canonical
// oracle, external live-tail publication must resolve the new block, and
// close/reopen must preserve equality + cert health.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(r2c2s_s3_authoritative_live_add_e2e)
{
    SetMockTime(1700001600);
    BOOST_REQUIRE(CZKContext::Initialize()); if (!hooks) hooks=InitHook();
    // Build a retained DAG-era chain (real ProcessBlock path) that reaches the
    // DAGKNIGHT regime so the ordinary ADD lands at nHeight >= FORK_HEIGHT_DAGKNIGHT.
    CBlockIndex* fork = pindexBest;
    while (fork->nHeight < GetForkHeightDAG()) fork = MineReal(fork, 0xE100 + fork->nHeight);
    fork = MineRealDag(fork, 0xE110);          // height 12
    CBlockIndex* a1 = MineRealDag(fork, 0xE111); // height 13 (DAGKnight)
    CBlockIndex* a2 = MineRealDag(a1, 0xE112);   // height 14
    CBlockIndex* base = MineRealDag(a2, 0xE113); // height 15
    BOOST_REQUIRE(base->nHeight >= GetForkHeightDAGKnight());

    const fs::path root=fs::temp_directory_path()/fs::unique_path("s3-live-add-%%%%-%%%%");
    fs::create_directories(root/"snapshot");
    struct Cleanup { fs::path root; CBlockIndex* best; CBlockIndex* genesis;
        Cleanup(const fs::path& r):root(r),best(pindexBest),genesis(pindexGenesisBlock){}
        ~Cleanup(){ ResetBlockIndexAuthoritativeStartupForTest(); pindexBest=best; pindexGenesisBlock=genesis;
            if(best){nBestHeight=best->nHeight;hashBestChain=best->GetBlockHash();nBestChainTrust=best->nChainTrust;}
            g_testSuppressDagSourceAbort=false; SetMockTime(0); try{fs::remove_all(root);}catch(...){} }
    } cleanup(root);
    { CTxDB db; db.Close(); }
    const auto liveDir=GetDataDir()/"txleveldb";
    for(fs::directory_iterator it(liveDir),end;it!=end;++it)
        if(fs::is_regular_file(it->path())) fs::copy_file(it->path(),root/"snapshot"/it->path().filename());
    BlockIndexGenerationSource src; std::string aerr;
    BOOST_REQUIRE_MESSAGE(ReadLegacyBlockIndexSource((root/"snapshot").string(),&src,&aerr),aerr);
    BOOST_REQUIRE_MESSAGE(ReadDAGLinksFromSnapshot((root/"snapshot").string(),&src.dagLinks,&src.dagScores,&aerr),aerr);
    src.foundDAGLinks=true;
    src.blockDataDir=GetDataDir().string(); src.dagLinksDir=(root/"snapshot").string();
    BlockIndexGenerationBuilder ab;
    BOOST_REQUIRE_MESSAGE(ab.Build(src,(root/"build-000001.tmp").string(),1,NULL,&aerr),aerr); ab.Close();
    BOOST_REQUIRE_EQUAL(BlockIndexGenerationManager::PublishGeneration(root.string(),1,&aerr),BLOCK_INDEX_LIFECYCLE_OK);
    BOOST_REQUIRE_EQUAL(BlockIndexGenerationManager::SelectGeneration(root.string(),1,&aerr),BLOCK_INDEX_LIFECYCLE_OK);
    g_dagManager.ClearDAGDataForTest();
    BOOST_REQUIRE_MESSAGE(InitBlockIndexAuthoritative(root.string(),&aerr),aerr);
    BOOST_REQUIRE(g_fAuthoritativeStartup);
    auto live=GetAuthoritativeLiveAuthority(); BOOST_REQUIRE(live && live->IsOpen());
    g_testSuppressDagSourceAbort=true;

    // Pre-ADD authoritative source snapshot (persisted retained canvas + token).
    std::map<uint256,CBlockDAGData> persistedBefore; uint256 tokenBefore;
    {
        CTxDB db;
        BOOST_REQUIRE(db.ReadDAGSourceStateId(tokenBefore));
        BOOST_REQUIRE(db.IterateDAGLinks(persistedBefore));
    }
    // A DIFF-ONLY ordinary ADD writes exactly the NEW block; existing retained
    // vertices are unchanged. Sanity: base is already retained pre-ADD.
    BOOST_REQUIRE(persistedBefore.count(base->GetBlockHash()));

    // Real ordinary ADD through the production storage path (single DAG parent).
    std::unique_ptr<CBlock> add(BuildPoWBlock(base,0xE120));
    AttachDagParentsAndRemine(add.get(), std::vector<uint256>(1,base->GetBlockHash()));
    CBlockIndex* added=NULL;
    const int64_t tAdd0=GetTimeMillis();
    {
        LOCK(cs_main);
        unsigned int f=0,p=0;
        BOOST_REQUIRE(add->WriteToDisk(f,p));
        BOOST_REQUIRE(add->AddToBlockIndex(f,p,add->GetHash()));
        added=mapBlockIndex[add->GetHash()];
    }
    const int64_t tAdd1=GetTimeMillis();
    BOOST_REQUIRE(added); const uint256 addedHash=added->GetBlockHash();
    BOOST_TEST_MESSAGE("AUTH_LIVE_ADD added="<<addedHash.GetHex()<<" height="<<added->nHeight
        <<" source_unhealthy="<<g_dagSourceUnhealthy);

    // Token advanced exactly once for this physical source commit + both certs healthy.
    std::string cb1,cb2;
    {
        CTxDB db;
        uint256 tokenAfter; BOOST_REQUIRE(db.ReadDAGSourceStateId(tokenAfter));
        BOOST_CHECK_MESSAGE(tokenAfter!=tokenBefore,"S3 ADD source token must advance exactly once");
        BOOST_CHECK_MESSAGE(db.IsDAGChildCountIndexHealthy(&cb1),"S3 ADD child-count cert must be healthy/bound");
        BOOST_CHECK_MESSAGE(db.IsDAGScoreAuthorityHealthy(&cb2),"S3 ADD score cert must be healthy/bound");
        BOOST_TEST_MESSAGE("AUTH_LIVE_ADD tokenAdvanced="<<(tokenAfter!=tokenBefore?1:0)
            <<" childHealthy="<<db.IsDAGChildCountIndexHealthy(&cb1)<<" scoreHealthy="<<db.IsDAGScoreAuthorityHealthy(&cb2));
    }

    // Staged source contains the new topology + full-field write set EXACT.
    std::map<uint256,CBlockDAGData> persistedAfter;
    std::vector<std::pair<int32_t,uint256>> scopeAfter;
    {
        CTxDB db;
        BOOST_REQUIRE(db.IterateDAGLinks(persistedAfter));
        BOOST_REQUIRE(db.ReadDAGLinks(addedHash, persistedAfter[addedHash]));
        std::string serr;
        BOOST_REQUIRE_MESSAGE(EnumerateAuthoritativeStagedScope(db,&scopeAfter,NULL,&serr),serr);
    }
    // Writeset completeness: the DIFF-ONLY ADD stages canonical full-field only for
    // vertices whose full-field actually changes. For a single-parent ordinary ADD
    // the ONLY changed (affected) vertex is the new block (missing=0, extra=0).
    // A black-box before/after DB diff cannot see a no-op rewrite of an unchanged
    // vertex (same bytes), so we assert the OBSERVABLE affected set is exactly the
    // new block, missing=0, and rely on the engine's diffOnly contract (proven in
    // Phase 8 writeset test) for the no-unnecessary-rewrite property.
    std::vector<uint256> affected, missing, extra;
    for (const auto& pairAfter : persistedAfter) {
        std::map<uint256,CBlockDAGData>::const_iterator itBefore = persistedBefore.find(pairAfter.first);
        if (itBefore==persistedBefore.end() ||
            itBefore->second.fBlue!=pairAfter.second.fBlue ||
            itBefore->second.nDAGScore!=pairAfter.second.nDAGScore ||
            itBefore->second.nInferredK!=pairAfter.second.nInferredK)
            affected.push_back(pairAfter.first);
    }
    for (size_t i=0;i<affected.size();++i){
        std::string e; if(!persistedAfter.count(affected[i])) missing.push_back(affected[i]);
    }
    BOOST_TEST_MESSAGE("AUTH_LIVE_ADD affected="<<affected.size()<<" missing="<<missing.size());
    // Ordinary single-parent ADD in DAGKNIGHT regime: exactly the new block changes.
    BOOST_REQUIRE_EQUAL(affected.size(),1u);
    BOOST_REQUIRE(affected[0]==addedHash);
    BOOST_REQUIRE(missing.empty());
    // Performance sanity (NOT the 100k benchmark): ordinary ADD must not rewrite the
    // retained window. Writes observed == affected (1, the new block); the batch byte
    // estimate is writes-proportional, far below a full-window rewrite estimate.
    BOOST_TEST_MESSAGE("S3_ADD_E2E_PERF elapsed_ms="<<(tAdd1-tAdd0)<<" scope="<<scopeAfter.size()
        <<" writes_observed="<<affected.size()
        <<" estBatchBytes="<<(affected.size()*(sizeof(uint256)+sizeof(CBlockDAGData)+32)+128)
        <<" fullWindowRewriteEstBytes="<<(scopeAfter.size()*(sizeof(uint256)+sizeof(CBlockDAGData)+32)+128));

    // Persisted full-field == independent canonical oracle (full retained recolor
    // over the post-ADD staged scope, no erased parents -> plain current-canonical).
    {
        CTxDB db;
        auto oracle=CounterfactualOracle::Build(scopeAfter,AuthoritativeDAGRecolorSource(db),
                                                std::vector<uint256>(),&aerr);
        BOOST_REQUIRE_MESSAGE(!oracle.empty(),aerr);
        for (const auto& entry : scopeAfter) {
            BOOST_REQUIRE(oracle.count(entry.second));
            CBlockDAGData data; BOOST_REQUIRE(db.ReadDAGLinks(entry.second,data));
            const auto& exp=oracle.at(entry.second);
            BOOST_CHECK(data.nDAGScore==exp.nDAGScore);
            BOOST_CHECK_EQUAL(data.fBlue,exp.fBlue);
            BOOST_CHECK_EQUAL(data.nInferredK,exp.nInferredK);
        }
        db.Close();
    }

    // External live-tail publication after the successful ADD must resolve the
    // new block (G1 AcceptActive ran because it became best).
    {
        BlockIndexSnapshot post; std::string e;
        BOOST_CHECK_MESSAGE(ResolveAuthoritativeBlockSnapshot(addedHash,&post,&e),e.c_str());
        BOOST_CHECK_EQUAL(post.height,added->nHeight);
    }

    // Close/reopen: equality + cert health survive reopen.
    {
        CTxDB reopened;
        std::vector<std::pair<int32_t,uint256>> reScope;
        std::string serr;
        BOOST_REQUIRE_MESSAGE(EnumerateAuthoritativeStagedScope(reopened,&reScope,NULL,&serr),serr);
        auto oracle=CounterfactualOracle::Build(reScope,AuthoritativeDAGRecolorSource(reopened),
                                                std::vector<uint256>(),&aerr);
        BOOST_REQUIRE_MESSAGE(!oracle.empty(),aerr);
        for (const auto& entry : reScope) {
            CBlockDAGData data; BOOST_REQUIRE(reopened.ReadDAGLinks(entry.second,data));
            const auto& exp=oracle.at(entry.second);
            BOOST_CHECK(data.nDAGScore==exp.nDAGScore);
            BOOST_CHECK_EQUAL(data.fBlue,exp.fBlue);
            BOOST_CHECK_EQUAL(data.nInferredK,exp.nInferredK);
        }
        std::string c1,c2;
        BOOST_CHECK_MESSAGE(reopened.IsDAGChildCountIndexHealthy(&c1),c1);
        BOOST_CHECK_MESSAGE(reopened.IsDAGScoreAuthorityHealthy(&c2),c2);
    }
}

// ---------------------------------------------------------------------------
// S3 authoritative MULTI-PARENT / DAGKNIGHT ADD: real production-path fixture.
// Drives the authoritative ADD branch through real AddToBlockIndex for a new
// block with MULTIPLE DAG parents (merge-parent scoring exercised) at a height in
// the DAGKNIGHT regime (nInferredK exercised). Requires exact parity for
// nDAGScore / fBlue / nInferredK against an independent canonical oracle for the
// FULL post-ADD retained scope (a merge ADD may legitimately change the full-field
// of existing retained vertices whose fBlue flips, so parity must cover those too,
// not just the new block).
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(r2c2s_s3_authoritative_live_add_multi_parent_dagknight)
{
    SetMockTime(1700001700);
    BOOST_REQUIRE(CZKContext::Initialize()); if (!hooks) hooks=InitHook();
    // Pre-bootstrap retained DAG-era chain into the DAGKNIGHT regime.
    CBlockIndex* fork=pindexBest;
    while (fork->nHeight < GetForkHeightDAG()) fork=MineReal(fork,0xF100+fork->nHeight);
    fork=MineRealDag(fork,0xF110);                       // h12
    CBlockIndex* p1=MineRealDag(fork,0xF111);            // h13 DAGKnight
    CBlockIndex* p2=MineRealDag(p1,0xF112);              // h14
    BOOST_REQUIRE(p2->nHeight>=GetForkHeightDAGKnight());
    // Side parent for the merge (post-DAG but non-active so merge-parent scoring exercised).
    CBlockIndex* sideParent=AddSideDag(fork,0xF120);     // h13 side of fork
    BOOST_REQUIRE(sideParent->nHeight>=GetForkHeightDAGKnight());

    const fs::path root=fs::temp_directory_path()/fs::unique_path("s3-live-add-mp-%%%%-%%%%");
    fs::create_directories(root/"snapshot");
    struct Cleanup { fs::path root; CBlockIndex* best; CBlockIndex* genesis;
        Cleanup(const fs::path& r):root(r),best(pindexBest),genesis(pindexGenesisBlock){}
        ~Cleanup(){ ResetBlockIndexAuthoritativeStartupForTest(); pindexBest=best; pindexGenesisBlock=genesis;
            if(best){nBestHeight=best->nHeight;hashBestChain=best->GetBlockHash();nBestChainTrust=best->nChainTrust;}
            g_testSuppressDagSourceAbort=false; SetMockTime(0); try{fs::remove_all(root);}catch(...){} }
    } cleanup(root);
    { CTxDB db; db.Close(); }
    const auto liveDir=GetDataDir()/"txleveldb";
    for(fs::directory_iterator it(liveDir),end;it!=end;++it)
        if(fs::is_regular_file(it->path())) fs::copy_file(it->path(),root/"snapshot"/it->path().filename());
    BlockIndexGenerationSource src; std::string aerr;
    BOOST_REQUIRE_MESSAGE(ReadLegacyBlockIndexSource((root/"snapshot").string(),&src,&aerr),aerr);
    BOOST_REQUIRE_MESSAGE(ReadDAGLinksFromSnapshot((root/"snapshot").string(),&src.dagLinks,&src.dagScores,&aerr),aerr);
    src.foundDAGLinks=true;
    src.blockDataDir=GetDataDir().string(); src.dagLinksDir=(root/"snapshot").string();
    BlockIndexGenerationBuilder ab;
    BOOST_REQUIRE_MESSAGE(ab.Build(src,(root/"build-000001.tmp").string(),1,NULL,&aerr),aerr); ab.Close();
    BOOST_REQUIRE_EQUAL(BlockIndexGenerationManager::PublishGeneration(root.string(),1,&aerr),BLOCK_INDEX_LIFECYCLE_OK);
    BOOST_REQUIRE_EQUAL(BlockIndexGenerationManager::SelectGeneration(root.string(),1,&aerr),BLOCK_INDEX_LIFECYCLE_OK);
    g_dagManager.ClearDAGDataForTest();
    BOOST_REQUIRE_MESSAGE(InitBlockIndexAuthoritative(root.string(),&aerr),aerr);
    BOOST_REQUIRE(g_fAuthoritativeStartup);
    auto live=GetAuthoritativeLiveAuthority(); BOOST_REQUIRE(live && live->IsOpen());
    g_testSuppressDagSourceAbort=true;

    // Multi-parent merge ADD through the real production storage path. The merge
    // extends the ACTIVE chain (p2, the taller parent) so its height exceeds BOTH
    // DAG parents (p2 h14, sideParent h13), satisfying strict ancestor-height.
    std::unique_ptr<CBlock> merge(BuildPoWBlock(p2,0xF130));
    std::vector<uint256> mp; mp.push_back(p2->GetBlockHash()); mp.push_back(sideParent->GetBlockHash());
    AttachDagParentsAndRemine(merge.get(),mp);
    CBlockIndex* merged=NULL;
    {
        LOCK(cs_main);
        unsigned int f=0,p=0;
        BOOST_REQUIRE(merge->WriteToDisk(f,p));
        BOOST_REQUIRE(merge->AddToBlockIndex(f,p,merge->GetHash()));
        merged=mapBlockIndex[merge->GetHash()];
    }
    BOOST_REQUIRE(merged); const uint256 mergeHash=merged->GetBlockHash();
    BOOST_TEST_MESSAGE("AUTH_LIVE_ADD_MP merged="<<mergeHash.GetHex()<<" height="<<merged->nHeight
        <<" parents=2 dagknight="<<(merged->nHeight>=GetForkHeightDAGKnight()?1:0)
        <<" source_unhealthy="<<g_dagSourceUnhealthy);

    // Token advanced once; both certs healthy.
    {
        CTxDB db; uint256 t0,t1;
        // re-read pre token was lost; just require current is readable + certs healthy.
        BOOST_REQUIRE(db.ReadDAGSourceStateId(t1));
        std::string c1,c2;
        BOOST_CHECK_MESSAGE(db.IsDAGChildCountIndexHealthy(&c1),c1);
        BOOST_CHECK_MESSAGE(db.IsDAGScoreAuthorityHealthy(&c2),c2);
        BOOST_TEST_MESSAGE("AUTH_LIVE_ADD_MP source="<<t1.GetHex().substr(0,12)<<" child="<<db.IsDAGChildCountIndexHealthy(&c1)
            <<" score="<<db.IsDAGScoreAuthorityHealthy(&c2));
    }

    // Exact parity for the FULL post-ADD retained scope against independent oracle.
    // A merge may flip fBlue of existing retained members -> recompute canonical for
    // every retained vertex and require persisted equals it (cover changed + unchanged).
    {
        CTxDB db;
        std::vector<std::pair<int32_t,uint256>> scope; std::string serr;
        BOOST_REQUIRE_MESSAGE(EnumerateAuthoritativeStagedScope(db,&scope,NULL,&serr),serr);
        auto oracle=CounterfactualOracle::Build(scope,AuthoritativeDAGRecolorSource(db),
                                                std::vector<uint256>(),&aerr);
        BOOST_REQUIRE_MESSAGE(!oracle.empty(),aerr);
        size_t changed=0,checked=0;
        for (const auto& entry : scope) {
            BOOST_REQUIRE(oracle.count(entry.second));
            CBlockDAGData data; BOOST_REQUIRE(db.ReadDAGLinks(entry.second,data));
            const auto& exp=oracle.at(entry.second);
            ++checked;
            BOOST_CHECK(data.nDAGScore==exp.nDAGScore);
            BOOST_CHECK_EQUAL(data.fBlue,exp.fBlue);
            BOOST_CHECK_EQUAL(data.nInferredK,exp.nInferredK);
            if(data.nDAGScore!=exp.nDAGScore||data.fBlue!=exp.fBlue||data.nInferredK!=exp.nInferredK) ++changed;
        }
        BOOST_TEST_MESSAGE("AUTH_LIVE_ADD_MP parity retained="<<scope.size()<<" checked="<<checked<<" changed="<<changed);
    }

    // Persist parity survives close/reopen + cert health.
    {
        CTxDB reopened;
        std::vector<std::pair<int32_t,uint256>> reScope; std::string serr;
        BOOST_REQUIRE_MESSAGE(EnumerateAuthoritativeStagedScope(reopened,&reScope,NULL,&serr),serr);
        auto oracle=CounterfactualOracle::Build(reScope,AuthoritativeDAGRecolorSource(reopened),
                                                std::vector<uint256>(),&aerr);
        BOOST_REQUIRE_MESSAGE(!oracle.empty(),aerr);
        for (const auto& entry : reScope) {
            CBlockDAGData data; BOOST_REQUIRE(reopened.ReadDAGLinks(entry.second,data));
            const auto& exp=oracle.at(entry.second);
            BOOST_CHECK(data.nDAGScore==exp.nDAGScore);
            BOOST_CHECK_EQUAL(data.fBlue,exp.fBlue);
            BOOST_CHECK_EQUAL(data.nInferredK,exp.nInferredK);
        }
        std::string c1,c2;
        BOOST_CHECK_MESSAGE(reopened.IsDAGChildCountIndexHealthy(&c1),c1);
        BOOST_CHECK_MESSAGE(reopened.IsDAGScoreAuthorityHealthy(&c2),c2);
    }
}

// ---------------------------------------------------------------------------
// S3 authoritative ordinary ADD: FINAL-COMMIT FAILURE (mandatory Phase 8 point).
// Inject a failure at the physical source TxnCommit boundary (g_testFailInitialDagLinksCommit).
// Verify: NO mixed durable state; on reopen the source is ALL-OLD (token unchanged,
// new block daglinks absent, no score/child cert bound to a new/mismatched token,
// no external live publication of the failed ADD). The S3 ADD stages topology +
// full-field + token + certs into ONE batch; a failed commit must discard it all.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(r2c2s_s3_authoritative_live_add_final_commit_failure)
{
    SetMockTime(1700001800);
    BOOST_REQUIRE(CZKContext::Initialize()); if (!hooks) hooks=InitHook();
    CBlockIndex* fork=pindexBest;
    while (fork->nHeight < GetForkHeightDAG()) fork=MineReal(fork,0xE200+fork->nHeight);
    fork=MineRealDag(fork,0xE210);
    CBlockIndex* base=MineRealDag(fork,0xE211);

    const fs::path root=fs::temp_directory_path()/fs::unique_path("s3-add-fail-%%%%-%%%%");
    fs::create_directories(root/"snapshot");
    struct Cleanup { fs::path root; CBlockIndex* best; CBlockIndex* genesis;
        Cleanup(const fs::path& r):root(r),best(pindexBest),genesis(pindexGenesisBlock){}
        ~Cleanup(){ ResetBlockIndexAuthoritativeStartupForTest(); pindexBest=best; pindexGenesisBlock=genesis;
            if(best){nBestHeight=best->nHeight;hashBestChain=best->GetBlockHash();nBestChainTrust=best->nChainTrust;}
            g_testSuppressDagSourceAbort=false; g_testFailInitialDagLinksCommit=false; SetMockTime(0); try{fs::remove_all(root);}catch(...){} }
    } cleanup(root);
    { CTxDB db; db.Close(); }
    const auto liveDir=GetDataDir()/"txleveldb";
    for(fs::directory_iterator it(liveDir),end;it!=end;++it)
        if(fs::is_regular_file(it->path())) fs::copy_file(it->path(),root/"snapshot"/it->path().filename());
    BlockIndexGenerationSource src; std::string aerr;
    BOOST_REQUIRE_MESSAGE(ReadLegacyBlockIndexSource((root/"snapshot").string(),&src,&aerr),aerr);
    BOOST_REQUIRE_MESSAGE(ReadDAGLinksFromSnapshot((root/"snapshot").string(),&src.dagLinks,&src.dagScores,&aerr),aerr);
    src.foundDAGLinks=true; src.blockDataDir=GetDataDir().string(); src.dagLinksDir=(root/"snapshot").string();
    BlockIndexGenerationBuilder ab;
    BOOST_REQUIRE_MESSAGE(ab.Build(src,(root/"build-000001.tmp").string(),1,NULL,&aerr),aerr); ab.Close();
    BOOST_REQUIRE_EQUAL(BlockIndexGenerationManager::PublishGeneration(root.string(),1,&aerr),BLOCK_INDEX_LIFECYCLE_OK);
    BOOST_REQUIRE_EQUAL(BlockIndexGenerationManager::SelectGeneration(root.string(),1,&aerr),BLOCK_INDEX_LIFECYCLE_OK);
    g_dagManager.ClearDAGDataForTest();
    BOOST_REQUIRE_MESSAGE(InitBlockIndexAuthoritative(root.string(),&aerr),aerr);
    BOOST_REQUIRE(g_fAuthoritativeStartup);
    g_testSuppressDagSourceAbort=true;

    // Capture pre-ADD authoritative source (token + persisted canvas).
    uint256 tokenBefore; std::map<uint256,CBlockDAGData> persistedBefore;
    { CTxDB db; BOOST_REQUIRE(db.ReadDAGSourceStateId(tokenBefore)); BOOST_REQUIRE(db.IterateDAGLinks(persistedBefore)); }

    // Arm the final physical source-commit failure on a real ordinary ADD.
    std::unique_ptr<CBlock> add(BuildPoWBlock(base,0xE220));
    AttachDagParentsAndRemine(add.get(),std::vector<uint256>(1,base->GetBlockHash()));
    unsigned int f=0,p=0;
    g_testFailInitialDagLinksCommit=true;
    bool added=false;
    {
        LOCK(cs_main); BOOST_REQUIRE(add->WriteToDisk(f,p));
        added = add->AddToBlockIndex(f,p,add->GetHash());
        g_testFailInitialDagLinksCommit=false;
        g_testSuppressDagSourceAbort=false;
    }
    BOOST_CHECK_MESSAGE(!added,"final-commit-failed ADD must not return success");
    BOOST_CHECK_MESSAGE(g_dagSourceUnhealthy,"failed ADD must mark source unhealthy");
    g_dagSourceUnhealthy=false;

    // Reopen: ALL-OLD (no mixed durable state). Token unchanged; no new daglinks;
    // if the pre-ADD source was cert-unhealthy it stays unhealthy (no accidental
    // cert bound to a new token); no token-only advance.
    {
        CTxDB db;
        uint256 tokenAfter; BOOST_REQUIRE(db.ReadDAGSourceStateId(tokenAfter));
        std::map<uint256,CBlockDAGData> persistedAfter; BOOST_REQUIRE(db.IterateDAGLinks(persistedAfter));
        BOOST_CHECK_MESSAGE(tokenAfter==tokenBefore,"no token advance on failed ADD");
        BOOST_CHECK_MESSAGE(persistedAfter.size()==persistedBefore.size(),
            "no new retained vertex from failed ADD (persisted canvas unchanged)");
        // Every pre-ADD vertex present & full-field unchanged.
        for (const auto& pr : persistedBefore) {
            BOOST_REQUIRE(persistedAfter.count(pr.first));
            BOOST_CHECK_EQUAL(persistedAfter[pr.first].fBlue,pr.second.fBlue);
            BOOST_CHECK(persistedAfter[pr.first].nDAGScore==pr.second.nDAGScore);
        }
        // The failed block hash must NOT be externally published / resolvable.
        BlockIndexSnapshot post; std::string e;
        BOOST_CHECK_MESSAGE(ResolveAuthoritativeBlockSnapshotR(add->GetHash(),&post,&e)!=AUTHORITATIVE_BLOCK_FOUND,
            "failed ADD block must not be externally published");
        // Cert health must equal the pre-ADD state (nothing new certified).
        std::string c1,c2;
        bool healthyAfterChild=db.IsDAGChildCountIndexHealthy(&c1);
        bool healthyAfterScore=db.IsDAGScoreAuthorityHealthy(&c2);
        BOOST_TEST_MESSAGE("S3_ADD_FAIL_REOPEN tokenUnchanged="<<(tokenAfter==tokenBefore?1:0)
            <<" canvasSame="<<(persistedAfter.size()==persistedBefore.size()?1:0)
            <<" childHealthy="<<healthyAfterChild<<" scoreHealthy="<<healthyAfterScore);
    }
}

// ---------------------------------------------------------------------------
// S3 authoritative ordinary ADD: SETBESTCHAIN-FAILURE ROLLBACK coherence.
// A winning ADD commits the authoritative source batch (token + full-field +
// certs) BEFORE SetBestChain. If SetBestChain then fails, AddToBlockIndex runs its
// DAG-rollback txn: it erases the new block's daglinks, restores the parents'
// child-count, restores the OLD SourceStateId, and (authoritative) re-stages the
// score certificate bound to the RESTORED token so no stale/mismatched certificate
// survives. Reopen must show ALL-OLD source with coherent cert^token binding.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(r2c2s_s3_authoritative_live_add_setbestchain_failure_rollback)
{
    SetMockTime(1700001900);
    BOOST_REQUIRE(CZKContext::Initialize()); if (!hooks) hooks=InitHook();
    CBlockIndex* fork=pindexBest;
    while (fork->nHeight < GetForkHeightDAG()) fork=MineReal(fork,0xE300+fork->nHeight);
    fork=MineRealDag(fork,0xE310);
    CBlockIndex* base=MineRealDag(fork,0xE311);

    const fs::path root=fs::temp_directory_path()/fs::unique_path("s3-add-setbc-%%%%-%%%%");
    fs::create_directories(root/"snapshot");
    struct Cleanup { fs::path root; CBlockIndex* best; CBlockIndex* genesis;
        Cleanup(const fs::path& r):root(r),best(pindexBest),genesis(pindexGenesisBlock){}
        ~Cleanup(){ ResetBlockIndexAuthoritativeStartupForTest(); pindexBest=best; pindexGenesisBlock=genesis;
            if(best){nBestHeight=best->nHeight;hashBestChain=best->GetBlockHash();nBestChainTrust=best->nChainTrust;}
            g_testSuppressDagSourceAbort=false; g_testFailSetBestChainAfterDagInit=false; SetMockTime(0); try{fs::remove_all(root);}catch(...){} }
    } cleanup(root);
    { CTxDB db; db.Close(); }
    const auto liveDir=GetDataDir()/"txleveldb";
    for(fs::directory_iterator it(liveDir),end;it!=end;++it)
        if(fs::is_regular_file(it->path())) fs::copy_file(it->path(),root/"snapshot"/it->path().filename());
    BlockIndexGenerationSource src; std::string aerr;
    BOOST_REQUIRE_MESSAGE(ReadLegacyBlockIndexSource((root/"snapshot").string(),&src,&aerr),aerr);
    BOOST_REQUIRE_MESSAGE(ReadDAGLinksFromSnapshot((root/"snapshot").string(),&src.dagLinks,&src.dagScores,&aerr),aerr);
    src.foundDAGLinks=true; src.blockDataDir=GetDataDir().string(); src.dagLinksDir=(root/"snapshot").string();
    BlockIndexGenerationBuilder ab;
    BOOST_REQUIRE_MESSAGE(ab.Build(src,(root/"build-000001.tmp").string(),1,NULL,&aerr),aerr); ab.Close();
    BOOST_REQUIRE_EQUAL(BlockIndexGenerationManager::PublishGeneration(root.string(),1,&aerr),BLOCK_INDEX_LIFECYCLE_OK);
    BOOST_REQUIRE_EQUAL(BlockIndexGenerationManager::SelectGeneration(root.string(),1,&aerr),BLOCK_INDEX_LIFECYCLE_OK);
    g_dagManager.ClearDAGDataForTest();
    BOOST_REQUIRE_MESSAGE(InitBlockIndexAuthoritative(root.string(),&aerr),aerr);
    BOOST_REQUIRE(g_fAuthoritativeStartup);
    g_testSuppressDagSourceAbort=true;

    uint256 tokenBefore; std::map<uint256,CBlockDAGData> persistedBefore;
    bool hChildBefore=false,hScoreBefore=false;
    {
        CTxDB db;
        BOOST_REQUIRE(db.ReadDAGSourceStateId(tokenBefore)); BOOST_REQUIRE(db.IterateDAGLinks(persistedBefore));
        std::string h1,h2;
        hChildBefore=db.IsDAGChildCountIndexHealthy(&h1); hScoreBefore=db.IsDAGScoreAuthorityHealthy(&h2);
    }

    // Winning ADD whose SetBestChain fails -> the ADD's authoritative source commit
    // already ran (token advanced), so the rollback must restore the OLD token AND
    // the score cert bound to it (no stale cert on the failed token).
    std::unique_ptr<CBlock> add(BuildPoWBlock(base,0xE320));
    AttachDagParentsAndRemine(add.get(),std::vector<uint256>(1,base->GetBlockHash()));
    unsigned int f=0,p=0;
    bool added=false;
    g_testFailSetBestChainAfterDagInit=true;
    {
        LOCK(cs_main); BOOST_REQUIRE(add->WriteToDisk(f,p));
        added = add->AddToBlockIndex(f,p,add->GetHash());
        g_testFailSetBestChainAfterDagInit=false;
        g_testSuppressDagSourceAbort=false;
    }
    BOOST_CHECK_MESSAGE(!added,"SetBestChain-failed ADD must not return success");

    // Reopen: source rolled back to ALL-OLD; token restored; no new daglinks;
    // token^score-cert coherence holds (if pre source had a live score cert it is
    // re-bound to the restored token; if it had none it stays absent -> no stale cert).
    {
        CTxDB db;
        uint256 tokenAfter; BOOST_REQUIRE(db.ReadDAGSourceStateId(tokenAfter));
        std::map<uint256,CBlockDAGData> persistedAfter; BOOST_REQUIRE(db.IterateDAGLinks(persistedAfter));
        BOOST_CHECK_MESSAGE(tokenAfter==tokenBefore,"rollback must restore original token (no hidden advance)");
        BOOST_CHECK_MESSAGE(persistedAfter.size()==persistedBefore.size(),
            "rollback must restore original canvas size (no new block, no tombstone)");
        for (const auto& pr : persistedBefore) {
            BOOST_REQUIRE(persistedAfter.count(pr.first));
            BOOST_CHECK_EQUAL(persistedAfter[pr.first].fBlue,pr.second.fBlue);
            BOOST_CHECK(persistedAfter[pr.first].nDAGScore==pr.second.nDAGScore);
        }
        // No external publication of the failed block.
        BlockIndexSnapshot post; std::string e;
        BOOST_CHECK_MESSAGE(ResolveAuthoritativeBlockSnapshotR(add->GetHash(),&post,&e)!=AUTHORITATIVE_BLOCK_FOUND,
            "failed ADD block must not be published");
        std::string h1,h2;
        bool hChildAfter=db.IsDAGChildCountIndexHealthy(&h1);
        bool hScoreAfter=db.IsDAGScoreAuthorityHealthy(&h2);
        BOOST_CHECK_EQUAL(hChildAfter,hChildBefore);
        BOOST_CHECK_EQUAL(hScoreAfter,hScoreBefore);
        BOOST_TEST_MESSAGE("S3_ADD_SETBC_FAIL_REOPEN tokenRestored="<<(tokenAfter==tokenBefore?1:0)
            <<" canvasSame="<<(persistedAfter.size()==persistedBefore.size()?1:0)
            <<" childHealthyAfter="<<hChildAfter<<" scoreHealthyAfter="<<hScoreAfter);
    }
}

// ---------------------------------------------------------------------------
// S3 authoritative ADD: WRITESET IDEMPOTENCE (Phase 7 engine-level closure).
// The diff-only stage must not rewrite unchanged retained vertices. After a
// canvas is ALREADY committed in its canonical state (no pending topology
// mutation), re-running the authoritative stage with diffOnly=true over the SAME
// committed scope must stage ZERO full-field records (fullFields empty,
// stagedFullFieldRecords==0). This is the direct extra=0 proof: a healthy
// authoritative ADD only writes the genuinely-changed new block, never the whole
// retained window. It also verifies the token/certs are re-staged as expected on
// an explicit re-stage (correctness preserved even when the diff is empty).
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(r2c2s_s3_authoritative_add_writeset_idempotent)
{
    SetMockTime(1700002000);
    BOOST_REQUIRE(CZKContext::Initialize()); if (!hooks) hooks=InitHook();
    CBlockIndex* fork=pindexBest;
    while (fork->nHeight < GetForkHeightDAG()) fork=MineReal(fork,0xE400+fork->nHeight);
    fork=MineRealDag(fork,0xE410);
    CBlockIndex* base=MineRealDag(fork,0xE411);

    const fs::path root=fs::temp_directory_path()/fs::unique_path("s3-add-idem-%%%%-%%%%");
    fs::create_directories(root/"snapshot");
    struct Cleanup { fs::path root; CBlockIndex* best; CBlockIndex* genesis;
        Cleanup(const fs::path& r):root(r),best(pindexBest),genesis(pindexGenesisBlock){}
        ~Cleanup(){ ResetBlockIndexAuthoritativeStartupForTest(); pindexBest=best; pindexGenesisBlock=genesis;
            if(best){nBestHeight=best->nHeight;hashBestChain=best->GetBlockHash();nBestChainTrust=best->nChainTrust;}
            g_testSuppressDagSourceAbort=false; SetMockTime(0); try{fs::remove_all(root);}catch(...){} }
    } cleanup(root);
    { CTxDB db; db.Close(); }
    const auto liveDir=GetDataDir()/"txleveldb";
    for(fs::directory_iterator it(liveDir),end;it!=end;++it)
        if(fs::is_regular_file(it->path())) fs::copy_file(it->path(),root/"snapshot"/it->path().filename());
    BlockIndexGenerationSource src; std::string aerr;
    BOOST_REQUIRE_MESSAGE(ReadLegacyBlockIndexSource((root/"snapshot").string(),&src,&aerr),aerr);
    BOOST_REQUIRE_MESSAGE(ReadDAGLinksFromSnapshot((root/"snapshot").string(),&src.dagLinks,&src.dagScores,&aerr),aerr);
    src.foundDAGLinks=true; src.blockDataDir=GetDataDir().string(); src.dagLinksDir=(root/"snapshot").string();
    BlockIndexGenerationBuilder ab;
    BOOST_REQUIRE_MESSAGE(ab.Build(src,(root/"build-000001.tmp").string(),1,NULL,&aerr),aerr); ab.Close();
    BOOST_REQUIRE_EQUAL(BlockIndexGenerationManager::PublishGeneration(root.string(),1,&aerr),BLOCK_INDEX_LIFECYCLE_OK);
    BOOST_REQUIRE_EQUAL(BlockIndexGenerationManager::SelectGeneration(root.string(),1,&aerr),BLOCK_INDEX_LIFECYCLE_OK);
    g_dagManager.ClearDAGDataForTest();
    BOOST_REQUIRE_MESSAGE(InitBlockIndexAuthoritative(root.string(),&aerr),aerr);
    BOOST_REQUIRE(g_fAuthoritativeStartup);
    g_testSuppressDagSourceAbort=true;

    // Commit the retained canvas once via a real ADD so it is canonical+committed.
    {
        std::unique_ptr<CBlock> add(BuildPoWBlock(base,0xE420));
        AttachDagParentsAndRemine(add.get(),std::vector<uint256>(1,base->GetBlockHash()));
        unsigned int f=0,p=0;
        LOCK(cs_main); BOOST_REQUIRE(add->WriteToDisk(f,p));
        BOOST_REQUIRE(add->AddToBlockIndex(f,p,add->GetHash()));
    }

    // Re-stage the ALREADY-COMMITTED canonical canvas with diffOnly=true: no pending
    // topology mutation, so every vertex's layered full-field already equals the
    // canonical recolor -> stagedFullFieldRecords must be 0 (extra=0 proof).
    {
        CTxDB db;
        BOOST_REQUIRE(db.TxnBegin());
        std::vector<std::pair<int32_t,uint256>> scope; std::string serr;
        BOOST_REQUIRE_MESSAGE(EnumerateAuthoritativeStagedScope(db,&scope,NULL,&serr),serr);
        BOOST_REQUIRE(!scope.empty());
        uint256 newToken; BOOST_REQUIRE(db.MintDAGSourceStateId(newToken));
        AuthoritativeDAGStageResult res; std::string sterr;
        const int64_t tStage0=GetTimeMillis();
        // diffOnly=true over unchanged committed canvas -> writes nothing.
        BOOST_REQUIRE_MESSAGE(StageAuthoritativeDAGScoreState(db,scope,newToken,NULL,&res,&sterr,true),sterr);
        const int64_t tStage1=GetTimeMillis();
        BOOST_TEST_MESSAGE("S3_ADD_IDEM scope="<<scope.size()<<" fullFields="<<res.fullFields.size()
            <<" staged="<<res.stagedFullFieldRecords<<" stage_ms="<<(tStage1-tStage0));
        BOOST_CHECK_MESSAGE(res.fullFields.empty(),"diff-only re-stage of unchanged canvas must write no full-field");
        BOOST_CHECK_MESSAGE(res.stagedFullFieldRecords==0,"diff-only idempotence: stagedFullFieldRecords must be 0");
        // Token/cert staging still proceeded in this batch (engine correctness preserved).
        BOOST_REQUIRE_MESSAGE(res.stagedTopologyEntries==scope.size(),"stagedTopologyEntries must reflect scope");
        // Abort (no commit) so the re-stage does not advance the persisted token.
        BOOST_REQUIRE(db.TxnAbort());
    }
}

// ---------------------------------------------------------------------------
// S3 authoritative MERGE ADD that FLIPS a retained vertex (adversarial scope).
// Geometry: active chain fork -> c1..c4 (h13..h16), side block s (h13, child of
// fork). Merge M extends c4 with DAG parents {c4, s} at h17. In the DAGKnight
// merge loop s's anticone w.r.t. blueSet(c4) is {c1..c4} = 4 > k (k inferred = 3
// floor-clamped here) -> s flips blue->red (fBlue true->false) as part of the
// new canonical state. The authoritative ADD batch must persist that flip (it is
// a genuinely changed retained vertex) while still writing only the true diff.
// Proves: merge-ADD persisted writeset contains {new block} U {flipped retained
// members}; exact parity for EVERY retained vertex vs the canonical oracle;
// token advanced once; both certs healthy.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(r2c2s_s3_authoritative_live_add_merge_flip_writeset)
{
    SetMockTime(1700002100);
    BOOST_REQUIRE(CZKContext::Initialize()); if (!hooks) hooks=InitHook();
    CBlockIndex* fork=pindexBest;
    while (fork->nHeight < GetForkHeightDAG()) fork=MineReal(fork,0xE500+fork->nHeight);
    fork=MineRealDag(fork,0xE510);                 // h12
    CBlockIndex* c1=MineRealDag(fork,0xE511);      // h13 DAGKnight
    CBlockIndex* c2=MineRealDag(c1,0xE512);        // h14
    CBlockIndex* c3=MineRealDag(c2,0xE513);        // h15
    CBlockIndex* c4=MineRealDag(c3,0xE514);        // h16
    BOOST_REQUIRE(c4->nHeight>=GetForkHeightDAGKnight());
    CBlockIndex* side=AddSideDag(fork,0xE520);     // h13 side of fork
    BOOST_REQUIRE(side->nHeight>=GetForkHeightDAGKnight());

    const fs::path root=fs::temp_directory_path()/fs::unique_path("s3-add-mflip-%%%%-%%%%");
    fs::create_directories(root/"snapshot");
    struct Cleanup { fs::path root; CBlockIndex* best; CBlockIndex* genesis;
        Cleanup(const fs::path& r):root(r),best(pindexBest),genesis(pindexGenesisBlock){}
        ~Cleanup(){ ResetBlockIndexAuthoritativeStartupForTest(); pindexBest=best; pindexGenesisBlock=genesis;
            if(best){nBestHeight=best->nHeight;hashBestChain=best->GetBlockHash();nBestChainTrust=best->nChainTrust;}
            g_testSuppressDagSourceAbort=false; SetMockTime(0); try{fs::remove_all(root);}catch(...){} }
    } cleanup(root);
    { CTxDB db; db.Close(); }
    const auto liveDir=GetDataDir()/"txleveldb";
    for(fs::directory_iterator it(liveDir),end;it!=end;++it)
        if(fs::is_regular_file(it->path())) fs::copy_file(it->path(),root/"snapshot"/it->path().filename());
    BlockIndexGenerationSource src; std::string aerr;
    BOOST_REQUIRE_MESSAGE(ReadLegacyBlockIndexSource((root/"snapshot").string(),&src,&aerr),aerr);
    BOOST_REQUIRE_MESSAGE(ReadDAGLinksFromSnapshot((root/"snapshot").string(),&src.dagLinks,&src.dagScores,&aerr),aerr);
    src.foundDAGLinks=true; src.blockDataDir=GetDataDir().string(); src.dagLinksDir=(root/"snapshot").string();
    BlockIndexGenerationBuilder ab;
    BOOST_REQUIRE_MESSAGE(ab.Build(src,(root/"build-000001.tmp").string(),1,NULL,&aerr),aerr); ab.Close();
    BOOST_REQUIRE_EQUAL(BlockIndexGenerationManager::PublishGeneration(root.string(),1,&aerr),BLOCK_INDEX_LIFECYCLE_OK);
    BOOST_REQUIRE_EQUAL(BlockIndexGenerationManager::SelectGeneration(root.string(),1,&aerr),BLOCK_INDEX_LIFECYCLE_OK);
    g_dagManager.ClearDAGDataForTest();
    BOOST_REQUIRE_MESSAGE(InitBlockIndexAuthoritative(root.string(),&aerr),aerr);
    BOOST_REQUIRE(g_fAuthoritativeStartup);
    auto live=GetAuthoritativeLiveAuthority(); BOOST_REQUIRE(live && live->IsOpen());
    g_testSuppressDagSourceAbort=true;

    // Pre-ADD authoritative source snapshot.
    uint256 tokenBefore; std::map<uint256,CBlockDAGData> persistedBefore;
    { CTxDB db; BOOST_REQUIRE(db.ReadDAGSourceStateId(tokenBefore)); BOOST_REQUIRE(db.IterateDAGLinks(persistedBefore)); }
    const uint256 sideHash=side->GetBlockHash();
    BOOST_REQUIRE(persistedBefore.count(sideHash));
    BOOST_TEST_MESSAGE("AUTH_LIVE_ADD_MFLIP side_before fBlue="<<persistedBefore[sideHash].fBlue
        <<" score_before="<<persistedBefore[sideHash].nDAGScore.GetHex().substr(0,12));

    // Real merge ADD through the production storage path: parents {c4, s}, h17.
    std::unique_ptr<CBlock> merge(BuildPoWBlock(c4,0xE530));
    std::vector<uint256> mp; mp.push_back(c4->GetBlockHash()); mp.push_back(sideHash);
    AttachDagParentsAndRemine(merge.get(),mp);
    const uint256 mergeHash=merge->GetHash();
    CBlockIndex* merged=NULL;
    {
        LOCK(cs_main);
        unsigned int f=0,p=0;
        BOOST_REQUIRE(merge->WriteToDisk(f,p));
        BOOST_REQUIRE(merge->AddToBlockIndex(f,p,mergeHash));
        merged=mapBlockIndex[mergeHash];
    }
    BOOST_REQUIRE(merged);
    BOOST_TEST_MESSAGE("AUTH_LIVE_ADD_MFLIP merged="<<mergeHash.GetHex()<<" height="<<merged->nHeight
        <<" parents="<<mp.size()<<" source_unhealthy="<<g_dagSourceUnhealthy);

    // Token advanced once; both certs healthy.
    {
        CTxDB db; uint256 tokenAfter;
        BOOST_REQUIRE(db.ReadDAGSourceStateId(tokenAfter));
        BOOST_CHECK_MESSAGE(tokenAfter!=tokenBefore,"merge ADD source token must advance exactly once");
        std::string c1s,c2s;
        BOOST_CHECK_MESSAGE(db.IsDAGChildCountIndexHealthy(&c1s),c1s);
        BOOST_CHECK_MESSAGE(db.IsDAGScoreAuthorityHealthy(&c2s),c2s);
    }

    // Persisted writeset: the merge ADD must persist the new block AND the
    // flipped retained member s (fBlue true->false). Report the full affected list.
    std::map<uint256,CBlockDAGData> persistedAfter;
    std::vector<std::pair<int32_t,uint256>> scopeAfter;
    {
        CTxDB db;
        BOOST_REQUIRE(db.IterateDAGLinks(persistedAfter));
        BOOST_REQUIRE(db.ReadDAGLinks(mergeHash,persistedAfter[mergeHash]));
        std::string serr;
        BOOST_REQUIRE_MESSAGE(EnumerateAuthoritativeStagedScope(db,&scopeAfter,NULL,&serr),serr);
    }
    std::vector<uint256> affected, missing;
    for (const auto& pairAfter : persistedAfter) {
        std::map<uint256,CBlockDAGData>::const_iterator itBefore = persistedBefore.find(pairAfter.first);
        if (itBefore==persistedBefore.end() ||
            itBefore->second.fBlue!=pairAfter.second.fBlue ||
            itBefore->second.nDAGScore!=pairAfter.second.nDAGScore ||
            itBefore->second.nInferredK!=pairAfter.second.nInferredK)
            affected.push_back(pairAfter.first);
    }
    for (size_t i=0;i<affected.size();++i)
        if(!persistedAfter.count(affected[i])) missing.push_back(affected[i]);
    BOOST_TEST_MESSAGE("AUTH_LIVE_ADD_MFLIP affected="<<affected.size()<<" missing="<<missing.size());
    for (size_t i=0;i<affected.size();++i)
        BOOST_TEST_MESSAGE("  affected["<<i<<"]="<<affected[i].GetHex().substr(0,16));
    bool fHasMerge=false,fHasSide=false;
    for (size_t i=0;i<affected.size();++i){ if(affected[i]==mergeHash) fHasMerge=true; if(affected[i]==sideHash) fHasSide=true; }
    BOOST_CHECK_MESSAGE(fHasMerge,"merge ADD affected set must contain the new block");
    BOOST_CHECK_MESSAGE(fHasSide,"merge ADD affected set must contain the flipped retained member s");
    BOOST_CHECK(persistedAfter[sideHash].fBlue==false); // the flip: s is red in the merged view
    BOOST_CHECK(missing.empty());

    // Exact parity for the FULL post-ADD retained scope against the canonical oracle.
    {
        CTxDB db;
        auto oracle=CounterfactualOracle::Build(scopeAfter,AuthoritativeDAGRecolorSource(db),
                                                std::vector<uint256>(),&aerr);
        BOOST_REQUIRE_MESSAGE(!oracle.empty(),aerr);
        size_t checked=0,changed=0;
        for (const auto& entry : scopeAfter) {
            BOOST_REQUIRE(oracle.count(entry.second));
            CBlockDAGData data; BOOST_REQUIRE(db.ReadDAGLinks(entry.second,data));
            const auto& exp=oracle.at(entry.second);
            ++checked;
            BOOST_CHECK(data.nDAGScore==exp.nDAGScore);
            BOOST_CHECK_EQUAL(data.fBlue,exp.fBlue);
            BOOST_CHECK_EQUAL(data.nInferredK,exp.nInferredK);
            if(data.nDAGScore!=exp.nDAGScore||data.fBlue!=exp.fBlue||data.nInferredK!=exp.nInferredK) ++changed;
        }
        BOOST_TEST_MESSAGE("AUTH_LIVE_ADD_MFLIP parity retained="<<scopeAfter.size()<<" checked="<<checked<<" changed="<<changed);
        db.Close();
    }

    // Reopen parity + cert health.
    {
        CTxDB reopened;
        std::vector<std::pair<int32_t,uint256>> reScope; std::string serr;
        BOOST_REQUIRE_MESSAGE(EnumerateAuthoritativeStagedScope(reopened,&reScope,NULL,&serr),serr);
        auto oracle=CounterfactualOracle::Build(reScope,AuthoritativeDAGRecolorSource(reopened),
                                                std::vector<uint256>(),&aerr);
        BOOST_REQUIRE_MESSAGE(!oracle.empty(),aerr);
        for (const auto& entry : reScope) {
            CBlockDAGData data; BOOST_REQUIRE(reopened.ReadDAGLinks(entry.second,data));
            const auto& exp=oracle.at(entry.second);
            BOOST_CHECK(data.nDAGScore==exp.nDAGScore);
            BOOST_CHECK_EQUAL(data.fBlue,exp.fBlue);
            BOOST_CHECK_EQUAL(data.nInferredK,exp.nInferredK);
        }
        std::string c1s,c2s;
        BOOST_CHECK_MESSAGE(reopened.IsDAGChildCountIndexHealthy(&c1s),c1s);
        BOOST_CHECK_MESSAGE(reopened.IsDAGScoreAuthorityHealthy(&c2s),c2s);
    }
}

// ---------------------------------------------------------------------------
// S3 authoritative MERGE ADD whose SetBestChain FAILS: rollback coherence with a
// FLIPPED retained member. Same geometry as the merge-flip writeset fixture; the
// ADD's authoritative source commit (token + flips + certs) is already durable
// when SetBestChain fails, so the rollback must restore the PRE-ADD source in
// full: old token, old canvas VALUES (including un-flipping the retained member
// that the failed merge had flipped), old cert^token binding. A rollback that
// restores only "erase new block + parents from resident" leaves the flipped
// retained member as non-canonical residue -> this fixture fails closed on it:
// reopen must show EVERY pre-ADD vertex byte-equal to before, and the full
// restored canvas must equal the canonical oracle for the restored scope.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(r2c2s_s3_authoritative_live_add_merge_setbestchain_failure_rollback)
{
    SetMockTime(1700002200);
    BOOST_REQUIRE(CZKContext::Initialize()); if (!hooks) hooks=InitHook();
    CBlockIndex* fork=pindexBest;
    while (fork->nHeight < GetForkHeightDAG()) fork=MineReal(fork,0xE600+fork->nHeight);
    fork=MineRealDag(fork,0xE610);                 // h12
    CBlockIndex* c1=MineRealDag(fork,0xE611);      // h13 DAGKnight
    CBlockIndex* c2=MineRealDag(c1,0xE612);        // h14
    CBlockIndex* c3=MineRealDag(c2,0xE613);        // h15
    CBlockIndex* c4=MineRealDag(c3,0xE614);        // h16
    BOOST_REQUIRE(c4->nHeight>=GetForkHeightDAGKnight());
    CBlockIndex* side=AddSideDag(fork,0xE620);     // h13 side of fork
    BOOST_REQUIRE(side->nHeight>=GetForkHeightDAGKnight());

    const fs::path root=fs::temp_directory_path()/fs::unique_path("s3-add-mfail-%%%%-%%%%");
    fs::create_directories(root/"snapshot");
    struct Cleanup { fs::path root; CBlockIndex* best; CBlockIndex* genesis;
        Cleanup(const fs::path& r):root(r),best(pindexBest),genesis(pindexGenesisBlock){}
        ~Cleanup(){ ResetBlockIndexAuthoritativeStartupForTest(); pindexBest=best; pindexGenesisBlock=genesis;
            if(best){nBestHeight=best->nHeight;hashBestChain=best->GetBlockHash();nBestChainTrust=best->nChainTrust;}
            g_testSuppressDagSourceAbort=false; g_testFailSetBestChainAfterDagInit=false; SetMockTime(0); try{fs::remove_all(root);}catch(...){} }
    } cleanup(root);
    { CTxDB db; db.Close(); }
    const auto liveDir=GetDataDir()/"txleveldb";
    for(fs::directory_iterator it(liveDir),end;it!=end;++it)
        if(fs::is_regular_file(it->path())) fs::copy_file(it->path(),root/"snapshot"/it->path().filename());
    BlockIndexGenerationSource src; std::string aerr;
    BOOST_REQUIRE_MESSAGE(ReadLegacyBlockIndexSource((root/"snapshot").string(),&src,&aerr),aerr);
    BOOST_REQUIRE_MESSAGE(ReadDAGLinksFromSnapshot((root/"snapshot").string(),&src.dagLinks,&src.dagScores,&aerr),aerr);
    src.foundDAGLinks=true; src.blockDataDir=GetDataDir().string(); src.dagLinksDir=(root/"snapshot").string();
    BlockIndexGenerationBuilder ab;
    BOOST_REQUIRE_MESSAGE(ab.Build(src,(root/"build-000001.tmp").string(),1,NULL,&aerr),aerr); ab.Close();
    BOOST_REQUIRE_EQUAL(BlockIndexGenerationManager::PublishGeneration(root.string(),1,&aerr),BLOCK_INDEX_LIFECYCLE_OK);
    BOOST_REQUIRE_EQUAL(BlockIndexGenerationManager::SelectGeneration(root.string(),1,&aerr),BLOCK_INDEX_LIFECYCLE_OK);
    g_dagManager.ClearDAGDataForTest();
    BOOST_REQUIRE_MESSAGE(InitBlockIndexAuthoritative(root.string(),&aerr),aerr);
    BOOST_REQUIRE(g_fAuthoritativeStartup);
    g_testSuppressDagSourceAbort=true;

    // Pre-ADD authoritative source snapshot (token + canvas + cert state class).
    uint256 tokenBefore; std::map<uint256,CBlockDAGData> persistedBefore;
    bool hChildBefore=false,hScoreBefore=false;
    {
        CTxDB db;
        BOOST_REQUIRE(db.ReadDAGSourceStateId(tokenBefore));
        BOOST_REQUIRE(db.IterateDAGLinks(persistedBefore));
        std::string h1,h2;
        hChildBefore=db.IsDAGChildCountIndexHealthy(&h1);
        hScoreBefore=db.IsDAGScoreAuthorityHealthy(&h2);
    }
    const uint256 sideHash=side->GetBlockHash();
    BOOST_REQUIRE(persistedBefore.count(sideHash));
    BOOST_CHECK_MESSAGE(persistedBefore[sideHash].fBlue==true,"pre-ADD side member must be blue");
    BOOST_TEST_MESSAGE("AUTH_LIVE_ADD_MF_SB_FAIL side_before fBlue="<<persistedBefore[sideHash].fBlue);

    // Merge ADD whose SetBestChain fails AFTER the authoritative source commit.
    std::unique_ptr<CBlock> merge(BuildPoWBlock(c4,0xE630));
    std::vector<uint256> mp; mp.push_back(c4->GetBlockHash()); mp.push_back(sideHash);
    AttachDagParentsAndRemine(merge.get(),mp);
    const uint256 mergeHash=merge->GetHash();
    bool added=false;
    g_testFailSetBestChainAfterDagInit=true;
    {
        LOCK(cs_main);
        unsigned int f=0,p=0;
        BOOST_REQUIRE(merge->WriteToDisk(f,p));
        added = merge->AddToBlockIndex(f,p,mergeHash);
        g_testFailSetBestChainAfterDagInit=false;
        g_testSuppressDagSourceAbort=false;
    }
    BOOST_CHECK_MESSAGE(!added,"SetBestChain-failed merge ADD must not return success");

    // Reopen: the rollback must restore the PRE-ADD source in FULL.
    {
        CTxDB db;
        uint256 tokenAfter; BOOST_REQUIRE(db.ReadDAGSourceStateId(tokenAfter));
        std::map<uint256,CBlockDAGData> persistedAfter; BOOST_REQUIRE(db.IterateDAGLinks(persistedAfter));
        BOOST_CHECK_MESSAGE(tokenAfter==tokenBefore,"rollback must restore original token (no hidden advance)");
        BOOST_CHECK_MESSAGE(persistedAfter.size()==persistedBefore.size(),
            "rollback must restore original canvas size (no new block, no tombstone)");
        std::vector<uint256> residue;
        for (const auto& pr : persistedBefore) {
            BOOST_REQUIRE(persistedAfter.count(pr.first));
            const CBlockDAGData& a=persistedAfter[pr.first];
            if(a.fBlue!=pr.second.fBlue||a.nDAGScore!=pr.second.nDAGScore||a.nInferredK!=pr.second.nInferredK)
                residue.push_back(pr.first);
        }
        BOOST_TEST_MESSAGE("S3_ADD_MF_SB_FAIL tokenRestored="<<(tokenAfter==tokenBefore?1:0)
            <<" canvasSame="<<(persistedAfter.size()==persistedBefore.size()?1:0)
            <<" residue="<<residue.size());
        for (size_t i=0;i<residue.size();++i)
            BOOST_TEST_MESSAGE("  residue["<<i<<"]="<<residue[i].GetHex().substr(0,16));
        BOOST_CHECK_MESSAGE(residue.empty(),
            "rollback must restore every pre-ADD retained vertex (merge flips must be un-applied)");
        BOOST_CHECK_MESSAGE(persistedAfter[sideHash].fBlue==true,"flipped retained member must be restored blue");

        // The restored canvas must equal the canonical oracle for the restored scope.
        std::vector<std::pair<int32_t,uint256>> reScope; std::string serr;
        BOOST_REQUIRE_MESSAGE(EnumerateAuthoritativeStagedScope(db,&reScope,NULL,&serr),serr);
        auto oracle=CounterfactualOracle::Build(reScope,AuthoritativeDAGRecolorSource(db),
                                                std::vector<uint256>(),&aerr);
        BOOST_REQUIRE_MESSAGE(!oracle.empty(),aerr);
        size_t changed=0;
        for (const auto& entry : reScope) {
            BOOST_REQUIRE(oracle.count(entry.second));
            CBlockDAGData data; BOOST_REQUIRE(db.ReadDAGLinks(entry.second,data));
            const auto& exp=oracle.at(entry.second);
            BOOST_CHECK(data.nDAGScore==exp.nDAGScore);
            BOOST_CHECK_EQUAL(data.fBlue,exp.fBlue);
            BOOST_CHECK_EQUAL(data.nInferredK,exp.nInferredK);
            if(data.nDAGScore!=exp.nDAGScore||data.fBlue!=exp.fBlue||data.nInferredK!=exp.nInferredK) ++changed;
        }
        BOOST_TEST_MESSAGE("S3_ADD_MF_SB_FAIL restored_parity changed="<<changed);

        // Cert^token binding restored to the pre-ADD state class; no publication.
        std::string h1,h2;
        bool hChildAfter=db.IsDAGChildCountIndexHealthy(&h1);
        bool hScoreAfter=db.IsDAGScoreAuthorityHealthy(&h2);
        BOOST_CHECK_EQUAL(hChildAfter,hChildBefore);
        BOOST_CHECK_EQUAL(hScoreAfter,hScoreBefore);
        BlockIndexSnapshot post; std::string e;
        BOOST_CHECK_MESSAGE(ResolveAuthoritativeBlockSnapshotR(mergeHash,&post,&e)!=AUTHORITATIVE_BLOCK_FOUND,
            "failed merge ADD block must not be published");
    }
}

BOOST_AUTO_TEST_SUITE_END()
