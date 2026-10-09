// PM1-P0-06 A1-b — CURRENT-SOURCE CONNECTABLE N/2N/4N RESIDENCY HARNESS (S4/S5)
//
// Mines a REAL connectable regtest chain through the production-equivalent
// consensus path (CreateNewBlock -> CheckBlock -> ProcessBlock -> AcceptBlock ->
// AddToBlockIndex -> ConnectBlock -> SetBestChain), boots the PRODUCTION
// authoritative startup (InitBlockIndexAuthoritative) with a TEST-ONLY small
// horizon (-blockindexlivetail=32; the only test-specific difference), and
// measures persistent residency at N / 2N / 4N.
//
// Isolated temp V2 root + the suite's own disposable regtest datadir. Never the
// production datadir.
#include <boost/test/unit_test.hpp>
#include <boost/filesystem.hpp>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include "db.h"
#include "txdb.h"
#include "main.h"
#include "miner.h"
#include "wallet.h"
#include "kernel.h"
#include "blockindex_authoritative_live.h"
#include "blockindex_tip.h"
#include "blockindex_authoritative_startup.h"
#include "blockindex_generation_builder.h"
#include "blockindex_generation_lifecycle.h"

namespace fs = boost::filesystem;
extern CWallet* pwalletMain;
extern bool fPrintToConsole;

// ---- genuine-block mining via the real consensus path (window2 pattern) ----
static CBlock* P06BuildPoWBlock(CBlockIndex* pindexPrev, unsigned int nExtra)
{
    CBlock* pblock = CreateNewBlock(pwalletMain, false, NULL, NULL);
    if (!pblock) return NULL;
    pblock->nVersion = 1;
    pblock->nTime = std::max((unsigned int)GetTime(),
                             (unsigned int)(pindexPrev->GetMedianTimePast() + 1));
    pblock->hashPrevBlock = *pindexPrev->phashBlock;
    pblock->vtx[0].vin[0].scriptSig = CScript() << (pindexPrev->nHeight + 1) << nExtra;
    if (!pblock->vtx.empty() && !pblock->vtx[0].vout.empty())
        pblock->vtx[0].vout[0].scriptPubKey = CScript() << OP_DUP << OP_HASH160
            << uint160(0x0000000000000000000000000000000000000001ULL) << OP_EQUALVERIFY << OP_CHECKSIG;
    pblock->hashMerkleRoot = pblock->BuildMerkleTree();
    uint256 hashTarget = CBigNum().SetCompact(pblock->nBits).getuint256();
    while (pblock->GetHash() > hashTarget && pblock->nNonce < 0xffffffff)
        ++pblock->nNonce;
    return pblock;
}

static CBlockIndex* P06MineReal(CBlockIndex* pindexPrev, unsigned int nExtra)
{
    CBlock* pblock = P06BuildPoWBlock(pindexPrev, nExtra);
    BOOST_REQUIRE(pblock != NULL);
    CBlockIndex* pindex = NULL;
    bool fOk = false;
    {
        LOCK(cs_main);
        const uint256 hash = pblock->GetHash();
        const bool fChecked = pblock->CheckBlock(true, true, true);
        const bool fProcessed = fChecked && ProcessBlock(NULL, pblock);
        if (fProcessed) { fOk = true; pindex = mapBlockIndex[hash]; }
        else fprintf(stderr, "P06MineReal FAIL prevH=%d checkBlock=%d processBlock=%d orphaned=%d\n",
                     pindexPrev->nHeight, (int)fChecked, (int)fProcessed,
                     (int)(mapOrphanBlocks.count(hash) != 0));
    }
    delete pblock;
    BOOST_REQUIRE_MESSAGE(fOk, "mined block was not accepted by ProcessBlock");
    BOOST_REQUIRE(pindex != NULL);
    return pindex;
}

// Build + publish + select a V2 generation from the CURRENT live txleveldb.
static void P06BuildRoot(const fs::path& root, std::string* error)
{
    fs::create_directories(root / "snapshot");
    { CTxDB db; db.Close(); }
    const auto live = GetDataDir() / "txleveldb";
    for (fs::directory_iterator it(live), end; it != end; ++it)
        if (fs::is_regular_file(it->path()))
            fs::copy_file(it->path(), root / "snapshot" / it->path().filename());
    BlockIndexGenerationSource source;
    BOOST_REQUIRE_MESSAGE(ReadLegacyBlockIndexSource((root / "snapshot").string(), &source, error), *error);
    source.blockDataDir = GetDataDir().string();
    BlockIndexGenerationBuilder builder;
    BOOST_REQUIRE_MESSAGE(builder.Build(source, (root / "blockindex-build-000001.tmp").string(), 1, NULL, error), *error);
    builder.Close();
    BOOST_REQUIRE_EQUAL(BlockIndexGenerationManager::PublishGeneration(root.string(), 1, error), BLOCK_INDEX_LIFECYCLE_OK);
    BOOST_REQUIRE_EQUAL(BlockIndexGenerationManager::SelectGeneration(root.string(), 1, error), BLOCK_INDEX_LIFECYCLE_OK);
}

// Repair #4 / p06 shared-state isolation (the demonstrated cause of the suite's
// cross-case heap corruption). The cases below mine REAL blocks into the shared
// mapBlockIndex; in authoritative mode the production bounded-residency retirer
// (RetireBlockIndexBelowFloor, main.cpp) frees every entry below the live floor
// and detaches survivor links, which destroys the shared regtest chain and leaves
// pindexBest dangling for the NEXT case. Move the shared chain aside for the
// authoritative phase and restore it afterwards. This changes ONLY the harness's
// shared-state handling; no assertion is relaxed.
//
// Ownership: objects created during the authoritative phase belong to the
// authority (freed by ResetBlockIndexAuthoritativeStartupForTest) or to the test
// (AddToBlockIndex; bounded leak, never double-freed because we never delete them
// here). RAII restores the shared map even if a BOOST_REQUIRE throws mid-case.
struct P06SharedIndexIsolation
{
    std::map<uint256, CBlockIndex*> shared;   // protected: shared regtest chain
    std::map<uint256, CBlockIndex*> scratch;  // case-local: authority/mined entries
    bool armed;
    // Globals captured at detach() so an EARLY-THROW teardown still restores them
    // (a BOOST_REQUIRE mid-case must not leave pindexBest NULL for the next case).
    CBlockIndex* sBest;
    CBlockIndex* sGenesis;
    uint256 sBestChain;
    int sHeight;
    uint256 sTrust;
    P06SharedIndexIsolation()
        : armed(false), sBest(NULL), sGenesis(NULL), sHeight(-1) {}
    ~P06SharedIndexIsolation() { if (armed) restore(); }

