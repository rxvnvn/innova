// ANONSTATE isolated slice - implementation.
// Mirrors the store3a commitBarriers crash-safety protocol in a compact anon-focused form:
//   connect:  CoW-mutate btree -> append UNDO record -> fsync -> MANIFEST.tmp+fsync ->
//             atomic rename -> dir fsync -> ACK.
//   open:     authority = MANIFEST; load MANIFEST -> tree.open(pagetree) -> setRoot (hash-auth).
//             Undo bytes beyond committed undoSize are ignored (dropped, never authoritative).
//             A MANIFEST whose fsync/rename did not complete is never trusted.
//   disconnect: read tip UNDO record at committed undoSize; apply reverse mutations (CoW);
//             write MANIFEST for parent; same commit barriers.
// Deep disconnect beyond the retained undo window => FAIL CLOSED (never silent truncation).
#include "anonstore.h"
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <algorithm>

namespace cs { namespace anon {

using namespace store3a;

// fixed-width LE codecs -------------------------------------------------------------
std::vector<uint8_t> CAnonOutput::encode() const {
    std::vector<uint8_t> o; o.reserve(49);
    o.insert(o.end(), txid.begin(), txid.end());
    putU32(o, n);
    putI64(o, nValue);
    putI32(o, nBlockHeight);
    o.push_back(nCompromised);
    return o;
}
bool CAnonOutput::decode(const uint8_t* d, size_t n, size_t* off) {
    if (*off + 49 > n) return false;
    memcpy(txid.data(), d + *off, 32); *off += 32;
    uint32_t o0; if (!getU32(d, n, off, &o0)) return false; this->n = o0;
    uint64_t v64; if (!getU64(d, n, off, &v64)) return false; nValue = (int64_t)v64;
    uint32_t h0; if (!getU32(d, n, off, &h0)) return false; nBlockHeight = (int32_t)h0;
    nCompromised = d[(*off)++];
    return true;
}
std::vector<uint8_t> CKeyImageSpent::encode() const {
    std::vector<uint8_t> o; o.reserve(44);
    o.insert(o.end(), txnHash.begin(), txnHash.end());
    putU32(o, inputNo);
    putI64(o, nValue);
    return o;
}
bool CKeyImageSpent::decode(const uint8_t* d, size_t n, size_t* off) {
    if (*off + 44 > n) return false;
    memcpy(txnHash.data(), d + *off, 32); *off += 32;
    uint32_t o0; if (!getU32(d, n, off, &o0)) return false; inputNo = o0;
    uint64_t v64; if (!getU64(d, n, off, &v64)) return false; nValue = (int64_t)v64;
    return true;
}

// 36-byte tree key from ns + 33 raw bytes
static store3a::Key makeKey(uint8_t ns, const uint8_t key33[33]) {
    store3a::Key k{};
    k[0] = ns;
    memcpy(k.data() + 1, key33, 33);
    return k;
}

// UNDO record wire format (mirrors store3a UndoRecord framing):
//   [0..7]   'A','N','N','U','N','D','O','1'
//   [8..11]  len (u32, total record incl. 4-byte crc)
//   [12..43] blockHash
//   [44..47] height (i32)
//   [48..51] mutation count (u32)
//   [52..]   per-mutation: ns(u8) key33 beforePresent(u8)[+len(u32)+before] afterPresent(u8)[+len(u32)+after]
//   [..-5]   ... payload ... last 4 bytes = crc32 of bytes [record_start, len-4)
static const size_t UNDO_HEADER = 52;
static const uint8_t UNDO_MAGIC[8] = { 'A','N','N','U','N','D','O','1' };

static void putMut(std::vector<uint8_t>& o, const Mutation& m) {
    o.push_back(m.ns);
    o.insert(o.end(), m.key.begin(), m.key.end());
    o.push_back(m.beforePresent ? 1 : 0);
    if (m.beforePresent) { putU32(o, (uint32_t)m.before.size()); o.insert(o.end(), m.before.begin(), m.before.end()); }
    o.push_back(m.afterPresent ? 1 : 0);
    if (m.afterPresent) { putU32(o, (uint32_t)m.after.size()); o.insert(o.end(), m.after.begin(), m.after.end()); }
}
static bool getMut(const uint8_t* d, size_t n, size_t* off, size_t end, Mutation* m) {
    if (*off + 1 + 33 > end) return false;
    m->ns = d[(*off)++];
    memcpy(m->key.data(), d + *off, 33); *off += 33;
    if (*off + 1 > end) return false;
    m->beforePresent = d[(*off)++] != 0;
    m->before.clear();
    if (m->beforePresent) {
        uint32_t l; if (!getU32(d, end, off, &l)) return false;
        if (*off + l > end) return false;
        m->before.assign(d + *off, d + *off + l); *off += l;
    }
    if (*off + 1 > end) return false;
    m->afterPresent = d[(*off)++] != 0;
    m->after.clear();
    if (m->afterPresent) {
        uint32_t l; if (!getU32(d, end, off, &l)) return false;
        if (*off + l > end) return false;
        m->after.assign(d + *off, d + *off + l); *off += l;
    }
    return true;
}
// encode a full undo record (framing + payload + crc)
static std::vector<uint8_t> encodeUndo(const Hash& bh, int32_t height, const std::vector<Mutation>& muts) {
    std::vector<uint8_t> o;
    o.insert(o.end(), UNDO_MAGIC, UNDO_MAGIC + 8);
    putU32(o, 0);                       // len placeholder, filled below
    o.insert(o.end(), bh.begin(), bh.end());
    putI32(o, height);
    putU32(o, (uint32_t)muts.size());
    for (const Mutation& m : muts) putMut(o, m);
    uint32_t len = (uint32_t)o.size() + 4;
    memcpy(&o[8], &len, 4);
    uint32_t crc = csCrc32(o.data(), o.size());
    for (int i = 0; i < 4; ++i) o.push_back((uint8_t)((crc >> (8 * i)) & 0xff));
    return o;
}
// decode a full undo record buffer (validates magic, len, crc, boundaries). o must begin at record start.
static bool decodeUndo(const uint8_t* d, size_t n, size_t* off, Hash* bh_out, int32_t* h_out, std::vector<Mutation>* muts, ChainError* e) {
    size_t recStart = *off;
    if (recStart + UNDO_HEADER + 4 > n) { setErr(e, ErrCode::CORRUPT, "undo record too short"); return false; }
    if (memcmp(d + recStart, UNDO_MAGIC, 8) != 0) { setErr(e, ErrCode::CORRUPT, "undo magic mismatch"); return false; }
    size_t p = recStart + 8;         // skip magic
    uint32_t len = 0; if (!getU32(d, n, &p, &len)) { setErr(e, ErrCode::CORRUPT, "undo len"); return false; }
    if (len < UNDO_HEADER + 4 || recStart + (size_t)len > n) { setErr(e, ErrCode::CORRUPT, "undo len out of range"); return false; }
    memcpy(bh_out->data(), d + p, 32); p += 32;
    uint32_t hh; if (!getU32(d, n, &p, &hh)) { setErr(e, ErrCode::CORRUPT, "undo height"); return false; }
    if (h_out) *h_out = (int32_t)hh;
    uint32_t cnt; if (!getU32(d, n, &p, &cnt)) { setErr(e, ErrCode::CORRUPT, "undo count"); return false; }
    size_t end = recStart + len - 4;   // crc occupies last 4 bytes of the record
    muts->clear();
    for (uint32_t i = 0; i < cnt; ++i) {
        Mutation m; if (!getMut(d, n, &p, end, &m)) { setErr(e, ErrCode::CORRUPT, "undo mutation"); return false; }
        muts->push_back(m);
    }
    if (p != end) { setErr(e, ErrCode::CORRUPT, "undo payload length mismatch"); return false; }
    uint32_t stored = 0; for (int i = 0; i < 4; ++i) stored |= (uint32_t)d[recStart + len - 4 + i] << (8 * i);
    if (csCrc32(d + recStart, len - 4) != stored) { setErr(e, ErrCode::CORRUPT, "undo crc mismatch"); return false; }
    *off = recStart + len;
    return true;
}

bool AnonStore::recomputeValueCount(ChainError* e) {
    std::vector<LeafEntry> ents;
    if (!tree_.collectAll(&ents, e)) return false;
    valueCount_ = ents.size();
    return true;
}

// ---- lifecycle --------------------------------------------------------------------
void AnonStore::close() {
    tree_.close();
    dir_.clear();
    opened_ = false;
    needsReopen_ = false;
    generation_ = 0;
    undoCommittedBytes_ = 0;
    blockHash_ = Hash{};
    tipHeight_ = -1;
    valueCount_ = 0;
    undoMetas_.clear();
    crashPoint_ = CrashPoint::None;
    injectStage_ = InjectStage::None;
    injectNoSpace_ = false;
    injectFsyncFail_ = false;
    injectUndoShort_ = 0;
}

static bool ensureDir(const std::string& path) {
    if (path.empty()) return false;
    struct stat st; if (::stat(path.c_str(), &st) == 0) return S_ISDIR(st.st_mode);
    std::string parent = path; size_t pos = parent.find_last_of('/');
    if (pos != std::string::npos && pos > 0) ensureDir(parent.substr(0, pos));
    if (::mkdir(path.c_str(), 0755) == 0) return true;
    return errno == EEXIST;
}

bool AnonStore::open(const std::string& dir, ChainError* e) {
    clearErr(e);
    if (opened_) { setErr(e, ErrCode::ALREADY_OPEN, dir); return false; }
    dir_ = dir;
    if (!ensureDir(dir_)) { setErr(e, ErrCode::IO, "cannot create dir " + dir_); return false; }

    bool mEx = false;
    std::vector<uint8_t> mbytes;
    if (!csReadFile(p("MANIFEST"), &mbytes, &mEx, e)) return false;

    if (mEx) {
        // authority = MANIFEST
        store3a::Manifest m;
        if (!store3a::Manifest::decode(mbytes.data(), mbytes.size(), &m, e)) return false;
        if (!tree_.open(p("tree.dat"), e)) return false;
        if (!tree_.setRoot(m.rootPageId, m.rootHash, e)) return false;  // hash-auth root; corrupt -> fail closed
        generation_ = m.generation;
        undoCommittedBytes_ = m.undoSize;
        blockHash_ = m.blockHash;
        tipHeight_ = m.tipHeight;
        if (!loadUndoMeta(e)) { close(); return false; }
    } else {
        // fresh store (genesis): empty tree.
        if (!tree_.create(p("tree.dat"), e)) return false;
        uint64_t z = 0; (void)z;
        undoCommittedBytes_ = 0;
        generation_ = 0;
        blockHash_ = Hash{};
        tipHeight_ = -1;
    }
    // recalc valueCount from the tree (authoritative)
    std::vector<LeafEntry> ents;
    if (!tree_.collectAll(&ents, e)) { close(); return false; }
    valueCount_ = ents.size();

    if (undoCommittedBytes_ > 0) {
        // verify the committed undo prefix parses cleanly (fail-closed on corrupt retained record)
        std::vector<uint8_t> all;
        bool uEx = false;
        if (!csReadFile(p("undo.dat"), &all, &uEx, e)) { close(); return false; }
        if (!uEx || all.size() < undoCommittedBytes_) {
            setErr(e, ErrCode::CORRUPT, "undo.dat shorter than committed undoSize");
            close(); return false;
        }
        size_t off = 0;
        for (const UndoMeta& meta : undoMetas_) {
            Hash bh; int32_t hh; std::vector<Mutation> m2;
            if (!decodeUndo(all.data(), all.size(), &off, &bh, &hh, &m2, e)) { close(); return false; }
            if (off != meta.offset + meta.len) { setErr(e, ErrCode::CORRUPT, "undo scan position mismatch"); close(); return false; }
        }
    }

    opened_ = true;
    needsReopen_ = false;
    return true;
}

// scan the committed undo stream [0, undoCommittedBytes_) collecting (offset,len,hash,height) per record.
// Does NOT touch bytes beyond committed undoSize (they are ignored, never authoritative).
bool AnonStore::loadUndoMeta(ChainError* e) {
    undoMetas_.clear();
    if (undoCommittedBytes_ == 0) return true;
    std::vector<uint8_t> buf; bool ex = false;
    // Slurp the whole committed region (test-scale store). Bounded here because undoCommittedBytes_ is authority.
    if (!csReadFile(p("undo.dat"), &buf, &ex, e)) return false;
    if (!ex || buf.size() < undoCommittedBytes_) { setErr(e, ErrCode::CORRUPT, "undo.dat shorter than committed undoSize"); return false; }
    size_t off = 0;
    while (off < undoCommittedBytes_) {
        if (off + UNDO_HEADER + 4 > undoCommittedBytes_) { setErr(e, ErrCode::CORRUPT, "undo header crosses committed length"); return false; }
        if (memcmp(buf.data() + off, UNDO_MAGIC, 8) != 0) { setErr(e, ErrCode::CORRUPT, "undo magic at offset " + std::to_string(off)); return false; }
        size_t p = off + 8;                 // skip magic
        uint32_t len = 0; if (!getU32(buf.data(), buf.size(), &p, &len)) return false;
        if (len < UNDO_HEADER + 4 || (uint64_t)off + len > undoCommittedBytes_) { setErr(e, ErrCode::CORRUPT, "undo len out of range"); return false; }
        UndoMeta meta;
        memcpy(meta.blockHash.data(), buf.data() + p, 32); p += 32;
        { size_t o2 = p; uint32_t hh; getU32(buf.data(), buf.size(), &o2, &hh); meta.height = (int32_t)hh; }
        meta.offset = off;
        meta.len = len;
        undoMetas_.push_back(meta);
        off += len;
    }
    if (off != undoCommittedBytes_) { setErr(e, ErrCode::CORRUPT, "undo scan overrun committed length"); return false; }
    return true;
}

// ---- reads -------------------------------------------------------------------------
bool AnonStore::getAo(const uint8_t key33[33], CAnonOutput* out, bool* found, ChainError* e) const {
    clearErr(e);
    if (!opened_) { setErr(e, ErrCode::NOT_OPEN, dir_); return false; }
    store3a::Key k = makeKey(ANON_OUTPUT_NS, key33);
    std::vector<uint8_t> v; bool f = false;
    if (!tree_.get(k, &v, &f, e)) return false;
    *found = f;
    if (!f) return true;
    size_t off = 0;
    if (!out->decode(v.data(), v.size(), &off)) { setErr(e, ErrCode::CORRUPT, "bad anon output value"); return false; }
    (void)off;
    return true;
}
bool AnonStore::getKi(const uint8_t key33[33], CKeyImageSpent* out, bool* found, ChainError* e) const {
    clearErr(e);
    if (!opened_) { setErr(e, ErrCode::NOT_OPEN, dir_); return false; }
    store3a::Key k = makeKey(KEY_IMAGE_NS, key33);
    std::vector<uint8_t> v; bool f = false;
    if (!tree_.get(k, &v, &f, e)) return false;
    *found = f;
    if (!f) return true;
    size_t off = 0;
    if (!out->decode(v.data(), v.size(), &off)) { setErr(e, ErrCode::CORRUPT, "bad key image value"); return false; }
    (void)off;
    return true;
}

// ---- commit protocol -----------------------------------------------------------------
void AnonStore::crashIf(CrashPoint at) { if (crashPoint_ == at) { ::fflush(NULL); ::_exit(42); } }

bool AnonStore::writeAck(uint64_t gen, ChainError* e) {
    std::vector<uint8_t> b(12, 0);
    for (int i = 0; i < 8; ++i) b[i] = (uint8_t)((gen >> (8 * i)) & 0xff);
    uint32_t crc = csCrc32(b.data(), 8);
    for (int i = 0; i < 4; ++i) b[8 + i] = (uint8_t)((crc >> (8 * i)) & 0xff);
    if (inj(InjectStage::AckWrite)) { injErr(e, "ACK write"); return false; }
    std::vector<uint8_t> tmpB = b; bool tEx = false;
    // write ACK atomically via tmp+rename for consistency with the other barriers
    if (!csWriteFile(p("ACK.tmp"), b.data(), b.size(), ns(InjectStage::AckWrite), ff(InjectStage::AckWrite), e)) return false;
    if (::rename(p("ACK.tmp").c_str(), p("ACK").c_str()) != 0) { setErr(e, ErrCode::FS, "rename ACK"); return false; }
    if (!csFsyncDir(dir_, e)) { return false; }
    (void)tmpB; (void)tEx;
    return true;
}

bool AnonStore::commitBarriers(const store3a::Manifest& m, uint64_t undoOffset,
                               const uint8_t* undoAppend, size_t undoAppendLen, ChainError* e) {
    renamePassed_ = false;
    crashIf(CrashPoint::BeforeUndoWrite);
    // 1) durable tree pages
    if (inj(InjectStage::TreeFsync)) { injErr(e, "tree fsync"); return false; }
    if (!tree_.fsync(e)) return false;
    // 2) durable undo append at the committed logical offset (never O_APPEND, never truncate)
    if (undoAppendLen > 0) {
        if (injectUndoShort_ > 0 && injectUndoShort_ < undoAppendLen) {   // torn write test hook
            size_t part = injectUndoShort_; injectUndoShort_ = 0;
            if (!csWriteAtFile(p("undo.dat"), undoOffset, undoAppend, part, ns(InjectStage::UndoAppend), ff(InjectStage::UndoAppend), e)) return false;
            setErr(e, ErrCode::IO, "injected short undo write"); return false;
        }
        if (!csWriteAtFile(p("undo.dat"), undoOffset, undoAppend, undoAppendLen, ns(InjectStage::UndoAppend), ff(InjectStage::UndoAppend), e)) return false;
    }
    crashIf(CrashPoint::AfterUndoWrite);
    // 3) manifest tmp + fsync (csWriteFile fsyncs)
    std::vector<uint8_t> mb = m.encode();
    if (!csWriteFile(p("MANIFEST.tmp"), mb.data(), mb.size(), ns(InjectStage::ManifestTmp), ff(InjectStage::ManifestTmp), e)) return false;
    // 4) VISIBILITY: atomic rename
    crashIf(CrashPoint::BeforeRename);
    if (::rename(p("MANIFEST.tmp").c_str(), p("MANIFEST").c_str()) != 0) { setErr(e, ErrCode::FS, "rename MANIFEST"); return false; }
    renamePassed_ = true;
    crashIf(CrashPoint::AfterRename);
    // 5) DURABILITY: dir fsync
    if (inj(InjectStage::DirFsync)) { injErr(e, "dir fsync"); return false; }
    if (!csFsyncDir(dir_, e)) return false;
    crashIf(CrashPoint::AfterDirSync);
    return true;
}

bool AnonStore::connect(const Hash& blockHash, int32_t height,
                        const std::vector<Mutation>& muts, ChainError* e) {
    clearErr(e);
    if (!opened_) { setErr(e, ErrCode::NOT_OPEN, dir_); return false; }
    if (needsReopen_) { setErr(e, ErrCode::NOT_OPEN, "store requires reopen before further mutation"); return false; }
    uint32_t oldRoot = tree_.rootId(); Hash oldHash = tree_.rootHash();
    tree_.clearRetired();
    // apply mutations (CoW)
    for (const Mutation& m : muts) {
        store3a::Key k = makeKey(m.ns, m.key.data());
        if (m.afterPresent) {
            // duplicate-anon-output fail-closed: putting an anon output over a DIFFERENT existing value is refused.
            if (m.ns == ANON_OUTPUT_NS) {
                std::vector<uint8_t> cur; bool cf = false;
                if (!tree_.get(k, &cur, &cf, e)) { ChainError x; tree_.setRoot(oldRoot, oldHash, &x); return false; }
                if (cf && cur != m.after) {
                    setErr(e, ErrCode::DUP_OUTPOINT, "duplicate anon output at key (different value already present)");
                    ChainError x; tree_.setRoot(oldRoot, oldHash, &x);
                    return false;
                }
            }
            if (!tree_.put(k, m.after, e)) { ChainError x; tree_.setRoot(oldRoot, oldHash, &x); return false; }
        } else {
            if (!tree_.erase(k, e)) { ChainError x; tree_.setRoot(oldRoot, oldHash, &x); return false; }
        }
    }
    std::vector<uint8_t> rec = encodeUndo(blockHash, height, muts);
    uint64_t newGen = generation_ + 1;
    uint64_t newUndoSize = undoCommittedBytes_ + rec.size();
    store3a::Manifest m;
    m.generation = newGen; m.undoSize = newUndoSize;
    m.rootPageId = tree_.rootId(); m.rootHash = tree_.rootHash();
    m.blockHash = blockHash; m.tipHeight = height;
    if (!commitBarriers(m, undoCommittedBytes_, rec.data(), rec.size(), e)) {
        if (!renamePassed_) { ChainError x; tree_.setRoot(oldRoot, oldHash, &x); }
        else needsReopen_ = true;
        return false;
    }
    bool ackOk = writeAck(newGen, e);
    if (!ackOk) { needsReopen_ = true; return false; }
    // commit success
    UndoMeta meta; meta.blockHash = blockHash; meta.height = height;
    meta.offset = undoCommittedBytes_; meta.len = (uint32_t)rec.size();
    undoMetas_.push_back(meta);
    generation_ = newGen; undoCommittedBytes_ = newUndoSize;
    blockHash_ = blockHash; tipHeight_ = height;
    if (!recomputeValueCount(e)) { needsReopen_ = true; return false; }
    tree_.retireMovedToFree();
    return true;
}

bool AnonStore::disconnect(const Hash& blockHash, const Hash& parentTipHash, ChainError* e) {
    clearErr(e);
    if (!opened_) { setErr(e, ErrCode::NOT_OPEN, dir_); return false; }
    if (needsReopen_) { setErr(e, ErrCode::NOT_OPEN, "store requires reopen before further mutation"); return false; }
    if (undoMetas_.empty()) {
        setErr(e, ErrCode::NOT_FOUND, "no committed undo record to disconnect (deep beyond retained window)");
        return false;
    }
    const UndoMeta& meta = undoMetas_.back();
    if (meta.blockHash != blockHash) {
        setErr(e, ErrCode::NOT_FOUND, "only the most recent block can be disconnected (mismatch)");
        return false;
    }
    // read the tip undo record (authoritative region) and reverse-apply
    std::vector<uint8_t> body;
    if (!csReadRange(p("undo.dat"), meta.offset, meta.len, &body, e)) return false;
    size_t off = 0; Hash bh; int32_t hh; std::vector<Mutation> muts;
    if (!decodeUndo(body.data(), body.size(), &off, &bh, &hh, &muts, e)) return false;
    if (bh != blockHash) { setErr(e, ErrCode::CORRUPT, "undo block hash mismatch"); return false; }

    uint32_t oldRoot = tree_.rootId(); Hash oldHash = tree_.rootHash();
    tree_.clearRetired();
    for (size_t i = muts.size(); i-- > 0; ) {
        Mutation m = muts[i];
        store3a::Key k = makeKey(m.ns, m.key.data());
        if (m.beforePresent) {
            if (!tree_.put(k, m.before, e)) { ChainError x; tree_.setRoot(oldRoot, oldHash, &x); return false; }
        } else {
            if (!tree_.erase(k, e)) { ChainError x; tree_.setRoot(oldRoot, oldHash, &x); return false; }
        }
    }
    if (!(undoCommittedBytes_ >= meta.len)) { setErr(e, ErrCode::CORRUPT, "undoSize smaller than record"); return false; }
    uint64_t newUndoSize = undoCommittedBytes_ - meta.len;
    uint64_t newGen = generation_ + 1;
    int32_t newHeight = tipHeight_ - 1;
    store3a::Manifest m;
    m.generation = newGen; m.undoSize = newUndoSize;
    m.rootPageId = tree_.rootId(); m.rootHash = tree_.rootHash();
    m.blockHash = parentTipHash; m.tipHeight = newHeight;
    if (!commitBarriers(m, 0 /*no undo append*/, nullptr, 0, e)) {
        if (!renamePassed_) { ChainError x; tree_.setRoot(oldRoot, oldHash, &x); }
        else needsReopen_ = true;
        return false;
    }
    bool ackOk = writeAck(newGen, e);
    if (!ackOk) { needsReopen_ = true; return false; }
    undoMetas_.pop_back();
    generation_ = newGen; undoCommittedBytes_ = newUndoSize;
    blockHash_ = parentTipHash; tipHeight_ = newHeight;
    if (!recomputeValueCount(e)) { needsReopen_ = true; return false; }
    tree_.retireMovedToFree();
    return true;
}

}} // namespace cs::anon