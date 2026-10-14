// Isolated chainstate vertical slice - core types, canonical serialization,
// ordered UTXO delta, deterministic apply/revert, canonical state digest.
//
// ARCHITECTURE ONLY -> SLICE prototype. NOT wired into production; no consensus,
// wallet, P2P or mining path is touched. Compiled only by the slice harness.
#pragma once
#include "sha256.h"
#include <cstdint>
#include <string>
#include <map>
#include <vector>
#include <array>

namespace cs {

// ---- error taxonomy (explicit classes) -------------------------------------
enum class ErrCode {
    OK = 0,
    DUP_OUTPOINT,     // BIP30-style: an unspent outpoint with this key already exists
    MISSING_INPUT,    // spent prevout not present in state (incl. intra-block view)
    IMMATURE,         // coinbase/coinstake spend before maturity
    NEGATIVE_SPEND,   // coin not present where a spend requires it
    VALUE_RANGE,      // MoneyRange violation
    VALUE_OVERFLOW,
    BAD_SERIALIZE,
    IO,               // file open/read/write failure
    FS,               // fsync/rename failure
    NO_SPACE,         // simulated out-of-space (ENOSPC is a system macro; renamed)
    CORRUPT,          // digest/CRC/size mismatch
    UNKNOWN_VERSION,  // fail-closed on unrecognized manifest/state format
    LOCKED,           // second writer
    ALREADY_OPEN,
    NOT_OPEN,
    NOT_FOUND,
    INTERNAL
};
const char* errName(ErrCode c);

struct ChainError {
    ErrCode code = ErrCode::OK;
    std::string msg;
    ChainError() {}
    ChainError(ErrCode c, const std::string& m) : code(c), msg(m) {}
    bool ok() const { return code == ErrCode::OK; }
    std::string str() const { return std::string(errName(code)) + (msg.empty() ? "" : (": " + msg)); }
};

inline void setErr(ChainError* e, ErrCode c, const std::string& m) { if (e) { e->code = c; e->msg = m; } }
inline void clearErr(ChainError* e) { if (e) { e->code = ErrCode::OK; e->msg.clear(); } }

// ---- money (legacy semantics preserved) ------------------------------------
static const int64_t MAX_MONEY = 21000000LL * 100000000LL; // COIN = 1e8
inline bool moneyRange(int64_t v) { return v >= 0 && v <= MAX_MONEY; }

// ---- canonical little-endian encoders --------------------------------------
inline void putU32(std::vector<uint8_t>& o, uint32_t v){ for(int i=0;i<4;i++) o.push_back((v>>(8*i))&0xff); }
inline void putI32(std::vector<uint8_t>& o, int32_t v){ putU32(o,(uint32_t)v); }
inline void putU64(std::vector<uint8_t>& o, uint64_t v){ for(int i=0;i<8;i++) o.push_back((v>>(8*i))&0xff); }
inline void putI64(std::vector<uint8_t>& o, int64_t v){ putU64(o,(uint64_t)v); }
inline bool getU32(const uint8_t* d, size_t n, size_t* off, uint32_t* out){
    if (*off + 4 > n) return false; uint32_t v=0; for(int i=0;i<4;i++) v|=(uint32_t)d[*off+i]<<(8*i); *off+=4; *out=v; return true;
}
inline bool getU64(const uint8_t* d, size_t n, size_t* off, uint64_t* out){
    if (*off + 8 > n) return false; uint64_t v=0; for(int i=0;i<8;i++) v|=(uint64_t)d[*off+i]<<(8*i); *off+=8; *out=v; return true;
}

// ---- outpoint & coin -------------------------------------------------------
struct OutPoint {
    Hash txid{};   // 32-byte txid
    uint32_t n = 0;
    std::array<uint8_t,36> keyBytes() const {
        std::array<uint8_t,36> k{}; memcpy(k.data(), txid.data(), 32);
        for (int i=0;i<4;i++) k[32+i] = (uint8_t)((n >> (8*i)) & 0xff);
        return k;
    }
    bool operator<(const OutPoint& o) const { return keyBytes() < o.keyBytes(); }
    bool operator==(const OutPoint& o) const { return keyBytes() == o.keyBytes(); }
};

// Full UTXO coin. Fields map 1:1 to named legacy consumers:
//   value/script  -> spend validation (legacy CTxOut)
//   nHeight       -> maturity (legacy pprev walk / ResolveSpendMaturity)
//   fCoinBase     -> maturity + PoS exclusion
//   fCoinStake    -> PoS coinstake distinction
// stake modifier is a block/index attribute, deliberately NOT a coin field.
struct Coin {
    int64_t value = 0;
    std::vector<uint8_t> script;
    int32_t height = 0;
    bool fCoinBase = false;
    bool fCoinStake = false;

