// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-2012 The Bitcoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "main.h"
#include "blockindex_accessor.h"
#include "db.h"
#include "init.h"
#include "miner.h"
#include "collateralnode.h"
#include "innovarpc.h"
#include "base58.h"
#include <chrono>

static int64_t RPCPerfTimeMicros()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

#define RPCPERF_LOG(...) do { try { printf(__VA_ARGS__); } catch (...) {} } while (0)

using namespace json_spirit;
using namespace std;

Value getgenerate(const Array& params, bool fHelp)
{
    if (fHelp || params.size() != 0)
        throw runtime_error(
            "getgenerate\n"
            "Returns true only while CPU mining workers are actually running.");

    return GetCPUMinerController().GetStatus().running;
}

Value getsubsidy(const Array& params, bool fHelp)
{
    if (fHelp || params.size() > 1)
        throw runtime_error(
            "getsubsidy [nTarget]\n"
            "Returns proof-of-work subsidy value for the specified value of target.");

    int nShowHeight;
    if (params.size() > 0)
        nShowHeight = atoi(params[0].get_str());
    else
        nShowHeight = nBestHeight+1; // block currently being solved

    return (uint64_t)GetProofOfWorkReward(nShowHeight, 0);
}

Value getmininginfo(const Array& params, bool fHelp)
{
    if (fHelp || params.size() != 0)
        throw runtime_error(
            "getmininginfo\n"
            "Returns mining-related information.\n"
            "cpumining is true only while CPU workers are running; cputhreads is the\n"
            "actual live-worker count and requestedcputhreads is the configured pool size.\n"
            "Network hash-rate fields are not local CPU miner performance.");

    const bool fRPCPerfTrace = GetBoolArg("-rpcperftrace", false);
    const int64_t nRPCStartTime = fRPCPerfTrace ? RPCPerfTimeMicros() : 0;
    int64_t nMeasuredMicros = 0;
    if (fRPCPerfTrace)
        RPCPERF_LOG("RPCPERF rpc=getmininginfo event=start\n");

    uint64_t nMinWeight = 0, nMaxWeight = 0, nWeight = 0;
    int64_t nSectionStart = fRPCPerfTrace ? RPCPerfTimeMicros() : 0;
    pwalletMain->GetStakeWeight(*pwalletMain, nMinWeight, nMaxWeight, nWeight);
    if (fRPCPerfTrace)
    {
        const int64_t nDuration = RPCPerfTimeMicros() - nSectionStart;
        nMeasuredMicros += nDuration;
        RPCPERF_LOG("RPCPERF rpc=getmininginfo section=stake_weight duration_us=%lld\n", (long long)nDuration);
    }

    Object obj, diff, weight;
    // S2-a: authoritative tip height + next-height reward BY VALUE (fail-closed).
    extern bool RpcAuthoritativeTipScalars(int* height, uint256* hash, std::string* error);
    int nTipHeight = -1; uint256 tipHashUnused; std::string tipErr;
    {
        if (!RpcAuthoritativeTipScalars(&nTipHeight, &tipHashUnused, &tipErr))
        {
            if (g_fAuthoritativeStartup)
                throw JSONRPCError(RPC_MISC_ERROR, tipErr.empty() ? "Active tip unavailable" : tipErr);
            nTipHeight = nBestHeight;   // non-authoritative harness only
        }
    }
    obj.push_back(Pair("blocks",        nTipHeight));
    obj.push_back(Pair("currentblocksize",(uint64_t)nLastBlockSize));
    obj.push_back(Pair("currentblocktx",(uint64_t)nLastBlockTx));

    nSectionStart = fRPCPerfTrace ? RPCPerfTimeMicros() : 0;
    extern bool ManagerDifficultyPair(double* dPoW, double* dPoS);
    double dPoW, dPoS;
    if (!ManagerDifficultyPair(&dPoW, &dPoS))
    {
        // S2-a: by-value first, fail-closed in authoritative mode.
        if (g_fAuthoritativeStartup)
            throw JSONRPCError(RPC_MISC_ERROR, "Difficulty unavailable from the authoritative block index");
        dPoW = GetDifficulty();
        dPoS = GetDifficulty(GetLastBlockIndex(pindexBest, true));
    }
    diff.push_back(Pair("proof-of-work",  dPoW));
    diff.push_back(Pair("proof-of-stake", dPoS));
    if (fRPCPerfTrace)
    {
        const int64_t nDuration = RPCPerfTimeMicros() - nSectionStart;
        nMeasuredMicros += nDuration;
        RPCPERF_LOG("RPCPERF rpc=getmininginfo section=difficulty duration_us=%lld\n", (long long)nDuration);
    }

    diff.push_back(Pair("search-interval",      (int)nLastCoinStakeSearchInterval));
    obj.push_back(Pair("difficulty",    diff));
    obj.push_back(Pair("blockvalue",    (uint64_t)GetProofOfWorkReward(nTipHeight + 1, 0)));

    nSectionStart = fRPCPerfTrace ? RPCPerfTimeMicros() : 0;
    double dPoWMHashPS = GetPoWMHashPS();
    if (fRPCPerfTrace)
    {
        const int64_t nDuration = RPCPerfTimeMicros() - nSectionStart;
        nMeasuredMicros += nDuration;
        RPCPERF_LOG("RPCPERF rpc=getmininginfo section=pow_hashrate duration_us=%lld\n", (long long)nDuration);
    }
    obj.push_back(Pair("netmhashps", dPoWMHashPS));

    nSectionStart = fRPCPerfTrace ? RPCPerfTimeMicros() : 0;
    double dPoSKernelPS = GetPoSKernelPS();
    if (fRPCPerfTrace)
    {
        const int64_t nDuration = RPCPerfTimeMicros() - nSectionStart;
        nMeasuredMicros += nDuration;
        RPCPERF_LOG("RPCPERF rpc=getmininginfo section=pos_kernel_rate duration_us=%lld\n", (long long)nDuration);
    }

    obj.push_back(Pair("netstakeweight", dPoSKernelPS));
    obj.push_back(Pair("errors",        GetWarnings("statusbar")));
    obj.push_back(Pair("pooledtx",      (uint64_t)mempool.size()));

    weight.push_back(Pair("minimum",    (uint64_t)nMinWeight));
    weight.push_back(Pair("maximum",    (uint64_t)nMaxWeight));
    weight.push_back(Pair("combined",  (uint64_t)nWeight));
    obj.push_back(Pair("stakeweight", weight));

    obj.push_back(Pair("stakeinterest",    (uint64_t)COIN_YEAR_REWARD));
    obj.push_back(Pair("testnet",       fTestNet));
    CCPUMinerController::Status cpuMining = GetCPUMinerController().GetStatus();
    obj.push_back(Pair("cpumining",     cpuMining.running));
    obj.push_back(Pair("cputhreads",    cpuMining.activeThreads));
    obj.push_back(Pair("requestedcputhreads", cpuMining.requestedThreads));
    // Additive: the same CPU-capacity figure the `gen` RPC reports, so a caller
    // can see why a requested thread count may have been clamped.
    obj.push_back(Pair("availablecputhreads", AvailableLogicalCPUs()));
    // Stage 1 additive observability: truthful hashing/pause state. Existing
    // pairs above keep their exact semantics.
    obj.push_back(Pair("cpuhashing",    cpuMining.hashing));
    obj.push_back(Pair("cpupausereason",
                        CCPUMinerController::PauseReasonString(cpuMining.pauseReason)));
    obj.push_back(Pair("cpulastHashTime", cpuMining.lastHashProgress));
    obj.push_back(Pair("cputotalhashes",
                        (uint64_t)cpuMining.totalHashes));

    if (fRPCPerfTrace)
    {
        const int64_t nTotalMicros = RPCPerfTimeMicros() - nRPCStartTime;
        RPCPERF_LOG("RPCPERF rpc=getmininginfo section=remaining_handler duration_us=%lld\n", (long long)std::max<int64_t>(0, nTotalMicros - nMeasuredMicros));
        RPCPERF_LOG("RPCPERF rpc=getmininginfo event=end total_us=%lld\n", (long long)nTotalMicros);
    }
    return obj;
}

