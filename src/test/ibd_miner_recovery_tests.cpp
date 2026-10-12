// Stage 1: focused deterministic tests for the activity-corroborated IBD
// peer-ahead-lag predicate and for the truthful CPU-mining status
// (requested / alive / hashing / paused+reason).
//
// IBD cases replicate the scoped peer/N-vNodes pattern used by
// src/test/p2p_sync_tests.cpp (CScopedInitialBlockDownloadState). Controller
// cases use the injectable WorkerFunction pattern from
// src/test/cpu_mining_tests.cpp.

#define BOOST_TEST_DYN_LINK
#include <boost/test/unit_test.hpp>

#include "checkpoints.h"
#include "main.h"
#include "miner.h"
#include "net.h"
#include "util.h"
#include "wallet.h"

#include <atomic>
#include <chrono>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

extern void TxnDBShutdown();
extern CWallet* pwalletMain;

namespace
{

static CAddress TestPeerAddress_ibd_rec(unsigned int nPeer)
{
    struct in_addr addr;
    addr.s_addr = 0x0100007f + (nPeer << 24);
    return CAddress(CService(addr, GetDefaultPort()));
}

// Saves/restores the global IBD inputs (fRegTest/fImporting/fReindex/fTestNet/
// nBestHeight) and an isolated single-peer vNodes snapshot, matching the
// pattern of CScopedInitialBlockDownloadState in p2p_sync_tests.cpp.
class CScopedIBDRecoveryState
{
private:
    bool fRegTestSaved;
    bool fTestNetSaved;
    bool fImportingSaved;
    bool fReindexSaved;
    int nBestHeightSaved;
    std::vector<CNode*> vNodesSaved;

public:
    explicit CScopedIBDRecoveryState()
        : fRegTestSaved(fRegTest),
          fTestNetSaved(fTestNet),
          fImportingSaved(fImporting),
          fReindexSaved(fReindex),
          nBestHeightSaved(nBestHeight)
    {
        fRegTest = false;
        fTestNet = false;
        fImporting = false;
        fReindex = false;
        nBestHeight = std::max(
            nBestHeight, Checkpoints::GetTotalBlocksEstimate());
        {
            LOCK(cs_vNodes);
            vNodesSaved = vNodes;
            vNodes.clear();
        }
    }

