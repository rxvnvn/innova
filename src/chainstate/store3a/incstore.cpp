// Stage 3A - incremental chainstate store implementation.
// Carries the audited Stage-2B commit / backup / ACK / fail-loud recovery protocol,
// backed by the paged CoW B+tree (O(delta) commit instead of a full-state rewrite).
#include "incstore.h"
#include "incdelta.h"
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <time.h>
#include <cerrno>
#include <cstring>
#include <algorithm>

namespace cs { namespace store3a {

static const size_t UNDO_HEADER_SIZE = 52;
static const size_t ACK_SIZE = 12;

uint32_t csCrc32(const uint8_t* d, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) { c ^= d[i]; for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(c & 1))); }
    return c ^ 0xFFFFFFFFu;
}
static bool ensureDir(const std::string& path) {
    if (path.empty()) return false;
    struct stat st; if (stat(path.c_str(), &st) == 0) return S_ISDIR(st.st_mode);
    std::string parent = path; size_t pos = parent.find_last_of('/');
    if (pos != std::string::npos && pos > 0) ensureDir(parent.substr(0, pos));
    if (mkdir(path.c_str(), 0755) == 0) return true; return errno == EEXIST;
}
static bool endsWith(const std::string& s, const std::string& suf) { return s.size() >= suf.size() && s.compare(s.size()-suf.size(), suf.size(), suf) == 0; }

// R1 (F0-2): classify an on-disk undo.dat as evidence of a prior store BEFORE anything is created,
// truncated or overwritten on the fresh-open path. lstat (never stat) so a symlink is reported as a
// symlink instead of being silently followed to some other file.
//   0 = absent, 1 = present and empty (no byte can be destroyed: accepted residue), 2 = present with
//   bytes (prior-store evidence: REFUSE), 3 = present but not a regular file (REFUSE), -1 = lstat
//   failed for any reason other than ENOENT (REFUSE: an unreadable path is not file absence).
// Pure: no filesystem mutation, deterministic diagnostic in *detail.
static int classifyUndoEvidence(const std::string& path, std::string* detail){
    struct stat st;
    if (::lstat(path.c_str(), &st) != 0){
        if (errno == ENOENT){ *detail = "absent"; return 0; }
        *detail = "lstat failed (errno=" + std::to_string(errno) + ")"; return -1;
    }
    if (S_ISLNK(st.st_mode)){ *detail = "symbolic link"; return 3; }
    if (!S_ISREG(st.st_mode)){
        *detail = S_ISDIR(st.st_mode) ? "directory" : (S_ISFIFO(st.st_mode) ? "fifo" : "non-regular file");
        return 3;
    }
    // R2 (F0-2-R1): the caller pins the classified object by descriptor; see csWriteAtFile.
    if (st.st_size == 0){
        // R1: a zero-length file can be adopted (nothing can be destroyed), but only if it is actually
        // usable: an empty-yet-inaccessible log is an anomaly, not evidence of a clean store. R2 refuses
        // an unreadable tree.dat at the same point, so this is the consistent fail-closed reading.
        if (::access(path.c_str(), R_OK|W_OK) != 0){
            *detail = "empty regular file that is not readable+writable (errno=" + std::to_string(errno) + ")";
            return 3;
        }
        *detail = "empty regular file"; return 1;
    }
    *detail = std::to_string((long long)st.st_size) + " bytes";
    return 2;
}

bool csWriteFile(const std::string& path, const uint8_t* d, size_t n, bool injNs, bool injFs, ChainError* e) {
    if (injNs) { setErr(e, ErrCode::NO_SPACE, "injected ENOSPC on " + path); return false; }
    int fd = ::open(path.c_str(), O_WRONLY|O_CREAT|O_TRUNC, 0644);
    if (fd < 0) { setErr(e, ErrCode::IO, "open " + path); return false; }
    size_t off=0; while (off<n){ ssize_t w=::write(fd,d+off,n-off); if(w<=0){ ::close(fd); setErr(e,ErrCode::IO,"write "+path); return false;} off+=(size_t)w; }
    if (injFs) { ::close(fd); setErr(e, ErrCode::FS, "injected fsync failure on " + path); return false; }
    if (::fsync(fd)!=0){ ::close(fd); setErr(e,ErrCode::FS,"fsync "+path); return false; }
    if (::close(fd)!=0){ setErr(e,ErrCode::FS,"close "+path); return false; }
    return true;
}
bool csAppendFile(const std::string& path, const uint8_t* d, size_t n, bool injNs, bool injFs, ChainError* e) {
    if (injNs) { setErr(e, ErrCode::NO_SPACE, "injected ENOSPC append " + path); return false; }
    int fd = ::open(path.c_str(), O_WRONLY|O_CREAT|O_APPEND, 0644);
    if (fd < 0) { setErr(e, ErrCode::IO, "open " + path); return false; }
    size_t off=0; while (off<n){ ssize_t w=::write(fd,d+off,n-off); if(w<=0){ ::close(fd); setErr(e,ErrCode::IO,"append "+path); return false;} off+=(size_t)w; }
    if (injFs) { ::close(fd); setErr(e, ErrCode::FS, "injected fsync failure append " + path); return false; }
    if (::fsync(fd)!=0){ ::close(fd); setErr(e,ErrCode::FS,"fsync append "+path); return false; }
    ::close(fd); return true;
}
// F0 (undo offset coherence): write the undo record at the COMMITTED logical offset.
// - Never truncates and never moves the write offset implicitly (no O_APPEND): the durable bytes
//   [0, off) are left byte-identical, so no manifest (current primary or MANIFEST.bak fallback) can
//   lose record bytes it references.
// - Only bytes at or beyond the committed logical end are ever (re)written: those are exactly the
//   records logically discarded by an earlier disconnect, which no retained manifest references.
// - Refuses to write past a hole (file shorter than off) instead of zero-extending into a
//   descriptor whose record layout would no longer match the manifest.
bool csWriteAtFile(const std::string& path, uint64_t off, const uint8_t* d, size_t n, bool injNs, bool injFs, ChainError* e,
                   UndoCreate create, int pinnedFd, int* holdFd) {
    if (injNs) { setErr(e, ErrCode::NO_SPACE, "injected ENOSPC writing " + path); return false; }
    // R1 (F0-2): the create policy is enforced by the OPEN, not by a preceding lstat, so a file that
    // appears (or disappears) between the open-time classification and this write cannot slip through.
    // R2 (F0-2-R1): a fresh-open policy additionally PINS the write to the object classified at open time
    // (the empty residue) or to the object this call creates itself. That pin is enforced on the very
    // descriptor used for the write, never on a second pathname lookup:
    //   O_NOFOLLOW  - a symlink substituted at the pathname is never followed (ELOOP);
    //   O_NONBLOCK  - a FIFO substituted at the pathname cannot block this process (ENXIO);
    //   fstat(fd)   - the descriptor itself must be regular, still empty, and carry dev/ino/ctime equal to
    //                 the classification record: no substituted inode is ever written;
    //   lstat(name) - before the write and again after the fsync, the pathname must still denote that same
    //                 descriptor, so the bytes that become durable are the bytes reachable under the name.
    // Ordering note: the O_EXCL open for RequireAbsent and the fstat/lstat pair for RequireExisting are the
    // last checks before pwrite. A concurrent swapper can therefore only cause a fail-closed refusal; it can
    // never redirect or corrupt a write, because the write's target is the validated descriptor itself.
    const bool pinned = (create != UndoCreate::Any);
    int fd = -1; bool owned = false;
    if (create == UndoCreate::RequireExisting) {
        // R2 (F0-2-R1): the accepted empty residue was pinned by descriptor at fresh-open classification.
        // The write therefore never resolves the pathname to find its target: it uses that descriptor.
        if (pinnedFd < 0) {
            setErr(e, ErrCode::CORRUPT, path + ": the fresh-open classification did not pin a descriptor for "
                   "the accepted empty residue; refusing to write an unverified log (identity mismatch)");
            return false;
        }
        fd = pinnedFd; owned = false;                 // caller owns the pinned descriptor
    } else {
        int flags = O_WRONLY;
        if (create == UndoCreate::Any) flags |= O_CREAT;
        else flags |= O_CREAT | O_EXCL;               // RequireAbsent: kernel-atomic create, no lstat race
        fd = ::open(path.c_str(), flags, 0644);
        if (fd < 0) {
            const int err = errno;
            if (create == UndoCreate::RequireAbsent && err == EEXIST) {
                setErr(e, ErrCode::CORRUPT, path + " appeared after the fresh-open classification; refusing to "
                       "overwrite a file this store never created (prior-store evidence)");
            } else {
                setErr(e, ErrCode::IO, "open " + path + " (errno=" + std::to_string(err) + ")");
            }
            return false;
        }
        owned = true;
    }
    auto shut = [&](){ if (owned) ::close(fd); };
    struct stat fst;
    if (::fstat(fd, &fst) != 0) { int er=errno; shut(); setErr(e, ErrCode::IO, "fstat " + path + " (errno=" + std::to_string(er) + ")"); return false; }
    if (pinned) {
        if (!S_ISREG(fst.st_mode)) {
            shut();
            setErr(e, ErrCode::CORRUPT, path + " resolved to a non-regular file; refusing to write a "
                   "substituted pathname (identity mismatch)");
            return false;
        }
        if (fst.st_nlink < 1) {
            shut();
            setErr(e, ErrCode::CORRUPT, path + ": link count is 0, i.e. the inode classified at open time has "
                   "been unlinked; refusing to write a log that no pathname can reach (identity mismatch)");
            return false;
        }
        if (create == UndoCreate::RequireExisting && fst.st_size != 0) {
            shut();
            setErr(e, ErrCode::CORRUPT, path + " is no longer empty (size " + std::to_string((long long)fst.st_size) +
                   "); refusing to overwrite bytes this store never wrote (identity mismatch)");
            return false;
        }
        struct stat lst;
        if (::lstat(path.c_str(), &lst) != 0) {
            int er=errno; shut();
            setErr(e, ErrCode::CORRUPT, path + " pathname no longer resolves before the write (errno=" +
                   std::to_string(er) + "; identity mismatch)");
            return false;
        }
        if (!S_ISREG(lst.st_mode) || lst.st_dev != fst.st_dev || lst.st_ino != fst.st_ino) {
            shut();
            setErr(e, ErrCode::CORRUPT, path + " pathname no longer denotes the inode validated for this write "
                   "(identity mismatch); refusing to write a substituted file");
            return false;
        }
    }
    off_t end = ::lseek(fd, 0, SEEK_END);
    if (end < 0) { shut(); setErr(e, ErrCode::IO, "lseek " + path); return false; }
    if ((uint64_t)end < off) {
        shut();
        setErr(e, ErrCode::CORRUPT, path + " is shorter than the committed length (end=" +
               std::to_string((long long)end) + " < off=" + std::to_string((unsigned long long)off) +
               "); refusing to zero-extend over a committed record region");
        return false;
    }
    size_t done=0; while (done<n){ ssize_t w=::pwrite(fd,d+done,n-done,(off_t)(off+done)); if(w<=0){ shut(); setErr(e,ErrCode::IO,"write "+path); return false; } done+=(size_t)w; }
    if (injFs) { shut(); setErr(e, ErrCode::FS, "injected fsync failure on " + path); return false; }
    if (::fsync(fd)!=0){ shut(); setErr(e,ErrCode::FS,"fsync "+path); return false; }
    // R2 (F0-2-R1) durability binding: the record that just became durable must still be reachable under
    // the committed pathname. If the name was moved away while the bytes were written, the publish is
    // refused BEFORE any manifest can acknowledge a log that no reader could ever reach by name.
    if (pinned) {
        struct stat lst2;
        if (::lstat(path.c_str(), &lst2) != 0 || !S_ISREG(lst2.st_mode) ||
            lst2.st_dev != fst.st_dev || lst2.st_ino != fst.st_ino) {
            shut();
            setErr(e, ErrCode::CORRUPT, path + " pathname no longer denotes the inode whose log was just "
                   "fsynced (identity mismatch); refusing to publish a manifest that references an "
                   "unreachable log");
            return false;
        }
    }
    // R3 (F0-2-R2-2): hand the (still open) descriptor to the caller on request, so it can hold the object
    // across the rename/fsync/ACK window and recheck the pathname binding before acknowledging.
    if (holdFd) *holdFd = fd;
    if (owned && !holdFd && ::close(fd)!=0){ setErr(e,ErrCode::FS,"close "+path); return false; }
    return true;
}
bool csReadFile(const std::string& path, std::vector<uint8_t>* out, bool* exists, ChainError* e) {
    *exists=false; int fd=::open(path.c_str(),O_RDONLY);
    if (fd<0){ if(errno==ENOENT) return true; setErr(e,ErrCode::IO,"open "+path); return false; }
    *exists=true; out->clear(); uint8_t buf[65536];
    for(;;){ ssize_t r=::read(fd,buf,sizeof(buf)); if(r<0){ ::close(fd); setErr(e,ErrCode::IO,"read "+path); return false; } if(r==0) break; out->insert(out->end(),buf,buf+r); }
    ::close(fd); return true;
}
bool csReadRange(const std::string& path, uint64_t off, uint64_t len, std::vector<uint8_t>* out, ChainError* e) {
    out->clear(); out->resize((size_t)len); int fd=::open(path.c_str(),O_RDONLY);
    if (fd<0){ setErr(e,ErrCode::IO,"open "+path); return false; }
    uint64_t got=0; while(got<len){ ssize_t r=::pread(fd,out->data()+got,(size_t)(len-got),(off_t)(off+got)); if(r<=0){ ::close(fd); setErr(e,ErrCode::CORRUPT,"short read "+path); return false; } got+=(uint64_t)r; }
    ::close(fd); return true;
}
bool csFsyncDir(const std::string& dir, ChainError* e) {
    int fd=::open(dir.c_str(),O_RDONLY|O_DIRECTORY); if(fd<0){ setErr(e,ErrCode::IO,"open dir "+dir); return false; }
    int rc=::fsync(fd); ::close(fd); if(rc!=0){ setErr(e,ErrCode::FS,"fsync dir "+dir); return false; } return true;
}

