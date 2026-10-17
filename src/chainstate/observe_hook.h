// S1.6-2 -- guarded READ-ONLY observation hook installed at ConnectBlock's
// persist point. It lets the real ConnectBlock authority drive the
// observation adapter (observeDelta + authority gate) against a live store3a
// mirror during an actual block connect, WITHOUT changing ConnectBlock's
// decision: on any hook failure the block still connects exactly as before and
// the failure is recorded in the counters below for the pilot to assert on.
//
// This header intentionally carries NO main.h transaction types so it can be
// included both by main.cpp (where the heavy CTransaction->ObservedTx bridge
// and the hook body live) and by the real-block pilot test.
#pragma once
#include "chainstate/store3a/incstore.h"
#include <string>
#include <stdint.h>

namespace s16 {

struct ChainObserveState {
    bool enabled = false;                  // pilot sets this to observe real blocks
    cs::store3a::CommitStore3A* store = nullptr;  // pilot-owned mirror (must be open())
    bool checkAuthority = true;            // cross-check Delta spends vs mapQueuedChanges vSpent
    bool hardFail = false;                 // S1.9: when armed as V2 AUTHORITATIVE chainstate,
                                           // a gate failure REFUSES the block (ConnectBlock returns
                                           // error) instead of the read-only "block still connects".

    uint64_t blocks = 0;                   // blocks handled by the hook
    uint64_t published = 0;                // successful observeDelta+publish
    uint64_t fails = 0;                    // hook failures (block still connects)
    uint64_t mismatch = 0;                 // authority cross-check mismatches
    bool needsRecovery = false;            // ERROR/NEEDS_RECOVERY: a hard divergence (or
                                           // publish/open/disconnect/position error) was seen;
                                           // no further Delta is published until explicitly reset
    std::string lastOpError;               // last observeDelta/publish error
    std::string lastAuthorityMismatch;     // last authority-gate divergence
    uint64_t lastOps = 0;                  // ops in the last published Delta
    int32_t lastHeight = -1;               // last block height handled
};

extern ChainObserveState g_chainObserve;

// ---------------------------------------------------------------------------
// S2.0 BI-V2 AUTHORITATIVE STARTUP: legacy-index-open guard.
//
// In V2 store mode (-s16openstore=<dir>), the legacy LevelDB block/tx index
// (GetDataDir()/txleveldb) MUST NOT be opened even for reading in the startup
// path: the block index comes exclusively from the authoritative Block Index
// V2. These flags let the single legacy LevelDB open choke point
// (init_blockindex, txdb-leveldb.cpp) refuse such an open FAIL-CLOSED while a
// narrow, explicitly-authorized probe scope (the one consensus-critical
// non-index read, the persisted sync-checkpoint) is allowed through. Non-V2
// startup never arms the guard, so its behavior is untouched.
// ---------------------------------------------------------------------------
extern bool g_V2LegacyIndexGuardArmed;      // true while the V2 store startup must refuse legacy index opens
extern int  g_V2LegacyIndexProbeDepth;      // >0 inside an authorized non-index probe (sync-checkpoint)

inline bool V2LegacyIndexGuardActive() { return g_V2LegacyIndexGuardArmed; }
inline bool V2LegacyIndexProbeActive()
{
    return g_V2LegacyIndexGuardArmed && g_V2LegacyIndexProbeDepth > 0;
}

// RAII: marks the enclosing scope as the explicitly-justified, consensus-critical
// non-index legacy read (the persisted synced-checkpoint boot probe). Only this
// scope may open the legacy LevelDB in V2 store mode.
class V2LegacyIndexProbeScope
{
public:
    V2LegacyIndexProbeScope() { ++g_V2LegacyIndexProbeDepth; }
    ~V2LegacyIndexProbeScope() { --g_V2LegacyIndexProbeDepth; }
};

// Arms the guard when -s16openstore is requested. Call ONCE at the start of the
// V2 store startup path (before any legacy LevelDB open could occur). No-op in
// non-V2 session (guard stays disarmed -> legacy behavior byte-for-byte).
void ArmV2LegacyIndexGuardIfRequested();

} // namespace s16
