#include "observe.h"
#include <algorithm>

namespace cs {

namespace {

// The one authoritative way a created coin is materialised from a real block
// output. Used for BOTH the create op and the intra-block spend's before, so a
// coin created and spent within the same block has byte-identical before/after
// and store3a undoes it into the exact pre-block (absent) state.
Coin coinFor(const ObservedTx& tx, uint32_t j, int32_t height) {
    Coin c;
    if (j < tx.vout.size()) {
        c.value  = tx.vout[j].value;
        c.script = tx.vout[j].script;
    }
    c.height     = height;
    c.nTime      = tx.nTime;   // S2.0-M2-c: creator tx nTime -> restored ppcoin timestamp rule
    c.fCoinBase  = tx.coinbase;
    c.fCoinStake = tx.coinstake;
    return c;
}

// Block-local txid -> index, for intra-block spend resolution.
std::vector<std::pair<Hash, size_t> > txIndex(const ObservedBlock& b) {
    std::vector<std::pair<Hash, size_t> > idx;
    idx.reserve(b.txs.size());
    for (size_t i = 0; i < b.txs.size(); ++i) idx.push_back(std::make_pair(b.txs[i].txid, i));
    std::sort(idx.begin(), idx.end());
    return idx;
}

bool findTx(const std::vector<std::pair<Hash, size_t> >& idx, const Hash& txid, size_t* out) {
    std::pair<Hash, size_t> probe(txid, 0);
    typename std::vector<std::pair<Hash, size_t> >::const_iterator it =
        std::lower_bound(idx.begin(), idx.end(), probe);
    if (it == idx.end() || it->first != txid) return false;
    *out = it->second;
    return true;
}

} // namespace

bool observeDelta(const ObservedBlock& b, const GetPrevCoinFn& getPrevCoin,
                  Delta* out, ChainError* e) {
    clearErr(e);
    if (out) out->ops.clear();
    std::vector<std::pair<Hash, size_t> > idx = txIndex(b);
    Delta delta;

    for (size_t ti = 0; ti < b.txs.size(); ++ti) {
        const ObservedTx& tx = b.txs[ti];

        // spends (transparent prevouts only; anon-input prevouts are consumed
        // by the nullifier/CAnonOutput store, not this coin set)
        if (!tx.coinbase) {
            for (size_t i = 0; i < tx.vin.size(); ++i) {
                const OutPoint& prev = tx.vin[i];
                Coin before;
                bool have = false;
                size_t srcIdx = 0;
                if (findTx(idx, prev.txid, &srcIdx)) {
                    // intra-block: reuse THE SAME create-coin for consistency
                    const ObservedTx& src = b.txs[srcIdx];
                    if (prev.n < src.vout.size()) {
                        before = coinFor(src, prev.n, b.height);
                        have = true;
                    }
                    if (!have) {
                        setErr(e, ErrCode::MISSING_INPUT,
                               "intra-block prevout index out of range");
                        return false;
                    }
                } else if (getPrevCoin) {
                    if (!getPrevCoin(prev, &before)) {
                        setErr(e, ErrCode::MISSING_INPUT,
                               "external prevout not observable in mirror");
                        return false;
                    }
                    have = true;
                }
                if (!have) {
                    setErr(e, ErrCode::MISSING_INPUT, "prevout not observable");
                    return false;
                }

                Op o;
                o.op            = prev;
                o.beforePresent = true;
                o.before        = before;
                o.afterPresent  = false;
                delta.ops.push_back(o);
            }
        }

        // creates: every real output of every tx (incl. burn/unspendable, which
        // are ordinary (spendable-in-future) coins in the CTxIndex model)
        for (uint32_t j = 0; j < tx.vout.size(); ++j) {
            OutPoint key;
            key.txid = tx.txid; key.n = j;
            Op o;
            o.op            = key;
            o.beforePresent = false;
            o.afterPresent  = true;
            o.after         = coinFor(tx, j, b.height);
            delta.ops.push_back(o);
        }
    }

    if (out) *out = delta;
    return true;
}

void authoritySpendDiff(const Delta& d, const ObservedAuthority& auth,
                        std::vector<OutPoint>* dsOnly,
                        std::vector<OutPoint>* asOnly) {
    std::vector<OutPoint> ds, as;
    for (size_t i = 0; i < d.ops.size(); ++i)
        if (d.ops[i].afterPresent == false && d.ops[i].beforePresent == true)
            ds.push_back(d.ops[i].op);
    as = auth.spent;
    std::sort(ds.begin(), ds.end());
    std::sort(as.begin(), as.end());
    if (dsOnly) std::set_difference(ds.begin(), ds.end(), as.begin(), as.end(),
                                    std::back_inserter(*dsOnly));
    if (asOnly) std::set_difference(as.begin(), as.end(), ds.begin(), ds.end(),
                                    std::back_inserter(*asOnly));
}

bool authoritySpendsMatch(const Delta& d, const ObservedAuthority& auth,
                          std::string* mismatch) {
    std::vector<OutPoint> dsOnly, asOnly;
    authoritySpendDiff(d, auth, &dsOnly, &asOnly);
    if (!dsOnly.empty() || !asOnly.empty()) {
        if (mismatch) {
            if (!dsOnly.empty())
                *mismatch = "delta-spend not in authority (surplus): first absent in authority";
            else
                *mismatch = "authority-spend missing from delta (dropped)";
        }
        return false;
    }
    return true;
}

} // namespace cs