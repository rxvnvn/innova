// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_CANDIDATE_FRONTIER_H
#define BITCOIN_CANDIDATE_FRONTIER_H

#include "main.h"
#include "txdb.h"

#include <vector>

bool RebuildCandidateTips();

// ---------------------------------------------------------------------------
// A.10.1c: By-value authoritative candidate frontier.
//
// AUTHORITY != MATERIALIZATION. Candidate evaluation below consumes ONLY the
// by-value CandidateFrontierStore contract; it never resolves a candidate tip
// through the all-history resident mapBlockIndex graph and it never embeds
// block bytes / CBlockIndex* / disk coordinates into authority records.
//
// AUTHORITY fields  : tip logical hash, exact canonical chainTrust,
//                     height (ancestry/fork walks), operator validity,
//                     finality compatibility, best-trust threshold.
// MATERIALIZATION   : HasBlockData() is a separate availability predicate; a
//                     missing result never mutates candidate authority.
//
// The store leaves a clean seam for future async materialization: evaluation
// returns a LOGICAL TIP HASH (CandidateFrontierAuthorityRecord) and never
// assumes synchronous ReadFromDisk or a CBlock lifetime.
// ---------------------------------------------------------------------------

struct CandidateFrontierAuthorityRecord
{
    uint256 hash;          // logical block hash
    uint256 chainTrust;    // exact canonical trust
    int     height;        // block height
    bool    found;         // valid record present
    bool    isEligible;    // evaluated eligibility (authority filters passed)

    CandidateFrontierAuthorityRecord()
        : hash(0), chainTrust(0), height(-1), found(false), isEligible(false) {}
};

class CandidateFrontierStore
{
public:
    virtual ~CandidateFrontierStore() {}

    // --- live scalar state ---
    virtual bool    IsBestActive() const = 0;   // a best tip exists
    virtual uint256 GetBestTrust() const = 0;   // nBestChainTrust threshold
    virtual uint256 GetBestTip() const = 0;     // active tip logical hash
    virtual int     GetFinalizedHeight() const = 0; // 0 if not active
    virtual bool    IsOperatorHash(const uint256& hash) const = 0; // setInvalidBlockHash

    // --- by-value record access (ancestry/fork walks + tip enumeration) ---
    // Lookup a block's authority by hash. found=false on NOT_FOUND.
    virtual CandidateFrontierAuthorityRecord Lookup(const uint256& hash) const = 0;
    // Parent of a child hash. found=false if child has no parent / missing.
    virtual CandidateFrontierAuthorityRecord GetParent(const uint256& child) const = 0;
    // Enumerate current candidate tips by value (hash-sorted).
    virtual std::vector<uint256> GetCandidateTipHashes() const = 0;

    // --- materialization availability (NOT authority; never mutates it) ---
    virtual bool HasBlockData(const uint256& hash) const = 0;
};

// Evaluate the by-value frontier and return the best eligible tip using the
// EXACT same predicate as ActivateBestEligibleChain, but consuming only
// by-value authority (INV2: zero resident mapBlockIndex candidate resolves).
// Returns a found=false record if no eligible candidate above best trust.
CandidateFrontierAuthorityRecord EvaluateCandidateFrontierByValue(
    const CandidateFrontierStore& store);

// ---------------------------------------------------------------------------
// Pure by-value store — V2-generation shaped: no CBlockIndex*, no
// mapBlockIndex. A V2 generation (StartupAuthority + records.dat/derived.dat)
// populates this same structure. Also the test oracle that proves INV2: a
// fixture that clears the resident mapBlockIndex and evaluates ONLY from this
// value store cannot succeed unless the evaluator is truly by-value.
// ---------------------------------------------------------------------------
class SnapshotCandidateFrontierStore : public CandidateFrontierStore
{
public:
    struct BVec
    {
        uint256 parent;
        uint256 chainTrust;
        int height;
        BVec() : parent(0), chainTrust(0), height(-1) {}
    };

    uint256 bestHash;
    uint256 bestTrust;
    bool    hasBest;
    int     finalizedHeight;
    bool    finalityActive;  // = (finalizedHeight > 0 && finality gate open)
    std::map<uint256, BVec> blocks;    // hash -> record (hash-sorted)
    std::vector<uint256>    tipHashes;
    std::set<uint256>       operatorInvalid;
    std::set<uint256>       hasData;

    SnapshotCandidateFrontierStore()
        : bestHash(0), bestTrust(0), hasBest(false), finalizedHeight(0),
          finalityActive(false) {}

    void SetBest(const uint256& h, const uint256& trust)
    {
        bestHash = h; bestTrust = trust; hasBest = true;
    }
    void AddBlock(const uint256& h, const uint256& parent, const uint256& trust, int height)
    {
        BVec b; b.parent = parent; b.chainTrust = trust; b.height = height;
        blocks[h] = b;
    }

    bool IsBestActive() const { return hasBest; }
    uint256 GetBestTrust() const { return bestTrust; }
    uint256 GetBestTip() const { return bestHash; }
    int GetFinalizedHeight() const { return finalityActive ? finalizedHeight : 0; }
    bool IsOperatorHash(const uint256& h) const { return operatorInvalid.count(h) != 0; }

    CandidateFrontierAuthorityRecord Lookup(const uint256& hash) const
    {
        CandidateFrontierAuthorityRecord r;
        std::map<uint256, BVec>::const_iterator it = blocks.find(hash);
        if (it == blocks.end()) return r;
        r.hash = hash; r.chainTrust = it->second.chainTrust;
        r.height = it->second.height; r.found = true;
        return r;
    }
    CandidateFrontierAuthorityRecord GetParent(const uint256& child) const
    {
        std::map<uint256, BVec>::const_iterator it = blocks.find(child);
        if (it == blocks.end() || it->second.parent == uint256(0))
            return CandidateFrontierAuthorityRecord();
        return Lookup(it->second.parent);
    }
    std::vector<uint256> GetCandidateTipHashes() const { return tipHashes; }
    bool HasBlockData(const uint256& hash) const { return hasData.count(hash) != 0; }
};

// Largest/live clock-free helpers still present for shadow switching.
CBlockIndex* EvaluateCandidateFrontier();

/**
 * Shadow comparator: runs by-value frontier alongside legacy full scan and
 * reports any mismatch via printf.  Legacy scan remains authoritative.
 */
void ShadowCompareCandidateSelection();

// --- persistence (A.10.1c: generation-bound, fail-closed) ---
bool WriteCandidateTips(CTxDB& txdb);
bool ReadCandidateTips(CTxDB& txdb);

#endif // BITCOIN_CANDIDATE_FRONTIER_H