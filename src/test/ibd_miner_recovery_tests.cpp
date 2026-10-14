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

// main.cpp Stage 3 test hook (declared here because it was added after the
// Stage 2 hooks in main.h, to avoid touching the shipped header surface).
void PrimeIbdFreshPeerGraceLastSeenForTestingForTesting(int64_t nLastSeen);

// main.cpp Stage 3 test hook (defined in main.cpp alongside the other IBD
// grace-window test hooks; declared by including main.h for the others).
void PrimeIbdFreshPeerGraceLastSeenForTestingForTesting(int64_t nLastSeen);

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
            c.NotePaused(0, CCPUMinerController::PAUSE_IBD);
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
            c.NotePaused(0, CCPUMinerController::PAUSE_NO_PEERS);
            c.NoteHashing(0, 100);
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
            c.NotePaused(0, CCPUMinerController::PAUSE_CHAIN_STALE);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            c.NoteHashing(0, 1);
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
                c.NotePaused(0, CCPUMinerController::PAUSE_WORK_REBUILD);
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                c.NoteHashing(0, 1);
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

// ---------------------------------------------------------------------------
// Stage 2 (a): bounded grace window for FRESH-ONLY peer-ahead claims.
// ---------------------------------------------------------------------------

// (i) A peer that keeps sending fresh but FALSE height updates -- never
// delivering blocks and never having work in flight -- holds IBD inside the
// bounded grace window, but must NOT hold it once the continuous
// uncorroborated-ahead burst persists beyond the window. ENGAGED corroboration
// (work in flight) restores unbounded IBD.
BOOST_AUTO_TEST_CASE(ibd_fresh_only_false_peer_holds_ibd_within_grace_then_expires)
{
    BOOST_REQUIRE(pindexBest != NULL);
    CScopedIBDRecoveryState scope;
    ResetIbdFreshPeerGraceForTesting();

    CNode peer(INVALID_SOCKET, TestPeerAddress_ibd_rec(190),
               "ibd-rec-fresh-false-grace", true);
    ConfigurePeer(peer, /*nHeightDelta=*/+100000,
                  /*nAgeHeightUpdate=*/0, /*nAgeBlockRecv=*/1000);
    InstallPeer(peer);

    int64_t nLocalRecvSaved = nTimeBestReceived;
    nTimeBestReceived = GetTime(); // local chain progressing on its own

    // Within grace: a fresh claim far ahead is still conservative.
    BOOST_CHECK_EQUAL(IsInitialBlockDownload(), true);
    BOOST_CHECK(IbdFreshPeerGraceFirstSeenForTesting() > 0);

    // Same continuous burst, now older than the bounded grace window.
    const int64_t nWindow = IbdStaleSecondsForTesting();
    BOOST_CHECK(nWindow > 0);
    PrimeIbdFreshPeerGraceFirstSeenForTesting(GetTime() - (nWindow + 5));
    BOOST_CHECK_EQUAL(IsInitialBlockDownload(), false);

    // ENGAGED corroboration (outstanding/in-flight work) is unbounded and
    // restores IBD even after the fresh-only window expired.
    peer.setBlocksInFlight.insert(uint256(11));
    BOOST_CHECK_EQUAL(IsInitialBlockDownload(), true);

    nTimeBestReceived = nLocalRecvSaved;
    ResetIbdFreshPeerGraceForTesting();
}

// (ii) A genuinely catching-up peer WITH outstanding work stays IBD beyond the
// grace window (engaged corroboration is unbounded).
BOOST_AUTO_TEST_CASE(ibd_fresh_peer_with_outstanding_work_stays_ibd_beyond_grace)
{
    BOOST_REQUIRE(pindexBest != NULL);
    CScopedIBDRecoveryState scope;
    ResetIbdFreshPeerGraceForTesting();

    CNode peer(INVALID_SOCKET, TestPeerAddress_ibd_rec(191),
               "ibd-rec-fresh-work-beyond-grace", true);
    ConfigurePeer(peer, /*nHeightDelta=*/+5000,
                  /*nAgeHeightUpdate=*/0, /*nAgeBlockRecv=*/1000);
    peer.setBlocksInFlight.insert(uint256(12));
    InstallPeer(peer);

    const int64_t nWindow = IbdStaleSecondsForTesting();
    PrimeIbdFreshPeerGraceFirstSeenForTesting(GetTime() - (nWindow + 600));

    BOOST_CHECK_EQUAL(IsInitialBlockDownload(), true);
    // The grace clock is idle while engaged corroboration exists.
    BOOST_CHECK_EQUAL(IbdFreshPeerGraceFirstSeenForTesting(), (int64_t)0);

    ResetIbdFreshPeerGraceForTesting();
}