    // Call AFTER base-chain mining + any pre-init hash capture, BEFORE
    // InitBlockIndexAuthoritative().
    void detach()
    {
        sBest = pindexBest; sGenesis = pindexGenesisBlock;
        sBestChain = hashBestChain; sHeight = nBestHeight; sTrust = nBestChainTrust;
        ClearBlockIndexAccessorState();
        ClearFindBlockByHeightCache();
        shared.swap(mapBlockIndex);
        armed = true;
    }
    // Call in teardown INSTEAD OF ResetBlockIndexAuthoritativeStartupForTest().
    void restore()
    {
        if (!armed) return;
        armed = false;
        scratch.swap(mapBlockIndex);                  // move case-local map out first
        ResetBlockIndexAuthoritativeStartupForTest();  // frees authority-owned objects
        ClearBlockIndexAccessorState();
        ClearFindBlockByHeightCache();
        shared.swap(mapBlockIndex);                    // put the protected chain back
        ClearBlockIndexAccessorState();
        ClearFindBlockByHeightCache();
        pindexBest = sBest; pindexGenesisBlock = sGenesis;
        hashBestChain = sBestChain; nBestHeight = sHeight; nBestChainTrust = sTrust;
        // Free the CASE-LOCAL mapBlockIndex entries this case mined. They are NOT
        // authority-owned: the authority retains its parents as SEPARATE by-value
        // objects (ResolveAndRetainFullParent), so the
        // ResetBlockIndexAuthoritativeStartupForTest() above freed those but NOT
        // these. Leaving them was an accepted per-case leak (LSan: 33 CBlockIndex
        // / ~7920 B per case), because 'scratch' destructs without deleting.
        for (std::map<uint256, CBlockIndex*>::iterator d = scratch.begin(); d != scratch.end(); ++d)
            delete d->second;
        scratch.clear();
    }
};

BOOST_AUTO_TEST_SUITE(blockindex_p06_a1b_residency_scaling_tests)

