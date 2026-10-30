// REORG-314 isolated reproduction — consensus/chain-selection layer.
//
// Reproduces the 2026-10-29 real event shape in an ISOLATED regtest process:
//   * a common ancestor (LCA) at the current best tip,
//   * BRANCH A: 314 mined blocks that become the active chain (longer),
//   * BRANCH B: a competing branch delivered one block at a time,
//   * the natural chain selection inside SetBestChain()/Reorganize(): the switch
//     to B happens only when B's accumulated chaintrust exceeds A's, WITHOUT any
//     manual tip assignment, invalidateblock/reconsiderblock or reindex.
//
// There is NO consensus change and NO test-only bypass in this file: blocks are
// built from the production block structure, mined through the production PoW
// hash, and delivered through the production ProcessBlock/AcceptBlock path.
//
// Measurement + assertions are written to a JSON artifact under
// ~/data/reports/innova/reorg-314-reproduction/artifacts/.
#include <boost/test/unit_test.hpp>

#include "main.h"
#include "miner.h"
#include "wallet.h"
#include "ibdactivepath.h"
#include "blockindex_manager.h"
#include "util.h"
#include "serialize.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>
#include <set>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

extern CWallet* pwalletMain;

namespace reorg314 {

// Hermetic artifact directory: the default keeps the historical location (so the
// recorded artifacts stay reproducible), but RX_ARTIFACT_DIR redirects every write
// to a caller-owned directory. Without this the suites write into the shared
// report corpus of whatever checkout runs them.
static std::string ArtifactDir()
{
    const char* env = getenv("RX_ARTIFACT_DIR");
    if (env != NULL && env[0] != '\0')
        return std::string(env);
    return std::string("/home/user/data/reports/innova/reorg-314-reproduction/artifacts");
}

static void EnsureArtifactDir()
{
    const std::string dir = ArtifactDir();
    // Create the leaf only: do not manufacture parent directories of a
    // caller-supplied absolute path.
    mkdir(dir.c_str(), 0775);
}

static int64_t NowMs()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static uint64_t PeakRssKb()
{
    std::ifstream f("/proc/self/status");
    std::string line;
    uint64_t hwm = 0;
    while (std::getline(f, line))
        if (line.compare(0, 6, "VmHWM:") == 0)
            hwm = strtoull(line.c_str() + 6, NULL, 10);
    return hwm;
}

static std::string Hex(const uint256& h) { return h.ToString(); }

// A deterministic, consensus-valid regtest block with a single coinbase output.
// Timestamps advance exactly one second per block, which is the regtest target
// spacing, so the per-block difficulty rule (TIGHTER_DRIFT interval) keeps nBits
// constant instead of drifting.
// Same construction the project's own regtest tests use (invalidate_reconsider):
// the production block template, re-parented onto the requested parent, with a
// deterministic (target-spacing) timestamp and a real PoW nonce search.
static CBlock* MakeCoinbaseBlock(CBlockIndex* pindexPrev, unsigned int nTag)
{
    CBlock* pblock = CreateNewBlock(pwalletMain, false, NULL, NULL);
    if (pblock == NULL)
        return NULL;
    pblock->nVersion = 1;
    int64_t nTime = (int64_t)pindexPrev->GetBlockTime() + 1;
    if (nTime < (int64_t)pindexPrev->GetMedianTimePast() + 1)
        nTime = (int64_t)pindexPrev->GetMedianTimePast() + 1;
    pblock->nTime = (unsigned int)nTime;
    pblock->hashPrevBlock = *pindexPrev->phashBlock;
    pblock->vtx[0].nTime = pblock->nTime;   // consensus: block time >= tx time
    pblock->vtx[0].vin[0].scriptSig = CScript() << (pindexPrev->nHeight + 1) << nTag;
    pblock->hashMerkleRoot = pblock->BuildMerkleTree();

    const uint256 hashTarget = CBigNum().SetCompact(pblock->nBits).getuint256();
    while (pblock->GetHash() > hashTarget && pblock->nNonce < 0xffffffff)
        ++pblock->nNonce;
    return pblock;
}

static std::string BlockHex(const CBlock& block)
{
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << block;
    return HexStr(ss.begin(), ss.end());
}

struct CountersSnap {
    int64_t sb_total, sb_max, sb_count;
    int64_t cb_total, cb_max, cb_count;
    int64_t cs_total, cs_max, cs_count;   // Store3A chainstate commit (= undo/UTXO write)
    int64_t ai_max, rw_max, fc_max, bi_max, wc_max;
};

static CountersSnap SnapCounters()
{
    const ibdactivepath::ActivePathCounters& c = ibdactivepath::GetCounters();
    CountersSnap s;
    s.sb_total = c.setbestchain_us_total.load(); s.sb_max = c.setbestchain_us_max.load(); s.sb_count = c.setbestchain_count.load();
    s.cb_total = c.connectblock_us_total.load(); s.cb_max = c.connectblock_us_max.load(); s.cb_count = c.connectblock_count.load();
    s.cs_total = c.chainstate_commit_us_total.load(); s.cs_max = c.chainstate_commit_us_max.load(); s.cs_count = c.chainstate_commit_count.load();
    s.ai_max = c.addtoblockindex_us_max.load();
    s.rw_max = c.raw_block_write_us_max.load();
    s.fc_max = c.filecommit_us_max.load();
    s.bi_max = c.blockindex_commit_us_max.load();
    s.wc_max = c.wallet_callback_us_max.load();
    return s;
}

enum Delivery { DELIVERY_PROCESSBLOCK = 0, DELIVERY_ACCEPTBLOCK = 1, DELIVERY_REJECTED = 2 };

// Deliver a block through the real acceptance path. ProcessBlock is the peer
// entry point; when its anti-spam checkpoint work-gate refuses a non-tip parent
// in regtest (documented project behaviour), the block is still fully validated
// and indexed through AcceptBlock, which is the same consensus function
// ProcessBlock delegates to after its peer-oriented gates.
static Delivery Deliver(CBlock* pblock, uint256* outHash)
{
    const uint256 hash = pblock->GetHash();
    *outHash = hash;
    {
        LOCK(cs_main);
        if (pblock->CheckBlock(true, true, true) && ProcessBlock(NULL, pblock))
            return DELIVERY_PROCESSBLOCK;
    }
    unsigned int nFile = 0, nBlockPos = 0;
    {
        LOCK(cs_main);
        if (pblock->WriteToDisk(nFile, nBlockPos) && pblock->AcceptBlock())
            return DELIVERY_ACCEPTBLOCK;
    }
    return DELIVERY_REJECTED;
}

// Active-chain membership resolved by hash through mapBlockIndex, because the
// V2 sparse-hot index may legally carry detached pprev pointers on resident
// records (documented project behaviour). The parent is read from the block
// itself, never from pprev.
static std::set<std::string> ActiveChainSet(int maxDepth)
{
    std::set<std::string> out;
    CBlockIndex* p = pindexBest;
    while (p != NULL && (int)out.size() < maxDepth)
    {
        out.insert(p->GetBlockHash().ToString());
        uint256 prev;
        if (p->pprev != NULL)
            prev = p->pprev->GetBlockHash();
        else
        {
            CBlock b;
            if (!b.ReadFromDisk(p)) break;
            prev = b.hashPrevBlock;
        }
        std::map<uint256, CBlockIndex*>::iterator it = mapBlockIndex.find(prev);
        if (it == mapBlockIndex.end()) break;
        p = it->second;
    }
    return out;
}

struct Step {
    int height;
    std::string hash;
    std::string prev;
    std::string trust;
    int64_t deliver_ms;
    int delivery;
    bool best;
};

static void WriteJson(const std::string& name, const std::string& body)
{
    EnsureArtifactDir();
    const std::string path = ArtifactDir() + "/" + name;
    std::ofstream f(path.c_str());
    f << body;
    f.close();
    BOOST_TEST_MESSAGE("artifact: " << path);
}

static void WriteLines(const std::string& name, const std::vector<std::string>& lines)
{
    EnsureArtifactDir();
    const std::string path = ArtifactDir() + "/" + name;
    std::ofstream f(path.c_str());
    for (size_t i = 0; i < lines.size(); ++i)
        f << lines[i] << "\n";
    f.close();
    BOOST_TEST_MESSAGE("artifact: " << path << " (" << lines.size() << " blocks)");
}

} // namespace reorg314

