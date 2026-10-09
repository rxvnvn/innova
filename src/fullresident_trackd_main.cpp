//
// TRACK D — deterministic long-run the former operation-resident map (now removed) growth reproducer (standalone).
//
// Not a production or suite file: a scratch target appended (untracked) at the
// tail of makefile.unix builds obj/fullresident_trackd_main.o from
// fullresident_trackd_main.cpp and links fullresident_trackd_driver from the
// repo's object set - the suite's test_innova.cpp role (process globals,
// TestingSetup regtest bootstrap, StartShutdown/Shutdown stubs) is provided by
// this TU. It builds the same REAL authoritative V2 base generation as
// src/test/blockindex_authoritative_live_tests.cpp (G1Fixture) via the trusted
// BlockIndexGenerationBuilder, opens the production BlockIndexV2Reader +
// BlockIndexAuthoritativeLive, and drives N sequential PoW blocks through the
// REAL ProcessBlock path (test-armed authoritative mode:
// SetAuthoritativeLiveForTesting + g_fAuthoritativeStartup), exactly like
// g1_final_processblock_orphan_gate. Nothing (neither the former operation-resident map (now removed) nor any
// other component) is torn down between probes: the persistently-resident
// the former operation-resident map (now removed) map inside BlockIndexAuthoritativeLive::Impl
// (blockindex_authoritative_live.cpp:43) plus its anchors, the operation
// store, the live-tail count, mapBlockIndex, and this process's VmRSS are
// probed after N, 2N, and 4N accepted blocks via #define private public in
// this TU alone (a scoped-unset guard restores normal linkage immediately
// after the accessor block).
#include <sstream>
#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <stdint.h>
#include <boost/thread/mutex.hpp>
#include <boost/thread/locks.hpp>
#include <boost/thread/condition_variable.hpp>
#include <boost/date_time/posix_time/posix_time_types.hpp>
#define private public
#define protected public
#include "blockindex_authoritative_live.h"
#include "blockindex_authoritative_startup.h"
#include "blockindex_v2_reader.h"
#include "blockindex_generation_builder.h"
#include "blockindex_generation_lifecycle.h"
#include "checkpoints.h"
#include "main.h"
#include "txdb.h"
#include "db.h"
#include "wallet.h"
#include "blockrequesttrace.h"
#undef private
#undef protected

#include <boost/filesystem.hpp>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>
#include <stdlib.h>

// suite-process globals (normally provided by test/test_innova.cpp)
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

void Shutdown(void* parg) { exit(0); }
void StartShutdown() { exit(0); }

namespace fs = boost::filesystem;

static std::string TrackDTempDir()
{
    char tmpl[] = "/tmp/innova-trackd-XXXXXX";
    char* d = mkdtemp(tmpl);
    if (!d) { fprintf(stderr, "mkdtemp failed\n"); exit(1); }
    return std::string(d);
}

// Same record shape as the G1Fixture (blockindex_authoritative_live_tests.cpp).
static BlockIndexRecord TrackDRecord(uint256 hash, uint256 hashPrev, int height)
{
    BlockIndexRecord r;
    r.hash = hash;
    r.hashPrev = hashPrev;
    r.hashMerkleRoot = uint256(0x1111UL + (unsigned)height);
    r.height = height;
    r.nFile = 1;
    r.nBlockPos = 100u + (unsigned)height;
    r.nFlags = 0;
    r.nVersion = 7;
    r.nTime = 1700000000u + (unsigned)height;
    r.nBits = 0x1d00ffff;
    r.nNonce = (unsigned)height;
    r.nMint = 100;
    r.nMoneySupply = 500;
    return r;
}

struct TrackDFixture
{
    fs::path root;
    uint64_t baseGen;
    int baseTip;
    std::vector<uint256> baseActive;
    std::string rootStr;

