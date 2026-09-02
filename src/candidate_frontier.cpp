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
// By-value ancestry/fork helpers (no CBlockIndex* in the authority path).
// ---------------------------------------------------------------------------

// Return true if the tip's ancestor chain contains any operator-invalid hash.
static bool AncestryOperatorInvalidByValue(const CandidateFrontierStore& store,
                                           const uint256& tip)
{
    uint256 cur = tip;
    for (int guard = 0; guard < 100000 && cur != uint256(0); ++guard)
    {
        if (store.IsOperatorHash(cur))
            return true;
        const CandidateFrontierAuthorityRecord parent = store.GetParent(cur);
        if (!parent.found)
            break; // reached a missing root / genesis boundary
        cur = parent.hash;
    }
    return false;
}

// Compute the fork point height between two chains reachable by parent-walk,
// returning the fork height, or -1 if the fork is below the finalized height
// (i.e. the candidate is not finality-compatible). Returns >= 0 on success.
static int FinalityForkHeight(const CandidateFrontierStore& store,
                              const uint256& candidateHash, const uint256& bestHash)
{
    CandidateFrontierAuthorityRecord c = store.Lookup(candidateHash);
    CandidateFrontierAuthorityRecord b = store.Lookup(bestHash);
    if (!c.found || !b.found)
        return -1;
    // Equalize heights by parent-walk.
    while (c.height > b.height)
    {
        c = store.GetParent(c.hash);
        if (!c.found) return -1;
    }
    while (b.height > c.height)
    {
        b = store.GetParent(b.hash);
        if (!b.found) return -1;
    }
    while (c.hash != b.hash)
    {
        c = store.GetParent(c.hash);
        b = store.GetParent(b.hash);
        if (!c.found || !b.found) return -1;
    }
    if (!c.found)
        return -1;
    return c.height;
}

// ---------------------------------------------------------------------------
// A.10.1c core: by-value candidate evaluation (INV2).
//
// Consumes ONLY the CandidateFrontierStore by-value contract. No
// mapBlockIndex.find, no CBlockIndex*, no ReadFromDisk for authority — the
// iterator order over GetCandidateTipHashes() is hash-sorted(std::map), the
// comparator is strict > on chainTrust, equality never replaces the baseline;
// this reproduces the exact ActivateBestEligibleChain predicate (design F4-F7).
// ---------------------------------------------------------------------------
CandidateFrontierAuthorityRecord EvaluateCandidateFrontierByValue(
    const CandidateFrontierStore& store)
{
    CandidateFrontierAuthorityRecord best;
    best.found = false;
    best.isEligible = false;

    if (!store.IsBestActive())
        return best; // no best chain active

    const uint256 bestTrust = store.GetBestTrust();
    const int nFinalHeight = store.GetFinalizedHeight();
    const bool fFinalityActive = (nFinalHeight > 0);
    const uint256 bestHash = store.GetBestTip();

    const std::vector<uint256> tips = store.GetCandidateTipHashes();
    for (const uint256& tip : tips)
    {
        CandidateFrontierAuthorityRecord rec = store.Lookup(tip);
        if (!rec.found)
            continue; // authority missing: exclude this tip for the cycle

        // filter 1: trust strictly above best
        if (!(rec.chainTrust > bestTrust))
            continue;
        // filter 2: operator validity (ancestor walk over by-value parent chain)
        if (AncestryOperatorInvalidByValue(store, tip))
            continue;
        // filter 3: materialization availability (never mutates authority)
        if (!store.HasBlockData(tip))
            continue;
        // filter 4: finality fork compatibility
        if (fFinalityActive)
        {
            const int forkHt = FinalityForkHeight(store, tip, bestHash);
            if (forkHt < 0 || forkHt < nFinalHeight)
                continue;
        }
        // baseline + strict > (exact legacy comparator; equal trust never replaces)
        if (!best.found || rec.chainTrust > best.chainTrust)
        {
            best = rec;
            best.isEligible = true;
        }
    }

    return best;
}

// ---------------------------------------------------------------------------
// CandidateFrontierStore implementations
// ---------------------------------------------------------------------------

// Legacy shadow adapter. Internally reads the resident graph to construct
// by-value records (allowed for the transition/shadow adapter), but the
// evaluator (EvaluateCandidateFrontierByValue) never sees a CBlockIndex*.
// Evaluates finality activation exactly like legacy: active only when
// finalized height > 0 AND best height >= FORK_HEIGHT_FINALITY.
class LegacyCandidateFrontierStore : public CandidateFrontierStore
{
public:
    bool IsBestActive() const { return pindexBest != NULL; }
    uint256 GetBestTrust() const { return nBestChainTrust; }
    uint256 GetBestTip() const { return pindexBest ? *pindexBest->phashBlock : uint256(0); }
    int GetFinalizedHeight() const
    {
        const int h = g_finalityTracker.GetFinalizedHeight();
        if (h > 0 && pindexBest && pindexBest->nHeight >= FORK_HEIGHT_FINALITY)
            return h;
        return 0;
    }
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

// Pure by-value store. No CBlockIndex*, no mapBlockIndex. A V2 generation
// (StartupAuthority + records.dat/derived.dat) populates this same structure.
// Defined in candidate_frontier.h (SnapshotCandidateFrontierStore) so tests can
// use it as the INV2 oracle.

// ---------------------------------------------------------------------------
// Legacy tip rebuild from full mapBlockIndex scan (unchanged authority; used
// to populate mapCandidateTips for the shadow/adapter store).
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
// Pointer-returning frontier (shadow): evaluates the by-value path against the
// resident mapCandidateTips via the Legacy store, then RE-RESOLVES the winning
// logical hash through mapBlockIndex ONLY to hand a pointer to legacy callers.
// INV2 is measured on the by-value evaluator, not this pointer shim.
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
// PERSISTENCE (A.10.1c): generation-bound, fail-closed wiring of the existing
// dead CTxDB candidate-tips members. Authority is produced by RebuildCandidateTips
// on startup and at invalidate/reconsider; persistence is available for a future
// O(F) startup load and is deliberately NOT loaded blindly across generations.
// ---------------------------------------------------------------------------
bool WriteCandidateTips(CTxDB& txdb)
{
    return txdb.WriteCandidateTips(mapCandidateTips);
}

bool ReadCandidateTips(CTxDB& txdb)
{
    std::map<uint256, CandidateTipRecord> loaded;
    if (!txdb.ReadCandidateTips(loaded) || loaded.empty())
        return false; // FAIL CLOSED: no set or unreadable

    // RECORDS carry the generation identity; all records in a persisted set
    // must share one generation and it must equal the CURRENT expected
    // generation. Otherwise the set is stale/corrupt and is not consumed.
    uint64_t setGen = loaded.begin()->second.nGeneration;
    if (setGen == 0)
        return false;
    for (const auto& entry : loaded)
        if (entry.second.nGeneration != setGen)
            return false; // mixed generations: corrupt

    if (setGen != nCandidateTipGeneration)
        return false; // GENERATION MISMATCH: do not consume stale tips

    mapCandidateTips.swap(loaded);
    return true;
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