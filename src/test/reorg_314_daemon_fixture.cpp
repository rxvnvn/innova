// REORG-314 — DAEMON FIXTURE GENERATOR (test-only, no product change).
//
// The original 314 fixture advanced block time by exactly one second from the
// 2011 regtest genesis (nTime = 1296688602). A daemon then rejects every block
// of the fixture at the sync-checkpoint weak-work gate: the gate's anchor is the
// genesis checkpoint (nBits = 0x1e0fffff, i.e. ~2^236) and ComputeMinWork doubles
// the target only once per DAY of deltaTime, so 2 s of deltaTime yields a required
// target near 2^238 while the fixture blocks are at the regtest powLimit (~2^255)
// -> "block with too little proof-of-work". That is a FIXTURE defect, not a
// consensus defect: with realistic timestamps the same blocks must be accepted.
//
// This generator therefore:
//   * includes the LCA block (height 1) that the old dump omitted entirely,
//   * gives every block a realistic timestamp: base + i * 86400 (one day apart),
//     which keeps the DGW-required nBits at the regtest powLimit (a positive
//     spacing only ever makes bnNew EASIER and it is clamped to the limit) while
//     making deltaTime vs the genesis checkpoint large enough that the weak-work
//     gate's required target clamps to the powLimit too,
//   * builds branch A as the ACTIVE chain (production ProcessBlock) and branch B
//     as a competing branch (production WriteToDisk + AddToBlockIndex, exactly
//     what a peer-delivered side block goes through),
//   * emits hex + a manifest with the trust crossover points so the daemon
//     harness never has to guess.
//
// Nothing is written outside RX_FIXTURE_DIR (hermetic: no writes into any
// report directory, no datadir, no production path).
#include <boost/test/unit_test.hpp>

#include "main.h"
#include "miner.h"
#include "wallet.h"
#include "util.h"
#include "serialize.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <sys/stat.h>

extern CWallet* pwalletMain;

namespace reorg314daemon {

static const int kBranchA = 314;   // active branch: heights 2..315
static const int kBranchB = 315;   // competing branch: heights 2..316 (one extra)
static const int64_t kStepSec = 86400;  // one day between blocks

static std::string Hex(const uint256& h) { return h.ToString(); }

static std::string BlockHex(const CBlock& block)
{
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << block;
    return HexStr(ss.begin(), ss.end());
}

// Consensus-valid regtest block: production template re-parented onto pindexPrev
// with an EXPLICIT timestamp and a real PoW nonce search.
static CBlock* MakeBlock(CBlockIndex* pindexPrev, unsigned int nTag, int64_t nTime)
{
    CBlock* pblock = CreateNewBlock(pwalletMain, false, NULL, NULL);
    if (pblock == NULL)
        return NULL;
    pblock->nVersion = 1;
    int64_t t = nTime;
    if (t < (int64_t)pindexPrev->GetMedianTimePast() + 1)
        t = (int64_t)pindexPrev->GetMedianTimePast() + 1;
    pblock->nTime = (unsigned int)t;
    pblock->hashPrevBlock = *pindexPrev->phashBlock;
    pblock->vtx[0].nTime = pblock->nTime;
    pblock->vtx[0].vin[0].scriptSig = CScript() << (pindexPrev->nHeight + 1) << nTag;
    pblock->hashMerkleRoot = pblock->BuildMerkleTree();

    const uint256 hashTarget = CBigNum().SetCompact(pblock->nBits).getuint256();
    while (pblock->GetHash() > hashTarget && pblock->nNonce < 0xffffffff)
        ++pblock->nNonce;
    return pblock;
}

// Add a COMPETING block to the index through the same storage path a delivered
// side block uses (production WriteToDisk + AddToBlockIndex).
static CBlockIndex* AddSide(CBlockIndex* pindexPrev, unsigned int nTag, int64_t nTime)
{
    CBlock* pblock = MakeBlock(pindexPrev, nTag, nTime);
    BOOST_REQUIRE(pblock != NULL);
    CBlockIndex* pindex = NULL;
    bool fOk = false;
    {
        LOCK(cs_main);
        unsigned int nFile = 0, nBlockPos = 0;
        const bool fWrote = pblock->WriteToDisk(nFile, nBlockPos);
        const bool fAdded = fWrote && pblock->AddToBlockIndex(nFile, nBlockPos, pblock->GetHash());
        if (fAdded)
        {
            fOk = true;
            pindex = mapBlockIndex[pblock->GetHash()];
        }
    }
    delete pblock;
    BOOST_REQUIRE_MESSAGE(fOk, "side block rejected at height " << (pindexPrev->nHeight + 1));
    BOOST_REQUIRE(pindex != NULL);
    return pindex;
}

static void MkdirP(const std::string& dir)
{
    std::string cur;
    for (size_t i = 0; i < dir.size(); ++i)
    {
        cur += dir[i];
        if (dir[i] == '/' && cur.size() > 1)
            mkdir(cur.c_str(), 0775);
    }
    mkdir(dir.c_str(), 0775);
}

static void WriteLines(const std::string& dir, const char* name, const std::vector<std::string>& lines)
{
    std::ofstream f((dir + "/" + name).c_str());
    BOOST_REQUIRE_MESSAGE(f.good(), "cannot write " << dir << "/" << name);
    for (size_t i = 0; i < lines.size(); ++i)
        f << lines[i] << "\n";
    f.close();
    BOOST_REQUIRE(f.good());
}

} // namespace reorg314daemon

