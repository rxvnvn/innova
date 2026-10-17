// Isolated chainstate vertical slice - durable commit store implementation (Stage 2B).
// Boundaries: MANIFEST rename = visibility; parent-dir fsync = durability; durable ACK
// record = acknowledgment. CoW state files (one immutable file per generation).
// Single writer via flock. Fallback is fail-loud when it would discard an acknowledged
// generation (F01). Backup establishment is checked and aborts publish on failure (F02).
// Undo is indexed by metadata only; record bodies are read from disk on demand (F03 RAM).
#include "commitstore.h"
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>

namespace cs {

static const size_t UNDO_HEADER_SIZE = 52;   // 8 magic + 4 len + 32 blockHash + 4 height + 4 opCount
static const size_t ACK_SIZE = 12;           // 8 gen + 4 crc32

static uint32_t csCrc32(const uint8_t* d, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) {
        c ^= d[i];
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(c & 1)));
    }
    return c ^ 0xFFFFFFFFu;
}

static bool ensureDir(const std::string& path) {
    if (path.empty()) return false;
    struct stat st;
    if (stat(path.c_str(), &st) == 0) return S_ISDIR(st.st_mode);
    std::string parent = path;
    size_t pos = parent.find_last_of('/');
    if (pos != std::string::npos && pos > 0) ensureDir(parent.substr(0, pos));
    if (mkdir(path.c_str(), 0755) == 0) return true;
    return errno == EEXIST;
}

static bool endsWith(const std::string& s, const std::string& suf) {
    return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

bool csWriteFile(const std::string& path, const uint8_t* d, size_t n,
                 bool injectNoSpace, bool injectFsyncFail, ChainError* e) {
    if (injectNoSpace) { setErr(e, ErrCode::NO_SPACE, "injected ENOSPC on " + path); return false; }
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { setErr(e, ErrCode::IO, "open " + path); return false; }
    size_t off = 0;
    while (off < n) { ssize_t w = ::write(fd, d + off, n - off); if (w <= 0) { ::close(fd); setErr(e, ErrCode::IO, "write " + path); return false; } off += (size_t)w; }
    if (injectFsyncFail) { ::close(fd); setErr(e, ErrCode::FS, "injected fsync failure on " + path); return false; }
    if (::fsync(fd) != 0) { ::close(fd); setErr(e, ErrCode::FS, "fsync " + path); return false; }
    if (::close(fd) != 0) { setErr(e, ErrCode::FS, "close " + path); return false; }
    return true;
}

static bool csAppendFile(const std::string& path, const uint8_t* d, size_t n,
                         bool injectNoSpace, bool injectFsyncFail, ChainError* e) {
    if (injectNoSpace) { setErr(e, ErrCode::NO_SPACE, "injected ENOSPC append " + path); return false; }
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) { setErr(e, ErrCode::IO, "open " + path); return false; }
    size_t off = 0;
    while (off < n) { ssize_t w = ::write(fd, d + off, n - off); if (w <= 0) { ::close(fd); setErr(e, ErrCode::IO, "append " + path); return false; } off += (size_t)w; }
    if (injectFsyncFail) { ::close(fd); setErr(e, ErrCode::FS, "injected fsync failure append " + path); return false; }
    if (::fsync(fd) != 0) { ::close(fd); setErr(e, ErrCode::FS, "fsync append " + path); return false; }
    ::close(fd);
    return true;
}

bool csReadFile(const std::string& path, std::vector<uint8_t>* out, bool* exists, ChainError* e) {
    *exists = false;
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) { if (errno == ENOENT) return true; setErr(e, ErrCode::IO, "open " + path); return false; }
    *exists = true;
    out->clear();
    uint8_t buf[65536];
    for (;;) { ssize_t r = ::read(fd, buf, sizeof(buf)); if (r < 0) { ::close(fd); setErr(e, ErrCode::IO, "read " + path); return false; } if (r == 0) break; out->insert(out->end(), buf, buf + r); }
    ::close(fd);
    return true;
}

