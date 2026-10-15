// Stage 3A - focused test suite: B+tree unit, differential vs Stage-2B reference,
// crash barriers, corruption, scaling. Isolated runtime dir; no production paths.
#include "chainstate.h"          // reference model (cs::UtxoState, cs::buildDelta)
#include "store3a/incstore.h"
#include "store3a/incdelta.h"
#include "store3a/btree.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <time.h>
#include <stdlib.h>

using namespace cs;
using namespace cs::store3a;

static const int64_t COIN = 100000000LL;
static const int32_t MAT = 1;
static int g_pass=0,g_fail=0; static std::vector<std::string> g_failmsg; static std::string g_cur;
#define CHECK(c,m) do{ if(c) g_pass++; else { g_fail++; g_failmsg.push_back(g_cur+": "+(m)); } }while(0)
static long readVmHWM();

static std::string baseDir(){ return "/home/user/data/chainstate-3a-runtime/"; }
static std::string rmTree(const std::string& d){ std::string c="rm -rf '"+d+"'"; int r=system(c.c_str()); (void)r; return d; }
static std::string freshDir(const std::string& name){ std::string b=baseDir(); std::string d=b+name; rmTree(d); mkdir(b.c_str(),0755); mkdir(d.c_str(),0755); return d; }
static long fileSize(const std::string& p){ struct stat st; return stat(p.c_str(),&st)==0?(long)st.st_size:-1; }
static bool writeJunk(const std::string& p,const char* j){ FILE*f=fopen(p.c_str(),"wb"); if(!f)return false; size_t n=strlen(j); bool ok=fwrite(j,1,n,f)==n; fclose(f); return ok; }
static bool flipByte(const std::string& p,long off){ FILE*f=fopen(p.c_str(),"r+b"); if(!f)return false; if(fseek(f,off,SEEK_SET)!=0){fclose(f);return false;} int c=fgetc(f); if(c==EOF){fclose(f);return false;} fseek(f,off,SEEK_SET); fputc(c^0xFF,f); fclose(f); return true; }

// deterministic 32-byte id from a label + index
static Hash id32(uint32_t label, uint32_t i){ uint8_t b[8]; for(int k=0;k<4;k++){b[k]=(uint8_t)((label>>(8*k))&0xff);} for(int k=0;k<4;k++){b[4+k]=(uint8_t)((i>>(8*k))&0xff);} return sha256(b,8); }

// ---- deterministic block generator (feed to both models) -------------------
struct Gen {
    Hash cid; std::vector<Hash> coinbaseTxids;     // txid by height (coinbase)
    uint32_t ntx=0;
    Block make(int height) {
        Block b; b.hash = id32(0xB10C0000u,(uint32_t)height); b.height=height;
        // spend up to 2 mature coinbase outpoints (height+MAT<=height_block)
        int spent=0;
        for (size_t i=0; i<coinbaseTxids.size() && spent<2; ++i){
            int32_t ch = (int32_t)(i+1);        // coinbase txid index i corresponds to height i+1
            if (coinbaseTxids[i] == Hash()) continue;   // already spent
            if (ch + MAT <= height){
                OutPoint prev; prev.txid=coinbaseTxids[i]; prev.n=0;
                Tx sp; sp.txid = id32(0x5BE7D000u, ++ntx); sp.coinbase=false; sp.coinstake=false;
                TxIn in; in.prev=prev; sp.vin.push_back(in);
                TxOut o; o.value = 50*COIN - 1000; o.script={3,4}; sp.vout.push_back(o); // change
                b.txs.push_back(sp);
                coinbaseTxids[i] = Hash();          // mark spent
                ++spent;
            }
        }
        // coinbase
        Tx cb; cb.txid = id32(0xC0FFEE00u,(uint32_t)height); cb.coinbase=true; cb.coinstake=false;
        TxOut o; o.value = 50*COIN; o.script={1,2,3}; cb.vout.push_back(o);
        b.txs.push_back(cb);
        coinbaseTxids.push_back(cb.txid);
        return b;
    }
};

// ---- logical state extraction ---------------------------------------------
static bool storeToState(CommitStore3A& s, UtxoState* out, ChainError* e){
    std::vector<LeafEntry> ents; if(!const_cast<BTree&>(s.tree()).collectAll(&ents,e)) return false;
    out->map.clear();
    for (auto& en: ents){ size_t off=0; Coin c; if(!c.decode(en.val.data(),en.val.size(),&off)){ setErr(e,ErrCode::CORRUPT,"coin decode"); return false; } out->map[en.key]=c; }
    return true;
}
static bool publishRef(UtxoState& ref, const Block& b, ChainError* e){
    Delta d; UtxoState ns; if(!cs::buildDelta(ref,b,MAT,&d,&ns,e)) return false; ref=ns; return true;
}
static bool publishStore(CommitStore3A& s, const Block& b, ChainError* e){
    TreeReader tr(s.tree()); Delta d; if(!buildDeltaI(tr,b,MAT,&d,e)) return false; return s.publish(d,b.hash,b.height,e);
}

static bool setupOne3A(const std::string& dir, Hash* digest, ChainError* e){
    CommitStore3A s; if(!s.open(dir,e)) return false;
    Block b; b.hash=id32(0xAAAA0000u,1); b.height=1; Tx cb; cb.txid=id32(0xCB000000u,1); cb.coinbase=true; TxOut o; o.value=50*COIN; o.script={9}; cb.vout.push_back(o); b.txs.push_back(cb);
    if(!publishStore(s,b,e)){ s.close(); return false; }
    UtxoState st; if(!storeToState(s,&st,e)){ s.close(); return false; } *digest=st.digest(); s.close(); return true;
}

// ===========================================================================
static void test_b0_btree(){
    g_cur="B0-btree";
    std::string dir=freshDir("b0"); ChainError e;
    BTree t; CHECK(t.create(dir+"/tree.dat",&e), "create: "+e.str());
    const int N=500;
    for (int i=0;i<N;i++){ Key k{}; Hash h=id32(0x1u, (uint32_t)i); memcpy(k.data(),h.data(),32); k[32]=(uint8_t)i;
        std::vector<uint8_t> v={ (uint8_t)i, (uint8_t)(i>>8) }; CHECK(t.put(k,v,&e), "put "+std::to_string(i)+": "+e.str()); }
    CHECK(t.verifyAll(&e), "merkle verify after inserts: "+e.str());
    bool allok=true; for(int i=0;i<N;i++){ Key k{}; Hash h=id32(0x1u,(uint32_t)i); memcpy(k.data(),h.data(),32); k[32]=(uint8_t)i;
        std::vector<uint8_t> v; bool f=false; if(!t.get(k,&v,&f,&e)||!f||v.size()!=2||v[0]!=(uint8_t)i) allok=false; }
    CHECK(allok, "get all keys after splits");
    // erase half
    for(int i=0;i<N;i+=2){ Key k{}; Hash h=id32(0x1u,(uint32_t)i); memcpy(k.data(),h.data(),32); k[32]=(uint8_t)i; CHECK(t.erase(k,&e), "erase "+std::to_string(i)); }
    std::vector<LeafEntry> ents; CHECK(t.collectAll(&ents,&e), "collectAll: "+e.str());
    CHECK(ents.size()==(size_t)(N/2), "size after erase == N/2");
    bool sorted=true; for(size_t i=1;i<ents.size();++i) if(keyCmp(ents[i-1].key,ents[i].key)>=0) sorted=false;
    CHECK(sorted, "collected entries sorted");
    CHECK(t.verifyAll(&e), "merkle verify after erase");
    // txid prefix scan: even i were erased, odd i remain
    Hash probe8=id32(0x1u,8); std::vector<Key> ks; CHECK(t.keysWithTxidPrefix(probe8.data(),&ks,&e), "prefix scan: "+e.str());
    CHECK(ks.empty(), "prefix scan: erased key absent");
    Hash probe9=id32(0x1u,9); std::vector<Key> ks2; CHECK(t.keysWithTxidPrefix(probe9.data(),&ks2,&e), "prefix scan2: "+e.str());
    CHECK(ks2.size()==1, "prefix scan: present key found");
    t.close();
}

