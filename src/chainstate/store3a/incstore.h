// Stage 3A - incremental (O(delta)) chainstate store: paged CoW B+tree + durable
// MANIFEST/backup/ACK commit protocol (carried from the audited Stage-2B design).
#pragma once
#include "chainstate.h"
#include "btree.h"
#include <string>
#include <vector>

namespace cs { namespace store3a {

struct Manifest {
    uint32_t formatVersion = 1;
    uint64_t generation = 0;
    uint64_t undoSize = 0;
    uint32_t rootPageId = 0;
    Hash rootHash{};
    Hash blockHash{};
    int32_t tipHeight = -1;
    std::vector<uint8_t> encode() const;
    static bool decode(const uint8_t* d, size_t n, Manifest* m, ChainError* e);
};

struct UndoRecord {
    Hash blockHash{};
    int32_t height = 0;
    std::vector<Op> ops;
    std::vector<uint8_t> encode() const;           // includes trailing crc32 (S2B-1)
    static bool decode(const uint8_t* d, size_t n, size_t* off, UndoRecord* r, ChainError* e);
};

struct UndoMeta { Hash blockHash{}; int32_t height = 0; uint64_t offset = 0; uint32_t len = 0; };

enum class CrashPoint { None, BeforeBakReplace, BakPrepared, AfterBakReplace, AfterUndoWrite, BeforeRename, AfterRename, AfterDirSync };
enum class InjectStage { None, PageWrite, UndoAppend, ManifestTmp, BakWrite, PrimaryRename, DirFsync, AckWrite };
// R1 (F0-2): creation policy for the first undo write after a fresh open. It closes the TOCTOU window
// between the open-time classification of an orphan undo.dat and the first create/write of the log:
//   Any             - create if missing (ordinary publish on an established store).
//   RequireAbsent   - open O_CREAT|O_EXCL: fail closed if the path exists at all, i.e. if anything
//                     appeared between classification and creation (kernel-atomic, no lstat race).
//   RequireExisting - never create (no O_CREAT): fail closed if the file disappeared after it was
//                     classified as an accepted empty residue.
enum class UndoCreate { Any, RequireAbsent, RequireExisting };

class CommitStore3A {
public:
    ~CommitStore3A();
    bool open(const std::string& dir, ChainError* e);
    void close();

    bool opened() const { return opened_; }
    bool needsReopen() const { return needsReopen_; }
    uint64_t generation() const { return generation_; }
    uint64_t acknowledgedGeneration() const { return ackGen_; }
    const std::string& dir() const { return dir_; }
    const Hash& tipHash() const { return blockHash_; }
    int32_t tipHeight() const { return tipHeight_; }
    size_t undoCount() const { return undoIndex_.size(); }
    bool hadRecoveryIssue() const { return hadRecoveryIssue_; }
    const BTree& tree() const { return tree_; }
    BTree& tree() { return tree_; }
    uint32_t rootPageId() const { return tree_.rootId(); }
    const Hash& rootHash() const { return tree_.rootHash(); }
    bool truncatedTreeTail() const { return tree_.truncatedTail(); }

    bool get(const OutPoint& k, Coin* c, bool* found, ChainError* e) const;
    bool existsUnspentTxid(const Hash& txid, bool* found, ChainError* e) const;

    bool publish(const Delta& d, const Hash& blockHash, int32_t height, ChainError* e);
    bool disconnect(const Hash& blockHash, ChainError* e);