BOOST_AUTO_TEST_CASE(p06_a1b_s4_connectable_plateau)
{
    const int horizon = 32;
    int N = 64;
    if (const char* e = getenv("P06_N")) { int v = atoi(e); if (v > 0) N = v; }
    InitProcessBlockRejectTrace(true);
    InitAcceptBlockRejectTrace(true);
    fPrintToConsole = true; // route reject traces to stdout for localization

    // ---- 1. healthy chain: mine base blocks above the loaded regtest tip ----
    CBlockIndex* tip = pindexBest;
    while (tip->nHeight < 40)
        tip = P06MineReal(tip, 0x7000u + (unsigned)tip->nHeight);
    const int baseHeight = tip->nHeight;

    // ---- 2. isolated V2 root + PRODUCTION authoritative startup (horizon 32) ----
    const fs::path root = fs::temp_directory_path() / fs::unique_path("p06-s4-%%%%-%%%%");
    std::string error;
    P06BuildRoot(root, &error);

    CBlockIndex* savedBest = pindexBest; CBlockIndex* savedGenesis = pindexGenesisBlock;
    uint256 savedBestChain = hashBestChain; int savedBestHeight = nBestHeight;
    uint256 savedBestTrust = nBestChainTrust;
    const bool hadLivetail = mapArgs.count("-blockindexlivetail") != 0;
    const std::string savedLivetail = hadLivetail ? mapArgs["-blockindexlivetail"] : std::string();

    // Isolate the shared block index for the authoritative phase (see
    // P06SharedIndexIsolation): the production retirer must not free the shared
    // regtest chain that later cases depend on.
    P06SharedIndexIsolation iso;
    iso.detach();

    mapArgs["-blockindexlivetail"] = "32";
    BOOST_REQUIRE_MESSAGE(InitBlockIndexAuthoritative(root.string(), &error), error);
    BOOST_REQUIRE(g_fAuthoritativeStartup);
    BlockIndexAuthoritativeLive* live = GetAuthoritativeLiveAuthority();
    BOOST_REQUIRE(live != NULL);
    BOOST_REQUIRE_EQUAL(live->Horizon(), horizon);

    // ---- 3. pre-measurement precondition: active chain advances ----
    tip = pindexBest;
    const int authTipHeight = tip->nHeight;
    const int preMb = (int)mapBlockIndex.size();
    CBlockIndex* p1 = P06MineReal(tip, 0x7101u);
    {
        LOCK(cs_main);
        BOOST_REQUIRE_MESSAGE(p1->nHeight == tip->nHeight + 1, "block 1 did not extend the tip");
        BOOST_REQUIRE_MESSAGE(pindexBest == p1, "mined block is not the active best tip");
        BOOST_REQUIRE_MESSAGE(nBestHeight == p1->nHeight, "nBestHeight did not advance");
    }
    CBlockIndex* p2 = P06MineReal(p1, 0x7102u);
    {
        LOCK(cs_main);
        BOOST_REQUIRE_MESSAGE(p2->pprev == p1, "block 2 parent != block 1");
        BOOST_REQUIRE_MESSAGE(p2->nHeight >= 2, "active height < 2");
        BOOST_REQUIRE_MESSAGE(mapOrphanBlocks.empty(), "unexpected orphan accumulation");
    }
    fprintf(stderr, "P06-S4 PRECONDITION: baseHeight=%d authTipHeight=%d preMb=%d p1=%d p2=%d "
           "tipAdvanced=yes horizon=%d\n",
           baseHeight, authTipHeight, preMb, p1->nHeight, p2->nHeight, horizon);

    // ---- 4. N / 2N / 4N drive ----
    struct Snap { int i; int mb; size_t fr; size_t frPeak; int tipH; int floor; };
    std::vector<Snap> snaps;
    CBlockIndex* cur = p2;
    const int target = 4 * N;
    for (int i = 3; i <= target; ++i)
    {
        cur = P06MineReal(cur, 0x8000u + (unsigned)i);
        if (i == N || i == 2 * N || i == target)
        {
            LOCK(cs_main);
            Snap s;
            s.i = i; s.mb = (int)mapBlockIndex.size();
            s.fr = live->ResidentCount(); s.frPeak = live->ResidentPeak();
            s.tipH = cur->nHeight;
            s.floor = (nBestHeight - horizon + 1 > 0) ? (nBestHeight - horizon + 1) : 0;
            snaps.push_back(s);
            fprintf(stderr, "P06-S4 PROBE i=%d tipH=%d floor=%d mapBlockIndex=%d fullResident=%zu fullResidentPeak=%zu\n",
                   s.i, s.tipH, s.floor, s.mb, s.fr, s.frPeak);
        }
    }

    // ---- 5. repeated turnover: advance another 2*horizon ----
    for (int i = 0; i < 2 * horizon; ++i)
        cur = P06MineReal(cur, 0xA000u + (unsigned)i);
    int mbAfterTurnover = 0, tipAfterTurnover = 0;
    size_t frAfterTurnover = 0;
    {
        LOCK(cs_main);
        mbAfterTurnover = (int)mapBlockIndex.size();
        tipAfterTurnover = cur->nHeight;
        frAfterTurnover = live->ResidentCount();
    }

    fprintf(stderr, "P06-S4 TABLE horizon=%d N=%d\n", horizon, N);
    fprintf(stderr, "  turnoverTip=%d mapBlockIndexAfterTurnover=%d fullResidentAfterTurnover=%zu\n",
           tipAfterTurnover, mbAfterTurnover, frAfterTurnover);
    for (size_t k = 0; k < snaps.size(); ++k)
        fprintf(stderr, "  i=%d tipH=%d floor=%d mapBlockIndex=%d fullResident=%zu fullResidentPeak=%zu\n",
               snaps[k].i, snaps[k].tipH, snaps[k].floor, snaps[k].mb, snaps[k].fr, snaps[k].frPeak);

    iso.restore();
    pindexBest = savedBest; pindexGenesisBlock = savedGenesis;
    hashBestChain = savedBestChain; nBestHeight = savedBestHeight; nBestChainTrust = savedBestTrust;
    if (hadLivetail) mapArgs["-blockindexlivetail"] = savedLivetail; else mapArgs.erase("-blockindexlivetail");
    try { fs::remove_all(root); } catch (...) {}

    // ---- 6. bounded-plateau assertions (A1-b/A1-c acceptance) ----
    // mapBlockIndex must be WINDOW-BOUNDED (+ small fixed slack for the protected
    // tip/genesis and in-flight blocks), NOT one entry per accepted block.
    BOOST_REQUIRE_EQUAL(snaps.size(), (size_t)3);
    const int mbN = snaps[0].mb, mb2N = snaps[1].mb, mb4N = snaps[2].mb;
    BOOST_CHECK_GT(snaps[1].tipH, snaps[0].tipH);
    BOOST_CHECK_GT(snaps[2].tipH, snaps[1].tipH);
    BOOST_CHECK_GT(tipAfterTurnover, snaps[2].tipH);
    BOOST_CHECK_LE(mbN, horizon + 8);
    BOOST_CHECK_LE(mb2N, horizon + 8);
    BOOST_CHECK_LE(mb4N, horizon + 8);
    BOOST_CHECK_LE(mb4N, mbN + 4);                 // no linear growth N -> 4N
    BOOST_CHECK_LE(mbAfterTurnover, mb4N + 4);     // stable across turnover
    BOOST_CHECK_LE((int)snaps[2].fr, horizon + 16);
    BOOST_CHECK_LE((int)snaps[2].frPeak, horizon + 16);
    BOOST_CHECK_LE((int)frAfterTurnover, horizon + 16);
    fprintf(stderr, "P06-S4 PLATEAU: mapBlockIndex N/2N/4N=%d/%d/%d afterTurnover=%d "
           "(horizon=%d) -> WINDOW-BOUNDED; fullResident N/2N/4N=%zu/%zu/%zu "
           "afterTurnover=%zu -> CONSTANT\n",
           mbN, mb2N, mb4N, mbAfterTurnover, horizon,
           snaps[0].fr, snaps[1].fr, snaps[2].fr, frAfterTurnover);
}

