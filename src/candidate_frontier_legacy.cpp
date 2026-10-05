// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// PM1-P0-07b (C3) - LEGACY_RESIDENT candidate-frontier compatibility.
//
// This translation unit holds the ONLY remaining candidate-frontier code that
// touches the historical global mapBlockIndex / CBlockIndex pprev graph:
//   - RebuildCandidateTips()          (legacy full-scan tip rebuild)
//   - EvaluateCandidateFrontier()     (legacy pointer-returning shim)
//   - UpdateCandidateTips()           (legacy incremental stub)
//   - ShadowCompareCandidateSelection (legacy-vs-by-value diagnostic)
//
// It exists ONLY for the LEGACY_RESIDENT startup mode, which C4 retires as a
// normal production mode. The by-value authority core (candidate_frontier.cpp)
// does NOT reference this file's graph, and the authoritative V2 startup path
// builds the frontier from candidate-leaves.dat via
// blockindex_candidate_startup_builder.cpp. This file is scheduled for
// retirement at C4 (NORMAL LEGACY_RESIDENT MODE = REMOVED).

#include "candidate_frontier.h"
#include "blockindex_residency_counters.h"
#include "main.h"
#include "txdb.h"

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

bool IsBlockOperatorInvalid(const CBlockIndex* pindex);

// Legacy shadow adapter. Internally reads the resident graph to construct
// by-value records (allowed for the transition/shadow adapter), but the
// evaluator (EvaluateCandidateFrontierByValue) never sees a CBlockIndex*.
class LegacyCandidateFrontierStore : public CandidateFrontierStore
{
public:
    bool IsBestActive() const { return pindexBest != NULL; }
    uint256 GetBestTrust() const { return nBestChainTrust; }
    uint256 GetBestTip() const { return pindexBest ? *pindexBest->phashBlock : uint256(0); }
    bool IsOperatorHash(const uint256& h) const { return setInvalidBlockHash.count(h) != 0; }

    CandidateFrontierAuthorityRecord Lookup(const uint256& hash) const
    {
        CandidateFrontierAuthorityRecord r;
        std::map<uint256, CBlockIndex*>::const_iterator it = mapBlockIndex.find(hash);
        if (it == mapBlockIndex.end() || it->second == NULL)
            return r;
        r.hash = hash;
        r.chainTrust = it->second->nChainTrust;
        r.height = it->second->nHeight;
        r.found = true;
        return r;
    }
    CandidateFrontierAuthorityRecord GetParent(const uint256& child) const
    {
        std::map<uint256, CBlockIndex*>::const_iterator it = mapBlockIndex.find(child);
        if (it == mapBlockIndex.end() || it->second == NULL || it->second->pprev == NULL)
            return CandidateFrontierAuthorityRecord();
        return Lookup(*it->second->pprev->phashBlock);
    }
    std::vector<uint256> GetCandidateTipHashes() const
    {
        std::vector<uint256> out;
        out.reserve(mapCandidateTips.size());
        for (const auto& entry : mapCandidateTips)
            out.push_back(entry.first);
        return out; // hash-sorted (std::map keyed by uint256)
    }
    bool HasBlockData(const uint256& hash) const
    {
        // Honor the persisted/derived fHasData flag (materialization
        // availability) exactly like the legacy evaluator: if a candidate tip
        // record reports data present, treat it eligible without a disk read;
        // otherwise fall back to a real ReadFromDisk availability check.
        std::map<uint256, CandidateTipRecord>::const_iterator rec =
            mapCandidateTips.find(hash);
        if (rec != mapCandidateTips.end() && rec->second.fHasData)
            return true;

        std::map<uint256, CBlockIndex*>::const_iterator it = mapBlockIndex.find(hash);
        if (it == mapBlockIndex.end() || it->second == NULL)
            return false;
        CBlock b;
        return b.ReadFromDisk(it->second);
    }
};

