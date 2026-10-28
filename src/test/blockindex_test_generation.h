// Test-only shared helper: assemble the FULL genesis..tip source for an
// isolated V2 generation (checkpoint_v2_authoritative_tests +
// checkpoint_v2_startup_tests; both suites delegate here — single mechanism).
//
// WHY mapBlockIndex iteration is wrong: under BI-V2 authoritative mode
// ::mapBlockIndex holds only the bounded live tail (+ anchors) and blocks
// BELOW the retirement floor leave residency entirely (RetireBlockIndexBelowFloor
// frees them AND detaches survivor pprev links), so an iterator / deep pprev
// walk both go sparse -> the real BlockIndexGenerationBuilder::Build fails
// closed with "active chain height discontinuity".
//
// WHY the by-height authoritative resolver is wrong here: the isolated root's
// RAII resets the process authority BEFORE the source is assembled (the fresh
// generation must exist before InitBlockIndexAuthoritative on the new root can
// run), so no authoritative runtime is open while the source is built.
//
// THE MECHANISM (production by-value readers only, no CBlockIndex residency
// dependency): the CURRENT shared fixture authority has an OPEN, PROCESS-LIFE
// PRODUCTION V2 READER for the still-SELECTED shared generation. That reader is
// the immutable generation's full by-value authority, and its ACTIVE-CHAIN
// members are exactly the record source needed. Every record to include comes
// from ONE of:
//   1. ::pindexGenesisBlock height-0/depth barrier values -> getters below;
//   2. the live pprev RESIDENT TAIL (a bounded number of ACCEPTED tip-side
//      blocks); the split point is the anchor pass-through where the resolver's
//      own data replaces the live-walk;
//   3. ::GetAuthoritativeBaseReader() -> GetActiveByHeight(h) for the COLD
//      retired chain members (heights below the bounded tail).
// Both V2 sources are REAL readers; the isolation test's only test-only input
// is the HEIGHT SPLIT, which is a pure residency detail. hashPrev leakage is
// neutralized by using BlockIndexRecord::hashPrevStable (the persisted link)
// and the immutable parent identity for the historical records.
//
// All values come from production by-value APIs (the deterministic builder
// recomputes nChainTrust on its own, so the trust axis is builder-side).
//
// VALIDATION DONE HERE (relative/index-based; the builder re-verifies the
// absolute density 0..N semantics):
//   * every CONSECUTIVE pair advances EXACTLY by +1 height (isolated V2
//     generations legitimately start their base above 0: no absolute-0 rule);
//   * the parent identity chain is exact (data.prevLink == prevHash);
//   * final ordering is dense ascending heights BEFORE return.
#ifndef INNOVA_TEST_BLOCKINDEX_GENERATION_H
#define INNOVA_TEST_BLOCKINDEX_GENERATION_H

#include "main.h"
#include "blockindex_generation_builder.h"
#include "blockindex_generation_lifecycle.h"
#include "blockindex_v2_reader.h"
#include "blockindex_authoritative_startup.h"
#include "blockindex_shadow_startup.h"
#include <boost/filesystem.hpp>

namespace fs = boost::filesystem;

