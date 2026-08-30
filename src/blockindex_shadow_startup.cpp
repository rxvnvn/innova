#include "blockindex_shadow_startup.h"

#include "blockindex_shadow_runtime.h"
#include "blockindex_v2_reader.h"
#include "main.h"
#include "sync.h"
#include "util.h"

#include <stdio.h>
#include <string>
#include <memory>

namespace {

CCriticalSection cs_shadowState;
BlockIndexV2ShadowState g_shadowState;      // final/known shadow state
BlockIndexV2ShadowRuntime g_shadowRuntime;  // holds the last open result
bool g_shadowOpened = false;

// A.8: exactly one retained read-only reader bound to the READY generation.
// The reader is not movable/copyable (it owns CCriticalSection + handles), so it
// is heap-allocated once. NULL until a READY generation is retained.
std::unique_ptr<BlockIndexV2Reader> g_shadowReader;

// A.9a.3c: exactly one retained ColdHotSeamNavigator for production staking
// navigation. Owns its own read-only V2 reader on the validated generation plus
// the LegacyBlockIndexAccessor hot side. NULL until retained.
std::unique_ptr<ColdHotSeamNavigator> g_stakingNavigator;

} // namespace

BlockIndexRecord BlockIndexRecordFromIndex(const CBlockIndex* pindex)
{
    BlockIndexRecord rec;
    rec.hash = pindex->GetBlockHash();
    rec.hashPrev = pindex->pprev ? pindex->pprev->GetBlockHash() : uint256(0);
    rec.hashMerkleRoot = pindex->hashMerkleRoot;
    rec.hashProof = pindex->hashProof;
    rec.height = pindex->nHeight;
    rec.nFile = pindex->nFile;
    rec.nBlockPos = pindex->nBlockPos;
    rec.nFlags = pindex->nFlags;
    rec.nVersion = pindex->nVersion;
    rec.nTime = pindex->nTime;
    rec.nBits = pindex->nBits;
    rec.nNonce = pindex->nNonce;
    rec.nMint = pindex->nMint;
    rec.nMoneySupply = pindex->nMoneySupply;
    rec.nStakeModifier = pindex->nStakeModifier;
    rec.prevoutStake = pindex->prevoutStake;
    rec.nStakeTime = pindex->nStakeTime;
    return rec;
}

int LegacyBlockIndexShadowOracle::GetLegacyTipHeight()
{
    LOCK(cs_main);
    return pindexBest ? pindexBest->nHeight : -1;
}

bool LegacyBlockIndexShadowOracle::GetLegacyActiveRecord(int height, BlockIndexRecord* out)
{
    LOCK(cs_main);
    if (!pindexBest || height < 0 || height > pindexBest->nHeight)
        return false;
    CBlockIndex* pindex = FindBlockByHeight(height);
    if (!pindex)
        return false;
    *out = BlockIndexRecordFromIndex(pindex);
    return true;
}

bool LegacyBlockIndexShadowOracle::GetLegacyRecordByHash(const uint256& hash, BlockIndexRecord* out)
{
    LOCK(cs_main);
    std::map<uint256, CBlockIndex*>::iterator it = mapBlockIndex.find(hash);
    if (it == mapBlockIndex.end())
        return false;
    *out = BlockIndexRecordFromIndex(it->second);
    return true;
}