// ---------------------------------------------------------------------------
// Legacy tip rebuild from full mapBlockIndex scan (LEGACY_RESIDENT only; used
// to populate mapCandidateTips for the shadow/adapter store).
// ---------------------------------------------------------------------------
bool RebuildCandidateTips()
{
    g_res_rebuildcandidates_calls++;
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
// Pointer-returning frontier (legacy shadow): evaluates the by-value path over
// the resident mapCandidateTips via the Legacy store, then RE-RESOLVES the
// winning logical hash through mapBlockIndex ONLY to hand a pointer to legacy
// callers. INV2 is measured on the by-value evaluator, not this pointer shim.
// ---------------------------------------------------------------------------
CBlockIndex* EvaluateCandidateFrontier()
{
    if (mapCandidateTips.empty() || pindexBest == NULL)
        return pindexBest;

    LegacyCandidateFrontierStore store;
    const CandidateFrontierAuthorityRecord sel =
        EvaluateCandidateFrontierByValue(store);
    if (!sel.found)
        return pindexBest;

    std::map<uint256, CBlockIndex*>::iterator it = mapBlockIndex.find(sel.hash);
    if (it == mapBlockIndex.end())
        return pindexBest;
    return it->second;
}

void UpdateCandidateTips(CBlockIndex* /*pindexOldTip*/,
                         CBlockIndex* /*pindexNewTip*/)
{
    // Deterministic rebuild-on-evaluation keeps the tip set fresh; incremental
    // mutation is deferred (rare invalidate/reconsider only). Keeping the
    // Production signature intact so existing callers compile.
}

// ---------------------------------------------------------------------------
// Shadow comparator: legacy full scan (authoritative) vs the by-value frontier
// (INV0 hash + INV1 trust). Diagnostic only; legacy remains authoritative.
// ---------------------------------------------------------------------------
void ShadowCompareCandidateSelection()
{
    if (!fCandidateFrontierShadowActive)
        return;

    LOCK(cs_main);

    // Rebuild candidate tips from current mapBlockIndex so the frontier
    // is up-to-date with any recent chain changes.
    RebuildCandidateTips();

    // --- Legacy full scan (inline, authoritative) ---
    CBlockIndex* pindexLegacy = NULL;
    {
        std::set<uint256> setReferenced;
        for (const auto& item : mapBlockIndex)
            if (item.second->pprev != NULL)
                setReferenced.insert(*item.second->pprev->phashBlock);

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
            if (pindexLegacy == NULL ||
                pindex->nChainTrust > pindexLegacy->nChainTrust)
                pindexLegacy = pindex;
        }
    }

    // --- By-value frontier (INV2: no mapBlockIndex.resolve in evaluator) ---
    LegacyCandidateFrontierStore store;
    const CandidateFrontierAuthorityRecord selByValue =
        EvaluateCandidateFrontierByValue(store);

    const bool matchHash = (selByValue.found
        ? (pindexLegacy && selByValue.hash == *pindexLegacy->phashBlock)
        : (pindexLegacy == NULL));
    const bool matchTrust = (selByValue.found
        ? (pindexLegacy && selByValue.chainTrust == pindexLegacy->nChainTrust)
        : true);

    if (!matchHash || !matchTrust)
    {
        std::string legacyHash = pindexLegacy
            ? pindexLegacy->GetBlockHash().ToString().substr(0, 20)
            : "(none)";
        std::string frontierHash = selByValue.found
            ? selByValue.hash.ToString().substr(0, 20)
            : "(none)";
        printf("CANDIDATE_SHADOW: MISMATCH legacy=%s (h=%d trust=%s) "
               "frontier=%s (h=%d trust=%s)\n",
               legacyHash.c_str(),
               pindexLegacy ? pindexLegacy->nHeight : -1,
               pindexLegacy ? CBigNum(pindexLegacy->nChainTrust).ToString().c_str() : "0",
               frontierHash.c_str(),
               selByValue.found ? selByValue.height : -1,
               selByValue.found ? CBigNum(selByValue.chainTrust).ToString().c_str() : "0");
    }
}