// ---------------------------------------------------------------------------
static void test_d1_differential(){
    g_cur="D1-diff";
    std::string dir=freshDir("d1"); ChainError e;
    CommitStore3A s; CHECK(s.open(dir,&e), "open: "+e.str());
    UtxoState ref; Gen gen;
    bool ok=true;
    const int N=200;
    for (int h=1;h<=N;h++){
        Block b=gen.make(h);
        if(!publishRef(ref,b,&e)){ CHECK(false,"ref build h="+std::to_string(h)+": "+e.str()); ok=false; break; }
        if(!publishStore(s,b,&e)){ CHECK(false,"store publish h="+std::to_string(h)+": "+e.str()); ok=false; break; }
        if (h%25==0){
            UtxoState st; if(!storeToState(s,&st,&e)){ CHECK(false,"extract"); ok=false; break; }
            CHECK(st.digest()==ref.digest(), "logical digest matches reference at h="+std::to_string(h));
            CHECK(st.map.size()==ref.map.size(), "utxo count matches at h="+std::to_string(h));
        }
    }
    if (ok){
        UtxoState st; storeToState(s,&st,&e);
        CHECK(st.digest()==ref.digest(), "final logical digest matches reference");
        CHECK(s.tipHeight()==N, "tip height == N");
        CHECK(s.tipHash()==id32(0xB10C0000u,(uint32_t)N), "tip hash matches");
        CHECK(s.tree().verifyAll(&e), "merkle verify final: "+e.str());
        Hash rootA=s.rootHash(); uint64_t genA=s.generation();
        // disconnect differential: ref must equal pre-last-block state
        UtxoState refBefore = ref; // we don't have pre-state; recompute: disconnect and compare to a fresh ref of N-1 blocks
        Gen g2; UtxoState ref2; for(int h=1;h<=N-1;h++) publishRef(ref2,g2.make(h),&e);
        CHECK(s.disconnect(s.tipHash(),&e), "store disconnect last: "+e.str());
        UtxoState st2; storeToState(s,&st2,&e);
        CHECK(st2.digest()==ref2.digest(), "post-disconnect digest == reference N-1");
        CHECK(s.generation()==genA+1, "generation advanced on disconnect");
        // restart determinism
        s.close();
        CommitStore3A r; CHECK(r.open(dir,&e), "reopen: "+e.str());
        CHECK(r.generation()==genA+1, "restart generation");
        CHECK(r.rootHash()==s.rootHash(), "restart root hash stable");
        CHECK(r.tree().verifyAll(&e), "restart merkle verify");
        UtxoState st3; storeToState(r,&st3,&e);
        CHECK(st3.digest()==st2.digest(), "restart logical state == post-disconnect");
        r.close();
        (void)rootA;
    } else s.close();
}

// ---------------------------------------------------------------------------
static Hash expectedBlock2(const std::string& dir, uint64_t* gen, ChainError* e){
    // clean run: setupOne + publish one spend block, return logical digest
    CommitStore3A s; Hash out; s.open(dir,e);
    Block b; b.hash=id32(0xAAAA0000u,2); b.height=2;
    OutPoint prev; prev.txid=id32(0xCB000000u,1); prev.n=0;
    Tx sp; sp.txid=id32(0x55000000u,1); sp.coinbase=false; TxIn in; in.prev=prev; sp.vin.push_back(in);
    TxOut o; o.value=40*COIN; o.script={9,9}; sp.vout.push_back(o); b.txs.push_back(sp);
    publishStore(s,b,e);
    UtxoState st; storeToState(s,&st,e); out=st.digest(); *gen=s.generation(); s.close(); return out;
}
static void childCrash(const std::string& dir, CrashPoint cp, InjectStage st, const std::string& mode){
    ChainError e; CommitStore3A s; if(!s.open(dir,&e)) ::_exit(2);
    Block b; b.hash=id32(0xAAAA0000u,2); b.height=2;
    OutPoint prev; prev.txid=id32(0xCB000000u,1); prev.n=0;
    Tx sp; sp.txid=id32(0x55000000u,1); sp.coinbase=false; TxIn in; in.prev=prev; sp.vin.push_back(in);
    TxOut o; o.value=40*COIN; o.script={9,9}; sp.vout.push_back(o); b.txs.push_back(sp);
    TreeReader tr(s.tree()); Delta d; if(!buildDeltaI(tr,b,MAT,&d,&e)) ::_exit(3);
    if(mode=="crash") s.setCrashPoint(cp);
    if(mode=="nospace"){ s.setInjectStage(st); s.injectNoSpace(true); }
    if(mode=="fsyncfail"){ s.setInjectStage(st); s.injectFsyncFail(true); }
    bool ok=s.publish(d,b.hash,b.height,&e);
    if(mode=="crash") ::_exit(4);
    ::_exit(ok?0:1);
}
static bool recover3A(const std::string& dir, Hash* dig, uint64_t* gen, ChainError* e){
    CommitStore3A s; if(!s.open(dir,e)) return false; UtxoState st; storeToState(s,&st,e); *dig=st.digest(); *gen=s.generation(); s.close(); return true;
}

static void test_c1_crash(){
    g_cur="C1-crash";
    ChainError e;
    struct CP { const char* name; CrashPoint cp; };
    CP cps[]={ {"BeforeRename",CrashPoint::BeforeRename}, {"AfterRename",CrashPoint::AfterRename}, {"AfterDirSync",CrashPoint::AfterDirSync} };
    for (CP& c: cps){
        std::string dir=freshDir(std::string("c1_")+c.name);
        Hash dOld; CHECK(setupOne3A(dir,&dOld,&e), std::string(c.name)+" setup: "+e.str());
        // expected NEW digest from a clean clone
        std::string dirE=freshDir(std::string("c1e_")+c.name); Hash dOldE; setupOne3A(dirE,&dOldE,&e);
        uint64_t gNew=0; Hash dNew=expectedBlock2(dirE,&gNew,&e);
        pid_t pid=fork();
        if(pid==0){ childCrash(dir,c.cp,InjectStage::None,"crash"); ::_exit(7); }
        int st=0; waitpid(pid,&st,0);
        CHECK(WIFEXITED(st)&&WEXITSTATUS(st)==42, std::string(c.name)+" child crashed at barrier");
        Hash dr; uint64_t gr; CHECK(recover3A(dir,&dr,&gr,&e), std::string(c.name)+" reopen: "+e.str());
        if (c.cp==CrashPoint::BeforeRename){
            CHECK(gr==1 && dr==dOld, std::string(c.name)+" deterministically OLD");
        } else if (c.cp==CrashPoint::AfterDirSync){
            CHECK(gr==2 && dr==dNew, std::string(c.name)+" deterministically NEW");
        } else {
            bool oldB=(gr==1&&dr==dOld), newB=(gr==2&&dr==dNew);
            CHECK(oldB||newB, std::string(c.name)+" exactly OLD or NEW (non-mixed)");
        }
        Hash dr2; uint64_t gr2; CHECK(recover3A(dir,&dr2,&gr2,&e), std::string(c.name)+" reopen2");
        CHECK(dr2==dr&&gr2==gr, std::string(c.name)+" idempotent recovery");
    }
}