namespace innova_test_gen {

inline bool BuildLiveChainSourceAuthoritative(
    BlockIndexGenerationSource* source,
    std::string* error,
    CBlockIndex* tipChain = NULL)
{
    if (error) error->clear();
    source->records.clear();
    CBlockIndex* start = (tipChain ? tipChain : ::pindexBest);
    if (!start)
    { if (error) *error = "live chain source: no active tip"; return false; }
    const uint256 hashGenesis = pindexGenesisBlock->GetBlockHash();
    const int tipHeight = start->nHeight;

    // Split the ASSEMBLY span: walk the resident pprev tail first, so the
    // returned lineage is stable under the bounded CBlockIndex* residency
    // (deepest RESIDENT record = the tip-side anchor).
    std::vector<CBlockIndex*> residentSpan;
    for (CBlockIndex* p = start; p; p = p->pprev)
        residentSpan.push_back(p);
    int residentFloorHeight = residentSpan.empty() ? -1 : residentSpan.back()->nHeight;

    // ---- (1) COLD side: ACTIVE records of the OPEN shared generation reader --
    // Only heights BELOW the resident tail, from the production reader of the
    // CURRENT (still-selected) shared generation. Records resolve BY VALUE:
    // this is exactly what the "non-resident" checkpoint suite asserts the
    // authority supports. (No live authoritative resolver armed here; the
    // reader lifecycle is retained process-wide by the fixture boot.)
    // COLD side: the shared fixture's selected V2 generation. Under a STILL-
    // OPEN authority the navigator's cold reader is retained process-wide;
    // when the RAII has already torn the process authority down (a fresh
    // isolated-root path without a preceding open generation), the source
    // CANNOT be assembled by any production reader and we fail closed.
    const BlockIndexV2Reader* rd = GetAuthoritativeBaseReader();
    if (!rd || !rd->IsOpen())
        rd = GetAuthoritativeNavigatorReader();
    if (!rd || !rd->IsOpen())
        rd = GetBlockIndexV2ShadowReader();
    if (!rd || !rd->IsOpen())
    { if (error) *error = "live chain source: authoritative base reader not open"; return false; }

    for (int h = 0; h <= tipHeight; ++h)
    {
        // TIP-SIDE record: from the resident span when still connected.
        CBlockIndex* fromResident = NULL;
        if (h <= residentFloorHeight)
            fromResident = residentSpan[ (residentSpan.size() - 1) - (residentFloorHeight - h) ];
        // (h > residentFloorHeight never happens: the walk always reaches the
        // shared genesis, because the authority's genesis anchor stays
        // resident.)

        BlockIndexSnapshot snap;
        if (fromResident)
        {
            // The persisted record's full dataset is available on the resident
            // object; use the stable-key fields from the object itself.
            snap.found = true;
            snap.height = fromResident->nHeight;
            snap.hash = fromResident->GetBlockHash();
            snap.hashPrev = fromResident->hashPrevStable;
            snap.hashMerkleRoot = fromResident->hashMerkleRoot;
            snap.hashProof = fromResident->hashProof;
            snap.prevoutStake = fromResident->prevoutStake;
            snap.nFile = fromResident->nFile;
            snap.nBlockPos = fromResident->nBlockPos;
            snap.nFlags = fromResident->nFlags;
            snap.nVersion = fromResident->nVersion;
            snap.nTime = fromResident->nTime;
            snap.nBits = fromResident->nBits;
            snap.nNonce = fromResident->nNonce;
            snap.nMint = fromResident->nMint;
            snap.nMoneySupply = fromResident->nMoneySupply;
            snap.nStakeModifier = fromResident->nStakeModifier;
            snap.nStakeTime = fromResident->nStakeTime;
            snap.fInMainChain = true;
        }
        else
        {
            // COLD member: from the shared authority's base reader.
            const BlockIndexV2ReadStatus st = rd->GetActiveByHeight(h, &snap, NULL);
            if (st != BLOCK_INDEX_V2_READ_FOUND || !snap.found)
            {
                if (error) *error = "live chain source: cold resolution failed at height " +
                                    std::to_string(h);
                return false;
            }
        }

        BlockIndexGenerationSourceRecord rec;
        rec.hash = snap.hash;
        rec.record.hash = snap.hash;
        rec.record.hashPrev = (snap.height == 0) ? uint256(0) : snap.hashPrev;
        rec.record.hashMerkleRoot = snap.hashMerkleRoot;
        rec.record.hashProof = snap.hashProof;
        rec.record.prevoutStake = snap.prevoutStake;
        rec.record.height = snap.height;
        rec.record.nFile = snap.nFile;
        rec.record.nBlockPos = snap.nBlockPos;
        rec.record.nFlags = snap.nFlags;
        rec.record.nVersion = snap.nVersion;
        rec.record.nTime = snap.nTime;
        rec.record.nBits = snap.nBits;
        rec.record.nNonce = snap.nNonce;
        rec.record.nMint = snap.nMint;
        rec.record.nMoneySupply = snap.nMoneySupply;
        rec.record.nStakeModifier = snap.nStakeModifier;
        rec.record.nStakeTime = snap.nStakeTime;
        source->records.push_back(rec);
    }

    // Continuity guard (relative heights + parent-identity chain).
    for (size_t i = 1; i < source->records.size(); ++i)
    {
        const BlockIndexGenerationSourceRecord& prev = source->records[i-1];
        const BlockIndexGenerationSourceRecord& cur  = source->records[i];
        if (cur.record.height != prev.record.height + 1)
        { if (error) *error = "live chain source: height discontinuity at " + std::to_string(i); return false; }
        if (cur.record.hashPrev != prev.hash)
        { if (error) *error = "live chain source: prev-link mismatch at " + std::to_string(i); return false; }
    }
    if (source->records.empty())
    { if (error) *error = "live chain source: empty chain"; return false; }

    source->hashBestChain = start->GetBlockHash();
    source->foundBestChain = true;
    source->blockDataDir = GetDataDir().string();
    return true;
}

// Build -> publish -> select a fresh isolated V2 generation from the live chain.
inline bool BuildAndSelectLiveGeneration(const fs::path& root, std::string* error,
                                         CBlockIndex* tipChain = NULL)
{
    std::string serr;
    BlockIndexGenerationSource source;
    if (!BuildLiveChainSourceAuthoritative(&source, &serr, tipChain))
    { if (error) *error = std::string("live-gen source: ") + serr; return false; }
    BlockIndexGenerationBuilder builder;
    if (!builder.Build(source, (root / "build-000001.tmp").string(), 1, NULL, &serr))
    { if (error) *error = std::string("live-gen build: ") + serr; return false; }
    builder.Close();
    serr.clear();
    if (BlockIndexGenerationManager::PublishGeneration(root.string(), 1, &serr) != BLOCK_INDEX_LIFECYCLE_OK)
    { if (error) *error = std::string("live-gen publish failed: ") + serr; return false; }
    serr.clear();
    if (BlockIndexGenerationManager::SelectGeneration(root.string(), 1, &serr) != BLOCK_INDEX_LIFECYCLE_OK)
    { if (error) *error = std::string("live-gen select failed: ") + serr; return false; }
    return true;
}

} // namespace innova_test_gen
#endif