// A peer with requested-but-not-yet-in-flight work (mapAskFor) is equally
// engaged and therefore unbounded.
BOOST_AUTO_TEST_CASE(ibd_fresh_peer_with_asked_work_stays_ibd_beyond_grace)
{
    BOOST_REQUIRE(pindexBest != NULL);
    CScopedIBDRecoveryState scope;
    ResetIbdFreshPeerGraceForTesting();

    CNode peer(INVALID_SOCKET, TestPeerAddress_ibd_rec(192),
               "ibd-rec-fresh-askfor-beyond-grace", true);
    ConfigurePeer(peer, /*nHeightDelta=*/+5000,
                  /*nAgeHeightUpdate=*/0, /*nAgeBlockRecv=*/1000);
    CInv askInv(MSG_BLOCK, uint256(13));
    peer.mapAskFor.insert(std::make_pair((int64_t)0, askInv));
    InstallPeer(peer);

    const int64_t nWindow = IbdStaleSecondsForTesting();
    PrimeIbdFreshPeerGraceFirstSeenForTesting(GetTime() - (nWindow + 600));

    BOOST_CHECK_EQUAL(IsInitialBlockDownload(), true);
    ResetIbdFreshPeerGraceForTesting();
}

// (iii) A peer that is actually DELIVERING blocks (fresh height claim plus
// recent block receipt) stays IBD beyond the grace window.
BOOST_AUTO_TEST_CASE(ibd_fresh_peer_with_recent_block_delivery_stays_ibd_beyond_grace)
{
    BOOST_REQUIRE(pindexBest != NULL);
    CScopedIBDRecoveryState scope;
    ResetIbdFreshPeerGraceForTesting();

    CNode peer(INVALID_SOCKET, TestPeerAddress_ibd_rec(193),
               "ibd-rec-fresh-delivery-beyond-grace", true);
    ConfigurePeer(peer, /*nHeightDelta=*/+5000,
                  /*nAgeHeightUpdate=*/0, /*nAgeBlockRecv=*/0);
    InstallPeer(peer);

    const int64_t nWindow = IbdStaleSecondsForTesting();
    PrimeIbdFreshPeerGraceFirstSeenForTesting(GetTime() - (nWindow + 600));

    BOOST_CHECK_EQUAL(IsInitialBlockDownload(), true);
    ResetIbdFreshPeerGraceForTesting();
}

// A stale/fresh FLAP must not restart the bounded window: the grace clock runs
// for as long as some peer claims to be ahead without engaged activity,
// regardless of whether that claim is currently fresh. Once the claim is gone
// entirely the clock idles and a later fresh claim starts with a full window.
BOOST_AUTO_TEST_CASE(ibd_fresh_stale_flap_does_not_restart_grace_window)
{
    BOOST_REQUIRE(pindexBest != NULL);
    CScopedIBDRecoveryState scope;
    ResetIbdFreshPeerGraceForTesting();

    CNode peer(INVALID_SOCKET, TestPeerAddress_ibd_rec(194),
               "ibd-rec-fresh-stale-flap", true);
    ConfigurePeer(peer, /*nHeightDelta=*/+100000,
                  /*nAgeHeightUpdate=*/121, /*nAgeBlockRecv=*/1000); // stale claim
    InstallPeer(peer);

    int64_t nLocalRecvSaved = nTimeBestReceived;
    nTimeBestReceived = GetTime();

    const int64_t nWindow = IbdStaleSecondsForTesting();
    const int64_t nPrimed = GetTime() - (nWindow + 5);
    PrimeIbdFreshPeerGraceFirstSeenForTesting(nPrimed);

    // Stale uncorroborated claim does not hold IBD ...
    BOOST_CHECK_EQUAL(IsInitialBlockDownload(), false);
    // ... and the clock keeps running (claim still present, uncorroborated).
    const int64_t nFirstAfterStale = IbdFreshPeerGraceFirstSeenForTesting();
    BOOST_CHECK_EQUAL(nFirstAfterStale, nPrimed);

    // Now the same peer sends a FRESH update for the same inflated height: the
    // window must NOT restart, so it still must not hold IBD.
    peer.nLastHeightUpdate = GetTime();
    BOOST_CHECK_EQUAL(IsInitialBlockDownload(), false);
    BOOST_CHECK_EQUAL(IbdFreshPeerGraceFirstSeenForTesting(), nPrimed);

    // Stage 3: once the ahead claim disappears the clock IDLES (firstSeen is
    // retained) and only fully resets after a FULL grace window without any
    // uncorroborated ahead claim. A fresh claim that reappears quickly must
    // therefore resume the expired burst, not restart it.
    {
        LOCK(cs_vNodes);
        vNodes.clear();
    }
    BOOST_CHECK_EQUAL(IsInitialBlockDownload(), false);
    BOOST_CHECK_EQUAL(IbdFreshPeerGraceFirstSeenForTesting(), nPrimed);

    InstallPeer(peer);
    BOOST_CHECK_EQUAL(IsInitialBlockDownload(), false); // burst resumes, expired
    BOOST_CHECK(IbdFreshPeerGraceLastSeenForTesting() >= nPrimed);

    // Fully idle+reset (a much later genuinely new burst) is covered in
    // ibd_grace_reset_by_brief_claim_drop_is_refused below.

    nTimeBestReceived = nLocalRecvSaved;
    ResetIbdFreshPeerGraceForTesting();
}

