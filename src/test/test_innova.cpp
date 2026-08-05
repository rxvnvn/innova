#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MODULE Bitcoin Test Suite
#include <boost/test/unit_test.hpp>

#include "checkpoints.h"
#include "db.h"
#include "main.h"
#include "txdb.h"
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
extern void noui_connect();

struct TestingSetup {
    boost::filesystem::path pathTestData;
    bool fRegTestSaved;
    bool fTestNetSaved;

    TestingSetup()
        : fRegTestSaved(fRegTest),
          fTestNetSaved(fTestNet)
    {
        // Restore the initial runtime state so the fixture stays reusable: a
        // previous destructor stops background threads, and a new instance must
        // re-arm shutdown and thread joining before starting any threads again.
        fShutdown = false;
        ResetTrackedThreadJoinState();
        pathTestData = boost::filesystem::temp_directory_path() /
            boost::filesystem::unique_path("innova-test-%%%%-%%%%-%%%%");
        boost::filesystem::create_directories(pathTestData);
        mapArgs["-datadir"] = pathTestData.string();
        mapArgs["-regtest"] = "1";
        fRegTest = true;
        fTestNet = false;

        fPrintToDebugger = true; // don't want to write to debug.log file
        noui_connect();
        bitdb.MakeMock();
        LoadBlockIndex(true);
        bool fFirstRun;
        pwalletMain = new CWallet("wallet.dat");
        pwalletMain->LoadWallet(fFirstRun);
        RegisterWallet(pwalletMain);
    }
    ~TestingSetup()
    {
        // Deterministically stop background threads (wallet flush, any leftover
        // node threads) before tearing down wallet and static state, matching
        // the production shutdown contract.  Without this the test process
        // exits while the wallet thread is still running, racing the static
        // destructors (e.g. Checkpoints) at process teardown.
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

BOOST_GLOBAL_FIXTURE(TestingSetup);

void Shutdown(void* parg)
{
  exit(0);
}

void StartShutdown()
{
  exit(0);
}
