#include "txloc.h"
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <cstring>
#include <algorithm>
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

struct Record {
    unsigned char txid[32];
    Loc loc;
};

static bool cmpTxid(const unsigned char* a, const unsigned char* b) {
    return memcmp(a, b, 32) < 0;   // big-endian byte order = raw memcmp order
}

static bool writeAll(int fd, const unsigned char* p, size_t n, const std::string& what, ChainError* e) {
    size_t off = 0;
    while (off < n) {
        ssize_t w = ::write(fd, p + off, n - off);
        if (w < 0) { if (errno == EINTR) continue; setErr(e, ERR_IO, what + ": write: " + std::string(::strerror(errno))); return false; }
        off += (size_t)w;
    }
    return true;
}

bool buildIndex(const std::string& indexPath,
                const std::function<bool(unsigned char* txidOut, Loc* locOut)>& feed,
                ChainError* e) {
    // Materialize (offline mugnation/convertor-scale; ~48 B/record). Streaming
    // on-disk sort is a later slice for very large feeds.
    std::vector<Record> all;
    unsigned char t[32];
    Loc l;
    while (feed(t, &l)) { Record r; memcpy(r.txid, t, 32); r.loc = l; all.push_back(r); }
    std::sort(all.begin(), all.end(), [](const Record& a, const Record& b){ return cmpTxid(a.txid, b.txid); });
    // Dedupe keep-first (earliest on-chain occurrence).
    std::vector<Record> uniq;
    uniq.reserve(all.size());
    for (const Record& r : all) {
        if (uniq.empty() || memcmp(uniq.back().txid, r.txid, 32) != 0) uniq.push_back(r);
    }
    const uint64_t n = uniq.size();

    std::string dir = indexPath + ".d";
    std::string tmpMain = indexPath + ".tmp";
    std::string tmpDir = dir + ".tmp";

    // Build directory boundary keys.
    std::vector<unsigned char> dirBytes;
    for (uint64_t i = 0; i < n; i += DIR_STEP) dirBytes.insert(dirBytes.end(), uniq[i].txid, uniq[i].txid + 32);

    // Encode main body.
    std::vector<unsigned char> body;
    body.reserve(n * TXLOC_RECWIDTH);
    for (const Record& r : uniq) {
        body.insert(body.end(), r.txid, r.txid + 32);
        putU32(body, r.loc.nFile); putU32(body, r.loc.nBlockPos); putU32(body, r.loc.nTxPos); putU32(body, (uint32_t)r.loc.nHeight);
    }
    Header h; h.recordCount = n;
    std::vector<unsigned char> head;
    putU32(head, h.magic); putU32(head, h.version); putU32(head, h.recordWidth); putU64(head, h.recordCount);

    int f = ::open(tmpMain.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (f < 0) { setErr(e, ERR_IO, "build: open " + tmpMain + ": " + std::string(::strerror(errno))); return false; }
    if (!writeAll(f, head.data(), head.size(), "build", e)) { ::close(f); ::unlink(tmpMain.c_str()); return false; }
    if (!writeAll(f, body.data(), body.size(), "build", e)) { ::close(f); ::unlink(tmpMain.c_str()); return false; }
    if (::fsync(f) != 0) { setErr(e, ERR_IO, "build: fsync " + tmpMain + ": " + std::string(::strerror(errno))); ::close(f); ::unlink(tmpMain.c_str()); return false; }
    ::close(f);

    int fd = ::open(tmpDir.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { setErr(e, ERR_IO, "build: open " + tmpDir + ": " + std::string(::strerror(errno))); ::unlink(tmpMain.c_str()); return false; }
    if (!writeAll(fd, dirBytes.data(), dirBytes.size(), "build", e)) { ::close(fd); ::unlink(tmpMain.c_str()); ::unlink(tmpDir.c_str()); return false; }
    if (::fsync(fd) != 0) { ::close(fd); ::unlink(tmpMain.c_str()); ::unlink(tmpDir.c_str()); return false; }
    ::close(fd);

    if (::rename(tmpDir.c_str(), dir.c_str()) != 0) { setErr(e, ERR_IO, "build: rename dir: " + std::string(::strerror(errno))); ::unlink(tmpMain.c_str()); return false; }
    if (::rename(tmpMain.c_str(), indexPath.c_str()) != 0) { setErr(e, ERR_IO, "build: rename main: " + std::string(::strerror(errno))); return false; }
    return true;
}

TxLocator::~TxLocator() { if (fd_ >= 0) ::close(fd_); }

bool TxLocator::readHeader(ChainError* e) {
    unsigned char h[20];
    (void)!::pread(fd_, h, 20, 0);  // header at offset 0
    uint32_t magic, ver, width; uint64_t cnt;
    getU32(h, &magic); getU32(h + 4, &ver); getU32(h + 8, &width); getU64(h + 12, &cnt);
    if (magic != TXLOC_MAGIC) { setErr(e, ERR_CORRUPT, "txloc: bad magic (not a txloc index)"); return false; }
    if (ver != TXLOC_VERSION) { setErr(e, ERR_CORRUPT, "txloc: unsupported version " + std::to_string(ver)); return false; }
    if (width != TXLOC_RECWIDTH) { setErr(e, ERR_CORRUPT, "txloc: unexpected record width " + std::to_string(width)); return false; }
    struct stat st; if (fstat(fd_, &st) != 0) { setErr(e, ERR_IO, "txloc: fstat: " + std::string(::strerror(errno))); return false; }
    uint64_t expect = 20u + cnt * (uint64_t)TXLOC_RECWIDTH;
    if ((uint64_t)st.st_size < expect) { setErr(e, ERR_CORRUPT, "txloc: truncated main file (size " + std::to_string(st.st_size) + " < expected " + std::to_string(expect) + ")"); return false; }
    recordCount_ = cnt; recordWidth_ = width;
    return true;
}

bool TxLocator::open(const std::string& indexPath, bool* absent, ChainError* e) {
    if (absent) *absent = false;
    if (::access(indexPath.c_str(), F_OK) != 0) {
        if (errno == ENOENT) { if (absent) *absent = true; setErr(e, ERR_NONE, "txloc: index absent (rebuild by scan)"); return false; }
        setErr(e, ERR_IO, "txloc: access: " + std::string(::strerror(errno))); return false;
    }
    fd_ = ::open(indexPath.c_str(), O_RDONLY);
    if (fd_ < 0) { setErr(e, ERR_IO, "txloc: open: " + std::string(::strerror(errno))); return false; }
    indexPath_ = indexPath;
    if (!readHeader(e)) { ::close(fd_); fd_ = -1; return false; }
    return true;   // directory (.d) is advisory; get() uses binary search.
}

void TxLocator::close() { if (fd_ >= 0) { ::close(fd_); fd_ = -1; } recordCount_ = 0; }

bool TxLocator::readRecord(uint64_t idx, unsigned char* txidOut, Loc* locOut, ChainError* e) const {
    unsigned char buf[TXLOC_RECWIDTH];
    ssize_t r = ::pread(fd_, buf, TXLOC_RECWIDTH, (off_t)(20 + idx * TXLOC_RECWIDTH));
    if (r != TXLOC_RECWIDTH) { setErr(e, ERR_IO, "txloc: pread record (truncated/IO)"); return false; }
    if (txidOut) memcpy(txidOut, buf, 32);
    if (locOut) { uint32_t h; Loc l; getU32(buf + 32, &l.nFile); getU32(buf + 36, &l.nBlockPos); getU32(buf + 40, &l.nTxPos); getU32(buf + 44, &h); l.nHeight = (int32_t)h; *locOut = l; }
    return true;
}

bool TxLocator::get(const unsigned char* txid, Loc* out, bool* found, ChainError* e) const {
    if (found) *found = false;
    if (fd_ < 0) { setErr(e, ERR_NOT_OPEN, "txloc: not open"); return false; }
    if (recordCount_ == 0) { return true; }   // empty index, not found
    int64_t lo = 0, hi = (int64_t)recordCount_ - 1;
    while (lo <= hi) {
        int64_t mid = lo + (hi - lo) / 2;
        unsigned char k[32]; Loc dummy;
        ChainError ee;
        if (!readRecord((uint64_t)mid, k, &dummy, &ee)) { *e = ee; return false; }
        int c = memcmp(txid, k, 32);
        if (c == 0) { if (found) { *found = true; if (out) { Loc lc; readRecord((uint64_t)mid, NULL, &lc, &ee); *out = lc; } } return true; }
        if (c < 0) hi = mid - 1; else lo = mid + 1;
    }
    return true;   // not found, no error
}

} // namespace txloc