// ---------------------------------------------------------------------------
static void test_k1_corruption(){
    g_cur="K1-corrupt";
    ChainError e;
    { // (a) damaged ROOT page -> open detects (O(1)) and fails loud (acknowledged gen corrupt)
      std::string dir=freshDir("k1a"); Hash d1; CHECK(setupOne3A(dir,&d1,&e),"k1a setup: "+e.str());
      { CommitStore3A p; CHECK(p.open(dir,&e),"k1a open: "+e.str()); Block b; b.hash=id32(0xAAAA0000u,2); b.height=2; OutPoint prev; prev.txid=id32(0xCB000000u,1); prev.n=0;
        Tx sp; sp.txid=id32(0x55000000u,1); sp.coinbase=false; TxIn in; in.prev=prev; sp.vin.push_back(in); TxOut o; o.value=40*COIN; o.script={9,9}; sp.vout.push_back(o); b.txs.push_back(sp);
        TreeReader tr(p.tree()); Delta d; CHECK(buildDeltaI(tr,b,MAT,&d,&e),"k1a build: "+e.str()); CHECK(p.publish(d,b.hash,b.height,&e),"k1a publish: "+e.str()); p.close(); }
      { CommitStore3A r; CHECK(r.open(dir,&e),"k1a reopen: "+e.str()); uint32_t rid=r.rootPageId(); r.close(); CHECK(flipByte(dir+"/tree.dat",(long)rid*4096+6),"k1a damage root page"); }
      CommitStore3A c; bool ok=c.open(dir,&e);
      CHECK(!ok, "k1a damaged root page -> open fails loud (acknowledged corruption)");
    }
    { // (b) damaged undo body -> disconnect fails closed (per-record crc)
      std::string dir=freshDir("k1b"); Hash d1; setupOne3A(dir,&d1,&e);
      CommitStore3A p; p.open(dir,&e); Block b; b.hash=id32(0xAAAA0000u,2); b.height=2; OutPoint prev; prev.txid=id32(0xCB000000u,1); prev.n=0;
      Tx sp; sp.txid=id32(0x55000000u,1); sp.coinbase=false; TxIn in; in.prev=prev; sp.vin.push_back(in); TxOut o; o.value=40*COIN; o.script={9,9}; sp.vout.push_back(o); b.txs.push_back(sp);
      TreeReader tr(p.tree()); Delta d; buildDeltaI(tr,b,MAT,&d,&e); CHECK(p.publish(d,b.hash,b.height,&e),"k1b publish: "+e.str()); Hash tip=p.tipHash(); p.close();
      long usz=fileSize(dir+"/undo.dat"); CHECK(flipByte(dir+"/undo.dat",usz-1), "k1b corrupt undo body tail");
      CommitStore3A q; bool ok=q.open(dir,&e); CHECK(ok, "k1b open ok (body not read at open)"); 
      if(ok){ bool dok=q.disconnect(tip,&e); CHECK(!dok && e.code==ErrCode::CORRUPT, "k1b disconnect fails closed on undo-body corruption"); q.close(); }
    }
    { // (c) missing referenced tree.dat
      std::string dir=freshDir("k1c"); Hash d1; setupOne3A(dir,&d1,&e);
      CHECK(unlink((dir+"/tree.dat").c_str())==0, "k1c remove tree.dat");
      CommitStore3A c; bool ok=c.open(dir,&e); CHECK(!ok, "k1c missing tree.dat -> open fails");
    }
    { // (d) damaged MANIFEST + valid bak, acknowledged -> fail loud
      std::string dir=freshDir("k1d"); Hash d1; setupOne3A(dir,&d1,&e);
      { CommitStore3A p; p.open(dir,&e); Block b; b.hash=id32(0xAAAA0000u,2); b.height=2; OutPoint prev; prev.txid=id32(0xCB000000u,1); prev.n=0;
        Tx sp; sp.txid=id32(0x55000000u,1); sp.coinbase=false; TxIn in; in.prev=prev; sp.vin.push_back(in); TxOut o; o.value=40*COIN; o.script={9,9}; sp.vout.push_back(o); b.txs.push_back(sp);
        TreeReader tr(p.tree()); Delta d; buildDeltaI(tr,b,MAT,&d,&e); p.publish(d,b.hash,b.height,&e); p.close(); }
      writeJunk(dir+"/MANIFEST","GARBAGE-ACKED-MANIFEST-BYTES");
      CommitStore3A c; bool ok=c.open(dir,&e); CHECK(!ok, "k1d acknowledged MANIFEST damaged (valid bak) -> fail loud");
    }
    { // (e) corrupt ACK
      std::string dir=freshDir("k1e"); Hash d1; setupOne3A(dir,&d1,&e);
      CHECK(flipByte(dir+"/ACK",10), "k1e corrupt ACK crc");
      CommitStore3A c; bool ok=c.open(dir,&e); CHECK(!ok, "k1e corrupt ACK -> fail closed");
    }
    { // (f) stale ACK (gen1 vs gen2) -> reconcile forward
      std::string dir=freshDir("k1f"); Hash d1; setupOne3A(dir,&d1,&e);
      { CommitStore3A p; p.open(dir,&e); Block b; b.hash=id32(0xAAAA0000u,2); b.height=2; OutPoint prev; prev.txid=id32(0xCB000000u,1); prev.n=0;
        Tx sp; sp.txid=id32(0x55000000u,1); sp.coinbase=false; TxIn in; in.prev=prev; sp.vin.push_back(in); TxOut o; o.value=40*COIN; o.script={9,9}; sp.vout.push_back(o); b.txs.push_back(sp);
        TreeReader tr(p.tree()); Delta d; buildDeltaI(tr,b,MAT,&d,&e); p.publish(d,b.hash,b.height,&e); p.close(); }
      // write stale ACK gen=1
      { FILE*f=fopen((dir+"/ACK").c_str(),"wb"); uint8_t b[12]={0}; b[0]=1; uint32_t crc=csCrc32(b,8); for(int i=0;i<4;i++) b[8+i]=(uint8_t)((crc>>(8*i))&0xff); fwrite(b,1,12,f); fclose(f); }
      CommitStore3A c; bool ok=c.open(dir,&e); CHECK(ok && c.generation()==2, "k1f stale ACK reconciled to gen2");
      if(ok) c.close();
    }
    { // (g) future ACK -> fail closed
      std::string dir=freshDir("k1g"); Hash d1; setupOne3A(dir,&d1,&e);
      { FILE*f=fopen((dir+"/ACK").c_str(),"wb"); uint8_t b[12]={0}; b[0]=99; uint32_t crc=csCrc32(b,8); for(int i=0;i<4;i++) b[8+i]=(uint8_t)((crc>>(8*i))&0xff); fwrite(b,1,12,f); fclose(f); }
      CommitStore3A c; bool ok=c.open(dir,&e); CHECK(!ok, "k1g future ACK -> fail closed");
    }
}

// ---------------------------------------------------------------------------
static void test_s1_scaling(){
    g_cur="S1-scale";
    ChainError e;
    int sizes[3]={200,400,800};
    long perCommit[3]; double msPerCommit[3];
    for (int si=0; si<3; ++si){
        int N=sizes[si];
        std::string dir=freshDir("s1_"+std::to_string(N));
        CommitStore3A s; CHECK(s.open(dir,&e),"scale open: "+e.str());
        Gen gen; long before=fileSize(dir+"/tree.dat");
        struct timespec t0,t1; clock_gettime(CLOCK_MONOTONIC,&t0);
        for(int h=1;h<=N;h++){ Block b=gen.make(h); if(!publishStore(s,b,&e)){ CHECK(false,"scale publish h="+std::to_string(h)+": "+e.str()); break; } }
        clock_gettime(CLOCK_MONOTONIC,&t1);
        long after=fileSize(dir+"/tree.dat");
        perCommit[si]=(after-before)/N;
        msPerCommit[si]=((t1.tv_sec-t0.tv_sec)*1000.0+(t1.tv_nsec-t0.tv_nsec)/1e6)/N;
        size_t nstate=s.tree().rootCount();
        uint64_t writesPerCommit = s.tree().pageWrites()/(uint64_t)N;
        s.close();
        printf("  S1 N=%d treeBytes=%ld bytesPerCommit=%ld pageWritesPerCommit=%llu msPerCommit=%.3f peakRSS_KB=%ld\n", N, after, perCommit[si], (unsigned long long)writesPerCommit, msPerCommit[si], readVmHWM());
        (void)nstate;
    }
    // O(delta) criterion: per-commit write volume must NOT grow ~linearly with state size (4x from N=200..800)
    CHECK(perCommit[2] < perCommit[0]*3, "per-commit bytes do not grow linearly with state (800 vs 200)");
    printf("  S1 perCommit: 200=%ld 400=%ld 800=%ld bytes\n", perCommit[0], perCommit[1], perCommit[2]);
}