bool csReadRange(const std::string& path, uint64_t offset, uint64_t len, std::vector<uint8_t>* out, ChainError* e) {
    out->clear(); out->resize((size_t)len);
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) { setErr(e, ErrCode::IO, "open " + path); return false; }
    uint64_t got = 0;
    while (got < len) {
        ssize_t r = ::pread(fd, out->data() + got, (size_t)(len - got), (off_t)(offset + got));
        if (r <= 0) { ::close(fd); setErr(e, ErrCode::CORRUPT, "short read " + path); return false; }
        got += (uint64_t)r;
    }
    ::close(fd);
    return true;
}

bool csFsyncDir(const std::string& dir, ChainError* e) {
    int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
    if (fd < 0) { setErr(e, ErrCode::IO, "open dir " + dir); return false; }
    int rc = ::fsync(fd); ::close(fd);
    if (rc != 0) { setErr(e, ErrCode::FS, "fsync dir " + dir); return false; }
    return true;
}

// ---- Manifest --------------------------------------------------------------
bool Manifest::validStateName(const std::string& name) {
    // must equal "state-<digits>.dat"
    const std::string pre = "state-", suf = ".dat";
    if (name.size() <= pre.size() + suf.size()) return false;
    if (name.compare(0, pre.size(), pre) != 0) return false;
    if (name.compare(name.size() - suf.size(), suf.size(), suf) != 0) return false;
    for (size_t i = pre.size(); i < name.size() - suf.size(); ++i)
        if (name[i] < '0' || name[i] > '9') return false;
    return true;
}

std::vector<uint8_t> Manifest::encode() const {
    std::vector<uint8_t> o;
    const uint8_t magic[8] = {'I','N','N','C','S','M','N','T'};
    o.insert(o.end(), magic, magic + 8);
    putU32(o, formatVersion);
    uint32_t total = (uint32_t)(8 + 4 + 4 + 8 + 8 + 4 + stateName.size() + 32 + 32 + 32 + 4 + 4);
    putU32(o, total);
    putU64(o, generation);
    putU64(o, undoSize);
    putU32(o, (uint32_t)stateName.size());
    o.insert(o.end(), stateName.begin(), stateName.end());
    o.insert(o.end(), stateDigest.begin(), stateDigest.end());
    o.insert(o.end(), fileHash.begin(), fileHash.end());
    o.insert(o.end(), blockHash.begin(), blockHash.end());
    putI32(o, tipHeight);
    putU32(o, csCrc32(o.data(), o.size()));
    return o;
}

bool Manifest::decode(const uint8_t* d, size_t n, Manifest* m, ChainError* e) {
    const uint8_t magic[8] = {'I','N','N','C','S','M','N','T'};
    if (n < 140 || memcmp(d, magic, 8) != 0) { setErr(e, ErrCode::BAD_SERIALIZE, "manifest magic/size"); return false; }
    size_t off = 8;
    uint32_t ver = 0; if (!getU32(d, n, &off, &ver)) { setErr(e, ErrCode::BAD_SERIALIZE, "manifest ver"); return false; }
    if (ver != 1u) { setErr(e, ErrCode::UNKNOWN_VERSION, "manifest version " + std::to_string(ver)); return false; }
    uint32_t sz = 0; if (!getU32(d, n, &off, &sz)) { setErr(e, ErrCode::BAD_SERIALIZE, "manifest size"); return false; }
    if ((size_t)sz != n) { setErr(e, ErrCode::CORRUPT, "manifest size mismatch"); return false; }
    uint64_t gen = 0, us = 0;
    if (!getU64(d, n, &off, &gen) || !getU64(d, n, &off, &us)) { setErr(e, ErrCode::BAD_SERIALIZE, "manifest gen"); return false; }
    uint32_t nl = 0; if (!getU32(d, n, &off, &nl)) { setErr(e, ErrCode::BAD_SERIALIZE, "manifest namelen"); return false; }
    if (off + nl + 32 + 32 + 32 + 4 + 4 > n) { setErr(e, ErrCode::CORRUPT, "manifest truncated"); return false; }
    std::string name((const char*)d + off, nl); off += nl;
    memcpy(m->stateDigest.data(), d + off, 32); off += 32;
    memcpy(m->fileHash.data(), d + off, 32); off += 32;
    memcpy(m->blockHash.data(), d + off, 32); off += 32;
    uint32_t hh = 0; if (!getU32(d, n, &off, &hh)) { setErr(e, ErrCode::BAD_SERIALIZE, "manifest height"); return false; }
    uint32_t crcStored = 0; if (!getU32(d, n, &off, &crcStored)) { setErr(e, ErrCode::BAD_SERIALIZE, "manifest crc"); return false; }
    if (csCrc32(d, off - 4) != crcStored) { setErr(e, ErrCode::CORRUPT, "manifest crc mismatch"); return false; }
    if (!validStateName(name)) { setErr(e, ErrCode::CORRUPT, "manifest stateName invalid: " + name); return false; }
    m->formatVersion = ver; m->generation = gen; m->undoSize = us; m->stateName = name; m->tipHeight = (int32_t)hh;
    return true;
}