    explicit TrackDFixture(int s)
        : root(TrackDTempDir()), baseGen(1), baseTip(s)
    {
        BlockIndexGenerationSource src;
        for (int h = 0; h <= baseTip; ++h)
        {
            uint256 hv = uint256(0xD0000000UL + h);
            uint256 hp = (h == 0) ? uint256(0) : baseActive[h - 1];
            BlockIndexRecord rec = TrackDRecord(hv, hp, h);
            baseActive.push_back(hv);
            BlockIndexGenerationSourceRecord sr; sr.hash = hv; sr.record = rec;
            src.records.push_back(sr);
        }
        src.hashBestChain = baseActive[baseTip];
        src.foundBestChain = true;
        BlockIndexGenerationBuilder b;
        BlockIndexGenerationStats stats;
        std::string error;
        fs::path tmp = root / "blockindex-build-000001.tmp";
        if (!b.Build(src, tmp.string(), baseGen, &stats, &error))
        { fprintf(stderr, "builder: %s\n", error.c_str()); exit(1); }
        b.Close();
        std::string perr;
        if (BlockIndexGenerationManager::PublishGeneration(root.string(), baseGen, &perr) !=
            (int)BLOCK_INDEX_LIFECYCLE_OK)
        { fprintf(stderr, "publish: %s\n", perr.c_str()); exit(1); }
        if (BlockIndexGenerationManager::SelectGeneration(root.string(), baseGen, &perr) !=
            (int)BLOCK_INDEX_LIFECYCLE_OK)
        { fprintf(stderr, "select: %s\n", perr.c_str()); exit(1); }
        rootStr = root.string();
    }
    ~TrackDFixture()
    { boost::system::error_code ec; fs::remove_all(root, ec); }
};

// Build a minimal PoW block (single OP_TRUE coinbase) passing CheckBlock with
// the proven-easy regtest target (mirrors BuildCheckBlockPassingPoWBallast in
// blockindex_authoritative_live_tests.cpp:366).
static CBlock* TrackDBuildBlock(uint256 hashPrev, unsigned int nExtra)
{
    CBlock b;
    b.nVersion = 1;
    b.nTime = (unsigned int)GetTime();
    b.hashPrevBlock = hashPrev;
    CTransaction coinbase;
    coinbase.nVersion = 1;
    coinbase.nTime = (int64_t)b.nTime;
    coinbase.vin.resize(1);
    coinbase.vin[0].prevout.SetNull();
    coinbase.vin[0].scriptSig = CScript() << nExtra;
    coinbase.vout.resize(1);
    coinbase.vout[0].nValue = 0;
    coinbase.vout[0].scriptPubKey = CScript() << OP_TRUE;
    b.vtx.push_back(coinbase);
    b.hashMerkleRoot = b.BuildMerkleTree();
    unsigned int nBits = 0x1d00ffffU;
    {
        LOCK(cs_main);
        if (pindexBest)
            nBits = pindexBest->nBits;
    }
    b.nBits = nBits;
    uint256 hashTarget = CBigNum().SetCompact(b.nBits).getuint256();
    for (unsigned int n = 0; n < 0x400000U; ++n)
    {
        b.nNonce = n;
        if (b.GetHash() <= hashTarget)
        {
            CBlock* out = new CBlock(b);
            return out;
        }
    }
    return NULL;
}

static long GetRssKb()
{
    FILE* f = fopen("/proc/self/status", "r");
    if (!f) return -1;
    char line[256];
    long rss = -1;
    while (fgets(line, sizeof(line), f))
        if (strncmp(line, "VmRSS:", 6) == 0)
        { sscanf(line + 6, "%ld", &rss); break; }
    fclose(f);
    return rss;
}

static long GetMapBlockIndexSize()
{
    LOCK(cs_main);
    return (long)mapBlockIndex.size();
}

namespace fs = boost::filesystem;

// TRACK D measurement TUs (see fullresident_trackd_measure.cpp).
long TrackDResidentSizeInternal(const BlockIndexAuthoritativeLive& live);
long TrackDAnchorSizeInternal(const BlockIndexAuthoritativeLive& live);
long TrackDOwnedChainSizeInternal(const BlockIndexAuthoritativeLive& live);
long TrackDTailCountInternal(const BlockIndexAuthoritativeLive& live);

static void Probe(const BlockIndexAuthoritativeLive& live, const char* tag, int n)
{
    long fr = TrackDResidentSizeInternal(live);
    long an = TrackDAnchorSizeInternal(live);
    long oc = TrackDOwnedChainSizeInternal(live);
    long tl = TrackDTailCountInternal(live);
    long mb = GetMapBlockIndexSize();
    long rss = GetRssKb();
    printf("TRACKD PROBE %s: N=%d fullResident=%ld anchor=%ld ownedChain=%ld liveTail=%ld mapBlockIndex=%ld rssKb=%ld\n",
           tag, n, fr, an, oc, tl, mb, rss);
    fflush(stdout);
}

static int N_TRACKD = 500;

