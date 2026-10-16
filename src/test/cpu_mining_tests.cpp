#include <boost/test/unit_test.hpp>

#include "hashblock.h"
#include "innovarpc.h"
#include "miner.h"
#include "main.h"
#include "txdb.h"
#include "blockindex_authoritative_startup.h"
#include "blockindex_generation_builder.h"
#include "blockindex_generation_lifecycle.h"
#include "blockindex_manager.h"
#include <boost/filesystem.hpp>

namespace fs = boost::filesystem;

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
    original.vFrontierTips.push_back(uint256(2));

    CPUMiningWorkIdentity current = original;
    BOOST_CHECK(CPUMiningWorkIdentityMatches(original, current, true));

    current.hashBestChain = uint256(3);
    current.hashPrimaryParent = uint256(3);
    BOOST_CHECK(!CPUMiningWorkIdentityMatches(original, current, false));

    current = original;
    current.vFrontierTips.push_back(uint256(4));
    BOOST_CHECK(!CPUMiningWorkIdentityMatches(original, current, false));

    current = original;
    current.nTransactionsUpdated++;
    BOOST_CHECK(CPUMiningWorkIdentityMatches(original, current, false));
    BOOST_CHECK(!CPUMiningWorkIdentityMatches(original, current, true));
}

// R2c.2/S7 / audit-D: an authoritative canonical source transition that leaves
// every structural field equal (e.g. a persisted-score-only rewrite, which can
// reorder the capped merge-parent vector) must still invalidate stale work.
BOOST_AUTO_TEST_CASE(work_identity_detects_authoritative_source_only_change)
{
    CPUMiningWorkIdentity original;
    original.hashBestChain = uint256(1);
    original.hashPrimaryParent = uint256(1);
    original.nHeight = 12;
    original.vFrontierTips.push_back(uint256(2));
    original.hashDAGSourceState = uint256(0xAA);

    CPUMiningWorkIdentity current = original;
    BOOST_CHECK(CPUMiningWorkIdentityMatches(original, current, true));

    // Source-only transition: every structural field identical, token advanced.
    current.hashDAGSourceState = uint256(0xAB);
    BOOST_CHECK(!CPUMiningWorkIdentityMatches(original, current, false));
    BOOST_CHECK(!CPUMiningWorkIdentityMatches(original, current, true));

    // Legacy mode leaves the field 0 on both sides: unchanged semantics.
    CPUMiningWorkIdentity legacyA, legacyB;
    legacyA.hashBestChain = legacyB.hashBestChain = uint256(1);
    legacyA.hashPrimaryParent = legacyB.hashPrimaryParent = uint256(1);
    legacyA.vFrontierTips.push_back(uint256(2));
    legacyB.vFrontierTips.push_back(uint256(2));
    BOOST_CHECK(CPUMiningWorkIdentityMatches(legacyA, legacyB, false));
    BOOST_CHECK(!CPUMiningWorkIdentityMatches(legacyA, original, false));
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

// ---------------------------------------------------------------------------
// R7-CPU-NBITS regression: the mining TEMPLATE must resolve its difficulty the
// same way AcceptBlock enforces it.
//
// Defect (live, V2 authoritative, 2026-10-14): CreateNewBlock called the legacy
// GetNextTargetRequired(), which walks pprev POINTERS. On a sparse-hot resident
// parent (pprev == NULL) that walk collapses and the helper returns the
// proof-type LIMIT (0x1e0fffff) instead of the retargeted target, so every mined
// PoW block failed AcceptBlock's `nBits != nComputedBits` check (incorrect-bits)
// and no local block was ever accepted, while hashing itself worked.
//
// Fix: ResolveTemplateNextTarget() resolves BY VALUE through the V2 authority in
// authoritative mode (fail closed if the authority cannot answer); legacy mode is
// unchanged. The fixture builds an isolated V2 generation whose PoW ancestors
// carry nBits = 0x1d00ffff (HARDER than the 0x1e0fffff pow limit, so the retarget
// cannot clamp to the limit) and passes a hand-built SPARSE-HOT parent
// (pprev == NULL) — the exact shape that collapses the legacy walk.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_SUITE(cpu_mining_nbits_tests)

namespace
{
const unsigned int NB_POW_BITS = 0x1d00ffffu;   // harder than the pow limit
const unsigned int NB_LIVE_LIMIT_LITERAL = 0x1e0fffffu; // the value logged live

// Loads the LIVE legacy index into a generation source (so every record carries a real
// nSize computed from the real blk files — the authoritative builder REQUIRES it), then
// overrides nBits on every non-genesis record so the last same-proof-type (PoW) ancestor
// is HARDER than the pow limit (0x1d00ffff has exponent 0x1d < 0x1e). With that ancestor
// the retarget cannot clamp to bnProofOfWorkLimit, so the authoritative answer is provably
// NOT 0x1e0fffff while the legacy pointer walk still collapses to the limit.
void NBLoadSourceWithHardAncestors(const fs::path& root, BlockIndexGenerationSource& src,
                                  unsigned int nHardBits)
{
    fs::create_directories(root / "snapshot");
    { CTxDB db; db.Close(); }
    const fs::path live = GetDataDir() / "txleveldb";
    for (fs::directory_iterator it(live), end; it != end; ++it)
        if (fs::is_regular_file(it->path()))
            fs::copy_file(it->path(), root / "snapshot" / it->path().filename());
    std::string error;
    BOOST_REQUIRE_MESSAGE(ReadLegacyBlockIndexSource((root / "snapshot").string(), &src, &error), error);
    src.blockDataDir = GetDataDir().string();
    BOOST_REQUIRE(src.foundBestChain);
    for (std::size_t k = 0; k < src.records.size(); ++k)
        if (src.records[k].record.height >= 1)
            src.records[k].record.nBits = nHardBits;
}

// Mines and accepts a PoW block onto `prev` (the harness's own shape), so the live index
// is guaranteed to hold >= 3 PoW records and the authoritative builder can compute a real
// nSize for each from the blk files. Returns the accepted index or NULL.
CBlockIndex* NBMine(CBlockIndex* prev, unsigned int extra)
{
    CBlock* b = CreateNewBlock(pwalletMain, false, NULL, NULL);
    BOOST_REQUIRE(b != NULL);
    BOOST_REQUIRE(!b->vtx.empty() && !b->vtx[0].vout.empty());
    b->nVersion = 1;
    b->nTime = std::max((unsigned int)GetTime(), (unsigned int)(prev->GetMedianTimePast() + 1));
    b->hashPrevBlock = *prev->phashBlock;
    b->vtx[0].vin[0].scriptSig = CScript() << (prev->nHeight + 1) << extra;
    b->vtx[0].vout[0].scriptPubKey = CScript() << OP_TRUE;
    b->hashMerkleRoot = b->BuildMerkleTree();
    const uint256 target = CBigNum().SetCompact(b->nBits).getuint256();
    while (b->GetHash() > target && b->nNonce < 0xffffffff) ++b->nNonce;
    CBlockIndex* idx = NULL;
    bool ok = false;
    {
        LOCK(cs_main);
        const uint256 h = b->GetHash();
        const bool c = b->CheckBlock(true, true, true);
        const bool p = c && ProcessBlock(NULL, b);
        if (p) { ok = true; idx = mapBlockIndex[h]; }
    }
    delete b;
    BOOST_REQUIRE_MESSAGE(ok, "fixture block not accepted by ProcessBlock");
    BOOST_REQUIRE(idx != NULL);
    return idx;
}

// Counts records of one proof type (nFlags has no BLOCK_PROOF_OF_STAKE bit => PoW).
int NBCountByProofType(const BlockIndexGenerationSource& src, bool fProofOfStake)
{
    int n = 0;
    for (std::size_t k = 0; k < src.records.size(); ++k)
    {
        const bool isPos = (src.records[k].record.nFlags & 1u) != 0;
        if (isPos == fProofOfStake) ++n;
    }
    return n;
}

} // namespace

// The template difficulty resolver must return the AUTHORITATIVE required bits.
BOOST_AUTO_TEST_CASE(template_nbits_uses_authoritative_resolver_on_sparse_hot_parent)
{
    const std::map<uint256, CBlockIndex*> savedMapBlockIndex = mapBlockIndex;
    const std::map<uint256, CandidateTipRecord> savedCandidateTips = mapCandidateTips;
    const std::set<std::pair<COutPoint, unsigned int> > savedStakeSeen = setStakeSeen;
    CBlockIndex* savedBest = pindexBest;
    CBlockIndex* savedGenesis = pindexGenesisBlock;
    uint256 savedBestChain = hashBestChain;
    int savedBestHeight = nBestHeight;
    uint256 savedBestTrust = nBestChainTrust;
    const bool savedAuth = g_fAuthoritativeStartup;

    // Chain-anchored mock clock so this case is position-independent (an earlier suite may
    // have left the shared chain future-stamped); RAII restores the process default.
    struct NBMockTime
    {
        NBMockTime() { SetMockTime((int64_t)pindexBest->GetBlockTime() + 60); }
        ~NBMockTime() { SetMockTime(0); }
    } nbMockTime;

    const fs::path root = fs::temp_directory_path() / fs::unique_path("nb-%%%%-%%%%");
    fs::create_directories(root);
    std::string error;

    // Guarantee >= 3 PoW records (genesis + 2 ancestors) so the retarget branch runs and the
    // result is not the "first/second block" limit short-circuit.
    while (pindexBest->nHeight < 6)
        NBMine(pindexBest, 0xB7u + (unsigned)pindexBest->nHeight);

    BlockIndexGenerationSource src;
    NBLoadSourceWithHardAncestors(root, src, NB_POW_BITS);
    const int nPowRecords = NBCountByProofType(src, false);
    const int nPosRecords = NBCountByProofType(src, true);
    BOOST_TEST_MESSAGE("NB fixture: records=" << src.records.size() << " pow=" << nPowRecords
                       << " pos=" << nPosRecords << " tipHeight=" << pindexBest->nHeight);
    BOOST_REQUIRE_MESSAGE(nPowRecords >= 3,
        "fixture needs >= 3 PoW records (genesis + 2 ancestors) so the retarget branch runs");
    const uint256 tipHash = src.hashBestChain;
    int nTipHeight = 0;
    for (std::size_t k = 0; k < src.records.size(); ++k)
        if (src.records[k].hash == tipHash) nTipHeight = src.records[k].record.height;

    BlockIndexGenerationBuilder builder;
    BOOST_REQUIRE_MESSAGE(builder.Build(src, (root / "blockindex-build-000001.tmp").string(),
                                        1, NULL, &error), error);
    builder.Close();
    BOOST_REQUIRE_EQUAL(BlockIndexGenerationManager::PublishGeneration(root.string(), 1, &error),
                        BLOCK_INDEX_LIFECYCLE_OK);
    BOOST_REQUIRE_EQUAL(BlockIndexGenerationManager::SelectGeneration(root.string(), 1, &error),
                        BLOCK_INDEX_LIFECYCLE_OK);
    BOOST_REQUIRE_MESSAGE(InitBlockIndexAuthoritative(root.string(), &error), error);
    BOOST_REQUIRE(g_fAuthoritativeStartup);
    BOOST_REQUIRE(GetAuthoritativeLiveAuthority() != NULL);

    // Hand-built SPARSE-HOT parent: pprev == NULL (the shape that collapses the legacy walk).
    CBlockIndex sparse;
    sparse.phashBlock = &tipHash;
    sparse.pprev = NULL;
    sparse.nHeight = nTipHeight;
    sparse.nBits = NB_POW_BITS;
    sparse.nTime = 0;
    sparse.nFlags = 0;

    // (1) the fixture really requires a NON-limit target
    unsigned int acceptBits = 0;
    std::string rerr;
    BOOST_REQUIRE_MESSAGE(ResolveNextTargetRequiredByValue(tipHash, false, &acceptBits, &rerr), rerr);
    BOOST_CHECK_MESSAGE(acceptBits != bnProofOfWorkLimit.GetCompact(),
        "fixture must require a non-limit target: got 0x" << std::hex << acceptBits);
    BOOST_CHECK_MESSAGE(acceptBits != NB_LIVE_LIMIT_LITERAL,
        "fixture required bits must not equal the live defect value 0x1e0fffff: got 0x"
        << std::hex << acceptBits);

    // (2) the PRODUCTION template resolver must agree with the accept side
    unsigned int tplBits = 0;
    BOOST_REQUIRE_MESSAGE(ResolveTemplateNextTargetForTest(&sparse, false, &tplBits),
        "authoritative template resolution must succeed");
    BOOST_CHECK_MESSAGE(tplBits == acceptBits,
        "template nBits (0x" << std::hex << tplBits << ") must equal the accept-side required bits (0x"
        << acceptBits << ") or AcceptBlock rejects with incorrect-bits");
    BOOST_CHECK_MESSAGE(tplBits != NB_LIVE_LIMIT_LITERAL,
        "template nBits must NOT be the pow limit 0x1e0fffff (the live defect signature)");
    BOOST_CHECK_MESSAGE(tplBits != bnProofOfWorkLimit.GetCompact(),
        "template nBits must not be the pow limit");

    ResetBlockIndexAuthoritativeStartupForTest();
    mapBlockIndex = savedMapBlockIndex;
    mapCandidateTips = savedCandidateTips;
    setStakeSeen = savedStakeSeen;
    pindexBest = savedBest;
    pindexGenesisBlock = savedGenesis;
    hashBestChain = savedBestChain;
    nBestHeight = savedBestHeight;
    nBestChainTrust = savedBestTrust;
    g_fAuthoritativeStartup = savedAuth;
    fs::remove_all(root);
}

// Documents the DEFECT MECHANISM: with the legacy pointer-walking helper and a
// sparse-hot parent, the result IS the pow limit — the live 0x1e0fffff signature.
BOOST_AUTO_TEST_CASE(legacy_pointer_path_yields_pow_limit_on_sparse_hot_parent)
{
    uint256 tipHash = uint256(13);
    CBlockIndex sparse;
    sparse.phashBlock = &tipHash;
    sparse.pprev = NULL;          // sparse-hot resident parent
    sparse.nHeight = 12;
    sparse.nBits = NB_POW_BITS;   // even a harder ancestor cannot help the pointer walk
    const unsigned int legacyBits = GetNextTargetRequired(&sparse, false);
    BOOST_CHECK_MESSAGE(legacyBits == bnProofOfWorkLimit.GetCompact(),
        "legacy walk on a pprev==NULL parent must collapse to the pow limit: got 0x"
        << std::hex << legacyBits);
    BOOST_CHECK_MESSAGE(legacyBits != NB_POW_BITS,
        "legacy walk must NOT find the harder ancestor (that is the defect)");
}

BOOST_AUTO_TEST_SUITE_END()
