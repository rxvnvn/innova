// Standalone fixture for the null-block guard test runner (p2p-nullblock-guard).
//
// test_innova cannot link on this tip: test/test_innova.cpp and several legacy
// suites still reference CTxdb/CTxDB, which was retired in e0ee4121.  That is
// pre-existing breakage, deliberately NOT fixed here.  This file provides the
// same global fixture (TestingSetup) without the legacy CTxDB teardown call so
// the p2p_sync suite (including the null-block guard regression cases) can be
// built and run standalone.
#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MODULE NullBlock Guard Test Suite
#include <boost/test/unit_test.hpp>

#include "checkpoints.h"
#include "db.h"
#include "main.h"
#include "wallet.h"

#include <boost/filesystem.hpp>

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
extern bool fDebug;
extern void noui_connect();

struct TestingSetup {
    boost::filesystem::path pathTestData;
    bool fRegTestSaved;
    bool fTestNetSaved;
    CBlockIndex* pBestSaved;
    int nBestHeightSaved;

    TestingSetup()
        : fRegTestSaved(fRegTest),
          fTestNetSaved(fTestNet)
    {
        fShutdown = false;
        ResetTrackedThreadJoinState();
        pathTestData = boost::filesystem::temp_directory_path() /
            boost::filesystem::unique_path("innova-nullguard-%%%%-%%%%");
        boost::filesystem::create_directories(pathTestData);
        mapArgs["-datadir"] = pathTestData.string();
        mapArgs["-regtest"] = "1";
        fRegTest = true;
        fTestNet = false;

        fPrintToDebugger = true;
        // Route every printf (== OutputDebugStringF) to stdout: with
        // fPrintToDebugger set and fPrintToConsole cleared the output is
        // silently swallowed, which hid the ProcessMessage dispatch trace.
        // (Left on for the guard tests only; harmless for other suites that
        // share this fixture — stdout noise is accepted while debugging.)
        extern bool fPrintToConsole;
        fPrintToConsole = true;
        fDebug = true;  // surface 'end of data' short-message diagnostics
        noui_connect();
        bitdb.MakeMock();
        LoadBlockIndex(true);
        // LoadBlockIndex reports "Block Index V2 authoritative startup
        // failed:" on an empty regtest datadir and leaves pindexBest == NULL.
        // The verack/getblocks/dispatch paths on the wire pin the tip and some
        // malformed-block handling derefs mapBlockIndex/pindexBest, so pin a
        // minimal valid chain pointer here (savedBest/restore pattern from
        // wt-protofilter's allowlist runner): a real CBlockIndex tip with a
        // real hash, on the stack-backed storage below.  Test scope only - no
        // production hook, LoadBlockIndex itself is NOT bypassed.
        pBestSaved = pindexBest;
        nBestHeightSaved = nBestHeight;
        static uint256 hashTip;
        hashTip.SetHex("0f");
        static CBlockIndex indexTip;
        indexTip.phashBlock = &hashTip;
        indexTip.pprev = NULL;
        indexTip.nHeight = 100;
        pindexBest = &indexTip;
        nBestHeight = 100;
        bool fFirstRun;
        pwalletMain = new CWallet("wallet.dat");
        pwalletMain->LoadWallet(fFirstRun);
        RegisterWallet(pwalletMain);
    }
    ~TestingSetup()
    {
        fShutdown = true;
        JoinTrackedThreads();
        delete pwalletMain;
        pwalletMain = NULL;
        // NOTE: no CTxDB teardown here -- the legacy txdb is retired; the
        // mocked Berkeley env is simply flushed.
        bitdb.Flush(true);
        pindexBest = pBestSaved;
        nBestHeight = nBestHeightSaved;
        fRegTest = fRegTestSaved;
        fTestNet = fTestNetSaved;
        boost::filesystem::remove_all(pathTestData);
    }
};

BOOST_GLOBAL_FIXTURE(TestingSetup);

void Shutdown(void* parg)
{
  exit(0);
}

void StartShutdown()
{
  exit(0);
}