// PM1-P0-06 A1-c — post-plateau safety proof: stake-modifier live/base seam
// parity (C1), deep historical access after eviction (C2/H3) and floor-boundary
// pointer safety (C3/H4). Uses the same connectable authoritative chain.
BOOST_AUTO_TEST_CASE(p06_a1c_safety_after_turnover)
{
    const int horizon = 32;
    const int target = 4 * 64 + 2 * horizon; // 320: several full horizon turnovers
    const int deepHeights[3] = {5, 20, horizon + 4};

    CBlockIndex* tip = pindexBest;
    while (tip->nHeight < 40) tip = P06MineReal(tip, 0x6000u + (unsigned)tip->nHeight);
    // The p06 cases share ONE regtest datadir and mine REAL blocks, so the chain
    // does NOT restart per case: a LATER case can begin far above height 40. Size
    // the height-indexed capture vector from the ACTUAL start height + the blocks
    // THIS case mines (+slack). A fixed (target+64) under-sized the vector once a
    // PRIOR case had advanced the shared chain, and `hashes[tip->nHeight]` then
    // wrote PAST THE END of the vector (the cross-case heap corruption).
    const int startH = tip->nHeight;
    // Height-indexed capture, GROWN ON DEMAND as the chain advances. The p06
    // cases share ONE regtest datadir and mine REAL blocks, and mining can even
    // adopt a longer candidate chain, so the maximum reached height is NOT
    // bounded by startH + target. A fixed-size vector under-sized here and the
    // `hashes[tip->nHeight]` write ran PAST THE END (the cross-case heap
    // corruption). Grow-on-demand keeps it exactly sized and never writes OOB;
    // below-floor probes stay indexed by absolute active height.
    std::vector<uint256> hashes((size_t)startH + 1, uint256(0));
    { LOCK(cs_main); for (std::map<uint256, CBlockIndex*>::iterator it = mapBlockIndex.begin(); it != mapBlockIndex.end(); ++it) if (it->second->nHeight >= 0 && it->second->nHeight <= startH) hashes[it->second->nHeight] = it->first; }

    const fs::path root = fs::temp_directory_path() / fs::unique_path("p06-safety-%%%%-%%%%");
    std::string error;
    P06BuildRoot(root, &error);
    CBlockIndex* savedBest = pindexBest; CBlockIndex* savedGenesis = pindexGenesisBlock;
    uint256 savedBestChain = hashBestChain; int savedBestHeight = nBestHeight;
    uint256 savedBestTrust = nBestChainTrust;
    const bool hadLivetail = mapArgs.count("-blockindexlivetail") != 0;
    const std::string savedLivetail = hadLivetail ? mapArgs["-blockindexlivetail"] : std::string();

    // Isolate the shared block index for the authoritative phase (see
    // P06SharedIndexIsolation): the production retirer must not free the shared
    // regtest chain that later cases depend on.
    P06SharedIndexIsolation iso;
    iso.detach();

    mapArgs["-blockindexlivetail"] = "32";
    BOOST_REQUIRE_MESSAGE(InitBlockIndexAuthoritative(root.string(), &error), error);
    BOOST_REQUIRE(g_fAuthoritativeStartup);
    BlockIndexAuthoritativeLive* live = GetAuthoritativeLiveAuthority();
    BOOST_REQUIRE(live != NULL);

    tip = pindexBest;
    for (int i = 0; i < target; ++i)
    {
        tip = P06MineReal(tip, 0x9000u + (unsigned)i);
        BOOST_REQUIRE(tip->nHeight >= 0);
        if ((size_t)tip->nHeight >= hashes.size())
            hashes.resize((size_t)tip->nHeight + 1, uint256(0));
        hashes[tip->nHeight] = tip->GetBlockHash();
    }
    int tipH = 0, floor = 0, mbBefore = 0;
    {
        LOCK(cs_main);
        tipH = tip->nHeight;
        floor = (nBestHeight - horizon + 1 > 0) ? (nBestHeight - horizon + 1) : 0;
        mbBefore = (int)mapBlockIndex.size();
    }
    const size_t frBefore = live->ResidentCount();
    fprintf(stderr, "P06-A1C SAFETY: tipH=%d floor=%d mapBlockIndex=%d fullResident=%zu\n",
            tipH, floor, mbBefore, frBefore);
    BOOST_REQUIRE(tipH >= target);          // several turnovers past the floor

    // ---- C1: stake-modifier live/base seam parity (by-value traversal) ----
    uint64_t mod = 0; bool gen = false;
    BOOST_CHECK_MESSAGE(ComputeNextStakeModifier(tip, mod, gen),
                        "stake modifier by-value traversal failed across the live/base seam");
    uint64_t mod2 = 0; bool gen2 = false;
    BOOST_CHECK(ComputeNextStakeModifier(tip, mod2, gen2));
    BOOST_CHECK_EQUAL(mod, mod2);           // deterministic / stable
    BOOST_CHECK_EQUAL(gen, gen2);
    // Modifier below the floor AND below the generation tip is reachable by value:
    // the tip's ancestry must contain a generated modifier resolvable across the seam.
    BOOST_CHECK_MESSAGE(mod != 0 || !gen, "unexpected zero generated modifier");

    // ---- C2/H3: deep historical access far below the floor, repeated ----
    for (int round = 0; round < 3; ++round)
    {
        for (int k = 0; k < 3; ++k)
        {
            const int h = deepHeights[k];
            if (h <= 0 || h >= (int)hashes.size() || hashes[h] == 0) continue;
            BOOST_REQUIRE(h < floor);       // genuinely below the floor
            // (a) peer-best far below floor -> by-value ancestry probe
            const int r = TipAncestorOfPeerBestKnown(hashes[h]);
            BOOST_CHECK_MESSAGE(r == 0 || r == 1, "TipAncestorOfPeerBestKnown below floor returned unknown");
            // (b) authoritative historical lookup by value
            BlockIndexSnapshot snap; std::string serr;
            BOOST_CHECK_MESSAGE(live->ResolveBlockSnapshot(hashes[h], &snap, &serr) == BlockIndexHotStatus::OK,
                                "authoritative below-floor lookup failed: " << serr);
            BOOST_CHECK_EQUAL(snap.height, h);
        }
    }
    int mbAfterDeep = 0; size_t frAfterDeep = 0;
    { LOCK(cs_main); mbAfterDeep = (int)mapBlockIndex.size(); }
    frAfterDeep = live->ResidentCount();

    // ---- C3/H4: floor-boundary pointer safety ----
    // The retirement runs at insert time with the PREVIOUS tip, so the retained
    // raw boundary trails the current tip-derived floor by one block:
    // boundary = floor - 1 (documented; still horizon+1 retained entries).
    const int boundary = (floor > 0) ? floor - 1 : 0;
    {
        LOCK(cs_main);
        int lowest = tipH;
        CBlockIndex* p = pindexBest;
        BOOST_REQUIRE(p != NULL);
        BOOST_CHECK_EQUAL(p->nHeight, tipH);
        for (int guard = 0; guard < target + 64 && p != NULL; ++guard)
        {
            if (p->nHeight < lowest) lowest = p->nHeight;
            // every surviving pointer must target retained (>= boundary) storage
            if (p->pprev) { BOOST_CHECK_GE(p->pprev->nHeight, boundary); }
            if (p->pskip) { BOOST_CHECK_GE(p->pskip->nHeight, boundary); }
            if (p->pnext) { BOOST_CHECK_GE(p->pnext->nHeight, boundary); }
            if (p->pprev == NULL) break;   // documented raw termination boundary
            p = p->pprev;
        }
        BOOST_CHECK_EQUAL(lowest, boundary); // raw chain terminates exactly at the boundary
    }

    int mbAfter = 0; { LOCK(cs_main); mbAfter = (int)mapBlockIndex.size(); }
    const size_t frAfter = live->ResidentCount();
    fprintf(stderr, "P06-A1C SAFETY RESULT: mapBlockIndex before/afterDeep/after=%d/%d/%d "
            "fullResident before/afterDeep/after=%zu/%zu/%zu (bound=%d)\n",
            mbBefore, mbAfterDeep, mbAfter, frBefore, frAfterDeep, frAfter, horizon + 1);

    iso.restore();
    pindexBest = savedBest; pindexGenesisBlock = savedGenesis;
    hashBestChain = savedBestChain; nBestHeight = savedBestHeight; nBestChainTrust = savedBestTrust;
    if (hadLivetail) mapArgs["-blockindexlivetail"] = savedLivetail; else mapArgs.erase("-blockindexlivetail");
    try { fs::remove_all(root); } catch (...) {}

    // No historical repopulation / no leak.
    BOOST_CHECK_LE(mbAfterDeep, mbBefore + 4);
    BOOST_CHECK_LE(mbAfter, mbBefore + 4);
    BOOST_CHECK_LE((int)frAfter, horizon + 16);
}

