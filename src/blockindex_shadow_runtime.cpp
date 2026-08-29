#include "blockindex_shadow_runtime.h"

#include "blockindex_generation_lifecycle.h"
#include "blockindex_hashindex.h"
#include "blockindex_activeindex.h"
#include "util.h"

#include <boost/filesystem.hpp>

#include <stdint.h>
#include <stdio.h>
#include <string>
#include <vector>

namespace {

static const char* const kShadowTag = "BLOCKINDEX_V2_SHADOW";

static bool RecordsMatchV1(const BlockIndexRecord& a, const BlockIndexRecord& b)
{
    if (a.hash != b.hash) return false;
    if (a.hashPrev != b.hashPrev) return false;
    if (a.hashMerkleRoot != b.hashMerkleRoot) return false;
    if (a.hashProof != b.hashProof) return false;
    if (!(a.prevoutStake == b.prevoutStake)) return false;
    if (a.height != b.height) return false;
    if (a.nFile != b.nFile) return false;
    if (a.nBlockPos != b.nBlockPos) return false;
    if (a.nFlags != b.nFlags) return false;
    if (a.nVersion != b.nVersion) return false;
    if (a.nTime != b.nTime) return false;
    if (a.nBits != b.nBits) return false;
    if (a.nNonce != b.nNonce) return false;
    if (a.nMint != b.nMint) return false;
    if (a.nMoneySupply != b.nMoneySupply) return false;
    if (a.nStakeModifier != b.nStakeModifier) return false;
    if (a.nStakeTime != b.nStakeTime) return false;
    return true;
}

// Deterministic xorshift64 (fixed seed, reproducible across a generation+tip).
struct XorShift64
{
    uint64_t s;
    explicit XorShift64(uint64_t seed) : s(seed ? seed : 0x9E3779B97F4A7C15ULL) {}
    uint64_t next() { uint64_t x = s; x ^= x << 13; x ^= x >> 7; x ^= x << 17; s = x; return x; }
    uint64_t range(uint64_t n) { return n ? (next() % n) : 0; }
};

} // namespace

BlockIndexV2ShadowRuntime::BlockIndexV2ShadowRuntime()
{
    state.enabled = false;
    state.authoritative = false;
}