// Real ProcessBlock path, verbatim from g1_final_processblock_orphan_gate.
static void TrackDProcessBlock(CBlock* blk, const uint256& expectedPrev)
{
    LOCK(cs_main);
    const uint256 h = blk->GetHash();
    (void)ProcessBlock(NULL, blk);
    bool fOrphaned = mapOrphanBlocks.count(h) != 0;
    if (fOrphaned)
    {
        std::map<uint256, CBlock*>::iterator it = mapOrphanBlocks.find(h);
        if (it != mapOrphanBlocks.end())
        {
            delete it->second;
            mapOrphanBlocks.erase(it);
        }
        mapOrphanBlocksByPrev.erase(h);
    }
}

static int RunTrackD()
{
    TrackDFixture fx(64); // S = 64 (G1Fixture replica-san-shape)
    BlockIndexV2Reader reader;
    BlockIndexV2ReaderOptions opts;
    std::string error;
    if (!reader.Open(fx.rootStr, opts, &error))
    { fprintf(stderr, "reader open: %s\n", error.c_str()); return 1; }

    BlockIndexAuthoritativeLive live;
    if (!live.Open(fx.rootStr, &reader, 2048, &error))
    { fprintf(stderr, "live open: %s\n", error.c_str()); return 1; }
    const int baseTip = fx.baseTip;
    const uint256 sHash = fx.baseActive[baseTip];

    // Precondition: parent S is authoritative-in-V2 but NOT resident.
    BlockIndexAuthoritativeParentInfo cold;
    if (live.ResolveParentInfo(sHash, &cold, &error) != BLOCK_INDEX_AUTHORITATIVE_PARENT_FOUND)
    { fprintf(stderr, "parent S resolvable: %s\n", error.c_str()); return 1; }
    if (mapBlockIndex.count(sHash) != 0)
    { fprintf(stderr, "parent S must NOT be resident pre-accept\n"); return 1; }

    printf("TRACKD START: N=%d (probes at N, 2N, 4N)\n", N_TRACKD);
    // N sequential accepted blocks, parent = previous accepted block.
    uint256 prev = sHash;
    for (int i = 1; i <= N_TRACKD; ++i)
    {
        CBlock* blk = TrackDBuildBlock(prev, 0x511000u + (unsigned)i);
        if (!blk)
        { fprintf(stderr, "mining failed at i=%d\n", i); return 1; }
        TrackDProcessBlock(blk, prev);
        if (mapBlockIndex.count(blk->GetHash()) == 0)
        { printf("TRACKD FAIL: block i=%d NOT accepted (stop).\n", i); delete blk; return 1; }
        delete blk;
        prev = blk->GetHash();
        if (i == N_TRACKD || i == 2 * N_TRACKD || i == 4 * N_TRACKD)
            Probe(live, "mid", i);
    }

    Probe(live, "outer", N_TRACKD * 4);
    live.Close();
    reader.Close();
    printf("TRACKD PASS: all probes recorded after accepted blocks, no teardown between probes.\n");
    return 0;
}

// Mirrors test/test_innova.cpp TestingSetup (adapted XP: mostly identical).
struct TrackDTestingSetup
{
    boost::filesystem::path pathTestData;
    bool fRegTestSaved;
    bool fTestNetSaved;

    TrackDTestingSetup() : fRegTestSaved(fRegTest), fTestNetSaved(fTestNet)
    {
        fShutdown = false;
        ResetTrackedThreadJoinState();
        pathTestData = boost::filesystem::temp_directory_path() /
            boost::filesystem::unique_path("innova-trackd-%%%%-%%%%-%%%%");
        boost::filesystem::create_directories(pathTestData);
        mapArgs["-datadir"] = pathTestData.string();
        mapArgs["-regtest"] = "1";
        fRegTest = true;
        fTestNet = false;
        fPrintToConsole = true; // driver: emit log to stdout for capture
        fPrintToDebugger = true;
        noui_connect();
        bitdb.MakeMock();
        LoadBlockIndex(true);
        bool fFirstRun;
        pwalletMain = new CWallet("wallet.dat");
        pwalletMain->LoadWallet(fFirstRun);
        RegisterWallet(pwalletMain);
    }
    ~TrackDTestingSetup()
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

int main(int argc, char** argv)
{
    if (getenv("TRACKD_N"))
        N_TRACKD = atoi(getenv("TRACKD_N"));
    if (N_TRACKD <= 0) N_TRACKD = 500;
    TrackDTestingSetup setup;
    InitAcceptBlockRejectTrace(true);
    InitProcessBlockRejectTrace(true);
    InitBlockRequestTrace(true, "");
    return RunTrackD();
}