// ---- UndoRecord ------------------------------------------------------------
std::vector<uint8_t> UndoRecord::encode() const {
    std::vector<uint8_t> o;
    const uint8_t magic[8] = {'I','N','N','U','N','D','O','1'};
    o.insert(o.end(), magic, magic + 8);
    putU32(o, 0);  // length placeholder
    o.insert(o.end(), blockHash.begin(), blockHash.end());
    putI32(o, height);
    putU32(o, (uint32_t)ops.size());
    for (const Op& op : ops) {
        auto k = op.op.keyBytes(); o.insert(o.end(), k.begin(), k.end());
        o.push_back(op.beforePresent ? 1 : 0);
        if (op.beforePresent) { auto b = op.before.encode(); o.insert(o.end(), b.begin(), b.end()); }
        o.push_back(op.afterPresent ? 1 : 0);
        if (op.afterPresent) { auto a = op.after.encode(); o.insert(o.end(), a.begin(), a.end()); }
    }
    uint32_t len = (uint32_t)o.size();
    memcpy(&o[8], &len, 4);
    return o;
}

bool UndoRecord::decode(const uint8_t* d, size_t n, size_t* off, UndoRecord* r, ChainError* e) {
    const uint8_t magic[8] = {'I','N','N','U','N','D','O','1'};
    size_t start = *off;
    if (start + UNDO_HEADER_SIZE > n) { setErr(e, ErrCode::CORRUPT, "undo header truncated"); return false; }
    if (memcmp(d + start, magic, 8) != 0) { setErr(e, ErrCode::CORRUPT, "undo magic"); return false; }
    size_t o = start + 8;
    uint32_t len = 0; for (int i = 0; i < 4; ++i) len |= (uint32_t)d[o + i] << (8 * i); o += 4;
    memcpy(r->blockHash.data(), d + o, 32); o += 32;
    uint32_t hh = 0; for (int i = 0; i < 4; ++i) hh |= (uint32_t)d[o + i] << (8 * i); o += 4; r->height = (int32_t)hh;
    uint32_t cnt = 0; for (int i = 0; i < 4; ++i) cnt |= (uint32_t)d[o + i] << (8 * i); o += 4;
    if (len < UNDO_HEADER_SIZE || start + len > n) { setErr(e, ErrCode::CORRUPT, "undo len out of range"); return false; }
    r->ops.clear();
    for (uint32_t i = 0; i < cnt; ++i) {
        if (o + 36 + 1 > start + len) { setErr(e, ErrCode::CORRUPT, "undo op truncated"); return false; }
        Op op; std::array<uint8_t,36> k; memcpy(k.data(), d + o, 36); o += 36;
        memcpy(op.op.txid.data(), k.data(), 32);
        op.op.n = (uint32_t)k[32] | ((uint32_t)k[33] << 8) | ((uint32_t)k[34] << 16) | ((uint32_t)k[35] << 24);
        uint8_t b = d[o++]; op.beforePresent = (b != 0);
        if (op.beforePresent) { if (!op.before.decode(d, start + len, &o)) { setErr(e, ErrCode::CORRUPT, "undo before coin"); return false; } }
        uint8_t a = d[o++]; op.afterPresent = (a != 0);
        if (op.afterPresent) { if (!op.after.decode(d, start + len, &o)) { setErr(e, ErrCode::CORRUPT, "undo after coin"); return false; } }
        r->ops.push_back(op);
    }
    if (o != start + len) { setErr(e, ErrCode::CORRUPT, "undo record length mismatch"); return false; }
    r->encLen = len;
    *off = start + len;
    return true;
}