// ---- Manifest --------------------------------------------------------------
std::vector<uint8_t> Manifest::encode() const {
    std::vector<uint8_t> o; const uint8_t magic[8]={'I','N','N','C','S','3','A','1'};
    o.insert(o.end(),magic,magic+8); putU32(o,formatVersion); putU32(o,108);
    putU64(o,generation); putU64(o,undoSize); putU32(o,rootPageId);
    o.insert(o.end(),rootHash.begin(),rootHash.end());
    o.insert(o.end(),blockHash.begin(),blockHash.end());
    putI32(o,tipHeight); putU32(o,csCrc32(o.data(),o.size())); return o;
}
bool Manifest::decode(const uint8_t* d, size_t n, Manifest* m, ChainError* e) {
    const uint8_t magic[8]={'I','N','N','C','S','3','A','1'};
    if (n<108 || memcmp(d,magic,8)!=0){ setErr(e,ErrCode::BAD_SERIALIZE,"manifest magic/size"); return false; }
    size_t off=8; uint32_t ver=0,sz=0;
    if(!getU32(d,n,&off,&ver)||!getU32(d,n,&off,&sz)){ setErr(e,ErrCode::BAD_SERIALIZE,"manifest hdr"); return false; }
    if(ver!=2u){ setErr(e,ErrCode::UNKNOWN_VERSION,"store format version "+std::to_string(ver)+" (expected 2; version <2 predates the Coin.nTime field and CANNOT be opened by this build - reconvert the store, do not reinterpret)"); return false; }
    if((size_t)sz!=n){ setErr(e,ErrCode::CORRUPT,"manifest size mismatch"); return false; }
    uint64_t gen=0,us=0; uint32_t rp=0;
    if(!getU64(d,n,&off,&gen)||!getU64(d,n,&off,&us)||!getU32(d,n,&off,&rp)){ setErr(e,ErrCode::BAD_SERIALIZE,"manifest gen"); return false; }
    Hash rh,bh; memcpy(rh.data(),d+off,32); off+=32; memcpy(bh.data(),d+off,32); off+=32;
    uint32_t hh=0; if(!getU32(d,n,&off,&hh)){ setErr(e,ErrCode::BAD_SERIALIZE,"manifest height"); return false; }
    uint32_t crc=0; if(!getU32(d,n,&off,&crc)){ setErr(e,ErrCode::BAD_SERIALIZE,"manifest crc"); return false; }
    if(csCrc32(d,off-4)!=crc){ setErr(e,ErrCode::CORRUPT,"manifest crc mismatch"); return false; }
    m->formatVersion=ver; m->generation=gen; m->undoSize=us; m->rootPageId=rp; m->rootHash=rh; m->blockHash=bh; m->tipHeight=(int32_t)hh;
    return true;
}

// ---- UndoRecord (with per-record CRC; S2B-1) -------------------------------
std::vector<uint8_t> UndoRecord::encode() const {
    std::vector<uint8_t> o; const uint8_t magic[8]={'I','N','N','U','N','D','O','1'};
    o.insert(o.end(),magic,magic+8); putU32(o,0);
    o.insert(o.end(),blockHash.begin(),blockHash.end()); putI32(o,height); putU32(o,(uint32_t)ops.size());
    for (const Op& op: ops){ auto k=op.op.keyBytes(); o.insert(o.end(),k.begin(),k.end());
        o.push_back(op.beforePresent?1:0); if(op.beforePresent){ auto b=op.before.encode(); o.insert(o.end(),b.begin(),b.end()); }
        o.push_back(op.afterPresent?1:0); if(op.afterPresent){ auto a=op.after.encode(); o.insert(o.end(),a.begin(),a.end()); } }
    uint32_t recLen=(uint32_t)o.size()+4; memcpy(&o[8],&recLen,4);
    uint32_t crc=csCrc32(o.data(),o.size()); for(int i=0;i<4;i++) o.push_back((uint8_t)((crc>>(8*i))&0xff));
    return o;
}
bool UndoRecord::decode(const uint8_t* d, size_t n, size_t* off, UndoRecord* r, ChainError* e) {
    const uint8_t magic[8]={'I','N','N','U','N','D','O','1'};
    size_t start=*off;
    if (start+UNDO_HEADER_SIZE>n){ setErr(e,ErrCode::CORRUPT,"undo header truncated"); return false; }
    if (memcmp(d+start,magic,8)!=0){ setErr(e,ErrCode::CORRUPT,"undo magic"); return false; }
    size_t o=start+8; uint32_t len=0; for(int i=0;i<4;i++) len|=(uint32_t)d[o+i]<<(8*i); o+=4;
    memcpy(r->blockHash.data(),d+o,32); o+=32;
    uint32_t hh=0; for(int i=0;i<4;i++) hh|=(uint32_t)d[o+i]<<(8*i); o+=4; r->height=(int32_t)hh;
    uint32_t cnt=0; for(int i=0;i<4;i++) cnt|=(uint32_t)d[o+i]<<(8*i); o+=4;
    if (len<UNDO_HEADER_SIZE+4 || start+len>n){ setErr(e,ErrCode::CORRUPT,"undo len out of range"); return false; }
    size_t bodyEnd=start+len-4;   // last 4 bytes are crc32
    for (uint32_t i=0;i<cnt;++i){
        if (o+36+1>bodyEnd){ setErr(e,ErrCode::CORRUPT,"undo op truncated"); return false; }
        Op op; std::array<uint8_t,36> k; memcpy(k.data(),d+o,36); o+=36;
        memcpy(op.op.txid.data(),k.data(),32);
        op.op.n=(uint32_t)k[32]|((uint32_t)k[33]<<8)|((uint32_t)k[34]<<16)|((uint32_t)k[35]<<24);
        uint8_t b=d[o++]; op.beforePresent=(b!=0);
        if(op.beforePresent){ if(!op.before.decode(d,bodyEnd,&o)){ setErr(e,ErrCode::CORRUPT,"undo before coin"); return false; } }
        uint8_t a=d[o++]; op.afterPresent=(a!=0);
        if(op.afterPresent){ if(!op.after.decode(d,bodyEnd,&o)){ setErr(e,ErrCode::CORRUPT,"undo after coin"); return false; } }
        r->ops.push_back(op);
    }
    if (o!=bodyEnd){ setErr(e,ErrCode::CORRUPT,"undo record length mismatch"); return false; }
    uint32_t stored=0; for(int i=0;i<4;i++) stored|=(uint32_t)d[start+len-4+i]<<(8*i);
    if (csCrc32(d+start,len-4)!=stored){ setErr(e,ErrCode::CORRUPT,"undo record crc mismatch"); return false; }
    *off=start+len; return true;
}

// ---- CommitStore3A ---------------------------------------------------------
CommitStore3A::~CommitStore3A(){ close(); }
void CommitStore3A::close(){ tree_.close(); if(lockFd_>=0){ ::flock(lockFd_,LOCK_UN); ::close(lockFd_); lockFd_=-1; } opened_=false;
    // R2 (F0-2-R1): the fresh-open undo policy and the descriptor pinning the adopted empty residue die
    // with the open that produced them.
    if (freshUndoFd_ >= 0) { ::close(freshUndoFd_); freshUndoFd_ = -1; }
    if (heldLogFd_ >= 0) { ::close(heldLogFd_); heldLogFd_ = -1; }
    freshUndoCreate_=UndoCreate::Any;
    // R3 (F0-2-R2-1): the one-shot bookkeeping dies with the open that produced it; a reopen reclassifies the
    // actual filesystem evidence and starts from a clean state.
    freshOneShotTaken_=false; reopenWhy_.clear(); pausePoint_=PausePoint::None; pauseMs_=0; }
void CommitStore3A::crashIf(CrashPoint at){ if(crashPoint_==at){ ::fflush(NULL); ::_exit(42); } }
// R3 (F0-2-R2-1): the single fail-closed transition of this stage: the instance is closed for mutation and
// records why, so every later publish/disconnect is refused with a truthful diagnostic instead of silently
// degrading (e.g. to an unpinned write). Reopening is the only way out, and it reclassifies disk evidence.
void CommitStore3A::requireReopen(const std::string& why){ needsReopen_=true; reopenWhy_=why; }
// R3 test hook (see incstore.h setPause). Inert unless a test armed it.
void CommitStore3A::pauseAt(PausePoint p) const {
    if (pausePoint_!=p || pauseMs_<=0) return;
    struct timespec ts; ts.tv_sec=(time_t)(pauseMs_/1000); ts.tv_nsec=(long)(pauseMs_%1000)*1000000L;
    ::nanosleep(&ts,NULL);
}
void CommitStore3A::cleanupTmp(){ DIR* d=opendir(dir_.c_str()); if(!d) return; struct dirent* ent; while((ent=readdir(d))!=NULL){ std::string nm=ent->d_name; if(endsWith(nm,".tmp")) ::unlink(p(nm).c_str()); } closedir(d); }