// PM1-P0-06 A1-c G1 (C4/H5) — authoritative operator-invalid ancestry STRICTLY
// below the live residency boundary, with a deterministic oracle and fail-closed.
BOOST_AUTO_TEST_CASE(p06_a1c_g1_below_floor_invalidation)
{
    const int horizon = 32;
    const int target = 4 * 64 + 2 * horizon; // 320

    CBlockIndex* tip = pindexBest;
    while (tip->nHeight < 40) tip = P06MineReal(tip, 0x4000u + (unsigned)tip->nHeight);

    const fs::path root = fs::temp_directory_path() / fs::unique_path("p06-g1-%%%%-%%%%");
    std::string error;
    P06BuildRoot(root, &error);
    CBlockIndex* savedBest = pindexBest; CBlockIndex* savedGenesis = pindexGenesisBlock;
    uint256 savedBestChain = hashBestChain; int savedBestHeight = nBestHeight;
    uint256 savedBestTrust = nBestChainTrust;
    const bool hadLivetail = mapArgs.count("-blockindexlivetail") != 0;
    const std::string savedLivetail = hadLivetail ? mapArgs["-blockindexlivetail"] : std::string();

    // Isolate the shared block index for the authoritative phase (see
    // P06SharedIndexIsolation): the production retirer must not free the shared
    // regtest chain that later cases depend on.
    P06SharedIndexIsolation iso;
    iso.detach();

    mapArgs["-blockindexlivetail"] = "32";
    BOOST_REQUIRE_MESSAGE(InitBlockIndexAuthoritative(root.string(), &error), error);
    BOOST_REQUIRE(g_fAuthoritativeStartup);

    tip = pindexBest;
    // Height-indexed capture, GROWN ON DEMAND (see a1c_safety): the shared
    // datadir accumulates real blocks across cases and mining may adopt a longer
    // candidate chain, so the reached height is not bounded by start + target.
    std::vector<uint256> hashes((size_t)tip->nHeight + 1, uint256(0));
    for (int i = 0; i < target; ++i)
    {
        tip = P06MineReal(tip, 0x4000u + (unsigned)i);
        BOOST_REQUIRE(tip->nHeight >= 0);
        if ((size_t)tip->nHeight >= hashes.size())
            hashes.resize((size_t)tip->nHeight + 1, uint256(0));
        hashes[tip->nHeight] = tip->GetBlockHash();
    }

    int tipH = 0, lowestRetained = -1, mbBefore = 0;
    {
        LOCK(cs_main);
        tipH = nBestHeight;
        for (std::map<uint256, CBlockIndex*>::iterator it = mapBlockIndex.begin(); it != mapBlockIndex.end(); ++it)
            if (lowestRetained < 0 || it->second->nHeight < lowestRetained) lowestRetained = it->second->nHeight;
        mbBefore = (int)mapBlockIndex.size();
    }
    BlockIndexAuthoritativeLive* liveAuth = GetAuthoritativeLiveAuthority();
    const size_t frBefore2 = liveAuth ? liveAuth->ResidentCount() : 0;
    const int hA = lowestRetained - 1;            // first NON-resident active height
    const int hB = lowestRetained - 3 * horizon;  // materially below the boundary
    fprintf(stderr, "P06-A1C G1: tipH=%d lowestRetained=%d hA=%d hB=%d mapBlockIndex=%d fullResident=%zu\n",
            tipH, lowestRetained, hA, hB, mbBefore, frBefore2);
    BOOST_REQUIRE(hB > 0 && hB < hA && hA < lowestRetained);

    // PRECONDITION: both tested ancestors are NOT resident.
    {
        LOCK(cs_main);
        BOOST_CHECK_EQUAL(mapBlockIndex.count(hashes[hA]), (size_t)0);
        BOOST_CHECK_EQUAL(mapBlockIndex.count(hashes[hB]), (size_t)0);
        BOOST_CHECK_EQUAL(mapBlockIndex.count(hashes[lowestRetained]), (size_t)1);
    }

    // ORACLE (independent, chain-structural): the active tip descends from every
    // active block below it, so an operator-invalid active ancestor at hA/hB MUST
    // make the tip INVALID; an invalid block ABOVE the candidate must not.
    // The authoritative operator-invalid authority is the tip authority's set
    // (SetOperatorInvalid), NOT the legacy global setInvalidBlockHash.
    BlockIndexTipAuthority* tipAuth = liveAuth ? liveAuth->TipAuthorityMutable() : NULL;
    BOOST_REQUIRE(tipAuth != NULL);
    std::string ierr;

    // CASE A — ancestor at the first non-resident height; then authoritative reconsider.
    BOOST_REQUIRE_EQUAL(tipAuth->SetOperatorInvalid(hashes[hA], true, &ierr), BLOCK_INDEX_TIP_OK);
    {
        LOCK(cs_main);
        BOOST_CHECK_EQUAL((int)IsBlockOperatorInvalidTyped(pindexBest), (int)BLOCK_INDEX_OPERATOR_INVALID);
    }
    BOOST_REQUIRE_EQUAL(tipAuth->SetOperatorInvalid(hashes[hA], false, &ierr), BLOCK_INDEX_TIP_OK); // reconsider
    {
        LOCK(cs_main);
        BOOST_CHECK_EQUAL((int)IsBlockOperatorInvalidTyped(pindexBest), (int)BLOCK_INDEX_OPERATOR_VALID);
    }
    // CASE B — ancestor materially below the boundary (3 horizons).
    BOOST_REQUIRE_EQUAL(tipAuth->SetOperatorInvalid(hashes[hB], true, &ierr), BLOCK_INDEX_TIP_OK);
    {
        LOCK(cs_main);
        BOOST_CHECK_EQUAL((int)IsBlockOperatorInvalidTyped(pindexBest), (int)BLOCK_INDEX_OPERATOR_INVALID);
    }
    BOOST_REQUIRE_EQUAL(tipAuth->SetOperatorInvalid(hashes[hB], false, &ierr), BLOCK_INDEX_TIP_OK);
    // CASE C — invalid block ABOVE the candidate must NOT poison it.
    {
        CBlockIndex* cand = NULL;
        { LOCK(cs_main);
          for (std::map<uint256, CBlockIndex*>::iterator it = mapBlockIndex.begin(); it != mapBlockIndex.end(); ++it)
              if (it->second->nHeight == lowestRetained + 1) { cand = it->second; break; } }
        BOOST_REQUIRE(cand != NULL);
        BOOST_REQUIRE_EQUAL(tipAuth->SetOperatorInvalid(hashes[tipH], true, &ierr), BLOCK_INDEX_TIP_OK);
        { LOCK(cs_main);
          BOOST_CHECK_EQUAL((int)IsBlockOperatorInvalidTyped(cand), (int)BLOCK_INDEX_OPERATOR_VALID); }
        BOOST_REQUIRE_EQUAL(tipAuth->SetOperatorInvalid(hashes[tipH], false, &ierr), BLOCK_INDEX_TIP_OK);
    }
    // FAIL-CLOSED — an operator-invalid id the authority cannot resolve must NOT
    // become VALID. If the authority refuses the intent outright that is itself the
    // fail-closed behaviour; otherwise the typed query must return UNAVAILABLE.
    {
        const uint256 hBad(0xBADBADBADUL);
        const BlockIndexTipStatus st = tipAuth->SetOperatorInvalid(hBad, true, &ierr);
        if (st == BLOCK_INDEX_TIP_OK)
        {
            LOCK(cs_main);
            BOOST_CHECK_EQUAL((int)IsBlockOperatorInvalidTyped(pindexBest), (int)BLOCK_INDEX_OPERATOR_UNAVAILABLE);
            tipAuth->SetOperatorInvalid(hBad, false, &ierr);
        }
        else
        {
            fprintf(stderr, "P06-A1C G1 FAIL-CLOSED: authority refused unresolvable invalid id (status=%d) -> fail closed at setter\n", (int)st);
            LOCK(cs_main);
            BOOST_CHECK_EQUAL((int)IsBlockOperatorInvalidTyped(pindexBest), (int)BLOCK_INDEX_OPERATOR_VALID);
        }
    }

    int mbAfter = 0; { LOCK(cs_main); mbAfter = (int)mapBlockIndex.size(); }
    const size_t frAfter = liveAuth ? liveAuth->ResidentCount() : 0;
    fprintf(stderr, "P06-A1C G1 RESULT: mapBlockIndex before/after=%d/%d fullResident before/after=%zu/%zu\n",
            mbBefore, mbAfter, frBefore2, frAfter);

    iso.restore();
    pindexBest = savedBest; pindexGenesisBlock = savedGenesis;
    hashBestChain = savedBestChain; nBestHeight = savedBestHeight; nBestChainTrust = savedBestTrust;
    if (hadLivetail) mapArgs["-blockindexlivetail"] = savedLivetail; else mapArgs.erase("-blockindexlivetail");
    try { fs::remove_all(root); } catch (...) {}

    BOOST_CHECK_LE(mbAfter, mbBefore + 4);   // no repopulation
    BOOST_CHECK_LE((int)frAfter, horizon + 16); // no leak
}