// ==================== R1 remediation regression ============================
static uint32_t rd32b(const uint8_t*p){ uint32_t v=0; for(int i=0;i<4;i++) v|=(uint32_t)p[i]<<(8*i); return v; }
static bool readRaw(const std::string& p, long off, uint8_t* buf, size_t n){ FILE*f=fopen(p.c_str(),"rb"); if(!f)return false; if(fseek(f,off,SEEK_SET)!=0){fclose(f);return false;} size_t r=fread(buf,1,n,f); fclose(f); return r==n; }
// walk the leftmost path; returns the first (non-root) leaf page id and sets depth
static uint32_t walkFirstLeaf(const std::string& path, uint32_t rootId, int* depth){
    uint32_t id=rootId; uint8_t pg[4096]; int d=1;
    for(int g=0;g<64;g++){ if(!readRaw(path,(long)id*4096,pg,4096)) return 0; if(pg[0]==1){ *depth=d; return id; } id=rd32b(pg+4); d++; }
    *depth=d; return id;
}
static uint32_t firstNonRootInternal(const std::string& path, uint32_t rootId){
    uint8_t pg[4096]; if(!readRaw(path,(long)rootId*4096,pg,4096)) return 0; if(pg[0]!=2) return 0;
    uint32_t c=rd32b(pg+4); if(!readRaw(path,(long)c*4096,pg,4096)) return 0; return pg[0]==2? c : 0;
}
static Block mkBlock2(){
    Block b; b.hash=id32(0xAAAA0000u,2); b.height=2; OutPoint prev; prev.txid=id32(0xCB000000u,1); prev.n=0;
    Tx sp; sp.txid=id32(0x55000000u,1); sp.coinbase=false; sp.coinstake=false; TxIn in; in.prev=prev; sp.vin.push_back(in);
    TxOut o; o.value=40*COIN; o.script={9,9}; sp.vout.push_back(o); b.txs.push_back(sp); return b;
}
static bool buildTreeDeep(const std::string& dir, int N, size_t valsz, BTree* t, ChainError* e){
    if(!t->create(dir+"/tree.dat",e)) return false;
    for(int i=0;i<N;i++){ Key k{}; Hash h=id32(0x1u,(uint32_t)i); memcpy(k.data(),h.data(),32); k[32]=(uint8_t)i;
        std::vector<uint8_t> v(valsz,(uint8_t)(i&0xff)); v[0]=(uint8_t)i; v[1]=(uint8_t)(i>>8);
        if(!t->put(k,v,e)) return false; }
    return true;
}
// locate the leaf page whose entries contain key k (test-only linear page scan)
static uint32_t findLeafWithKey(const std::string& path, uint32_t nextId, const Key& k){
    uint8_t pg[4096];
    for(uint32_t id=0; id<nextId; ++id){ if(!readRaw(path,(long)id*4096,pg,4096)) continue; if(pg[0]!=1) continue;
        uint16_t cnt=(uint16_t)(pg[1]|(pg[2]<<8)); size_t off=4;
        for(uint16_t i=0;i<cnt;i++){ if(off+38>4096) break; Key ek; memcpy(ek.data(),pg+off,36);
            if(keyCmp(ek,k)==0) return id;
            uint16_t vl=(uint16_t)(pg[off+36]|(pg[off+37]<<8)); off+=36+2+vl; } }
    return 0xFFFFFFFFu;
}
// descend the COMMITTED tree from rootId to the leaf the key belongs to (avoids stale CoW pages)
static uint32_t committedLeafForKey(const std::string& path, uint32_t rootId, const Key& k){
    uint32_t id=rootId; uint8_t pg[4096];
    for(int g=0;g<64;g++){
        if(!readRaw(path,(long)id*4096,pg,4096)) return 0xFFFFFFFFu;
        if(pg[0]==1) return id;
        uint16_t cnt=(uint16_t)(pg[1]|(pg[2]<<8)); size_t sepOff=4+(size_t)(cnt+1)*36;
        size_t ci=0; while(ci<cnt){ Key s; memcpy(s.data(),pg+sepOff+ci*36,36); if(keyCmp(k,s)>=0) ci++; else break; }
        id=rd32b(pg+4+ci*36);
    }
    return 0xFFFFFFFFu;
}

