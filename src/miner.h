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

/** Modify the extranonce in a block */
void IncrementExtraNonce(CBlock* pblock, CBlockIndex* pindexPrev, unsigned int& nExtraNonce);

/** Set an explicit coinbase extranonce and rebuild the merkle root. */
void SetExtraNonce(CBlock* pblock, CBlockIndex* pindexPrev, unsigned int nExtraNonce);

/** Do mining precalculation */
void FormatHashBuffers(CBlock* pblock, char* pmidstate, char* pdata, char* phash1);

/** Check mined proof-of-work block */
bool CheckWork(CBlock* pblock, CWallet& wallet, CReserveKey& reservekey);

/** Check mined proof-of-stake block */
bool CheckStake(CBlock* pblock, CWallet& wallet);

/** Base sha256 mining transform */
void SHA256Transform(void* pstate, void* pinput, const void* pinit);

struct CPUMiningWorkIdentity
{
    uint256 hashBestChain;
    uint256 hashPrimaryParent;
    std::vector<uint256> vDAGTips;
    unsigned int nTransactionsUpdated;
    int nHeight;

    CPUMiningWorkIdentity()
        : nTransactionsUpdated(0), nHeight(0)
    {
    }
};

/** Compare the chain/DAG portion of two mining-work snapshots. */
bool CPUMiningWorkIdentityMatches(const CPUMiningWorkIdentity& a,
                                  const CPUMiningWorkIdentity& b,
                                  bool fCheckMempool);

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

    struct Status
    {
        State state;
        bool running;
        bool stopping;
        bool shutdown;
        int requestedThreads;
        int activeThreads;
        uint64_t sessionId;
    };

    typedef std::function<void(CCPUMinerController&, CWallet*, unsigned int,
                               unsigned int, uint64_t)> WorkerFunction;
    typedef std::function<std::thread(const std::function<void()>&)> ThreadFactory;

    explicit CCPUMinerController(const WorkerFunction& worker = WorkerFunction(),
                                 const ThreadFactory& threadFactory = ThreadFactory());
    ~CCPUMinerController();

    bool Start(CWallet* pwallet, int nThreads, std::string& strError);
    void Stop();
    void Shutdown();

    Status GetStatus() const;
    bool ShouldStop(uint64_t nSessionId) const;
    bool WaitForStop(uint64_t nSessionId, int nMilliseconds);

private:
    CCPUMinerController(const CCPUMinerController&);
    CCPUMinerController& operator=(const CCPUMinerController&);

    void WorkerEntry(CWallet* pwallet, unsigned int nWorkerId,
                     unsigned int nThreads, uint64_t nSessionId);
    void MarkWorkerFailed();
    void StopAndJoinLocked(State finalState);
    void SetState(State state);

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
};

CCPUMinerController& GetCPUMinerController();
void ShutdownCPUMining();

#endif // NOVACOIN_MINER_H