bool CommitStore3A::get(const OutPoint& k, Coin* c, bool* found, ChainError* e) const {
    std::vector<uint8_t> v; bool f=false; if(!tree_.get(k.keyBytes(),&v,&f,e)) return false;
    *found=f; if(!f) return true; size_t off=0; return c->decode(v.data(),v.size(),&off);
}
bool CommitStore3A::existsUnspentTxid(const Hash& txid, bool* found, ChainError* e) const {
    return tree_.existsTxidPrefix(txid.data(), found, e);
}

bool CommitStore3A::readAck(bool* present, uint64_t* gen, ChainError* e){
    *present=false; *gen=0; std::vector<uint8_t> b; bool ex=false;
    if(!csReadFile(p("ACK"),&b,&ex,e)) return false; if(!ex) return true;
    if(b.size()!=ACK_SIZE){ setErr(e,ErrCode::CORRUPT,"ACK size"); return false; }
    uint32_t crc=0; for(int i=0;i<4;i++) crc|=(uint32_t)b[8+i]<<(8*i);
    if(csCrc32(b.data(),8)!=crc){ setErr(e,ErrCode::CORRUPT,"ACK crc"); return false; }
    uint64_t g=0; for(int i=0;i<8;i++) g|=(uint64_t)b[i]<<(8*i); *present=true; *gen=g; return true;
}
bool CommitStore3A::writeAck(uint64_t gen, ChainError* e){
    std::vector<uint8_t> b(ACK_SIZE,0); for(int i=0;i<8;i++) b[i]=(uint8_t)((gen>>(8*i))&0xff);
    uint32_t crc=csCrc32(b.data(),8); for(int i=0;i<4;i++) b[8+i]=(uint8_t)((crc>>(8*i))&0xff);
    const std::string tmp=p("ACK.tmp");
    if(!csWriteFile(tmp,b.data(),b.size(),ns(InjectStage::AckWrite),ff(InjectStage::AckWrite),e)) return false;
    if(::rename(tmp.c_str(),p("ACK").c_str())!=0){ setErr(e,ErrCode::FS,"rename ACK"); return false; }
    if(!csFsyncDir(dir_,e)) return false; return true;
}


// S1.2: cap on the transient buffer used for one closed segment (bounded allocation; a descriptor whose
// span exceeds this is walked with per-record header reads instead of one bulk read).
static const uint64_t UNDOINDEX_MAX_BULK_SPAN = 4ull*1024*1024;

// Defined below (after this S1.3 helper block).
static bool undoHeaderAt(const uint8_t* h, size_t hn, uint64_t absOff, uint64_t limit, UndoMeta* meta, ChainError* e);

// S1.3: bounded append to the most-recent record cache (drops from the front when it exceeds its bound).
void CommitStore3A::recentPush(const UndoMeta& m){
    undoRecent_.push_back(m);
    const size_t S = ixSegmentSize_?ixSegmentSize_:UNDOINDEX_DEFAULT_SEGMENT_SIZE;
    const size_t cap = UNDO_RECENT_CACHE_SEGMENTS * S;
    if(undoRecent_.size()>cap) undoRecent_.pop_front();
}
// S1.3: drop any segment whose committed range rollback has uncovered; keeps undoSegments_ == the closed
// segments at or below undoCommittedBytes_, which is what keeps subsequent appends non-overlapping.
void CommitStore3A::trimUncommittedSegments(){
    while(!undoSegments_.empty() && undoSegments_.back().endOffset > undoCommittedBytes_)
        undoSegments_.pop_back();
}
// S1.3: rebuild the bounded most-recent cache by parsing forward from the start of the most recent committed
// region (the last closed segment's end, or 0) to undoCommittedBytes_. This is the on-demand path for a
// disconnect deeper than the cache: it validates each record it walks (fail closed on any anomaly) and is
// bounded to at most one closed segment plus the tail (< 2 x segmentSize records).
bool CommitStore3A::reloadRecentFromStream(ChainError* e){
    undoRecent_.clear(); undoRecentRefills_++;
    if(undoCommittedBytes_==0) return true;
    // Locate the segment CONTAINING the last committed record (the tip we must re-serve). Starting at a
    // closed segment's END is wrong when the tip lands exactly on a segment boundary (that end ==
    // committedBytes -> nothing to parse). Use the record count: record #(undoRecordCount_-1) lies in
    // closed segment `seg`, or in the unindexed tail when `seg` equals undoSegments_.size().
    const size_t S=ixSegmentSize_?ixSegmentSize_:UNDOINDEX_DEFAULT_SEGMENT_SIZE;
    uint64_t start=0;
    if(undoRecordCount_>0){
        const size_t seg=(size_t)((undoRecordCount_-1)/S);
        if(seg<undoSegments_.size()) start=undoSegments_[seg].startOffset;
        else start = undoSegments_.empty()?0:undoSegments_.back().endOffset;
    }
    uint64_t off=start;
    while(off<undoCommittedBytes_){
        if(off+UNDO_HEADER_SIZE>undoCommittedBytes_){ setErr(e,ErrCode::CORRUPT,"undo header crosses committed length"); return false; }
        std::vector<uint8_t> hdr; if(!csReadRange(p("undo.dat"),off,UNDO_HEADER_SIZE,&hdr,e)) return false;
        UndoMeta meta; if(!undoHeaderAt(hdr.data(),hdr.size(),off,undoCommittedBytes_,&meta,e)) return false;
        recentPush(meta); off+=meta.len;
    }
    if(off!=undoCommittedBytes_){ setErr(e,ErrCode::CORRUPT,"undo.dat committed length mismatch"); return false; }
    return true;
}

// Decode one undo record header (52 bytes) at absolute offset `absOff`, requiring the whole record to fit
// below `limit`. Mirrors the authoritative scan's validations exactly; duplicated deliberately so the
// audited scan below stays byte-identical.
static bool undoHeaderAt(const uint8_t* h, size_t hn, uint64_t absOff, uint64_t limit, UndoMeta* meta, ChainError* e){
    static const uint8_t magic[8]={'I','N','N','U','N','D','O','1'};
    if(hn<UNDO_HEADER_SIZE) { setErr(e,ErrCode::CORRUPT,"undo header truncated"); return false; }
    if(memcmp(h,magic,8)!=0){ setErr(e,ErrCode::CORRUPT,"undo magic at offset "+std::to_string((unsigned long long)absOff)); return false; }
    uint32_t len=0; for(int i=0;i<4;i++) len|=(uint32_t)h[8+i]<<(8*i);
    uint32_t hh=0;  for(int i=0;i<4;i++) hh |=(uint32_t)h[44+i]<<(8*i);
    if(len<UNDO_HEADER_SIZE+4 || absOff+len>limit){ setErr(e,ErrCode::CORRUPT,"undo record length out of range at offset "+std::to_string((unsigned long long)absOff)); return false; }
    UndoMeta m; memcpy(m.blockHash.data(),h+12,32); m.height=(int32_t)hh; m.offset=absOff; m.len=len; *meta=m;
    return true;
}
// Parse every record wholly contained in a segment buffer (bytes of undo.dat starting at `base`).
static bool parseUndoRecords(const uint8_t* buf, size_t n, uint64_t base, std::vector<UndoMeta>* out, ChainError* e){
    size_t off=0;
    while(off<n){
        UndoMeta meta;
        if(!undoHeaderAt(buf+off,n-off,base+off,base+n,&meta,e)) return false;
        out->push_back(meta); off+=meta.len;
    }
    return true;
}
// Is the on-disk derived index usable as a startup acceleration for THIS committed stream? It is consumed
// only when it describes exactly the committed prefix: structurally valid, contiguous from offset 0, every
// descriptor a CLOSED segment, and committedBytes (== the last descriptor's endOffset) not beyond
// MANIFEST.undoSize. Anything else leaves `usable` false and the authoritative scan runs (the S1.1
// classification then rebuilds/refuses the artifact exactly as before -- nothing is accepted on trust).
bool CommitStore3A::tryLoadCurrentUndoIndex(const Manifest& m, UndoIndex* ix, bool* usable, ChainError* err){
    *usable=false;
    std::vector<uint8_t> bytes; bool ex=false;
    if(!csReadFile(p("UNDOINDEX"),&bytes,&ex,err)) return false;      // I/O error: caller declines, scan reports
    if(!ex) return true;
    if(!UndoIndex::decode(bytes.data(),bytes.size(),ix,err)) return true;
    if(ix->formatVersion!=UNDOINDEX_FORMAT_VERSION) return true;
    if(ix->segmentSize!=ixSegmentSize_) return true;
    if(ix->baseOffset!=0) return true;
    if(ix->segments.empty()) return true;
    if(ix->committedBytes>m.undoSize) return true;
    if(ix->committedBytes!=ix->segments.back().endOffset) return true;
    uint64_t cur=0;
    for(size_t i=0;i<ix->segments.size();++i){
        const UndoSegment& sg=ix->segments[i];
        if(sg.startOffset!=cur || sg.endOffset<=sg.startOffset) return true;
        if(sg.recordCount!=ix->segmentSize) return true;
        cur=sg.endOffset;
    }
    *usable=true;
    return true;
}

