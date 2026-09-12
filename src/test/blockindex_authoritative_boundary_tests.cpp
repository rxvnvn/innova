// Standalone authoritative-boundary Boost test executable.
//
// This is a SEPARATE process from the main test suite. It owns its own
// mapBlockIndex and other chain globals, so this test may SAFELY erase
// historical entries to make the parent V2-only (the exact isolation boundary
// the directive requires). It reuses the proven TestingSetup harness (BOOST
// global fixture sets an isolated -datadir before LoadBlockIndex) and the
// trusted G1 generation builder + the E2E genuine-mining helpers.
//
// Closes the sole remaining Window-2 runtime gate:
//   real mined chain 0..S  ->  real-record V2 generation @ S  ->  S erased from
//   mapBlockIndex (V2-only)  ->  genuine mined S+1 through the COMPLETE real
//   ProcessBlock/AcceptBlock/AddToBlockIndex/ConnectBlock/SetBestChain  ->
//   continuation S+2  ->  restart persistence.
#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MODULE Authoritative Boundary E2E
#include <boost/test/unit_test.hpp>

#include "blockindex_generation_builder.h"
#include "blockindex_generation_lifecycle.h"
#include "blockindex_v2_reader.h"
#include "blockindex_authoritative_live.h"
#include "blockindex_authoritative_startup.h"
#include "blockindex_live_acceptance.h"
#include "db.h"
#include "txdb.h"
#include "main.h"
#include "dag.h"
#include "miner.h"
#include "wallet.h"
#include "zkproof.h"
#include "hooks.h"
#include "checkpoints.h"
#include "blockrequesttrace.h"
#include "ui_interface.h"

#include <boost/filesystem.hpp>
#include <cstdio>
#include <string>
#include <vector>

namespace fs = boost::filesystem;

static std::vector<AcceptBlockDAGObserverEvent>* g_acceptBlockDAGEvents = NULL;
static void CollectAcceptBlockDAGEvent(const AcceptBlockDAGObserverEvent& event)
{
    if (g_acceptBlockDAGEvents)
        g_acceptBlockDAGEvents->push_back(event);
}

CWallet* pwalletMain;
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

// ---- proven test fixture (forces isolated datadir before LoadBlockIndex) ----
struct TestingSetupBoundary {
    boost::filesystem::path pathTestData;
    bool fRegTestSaved;
    bool fTestNetSaved;
    TestingSetupBoundary()
        : fRegTestSaved(fRegTest),
          fTestNetSaved(fTestNet)
    {
        fShutdown = false;
        ResetTrackedThreadJoinState();
        pathTestData = boost::filesystem::temp_directory_path() /
            boost::filesystem::unique_path("innova-boundary-%%%%-%%%%-%%%%");
        boost::filesystem::create_directories(pathTestData);
        mapArgs["-datadir"] = pathTestData.string();
        mapArgs["-regtest"] = "1";
        fRegTest = true;
        fTestNet = false;
        fPrintToDebugger = true;
        noui_connect();
        bitdb.MakeMock();
        LoadBlockIndex(true);
        bool fFirstRun;
        pwalletMain = new CWallet("wallet.dat");
        pwalletMain->LoadWallet(fFirstRun);
        RegisterWallet(pwalletMain);
    }
    ~TestingSetupBoundary()
    {
        fShutdown = true;
        JoinTrackedThreads();
        delete pwalletMain;
        pwalletMain = NULL;
        CTxDB txdb;
        txdb.Close();
        bitdb.Flush(true);
        fRegTest = fRegTestSaved;
        fTestNet = fTestNetSaved;
        boost::filesystem::remove_all(pathTestData);
    }
};
BOOST_GLOBAL_FIXTURE(TestingSetupBoundary);

void Shutdown(void*) { exit(0); }
void StartShutdown() { exit(0); }

