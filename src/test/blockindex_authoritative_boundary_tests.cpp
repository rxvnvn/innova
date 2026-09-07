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
#include "miner.h"
#include "wallet.h"
#include "zkproof.h"
#include "hooks.h"
#include "checkpoints.h"
#include "ui_interface.h"

#include <boost/filesystem.hpp>
#include <cstdio>
#include <string>
#include <vector>

namespace fs = boost::filesystem;

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
                              const uint256* prevHash = NULL)
{
    CBlock* pblock = CreateNewBlock(pwalletMain, false, NULL, NULL);
    if (!pblock) return NULL;
    pblock->nVersion = 1;
    pblock->nTime = std::max((unsigned int)GetTime(),
                             (unsigned int)(pindexPrev->GetMedianTimePast() + 1));
    pblock->hashPrevBlock = prevHash ? *prevHash : *pindexPrev->phashBlock;
    pblock->vtx[0].vin[0].scriptSig = CScript() << (pindexPrev->nHeight + 1) << nExtra;
    pblock->hashMerkleRoot = pblock->BuildMerkleTree();
    uint256 hashTarget = CBigNum().SetCompact(pblock->nBits).getuint256();
    while (pblock->GetHash() > hashTarget && pblock->nNonce < 0xffffffff)
        ++pblock->nNonce;
    return pblock;
}

static CBlockIndex* MineReal(CBlockIndex* pindexPrev, unsigned int nExtra,
                             bool* ok, BlockIndexAuthoritativeLive* live,
                             uint256* outHash = NULL, const uint256* prevHash = NULL)
{
    *ok = false;
    CBlock* pblock = BuildRealBlock(pindexPrev, nExtra, prevHash);
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
    r.nStakeTime = p->nStakeTime;
    return r;
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

    const int S = 4; // real chain 0..S
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
    src.hashBestChain = sHash;
    src.foundBestChain = true;

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

    // Arm authoritative mode.
    const bool savedAuth = ::g_fAuthoritativeStartup;
    SetAuthoritativeLiveForTesting(&live);
    ::g_fAuthoritativeStartup = true;

    // Genuine S+1 through the COMPLETE real consensus path (parent S is V2-only).
    bool ok = false;
    uint256 hashS1;
    CBlockIndex* pS1 = MineReal(pS, 0x2000, &ok, &live, &hashS1, &sHash);
    ::g_fAuthoritativeStartup = savedAuth;
    ClearAuthoritativeLiveForTesting();

    BOOST_CHECK(ok);                 // ACCEPT, no ABREJECT_PREV_NOT_FOUND, no orphan
    fprintf(stderr, "[BOUNDARY] ProcessBlock(S+1) ok=%d\n", ok);
    fprintf(stderr, "[BOUNDARY] hashBestChain=%s\n", hashBestChain.ToString().substr(0,18).c_str());
    fprintf(stderr, "[BOUNDARY] hashS1=%s  best==S1? %d\n",
            hashS1.ToString().substr(0,18).c_str(), (hashBestChain==hashS1));
    fprintf(stderr, "[BOUNDARY] in mapBlockIndex? %d (%zu total)\n",
                (int)mapBlockIndex.count(hashS1), mapBlockIndex.size());
        fprintf(stderr, "[BOUNDARY] pS live trust=%s\n",
                (pS?pS->nChainTrust.ToString().substr(0,18).c_str():"(none)"));

        // Decisive wiring check: was S+1 persisted as ACTIVE into the V2 tip?
        fprintf(stderr, "[BOUNDARY] live tip height=%d (expect %d)\n",
                live.TipAuthorityMutable()->TipHeight(), S+1);
    BOOST_CHECK_EQUAL(live.TipAuthorityMutable()->TipHeight(), S + 1);
    BOOST_CHECK(pS1 != NULL);
    if (!ok || !pS1) return;         // fail reported above; stop clean
    BOOST_CHECK_EQUAL(pS1->nHeight, S + 1);
    BOOST_CHECK(pS1->GetBlockHash() == hashBestChain); // S+1 active best tip
    BOOST_CHECK(mapBlockIndex.count(sHash) == 0);      // S STILL non-resident (no O(N) rebuild)
    printf("BOUNDARY PASS: genuine S+1 through COMPLETE real consensus against\n"
           "       V2-only parent S; S+1 best tip, S stays non-resident. trust=%s\n",
           pS1->nChainTrust.ToString().c_str());

    // Continuation S+2 through real consensus.
    ::g_fAuthoritativeStartup = true;
    SetAuthoritativeLiveForTesting(&live);
    bool ok2 = false;
    CBlockIndex* pS2 = MineReal(pS1, 0x2001, &ok2, &live);
    ::g_fAuthoritativeStartup = savedAuth;
    ClearAuthoritativeLiveForTesting();
    BOOST_CHECK(ok2 && pS2);
    if (ok2 && pS2)
    {
        BOOST_CHECK(pS2->GetBlockHash() == hashBestChain);
        // authoritative restart persistence: reopen fresh tip store.
        live.Close();
        reader.Close();
        BlockIndexV2Reader reader2;
        BOOST_REQUIRE(reader2.Open(root.string(), opts, &error));
        BlockIndexAuthoritativeLive live2;
        BOOST_REQUIRE(live2.Open(root.string(), &reader2, hor, &error));
        BOOST_CHECK_EQUAL(live2.TipAuthorityMutable()->TipHeight(), pS2->nHeight);
        printf("BOUNDARY: continuation S+2 accepted; authoritative restart tip=%d\n",
               live2.TipAuthorityMutable()->TipHeight());
    }
}

BOOST_AUTO_TEST_SUITE_END()