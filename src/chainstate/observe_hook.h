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

    uint64_t blocks = 0;                   // blocks handled by the hook
    uint64_t published = 0;                // successful observeDelta+publish
    uint64_t fails = 0;                    // hook failures (block still connects)
    uint64_t mismatch = 0;                 // authority cross-check mismatches
    std::string lastOpError;               // last observeDelta/publish error
    std::string lastAuthorityMismatch;     // last authority-gate divergence
    uint64_t lastOps = 0;                  // ops in the last published Delta
    int32_t lastHeight = -1;               // last block height handled
};

extern ChainObserveState g_chainObserve;

} // namespace s16