// ---- genuine PoW mining (real consensus-valid block construction) ----
// `prevHash` overrides pindexPrev->phashBlock: after the harness erases S from
// mapBlockIndex (to make it V2-only), pS->phashBlock dangles (points into a
// destroyed map node). Production delivers blocks from disk/network whose
// hashPrevBlock is the real persisted hash, so we pass S's hash by value here.
static CBlock* BuildRealBlock(CBlockIndex* pindexPrev, unsigned int nExtra,
                              const uint256* prevHash = NULL,
                              const std::vector<uint256>* dagParents = NULL)
{
    CBlock* pblock = CreateNewBlock(pwalletMain, false, NULL, NULL);
    if (!pblock) return NULL;
    pblock->nVersion = 1;
    pblock->nTime = std::max((unsigned int)GetTime(),
                             (unsigned int)(pindexPrev->GetMedianTimePast() + 1));
    pblock->hashPrevBlock = prevHash ? *prevHash : *pindexPrev->phashBlock;
    pblock->vtx[0].vin[0].scriptSig = CScript() << (pindexPrev->nHeight + 1) << nExtra;
    if (dagParents && !dagParents->empty())
    {
        for (std::vector<CTxOut>::iterator out = pblock->vtx[0].vout.begin();
             out != pblock->vtx[0].vout.end(); )
        {
            if (!ExtractDAGParents(out->scriptPubKey).empty())
                out = pblock->vtx[0].vout.erase(out);
            else
                ++out;
        }
        CTxOut dagOut;
        dagOut.nValue = 0;
        dagOut.scriptPubKey = BuildDAGParentScript(*dagParents);
        pblock->vtx[0].vout.push_back(dagOut);
    }
    pblock->hashMerkleRoot = pblock->BuildMerkleTree();
    uint256 hashTarget = CBigNum().SetCompact(pblock->nBits).getuint256();
    while (pblock->GetHash() > hashTarget && pblock->nNonce < 0xffffffff)
        ++pblock->nNonce;
    return pblock;
}

static CBlockIndex* MineReal(CBlockIndex* pindexPrev, unsigned int nExtra,
                             bool* ok, BlockIndexAuthoritativeLive* live,
                             uint256* outHash = NULL, const uint256* prevHash = NULL,
                             const std::vector<uint256>* dagParents = NULL)
{
    *ok = false;
    CBlock* pblock = BuildRealBlock(pindexPrev, nExtra, prevHash, dagParents);
    if (!pblock) return NULL;
    CBlockIndex* pindex = NULL;
    uint256 hash = pblock->GetHash();
    if (outHash) *outHash = hash;
    {
        LOCK(cs_main);
        if (pblock->CheckBlock(true,true,true) && ProcessBlock(NULL, pblock))
        {
            *ok = true;
            // In AUTHORITATIVE mode the accepted block lives in the V2 tip
            // authority, NOT historical mapBlockIndex (AddToBlockIndex persists
            // to V2; historical mapBlockIndex is never repopulated). Retrieve it
            // by value from the tip authority.
            if (live && live->IsOpen())
            {
                BlockIndexHotHandle h;
                std::string err;
                CBlockIndex* m = live->MaterializeParentChain(hash, &h, &err);
                if (m) { pindex = m; }
            }
            if (!pindex)
                pindex = mapBlockIndex[hash];
        }
    }
    delete pblock;
    return pindex;
}

static BlockIndexRecord CBlockIndexToRecord(const CBlockIndex* p, const uint256& h)
{
    BlockIndexRecord r;
    r.hash = h;
    r.hashPrev = (p->pprev ? *p->pprev->phashBlock : uint256(0));
    r.hashMerkleRoot = p->hashMerkleRoot;
    r.height = p->nHeight;
    r.nFile = p->nFile;
    r.nBlockPos = p->nBlockPos;
    r.nFlags = p->nFlags;
    r.nVersion = p->nVersion;
    r.nTime = p->nTime;
    r.nBits = p->nBits;
    r.nNonce = p->nNonce;
    r.nMint = p->nMint;
    r.nMoneySupply = p->nMoneySupply;
    r.nStakeModifier = p->nStakeModifier;
    r.prevoutStake = p->prevoutStake;
    r.nStakeTime = p->nStakeTime;
    return r;
}

struct DirectSeamResult
{
    bool checkOk;
    bool processOk;
    int nDoS;
    uint256 hash;
    std::vector<AcceptBlockDAGObserverEvent> events;
    DirectSeamResult() : checkOk(false), processOk(false), nDoS(0) {}
};

static DirectSeamResult RunDirectSeamCandidate(CBlockIndex* primary,
                                                const uint256& primaryHash,
                                                unsigned int extraNonce,
                                                const std::vector<uint256>& parents,
                                                bool authoritative,
                                                BlockIndexAuthoritativeLive* live,
                                                BlockIndexRecord* candidateRecord = NULL,
                                                bool rebaseHeaderToPrimary = false)
{
    DirectSeamResult result;
    CBlock* block = BuildRealBlock(primary, extraNonce, &primaryHash, &parents);
    BOOST_REQUIRE(block != NULL);
    if (rebaseHeaderToPrimary)
    {
        block->nBits = GetNextTargetRequired(primary, false);
        block->nNonce = 0;
        const uint256 target = CBigNum().SetCompact(block->nBits).getuint256();
        while (block->GetHash() > target && block->nNonce < 0xffffffff)
            ++block->nNonce;
    }
    result.hash = block->GetHash();
    if (candidateRecord)
    {
        CBlockIndex candidateIndex(1, 1, *block);
        candidateIndex.nHeight = primary->nHeight + 1;
        *candidateRecord = CBlockIndexToRecord(&candidateIndex, result.hash);
        candidateRecord->hashPrev = primaryHash;
    }
    const bool savedAuth = ::g_fAuthoritativeStartup;
    g_acceptBlockDAGEvents = authoritative ? &result.events : NULL;
    ::g_fAuthoritativeStartup = authoritative;
    if (authoritative) SetAuthoritativeLiveForTesting(live);
    {
        ScopedAcceptBlockDAGObserver observer(authoritative ? &CollectAcceptBlockDAGEvent : NULL);
        LOCK(cs_main);
        result.checkOk = block->CheckBlock(true, true, true);
        if (result.checkOk)
            result.processOk = ProcessBlock(NULL, block);
        result.nDoS = block->nDoS;
    }
    if (authoritative) ClearAuthoritativeLiveForTesting();
    ::g_fAuthoritativeStartup = savedAuth;
    g_acceptBlockDAGEvents = NULL;
    delete block;
    return result;
}