    // test hooks
    void setCrashPoint(CrashPoint c) { crashPoint_ = c; }
    void setInjectStage(InjectStage s) { injectStage_ = s; }
    void injectNoSpace(bool v) { injectNoSpace_ = v; }
    void injectFsyncFail(bool v) { injectFsyncFail_ = v; }
    void clearInjection() { injectStage_ = InjectStage::None; injectNoSpace_ = false; injectFsyncFail_ = false; }
    // R1 test hook: force the next tree page append to be a short/interrupted write.
    void injectPartialAppendOnce(size_t bytes) { tree_.injectPartialAppendOnce(bytes); }
    // R2 test hook: fail the n-th page append (earlier appends succeed and stage pages).
    void injectPartialAppendOn(size_t n) { tree_.injectPartialAppendOn(n); }
    // F0 test hook: write only the first `bytes` bytes of the next undo record, then fail (torn write).
    void injectUndoShortOnce(size_t bytes) { injectUndoShortOnce_ = bytes; }

private:
    bool ns(InjectStage s) const { return injectStage_ == s && injectNoSpace_; }
    bool ff(InjectStage s) const { return injectStage_ == s && injectFsyncFail_; }
    bool inj(InjectStage s) const { return injectStage_ == s && (injectNoSpace_ || injectFsyncFail_); }
    bool injErr(ChainError* e, const std::string& what) {
        if (injectNoSpace_) { setErr(e, ErrCode::NO_SPACE, "injected ENOSPC at " + what); return true; }
        if (injectFsyncFail_) { setErr(e, ErrCode::FS, "injected fsync failure at " + what); return true; }
        return false;
    }

    bool tryLoadManifest(const std::string& path, bool* exists, uint32_t* rootId, Hash* rootHash, ChainError* e);
    bool loadUndoFor(const Manifest& m, ChainError* e);
    bool commitBarriers(const Manifest& m, uint64_t undoOffset, const uint8_t* undoAppend, size_t undoAppendLen, ChainError* e);
    bool writeAck(uint64_t gen, ChainError* e);
    bool readAck(bool* present, uint64_t* gen, ChainError* e);
    void crashIf(CrashPoint at);
    void cleanupTmp();

    std::string p(const std::string& name) const { return dir_ + "/" + name; }

    std::string dir_;
    int lockFd_ = -1;
    bool opened_ = false;
    bool needsReopen_ = false;
    BTree tree_;
    uint64_t generation_ = 0;
    uint64_t undoCommittedBytes_ = 0;
    uint64_t ackGen_ = 0;
    bool ackPresent_ = false;
    Hash blockHash_{};
    int32_t tipHeight_ = -1;
    bool hadRecoveryIssue_ = false;
    bool renamePassed_ = false;
    std::vector<UndoMeta> undoIndex_;
    CrashPoint crashPoint_ = CrashPoint::None;
    InjectStage injectStage_ = InjectStage::None;
    bool injectNoSpace_ = false;
    bool injectFsyncFail_ = false;
    size_t injectUndoShortOnce_ = 0;
    // R1 (F0-2): the creation policy for the next undo write after a fresh open (consumed by it).
    UndoCreate freshUndoCreate_ = UndoCreate::Any;
};

// R1 (F0-2): the create policy is documented on the UndoCreate enum above.
bool csWriteFile(const std::string& path, const uint8_t* d, size_t n, bool injNs, bool injFs, ChainError* e);
bool csAppendFile(const std::string& path, const uint8_t* d, size_t n, bool injNs, bool injFs, ChainError* e);
// F0: write at an explicit offset. Never truncates; never moves the offset implicitly (no O_APPEND);
// refuses to write past a hole (file shorter than off). Bytes < off are left byte-identical.
// R1: the trailing `create` policy decides whether the file may be created, and fails closed on any
// classification/creation race (see UndoCreate). Every failure path here is fail-closed: the caller
// must not treat a false return as "nothing was written" without checking its own recovery contract.
bool csWriteAtFile(const std::string& path, uint64_t off, const uint8_t* d, size_t n, bool injNs, bool injFs, ChainError* e,
                   UndoCreate create = UndoCreate::Any);
bool csReadFile(const std::string& path, std::vector<uint8_t>* out, bool* exists, ChainError* e);
bool csReadRange(const std::string& path, uint64_t off, uint64_t len, std::vector<uint8_t>* out, ChainError* e);
bool csFsyncDir(const std::string& dir, ChainError* e);
uint32_t csCrc32(const uint8_t* d, size_t n);

}} // namespace cs::store3a
