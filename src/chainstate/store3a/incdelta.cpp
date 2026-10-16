// Stage 3A - reader-based delta builder implementation.
#include "incdelta.h"

namespace cs { namespace store3a {

bool buildDeltaI(const UtxoReader& base, const Block& b, int32_t maturity, Delta* out, ChainError* e) {
    clearErr(e);
    if (out) out->ops.clear();
    BlockView work(&base);
    Delta delta;

    for (const Tx& tx : b.txs) {
        std::vector<Key> ks;
        if (!work.keysWithTxid(tx.txid.data(), &ks)) { setErr(e, ErrCode::INTERNAL, "txid scan failed"); return false; }
        if (!ks.empty()) { setErr(e, ErrCode::DUP_OUTPOINT, "BIP30: unspent output of same txid exists"); return false; }

        int64_t valueIn = 0, valueOut = 0;
        if (!tx.coinbase) {
            for (const TxIn& in : tx.vin) {
                Key k = in.prev.keyBytes();
                Coin prev;
                if (!work.get(k, &prev)) { setErr(e, ErrCode::MISSING_INPUT, "prevout not found (incl. intra-block)"); return false; }
                if (prev.fCoinBase || prev.fCoinStake) {
                    int64_t depth = (int64_t)b.height - (int64_t)prev.height;
                    if (depth < (int64_t)maturity) { setErr(e, ErrCode::IMMATURE, "coinbase/coinstake spend below maturity"); return false; }
                }
                if (!moneyRange(prev.value)) { setErr(e, ErrCode::VALUE_RANGE, "input out of range"); return false; }
                valueIn += prev.value;
                if (!moneyRange(valueIn)) { setErr(e, ErrCode::VALUE_OVERFLOW, "valueIn overflow"); return false; }
                Op o; o.op = in.prev; o.beforePresent = true; o.before = prev; o.afterPresent = false;
                delta.ops.push_back(o);
                work.spend(k);
            }
        }
        for (uint32_t j = 0; j < tx.vout.size(); ++j) {
            const TxOut& t = tx.vout[j];
            if (!moneyRange(t.value)) { setErr(e, ErrCode::VALUE_RANGE, "output out of range"); return false; }
            valueOut += t.value;
            if (!moneyRange(valueOut)) { setErr(e, ErrCode::VALUE_OVERFLOW, "valueOut overflow"); return false; }
            OutPoint key; key.txid = tx.txid; key.n = j;
            Key kk = key.keyBytes();
            Coin exist;
            if (work.get(kk, &exist)) { setErr(e, ErrCode::DUP_OUTPOINT, "BIP30: exact outpoint exists"); return false; }
            Coin c; c.value = t.value; c.script = t.script; c.height = b.height;
            c.fCoinBase = tx.coinbase; c.fCoinStake = tx.coinstake;
            Op o; o.op = key; o.beforePresent = false; o.afterPresent = true; o.after = c;
            delta.ops.push_back(o);
            work.create(kk, c);
        }
        if (!tx.coinbase && !tx.coinstake && valueOut > valueIn) { setErr(e, ErrCode::VALUE_RANGE, "non-coinbase spends more than inputs"); return false; }
    }
    if (out) *out = delta;
    return true;
}

}} // namespace cs::store3a
