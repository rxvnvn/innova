#ifndef INNOVA_BLOCKINDEX_SHADOW_STARTUP_H
#define INNOVA_BLOCKINDEX_SHADOW_STARTUP_H

#include "blockindex_shadow_runtime.h"
#include "blockindex_v2_reader.h"

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

// A.8 retained-reader integration. After a READY open, the startup layer retains
// exactly one read-only BlockIndexV2Reader bound to the validated generation, so
// the diagnostic RPC does NOT reopen the store per call. The reader is never
// authoritative (authoritative stays false) and never consumes cs_main.
//
// Retain the single shadow reader at <root> (opens the CURRENT-selected
// generation). Replaces any previously retained reader. Returns true and a
// usable reader on success; false and unchanged on failure.
bool RetainBlockIndexV2ShadowReader(const std::string& root, std::string* error);

// Accessor for the single retained reader, or NULL when not retained (disabled /
// not READY). The returned object is bound to the generation it opened and does
// not auto-switch if CURRENT changes.
const BlockIndexV2Reader* GetBlockIndexV2ShadowReader();

// Cache statistics of the retained reader (zeroed when none retained). Used by
// the diagnostic RPC to report bounded cache state without reopening.
BlockIndexV2ReaderCacheStats GetBlockIndexV2ShadowReaderCacheStats();

#endif