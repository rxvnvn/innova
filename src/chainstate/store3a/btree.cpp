// Stage 3A - copy-on-write paged B+tree implementation.
// R1: authenticated (Merkle) traversal verification + unreferenced trailing-page recovery.
#include "btree.h"
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <cstdio>

namespace cs { namespace store3a {

static void putU32b(uint8_t* p, uint32_t v){ for(int i=0;i<4;i++) p[i]=(uint8_t)((v>>(8*i))&0xff); }
static void putU64b(uint8_t* p, uint64_t v){ for(int i=0;i<8;i++) p[i]=(uint8_t)((v>>(8*i))&0xff); }
static uint32_t rdU32(const uint8_t* p){ uint32_t v=0; for(int i=0;i<4;i++) v|=(uint32_t)p[i]<<(8*i); return v; }
static uint16_t rdU16(const uint8_t* p){ return (uint16_t)(p[0] | ((uint16_t)p[1]<<8)); }
static void putU16b(uint8_t* p, uint16_t v){ p[0]=(uint8_t)(v&0xff); p[1]=(uint8_t)((v>>8)&0xff); }

Hash leafHash(const Node& n){
    ChainedHash h; const uint8_t t=NT_LEAF; h.put(&t,1);
    for (const LeafEntry& e : n.entries){ h.put(e.key.data(),36);
        uint8_t lb[4]; putU32b(lb,(uint32_t)e.val.size()); h.put(lb,4); h.put(e.val); }
    return h.final();
}
Hash internalHash(const Node& n){
    ChainedHash h; const uint8_t t=NT_INTERNAL; h.put(&t,1);
    for (size_t i=0;i<n.children.size();++i){ uint8_t ib[4]; putU32b(ib,n.children[i]); h.put(ib,4);
        h.put(n.chashes[i].data(),32); }
    return h.final();
}
size_t leafSize(const Node& n){ size_t s=4; for (const LeafEntry& e: n.entries) s += 36 + 2 + e.val.size(); return s; }
size_t internalSize(const Node& n){ return 4 + n.children.size()*(4+32) + n.keys.size()*36; }

bool encodeNode(const Node& n, uint8_t* buf){
    memset(buf,0,PAGE_SIZE);
    if (n.type==NT_LEAF){
        if (leafSize(n) > PAGE_SIZE) return false;
        buf[0]=NT_LEAF; putU16b(buf+1,(uint16_t)n.entries.size());
        size_t off=4;
        for (const LeafEntry& e: n.entries){ memcpy(buf+off,e.key.data(),36); off+=36;
            putU16b(buf+off,(uint16_t)e.val.size()); off+=2; memcpy(buf+off,e.val.data(),e.val.size()); off+=e.val.size(); }
    } else {
        if (internalSize(n) > PAGE_SIZE) return false;
        buf[0]=NT_INTERNAL; putU16b(buf+1,(uint16_t)n.keys.size());
        size_t off=4;
        for (size_t i=0;i<n.children.size();++i){ putU32b(buf+off,n.children[i]); off+=4; memcpy(buf+off,n.chashes[i].data(),32); off+=32; }
        for (const Key& k: n.keys){ memcpy(buf+off,k.data(),36); off+=36; }
    }
    return true;
}

bool decodeNode(const uint8_t* buf, Node* n, ChainError* e){
    uint8_t t=buf[0]; uint16_t cnt=rdU16(buf+1);
    if (t==NT_LEAF){
        n->type=NT_LEAF; n->entries.clear(); size_t off=4;
        for (uint16_t i=0;i<cnt;i++){
            if (off+36+2>PAGE_SIZE){ setErr(e,ErrCode::CORRUPT,"leaf key truncated"); return false; }
            LeafEntry en; memcpy(en.key.data(),buf+off,36); off+=36;
            uint16_t vl=rdU16(buf+off); off+=2;
            if (off+vl>PAGE_SIZE){ setErr(e,ErrCode::CORRUPT,"leaf val truncated"); return false; }
            en.val.assign(buf+off,buf+off+vl); off+=vl; n->entries.push_back(en);
        }
    } else if (t==NT_INTERNAL){
        n->type=NT_INTERNAL; n->children.clear(); n->chashes.clear(); n->keys.clear(); size_t off=4;
        for (size_t i=0;i<(size_t)cnt+1;i++){
            if (off+36>PAGE_SIZE){ setErr(e,ErrCode::CORRUPT,"internal child truncated"); return false; }
            n->children.push_back(rdU32(buf+off)); off+=4;
            Hash h; memcpy(h.data(),buf+off,32); off+=32; n->chashes.push_back(h);
        }
        for (size_t i=0;i<(size_t)cnt;i++){
            if (off+36>PAGE_SIZE){ setErr(e,ErrCode::CORRUPT,"internal key truncated"); return false; }
            Key k; memcpy(k.data(),buf+off,36); off+=36; n->keys.push_back(k);
        }
    } else { setErr(e,ErrCode::CORRUPT,"unknown node type"); return false; }
    return true;
}

void BTree::close(){ if (fd_>=0){ ::close(fd_); fd_=-1; } }

bool BTree::create(const std::string& path, ChainError* e){
    close(); path_=path; nextId_=0; truncatedTail_=false;
    fd_ = ::open(path.c_str(), O_RDWR|O_CREAT|O_TRUNC, 0644);
    if (fd_<0){ setErr(e,ErrCode::IO,"create tree "+path); return false; }
    Node root; root.type=NT_LEAF;
    uint8_t buf[PAGE_SIZE]; encodeNode(root,buf);
    if (::pwrite(fd_,buf,PAGE_SIZE,0)!= (ssize_t)PAGE_SIZE){ setErr(e,ErrCode::IO,"write root"); return false; }
    nextId_=1; rootId_=0; rootHash_=leafHash(root); rootIsLeaf_=true; rootCount_=0;
    return true;
}
bool BTree::open(const std::string& path, ChainError* e){
    close(); path_=path; truncatedTail_=false;
    fd_ = ::open(path.c_str(), O_RDWR);
    if (fd_<0){ setErr(e,ErrCode::IO,"open tree "+path); return false; }
    off_t sz = ::lseek(fd_,0,SEEK_END);
    if (sz<0){ setErr(e,ErrCode::IO,"lseek tree"); return false; }
    // R1 (S3A-3): a partial trailing page means an append was interrupted before its
    // full PAGE_SIZE was written. Pages are only ever written at whole-page offsets and
    // a referenced page is always fully written+fsynced before any MANIFEST that points
    // to it becomes durable, so bytes past the last whole page are provably unreachable
    // (never referenced by the committed root nor by the retained fallback root). Drop
    // exactly that footer; never truncate a whole page.
    if (sz % (off_t)PAGE_SIZE != 0){
        off_t aligned = sz - (sz % (off_t)PAGE_SIZE);
        if (::ftruncate(fd_, aligned)!=0){ setErr(e,ErrCode::FS,"truncate partial tree tail"); return false; }
        if (::fsync(fd_)!=0){ setErr(e,ErrCode::FS,"fsync after tail truncate"); return false; }
        truncatedTail_=true; sz=aligned;
    }
    if (sz<=0){ setErr(e,ErrCode::CORRUPT,"tree file empty/invalid"); return false; }
    nextId_ = (uint32_t)(sz/(off_t)PAGE_SIZE);
    return true;
}
bool BTree::fsync(ChainError* e){ if (fd_<0){ setErr(e,ErrCode::NOT_OPEN,"btree"); return false; }
    if (::fsync(fd_)!=0){ setErr(e,ErrCode::FS,"fsync tree"); return false; } return true; }

bool BTree::readPage(uint32_t id, uint8_t* buf, ChainError* e) const {
    if (fd_<0){ setErr(e,ErrCode::NOT_OPEN,"btree"); return false; }
    if (id>=nextId_){ setErr(e,ErrCode::CORRUPT,"page id out of range "+std::to_string(id)); return false; }
    ssize_t r = ::pread(fd_, buf, PAGE_SIZE, (off_t)id*PAGE_SIZE);
    reads_++;
    if (r!=(ssize_t)PAGE_SIZE){ setErr(e,ErrCode::CORRUPT,"short page read "+std::to_string(id)); return false; }
    return true;
}
// R1: read + decode + authenticate a page against its expected Merkle node hash.
bool BTree::readNodeVerified(uint32_t id, const Hash& expect, Node* n, ChainError* e) const {
    uint8_t buf[PAGE_SIZE]; if (!readPage(id,buf,e)) return false;
    if (!decodeNode(buf,n,e)) return false;
    Hash calc = (n->type==NT_LEAF)? leafHash(*n) : internalHash(*n);
    if (memcmp(calc.data(),expect.data(),32)!=0){
        setErr(e,ErrCode::CORRUPT,"node hash mismatch at page "+std::to_string(id)+" (corrupt/unauthenticated page)")
        ; return false;
    }
    return true;
}
bool BTree::appendPage(const uint8_t* buf, uint32_t* outId, ChainError* e){
    if (fd_<0){ setErr(e,ErrCode::NOT_OPEN,"btree"); return false; }
    uint32_t id=nextId_;
    if (partialAppendOn_>0){ partialAppendOn_--; if(partialAppendOn_==0) partialAppendBytes_=PAGE_SIZE/3; }
    if (partialAppendBytes_>0){   // R1/R2 test hook: simulated short/interrupted append
        size_t nb=partialAppendBytes_; partialAppendBytes_=0;
        if (nb>(size_t)PAGE_SIZE){ setErr(e,ErrCode::INTERNAL,"partial append size"); return false; }
        if (::pwrite(fd_,buf,nb,(off_t)id*PAGE_SIZE)!=(ssize_t)nb){ setErr(e,ErrCode::IO,"injected short append"); return false; }
        setErr(e,ErrCode::IO,"injected interrupted page append (short write)"); return false;
    }
    if (::pwrite(fd_,buf,PAGE_SIZE,(off_t)id*PAGE_SIZE)!=(ssize_t)PAGE_SIZE){ setErr(e,ErrCode::IO,"append page"); return false; }
    nextId_++; writes_++; *outId=id; return true;
}

bool BTree::setRoot(uint32_t id, const Hash& h, ChainError* e){
    Node n; if (!readNodeVerified(id,h,&n,e)) return false;
    rootId_=id; rootHash_=h; rootIsLeaf_=(n.type==NT_LEAF);
    rootCount_ = n.type==NT_LEAF ? n.entries.size() : (size_t)-1;
    return true;
}
bool BTree::checkRootHash(uint32_t id, const Hash& h, ChainError* e) const {
    Node n; return readNodeVerified(id,h,&n,e);
}
bool BTree::loadPage0AsRoot(ChainError* e){
    uint8_t buf[PAGE_SIZE]; if (!readPage(0,buf,e)) return false;
    Node n; if (!decodeNode(buf,&n,e)) return false;
    rootId_=0; rootIsLeaf_=(n.type==NT_LEAF); rootCount_ = n.type==NT_LEAF ? n.entries.size() : (size_t)-1;
    rootHash_= n.type==NT_LEAF ? leafHash(n) : internalHash(n);
    return true;
}

bool BTree::get(const Key& k, std::vector<uint8_t>* val, bool* found, ChainError* e) const {
    *found=false; uint32_t id=rootId_; Hash expect=rootHash_; Node n;
    for (int guard=0; guard<64; ++guard){
        if (!readNodeVerified(id,expect,&n,e)) return false;
        if (n.type==NT_LEAF){
            for (const LeafEntry& en: n.entries){ int c=keyCmp(en.key,k); if (c==0){ *val=en.val; *found=true; return true; } if (c>0) return true; }
            return true;
        }
        size_t ci=0; while (ci<n.keys.size() && keyCmp(k,n.keys[ci])>=0) ++ci; id=n.children[ci]; expect=n.chashes[ci];
    }
    setErr(e,ErrCode::CORRUPT,"tree depth exceeded"); return false;
}

bool BTree::existsTxidPrefix(const uint8_t* txid32, bool* found, ChainError* e) const {
    *found=false; uint32_t id=rootId_; Hash expect=rootHash_; Node n;
    for (int guard=0; guard<64; ++guard){
        if (!readNodeVerified(id,expect,&n,e)) return false;
        if (n.type==NT_LEAF){
            for (const LeafEntry& en: n.entries){ if (memcmp(en.key.data(),txid32,32)==0){ *found=true; return true; } }
            return true;
        }
        size_t ci=0;
        for (size_t i=0;i<n.keys.size();++i){ if (memcmp(txid32,n.keys[i].data(),32) >= 0) ci=i+1; else break; }
        id=n.children[ci]; expect=n.chashes[ci];
    }
    setErr(e,ErrCode::CORRUPT,"tree depth exceeded"); return false;
}

bool BTree::modRec(uint32_t id, const Hash& expect, const Key& k, const std::vector<uint8_t>* val,
                   uint32_t* outId, Hash* outHash, SplitResult* split, ChainError* e){
    Node n; if (!readNodeVerified(id,expect,&n,e)) return false;
    split->split=false; split->rightId=0;

    if (n.type==NT_LEAF){
        size_t pos=0; while (pos<n.entries.size() && keyCmp(n.entries[pos].key,k)<0) ++pos;
        if (val){ if (pos<n.entries.size() && keyCmp(n.entries[pos].key,k)==0) n.entries[pos].val=*val;
                  else { LeafEntry en; en.key=k; en.val=*val; n.entries.insert(n.entries.begin()+pos,en); } }
        else { if (pos<n.entries.size() && keyCmp(n.entries[pos].key,k)==0) n.entries.erase(n.entries.begin()+pos); }
        if (leafSize(n)<=PAGE_SIZE){
            uint8_t ob[PAGE_SIZE]; encodeNode(n,ob); uint32_t nid; if(!appendPage(ob,&nid,e)) return false;
            *outId=nid; *outHash=leafHash(n); return true;
        }
        size_t half=n.entries.size()/2;
        Node L,R; L.type=R.type=NT_LEAF;
        L.entries.assign(n.entries.begin(), n.entries.begin()+half);
        R.entries.assign(n.entries.begin()+half, n.entries.end());
        if (R.entries.empty()){ setErr(e,ErrCode::INTERNAL,"leaf split empty right"); return false; }
        uint8_t lb[PAGE_SIZE], rb[PAGE_SIZE]; if(!encodeNode(L,lb)||!encodeNode(R,rb)){ setErr(e,ErrCode::INTERNAL,"leaf split overflow"); return false; }
        uint32_t lid,rid; if(!appendPage(lb,&lid,e)) return false; if(!appendPage(rb,&rid,e)) return false;
        *outId=lid; *outHash=leafHash(L); split->split=true; split->sep=R.entries.front().key; split->rightId=rid; split->rightHash=leafHash(R);
        return true;
    }

    // internal
    size_t ci=0; while (ci<n.keys.size() && keyCmp(k,n.keys[ci])>=0) ++ci;
    uint32_t ncid; Hash nch; SplitResult csr;
    if (!modRec(n.children[ci], n.chashes[ci], k, val, &ncid, &nch, &csr, e)) return false;
    n.children[ci]=ncid; n.chashes[ci]=nch;
    if (csr.split){ n.keys.insert(n.keys.begin()+ci, csr.sep); n.children.insert(n.children.begin()+ci+1, csr.rightId); n.chashes.insert(n.chashes.begin()+ci+1, csr.rightHash); }
    if (internalSize(n)<=PAGE_SIZE){
        uint8_t ob[PAGE_SIZE]; encodeNode(n,ob); uint32_t nid; if(!appendPage(ob,&nid,e)) return false;
        *outId=nid; *outHash=internalHash(n); return true;
    }
    size_t half=n.keys.size()/2;
    Node L,R; L.type=R.type=NT_INTERNAL;
    L.keys.assign(n.keys.begin(), n.keys.begin()+half);
    L.children.assign(n.children.begin(), n.children.begin()+half+1);
    L.chashes.assign(n.chashes.begin(), n.chashes.begin()+half+1);
    Key sep=n.keys[half];
    R.keys.assign(n.keys.begin()+half+1, n.keys.end());
    R.children.assign(n.children.begin()+half+1, n.children.end());
    R.chashes.assign(n.chashes.begin()+half+1, n.chashes.end());
    uint8_t lb[PAGE_SIZE], rb[PAGE_SIZE]; if(!encodeNode(L,lb)||!encodeNode(R,rb)){ setErr(e,ErrCode::INTERNAL,"internal split overflow"); return false; }
    uint32_t lid,rid; if(!appendPage(lb,&lid,e)) return false; if(!appendPage(rb,&rid,e)) return false;
    *outId=lid; *outHash=internalHash(L); split->split=true; split->sep=sep; split->rightId=rid; split->rightHash=internalHash(R);
    return true;
}

bool BTree::put(const Key& k, const std::vector<uint8_t>& val, ChainError* e){
    uint32_t nid; Hash nh; SplitResult sr;
    if (!modRec(rootId_, rootHash_, k, &val, &nid, &nh, &sr, e)) return false;
    if (sr.split){
        Node r; r.type=NT_INTERNAL; r.children={nid, sr.rightId}; r.chashes={nh, sr.rightHash}; r.keys={sr.sep};
        uint8_t ob[PAGE_SIZE]; if(!encodeNode(r,ob)){ setErr(e,ErrCode::INTERNAL,"root overflow"); return false; }
        uint32_t rid; if(!appendPage(ob,&rid,e)) return false;
        rootId_=rid; rootHash_=internalHash(r); rootIsLeaf_=false;
    } else { rootId_=nid; rootHash_=nh; }
    return true;
}
bool BTree::erase(const Key& k, ChainError* e){
    uint32_t nid; Hash nh; SplitResult sr;
    if (!modRec(rootId_, rootHash_, k, NULL, &nid, &nh, &sr, e)) return false;
    if (sr.split){ setErr(e,ErrCode::INTERNAL,"erase produced split"); return false; }
    rootId_=nid; rootHash_=nh; return true;
}

bool BTree::collectRec(uint32_t id, const Hash& expect, std::vector<LeafEntry>* out, ChainError* e) const {
    Node n; if (!readNodeVerified(id,expect,&n,e)) return false;
    if (n.type==NT_LEAF){ for (const LeafEntry& en: n.entries) out->push_back(en); return true; }
    for (size_t i=0;i<n.children.size();++i) if (!collectRec(n.children[i],n.chashes[i],out,e)) return false;
    return true;
}
bool BTree::collectAll(std::vector<LeafEntry>* out, ChainError* e) const {
    out->clear();
    return collectRec(rootId_, rootHash_, out, e);
}
bool BTree::verifyRec(uint32_t id, const Hash& expect, ChainError* e) const {
    Node n; if (!readNodeVerified(id,expect,&n,e)) return false;
    if (n.type==NT_INTERNAL){ for (size_t i=0;i<n.children.size();++i) if(!verifyRec(n.children[i], n.chashes[i], e)) return false; }
    return true;
}
bool BTree::verifyAll(ChainError* e) const { return verifyRec(rootId_, rootHash_, e); }

bool BTree::keysPrefixRec(uint32_t id, const Hash& expect, const Key& lo, const Key& hi, std::vector<Key>* out, ChainError* e) const {
    Node n; if (!readNodeVerified(id,expect,&n,e)) return false;
    if (n.type==NT_LEAF){
        for (const LeafEntry& en: n.entries){ if (keyCmp(en.key,lo)>=0 && keyCmp(en.key,hi)<=0) out->push_back(en.key); }
        return true;
    }
    size_t count=n.keys.size();
    for (size_t i=0;i<count+1;++i){
        bool overlap = true;
        if (i>0 && keyCmp(n.keys[i-1], hi) > 0) overlap=false;
        if (i<count && keyCmp(n.keys[i], lo) < 0) overlap=false;
        if (overlap) if (!keysPrefixRec(n.children[i], n.chashes[i], lo, hi, out, e)) return false;
    }
    return true;
}
bool BTree::keysWithTxidPrefix(const uint8_t* txid32, std::vector<Key>* out, ChainError* e) const {
    out->clear();
    Key lo{}, hi{}; memcpy(lo.data(),txid32,32); memcpy(hi.data(),txid32,32);
    for (int i=0;i<4;i++){ lo[32+i]=0x00; hi[32+i]=0xff; }
    return keysPrefixRec(rootId_, rootHash_, lo, hi, out, e);
}

}} // namespace cs::store3a