Value getstakinginfo(const Array& params, bool fHelp)
{
    if (fHelp || params.size() != 0)
        throw runtime_error(
            "getstakinginfo\n"
            "Returns an object containing staking-related information.");

    uint64_t nMinWeight = 0, nMaxWeight = 0, nWeight = 0;
    pwalletMain->GetStakeWeight(*pwalletMain, nMinWeight, nMaxWeight, nWeight);

    double dNetworkWeight = GetPoSKernelPS();
    uint64_t nNetworkWeight = (uint64_t)dNetworkWeight;
    const bool fPoSBlockProduction = true; // linear profile: staking enabled
    bool staking = fPoSBlockProduction && nLastCoinStakeSearchInterval && nWeight;
    int64_t nExpectedTime = -1;
    if (staking && nWeight > 0 && dNetworkWeight > 0.0)
    {
        unsigned int nSpacing = GetTargetSpacingForHeight(nBestHeight + 1);
        double dExpectedTime = (double)nSpacing * dNetworkWeight / (double)nWeight;
        if (dExpectedTime < 1.0)
            nExpectedTime = 1;
        else
            nExpectedTime = (int64_t)(dExpectedTime + 0.999999);
    }

    Object obj;

    obj.push_back(Pair("enabled", GetBoolArg("-staking", true)));
    obj.push_back(Pair("staking", staking));
    obj.push_back(Pair("pos_block_production", fPoSBlockProduction));
    obj.push_back(Pair("errors", GetWarnings("statusbar")));

    obj.push_back(Pair("currentblocksize", (uint64_t)nLastBlockSize));
    obj.push_back(Pair("currentblocktx", (uint64_t)nLastBlockTx));
    obj.push_back(Pair("pooledtx", (uint64_t)mempool.size()));

    extern bool ManagerDifficultyPair(double* dPoW, double* dPoS);
    double dPoWgt, dPoSgt;
    if (!ManagerDifficultyPair(&dPoWgt, &dPoSgt))
        dPoSgt = GetDifficulty(GetLastBlockIndex(pindexBest, true));
    obj.push_back(Pair("difficulty", dPoSgt));
    obj.push_back(Pair("search-interval", (int)nLastCoinStakeSearchInterval));

    obj.push_back(Pair("weight", (uint64_t)nWeight));
    obj.push_back(Pair("netstakeweight", nNetworkWeight));

    obj.push_back(Pair("expectedtime", nExpectedTime));

    return obj;
}

