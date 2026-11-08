// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2012 The Bitcoin developers
// Copyright (c) 2013 The NovaCoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#ifndef NOVACOIN_MINER_H
#define NOVACOIN_MINER_H

#include "main.h"
#include "wallet.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

/* Generate a new block, without valid proof-of-work */
CBlock* CreateNewBlock(CWallet* pwallet, bool fProofOfStake=false, int64_t* pFees = 0,
                       CReserveKey* pReserveKey = 0);


/** Set an explicit coinbase extranonce and rebuild the merkle root. */
void SetExtraNonce(CBlock* pblock, CBlockIndex* pindexPrev, unsigned int nExtraNonce);



/** Check mined proof-of-stake block */
bool CheckStake(CBlock* pblock, CWallet& wallet);

/** Base sha256 mining transform */
void SHA256Transform(void* pstate, void* pinput, const void* pinit);

struct CPUMiningWorkIdentity
{
    uint256 hashBestChain;
    uint256 hashPrimaryParent;
    std::vector<uint256> vFrontierTips;
    unsigned int nTransactionsUpdated;
    int nHeight;
    // R2c.2/S6: set when the authoritative primary selection was UNAVAILABLE
    // at capture time; such an identity is never considered current.
    bool fSelectionUnavailable;

    CPUMiningWorkIdentity()
        : nTransactionsUpdated(0), nHeight(0), fSelectionUnavailable(false)
    {
    }
};

/** Compare the chain-work portion of two mining-work snapshots. */
bool CPUMiningWorkIdentityMatches(const CPUMiningWorkIdentity& a,
                                  const CPUMiningWorkIdentity& b,
                                  bool fCheckMempool);

// R2c.2/S6 test-facing exports: call the real production CPU-mining consumers
// (capture / collateral gate / work-current check) from focused fixtures.
CPUMiningWorkIdentity CaptureCurrentCPUMiningWorkIdentityForTest();
bool IsCPUMiningCollateralStateReadyForTest();
bool IsCPUMiningWorkCurrentForTest(const CPUMiningWorkIdentity& identity, bool fCheckMempool);
// R2c.2/S6-repair: call the real production attempt-prep consumer with a
// test-scoped materialization token (released before return; no pointer
// escapes to the caller).
// R7-CPU-NBITS: difficulty source for the mining template (authoritative by-value
// resolver in V2 mode, legacy helper otherwise; false => fail closed).
bool ResolveTemplateNextTarget(const CBlockIndex* pindexPrev, bool fProofOfStake,
                               unsigned int* pBitsOut);
bool ResolveTemplateNextTargetForTest(const CBlockIndex* pindexPrev, bool fProofOfStake,
                                      unsigned int* pBitsOut);

bool PrepareCPUMiningAttemptForTest(CBlock* pblock,
                                    const CPUMiningWorkIdentity& workIdentity,
                                    unsigned int nExtraNonce,
                                    int* outParentHeight,
                                    uint256* outTarget);

/** Check that a built block commits to the chain work snapshot. */
bool CPUMiningBlockMatchesWorkIdentity(const CBlock& block,
                                       const CPUMiningWorkIdentity& identity);

class CCPUMinerController
{
public:
    static const int MAX_THREADS = 16;

    enum State
    {
        STOPPED,
        STARTING,
        RUNNING,
        STOPPING,
        SHUTTING_DOWN
    };

    // Truthful hashing-status observability: why workers are parked at a gate
    // (PAUSE_NONE means they are hashing). Ordering is stable API surface.
    enum PauseReason
    {
        PAUSE_NONE = 0,
        PAUSE_NO_PEERS,
        PAUSE_IBD,
        PAUSE_CHAIN_STALE,
        PAUSE_COLLATERAL,
        PAUSE_CREATEBLOCK_FAILED,
        PAUSE_WORK_REBUILD
    };

    struct Status
    {
        State state;
        bool running;
        bool stopping;
        bool shutdown;
        int requestedThreads;
        int activeThreads;
        uint64_t sessionId;
        // Stage 1 additive fields (legacy fields above keep their semantics).
        bool hashing;
        // Stage 2: how many workers currently report hashing. `hashing` is the
        // truthful aggregate (hashing true iff hashingWorkers > 0).
        int hashingWorkers;
        int pauseReason;
        int64_t lastHashProgress; // unix time of last hashing progress
        unsigned long long totalHashes;
    };

    typedef std::function<void(CCPUMinerController&, CWallet*, unsigned int,
                               unsigned int, uint64_t)> WorkerFunction;
    typedef std::function<std::thread(const std::function<void()>&)> ThreadFactory;

    // Stable short strings for RPC reporting (index by PauseReason).
    static const char* PauseReasonString(int reason);

    explicit CCPUMinerController(const WorkerFunction& worker = WorkerFunction(),
                                 const ThreadFactory& threadFactory = ThreadFactory());
    ~CCPUMinerController();

    bool Start(CWallet* pwallet, int nThreads, std::string& strError);
    void Stop();
    void Shutdown();

    Status GetStatus() const;
    bool ShouldStop(uint64_t nSessionId) const;
    bool WaitForStop(uint64_t nSessionId, int nMilliseconds);

