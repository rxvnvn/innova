#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MODULE Bitcoin Test Suite
#include <boost/test/unit_test.hpp>

#include "checkpoints.h"
#include "db.h"
#include "main.h"
#include "wallet.h"
#include "blockindex_authoritative_startup.h"
#include "blockindex_native_bootstrap.h"
#include "blockindex_startup_ownership.h"

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

        // Mirror the production startup sequence (init.cpp) so the fixture does
        // NOT bypass authority: classify the datadir's block-index ownership
        // (a fresh temp datadir classifies as EMPTY_NEW), build+publish+select
        // the first V2 generation through the REAL daemon-native primitive
        // (PrepareNativeBlockIndexGeneration), then load through the REAL
        // InitBlockIndexAuthoritative via LoadBlockIndex. No legacy fallback,
        // no synthetic CBlockIndex; any failure must fail the suite loudly.
        const std::string v2Root = ResolveBlockIndexV2Root(
            GetArg("-blockindexv2authoritative", ""), GetDataDir().string());
        BlockIndexStartupOwnershipDecision ownership;
        std::string ownershipErr;
        if (!ClassifyBlockIndexStartupOwnership(v2Root, GetDataDir().string(),
                                                &ownership, &ownershipErr))
            BOOST_FAIL(std::string("TestingSetup: block index startup ownership "
                                   "classification failed: ") + ownershipErr);
        if (ownership.state == BLOCK_INDEX_STARTUP_OWNERSHIP_EMPTY_NEW)
        {
            std::string prepErr;
            if (PrepareNativeBlockIndexGeneration(v2Root, GetDataDir().string(),
                                                  ownership.state, &prepErr)
                != BLOCK_INDEX_NATIVE_PREPARE_OK)
                BOOST_FAIL(std::string("TestingSetup: block index V2 native "
                                       "bootstrap failed (EMPTY_NEW): ") + prepErr);
        }
        else if (ownership.state != BLOCK_INDEX_STARTUP_OWNERSHIP_V2_AUTHORITATIVE)
        {
            BOOST_FAIL(std::string("TestingSetup: block index ownership state is ") +
                       BlockIndexStartupOwnershipStateName(ownership.state) +
                       " (fatal; no legacy fallback) detail=" + ownership.detail);
        }

        const bool fLoaded = LoadBlockIndex(true);
        if (!fLoaded || pindexBest == NULL || pindexGenesisBlock == NULL ||
            nBestHeight < 0)
            BOOST_FAIL(strprintf(
                "TestingSetup: authoritative regtest bootstrap failed: "
                "LoadBlockIndex=%d pindexBest=%p pindexGenesisBlock=%p "
                "nBestHeight=%d", (int)fLoaded, (void*)pindexBest,
                (void*)pindexGenesisBlock, (int)nBestHeight));
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
        // No legacy txdb teardown: txdb-leveldb (and CTxDB) was physically
        // removed from this tree. The V2 stores are closed by each suite's own
        // authoritative-startup reset before this global fixture runs.
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