static bool HasDAGEvent(const DirectSeamResult& result,
                        AcceptBlockDAGObserverEventType type)
{
    for (size_t i = 0; i < result.events.size(); ++i)
        if (result.events[i].type == type) return true;
    return false;
}

static bool HasFoundMetadata(const DirectSeamResult& result,
                             const uint256& hash, int height, bool proofOfStake)
{
    for (size_t i = 0; i < result.events.size(); ++i)
    {
        const AcceptBlockDAGObserverEvent& event = result.events[i];
        if (event.type == ACCEPTBLOCK_DAG_MERGE_PARENT_FOUND &&
            event.hash == hash && event.height == height &&
            event.proofOfStake == proofOfStake)
            return true;
    }
    return false;
}

static BlockIndexRecord BuildRealPosSideRecord(const uint256& parentHash,
                                                CBlockIndex* parentIndex,
                                                int height,
                                                uint256* blockHash)
{
    CTransaction coinbase;
    coinbase.nVersion = 1;
    coinbase.nTime = 1700000000U + (unsigned int)height;
    CTxIn coinbaseIn;
    coinbaseIn.prevout.SetNull();
    coinbaseIn.scriptSig = CScript() << height << 0x5053;
    coinbase.vin.push_back(coinbaseIn);
    coinbase.vout.push_back(CTxOut(0, CScript() << OP_TRUE));

    CTransaction coinstake;
    coinstake.nVersion = 1;
    coinstake.nTime = coinbase.nTime;
    coinstake.vin.push_back(CTxIn(COutPoint(uint256(0xA13602UL), 1)));
    CTxOut marker;
    marker.SetEmpty();
    coinstake.vout.push_back(marker);
    coinstake.vout.push_back(CTxOut(1, CScript() << OP_TRUE));

    CBlock block;
    block.nVersion = 1;
    block.hashPrevBlock = parentHash;
    block.nTime = coinstake.nTime;
    block.nBits = parentIndex->nBits;
    block.nNonce = 1;
    block.vtx.push_back(coinbase);
    block.vtx.push_back(coinstake);
    block.hashMerkleRoot = block.BuildMerkleTree();
    BOOST_REQUIRE(block.IsProofOfStake());

    *blockHash = block.GetHash();
    CBlockIndex index(1, 1, block);
    index.pprev = parentIndex;
    index.nHeight = height;
    BOOST_REQUIRE(index.IsProofOfStake());
    BOOST_REQUIRE(!index.prevoutStake.IsNull());
    BOOST_REQUIRE(index.nStakeTime != 0);
    return CBlockIndexToRecord(&index, *blockHash);
}

static std::string MakeTempDir()
{
    char tmpl[] = "/tmp/innova-boundgen-XXXXXX";
    char* d = mkdtemp(tmpl);
    BOOST_REQUIRE(d != NULL);
    return std::string(d);
}

BOOST_AUTO_TEST_SUITE(blockindex_authoritative_boundary)

