#include "txloc.h"
#include <fcntl.h>
#include <unistd.h>
#include <sstream>
#include <errno.h>
#include <cstring>
#include <algorithm>
#include <mutex>
#include <sys/stat.h>

namespace txloc {

static void setErr(ChainError* e, int code, const std::string& m) {
    if (e) { e->code = code; e->msg = m; }
}
static bool putU32(std::vector<unsigned char>& o, uint32_t v) {
    for (int i = 0; i < 4; i++) o.push_back((unsigned char)((v >> (8*i)) & 0xFF));
    return true;
}
static bool putU64(std::vector<unsigned char>& o, uint64_t v) {
    for (int i = 0; i < 8; i++) o.push_back((unsigned char)((v >> (8*i)) & 0xFF));
    return true;
}
static bool getU32(const unsigned char* p, uint32_t* v) {
    uint32_t r = 0; for (int i = 0; i < 4; i++) r |= ((uint32_t)p[i]) << (8*i); *v = r; return true;
}
static bool getU64(const unsigned char* p, uint64_t* v) {
    uint64_t r = 0; for (int i = 0; i < 8; i++) r |= ((uint64_t)p[i]) << (8*i); *v = r; return true;
}
static void encodeLoc(const Loc& l, unsigned char* p) {
    for (int i = 0; i < 4; i++){ p[32+i] = (unsigned char)((l.nFile >> (8*i)) & 0xFF); p[36+i] = (unsigned char)((l.nBlockPos >> (8*i)) & 0xFF); p[40+i] = (unsigned char)((l.nTxPos >> (8*i)) & 0xFF); }
    uint32_t h = (uint32_t)l.nHeight; for (int i = 0; i < 4; i++) p[44+i] = (unsigned char)((h >> (8*i)) & 0xFF);
}
static void decodeLoc(const unsigned char* p, Loc* l) {
    uint32_t a,b,c,d; getU32(p+32,&a); getU32(p+36,&b); getU32(p+40,&c); getU32(p+44,&d);
    l->nFile=a; l->nBlockPos=b; l->nTxPos=c; l->nHeight=(int32_t)d;
}
static void putU32Bytes(unsigned char* p, uint32_t v){ for(int i=0;i<4;i++) p[i]=(unsigned char)((v>>(8*i))&0xFF); }
static void putU64Bytes(unsigned char* p, uint64_t v){ for(int i=0;i<8;i++) p[i]=(unsigned char)((v>>(8*i))&0xFF); }
static void encodeHeader(const Header& h, unsigned char* p) { putU32Bytes(p, h.magic); putU32Bytes(p+4, h.version); putU32Bytes(p+8, h.recordWidth); putU64Bytes(p+12, h.recordCount); }

struct Record { unsigned char txid[32]; Loc loc; };
static bool cmpTxid(const unsigned char* a, const unsigned char* b) { return memcmp(a, b, 32) < 0; }

static bool writeAll(int fd, const unsigned char* p, size_t n, const std::string& what, ChainError* e) {
    size_t off = 0;
    while (off < n) {
        ssize_t w = ::write(fd, p + off, n - off);
        if (w < 0) { if (errno == EINTR) continue; setErr(e, ERR_IO, what + ": write: " + std::string(::strerror(errno))); return false; }
        off += (size_t)w;
    }
    return true;
}

// Bounded-RAM bulk build (Item 1 slice 2 bulk path):
//
// The feed is spilled to chunk files in FEED ORDER; each chunk is sorted + deduped
// in RAM slice-by-slice, then the final sorted/unique record stream is produced by
// a k-way merge (ties broken toward the EARLIER chunk, i.e. the first fed
// occurrence = keep-first, exactly the documented semantics). Peak RAM is one
// chunk of records (TXLOC_BULK_CHUNK_MAX records), never the whole index.
//
// Output remains: atomic temp+rename of txloc.dat (header + sorted unique
// records) and txloc.dir (one boundary key every DIR_STEP records).
static const size_t TXLOC_BULK_CHUNK_MAX = 4u * 1024u * 1024u;   // 4M records (~192 MiB of Loc slots)

// One sorted chunk spill file: raw 48B records, ascending by txid.
struct BulkChunk {
    std::string path;
    uint64_t count = 0;
};
static bool bulkChunkOpenWrite(const std::string& path, int* outFd) {
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return false;
    *outFd = fd;
    return true;
}
static bool bulkChunkFlush(std::vector<Record>& buf, const std::string& path) {
    std::sort(buf.begin(), buf.end(), [](const Record& a, const Record& b){ return memcmp(a.txid, b.txid, 32) < 0; });
    int fd = -1;
    if (!bulkChunkOpenWrite(path, &fd)) return false;
    bool ok = true;
    for (const Record& r : buf) {
        unsigned char out[TXLOC_RECWIDTH];
        memcpy(out, r.txid, 32); encodeLoc(r.loc, out);
        if (!writeAll(fd, out, TXLOC_RECWIDTH, "bulk spill", 0)) { ok = false; break; }
    }
    if (::fsync(fd) != 0) ok = false;
    ::close(fd);
    return ok;
}
// Merge reader over one sorted chunk file.
struct BulkChunkReader {
    int fd = -1;
    uint64_t count = 0, pos = 0;
    Record cur;
    bool valid = false;
};
static bool bulkChunkReaderNext(BulkChunkReader* r, ChainError* e) {
    if (r->pos >= r->count) { r->valid = false; return true; }
    unsigned char buf[TXLOC_RECWIDTH];
    ssize_t n = ::pread(r->fd, buf, TXLOC_RECWIDTH, (off_t)(r->pos * TXLOC_RECWIDTH));
    if (n != (ssize_t)TXLOC_RECWIDTH) { setErr(e, ERR_IO, "build: chunk pread failed"); return false; }
    memcpy(r->cur.txid, buf, 32); decodeLoc(buf, &r->cur.loc);
    r->pos++; r->valid = true;
    return true;
}
// Legacy format layout reference: header count + size are written last (after
// the record body fsyncs), then dir; both renames follow. Unchanged from the
// previous implementation; recorded here so the invariant stays auditable:
//   main: open(tmp) -> write header(20, count=0) -> write records -> fsync -> close
//   dir : open(tmp) -> write boundary keys -> fsync -> close
//   rename dir tmp->final, then rename main tmp->final.
bool buildIndex(const std::string& indexPath,
                const std::function<bool(unsigned char* txidOut, Loc* locOut)>& feed,
                ChainError* e) {
    // Phase 1: spill feed into sorted chunks (bounded RAM).
    std::vector<Record> buf;
    buf.reserve(TXLOC_BULK_CHUNK_MAX < 65536u ? TXLOC_BULK_CHUNK_MAX : 65536u);  // grow later
    std::vector<BulkChunk> chunks;
    std::string dirBase = indexPath + ".bulktmp";
    // target chunk dir on same fs as final rename (indexPath dir)
    for (;;) {
        bool more = false;
        unsigned char t[32]; Loc l;
        while (buf.size() < TXLOC_BULK_CHUNK_MAX) {
            if (!feed(t, &l)) { more = false; goto spill_tail; }
            Record r; memcpy(r.txid, t, 32); r.loc = l;
            buf.push_back(r);
        }
        {
            more = true;
            BulkChunk c; {
                std::ostringstream ss; ss << dirBase << ".chunk" << chunks.size();
                c.path = ss.str();
            }
            if (!bulkChunkFlush(buf, c.path)) { setErr(e, ERR_IO, "build: chunk spill failed"); return false; }
            c.count = buf.size();
            chunks.push_back(c);
            buf.clear();
        }
    spill_tail:
        if (!more && !buf.empty()) {
            BulkChunk c; {
                std::ostringstream ss; ss << dirBase << ".chunk" << chunks.size();
                c.path = ss.str();
            }
            if (!bulkChunkFlush(buf, c.path)) { setErr(e, ERR_IO, "build: final chunk spill failed"); return false; }
            c.count = buf.size();
            chunks.push_back(c);
            buf.clear();
        }
        if (!more && buf.empty()) break;
    }

    // Phase 2: k-way merge the sorted chunks, dedup keep-first, emit the sorted
    // main file + directory atomically (identical layout to the original
    // implementation).
    std::vector<BulkChunkReader> readers(chunks.size());
    for (size_t i = 0; i < chunks.size(); i++) {
        readers[i].fd = ::open(chunks[i].path.c_str(), O_RDONLY);
        readers[i].count = chunks[i].count;
        if (readers[i].fd < 0) { setErr(e, ERR_IO, "build: open chunk failed"); return false; }
        ChainError se;
        if (!bulkChunkReaderNext(&readers[i], &se)) { *e = se; return false; }  // preload first record
    }

    std::string dir = indexPath + ".d";
    std::string tmpMain = indexPath + ".tmp";
    std::string tmpDir = dir + ".tmp";
    ::unlink(tmpDir.c_str());

    int f = ::open(tmpMain.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (f < 0) { setErr(e, ERR_IO, "build: open " + tmpMain + ": " + std::string(::strerror(errno))); return false; }
    Header h; h.recordCount = 0;
    unsigned char head[20]; encodeHeader(h, head);
    if (!writeAll(f, head, 20, "build", e)) { ::close(f); ::unlink(tmpMain.c_str()); return false; }

    int fd = ::open(tmpDir.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        setErr(e, ERR_IO, "build: open " + tmpDir + ": " + std::string(::strerror(errno)));
        ::close(f); ::unlink(tmpMain.c_str());
        return false;
    }

    std::vector<unsigned char> dirBytes;
    uint64_t n = 0;
    Record hold;  // the current candidate record (the next unique one to emit)
    bool haveHold = false;
    for (;;) {
        // Find the smallest txid among the current reader cursors.
        int best = -1;
        for (size_t i = 0; i < readers.size(); i++) {
            if (!readers[i].valid) continue;
            if (best < 0) { best = (int)i; continue; }
            int c = memcmp(readers[i].cur.txid, readers[best].cur.txid, 32);
            if (c < 0 || (c == 0 && i < (size_t)best)) best = (int)i;  // tie -> earlier chunk (keep-first)
        }
        if (best < 0) break;
        Record cand = readers[best].cur;
        if (haveHold && memcmp(hold.txid, cand.txid, 32) == 0) {
            // duplicate -> keep-first (hold was fed earlier; already held). Skip.
            ChainError se; if (!bulkChunkReaderNext(&readers[best], &se)) { ::close(f); ::close(fd); *e = se; return false; }
            continue;
        }
        // Emit hold, become cand.
        if (haveHold) {
            unsigned char out[TXLOC_RECWIDTH];
            memcpy(out, hold.txid, 32); encodeLoc(hold.loc, out);
            if (!writeAll(f, out, TXLOC_RECWIDTH, "build", e)) { ::close(f); ::close(fd); return false; }
            if (n % DIR_STEP == 0)
                dirBytes.insert(dirBytes.end(), hold.txid, hold.txid + 32);
            n++;
        }
        hold = cand; haveHold = true;
        ChainError se; if (!bulkChunkReaderNext(&readers[best], &se)) { ::close(f); ::close(fd); *e = se; return false; }
    }
    if (haveHold) {
        unsigned char out[TXLOC_RECWIDTH];
        memcpy(out, hold.txid, 32); encodeLoc(hold.loc, out);
        if (!writeAll(f, out, TXLOC_RECWIDTH, "build", e)) { ::close(f); ::close(fd); return false; }
        if (n % DIR_STEP == 0)
            dirBytes.insert(dirBytes.end(), hold.txid, hold.txid + 32);
        n++;
    }

    if (!writeAll(fd, dirBytes.data(), dirBytes.size(), "build", e)) {
        ::close(f); ::close(fd); ::unlink(tmpMain.c_str()); ::unlink(tmpDir.c_str()); return false;
    }
    if (::fsync(fd) != 0) { setErr(e, ERR_IO, "build: fsync dir"); ::close(f); ::close(fd); ::unlink(tmpMain.c_str()); ::unlink(tmpDir.c_str()); return false; }
    ::close(fd);

    // header recordCount is rewritten after the record body is durable (same
    // final-encode invariant as the original implementation, which knew N up
    // front; here the count is only known once the merge completes).
    h.recordCount = n; encodeHeader(h, head);
    if (::pwrite(f, head, 20, 0) != 20) { setErr(e, ERR_IO, "build: header rewrite"); ::close(f); ::unlink(tmpMain.c_str()); ::unlink(tmpDir.c_str()); return false; }
    if (::fsync(f) != 0) { setErr(e, ERR_IO, "build: fsync main"); ::close(f); ::unlink(tmpMain.c_str()); ::unlink(tmpDir.c_str()); return false; }
    ::close(f);

    if (::rename(tmpDir.c_str(), dir.c_str()) != 0) { setErr(e, ERR_IO, "build: rename dir: " + std::string(::strerror(errno))); ::unlink(tmpMain.c_str()); return false; }
    if (::rename(tmpMain.c_str(), indexPath.c_str()) != 0) { setErr(e, ERR_IO, "build: rename main: " + std::string(::strerror(errno))); return false; }

    // Cleanup spill files (best-effort; never fatal).
    for (size_t i = 0; i < chunks.size(); i++) ::unlink(chunks[i].path.c_str());
    ::rmdir(dirBase.c_str());
    return true;
}

TxLocator::~TxLocator() { close(); delete mem_; }

// Global production locator handle (defined here; opened by init.cpp V2 arm).
TxLocator g_txloc;

bool TxLocator::readHeader(ChainError* e) {
    unsigned char h[20];
    ssize_t r = ::pread(fd_, h, 20, 0);
    if (r != 20) { setErr(e, ERR_CORRUPT, "txloc: short header"); return false; }
    uint32_t magic, ver, width; uint64_t cnt;
    getU32(h, &magic); getU32(h + 4, &ver); getU32(h + 8, &width); getU64(h + 12, &cnt);
    if (magic != TXLOC_MAGIC) { setErr(e, ERR_CORRUPT, "txloc: bad magic"); return false; }
    if (ver != TXLOC_VERSION) { setErr(e, ERR_CORRUPT, "txloc: unsupported version " + std::to_string(ver)); return false; }
    if (width != TXLOC_RECWIDTH) { setErr(e, ERR_CORRUPT, "txloc: unexpected record width " + std::to_string(width)); return false; }
    struct stat st; if (fstat(fd_, &st) != 0) { setErr(e, ERR_IO, "txloc: fstat"); return false; }
    uint64_t expect = 20u + cnt * (uint64_t)TXLOC_RECWIDTH;
    if ((uint64_t)st.st_size < expect) { setErr(e, ERR_CORRUPT, "txloc: truncated main file (size " + std::to_string(st.st_size) + " < expected " + std::to_string(expect) + ")"); return false; }
    recordCount_ = cnt; recordWidth_ = width;
    return true;
}

bool TxLocator::readMain(ChainError* e) {
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    fd_ = ::open(mainPath_.c_str(), O_RDONLY);
    if (fd_ < 0) { setErr(e, ERR_IO, "txloc: open main: " + std::string(::strerror(errno))); return false; }
    if (!readHeader(e)) { ::close(fd_); fd_ = -1; return false; }
    hadMain_ = true;
    return true;
}

bool TxLocator::open(const std::string& indexPath, bool* absent, ChainError* e) {
    if (absent) *absent = false;
    close();
    if (::access(indexPath.c_str(), F_OK) != 0) {
        if (errno == ENOENT) { if (absent) *absent = true; setErr(e, ERR_NONE, "txloc: index absent (rebuild by scan)"); return false; }
        setErr(e, ERR_IO, "txloc: access: " + std::string(::strerror(errno))); return false;
    }
    fd_ = ::open(indexPath.c_str(), O_RDONLY);
    if (fd_ < 0) { setErr(e, ERR_IO, "txloc: open: " + std::string(::strerror(errno))); return false; }
    mainPath_ = indexPath; hadMain_ = true;
    if (!readHeader(e)) { ::close(fd_); fd_ = -1; return false; }
    return true;
}

static bool mkdirs(const std::string& dir) {
    if (dir.empty()) return false;
    std::string cur;
    size_t i = 0;
    std::string rest = dir;
    if (!rest.empty() && rest[0] == '/') cur = "/";
    while (i < rest.size()) {
        size_t sl = rest.find('/', i);
        if (sl == std::string::npos) sl = rest.size();
        if (sl > i) {
            std::string piece = rest.substr(i, sl - i);
            cur = (cur == "/" || cur.empty()) ? ("/" + piece) : (cur + "/" + piece);
            if (cur == "//") cur = "/";
            if (::mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) return false;
        }
        i = sl + 1;
    }
    return true;
}

bool TxLocator::openDir(const std::string& dir, bool* absent, ChainError* e) {
    if (absent) *absent = false;
    close();
    if (!mkdirs(dir)) { setErr(e, ERR_IO, "txloc: cannot create dir " + dir); return false; }
    dir_ = dir;
    mainPath_ = dir + "/txloc.dat"; walPath_ = dir + "/txloc.dat.wal"; tmpPath_ = dir + "/txloc.dat.tmp";
    ::unlink(tmpPath_.c_str());                       // orphan from an interrupted compaction
    bool mainAbsent = (::access(mainPath_.c_str(), F_OK) != 0);
    if (mainAbsent) { hadMain_ = false; recordCount_ = 0; }
    else if (!readMain(e)) { return false; }

    walFd_ = ::open(walPath_.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (walFd_ < 0) { setErr(e, ERR_IO, "txloc: open wal: " + std::string(::strerror(errno))); return false; }
    if (!mem_) mem_ = new std::map<Key, Loc, KeyCmp>();

    // Replay the WAL into the memtable (crash recovery of the durable tail).
    {
        int rf = ::open(walPath_.c_str(), O_RDONLY);
        if (rf >= 0) {
            unsigned char buf[TXLOC_RECWIDTH];
            for (;;) {
                ssize_t n = ::read(rf, buf, TXLOC_RECWIDTH);
                if (n <= 0) { if (n < 0) { setErr(e, ERR_IO, "txloc: wal read error"); ::close(rf); return false; } break; }
                if (n < (ssize_t)TXLOC_RECWIDTH) break;   // torn trailing write; discard (crash)
                Key k; memcpy(k.b, buf, 32); Loc l; decodeLoc(buf, &l);
                if (mem_->find(k) == mem_->end()) (*mem_)[k] = l;
            }
            struct stat wst; if (fstat(rf, &wst) == 0 && wst.st_size % TXLOC_RECWIDTH != 0) {
                uint64_t clean = (uint64_t)(wst.st_size / TXLOC_RECWIDTH) * TXLOC_RECWIDTH;
                ::ftruncate(rf, (off_t)clean);
            }
            ::close(rf);
        }
    }
    if (absent) *absent = (!hadMain_ && mem_->empty());
    return true;
}

bool TxLocator::add(const unsigned char* txid, const Loc& loc, ChainError* e) {
    if (!mem_ || walFd_ < 0) { setErr(e, ERR_NOT_OPEN, "txloc: not open for add"); return false; }
    std::lock_guard<std::mutex> lk(mtx_);
    Key k; memcpy(k.b, txid, 32);
    if (mem_->find(k) != mem_->end()) return true;       // keep-first (re-add ignored)
    unsigned char buf[TXLOC_RECWIDTH]; memcpy(buf, txid, 32); encodeLoc(loc, buf);
    if (!writeAll(walFd_, buf, TXLOC_RECWIDTH, "add", e)) return false;
    (*mem_)[k] = loc;
    if (mem_->size() >= MEMTABLE_MAX) { ChainError ce; if (!writeMergedTrimWAL(&ce)) { *e = ce; return false; } }
    return true;
}

bool TxLocator::sync(ChainError* e) {
    if (walFd_ < 0) { setErr(e, ERR_NOT_OPEN, "txloc: not open"); return false; }
    std::lock_guard<std::mutex> lk(mtx_);
    if (::fsync(walFd_) != 0) { setErr(e, ERR_IO, "txloc: wal fsync"); return false; }
    return true;
}

bool TxLocator::compact(ChainError* e) {
    if (!mem_) { setErr(e, ERR_NOT_OPEN, "txloc: not open"); return false; }
    std::lock_guard<std::mutex> lk(mtx_);
    return writeMergedTrimWAL(e);
}

static bool appendRawRec(int tf, const unsigned char* key, const Loc& l, const std::string& what, ChainError* e) {
    unsigned char out[48]; memcpy(out, key, 32); encodeLoc(l, out);
    return writeAll(tf, out, 48, what, e);
}

bool TxLocator::writeMergedTrimWAL(ChainError* e) {
    int tf = ::open(tmpPath_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (tf < 0) { setErr(e, ERR_IO, "txloc: open tmp: " + std::string(::strerror(errno))); return false; }
    unsigned char head[20]; Header h; h.recordCount = 0; encodeHeader(h, head);
    if (!writeAll(tf, head, 20, "compact", e)) { ::close(tf); return false; }

    uint64_t merged = 0;
    unsigned char mkey[32]; Loc mloc; ChainError se;
    if (hadMain_) {
        uint64_t mi = 0;
        auto it = mem_->begin();
        while (mi < recordCount_ || it != mem_->end()) {
            bool haveM = (mi < recordCount_);
            const Key* k = (it != mem_->end()) ? &it->first : NULL;
            if (haveM && k) {
                if (!readRecord(mi, mkey, &mloc, &se)) { ::close(tf); *e = se; return false; }
                int c = memcmp(mkey, k->b, 32);
                if (c < 0) {                                        // main earlier -> keep main, advance main
                    if (!appendRawRec(tf, mkey, mloc, "compact", e)) { ::close(tf); return false; }
                    mi++; merged++;
                } else if (c > 0) {                                // memtable earlier -> keep, advance memtable
                    if (!appendRawRec(tf, it->first.b, it->second, "compact", e)) { ::close(tf); return false; }
                    ++it; merged++;
                } else {                                            // TIE same txid -> keep FIRST (main), skip memtable dup
                    if (!appendRawRec(tf, mkey, mloc, "compact", e)) { ::close(tf); return false; }
                    mi++; ++it; merged++;
                }
            } else if (haveM) {
                if (!readRecord(mi, mkey, &mloc, &se)) { ::close(tf); *e = se; return false; }
                if (!appendRawRec(tf, mkey, mloc, "compact", e)) { ::close(tf); return false; }
                mi++; merged++;
            } else {
                if (!appendRawRec(tf, it->first.b, it->second, "compact", e)) { ::close(tf); return false; }
                ++it; merged++;
            }
        }
    } else {
        for (auto& kv : *mem_) { if (!appendRawRec(tf, kv.first.b, kv.second, "compact", e)) { ::close(tf); return false; } merged++; }
    }
    h.recordCount = merged; encodeHeader(h, head);
    if (::pwrite(tf, head, 20, 0) != 20) { setErr(e, ERR_IO, "txloc: compact count rewrite"); ::close(tf); return false; }
    if (::fsync(tf) != 0) { setErr(e, ERR_IO, "txloc: compact fsync"); ::close(tf); return false; }
    ::close(tf);
    if (::rename(tmpPath_.c_str(), mainPath_.c_str()) != 0) { setErr(e, ERR_IO, "txloc: compact rename: " + std::string(::strerror(errno))); return false; }

    if (walFd_ >= 0) { if (::ftruncate(walFd_, 0) != 0) { setErr(e, ERR_IO, "txloc: wal truncate"); return false; } if (::lseek(walFd_, 0, SEEK_SET) < 0 || ::fsync(walFd_) != 0) { setErr(e, ERR_IO, "txloc: wal fsync after truncate"); return false; } }
    mem_->clear();

    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    fd_ = ::open(mainPath_.c_str(), O_RDONLY);
    if (fd_ < 0) { setErr(e, ERR_IO, "txloc: reopen main: " + std::string(::strerror(errno))); return false; }
    recordCount_ = merged; recordWidth_ = TXLOC_RECWIDTH; hadMain_ = true;
    return true;
}

void TxLocator::close() {
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    if (walFd_ >= 0) { ::close(walFd_); walFd_ = -1; }
    recordCount_ = 0; hadMain_ = false;
}

uint64_t TxLocator::memCount() const { return mem_ ? mem_->size() : 0; }
size_t TxLocator::memTableBytes() const { return mem_ ? mem_->size() * (32 + sizeof(Loc)) : 0; }

bool TxLocator::readRecord(uint64_t idx, unsigned char* txidOut, Loc* locOut, ChainError* e) const {
    unsigned char buf[TXLOC_RECWIDTH];
    ssize_t r = ::pread(fd_, buf, TXLOC_RECWIDTH, (off_t)(20 + idx * TXLOC_RECWIDTH));
    if (r != (ssize_t)TXLOC_RECWIDTH) { setErr(e, ERR_IO, "txloc: pread record (truncated/IO)"); return false; }
    if (txidOut) memcpy(txidOut, buf, 32);
    if (locOut) decodeLoc(buf, locOut);
    return true;
}

bool TxLocator::get(const unsigned char* txid, Loc* out, bool* found, ChainError* e) const {
    if (found) *found = false;
    if (fd_ < 0 && mem_ == NULL) { setErr(e, ERR_NOT_OPEN, "txloc: not open"); return false; }   // truly unopened
    std::lock_guard<std::mutex> lk(mtx_);
    if (mem_) {
        Key k; memcpy(k.b, txid, 32);
        auto it = mem_->find(k);
        if (it != mem_->end()) { if (found) { *found = true; if (out) *out = it->second; } return true; }
        if (fd_ < 0) return true;                 // memtable-only locator, not in it
    }
    if (fd_ < 0 || recordCount_ == 0) return true;
    int64_t lo = 0, hi = (int64_t)recordCount_ - 1;
    while (lo <= hi) {
        int64_t mid = lo + (hi - lo) / 2;
        unsigned char k[32]; Loc dummy; ChainError ee;
        if (!readRecord((uint64_t)mid, k, &dummy, &ee)) { *e = ee; return false; }
        int c = memcmp(txid, k, 32);
        if (c == 0) { if (found) { *found = true; if (out) readRecord((uint64_t)mid, NULL, out, &ee); } return true; }
        if (c < 0) hi = mid - 1; else lo = mid + 1;
    }
    return true;
}

} // namespace txloc