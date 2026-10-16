// Isolated chainstate vertical slice - durable commit store (Stage 2B remediation).
// Phase sequence: STAGE -> PUBLISH(rename MANIFEST = visibility) -> DURABLE(dir fsync)
// -> ACK(durable acknowledgment) -> POST(in-memory publish + GC).
//
// Fallback policy (F01): a corrupt PRIMARY is only ever recovered by falling back
// to MANIFEST.bak when doing so does NOT discard an acknowledged generation. If the
// acknowledged generation would be lost, open() FAILS LOUD (no silent rollback).
// The ACK record is an independent durable boundary, so a corrupt primary can be
// classified acknowledged vs unacknowledged; if that evidence is absent -> fail closed.
#pragma once
#include "chainstate.h"
#include <string>
#include <vector>

namespace cs {

struct Manifest {
    uint32_t formatVersion = 1;
    uint64_t generation = 0;
    uint64_t undoSize = 0;      // committed byte length of undo.dat
    std::string stateName;      // bare relative file name of the committed state
    Hash stateDigest{};         // canonical UTXO-set digest (semantic equality)
    Hash fileHash{};            // sha256 of the serialized state file (integrity)
    Hash blockHash{};           // tip block
    int32_t tipHeight = -1;
    std::vector<uint8_t> encode() const;
    static bool decode(const uint8_t* d, size_t n, Manifest* m, ChainError* e);
    static bool validStateName(const std::string& name);
};

struct UndoRecord {
    Hash blockHash{};
    int32_t height = 0;
    std::vector<Op> ops;
    size_t encLen = 0;          // encoded byte length (valid after decode/build)
    std::vector<uint8_t> encode() const;
    static bool decode(const uint8_t* d, size_t n, size_t* off, UndoRecord* r, ChainError* e);
};

// Small in-RAM index of undo records (metadata only; bodies live on disk).
struct UndoMeta { Hash blockHash{}; int32_t height = 0; uint64_t offset = 0; uint32_t len = 0; };

enum class CrashPoint { None, BeforeRename, AfterRename, AfterDirSync };
enum class InjectStage { None, StateStage, UndoAppend, ManifestTmp, BakWrite, PrimaryRename, DirFsync, AckWrite };

class CommitStore {
public:
    CommitStore() {}
    ~CommitStore();

    bool open(const std::string& dir, ChainError* e);
    void close();

    bool opened() const { return opened_; }
    bool needsReopen() const { return needsReopen_; }
    uint64_t generation() const { return generation_; }
    uint64_t acknowledgedGeneration() const { return ackGen_; }
    bool ackPresent() const { return ackPresent_; }
    const std::string& dir() const { return dir_; }
    const UtxoState& state() const { return state_; }
    const Hash& tipHash() const { return blockHash_; }
    int32_t tipHeight() const { return tipHeight_; }
    size_t undoCount() const { return undoIndex_.size(); }
    bool hadRecoveryIssue() const { return hadRecoveryIssue_; }
    std::vector<uint64_t> stateFileGens() const;

    bool publish(const Delta& d, const Hash& blockHash, int32_t height, ChainError* e);
    bool disconnect(const Hash& blockHash, ChainError* e);

    // test hooks
    void setCrashPoint(CrashPoint c) { crashPoint_ = c; }
    void setInjectStage(InjectStage s) { injectStage_ = s; }
    void injectNoSpace(bool v) { injectNoSpace_ = v; }
    void injectFsyncFail(bool v) { injectFsyncFail_ = v; }
    void clearInjection() { injectStage_ = InjectStage::None; injectNoSpace_ = false; injectFsyncFail_ = false; }

private:
    bool ns(InjectStage s) const { return injectStage_ == s && injectNoSpace_; }
    bool ff(InjectStage s) const { return injectStage_ == s && injectFsyncFail_; }
    bool inj(InjectStage s) const { return injectStage_ == s && (injectNoSpace_ || injectFsyncFail_); }
    bool injErr(ChainError* e, const std::string& what) {
        if (injectNoSpace_) { setErr(e, ErrCode::NO_SPACE, "injected ENOSPC at " + what); return true; }
        if (injectFsyncFail_) { setErr(e, ErrCode::FS, "injected fsync failure at " + what); return true; }
        return false;
    }

    bool tryLoadManifest(const std::string& path, bool* exists, ChainError* e);
    bool loadStateFor(const Manifest& m, ChainError* e);
    bool loadUndoFor(const Manifest& m, ChainError* e);
    bool commitManifest(const Manifest& m, const std::vector<uint8_t>& stateBytes,
                        const uint8_t* undoAppend, size_t undoAppendLen, ChainError* e);
    bool writeAck(uint64_t gen, ChainError* e);
    bool readAck(bool* present, uint64_t* gen, ChainError* e);
    void gcStateFiles();
    void crashIf(CrashPoint at);

    std::string p(const std::string& name) const { return dir_ + "/" + name; }
    std::string stateNameFor(uint64_t gen) const { return "state-" + std::to_string(gen) + ".dat"; }

    std::string dir_;
    int lockFd_ = -1;
    bool opened_ = false;
    bool needsReopen_ = false;
    UtxoState state_;
    std::vector<UndoMeta> undoIndex_;
    uint64_t generation_ = 0;
    uint64_t undoCommittedBytes_ = 0;
    uint64_t ackGen_ = 0;
    bool ackPresent_ = false;
    Hash blockHash_{};
    int32_t tipHeight_ = -1;
    bool hadRecoveryIssue_ = false;
    bool renamePassed_ = false;
    CrashPoint crashPoint_ = CrashPoint::None;
    InjectStage injectStage_ = InjectStage::None;
    bool injectNoSpace_ = false;
    bool injectFsyncFail_ = false;
};

// low-level checked durable primitives (mirror util.cpp:1490-1568 semantics)
bool csWriteFile(const std::string& path, const uint8_t* d, size_t n, bool injectNoSpace, bool injectFsyncFail, ChainError* e);
bool csReadFile(const std::string& path, std::vector<uint8_t>* out, bool* exists, ChainError* e);
bool csReadRange(const std::string& path, uint64_t offset, uint64_t len, std::vector<uint8_t>* out, ChainError* e);
bool csFsyncDir(const std::string& dir, ChainError* e);

} // namespace cs