const BlockIndexV2ShadowState& TryOpenBlockIndexV2Shadow()
{
    LOCK(cs_shadowState);
    if (g_shadowOpened)
        return g_shadowState;

    const std::string root = GetArg("-blockindexv2shadow", "");
    const bool strict = GetBoolArg("-blockindexv2shadowstrict", false);

    if (root.empty())
    {
        // Not configured: shadow DISABLED, legacy path unchanged.
        g_shadowState = BlockIndexV2ShadowState();
        g_shadowState.enabled = false;
        g_shadowOpened = true;
        return g_shadowState;
    }

    // Build config.
    BlockIndexShadowOpenConfig cfg;
    cfg.root = root;
    cfg.strict = strict;
    const int64_t hashSamples = GetArg("-blockindexv2shadowhashsamples", 1024);
    const int64_t heightSamples = GetArg("-blockindexv2shadowheightsamples", 1024);
    cfg.hashSamples = hashSamples <= 0 ? 0 : (uint64_t)hashSamples;
    cfg.heightSamples = heightSamples <= 0 ? 0 : (uint64_t)heightSamples;

    LegacyBlockIndexShadowOracle legacy;

    printf("BLOCKINDEX_V2_SHADOW enabled=1 root=%s strict=%s\n",
           root.c_str(), strict ? "1" : "0");
    std::string err;
    // OpenShadow runs the bounded differential and sets the state.
    const BlockIndexV2ShadowState& st = g_shadowRuntime.OpenShadow(cfg, &legacy, &err);
    g_shadowState = st;

    // Diagnostics (Section 15).
    printf("BLOCKINDEX_V2_SHADOW current_generation=%llu\n",
           (unsigned long long)g_shadowState.generation);
    printf("BLOCKINDEX_V2_SHADOW shadow_tip_height=%d\n", g_shadowState.shadowTipHeight);
    printf("BLOCKINDEX_V2_SHADOW legacy_tip_height=%d\n", g_shadowState.legacyTipHeightAtValidation);
    printf("BLOCKINDEX_V2_SHADOW compatibility=%d height_lag=%d\n",
           g_shadowState.compatibility, g_shadowState.heightLag);
    printf("BLOCKINDEX_V2_SHADOW sample_hash_checks=%llu sample_height_checks=%llu sample_mismatches=%llu\n",
           (unsigned long long)g_shadowState.sampleHashChecks,
           (unsigned long long)g_shadowState.sampleHeightChecks,
           (unsigned long long)g_shadowState.sampleMismatches);
    printf("BLOCKINDEX_V2_SHADOW structural_validation=%s\n",
           g_shadowState.structuralValidationOk ? "OK" : "FAIL");
    printf("BLOCKINDEX_V2_SHADOW tip_on_legacy_active_chain=%s\n",
           g_shadowState.tipOnLegacyActiveChain ? "YES" : "NO");
    printf("BLOCKINDEX_V2_SHADOW authoritative=NO\n");
    printf("BLOCKINDEX_V2_SHADOW status=%d\n", g_shadowState.status);

    const bool ready = g_shadowState.structuralValidationOk &&
        (g_shadowState.compatibility == BLOCK_INDEX_SHADOW_COMPAT_EXACT ||
         g_shadowState.compatibility == BLOCK_INDEX_SHADOW_COMPAT_ANCESTOR) &&
        g_shadowState.sampleMismatches == 0;

    if (ready)
        printf("BLOCKINDEX_V2_SHADOW status=READY\n");
    else
        printf("BLOCKINDEX_V2_SHADOW status=ERROR last_error=%s\n", g_shadowState.lastError.c_str());

    if (ready)
    {
        // A.8: retain exactly one reader on the validated, CURRENT-selected
        // generation so the diagnostic RPC never reopens the store per call.
        // Reader is read-only, authoritative=false, consumes no cs_main.
        std::string readerErr;
        bool retained = RetainBlockIndexV2ShadowReader(root, &readerErr);
        if (retained)
            printf("BLOCKINDEX_V2_SHADOW reader_retained=1 generation=%llu\n",
                   (unsigned long long)g_shadowReader->Generation());
        else
            printf("BLOCKINDEX_V2_SHADOW reader_retain_failed error=%s\n", readerErr.c_str());

        // A.9a.3c: retain the production staking navigator on the same validated
        // generation. Non-strict: if this fails, staking falls back to legacy
        // (which remains authoritative until A.10); the shadow state is unchanged.
        std::string navErr;
        bool navRetained = RetainBlockIndexStakingNavigator(root, &navErr);
        if (navRetained)
            printf("BLOCKINDEX_V2_SHADOW staking_navigator_retained=1 generation=%llu\n",
                   (unsigned long long)g_stakingNavigator->ColdGeneration());
        else
            printf("BLOCKINDEX_V2_SHADOW staking_navigator_retain_failed error=%s\n", navErr.c_str());
    }
    else
    {
        g_shadowReader.reset();
        g_stakingNavigator.reset();
    }

    g_shadowOpened = true;

    // In strict mode, a failed open must be surfaced as a startup error. We do
    // NOT abort here; the caller (init) inspects g_shadowState.status after this
    // returns and decides whether to fail startup.
    return g_shadowState;
}

BlockIndexV2ShadowState GetBlockIndexV2ShadowState()
{
    LOCK(cs_shadowState);
    return g_shadowState;
}

bool RevalidateBlockIndexV2ShadowTip()
{
    LOCK(cs_shadowState);
    if (!g_shadowState.enabled || g_shadowState.shadowTipHeight < 0)
        return false;
    LegacyBlockIndexShadowOracle legacy;
    return g_shadowRuntime.RevalidateTipOnActiveChain(&legacy);
}

bool RetainBlockIndexV2ShadowReader(const std::string& root, std::string* error)
{
    if (error) error->clear();
    LOCK(cs_shadowState);
    std::unique_ptr<BlockIndexV2Reader> reader(new BlockIndexV2Reader());
    BlockIndexV2ReaderOptions options;   // single shared 64 MiB bounded LRU default
    std::string readerErr;
    if (!reader->Open(root, options, &readerErr))
    {
        if (error) *error = readerErr;
        return false;
    }
    // Replace any prior retained reader: exactly one is retained at a time.
    g_shadowReader.swap(reader);
    return true;
}

const BlockIndexV2Reader* GetBlockIndexV2ShadowReader()
{
    LOCK(cs_shadowState);
    return g_shadowReader.get();
}

bool RetainBlockIndexStakingNavigator(const std::string& root, std::string* error)
{
    if (error)
        error->clear();
    LOCK(cs_shadowState);
    std::unique_ptr<ColdHotSeamNavigator> nav(new ColdHotSeamNavigator());
    BlockIndexV2ReaderOptions options;   // single shared 64 MiB bounded LRU default
    std::string navErr;
    if (!nav->Open(root, options, &navErr))
    {
        if (error)
            *error = navErr;
        return false;
    }
    g_stakingNavigator.swap(nav);
    return true;
}

const ColdHotSeamNavigator* GetBlockIndexStakingNavigator()
{
    LOCK(cs_shadowState);
    return g_stakingNavigator.get();
}

void ClearBlockIndexStakingNavigator()
{
    LOCK(cs_shadowState);
    g_stakingNavigator.reset();
}

BlockIndexV2ReaderCacheStats GetBlockIndexV2ShadowReaderCacheStats()
{
    LOCK(cs_shadowState);
    BlockIndexV2ReaderCacheStats stats;
    if (g_shadowReader && g_shadowReader->IsOpen())
        stats = g_shadowReader->CacheStats();
    return stats;
}