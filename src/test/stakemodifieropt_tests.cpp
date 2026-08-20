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

BOOST_AUTO_TEST_SUITE_END()