// ---- CommitStore -----------------------------------------------------------
CommitStore::~CommitStore() { close(); }

void CommitStore::close() {
    if (lockFd_ >= 0) { ::flock(lockFd_, LOCK_UN); ::close(lockFd_); lockFd_ = -1; }
    opened_ = false;
}

std::vector<uint64_t> CommitStore::stateFileGens() const {
    std::vector<uint64_t> gens;
    DIR* d = opendir(dir_.c_str());
    if (!d) return gens;
    struct dirent* ent;
    while ((ent = readdir(d)) != NULL) {
        std::string nm = ent->d_name;
        if (nm.rfind("state-", 0) != 0 || !endsWith(nm, ".dat") || endsWith(nm, ".tmp")) continue;
        std::string num = nm.substr(6, nm.size() > 10 ? nm.size() - 10 : 0);
        bool numeric = !num.empty();
        for (char ch : num) if (ch < '0' || ch > '9') numeric = false;
        if (numeric) gens.push_back(strtoull(num.c_str(), NULL, 10));
    }
    closedir(d);
    std::sort(gens.begin(), gens.end());
    return gens;
}

void CommitStore::gcStateFiles() {
    for (uint64_t g : stateFileGens())
        if (generation_ == 0 || (g != generation_ && g != generation_ - 1))
            ::unlink(p(stateNameFor(g)).c_str());
}

bool CommitStore::loadStateFor(const Manifest& m, ChainError* e) {
    std::vector<uint8_t> bytes; bool ex = false;
    if (!csReadFile(p(m.stateName), &bytes, &ex, e)) return false;
    if (!ex) { setErr(e, ErrCode::CORRUPT, "state file missing: " + m.stateName); return false; }
    if (sha256(bytes) != m.fileHash) { setErr(e, ErrCode::CORRUPT, "state file integrity hash mismatch: " + m.stateName); return false; }
    UtxoState tmp;
    if (!UtxoState::deserialize(bytes.data(), bytes.size(), &tmp, e)) return false;
    if (tmp.digest() != m.stateDigest) { setErr(e, ErrCode::CORRUPT, "state semantic digest mismatch: " + m.stateName); return false; }
    state_ = tmp;
    return true;
}

// Build a metadata-only index of the committed undo prefix; bodies stay on disk.
bool CommitStore::loadUndoFor(const Manifest& m, ChainError* e) {
    undoIndex_.clear();
    undoCommittedBytes_ = 0;
    if (m.undoSize == 0) return true;
    struct stat st;
    if (stat(p("undo.dat").c_str(), &st) != 0) { setErr(e, ErrCode::CORRUPT, "undo.dat missing but undoSize=" + std::to_string(m.undoSize)); return false; }
    uint64_t off = 0;
    const uint8_t magic[8] = {'I','N','N','U','N','D','O','1'};
    while (off < m.undoSize) {
        if (off + UNDO_HEADER_SIZE > m.undoSize) { setErr(e, ErrCode::CORRUPT, "undo header crosses committed length"); return false; }
        std::vector<uint8_t> hdr;
        if (!csReadRange(p("undo.dat"), off, UNDO_HEADER_SIZE, &hdr, e)) return false;
        if (memcmp(hdr.data(), magic, 8) != 0) { setErr(e, ErrCode::CORRUPT, "undo magic at offset " + std::to_string(off)); return false; }
        uint32_t len = 0; for (int i = 0; i < 4; ++i) len |= (uint32_t)hdr[8 + i] << (8 * i);
        UndoMeta meta;
        memcpy(meta.blockHash.data(), hdr.data() + 12, 32);
        uint32_t hh = 0; for (int i = 0; i < 4; ++i) hh |= (uint32_t)hdr[44 + i] << (8 * i); meta.height = (int32_t)hh;
        if (len < UNDO_HEADER_SIZE || off + len > m.undoSize) { setErr(e, ErrCode::CORRUPT, "undo record length out of range"); return false; }
        meta.offset = off; meta.len = len;
        undoIndex_.push_back(meta);
        off += len;
    }
    if (off != m.undoSize) { setErr(e, ErrCode::CORRUPT, "undo.dat committed length mismatch"); return false; }
    undoCommittedBytes_ = m.undoSize;
    return true;
}