using namespace reorg314;

BOOST_AUTO_TEST_SUITE(reorg_314_chain_tests)

// PHASE4 (1)(3): the mode-dependent chain parameters must be identical on both
// startup paths. This case verifies the LEGACY path (the fixture ran
// LoadBlockIndex(true) with fRegTest) and that applying the shared initializer
// with fRegTest=false is a strict no-op (mainnet inertness). The V2-authoritative
// path is verified on the real daemon in PHASE4-DAEMON-E2E.md (getmininginfo
// difficulty == 1 and acceptance of 0x207fffff blocks).
BOOST_AUTO_TEST_CASE(mode_params_legacy_path_and_mainnet_inertness)
{
    BOOST_REQUIRE(fRegTest);
    const CBigNum regtestPowLimit = CBigNum(~uint256(0) >> 1);

    // (a) legacy path values applied by the fixture's LoadBlockIndex()
    BOOST_CHECK(bnProofOfWorkLimit == regtestPowLimit);
    BOOST_CHECK_EQUAL(nCoinbaseMaturity, 1);
    BOOST_CHECK_EQUAL(nTargetSpacing, 1);
    BOOST_CHECK_EQUAL((int)nStakeMinAge, 0);
    BOOST_CHECK_EQUAL(GetNextTargetRequired(pindexBest, false), regtestPowLimit.GetCompact());

    // (b) mainnet inertness: with the regtest/testnet flags off, the shared
    // initializer must not touch any consensus parameter.
    const CBigNum savedPow = bnProofOfWorkLimit;
    const unsigned int savedMaturity = nCoinbaseMaturity;
    const unsigned int savedSpacing = nTargetSpacing;
    const int64_t savedStakeAge = nStakeMinAge;
    const char savedMsg[4] = {pchMessageStart[0], pchMessageStart[1],
                              pchMessageStart[2], pchMessageStart[3]};
    const bool savedReg = fRegTest, savedTest = fTestNet;
    fRegTest = false;
    fTestNet = false;
    ApplyModeDependentChainParams();
    BOOST_CHECK(bnProofOfWorkLimit == savedPow);
    BOOST_CHECK_EQUAL(nCoinbaseMaturity, savedMaturity);
    BOOST_CHECK_EQUAL(nTargetSpacing, savedSpacing);
    BOOST_CHECK_EQUAL(nStakeMinAge, savedStakeAge);
    BOOST_CHECK_EQUAL(pchMessageStart[0], (unsigned char)savedMsg[0]);
    BOOST_CHECK_EQUAL(pchMessageStart[3], (unsigned char)savedMsg[3]);

    // (c) idempotence: re-applying in regtest mode keeps the same values
    fRegTest = savedReg;
    fTestNet = savedTest;
    ApplyModeDependentChainParams();
    ApplyModeDependentChainParams();
    BOOST_CHECK(bnProofOfWorkLimit == regtestPowLimit);
    BOOST_CHECK_EQUAL(nCoinbaseMaturity, 1);
    BOOST_CHECK_EQUAL(GetNextTargetRequired(pindexBest, false), regtestPowLimit.GetCompact());
}


