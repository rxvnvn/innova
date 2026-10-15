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
    if(ver!=1u){ setErr(e,ErrCode::UNKNOWN_VERSION,"manifest version "+std::to_string(ver)); return false; }
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
void CommitStore3A::close(){ tree_.close(); if(lockFd_>=0){ ::flock(lockFd_,LOCK_UN); ::close(lockFd_); lockFd_=-1; } opened_=false; }
void CommitStore3A::crashIf(CrashPoint at){ if(crashPoint_==at){ ::fflush(NULL); ::_exit(42); } }
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

bool CommitStore3A::loadUndoFor(const Manifest& m, ChainError* e){
    undoIndex_.clear(); undoCommittedBytes_=0; if(m.undoSize==0) return true;
    struct stat st; if(stat(p("undo.dat").c_str(),&st)!=0){ setErr(e,ErrCode::CORRUPT,"undo.dat missing but undoSize="+std::to_string(m.undoSize)); return false; }
    uint64_t off=0; const uint8_t magic[8]={'I','N','N','U','N','D','O','1'};
    while(off<m.undoSize){
        if(off+UNDO_HEADER_SIZE>m.undoSize){ setErr(e,ErrCode::CORRUPT,"undo header crosses committed length"); return false; }
        std::vector<uint8_t> hdr; if(!csReadRange(p("undo.dat"),off,UNDO_HEADER_SIZE,&hdr,e)) return false;
        if(memcmp(hdr.data(),magic,8)!=0){ setErr(e,ErrCode::CORRUPT,"undo magic at offset "+std::to_string(off)); return false; }
        uint32_t len=0; for(int i=0;i<4;i++) len|=(uint32_t)hdr[8+i]<<(8*i);
        UndoMeta meta; memcpy(meta.blockHash.data(),hdr.data()+12,32);
        uint32_t hh=0; for(int i=0;i<4;i++) hh|=(uint32_t)hdr[44+i]<<(8*i); meta.height=(int32_t)hh;
        if(len<UNDO_HEADER_SIZE+4 || off+len>m.undoSize){ setErr(e,ErrCode::CORRUPT,"undo record length out of range"); return false; }
        meta.offset=off; meta.len=len; undoIndex_.push_back(meta); off+=len;
    }
    if(off!=m.undoSize){ setErr(e,ErrCode::CORRUPT,"undo.dat committed length mismatch"); return false; }
    undoCommittedBytes_=m.undoSize; return true;
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

bool CommitStore3A::open(const std::string& dir, ChainError* e){
    clearErr(e); if(opened_){ setErr(e,ErrCode::ALREADY_OPEN,dir); return false; }
    dir_=dir; if(!ensureDir(dir_)){ setErr(e,ErrCode::IO,"cannot create dir "+dir_); return false; }
    lockFd_=::open(p("LOCK").c_str(),O_RDWR|O_CREAT,0644);
    if(lockFd_<0){ setErr(e,ErrCode::IO,"open LOCK"); return false; }
    if(::flock(lockFd_,LOCK_EX|LOCK_NB)!=0){ ::close(lockFd_); lockFd_=-1; setErr(e,ErrCode::LOCKED,"another writer holds LOCK"); return false; }

    if(!readAck(&ackPresent_,&ackGen_,e)){ close(); return false; }

    ChainError e1; bool pEx=false; uint32_t rId=0; Hash rH;
    bool pOk=tryLoadManifest(p("MANIFEST"),&pEx,&rId,&rH,&e1);
    if(pOk){
        uint64_t pg=generation_;
        if(ackPresent_){ if(pg<ackGen_){ close(); setErr(e,ErrCode::CORRUPT,"FAIL-CLOSED: primary generation "+std::to_string(pg)+" is OLDER than acknowledged generation "+std::to_string(ackGen_)+" (rollback of acknowledged state refused)"); return false; }
                         if(pg>ackGen_+1){ close(); setErr(e,ErrCode::CORRUPT,"primary generation "+std::to_string(pg)+" ahead of acknowledged generation "+std::to_string(ackGen_)); return false; } }
        else { if(pg!=0){ close(); setErr(e,ErrCode::CORRUPT,"acknowledgment evidence missing for generation "+std::to_string(pg)); return false; } }
        if(!ackPresent_ || pg>ackGen_){ if(!writeAck(pg,e)){ close(); return false; } ackGen_=pg; ackPresent_=true; }
    } else if(!pEx){
        if(ackPresent_ && ackGen_>0){ close(); setErr(e,ErrCode::CORRUPT,"acknowledged MANIFEST (gen "+std::to_string(ackGen_)+") is missing"); return false; }
        struct stat st;
        bool ok=false;
        if(stat(p("tree.dat").c_str(),&st)==0){ if(tree_.open(p("tree.dat"),&e1)){ ChainError e0; if(tree_.loadPage0AsRoot(&e0)) ok=true; } }
        if(!ok){ tree_.close(); clearErr(e); if(!tree_.create(p("tree.dat"),e)){ close(); return false; } }
        generation_=0; undoIndex_.clear(); undoCommittedBytes_=0; blockHash_=Hash(); tipHeight_=-1;
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
    cleanupTmp(); needsReopen_=false; opened_=true; return true;
}

bool CommitStore3A::commitBarriers(const Manifest& m, const uint8_t* undoAppend, size_t undoAppendLen, ChainError* e){
    renamePassed_=false;
    // 1) durable tree pages
    if(inj(InjectStage::PageWrite)){ injErr(e,"tree fsync"); return false; }
    if(!tree_.fsync(e)) return false;
    // 2) undo append
    if(undoAppendLen>0){ if(!csAppendFile(p("undo.dat"),undoAppend,undoAppendLen,ns(InjectStage::UndoAppend),ff(InjectStage::UndoAppend),e)) return false; }
    // 3) manifest tmp
    std::vector<uint8_t> mb=m.encode();
    if(!csWriteFile(p("MANIFEST.tmp"),mb.data(),mb.size(),ns(InjectStage::ManifestTmp),ff(InjectStage::ManifestTmp),e)) return false;
    // 4) establish backup (checked)
    { std::vector<uint8_t> cur; bool curEx=false; ChainError e2;
      if(!csReadFile(p("MANIFEST"),&cur,&curEx,&e2)){ setErr(e,e2.code,"read prior MANIFEST: "+e2.msg); ::unlink(p("MANIFEST.tmp").c_str()); return false; }
      if(curEx){
        if(!csWriteFile(p("MANIFEST.bak"),cur.data(),cur.size(),ns(InjectStage::BakWrite),ff(InjectStage::BakWrite),e)){ ::unlink(p("MANIFEST.tmp").c_str()); return false; }
        std::vector<uint8_t> rb; bool rex=false; ChainError e3; Manifest bman;
        if(!csReadFile(p("MANIFEST.bak"),&rb,&rex,&e3)||!rex){ setErr(e,ErrCode::FS,"backup not readable after write"); ::unlink(p("MANIFEST.tmp").c_str()); return false; }
        if(!Manifest::decode(rb.data(),rb.size(),&bman,&e3)){ setErr(e,ErrCode::FS,"backup not decodable after write: "+e3.msg); ::unlink(p("MANIFEST.tmp").c_str()); return false; }
        if(!tree_.checkRootHash(bman.rootPageId,bman.rootHash,&e3)){ setErr(e,ErrCode::FS,"backup references unreadable root: "+e3.msg); ::unlink(p("MANIFEST.tmp").c_str()); return false; }
      }
    }
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
    return true;
}

bool CommitStore3A::publish(const Delta& d, const Hash& blockHash, int32_t height, ChainError* e){
    clearErr(e);
    if(!opened_){ setErr(e,ErrCode::NOT_OPEN,dir_); return false; }
    if(needsReopen_){ setErr(e,ErrCode::NOT_OPEN,"store requires reopen after a post-rename failure"); return false; }
    uint32_t oldRoot=tree_.rootId(); Hash oldHash=tree_.rootHash(); ChainError er;
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
    if(!commitBarriers(m,recBytes.data(),recBytes.size(),e)){ if(renamePassed_) needsReopen_=true; else tree_.setRoot(oldRoot,oldHash,&er); return false; }
    if(!writeAck(newGen,e)){ needsReopen_=true; return false; }
    UndoMeta meta; meta.blockHash=blockHash; meta.height=height; meta.offset=undoCommittedBytes_; meta.len=(uint32_t)recBytes.size();
    generation_=newGen; undoCommittedBytes_=newUndoSize; undoIndex_.push_back(meta);
    ackGen_=newGen; ackPresent_=true; blockHash_=blockHash; tipHeight_=height;
    return true;
}

bool CommitStore3A::disconnect(const Hash& blockHash, ChainError* e){
    clearErr(e);
    if(!opened_){ setErr(e,ErrCode::NOT_OPEN,dir_); return false; }
    if(needsReopen_){ setErr(e,ErrCode::NOT_OPEN,"store requires reopen after a post-rename failure"); return false; }
    if(undoIndex_.empty()){ setErr(e,ErrCode::NOT_FOUND,"no committed undo to disconnect"); return false; }
    const UndoMeta meta=undoIndex_.back();
    if(meta.blockHash!=blockHash){ setErr(e,ErrCode::NOT_FOUND,"only the most recent block can be disconnected"); return false; }
    std::vector<uint8_t> body; if(!csReadRange(p("undo.dat"),meta.offset,meta.len,&body,e)) return false;
    size_t off=0; UndoRecord rec; if(!UndoRecord::decode(body.data(),body.size(),&off,&rec,e)) return false;
    uint32_t oldRoot=tree_.rootId(); Hash oldHash=tree_.rootHash(); ChainError er;
    for (size_t i=rec.ops.size(); i-->0; ){ const Op& o=rec.ops[i]; Key k=o.op.keyBytes();
        if(o.beforePresent){ std::vector<uint8_t> v=o.before.encode(); if(!tree_.put(k,v,e)){ ChainError x; tree_.setRoot(oldRoot,oldHash,&x); return false; } }
        else { if(!tree_.erase(k,e)){ ChainError x; tree_.setRoot(oldRoot,oldHash,&x); return false; } } }
    uint64_t newGen=generation_+1; uint64_t newUndoSize=undoCommittedBytes_-meta.len;
    Manifest m; m.generation=newGen; m.undoSize=newUndoSize; m.rootPageId=tree_.rootId(); m.rootHash=tree_.rootHash(); m.blockHash=Hash(); m.tipHeight=tipHeight_-1;
    if(!commitBarriers(m,NULL,0,e)){ if(renamePassed_) needsReopen_=true; else tree_.setRoot(oldRoot,oldHash,&er); return false; }
    if(!writeAck(newGen,e)){ needsReopen_=true; return false; }
    generation_=newGen; undoCommittedBytes_=newUndoSize; undoIndex_.pop_back();
    ackGen_=newGen; ackPresent_=true; blockHash_=Hash(); tipHeight_=m.tipHeight;
    return true;
}

}} // namespace cs::store3a