bool CommitStore::tryLoadManifest(const std::string& path, bool* exists, ChainError* e) {
    std::vector<uint8_t> bytes;
    if (!csReadFile(path, &bytes, exists, e)) return false;
    if (!*exists) return false;
    Manifest m;
    if (!Manifest::decode(bytes.data(), bytes.size(), &m, e)) return false;
    if (!loadStateFor(m, e)) return false;
    if (!loadUndoFor(m, e)) return false;
    generation_ = m.generation;
    blockHash_ = m.blockHash;
    tipHeight_ = m.tipHeight;
    return true;
}

bool CommitStore::readAck(bool* present, uint64_t* gen, ChainError* e) {
    *present = false; *gen = 0;
    std::vector<uint8_t> b; bool ex = false;
    if (!csReadFile(p("ACK"), &b, &ex, e)) return false;
    if (!ex) return true;
    if (b.size() != ACK_SIZE) { setErr(e, ErrCode::CORRUPT, "ACK size"); return false; }
    uint32_t crc = 0; for (int i = 0; i < 4; ++i) crc |= (uint32_t)b[8 + i] << (8 * i);
    if (csCrc32(b.data(), 8) != crc) { setErr(e, ErrCode::CORRUPT, "ACK crc"); return false; }
    uint64_t g = 0; for (int i = 0; i < 8; ++i) g |= (uint64_t)b[i] << (8 * i);
    *present = true; *gen = g;
    return true;
}

bool CommitStore::writeAck(uint64_t gen, ChainError* e) {
    std::vector<uint8_t> b(ACK_SIZE, 0);
    for (int i = 0; i < 8; ++i) b[i] = (uint8_t)((gen >> (8 * i)) & 0xff);
    uint32_t crc = csCrc32(b.data(), 8);
    for (int i = 0; i < 4; ++i) b[8 + i] = (uint8_t)((crc >> (8 * i)) & 0xff);
    const std::string tmp = p("ACK.tmp");
    if (!csWriteFile(tmp, b.data(), b.size(), ns(InjectStage::AckWrite), ff(InjectStage::AckWrite), e)) return false;
    if (::rename(tmp.c_str(), p("ACK").c_str()) != 0) { setErr(e, ErrCode::FS, "rename ACK"); return false; }
    if (!csFsyncDir(dir_, e)) return false;
    return true;
}

