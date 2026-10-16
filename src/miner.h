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
    // R2c.2/S7 / audit-D (OPEN D): authoritative canonical DAG source-state
    // token at capture time. Every in-tree topology / child-count / persisted
    // score mutation stages its new token inside the same authoritative
    // envelope, so two identities that agree on every structural field but
    // disagree on this token are NOT the same work: a source-only (e.g.
    // persisted recolor) transition can reorder the template's capped
    // merge-parent vector without moving hashBestChain / hashPrimaryParent /
    // vFrontierTips. Authoritative mode fills it under cs_main and fails closed if
    // it is unreadable; legacy mode leaves it 0 on both sides, so the legacy
    // comparison is unchanged.
    uint256 hashDAGSourceState;
    // R2c.2/S6: set when the authoritative primary selection was UNAVAILABLE
    // at capture time; such an identity is never considered current.
    bool fSelectionUnavailable;

    CPUMiningWorkIdentity()
        : nTransactionsUpdated(0), nHeight(0), fSelectionUnavailable(false)
    {
    }
};

/** Compare the chain/DAG portion of two mining-work snapshots. */
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

/** Check that a built block commits to the chain/DAG work snapshot. */
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

#endif // NOVACOIN_MINER_H