static void test_m1_path_verify(){
    g_cur="M1-pathverify";
    ChainError e;
    // ---- (a) non-root LEAF corruption must fail closed for every traversal
    std::string dir=freshDir("m1a");
    BTree t; CHECK(buildTreeDeep(dir,3000,120,&t,&e), "m1a deep build: "+e.str());
    uint32_t root=t.rootId(); int depth=0; uint32_t leafId=walkFirstLeaf(dir+"/tree.dat",root,&depth);
    CHECK(depth>=3, "m1a tree has depth >= 3 (non-root internal present)");
    uint8_t lp[4096]; readRaw(dir+"/tree.dat",(long)leafId*4096,lp,4096);
    Key kk; memcpy(kk.data(),lp+4,36); Hash kkTxid; memcpy(kkTxid.data(),lp+4,32);
    std::vector<uint8_t> v; bool f=false; CHECK(t.get(kk,&v,&f,&e)&&f, "m1a get before corruption ok");
    uint64_t r0=t.pageReads(); t.get(kk,&v,&f,&e); uint64_t r1=t.pageReads();
    CHECK((r1-r0)==(uint64_t)depth, "m1a page reads per lookup == depth (O(depth), no O(state))");
    // flip one byte in the non-root leaf value area
    CHECK(flipByte(dir+"/tree.dat",(long)leafId*4096+42), "m1a flip non-root leaf byte");
    ChainError ge; bool gok=t.get(kk,&v,&f,&ge);
    CHECK(!gok && ge.code==ErrCode::CORRUPT, "m1a get on corrupt non-root leaf fails closed");
    Hash rootBefore=t.rootHash();
    ChainError pe; std::vector<uint8_t> nv(2,7);
    CHECK(!t.put(kk,nv,&pe), "m1a put on corrupt non-root leaf fails closed");
    CHECK(t.rootHash()==rootBefore, "m1a corrupt data did not yield a new authenticated root");
    ChainError ee; Key anyk{}; memcpy(anyk.data(),kkTxid.data(),32);
    CHECK(!t.erase(kk,&ee), "m1a erase on corrupt non-root leaf fails closed");
    ChainError be; bool bf=true; CHECK(!t.existsTxidPrefix(kkTxid.data(),&bf,&be), "m1a BIP30 lookup on corrupt leaf fails closed");
    ChainError se; std::vector<Key> ks; CHECK(!t.keysWithTxidPrefix(kkTxid.data(),&ks,&se), "m1a prefix scan on corrupt leaf fails closed");
    CHECK(t.truncatedTail()==false, "m1a no tail truncation (aligned file)");
    t.close();
    // ---- (b) non-root INTERNAL page corruption must also fail closed
    std::string dir2=freshDir("m1b");
    BTree t2; CHECK(buildTreeDeep(dir2,3000,120,&t2,&e), "m1b deep build: "+e.str());
    uint32_t iid=firstNonRootInternal(dir2+"/tree.dat",t2.rootId());
    CHECK(iid!=0, "m1b found a non-root internal page");
    if(iid){
        int dep2=0; uint32_t lf2=walkFirstLeaf(dir2+"/tree.dat",iid,&dep2);
        uint8_t lp2[4096]; readRaw(dir2+"/tree.dat",(long)lf2*4096,lp2,4096);
        Key kk2; memcpy(kk2.data(),lp2+4,36);   // first key of the leftmost leaf under that internal page
        std::vector<uint8_t> v2; bool f2=false; ChainError oe; CHECK(t2.get(kk2,&v2,&f2,&oe), "m1b get ok before corruption");
        CHECK(flipByte(dir2+"/tree.dat",(long)iid*4096+8), "m1b flip non-root internal child hash byte");
        ChainError ge2; bool gok2=t2.get(kk2,&v2,&f2,&ge2);
        CHECK(!gok2 && ge2.code==ErrCode::CORRUPT, "m1b get under corrupt non-root internal fails closed");
        ChainError pe2; CHECK(!t2.put(kk2,v2,&pe2), "m1b put under corrupt internal fails closed");
    }
    t2.close();
    // ---- (c) store level: corrupt a non-root leaf in a multi-level store tree
    std::string dir3=freshDir("m1c");
    Hash lastcb; Hash lastTip; uint32_t lleaf=0xFFFFFFFFu; uint32_t lroot=0;
    { CommitStore3A s; CHECK(s.open(dir3,&e), "m1c open: "+e.str()); Gen g; bool okb=true;
      for(int h=1;h<=300;h++){ Block b=g.make(h); if(!publishStore(s,b,&e)){ CHECK(false,"m1c publish h="+std::to_string(h)+": "+e.str()); okb=false; break; } }
      if(okb){ lastcb=g.coinbaseTxids.back(); lastTip=id32(0xB10C0000u,300); lroot=s.rootPageId(); uint32_t nid=s.tree().nextPageId();
        int dep=0; uint32_t rl=walkFirstLeaf(dir3+"/tree.dat",lroot,&dep); (void)rl;
        CHECK(dep>=2, "m1c store tree has a non-root leaf");
        Key lk{}; memcpy(lk.data(),lastcb.data(),32);
        lleaf=findLeafWithKey(dir3+"/tree.dat",nid,lk);
        CHECK(lleaf!=0xFFFFFFFFu, "m1c located leaf holding the last coinbase key");
        CHECK(lleaf!=lroot, "m1c target leaf is not the root page (non-root corruption)");
      }
      s.close(); }
    if(lleaf!=0xFFFFFFFFu && lleaf!=lroot){
        CHECK(flipByte(dir3+"/tree.dat",(long)lleaf*4096+42), "m1c flip non-root leaf in store");
        OutPoint op; op.txid=lastcb; op.n=0;
        { CommitStore3A r; bool ok=r.open(dir3,&e); CHECK(ok, "m1c open ok (root intact): "+e.str());
          if(ok){ ChainError fe; Coin c; bool found=false;
            bool gok=r.get(op,&c,&found,&fe); CHECK(!gok, "m1c store get on corrupt non-root leaf fails closed");
            ChainError de; bool dok=r.disconnect(lastTip,&de); CHECK(!dok, "m1c disconnect touching corrupt leaf fails closed");
            r.close(); } }
        // ---- (d) restart over corruption: root verifies at open, corruption detected on access
        { CommitStore3A r; bool ok=r.open(dir3,&e); CHECK(ok, "m1d restart open ok (root hash verified O(1))");
          if(ok){ ChainError fe; Coin c; bool found=false;
            CHECK(!r.get(op,&c,&found,&fe), "m1d after restart corrupt non-root leaf still fails closed"); r.close(); } }
    }
}

static void test_p6_partial_append(){
    g_cur="P6-partialappend";
    ChainError e;
    // expected NEW digest from a clean clone
    std::string dirE=freshDir("p6e"); Hash d1e; CHECK(setupOne3A(dirE,&d1e,&e), "p6e setup: "+e.str());
    uint64_t gNew=0; Hash dNew=expectedBlock2(dirE,&gNew,&e);
    // in-process interrupted append (real short write, before any fsync)
    for (size_t cut : { (size_t)1, (size_t)1500, (size_t)4095 }){
        std::string tag="p6_"+std::to_string(cut); std::string dir=freshDir(tag); Hash d1; CHECK(setupOne3A(dir,&d1,&e), tag+" setup: "+e.str());
        { CommitStore3A s; s.open(dir,&e); Block b=mkBlock2(); TreeReader tr(s.tree()); Delta d; buildDeltaI(tr,b,MAT,&d,&e);
          s.injectPartialAppendOnce(cut);
          struct timespec a,c2; clock_gettime(CLOCK_MONOTONIC,&a);
          bool ok=s.publish(d,b.hash,2,&e); clock_gettime(CLOCK_MONOTONIC,&c2);
          double ms=((c2.tv_sec-a.tv_sec)*1000.0+(c2.tv_nsec-a.tv_nsec)/1e6);
          CHECK(!ok, tag+" publish fails on interrupted append");
          if(cut==1500) printf("  P6 inject=%zu publishLatencyMs=%.3f\n", cut, ms);
          s.close(); }
        long sz=fileSize(dir+"/tree.dat");
        CHECK(sz%4096!=(long)0, tag+" tree.dat left non-aligned by the interrupted append");
        struct timespec a,c2; clock_gettime(CLOCK_MONOTONIC,&a);
        CommitStore3A r; bool ok=r.open(dir,&e); clock_gettime(CLOCK_MONOTONIC,&c2);
        double rms=((c2.tv_sec-a.tv_sec)*1000.0+(c2.tv_nsec-a.tv_nsec)/1e6);
        CHECK(ok, tag+" recovers intact acknowledged generation: "+e.str());
        if(ok){
            CHECK(r.generation()==1, tag+" recovered generation == 1 (OLD retained)");
            UtxoState st; storeToState(r,&st,&e); CHECK(st.digest()==d1, tag+" recovered digest == committed OLD");
            CHECK(r.truncatedTreeTail(), tag+" reported unreferenced tail drop");
            long sz2=fileSize(dir+"/tree.dat"); CHECK(sz2%4096==0 && sz2<=sz, tag+" tree aligned after recovery, no whole page lost");
            CHECK(r.tree().verifyAll(&e), tag+" merkle verify after recovery");
            if(cut==1500) printf("  P6 recoveryLatencyMs=%.3f\n", rms);
            r.close();
            CommitStore3A r2; CHECK(r2.open(dir,&e), tag+" repeated restart ok");
            CHECK(r2.generation()==1 && !r2.truncatedTreeTail(), tag+" repeated restart idempotent (no further truncation)");
            r2.tree().verifyAll(&e); r2.close();
            // clean publish now reaches the NEW state identically to the clean clone
            CommitStore3A s; s.open(dir,&e); Block b=mkBlock2(); TreeReader tr(s.tree()); Delta d; buildDeltaI(tr,b,MAT,&d,&e);
            CHECK(s.publish(d,b.hash,2,&e), tag+" clean publish after recovery");
            UtxoState st2; storeToState(s,&st2,&e); CHECK(st2.digest()==dNew, tag+" post-recovery state == clean NEW digest");
            uint64_t g2=s.generation(); CHECK(g2==2, tag+" generation back to 2");
            s.close();
        }
    }
    // fork-crash variant: process dies right after the partial page hit the file
    std::string dirF=freshDir("p6f"); Hash dF; CHECK(setupOne3A(dirF,&dF,&e), "p6f setup");
    pid_t pid=fork();
    if(pid==0){ ChainError ce; CommitStore3A s; if(!s.open(dirF,&ce)) ::_exit(2);
        Block b=mkBlock2(); TreeReader tr(s.tree()); Delta d; if(!buildDeltaI(tr,b,MAT,&d,&ce)) ::_exit(3);
        s.injectPartialAppendOnce(2000); s.publish(d,b.hash,2,&ce); ::_exit(42); }
    int stt=0; waitpid(pid,&stt,0);
    CHECK(WIFEXITED(stt)&&WEXITSTATUS(stt)==42, "p6f child died mid-append");
    Hash dr; uint64_t gr; CHECK(recover3A(dirF,&dr,&gr,&e), "p6f reopen: "+e.str());
    CHECK(gr==1 && dr==dF, "p6f fork-crash recovers committed gen1 intact");
    CHECK(fileSize(dirF+"/tree.dat")%4096==0, "p6f tree aligned after fork-crash recovery");
    Hash dr2; uint64_t gr2; CHECK(recover3A(dirF,&dr2,&gr2,&e), "p6f reopen2");
    CHECK(dr2==dr&&gr2==gr, "p6f repeated recovery idempotent");
}

