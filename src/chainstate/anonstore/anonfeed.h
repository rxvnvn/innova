// ANONSTATE deterministic feed (M8.3 slice-b-feed, condition 2 of the mandatory
// cutover contract): derive the ANON-output / key-image mutations STRICTLY from a
// block's transactions, INDEPENDENT of wallet ownership, wallet scan, or pwalletMain.
// The daemon-side adapter (main.cpp) extracts FeedOutput/FeedInput records from a
// CBlock (see core.h: IsAnonInput/ExtractKeyImage/ExtractRingSize, IsAnonOutput);
// this module owns the deterministic, consensus-facing derivation and reconciliation
// of values, and is isolated-linkable (depends only on chainstate/anonstore types --
// NO core.h, NO wallet).
//
// Invariants that are the CALLER's contract (enforced here fail-closed):
//   * the block already passed consensus validation (CheckAnonInputs), so every ring
//     member's anon output exists, all ring members have an EQUAL value, and the
//     spend depth is satisfied. The derivation does NOT re-validate the ring crypto;
//     it faithfully records the canonical anon state a valid block implies.
//   * a ring member anon output is either in the committed (pre-block) store view or
//     was created earlier in the SAME block (intra-block visibility). If neither,
//     deriveAnonMutations FAILS CLOSED (error) -- it never fabricates a value.
#pragma once
#include "anonstore.h"
#include <array>
#include <vector>
#include <map>

namespace cs { namespace anon {

// One anon-bearing output record of a block (daemon adapter fills from CBlock txs).
struct FeedOutput {
    std::array<uint8_t,32> txid{};          // creating transaction hash
    uint32_t n = 0;                         // vout index
    std::array<uint8_t,33> pkCoin{};        // one-time key (scriptPubKey[3..36], core.h IsAnonOutput)
    int64_t nValue = 0;                     // txout.nValue
};
// One anon-spend (key-image) input record of a block.
struct FeedInput {
    std::array<uint8_t,32> txid{};          // spending transaction hash
    uint32_t inputNo = 0;                   // vin index
    std::array<uint8_t,33> keyImage{};      // 33B (prevout.hash[0..31]+prevout.n&0xFF)
    std::vector<std::array<uint8_t,33>> ring; // ring-member pubkeys, in order
};
struct BlockFeed {
    int32_t height = 0;
    // Preserves PER-TX sequence: the daemon adapter emits one FeedTx per block tx (in
    // block order). Intra-block visibility matches the sequential legacy semantics --
    // a later tx's inputs resolve against the committed store PLUS the anon outputs of
    // strictly EARLIER txs in this block, never its own or later txs' outputs.
    struct FeedTx {
        std::vector<FeedInput> inputs;
        std::vector<FeedOutput> outputs;
    };
    std::vector<FeedTx> txs;
};
// Read view over the COMMITTED (pre-block) anon-output state -- the daemon supplies a
// bridge to the authoritative store, the harness supplies a mock map.
struct CommittedAoView {
    virtual ~CommittedAoView() {}
    virtual bool getAo(const uint8_t key33[33], CAnonOutput* out) const = 0;
};
// Key-image value resolution wants the ring's common value: it is taken from ring[0]
// (all ring members are value-equal per the caller's consensus contract); every ring
// member is re-checked equal and any missing/unequal member fails closed.
//
// Produces the ordered `Mutation` vector that a block connect must apply (putAo for
// created anon outputs, putKi for spent key images) and that a disconnect must revert.
bool deriveAnonMutations(const BlockFeed& feed, const CommittedAoView& committed,
                         std::vector<Mutation>* out, ChainError* e);

}} // namespace cs::anon