// Shared JSON projection for the CPU-mining control RPCs. `mining`/`threads`
// keep the historic setgenerate field names; the requested/effective/available
// triple is what `gen` reports and what getmininginfo exposes as
// requestedcputhreads/cputhreads/availablecputhreads.
static Object CPUMiningResultToJSON(const CPUMiningApplyResult& r)
{
    Object result;
    result.push_back(Pair("mining_enabled", r.mining));
    result.push_back(Pair("mining", r.mining));
    result.push_back(Pair("threads", r.effectiveThreads));
    result.push_back(Pair("requested_threads", r.requestedThreads));
    result.push_back(Pair("effective_threads", r.effectiveThreads));
    result.push_back(Pair("available_logical_cpus", r.availableCPUs));
    if (!r.warning.empty())
        result.push_back(Pair("warning", r.warning));
    int nHeight;
    {
        LOCK(cs_main);
        nHeight = nBestHeight;
    }
    result.push_back(Pair("height", nHeight));
    return result;
}

Value gen(const Array& params, bool fHelp)
{
    if (fHelp)
        throw runtime_error(
            "gen [generate] [threads]\n"
            "Control or inspect Tribus CPU mining. This is the primary entry point;\n"
            "setgenerate is kept as a compatible alias of the same mechanism.\n"
            "\n"
            "gen                     Report state: mining_enabled, requested_threads,\n"
            "                        effective_threads, available_logical_cpus, hashing,\n"
            "                        pause_reason. Changes nothing.\n"
            "gen true [threads]      Enable mining with `threads` workers (default: the\n"
            "                        current request while mining, otherwise 1). -1 means\n"
            "                        all available CPUs. The effective count is exactly\n"
            "                        min(requested, available logical CPUs), with no other\n"
            "                        ceiling: available is affinity/cpuset/cgroup-quota\n"
            "                        aware, and a `warning` field reports any clamping.\n"
            "                        If the process is barred from every CPU, nothing runs.\n"
            "gen false               Stop mining and join the workers.\n"
            "\n"
            "A runtime change does not rewrite innova.conf: the file is re-applied at\n"
            "the next start (gen=N). Repeating the same command is idempotent and never\n"
            "creates a second worker pool.");

    if (params.empty())
    {
        const CCPUMinerController::Status st = GetCPUMinerController().GetStatus();
        bool fFallback = false;
        const int nAvailable = AvailableLogicalCPUs(&fFallback);
        Object obj;
        obj.push_back(Pair("mining_enabled", st.running));
        obj.push_back(Pair("requested_threads", st.requestedThreads));
        obj.push_back(Pair("effective_threads", st.activeThreads));
        obj.push_back(Pair("available_logical_cpus", nAvailable));
        obj.push_back(Pair("hashing", st.hashing));
        obj.push_back(Pair("hashing_workers", st.hashingWorkers));
        obj.push_back(Pair("pause_reason",
                            CCPUMinerController::PauseReasonString(st.pauseReason)));
        if (fFallback)
            obj.push_back(Pair("cpu_detection_fallback", true));
        return obj;
    }

    if (params.size() > 2)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "gen takes at most two parameters: <generate> [threads]");

    const bool fEnable = params[0].get_bool();
    if (!fEnable)
    {
        if (params.size() != 1)
            throw JSONRPCError(RPC_INVALID_PARAMETER,
                               "threads must not be specified when stopping CPU mining");
        const CPUMiningApplyResult r = ApplyCPUMiningRequest(pwalletMain, false, 0);
        if (!r.ok)
            throw JSONRPCError(RPC_INTERNAL_ERROR, r.error);
        return CPUMiningResultToJSON(r);
    }

    int nThreads = 0;
    if (params.size() > 1)
    {
        nThreads = params[1].get_int();
        if (nThreads == 0 || nThreads < -1)
            throw JSONRPCError(RPC_INVALID_PARAMETER,
                               "threads must be -1 or a positive integer");
    }
    else
    {
        const CCPUMinerController::Status st = GetCPUMinerController().GetStatus();
        nThreads = (st.running && st.requestedThreads > 0) ? st.requestedThreads : 1;
    }

    const CPUMiningApplyResult r = ApplyCPUMiningRequest(pwalletMain, true, nThreads);
    if (!r.ok)
        throw JSONRPCError(RPC_INTERNAL_ERROR, r.error);
    if (!r.warning.empty())
        printf("CPUMINING gen: %s\n", r.warning.c_str());
    return CPUMiningResultToJSON(r);
}