bool CommitStore3A::loadUndoForIndexed(const Manifest& m, const UndoIndex& ix, ChainError* e){
    const size_t S = ix.segmentSize;
    undoSegments_.clear(); undoRecent_.clear(); undoRecordCount_=0;
    undoLoadStats_.descriptors=ix.segments.size();
    // Build the committed segment directory from the (already validated, CURRENT) index descriptors.
    // Every descriptor is a CLOSED segment, so the map mirrors the committed stream 1:1 (consistent with
    // what the authoritative scan would derive -- S1.3 parity requirement).
    for(size_t i=0;i<ix.segments.size();++i)
        undoSegments_.push_back(CommittedSegment{ix.segments[i].startOffset, ix.segments[i].endOffset});
    undoRecordCount_ = (uint64_t)ix.segments.size() * (uint64_t)S;
    // Bounded startup: parse EXACTLY the uncovered tail plus enough trailing CLOSED segments to fill the
    // recent cache. This validates the tail and the recent window at load; undoLoadStats_.records stays a
    // function of the recent window, NOT of undoCount() (acceptance evidence that startup no longer parses
    // every historical record). Older segments are validated on demand when (and only when) they are read.
    std::vector<UndoMeta> tailMetas;
    uint64_t off=ix.committedBytes; size_t tail=0;
    while(off<m.undoSize){
        if(off+UNDO_HEADER_SIZE>m.undoSize){ setErr(e,ErrCode::CORRUPT,"undo header crosses committed length"); return false; }
        std::vector<uint8_t> hdr; if(!csReadRange(p("undo.dat"),off,UNDO_HEADER_SIZE,&hdr,e)) return false;
        undoLoadStats_.headerReads++; undoLoadStats_.bytesRead+=UNDO_HEADER_SIZE;
        UndoMeta meta; if(!undoHeaderAt(hdr.data(),hdr.size(),off,m.undoSize,&meta,e)) return false;
        tailMetas.push_back(meta); off+=meta.len; undoLoadStats_.records++;
        // A CURRENT index leaves a tail SMALLER than one closed segment; reaching a full segment means the
        // index missed a closure -> STALE, which the classification/normalisation pass repairs. Decline.
        if(++tail>=S){ setErr(e,ErrCode::CORRUPT,"index covers fewer records than committed (stale)"); return false; }
    }
    if(off!=m.undoSize){ setErr(e,ErrCode::CORRUPT,"undo.dat committed length mismatch"); return false; }
    undoRecordCount_ += (uint64_t)tailMetas.size();          // closed segments x S + the uncovered tail
    // trailing CLOSED segments, oldest first, until the recent cache is full or the history is exhausted.
    const size_t avail = undoSegments_.size();
    if(avail>0){
        const size_t cap = UNDO_RECENT_CACHE_SEGMENTS*S;
        size_t want = (cap > tailMetas.size()) ? (cap - tailMetas.size() + S - 1)/S : 0;
        size_t s0 = (want>=avail) ? 0 : avail-want;
        std::vector<UndoMeta> scratch;
        for(size_t i=s0;i<avail;++i){
            const CommittedSegment& cs=undoSegments_[i];
            scratch.clear();
            if(cs.endOffset-cs.startOffset<=UNDOINDEX_MAX_BULK_SPAN){
                std::vector<uint8_t> buf;
                if(!csReadRange(p("undo.dat"),cs.startOffset,(size_t)(cs.endOffset-cs.startOffset),&buf,e)) return false;
                undoLoadStats_.segmentReads++; undoLoadStats_.bytesRead+=cs.endOffset-cs.startOffset;
                if(!parseUndoRecords(buf.data(),buf.size(),cs.startOffset,&scratch,e)) return false;
            } else {
                uint64_t q=cs.startOffset;
                while(q<cs.endOffset){
                    if(q+UNDO_HEADER_SIZE>cs.endOffset){ setErr(e,ErrCode::CORRUPT,"undo header crosses segment end"); return false; }
                    std::vector<uint8_t> hdr; if(!csReadRange(p("undo.dat"),q,UNDO_HEADER_SIZE,&hdr,e)) return false;
                    undoLoadStats_.headerReads++; undoLoadStats_.bytesRead+=UNDO_HEADER_SIZE;
                    UndoMeta meta; if(!undoHeaderAt(hdr.data(),hdr.size(),q,cs.endOffset,&meta,e)) return false;
                    scratch.push_back(meta); q+=meta.len;
                }
            }
            if(scratch.size()!=S){ setErr(e,ErrCode::CORRUPT,"segment record count mismatch"); return false; }
            for(const UndoMeta& mm: scratch) recentPush(mm);
            undoLoadStats_.records += (uint64_t)scratch.size();
        }
    }
    for(const UndoMeta& mm: tailMetas) recentPush(mm);   // the tail is the MOST recent committed records
    undoCommittedBytes_=m.undoSize; return true;
}

bool CommitStore3A::loadUndoFor(const Manifest& m, ChainError* e){
    undoSegments_.clear(); undoRecent_.clear(); undoRecordCount_=0;
    undoCommittedBytes_=0; undoLoadStats_=UndoLoadStats(); if(m.undoSize==0) return true;
    // R1 (F0-4): the referenced log must be a regular file. lstat (not stat) so a symlink or any other
    // non-regular object is refused here instead of being followed/read and only failing later.
    struct stat st; if(::lstat(p("undo.dat").c_str(),&st)!=0){ setErr(e,ErrCode::CORRUPT,"undo.dat missing but undoSize="+std::to_string(m.undoSize)); return false; }
    if(!S_ISREG(st.st_mode)){ setErr(e,ErrCode::CORRUPT,"undo.dat is not a regular file ("+(S_ISLNK(st.st_mode)?std::string("symbolic link"):(S_ISDIR(st.st_mode)?std::string("directory"):std::string("non-regular file")))+") but undoSize="+std::to_string(m.undoSize)+"; refusing to read it"); return false; }
    // S1.2: consume the derived index when (and only when) it is CURRENT for this manifest. The fast path
    // re-parses every record it reads and adds two cross-checks the scan lacks (exact landing on each
    // descriptor endOffset, exact recordCount per segment). On ANY anomaly it declines -- the counters
    // record why and the authoritative scan below runs unchanged, so no failure mode is masked and no
    // validation is relaxed.
    if(!ixDisabledForTest_){
        UndoIndex ix; bool usable=false; ChainError ixErr;
        if(tryLoadCurrentUndoIndex(m,&ix,&usable,&ixErr) && usable){
            if(loadUndoForIndexed(m,ix,e)){ undoLoadStats_.fastPath=1; return true; }
            const std::string why=e?e->msg:std::string("unknown");
            undoSegments_.clear(); undoRecent_.clear(); undoRecordCount_=0;
            undoCommittedBytes_=0; undoLoadStats_=UndoLoadStats();
            undoLoadStats_.fallback=why;
            clearErr(e);                       // a decline is not a failure: the scan decides the outcome
        } else if(!ixErr.msg.empty() && ixErr.code!=ErrCode::OK){
            undoLoadStats_.fallback="index unreadable: "+ixErr.msg;
        } else if(!usable){
            undoLoadStats_.fallback="index absent/invalid/not-current (scan + S1.1 classification own it)";
        }
    }
    // S1.3: the authoritative scan is also the ONLY full-history parse, and it runs only when the derived
    // index is absent/invalid/stale/ahead (the documented S1.1 repair path). It builds the committed
    // segment directory (O(1) per record) and fills the bounded recent cache instead of a full vector.
    const size_t S=ixSegmentSize_?ixSegmentSize_:UNDOINDEX_DEFAULT_SEGMENT_SIZE;
    uint64_t blockStart=0;
    uint64_t off=0; const uint8_t magic[8]={'I','N','N','U','N','D','O','1'};
    while(off<m.undoSize){
        if(off+UNDO_HEADER_SIZE>m.undoSize){ setErr(e,ErrCode::CORRUPT,"undo header crosses committed length"); return false; }
        std::vector<uint8_t> hdr; if(!csReadRange(p("undo.dat"),off,UNDO_HEADER_SIZE,&hdr,e)) return false;
        undoLoadStats_.headerReads++; undoLoadStats_.bytesRead+=UNDO_HEADER_SIZE;
        if(memcmp(hdr.data(),magic,8)!=0){ setErr(e,ErrCode::CORRUPT,"undo magic at offset "+std::to_string(off)); return false; }
        uint32_t len=0; for(int i=0;i<4;i++) len|=(uint32_t)hdr[8+i]<<(8*i);
        UndoMeta meta; memcpy(meta.blockHash.data(),hdr.data()+12,32);
        uint32_t hh=0; for(int i=0;i<4;i++) hh|=(uint32_t)hdr[44+i]<<(8*i); meta.height=(int32_t)hh;
        if(len<UNDO_HEADER_SIZE+4 || off+len>m.undoSize){ setErr(e,ErrCode::CORRUPT,"undo record length out of range"); return false; }
        meta.offset=off; meta.len=len; recentPush(meta); undoLoadStats_.records++;
        off+=len; undoRecordCount_++;
        if(undoRecordCount_ % S == 0){ undoLoadStats_.descriptors++;
            undoSegments_.push_back(CommittedSegment{blockStart,off}); blockStart=off; }
    }
    if(off!=m.undoSize){ setErr(e,ErrCode::CORRUPT,"undo.dat committed length mismatch"); return false; }
    undoCommittedBytes_=m.undoSize; return true;
}

// ---------------------------------------------------------------------------------------------
// S1.1 -- durable segmented undo index (derived metadata; MANIFEST.undoSize stays authoritative).
// Everything below only READS undo.dat/tree.dat/MANIFEST/ACK and only WRITES the `UNDOINDEX` file
// (via `UNDOINDEX.tmp`); it never truncates, recreates or deletes any other artifact.
// ---------------------------------------------------------------------------------------------
static void ixPutU32(std::vector<uint8_t>& o, uint32_t v){ for(int i=0;i<4;i++) o.push_back((uint8_t)((v>>(8*i))&0xff)); }
static void ixPutU64(std::vector<uint8_t>& o, uint64_t v){ for(int i=0;i<8;i++) o.push_back((uint8_t)((v>>(8*i))&0xff)); }
static bool ixGetU32(const uint8_t* d, size_t n, size_t* o, uint32_t* v){
    if(*o+4>n) return false; uint32_t r=0; for(int i=0;i<4;i++) r|=(uint32_t)d[*o+i]<<(8*i); *o+=4; *v=r; return true; }
static bool ixGetU64(const uint8_t* d, size_t n, size_t* o, uint64_t* v){
    if(*o+8>n) return false; uint64_t r=0; for(int i=0;i<8;i++) r|=(uint64_t)d[*o+i]<<(8*i); *o+=8; *v=r; return true; }
static const char UNDOINDEX_MAGIC[8] = {'I','N','N','U','I','D','X','1'};

std::vector<uint8_t> UndoIndex::encode() const {
    std::vector<uint8_t> out;
    out.reserve(UNDOINDEX_HEADER_SIZE + UNDOINDEX_SEGMENT_SIZE*segments.size());
    out.insert(out.end(), UNDOINDEX_MAGIC, UNDOINDEX_MAGIC+8);
    ixPutU32(out, formatVersion); ixPutU32(out, segmentSize);
    ixPutU64(out, committedBytes); ixPutU64(out, baseOffset); ixPutU64(out, baseGeneration);
    ixPutU32(out, (uint32_t)segments.size());
    ixPutU32(out, csCrc32(out.data(), out.size()));            // header crc over bytes [0,44)
    for (size_t i=0;i<segments.size();++i){
        const UndoSegment& s=segments[i];
        size_t start=out.size();
        ixPutU64(out, s.startOffset); ixPutU64(out, s.endOffset); ixPutU32(out, s.recordCount);
        ixPutU32(out, csCrc32(out.data()+start, out.size()-start));   // per-segment crc
    }
    return out;
}