BOOST_AUTO_TEST_SUITE(reorg_314_daemon_fixture)

// Generates the daemon-consumable fixture (LCA + branch A + branch B) with
// realistic timestamps. Requires RX_FIXTURE_DIR; RX_FIXTURE_BASE_TIME overrides
// the first timestamp (default: now - 500 days, so the whole branch stays in the
// past and no block can trip the 2-hour future-time rule).
BOOST_AUTO_TEST_CASE(generate_fixture)
{
    using namespace reorg314daemon;

    const char* dirEnv = getenv("RX_FIXTURE_DIR");
    if (dirEnv == NULL || dirEnv[0] == '\0')
    {
        BOOST_TEST_MESSAGE("reorg_314_daemon_fixture: skipped (RX_FIXTURE_DIR not set)");
        return;
    }
    const std::string dir = dirEnv;
    MkdirP(dir);

    int64_t baseTime = (int64_t)time(NULL) - 500 * kStepSec;
    const char* baseEnv = getenv("RX_FIXTURE_BASE_TIME");
    if (baseEnv != NULL && baseEnv[0] != '\0')
        baseTime = strtoll(baseEnv, NULL, 10);

    std::vector<std::string> hexLca, hexA, hexB;

    // ---- LCA at height 1 (the block the old dump omitted) -------------------
    CBlockIndex* pLca = NULL;
    {
        CBlock* pblock = MakeBlock(pindexGenesisBlock, 0x4c4341u, baseTime);
        BOOST_REQUIRE(pblock != NULL);
        {
            LOCK(cs_main);
            const uint256 hash = pblock->GetHash();
            hexLca.push_back(BlockHex(*pblock));
            BOOST_REQUIRE_MESSAGE(pblock->CheckBlock(true, true, true), "LCA CheckBlock failed");
            BOOST_REQUIRE_MESSAGE(ProcessBlock(NULL, pblock), "LCA rejected by ProcessBlock");
            pLca = mapBlockIndex[hash];
        }
        delete pblock;
    }
    BOOST_REQUIRE(pLca != NULL);
    BOOST_REQUIRE_EQUAL(pLca->nHeight, 1);

    // ---- branch A: 314 blocks on top of the LCA (active chain) --------------
    CBlockIndex* pA = pLca;
    uint256 trustA313, trustA314;
    for (int i = 1; i <= kBranchA; ++i)
    {
        CBlock* pblock = MakeBlock(pA, 0x41000000u + (unsigned)i, baseTime + (int64_t)i * kStepSec);
        BOOST_REQUIRE(pblock != NULL);
        {
            LOCK(cs_main);
            const uint256 hash = pblock->GetHash();
            hexA.push_back(BlockHex(*pblock));
            BOOST_REQUIRE_MESSAGE(pblock->CheckBlock(true, true, true), "branch A CheckBlock failed at " << i);
            BOOST_REQUIRE_MESSAGE(ProcessBlock(NULL, pblock), "branch A block rejected at " << i);
            pA = mapBlockIndex[hash];
        }
        delete pblock;
        if (i == 313) trustA313 = pA->nChainTrust;
        if (i == 314) trustA314 = pA->nChainTrust;
    }
    BOOST_REQUIRE_EQUAL(pA->nHeight, 1 + kBranchA);
    BOOST_CHECK(pindexBest->GetBlockHash() == pA->GetBlockHash());

    const std::string trustA = pA->nChainTrust.ToString();
    uint256 trustAtB313, trustAtB314, trustAtB315;
    std::string hashB314;

    // ---- branch B: 315 competing blocks on the LCA --------------------------
    CBlockIndex* pB = pLca;
    for (int i = 1; i <= kBranchB; ++i)
    {
        CBlock* pblock = MakeBlock(pB, 0x42000000u + (unsigned)i, baseTime + (int64_t)i * kStepSec);
        BOOST_REQUIRE(pblock != NULL);
        {
            LOCK(cs_main);
            const uint256 hash = pblock->GetHash();
            hexB.push_back(BlockHex(*pblock));
            unsigned int nFile = 0, nBlockPos = 0;
            BOOST_REQUIRE_MESSAGE(pblock->WriteToDisk(nFile, nBlockPos), "branch B WriteToDisk failed at " << i);
            BOOST_REQUIRE_MESSAGE(pblock->AddToBlockIndex(nFile, nBlockPos, hash), "branch B AddToBlockIndex failed at " << i);
            pB = mapBlockIndex[hash];
        }
        delete pblock;
        if (i == 313) trustAtB313 = pB->nChainTrust;
        if (i == 314) { trustAtB314 = pB->nChainTrust; hashB314 = Hex(pB->GetBlockHash()); }
        if (i == 315) trustAtB315 = pB->nChainTrust;
    }
    BOOST_REQUIRE_EQUAL(pB->nHeight, 1 + kBranchB);

    // NOTE: inserting B@314 already reorgs the in-process chain when B@314's
    // accumulated trust exceeds A's, so pindexBest is legitimately B by now.
    // B's per-block trust runs a hair above A's at the same height (measured, not
    // assumed): the crossover that matters is against A's TIP, i.e. the chain the
    // daemon will have after A[:kBranchA].
    BOOST_CHECK_MESSAGE(trustAtB313 < trustA314,
                        "B@313 (" << trustAtB313.ToString() << ") must NOT exceed A@314 ("
                                  << trustA314.ToString() << ") -- no early switch");
    BOOST_CHECK_MESSAGE(trustAtB314 > trustA314,
                        "B@314 (" << trustAtB314.ToString() << ") must EXCEED A@314 ("
                                  << trustA314.ToString() << ") -- this is the exact crossover");
    BOOST_CHECK_MESSAGE(trustAtB315 > trustAtB314, "B@315 must exceed B@314");
    BOOST_CHECK_MESSAGE(trustAtB313 > trustA313,
                        "measured: B@313 exceeds A@313 (per-height bias recorded as evidence)");

    // ---- emit ---------------------------------------------------------------
    WriteLines(dir, "blocks-LCA.hex", hexLca);
    WriteLines(dir, "blocks-A.hex", hexA);
    WriteLines(dir, "blocks-B.hex", hexB);

    {
        std::ostringstream os;
        os << "{\n";
        os << "  \"generator\": \"reorg_314_daemon_fixture/generate_fixture\",\n";
        os << "  \"time_base\": " << baseTime << ",\n";
        os << "  \"time_step_s\": " << kStepSec << ",\n";
        os << "  \"lca\": {\"height\": 1, \"hash\": \"" << Hex(pLca->GetBlockHash())
           << "\", \"time\": " << pLca->GetBlockTime() << "},\n";
        os << "  \"branch_a\": {\"blocks\": " << kBranchA << ", \"tip\": \"" << Hex(pA->GetBlockHash())
           << "\", \"height\": " << pA->nHeight << ", \"trust\": \"" << trustA
           << "\", \"nbits\": " << pA->nBits << "},\n";
        os << "  \"branch_b\": {\"blocks\": " << kBranchB << ", \"tip\": \"" << Hex(pB->GetBlockHash())
           << "\", \"height\": " << pB->nHeight << ", \"trust\": \"" << pB->nChainTrust.ToString()
           << "\", \"nbits\": " << pB->nBits << "},\n";
        os << "  \"crossover\": {\"b313\": \"" << trustAtB313.ToString()
           << "\", \"b314\": \"" << trustAtB314.ToString()
           << "\", \"b315\": \"" << trustAtB315.ToString() << "\"},\n";
        os << "  \"trust_a\": {\"at313\": \"" << trustA313.ToString()
           << "\", \"at314\": \"" << trustA314.ToString() << "\"},\n";
        os << "  \"expected\": {\n";
        os << "    \"primary_switch_at_314th\": {\"a_blocks\": 314, \"b_blocks\": 314, \"disconnect\": 314, \"connect\": 314,\n";
        os << "       \"tip\": \"" << hashB314 << "\", \"height\": 315,\n";
        os << "       \"note\": \"feed LCA then A[:314] then B[:314]; the 314th B block crosses the trust threshold\"},\n";
        os << "    \"post_reorg_extra_block\": {\"b_blocks\": 315, \"tip\": \"" << Hex(pB->GetBlockHash())
           << "\", \"height\": 316,\n";
        os << "       \"note\": \"B[315] extends the already-reorged chain (used by restart/crash resume)\"}\n";
        os << "  }\n";
        os << "}\n";
        std::ofstream f((dir + "/manifest.json").c_str());
        BOOST_REQUIRE(f.good());
        f << os.str();
    }

    // A short human-readable record next to the JSON.
    {
        std::ofstream f((dir + "/fixture.txt").c_str());
        BOOST_REQUIRE(f.good());
        f << "lca height=1 hash=" << Hex(pLca->GetBlockHash()) << " time=" << pLca->GetBlockTime() << "\n";
        f << "branch A blocks=" << kBranchA << " tip=" << Hex(pA->GetBlockHash())
          << " height=" << pA->nHeight << " trust=" << trustA << " nbits=" << pA->nBits << "\n";
        f << "branch B blocks=" << kBranchB << " tip=" << Hex(pB->GetBlockHash())
          << " height=" << pB->nHeight << " trust=" << pB->nChainTrust.ToString() << " nbits=" << pB->nBits << "\n";
        f << "trust b313=" << trustAtB313.ToString() << " b314=" << trustAtB314.ToString()
          << " b315=" << trustAtB315.ToString() << "\n";
        f << "time_base=" << baseTime << " step=" << kStepSec << "\n";
    }
}

BOOST_AUTO_TEST_SUITE_END()
