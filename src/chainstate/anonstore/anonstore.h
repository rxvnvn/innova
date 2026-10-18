// ANONSTATE isolated slice - standalone ANON / key-image store under chainstate/anonstore.
// Implements the M8.3 slice-a contract in DESIGN.md. Reuses the audited store3a BTree for
// the disk-resident CoW paged structure and the incstore dump-suffix helpers for the
// commit-protocol files. This is an ISOLATED slice compiled only by its own harness main();
// NOT wired into the daemon build. Do NOT depend on core.h here.
#pragma once
#include "chainstate.h"
#include "store3a/btree.h"
#include "store3a/incstore.h"
#include <array>
#include <vector>
#include <string>
#include <set>

namespace cs { namespace anon {

// namespace bytes for the 36-byte tree key prefix (KEY layout in DESIGN.md).
static const uint8_t ANON_OUTPUT_NS = 0xA1;
static const uint8_t KEY_IMAGE_NS   = 0xB1;
static const uint32_t MANIFEST_FORMAT_VERSION = 2u;   // mirrors store3a Manifest (reused encoder)

// ---- thin value structs (fixed-width LE encode/decode; do NOT use core.h) ---------
struct CAnonOutput {
    std::array<uint8_t,32> txid;   // outpoint.txid
    uint32_t n;                    // outpoint.n
    int64_t  nValue;
    int32_t  nBlockHeight;
    uint8_t  nCompromised;

    CAnonOutput() : txid{}, n(0), nValue(0), nBlockHeight(0), nCompromised(0) {}
    std::vector<uint8_t> encode() const;                 // 49 bytes LE
    bool decode(const uint8_t* d, size_t n, size_t* off); // 49 bytes
};

struct CKeyImageSpent {
    std::array<uint8_t,32> txnHash; // uint256 txnHash
    uint32_t inputNo;
    int64_t  nValue;

    CKeyImageSpent() : txnHash{}, inputNo(0), nValue(0) {}
    std::vector<uint8_t> encode() const;                 // 44 bytes LE
    bool decode(const uint8_t* d, size_t n, size_t* off); // 44 bytes
};

// One key mutation carried across a block connect / undo record.
struct Mutation {
    uint8_t  ns;                   // ANON_OUTPUT_NS | KEY_IMAGE_NS
    std::array<uint8_t,33> key;    // 33 raw bytes (CPubKey::Raw() / ec_point bytes)
    bool beforePresent;
    std::vector<uint8_t> before;   // pre-block value bytes (present iff beforePresent)
    bool afterPresent;
    std::vector<uint8_t> after;    // post-block value bytes (present iff afterPresent)

    Mutation() : ns(0), key{}, beforePresent(false), afterPresent(false) {}
};

// Commit-barrier crash points (mirror store3a CrashPoint, anon-focused form).
enum class CrashPoint {
    None,
    BeforeUndoWrite,   // after tree fsync, before undo write
    AfterUndoWrite,    // undo durable, before MANIFEST.tmp
    BeforeRename,      // MANIFEST.tmp written+fsynced, before atomic rename
    AfterRename,       // rename visible, before dir fsync
    AfterDirSync       // dir fsync done (fully committed; ACK advisory)
};
// Which barrier to inject a simulated fault into (mirror store3a InjectStage).
enum class InjectStage {
    None,
    TreeFsync,
    UndoAppend,
    ManifestTmp,
    DirFsync,
    AckWrite
};

class AnonStore {
public:
    AnonStore() {}
    ~AnonStore() { close(); }

    bool open(const std::string& dir, ChainError* e);   // fail-closed; authority = MANIFEST
    bool opened() const { return opened_; }
    void close();

    bool getAo(const uint8_t key33[33], CAnonOutput* out, bool* found, ChainError* e) const;
    bool getKi(const uint8_t key33[33], CKeyImageSpent* out, bool* found, ChainError* e) const;
    bool connect(const Hash& blockHash, int32_t height,
                 const std::vector<Mutation>& muts, ChainError* e);                 // atomic
    bool disconnect(const Hash& blockHash, const Hash& parentTipHash, ChainError* e); // atomic revert