Value setgenerate(const Array& params, bool fHelp)
{
    if (fHelp || params.size() < 1 || params.size() > 2)
        throw runtime_error(
            "setgenerate <generate> [threads]\n"
            "Compatibility alias of `gen`: starts or stops background Tribus CPU\n"
            "mining through the SAME controller and the SAME thread-capacity rule.\n"
            "<generate> true to start mining, false to stop.\n"
            "[threads] number of workers (default: 1, -1 = all available CPUs).\n"
            "The effective count is min(requested, available logical CPUs) with no\n"
            "other ceiling (available is affinity / cpuset / cgroup-quota aware).\n"
            "CPU mining can heavily load and heat the processor. It does not guarantee a block.\n"
            "When current Collateral Node payment rules apply, 65% of the PoW reward goes\n"
            "to the Collateral Node and the miner receives 35%.");

    bool fGenerate = params[0].get_bool();

    if (!fGenerate)
    {
        if (params.size() != 1)
            throw JSONRPCError(RPC_INVALID_PARAMETER,
                               "threads must not be specified when stopping CPU mining");
        const CPUMiningApplyResult r = ApplyCPUMiningRequest(pwalletMain, false, 0);
        if (!r.ok)
            throw JSONRPCError(RPC_INTERNAL_ERROR, r.error);
        return CPUMiningResultToJSON(r);
    }

    int nThreads = 1;
    if (params.size() > 1)
        nThreads = params[1].get_int();

    if (nThreads == 0)
        throw JSONRPCError(RPC_INVALID_PARAMETER,
                           "threads=0 is invalid; use setgenerate false to stop");
    if (nThreads < -1)
        throw JSONRPCError(RPC_INVALID_PARAMETER,
                           "threads must be -1 or a positive integer");

    const CPUMiningApplyResult r = ApplyCPUMiningRequest(pwalletMain, true, nThreads);
    if (!r.ok)
        throw JSONRPCError(RPC_INTERNAL_ERROR, r.error);
    if (!r.warning.empty())
        printf("CPUMINING setgenerate: %s\n", r.warning.c_str());
    return CPUMiningResultToJSON(r);
}

