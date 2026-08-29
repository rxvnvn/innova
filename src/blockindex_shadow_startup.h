#ifndef INNOVA_BLOCKINDEX_SHADOW_STARTUP_H
#define INNOVA_BLOCKINDEX_SHADOW_STARTUP_H

#include "blockindex_shadow_runtime.h"

#include <string>

// Phase A.7 startup integration: optional read-only shadow open from innovad
// startup. Opt-in via -blockindexv2shadow=<root>. NON-STRICT by default:
// any shadow failure logs and leaves the shadow ERROR/DISABLED but does NOT
// fail legacy startup. Legacy remains authoritative.

// Build a BlockIndexRecord snapshot from a legacy CBlockIndex (by value, no
// pointer escape). Caller must hold cs_main.
BlockIndexRecord BlockIndexRecordFromIndex(const CBlockIndex* pindex);

// Legacy oracle adapter that answers from the authoritative in-memory legacy
// chain under cs_main.
class LegacyBlockIndexShadowOracle : public BlockIndexShadowLegacyOracle
{
public:
    virtual int GetLegacyTipHeight();
    virtual bool GetLegacyActiveRecord(int height, BlockIndexRecord* out);
    virtual bool GetLegacyRecordByHash(const uint256& hash, BlockIndexRecord* out);
};

// Open the shadow (once) during startup after the legacy block index is loaded.
// Reads -blockindexv2shadow and -blockindexv2shadowstrict. Never throws/aborts;
// returns the runtime state. Thread-safe against concurrent RPC reads of the
// same global shadow state. Prints explicit BLOCKINDEX_V2_SHADOW diagnostics.
const BlockIndexV2ShadowState& TryOpenBlockIndexV2Shadow();

// Accessor for the diagnostic RPC (read-only re-check, thread-safe).
BlockIndexV2ShadowState GetBlockIndexV2ShadowState();
// Cheap read-only revalidation: returns whether the shadow tip is still on the
// current legacy active chain.
bool RevalidateBlockIndexV2ShadowTip();

#endif