// ==================== R2 fresh-open contract regression ====================
static bool fexists(const std::string& p){ struct stat st; return ::stat(p.c_str(),&st)==0; }
static long fsz(const std::string& p){ struct stat st; return ::stat(p.c_str(),&st)==0?(long)st.st_size:-1; }
static Hash fdig(const std::string& p){ FILE* f=fopen(p.c_str(),"rb"); if(!f) return Hash(); ChainedHash h; uint8_t b[65536]; size_t n; while((n=fread(b,1,sizeof(b),f))>0) h.put(b,n); fclose(f); return h.final(); }
static bool publishCB(CommitStore3A& s, int h, ChainError* e){
    Block b; b.hash=id32(0xB10C0000u,(uint32_t)h); b.height=h;
    Tx cb; cb.txid=id32(0xCB000000u,(uint32_t)h); cb.coinbase=true; cb.coinstake=false;
    TxOut o; o.value=50*COIN; o.script={1,2,3}; cb.vout.push_back(o); b.txs.push_back(cb);
    TreeReader tr(s.tree()); Delta d; if(!buildDeltaI(tr,b,MAT,&d,e)) return false;
    return s.publish(d,b.hash,h,e);
}
static bool mkStore6(const std::string& dir, ChainError* e){
    CommitStore3A s; if(!s.open(dir,e)) return false;
    for(int h=1;h<=6;h++) if(!publishCB(s,h,e)){ s.close(); return false; }
    s.close(); return true;
}

