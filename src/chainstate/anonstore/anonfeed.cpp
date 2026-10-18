// ANONSTATE deterministic feed implementation (see anonfeed.h for the contract).
#include "anonfeed.h"
#include <map>

namespace cs { namespace anon {

// Resolve one ring member's anon output value from the in-block overlay (earlier txs'
// creations) then the committed store view. Returns false if not resolvable.
static bool resolveAo(const std::map<std::array<uint8_t,33>, CAnonOutput>& overlay,
                      const CommittedAoView& committed, const std::array<uint8_t,33>& key,
                      CAnonOutput* out)
{
    std::map<std::array<uint8_t,33>, CAnonOutput>::const_iterator it = overlay.find(key);
    if (it != overlay.end()) { *out = it->second; return true; }
    return committed.getAo(key.data(), out);
}

bool deriveAnonMutations(const BlockFeed& feed, const CommittedAoView& committed,
                         std::vector<Mutation>* out, ChainError* e)
{
    clearErr(e);
    if (!out) { setErr(e, ErrCode::INTERNAL, "null mutation output"); return false; }
    out->clear();

    // in-block overlay: anon outputs created by strictly EARLIER txs in this block
    // (sequential intra-block visibility, matching the legacy per-tx processing order).
    std::map<std::array<uint8_t,33>, CAnonOutput> overlay;

    for (const BlockFeed::FeedTx& tx : feed.txs) {
        // 1) inputs (spends) first: resolve ring value against overlay(earlier txs)+committed.
        for (const FeedInput& in : tx.inputs) {
            if (in.ring.empty()) {
                setErr(e, ErrCode::CORRUPT, "anon input ring is empty");
                return false;
            }
            CAnonOutput v0;
            if (!resolveAo(overlay, committed, in.ring[0], &v0)) {
                setErr(e, ErrCode::MISSING_INPUT,
                       "anon input ring[0] anon output not resolvable (fails closed)");
                return false;
            }
            const int64_t ringValue = v0.nValue;
            for (size_t r = 1; r < in.ring.size(); ++r) {
                CAnonOutput vr;
                if (!resolveAo(overlay, committed, in.ring[r], &vr)) {
                    setErr(e, ErrCode::MISSING_INPUT,
                           "anon input ring member " + std::to_string(r) + " not resolvable (fails closed)");
                    return false;
                }
                if (vr.nValue != ringValue) {
                    setErr(e, ErrCode::CORRUPT, "anon ring amount mismatch (fails closed)");
                    return false;
                }
            }
            // Fresh spend: write-once key image (double-spend prevented upstream). before
            // is absent; disconnect reverts by erasing this key.
            CKeyImageSpent ki; ki.txnHash = in.txid; ki.inputNo = in.inputNo; ki.nValue = ringValue;
            Mutation m; m.ns = KEY_IMAGE_NS; m.key = in.keyImage;
            m.beforePresent = false; m.before.clear();
            m.afterPresent  = true;  m.after = ki.encode();
            out->push_back(m);
        }
        // 2) outputs (creations): add to in-block overlay + emit putAo.
        for (const FeedOutput& o : tx.outputs) {
            CAnonOutput ao;
            ao.txid = o.txid; ao.n = o.n; ao.nValue = o.nValue;
            ao.nBlockHeight = feed.height; ao.nCompromised = 0;
            if (!overlay.insert(std::make_pair(o.pkCoin, ao)).second) {
                setErr(e, ErrCode::DUP_OUTPOINT,
                       "anon output key created twice in one block (fails closed)");
                return false;
            }
            Mutation m; m.ns = ANON_OUTPUT_NS; m.key = o.pkCoin;
            m.beforePresent = false; m.before.clear();
            m.afterPresent  = true;  m.after = ao.encode();
            out->push_back(m);
        }
    }
    return true;
}

}} // namespace cs::anon