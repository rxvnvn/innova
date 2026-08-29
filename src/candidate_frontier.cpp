// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "candidate_frontier.h"
#include "main.h"
#include "txdb.h"
#include "finality.h"
#include "dag.h"

#include <set>
#include <map>
#include <string>

extern std::map<uint256, CBlockIndex*> mapBlockIndex;
extern CBlockIndex* pindexBest;
extern uint256 nBestChainTrust;
extern std::set<uint256> setInvalidBlockHash;
extern std::map<uint256, CandidateTipRecord> mapCandidateTips;
extern uint64_t nCandidateTipGeneration;
extern bool fCandidateFrontierShadowActive;
extern CFinalityTracker g_finalityTracker;

bool IsBlockOperatorInvalid(const CBlockIndex* pindex);

// ---------------------------------------------------------------------------
// Rebuild candidate tips from full mapBlockIndex scan
// ---------------------------------------------------------------------------

bool RebuildCandidateTips()
{
    AssertLockHeld(cs_main);
    mapCandidateTips.clear();

    // Build setReferenced: blocks that are SOMEONE's parent
    std::set<uint256> setReferenced;
    for (const auto& item : mapBlockIndex)
        if (item.second->pprev != NULL)
            setReferenced.insert(*item.second->pprev->phashBlock);

    // Every block NOT in setReferenced is a tip
    for (const auto& item : mapBlockIndex)
    {
        CBlockIndex* pindex = item.second;
        if (setReferenced.count(*pindex->phashBlock))
            continue;

        // Compute fork point relative to best chain
        uint256 hashFork = 0;
        int nForkHt = -1;
        if (pindexBest)
        {
            CBlockIndex* pFork = pindex;
            CBlockIndex* pOther = pindexBest;
            while (pFork != pOther)
            {
                while (pFork != NULL && pFork->nHeight > pOther->nHeight)
                    pFork = pFork->pprev;
                if (pFork == pOther)
                    break;
                if (pOther != NULL)
                    pOther = pOther->pprev;
            }
            if (pFork)
            {
                hashFork = pFork->GetBlockHash();
                nForkHt = pFork->nHeight;
            }
        }

        bool fValid = !IsBlockOperatorInvalid(pindex);
        bool fHasData = false;
        {
            CBlock b;
            fHasData = b.ReadFromDisk(pindex);
        }

        mapCandidateTips[*pindex->phashBlock] = CandidateTipRecord(
            *pindex->phashBlock,
            pindex->nChainTrust,
            hashFork, nForkHt,
            fHasData ? 1 : 0,
            fValid ? 1 : 0,
            nCandidateTipGeneration);
    }

    nCandidateTipGeneration++;
    printf("RebuildCandidateTips: rebuilt %d tips (gen %llu)\n",
           (int)mapCandidateTips.size(),
           (unsigned long long)nCandidateTipGeneration);
    return true;
}


// ---------------------------------------------------------------------------
// Evaluate the bounded candidate frontier
// ---------------------------------------------------------------------------

