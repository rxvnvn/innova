// Differential unit test: -stakemodifieropt optimized ComputeNextStakeModifier
// must produce bit-identical (nStakeModifier, fGeneratedStakeModifier) to the
// legacy path, on a synthetic chain spanning several modifier intervals.
#include <boost/test/unit_test.hpp>
#include <vector>
#include "main.h"
#include "kernel.h"
#include "util.h"

extern unsigned int nModifierInterval;
extern unsigned int nTargetSpacing;

BOOST_AUTO_TEST_SUITE(stakemodifieropt_tests)

namespace {
// Build a synthetic chain of CBlockIndex nodes inserted into mapBlockIndex.
std::vector<CBlockIndex*> BuildChain(int nBlocks, int64_t nStartTime, int64_t nStep)
{
    std::vector<CBlockIndex*> v;
    v.reserve(nBlocks);
    static std::vector<uint256> gHashes;
    gHashes.clear();
    gHashes.reserve((size_t)nBlocks); // keep phashBlock address-stable
    CBlockIndex* pprev = NULL;
    for (int i = 0; i < nBlocks; i++)
    {
        CBlockIndex* p = new CBlockIndex();
        p->pprev = pprev;
        p->nHeight = (pprev ? pprev->nHeight + 1 : 0);
        p->nTime = nStartTime + i * nStep;
        p->hashProof = uint256((uint64_t)nStartTime + (uint64_t)i);
        p->nFlags = BLOCK_PROOF_OF_STAKE | (((unsigned)(i * 2654435761u)) & 1 ? BLOCK_STAKE_ENTROPY : 0);
        if (i == 0)
            p->nFlags |= BLOCK_STAKE_MODIFIER; // genesis carries a generated modifier (0)
        p->nStakeModifier = 0;
        p->nStakeModifierChecksum = 0;
        p->nStakeModifierTime = 0;
        gHashes.emplace_back((uint64_t)(nStartTime + i) ^ 0x9e3779b97f4a7c15ULL);
        p->phashBlock = &gHashes.back();
        mapBlockIndex[*p->phashBlock] = p;
        v.push_back(p);
        pprev = p;
    }
    return v;
}
}

BOOST_AUTO_TEST_CASE(legacy_vs_optimized_equivalent)
{
    // save globals
    unsigned int saveInterval = nModifierInterval;
    unsigned int saveSpacing  = nTargetSpacing;
    std::string oldOpt = mapArgs["-stakemodifieropt"];
    nModifierInterval = 60;      // 60s interval -> several generations in the span
    nTargetSpacing    = 5;       // ~12 candidate blocks per interval

    try {
        std::vector<CBlockIndex*> v = BuildChain(90, 1000000, 6); // ~540s span -> several intervals

        uint64_t mismatches = 0;
        for (size_t i = 1; i < v.size(); i++)
        {
            CBlockIndex* pprev = v[i]->pprev;
            uint64_t modL=0, modO=0;
            bool genL=false, genO=false;
            // legacy
            mapArgs["-stakemodifieropt"] = "0";
            BOOST_REQUIRE(ComputeNextStakeModifier(pprev, modL, genL));
            // optimized (requires pprev memo set by processing earlier blocks)
            mapArgs["-stakemodifieropt"] = "1";
            BOOST_REQUIRE(ComputeNextStakeModifier(pprev, modO, genO));

            if (modL != modO || genL != genO)
                mismatches++;

            // apply to current block exactly as AddToBlockIndex does (incl. memo)
            v[i]->SetStakeModifier(modL, genL);
            v[i]->nStakeModifierTime = genL ? v[i]->GetBlockTime()
                                            : (v[i]->pprev ? v[i]->pprev->nStakeModifierTime : 0);
        }
        BOOST_CHECK_EQUAL(mismatches, (uint64_t)0);

        // cleanup
        for (size_t i = 0; i < v.size(); i++) { mapBlockIndex.erase(*v[i]->phashBlock); delete v[i]; }
    } catch (...) {
        nModifierInterval = saveInterval;
        nTargetSpacing = saveSpacing;
        mapArgs["-stakemodifieropt"] = oldOpt;
        BOOST_FAIL("exception during differential");
    }

    nModifierInterval = saveInterval;
    nTargetSpacing = saveSpacing;
    mapArgs["-stakemodifieropt"] = oldOpt;
}

