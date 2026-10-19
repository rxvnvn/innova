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


// M5: cross-TU V2 transaction-presence helper for the WALLET (defined in main.cpp).
// "Known" = the txid has a historical location in TX-LOC or an unspent output in
// Store3A, i.e. it has been seen on-chain. Used by the V2 wallet cutover for
// ReacceptWalletTransactions / RelayWalletTransaction instead of txdb.ContainsTx.
// Presence is a KNOWN/DUPLICATE gate only, never a spend/confirmation authority.
bool V2WalletTxidKnown(const uint256& hash);

// M6: cross-TU collateral spent-state (Store3A coin presence — the V2 spend
// authority). Returns 1 = authoritative (spent = !coin-present in the active-chain
// Store3A; *spent set), 0 = V2 not armed (caller should use its legacy path), and
// -1 = Store3A authority error (FAIL CLOSED: the caller must reject, never fall
// back to the legacy txindex). Used by collateral / collateralnode instead of
// txindex.vSpent.
int V2OutpointUnspent(const uint256& txid, uint32_t n, bool* unspent);


} // namespace s16