bool UndoIndex::decode(const uint8_t* d, size_t n, UndoIndex* ix, ChainError* e){
    if (n < UNDOINDEX_HEADER_SIZE){ setErr(e,ErrCode::CORRUPT,"UNDOINDEX shorter than its 48-byte header"); return false; }
    if (memcmp(d, UNDOINDEX_MAGIC, 8)!=0){ setErr(e,ErrCode::CORRUPT,"UNDOINDEX magic mismatch (not 'INNUIDX1')"); return false; }
    size_t o=8; uint32_t ver=0,ssz=0,cnt=0,hc=0; uint64_t cb=0,bo=0,bg=0;
    if(!ixGetU32(d,n,&o,&ver)||!ixGetU32(d,n,&o,&ssz)||!ixGetU64(d,n,&o,&cb)||!ixGetU64(d,n,&o,&bo)||
       !ixGetU64(d,n,&o,&bg)||!ixGetU32(d,n,&o,&cnt)||!ixGetU32(d,n,&o,&hc)){ setErr(e,ErrCode::CORRUPT,"UNDOINDEX header truncated"); return false; }
    if (o != UNDOINDEX_HEADER_SIZE){ setErr(e,ErrCode::CORRUPT,"UNDOINDEX header size mismatch"); return false; }
    if (csCrc32(d, o-4) != hc){ setErr(e,ErrCode::CORRUPT,"UNDOINDEX header crc mismatch"); return false; }
    if (ver != UNDOINDEX_FORMAT_VERSION){ setErr(e,ErrCode::CORRUPT,"UNDOINDEX format version "+std::to_string(ver)+" is not the supported version "+std::to_string(UNDOINDEX_FORMAT_VERSION)); return false; }
    if (ssz == 0){ setErr(e,ErrCode::CORRUPT,"UNDOINDEX declares segment size 0"); return false; }
    if (n != UNDOINDEX_HEADER_SIZE + (size_t)UNDOINDEX_SEGMENT_SIZE*cnt){ setErr(e,ErrCode::CORRUPT,"UNDOINDEX length does not match its segment count"); return false; }
    ix->formatVersion=ver; ix->segmentSize=ssz; ix->committedBytes=cb; ix->baseOffset=bo; ix->baseGeneration=bg;
    ix->segments.clear(); ix->segments.reserve(cnt);
    size_t p=UNDOINDEX_HEADER_SIZE;
    for (uint32_t i=0;i<cnt;++i){
        size_t q=p; uint64_t s0=0,e0=0; uint32_t rc=0,crc=0;
        if(!ixGetU64(d,n,&q,&s0)||!ixGetU64(d,n,&q,&e0)||!ixGetU32(d,n,&q,&rc)||!ixGetU32(d,n,&q,&crc)){ setErr(e,ErrCode::CORRUPT,"UNDOINDEX segment "+std::to_string(i)+" truncated"); return false; }
        if (csCrc32(d+p,20) != crc){ setErr(e,ErrCode::CORRUPT,"UNDOINDEX segment "+std::to_string(i)+" crc mismatch"); return false; }
        if (rc == 0){ setErr(e,ErrCode::CORRUPT,"UNDOINDEX segment "+std::to_string(i)+" declares 0 records"); return false; }
        if (rc > ssz){ setErr(e,ErrCode::CORRUPT,"UNDOINDEX segment "+std::to_string(i)+" declares "+std::to_string(rc)+" records, more than the segment size "+std::to_string(ssz)); return false; }
        if (e0 <= s0){ setErr(e,ErrCode::CORRUPT,"UNDOINDEX segment "+std::to_string(i)+" has a non-positive byte range"); return false; }
        const uint64_t expect = ix->segments.empty() ? bo : ix->segments.back().endOffset;
        if (s0 != expect){ setErr(e,ErrCode::CORRUPT,"UNDOINDEX segments are not contiguous at segment "+std::to_string(i)); return false; }
        UndoSegment s; s.startOffset=s0; s.endOffset=e0; s.recordCount=rc; s.crc32=crc;
        ix->segments.push_back(s); p += UNDOINDEX_SEGMENT_SIZE;
    }
    const uint64_t tail = ix->segments.empty() ? bo : ix->segments.back().endOffset;
    if (cb != tail){ setErr(e,ErrCode::CORRUPT,"UNDOINDEX committedBytes does not equal the end of its last closed segment"); return false; }
    return true;
}

// S1.3: derive the expected UNDOINDEX from the committed segment directory (O(#segments), no per-record
// history). Every map entry is a CLOSED segment; encode() fills the real per-descriptor crc32.
bool CommitStore3A::buildUndoIndexFromSegments(UndoIndex* out) const {
    out->formatVersion = UNDOINDEX_FORMAT_VERSION;
    out->segmentSize   = (ixSegmentSize_ ? ixSegmentSize_ : UNDOINDEX_DEFAULT_SEGMENT_SIZE);
    out->baseOffset = 0; out->baseGeneration = 0; out->segments.clear();
    for (size_t i=0;i<undoSegments_.size();++i){
        UndoSegment s;
        s.startOffset = undoSegments_[i].startOffset;
        s.endOffset   = undoSegments_[i].endOffset;
        s.recordCount = (uint32_t)out->segmentSize;
        s.crc32 = 0;
        out->segments.push_back(s);
    }
    if (!undoSegments_.empty()) out->committedBytes = undoSegments_.back().endOffset;
    return true;
}

bool CommitStore3A::loadUndoIndexFromDisk(bool* exists, UndoIndex* ix, ChainError* e){
    std::vector<uint8_t> bytes; bool ex=false;
    if(!csReadFile(p("UNDOINDEX"),&bytes,&ex,e)) return false;
    *exists = ex;
    if(!ex) return true;
    return UndoIndex::decode(bytes.data(), bytes.size(), ix, e);   // structural validation -> FAIL CLOSED
}

bool CommitStore3A::readUndoIndexForTest(UndoIndex* ix, ChainError* e){
    std::vector<uint8_t> bytes; bool ex=false;
    if(!csReadFile(p("UNDOINDEX"),&bytes,&ex,e)) return false;
    if(!ex){ setErr(e,ErrCode::OK,"UNDOINDEX absent"); return false; }
    return UndoIndex::decode(bytes.data(), bytes.size(), ix, e);
}

bool CommitStore3A::publishUndoIndex(const UndoIndex& ix, ChainError* e){
    if (ixDisabledForTest_) return true;
    if (ixSkipNextPublish_){ ixSkipNextPublish_=false; return true; }   // test hook: simulated lost publication
    struct timespec t0,t1; clock_gettime(CLOCK_MONOTONIC,&t0);          // measured publication latency
    std::vector<uint8_t> bytes = ix.encode();
    // S1.1-F1: one-shot test-only faults, each placed where the real syscall failure occurs. Default None.
    if(ixFault_ == (int)UndoIndexFault::TmpCreate){
        ixFault_ = (int)UndoIndexFault::None;
        setErr(e,ErrCode::IO,"injected UNDOINDEX.tmp creation failure"); return false; }
    if(ixFault_ == (int)UndoIndexFault::TmpShortWrite){
        ixFault_ = (int)UndoIndexFault::None;                            // real shape: a truncated copy is left
        size_t shortLen = bytes.size() ? bytes.size()-1 : 0;
        if(!csWriteFile(p("UNDOINDEX.tmp"), bytes.data(), shortLen, false, false, e)){
            setErr(e,ErrCode::IO,"injected short write on UNDOINDEX.tmp"); return false; }
        setErr(e,ErrCode::IO,"injected short write on UNDOINDEX.tmp ("+std::to_string((unsigned long long)shortLen)+
               " of "+std::to_string((unsigned long long)bytes.size())+" bytes)"); return false; }
    if(!csWriteFile(p("UNDOINDEX.tmp"), bytes.data(), bytes.size(), false, false, e)){
        ::unlink(p("UNDOINDEX.tmp").c_str()); return false; }
    ++ixFsyncs_;                                                        // csWriteFile fsyncs the file
    if(ixFault_ == (int)UndoIndexFault::TmpFsync){
        ixFault_ = (int)UndoIndexFault::None;                            // real shape: the tmp survives on disk
        setErr(e,ErrCode::FS,"injected UNDOINDEX.tmp fsync failure"); return false; }
    if(ixFault_ == (int)UndoIndexFault::Rename){
        ixFault_ = (int)UndoIndexFault::None;
        ::unlink(p("UNDOINDEX.tmp").c_str());
        setErr(e,ErrCode::FS,"injected UNDOINDEX rename failure"); return false; }
    if(::rename(p("UNDOINDEX.tmp").c_str(), p("UNDOINDEX").c_str())!=0){
        setErr(e,ErrCode::FS,"rename UNDOINDEX"); ::unlink(p("UNDOINDEX.tmp").c_str()); return false; }
    ++ixPublishes_; ixBytesWritten_ += (uint64_t)bytes.size();
    clock_gettime(CLOCK_MONOTONIC,&t1);
    ixNanos_ += (uint64_t)(t1.tv_sec-t0.tv_sec)*1000000000ull + (uint64_t)(t1.tv_nsec-t0.tv_nsec);
    return true;
}

bool CommitStore3A::publishUndoIndexOnSegmentClose(uint64_t appendEndOffset, ChainError* e){
    const size_t S = (ixSegmentSize_ ? ixSegmentSize_ : UNDOINDEX_DEFAULT_SEGMENT_SIZE);
    if (undoRecordCount_+1 == 0 || (undoRecordCount_ + 1) % S != 0) return true;   // no boundary crossed
    // The record being committed closes segment #undoSegments_.size(), whose byte range is [previous closed
    // end, appendEndOffset]. The committed directory must reflect ONLY committed segments, so it is NOT
    // mutated here: if the publication (or the enclosing commit) fails, nothing was committed and the map
    // is left exactly as it was (a spurious entry here made a one-shot fault double-push the segment on the
    // retry, writing a non-positive byte range -- see S1.3 report).
    const uint64_t segStart = undoSegments_.empty() ? 0 : undoSegments_.back().endOffset;
    UndoIndex ix; buildUndoIndexFromSegments(&ix);                 // existing closed segments only
    { UndoSegment ns; ns.startOffset = segStart; ns.endOffset = appendEndOffset;
      ns.recordCount = (uint32_t)ix.segmentSize; ns.crc32 = 0;
      ix.segments.push_back(ns); ix.committedBytes = appendEndOffset; }   // + the closing segment
    if(!publishUndoIndex(ix, e)) return false;
    undoSegments_.push_back(CommittedSegment{segStart, appendEndOffset});  // committed ONLY on success
    return true;
}

// S1.1-F1 (finding S1.1-1): preserve a structurally corrupt derived artifact so the operator keeps the
// diagnostic evidence, then let the caller rebuild from authoritative state. Guarantees: never overwrite or
// delete an existing quarantined artifact; collision-safe naming; deterministic for a given datadir state;
// MANIFEST/ACK/tree.dat/undo.dat are never touched. Only ever reached under explicit authorization.
bool CommitStore3A::quarantineCorruptUndoIndex(std::string* quarantinePath, uint64_t* bytesOut, ChainError* e){
    const std::string src = p("UNDOINDEX");
    struct stat st;
    if(::lstat(src.c_str(),&st)!=0){ setErr(e,ErrCode::IO,"lstat "+src); return false; }
    *bytesOut = S_ISREG(st.st_mode) ? (uint64_t)st.st_size : 0;   // report size only; no move decision uses it
    const std::string base = "UNDOINDEX.corrupt.g" + std::to_string((unsigned long long)generation_);
    for(int n=0;n<1000;++n){
        const std::string cand = p(n ? base+"."+std::to_string(n) : base);
        int lrc;
        if(ixQuarantineLinkErrno_){                    // S1.1-F2: one-shot test-only injection (off by default)
            errno = ixQuarantineLinkErrno_; ixQuarantineLinkErrno_ = 0; lrc = -1;
        } else lrc = ::link(src.c_str(), cand.c_str());
        if(lrc==0){                                    // atomic; EEXIST is the only recoverable outcome
            if(::unlink(src.c_str())!=0){
                // The link exists but the SOURCE could not be removed. Proceeding would let the rebuild publish
                // over the still-present corrupt artifact and destroy the evidence just preserved, so this is a
                // fail-closed abort (never a success), with the residue named for the operator.
                const int ue=errno;
                setErr(e,ErrCode::IO,"automatic quarantine of "+src+" could not be completed: link("+src+" -> "
                       +cand+") succeeded but unlink("+src+") failed (errno="+std::to_string(ue)+"); the corrupt "
                       "artifact is STILL IN PLACE and a hard link to it may remain at "+cand+
                       " -- preserve/move it manually and retry");
                return false; }
            *quarantinePath = cand; return true;
        }
        if(errno==EEXIST) continue;                    // name taken: try the next suffix
        // S1.1-F2 (finding F1-1): hard links are the ONLY move primitive. There is deliberately NO
        // lstat-then-rename fallback: on Linux rename() atomically REPLACES an existing regular-file
        // destination and does not report EEXIST, so such a fallback could silently destroy an existing
        // quarantine artifact. Any error other than "name taken" therefore fails closed and changes nothing.
        const int le=errno;
        setErr(e,ErrCode::IO,"automatic quarantine of "+src+" could not be completed: link("+src+" -> "+cand+
               ") errno="+std::to_string(le)+" (hard links unavailable or denied); preserve/move the corrupt "
               "artifact manually and retry");
        return false;
    }
    setErr(e,ErrCode::FS,"no free quarantine name for "+src+" -- preserve/move existing quarantine artifacts "
           "manually and retry"); return false;
}