bool CommitStore::open(const std::string& dir, ChainError* e) {
    clearErr(e);
    if (opened_) { setErr(e, ErrCode::ALREADY_OPEN, dir); return false; }
    dir_ = dir;
    if (!ensureDir(dir_)) { setErr(e, ErrCode::IO, "cannot create dir " + dir_); return false; }

    lockFd_ = ::open(p("LOCK").c_str(), O_RDWR | O_CREAT, 0644);
    if (lockFd_ < 0) { setErr(e, ErrCode::IO, "open LOCK"); return false; }
    if (::flock(lockFd_, LOCK_EX | LOCK_NB) != 0) { ::close(lockFd_); lockFd_ = -1; setErr(e, ErrCode::LOCKED, "another writer holds LOCK"); return false; }

    if (!readAck(&ackPresent_, &ackGen_, e)) { close(); return false; }

    ChainError e1; bool pEx = false;
    bool pOk = tryLoadManifest(p("MANIFEST"), &pEx, &e1);
    if (pOk) {
        uint64_t pg = generation_;
        if (ackPresent_) {
            if (pg < ackGen_) { close(); setErr(e, ErrCode::CORRUPT, "FAIL-CLOSED: primary generation " + std::to_string(pg) + " is OLDER than acknowledged generation " + std::to_string(ackGen_) + " (rollback of acknowledged state refused)"); return false; }
            if (pg > ackGen_ + 1) { close(); setErr(e, ErrCode::CORRUPT, "primary generation " + std::to_string(pg) + " ahead of acknowledged generation " + std::to_string(ackGen_)); return false; }
        } else {
            if (pg != 0) { close(); setErr(e, ErrCode::CORRUPT, "acknowledgment evidence missing for generation " + std::to_string(pg)); return false; }
        }
        // Reconcile ACK to cover a recovered but previously-unacknowledged generation.
        if (!ackPresent_ || pg > ackGen_) { if (!writeAck(pg, e)) { close(); return false; } ackGen_ = pg; ackPresent_ = true; }
    } else if (!pEx) {
        if (ackPresent_ && ackGen_ > 0) { close(); setErr(e, ErrCode::CORRUPT, "acknowledged MANIFEST (gen " + std::to_string(ackGen_) + ") is missing"); return false; }
        state_ = UtxoState(); undoIndex_.clear(); generation_ = 0; undoCommittedBytes_ = 0; blockHash_ = Hash(); tipHeight_ = -1;
    } else {
        // Primary present but unloadable -> classify with the independent ACK evidence.
        ChainError e2; bool bEx = false;
        bool bOk = tryLoadManifest(p("MANIFEST.bak"), &bEx, &e2);
        if (!bOk) { close(); setErr(e, e1.code == ErrCode::OK ? ErrCode::CORRUPT : e1.code, "primary corrupt and no valid backup: " + e1.msg); return false; }
        uint64_t bg = generation_;
        if (!ackPresent_) { close(); setErr(e, ErrCode::CORRUPT, "cannot classify corrupt primary: no acknowledgment evidence"); return false; }
        if (ackGen_ > bg) { close(); setErr(e, ErrCode::CORRUPT, "FAIL-LOUD: acknowledged generation " + std::to_string(ackGen_) + " is corrupt/unloadable; refusing silent rollback to generation " + std::to_string(bg)); return false; }
        if (ackGen_ < bg) { close(); setErr(e, ErrCode::CORRUPT, "inconsistent acknowledgment(" + std::to_string(ackGen_) + ") vs backup(" + std::to_string(bg) + ") generations"); return false; }
        // ackGen_ == bg: the corrupt primary was an unacknowledged attempt; the acknowledged
        // state (== backup) is preserved. Repair MANIFEST to the acknowledged generation.
        hadRecoveryIssue_ = true;
        std::vector<uint8_t> bakBytes; bool bkEx = false; ChainError e3;
        if (csReadFile(p("MANIFEST.bak"), &bakBytes, &bkEx, &e3) && bkEx) {
            if (!csWriteFile(p("MANIFEST"), bakBytes.data(), bakBytes.size(), false, false, &e3)) { close(); setErr(e, ErrCode::FS, "failed to repair MANIFEST from backup"); return false; }
            if (!csFsyncDir(dir_, &e3)) { close(); setErr(e, ErrCode::FS, "failed to fsync repair of MANIFEST"); return false; }
        }
    }

    // Clean staging and GC unreferenced CoW state files left by an interrupted publish.
    { DIR* d = opendir(dir_.c_str()); if (d) { struct dirent* ent; while ((ent = readdir(d)) != NULL) { std::string nm = ent->d_name; if (endsWith(nm, ".tmp")) ::unlink(p(nm).c_str()); } closedir(d); } }
    gcStateFiles();

    needsReopen_ = false;
    opened_ = true;
    return true;
}

void CommitStore::crashIf(CrashPoint at) {
    if (crashPoint_ == at) { ::fflush(NULL); ::_exit(42); }
}