    ~CScopedIBDRecoveryState()
    {
        {
            LOCK(cs_vNodes);
            vNodes = vNodesSaved;
        }
        nBestHeight = nBestHeightSaved;
        fReindex = fReindexSaved;
        fImporting = fImportingSaved;
        fTestNet = fTestNetSaved;
        fRegTest = fRegTestSaved;
    }
};

// Configure the single test peer against the current nBestHeight baseline.
static void ConfigurePeer(CNode& peer, int nHeightDelta, int64_t nAgeHeightUpdate,
                          int64_t nAgeBlockRecv)
{
    peer.fClient = false;
    peer.nBestKnownHeight = nBestHeight + nHeightDelta;
    peer.nChainHeight = peer.nBestKnownHeight;
    peer.nLastHeightUpdate = nAgeHeightUpdate >= 0 ? GetTime() - nAgeHeightUpdate : 0;
    peer.nLastBlockRecv = nAgeBlockRecv >= 0 ? GetTime() - nAgeBlockRecv : 0;
    peer.setBlocksInFlight.clear();
    peer.mapBlockInFlightSince.clear();
    peer.mapAskFor.clear();
}

static void InstallPeer(CNode& peer)
{
    LOCK(cs_vNodes);
    vNodes.push_back(&peer);
}

static bool WaitForHashingOrPaused(CCPUMinerController& controller)
{
    for (int i = 0; i < 5000; ++i)
    {
        CCPUMinerController::Status status = controller.GetStatus();
        if (status.hashing)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return controller.GetStatus().hashing;
}

static bool WaitForPauseReasonReached(CCPUMinerController& controller, int nReason)
{
    for (int i = 0; i < 5000; ++i)
    {
        CCPUMinerController::Status status = controller.GetStatus();
        if (status.pauseReason == nReason)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return controller.GetStatus().pauseReason == nReason;
}

} // anon namespace

BOOST_AUTO_TEST_SUITE(ibd_miner_recovery_tests)

BOOST_AUTO_TEST_CASE(ibd_stale_uncorroborated_peer_far_ahead_is_not_ibd)
{
    BOOST_REQUIRE(pindexBest != NULL);
    CScopedIBDRecoveryState scope;
    CNode peer(INVALID_SOCKET, TestPeerAddress_ibd_rec(180),
               "ibd-rec-stale-uncorroborated", true);
    ConfigurePeer(peer, /*nHeightDelta=*/+100000, /*nAgeHeightUpdate=*/1000,
                  /*nAgeBlockRecv=*/1000);
    InstallPeer(peer);

    // Stale height claim (>3h old), zero blocks received, nothing in flight:
    // the single stale claim above the lag tolerance must NOT hold IBD.
    BOOST_CHECK_EQUAL(IsInitialBlockDownload(), false);
}

BOOST_AUTO_TEST_CASE(ibd_fresh_peer_genuinely_ahead_is_ibd)
{
    BOOST_REQUIRE(pindexBest != NULL);
    CScopedIBDRecoveryState scope;
    CNode peer(INVALID_SOCKET, TestPeerAddress_ibd_rec(181),
               "ibd-rec-fresh-ahead", true);
    ConfigurePeer(peer, +1000, /*nAgeHeightUpdate=*/0, /*nAgeBlockRecv=*/1000);
    InstallPeer(peer);

    BOOST_CHECK_EQUAL(IsInitialBlockDownload(), true);
}

BOOST_AUTO_TEST_CASE(ibd_stale_claims_while_chain_progresses_is_not_ibd)
{
    BOOST_REQUIRE(pindexBest != NULL);
    CScopedIBDRecoveryState scope;
    CNode peer(INVALID_SOCKET, TestPeerAddress_ibd_rec(182),
               "ibd-rec-stale-progress", true);
    ConfigurePeer(peer, +5000, /*nAgeHeightUpdate=*/600, /*nAgeBlockRecv=*/1000);
    InstallPeer(peer);

    // Local chain progressing (fresh best-block receive time) with only a
    // stale, uncorroborated peer claim => not IBD.
    int64_t nLocalLastBlockRecvSaved = nTimeBestReceived;
    nTimeBestReceived = GetTime();
    BOOST_CHECK_EQUAL(IsInitialBlockDownload(), false);
    nTimeBestReceived = nLocalLastBlockRecvSaved;
}

BOOST_AUTO_TEST_CASE(ibd_fresh_ahead_with_inflight_work_is_ibd)
{
    BOOST_REQUIRE(pindexBest != NULL);
    CScopedIBDRecoveryState scope;
    CNode peer(INVALID_SOCKET, TestPeerAddress_ibd_rec(183),
               "ibd-rec-fresh-inflight", true);
    ConfigurePeer(peer, +1000, /*nAgeHeightUpdate=*/0, /*nAgeBlockRecv=*/1000);
    peer.setBlocksInFlight.insert(uint256(7));
    InstallPeer(peer);

    // Heavier competing branch with active catch-up must stay conservative.
    BOOST_CHECK_EQUAL(IsInitialBlockDownload(), true);
}

BOOST_AUTO_TEST_CASE(ibd_stale_height_but_receiving_blocks_is_conservative)
{
    BOOST_REQUIRE(pindexBest != NULL);
    CScopedIBDRecoveryState scope;
    CNode peer(INVALID_SOCKET, TestPeerAddress_ibd_rec(184),
               "ibd-rec-stale-recv", true);
    ConfigurePeer(peer, +1000, /*nAgeHeightUpdate=*/121, /*nAgeBlockRecv=*/0);
    InstallPeer(peer);

    // Mirrors p2p_sync_tests
    // ibd_stale_height_with_continuing_blocks_is_conservative: a stale height
    // claim corroborated by fresh block delivery stays IBD.
    BOOST_CHECK_EQUAL(IsInitialBlockDownload(), true);
}

BOOST_AUTO_TEST_CASE(controller_start_unchanged_thread_count_is_noop)
{
    int nLive = 0;
    CCPUMinerController controller(
        [&nLive, &controller](CCPUMinerController&, CWallet*, unsigned int,
                              unsigned int, uint64_t sessionId) {
            nLive++;
            while (!controller.WaitForStop(sessionId, 10))
            {
            }
            nLive--;
        });
    std::string strError;

    BOOST_REQUIRE(controller.Start(pwalletMain, 2, strError));
    for (int i = 0; i < 5000 && nLive < 2; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    BOOST_CHECK_EQUAL(nLive, 2);

    CCPUMinerController::Status first = controller.GetStatus();
    BOOST_CHECK_EQUAL(first.requestedThreads, 2);
    BOOST_CHECK_EQUAL(first.activeThreads, 2);

    const uint64_t nSessionId = first.sessionId;
    BOOST_CHECK(controller.Start(pwalletMain, 2, strError));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    BOOST_CHECK_EQUAL(nLive, 2);

    CCPUMinerController::Status second = controller.GetStatus();
    BOOST_CHECK_EQUAL(second.requestedThreads, 2);
    BOOST_CHECK_EQUAL(second.activeThreads, 2);
    BOOST_CHECK_EQUAL(second.sessionId, nSessionId);

    controller.Stop();
    CCPUMinerController::Status stopped = controller.GetStatus();
    BOOST_CHECK_EQUAL(stopped.activeThreads, 0);
    BOOST_CHECK_EQUAL(stopped.requestedThreads, 0);
}

BOOST_AUTO_TEST_CASE(controller_parked_worker_reports_running_but_not_hashing)
{
    int nLive = 0;
    CCPUMinerController controller(
        [&nLive](CCPUMinerController& c, CWallet*, unsigned int, unsigned int,
                 uint64_t sessionId) {
            nLive++;
            c.NotePaused(CCPUMinerController::PAUSE_IBD);
            while (!c.WaitForStop(sessionId, 10))
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            nLive--;
        });
    std::string strError;

    BOOST_REQUIRE(controller.Start(pwalletMain, 1, strError));
    BOOST_REQUIRE(WaitForPauseReasonReached(
        controller, CCPUMinerController::PAUSE_IBD));

    CCPUMinerController::Status parked = controller.GetStatus();
    BOOST_CHECK(parked.running);
    BOOST_CHECK(!parked.hashing);
    BOOST_CHECK_EQUAL(parked.pauseReason,
                      (int)CCPUMinerController::PAUSE_IBD);

    controller.Stop();
    CCPUMinerController::Status stopped = controller.GetStatus();
    BOOST_CHECK(!stopped.running);
    BOOST_CHECK(!stopped.hashing);
}

BOOST_AUTO_TEST_CASE(controller_hashing_worker_reports_hashing)
{
    int nLive = 0;
    CCPUMinerController controller(
        [&nLive](CCPUMinerController& c, CWallet*, unsigned int, unsigned int,
                 uint64_t sessionId) {
            nLive++;
            c.NotePaused(CCPUMinerController::PAUSE_NO_PEERS);
            c.NoteHashing(100);
            while (!c.WaitForStop(sessionId, 10))
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            nLive--;
        });
    std::string strError;

    BOOST_REQUIRE(controller.Start(pwalletMain, 1, strError));
    BOOST_REQUIRE(WaitForHashingOrPaused(controller));

    CCPUMinerController::Status status = controller.GetStatus();
    BOOST_CHECK(status.running);
    BOOST_CHECK(status.hashing);
    BOOST_CHECK_EQUAL(status.pauseReason,
                      (int)CCPUMinerController::PAUSE_NONE);
    BOOST_CHECK(status.lastHashProgress > 0);
    BOOST_CHECK_GE(status.totalHashes, (unsigned long long)100);
    BOOST_CHECK_GE(status.lastHashProgress, GetTime() - 5);

    controller.Stop();
}

BOOST_AUTO_TEST_CASE(controller_pause_then_resume_clears_pause_reason)
{
    int nLive = 0;
    CCPUMinerController controller(
        [&nLive](CCPUMinerController& c, CWallet*, unsigned int, unsigned int,
                 uint64_t sessionId) {
            nLive++;
            c.NotePaused(CCPUMinerController::PAUSE_CHAIN_STALE);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            c.NoteHashing(1);
            while (!c.WaitForStop(sessionId, 10))
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            nLive--;
        });
    std::string strError;

    BOOST_REQUIRE(controller.Start(pwalletMain, 1, strError));
    BOOST_REQUIRE(WaitForHashingOrPaused(controller));

    CCPUMinerController::Status status = controller.GetStatus();
    BOOST_CHECK(status.hashing);
    BOOST_CHECK_EQUAL(status.pauseReason,
                      (int)CCPUMinerController::PAUSE_NONE);
    BOOST_CHECK(status.running);

    controller.Stop();
}

BOOST_AUTO_TEST_CASE(controller_status_consistency_parked_vs_hashing)
{
    int nLive = 0;
    std::atomic<int> nFlips(0);
    std::atomic<int> nInconsistency(0);
    CCPUMinerController controller(
        [&](CCPUMinerController& c, CWallet*, unsigned int, unsigned int,
            uint64_t sessionId) {
            nLive++;
            while (!c.WaitForStop(sessionId, 5))
            {
                c.NotePaused(CCPUMinerController::PAUSE_WORK_REBUILD);
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                c.NoteHashing(1);
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                std::atomic_thread_fence(std::memory_order_seq_cst);
                nFlips.fetch_add(1);
                CCPUMinerController::Status s = c.GetStatus();
                if (s.running && !s.hashing &&
                    s.pauseReason == CCPUMinerController::PAUSE_NONE)
                    nInconsistency.fetch_add(1);
            }
            nLive--;
        });

    std::string strError;
    BOOST_REQUIRE(controller.Start(pwalletMain, 1, strError));
    for (int i = 0; i < 5000 && nFlips.load() < 20; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    BOOST_CHECK_GE(nFlips.load(), 10);
    BOOST_CHECK_EQUAL(nInconsistency.load(), 0);

    controller.Stop();
    nLive = 0;
}

BOOST_AUTO_TEST_SUITE_END()