// PM1-P0-06 Phase-D L3 — DETERMINISTIC below-floor accept (regression for the
// real-chain SIGSEGV). A VALID block whose parent is far below the retained floor
// must be accepted AND durably persisted by value, with NO use-after-free. Before
// the L3 repair, RetireBlockIndexBelowFloor() freed the just-inserted index and
// the legacy mirror write at main.cpp:7758 dereferenced it (GPF / SIGSEGV).
BOOST_AUTO_TEST_CASE(p06_l3_below_floor_accept_no_uaf)
{
    const int horizon = 32;
    CBlockIndex* tip = pindexBest;
    while (tip->nHeight < 40)
        tip = P06MineReal(tip, 0x6000u + (unsigned)tip->nHeight);

    // Capture a very old block hash (height 5) BEFORE authoritative startup.
    uint256 oldHash(0);
    { LOCK(cs_main);
      for (std::map<uint256, CBlockIndex*>::iterator it = mapBlockIndex.begin(); it != mapBlockIndex.end(); ++it)
          if (it->second->nHeight == 5) { oldHash = it->first; break; } }
    BOOST_REQUIRE(oldHash != uint256(0));

    const fs::path root = fs::temp_directory_path() / fs::unique_path("p06-l3-%%%%-%%%%");
    std::string error;
    P06BuildRoot(root, &error);

    CBlockIndex* savedBest = pindexBest; CBlockIndex* savedGenesis = pindexGenesisBlock;
    uint256 savedBestChain = hashBestChain; int savedBestHeight = nBestHeight;
    uint256 savedBestTrust = nBestChainTrust;
    const bool hadLivetail = mapArgs.count("-blockindexlivetail") != 0;
    const std::string savedLivetail = hadLivetail ? mapArgs["-blockindexlivetail"] : std::string();

    // Isolate the shared block index for the authoritative phase (see
    // P06SharedIndexIsolation): the production retirer must not free the shared
    // regtest chain that later cases depend on.
    P06SharedIndexIsolation iso;
    iso.detach();

    mapArgs["-blockindexlivetail"] = "32";
    BOOST_REQUIRE_MESSAGE(InitBlockIndexAuthoritative(root.string(), &error), error);
    BOOST_REQUIRE(g_fAuthoritativeStartup);
    BlockIndexAuthoritativeLive* live = GetAuthoritativeLiveAuthority();
    BOOST_REQUIRE(live != NULL);

    // Drive the active chain several full horizons past the old block.
    tip = pindexBest;
    for (int i = 0; i < 4 * 64 + 2 * horizon; ++i)
        tip = P06MineReal(tip, 0xA000u + (unsigned)i);

    int tipH = 0, floor = 0;
    { LOCK(cs_main); tipH = tip->nHeight; floor = (nBestHeight - horizon + 1 > 0) ? (nBestHeight - horizon + 1) : 0; }
    BOOST_REQUIRE(floor > 6); // a height-5 parent (child height 6) is genuinely below the floor

    // Resolve the OLD parent BY VALUE — it is no longer resident in mapBlockIndex.
    std::string rerr;
    CBlockIndex* oldParent = live->ResolveAndRetainFullParent(oldHash, &rerr);
    BOOST_REQUIRE_MESSAGE(oldParent != NULL, "below-floor parent resolve failed: " << rerr);
    BOOST_REQUIRE_EQUAL(oldParent->nHeight, 5);
    BOOST_REQUIRE(oldParent->nHeight < floor);

    // Build a VALID child of the below-floor parent and drive the REAL accept path
    // (CheckBlock -> ProcessBlock -> AcceptBlock -> AddToBlockIndex).
    CBlock* pblock = P06BuildPoWBlock(oldParent, 0xBEEF00u);
    BOOST_REQUIRE(pblock != NULL);
    const uint256 childHash = pblock->GetHash();
    bool fChecked = false, fProcessed = false;
    {
        LOCK(cs_main);
        fChecked = pblock->CheckBlock(true, true, true);
        fProcessed = fChecked && ProcessBlock(NULL, pblock);
    }
    delete pblock;

    BOOST_CHECK_MESSAGE(fChecked, "below-floor child failed CheckBlock");
    BOOST_CHECK_MESSAGE(fProcessed, "below-floor child not accepted through ProcessBlock (pre-repair: UAF)");

    if (fProcessed)
    {
        // (a) Durable BY-VALUE persistence in the mutable tip authority (AcceptSide/AcceptActive).
        BlockIndexSnapshot snap; std::string serr;
        const BlockIndexHotStatus st = live->ResolveBlockSnapshot(childHash, &snap, &serr);
        BOOST_CHECK_MESSAGE(st == BlockIndexHotStatus::OK,
                            "below-floor child was not durably persisted: " << serr);
        if (st == BlockIndexHotStatus::OK)
            BOOST_CHECK_EQUAL(snap.height, 6);
    }

    // (b) Residency stays bounded (no repopulation / no leak).
    int mb = 0; { LOCK(cs_main); mb = (int)mapBlockIndex.size(); }
    fprintf(stderr, "P06-L3 BELOW-FLOOR ACCEPT: tipH=%d floor=%d accepted=%d mapBlockIndex=%d fullResident=%zu\n",
            tipH, floor, (int)fProcessed, mb, live->ResidentCount());
    BOOST_CHECK_LE(mb, horizon + 8);
    BOOST_CHECK_LE((int)live->ResidentCount(), horizon + 16);

    iso.restore();
    pindexBest = savedBest; pindexGenesisBlock = savedGenesis;
    hashBestChain = savedBestChain; nBestHeight = savedBestHeight; nBestChainTrust = savedBestTrust;
    if (hadLivetail) mapArgs["-blockindexlivetail"] = savedLivetail; else mapArgs.erase("-blockindexlivetail");
    try { fs::remove_all(root); } catch (...) {}
}