// Stage 3 regression: the DOCUMENTED grace-window reset evasion. A peer drops
// its inflated height claim for one evaluation (the node re-evaluates IBD every
// second), the idle clock resets, and the next fresh claim re-arms a FULL
// window. Under Stage 3 the reset requires a FULL window without any
// uncorroborated ahead claim, so rapid claim dropping cannot extend mining
// suppression beyond BURST + GAP, both bounded by the same grace window.
BOOST_AUTO_TEST_CASE(ibd_grace_reset_by_brief_claim_drop_is_refused)
{
    BOOST_REQUIRE(pindexBest != NULL);
    CScopedIBDRecoveryState scope;
    ResetIbdFreshPeerGraceForTesting();

    CNode peer(INVALID_SOCKET, TestPeerAddress_ibd_rec(195),
               "ibd-rec-grace-reset-evasion", true);
    ConfigurePeer(peer, /*nHeightDelta=*/+100000,
                  /*nAgeHeightUpdate=*/0, /*nAgeBlockRecv=*/1000);
    InstallPeer(peer);

    int64_t nLocalRecvSaved = nTimeBestReceived;
    nTimeBestReceived = GetTime(); // local chain progressing on its own

    const int64_t nWindow = IbdStaleSecondsForTesting();
    BOOST_CHECK(nWindow > 0);

    // First burst: within grace the fresh inflated claim holds IBD ...
    BOOST_CHECK_EQUAL(IsInitialBlockDownload(), true);
    // ... and the burst clock starts.
    {
        const bool fBurstClockStarted =
            IbdFreshPeerGraceFirstSeenForTesting() > 0;
        BOOST_CHECK(fBurstClockStarted);
    }

    // The claim disappears for a single evaluation: the fresh-only rule gives
    // way, but the burst clock must NOT have been reset by this brief gap.
    // The optimistic margin keeps this read robust against a local block
    // confirmed between the two evaluations (claim never crosses the
    // lag-tolerance floor).
    const int nLocalHeightHop = nBestHeight;
    ConfigurePeer(peer, const_cast<int&>(nLocalHeightHop) - nLocalHeightHop + 1,
                  /*nAgeHeightUpdate=*/0, /*nAgeBlockRecv=*/1000);
    BOOST_CHECK_EQUAL(IsInitialBlockDownload(), false);
    BOOST_CHECK_MESSAGE(
        IbdFreshPeerGraceFirstSeenForTesting() > 0,
        "brief claim drop must NOT reset the burst clock");

    // Claim returns: it must resume the SAME burst (not restart a full window).
    // Prime the burst clock as expired so the resumed state is unambiguous.
    PrimeIbdFreshPeerGraceFirstSeenForTesting(GetTime() - (nWindow + 5));
    ConfigurePeer(peer, /*nHeightDelta=*/+100000, /*nAgeHeightUpdate=*/0,
                  /*nAgeBlockRecv=*/1000);
    BOOST_CHECK_EQUAL(IsInitialBlockDownload(), false);
    BOOST_CHECK_MESSAGE(
        IbdFreshPeerGraceFirstSeenForTesting() > 0 &&
            IbdFreshPeerGraceLastSeenForTesting() > 0,
        "returned claim must not have restarted the window");

    // Idleness of a FULL window is required before the clock resets: prime the
    // idle companion clock to the fully-idle branch, drop the claim once more,
    // and confirm the reset now happens (fresh start for a genuinely new peer
    // burst much later).
    {
        LOCK(cs_vNodes);
        vNodes.clear();
    }
    PrimeIbdFreshPeerGraceLastSeenForTestingForTesting(
        GetTime() - (nWindow + 5));
    BOOST_CHECK_EQUAL(IsInitialBlockDownload(), false);
    BOOST_CHECK_EQUAL(IbdFreshPeerGraceFirstSeenForTesting(), (int64_t)0);
    BOOST_CHECK_EQUAL(IbdFreshPeerGraceLastSeenForTesting(), (int64_t)0);

    nTimeBestReceived = nLocalRecvSaved;
    ResetIbdFreshPeerGraceForTesting();
}