CBlockIndex* EvaluateCandidateFrontier()
{
    AssertLockHeld(cs_main);

    if (mapCandidateTips.empty())
        return pindexBest;

    const int nFinalHeight = g_finalityTracker.GetFinalizedHeight();
    bool fFinalityActive = (nFinalHeight > 0 && pindexBest &&
                            pindexBest->nHeight >= FORK_HEIGHT_FINALITY);
    CBlockIndex* pindexCandidate = NULL;

    for (const auto& entry : mapCandidateTips)
    {
        std::map<uint256, CBlockIndex*>::iterator mi =
            mapBlockIndex.find(entry.first);
        if (mi == mapBlockIndex.end())
            continue;

        CBlockIndex* pindex = mi->second;

        // Re-check conditions at evaluation time (not just persisted flags)
        if (pindex->nChainTrust <= nBestChainTrust)
            continue;
        if (IsBlockOperatorInvalid(pindex))
            continue;

        // Data availability check
        if (!entry.second.fHasData)
        {
            CBlock b;
            if (!b.ReadFromDisk(pindex))
                continue;
        }

        // Finality fork check
        if (fFinalityActive)
        {
            CBlockIndex* pFork = pindex;
            CBlockIndex* pOther = pindexBest;
            while (pFork != pOther)
            {
                while (pFork != NULL && pFork->nHeight > pOther->nHeight)
                    pFork = pFork->pprev;
                if (pFork == pOther)
                    break;
                if (pOther != NULL)
                    pOther = pOther->pprev;
            }
            if (pFork == NULL || pFork->nHeight < nFinalHeight)
                continue;
        }

        // Strict > comparison (same as legacy)
        if (pindexCandidate == NULL ||
            pindex->nChainTrust > pindexCandidate->nChainTrust)
            pindexCandidate = pindex;
    }

    return pindexCandidate;
}


// ---------------------------------------------------------------------------
// Shadow comparator: runs bounded frontier alongside legacy full scan
// ---------------------------------------------------------------------------

void ShadowCompareCandidateSelection()
{
    if (!fCandidateFrontierShadowActive)
        return;

    LOCK(cs_main);

    // --- Legacy full scan (inline) ---
    CBlockIndex* pindexLegacy = NULL;
    {
        std::set<uint256> setReferenced;
        for (const auto& item : mapBlockIndex)
            if (item.second->pprev != NULL)
                setReferenced.insert(*item.second->pprev->phashBlock);

        const int nFinalHeight = g_finalityTracker.GetFinalizedHeight();
        bool fFinActive = (nFinalHeight > 0 && pindexBest &&
                           pindexBest->nHeight >= FORK_HEIGHT_FINALITY);

        for (const auto& item : mapBlockIndex)
        {
            CBlockIndex* pindex = item.second;
            if (setReferenced.count(*pindex->phashBlock))
                continue;
            if (IsBlockOperatorInvalid(pindex))
                continue;
            if (pindex->nChainTrust <= nBestChainTrust)
                continue;
            CBlock block;
            if (!block.ReadFromDisk(pindex))
                continue;
            if (fFinActive)
            {
                CBlockIndex* pFork = pindex;
                CBlockIndex* pOther = pindexBest;
                while (pFork != pOther)
                {
                    while (pFork != NULL && pFork->nHeight > pOther->nHeight)
                        pFork = pFork->pprev;
                    if (pFork == pOther)
                        break;
                    if (pOther != NULL)
                        pOther = pOther->pprev;
                }
                if (pFork == NULL || pFork->nHeight < nFinalHeight)
                    continue;
            }
            if (pindexLegacy == NULL ||
                pindex->nChainTrust > pindexLegacy->nChainTrust)
                pindexLegacy = pindex;
        }
    }

    // --- Bounded frontier ---
    CBlockIndex* pindexFrontier = EvaluateCandidateFrontier();

    // --- Compare and report ---
    if (pindexLegacy != pindexFrontier)
    {
        std::string legacyHash = pindexLegacy
            ? pindexLegacy->GetBlockHash().ToString().substr(0, 20)
            : "(none)";
        std::string frontierHash = pindexFrontier
            ? pindexFrontier->GetBlockHash().ToString().substr(0, 20)
            : "(none)";
        printf("CANDIDATE_SHADOW: MISMATCH legacy=%s (h=%d trust=%s) "
               "frontier=%s (h=%d trust=%s)\n",
               legacyHash.c_str(),
               pindexLegacy ? pindexLegacy->nHeight : -1,
               pindexLegacy ? CBigNum(pindexLegacy->nChainTrust).ToString().c_str() : "0",
               frontierHash.c_str(),
               pindexFrontier ? pindexFrontier->nHeight : -1,
               pindexFrontier ? CBigNum(pindexFrontier->nChainTrust).ToString().c_str() : "0");
    }
}