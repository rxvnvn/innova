// Isolated chainstate vertical slice - delta application + block->delta builder.
#include "chainstate.h"

namespace cs {

const char* errName(ErrCode c) {
    switch (c) {
        case ErrCode::OK: return "OK";
        case ErrCode::DUP_OUTPOINT: return "DUP_OUTPOINT";
        case ErrCode::MISSING_INPUT: return "MISSING_INPUT";
        case ErrCode::IMMATURE: return "IMMATURE";
        case ErrCode::NEGATIVE_SPEND: return "NEGATIVE_SPEND";
        case ErrCode::VALUE_RANGE: return "VALUE_RANGE";
        case ErrCode::VALUE_OVERFLOW: return "VALUE_OVERFLOW";
        case ErrCode::BAD_SERIALIZE: return "BAD_SERIALIZE";
        case ErrCode::IO: return "IO";
        case ErrCode::FS: return "FS";
        case ErrCode::NO_SPACE: return "NO_SPACE";
        case ErrCode::CORRUPT: return "CORRUPT";
        case ErrCode::UNKNOWN_VERSION: return "UNKNOWN_VERSION";
        case ErrCode::LOCKED: return "LOCKED";
        case ErrCode::ALREADY_OPEN: return "ALREADY_OPEN";
        case ErrCode::NOT_OPEN: return "NOT_OPEN";
        case ErrCode::NOT_FOUND: return "NOT_FOUND";
        case ErrCode::INTERNAL: return "INTERNAL";
    }
    return "?";
}

void applyDelta(UtxoState& s, const Delta& d) {
    for (const Op& o : d.ops) {
        if (o.afterPresent) s.put(o.op, o.after);
        else s.erase(o.op);
    }
}

void revertDelta(UtxoState& s, const Delta& d) {
    for (size_t i = d.ops.size(); i-- > 0; ) {
        const Op& o = d.ops[i];
        if (o.beforePresent) s.put(o.op, o.before);
        else s.erase(o.op);
    }
}

bool buildDelta(const UtxoState& base, const Block& b, int32_t maturity,
                Delta* out, UtxoState* newState, ChainError* e) {
    clearErr(e);
    if (out) out->ops.clear();
    UtxoState work = base;               // working copy: intra-block visibility
    Delta delta;

    for (const Tx& tx : b.txs) {
        // BIP30 (legacy-consistent): reject a tx if ANY unspent output of the same
        // txid already exists (key (txid,n)), including output numbers below.
        if (work.existsUnspentTxid(tx.txid)) {
            setErr(e, ErrCode::DUP_OUTPOINT, "BIP30: unspent output of same txid exists");
            return false;
        }

        int64_t valueIn = 0, valueOut = 0;

        if (!tx.coinbase) {
            for (const TxIn& in : tx.vin) {
                Coin prev;
                if (!work.get(in.prev, &prev)) {
                    setErr(e, ErrCode::MISSING_INPUT, "prevout not found (incl. intra-block)");
                    return false;
                }
                if (prev.fCoinBase || prev.fCoinStake) {
                    int64_t depth = (int64_t)b.height - (int64_t)prev.height;
                    if (depth < (int64_t)maturity) {
                        setErr(e, ErrCode::IMMATURE, "coinbase/coinstake spend below maturity");
                        return false;
                    }
                }
                if (!moneyRange(prev.value)) { setErr(e, ErrCode::VALUE_RANGE, "input out of range"); return false; }
                valueIn += prev.value;
                if (!moneyRange(valueIn)) { setErr(e, ErrCode::VALUE_OVERFLOW, "valueIn overflow"); return false; }

                Op o; o.op = in.prev; o.beforePresent = true; o.before = prev; o.afterPresent = false;
                delta.ops.push_back(o);
                work.erase(in.prev);
            }
        }

        for (uint32_t j = 0; j < tx.vout.size(); ++j) {
            const TxOut& t = tx.vout[j];
            if (!moneyRange(t.value)) { setErr(e, ErrCode::VALUE_RANGE, "output out of range"); return false; }
            valueOut += t.value;
            if (!moneyRange(valueOut)) { setErr(e, ErrCode::VALUE_OVERFLOW, "valueOut overflow"); return false; }

            OutPoint key; key.txid = tx.txid; key.n = j;
            if (work.has(key)) { setErr(e, ErrCode::DUP_OUTPOINT, "BIP30: exact outpoint exists"); return false; }

            Coin c; c.value = t.value; c.script = t.script; c.height = b.height;
            c.fCoinBase = tx.coinbase; c.fCoinStake = tx.coinstake;

            Op o; o.op = key; o.beforePresent = false; o.afterPresent = true; o.after = c;
            delta.ops.push_back(o);
            work.put(key, c);
        }

        if (!tx.coinbase && !tx.coinstake && valueOut > valueIn) {
            setErr(e, ErrCode::VALUE_RANGE, "non-coinbase spends more than inputs");
            return false;
        }
    }

    if (out) *out = delta;
    if (newState) *newState = work;
    return true;
}

} // namespace cs