bool CommitStore3A::classifyAndNormalizeUndoIndex(ChainError* e){
    if (ixDisabledForTest_){ undoIndexState_ = UndoIndexState::None; return true; }
    UndoIndex onDisk; bool exists=false;
    // S1.1-F1: read and validate the artifact in two distinct steps, because the recovery decision must
    // tell "the bytes could not be read at all" (an I/O problem with the datadir -- never quarantined)
    // apart from "the bytes were read and are structurally invalid" (a corrupt derived artifact).
    std::vector<uint8_t> raw;
    if(!csReadFile(p("UNDOINDEX"),&raw,&exists,e)){
        undoIndexState_ = UndoIndexState::Corrupt; ixRecovery_.indexReadError = true; ixRecovery_.resolution="refused";
        return false; }
    ixRecovery_.indexPresent = exists;
    if(exists && !UndoIndex::decode(raw.data(), raw.size(), &onDisk, e)){
        if(!ixAuthorizeRecovery_){
            undoIndexState_ = UndoIndexState::Corrupt; ixRecovery_.indexCorrupt = true;
            ixRecovery_.resolution = "refused";                                  // DEFAULT: fail closed
            return false;
        }
        // Explicit operator authorization: preserve the corrupt artifact for diagnostics, then rebuild
        // EXCLUSIVELY from the committed stream that tryLoadManifest/loadUndoFor already validated above.
        ixRecovery_.indexCorrupt = true;
        std::string qp; uint64_t qb=0; ChainError qe;
        if(!quarantineCorruptUndoIndex(&qp,&qb,&qe)){
            setErr(e,ErrCode::FS,"operator-authorized UNDOINDEX recovery failed to quarantine the corrupt "
                   "artifact ("+qe.str()+"); refusing to open: "+dir_);
            ixRecovery_.resolution = "quarantine-failed"; return false; }
        ixRecovery_.quarantinePath = qp; ixRecovery_.quarantinedBytes = qb;
        printf("UNDOINDEX: operator-authorized recovery requested; quarantined corrupt artifact %s -> %s "
               "(%llu bytes); rebuilding exclusively from the committed stream (MANIFEST.undoSize=%llu)\n",
               p("UNDOINDEX").c_str(), qp.c_str(), (unsigned long long)qb,
               (unsigned long long)undoCommittedBytes_);
        exists = false;                       // the name is free again -> the rebuild path below publishes it
        clearErr(e);                          // the artifact is preserved off to the side; the open continues
    }
    UndoIndex expected; buildUndoIndexFromSegments(&expected);
    bool same = exists && onDisk.segmentSize==expected.segmentSize && onDisk.committedBytes==expected.committedBytes &&
                onDisk.segments.size()==expected.segments.size();
    if (same) for(size_t i=0;i<expected.segments.size();++i){
        const UndoSegment&a=onDisk.segments[i]; const UndoSegment&b=expected.segments[i];
        if(a.startOffset!=b.startOffset||a.endOffset!=b.endOffset||a.recordCount!=b.recordCount){ same=false; break; } }
    if (same){
        undoIndexState_ = UndoIndexState::Current;
        // S1.1-F1: a repeated recovery request on a healthy index must be a no-op.
        ixRecovery_.resolution = ixAuthorizeRecovery_ ? "already-current" : "none";
        return true; }
    if (!exists && expected.segments.empty()){
        undoIndexState_ = UndoIndexState::Absent;
        ixRecovery_.rebuilt = true; ixRecovery_.nothingToIndex = true;     // nothing closed -> no file written
        ixRecovery_.resolution = ixRecovery_.indexCorrupt ? "quarantined+rebuilt" : "already-current";
        return true; }
    UndoIndexState cls = UndoIndexState::Absent;
    if (exists) {
        // Classified against the closed-segment prefix IMPLIED BY THE COMMITTED RANGE -- not against raw
        // byte counts. A prefix reaching further than the committed range describes records the manifest
        // has not committed (an interrupted publication: the tail is dropped, never authoritative); a
        // prefix falling short of the committed range is missing closed segments (never trusted).
        // Equal prefixes (including the normal "active tail not yet closed" state) are Current above.
        const bool beyond = onDisk.segments.size() > expected.segments.size() ||
                            onDisk.committedBytes > expected.committedBytes;
        cls = beyond ? UndoIndexState::Ahead : UndoIndexState::Stale;
    }
    const char* why =
        cls==UndoIndexState::Ahead ? "ahead of the manifest (uncommitted tail dropped, never authoritative)" :
        cls==UndoIndexState::Stale ? "stale: behind the committed range (rebuilt, not trusted)" :
                                     "absent (rebuilt from the committed stream)";
    printf("UNDOINDEX: %s; rebuilding %zu closed segment(s) for committedBytes=%llu\n",
           why, expected.segments.size(), (unsigned long long)expected.committedBytes);
    if(!publishUndoIndex(expected, e)) return false;
    if(ixFault_ == (int)UndoIndexFault::DirFsync){                       // S1.1-F1: one-shot test-only fault
        ixFault_ = (int)UndoIndexFault::None;
        setErr(e,ErrCode::FS,"injected UNDOINDEX directory fsync failure");
        ixRecovery_.resolution = "refused"; return false; }
    if(!csFsyncDir(dir_, e)) return false;                              // normalisation is its own publication
    ++ixFsyncs_;
    undoIndexState_ = cls;
    ixRecovery_.rebuilt = true;
    ixRecovery_.rebuiltSegments = expected.segments.size();
    ixRecovery_.rebuiltCommittedBytes = expected.committedBytes;
    ixRecovery_.resolution = ixRecovery_.indexCorrupt ? "quarantined+rebuilt" : "rebuilt";
    return true;
}

bool CommitStore3A::tryLoadManifest(const std::string& path, bool* exists, uint32_t* rootId, Hash* rootHash, ChainError* e){
    std::vector<uint8_t> bytes; bool ex=false;
    if(!csReadFile(path,&bytes,&ex,e)) return false;
    *exists=false; if(!ex) return false; *exists=true;
    Manifest m; if(!Manifest::decode(bytes.data(),bytes.size(),&m,e)) return false;
    if(tree_.fd()<0){ if(!tree_.open(p("tree.dat"),e)) return false; }
    if(!tree_.setRoot(m.rootPageId,m.rootHash,e)) return false;
    if(!loadUndoFor(m,e)) return false;
    generation_=m.generation; blockHash_=m.blockHash; tipHeight_=m.tipHeight;
    *rootId=m.rootPageId; *rootHash=m.rootHash; return true;
}

// S1.1-F1: open() is exactly the fail-closed default; recovery of a corrupt derived index requires the
// explicit one-shot authorization carried by OpenOptions (see the header). No other behaviour differs.
bool CommitStore3A::open(const std::string& dir, ChainError* e){
    return openWithOptions(dir, OpenOptions(), e);
}