// Diagnostic: how does a non-tip-parent block travel in this regtest client?
// Records which delivery path accepts a side block (ProcessBlock vs AcceptBlock)
// so the deep-reorg case below is explicit about what was exercised.
BOOST_AUTO_TEST_CASE(side_branch_delivery_path_probe)
{
    BOOST_REQUIRE(pindexBest != NULL);
    CBlockIndex* pBase = pindexBest;

    CBlock* pMain = MakeCoinbaseBlock(pBase, 0x9101);
    uint256 hMain;
    BOOST_REQUIRE(Deliver(pMain, &hMain) != DELIVERY_REJECTED);
    delete pMain;
    CBlockIndex* pMainIdx = NULL;
    { LOCK(cs_main); pMainIdx = mapBlockIndex[hMain]; }
    BOOST_REQUIRE(pMainIdx != NULL);
    BOOST_REQUIRE(pMainIdx->GetBlockHash() == hashBestChain);

    CBlock* pSide = MakeCoinbaseBlock(pBase, 0x9102);   // non-tip parent
    uint256 hSide;
    const Delivery d = Deliver(pSide, &hSide);
    delete pSide;
    CBlockIndex* pSideIdx = NULL;
    { LOCK(cs_main); pSideIdx = mapBlockIndex[hSide]; }

    BOOST_TEST_MESSAGE("side-branch delivery path = "
                       << (d == DELIVERY_PROCESSBLOCK ? "ProcessBlock"
                           : d == DELIVERY_ACCEPTBLOCK ? "AcceptBlock(fallback)" : "REJECTED")
                       << "  indexed=" << (pSideIdx != NULL));
    BOOST_REQUIRE_MESSAGE(d != DELIVERY_REJECTED,
                          "a consensus-valid side-branch block must be indexable");
    BOOST_REQUIRE(pSideIdx != NULL);
    BOOST_CHECK_EQUAL(pSideIdx->nHeight, pBase->nHeight + 1);
    // The side block must NOT displace the active chain (height 1 vs 1, trust tie
    // may or may not win; assert only that the main block chain hash is unchanged
    // when the side block has equal-or-lower trust).
    if (pSideIdx->nChainTrust <= pMainIdx->nChainTrust)
        BOOST_CHECK(hashBestChain == pMainIdx->GetBlockHash());
}