const BlockIndexV2ShadowState& BlockIndexV2ShadowRuntime::OpenShadow(
    const BlockIndexShadowOpenConfig& cfg,
    BlockIndexShadowLegacyOracle* legacy,
    std::string* error)
{
    state = BlockIndexV2ShadowState();
    state.enabled = true;
    state.configuredRoot = cfg.root;
    state.authoritative = false;

    if (!legacy)
    {
        state.status = (int)BLOCK_INDEX_LIFECYCLE_ERROR;
        state.compatibility = BLOCK_INDEX_SHADOW_COMPAT_ERROR;
        state.lastError = "null legacy oracle";
        if (error) *error = state.lastError;
        return state;
    }

    const int64_t openStart = GetTimeMillis();

    // ---- OpenCurrent: read CURRENT + structurally validate selected generation.
    // Use the lifecycle manager's ReadCurrent + OpenCurrent semantics.
    {
        std::string e;
        BlockIndexCurrentRecord cur;
        BlockIndexLifecycleStatus rc = BlockIndexGenerationManager::ReadCurrent(cfg.root, &cur, &e);
        if (rc == BLOCK_INDEX_LIFECYCLE_NOT_PUBLISHED)
        {
            state.status = (int)rc;
            state.compatibility = BLOCK_INDEX_SHADOW_COMPAT_NOT_PUBLISHED;
            state.lastError = "CURRENT absent (no published generation)";
            if (error) *error = state.lastError;
            return state;
        }
        if (rc != BLOCK_INDEX_LIFECYCLE_OK)
        {
            state.status = (int)rc;
            state.compatibility = BLOCK_INDEX_SHADOW_COMPAT_CORRUPT;
            state.lastError = "CURRENT corrupt or unreadable: " + e;
            if (error) *error = state.lastError;
            return state;
        }
        state.generation = cur.generation;
    }

    // Structural validation of the selected generation (lifecycle-level).
    {
        std::string e;
        BlockIndexLifecycleStatus v = BlockIndexGenerationManager::ValidateGeneration(cfg.root, state.generation, &e);
        state.structuralValidationOk = (v == BLOCK_INDEX_LIFECYCLE_OK);
        if (!state.structuralValidationOk)
        {
            state.status = (int)v;
            state.compatibility = BLOCK_INDEX_SHADOW_COMPAT_CORRUPT;
            state.lastError = "structural validation failed: " + e;
            if (error) *error = state.lastError;
            return state;
        }
    }

    // Read the selected generation's committed tip from MANIFEST (generation dir
    // resolved and validated, so this succeeds).
    {
        FixedBlockIndexOpenOptions opts;
        opts.requireCompleteManifest = true;
        std::string genDir = BlockIndexGenerationManager::GenerationPath(cfg.root, state.generation);
        FixedBlockIndexStore store;
        std::string e;
        if (!FixedBlockIndexStore::OpenReadOnly(genDir, opts, &store, &e))
        {
            state.status = (int)BLOCK_INDEX_LIFECYCLE_ERROR;
            state.compatibility = BLOCK_INDEX_SHADOW_COMPAT_ERROR;
            state.lastError = "shadow store open failed: " + e;
            if (error) *error = state.lastError;
            return state;
        }
        const FixedBlockIndexManifest& m = store.GetManifest();
        state.shadowTipHeight = m.committedTipHeight;
        state.shadowTipHash = m.committedTipHash;
    }

    state.openDurationMs = GetTimeMillis() - openStart;

    // ---- Compatibility classification against the legacy oracle.
    const int legacyTip = legacy->GetLegacyTipHeight();
    state.legacyTipHeightAtValidation = legacyTip;
    state.heightLag = legacyTip - (int)state.shadowTipHeight;

    BlockIndexRecord legacyTipRec;
    if (state.shadowTipHeight < 0)
    {
        state.compatibility = BLOCK_INDEX_SHADOW_COMPAT_ERROR;
        state.lastError = "shadow committed tip is negative";
        if (error) *error = state.lastError;
        return state;
    }

    if (legacyTip < state.shadowTipHeight)
    {
        // Shadow is AHEAD of the legacy chain (invalid for A.7 shadow).
        state.compatibility = BLOCK_INDEX_SHADOW_COMPAT_AHEAD;
        state.tipOnLegacyActiveChain = false;
        state.lastError = strprintf("shadow tip height %d ahead of legacy tip %d",
                                    state.shadowTipHeight, legacyTip);
        if (error) *error = state.lastError;
        return state;
    }

    // Legacy active block must exist at shadow height and its hash must equal the
    // shadow committed tip hash.
    bool legacyAtShadowHeight = legacy->GetLegacyActiveRecord(state.shadowTipHeight, &legacyTipRec);
    if (!legacyAtShadowHeight)
    {
        state.compatibility = BLOCK_INDEX_SHADOW_COMPAT_ERROR;
        state.tipOnLegacyActiveChain = false;
        state.lastError = "no legacy active block at shadow tip height";
        if (error) *error = state.lastError;
        return state;
    }
    state.tipOnLegacyActiveChain = (legacyAtShadowHeight && legacyTipRec.hash == state.shadowTipHash);
    if (!state.tipOnLegacyActiveChain)
    {
        state.compatibility = BLOCK_INDEX_SHADOW_COMPAT_DIVERGED;
        state.lastError = strprintf("shadow tip hash %s != legacy active hash at %d",
                                    state.shadowTipHash.ToString().substr(0,16).c_str(),
                                    state.shadowTipHeight);
        if (error) *error = state.lastError;
        return state;
    }

    // Tip on legacy active chain -> classify EXACT or ANCESTOR.
    if (legacyTip == state.shadowTipHeight)
        state.compatibility = BLOCK_INDEX_SHADOW_COMPAT_EXACT;
    else
        state.compatibility = BLOCK_INDEX_SHADOW_COMPAT_ANCESTOR;

    // ---- Bounded deterministic differential + boundary checks.
    const int64_t valStart = GetTimeMillis();
    RunBoundedDifferential(legacy, cfg);
    RunBoundaryChecks(legacy);
    state.validationDurationMs = GetTimeMillis() - valStart;

    // Recompute height lag (may have advanced during sample).
    state.legacyTipHeightAtValidation = legacy->GetLegacyTipHeight();
    state.heightLag = state.legacyTipHeightAtValidation - (int)state.shadowTipHeight;

    // If any sample mismatch was found, shadow is not clean (but compatibility
    // classification stays; the runtime reports SAMPLE_MISMATCH only if we want
    // to flag it distinctly). We keep the EXACT/ANCESTOR classification but flag
    // a mismatch in the state; the caller (strict mode) can fail.
    state.lastError.clear();

    return state;
}

void BlockIndexV2ShadowRuntime::RunBoundaryChecks(BlockIndexShadowLegacyOracle* legacy)
{
    // Compare non-random boundary points when within the shadow committed range.
    std::vector<int> boundaryHeights;
    boundaryHeights.push_back(0); // genesis
    if (state.shadowTipHeight > 0)
        boundaryHeights.push_back(state.shadowTipHeight - 1);
    boundaryHeights.push_back(state.shadowTipHeight);
    if (state.shadowTipHeight > 1)
        boundaryHeights.push_back(state.shadowTipHeight / 2);

    std::string genDir = BlockIndexGenerationManager::GenerationPath(state.configuredRoot, state.generation);
    FixedBlockIndexOpenOptions opts;
    opts.requireCompleteManifest = true;
    FixedBlockIndexStore store;
    BlockIndexActiveIndex active;
    {
        std::string e;
        if (!FixedBlockIndexStore::OpenReadOnly(genDir, opts, &store, &e)) return;
        if (!BlockIndexActiveIndex::Open(genDir, state.generation, &active, &e)) return;
    }

    // Active-height boundary comparisons.
    for (size_t i = 0; i < boundaryHeights.size(); ++i)
    {
        const int h = boundaryHeights[i];
        if (h < 0 || h > state.shadowTipHeight) continue;
        ++state.sampleHeightChecks;
        BlockIndexId id = BLOCK_INDEX_ID_INVALID;
        std::string e;
        if (!active.ReadEntry(h, &id, &e)) { ++state.sampleMismatches; continue; }
        if (id == BLOCK_INDEX_ID_INVALID) { ++state.sampleMismatches; continue; }
        BlockIndexRecord v2rec;
        if (!store.Read(id, &v2rec, &e)) { ++state.sampleMismatches; continue; }

        BlockIndexRecord legacyRec;
        if (!legacy->GetLegacyActiveRecord(h, &legacyRec)) { ++state.sampleMismatches; continue; }
        if (!RecordsMatchV1(v2rec, legacyRec)) ++state.sampleMismatches;
    }
}