bool CommitStore3A::openWithOptions(const std::string& dir, const OpenOptions& opts, ChainError* e){
    ixAuthorizeRecovery_ = opts.authorizeUndoIndexRecovery;
    ixRecovery_ = UndoIndexRecoveryReport();
    ixRecovery_.authorized = ixAuthorizeRecovery_;
    ixRecovery_.resolution = "none";
    clearErr(e); if(opened_){ setErr(e,ErrCode::ALREADY_OPEN,dir); return false; }
    dir_=dir; if(!ensureDir(dir_)){ setErr(e,ErrCode::IO,"cannot create dir "+dir_); return false; }
    lockFd_=::open(p("LOCK").c_str(),O_RDWR|O_CREAT,0644);
    if(lockFd_<0){ setErr(e,ErrCode::IO,"open LOCK"); return false; }
    if(::flock(lockFd_,LOCK_EX|LOCK_NB)!=0){ ::close(lockFd_); lockFd_=-1; setErr(e,ErrCode::LOCKED,"another writer holds LOCK"); return false; }

    if(!readAck(&ackPresent_,&ackGen_,e)){ close(); return false; }

    ChainError e1; bool pEx=false; uint32_t rId=0; Hash rH;
    ixFreshPath_ = false;
    bool pOk=tryLoadManifest(p("MANIFEST"),&pEx,&rId,&rH,&e1);
    if(pOk){
        uint64_t pg=generation_;
        if(ackPresent_){ if(pg<ackGen_){ close(); setErr(e,ErrCode::CORRUPT,"FAIL-CLOSED: primary generation "+std::to_string(pg)+" is OLDER than acknowledged generation "+std::to_string(ackGen_)+" (rollback of acknowledged state refused)"); return false; }
                         if(pg>ackGen_+1){ close(); setErr(e,ErrCode::CORRUPT,"primary generation "+std::to_string(pg)+" ahead of acknowledged generation "+std::to_string(ackGen_)); return false; } }
        else { if(pg!=0){ close(); setErr(e,ErrCode::CORRUPT,"acknowledgment evidence missing for generation "+std::to_string(pg)); return false; } }
        if(!ackPresent_ || pg>ackGen_){ if(!writeAck(pg,e)){ close(); return false; } ackGen_=pg; ackPresent_=true; }
    } else if(!pEx){
        // R2 (closes R1-1 destructive fresh-create, R1-2 silent-empty presentation).
        // MANIFEST and ACK are both absent. Existing on-disk state is authoritative evidence of a
        // prior store; a new empty store may be created ONLY when tree.dat is genuinely ABSENT
        // (stat -> ENOENT). Never O_TRUNC/recreate an existing tree.dat, never treat a stat error
        // as absence, and never present existing data as a clean genesis.
        if(ackPresent_){ close(); setErr(e,ErrCode::CORRUPT,"ACK present (generation "+std::to_string(ackGen_)+") but MANIFEST is missing; refusing to infer a fresh store"); return false; }
        { struct stat bst; if(::stat(p("MANIFEST.bak").c_str(),&bst)==0){ close(); setErr(e,ErrCode::CORRUPT,"MANIFEST is missing but MANIFEST.bak exists (prior store evidence); refusing to infer a fresh store"); return false; } }
        const std::string tp=p("tree.dat");
        struct stat st; bool hasTree=false;
        if(::stat(tp.c_str(),&st)==0){
            hasTree=true;
            if(!S_ISREG(st.st_mode)){ close(); setErr(e,ErrCode::IO,"existing tree.dat is not a regular file; refusing to recreate it"); return false; }
            if(st.st_size<=0 || (st.st_size % (off_t)PAGE_SIZE)!=0){ close(); setErr(e,ErrCode::CORRUPT,"existing tree.dat (size "+std::to_string((long long)st.st_size)+") without MANIFEST/ACK is not a whole-page image; refusing to truncate or recreate"); return false; }
        } else if(errno!=ENOENT){ close(); setErr(e,ErrCode::IO,"stat(tree.dat) failed (errno="+std::to_string(errno)+"); not treating a stat error as file absence"); return false; }
        // R1 (F0-2): an orphan undo.dat is prior-store evidence exactly like tree.dat. Classify it BEFORE
        // anything is created or written, and remember the policy the first undo write must enforce so no
        // byte of an operator's or a crashed initialization's file is silently overwritten.
        { std::string det; int uk=classifyUndoEvidence(p("undo.dat"),&det);
          if(uk==-1){ close(); setErr(e,ErrCode::IO,"lstat(undo.dat) failed ("+det+"); not treating a stat error as file absence"); return false; }
          if(uk==3){ close(); setErr(e,ErrCode::CORRUPT,"existing undo.dat is not a regular file ("+det+") and there is no MANIFEST/ACK; refusing to overwrite it"); return false; }
          if(uk==2){ close(); setErr(e,ErrCode::CORRUPT,"existing undo.dat holds "+det+" of undo log without MANIFEST/ACK (prior-store evidence); refusing to overwrite the orphan log"); return false; }
          freshUndoCreate_ = (uk==0) ? UndoCreate::RequireAbsent : UndoCreate::RequireExisting;
          freshUndoFd_ = -1;
          // R2 (F0-2-R1): PIN the accepted EMPTY residue with an open descriptor and re-validate the object
          // on the descriptor itself (the lstat above is only advisory: the pathname may already have been
          // swapped). fstat on the pinned fd must still show a regular, still-empty file. From here the
          // first record is written through THIS descriptor, so a later substitution at the pathname can
          // neither receive the bytes nor block/follow the writer.
          if (uk==1){
              int pfd = ::open(p("undo.dat").c_str(), O_RDWR|O_NOFOLLOW|O_NONBLOCK);
              if (pfd < 0){
                  int er=errno; close();
                  setErr(e,ErrCode::CORRUPT,"empty undo.dat could not be pinned by descriptor (open errno="+std::to_string(er)+"); refusing to adopt it");
                  return false;
              }
              struct stat pst;
              if (::fstat(pfd,&pst)!=0 || !S_ISREG(pst.st_mode) || pst.st_size!=0 || pst.st_nlink<1){
                  int er=errno; ::close(pfd); close();
                  setErr(e,ErrCode::CORRUPT,"empty undo.dat stopped being a live empty regular file while it was being adopted (fstat errno="+std::to_string(er)+"); refusing to adopt it");
                  return false;
              }
              freshUndoFd_ = pfd;
          } }
        if(!hasTree){
            clearErr(e); if(!tree_.create(tp,e)){ close(); return false; }   // genuinely no prior store
        } else {
            ChainError et; if(!tree_.open(tp,&et)){ close(); setErr(e,ErrCode::CORRUPT,"existing tree.dat cannot be opened without MANIFEST/ACK; refusing to recreate it: "+et.msg); return false; }
            if(tree_.nextPageId()!=1u){ close(); setErr(e,ErrCode::CORRUPT,"existing tree.dat holds "+std::to_string(tree_.nextPageId())+" pages (prior store data) while MANIFEST/ACK are absent; refusing to present it as fresh genesis"); return false; }
            ChainError e0; if(!tree_.loadPage0AsRoot(&e0)){ close(); setErr(e,ErrCode::CORRUPT,"existing tree.dat page 0 is unusable: "+e0.msg); return false; }
            if(!(tree_.rootIsLeaf() && tree_.rootCount()==0)){ close(); setErr(e,ErrCode::CORRUPT,"existing tree.dat is not an empty store; refusing to present it as fresh genesis"); return false; }
        }
        ixFreshPath_ = true;   // S1.1: no MANIFEST -> nothing committed -> never touch UNDOINDEX
        generation_=0; undoSegments_.clear(); undoRecent_.clear(); undoRecordCount_=0;
        undoCommittedBytes_=0; blockHash_=Hash(); tipHeight_=-1;
    } else {
        ChainError e2; bool bEx=false; uint32_t bId=0; Hash bH;
        bool bOk=tryLoadManifest(p("MANIFEST.bak"),&bEx,&bId,&bH,&e2);
        if(!bOk){ close(); setErr(e, e1.code==ErrCode::OK?ErrCode::CORRUPT:e1.code, "primary corrupt and no valid backup: "+e1.msg); return false; }
        uint64_t bg=generation_;
        if(!ackPresent_){ close(); setErr(e,ErrCode::CORRUPT,"cannot classify corrupt primary: no acknowledgment evidence"); return false; }
        if(ackGen_>bg){ close(); setErr(e,ErrCode::CORRUPT,"FAIL-LOUD: acknowledged generation "+std::to_string(ackGen_)+" is corrupt/unloadable; refusing silent rollback to generation "+std::to_string(bg)); return false; }
        if(ackGen_<bg){ close(); setErr(e,ErrCode::CORRUPT,"inconsistent acknowledgment("+std::to_string(ackGen_)+") vs backup("+std::to_string(bg)+") generations"); return false; }
        hadRecoveryIssue_=true;
        std::vector<uint8_t> bakBytes; bool bkEx=false; ChainError e3;
        if(csReadFile(p("MANIFEST.bak"),&bakBytes,&bkEx,&e3) && bkEx){
            if(!csWriteFile(p("MANIFEST"),bakBytes.data(),bakBytes.size(),false,false,&e3)){ close(); setErr(e,ErrCode::FS,"failed to repair MANIFEST from backup"); return false; }
            if(!csFsyncDir(dir_,&e3)){ close(); setErr(e,ErrCode::FS,"failed to fsync repair of MANIFEST"); return false; }
        }
    }
    // S1.1: validate/normalise the derived UNDOINDEX against the committed state. The MANIFEST-absent
    // (fresh / prior-store) paths never touch it: with no committed undo there is nothing to index, and
    // an UNDOINDEX left over from another store is not treated as authority (documented as OPEN).
    if (!ixFreshPath_ && !classifyAndNormalizeUndoIndex(e)){ close(); return false; }
    undoIndexState_ = ixFreshPath_ ? UndoIndexState::None : undoIndexState_;
    cleanupTmp(); needsReopen_=false; reopenWhy_.clear(); freshOneShotTaken_=false; heldLogFd_=-1;
    // S1.7-G: open stays O(1) root-hash verification (non-root corruption is detected lazily, fail-closed
    // on read — see M1-pathverify). Free-page reuse is done WITHIN a run (commitRetired + retire-after-ACK +
    // appendPage reuse), which bounds tree.dat for fresh stores without an open-time full-tree walk.
    opened_=true; return true;
}

bool CommitStore3A::commitBarriers(const Manifest& m, uint64_t undoOffset, const uint8_t* undoAppend, size_t undoAppendLen, ChainError* e){
    renamePassed_=false; freshOneShotTaken_=false;
    // 1) durable tree pages
    if(inj(InjectStage::PageWrite)){ injErr(e,"tree fsync"); return false; }
    if(!tree_.fsync(e)) return false;
    // 2) R1 (closes F0-1): establish MANIFEST.bak from the CURRENT primary BEFORE any byte of the undo
    //    log is (re)written, atomically, and make that replacement durable BEFORE the write.
    //    Why the ordering is load-bearing: after a disconnect, the retained MANIFEST.bak still describes
    //    the pre-disconnect generation, whose undoSize is LARGER than the new committed logical end.
    //    Writing the next undo record at that end reuses the disconnected (stale) tail -- bytes the
    //    backup still references -- so a crash between the write and the backup replacement would leave
    //    a manifest pointing at overwritten log bytes (F0-1). Replacing the backup first establishes the
    //    invariant: every retained manifest (primary AND MANIFEST.bak) references only bytes below the
    //    write offset, in every crash state. The replacement is atomic (tmp + rename; the live backup is
    //    never truncated in place) and is followed by a directory fsync, because a non-durable rename
    //    would be rolled back by a power loss and re-expose the older, larger backup mid-write.
    crashIf(CrashPoint::BeforeBakReplace);
    {
        std::vector<uint8_t> cur; bool curEx=false; ChainError e2;
        if(!csReadFile(p("MANIFEST"),&cur,&curEx,&e2)){ setErr(e,e2.code,"read prior MANIFEST: "+e2.msg); ::unlink(p("MANIFEST.tmp").c_str()); return false; }
        if(curEx){
            Manifest bman;
            if(!Manifest::decode(cur.data(),cur.size(),&bman,&e2)){ setErr(e,e2.code,"prior MANIFEST is undecodable; refusing to publish over it: "+e2.msg); ::unlink(p("MANIFEST.tmp").c_str()); return false; }
            if(!tree_.checkRootHash(bman.rootPageId,bman.rootHash,&e2)){ setErr(e,e2.code,"prior MANIFEST references an unreadable root; refusing to publish: "+e2.msg); ::unlink(p("MANIFEST.tmp").c_str()); return false; }
            if(undoAppendLen>0 && bman.undoSize!=undoOffset){
                setErr(e,ErrCode::CORRUPT,"prior MANIFEST undoSize="+std::to_string((unsigned long long)bman.undoSize)+
                       " does not equal the committed write offset "+std::to_string((unsigned long long)undoOffset)+
                       "; refusing to write undo bytes over/at a region the retained backup references");
                ::unlink(p("MANIFEST.tmp").c_str()); return false;
            }
            const std::string bakTmp=p("MANIFEST.bak.tmp");
            if(!csWriteFile(bakTmp,cur.data(),cur.size(),ns(InjectStage::BakWrite),ff(InjectStage::BakWrite),e)){ ::unlink(p("MANIFEST.tmp").c_str()); return false; }
            std::vector<uint8_t> rb; bool rex=false; ChainError e3;
            if(!csReadFile(bakTmp,&rb,&rex,&e3)||!rex){ setErr(e,ErrCode::FS,"backup not readable after write"); ::unlink(bakTmp.c_str()); ::unlink(p("MANIFEST.tmp").c_str()); return false; }
            if(!Manifest::decode(rb.data(),rb.size(),&bman,&e3)){ setErr(e,ErrCode::FS,"backup not decodable after write: "+e3.msg); ::unlink(bakTmp.c_str()); ::unlink(p("MANIFEST.tmp").c_str()); return false; }
            if(!tree_.checkRootHash(bman.rootPageId,bman.rootHash,&e3)){ setErr(e,ErrCode::FS,"backup references unreadable root: "+e3.msg); ::unlink(bakTmp.c_str()); ::unlink(p("MANIFEST.tmp").c_str()); return false; }
            crashIf(CrashPoint::BakPrepared);
            if(::rename(bakTmp.c_str(),p("MANIFEST.bak").c_str())!=0){ setErr(e,ErrCode::FS,"rename MANIFEST.bak"); ::unlink(bakTmp.c_str()); ::unlink(p("MANIFEST.tmp").c_str()); return false; }
            if(!csFsyncDir(dir_,e)){ ::unlink(p("MANIFEST.tmp").c_str()); return false; }
        }
    }
    crashIf(CrashPoint::AfterBakReplace);
    // 3) undo write, at the COMMITTED logical offset (F0: never O_APPEND, never truncate)
    heldLogFd_ = -1;
    if(undoAppendLen>0){
        // R1 (F0-2): the first undo write after a fresh open obeys the create policy fixed by the
        // open-time classification (absent -> O_EXCL create; accepted empty residue -> never create).
        // R2 (F0-2-R1): for an accepted empty residue it writes through the descriptor PINNED at open time,
        // so a file substituted at undo.dat between open and this write can never receive a byte.
        // The pinned descriptor's ownership moves to this write and is closed here on every path.
        UndoCreate cre=freshUndoCreate_; freshUndoCreate_=UndoCreate::Any;
        int pfd=freshUndoFd_; freshUndoFd_=-1;
        // R3 (F0-2-R2-1): this write SPENDS the fresh-open one-shot. Record it, so a failure below makes the
        // caller require a reopen instead of silently retrying with UndoCreate::Any (unpinned, pathname-based).
        if (cre != UndoCreate::Any) freshOneShotTaken_=true;
        bool wok; int outFd=-1;
        if(injectUndoShortOnce_>0 && injectUndoShortOnce_<undoAppendLen){   // F0 test hook: torn write
            size_t part=injectUndoShortOnce_; injectUndoShortOnce_=0;
            wok=csWriteAtFile(p("undo.dat"),undoOffset,undoAppend,part,ns(InjectStage::UndoAppend),ff(InjectStage::UndoAppend),e,cre,pfd,&outFd);
            if(pfd>=0) heldLogFd_=pfd; else if(wok) heldLogFd_=outFd;
            if(!wok) return false;
            setErr(e,ErrCode::IO,"injected short undo write"); return false;
        }
        wok=csWriteAtFile(p("undo.dat"),undoOffset,undoAppend,undoAppendLen,ns(InjectStage::UndoAppend),ff(InjectStage::UndoAppend),e,cre,pfd,&outFd);
        if(pfd>=0) heldLogFd_=pfd; else if(wok) heldLogFd_=outFd;
        if(!wok) return false;
    }
    crashIf(CrashPoint::AfterUndoWrite);
    // 3b) S1.1: if this append closed a segment, publish the derived UNDOINDEX. Ordered AFTER the
    //     durable undo write and BEFORE MANIFEST.tmp, so the index can only ever be AHEAD of the
    //     manifest when a crash intervenes. It is derived metadata: nothing here is authoritative,
    //     the manifest rename + dir fsync below (step 5/6) also make this rename durable, and a
    //     failure fails the commit exactly like every other barrier (no partial authority).
    if (undoAppendLen > 0 && !publishUndoIndexOnSegmentClose(undoOffset + undoAppendLen, e)) return false;
    // 4) manifest tmp
    std::vector<uint8_t> mb=m.encode();
    if(!csWriteFile(p("MANIFEST.tmp"),mb.data(),mb.size(),ns(InjectStage::ManifestTmp),ff(InjectStage::ManifestTmp),e)) return false;
    // 5) VISIBILITY
    crashIf(CrashPoint::BeforeRename);
    if(inj(InjectStage::PrimaryRename)){ injErr(e,"primary rename"); ::unlink(p("MANIFEST.tmp").c_str()); return false; }
    if(::rename(p("MANIFEST.tmp").c_str(),p("MANIFEST").c_str())!=0){ setErr(e,ErrCode::FS,"rename MANIFEST"); return false; }
    renamePassed_=true;
    crashIf(CrashPoint::AfterRename);
    // 6) DURABILITY
    if(inj(InjectStage::DirFsync)){ injErr(e,"dir fsync"); return false; }
    if(!csFsyncDir(dir_,e)) return false;
    crashIf(CrashPoint::AfterDirSync);
    // R3 (F0-2-R2-2): the generation must not be acknowledged while the committed pathname no longer denotes
    // the log whose bytes this commit just made durable. Every other check is already behind us (the
    // write-time binding, the manifest rename); this is the LAST point at which refusing can still prevent
    // `MANIFEST(gen N) + ACK(gen N)` from being written over a log that no reader could reach by name.
    // Soundness argument for the comparison: heldLogFd_ is still OPEN, so the log inode cannot have been
    // freed and its NUMBER cannot have been reused (this filesystem does reuse inode numbers after
    // unlink+create -- measured in R2); a dev/ino difference here is therefore real substitution, not
    // aliasing. One fstat + one lstat, no new state, no O(state) work. It NARROWS the window (from the
    // write-time binding down to just before the ACK write) and removes the silent-success case, but it
    // cannot close an interval that ends with the name-based ACK write itself: a swap after this check stays
    // undetectable. See DESIGN.md 13.3 -- F0-2-R2-2 remains OPEN with an external-actor threat model.
    if (heldLogFd_ >= 0) {
        pauseAt(PausePoint::BeforeUndoNameRecheck);
        struct stat hst; struct stat lst;
        if (::fstat(heldLogFd_,&hst)!=0 || !S_ISREG(hst.st_mode) || hst.st_nlink<1 ||
            ::lstat(p("undo.dat").c_str(),&lst)!=0 || !S_ISREG(lst.st_mode) ||
            lst.st_dev!=hst.st_dev || lst.st_ino!=hst.st_ino) {
            setErr(e, ErrCode::CORRUPT, "undo.dat pathname no longer denotes the log this generation made "
                   "durable (identity mismatch); refusing to acknowledge generation "+
                   std::to_string((unsigned long long)m.generation)+" and requiring a reopen before any "
                   "further mutation");
            return false;
        }
    }
    return true;
}

