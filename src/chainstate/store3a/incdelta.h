// Stage 3A - block -> ordered delta over a read-only UTXO view (O(delta) lookups).
// Mirrors the audited Stage-2B buildDelta semantics without materializing the state.
#pragma once
#include "chainstate.h"
#include "btree.h"
#include <map>

namespace cs { namespace store3a {

struct UtxoReader {
    virtual ~UtxoReader() {}
    virtual bool get(const Key& k, Coin* c) const = 0;                        // present?
    virtual bool keysWithTxid(const uint8_t* txid32, std::vector<Key>* out) const = 0;
};

// Intra-block overlay: created outputs shadow the base; spent keys hide base entries.
class BlockView : public UtxoReader {
public:
    explicit BlockView(const UtxoReader* base) : base_(base) {}
    bool get(const Key& k, Coin* c) const override {
        auto it = present_.find(k);
        if (it != present_.end()) { if (!it->second) return false; auto ci = coin_.find(k); if (ci == coin_.end()) return false; *c = ci->second; return true; }
        return base_->get(k, c);
    }
    bool keysWithTxid(const uint8_t* txid32, std::vector<Key>* out) const override {
        for (auto& kv : present_) if (kv.second && memcmp(kv.first.data(), txid32, 32) == 0) out->push_back(kv.first);
        std::vector<Key> bk; base_->keysWithTxid(txid32, &bk);
        for (auto& k : bk) if (!present_.count(k)) out->push_back(k);
        return true;
    }
    void spend(const Key& k) { present_[k] = false; coin_.erase(k); }
    void create(const Key& k, const Coin& c) { present_[k] = true; coin_[k] = c; }
private:
    const UtxoReader* base_;
    std::map<Key, bool> present_;
    std::map<Key, Coin> coin_;
};

// B+tree-backed reader (bounded prefix scans via the tree).
class TreeReader : public UtxoReader {
public:
    explicit TreeReader(const BTree& t) : t_(t) {}
    bool get(const Key& k, Coin* c) const override {
        std::vector<uint8_t> v; bool f = false; ChainError e;
        if (!t_.get(k, &v, &f, &e)) return false;
        if (!f) return false;
        size_t off = 0; return c->decode(v.data(), v.size(), &off);
    }
    bool keysWithTxid(const uint8_t* txid32, std::vector<Key>* out) const override {
        ChainError e; return t_.keysWithTxidPrefix(txid32, out, &e);
    }
private:
    const BTree& t_;
};

// Produces an ordered delta identical in semantics to cs::buildDelta; no state mutation.
bool buildDeltaI(const UtxoReader& base, const Block& b, int32_t maturity, Delta* out, ChainError* e);

}} // namespace cs::store3a