static void test_r2_fresh_open(){
    g_cur="R2-freshopen";
    ChainError e;
    const char* M="MANIFEST"; const char* A="ACK"; const char* B="MANIFEST.bak";

    // R2-01 genuinely fresh empty directory creates correctly
    std::string d1=freshDir("r2_01");
    CHECK(!fexists(d1+"/tree.dat"), "R2-01 no tree.dat before first open");
    { CommitStore3A s; bool ok=s.open(d1,&e); CHECK(ok, "R2-01 fresh open: "+e.str());
      CHECK(s.generation()==0, "R2-01 fresh generation 0");
      CHECK(fexists(d1+"/tree.dat") && fsz(d1+"/tree.dat")==4096, "R2-01 empty sentinel page created (4096 B)");
      CHECK(!fexists(d1+"/MANIFEST"), "R2-01 no MANIFEST before first publish"); s.close(); }
    // R2-02 fresh store restarts correctly (byte-identical)
    { Hash dg=fdig(d1+"/tree.dat"); long sz=fsz(d1+"/tree.dat");
      CommitStore3A s; bool ok=s.open(d1,&e); CHECK(ok,"R2-02 restart: "+e.str());
      CHECK(s.generation()==0, "R2-02 restart generation 0");
      CHECK(fdig(d1+"/tree.dat")==dg && fsz(d1+"/tree.dat")==sz, "R2-02 tree.dat byte-identical across restart"); s.close(); }

    // R2-03 existing valid non-empty tree.dat with pointer files missing -> fail closed, unmodified
    std::string d3=freshDir("r2_03"); CHECK(mkStore6(d3,&e), "R2-03 build gen6: "+e.str());
    CHECK(fexists(d3+"/MANIFEST") && fexists(d3+"/ACK"), "R2-03 pointers present after commit");
    unlink((d3+"/"+M).c_str()); unlink((d3+"/"+A).c_str()); unlink((d3+"/"+B).c_str());
    { Hash dg=fdig(d3+"/tree.dat"); long sz=fsz(d3+"/tree.dat");
      CommitStore3A s; bool ok=s.open(d3,&e);
      CHECK(!ok, "R2-03 non-empty tree + no pointers -> FAIL CLOSED");
      CHECK(!fexists(d3+"/MANIFEST") && !fexists(d3+"/ACK"), "R2-03 no pointer files invented");
      CHECK(fdig(d3+"/tree.dat")==dg && fsz(d3+"/tree.dat")==sz, "R2-03 tree.dat byte-identical (no truncation)"); }

    // R2-04 existing corrupt tree.dat with pointer files missing -> fail closed, no truncation
    { std::string d=freshDir("r2_04a"); CHECK(mkStore6(d,&e),"R2-04a build");
      unlink((d+"/"+M).c_str()); unlink((d+"/"+A).c_str()); unlink((d+"/"+B).c_str());
      flipByte(d+"/tree.dat",0);
      Hash dg=fdig(d+"/tree.dat"); long sz=fsz(d+"/tree.dat");
      CommitStore3A s; bool ok=s.open(d,&e);
      CHECK(!ok, "R2-04a corrupt aligned tree.dat + no pointers -> FAIL CLOSED");
      CHECK(fdig(d+"/tree.dat")==dg && fsz(d+"/tree.dat")==sz, "R2-04a corrupt tree.dat untouched"); }
    { std::string d=freshDir("r2_04b"); CHECK(mkStore6(d,&e),"R2-04b build");
      unlink((d+"/"+M).c_str()); unlink((d+"/"+A).c_str()); unlink((d+"/"+B).c_str());
      { FILE* f=fopen((d+"/tree.dat").c_str(),"ab"); std::vector<uint8_t> j(1000,0x5A); size_t w=fwrite(j.data(),1,j.size(),f); (void)w; fclose(f); }
      Hash dg=fdig(d+"/tree.dat"); long sz=fsz(d+"/tree.dat"); CHECK(sz%4096!=0, "R2-04b tree.dat non-aligned");
      CommitStore3A s; bool ok=s.open(d,&e);
      CHECK(!ok, "R2-04b non-aligned tree.dat + no pointers -> FAIL CLOSED");
      CHECK(fdig(d+"/tree.dat")==dg && fsz(d+"/tree.dat")==sz, "R2-04b no tail truncation on the no-pointer path"); }

    // R2-05 / R2-06 unreadable or non-regular tree.dat must fail closed and must not create
    { std::string d=freshDir("r2_05a"); CHECK(mkStore6(d,&e),"R2-05a build");
      unlink((d+"/"+M).c_str()); unlink((d+"/"+A).c_str()); unlink((d+"/"+B).c_str());
      unlink((d+"/tree.dat").c_str()); CHECK(mkdir((d+"/tree.dat").c_str(),0755)==0, "R2-05a tree.dat replaced by a directory");
      CommitStore3A s; bool ok=s.open(d,&e); CHECK(!ok, "R2-05a non-regular tree.dat -> FAIL CLOSED");
      CHECK(fexists(d+"/tree.dat"), "R2-05a directory not deleted/replaced"); }
    { std::string d=freshDir("r2_06"); CHECK(mkStore6(d,&e),"R2-06 build");
      unlink((d+"/"+M).c_str()); unlink((d+"/"+A).c_str()); unlink((d+"/"+B).c_str());
      Hash dg=fdig(d+"/tree.dat"); long sz=fsz(d+"/tree.dat");
      bool nonroot=(::geteuid()!=0);
      if(nonroot){ CHECK(::chmod((d+"/tree.dat").c_str(),0000)==0, "R2-06 chmod 000 tree.dat");
        CommitStore3A s; bool ok=s.open(d,&e); CHECK(!ok, "R2-06 unreadable tree.dat (EACCES) -> FAIL CLOSED");
        ::chmod((d+"/tree.dat").c_str(),0644); }
      else printf("  R2-06 note: running as root; EACCES path not exercised\n");
      CHECK(fsz(d+"/tree.dat")==sz && fdig(d+"/tree.dat")==dg, "R2-06 stat/open error did not trigger creation or mutation"); }

    // R2-07 MANIFEST.bak present (primary+ACK missing) must not be ignored as evidence
    { std::string d=freshDir("r2_07"); CHECK(mkStore6(d,&e),"R2-07 build");
      CHECK(fexists(d+"/MANIFEST.bak"), "R2-07 backup present after commits");
      unlink((d+"/"+M).c_str()); unlink((d+"/"+A).c_str());
      Hash bdg=fdig(d+"/MANIFEST.bak"); long sz=fsz(d+"/tree.dat");
      CommitStore3A s; bool ok=s.open(d,&e);
      CHECK(!ok, "R2-07 backup-only state -> FAIL CLOSED (not treated as fresh)");
      CHECK(fdig(d+"/MANIFEST.bak")==bdg && fsz(d+"/tree.dat")==sz, "R2-07 backup and tree untouched"); }

    // R2-08 missing MANIFEST with acknowledged ACK must fail closed
    { std::string d=freshDir("r2_08"); CHECK(mkStore6(d,&e),"R2-08 build");
      unlink((d+"/"+M).c_str());
      Hash adg=fdig(d+"/ACK"); Hash tdg=fdig(d+"/tree.dat");
      CommitStore3A s; bool ok=s.open(d,&e); CHECK(!ok, "R2-08 ACK present, MANIFEST missing -> FAIL CLOSED");
      CHECK(fdig(d+"/ACK")==adg && fdig(d+"/tree.dat")==tdg, "R2-08 ACK and tree untouched");
      CHECK(!fexists(d+"/MANIFEST"), "R2-08 no MANIFEST invented"); }

    // R2-09 corrupt / future ACK must fail closed
    { std::string d=freshDir("r2_09a"); CHECK(mkStore6(d,&e),"R2-09a build");
      flipByte(d+"/ACK",10); CommitStore3A s; bool ok=s.open(d,&e); CHECK(!ok, "R2-09a corrupt ACK -> FAIL CLOSED"); }
    { std::string d=freshDir("r2_09b"); CHECK(mkStore6(d,&e),"R2-09b build");
      { FILE* f=fopen((d+"/ACK").c_str(),"wb"); uint8_t b[12]={0}; b[0]=99; uint32_t crc=csCrc32(b,8); for(int i=0;i<4;i++) b[8+i]=(uint8_t)((crc>>(8*i))&0xff); fwrite(b,1,12,f); fclose(f); }
      CommitStore3A s; bool ok=s.open(d,&e); CHECK(!ok, "R2-09b future ACK -> FAIL CLOSED"); }

    // R2-10 repeated restart after fail-closed preserves every file byte-for-byte
    { std::string d=freshDir("r2_10"); CHECK(mkStore6(d,&e),"R2-10 build");
      unlink((d+"/"+M).c_str()); unlink((d+"/"+A).c_str()); unlink((d+"/"+B).c_str());
      Hash t=fdig(d+"/tree.dat"); long ts=fsz(d+"/tree.dat");
      for(int i=0;i<3;i++){ CommitStore3A s; bool ok=s.open(d,&e); CHECK(!ok, "R2-10 fail-closed restart "+std::to_string(i)); }
      CHECK(fdig(d+"/tree.dat")==t && fsz(d+"/tree.dat")==ts, "R2-10 tree.dat byte-identical after repeated fail-closed opens"); }

    // R2-11 partial-page-tail recovery still preserves the acknowledged generation
    { std::string d=freshDir("r2_11"); CHECK(mkStore6(d,&e),"R2-11 build");
      { CommitStore3A s; CHECK(s.open(d,&e),"R2-11 reopen"); UtxoState st; storeToState(s,&st,&e); Hash dig=st.digest();
        s.close(); { FILE* f=fopen((d+"/tree.dat").c_str(),"ab"); std::vector<uint8_t> j(1200,0x33); size_t w=fwrite(j.data(),1,j.size(),f); (void)w; fclose(f); }
        CommitStore3A r; bool ok=r.open(d,&e); CHECK(ok, "R2-11 tail recovery open: "+e.str());
        if(ok){ CHECK(r.generation()==6, "R2-11 acknowledged generation preserved");
          UtxoState st2; storeToState(r,&st2,&e); CHECK(st2.digest()==dig, "R2-11 logical state preserved");
          CHECK(fsz(d+"/tree.dat")%4096==0, "R2-11 re-aligned"); r.close(); } } }

    // R2-12 interrupted fresh initialization never destroys pre-existing data
    { std::string d=freshDir("r2_12"); CHECK(mkStore6(d,&e),"R2-12 build");
      unlink((d+"/"+M).c_str()); unlink((d+"/"+A).c_str()); unlink((d+"/"+B).c_str());
      writeJunk(d+"/MANIFEST.tmp","PARTIAL-INIT-MARKER");
      Hash t=fdig(d+"/tree.dat"); long ts=fsz(d+"/tree.dat"); Hash tmp=fdig(d+"/MANIFEST.tmp");
      CommitStore3A s; bool ok=s.open(d,&e); CHECK(!ok, "R2-12 interrupted init over existing data -> FAIL CLOSED");
      CHECK(fdig(d+"/tree.dat")==t && fsz(d+"/tree.dat")==ts, "R2-12 existing tree data NOT destroyed");
      CHECK(fexists(d+"/MANIFEST.tmp") && fdig(d+"/MANIFEST.tmp")==tmp, "R2-12 pre-existing tmp preserved"); }

    // R2-13 successful creation followed by ordinary publish, disconnect, restart
    { std::string d=freshDir("r2_13");
      CommitStore3A s; CHECK(s.open(d,&e), "R2-13 fresh open: "+e.str());
      CHECK(publishCB(s,1,&e) && publishCB(s,2,&e), "R2-13 publish 2 blocks");
      CHECK(s.generation()==2, "R2-13 generation 2");
      Hash tip=s.tipHash();
      CHECK(s.disconnect(tip,&e), "R2-13 disconnect last: "+e.str());
      CHECK(s.generation()==3 && s.tipHeight()==1, "R2-13 generation advanced, tip height 1");
      UtxoState st; storeToState(s,&st,&e); Hash dig=st.digest(); uint64_t g=s.generation(); Hash rh=s.rootHash(); s.close();
      CommitStore3A r; CHECK(r.open(d,&e), "R2-13 restart: "+e.str());
      CHECK(r.generation()==g && r.rootHash()==rh, "R2-13 restart generation/root stable");
      UtxoState st2; storeToState(r,&st2,&e); CHECK(st2.digest()==dig, "R2-13 restart logical state stable"); r.close(); }

    // R2-14 existing empty generation-0 store with missing pointer files: explicit state contract
    { std::string d=freshDir("r2_14"); 
      { CommitStore3A s; CHECK(s.open(d,&e),"R2-14 create empty store: "+e.str()); s.close(); }
      Hash dg=fdig(d+"/tree.dat"); long sz=fsz(d+"/tree.dat");
      { CommitStore3A s; bool ok=s.open(d,&e); CHECK(ok, "R2-14 empty sentinel restart -> OPEN_EXISTING: "+e.str());
        CHECK(ok && s.generation()==0 && s.tree().rootCount()==0, "R2-14 explicit empty contract (gen0, 0 entries)");
        CHECK(fdig(d+"/tree.dat")==dg && fsz(d+"/tree.dat")==sz, "R2-14 sentinel untouched"); if(ok) s.close(); }
      // crafted single-page NON-empty leaf must NOT be accepted as empty
      std::string d2=freshDir("r2_14b");
      { Node n; n.type=NT_LEAF; LeafEntry en; Hash h=id32(0x1u,42); memcpy(en.key.data(),h.data(),32); en.key[32]=0; en.val={1,2,3,4};
        n.entries.push_back(en); uint8_t buf[4096]; CHECK(encodeNode(n,buf), "R2-14b encode one-entry leaf");
        FILE* f=fopen((d2+"/tree.dat").c_str(),"wb"); if(f){ size_t w=fwrite(buf,1,4096,f); (void)w; fclose(f); } }
      Hash dg2=fdig(d2+"/tree.dat");
      CommitStore3A s2; bool ok2=s2.open(d2,&e);
      CHECK(!ok2, "R2-14b single-page non-empty leaf without pointers -> FAIL CLOSED");
      CHECK(fdig(d2+"/tree.dat")==dg2, "R2-14b crafted tree untouched"); }
}