bool CommitStore3A::publish(const Delta& d, const Hash& blockHash, int32_t height, ChainError* e){
    clearErr(e);
    if(!opened_){ setErr(e,ErrCode::NOT_OPEN,dir_); return false; }
    if(needsReopen_){ setErr(e,ErrCode::NOT_OPEN,"store requires reopen before any further mutation: "+reopenWhy_); return false; }
    uint32_t oldRoot=tree_.rootId(); Hash oldHash=tree_.rootHash(); ChainError er;
    tree_.clearRetired();   // S1.7-G: drop any staged retired set from a prior aborted commit (a
                            // rolled-back root may still reference those pages -> never free them)
    // apply delta (CoW; new pages only)
    for (const Op& op: d.ops){
        Key k=op.op.keyBytes();
        if(op.afterPresent){ std::vector<uint8_t> v=op.after.encode(); if(!tree_.put(k,v,e)){ ChainError x; tree_.setRoot(oldRoot,oldHash,&x); return false; } }
        else { if(!tree_.erase(k,e)){ ChainError x; tree_.setRoot(oldRoot,oldHash,&x); return false; } }
    }
    UndoRecord rec; rec.blockHash=blockHash; rec.height=height; rec.ops=d.ops;
    std::vector<uint8_t> recBytes=rec.encode();
    uint64_t newGen=generation_+1; uint64_t newUndoSize=undoCommittedBytes_+recBytes.size();
    Manifest m; m.generation=newGen; m.undoSize=newUndoSize; m.rootPageId=tree_.rootId(); m.rootHash=tree_.rootHash(); m.blockHash=blockHash; m.tipHeight=height;
    if(!commitBarriers(m,undoCommittedBytes_,recBytes.data(),recBytes.size(),e)){
        if(!renamePassed_) tree_.setRoot(oldRoot,oldHash,&er);      // no new generation became authoritative
        if(heldLogFd_>=0){ ::close(heldLogFd_); heldLogFd_=-1; }   // R3: release the held log descriptor
        // R3 (F0-2-R2-1): a post-rename failure was already fatal (the new MANIFEST may be visible); a
        // failure AFTER the undo write phase is now fatal too, because the fresh-open one-shot is spent and
        // a retry on this instance would write the log WITHOUT the pin/identity checks.
        if(renamePassed_) requireReopen("the new MANIFEST may be visible while its durability barrier failed");
        else if(freshOneShotTaken_) requireReopen("the fresh-open undo policy was spent by a failed first publication (a retry would write unpinned)");
        return false;
    }
    pauseAt(PausePoint::BeforeAck);
    bool ackOk=writeAck(newGen,e);
    // R3 (F0-2-R2-2): the log descriptor is held until the acknowledgment has actually been attempted.
    if(heldLogFd_>=0){ ::close(heldLogFd_); heldLogFd_=-1; }
    if(!ackOk){ requireReopen("the generation was published but its acknowledgment failed"); return false; }
    UndoMeta meta; meta.blockHash=blockHash; meta.height=height; meta.offset=undoCommittedBytes_; meta.len=(uint32_t)recBytes.size();
    generation_=newGen; undoCommittedBytes_=newUndoSize; recentPush(meta); undoRecordCount_++;
    ackGen_=newGen; ackPresent_=true; blockHash_=blockHash; tipHeight_=height;
    tree_.retireMovedToFree();   // S1.7-G: acked generation -> its CoW-retired pages become reusable
    return true;
}

bool CommitStore3A::disconnect(const Hash& blockHash, ChainError* e){
    clearErr(e);
    if(!opened_){ setErr(e,ErrCode::NOT_OPEN,dir_); return false; }
    if(needsReopen_){ setErr(e,ErrCode::NOT_OPEN,"store requires reopen before any further mutation: "+reopenWhy_); return false; }
    tree_.clearRetired();
    if(undoRecent_.empty()){            // deep peel beyond the bounded recent window: refill on demand
        if(!reloadRecentFromStream(e)) return false;
        if(undoRecent_.empty() || undoRecent_.back().offset + undoRecent_.back().len != undoCommittedBytes_){
            setErr(e,ErrCode::NOT_FOUND,"no committed undo to disconnect"); return false; } }
    const UndoMeta meta=undoRecent_.back();
    if(meta.blockHash!=blockHash){ setErr(e,ErrCode::NOT_FOUND,"only the most recent block can be disconnected"); return false; }
    std::vector<uint8_t> body; if(!csReadRange(p("undo.dat"),meta.offset,meta.len,&body,e)) return false;
    size_t off=0; UndoRecord rec; if(!UndoRecord::decode(body.data(),body.size(),&off,&rec,e)) return false;
    uint32_t oldRoot=tree_.rootId(); Hash oldHash=tree_.rootHash(); ChainError er;
    for (size_t i=rec.ops.size(); i-->0; ){ const Op& o=rec.ops[i]; Key k=o.op.keyBytes();
        if(o.beforePresent){ std::vector<uint8_t> v=o.before.encode(); if(!tree_.put(k,v,e)){ ChainError x; tree_.setRoot(oldRoot,oldHash,&x); return false; } }
        else { if(!tree_.erase(k,e)){ ChainError x; tree_.setRoot(oldRoot,oldHash,&x); return false; } } }
    uint64_t newGen=generation_+1; uint64_t newUndoSize=undoCommittedBytes_-meta.len;
    Manifest m; m.generation=newGen; m.undoSize=newUndoSize; m.rootPageId=tree_.rootId(); m.rootHash=tree_.rootHash(); m.blockHash=Hash(); m.tipHeight=tipHeight_-1;
    if(!commitBarriers(m,0,NULL,0,e)){
        if(!renamePassed_) tree_.setRoot(oldRoot,oldHash,&er);      // no new generation became authoritative
        if(heldLogFd_>=0){ ::close(heldLogFd_); heldLogFd_=-1; }   // R3: release the held log descriptor
        if(renamePassed_) requireReopen("the new MANIFEST may be visible while its durability barrier failed");
        else if(freshOneShotTaken_) requireReopen("the fresh-open undo policy was spent by a failed first publication (a retry would write unpinned)");
        return false;
    }
    pauseAt(PausePoint::BeforeAck);
    bool ackOk=writeAck(newGen,e);
    if(heldLogFd_>=0){ ::close(heldLogFd_); heldLogFd_=-1; }
    if(!ackOk){ requireReopen("the generation was published but its acknowledgment failed"); return false; }
    generation_=newGen; undoCommittedBytes_=newUndoSize; undoRecent_.pop_back(); undoRecordCount_--;
    trimUncommittedSegments();
    ackGen_=newGen; ackPresent_=true; blockHash_=Hash(); tipHeight_=m.tipHeight;
    tree_.retireMovedToFree();   // S1.7-G: acked disconnect gen -> retired pages reusable
    return true;
}

}} // namespace cs::store3a
