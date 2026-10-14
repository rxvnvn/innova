// Isolated chainstate vertical slice - durable commit store.
// Phase sequence: STAGE (files) -> PUBLISH(rename MANIFEST) -> DUIRABLE(dir fsync)
// -> POST(in-memory snapshot + notify). The MANIFEST rename is the visibility
// boundary; the parent-dir fsync is the durability boundary. Single writer.
#pragma once
#include "chainstate.h"
#include <string>
#include <vector>

namespace cs {

struct Manifest {
    uint32_t formatVersion = 1;
    uint64_t generation = 0;
    uint64_t undoSize = 0;      // committed byte length of undo.dat
    std::string stateName;      // relative file name of the committed state
    Hash stateDigest{};         // canonical UTXO-set digest (semantic equality)
    Hash fileHash{};            // sha256 of the serialized state file (integrity)
    Hash blockHash{};           // tip block
    int32_t tipHeight = -1;
    std::vector<uint8_t> encode() const;
    static bool decode(const uint8_t* d, size_t n, Manifest* m, ChainError* e);
};

struct UndoRecord {
    Hash blockHash{};
    int32_t height = 0;
    std::vector<Op> ops;
    size_t encLen = 0;          // encoded byte length (valid after decode/build)
    std::vector<uint8_t> encode() const;
    static bool decode(const uint8_t* d, size_t n, size_t* off, UndoRecord* r, ChainError* e);
};

enum class CrashPoint { None, BeforeRename, AfterRename, AfterDirSync, DuringRecovery };

class CommitStore {
public:
    CommitStore() {}
    ~CommitStore();

    bool open(const std::string& dir, ChainError* e);
    void close();

    bool opened() const { return opened_; }
    uint64_t generation() const { return generation_; }
    const std::string& dir() const { return dir_; }
    const UtxoState& state() const { return state_; }
    const Hash& tipHash() const { return blockHash_; }
    int32_t tipHeight() const { return tipHeight_; }
    size_t undoCount() const { return undo_.size(); }
    bool hadRecoveryIssue() const { return hadRecoveryIssue_; }
    size_t stateFileCount() const;

    bool publish(const Delta& d, const Hash& blockHash, int32_t height, ChainError* e);
    bool disconnect(const Hash& blockHash, ChainError* e);

    // test hooks
    void setCrashPoint(CrashPoint c) { crashPoint_ = c; }
    void injectNoSpace(bool v) { injectNoSpace_ = v; }
    void injectFsyncFail(bool v) { injectFsyncFail_ = v; }

private:
    bool tryLoadManifestFile(const std::string& path, bool* exists);
    bool tryLoadManifest(const std::string& path, bool* exists, ChainError* e);
    bool loadStateFor(const Manifest& m, ChainError* e);
    bool loadUndoFor(const Manifest& m, ChainError* e);
    bool commitManifest(const Manifest& m, const std::vector<uint8_t>& stateBytes,
                        const uint8_t* undoAppend, size_t undoAppendLen, ChainError* e);
    void crashIf(CrashPoint at);

    std::string p(const std::string& name) const { return dir_ + "/" + name; }
    std::string stateNameFor(uint64_t gen) const { return "state-" + std::to_string(gen) + ".dat"; }

    std::string dir_;
    int lockFd_ = -1;
    bool opened_ = false;
    UtxoState state_;
    std::vector<UndoRecord> undo_;
    uint64_t generation_ = 0;
    uint64_t undoCommittedBytes_ = 0;
    Hash blockHash_{};
    int32_t tipHeight_ = -1;
    bool hadRecoveryIssue_ = false;
    CrashPoint crashPoint_ = CrashPoint::None;
    bool injectNoSpace_ = false;
    bool injectFsyncFail_ = false;
};

// low-level checked durable primitives (mirror util.cpp:1490-1568 semantics)
bool csWriteFile(const std::string& path, const uint8_t* d, size_t n, bool injectNoSpace, bool injectFsyncFail, ChainError* e);
bool csReadFile(const std::string& path, std::vector<uint8_t>* out, bool* exists, ChainError* e);
bool csFsyncDir(const std::string& dir, ChainError* e);

} // namespace cs