static void test_r2_multi_key_atomic(){
    g_cur="R2-atomic";
    ChainError e;
    std::string d=freshDir("r2_15");
    CommitStore3A s; CHECK(s.open(d,&e), "R2-15 open: "+e.str());
    const int N=120; bool pok=true;
    for(int h=1;h<=N;h++) if(!publishCB(s,h,&e)){ pok=false; break; }
    CHECK(pok, "R2-15 build "+std::to_string(N)+" blocks: "+e.str());
    CHECK(!s.tree().rootIsLeaf(), "R2-15 store tree is multi-level (root internal)");
    Hash rootOld=s.rootHash(); uint64_t genOld=s.generation(); uint32_t pagesOld=s.tree().nextPageId();
    UtxoState st; storeToState(s,&st,&e); Hash digOld=st.digest();
    Hash mdig=fdig(d+"/MANIFEST"); Hash adig=fdig(d+"/ACK"); long tsize=fsz(d+"/tree.dat");
    Hash cb2=id32(0xCB000000u,2);
    Key k2{}; memcpy(k2.data(),cb2.data(),32); k2[32]=0;
    uint32_t rootIdOld=s.rootPageId();
    uint32_t leafKb2=committedLeafForKey(d+"/tree.dat", rootIdOld, k2);
    CHECK(leafKb2!=0xFFFFFFFFu && leafKb2!=rootIdOld, "R2-15 erase-target leaf located on the committed path (non-root)");
    Hash cbNew=id32(0xCB000000u,5000); Key kNew{}; memcpy(kNew.data(),cbNew.data(),32); kNew[32]=0;
    int32_t H=121;
    Block b; b.hash=id32(0xB10C0000u,(uint32_t)H); b.height=H;
    Tx sp; sp.txid=id32(0x5BE7D000u,(uint32_t)H); sp.coinbase=false; sp.coinstake=false; TxIn in; in.prev.txid=cb2; in.prev.n=0; sp.vin.push_back(in);
    TxOut co; co.value=40*COIN; co.script={9,9}; sp.vout.push_back(co); b.txs.push_back(sp);        // op1 = erase kb2
    Tx cb; cb.txid=cbNew; cb.coinbase=true; cb.coinstake=false; TxOut o; o.value=50*COIN; o.script={1,2,3}; cb.vout.push_back(o); b.txs.push_back(cb); // op3 = put kNew
    TreeReader tr(s.tree()); Delta dl; CHECK(buildDeltaI(tr,b,MAT,&dl,&e), "R2-15 build multi-key delta: "+e.str());
    CHECK(dl.ops.size()>=3, "R2-15 delta is multi-key (>=3 ops)");
    // controlled failure on the 4th page append: earlier mutations stage pages, a later one fails
    s.injectPartialAppendOn(4);
    bool ok=s.publish(dl,b.hash,H,&e);
    CHECK(!ok, "R2-15 publish fails mid-way (controlled later-op append failure)");
    CHECK(s.generation()==genOld && s.rootHash()==rootOld, "R2-15 committed root/generation unchanged in memory");
    CHECK(s.tree().nextPageId()>pagesOld, "R2-15 earlier mutations staged pages before the failure");
    CHECK(fdig(d+"/MANIFEST")==mdig, "R2-15 MANIFEST bytes unchanged (no new generation durable)");
    CHECK(fdig(d+"/ACK")==adig, "R2-15 ACK bytes unchanged (no false acknowledgment)");
    CHECK(s.tree().verifyAll(&e), "R2-15 committed tree intact and verifies");
    { OutPoint op; op.txid=cb2; op.n=0; Coin c; bool found=false; ChainError ge2;
      CHECK(s.get(op,&c,&found,&ge2) && found, "R2-15 no op of the partial delta is visible"); }
    { UtxoState stc; storeToState(s,&stc,&e); CHECK(stc.digest()==digOld, "R2-15 committed logical state unchanged"); }
    uint32_t pagesAfter=s.tree().nextPageId(); s.close();
    CHECK(fsz(d+"/tree.dat")>tsize, "R2-15 staged/orphan pages present on disk after the failure");
    { CommitStore3A r; bool ok2=r.open(d,&e); CHECK(ok2, "R2-15 restart: "+e.str());
      if(ok2){ CHECK(r.generation()==genOld && r.rootHash()==rootOld, "R2-15 restart returns OLD generation/root");
        CHECK(r.rootPageId()==rootIdOld, "R2-15 committed root page unchanged across restart");
        UtxoState st2; storeToState(r,&st2,&e); CHECK(st2.digest()==digOld, "R2-15 restart logical state == OLD digest");
        CHECK(r.tree().verifyAll(&e), "R2-15 restart merkle verify (staged pages unreachable)");
        CHECK(r.tree().nextPageId()==pagesAfter, "R2-15 staged pages persist but are unreferenced (append-only)");
        r.close(); } }
}

static long readVmHWM(){ FILE*f=fopen("/proc/self/status","r"); if(!f)return -1; char l[256]; long kb=-1; while(fgets(l,sizeof(l),f)){ if(strncmp(l,"VmHWM:",6)==0){ sscanf(l+6,"%ld",&kb); break; } } fclose(f); return kb; }

int main(){
    setvbuf(stdout,NULL,_IONBF,0);
    struct Case{ const char* n; void(*fn)(); };
    Case cases[]={ {"B0-btree",test_b0_btree}, {"D1-diff",test_d1_differential}, {"C1-crash",test_c1_crash}, {"K1-corrupt",test_k1_corruption}, {"M1-pathverify",test_m1_path_verify}, {"P6-partialappend",test_p6_partial_append}, {"S1-scale",test_s1_scaling}, {"R2-freshopen",test_r2_fresh_open}, {"R2-atomic",test_r2_multi_key_atomic} };
    printf("== Stage 3A incremental chainstate test suite ==\n");
    const char* only=getenv("STORE3A_ONLY");
    for (auto& c: cases){
        if (only && *only && strstr(c.n, only)==NULL) continue;
        int p0=g_pass,f0=g_fail; c.fn();
        printf("%-10s %s  (checks +%d, failures %d)\n", c.n, (g_fail==f0?"PASS":"FAIL"), g_pass-p0, g_fail-f0);
    }
    printf("\n---- summary ----\n"); for(auto&m:g_failmsg) printf("%s\n",m.c_str());
    printf("PASS=%d FAIL=%d\nPEAK_RSS_KB=%ld\n", g_pass, g_fail, readVmHWM());
    return g_fail==0?0:1;
}