// The deep reproduction: 314 disconnected blocks, natural reorg by chaintrust.
BOOST_AUTO_TEST_CASE(deep_reorg_314)
{
    BOOST_REQUIRE(pindexBest != NULL);
    // Turn on the PRODUCTION timing instrumentation (runtime flag, no code change).
    ibdactivepath::InitIBDActivePathTrace(true);
    const CountersSnap cStart = SnapCounters();
    const int64_t t0 = NowMs();
    const uint64_t rssBefore = PeakRssKb();

    CBlockIndex* pLca = pindexBest;
    const std::string lcaHash = Hex(pLca->GetBlockHash());
    const int lcaHeight = pLca->nHeight;
    const std::string lcaTrust = pLca->nChainTrust.ToString();

    // ---- BRANCH A: 314 blocks, becomes the active chain -------------------
    const int kBranchA = 314;
    std::vector<CBlockIndex*> branchA;
    branchA.push_back(pLca);
    std::vector<Step> stepsA;
    std::vector<std::string> hexA, hexB;
    int64_t miningMsA = 0, deliveryMsA = 0;
    for (int i = 1; i <= kBranchA; ++i)
    {
        const int64_t tm0 = NowMs();
        CBlock* pblock = MakeCoinbaseBlock(branchA.back(), 0x1000 + i);
        const std::string blockHex = BlockHex(*pblock);
        const uint256 pblockPrev = pblock->hashPrevBlock;
        const int64_t tm1 = NowMs();
        uint256 hash;
        const Delivery d = Deliver(pblock, &hash);
        const int64_t tm2 = NowMs();
        delete pblock;
        hexA.push_back(blockHex);
        miningMsA += (tm1 - tm0);
        deliveryMsA += (tm2 - tm1);
        BOOST_REQUIRE_MESSAGE(d != DELIVERY_REJECTED,
                              "branch A block " << i << " rejected at height " << (lcaHeight + i));
        CBlockIndex* pindex = NULL;
        { LOCK(cs_main); pindex = mapBlockIndex[hash]; }
        BOOST_REQUIRE(pindex != NULL);
        branchA.push_back(pindex);
        Step s;
        s.height = pindex->nHeight;
        s.hash = Hex(hash);
        s.prev = Hex(pblockPrev);
        s.trust = pindex->nChainTrust.ToString();
        s.deliver_ms = tm2 - tm1;
        s.delivery = (int)d;
        s.best = (hashBestChain == hash);
        stepsA.push_back(s);
    }
    BOOST_REQUIRE(branchA.back()->GetBlockHash() == hashBestChain);
    BOOST_REQUIRE_EQUAL(nBestHeight, lcaHeight + kBranchA);
    const CountersSnap cAfterA = SnapCounters();
    const std::string tipAHash = Hex(branchA.back()->GetBlockHash());
    const std::string trustA = branchA.back()->nChainTrust.ToString();
    BOOST_REQUIRE_EQUAL(nBestChainTrust.ToString(), trustA);

    // ---- BRANCH B: competing branch delivered block by block --------------
    // Regtest trust is entropy-based at/after FORK_HEIGHT_POEM, so B must carry a
    // little more height than A to be strictly heavier; the disconnect depth is
    // nevertheless exactly kBranchA (B forks at the LCA).
    const int kBranchBMax = kBranchA + 40;
    std::vector<CBlockIndex*> branchB;
    branchB.push_back(pLca);
    std::vector<Step> stepsB;
    int crossedAt = -1;
    int64_t deliverMsB = 0;
    int deliveryKind = -1;
    for (int i = 1; i <= kBranchBMax; ++i)
    {
        CBlock* pblock = MakeCoinbaseBlock(branchB.back(), 0x2000 + i);
        const std::string blockHexB = BlockHex(*pblock);
        const uint256 pblockPrevB = pblock->hashPrevBlock;
        const bool expectedActive = (crossedAt > 0);
        uint256 hash;
        const int64_t td0 = NowMs();
        const Delivery d = Deliver(pblock, &hash);
        const int64_t td1 = NowMs();
        delete pblock;
        deliverMsB += (td1 - td0);
        BOOST_REQUIRE_MESSAGE(d != DELIVERY_REJECTED,
                              "branch B block " << i << " rejected at height " << (lcaHeight + i));
        hexB.push_back(blockHexB);
        if (deliveryKind < 0)
            deliveryKind = (int)d;
        BOOST_REQUIRE_MESSAGE(d == (Delivery)deliveryKind,
                              "branch B delivery path changed at block " << i);
        CBlockIndex* pindex = NULL;
        { LOCK(cs_main); pindex = mapBlockIndex[hash]; }
        BOOST_REQUIRE(pindex != NULL);
        branchB.push_back(pindex);
        Step s;
        s.height = pindex->nHeight;
        s.hash = Hex(hash);
        s.prev = Hex(pblockPrevB);
        s.trust = pindex->nChainTrust.ToString();
        s.deliver_ms = td1 - td0;
        s.delivery = (int)d;
        s.best = (hashBestChain == hash);
        stepsB.push_back(s);

        if (hashBestChain == hash)
        {
            crossedAt = i;
            // The switch must be a TRUST decision, not a height decision and not a
            // manual tip assignment.
            BOOST_CHECK_MESSAGE(pindex->nChainTrust > branchA.back()->nChainTrust,
                                "the winning block must be strictly heavier than the old tip");
            // The rule under test is chaintrust. The height relation is recorded,
            // not asserted: regtest's entropy-based per-block trust makes the
            // winner's height an outcome, never the deciding factor.
            BOOST_TEST_MESSAGE("switch heights: old=" << branchA.back()->nHeight
                               << " incoming=" << pindex->nHeight);
            BOOST_TEST_MESSAGE("reorg crossed at branch-B block " << i
                               << " height=" << pindex->nHeight
                               << " trust=" << pindex->nChainTrust.ToString()
                               << " oldBest=" << tipAHash);
            break;
        }
        (void)expectedActive;
    }
    BOOST_REQUIRE_MESSAGE(crossedAt > 0, "the natural reorg must happen without any manual tip switch");

    const int64_t tCross = NowMs();
    const CountersSnap cAtSwitch = SnapCounters();
    CBlockIndex* pNewTip = branchB.back();

    // ---- post-reorg assertions -------------------------------------------
    BOOST_REQUIRE(hashBestChain == pNewTip->GetBlockHash());
    BOOST_REQUIRE_EQUAL(nBestHeight, lcaHeight + crossedAt);
    BOOST_REQUIRE_EQUAL(nBestChainTrust.ToString(), pNewTip->nChainTrust.ToString());
    BOOST_REQUIRE(pNewTip->nChainTrust > branchA.back()->nChainTrust);

    // Parent continuity of the delivered branch (recorded at build time).
    for (size_t i = 1; i < stepsB.size(); ++i)
        BOOST_CHECK_MESSAGE(stepsB[i].prev == stepsB[i - 1].hash,
                            "branch B block " << i << " must link to its predecessor");
    BOOST_CHECK_EQUAL(stepsB[0].prev, lcaHash);   // B forks exactly at the LCA

    // Active chain after the reorg, resolved BY HEIGHT through the production
    // authoritative index (no pprev walks, no legacy fallback).
    int disconnected = 0;
    int bAtHeight = 0;
    int aStillActive = 0;
    for (int i = 1; i <= crossedAt; ++i)
    {
        BlockIndexSnapshot snap;
        std::string err;
        const BlockIndexManagerStatus st =
            GetBlockIndexManager().GetActiveByHeight(lcaHeight + i, &snap, &err);
        BOOST_REQUIRE_MESSAGE(st == BLOCK_INDEX_MANAGER_OK,
                              "active height " << (lcaHeight + i) << " must resolve: " << err);
        if (i <= kBranchA && snap.hash.GetHex() == stepsA[i - 1].hash)
            ++aStillActive;
        if (i <= kBranchA && snap.hash.GetHex() != stepsA[i - 1].hash)
            ++disconnected;
        if (snap.hash.GetHex() == stepsB[i - 1].hash)
            ++bAtHeight;
    }
    BOOST_CHECK_EQUAL(aStillActive, 0);
    BOOST_CHECK_EQUAL(disconnected, (crossedAt < kBranchA) ? crossedAt : kBranchA);
    BOOST_CHECK_EQUAL(disconnected, kBranchA);
    BOOST_CHECK_EQUAL(bAtHeight, crossedAt);

    // The LCA height itself still resolves to the LCA block.
    {
        BlockIndexSnapshot snap;
        std::string err;
        BOOST_REQUIRE(GetBlockIndexManager().GetActiveByHeight(lcaHeight, &snap, &err) ==
                      BLOCK_INDEX_MANAGER_OK);
        BOOST_CHECK_EQUAL(snap.hash.GetHex(), lcaHash);
    }

    // Continuation after the switch: a new block must extend the NEW chain.
    {
        CBlock* pNext = MakeCoinbaseBlock(pindexBest, 0x3001);
        uint256 hNext;
        const Delivery d = Deliver(pNext, &hNext);
        delete pNext;
        BOOST_REQUIRE(d != DELIVERY_REJECTED);
        BOOST_REQUIRE_MESSAGE(hashBestChain == hNext, "post-reorg block must become the active tip");
        BOOST_CHECK_EQUAL(nBestHeight, lcaHeight + crossedAt + 1);
    }

    const CountersSnap cEnd = SnapCounters();
    const uint64_t rssAfter = PeakRssKb();
    const int64_t t1 = NowMs();

    // ---- artifact ---------------------------------------------------------
    {
        std::ostringstream os;
        os << "{\n";
        os << "  \"case\": \"deep_reorg_314\",\n";
        os << "  \"generated_utc\": " << (long)time(NULL) << ",\n";
        os << "  \"lca\": {\"height\": " << lcaHeight << ", \"hash\": \"" << lcaHash
           << "\", \"chaintrust\": \"" << lcaTrust << "\"},\n";
        os << "  \"branch_a\": {\"blocks\": " << kBranchA << ", \"tip_hash\": \"" << tipAHash
           << "\", \"tip_chaintrust\": \"" << trustA << "\", \"mining_ms\": " << miningMsA
           << ", \"delivery_ms\": " << deliveryMsA << "},\n";
        os << "  \"branch_b\": {\"blocks_delivered\": " << crossedAt
           << ", \"delivery_path\": \"" << (deliveryKind == DELIVERY_PROCESSBLOCK ? "ProcessBlock" : "AcceptBlock")
           << "\", \"delivery_ms_total\": " << deliverMsB << "},\n";
        os << "  \"reorg\": {\"height_before_switch\": " << branchA.back()->nHeight
           << ", \"height_at_switch\": " << pNewTip->nHeight
           << ", \"trust_before_switch\": \"" << trustA << "\""
           << ", \"crossed_at_block\": " << crossedAt
           << ", \"disconnected_blocks\": " << disconnected
           << ", \"connected_blocks\": " << crossedAt
           << ", \"new_height\": " << nBestHeight
           << ", \"new_hash\": \"" << Hex(hashBestChain) << "\""
           << ", \"new_chaintrust\": \"" << nBestChainTrust.ToString() << "\""
           << ", \"ms_from_first_b_to_switch\": " << (tCross - t0 - miningMsA - deliveryMsA) << "},\n";
        os << "  \"process\": {\"total_ms\": " << (t1 - t0) << ", \"vmhwm_kb_before\": " << rssBefore
           << ", \"vmhwm_kb_after\": " << rssAfter << "},\n";
        os << "  \"timing\": {"
           << "\"setbestchain\": {\"count\": " << (cEnd.sb_count - cStart.sb_count)
           << ", \"total_us\": " << (cEnd.sb_total - cStart.sb_total)
           << ", \"max_us\": " << cEnd.sb_max << "}"
           << ", \"setbestchain_max_us_before_reorg\": " << cAfterA.sb_max
           << ", \"setbestchain_max_us_incl_reorg\": " << cAtSwitch.sb_max
           << ", \"connectblock\": {\"count\": " << (cEnd.cb_count - cStart.cb_count)
           << ", \"total_us\": " << (cEnd.cb_total - cStart.cb_total)
           << ", \"max_us\": " << cEnd.cb_max << "}"
           << ", \"store3a_chainstate_commit\": {\"count\": " << (cEnd.cs_count - cStart.cs_count)
           << ", \"total_us\": " << (cEnd.cs_total - cStart.cs_total)
           << ", \"max_us\": " << cEnd.cs_max << "}"
           << ", \"addtoblockindex_max_us\": " << cEnd.ai_max
           << ", \"raw_block_write_max_us\": " << cEnd.rw_max
           << ", \"filecommit_max_us\": " << cEnd.fc_max
           << ", \"blockindex_commit_max_us\": " << cEnd.bi_max
           << ", \"wallet_callback_max_us\": " << cEnd.wc_max
           << "},\n";
        os << "  \"steps_a\": [";
        for (size_t i = 0; i < stepsA.size(); ++i)
            os << (i ? "," : "") << "{\"h\":" << stepsA[i].height << ",\"hash\":\"" << stepsA[i].hash
               << "\",\"trust\":\"" << stepsA[i].trust << "\",\"ms\":" << stepsA[i].deliver_ms << "}";
        os << "],\n  \"steps_b\": [";
        for (size_t i = 0; i < stepsB.size(); ++i)
            os << (i ? "," : "") << "{\"h\":" << stepsB[i].height << ",\"hash\":\"" << stepsB[i].hash
               << "\",\"trust\":\"" << stepsB[i].trust << "\",\"ms\":" << stepsB[i].deliver_ms
               << ",\"best\":" << (stepsB[i].best ? "true" : "false") << "}";
        os << "]\n}\n";
        WriteJson("chain-reorg-314.json", os.str());
        WriteLines("blocks-A.hex", hexA);
        WriteLines("blocks-B.hex", hexB);
    }

    // ---- production counters (deltas over this case only) ----------------
    {
        // SetBestChain() runs once per block that becomes the best tip: the 314
        // branch-A blocks, the single winning branch-B block and the continuation.
        // Side-branch deliveries do NOT switch the tip, so they must not add to it.
        const int64_t sbBlocks = cEnd.sb_count - cStart.sb_count;
        const int64_t expectedSb = kBranchA + 1 + 1;
        BOOST_TEST_MESSAGE("counters: setbestchain=" << sbBlocks << " (expected " << expectedSb << ")"
                           << " connectblock=" << (cEnd.cb_count - cStart.cb_count)
                           << " chainstate_commit=" << (cEnd.cs_count - cStart.cs_count)
                           << " setbestchain_max_us=" << cEnd.sb_max
                           << " chainstate_commit_max_us=" << cEnd.cs_max);
        BOOST_CHECK_EQUAL(sbBlocks, expectedSb);
        BOOST_CHECK((cEnd.cb_count - cStart.cb_count) >= kBranchA);
        BOOST_CHECK((cEnd.cs_count - cStart.cs_count) > 0);
        BOOST_CHECK(cAtSwitch.sb_max >= cAfterA.sb_max);   // the reorg block is a setbestchain
    }
}

BOOST_AUTO_TEST_SUITE_END()