// THE decisive runtime gate: a genuinely-mined S+1 crosses the V2-only
// historical boundary through the COMPLETE real consensus path.
BOOST_AUTO_TEST_CASE(boundary_genuine_s1_complete_connectblock)
{
    BOOST_REQUIRE(CZKContext::Initialize());
    if (hooks == NULL) hooks = InitHook();
    BOOST_REQUIRE(pindexBest != NULL);

    const int S = 80; // enough history for exact DAG_MERGE_DEPTH and +1
    std::vector<CBlockIndex*> chain;
    chain.push_back(pindexBest); // genesis
    for (int i = 1; i <= S; ++i)
    {
        bool ok = false;
        CBlockIndex* bi = MineReal(chain.back(), 0x1000 + i, &ok, NULL);
        BOOST_REQUIRE(ok && bi);
        BOOST_REQUIRE(bi->GetBlockHash() == hashBestChain);
        chain.push_back(bi);
    }
    CBlockIndex* pS = chain[S];
    const uint256 sHash = *pS->phashBlock;
    const uint256 coldMergeHash = *chain[S - 2]->phashBlock;
    const uint256 exactDepthHash = *chain[S - DAG_MERGE_DEPTH]->phashBlock;
    const uint256 overflowDepthHash = *chain[S - DAG_MERGE_DEPTH - 1]->phashBlock;
    const uint256 equalHeightHash(0xA1361001UL);
    const uint256 higherHeightHash(0xA1361002UL);
    BlockIndexRecord equalHeightRecord = CBlockIndexToRecord(pS, equalHeightHash);
    equalHeightRecord.hashPrev = sHash;
    equalHeightRecord.height = S + 1;
    BlockIndexRecord higherHeightRecord = equalHeightRecord;
    higherHeightRecord.hash = higherHeightHash;
    higherHeightRecord.hashPrev = equalHeightHash;
    higherHeightRecord.height = S + 2;
    uint256 posMergeHash;
    BlockIndexRecord posMergeRecord = BuildRealPosSideRecord(
        *chain[S - 2]->phashBlock, chain[S - 2], S - 1, &posMergeHash);
    BOOST_REQUIRE(posMergeRecord.nFlags & CBlockIndex::BLOCK_PROOF_OF_STAKE);
    BOOST_REQUIRE(!posMergeRecord.prevoutStake.IsNull());
    printf("BOUNDARY: real chain 0..%d mined; S=%d hash=%s\n",
           S, pS->nHeight, sHash.ToString().c_str());

    // Build a REAL-record V2 generation @ S from the genuine mined CBlockIndex.
    BlockIndexGenerationSource src;
    for (int h = 0; h <= S; ++h)
    {
        BlockIndexGenerationSourceRecord sr;
        sr.hash = *chain[h]->phashBlock;
        sr.record = CBlockIndexToRecord(chain[h], sr.hash);
        src.records.push_back(sr);
    }
    {
        BlockIndexGenerationSourceRecord sr;
        sr.hash = equalHeightHash;
        sr.record = equalHeightRecord;
        src.records.push_back(sr);
    }
    {
        BlockIndexGenerationSourceRecord sr;
        sr.hash = higherHeightHash;
        sr.record = higherHeightRecord;
        src.records.push_back(sr);
    }
    {
        BlockIndexGenerationSourceRecord sr;
        sr.hash = posMergeHash;
        sr.record = posMergeRecord;
        src.records.push_back(sr);
    }
    src.hashBestChain = sHash;
    src.foundBestChain = true;

    CBlockIndex equalLegacy;
    equalLegacy.nHeight = equalHeightRecord.height;
    equalLegacy.pprev = pS;
    CBlockIndex higherLegacy;
    higherLegacy.nHeight = higherHeightRecord.height;
    higherLegacy.pprev = &equalLegacy;
    CBlockIndex posLegacy;
    posLegacy.nHeight = posMergeRecord.height;
    posLegacy.pprev = chain[S - 2];
    posLegacy.nFlags = posMergeRecord.nFlags;
    posLegacy.prevoutStake = posMergeRecord.prevoutStake;
    posLegacy.nStakeTime = posMergeRecord.nStakeTime;
    mapBlockIndex[equalHeightHash] = &equalLegacy;
    equalLegacy.phashBlock = &mapBlockIndex.find(equalHeightHash)->first;
    mapBlockIndex[higherHeightHash] = &higherLegacy;
    higherLegacy.phashBlock = &mapBlockIndex.find(higherHeightHash)->first;
    mapBlockIndex[posMergeHash] = &posLegacy;
    posLegacy.phashBlock = &mapBlockIndex.find(posMergeHash)->first;

    std::vector<uint256> equalParents;
    equalParents.push_back(sHash);
    equalParents.push_back(equalHeightHash);
    std::vector<uint256> higherParents;
    higherParents.push_back(sHash);
    higherParents.push_back(higherHeightHash);
    std::vector<uint256> overflowParents;
    overflowParents.push_back(sHash);
    overflowParents.push_back(overflowDepthHash);
    std::vector<uint256> posParents;
    posParents.push_back(sHash);
    posParents.push_back(posMergeHash);
    std::vector<uint256> exactDepthParents;
    exactDepthParents.push_back(sHash);
    exactDepthParents.push_back(exactDepthHash);

    const bool savedRegTestIbdForMatrix = fRegTestIbd;
    fRegTestIbd = false;
    const DirectSeamResult legacyEqual = RunDirectSeamCandidate(
        pS, sHash, 0x3101, equalParents, false, NULL);
    const DirectSeamResult legacyHigher = RunDirectSeamCandidate(
        pS, sHash, 0x3102, higherParents, false, NULL);
    const DirectSeamResult legacyOverflow = RunDirectSeamCandidate(
        pS, sHash, 0x3103, overflowParents, false, NULL);
    const DirectSeamResult legacyPos = RunDirectSeamCandidate(
        pS, sHash, 0x3104, posParents, false, NULL);
    fRegTestIbd = savedRegTestIbdForMatrix;
    BOOST_REQUIRE(legacyEqual.checkOk && !legacyEqual.processOk);
    BOOST_REQUIRE(legacyHigher.checkOk && !legacyHigher.processOk);
    BOOST_REQUIRE(legacyOverflow.checkOk && !legacyOverflow.processOk);
    BOOST_REQUIRE(legacyPos.checkOk && !legacyPos.processOk);
    BOOST_CHECK_EQUAL(legacyEqual.nDoS, 100);
    BOOST_CHECK_EQUAL(legacyHigher.nDoS, 100);
    BOOST_CHECK_EQUAL(legacyOverflow.nDoS, 50);
    BOOST_CHECK_EQUAL(legacyPos.nDoS, 100);
    mapBlockIndex.erase(equalHeightHash);
    mapBlockIndex.erase(higherHeightHash);
    mapBlockIndex.erase(posMergeHash);

    // Provenance-recovered retained observer fixture.  Keep it on the original
    // chain-only generation: the previously green setup accepted S+1 first,
    // retained that primary in bounded live authority, and only then closed the
    // cold reader for the authority-failure child.
    BlockIndexGenerationSource retainedSrc;
    retainedSrc.records.assign(src.records.begin(), src.records.begin() + S + 1);
    retainedSrc.hashBestChain = sHash;
    retainedSrc.foundBestChain = true;
    fs::path retainedRoot = fs::path(MakeTempDir());
    fs::path retainedTmp = retainedRoot / "build-000001.tmp";
    BlockIndexGenerationBuilder retainedBuilder;
    BlockIndexGenerationStats retainedStats;
    std::string retainedError;
    BOOST_REQUIRE_MESSAGE(retainedBuilder.Build(retainedSrc, retainedTmp.string(), 1,
                                                &retainedStats, &retainedError),
                          retainedError);
    retainedBuilder.Close();
    BOOST_REQUIRE(BlockIndexGenerationManager::PublishGeneration(
                      retainedRoot.string(), 1, &retainedError) == BLOCK_INDEX_LIFECYCLE_OK);
    BOOST_REQUIRE(BlockIndexGenerationManager::SelectGeneration(
                      retainedRoot.string(), 1, &retainedError) == BLOCK_INDEX_LIFECYCLE_OK);

    BlockIndexV2Reader retainedReader;
    BlockIndexV2ReaderOptions retainedOpts;
    BOOST_REQUIRE_MESSAGE(retainedReader.Open(retainedRoot.string(), retainedOpts,
                                              &retainedError), retainedError);
    const int retainedHorizon = 2;
    BlockIndexAuthoritativeLive retainedLive;
    BOOST_REQUIRE_MESSAGE(retainedLive.Open(retainedRoot.string(), &retainedReader,
                                            retainedHorizon, &retainedError), retainedError);

    for (int h = 1; h <= S; ++h)
        mapBlockIndex.erase(*chain[h]->phashBlock);
    BOOST_REQUIRE(mapBlockIndex.count(sHash) == 0);

    std::vector<uint256> retainedColdParents;
    retainedColdParents.push_back(sHash);
    retainedColdParents.push_back(coldMergeHash);
    fRegTestIbd = false;
    BlockIndexRecord retainedPrimaryRecord;
    const DirectSeamResult retainedCold = RunDirectSeamCandidate(
        pS, sHash, 0x3301, retainedColdParents, true, &retainedLive,
        &retainedPrimaryRecord);
    BOOST_REQUIRE(retainedCold.checkOk);
    BOOST_CHECK(HasDAGEvent(retainedCold, ACCEPTBLOCK_DAG_ENTERED));
    BOOST_CHECK(HasFoundMetadata(retainedCold, coldMergeHash, S - 2, false));
    BOOST_CHECK(HasDAGEvent(retainedCold, ACCEPTBLOCK_DAG_MERGE_VALIDATION_PASSED));
    BOOST_CHECK_EQUAL(retainedCold.nDoS, 0);
    BOOST_CHECK(mapBlockIndex.count(coldMergeHash) == 0);

    // The S=80 final fixture intentionally does not require downstream DAG
    // coloring/SetBestChain success. Persist the exact real-block record through
    // the existing bounded-live acceptance seam so the authority-failure child
    // has the same retained-primary topology as the previously green S=10 case.
    BlockIndexDerivedEntry retainedPrimaryDerived;
    CBlockIndex retainedPrimaryIndex;
    retainedPrimaryIndex.nBits = retainedPrimaryRecord.nBits;
    retainedPrimaryDerived.chainTrust = pS->nChainTrust +
        retainedPrimaryIndex.GetBlockTrust();
    BOOST_REQUIRE_MESSAGE(retainedLive.AcceptActive(retainedPrimaryRecord,
                                                     retainedPrimaryDerived,
                                                     S + 1,
                                                     &retainedError),
                          retainedError);

    BlockIndexHotHandle retainedPrimaryHandle;
    CBlockIndex* retainedPrimary = retainedLive.MaterializeParentChain(
        retainedCold.hash, &retainedPrimaryHandle, &retainedError);
    BOOST_REQUIRE_MESSAGE(retainedPrimary != NULL, retainedError);
    BOOST_CHECK_EQUAL(retainedPrimary->nHeight, S + 1);

    const uint256 hotSideHash(0xFACE0001UL);
    BlockIndexRecord hotSideRecord = retainedSrc.records[S].record;
    hotSideRecord.hash = hotSideHash;
    hotSideRecord.hashPrev = sHash;
    hotSideRecord.height = S;
    BlockIndexDerivedEntry hotSideDerived;
    hotSideDerived.chainTrust = uint256(0x1234UL);
    BOOST_REQUIRE_MESSAGE(retainedLive.AcceptSide(hotSideRecord, hotSideDerived,
                                                  &retainedError), retainedError);
    std::vector<uint256> hotSideParents;
    hotSideParents.push_back(sHash);
    hotSideParents.push_back(hotSideHash);
    const DirectSeamResult retainedHotSide = RunDirectSeamCandidate(
        pS, sHash, 0x3302, hotSideParents, true, &retainedLive);
    bool sawInactiveHotSide = false;
    for (size_t eventIndex = 0; eventIndex < retainedHotSide.events.size(); ++eventIndex)
    {
        const AcceptBlockDAGObserverEvent& event = retainedHotSide.events[eventIndex];
        if (event.type == ACCEPTBLOCK_DAG_MERGE_PARENT_FOUND &&
            event.hash == hotSideHash && event.height == S &&
            !event.proofOfStake && !event.active)
            sawInactiveHotSide = true;
    }
    BOOST_REQUIRE(retainedHotSide.checkOk);
    BOOST_CHECK(HasDAGEvent(retainedHotSide, ACCEPTBLOCK_DAG_ENTERED));
    BOOST_CHECK(sawInactiveHotSide);
    BOOST_CHECK(HasDAGEvent(retainedHotSide, ACCEPTBLOCK_DAG_MERGE_VALIDATION_PASSED));
    BOOST_CHECK_EQUAL(retainedHotSide.nDoS, 0);
    BOOST_CHECK(mapBlockIndex.count(hotSideHash) == 0);

    const uint256 unknownParent(0xBAD10001UL);
    std::vector<uint256> unknownParents;
    unknownParents.push_back(sHash);
    unknownParents.push_back(unknownParent);
    fRegTestIbd = true;
    const DirectSeamResult retainedIbdUnknown = RunDirectSeamCandidate(
        pS, sHash, 0x3303, unknownParents, true, &retainedLive);
    fRegTestIbd = false;
    const DirectSeamResult retainedNonIbdUnknown = RunDirectSeamCandidate(
        pS, sHash, 0x3304, unknownParents, true, &retainedLive);
    fRegTestIbd = savedRegTestIbdForMatrix;

    BOOST_REQUIRE(retainedIbdUnknown.checkOk);
    BOOST_CHECK(HasDAGEvent(retainedIbdUnknown, ACCEPTBLOCK_DAG_ENTERED));
    BOOST_CHECK(HasDAGEvent(retainedIbdUnknown, ACCEPTBLOCK_DAG_MERGE_PARENT_NOT_FOUND));
    BOOST_CHECK(HasDAGEvent(retainedIbdUnknown, ACCEPTBLOCK_DAG_IBD_UNKNOWN_PARENT_DEFER));
    BOOST_CHECK(!HasDAGEvent(retainedIbdUnknown, ACCEPTBLOCK_DAG_MERGE_VALIDATION_PASSED));
    BOOST_CHECK_EQUAL(retainedIbdUnknown.nDoS, 0);

    BOOST_REQUIRE(retainedNonIbdUnknown.checkOk && !retainedNonIbdUnknown.processOk);
    BOOST_CHECK(HasDAGEvent(retainedNonIbdUnknown, ACCEPTBLOCK_DAG_ENTERED));
    BOOST_CHECK(HasDAGEvent(retainedNonIbdUnknown, ACCEPTBLOCK_DAG_MERGE_PARENT_NOT_FOUND));
    BOOST_CHECK(HasDAGEvent(retainedNonIbdUnknown,
                            ACCEPTBLOCK_DAG_NON_IBD_UNKNOWN_PARENT_REJECT));
    BOOST_CHECK(!HasDAGEvent(retainedNonIbdUnknown,
                             ACCEPTBLOCK_DAG_MERGE_VALIDATION_PASSED));
    BOOST_CHECK_EQUAL(retainedNonIbdUnknown.nDoS, 10);

    retainedReader.Close();
    int retainedPrimaryHeightAfterClose = -1;
    std::string retainedPrimaryResolveError;
    BOOST_REQUIRE_MESSAGE(retainedLive.ResolveParent(
                              retainedCold.hash,
                              &retainedPrimaryHeightAfterClose,
                              &retainedPrimaryResolveError),
                          retainedPrimaryResolveError);
    BOOST_CHECK_EQUAL(retainedPrimaryHeightAfterClose, S + 1);
    mapBlockIndex[retainedCold.hash] = retainedPrimary;
    BOOST_REQUIRE(mapBlockIndex.count(retainedCold.hash) == 1);
    BOOST_REQUIRE(mapBlockIndex.count(coldMergeHash) == 0);
    std::vector<uint256> authorityFailureParents;
    authorityFailureParents.push_back(retainedCold.hash);
    authorityFailureParents.push_back(coldMergeHash);
    fRegTestIbd = true;
    const DirectSeamResult retainedAuthorityFailure = RunDirectSeamCandidate(
        retainedPrimary, retainedCold.hash, 0x3305,
        authorityFailureParents, true, &retainedLive, NULL, true);
    fRegTestIbd = savedRegTestIbdForMatrix;
    BOOST_REQUIRE(retainedAuthorityFailure.checkOk &&
                  !retainedAuthorityFailure.processOk);
    BOOST_CHECK(HasDAGEvent(retainedAuthorityFailure, ACCEPTBLOCK_DAG_ENTERED));
    BOOST_CHECK(HasDAGEvent(retainedAuthorityFailure,
                            ACCEPTBLOCK_DAG_MERGE_PARENT_AUTHORITY_FAILURE));
    BOOST_CHECK(HasDAGEvent(retainedAuthorityFailure,
                            ACCEPTBLOCK_DAG_AUTHORITY_FAILURE_LOCAL_REJECT));
    BOOST_CHECK(!HasDAGEvent(retainedAuthorityFailure,
                             ACCEPTBLOCK_DAG_MERGE_PARENT_NOT_FOUND));
    BOOST_CHECK(!HasDAGEvent(retainedAuthorityFailure,
                             ACCEPTBLOCK_DAG_IBD_UNKNOWN_PARENT_DEFER));
    BOOST_CHECK(!HasDAGEvent(retainedAuthorityFailure,
                             ACCEPTBLOCK_DAG_MERGE_VALIDATION_PASSED));
    BOOST_CHECK_EQUAL(retainedAuthorityFailure.nDoS, 0);
    mapBlockIndex.erase(retainedCold.hash);

    retainedLive.Close();
    boost::system::error_code retainedCleanupError;
    fs::remove_all(retainedRoot, retainedCleanupError);

    fs::path root = fs::path(MakeTempDir());
    BlockIndexGenerationBuilder b;
    BlockIndexGenerationStats stats;
    std::string error;
    fs::path tmp = root / "build-000001.tmp";
    BOOST_REQUIRE_MESSAGE(b.Build(src, tmp.string(), 1, &stats, &error), error);
    b.Close();
    std::string perr;
    BOOST_REQUIRE(BlockIndexGenerationManager::PublishGeneration(root.string(), 1, &perr) ==
            BLOCK_INDEX_LIFECYCLE_OK);
    BOOST_REQUIRE(BlockIndexGenerationManager::SelectGeneration(root.string(), 1, &perr) ==
            BLOCK_INDEX_LIFECYCLE_OK);

    BlockIndexV2Reader reader;
    BlockIndexV2ReaderOptions opts;
    BOOST_REQUIRE_MESSAGE(reader.Open(root.string(), opts, &error), error);
    const int hor = 2; // deliberately small live-tail horizon
    BlockIndexAuthoritativeLive live;
    BOOST_REQUIRE_MESSAGE(live.Open(root.string(), &reader, hor, &error), error);

    // Make S V2-only in THIS process: erase 0..S from this process's
    // mapBlockIndex (safe: separate process, own globals).
    for (int h = 1; h <= S; ++h)
        mapBlockIndex.erase(*chain[h]->phashBlock);
    BOOST_REQUIRE(mapBlockIndex.count(sHash) == 0);
    BOOST_CHECK_EQUAL(mapBlockIndex.size(), (size_t)1); // genesis only (re-added lazily)-> ~0-1
    printf("BOUNDARY: S erased from process mapBlockIndex (historical residency %zu)\n",
           mapBlockIndex.size());

    fRegTestIbd = false;
    const DirectSeamResult authEqual = RunDirectSeamCandidate(
        pS, sHash, 0x3201, equalParents, true, &live);
    const DirectSeamResult authHigher = RunDirectSeamCandidate(
        pS, sHash, 0x3202, higherParents, true, &live);
    const DirectSeamResult authOverflow = RunDirectSeamCandidate(
        pS, sHash, 0x3203, overflowParents, true, &live);
    const DirectSeamResult authPos = RunDirectSeamCandidate(
        pS, sHash, 0x3204, posParents, true, &live);
    fRegTestIbd = savedRegTestIbdForMatrix;

    BOOST_REQUIRE(authEqual.checkOk && !authEqual.processOk);
    BOOST_REQUIRE(authHigher.checkOk && !authHigher.processOk);
    BOOST_REQUIRE(authOverflow.checkOk && !authOverflow.processOk);
    BOOST_REQUIRE(authPos.checkOk && !authPos.processOk);
    BOOST_CHECK(HasFoundMetadata(authEqual, equalHeightHash, S + 1, false));
    BOOST_CHECK(HasFoundMetadata(authHigher, higherHeightHash, S + 2, false));
    BOOST_CHECK(HasFoundMetadata(authOverflow, overflowDepthHash,
                                 S - DAG_MERGE_DEPTH - 1, false));
    BOOST_CHECK(HasFoundMetadata(authPos, posMergeHash, S - 1, true));
    BOOST_CHECK(!HasDAGEvent(authEqual, ACCEPTBLOCK_DAG_MERGE_VALIDATION_PASSED));
    BOOST_CHECK(!HasDAGEvent(authHigher, ACCEPTBLOCK_DAG_MERGE_VALIDATION_PASSED));
    BOOST_CHECK(!HasDAGEvent(authOverflow, ACCEPTBLOCK_DAG_MERGE_VALIDATION_PASSED));
    BOOST_CHECK(!HasDAGEvent(authPos, ACCEPTBLOCK_DAG_MERGE_VALIDATION_PASSED));
    BOOST_CHECK_EQUAL(authEqual.nDoS, legacyEqual.nDoS);
    BOOST_CHECK_EQUAL(authHigher.nDoS, legacyHigher.nDoS);
    BOOST_CHECK_EQUAL(authOverflow.nDoS, legacyOverflow.nDoS);
    BOOST_CHECK_EQUAL(authPos.nDoS, legacyPos.nDoS);
    BOOST_CHECK(mapBlockIndex.count(equalHeightHash) == 0);
    BOOST_CHECK(mapBlockIndex.count(higherHeightHash) == 0);
    BOOST_CHECK(mapBlockIndex.count(posMergeHash) == 0);

    const DirectSeamResult authExactDepth = RunDirectSeamCandidate(
        pS, sHash, 0x3205, exactDepthParents, true, &live);
    BOOST_REQUIRE(authExactDepth.checkOk);
    BOOST_CHECK(HasFoundMetadata(authExactDepth, exactDepthHash,
                                 S - DAG_MERGE_DEPTH, false));
    BOOST_CHECK(HasDAGEvent(authExactDepth,
                            ACCEPTBLOCK_DAG_MERGE_VALIDATION_PASSED));
    BOOST_CHECK(mapBlockIndex.count(exactDepthHash) == 0);

    // Legacy exact-boundary oracle runs last because a valid seam may mutate
    // downstream chain state before an unrelated later DAG failure.
    mapBlockIndex[sHash] = pS;
    pS->phashBlock = &mapBlockIndex.find(sHash)->first;
    CBlockIndex* exactDepthIndex = chain[S - DAG_MERGE_DEPTH];
    mapBlockIndex[exactDepthHash] = exactDepthIndex;
    exactDepthIndex->phashBlock = &mapBlockIndex.find(exactDepthHash)->first;
    fRegTestIbd = false;
    const DirectSeamResult legacyExactDepth = RunDirectSeamCandidate(
        pS, sHash, 0x3105, exactDepthParents, false, NULL);
    fRegTestIbd = savedRegTestIbdForMatrix;
    BOOST_REQUIRE(legacyExactDepth.checkOk);
    BOOST_CHECK_EQUAL(legacyExactDepth.nDoS, 0);
    BOOST_CHECK_EQUAL(authExactDepth.nDoS, legacyExactDepth.nDoS);

    printf("R2a fixture PASS: height equal/higher, depth exact/+1, and real PoS merge-parent semantics.\n");

    live.Close();
    reader.Close();
    boost::system::error_code cleanupError;
    fs::remove_all(root, cleanupError);
}

BOOST_AUTO_TEST_SUITE_END()
