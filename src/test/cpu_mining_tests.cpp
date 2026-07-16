#include <boost/test/unit_test.hpp>

#include "hashblock.h"
#include "innovarpc.h"
#include "miner.h"
#include "dag.h"

#include <atomic>
#include <chrono>
#include <set>
#include <stdexcept>
#include <thread>

extern CWallet* pwalletMain;

namespace
{
struct MockWorkerState
{
    std::atomic<int> starts;
    std::atomic<int> exits;
    std::atomic<int> live;

    MockWorkerState() : starts(0), exits(0), live(0) {}

    void Run(CCPUMinerController& controller, CWallet*, unsigned int,
             unsigned int, uint64_t sessionId)
    {
        starts.fetch_add(1);
        live.fetch_add(1);
        while (!controller.WaitForStop(sessionId, 10))
        {
        }
        live.fetch_sub(1);
        exits.fetch_add(1);
    }
};

CCPUMinerController::WorkerFunction MockWorker(MockWorkerState& state)
{
    return [&state](CCPUMinerController& controller, CWallet* wallet,
                    unsigned int workerId, unsigned int threads,
                    uint64_t sessionId) {
        state.Run(controller, wallet, workerId, threads, sessionId);
    };
}

bool WaitForAtLeast(const std::atomic<int>& value, int expected)
{
    for (int i = 0; i < 5000; ++i)
    {
        if (value.load() >= expected)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return value.load() >= expected;
}
}

BOOST_AUTO_TEST_SUITE(cpu_mining_controller_tests)

BOOST_AUTO_TEST_CASE(start_stop_and_idempotent_stop)
{
    MockWorkerState workers;
    CCPUMinerController controller(MockWorker(workers));
    std::string error;

    BOOST_REQUIRE(controller.Start(pwalletMain, 1, error));
    BOOST_REQUIRE(WaitForAtLeast(workers.starts, 1));
    CCPUMinerController::Status running = controller.GetStatus();
    BOOST_CHECK(running.running);
    BOOST_CHECK_EQUAL(running.requestedThreads, 1);
    BOOST_CHECK_EQUAL(running.activeThreads, 1);

    controller.Stop();
    CCPUMinerController::Status stopped = controller.GetStatus();
    BOOST_CHECK(!stopped.running);
    BOOST_CHECK_EQUAL(stopped.requestedThreads, 0);
    BOOST_CHECK_EQUAL(stopped.activeThreads, 0);
    BOOST_CHECK_EQUAL(workers.live.load(), 0);
    BOOST_CHECK_EQUAL(workers.exits.load(), 1);

    controller.Stop();
    BOOST_CHECK_EQUAL(controller.GetStatus().activeThreads, 0);
}

BOOST_AUTO_TEST_CASE(same_thread_count_is_noop_and_different_count_restarts)
{
    MockWorkerState workers;
    CCPUMinerController controller(MockWorker(workers));
    std::string error;

    BOOST_REQUIRE(controller.Start(pwalletMain, 2, error));
    uint64_t firstSession = controller.GetStatus().sessionId;
    BOOST_REQUIRE(WaitForAtLeast(workers.starts, 2));
    BOOST_CHECK_EQUAL(workers.starts.load(), 2);

    BOOST_REQUIRE(controller.Start(pwalletMain, 2, error));
    BOOST_CHECK_EQUAL(controller.GetStatus().sessionId, firstSession);
    BOOST_CHECK_EQUAL(workers.starts.load(), 2);

    BOOST_REQUIRE(controller.Start(pwalletMain, 4, error));
    BOOST_REQUIRE(WaitForAtLeast(workers.starts, 6));
    CCPUMinerController::Status restarted = controller.GetStatus();
    BOOST_CHECK(restarted.sessionId > firstSession);
    BOOST_CHECK_EQUAL(restarted.activeThreads, 4);
    BOOST_CHECK_EQUAL(workers.starts.load(), 6);
    BOOST_CHECK_EQUAL(workers.exits.load(), 2);

    controller.Stop();
    BOOST_CHECK_EQUAL(workers.live.load(), 0);
    BOOST_CHECK_EQUAL(workers.exits.load(), 6);
}

BOOST_AUTO_TEST_CASE(partial_thread_creation_failure_rolls_back)
{
    MockWorkerState workers;
    std::atomic<int> createCalls(0);
    CCPUMinerController::ThreadFactory factory = [&createCalls](const std::function<void()>& fn) {
        int call = createCalls.fetch_add(1);
        if (call == 2)
            throw std::runtime_error("injected thread creation failure");
        return std::thread(fn);
    };
    CCPUMinerController controller(MockWorker(workers), factory);
    std::string error;

    BOOST_CHECK(!controller.Start(pwalletMain, 4, error));
    BOOST_CHECK(!error.empty());
    CCPUMinerController::Status status = controller.GetStatus();
    BOOST_CHECK(!status.running);
    BOOST_CHECK_EQUAL(status.activeThreads, 0);
    BOOST_CHECK_EQUAL(status.requestedThreads, 0);
    BOOST_CHECK_EQUAL(workers.live.load(), 0);
}

BOOST_AUTO_TEST_CASE(repeated_start_stop_cycles_leave_no_workers)
{
    MockWorkerState workers;
    CCPUMinerController controller(MockWorker(workers));
    std::string error;

    for (int i = 0; i < 100; ++i)
    {
        int threads = (i % 4) + 1;
        BOOST_REQUIRE(controller.Start(pwalletMain, threads, error));
        BOOST_CHECK(controller.GetStatus().running);
        controller.Stop();
        BOOST_CHECK_EQUAL(controller.GetStatus().activeThreads, 0);
    }
    BOOST_CHECK_EQUAL(workers.live.load(), 0);
    BOOST_CHECK_EQUAL(workers.starts.load(), workers.exits.load());
}

BOOST_AUTO_TEST_CASE(shutdown_joins_and_rejects_restart)
{
    MockWorkerState workers;
    CCPUMinerController controller(MockWorker(workers));
    std::string error;

    BOOST_REQUIRE(controller.Start(pwalletMain, 2, error));
    BOOST_REQUIRE(WaitForAtLeast(workers.starts, 2));
    controller.Shutdown();
    CCPUMinerController::Status status = controller.GetStatus();
    BOOST_CHECK(status.shutdown);
    BOOST_CHECK(!status.running);
    BOOST_CHECK_EQUAL(status.activeThreads, 0);
    BOOST_CHECK_EQUAL(workers.live.load(), 0);

    error.clear();
    BOOST_CHECK(!controller.Start(pwalletMain, 1, error));
    BOOST_CHECK(!error.empty());
}

BOOST_AUTO_TEST_CASE(explicit_extranonces_produce_distinct_coinbases_and_merkle_roots)
{
    CBlockIndex previous;
    previous.nHeight = 100;
    std::set<CScript> scripts;
    std::set<uint256> merkleRoots;

    for (unsigned int worker = 0; worker < 4; ++worker)
    {
        CBlock block;
        CTransaction coinbase;
        coinbase.vin.resize(1);
        coinbase.vin[0].prevout.SetNull();
        coinbase.vout.resize(1);
        block.vtx.push_back(coinbase);

        SetExtraNonce(&block, &previous, 1000 + worker);
        scripts.insert(block.vtx[0].vin[0].scriptSig);
        merkleRoots.insert(block.hashMerkleRoot);
    }

    BOOST_CHECK_EQUAL(scripts.size(), 4U);
    BOOST_CHECK_EQUAL(merkleRoots.size(), 4U);
}

BOOST_AUTO_TEST_CASE(work_identity_detects_same_height_reorg_and_dag_tip_change)
{
    CPUMiningWorkIdentity original;
    original.hashBestChain = uint256(1);
    original.hashPrimaryParent = uint256(1);
    original.nHeight = 101;
    original.nTransactionsUpdated = 5;
    original.vDAGTips.push_back(uint256(2));

    CPUMiningWorkIdentity current = original;
    BOOST_CHECK(CPUMiningWorkIdentityMatches(original, current, true));

    current.hashBestChain = uint256(3);
    current.hashPrimaryParent = uint256(3);
    BOOST_CHECK(!CPUMiningWorkIdentityMatches(original, current, false));

    current = original;
    current.vDAGTips.push_back(uint256(4));
    BOOST_CHECK(!CPUMiningWorkIdentityMatches(original, current, false));

    current = original;
    current.nTransactionsUpdated++;
    BOOST_CHECK(CPUMiningWorkIdentityMatches(original, current, false));
    BOOST_CHECK(!CPUMiningWorkIdentityMatches(original, current, true));
}

BOOST_AUTO_TEST_CASE(block_identity_detects_stale_parent_and_dag_commitment)
{
    CPUMiningWorkIdentity identity;
    identity.hashBestChain = uint256(1);
    identity.hashPrimaryParent = uint256(1);
    identity.nHeight = FORK_HEIGHT_DAG;
    identity.vDAGTips.push_back(uint256(1));
    identity.vDAGTips.push_back(uint256(2));

    CBlock block;
    block.hashPrevBlock = identity.hashPrimaryParent;
    CTransaction coinbase;
    coinbase.vin.resize(1);
    coinbase.vout.resize(1);
    std::vector<uint256> parents;
    parents.push_back(uint256(1));
    parents.push_back(uint256(2));
    coinbase.vout[0].scriptPubKey = BuildDAGParentScript(parents);
    block.vtx.push_back(coinbase);

    BOOST_CHECK(CPUMiningBlockMatchesWorkIdentity(block, identity));

    CPUMiningWorkIdentity replacement = identity;
    replacement.hashBestChain = uint256(3);
    replacement.hashPrimaryParent = uint256(3);
    BOOST_CHECK(!CPUMiningBlockMatchesWorkIdentity(block, replacement));

    replacement = identity;
    replacement.vDAGTips.resize(1);
    BOOST_CHECK(!CPUMiningBlockMatchesWorkIdentity(block, replacement));
}

BOOST_AUTO_TEST_CASE(rpc_reports_actual_stopped_state)
{
    json_spirit::Array params;
    BOOST_CHECK(!getgenerate(params, false).get_bool());

    json_spirit::Object info = getmininginfo(params, false).get_obj();
    BOOST_CHECK(!find_value(info, "cpumining").get_bool());
    BOOST_CHECK_EQUAL(find_value(info, "cputhreads").get_int(), 0);
    BOOST_CHECK_EQUAL(find_value(info, "requestedcputhreads").get_int(), 0);
}

BOOST_AUTO_TEST_CASE(pow_hash_path_is_tribus_and_cn_split_remains_65_percent)
{
    CBlock block;
    block.nVersion = CBlock::CURRENT_VERSION;
    block.hashPrevBlock = uint256(1);
    block.hashMerkleRoot = uint256(2);
    block.nTime = 1234567890;
    block.nBits = 0x1d00ffff;
    block.nNonce = 42;

    BOOST_CHECK(block.GetPoWHash() == Tribus(BEGIN(block.nVersion), END(block.nNonce)));
    BOOST_CHECK_EQUAL(GetCollateralnodePayment(1, 25000000), 16250000);
    BOOST_CHECK_EQUAL(25000000 - GetCollateralnodePayment(1, 25000000), 8750000);
}

BOOST_AUTO_TEST_SUITE_END()