    // Stage 2: truthful hashing/pause observability, aggregated per worker.
    // Workers call NoteHashing(nWorkerId, nHashes) when hashing makes progress
    // (NOT per-hash) and NotePaused(nWorkerId, reason) when parked at a gate.
    // The aggregate in GetStatus() is the per-worker OR: hashing is true iff at
    // least one worker reports hashing, and pauseReason is NONE whenever any
    // worker is hashing (so one parked worker can no longer misreport the whole
    // miner as paused while another is still hashing). No automatic restarts.
    void NoteHashing(unsigned int nWorkerId, unsigned long long nHashes);
    void NotePaused(unsigned int nWorkerId, int reason);

private:
    CCPUMinerController(const CCPUMinerController&);
    CCPUMinerController& operator=(const CCPUMinerController&);

    void WorkerEntry(CWallet* pwallet, unsigned int nWorkerId,
                     unsigned int nThreads, uint64_t nSessionId);
    void MarkWorkerFailed();
    void StopAndJoinLocked(State finalState);
    void SetState(State state);
    // Stage 2: per-worker status arrays are plain fields guarded by
    // m_stateMutex so GetStatus() takes a consistent (untorn) snapshot.
    void ClearWorkerStatusLocked();

    WorkerFunction m_worker;
    ThreadFactory m_threadFactory;
    mutable std::mutex m_lifecycleMutex;
    mutable std::mutex m_stateMutex;
    std::condition_variable m_stateChanged;
    std::condition_variable m_stopChanged;
    std::vector<std::thread> m_threads;
    std::atomic<int> m_state;
    std::atomic<bool> m_stopRequested;
    std::atomic<bool> m_shutdownRequested;
    std::atomic<bool> m_workerFailed;
    std::atomic<bool> m_startReleased;
    std::atomic<int> m_requestedThreads;
    std::atomic<int> m_activeThreads;
    std::atomic<uint64_t> m_sessionId;
    // Stage 2 per-worker status (guarded by m_stateMutex).
    std::vector<unsigned char> m_workerHashing;     // 0/1 per worker id
    std::vector<int> m_workerPauseReason;           // last reason per worker id
    // Monotonic aggregates (atomic; not part of the per-worker snapshot).
    std::atomic<int64_t> m_lastHashProgress;
    std::atomic<unsigned long long> m_totalHashes;
};

CCPUMinerController& GetCPUMinerController();
void ShutdownCPUMining();

// ---- CPU capacity (affinity / cpuset / cgroup aware) ----------------------
// Number of logical CPUs this process may actually run on: the affinity mask
// (which is what cpuset and systemd CPUAffinity also reduce), further bounded
// by a cgroup v2 CPU bandwidth quota when one is set. Never returns < 1.
// *pFallback (optional) is set to true when neither affinity nor hardware
// detection could be used and the value came from the last-resort fallback.
int AvailableLogicalCPUs(bool* pFallback = NULL);

// THE clamping rule shared by RPC gen, RPC setgenerate and -gen=N:
//   effective = max(1, min(requested, available, CCPUMinerController::MAX_THREADS))
// *pClamped (optional) is true when the request was reduced. There is exactly
// one rule so the config path and both RPC paths cannot drift.
int ClampCPUMiningThreads(int nRequested, int nAvailable, bool* pClamped = NULL);

// Parse the innova.conf value of `gen` — exactly one integer, no ambiguity:
//   absent / empty          -> GEN_CONFIG_ABSENT    (autostart off)
//   "0"                     -> GEN_CONFIG_DISABLED  (autostart off)
//   N > 0                   -> GEN_CONFIG_ENABLED   (*pnThreads = N)
//   anything else (negative, non-numeric, overflow, trailing junk)
//                           -> GEN_CONFIG_INVALID   (*pstrError set)
enum GenConfigParseResult
{
    GEN_CONFIG_ABSENT = 0,
    GEN_CONFIG_DISABLED,
    GEN_CONFIG_ENABLED,
    GEN_CONFIG_INVALID,
};
GenConfigParseResult ParseGenConfigValue(const std::string& strRaw, bool fPresent,
                                         int* pnThreads, std::string* pstrError);

// Result of one CPU-mining control request. `requestedThreads` is what the
// caller asked for (after -1 = "all available" is resolved), `effectiveThreads`
// is what the controller actually runs.
struct CPUMiningApplyResult
{
    bool ok;
    bool mining;
    int requestedThreads;
    int effectiveThreads;
    int availableCPUs;
    bool clamped;
    std::string warning;
    std::string error;

    CPUMiningApplyResult()
        : ok(false), mining(false), requestedThreads(0), effectiveThreads(0),
          availableCPUs(1), clamped(false) {}
};

// THE single internal control path for CPU mining. Both the `gen` RPC and the
// `setgenerate` compatibility alias call this, and so does the -gen=N startup
// autostart. It never creates a second pool: CCPUMinerController::Start()
// replaces the existing worker set (same count = no-op, different count =
// ordered stop+start), and Stop() joins the workers.
CPUMiningApplyResult ApplyCPUMiningRequest(CWallet* pwallet, bool fEnable, int nRequestedThreads);

#endif // NOVACOIN_MINER_H