void BlockIndexV2ShadowRuntime::RunBoundedDifferential(BlockIndexShadowLegacyOracle* legacy,
                                                       const BlockIndexShadowOpenConfig& cfg)
{
    std::string genDir = BlockIndexGenerationManager::GenerationPath(state.configuredRoot, state.generation);
    FixedBlockIndexOpenOptions opts;
    opts.requireCompleteManifest = true;
    FixedBlockIndexStore store;
    BlockIndexHashIndex hashIndex;
    BlockIndexActiveIndex active;
    {
        std::string e;
        if (!FixedBlockIndexStore::OpenReadOnly(genDir, opts, &store, &e)) return;
        if (!BlockIndexHashIndex::Open(genDir, state.generation, &hashIndex, &e)) return;
        if (!BlockIndexActiveIndex::Open(genDir, state.generation, &active, &e)) return;
    }
    const uint64_t recordCount = store.GetManifest().recordCount;

    // Deterministic PRNG seeded from generation + recordCount + tip so failures
    // are reproducible and independent of nondeterministic entropy.
    uint64_t seed = 0x9E3779B97F4A7C15ULL ^ (state.generation * 0x9E3779B9ULL) ^
                    (recordCount * 0x85EBCA6BULL) ^ ((uint64_t)state.shadowTipHeight * 0xC2B2AE35ULL);
    XorShift64 rng(seed);

    // Active-height samples within [0, shadowTip].
    for (uint64_t k = 0; k < cfg.heightSamples && state.shadowTipHeight >= 0; ++k)
    {
        const uint64_t h64 = rng.range((uint64_t)state.shadowTipHeight + 1);
        const int h = (int)h64;
        ++state.sampleHeightChecks;
        BlockIndexId id = BLOCK_INDEX_ID_INVALID;
        std::string e;
        if (!active.ReadEntry(h, &id, &e)) { ++state.sampleMismatches; continue; }
        if (id == BLOCK_INDEX_ID_INVALID) { ++state.sampleMismatches; continue; }
        BlockIndexRecord v2rec;
        if (!store.Read(id, &v2rec, &e)) { ++state.sampleMismatches; continue; }
        BlockIndexRecord legacyRec;
        if (!legacy->GetLegacyActiveRecord(h, &legacyRec)) { ++state.sampleMismatches; continue; }
        if (!RecordsMatchV1(v2rec, legacyRec)) ++state.sampleMismatches;
    }

    // Hash-based samples: choose a deterministic RecordId, read the V2 record,
    // verify hashindex resolves it, and compare to legacy by hash.
    if (recordCount > 0)
    {
        for (uint64_t k = 0; k < cfg.hashSamples; ++k)
        {
            const uint64_t id = rng.range(recordCount) + 1; // 1..recordCount
            ++state.sampleHashChecks;
            BlockIndexRecord v2rec;
            std::string e;
            if (!store.Read(id, &v2rec, &e)) { ++state.sampleMismatches; continue; }

            BlockIndexId resolved = BLOCK_INDEX_ID_INVALID;
            if (hashIndex.Lookup(v2rec.hash, &resolved, &e) != BLOCK_INDEX_HASH_LOOKUP_FOUND)
            {
                ++state.sampleMismatches;
                continue;
            }
            if (resolved != id) { ++state.sampleMismatches; continue; }

            BlockIndexRecord legacyRec;
            if (!legacy->GetLegacyRecordByHash(v2rec.hash, &legacyRec))
            {
                ++state.sampleMismatches;
                continue;
            }
            if (!RecordsMatchV1(v2rec, legacyRec)) ++state.sampleMismatches;
        }
    }
    hashIndex.Close();
}

bool BlockIndexV2ShadowRuntime::RevalidateTipOnActiveChain(BlockIndexShadowLegacyOracle* legacy) const
{
    if (!state.enabled || state.shadowTipHeight < 0)
        return false;
    BlockIndexRecord rec;
    if (!legacy->GetLegacyActiveRecord(state.shadowTipHeight, &rec))
        return false;
    return rec.hash == state.shadowTipHash;
}