#ifndef INNOVA_BLOCKINDEX_SHADOW_RUNTIME_H
#define INNOVA_BLOCKINDEX_SHADOW_RUNTIME_H

#include "fixed_blockindex_store.h"

#include <stdint.h>
#include <string>

// Phase A.7: boot-time Block Index V2 SHADOW open + legacy differential.
//
// BLOCK INDEX V2 IS SHADOW ONLY. Legacy mapBlockIndex / pindexBest remains the
// sole authoritative state. The shadow NEVER influences chain selection,
// validation, block/transaction acceptance, staking, finality, CN logic, P2P,
// getblocks/getheaders, wallet, txindex, UTXO, or consensus.
//
// READ-ONLY WORDING: Block Index V2 is runtime/semantic read-only (no logical
// block-index records/mappings are modified). The bundled LevelDB hashindex open
// is logically read-only but may refresh LOG/LOCK metadata inside hashindex/; we
// do NOT claim filesystem-byte-read-only generation. No publish/select/build
// happens from the shadow consumer.

// Compatibility classification (Section 9).
enum BlockIndexShadowCompatibility
{
    BLOCK_INDEX_SHADOW_COMPAT_NOT_CONFIGURED = 0,
    BLOCK_INDEX_SHADOW_COMPAT_NOT_PUBLISHED = 1,
    BLOCK_INDEX_SHADOW_COMPAT_CORRUPT = 2,
    BLOCK_INDEX_SHADOW_COMPAT_EXACT = 3,
    BLOCK_INDEX_SHADOW_COMPAT_ANCESTOR = 4,
    BLOCK_INDEX_SHADOW_COMPAT_AHEAD = 5,
    BLOCK_INDEX_SHADOW_COMPAT_DIVERGED = 6,
    BLOCK_INDEX_SHADOW_COMPAT_SAMPLE_MISMATCH = 7,
    BLOCK_INDEX_SHADOW_COMPAT_ERROR = 8,
};

// Legacy oracle: the authoritative legacy chain, accessed through this interface
// so the shadow runtime is deterministic and unit-testable without a running
// daemon. Implementations (e.g. the daemon, or a test fixture) MUST return
// by-value records with no raw CBlockIndex* escape. Callers hold whatever lock
// the implementation requires (the daemon uses cs_main).
class BlockIndexShadowLegacyOracle
{
public:
    virtual ~BlockIndexShadowLegacyOracle() {}

    // Current authoritative legacy active tip height, or -1 if none.
    virtual int GetLegacyTipHeight() = 0;
    // Fill the legacy active-chain record at `height`. Returns false if no
    // active block exists at that height.
    virtual bool GetLegacyActiveRecord(int height, BlockIndexRecord* out) = 0;
    // Fill a legacy record for any hash known to the legacy index. Returns false
    // if the hash is unknown (side/orphan behavior still exposes the record).
    virtual bool GetLegacyRecordByHash(const uint256& hash, BlockIndexRecord* out) = 0;
};

// Runtime shadow state (Section 8). All values; no wallet data; no secrets.
struct BlockIndexV2ShadowState
{
    bool enabled;
    std::string configuredRoot;
    uint64_t generation;
    int status;                       // BlockIndexLifecycleStatus or 0
    int compatibility;                // BlockIndexShadowCompatibility
    int32_t shadowTipHeight;
    uint256 shadowTipHash;
    int legacyTipHeightAtValidation;
    int heightLag;
    bool structuralValidationOk;
    bool tipOnLegacyActiveChain;
    uint64_t sampleHashChecks;
    uint64_t sampleHeightChecks;
    uint64_t sampleMismatches;
    int64_t openDurationMs;
    int64_t validationDurationMs;
    std::string lastError;
    bool authoritative;               // always false in A.7

    BlockIndexV2ShadowState()
        : enabled(false),
          generation(0),
          status(0),
          compatibility(BLOCK_INDEX_SHADOW_COMPAT_NOT_CONFIGURED),
          shadowTipHeight(-1),
          shadowTipHash(0),
          legacyTipHeightAtValidation(-1),
          heightLag(0),
          structuralValidationOk(false),
          tipOnLegacyActiveChain(false),
          sampleHashChecks(0),
          sampleHeightChecks(0),
          sampleMismatches(0),
          openDurationMs(0),
          validationDurationMs(0),
          authoritative(false)
    {
    }
};

// Configuration for a shadow open.
struct BlockIndexShadowOpenConfig
{
    std::string root;
    bool strict;              // false = non-fatal (default)
    uint64_t hashSamples;     // bounded deterministic hash samples (default 1024)
    uint64_t heightSamples;   // bounded deterministic active-height samples (default 1024)
    uint64_t randomSeed;      // deterministic PRNG seed (default from generation provenance)

    BlockIndexShadowOpenConfig()
        : strict(false),
          hashSamples(1024),
          heightSamples(1024),
          randomSeed(0x5EED5EED5EED5EE5ULL)
    {
    }
};

// Open + validate + classify + bounded differential against a legacy oracle.
// Pure logic, callable in unit tests and from the daemon startup path. Does NOT
// publish/select/build or modify any generation content.
class BlockIndexV2ShadowRuntime
{
public:
    BlockIndexV2ShadowRuntime();

    // Opens the CURRENT-selected generation at cfg.root, structurally validates
    // it, classifies compatibility against the legacy oracle, and runs the
    // bounded deterministic differential. On any failure in non-strict mode the
    // state is left ERROR/DISABLED and `false` NOT propagated as fatal to the
    // caller (the caller decides fatal vs continue). Returns the state.
    const BlockIndexV2ShadowState& OpenShadow(const BlockIndexShadowOpenConfig& cfg,
                                              BlockIndexShadowLegacyOracle* legacy,
                                              std::string* error);

    // Cheap diagnostic revalidation (Section 42): re-check that the shadow tip
    // is still an ancestor of the current legacy tip. Read-only, no exhaustive
    // sample. Returns true if still on the legacy active chain.
    bool RevalidateTipOnActiveChain(BlockIndexShadowLegacyOracle* legacy) const;

    const BlockIndexV2ShadowState& GetState() const { return state; }

private:
    BlockIndexV2ShadowState state;

    void RunBoundedDifferential(BlockIndexShadowLegacyOracle* legacy,
                                const BlockIndexShadowOpenConfig& cfg);
    void RunBoundaryChecks(BlockIndexShadowLegacyOracle* legacy);
};

#endif