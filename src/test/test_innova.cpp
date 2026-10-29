#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MODULE Bitcoin Test Suite
#include <boost/test/unit_test.hpp>

#include "checkpoints.h"
#include "db.h"
#include "main.h"
#include "wallet.h"
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
        try {
        // PRODUCTION STARTUP PARITY: the datadir is fresh, so the block-index
        // ownership classifier says EMPTY_NEW and the native genesis bootstrap
        // (PrepareNativeBlockIndexGeneration) must build+validate+publish+select
        // the first V2 generation BEFORE LoadBlockIndex(true) opens it. Skipping
        // this step leaves LoadBlockIndex with no published generation, so it
        // returns false and pindexBest/pindexGenesisBlock stay NULL on every
        // suite that dereferences the tip.
        {
            const std::string v2Root = ResolveBlockIndexV2Root(
                GetArg("-blockindexv2authoritative", ""), GetDataDir().string());
            BlockIndexStartupOwnershipDecision own;
            std::string ownErr;
            BOOST_REQUIRE_MESSAGE(
                ClassifyBlockIndexStartupOwnership(v2Root, GetDataDir().string(), &own, &ownErr),
                "TestingSetup ownership classification failed: " << ownErr);
            if (own.state == BLOCK_INDEX_STARTUP_OWNERSHIP_EMPTY_NEW)
            {
                std::string prepErr;
                const BlockIndexNativePrepareStatus prep =
                    PrepareNativeBlockIndexGeneration(v2Root, GetDataDir().string(), own.state,
                                                      &prepErr, "");
                BOOST_REQUIRE_MESSAGE(prep == BLOCK_INDEX_NATIVE_PREPARE_OK,
                                      "TestingSetup native genesis bootstrap failed: " << prepErr);
            }
        }
        LoadBlockIndex(true);
        bool fFirstRun;
        pwalletMain = new CWallet("wallet.dat");
        pwalletMain->LoadWallet(fFirstRun);
        RegisterWallet(pwalletMain);
        } catch (const std::exception& e) {
            fprintf(stderr, "TESTFIX ctor std::exception: %s\n", e.what());
            throw;
        } catch (...) {
            fprintf(stderr, "TESTFIX ctor unknown exception\n");
            throw;
        }
    }
    ~TestingSetup()
    {
        // Deterministically stop background threads (wallet flush, any leftover
        // node threads) before tearing down wallet and static state, matching
        // the production shutdown contract.  Without this the test process
        // exits while the wallet thread is still running, racing the static
        // destructors (e.g. Checkpoints) at process teardown.
        try {
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
        // The fixture now creates a real Block Index V2 generation (LevelDB-backed
        // hashindex). Deleting the temp datadir during fixture teardown races the
        // LevelDB background threads and Boost reports the run as a "setup error"
        // even though every case passed. The directory is left to the OS; each run
        // uses a unique path under the system temp dir.
        (void)pathTestData;
        } catch (const std::exception& e) {
            fprintf(stderr, "TESTFIX dtor std::exception: %s\n", e.what());
        } catch (...) {
            fprintf(stderr, "TESTFIX dtor unknown exception\n");
        }
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