Value getblocktemplate(const Array& params, bool fHelp)
{
    if (fHelp || params.size() > 1)
        throw runtime_error(
            "getblocktemplate [params]\n"
            "Returns data needed to construct a block to work on:\n"
            "  \"version\" : block version\n"
            "  \"previousblockhash\" : hash of current highest block\n"
            "  \"transactions\" : contents of non-coinbase transactions that should be included in the next block\n"
            "  \"coinbaseaux\" : data that should be included in coinbase\n"
            "  \"coinbasevalue\" : maximum allowable input to coinbase transaction, including the generation award and transaction fees\n"
            "  \"target\" : hash target\n"
            "  \"mintime\" : minimum timestamp appropriate for next block\n"
            "  \"curtime\" : current timestamp\n"
            "  \"mutable\" : list of ways the block template may be changed\n"
            "  \"noncerange\" : range of valid nonces\n"
            "  \"sigoplimit\" : limit of sigops in blocks\n"
            "  \"sizelimit\" : limit of block size\n"
            "  \"bits\" : compressed target of next block\n"
            "  \"height\" : height of the next block\n"
            "  \"payee\" : required payee\n"
            "  \"payee_amount\" : required amount to pay\n"
			      "  \"collateralnode_payments\" : true|false,         (boolean) true, if collateralnode payments are enabled"
            "  \"enforce_collateralnode_payments\" : true|false  (boolean) true, if collateralnode payments are enforced"
            "  \"masternode_payments\" : true|false,         (boolean) true, if collateralnode payments are enabled"
            "  \"enforce_masternode_payments\" : true|false  (boolean) true, if collateralnode payments are enforced"
            "See https://en.bitcoin.it/wiki/BIP_0022 for full specification.");

    std::string strMode = "template";
    if (params.size() > 0)
    {
        const Object& oparam = params[0].get_obj();
        const Value& modeval = find_value(oparam, "mode");
        if (modeval.type() == str_type)
            strMode = modeval.get_str();
        else if (modeval.type() == null_type)
        {
            /* Do nothing */
        }
        else
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid mode");
    }

    if (strMode != "template")
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid mode");

    if (vNodes.empty())
        throw JSONRPCError(RPC_CLIENT_NOT_CONNECTED, "Innova is not connected!");

    if (IsInitialBlockDownload())
        throw JSONRPCError(RPC_CLIENT_IN_INITIAL_DOWNLOAD, "Innova is downloading blocks...");

    static CReserveKey reservekey(pwalletMain);

    // Update block
    static unsigned int nTransactionsUpdatedLast;
    static CBlockIndex* pindexPrev;
    static int64_t nStart;
    static CBlock* pblock;
    if (pindexPrev != pindexBest ||
        (mempool.GetTransactionsUpdated() != nTransactionsUpdatedLast && GetTime() - nStart > 5))
    {
        // Clear pindexPrev so future calls make a new block, despite any failures from here on
        pindexPrev = NULL;

        // Store the pindexBest used before CreateNewBlock, to avoid races
        nTransactionsUpdatedLast = mempool.GetTransactionsUpdated();
        CBlockIndex* pindexPrevNew = pindexBest;
        nStart = GetTime();

        // Create new block
        if(pblock)
        {
            delete pblock;
            pblock = NULL;
        }
        pblock = CreateNewBlock(pwalletMain);
        if (!pblock)
            throw JSONRPCError(RPC_OUT_OF_MEMORY, "Out of memory");

        // Need to update only after we know CreateNewBlock succeeded
        pindexPrev = pindexPrevNew;
    }

    // Update nTime
    pblock->UpdateTime(pindexPrev);
    pblock->nNonce = 0;

    Array transactions;
    map<uint256, int64_t> setTxIndex;
    int i = 0;
    for (CTransaction& tx : pblock->vtx)
    {
        uint256 txHash = tx.GetHash();
        setTxIndex[txHash] = i++;

        if (tx.IsCoinBase() || tx.IsCoinStake())
            continue;

        Object entry;

        CDataStream ssTx(SER_NETWORK, PROTOCOL_VERSION);
        ssTx << tx;
        entry.push_back(Pair("data", HexStr(ssTx.begin(), ssTx.end())));

        entry.push_back(Pair("hash", txHash.GetHex()));

        MapPrevTx mapInputs;
        map<uint256, CTxIndex> mapUnused;
        bool fInvalid = false;
        if (tx.FetchInputs(mapUnused, false, false, mapInputs, fInvalid))
        {
            entry.push_back(Pair("fee", (int64_t)(tx.GetValueIn(mapInputs) - tx.GetValueOut())));

            Array deps;
            BOOST_FOREACH (MapPrevTx::value_type& inp, mapInputs)
            {
                if (setTxIndex.count(inp.first))
                    deps.push_back(setTxIndex[inp.first]);
            }
            entry.push_back(Pair("depends", deps));

            int64_t nSigOps = tx.GetLegacySigOpCount();
            nSigOps += tx.GetP2SHSigOpCount(mapInputs);
            entry.push_back(Pair("sigops", nSigOps));
        }

        transactions.push_back(entry);
    }

    Object aux;
    aux.push_back(Pair("flags", HexStr(COINBASE_FLAGS.begin(), COINBASE_FLAGS.end())));

    uint256 hashTarget = CBigNum().SetCompact(pblock->nBits).getuint256();

    static Array aMutable;
    if (aMutable.empty())
    {
        aMutable.push_back("time");
        aMutable.push_back("transactions");
        aMutable.push_back("prevblock");
    }

	CScript payee;

    Object result;
    result.push_back(Pair("version", pblock->nVersion));
    result.push_back(Pair("previousblockhash", pblock->hashPrevBlock.GetHex()));
    result.push_back(Pair("transactions", transactions));
    result.push_back(Pair("coinbaseaux", aux));
	  result.push_back(Pair("coinbasevalue", (int64_t)pblock->vtx[0].GetValueOut()));
    result.push_back(Pair("target", hashTarget.GetHex()));
    result.push_back(Pair("mintime", (int64_t)pindexPrev->GetPastTimeLimit()+1));
    result.push_back(Pair("mutable", aMutable));
    result.push_back(Pair("noncerange", "00000000ffffffff"));
    result.push_back(Pair("sigoplimit", (int64_t)MAX_BLOCK_SIGOPS));
    result.push_back(Pair("sizelimit", (int64_t)MAX_BLOCK_SIZE));
    result.push_back(Pair("curtime", (int64_t)pblock->nTime));
    result.push_back(Pair("bits", HexBits(pblock->nBits)));
    result.push_back(Pair("height", (int64_t)(pindexPrev->nHeight+1)));


    // ---- Collateralnode info ---

    bool bCollateralnodePayments = false;

    if(fTestNet) {
        if(pindexPrev->nHeight+1 >= BLOCK_START_COLLATERALNODE_PAYMENTS_TESTNET) bCollateralnodePayments = true;
    } else {
        if(pindexPrev->nHeight+1 >= BLOCK_START_COLLATERALNODE_PAYMENTS && pindexPrev->nHeight+1 >= 2085000) bCollateralnodePayments = true;
    }
    if(fDebug && fDebugCN) { printf("GetBlockTemplate(): Collateralnode Payments : %i\n", bCollateralnodePayments); }

    if(!collateralnodePayments.GetBlockPayee(pindexPrev->nHeight+1, payee)){
        //no collateralnode detected
		bool found = false;
                if (vecCollateralnodes.size() > 0) {
                GetCollateralnodeRanks(pindexBest);
                BOOST_FOREACH(PAIRTYPE(int, CCollateralNode*)& s, vecCollateralnodeScores)
                {
                        if (s.second->nBlockLastPaid < pindexBest->nHeight - 10) {
                                payee.SetDestination(s.second->pubkey.GetID());
                                found = true;
                                break;
                        }
                }
                }
                if (found) {
                    printf("CreateNewBlock: Found a collateralnode to pay: %s\n",payee.ToString(true).c_str());
                } else {
                    printf("CreateNewBlock: Failed to detect collateralnode to pay\n");
                    // pay the burn address if it can't detect
                    if (fDebug) printf("CreateNewBlock(): Failed to detect collateralnode to pay, burning coins.");
                    std::string burnAddress;
                    if (fTestNet) burnAddress = "8TestXXXXXXXXXXXXXXXXXXXXXXXXbCvpq";
                    else burnAddress = "INNXXXXXXXXXXXXXXXXXXXXXXXXXZeeDTw";
                    CBitcoinAddress burnAddr;
                    burnAddr.SetString(burnAddress);
                    payee = GetScriptForDestination(burnAddr.Get());
                }
    }
    if (fDebug && fDebugNet) printf("getblock : payee = %i, bCollateralnode = %i\n",payee != CScript(),bCollateralnodePayments);
    if(payee != CScript()){
		CTxDestination address1;
		ExtractDestination(payee, address1);
		CBitcoinAddress address2(address1);
		result.push_back(Pair("payee", address2.ToString().c_str()));
		result.push_back(Pair("payee_amount", (int64_t)GetCollateralnodePayment(pindexPrev->nHeight+1, pblock->vtx[0].GetValueOut())));
	  } else {
        result.push_back(Pair("payee", fTestNet ? "8TestXXXXXXXXXXXXXXXXXXXXXXXXbCvpq" : "INNXXXXXXXXXXXXXXXXXXXXXXXXXZeeDTw"));
	result.push_back(Pair("payee_amount", (int64_t)GetCollateralnodePayment(pindexPrev->nHeight+1, pblock->vtx[0].GetValueOut())));
    }

	  result.push_back(Pair("collateralnode_payments", bCollateralnodePayments));
    result.push_back(Pair("enforce_collateralnode_payments", bCollateralnodePayments));
    result.push_back(Pair("masternode_payments", bCollateralnodePayments));
    result.push_back(Pair("enforce_masternode_payments", bCollateralnodePayments));

    return result;
}

Value submitblock(const Array& params, bool fHelp)
{
    if (fHelp || params.size() < 1 || params.size() > 2)
        throw runtime_error(
            "submitblock <hex data> [optional-params-obj]\n"
            "[optional-params-obj] parameter is currently ignored.\n"
            "Attempts to submit new block to network.\n"
            "See https://en.bitcoin.it/wiki/BIP_0022 for full specification.");

    vector<unsigned char> blockData(ParseHex(params[0].get_str()));
    CDataStream ssBlock(blockData, SER_NETWORK, PROTOCOL_VERSION);
    CBlock block;
    try {
        ssBlock >> block;
    }
    catch (std::exception &e) {
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "Block decode failed");
    }

    bool fAccepted = ProcessBlock(NULL, &block);
    if (!fAccepted)
        return "rejected";

    return Value::null;
}