BOOST_AUTO_TEST_CASE(controller_multithread_status_aggregation_is_per_worker_or)
{
    // Phase 0: every worker parked. Phase 1: worker 0 hashing, worker 1 parked.
    // Phase 2: worker 0 parked again.
    std::atomic<int> nPhase(0);
    std::atomic<bool> fStopReaders(false);
    std::atomic<int> nTornReads(0);
    std::atomic<int> nReaders(0);
    std::atomic<int> nLive(0);

    CCPUMinerController controller(
        [&](CCPUMinerController& c, CWallet*, unsigned int nWorkerId, unsigned int,
            uint64_t sessionId) {
            nLive++;
            if (nWorkerId == 0)
            {
                while (!c.WaitForStop(sessionId, 2))
                {
                    const int n = nPhase.load();
                    if (n == 1)
                        c.NoteHashing(0, 10);
                    else
                        c.NotePaused(0, n == 0 ? CCPUMinerController::PAUSE_WORK_REBUILD
                                               : CCPUMinerController::PAUSE_CHAIN_STALE);
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
            else
            {
                // A parked worker that keeps (re)announcing its pause reason must
                // never clear another worker's hashing state.
                c.NotePaused(nWorkerId, CCPUMinerController::PAUSE_IBD);
                while (!c.WaitForStop(sessionId, 2))
                {
                    c.NotePaused(nWorkerId, CCPUMinerController::PAUSE_IBD);
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
            }
            nLive--;
        });

    std::string strError;
    BOOST_REQUIRE(controller.Start(pwalletMain, 2, strError));
    for (int i = 0; i < 5000 && nLive.load() < 2; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    BOOST_REQUIRE_EQUAL(nLive.load(), 2);

    // Concurrent readers: assert the snapshot is never internally torn, i.e.
    // never "hashing with a pause reason" and never "hashing with zero hashing
    // workers".
    std::vector<std::thread> readers;
    for (int r = 0; r < 4; ++r)
    {
        readers.push_back(std::thread([&]() {
            nReaders++;
            while (!fStopReaders.load())
            {
                CCPUMinerController::Status s = controller.GetStatus();
                if ((s.hashing && s.pauseReason != (int)CCPUMinerController::PAUSE_NONE) ||
                    (s.hashing && s.hashingWorkers <= 0) ||
                    (s.hashingWorkers < 0))
                    nTornReads.fetch_add(1);
            }
            nReaders--;
        }));
    }

    // Phase 0: both workers parked.
    bool fSawParked = false;
    for (int i = 0; i < 5000 && !fSawParked; ++i)
    {
        CCPUMinerController::Status s = controller.GetStatus();
        fSawParked = !s.hashing && s.pauseReason != (int)CCPUMinerController::PAUSE_NONE &&
                     s.activeThreads == 2;
        if (!fSawParked)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    BOOST_CHECK(fSawParked);
    {
        CCPUMinerController::Status s = controller.GetStatus();
        BOOST_CHECK_EQUAL(s.activeThreads, 2);
        BOOST_CHECK_EQUAL(s.requestedThreads, 2);
        BOOST_CHECK(!s.hashing);
        BOOST_CHECK_EQUAL(s.hashingWorkers, 0);
    }

    // Phase 1: worker 0 hashes while worker 1 keeps announcing a pause.
    nPhase.store(1);
    bool fSawHashing = false;
    for (int i = 0; i < 5000 && !fSawHashing; ++i)
    {
        fSawHashing = controller.GetStatus().hashing;
        if (!fSawHashing)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    BOOST_CHECK(fSawHashing);
    {
        CCPUMinerController::Status s = controller.GetStatus();
        BOOST_CHECK(s.hashing);
        BOOST_CHECK_EQUAL(s.hashingWorkers, 1);
        BOOST_CHECK_EQUAL(s.pauseReason, (int)CCPUMinerController::PAUSE_NONE);
        BOOST_CHECK_EQUAL(s.activeThreads, 2);
        BOOST_CHECK_EQUAL(s.requestedThreads, 2);
        BOOST_CHECK(s.running);
    }

    // Phase 2: worker 0 parks too; the aggregate must go to not-hashing with a
    // real pause reason.
    nPhase.store(2);
    bool fSawPausedAgain = false;
    for (int i = 0; i < 5000 && !fSawPausedAgain; ++i)
    {
        CCPUMinerController::Status s = controller.GetStatus();
        fSawPausedAgain = !s.hashing;
        if (!fSawPausedAgain)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    BOOST_CHECK(fSawPausedAgain);
    {
        CCPUMinerController::Status s = controller.GetStatus();
        BOOST_CHECK(!s.hashing);
        BOOST_CHECK_EQUAL(s.hashingWorkers, 0);
        BOOST_CHECK(s.pauseReason != (int)CCPUMinerController::PAUSE_NONE);
        BOOST_CHECK_EQUAL(s.activeThreads, 2);
    }

    fStopReaders.store(true);
    for (std::thread& t : readers)
        t.join();
    BOOST_CHECK_EQUAL(nReaders.load(), 0);
    BOOST_CHECK_EQUAL(nTornReads.load(), 0);

    controller.Stop();
    {
        CCPUMinerController::Status s = controller.GetStatus();
        BOOST_CHECK(!s.hashing);
        BOOST_CHECK_EQUAL(s.hashingWorkers, 0);
        BOOST_CHECK_EQUAL(s.activeThreads, 0);
    }
}

// ---------------------------------------------------------------------------
// Stage 2 (c): totalHashes / lastHashProgress reliability.
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(controller_total_hashes_accumulate_and_progress_advances)
{
    std::atomic<bool> fGo(false);
    std::atomic<bool> fDone(false);

    CCPUMinerController controller(
        [&](CCPUMinerController& c, CWallet*, unsigned int, unsigned int,
            uint64_t sessionId) {
            c.NotePaused(0, CCPUMinerController::PAUSE_IBD);
            while (!fGo.load() && !c.WaitForStop(sessionId, 2))
            {
            }
            // Three reported batches, one of them with zero new hashes.
            c.NoteHashing(0, 1000);
            c.NoteHashing(0, 500);
            c.NoteHashing(0, 0);
            // "No per-hash accumulation": many zero-progress calls must not
            // inflate the counter, and the counter must not add a hidden +1.
            for (int i = 0; i < 200; ++i)
                c.NoteHashing(0, 0);
            fDone.store(true);
            while (!c.WaitForStop(sessionId, 5))
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        });

    std::string strError;
    BOOST_REQUIRE(controller.Start(pwalletMain, 1, strError));

    // Wait until the worker has announced it is parked and has not yet hashed.
    bool fParked = false;
    for (int i = 0; i < 5000 && !fParked; ++i)
    {
        fParked = controller.GetStatus().pauseReason == (int)CCPUMinerController::PAUSE_IBD;
        if (!fParked)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    BOOST_REQUIRE(fParked);

    CCPUMinerController::Status before = controller.GetStatus();
    BOOST_CHECK_EQUAL(before.totalHashes, (unsigned long long)0);
    BOOST_CHECK_EQUAL(before.lastHashProgress, (int64_t)0);

    fGo.store(true);
    for (int i = 0; i < 5000 && !fDone.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    BOOST_REQUIRE(fDone.load());

    CCPUMinerController::Status after = controller.GetStatus();
    BOOST_CHECK_EQUAL(after.totalHashes, (unsigned long long)1500);
    BOOST_CHECK(after.hashing);
    BOOST_CHECK_EQUAL(after.hashingWorkers, 1);
    BOOST_CHECK_EQUAL(after.pauseReason, (int)CCPUMinerController::PAUSE_NONE);
    BOOST_CHECK(after.lastHashProgress > before.lastHashProgress);
    BOOST_CHECK_GE(after.lastHashProgress, GetTime() - 5);
    BOOST_CHECK_LE(after.lastHashProgress, GetTime());

    controller.Stop();
}

BOOST_AUTO_TEST_SUITE_END()