    // ---- bulk-seed (migration) path ----------------------------------------------
    // Seed the HISTORIC anon-output/key-image base state (slice-c): each entry via
    // seedPut (CoW append with free-page reclaim, root hash authenticated on every
    // write so an incomplete seed is never mistaken for complete). finalizeSeed does
    // ONE atomic publish (undoSize = 0 base: historic blocks carry no per-block undo;
    // post-seed connect()/disconnect() add/revert their own undo records). A crash
    // before finalize leaves the store at its empty gen-0 state -> re-run the seed.
    bool seedAo(const uint8_t key33[33], const CAnonOutput& v, ChainError* e);
    bool seedKi(const uint8_t key33[33], const CKeyImageSpent& v, ChainError* e);
    bool finalizeSeed(const Hash& tipBlockHash, int32_t height, ChainError* e);
    bool seeded() const { return seeded_; }

    // diagnostics
    uint64_t generation() const { return generation_; }
    const Hash& tipHash() const { return blockHash_; }
    int32_t tipHeight() const { return tipHeight_; }
    size_t valueCount() const { return valueCount_; }
    bool needsReopen() const { return needsReopen_; }
    const std::string& dir() const { return dir_; }
    const store3a::BTree& tree() const { return tree_; }
    store3a::BTree& tree() { return tree_; }

    // test hooks
    void setCrashPoint(CrashPoint p) { crashPoint_ = p; }
    void setInjectStage(InjectStage s) { injectStage_ = s; }
    void injectNoSpace(bool v) { injectNoSpace_ = v; }
    void injectFsyncFail(bool v) { injectFsyncFail_ = v; }
    void clearInjection() { injectStage_ = InjectStage::None; injectNoSpace_ = false; injectFsyncFail_ = false; }
    void injectUndoShortOnce(size_t bytes) { injectUndoShort_ = bytes; }
    void injectPartialAppendOnce(size_t bytes = 0) { if (bytes) tree_.injectPartialAppendOnce(bytes); }

private:
    bool commitBarriers(const store3a::Manifest& m, uint64_t undoOffset,
                        const uint8_t* undoAppend, size_t undoAppendLen, ChainError* e);
    bool writeAck(uint64_t gen, ChainError* e);
    void crashIf(CrashPoint at);
    bool loadUndoMeta(ChainError* e);   // scan undo.dat over committed undoSize; fill undoMetas_
    bool recomputeValueCount(ChainError* e);  // O(state): exact distinct-entry count (test-scale)
    bool seedPutBoth(uint8_t ns, const uint8_t key33[33], const std::vector<uint8_t>& val, ChainError* e);
    std::string p(const std::string& name) const { return dir_ + "/" + name; }
    bool ns(InjectStage s) const { return injectStage_ == s && injectNoSpace_; }
    bool ff(InjectStage s) const { return injectStage_ == s && injectFsyncFail_; }
    bool inj(InjectStage s) const { return injectStage_ == s && (injectNoSpace_ || injectFsyncFail_); }
    bool injErr(ChainError* e, const std::string& what) {
        if (injectNoSpace_) { setErr(e, ErrCode::NO_SPACE, "injected ENOSPC at " + what); return true; }
        if (injectFsyncFail_) { setErr(e, ErrCode::FS, "injected fsync failure at " + what); return true; }
        return false;
    }

    struct UndoMeta {
        Hash blockHash{};
        int32_t height = 0;
        uint64_t offset = 0;
        uint32_t len = 0;
    };

    std::string dir_;
    store3a::BTree tree_;
    bool opened_ = false;
    bool needsReopen_ = false;
    bool renamePassed_ = false;        // did MANIFEST.tmp rename to MANIFEST this commit?
    uint64_t generation_ = 0;
    uint64_t undoCommittedBytes_ = 0;  // authority-derived committed undo length
    Hash blockHash_{};
    int32_t tipHeight_ = -1;
    size_t valueCount_ = 0;            // distinct key->value entries in the tree
    std::vector<UndoMeta> undoMetas_;  // committed undo records in commit order (offset,len,hash,height)
    bool seeded_ = false;              // a bulk base seed has been finalized (migration complete)
    std::set<store3a::Key> seedSeen_;   // keys already seeded in the current seed batch (dup detection)

    CrashPoint crashPoint_ = CrashPoint::None;
    InjectStage injectStage_ = InjectStage::None;
    bool injectNoSpace_ = false;
    bool injectFsyncFail_ = false;
    size_t injectUndoShort_ = 0;
};

}} // namespace cs::anon