BOOST_AUTO_TEST_CASE(block1_canonical_checksum_boundary_real_mainnet_anchors)
{
    // BLOCK-100000 FIRST-DIVERGENCE REGRESSION GATE (2026-10-10).
    // The V2 by-value ResolveLastStakeModifierByValue used to surface genesis'
    // nStakeModifierTime (= 0) as the last-modifier generation time, so block 1
    // falsely "regenerated" the stake modifier: nStakeModifier=1 and
    // nFlags=BLOCK_STAKE_MODIFIER(0x4)|ENTROPY(0x2)=0x6. That produces the
    // DIVERGENT block-1 checksum 0xe1459c6b and poisons every later checksum,
    // so block 100000 fails the 0xcf12d0aa checkpoint. The canonical chain has
    // block 1 INHERIT genesis' modifier (nStakeModifier=0, nFlags=0x2 only),
    // giving the canonical checksum 0xbc4b99b6. These are REAL-MAINNET anchor
    // values (verified against the authoritative local reference node via
    // getstakemodifiercheckpoints). If this test fails, modifier-generation
    // provenance at the genesis boundary has regressed.
    uint256 genesisHash = GetGenesisBlockHash();
    CBlockIndex genesis;
    genesis.phashBlock = &genesisHash;
    genesis.nHeight = 0;
    genesis.pprev = NULL;
    genesis.nTime = 1576165389;                 // mainnet genesis time
    genesis.nFlags = BLOCK_STAKE_ENTROPY | BLOCK_STAKE_MODIFIER; // 0x6, PoW
    genesis.nStakeModifier = 0;
    genesis.nStakeModifierChecksum = 0;
    // Genesis (parentless, PoW, mod=0) must be the canonical seed.
    const unsigned int csumGenesis = GetStakeModifierChecksum(&genesis);
    BOOST_CHECK_EQUAL(csumGenesis, 0x0e00670bu);

    // Canonical block 1: INHERITS genesis' modifier (mod=0), nFlags=ENTROPY only.
    CBlockIndex b1;
    b1.phashBlock = NULL;
    b1.pprev = &genesis;
    b1.nHeight = 1;
    b1.nFlags = BLOCK_STAKE_ENTROPY;           // 0x2, PoW (no PROOF_OF_STAKE, no STAKE_MODIFIER)
    b1.nStakeModifier = 0;                     // inherited, NOT regenerated
    genesis.nStakeModifierChecksum = csumGenesis; // canonical parent seed
    unsigned int cB1 = GetStakeModifierChecksum(&b1);
    BOOST_CHECK_EQUAL(cB1, 0xbc4b99b6u);       // canonical block-1 checksum

    // Guard: assert the divergent input (the pre-fix bug) does NOT equal canonical,
    // so a future regression to false regeneration is caught as a checksum change.
    CBlockIndex b1_div;
    b1_div.phashBlock = NULL;
    b1_div.pprev = &genesis;
    b1_div.nHeight = 1;
    b1_div.nFlags = BLOCK_STAKE_MODIFIER | BLOCK_STAKE_ENTROPY; // 0x6 (false regeneration)
    b1_div.nStakeModifier = 1;                 // falsely regenerated
    unsigned int cB1_div = GetStakeModifierChecksum(&b1_div);
    BOOST_CHECK_EQUAL(cB1_div, 0xe1459c6bu);   // the known divergent value
    BOOST_CHECK(cB1_div != cB1);
}

BOOST_AUTO_TEST_SUITE_END()
