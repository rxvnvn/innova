// Isolated chainstate vertical slice - durable commit store implementation.
// Reviewed design: MANIFEST rename = visibility boundary; parent-dir fsync =
// durability boundary. CoW state files (one immutable file per generation) so a
// recovery can always select exactly one committed state. Single writer via flock.
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

bool csFsyncDir(const std::string& dir, ChainError* e) {
    int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
    if (fd < 0) { setErr(e, ErrCode::IO, "open dir " + dir); return false; }
    int rc = ::fsync(fd); ::close(fd);
    if (rc != 0) { setErr(e, ErrCode::FS, "fsync dir " + dir); return false; }
    return true;
}

// ---- Manifest --------------------------------------------------------------
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
    if (start + 8 + 4 + 32 + 4 + 4 > n) { setErr(e, ErrCode::CORRUPT, "undo header truncated"); return false; }
    if (memcmp(d + start, magic, 8) != 0) { setErr(e, ErrCode::CORRUPT, "undo magic"); return false; }
    size_t o = start + 8;
    uint32_t len = 0; uint32_t t = 0; for (int i = 0; i < 4; ++i) t |= (uint32_t)d[o + i] << (8 * i); len = t; o += 4;
    memcpy(r->blockHash.data(), d + o, 32); o += 32;
    uint32_t hh = 0; for (int i = 0; i < 4; ++i) hh |= (uint32_t)d[o + i] << (8 * i); o += 4; r->height = (int32_t)hh;
    uint32_t cnt = 0; for (int i = 0; i < 4; ++i) cnt |= (uint32_t)d[o + i] << (8 * i); o += 4;
    r->ops.clear();
    for (uint32_t i = 0; i < cnt; ++i) {
        if (o + 36 + 1 > n) { setErr(e, ErrCode::CORRUPT, "undo op truncated"); return false; }
        Op op; std::array<uint8_t,36> k; memcpy(k.data(), d + o, 36); o += 36;
        op.op.txid = Hash(); memcpy(op.op.txid.data(), k.data(), 32); op.op.n = (uint32_t)k[32] | ((uint32_t)k[33] << 8) | ((uint32_t)k[34] << 16) | ((uint32_t)k[35] << 24);
        uint8_t b = d[o++]; op.beforePresent = (b != 0);
        if (op.beforePresent) { size_t save = o; if (!op.before.decode(d, n, &o)) { setErr(e, ErrCode::CORRUPT, "undo before coin"); return false; } (void)save; }
        uint8_t a = d[o++]; op.afterPresent = (a != 0);
        if (op.afterPresent) { if (!op.after.decode(d, n, &o)) { setErr(e, ErrCode::CORRUPT, "undo after coin"); return false; } }
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

static bool endsWith(const std::string& s, const std::string& suf) {
    return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

size_t CommitStore::stateFileCount() const {
    DIR* d = opendir(dir_.c_str());
    if (!d) return 0;
    size_t c = 0; struct dirent* ent;
    while ((ent = readdir(d)) != NULL) {
        std::string nm = ent->d_name;
        if (nm.size() > 0 && nm.rfind("state-", 0) == 0 && endsWith(nm, ".dat") && !endsWith(nm, ".tmp")) ++c;
    }
    closedir(d);
    return c;
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

bool CommitStore::loadUndoFor(const Manifest& m, ChainError* e) {
    undo_.clear();
    undoCommittedBytes_ = 0;
    if (m.undoSize == 0) return true;
    std::vector<uint8_t> bytes; bool ex = false;
    if (!csReadFile(p("undo.dat"), &bytes, &ex, e)) return false;
    if (!ex) { setErr(e, ErrCode::CORRUPT, "undo.dat missing but undoSize=" + std::to_string(m.undoSize)); return false; }
    size_t off = 0;
    while (off < m.undoSize) {
        UndoRecord r;
        if (!UndoRecord::decode(bytes.data(), bytes.size(), &off, &r, e)) return false;
        undo_.push_back(r);
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

bool CommitStore::open(const std::string& dir, ChainError* e) {
    clearErr(e);
    if (opened_) { setErr(e, ErrCode::ALREADY_OPEN, dir); return false; }
    dir_ = dir;
    if (!ensureDir(dir_)) { setErr(e, ErrCode::IO, "cannot create dir " + dir_); return false; }

    lockFd_ = ::open(p("LOCK").c_str(), O_RDWR | O_CREAT, 0644);
    if (lockFd_ < 0) { setErr(e, ErrCode::IO, "open LOCK"); return false; }
    if (::flock(lockFd_, LOCK_EX | LOCK_NB) != 0) { ::close(lockFd_); lockFd_ = -1; setErr(e, ErrCode::LOCKED, "another writer holds LOCK"); return false; }

    // recovery: primary MANIFEST, else MANIFEST.bak, else empty (NOT_PUBLISHED)
    ChainError e1; bool pEx = false;
    if (tryLoadManifest(p("MANIFEST"), &pEx, &e1)) {
        // loaded primary
    } else {
        ChainError e2; bool bEx = false;
        if (tryLoadManifest(p("MANIFEST.bak"), &bEx, &e2)) {
            hadRecoveryIssue_ = true;   // recovered to previous committed state
        } else if (!pEx) {
            // fresh / never committed -> empty
            state_ = UtxoState(); undo_.clear(); generation_ = 0; undoCommittedBytes_ = 0; blockHash_ = Hash(); tipHeight_ = -1;
        } else {
            // primary present but corrupt and no valid previous -> fail closed
            close(); if (e) *e = e1; return false;
        }
    }

    // clean stray staging files
    ::unlink(p("MANIFEST.tmp").c_str());
    DIR* d = opendir(dir_.c_str());
    if (d) { struct dirent* ent; while ((ent = readdir(d)) != NULL) { std::string nm = ent->d_name; if (endsWith(nm, ".tmp")) ::unlink(p(nm).c_str()); } closedir(d); }

    // GC any unreferenced CoW state files left by an interrupted publish
    {
        DIR* d2 = opendir(dir_.c_str());
        if (d2) {
            struct dirent* ent;
            while ((ent = readdir(d2)) != NULL) {
                std::string nm = ent->d_name;
                if (nm.rfind("state-", 0) != 0 || !endsWith(nm, ".dat") || endsWith(nm, ".tmp")) continue;
                std::string num = nm.substr(6, nm.size() > 10 ? nm.size() - 10 : 0);
                bool numeric = !num.empty();
                for (char ch : num) if (ch < '0' || ch > '9') numeric = false;
                if (!numeric) continue;
                uint64_t g = strtoull(num.c_str(), NULL, 10);
                if (generation_ == 0 || (g != generation_ && g != generation_ - 1)) ::unlink(p(nm).c_str());
            }
            closedir(d2);
        }
    }

    opened_ = true;
    return true;
}

void CommitStore::crashIf(CrashPoint at) {
    if (crashPoint_ == at) { ::fflush(NULL); ::_exit(42); }
}

bool CommitStore::commitManifest(const Manifest& m, const std::vector<uint8_t>& stateBytes,
                                 const uint8_t* undoAppend, size_t undoAppendLen, ChainError* e) {
    const std::string stateFinal = p(m.stateName);
    const std::string stateTmp = stateFinal + ".tmp";

    // 1) STAGE state (immutable CoW file) - durable before publication
    if (!csWriteFile(stateTmp, stateBytes.data(), stateBytes.size(), injectNoSpace_, injectFsyncFail_, e)) return false;
    // 2) STAGE undo append (durable) - only if there is a delta to record
    if (undoAppendLen > 0) {
        if (!csAppendFile(p("undo.dat"), undoAppend, undoAppendLen, injectNoSpace_, injectFsyncFail_, e)) { ::unlink(stateTmp.c_str()); return false; }
    }
    // 3) STAGE manifest tmp (durable)
    std::vector<uint8_t> mb = m.encode();
    if (!csWriteFile(p("MANIFEST.tmp"), mb.data(), mb.size(), injectNoSpace_, injectFsyncFail_, e)) { ::unlink(stateTmp.c_str()); return false; }
    // 4) keep the previous committed manifest for recovery fallback
    { std::vector<uint8_t> cur; bool ex = false; ChainError e2; if (csReadFile(p("MANIFEST"), &cur, &ex, &e2) && ex) { ChainError e3; csWriteFile(p("MANIFEST.bak"), cur.data(), cur.size(), false, false, &e3); } }
    // 5) move staged state into its final (unreferenced) name
    if (::rename(stateTmp.c_str(), stateFinal.c_str()) != 0) { setErr(e, ErrCode::FS, "rename state"); ::unlink(p("MANIFEST.tmp").c_str()); return false; }
    // 6) === VISIBILITY BOUNDARY === rename manifest into place
    crashIf(CrashPoint::BeforeRename);
    if (::rename(p("MANIFEST.tmp").c_str(), p("MANIFEST").c_str()) != 0) { setErr(e, ErrCode::FS, "rename MANIFEST"); return false; }
    crashIf(CrashPoint::AfterRename);
    // 7) === DURABILITY BOUNDARY === parent-dir fsync
    if (!csFsyncDir(dir_, e)) return false;
    crashIf(CrashPoint::AfterDirSync);
    return true;
}

bool CommitStore::publish(const Delta& d, const Hash& blockHash, int32_t height, ChainError* e) {
    clearErr(e);
    if (!opened_) { setErr(e, ErrCode::NOT_OPEN, dir_); return false; }
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
    if (!commitManifest(m, stateBytes, recBytes.data(), recBytes.size(), e)) return false;

    // POST: durable commit succeeded -> publish to readers
    state_ = newState; generation_ = newGen; undo_.push_back(rec);
    undoCommittedBytes_ = m.undoSize; blockHash_ = blockHash; tipHeight_ = height;

    // GC: keep current + previous generation only
    DIR* dirp = opendir(dir_.c_str());
    if (dirp) {
        struct dirent* ent;
        while ((ent = readdir(dirp)) != NULL) {
            std::string nm = ent->d_name;
            if (nm.rfind("state-", 0) == 0 && endsWith(nm, ".dat") && nm != stateNameFor(generation_) && nm != stateNameFor(generation_ ? generation_ - 1 : 0))
                ::unlink(p(nm).c_str());
        }
        closedir(dirp);
    }
    return true;
}

bool CommitStore::disconnect(const Hash& blockHash, ChainError* e) {
    clearErr(e);
    if (!opened_) { setErr(e, ErrCode::NOT_OPEN, dir_); return false; }
    if (undo_.empty()) { setErr(e, ErrCode::NOT_FOUND, "no committed undo to disconnect"); return false; }
    if (undo_.back().blockHash != blockHash) { setErr(e, ErrCode::NOT_FOUND, "only the most recent block can be disconnected"); return false; }

    UndoRecord last = undo_.back();
    UtxoState newState = state_;
    Delta dd; dd.ops = last.ops;
    revertDelta(newState, dd);

    uint64_t newGen = generation_ + 1;
    uint64_t newUndoSize = undoCommittedBytes_ - last.encLen;

    Manifest m;
    m.generation = newGen; m.undoSize = newUndoSize;
    m.stateName = stateNameFor(newGen); m.stateDigest = newState.digest();
    m.blockHash = Hash(); m.tipHeight = tipHeight_ - 1;

    std::vector<uint8_t> stateBytes = newState.serialize();
    m.fileHash = sha256(stateBytes);
    if (!commitManifest(m, stateBytes, NULL, 0, e)) return false;  // no undo append; MANIFEST truncates undo

    state_ = newState; generation_ = newGen; undo_.pop_back();
    undoCommittedBytes_ = newUndoSize; blockHash_ = Hash(); tipHeight_ = m.tipHeight;
    return true;
}

} // namespace cs