bool CommitStore::commitManifest(const Manifest& m, const std::vector<uint8_t>& stateBytes,
                                 const uint8_t* undoAppend, size_t undoAppendLen, ChainError* e) {
    renamePassed_ = false;
    const std::string stateFinal = p(m.stateName);
    const std::string stateTmp = stateFinal + ".tmp";

    // 1) STAGE state (immutable CoW file)
    if (!csWriteFile(stateTmp, stateBytes.data(), stateBytes.size(), ns(InjectStage::StateStage), ff(InjectStage::StateStage), e)) return false;
    // 2) STAGE undo append
    if (undoAppendLen > 0) {
        if (!csAppendFile(p("undo.dat"), undoAppend, undoAppendLen, ns(InjectStage::UndoAppend), ff(InjectStage::UndoAppend), e)) { ::unlink(stateTmp.c_str()); return false; }
    }
    // 3) STAGE manifest tmp
    std::vector<uint8_t> mb = m.encode();
    if (!csWriteFile(p("MANIFEST.tmp"), mb.data(), mb.size(), ns(InjectStage::ManifestTmp), ff(InjectStage::ManifestTmp), e)) { ::unlink(stateTmp.c_str()); return false; }
    // 4) ESTABLISH FALLBACK (F02): when a prior MANIFEST exists, durably establish MANIFEST.bak,
    //    verify it decodes and references an existing state file; abort on any failure.
    {
        std::vector<uint8_t> cur; bool curEx = false; ChainError e2;
        if (!csReadFile(p("MANIFEST"), &cur, &curEx, &e2)) { setErr(e, e2.code, "read prior MANIFEST for backup: " + e2.msg); ::unlink(stateTmp.c_str()); ::unlink(p("MANIFEST.tmp").c_str()); return false; }
        if (curEx) {
            if (!csWriteFile(p("MANIFEST.bak"), cur.data(), cur.size(), ns(InjectStage::BakWrite), ff(InjectStage::BakWrite), e)) { ::unlink(stateTmp.c_str()); ::unlink(p("MANIFEST.tmp").c_str()); return false; }
            std::vector<uint8_t> rb; bool rex = false; ChainError e3;
            Manifest bman;
            if (!csReadFile(p("MANIFEST.bak"), &rb, &rex, &e3) || !rex) { setErr(e, ErrCode::FS, "backup not readable after write"); ::unlink(stateTmp.c_str()); ::unlink(p("MANIFEST.tmp").c_str()); return false; }
            if (!Manifest::decode(rb.data(), rb.size(), &bman, &e3)) { setErr(e, ErrCode::FS, "backup not decodable after write: " + e3.msg); ::unlink(stateTmp.c_str()); ::unlink(p("MANIFEST.tmp").c_str()); return false; }
            struct stat st;
            if (stat(p(bman.stateName).c_str(), &st) != 0) { setErr(e, ErrCode::FS, "backup references missing state " + bman.stateName); ::unlink(stateTmp.c_str()); ::unlink(p("MANIFEST.tmp").c_str()); return false; }
        }
    }
    // 5) move staged state into its final (still unreferenced) name
    if (::rename(stateTmp.c_str(), stateFinal.c_str()) != 0) { setErr(e, ErrCode::FS, "rename state"); ::unlink(p("MANIFEST.tmp").c_str()); return false; }
    // 6) === VISIBILITY BOUNDARY ===
    crashIf(CrashPoint::BeforeRename);
    if (inj(InjectStage::PrimaryRename)) { injErr(e, "primary rename"); ::unlink(p("MANIFEST.tmp").c_str()); return false; }
    if (::rename(p("MANIFEST.tmp").c_str(), p("MANIFEST").c_str()) != 0) { setErr(e, ErrCode::FS, "rename MANIFEST"); return false; }
    renamePassed_ = true;
    crashIf(CrashPoint::AfterRename);
    // 7) === DURABILITY BOUNDARY ===
    if (inj(InjectStage::DirFsync)) { injErr(e, "dir fsync"); return false; }
    if (!csFsyncDir(dir_, e)) return false;
    crashIf(CrashPoint::AfterDirSync);
    return true;
}

