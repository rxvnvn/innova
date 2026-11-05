// ---------------------------------------------------------------------------
// ATOMIC-REORG (S1-d): READ-ONLY FORK-STATE BRANCH OVERLAY (the V-phase view).
//
// Owner-approved atomic-reorg protocol (v2-legacy-elimination/DESIGN.md
// ATOMIC-REORG-OVERLAY; ADR-0002 §3-reorg through the overlay alternative):
// the ENTIRE candidate branch is consensus-validated against the
// POST-DISCONNECT (fork) UTXO state BEFORE any durable change. CommitStore3A
// exposes that state read-only through historic undo records (S1-c `peekUndo`);
// the disconnected branch's coins must be hidden and the undo-restored coins
// must be re-shown, while everything else resolves from the authoritative
// Store3A as-is.
//
// ReorgBranchView IS the fork-state representation. It never touches the
// store: the pointer is const and every mutation target is operation-local
// RAM. main.cpp arms it for a single V-phase through a cs_main-serialised
// pointer and consults it from the ALREADY-EXISTING coin sources (the
// s16_V2InputView adapter, the M1 existence gate, the BIP30 duplicate-txid
// gate and s16_V2PrevBlockTime), so no consensus algorithm is duplicated.
//
// Representation (bounded RAM; DESIGN.md §5: ~200 B per touched coin, so a
// 2101-depth operation stays far below 20 MB):
//   added   : coins present at the fork state that Store3A does NOT currently
//             show as present (restored by undo, or created by earlier
//             already-validated branch blocks);
//   removed : coins created by the disconnected branch (absent at the fork
//             state) that Store3A still holds because the disconnect has not
//             been applied durably;
//   spent   : coins spent earlier in the branch whose coins still resolve from
//             the store — a cross-block double-spend guard the store cannot
//             answer yet.
// Resolution order: added -> removed (absent) -> spent (absent) -> store.
// ---------------------------------------------------------------------------
#pragma once
#include "chainstate.h"
#include "store3a/incstore.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string.h>

namespace cs {

class ReorgBranchView
{
public:
    const store3a::CommitStore3A* store;      // committed old-tip state (const)
    int32_t forkHeight;                       // -1 = not armed
    std::map<OutPoint, Coin> added;
    std::map<OutPoint, char>     removed;     // char value: ordered map for deterministic iteration
    std::set<OutPoint>           spent;
    std::map<int32_t, int64_t>   branchTime;  // CONNECT-branch heights only (never disconnect entries)

    ReorgBranchView() : store(NULL), forkHeight(-1) {}
    ReorgBranchView(const store3a::CommitStore3A* s, int32_t fork)
        : store(s), forkHeight(fork) {}
    bool armed() const { return store != NULL && forkHeight >= 0; }
    size_t entryCount() const { return added.size() + removed.size() + spent.size(); }

    // Resolve one coin against the fork-state view.
    // Returns 1 = present (`out` set), 0 = absent at the fork state,
    // -1 = Store3A authority read error underneath (fail closed; callers reject).
    int observed(const OutPoint& k, Coin* out, ChainError* e = NULL) const
    {
        std::map<OutPoint, Coin>::const_iterator ia = added.find(k);
        if (ia != added.end())
        {
            if (out) *out = ia->second;
            return 1;
        }
        if (removed.count(k) || spent.count(k))
            return 0;
        bool found = false; ChainError se;
        if (!store->get(k, out, &found, &se)) return -1;   // authority error: fail closed
        (void)se;
        return found ? 1 : 0;
    }

    // BIP30 duplicate-txid existence against the fork-state view.
    // Overlay-true if an `added` coin of this txid exists (created by an
    // already-validated branch block). Otherwise the store's answer stands
    // unless a coin OF THIS TXID is hidden: a txid created by the disconnected
    // (above-fork) chain enumerates ALL of its outpoints in `removed` — the
    // delta observer records full per-coin before/after — so any hidden key of
    // the txid means the whole txid is absent at the fork state.
    bool unspentTxidKnown(const Hash& txid, bool* out, ChainError* e) const
    {
        for (std::map<OutPoint, Coin>::const_iterator it = added.begin();
             it != added.end(); ++it)
        {
            if (memcmp(it->first.txid.data(), txid.data(), 32) == 0)
            {
                if (out) *out = true;
                return true;
            }
        }
        bool hidden = false;
        for (std::map<OutPoint, char>::const_iterator it = removed.begin();
             it != removed.end() && !hidden; ++it)
            if (memcmp(it->first.txid.data(), txid.data(), 32) == 0) hidden = true;
        if (!hidden)
            for (std::set<OutPoint>::const_iterator it = spent.begin();
                 it != spent.end() && !hidden; ++it)
                if (memcmp(it->txid.data(), txid.data(), 32) == 0) hidden = true;
        bool storeHas = false;
        if (!store->existsUnspentTxid(txid, &storeHas, e)) return false;
        if (out) *out = (storeHas && !hidden);
        return true;
    }

    // Header time of a branch-created coin's creating block. Returns false when
    // the height is at/below the fork (the overlay is NOT authoritative there:
    // the fork prefix IS the committed active-chain prefix and the caller's
    // normal by-value resolution covers it) or when the height is unknown.
    bool prevBlockTimeFor(int32_t h, int64_t* outTime) const
    {
        if (h <= forkHeight) return false;
        std::map<int32_t, int64_t>::const_iterator it = branchTime.find(h);
        if (it == branchTime.end()) return false;
        if (outTime) *outTime = it->second;
        return true;
    }

    // Apply ONE disconnect step (tip downward, exactly the order the disconnect
    // list is walked): the undo record reverses that block's own connect.
    //   op.beforePresent    -> the coin existed before the block: the fork-state
    //                          truth is `before` (restore; drops an earlier
    //                          tracking state for the outpoint).
    //   else op.afterPresent -> the block CREATED the coin: it is absent at the
    //                          fork state (erases any earlier restored copy,
    //                          which cannot survive a re-creation below it).
    void applyUndo(const store3a::UndoRecord& rec)
    {
        for (std::vector<Op>::const_iterator it = rec.ops.begin();
             it != rec.ops.end(); ++it)
        {
            if (it->beforePresent)
            {
                added.erase(it->op);
                removed.erase(it->op);
                spent.erase(it->op);
                added[it->op] = it->before;
            }
            else if (it->afterPresent)
            {
                added.erase(it->op);
                removed[it->op] = 1;
            }
            // both-absent records do not exist at the fork state or the tip.
        }
    }

    // A coin just spent by an ALREADY-VALIDATED branch block (cross-block: the
    // per-block V2 input view guards only in-block spends). The coin may come
    // from `added` (restored) or from the store, untouched by the branch so
    // far — either way the fork-state truth for LATER branch blocks is "spent".
    void markSpent(const OutPoint& k)
    {
        added.erase(k);
        removed.erase(k);
        spent.insert(k);
    }

    // A coin created by an ALREADY-VALIDATED branch block.
    void addCreated(const OutPoint& k, const Coin& c)
    {
        removed.erase(k);
        spent.erase(k);
        added[k] = c;
    }
};

} // namespace cs