    std::vector<uint8_t> encode() const {
        std::vector<uint8_t> o; o.reserve(17 + script.size());
        putI64(o, value); putI32(o, height);
        uint8_t flags = (fCoinBase?1:0) | (fCoinStake?2:0);
        o.push_back(flags);
        putU32(o, (uint32_t)script.size());
        o.insert(o.end(), script.begin(), script.end());
        return o;
    }
    bool decode(const uint8_t* d, size_t n, size_t* off) {
        uint64_t v64; if (!getU64(d,n,off,&v64)) return false; value=(int64_t)v64;
        uint32_t h; if (!getU32(d,n,off,&h)) return false; height=(int32_t)h;
        if (*off + 1 > n) return false; uint8_t flags=d[(*off)++];
        fCoinBase = (flags&1)!=0; fCoinStake=(flags&2)!=0;
        uint32_t sl; if (!getU32(d,n,off,&sl)) return false;
        if (*off + sl > n) return false; script.assign(d+*off, d+*off+sl); *off+=sl;
        return true;
    }
};

// ---- transactions / blocks (slice-level model) -----------------------------
struct TxIn { OutPoint prev; };
struct TxOut { int64_t value = 0; std::vector<uint8_t> script; bool unspendable = false; };
struct Tx {
    Hash txid{};
    bool coinbase = false;
    bool coinstake = false;
    std::vector<TxIn> vin;
    std::vector<TxOut> vout;
};
struct Block {
    Hash hash{};
    int32_t height = 0;
    std::vector<Tx> txs;
};

// ---- ordered normalized delta (full before/after per op) -------------------
struct Op {
    OutPoint op;
    bool beforePresent = false;
    Coin before;
    bool afterPresent = false;
    Coin after;
};
struct Delta { std::vector<Op> ops; };

// ---- authoritative UTXO set (single writer) --------------------------------
class UtxoState {
public:
    std::map<std::array<uint8_t,36>, Coin> map;

    size_t size() const { return map.size(); }
    bool get(const OutPoint& k, Coin* out) const {
        auto it = map.find(k.keyBytes()); if (it==map.end()) return false; *out=it->second; return true;
    }
    bool has(const OutPoint& k) const { return map.count(k.keyBytes())!=0; }
    bool existsUnspentTxid(const Hash& txid) const {
        for (auto& kv : map) if (memcmp(kv.first.data(), txid.data(), 32)==0) return true;
        return false;
    }
    void put(const OutPoint& k, const Coin& c){ map[k.keyBytes()]=c; }
    void erase(const OutPoint& k){ map.erase(k.keyBytes()); }
    int64_t totalValue() const { int64_t s=0; for (auto& kv:map) s+=kv.second.value; return s; }

    // canonical digest: SHA-256 over entries sorted by canonical key bytes
    Hash digest() const {
        ChainedHash h;
        const uint8_t hdr[4]={'I','U','T','1'}; h.put(hdr,4);
        for (auto& kv : map){ h.put(kv.first.data(), kv.first.size()); h.put(kv.second.encode()); }
        return h.final();
    }

    std::vector<uint8_t> serialize() const {
        std::vector<uint8_t> o;
        const uint8_t magic[8]={'I','N','N','C','U','T','X','1'};
        o.insert(o.end(), magic, magic+8);
        putU32(o, 1u);                       // format version
        putU64(o, (uint64_t)map.size());
        for (auto& kv : map){ o.insert(o.end(), kv.first.begin(), kv.first.end()); std::vector<uint8_t> ce = kv.second.encode(); o.insert(o.end(), ce.begin(), ce.end()); }
        return o;
    }
    static bool deserialize(const uint8_t* d, size_t n, UtxoState* out, ChainError* e) {
        static const uint8_t magic[8]={'I','N','N','C','U','T','X','1'};
        if (n < 20 || memcmp(d, magic, 8)!=0){ setErr(e, ErrCode::BAD_SERIALIZE, "utxo magic/size"); return false; }
        size_t off=8; uint32_t ver=0; if(!getU32(d,n,&off,&ver)){ setErr(e,ErrCode::BAD_SERIALIZE,"utxo ver"); return false; }
        if (ver!=1u){ setErr(e, ErrCode::UNKNOWN_VERSION, "utxo version "+std::to_string(ver)); return false; }
        uint64_t cnt=0; if(!getU64(d,n,&off,&cnt)){ setErr(e,ErrCode::BAD_SERIALIZE,"utxo count"); return false; }
        out->map.clear();
        for (uint64_t i=0;i<cnt;i++){
            if (off+36>n){ setErr(e,ErrCode::CORRUPT,"utxo key truncated"); return false; }
            std::array<uint8_t,36> key; memcpy(key.data(), d+off, 36); off+=36;
            Coin c; if(!c.decode(d,n,&off)){ setErr(e,ErrCode::CORRUPT,"utxo coin truncated"); return false; }
            out->map[key]=c;
        }
        if (off!=n){ setErr(e,ErrCode::CORRUPT,"utxo trailing bytes"); return false; }
        return true;
    }
};

// ---- delta application (deterministic) -------------------------------------
void applyDelta(UtxoState& s, const Delta& d);
void revertDelta(UtxoState& s, const Delta& d);

// ---- block -> delta (legacy-consistent semantics, minimal) -----------------
// Enforces, in block order: intra-block visibility, maturity, BIP30, value range.
// On any failure NO effect is produced on `out` (fail-closed).
bool buildDelta(const UtxoState& base, const Block& b, int32_t maturity,
                Delta* out, UtxoState* newState, ChainError* e);

} // namespace cs