bool CommitStore::publish(const Delta& d, const Hash& blockHash, int32_t height, ChainError* e) {
    clearErr(e);
    if (!opened_) { setErr(e, ErrCode::NOT_OPEN, dir_); return false; }
    if (needsReopen_) { setErr(e, ErrCode::NOT_OPEN, "store requires reopen after a post-rename failure"); return false; }
    uint64_t newGen = generation_ + 1;
    UtxoState newState = state_;
    applyDelta(newState, d);

    UndoRecord rec; rec.blockHash = blockHash; rec.height = height; rec.ops = d.ops;
    std::vector<uint8_t> recBytes = rec.encode();

    Manifest m;
    m.generation = newGen; m.undoSize = undoCommittedBytes_ + recBytes.size();
    m.stateName = stateNameFor(newGen); m.stateDigest = newState.digest();
    m.blockHash = blockHash; m.tipHeight = height;
    std::vector<uint8_t> stateBytes = newState.serialize();
    m.fileHash = sha256(stateBytes);

    if (!commitManifest(m, stateBytes, recBytes.data(), recBytes.size(), e)) {
        if (renamePassed_) needsReopen_ = true;   // failure after visibility: state on disk may be NEW
        return false;
    }
    // === ACKNOWLEDGMENT BOUNDARY === durable ACK after durability
    if (!writeAck(newGen, e)) { needsReopen_ = true; return false; }

    // POST: publish to readers
    UndoMeta meta; meta.blockHash = blockHash; meta.height = height;
    meta.offset = undoCommittedBytes_; meta.len = (uint32_t)recBytes.size();
    state_ = newState; generation_ = newGen;
    undoIndex_.push_back(meta); undoCommittedBytes_ = m.undoSize;
    ackGen_ = newGen; ackPresent_ = true;
    blockHash_ = blockHash; tipHeight_ = height;
    gcStateFiles();
    return true;
}

bool CommitStore::disconnect(const Hash& blockHash, const Hash& parentTipHash, ChainError* e) {
    clearErr(e);
    if (!opened_) { setErr(e, ErrCode::NOT_OPEN, dir_); return false; }
    if (needsReopen_) { setErr(e, ErrCode::NOT_OPEN, "store requires reopen after a post-rename failure"); return false; }
    if (undoIndex_.empty()) { setErr(e, ErrCode::NOT_FOUND, "no committed undo to disconnect"); return false; }
    const UndoMeta meta = undoIndex_.back();
    if (meta.blockHash != blockHash) { setErr(e, ErrCode::NOT_FOUND, "only the most recent block can be disconnected"); return false; }

    std::vector<uint8_t> body;
    if (!csReadRange(p("undo.dat"), meta.offset, meta.len, &body, e)) return false;
    size_t off = 0; UndoRecord rec;
    if (!UndoRecord::decode(body.data(), body.size(), &off, &rec, e)) return false;

    UtxoState newState = state_;
    Delta dd; dd.ops = rec.ops;
    revertDelta(newState, dd);

    uint64_t newGen = generation_ + 1;
    uint64_t newUndoSize = undoCommittedBytes_ - meta.len;

    Manifest m;
    m.generation = newGen; m.undoSize = newUndoSize;
    m.stateName = stateNameFor(newGen); m.stateDigest = newState.digest();
    m.blockHash = parentTipHash; m.tipHeight = tipHeight_ - 1;
    std::vector<uint8_t> stateBytes = newState.serialize();
    m.fileHash = sha256(stateBytes);

    if (!commitManifest(m, stateBytes, NULL, 0, e)) {
        if (renamePassed_) needsReopen_ = true;
        return false;
    }
    if (!writeAck(newGen, e)) { needsReopen_ = true; return false; }

    state_ = newState; generation_ = newGen; undoIndex_.pop_back();
    undoCommittedBytes_ = newUndoSize; ackGen_ = newGen; ackPresent_ = true;
    blockHash_ = parentTipHash; tipHeight_ = m.tipHeight;
    gcStateFiles();
    return true;
}

} // namespace cs
