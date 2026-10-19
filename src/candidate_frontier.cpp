// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// PM1-P0-07b (C3) - candidate frontier DECOUPLING.
//
// This translation unit is the by-value candidate frontier AUTHORITY CORE. It
// has ZERO dependency on the historical global resident block index graph / CBlockIndex
// pprev graph: no full-history scan, no pointer resolve, no substitute cache.
//
// Source of candidate tips (production, authoritative): the immutable static
// leaf frontier persisted as `candidate-leaves.dat` inside the selected V2
// generation, consumed through
//   blockindex_candidate_startup_builder.cpp -> ReadCandidateLeafMetadata(...)
// which populates the by-value SnapshotCandidateFrontierStore. Evaluation then
// runs over that bounded (O(frontier)) value store only.
//
// Any remaining legacy resident-graph scanning lives in
// candidate_frontier_legacy.cpp (LEGACY_RESIDENT compatibility; retired at C4).

#include "candidate_frontier.h"
#include "main.h"

#include <set>
#include <map>
#include <string>

extern std::map<uint256, CandidateTipRecord> mapCandidateTips;
extern uint64_t nCandidateTipGeneration;

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

// ---------------------------------------------------------------------------
// A.10.1c core: by-value candidate evaluation (INV2).
//
// Consumes ONLY the CandidateFrontierStore by-value contract. No
// resident-index find, no CBlockIndex*, no ReadFromDisk for authority — the
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
// PERSISTENCE (A.10.1c): generation-bound, fail-closed wiring of the existing
// dead CTxDB candidate-tips members. Persistence is available for a bounded
// O(F) startup load and is deliberately NOT loaded blindly across generations.
// ---------------------------------------------------------------------------
// Candidate-tip persistence lived in the retired transaction database. The V2
// block index owns candidate leaves in its own generation (candidate-leaves.dat),
// so this legacy keyed store has no reader and is removed.