// PM1-P0-06 Phase-D DR — KNOWN BLOCK != RESIDENT CBlockIndex.
// A block whose identity is durable in the V2 authority but which has NO resident
// CBlockIndex (below the residency floor) must NOT be re-accepted, must NOT be
// re-written to blk*.dat, and must NOT append another tip record — while a
// genuinely NEW block still proceeds through normal acceptance + durable append.
// This reproduces the real L5 write-amplification defect deterministically.
BOOST_AUTO_TEST_CASE(p06_dr_known_block_not_rewritten)
{
    const int horizon = 32;

    // total bytes of blk*.dat in the test datadir
    struct BlkSum {
        static int64_t Sum() {
            int64_t total = 0;
            for (fs::directory_iterator it(GetDataDir()), end; it != end; ++it) {
                const std::string n = it->path().filename().string();
                if (n.size() > 4 && n.compare(0, 3, "blk") == 0 &&
                    n.compare(n.size() - 4, 4, ".dat") == 0)
                    total += (int64_t)fs::file_size(it->path());
            }
            return total;
        }
    };

    CBlockIndex* tip = pindexBest;
    while (tip->nHeight < 40)
        tip = P06MineReal(tip, 0x7000u + (unsigned)tip->nHeight);

    uint256 oldHash(0);
    { LOCK(cs_main);
      for (std::map<uint256, CBlockIndex*>::iterator it = mapBlockIndex.begin(); it != mapBlockIndex.end(); ++it)
          if (it->second->nHeight == 5) { oldHash = it->first; break; } }
    BOOST_REQUIRE(oldHash != uint256(0));

    const fs::path root = fs::temp_directory_path() / fs::unique_path("p06-dr-%%%%-%%%%");
    std::string error;
    P06BuildRoot(root, &error);
    const fs::path tipRecords = root / "blockindex_tip" / "tip-records.dat";

    CBlockIndex* savedBest = pindexBest; CBlockIndex* savedGenesis = pindexGenesisBlock;
    uint256 savedBestChain = hashBestChain; int savedBestHeight = nBestHeight;
    uint256 savedBestTrust = nBestChainTrust;
    const bool hadLivetail = mapArgs.count("-blockindexlivetail") != 0;
    const std::string savedLivetail = hadLivetail ? mapArgs["-blockindexlivetail"] : std::string();

    // Isolate the shared block index for the authoritative phase (see
    // P06SharedIndexIsolation): the production retirer must not free the shared
    // regtest chain that later cases depend on.
    P06SharedIndexIsolation iso;
    iso.detach();

    mapArgs["-blockindexlivetail"] = "32";
    BOOST_REQUIRE_MESSAGE(InitBlockIndexAuthoritative(root.string(), &error), error);
    BOOST_REQUIRE(g_fAuthoritativeStartup);
    BlockIndexAuthoritativeLive* live = GetAuthoritativeLiveAuthority();
    BOOST_REQUIRE(live != NULL);

    // Drive the chain several horizons past the old block so height-5/-6 retire.
    tip = pindexBest;
    for (int i = 0; i < 4 * 64 + 2 * horizon; ++i)
        tip = P06MineReal(tip, 0xB000u + (unsigned)i);

    std::string rerr;
    CBlockIndex* oldParent = live->ResolveAndRetainFullParent(oldHash, &rerr);
    BOOST_REQUIRE_MESSAGE(oldParent != NULL, "below-floor parent resolve failed: " << rerr);

    CBlock* pblock = P06BuildPoWBlock(oldParent, 0xD00Du);
    BOOST_REQUIRE(pblock != NULL);
    const uint256 childHash = pblock->GetHash();

    // --- 1st presentation: genuinely NEW -> accepted, written to blk, appended to tip
    const int64_t blkBefore = BlkSum::Sum();
    const int64_t tipBefore = (int64_t)fs::file_size(tipRecords);
    bool fFirst = false;
    { LOCK(cs_main); fFirst = pblock->CheckBlock(true, true, true) && ProcessBlock(NULL, pblock); }
    const int64_t blkAfterFirst = BlkSum::Sum();
    const int64_t tipAfterFirst = (int64_t)fs::file_size(tipRecords);
    BOOST_CHECK_MESSAGE(fFirst, "control: genuinely new below-floor child was not accepted");
    BOOST_CHECK_MESSAGE(blkAfterFirst > blkBefore, "control: new block was not written to blk*.dat");
    BOOST_CHECK_MESSAGE(tipAfterFirst > tipBefore, "control: new block was not appended to blockindex_tip");

    // It is now authoritative-KNOWN and NON-RESIDENT.
    BlockIndexSnapshot snap; std::string serr;
    BOOST_CHECK(live->ResolveBlockSnapshot(childHash, &snap, &serr) == BlockIndexHotStatus::OK);
    { LOCK(cs_main); BOOST_CHECK_EQUAL(mapBlockIndex.count(childHash), 0); }

    // --- 2nd presentation of the SAME block (known, non-resident) through the REAL path
    bool fSecond = false;
    { LOCK(cs_main); fSecond = ProcessBlock(NULL, pblock); }
    const int64_t blkAfterSecond = BlkSum::Sum();
    const int64_t tipAfterSecond = (int64_t)fs::file_size(tipRecords);
    int mb = 0; { LOCK(cs_main); mb = (int)mapBlockIndex.size(); }
    fprintf(stderr, "P06-DR DUP: blkBefore=%lld blkAfter1=%lld blkAfter2=%lld | tipBefore=%lld tipAfter1=%lld tipAfter2=%lld | mapBlockIndex=%d accepted2nd=%d\n",
            (long long)blkBefore, (long long)blkAfterFirst, (long long)blkAfterSecond,
            (long long)tipBefore, (long long)tipAfterFirst, (long long)tipAfterSecond,
            mb, (int)fSecond);
    BOOST_CHECK_MESSAGE(!fSecond, "known non-resident block was re-accepted through ProcessBlock");
    BOOST_CHECK_EQUAL(blkAfterSecond, blkAfterFirst);  // NO blk*.dat growth from the duplicate
    BOOST_CHECK_EQUAL(tipAfterSecond, tipAfterFirst);   // NO extra tip record
    BOOST_CHECK_LE(mb, horizon + 8);                    // residency stays bounded

    // --- negative control: a genuinely NEW block still proceeds + appends
    CBlock* pnew = P06BuildPoWBlock(tip, 0xC0FFEEu);
    BOOST_REQUIRE(pnew != NULL);
    bool fNew = false;
    { LOCK(cs_main); fNew = pnew->CheckBlock(true, true, true) && ProcessBlock(NULL, pnew); }
    const int64_t tipAfterNew = (int64_t)fs::file_size(tipRecords);
    BOOST_CHECK_MESSAGE(fNew, "control: a genuinely new tip block was not accepted");
    BOOST_CHECK_MESSAGE(tipAfterNew > tipAfterSecond, "control: genuinely new block was not appended to blockindex_tip");
    delete pnew;

    delete pblock;

    iso.restore();
    pindexBest = savedBest; pindexGenesisBlock = savedGenesis;
    hashBestChain = savedBestChain; nBestHeight = savedBestHeight; nBestChainTrust = savedBestTrust;
    if (hadLivetail) mapArgs["-blockindexlivetail"] = savedLivetail; else mapArgs.erase("-blockindexlivetail");
    try { fs::remove_all(root); } catch (...) {}
}

BOOST_AUTO_TEST_SUITE_END()
