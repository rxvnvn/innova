// S1.6-2 -- AUTHORITATIVE UTXO OBSERVATION ADAPTER (neutral projection).
//
// Bridges the REAL ConnectBlock authority to the chainstate Delta model WITHOUT
// re-implementing consensus. ConnectBlock has already proved the block valid
// (maturity, BIP30, value-in/out, sigops, anon-nullifier, HREG, burn). This
// adapter only OBSERVES the block's real coin transitions and maps them to a
// cs::Delta that store3a can publish/undo.
//
// What is observed (authoritative, in-scope):
//   * creates : every vout of every real block tx  -> after=coin(created)
//   * spends  : every TRANSPARENT vin prevout      -> before=observed pre-coin
//     - an intra-block prevout (prev.txid is a tx in this block) reuses THE SAME
//       create-coin so connect/undo net intra-block churn identically to the
//       reference model (cs::buildDelta).
//     - an external prevout is fetched from the authoritative mirror via
//       getPrevCoin (the store's pre-block coin). Absence is FAIL-CLOSED: a
//       spend of a coin the mirror does not yet hold cannot be observed, so no
//       partial Delta is produced.
//
// Consensus-bounded scope (NOT observed here; do not mix into ordinary UTXO):
//   anonymous ring-nullifier inputs/outputs, HREG registry state, the burn
//   ledger tally. Their TRANSPARENT vin/vout still flow through this path as
//   ordinary coins (which is why they are projected here like any other tx).
//   Anon-input prevouts are EXCLUDED from the spend enumeration because
//   ConnectBlock consumes them via the CAnonOutput/nullifier store, never via
//   CTxIndex::vSpent -- confirmed at main.cpp:5717-5878 (ConnectInputs skips
//   IsAnonInput vins before the "Mark outpoints as spent" step).
//
// The mandate's equivalence gate ("do not trust vin/vout enumeration until it
// equals the authority") is enforced by authoritySpendsMatch(): it compares the
// adapter's spend-outpoint set against the authoritative spent set recorded in
// ConnectBlock's mapQueuedChanges/CTxIndex::vSpent for the same block.
#pragma once
#include "chainstate.h"
#include <functional>
#include <string>
#include <vector>

namespace cs {

// A single block transaction as observed by the real ConnectBlock path. This is
// a neutral projection of the in-scope authority structures (vtx); nothing here
// re-derives a consensus rule.
struct ObservedTx {
    Hash txid;
    bool coinbase  = false;   // tx.IsCoinBase()
    bool coinstake = false;   // tx.IsCoinStake()
    std::vector<OutPoint> vin;   // TRANSPARENT (non-anon-input) prevouts, in order
    std::vector<TxOut>    vout;  // the block's real outputs (value + script bytes)
};

struct ObservedBlock {
    int32_t height = -1;
    std::vector<ObservedTx> txs;
};

// Read the authoritative pre-block coin for an EXTERNAL spent prevout (one not
// created in this block). Return false when the coin is not observable; the
// adapter then FAILS CLOSED (no partial Delta).
typedef std::function<bool(const OutPoint&, Coin*)> GetPrevCoinFn;

// Authoritative spent-outpoint set for the block, recorded by ConnectBlock at
// its persist point: every (txid, output-index) whose CTxIndex::vSpent[index]
// is NON-NULL (observed from mapQueuedChanges). This is the authority that
// vin/vout enumeration must equal.
struct ObservedAuthority {
    std::vector<OutPoint> spent;   // spent outpoints in mapQueuedChanges/vSpent order
};

// Build a Delta by OBSERVING the block's real coin transitions. Op ordering
// matches cs::buildDelta (per tx: spends then creates, block order) so store3a
// publish() and disconnect() net intra-block churn identically. FAILS CLOSED on
// any external prevout whose pre-coin cannot be observed.
bool observeDelta(const ObservedBlock& b, const GetPrevCoinFn& getPrevCoin,
                  Delta* out, ChainError* e);

// Equivalence gate. Returns true iff the Delta's spend-outpoint SET equals the
// authoritative spent set (ObservedAuthority::spent) exactly (same members, no
// missing and no surplus). On mismatch, `mismatch` names the first divergence.
// Provides the mandate's proof that enumeration == authority for the block's
// spend transitions.
bool authoritySpendsMatch(const Delta& d, const ObservedAuthority& auth,
                          std::string* mismatch);

} // namespace cs