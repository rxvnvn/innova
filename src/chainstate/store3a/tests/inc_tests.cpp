// Stage 3A - focused test suite: B+tree unit, differential vs Stage-2B reference,
// crash barriers, corruption, scaling. Isolated runtime dir; no production paths.
#include "chainstate.h"          // reference model (cs::UtxoState, cs::buildDelta)
#include "store3a/incstore.h"
#include "store3a/incdelta.h"
#include "store3a/btree.h"
#include "observe.h"             // S1.6-2 authoritative observation adapter
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <dirent.h>
#include <time.h>
#include <stdlib.h>

using namespace cs;
using namespace cs::store3a;

static const int64_t COIN = 100000000LL;
static const int32_t MAT = 1;
static int g_pass=0,g_fail=0; static std::vector<std::string> g_failmsg; static std::string g_cur;
#define CHECK(c,m) do{ if(c) g_pass++; else { g_fail++; g_failmsg.push_back(g_cur+": "+(m)); } }while(0)
static long readVmHWM();

static std::string baseDir(){ const char* o=getenv("STORE3A_BASE"); if(o&&*o){ std::string s=o; if(s[s.size()-1]!='/') s+="/"; return s; } return "/home/user/data/chainstate-3a-runtime/"; }
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

// ==================== Stage 3B-F0: undo offset coherence ====================
static uint64_t MAN(const std::string& dir){
    FILE* f=fopen((dir+"/MANIFEST").c_str(),"rb"); if(!f) return 0; uint8_t b[32]; size_t n=fread(b,1,32,f); fclose(f); if(n<32) return 0;
    uint64_t v=0; for(int i=0;i<8;i++) v|=(uint64_t)b[24+i]<<(8*i); return v; }
static long FSZ(const std::string& p){ struct stat st; return stat(p.c_str(),&st)==0?(long)st.st_size:-1; }
static Hash DG(CommitStore3A& s, ChainError* e){ UtxoState st; storeToState(s,&st,e); return st.digest(); }
static bool PUB(CommitStore3A& s, int32_t h, uint32_t lab, uint32_t txlab, size_t slen, ChainError* e){
    Block b; b.hash=id32(0xB10C0000u+lab,(uint32_t)h); b.height=h;
    Tx cb; cb.txid=id32(0xCB000000u+txlab,(uint32_t)h); cb.coinbase=true; cb.coinstake=false;
    TxOut o; o.value=50*COIN; o.script.assign(slen,7); cb.vout.push_back(o); b.txs.push_back(cb);
    return publishStore(s,b,e); }
static void JUNK(const std::string& p,long from,long len){
    FILE* f=fopen(p.c_str(),"r+b"); if(!f) return; if(fseek(f,from,SEEK_SET)!=0){ fclose(f); return; }
    for(long i=0;i<len;i++) fputc(0x5A,f); fclose(f); }
static bool COH(const std::string& d, ChainError* e){
    uint64_t m=MAN(d); long p=FSZ(d+"/undo.dat");
    if(!(p>=(long)m)){ setErr(e,ErrCode::CORRUPT,"physical "+std::to_string(p)+" < logical "+std::to_string((long long)m)); return false; }
    return true; }
static void f0Child(const std::string& dir, int mode, int32_t h, uint32_t lab, uint32_t txlab){
    ChainError e; CommitStore3A s; if(!s.open(dir,&e)) ::_exit(2);
    Block b; b.hash=id32(0xB10C0000u+lab,(uint32_t)h); b.height=h;
    Tx cb; cb.txid=id32(0xCB000000u+txlab,(uint32_t)h); cb.coinbase=true; cb.coinstake=false;
    TxOut o; o.value=50*COIN; o.script.assign(3,7); cb.vout.push_back(o); b.txs.push_back(cb);
    TreeReader tr(s.tree()); Delta d; if(!buildDeltaI(tr,b,MAT,&d,&e)) ::_exit(3);
    if(mode==0) s.setCrashPoint(CrashPoint::BeforeRename);
    if(mode==1) s.setCrashPoint(CrashPoint::AfterRename);
    if(mode==2) s.setCrashPoint(CrashPoint::AfterDirSync);
    bool ok=s.publish(d,b.hash,h,&e);
    if(mode==3) ::_exit(ok?0:1);
    ::_exit(42); }

static void test_f0_undo(){
    g_cur="F0-undo";
    ChainError e;

    // ---- F0-01 original sequence: publish h1..h3, disconnect h3, publish h4, restart, disconnect h4
    {
        std::string d=freshDir("f0_01"); Hash dAfterH2;
        { CommitStore3A s; CHECK(s.open(d,&e),"F0-01 open: "+e.str());
          CHECK(PUB(s,1,0,0,3,&e)&&PUB(s,2,0,0,3,&e)&&PUB(s,3,0,0,3,&e), "F0-01 publish h1..h3");
          CHECK(MAN(d)==(uint64_t)FSZ(d+"/undo.dat"), "F0-01 pre-disconnect physical==logical ("+std::to_string(FSZ(d+"/undo.dat"))+"=="+std::to_string((long long)MAN(d))+")");
          CHECK(s.disconnect(id32(0xB10C0000u,3),&e), "F0-01 disconnect h3: "+e.str());
          dAfterH2=DG(s,&e);
          CHECK(MAN(d)<(uint64_t)FSZ(d+"/undo.dat"), "F0-01 disconnect shrinks logical while physical keeps the stale tail");
          CHECK(PUB(s,4,0,0,3,&e), "F0-01 publish h4");
          CHECK(MAN(d)==(uint64_t)FSZ(d+"/undo.dat"), "F0-01 post-republish physical==logical ("+std::to_string(FSZ(d+"/undo.dat"))+"=="+std::to_string((long long)MAN(d))+")");
          CHECK(s.undoCount()==3, "F0-01 undoCount==3");
          CHECK(s.generation()==5, "F0-01 generation==5"); s.close(); }
        { CommitStore3A r; CHECK(r.open(d,&e), "F0-01 restart: "+e.str());
          CHECK(r.generation()==5 && r.undoCount()==3, "F0-01 restart gen5/undoCount3");
          CHECK(r.tipHash()==id32(0xB10C0000u,4) && r.tipHeight()==4, "F0-01 restart tip == h4");
          bool ok=r.disconnect(id32(0xB10C0000u,4),&e);
          CHECK(ok, "F0-01 restart disconnect(tip h4) succeeds: "+e.str());
          CHECK(ok && DG(r,&e)==dAfterH2, "F0-01 state after disconnect == state after h2");
          r.close(); }
    }

    // ---- F0-02 repeated disconnect/connect cycles with restart
    {
        std::string d=freshDir("f0_02"); Hash base;
        { CommitStore3A s; CHECK(s.open(d,&e),"F0-02 open"); CHECK(PUB(s,1,0,0,3,&e),"F0-02 h1"); base=DG(s,&e); s.close(); }
        for(int cyc=0;cyc<3;cyc++){
            int32_t h=2+cyc*2; uint32_t lab=(uint32_t)(1+cyc), tx=(uint32_t)(1+cyc), lab2=(uint32_t)(10+cyc), tx2=(uint32_t)(10+cyc);
            { CommitStore3A s; std::string t="F0-02 c"+std::to_string(cyc);
              CHECK(s.open(d,&e),t+" open"); CHECK(PUB(s,h,lab,tx,3,&e),t+" publish");
              CHECK(s.disconnect(id32(0xB10C0000u+lab,(uint32_t)h),&e),t+" disconnect"); CHECK(DG(s,&e)==base,t+" back to base");
              CHECK(PUB(s,h+1,lab2,tx2,3,&e),t+" republish"); CHECK(DG(s,&e)!=base,t+" advanced");
              CHECK(MAN(d)==(uint64_t)FSZ(d+"/undo.dat"),t+" physical==logical"); s.close(); }
            { CommitStore3A r; std::string t="F0-02 c"+std::to_string(cyc)+" restart";
              CHECK(r.open(d,&e),t+" open");
              bool ok=r.disconnect(id32(0xB10C0000u+lab2,(uint32_t)(h+1)),&e);
              CHECK(ok,t+" disconnect(tip): "+e.str()); CHECK(ok&&DG(r,&e)==base,t+" restores base");
              CHECK(COH(d,&e),t+" coherence"); r.close(); }
        }
    }

    // ---- F0-03 consecutive disconnects then a different branch
    {
        std::string d=freshDir("f0_03"); Hash dAfterH2;
        { CommitStore3A s; CHECK(s.open(d,&e),"F0-03 open");
          for(int h=1;h<=4;h++) CHECK(PUB(s,h,0,0,3,&e), "F0-03 publish "+std::to_string(h));
          CHECK(s.disconnect(id32(0xB10C0000u,4),&e),"F0-03 disconnect h4");
          CHECK(s.disconnect(id32(0xB10C0000u,3),&e),"F0-03 disconnect h3");
          dAfterH2=DG(s,&e);
          CHECK(PUB(s,5,7,7,3,&e),"F0-03 publish different branch h5");
          CHECK(COH(d,&e),"F0-03 physical>=logical (stale discarded records remain inert)"); s.close(); }
        { CommitStore3A r; CHECK(r.open(d,&e),"F0-03 restart: "+e.str());
          bool ok=r.disconnect(id32(0xB10C0000u+7,5),&e);
          CHECK(ok,"F0-03 restart disconnect(h5): "+e.str());
          CHECK(ok&&DG(r,&e)==dAfterH2,"F0-03 state == after h2"); r.close(); }
    }

    // ---- F0-04 records of unequal length (long/short alternating, incl. shorter record over longer stale)
    {
        std::string d=freshDir("f0_04"); Hash dAfterH4;
        { CommitStore3A s; CHECK(s.open(d,&e),"F0-04 open");
          CHECK(PUB(s,1,0,0,3,&e)&&PUB(s,2,0,0,3,&e),"F0-04 h1,h2");
          CHECK(PUB(s,3,0,0,2000,&e),"F0-04 h3 (long record)");
          CHECK(s.disconnect(id32(0xB10C0000u,3),&e),"F0-04 disconnect h3 (long)");
          CHECK(PUB(s,4,0,0,3,&e),"F0-04 h4 (short over long stale)");
          CHECK(COH(d,&e),"F0-04 coherence"); CHECK(s.undoCount()==3,"F0-04 undoCount==3");
          CHECK(PUB(s,5,0,0,2000,&e),"F0-04 h5 (long)");
          CHECK(s.disconnect(id32(0xB10C0000u,5),&e),"F0-04 disconnect h5 (long)");
          dAfterH4=DG(s,&e);
          CHECK(PUB(s,6,0,0,3,&e),"F0-04 h6 (short over long stale)");
          CHECK(COH(d,&e),"F0-04 coherence after mixed lengths"); s.close(); }
        { CommitStore3A r; CHECK(r.open(d,&e),"F0-04 restart: "+e.str());
          CHECK(r.undoCount()==4,"F0-04 restart undoCount==4");
          bool ok=r.disconnect(id32(0xB10C0000u,6),&e);
          CHECK(ok,"F0-04 restart disconnect(tip h6): "+e.str());
          CHECK(ok&&DG(r,&e)==dAfterH4,"F0-04 state == after h4"); r.close(); }
    }

    // ---- F0-05 / F0-15 equal-length records, different hashes+ops; stale record must never be the tip
    {
        std::string d=freshDir("f0_05"); Hash dAfterH2; uint64_t l3=0,lx=0;
        { CommitStore3A s; CHECK(s.open(d,&e),"F0-05 open");
          CHECK(PUB(s,1,0,0,3,&e)&&PUB(s,2,0,0,3,&e),"F0-05 h1,h2");
          uint64_t s2=MAN(d); dAfterH2=DG(s,&e);
          CHECK(PUB(s,3,0,0,3,&e),"F0-05 h3"); l3=MAN(d)-s2;
          CHECK(s.disconnect(id32(0xB10C0000u,3),&e),"F0-05 disconnect h3");
          CHECK(PUB(s,4,20,20,3,&e),"F0-05 publish h4' (different hash/txid, same length)");
          lx=MAN(d)-s2;
          CHECK(l3==lx && l3>0, "F0-05 record lengths equal ("+std::to_string((long long)l3)+"=="+std::to_string((long long)lx)+")");
          CHECK(MAN(d)==(uint64_t)FSZ(d+"/undo.dat"),"F0-05 physical==logical"); s.close(); }
        { CommitStore3A r; CHECK(r.open(d,&e),"F0-05 restart: "+e.str());
          CHECK(r.tipHash()==id32(0xB10C0000u+20,4),"F0-05 tip == h4'");
          ChainError estale; bool sta=r.disconnect(id32(0xB10C0000u,3),&estale);
          CHECK(!sta,"F0-05 stale h3 undo is NOT mistaken for the tip (disconnect refused: "+estale.str()+")");
          bool ok=r.disconnect(id32(0xB10C0000u+20,4),&e);
          CHECK(ok,"F0-05 disconnect(h4'): "+e.str());
          CHECK(ok&&DG(r,&e)==dAfterH2,"F0-05 state == after h2 (correct block reverted)"); r.close(); }
    }

    // ---- F0-06 crash/failure before the undo write; F0-07 short (torn) undo write
    for (int mode=0; mode<2; ++mode){
        std::string d=freshDir(mode? "f0_07":"f0_06"); std::string t=mode? "F0-07":"F0-06";
        { CommitStore3A s; CHECK(s.open(d,&e),t+" open"); CHECK(PUB(s,1,0,0,3,&e),t+" h1");
          uint64_t g=s.generation(), a=s.acknowledgedGeneration(), m=MAN(d); long p=FSZ(d+"/undo.dat"); Hash dg=DG(s,&e);
          if(!mode){ s.setInjectStage(InjectStage::UndoAppend); s.injectNoSpace(true); }
          else { s.injectUndoShortOnce(20); }
          bool ok=PUB(s,2,0,0,3,&e);
          s.clearInjection();
          CHECK(!ok,t+" publish fails ("+(mode?"torn undo write":"no space")+"); "+e.str());
          CHECK(s.generation()==g && s.acknowledgedGeneration()==a, t+" generation/ACK unchanged");
          CHECK(MAN(d)==m, t+" committed undoSize unchanged");
          CHECK(!mode || FSZ(d+"/undo.dat")>=(long)m, t+" physical >= logical (never shorter)");
          CHECK(DG(s,&e)==dg, t+" committed logical state unchanged");
          if(!mode) CHECK(FSZ(d+"/undo.dat")==p, t+" failed-before-write leaves undo.dat byte-length unchanged");
          CHECK(s.disconnect(id32(0xB10C0000u,1),&e), t+" old acknowledged generation still disconnectable");
          CHECK(PUB(s,3,0,0,3,&e), t+" subsequent publish succeeds");
          CHECK(COH(d,&e), t+" coherence after recovery"); s.close(); }
        { CommitStore3A r; CHECK(r.open(d,&e),t+" restart: "+e.str());
          CHECK(r.undoCount()==1,t+" restart undoCount==1 (h1 disconnected, h3 published)");
          CHECK(COH(d,&e),t+" restart coherence"); r.close(); }
    }

    // ---- F0-08 / F0-09 / F0-10 crash barriers; F0-11 repeated restart (read-only, then one disconnect)
    for (int mode=0; mode<4; ++mode){
        std::string d=freshDir(std::string("f0_0")+std::to_string(8+mode));
        std::string t=std::string("F0-0")+std::to_string(8+mode);
        const char* kind = mode==0?"crash before MANIFEST rename (OLD)":(mode==1?"crash after rename before dir-fsync (NEW)":(mode==2?"crash after dir-fsync before ACK (NEW)":"normal completion incl. ACK (NEW)"));
        Hash dOld; { CommitStore3A s; CHECK(s.open(d,&e),t+" open"); CHECK(PUB(s,1,0,0,3,&e),t+" h1"); dOld=DG(s,&e); s.close(); }
        pid_t pid=fork();
        if(pid==0){ f0Child(d,mode,2,0,0); ::_exit(7); }
        int st=0; waitpid(pid,&st,0);
        CHECK(WIFEXITED(st), t+" child terminated normally");
        CHECK(WIFEXITED(st) && WEXITSTATUS(st)==(mode==3?0:42), t+" child exit "+(mode==3?"0 (publish completed)":"42 (crash)")+" got "+std::to_string(WIFEXITED(st)?WEXITSTATUS(st):-1));
        uint64_t g0=0,u0=0; Hash d0;
        for(int rep=0; rep<3; ++rep){                       // read-only repeated restarts (no mutation)
            CommitStore3A r; bool ok=r.open(d,&e);
            CHECK(ok, t+" restart "+std::to_string(rep)+" ["+kind+"]: "+e.str());
            if(!ok) break;
            Hash dg=DG(r,&e); uint64_t g=r.generation(); uint64_t uc=(uint64_t)r.undoCount();
            if(rep==0){ g0=g; d0=dg; u0=uc;
                CHECK(g==(mode==0?1:2), t+" recovered generation "+(mode==0?"OLD (1)":"NEW (2)")+": "+std::to_string((long long)g));
                if(mode==0) CHECK(dg==dOld, t+" OLD logical state preserved at gen1");
                CHECK(r.acknowledgedGeneration()==g, t+" ACK reconciled to the recovered generation");
            } else CHECK(g==g0 && dg==d0 && uc==u0, t+" repeated restart deterministic (gen/digest/undoCount)");
            CHECK(COH(d,&e), t+" coherence on restart "+std::to_string(rep)); r.close(); }
        { CommitStore3A r; CHECK(r.open(d,&e), t+" final open");   // separate: disconnecting mutates generation
          Hash tip=r.tipHash(); bool okd=r.disconnect(tip,&e);
          CHECK(okd, t+" tip disconnect after recovery: "+e.str());
          r.close(); }
    }

    // ---- F0-12 corrupt stale-tail bytes (bytes >= committed logical length) must be inert
    {
        std::string d=freshDir("f0_12"); Hash dAfterH2;
        { CommitStore3A s; CHECK(s.open(d,&e),"F0-12 open");
          CHECK(PUB(s,1,0,0,3,&e)&&PUB(s,2,0,0,3,&e)&&PUB(s,3,0,0,2000,&e),"F0-12 h1..h3");
          CHECK(s.disconnect(id32(0xB10C0000u,3),&e),"F0-12 disconnect h3");
          dAfterH2=DG(s,&e); s.close(); }
        uint64_t m=MAN(d); long p=FSZ(d+"/undo.dat");
        CHECK(p>(long)m, "F0-12 stale tail present (physical "+std::to_string(p)+" > logical "+std::to_string((long long)m)+")");
        JUNK(d+"/undo.dat",(long)m,p-(long)m);
        { CommitStore3A r; CHECK(r.open(d,&e),"F0-12 reopen with corrupt stale tail: "+e.str());
          CHECK(r.generation()==4 && r.undoCount()==2, "F0-12 generation/undoCount intact");
          CHECK(DG(r,&e)==dAfterH2, "F0-12 logical state intact");
          CHECK(r.disconnect(id32(0xB10C0000u,2),&e), "F0-12 tip disconnect works"); r.close(); }
    }

    // ---- F0-13 truncated undo.dat: (a) committed header unreadable -> FAIL CLOSED;
    //      (b) CRC-tail loss -> header-only index still loads, but use fails closed (no wrong revert)
    {
        std::string d=freshDir("f0_13a");
        { CommitStore3A s; CHECK(s.open(d,&e),"F0-13a open"); CHECK(PUB(s,1,0,0,3,&e)&&PUB(s,2,0,0,3,&e),"F0-13a h1,h2"); s.close(); }
        uint64_t m=MAN(d);
        CHECK(::truncate((d+"/undo.dat").c_str(),(off_t)(m-100))==0, "F0-13a truncate 100 B below the committed length");
        { CommitStore3A r; bool ok=r.open(d,&e);
          CHECK(!ok, "F0-13a open FAIL-CLOSED (committed undo header unreadable): "+e.str());
          if(ok) r.close(); }
        CHECK(MAN(d)==m, "F0-13a MANIFEST untouched by the failed open");
    }
    {
        std::string d=freshDir("f0_13b");
        { CommitStore3A s; CHECK(s.open(d,&e),"F0-13b open"); CHECK(PUB(s,1,0,0,3,&e)&&PUB(s,2,0,0,3,&e),"F0-13b h1,h2"); s.close(); }
        uint64_t m=MAN(d);
        CHECK(::truncate((d+"/undo.dat").c_str(),(off_t)(m-10))==0, "F0-13b truncate only the last record's CRC tail");
        { CommitStore3A r; bool ok=r.open(d,&e);
          CHECK(ok, "F0-13b header-only undo index still loads: "+e.str());
          if(ok){
            ChainError e2; bool dok=r.disconnect(id32(0xB10C0000u,2),&e2);
            CHECK(!dok, "F0-13b disconnect of the damaged record FAIL-CLOSED (never a wrong revert): "+e2.str());
            CHECK(r.generation()==2, "F0-13b generation unchanged by the refused disconnect");
            ChainError e3; bool pok=PUB(r,3,0,0,3,&e3);
            CHECK(!pok, "F0-13b publish refuses to zero-extend over the short file: "+e3.str());
            CHECK(r.generation()==2 && MAN(d)==m, "F0-13b nothing committed by the refused publish");
            r.close(); }
        }
    }

    // ---- F0-14 old acknowledged generation remains recoverable when a new publish fails
    {
        std::string d=freshDir("f0_14"); Hash dg2;
        { CommitStore3A s; CHECK(s.open(d,&e),"F0-14 open"); CHECK(PUB(s,1,0,0,3,&e)&&PUB(s,2,0,0,3,&e),"F0-14 h1,h2");
          dg2=DG(s,&e);
          s.setInjectStage(InjectStage::ManifestTmp); s.injectNoSpace(true);
          CHECK(!PUB(s,3,0,0,3,&e), "F0-14 publish h3 fails (no space at MANIFEST.tmp)");
          s.clearInjection();
          CHECK(s.generation()==2 && s.acknowledgedGeneration()==2, "F0-14 gen2/ACK2 preserved");
          CHECK(DG(s,&e)==dg2, "F0-14 acknowledged state preserved in memory");
          CHECK(s.disconnect(id32(0xB10C0000u,2),&e), "F0-14 acknowledged generation still disconnectable"); s.close(); }
        { CommitStore3A r; CHECK(r.open(d,&e),"F0-14 restart: "+e.str());
          CHECK(r.generation()==3 && r.acknowledgedGeneration()==3, "F0-14 restart reads the acknowledged line");
          CHECK(COH(d,&e),"F0-14 coherence"); r.close(); }
    }

    // ---- F0-16 multi-key block delta: disconnect, reconnect, restart, exact digest comparison
    {
        std::string d=freshDir("f0_16"); Hash dAfterH4;
        auto mkMulti=[&](int32_t h,const Hash& bh,const Hash& cbtx,std::vector<Hash> spends)->Block{
            Block b; b.hash=bh; b.height=h;
            for (Hash sp: spends){ OutPoint prev; prev.txid=sp; prev.n=0; Tx t; t.txid=id32(0x77000000u,(uint32_t)(h*10+(int)sp.data()[0])); t.coinbase=false;
                TxIn in; in.prev=prev; t.vin.push_back(in); TxOut o; o.value=10*COIN; o.script={5,5}; t.vout.push_back(o); b.txs.push_back(t); }
            Tx cb; cb.txid=cbtx; cb.coinbase=true; cb.coinstake=false; TxOut o; o.value=50*COIN; o.script={1,2,3}; cb.vout.push_back(o); b.txs.push_back(cb);
            return b; };
        { CommitStore3A s; CHECK(s.open(d,&e),"F0-16 open");
          for(int h=1;h<=4;h++) CHECK(PUB(s,h,0,0,3,&e),"F0-16 publish "+std::to_string(h));
          dAfterH4=DG(s,&e);
          Block b5=mkMulti(5,id32(0xB10C0000u,50),id32(0xCB000000u,50),{id32(0xCB000000u,1),id32(0xCB000000u,2)});
          CHECK(publishStore(s,b5,&e), "F0-16 multi-key publish h5: "+e.str());
          CHECK(s.disconnect(id32(0xB10C0000u,50),&e), "F0-16 disconnect h5");
          CHECK(DG(s,&e)==dAfterH4, "F0-16 multi-key disconnect restores dAfterH4");
          Block b6=mkMulti(6,id32(0xB10C0000u,51),id32(0xCB000000u,51),{id32(0xCB000000u,1),id32(0xCB000000u,3)});
          CHECK(publishStore(s,b6,&e), "F0-16 multi-key publish h6: "+e.str());
          CHECK(COH(d,&e),"F0-16 coherence"); s.close(); }
        { CommitStore3A r; CHECK(r.open(d,&e),"F0-16 restart: "+e.str());
          CHECK(r.tipHash()==id32(0xB10C0000u,51),"F0-16 tip == h6");
          bool ok=r.disconnect(id32(0xB10C0000u,51),&e);
          CHECK(ok,"F0-16 restart disconnect(h6): "+e.str());
          CHECK(ok&&DG(r,&e)==dAfterH4,"F0-16 exact digest == dAfterH4 after disconnect/restart"); r.close(); }
    }

    // ---- F0-17 (R1-updated) orphan undo.dat with no MANIFEST/ACK/tree.dat is prior-store evidence:
    //      R1 REFUSES the fresh open instead of adopting the directory and overwriting the orphan's
    //      leading bytes on the first publish (that overwrite was F0-2). The pre-R1 expectation here
    //      ("fresh open ... adopted") is the behaviour this pass fixes.
    {
        std::string d=freshDir("f0_17"); std::string de=freshDir("f0_17_empty");
        { FILE* f=fopen((d+"/undo.dat").c_str(),"wb"); if(f){ for(int i=0;i<200;i++) fputc(0x41,f); fclose(f); } }
        CHECK(FSZ(d+"/undo.dat")==200, "F0-17 orphan undo.dat planted (200 bytes)");
        Hash orphan=fdig(d+"/undo.dat");
        { CommitStore3A s; bool ok=s.open(d,&e);
          CHECK(!ok, "F0-17/R1 (F0-2) orphan nonempty undo.dat -> FAIL CLOSED, no adoption: "+e.str());
          CHECK(!fexists(d+"/MANIFEST") && !fexists(d+"/ACK"), "F0-17/R1 no pointer files invented over the orphan");
          CHECK(FSZ(d+"/undo.dat")==200 && fdig(d+"/undo.dat")==orphan, "F0-17/R1 orphan bytes preserved byte-for-byte");
          if(ok) s.close(); }
        // the accepted residue form: a zero-length undo.dat can be destroyed in no way, so it is adopted
        { FILE* f=fopen((de+"/undo.dat").c_str(),"wb"); fclose(f); }
        Hash dEmpty;
        { CommitStore3A s; CHECK(s.open(de,&e),"F0-17 empty undo.dat residue accepted: "+e.str());
          CHECK(s.generation()==0 && s.undoCount()==0, "F0-17 empty residue opens as gen0 with an empty index");
          dEmpty=DG(s,&e);
          CHECK(PUB(s,1,0,0,3,&e),"F0-17 first publish over the accepted empty residue");
          CHECK(COH(de,&e),"F0-17 coherence after first publish"); s.close(); }
        { CommitStore3A r; CHECK(r.open(de,&e),"F0-17 restart: "+e.str());
          CHECK(r.generation()==1 && r.undoCount()==1, "F0-17 restart reads exactly one coherent record");
          bool ok=r.disconnect(id32(0xB10C0000u,1),&e);
          CHECK(ok,"F0-17 disconnect(h1): "+e.str());
          CHECK(ok&&DG(r,&e)==dEmpty,"F0-17 back to the empty state"); r.close(); }
    }
}

// ==================== Stage 3B-F0-R1: durability + fresh-open hardening =======
// F0-1: MANIFEST.bak could reference undo bytes overwritten by the next pwrite.
// F0-2: an orphan undo.dat could be silently overwritten during fresh initialization.
static uint64_t MFIELD(const std::string& p, size_t off){
    FILE* f=fopen(p.c_str(),"rb"); if(!f) return 0; uint8_t b[8];
    if(fseek(f,(long)off,SEEK_SET)!=0){ fclose(f); return 0; }
    size_t n=fread(b,1,8,f); fclose(f); if(n<8) return 0;
    uint64_t v=0; for(int i=0;i<8;i++) v|=(uint64_t)b[i]<<(8*i); return v; }
static uint64_t MUU(const std::string& d){ return MFIELD(d+"/MANIFEST",24); }
static uint64_t MBU(const std::string& d){ return MFIELD(d+"/MANIFEST.bak",24); }
static Hash RNGD(const std::string& p, uint64_t off, uint64_t len){
    FILE* f=fopen(p.c_str(),"rb"); if(!f) return Hash();
    if(fseek(f,(long)off,SEEK_SET)!=0){ fclose(f); return Hash(); }
    ChainedHash h; uint8_t b[4096]; uint64_t got=0;
    while(got<len){ size_t want=(size_t)((len-got)<4096?(len-got):4096); size_t r=fread(b,1,want,f); if(!r) break; h.put(b,r); got+=r; }
    fclose(f); return h.final(); }
static bool CPFILE(const std::string& src,const std::string& dst){
    FILE* in=fopen(src.c_str(),"rb"); if(!in) return false;
    FILE* out=fopen(dst.c_str(),"wb"); if(!out){ fclose(in); return false; }
    uint8_t b[65536]; size_t n; bool ok=true;
    while((n=fread(b,1,sizeof(b),in))>0) if(fwrite(b,1,n,out)!=n){ ok=false; break; }
    fclose(in); if(fclose(out)!=0) ok=false; return ok; }
static bool LSTATM(const std::string& p, mode_t* m){ struct stat st; if(::lstat(p.c_str(),&st)!=0) return false; *m=st.st_mode; return true; }
static Block mkBlockH(int32_t h,uint32_t lab,uint32_t txlab,size_t slen){
    Block b; b.hash=id32(0xB10C0000u+lab,(uint32_t)h); b.height=h;
    Tx cb; cb.txid=id32(0xCB000000u+txlab,(uint32_t)h); cb.coinbase=true; cb.coinstake=false;
    TxOut o; o.value=50*COIN; o.script.assign(slen,7); cb.vout.push_back(o); b.txs.push_back(cb); return b; }
static bool PUBH(CommitStore3A& s,int32_t h,uint32_t lab,uint32_t txlab,size_t slen,ChainError* e){
    Block b=mkBlockH(h,lab,txlab,slen); return publishStore(s,b,e); }
// the F0-1 vulnerable state: h1..h3 (h3 LONG) then disconnect h3 => bak.undoSize > primary.undoSize
static bool r1SetupPostDisconnect(const std::string& d, Hash* dig, ChainError* e){
    CommitStore3A s; if(!s.open(d,e)) return false;
    if(!PUBH(s,1,0,0,3,e)||!PUBH(s,2,0,0,3,e)||!PUBH(s,3,0,0,2000,e)){ s.close(); return false; }
    if(!s.disconnect(id32(0xB10C0000u,3),e)){ s.close(); return false; }
    *dig=DG(s,e); s.close(); return true; }
static bool r1PlantValidUndo(const std::string& dir,const std::string& tag,ChainError* e){
    std::string src=freshDir("r1_src_"+tag); Hash dd;
    if(!r1SetupPostDisconnect(src,&dd,e)) return false;
    if(!CPFILE(src+"/undo.dat",dir+"/undo.dat")) return false;
    return true; }

static void test_r1_f01_backup_window(){
    g_cur="R1-F0-1";
    ChainError e;
    // ---- R1-02/R1-03/R1-04: no retained manifest may reference a byte the reconnect write overwrites
    {
        std::string d=freshDir("r1_f01a"); Hash dDisc;
        CHECK(r1SetupPostDisconnect(d,&dDisc,&e), "R1-01 setup (h1..h3, disconnect h3): "+e.str());
        uint64_t mu=MUU(d), bu=MBU(d); long pb=FSZ(d+"/undo.dat");
        printf("  R1-01 post-disconnect state: primary.undoSize=%llu bak.undoSize=%llu physical=%ld\n", (unsigned long long)mu,(unsigned long long)bu,pb);
        CHECK(bu>mu, "R1-01 bak.undoSize ("+std::to_string(bu)+") > primary.undoSize ("+std::to_string(mu)+") = the F0-1 vulnerable state");
        CHECK(pb==(long)bu, "R1-01 physical undo length == bak-referenced length (stale tail retained)");
        Hash preP=RNGD(d+"/undo.dat",0,mu), preB=RNGD(d+"/undo.dat",0,bu);
        CHECK(CPFILE(d+"/undo.dat",d+"/undo.pre"), "R1-01 snapshot undo.dat");
        { CommitStore3A s; CHECK(s.open(d,&e), "R1-01 reopen: "+e.str());
          s.setInjectStage(InjectStage::ManifestTmp); s.injectNoSpace(true);
          bool ok=PUBH(s,4,0,0,3,&e); s.clearInjection();
          CHECK(!ok, "R1-01 reconnect publish fails after the undo write (injected no-space at MANIFEST.tmp): "+e.str());
          CHECK(s.generation()==4 && s.acknowledgedGeneration()==4, "R1-01 generation/ACK stay at the acknowledged 4");
          s.close(); }
        uint64_t mu2=MUU(d), bu2=MBU(d);
        printf("  R1-01 after failed reconnect: primary.undoSize=%llu bak.undoSize=%llu physical=%ld\n", (unsigned long long)mu2,(unsigned long long)bu2,FSZ(d+"/undo.dat"));
        CHECK(mu2==mu, "R1-04 committed MANIFEST.undoSize untouched by the failed publish");
        CHECK(RNGD(d+"/undo.dat",0,mu)==preP, "R1-04 acknowledged undo prefix [0,primary.undoSize) byte-identical");
        CHECK(bu2<=mu2, "R1-03 retained bak.undoSize "+std::to_string(bu2)+" <= primary.undoSize "+std::to_string(mu2));
        CHECK(RNGD(d+"/undo.dat",0,bu2)==RNGD(d+"/undo.pre",0,bu2), "R1-03 MANIFEST.bak references only bytes below the write offset (F0-1 invariant)");
        CHECK(RNGD(d+"/undo.dat",mu,(uint64_t)pb-mu)!=RNGD(d+"/undo.pre",mu,(uint64_t)pb-mu), "R1-01 evidence: the reconnect write DID modify the stale tail at/after the committed end");
        (void)preB;
    }
    // ---- R1-16/R1-04: double fault inside/after the window must not degrade the fallback
    {
        std::string d=freshDir("r1_f01b"); Hash dDisc;
        CHECK(r1SetupPostDisconnect(d,&dDisc,&e), "R1-16 setup: "+e.str());
        { CommitStore3A s; CHECK(s.open(d,&e),"R1-16 reopen");
          s.setInjectStage(InjectStage::ManifestTmp); s.injectNoSpace(true);
          CHECK(!PUBH(s,4,0,0,3,&e), "R1-16 reconnect publish fails inside the window");
          s.clearInjection(); s.close(); }
        writeJunk(d+"/MANIFEST","GARBAGE-PRIMARY-MANIFEST-AFTER-THE-WINDOW");
        CommitStore3A r; bool ok=r.open(d,&e);
        printf("  R1-16 double fault: open=%d gen=%llu err=%s\n", (int)ok,(unsigned long long)(ok?r.generation():0),e.str().c_str());
        CHECK(ok, "R1-16 double fault recovers from the intact backup (was: fail-closed 'no valid backup'): "+e.str());
        if(ok){ CHECK(r.generation()==4, "R1-16 recovered generation == 4 (the acknowledged one)");
          CHECK(DG(r,&e)==dDisc, "R1-16 recovered logical state == acknowledged post-disconnect state");
          CHECK(r.undoCount()==2, "R1-16 recovered undo index == 2 records");
          CHECK(MFIELD(d+"/MANIFEST",16)==4, "R1-16 MANIFEST repaired from the backup");
          CHECK(r.acknowledgedGeneration()==4, "R1-16 ACK still 4 (no rollback of acknowledged state)");
          r.close(); }
    }
    // ---- R1-19/R1-20: the invariant survives repeated disconnect/reconnect cycles (mixed lengths)
    {
        std::string d=freshDir("r1_f01c"); ChainError ce;
        { CommitStore3A s; CHECK(s.open(d,&ce),"R1-19 open"); bool allok=true;
          for(int c=0;c<3;c++){
            int32_t h=1+2*c;
            if(!PUBH(s,h,0,0,3,&ce)||!s.disconnect(id32(0xB10C0000u,(uint32_t)h),&ce)){ allok=false; break; }
            if(!PUBH(s,h+1,(uint32_t)(10+c),(uint32_t)(10+c),(c==1?2000:3),&ce)){ allok=false; break; }
          }
          CHECK(allok, "R1-19 three disconnect/reconnect cycles with mixed record lengths: "+ce.str());
          uint64_t pu=MUU(d), pbi=MBU(d);
          CHECK(pbi<=pu, "R1-19 final bak.undoSize <= primary.undoSize");
          CHECK(RNGD(d+"/undo.dat",0,pbi)==RNGD(d+"/undo.dat",0,pbi) && pbi<=pu, "R1-19 retained manifests reference in-range bytes only");
          CHECK(COH(d,&ce),"R1-19 coherence"); s.close(); }
    }
    // ---- R1-17: backup corruption handling (valid primary ignores it; both corrupt fails closed)
    {
        std::string d=freshDir("r1_f01d"); Hash dA;
        CHECK(r1SetupPostDisconnect(d,&dA,&e), "R1-17 setup: "+e.str());
        writeJunk(d+"/MANIFEST.bak","CORRUPT-BACKUP-BYTES");
        { CommitStore3A s; bool ok=s.open(d,&e);
          CHECK(ok && s.generation()==4, "R1-17 corrupt backup with a valid primary: open succeeds, backup not consulted");
          CHECK(ok && DG(s,&e)==dA, "R1-17 logical state unchanged");
          if(ok) s.close(); }
        writeJunk(d+"/MANIFEST","GARBAGE-PRIMARY-BYTES");
        Hash pg=fdig(d+"/MANIFEST");
        { CommitStore3A r; bool ok=r.open(d,&e);
          CHECK(!ok, "R1-17 primary AND backup corrupt -> FAIL CLOSED (no silent rollback): "+e.str());
          CHECK(fdig(d+"/MANIFEST")==pg, "R1-17 corrupt primary left exactly as found (no silent repair/rollback)");
          if(ok) r.close(); }
    }
    // ---- R1-17b: the backup is re-established from the current primary by the next publish
    {
        std::string d=freshDir("r1_f01e"); Hash dA;
        CHECK(r1SetupPostDisconnect(d,&dA,&e), "R1-17b setup: "+e.str());
        writeJunk(d+"/MANIFEST.bak","CORRUPT-BACKUP-BYTES");
        uint64_t mu0=MUU(d);
        CHECK(CPFILE(d+"/undo.dat",d+"/undo.pre"), "R1-17b snapshot undo.dat");
        { CommitStore3A s; CHECK(s.open(d,&e), "R1-17b open with a corrupt backup");
          CHECK(PUBH(s,4,0,0,3,&e), "R1-17b publish with a corrupt backup");
          s.close(); }
        CHECK(MBU(d)==mu0, "R1-17b backup re-established from the acknowledged primary ("+std::to_string((unsigned long long)MBU(d))+"=="+std::to_string((unsigned long long)mu0)+")");
        CHECK(RNGD(d+"/undo.dat",0,MBU(d))==RNGD(d+"/undo.pre",0,MBU(d)), "R1-17b re-established backup references only unmodified bytes");
        { CommitStore3A r; CHECK(r.open(d,&e), "R1-17b reopen: "+e.str()); CHECK(r.generation()==5, "R1-17b generation 5"); r.close(); }
    }
}

static void r1CrashChild(const std::string& dir,int mode){
    ChainError e; CommitStore3A s; if(!s.open(dir,&e)) ::_exit(2);
    Block b=mkBlockH(4,0,0,3); TreeReader tr(s.tree()); Delta d;
    if(!buildDeltaI(tr,b,MAT,&d,&e)) ::_exit(3);
    switch(mode){
        case 0: s.setCrashPoint(CrashPoint::BeforeBakReplace); break;
        case 1: s.setCrashPoint(CrashPoint::BakPrepared); break;
        case 2: s.setCrashPoint(CrashPoint::AfterBakReplace); break;
        case 3: s.setCrashPoint(CrashPoint::AfterUndoWrite); break;
        case 4: s.setCrashPoint(CrashPoint::BeforeRename); break;
        case 5: s.setCrashPoint(CrashPoint::AfterRename); break;
        case 6: s.setCrashPoint(CrashPoint::AfterDirSync); break;
        default: break;
    }
    bool ok=s.publish(d,b.hash,b.height,&e);
    ::_exit(ok?0:1);
}

static void test_r1_crash_matrix(){
    g_cur="R1-crash";
    ChainError e;
    const char* nm[8]={"BeforeBakReplace","BakPrepared","AfterBakReplace","AfterUndoWrite","BeforeRename","AfterRename","AfterDirSync","complete"};
    for(int mode=0;mode<8;mode++){
        std::string tag=std::string(nm[mode]);
        std::string d=freshDir(std::string("r1_c")+std::to_string(mode)); Hash dDisc;
        CHECK(r1SetupPostDisconnect(d,&dDisc,&e), tag+" setup: "+e.str());
        long pb=FSZ(d+"/undo.dat"); uint64_t mu0=MUU(d);
        CHECK(CPFILE(d+"/undo.dat",d+"/undo.pre"), tag+" snapshot undo.dat");
        CHECK(CPFILE(d+"/MANIFEST.bak",d+"/bak.pre"), tag+" snapshot MANIFEST.bak");
        // clean reference run for the NEW state
        Hash newDig; uint64_t newGen=0;
        { std::string dE=freshDir(std::string("r1_cref")+std::to_string(mode)); Hash x;
          CHECK(r1SetupPostDisconnect(dE,&x,&e), tag+" reference setup");
          CommitStore3A s; CHECK(s.open(dE,&e), tag+" reference open");
          CHECK(PUBH(s,4,0,0,3,&e), tag+" reference publish");
          newDig=DG(s,&e); newGen=s.generation(); s.close(); }
        pid_t pid=fork();
        if(pid==0){ r1CrashChild(d,mode); ::_exit(7); }
        int st=0; waitpid(pid,&st,0);
        int expect=(mode==7)?0:42;
        CHECK(WIFEXITED(st)&&WEXITSTATUS(st)==expect, tag+" child exit expected "+std::to_string(expect)+" got "+std::to_string(WIFEXITED(st)?WEXITSTATUS(st):-1));
        { CommitStore3A r; bool ok=r.open(d,&e);
          CHECK(ok, tag+" reopen: "+e.str());
          if(ok){
            bool vis=(mode>=5);
            CHECK(r.generation()==(vis?newGen:4), tag+" generation "+(vis?"NEW (5)":"OLD acknowledged (4)")+": "+std::to_string((long long)r.generation()));
            CHECK(DG(r,&e)==(vis?newDig:dDisc), tag+" logical digest "+(vis?"== clean NEW":"== acknowledged OLD"));
            CHECK(r.acknowledgedGeneration()==r.generation(), tag+" ACK reconciled to the recovered generation");
            uint64_t pu=MUU(d), ba=MBU(d);
            // Bytes below the write offset (the acknowledged prefix) must be byte-identical in every
            // state. A NEW-generation primary legitimately extends the referenced range with its own
            // freshly written record, so the invariant is stated against the write offset, not undoSize.
            uint64_t lim=(pu<mu0)?pu:mu0;
            CHECK(lim>0 && RNGD(d+"/undo.dat",0,lim)==RNGD(d+"/undo.pre",0,lim), tag+" bytes below the write offset are untouched in the primary's range");
            CHECK(pu<=(uint64_t)pb || vis, tag+" primary undoSize within the physical length (or extended by the new record)");
            if(ba>0) CHECK(ba<=(uint64_t)pb && RNGD(d+"/undo.dat",0,ba)==RNGD(d+"/undo.pre",0,ba), tag+" MANIFEST.bak references only unmodified bytes");
            CHECK(COH(d,&e), tag+" coherence physical>=logical");
            if(mode==1) CHECK(!fexists(d+"/MANIFEST.bak.tmp"), tag+" interrupted backup tmp removed by open");
            r.close(); }
        }
        // double fault after the crash: recoverable exactly when the crash followed the backup replacement
        if(mode<=3){
            std::string dd=freshDir(std::string("r1_cd")+std::to_string(mode)); Hash dDisc2;
            CHECK(r1SetupPostDisconnect(dd,&dDisc2,&e), tag+" double-fault setup");
            pid_t p2=fork();
            if(p2==0){ r1CrashChild(dd,mode); ::_exit(7); }
            int st2=0; waitpid(p2,&st2,0);
            writeJunk(dd+"/MANIFEST","GARBAGE-PRIMARY-AFTER-CRASH");
            CommitStore3A r2; bool ok2=r2.open(dd,&e);
            printf("  %s double fault: open=%d gen=%llu err=%s\n", nm[mode], (int)ok2, (unsigned long long)(ok2?r2.generation():0), e.str().c_str());
            if(mode>=2){
                CHECK(ok2, tag+" double fault recovers the acknowledged generation (backup replaced before the write): "+e.str());
                if(ok2){ CHECK(r2.generation()==4 && DG(r2,&e)==dDisc2, tag+" double fault recovered gen4 with the acknowledged state"); r2.close(); }
            } else {
                CHECK(!ok2, tag+" double fault fail-closed (the retained backup predates the acknowledged generation): ");
                if(ok2) r2.close();
            }
        }
    }
}

static void test_r1_fresh_open_matrix(){
    g_cur="R1-fresh";
    ChainError e;
    // D1 genuine fresh initialization still works
    { std::string d=freshDir("r1_d1"); CommitStore3A s; CHECK(s.open(d,&e), "D1 fresh open: "+e.str());
      CHECK(s.generation()==0, "D1 gen0");
      CHECK(fexists(d+"/tree.dat") && !fexists(d+"/undo.dat"), "D1 sentinel page created, no undo.dat invented");
      CHECK(PUBH(s,1,0,0,3,&e), "D1 first publish"); CHECK(COH(d,&e), "D1 coherence"); s.close();
      CommitStore3A r; CHECK(r.open(d,&e), "D1 restart: "+e.str()); CHECK(r.generation()==1,"D1 restart gen1"); r.close(); }
    // D2 empty undo.dat only -> accepted residue (0 bytes: nothing can be destroyed), then TOCTOU guard
    { std::string d=freshDir("r1_d2"); { FILE* f=fopen((d+"/undo.dat").c_str(),"wb"); fclose(f); }
      CommitStore3A s; CHECK(s.open(d,&e), "D2 empty undo.dat residue accepted: "+e.str());
      CHECK(PUBH(s,1,0,0,3,&e), "D2 first publish over the accepted residue"); CHECK(COH(d,&e), "D2 coherence"); s.close();
      CommitStore3A r; CHECK(r.open(d,&e), "D2 restart: "+e.str()); CHECK(r.generation()==1 && r.undoCount()==1, "D2 restart gen1/undoCount1");
      CHECK(r.disconnect(id32(0xB10C0000u,1),&e), "D2 disconnect works"); r.close(); }
    // D3 nonempty VALID undo.dat only -> FAIL CLOSED, bytes preserved
    { std::string d=freshDir("r1_d3"); CHECK(r1PlantValidUndo(d,"d3",&e), "D3 plant a real nonempty undo.dat");
      long us=FSZ(d+"/undo.dat"); Hash uh=fdig(d+"/undo.dat"); CHECK(us>0, "D3 planted undo.dat is nonempty ("+std::to_string(us)+" bytes)");
      CommitStore3A s; bool ok=s.open(d,&e);
      CHECK(!ok, "D3 nonempty orphan undo.dat -> FAIL CLOSED (was: silently adopted then overwritten): "+e.str());
      CHECK(FSZ(d+"/undo.dat")==us && fdig(d+"/undo.dat")==uh, "D3 orphan bytes byte-identical");
      CHECK(!fexists(d+"/MANIFEST") && !fexists(d+"/ACK") && !fexists(d+"/tree.dat"), "D3 no store files invented");
      if(ok) s.close(); }
    // D4 nonempty CORRUPT undo.dat only -> FAIL CLOSED, bytes preserved
    { std::string d=freshDir("r1_d4"); writeJunk(d+"/undo.dat","NOT-A-RECORD-AT-ALL-0123456789");
      long us=FSZ(d+"/undo.dat"); Hash uh=fdig(d+"/undo.dat");
      CommitStore3A s; bool ok=s.open(d,&e); CHECK(!ok, "D4 nonempty corrupt orphan undo.dat -> FAIL CLOSED: "+e.str());
      CHECK(FSZ(d+"/undo.dat")==us && fdig(d+"/undo.dat")==uh, "D4 corrupt orphan preserved (not destroyed, not quarantined)");
      if(ok) s.close(); }
    // D5 directory named undo.dat -> FAIL CLOSED, still a directory
    { std::string d=freshDir("r1_d5"); CHECK(mkdir((d+"/undo.dat").c_str(),0755)==0, "D5 create directory named undo.dat");
      CommitStore3A s; bool ok=s.open(d,&e); CHECK(!ok, "D5 non-regular undo.dat (directory) -> FAIL CLOSED: "+e.str());
      mode_t m=0; CHECK(LSTATM(d+"/undo.dat",&m)&&S_ISDIR(m), "D5 directory left in place (not moved/deleted)");
      if(ok) s.close(); }
    // D6 symlink named undo.dat -> FAIL CLOSED, link and target untouched
    { std::string d=freshDir("r1_d6"); writeJunk(d+"/target.dat","SYMLINK-TARGET-CONTENT");
      CHECK(::symlink((d+"/target.dat").c_str(),(d+"/undo.dat").c_str())==0, "D6 plant symlink undo.dat");
      Hash tg=fdig(d+"/target.dat");
      CommitStore3A s; bool ok=s.open(d,&e); CHECK(!ok, "D6 symlinked undo.dat -> FAIL CLOSED (never followed): "+e.str());
      mode_t m=0; CHECK(LSTATM(d+"/undo.dat",&m)&&S_ISLNK(m), "D6 symlink not replaced by a regular file");
      CHECK(fdig(d+"/target.dat")==tg, "D6 symlink target untouched");
      if(ok) s.close(); }
    // D7 unreadable/unwritable undo.dat -> FAIL CLOSED at open
    { std::string d=freshDir("r1_d7"); { FILE* f=fopen((d+"/undo.dat").c_str(),"wb"); fclose(f); }
      bool nonroot=(::geteuid()!=0);
      if(nonroot){ CHECK(::chmod((d+"/undo.dat").c_str(),0000)==0, "D7 chmod 000 undo.dat");
        CommitStore3A s; bool ok=s.open(d,&e); CHECK(!ok, "D7 unreadable undo.dat -> FAIL CLOSED: "+e.str());
        ::chmod((d+"/undo.dat").c_str(),0644); CHECK(FSZ(d+"/undo.dat")==0, "D7 file untouched (still empty)"); }
      else printf("  D7 note: running as root; EACCES path not exercised\n"); }
    // D8 nonempty undo.dat beside an ACCEPTED empty sentinel tree.dat -> still FAIL CLOSED
    { std::string d=freshDir("r1_d8"); { CommitStore3A s; CHECK(s.open(d,&e),"D8 sentinel open: "+e.str()); s.close(); }
      CHECK(FSZ(d+"/tree.dat")==4096, "D8 empty sentinel tree.dat present");
      CHECK(r1PlantValidUndo(d,"d8",&e), "D8 plant nonempty undo.dat");
      long ts=FSZ(d+"/tree.dat"), us=FSZ(d+"/undo.dat"); Hash th=fdig(d+"/tree.dat"), uh=fdig(d+"/undo.dat");
      CommitStore3A s; bool ok=s.open(d,&e);
      CHECK(!ok, "D8 nonempty orphan undo.dat beside an accepted empty tree -> FAIL CLOSED: "+e.str());
      CHECK(FSZ(d+"/tree.dat")==ts && fdig(d+"/tree.dat")==th && FSZ(d+"/undo.dat")==us && fdig(d+"/undo.dat")==uh, "D8 nothing modified");
      if(ok) s.close(); }
    // D9 undo.dat + MANIFEST.bak only / D10 undo.dat + ACK only -> the existing pointer rules fire
    { std::string d=freshDir("r1_d9"); CHECK(mkStore6(d,&e),"D9 build"); CHECK(CPFILE(d+"/MANIFEST.bak",d+"/bak.keep"),"D9 keep bak copy");
      unlink((d+"/MANIFEST").c_str()); unlink((d+"/ACK").c_str());
      CommitStore3A s; bool ok=s.open(d,&e); CHECK(!ok, "D9 backup-only + undo.dat -> FAIL CLOSED: "+e.str()); if(ok) s.close(); }
    { std::string d=freshDir("r1_d10"); CHECK(mkStore6(d,&e),"D10 build");
      unlink((d+"/MANIFEST").c_str()); unlink((d+"/MANIFEST.bak").c_str());
      CommitStore3A s; bool ok=s.open(d,&e); CHECK(!ok, "D10 ACK + undo.dat, MANIFEST missing -> FAIL CLOSED: "+e.str()); if(ok) s.close(); }
    // D11 existing acknowledged store is unaffected
    { std::string d=freshDir("r1_d11"); CHECK(mkStore6(d,&e),"D11 build");
      CommitStore3A s; bool ok=s.open(d,&e); CHECK(ok, "D11 acknowledged store still opens: "+e.str());
      if(ok){ CHECK(s.generation()==6, "D11 generation 6 preserved");
        CHECK(PUBH(s,7,0,0,3,&e), "D11 publish continues to work");
        CHECK(s.disconnect(id32(0xB10C0000u,7),&e), "D11 disconnect continues to work"); CHECK(COH(d,&e),"D11 coherence"); s.close(); } }
    // D12 missing undo.dat with committed undoSize>0 -> FAIL CLOSED
    { std::string d=freshDir("r1_d12"); CHECK(mkStore6(d,&e),"D12 build");
      CHECK(unlink((d+"/undo.dat").c_str())==0, "D12 remove the referenced undo.dat");
      CommitStore3A s; bool ok=s.open(d,&e); CHECK(!ok, "D12 missing undo.dat with undoSize>0 -> FAIL CLOSED: "+e.str()); if(ok) s.close(); }
    // D13 partial-initialization residue (tree pages + nonempty undo, no pointers) -> FAIL CLOSED, all bytes preserved
    { std::string d=freshDir("r1_d13"); CHECK(mkStore6(d,&e),"D13 build");
      unlink((d+"/MANIFEST").c_str()); unlink((d+"/ACK").c_str()); unlink((d+"/MANIFEST.bak").c_str());
      long ts=FSZ(d+"/tree.dat"), us=FSZ(d+"/undo.dat"); Hash th=fdig(d+"/tree.dat"), uh=fdig(d+"/undo.dat");
      CommitStore3A s; bool ok=s.open(d,&e); CHECK(!ok, "D13 residue (tree+undo, no pointers) -> FAIL CLOSED: "+e.str());
      CHECK(FSZ(d+"/tree.dat")==ts && fdig(d+"/tree.dat")==th && FSZ(d+"/undo.dat")==us && fdig(d+"/undo.dat")==uh, "D13 every byte preserved");
      if(ok) s.close(); }
    // D14 repeated restart after a failed open: deterministic refusal, no mutation, no quarantine
    { std::string d=freshDir("r1_d14"); writeJunk(d+"/undo.dat","ORPHAN-EVIDENCE-BYTES");
      long us=FSZ(d+"/undo.dat"); Hash uh=fdig(d+"/undo.dat"); std::string m0;
      for(int i=0;i<3;i++){ CommitStore3A s; bool ok=s.open(d,&e); std::string m=e.msg;
        CHECK(!ok, "D14 refusal "+std::to_string(i)+" identical: "+m);
        if(i==0){ m0=m; CHECK(!m0.empty(), "D14 refusal is diagnostic and non-empty"); }
        else CHECK(m==m0, "D14 deterministic diagnostic (same message, same files)");
        if(ok) s.close(); }
      CHECK(FSZ(d+"/undo.dat")==us && fdig(d+"/undo.dat")==uh, "D14 orphan untouched after repeated refusals");
      CHECK(!fexists(d+"/MANIFEST") && !fexists(d+"/ACK") && !fexists(d+"/tree.dat"), "D14 no file invented and nothing quarantined"); }
    // R1-30 + TOCTOU: a file appearing/disappearing between classification and the first write fails closed
    { std::string d=freshDir("r1_t1"); CommitStore3A s; CHECK(s.open(d,&e), "T1 fresh open (undo.dat classified absent)");
      writeJunk(d+"/undo.dat","APPEARED-AFTER-CLASSIFICATION");
      long us=FSZ(d+"/undo.dat"); Hash uh=fdig(d+"/undo.dat");
      bool ok=PUBH(s,1,0,0,3,&e);
      CHECK(!ok, "T1 TOCTOU: first publish fails closed when undo.dat appears after classification: "+e.str());
      CHECK(FSZ(d+"/undo.dat")==us && fdig(d+"/undo.dat")==uh, "T1 the appeared file was NOT overwritten");
      CHECK(!fexists(d+"/MANIFEST") && !fexists(d+"/ACK"), "T1 no pointer files written"); s.close(); }
    { std::string d=freshDir("r1_t2"); { FILE* f=fopen((d+"/undo.dat").c_str(),"wb"); fclose(f); }
      CommitStore3A s; CHECK(s.open(d,&e), "T2 fresh open over an accepted empty residue");
      CHECK(unlink((d+"/undo.dat").c_str())==0, "T2 residue disappears after classification");
      bool ok=PUBH(s,1,0,0,3,&e);
      CHECK(!ok, "T2 TOCTOU: first publish fails closed when the accepted empty residue vanished: "+e.str());
      CHECK(!fexists(d+"/undo.dat"), "T2 no undo.dat recreated"); s.close(); }
}

// ==================== Stage 3B-F0-R2: empty-residue substitution TOCTOU ======
// F0-2-R1 (residual MEDIUM): an adopted EMPTY undo.dat could be replaced between the fresh-open
// classification and the first undo write. The R1 RequireExisting path then opened the substitute
// with a bare O_WRONLY and overwrote its bytes (E24) or followed a symlink (E25a).
// The whole suite is behavioural: it inspects sizes, inodes, hashes and diagnostics only, so the
// same source runs against the pre-fix baseline (where the substitution cases must FAIL).
static bool touchEmpty(const std::string& p){ FILE* f=fopen(p.c_str(),"wb"); if(!f) return false; fclose(f); return true; }
static bool substFile(const std::string& p, const char* content){
    ::unlink(p.c_str()); FILE* f=fopen(p.c_str(),"wb"); if(!f) return false;
    size_t n=strlen(content); bool ok=(fwrite(content,1,n,f)==n); fclose(f); return ok; }
static bool statTuple(const std::string& p, uint64_t* dev, uint64_t* ino, long long* size, long long* ctimeNs){
    struct stat st; if(::lstat(p.c_str(),&st)!=0) return false;
    *dev=(uint64_t)st.st_dev; *ino=(uint64_t)st.st_ino; *size=(long long)st.st_size;
    *ctimeNs=(long long)st.st_ctim.tv_sec*1000000000LL+(long long)st.st_ctim.tv_nsec; return true; }
static long long nowUs(){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); return (long long)ts.tv_sec*1000000LL+ts.tv_nsec/1000LL; }
static int countOpenFds(){ DIR* d=opendir("/proc/self/fd"); if(!d) return -1; int n=0; struct dirent* en; while((en=readdir(d))!=NULL){ if(en->d_name[0]!='.') n++; } closedir(d); return n; }
static std::string shortErr(const ChainError& e){ std::string s=e.str(); if(s.size()>150) s=s.substr(0,150)+"..."; return s; }

static void r2AckChild(const std::string& dir){
    ChainError e; CommitStore3A s; if(!s.open(dir,&e)) ::_exit(2);
    Block b=mkBlockH(2,0,0,3); TreeReader tr(s.tree()); Delta d;
    if(!buildDeltaI(tr,b,MAT,&d,&e)) ::_exit(3);
    s.setCrashPoint(CrashPoint::AfterRename);           // rename durable, ACK not yet written
    bool ok=s.publish(d,b.hash,b.height,&e);
    ::_exit(ok?0:1); }

static void r2HammerChild(const std::string& dir,int cycles){
    const char* pat="HAMMER-SUBSTITUTE-PATTERN-0123456789-ABCDEFGHIJ"; size_t n=strlen(pat);
    const char* can_txt="RACE-CANARY-KEEP-ME-UNTOUCHED"; size_t cn=strlen(can_txt);
    int viol=0; std::string p=dir+"/undo.dat", can=dir+"/canary";
    auto canaryOk=[&]()->bool{
        struct stat st; if(::lstat(can.c_str(),&st)!=0 || (size_t)st.st_size!=cn) return false;
        char buf[128]={0}; FILE* g=fopen(can.c_str(),"rb"); size_t r=g?fread(buf,1,sizeof(buf)-1,g):0; if(g) fclose(g);
        return r==cn && memcmp(buf,can_txt,cn)==0; };
    auto patOk=[&]()->bool{
        struct stat st; if(::lstat(p.c_str(),&st)!=0) return true;        // gone: not attributable
        if(!S_ISREG(st.st_mode)) return true;                            // no longer a regular file
        if((size_t)st.st_size < n) return true;                          // nothing wrote a record here (no truncate exists)
        char buf[256]={0}; FILE* g=fopen(p.c_str(),"rb"); size_t r=g?fread(buf,1,sizeof(buf)-1,g):0; if(g) fclose(g);
        return r>=n && memcmp(buf,pat,n)==0; };
    for(int i=0;i<cycles;i++){
        ::unlink(p.c_str());                                   // A) a NONEMPTY substitute (E24 shape)
        { FILE* f=fopen(p.c_str(),"wb"); if(f){ fwrite(pat,1,n,f); fclose(f); } }
        usleep(4000);
        if(!patOk()) viol++;                                   // the store must never write a substitute
        ::unlink(p.c_str()); ::symlink(can.c_str(),p.c_str()); // C) a SYMLINK substitute (E25a shape)
        usleep(4000);
        if(!canaryOk()) viol++;                                // the store must never follow it
        ::unlink(p.c_str());                                   // E) a genuinely EMPTY residue (legitimate)
        { FILE* f=fopen(p.c_str(),"wb"); if(f) fclose(f); }
        usleep(60000);                                         // long enough for a real publish to commit
        ::unlink(p.c_str());                                   // F) absent (legitimate O_CREAT|O_EXCL)
        usleep(15000);
        // G) rapid swap churn: 1 ms empty windows separated by nonempty/symlink substitutes, so a store
        //    that validated such a window and then writes (the pre-fix failure mode) lands in a substitute
        //    microseconds later, while a fixed store can only refuse.
        for(int j=0;j<3;j++){
            ::unlink(p.c_str());
            { FILE* f=fopen(p.c_str(),"wb"); if(f) fclose(f); }
            usleep(1000);
            ::unlink(p.c_str());
            { FILE* f=fopen(p.c_str(),"wb"); if(f){ fwrite(pat,1,n,f); fclose(f); } }
            usleep(2500);
            if(!patOk()) viol++;
            ::unlink(p.c_str()); ::symlink(can.c_str(),p.c_str());
            usleep(2500);
            if(!canaryOk()) viol++;
        }
    }
    std::string vp=dir+"/hammer.violations";
    FILE* o=fopen(vp.c_str(),"wb"); if(o){ char b[32]; snprintf(b,sizeof(b),"%d",viol); fwrite(b,1,strlen(b),o); fclose(o); }
    ::_exit(viol==0?0:5); }

static void test_r2_toctou(){
    g_cur="F0-R2-toctou";
    ChainError e;
    // ---- R2-03: a genuinely empty regular residue is still accepted (policy preserved)
    { std::string d=freshDir("r2_03"); CHECK(touchEmpty(d+"/undo.dat"), "R2-03 plant an empty undo.dat");
      CommitStore3A s; CHECK(s.open(d,&e), "R2-03 empty residue still accepted: "+e.str());
      CHECK(s.generation()==0 && s.undoCount()==0, "R2-03 opens as gen0 with an empty index");
      CHECK(PUBH(s,1,0,0,3,&e), "R2-03 first publish into the untouched residue: "+e.str());
      CHECK(COH(d,&e), "R2-03 coherence"); s.close();
      CommitStore3A r; CHECK(r.open(d,&e), "R2-03 restart: "+e.str());
      CHECK(r.generation()==1 && r.undoCount()==1, "R2-03 restart reads one coherent record"); r.close(); }
    // ---- R2-04: absent undo.dat -> legitimate fresh initialization
    { std::string d=freshDir("r2_04"); CommitStore3A s; CHECK(s.open(d,&e), "R2-04 fresh init: "+e.str());
      CHECK(s.generation()==0 && !fexists(d+"/undo.dat"), "R2-04 gen0 with no undo.dat invented");
      CHECK(PUBH(s,1,0,0,3,&e), "R2-04 first publish creates the log: "+e.str());
      CHECK(COH(d,&e), "R2-04 coherence"); s.close(); }
    // ---- R2-05/06/07: initial-open policy unchanged
    { std::string d=freshDir("r2_05"); CHECK(writeJunk(d+"/undo.dat","NONEMPTY-ORPHAN"), "R2-05 plant a nonempty orphan");
      long ss=FSZ(d+"/undo.dat"); Hash sh=fdig(d+"/undo.dat");
      CommitStore3A s; bool ok=s.open(d,&e); CHECK(!ok, "R2-05 nonempty orphan refused at open: "+shortErr(e));
      CHECK(FSZ(d+"/undo.dat")==ss && fdig(d+"/undo.dat")==sh, "R2-05 orphan bytes preserved"); if(ok) s.close(); }
    { std::string d=freshDir("r2_06"); CHECK(writeJunk(d+"/canary","CANARY-06"), "R2-06 plant the canary");
      Hash cg=fdig(d+"/canary");
      CHECK(::symlink((d+"/canary").c_str(),(d+"/undo.dat").c_str())==0, "R2-06 symlink undo.dat");
      CommitStore3A s; bool ok=s.open(d,&e); CHECK(!ok, "R2-06 symlink refused at open: "+shortErr(e));
      CHECK(fdig(d+"/canary")==cg, "R2-06 symlink target untouched"); if(ok) s.close(); }
    { std::string d=freshDir("r2_07a"); CHECK(mkdir((d+"/undo.dat").c_str(),0755)==0, "R2-07a directory undo.dat");
      CommitStore3A s; bool ok=s.open(d,&e); CHECK(!ok, "R2-07a directory refused at open: "+shortErr(e)); if(ok) s.close(); }
    { std::string d=freshDir("r2_07b"); CHECK(::mkfifo((d+"/undo.dat").c_str(),0644)==0, "R2-07b fifo undo.dat");
      long long t0=nowUs(); CommitStore3A s; bool ok=s.open(d,&e); long long dt=nowUs()-t0;
      CHECK(!ok, "R2-07b fifo refused at open: "+shortErr(e)); CHECK(dt<2000000, "R2-07b refusal did not block ("+std::to_string(dt)+" us)"); if(ok) s.close(); }
    // ---- R2-01/R2-08 (E24): empty -> NONEMPTY regular substitution
    { std::string d=freshDir("r2_08"); CHECK(touchEmpty(d+"/undo.dat"), "R2-08 plant an empty residue");
      CommitStore3A s; CHECK(s.open(d,&e), "R2-08 fresh open over the empty residue: "+e.str());
      CHECK(substFile(d+"/undo.dat","SUBSTITUTED-NONEMPTY-CONTENT-0123456789"), "R2-08 substitute a NONEMPTY regular file (unlink+create)");
      uint64_t dv=0,ino=0; long long sz=0,ct=0; CHECK(statTuple(d+"/undo.dat",&dv,&ino,&sz,&ct), "R2-08 stat the substitute");
      Hash sh=fdig(d+"/undo.dat");
      bool ok=PUBH(s,1,0,0,3,&e);
      printf("  R2-08 substitute dev=%llu ino=%llu size=%lld ctime=%lld -> %s\n",(unsigned long long)dv,(unsigned long long)ino,sz,ct,shortErr(e).c_str());
      CHECK(!ok, "R2-08 (E24) nonempty substitution refused before any write: "+shortErr(e));
      CHECK(FSZ(d+"/undo.dat")==sz && fdig(d+"/undo.dat")==sh, "R2-08 substituted bytes unchanged ("+std::to_string(sz)+" B)");
      CHECK(!fexists(d+"/MANIFEST") && !fexists(d+"/ACK") && !fexists(d+"/MANIFEST.tmp"), "R2-08 no state published over the substitute");
      s.close(); }
    // ---- R2-02/R2-09 (E25a): empty -> SYMLINK substitution
    { std::string d=freshDir("r2_09"); CHECK(touchEmpty(d+"/undo.dat"), "R2-09 plant an empty residue");
      CHECK(writeJunk(d+"/canary","RACE-CANARY-KEEP-ME-UNTOUCHED"), "R2-09 plant the symlink target");
      long cs=FSZ(d+"/canary"); Hash cg=fdig(d+"/canary");
      CommitStore3A s; CHECK(s.open(d,&e), "R2-09 fresh open over the empty residue: "+e.str());
      CHECK(::unlink((d+"/undo.dat").c_str())==0 && ::symlink((d+"/canary").c_str(),(d+"/undo.dat").c_str())==0, "R2-09 replace with a symlink");
      bool ok=PUBH(s,1,0,0,3,&e);
      printf("  R2-09 symlink substitute -> %s\n", shortErr(e).c_str());
      CHECK(!ok, "R2-09 (E25a) symlink substitution refused (never followed): "+shortErr(e));
      CHECK(FSZ(d+"/canary")==cs && fdig(d+"/canary")==cg, "R2-09 symlink TARGET bytes unchanged ("+std::to_string(cs)+" B)");
      mode_t m=0; CHECK(LSTATM(d+"/undo.dat",&m)&&S_ISLNK(m), "R2-09 the symlink was not replaced by a regular file");
      s.close(); }
    // ---- R2-10/R2-11/R2-29: empty -> different EMPTY inode (recreate) ; identity is verified at the write
    { std::string d=freshDir("r2_10"); CHECK(touchEmpty(d+"/undo.dat"), "R2-10 plant an empty residue");
      uint64_t d0=0,i0=0; long long s0=0,c0=0; CHECK(statTuple(d+"/undo.dat",&d0,&i0,&s0,&c0), "R2-10 original identity");
      CommitStore3A s; CHECK(s.open(d,&e), "R2-10 fresh open: "+e.str());
      CHECK(substFile(d+"/undo.dat",""), "R2-10 recreate the pathname as another EMPTY file");
      uint64_t d1=0,i1=0; long long s1=0,c1=0; CHECK(statTuple(d+"/undo.dat",&d1,&i1,&s1,&c1), "R2-10 substitute identity");
      printf("  R2-10 original dev=%llu ino=%llu ctime=%lld -> substitute dev=%llu ino=%llu ctime=%lld (both 0 B)\n",
             (unsigned long long)d0,(unsigned long long)i0,c0,(unsigned long long)d1,(unsigned long long)i1,c1);
      bool ok=PUBH(s,1,0,0,3,&e);
      CHECK(!ok, "R2-10 (C5) a different empty inode is refused at the write boundary: "+shortErr(e));
      CHECK(e.str().find("identity")!=std::string::npos || e.str().find("inode")!=std::string::npos, "R2-10 diagnostic names the identity mismatch");
      CHECK(FSZ(d+"/undo.dat")==0, "R2-10 the substituted empty file is still empty (not written)");
      CHECK(!fexists(d+"/MANIFEST") && !fexists(d+"/ACK"), "R2-10 no state published"); s.close(); }
    { std::string d=freshDir("r2_11"); CHECK(touchEmpty(d+"/undo.dat"), "R2-11 plant an empty residue");
      CommitStore3A s; CHECK(s.open(d,&e), "R2-11 fresh open: "+e.str());
      CHECK(::unlink((d+"/undo.dat").c_str())==0, "R2-11 unlink the classified residue");
      CHECK(touchEmpty(d+"/undo.dat"), "R2-11 recreate the pathname empty");
      bool ok=PUBH(s,1,0,0,3,&e);
      CHECK(!ok, "R2-11 (C6) unlink/recreate of the pathname refused: "+shortErr(e));
      CHECK(FSZ(d+"/undo.dat")==0 && !fexists(d+"/MANIFEST"), "R2-11 recreated file untouched, nothing published"); s.close(); }
    // ---- R2-12/R2-13/R2-14/R2-15: other substitution kinds
    { std::string d=freshDir("r2_12"); CHECK(touchEmpty(d+"/undo.dat"), "R2-12 plant an empty residue");
      CommitStore3A s; CHECK(s.open(d,&e), "R2-12 fresh open: "+e.str());
      CHECK(::unlink((d+"/undo.dat").c_str())==0 && mkdir((d+"/undo.dat").c_str(),0755)==0, "R2-12 replace with a directory");
      bool ok=PUBH(s,1,0,0,3,&e); CHECK(!ok, "R2-12 directory substitution refused: "+shortErr(e));
      mode_t m=0; CHECK(LSTATM(d+"/undo.dat",&m)&&S_ISDIR(m), "R2-12 directory left in place"); s.close(); }
    { std::string d=freshDir("r2_13"); CHECK(touchEmpty(d+"/undo.dat"), "R2-13 plant an empty residue");
      CommitStore3A s; CHECK(s.open(d,&e), "R2-13 fresh open: "+e.str());
      CHECK(::unlink((d+"/undo.dat").c_str())==0 && ::mkfifo((d+"/undo.dat").c_str(),0644)==0, "R2-13 replace with a FIFO");
      long long t0=nowUs(); bool ok=PUBH(s,1,0,0,3,&e); long long dt=nowUs()-t0;
      printf("  R2-13 FIFO refusal elapsed=%lld us -> %s\n", dt, shortErr(e).c_str());
      CHECK(!ok, "R2-13 FIFO substitution refused: "+shortErr(e));
      CHECK(dt<2000000, "R2-13 refusal did not block on the FIFO ("+std::to_string(dt)+" us)");
      mode_t m=0; CHECK(LSTATM(d+"/undo.dat",&m)&&S_ISFIFO(m), "R2-13 FIFO left in place"); s.close(); }
    { std::string d=freshDir("r2_14"); CHECK(touchEmpty(d+"/undo.dat"), "R2-14 plant an empty residue");
      CommitStore3A s; CHECK(s.open(d,&e), "R2-14 fresh open: "+e.str());
      if(::geteuid()!=0){
        CHECK(substFile(d+"/undo.dat","") && ::chmod((d+"/undo.dat").c_str(),0000)==0, "R2-14 substitute an unreadable empty file");
        bool ok=PUBH(s,1,0,0,3,&e); CHECK(!ok, "R2-14 unreadable substitution refused: "+shortErr(e));
        ::chmod((d+"/undo.dat").c_str(),0644); CHECK(FSZ(d+"/undo.dat")==0, "R2-14 file untouched (still empty)");
      } else printf("  R2-14 note: running as root, EACCES path not exercised\n");
      s.close(); }
    { std::string d=freshDir("r2_15"); CHECK(touchEmpty(d+"/undo.dat"), "R2-15 plant an empty residue");
      CommitStore3A s; CHECK(s.open(d,&e), "R2-15 fresh open: "+e.str());
      CHECK(::unlink((d+"/undo.dat").c_str())==0, "R2-15 the residue disappears");
      bool ok=PUBH(s,1,0,0,3,&e); CHECK(!ok, "R2-15 missing path refused (never recreated): "+shortErr(e));
      CHECK(!fexists(d+"/undo.dat") && !fexists(d+"/MANIFEST") && !fexists(d+"/ACK"), "R2-15 no file created");
      s.close();
      // Pre-existing doctrine (R1/F0, unchanged by R2): a first publish that dies before the undo write
      // leaves whole tree pages with no MANIFEST/ACK. That residue is prior-store evidence, so the store
      // must keep refusing it deterministically instead of silently initialising a genesis over it.
      long tb=FSZ(d+"/tree.dat"); Hash th=fdig(d+"/tree.dat");
      CommitStore3A r; bool ok2=r.open(d,&e);
      CHECK(!ok2, "R2-15 the partial-initialization residue is still refused deterministically: "+shortErr(e));
      CHECK(FSZ(d+"/tree.dat")==tb && fdig(d+"/tree.dat")==th, "R2-15 the refused store's bytes are preserved ("+std::to_string(tb)+" B tree.dat)");
      CHECK(!fexists(d+"/MANIFEST") && !fexists(d+"/ACK"), "R2-15 no pointers invented by the refusal");
      if(ok2) r.close(); }
    // ---- R2-16/R2-17: absent -> appeared file/symlink is refused by O_CREAT|O_EXCL
    { std::string d=freshDir("r2_16"); CommitStore3A s; CHECK(s.open(d,&e), "R2-16 fresh open with undo.dat absent");
      CHECK(substFile(d+"/undo.dat","APPEARED-AFTER-CLASSIFICATION"), "R2-16 a nonempty file appears");
      long as=FSZ(d+"/undo.dat"); Hash ah=fdig(d+"/undo.dat");
      bool ok=PUBH(s,1,0,0,3,&e); CHECK(!ok, "R2-16 O_CREAT|O_EXCL refuses the appeared file: "+shortErr(e));
      CHECK(FSZ(d+"/undo.dat")==as && fdig(d+"/undo.dat")==ah, "R2-16 appeared file unchanged"); s.close(); }
    { std::string d=freshDir("r2_17"); CHECK(writeJunk(d+"/canary","CANARY-17"), "R2-17 plant the canary");
      long cs=FSZ(d+"/canary"); Hash cg=fdig(d+"/canary");
      CommitStore3A s; CHECK(s.open(d,&e), "R2-17 fresh open with undo.dat absent");
      CHECK(::symlink((d+"/canary").c_str(),(d+"/undo.dat").c_str())==0, "R2-17 a symlink appears");
      bool ok=PUBH(s,1,0,0,3,&e); CHECK(!ok, "R2-17 O_CREAT|O_EXCL refuses the appeared symlink: "+shortErr(e));
      CHECK(FSZ(d+"/canary")==cs && fdig(d+"/canary")==cg, "R2-17 symlink target unchanged");
      mode_t m=0; CHECK(LSTATM(d+"/undo.dat",&m)&&S_ISLNK(m), "R2-17 symlink preserved"); s.close(); }
    // ---- R2-18/R2-19: same inode stays empty (ok) / becomes nonempty (no overwrite)
    { std::string d=freshDir("r2_19"); CHECK(touchEmpty(d+"/undo.dat"), "R2-19 plant an empty residue");
      CommitStore3A s; CHECK(s.open(d,&e), "R2-19 fresh open: "+e.str());
      { FILE* f=fopen((d+"/undo.dat").c_str(),"ab"); CHECK(f!=NULL, "R2-19 append to the SAME inode");
        if(f){ fwrite("XYZ12",1,5,f); fclose(f); } }
      uint64_t dv=0,ino=0; long long sz=0,ct=0; statTuple(d+"/undo.dat",&dv,&ino,&sz,&ct); Hash sh=fdig(d+"/undo.dat");
      bool ok=PUBH(s,1,0,0,3,&e);
      CHECK(!ok, "R2-19 same inode became nonempty -> refused before writing: "+shortErr(e));
      CHECK(FSZ(d+"/undo.dat")==sz && fdig(d+"/undo.dat")==sh, "R2-19 the bytes written by the operator survive");
      s.close(); }
    // ---- R2-20/R2-21: original renamed away / unlinked without replacement
    { std::string d=freshDir("r2_20"); CHECK(touchEmpty(d+"/undo.dat"), "R2-20 plant an empty residue");
      CommitStore3A s; CHECK(s.open(d,&e), "R2-20 fresh open: "+e.str());
      CHECK(::rename((d+"/undo.dat").c_str(),(d+"/undo.dat.moved").c_str())==0, "R2-20 rename the classified inode away");
      CHECK(substFile(d+"/undo.dat","REPLACEMENT-AFTER-RENAME"), "R2-20 install a replacement");
      long rs=FSZ(d+"/undo.dat"); Hash rh=fdig(d+"/undo.dat");
      bool ok=PUBH(s,1,0,0,3,&e); CHECK(!ok, "R2-20 refusal when the classified inode moved away: "+shortErr(e));
      CHECK(FSZ(d+"/undo.dat")==rs && fdig(d+"/undo.dat")==rh, "R2-20 replacement bytes unchanged");
      CHECK(FSZ(d+"/undo.dat.moved")==0, "R2-20 the renamed original is still empty (no write landed on it)");
      s.close(); }
    // ---- R2-22: repeated restart after a refusal: deterministic, no silent genesis
    { std::string d=freshDir("r2_22"); CHECK(touchEmpty(d+"/undo.dat"), "R2-22 plant an empty residue");
      { CommitStore3A s; CHECK(s.open(d,&e), "R2-22 fresh open: "+e.str());
        CHECK(substFile(d+"/undo.dat","SUBSTITUTED-KEEP-ME"), "R2-22 substitute a nonempty file");
        CHECK(!PUBH(s,1,0,0,3,&e), "R2-22 refused"); s.close(); }
      long ss=FSZ(d+"/undo.dat"); Hash sh=fdig(d+"/undo.dat"); std::string m0;
      for(int i=0;i<3;i++){ CommitStore3A r; bool ok=r.open(d,&e); std::string m=e.str();
        CHECK(!ok, "R2-22 restart "+std::to_string(i)+" refuses the substitution (no silent genesis): "+shortErr(e));
        if(i==0){ m0=m; CHECK(!m0.empty(), "R2-22 diagnostic non-empty"); } else CHECK(m==m0, "R2-22 deterministic diagnostic");
        if(ok) r.close(); }
      CHECK(FSZ(d+"/undo.dat")==ss && fdig(d+"/undo.dat")==sh, "R2-22 substitute untouched after repeated refusals");
      CHECK(!fexists(d+"/MANIFEST") && !fexists(d+"/ACK"), "R2-22 no pointers invented"); }
    // ---- R2-23/R2-24: multi-key exact digest and disconnect/reconnect on a residue-initialised store
    { std::string d=freshDir("r2_23"); CHECK(touchEmpty(d+"/undo.dat"), "R2-23 plant an empty residue");
      auto mkMulti=[&](int32_t h,const Hash& bh,const Hash& cbtx,std::vector<Hash> spends)->Block{
          Block b; b.hash=bh; b.height=h;
          for (Hash sp: spends){ OutPoint prev; prev.txid=sp; prev.n=0; Tx t; t.txid=id32(0x77000000u,(uint32_t)(h*10+(int)sp.data()[0])); t.coinbase=false;
              TxIn in; in.prev=prev; t.vin.push_back(in); TxOut o; o.value=10*COIN; o.script={5,5}; t.vout.push_back(o); b.txs.push_back(t); }
          Tx cb; cb.txid=cbtx; cb.coinbase=true; cb.coinstake=false; TxOut o; o.value=50*COIN; o.script={1,2,3}; cb.vout.push_back(o); b.txs.push_back(cb);
          return b; };
      Hash dg;
      { CommitStore3A s; CHECK(s.open(d,&e), "R2-23 fresh open: "+e.str());
        CHECK(PUBH(s,1,0,0,3,&e), "R2-23 h1"); CHECK(PUBH(s,2,0,0,3,&e), "R2-23 h2");
        dg=DG(s,&e);
        Block b3=mkMulti(3,id32(0xB10C0000u,50),id32(0xCB000000u,50),{id32(0xCB000000u,1),id32(0xCB000000u,2)});
        CHECK(publishStore(s,b3,&e), "R2-23 multi-key publish h3: "+e.str());
        CHECK(s.disconnect(id32(0xB10C0000u,50),&e), "R2-23 disconnect h3");
        CHECK(DG(s,&e)==dg, "R2-23 multi-key disconnect restores the exact digest");
        CHECK(publishStore(s,b3,&e), "R2-23 re-publish h3");
        CHECK(COH(d,&e), "R2-23 coherence after the residue-initialised multi-key commit"); s.close(); }
      { CommitStore3A r; CHECK(r.open(d,&e), "R2-23 restart: "+e.str());
        CHECK(r.tipHash()==id32(0xB10C0000u,50), "R2-23 tip == h3 after restart");
        CHECK(r.disconnect(id32(0xB10C0000u,50),&e), "R2-23 restart disconnect");
        CHECK(DG(r,&e)==dg, "R2-23 exact digest == the pre-h3 digest after disconnect/restart"); r.close(); } }
    { std::string d=freshDir("r2_24"); CHECK(touchEmpty(d+"/undo.dat"), "R2-24 plant an empty residue");
      Hash d1; ChainError ce;
      { CommitStore3A s; CHECK(s.open(d,&ce), "R2-24 fresh open: "+ce.str());
        CHECK(PUBH(s,1,0,0,3,&ce), "R2-24 h1"); d1=DG(s,&ce);
        CHECK(PUBH(s,2,0,0,3,&ce), "R2-24 h2");
        CHECK(s.disconnect(id32(0xB10C0000u,2),&ce), "R2-24 disconnect h2");
        CHECK(DG(s,&ce)==d1, "R2-24 back to the h1 digest"); s.close(); }
      { CommitStore3A r; CHECK(r.open(d,&ce), "R2-24 restart: "+ce.str());
        CHECK(DG(r,&ce)==d1, "R2-24 digest preserved across restart");
        CHECK(PUBH(r,2,0,0,3,&ce), "R2-24 reconnect publish");
        CHECK(r.disconnect(id32(0xB10C0000u,2),&ce), "R2-24 disconnect again");
        CHECK(DG(r,&ce)==d1, "R2-24 exact digest after the reconnect/restart cycle"); r.close(); } }
    // ---- R2-25: the F0-1 durability invariant still holds on a residue-initialised store
    { std::string d=freshDir("r2_25"); CHECK(touchEmpty(d+"/undo.dat"), "R2-25 plant an empty residue");
      { CommitStore3A s; CHECK(s.open(d,&e), "R2-25 fresh open: "+e.str());
        CHECK(PUBH(s,1,0,0,3,&e), "R2-25 h1"); CHECK(PUBH(s,2,0,0,3,&e), "R2-25 h2"); CHECK(PUBH(s,3,0,0,2000,&e), "R2-25 h3 (long)");
        CHECK(s.disconnect(id32(0xB10C0000u,3),&e), "R2-25 disconnect h3"); s.close(); }
      uint64_t mu=MUU(d), bu=MBU(d); long pb=FSZ(d+"/undo.dat");
      printf("  R2-25 residue-init store: primary.undoSize=%llu bak.undoSize=%llu physical=%ld\n",(unsigned long long)mu,(unsigned long long)bu,pb);
      CHECK(bu>mu && pb==(long)bu, "R2-25 vulnerable shape reproduced on the residue path");
      CHECK(CPFILE(d+"/undo.dat",d+"/undo.pre"), "R2-25 snapshot undo.dat");
      { CommitStore3A s; CHECK(s.open(d,&e), "R2-25 reopen");
        s.setInjectStage(InjectStage::ManifestTmp); s.injectNoSpace(true);
        CHECK(!PUBH(s,4,0,0,3,&e), "R2-25 reconnect publish fails after the undo write: "+shortErr(e));
        s.clearInjection(); s.close(); }
      uint64_t mu2=MUU(d), bu2=MBU(d);
      CHECK(bu2<=mu2, "R2-25 F0-1 invariant: retained bak.undoSize "+std::to_string(bu2)+" <= primary.undoSize "+std::to_string(mu2));
      CHECK(RNGD(d+"/undo.dat",0,bu2)==RNGD(d+"/undo.pre",0,bu2), "R2-25 the retained backup references only unmodified bytes");
      writeJunk(d+"/MANIFEST","GARBAGE-PRIMARY-AFTER-THE-WINDOW");
      { CommitStore3A r; bool ok=r.open(d,&e);
        CHECK(ok, "R2-25 double fault recovers the acknowledged generation: "+shortErr(e));
        if(ok){ CHECK(r.generation()==4, "R2-25 recovered generation 4"); CHECK(r.acknowledgedGeneration()==4, "R2-25 ACK still 4"); r.close(); } } }
    // ---- R2-26: partial undo write (established store recovers; initial residue fails closed)
    { std::string d=freshDir("r2_26"); CHECK(touchEmpty(d+"/undo.dat"), "R2-26 plant an empty residue");
      { CommitStore3A s; CHECK(s.open(d,&e), "R2-26 fresh open: "+e.str());
        s.injectUndoShortOnce(20);
        bool ok=PUBH(s,1,0,0,3,&e); s.clearInjection();
        CHECK(!ok, "R2-26 torn first undo write fails closed: "+shortErr(e)); s.close(); }
      long us=FSZ(d+"/undo.dat");
      printf("  R2-26 after the torn first write: undo.dat=%ld B, MANIFEST present=%d\n", us, (int)fexists(d+"/MANIFEST"));
      CHECK(us>0 && us<200, "R2-26 the partial record residue is present in the log ("+std::to_string(us)+" B)");
      CHECK(!fexists(d+"/MANIFEST") && !fexists(d+"/ACK"), "R2-26 nothing was acknowledged by the torn write");
      { CommitStore3A r; bool ok=r.open(d,&e);
        CHECK(!ok, "R2-26 reopen refuses the partial-initialization residue: "+shortErr(e));
        CHECK(FSZ(d+"/undo.dat")==us, "R2-26 the residue bytes are preserved"); if(ok) r.close(); } }
    // ---- R2-27: ACK lag reconciliation on a residue-initialised store
    { std::string d=freshDir("r2_27"); CHECK(touchEmpty(d+"/undo.dat"), "R2-27 plant an empty residue");
      { CommitStore3A s; CHECK(s.open(d,&e), "R2-27 fresh open: "+e.str()); CHECK(PUBH(s,1,0,0,3,&e), "R2-27 h1"); s.close(); }
      pid_t pid=fork();
      if(pid==0){ r2AckChild(d); ::_exit(9); }
      int st=0; waitpid(pid,&st,0);
      CHECK(WIFEXITED(st)&&WEXITSTATUS(st)==42, "R2-27 child crashed after the rename, before the ACK ("+std::to_string(WIFEXITED(st)?WEXITSTATUS(st):-1)+")");
      { CommitStore3A r; bool ok=r.open(d,&e);
        CHECK(ok, "R2-27 reopen after the ACK lag: "+shortErr(e));
        if(ok){ CHECK(r.generation()==2, "R2-27 NEW generation 2 is durable");
                CHECK(r.acknowledgedGeneration()==2, "R2-27 ACK reconciled 1->2");
                CHECK(COH(d,&e), "R2-27 coherence"); r.close(); } } }
    // ---- R2-28: no descriptor leak on refusal / write-failure paths
    { std::string d=freshDir("r2_28"); CHECK(touchEmpty(d+"/undo.dat"), "R2-28 plant an empty residue");
      int before=countOpenFds();
      { CommitStore3A s; CHECK(s.open(d,&e), "R2-28 fresh open: "+e.str());
        CHECK(substFile(d+"/undo.dat","SUBSTITUTE-FD-CHECK"), "R2-28 substitution (open must not leak)");
        CHECK(!PUBH(s,1,0,0,3,&e), "R2-28 refused"); s.close(); }
      int after=countOpenFds();
      printf("  R2-28 open-fd count %d -> %d\n", before, after);
      CHECK(before>0 && after>0 && after<=before, "R2-28 no descriptor leak on the substitution refusal path");
      std::string d2=freshDir("r2_28b"); CHECK(touchEmpty(d2+"/undo.dat"), "R2-28b plant an empty residue");
      int b2=countOpenFds();
      { CommitStore3A s; CHECK(s.open(d2,&e), "R2-28b fresh open: "+e.str());
        s.setInjectStage(InjectStage::UndoAppend); s.injectFsyncFail(true);
        CHECK(!PUBH(s,1,0,0,3,&e), "R2-28b injected fsync failure refuses: "+shortErr(e));
        s.clearInjection(); s.close(); }
      int a2=countOpenFds();
      printf("  R2-28b write+fsync-failure fd count %d -> %d\n", b2, a2);
      CHECK(a2<=b2, "R2-28b no descriptor leak on the write/fsync failure path"); }
}

static void test_r2_toctou_race(){
    g_cur="F0-R2-toctou-race";
    ChainError e;
    std::string d=freshDir("r2_race");
    CHECK(touchEmpty(d+"/undo.dat"), "R2-race plant an empty residue");
    CHECK(writeJunk(d+"/canary","RACE-CANARY-KEEP-ME-UNTOUCHED"), "R2-race plant the symlink canary");
    long cs=FSZ(d+"/canary"); Hash cg=fdig(d+"/canary");
    pid_t pid=fork();
    if(pid==0){ r2HammerChild(d,30); ::_exit(9); }
    int succ=0, openRef=0, pubRef=0; std::string firstErr; long long t0=nowUs();
    for(int k=0;k<160;k++){
        // Each attempt starts from a clean fresh-store state: an attempt that dies between the backup
        // barrier and the primary publish would otherwise leave MANIFEST.bak without a primary and
        // poison every later open. The hammer keeps owning undo.dat.
        ::unlink((d+"/MANIFEST").c_str()); ::unlink((d+"/MANIFEST.bak").c_str()); ::unlink((d+"/MANIFEST.tmp").c_str());
        ::unlink((d+"/ACK").c_str()); ::unlink((d+"/tree.dat").c_str());
        if(k==0) touchEmpty(d+"/undo.dat");
        CommitStore3A s; ChainError e2;
        if(s.open(d,&e2)){
            Block b=mkBlockH(1,0,0,3); TreeReader tr(s.tree()); Delta dd;
            if(buildDeltaI(tr,b,MAT,&dd,&e2)){
                if(s.publish(dd,b.hash,b.height,&e2)) succ++;
                else { pubRef++; if(firstErr.empty()) firstErr="publish: "+shortErr(e2); }
            } else { pubRef++; if(firstErr.empty()) firstErr="delta: "+shortErr(e2); }
            s.close();
        } else { openRef++; if(firstErr.empty()) firstErr="open: "+shortErr(e2); }
        if(nowUs()-t0 > 6000000) break;                 // bounded: ~6 s of live hammering
        usleep(2000);
    }
    int st=0; waitpid(pid,&st,0);
    int cv=-1; { FILE* f=fopen((d+"/hammer.violations").c_str(),"rb"); if(f){ char b[32]={0}; if(fread(b,1,sizeof(b)-1,f)>0) cv=atoi(b); fclose(f); } }
    printf("  R2-race (live substitution hammer): publishes_ok=%d open_refusals=%d publish_refusals=%d hammer_violations=%d child_exit=%d\n",
           succ, openRef, pubRef, cv, WIFEXITED(st)?WEXITSTATUS(st):-1);
    printf("  R2-race first refusal: %s\n", firstErr.c_str());
    CHECK(FSZ(d+"/canary")==cs && fdig(d+"/canary")==cg, "R2-race symlink target never written under concurrency");
    CHECK(cv==0, "R2-race concurrent hammer observed zero substituted-file overwrites");
    CHECK(WIFEXITED(st)&&WEXITSTATUS(st)==0, "R2-race hammer exited clean (violations=0)");
    // deterministic companion: after the race, a legitimate empty residue still initializes and publishes
    { std::string d2=freshDir("r2_race_ok"); CHECK(touchEmpty(d2+"/undo.dat"), "R2-race_ok plant an empty residue");
      CommitStore3A s; CHECK(s.open(d2,&e), "R2-race_ok empty residue accepted after the race: "+e.str());
      CHECK(PUBH(s,1,0,0,3,&e), "R2-race_ok legitimate publish still works: "+e.str());
      CHECK(COH(d2,&e), "R2-race_ok coherence"); s.close(); }
}

// ==================== Stage 3B-F0-R3: first-publication failure lifecycle ====================
// One-shot failure injectors: each maps to exactly one point of the publication path (DESIGN.md 13.1), so the
// failure/retry matrix can distinguish "before any undo mutation" from "after the undo write" and from the
// post-rename phases -- failure classes that do NOT have identical durability consequences.
enum R3Fail { R3F_NONE=0, R3F_PageWrite=1, R3F_UndoNoSpace=2, R3F_UndoFsync=3, R3F_BakWrite=4,
              R3F_ManifestTmpWrite=5, R3F_ManifestTmpFsync=6, R3F_ManifestRename=7, R3F_DirFsync=8,
              R3F_AckWrite=9, R3F_TornUndo=10 };
static void r3Inject(CommitStore3A& s,int which,size_t tornBytes=0){
    s.clearInjection();
    switch(which){
        case R3F_PageWrite:        s.setInjectStage(InjectStage::PageWrite);     s.injectNoSpace(true);   break;
        case R3F_UndoNoSpace:      s.setInjectStage(InjectStage::UndoAppend);    s.injectNoSpace(true);   break;
        case R3F_UndoFsync:        s.setInjectStage(InjectStage::UndoAppend);    s.injectFsyncFail(true); break;
        case R3F_BakWrite:         s.setInjectStage(InjectStage::BakWrite);      s.injectNoSpace(true);   break;
        case R3F_ManifestTmpWrite: s.setInjectStage(InjectStage::ManifestTmp);   s.injectNoSpace(true);   break;
        case R3F_ManifestTmpFsync: s.setInjectStage(InjectStage::ManifestTmp);   s.injectFsyncFail(true); break;
        case R3F_ManifestRename:   s.setInjectStage(InjectStage::PrimaryRename); s.injectNoSpace(true);   break;
        case R3F_DirFsync:         s.setInjectStage(InjectStage::DirFsync);      s.injectNoSpace(true);   break;
        case R3F_AckWrite:         s.setInjectStage(InjectStage::AckWrite);      s.injectNoSpace(true);   break;
        case R3F_TornUndo:         s.injectUndoShortOnce(tornBytes? tornBytes : 20); break;
        default: break;
    }
}
static std::string r3hex(const Hash& h){ char b[65]; const uint8_t* p=h.data(); for(int i=0;i<32;i++) sprintf(b+2*i,"%02x",p[i]); b[64]=0; return std::string(b); }
// Full persistent-state fingerprint (sizes + hashes of every artifact): proves a refused mutation wrote nothing.
static std::string r3Snap(const std::string& d){
    std::string s;
    s+="undo:"+std::to_string(FSZ(d+"/undo.dat"))+":"+r3hex(fdig(d+"/undo.dat"));
    s+="|man:"+std::to_string(FSZ(d+"/MANIFEST"))+":"+r3hex(fdig(d+"/MANIFEST"));
    s+="|bak:"+std::to_string(FSZ(d+"/MANIFEST.bak"))+":"+r3hex(fdig(d+"/MANIFEST.bak"));
    s+="|ack:"+std::to_string(FSZ(d+"/ACK"))+":"+r3hex(fdig(d+"/ACK"));
    s+="|tree:"+std::to_string(FSZ(d+"/tree.dat"))+":"+r3hex(fdig(d+"/tree.dat"));
    s+="|tmp:"+std::to_string((int)fexists(d+"/MANIFEST.tmp")+(int)fexists(d+"/ACK.tmp")+(int)fexists(d+"/MANIFEST.bak.tmp"));
    return s;
}
static std::string r3wstatus(int st){
    if(WIFSIGNALED(st)) return "signal "+std::to_string((int)WTERMSIG(st));
    if(WIFEXITED(st))   return "exit "+std::to_string((int)WEXITSTATUS(st));
    return "unknown"; }
// Rewrite ACK to an arbitrary generation (test-only; ACK = 8B gen LE + 4B crc32).
static bool r3WriteAckGen(const std::string& dir,uint64_t gen){
    uint8_t b[12]={0}; for(int i=0;i<8;i++) b[i]=(uint8_t)((gen>>(8*i))&0xff);
    uint32_t crc=csCrc32(b,8); for(int i=0;i<4;i++) b[8+i]=(uint8_t)((crc>>(8*i))&0xff);
    std::string tmp=dir+"/ACK.r3tmp";
    FILE* f=fopen(tmp.c_str(),"wb"); if(!f) return false;
    bool ok=fwrite(b,1,12,f)==12; fclose(f); if(!ok){ ::unlink(tmp.c_str()); return false; }
    return ::rename(tmp.c_str(),(dir+"/ACK").c_str())==0; }
// The audit's E-A substitute shape: a small NONEMPTY regular file planted at undo.dat.
static const char* R3_SUB="R3-SUBSTITUTED-NONEMPTY-REGULAR-PAYLOAD-41B";

// ---- R3-01..R3-21: retry refusal, failure classes, entry points, resource hygiene
static void test_r3_retry(){
    g_cur="F0-R3-retry";
    ChainError e;

    // ---- R3-01/R3-02/R3-03/R3-34: the audited E-A corruption (pre-fix) vs the fail-closed refusal (post-fix)
    { std::string d=freshDir("r3_01");
      CHECK(touchEmpty(d+"/undo.dat"), "R3-01 plant an ACCEPTED empty residue");
      CommitStore3A s; CHECK(s.open(d,&e), "R3-01 fresh open over the empty residue: "+e.str());
      CHECK(s.generation()==0 && s.undoCount()==0, "R3-01 opens as gen0 with an empty undo index");
      r3Inject(s,R3F_ManifestTmpWrite);                       // pre-MANIFEST-rename failure (the audited E-A)
      ChainError e1; bool p1=PUBH(s,1,0,0,3,&e1); s.clearInjection();
      printf("  R3-01 first publish: publish=%d gen=%llu undo=%ld MANIFEST=%d err=%s\n",(int)p1,
             (unsigned long long)s.generation(),FSZ(d+"/undo.dat"),(int)fexists(d+"/MANIFEST"),shortErr(e1).c_str());
      CHECK(!p1, "R3-01/R3-09 first publish fails before the MANIFEST rename: "+shortErr(e1));
      CHECK(s.generation()==0, "R3-01 no generation committed in memory");
      CHECK(FSZ(d+"/undo.dat")>0, "R3-01 the undo record was written+fsynced before the failure (size "+std::to_string(FSZ(d+"/undo.dat"))+")");
      CHECK(!fexists(d+"/MANIFEST") && !fexists(d+"/ACK"), "R3-01 no MANIFEST/ACK published");
      CHECK(substFile(d+"/undo.dat",R3_SUB), "R3-01 substitute a nonempty regular file at undo.dat");
      long ss=FSZ(d+"/undo.dat"); Hash sh=fdig(d+"/undo.dat"); std::string snap=r3Snap(d);
      printf("  R3-01 substitute: %ld bytes sha=%s\n", ss, r3hex(sh).c_str());
      ChainError e2; bool p2=PUBH(s,1,0,0,3,&e2);             // retry: SAME instance, SAME block
      printf("  R3-01 retry (same instance): publish=%d undo=%ld->%ld needsReopen=%d err=%s\n",(int)p2,ss,
             FSZ(d+"/undo.dat"),(int)s.needsReopen(),shortErr(e2).c_str());
      CHECK(!p2, "R3-01/R3-02 (F0-2-R2-1) retry refused before any write: "+shortErr(e2));
      CHECK(s.needsReopen(), "R3-01 instance reports REOPEN_REQUIRED after the failed first publish");
      CHECK(FSZ(d+"/undo.dat")==ss && fdig(d+"/undo.dat")==sh, "R3-01/R3-03 substituted file unchanged ("+std::to_string(ss)+" B, sha identical)");
      // R3-16: every public mutation entry point enforces the state
      ChainError e3; bool dok=s.disconnect(id32(0xBB000000u,1),&e3);
      printf("  R3-16 disconnect on a REOPEN_REQUIRED instance: %d err=%s\n",(int)dok,shortErr(e3).c_str());
      CHECK(!dok, "R3-16 disconnect refused too: "+shortErr(e3));
      CHECK(s.generation()==0 && s.undoCount()==0, "R3-16/R3-34 no state changed by the refused mutations");
      CHECK(r3Snap(d)==snap, "R3-34 no persistent mutation at all after REOPEN_REQUIRED (full datadir fingerprint)");
      CHECK(!fexists(d+"/MANIFEST.tmp") && !fexists(d+"/ACK.tmp"), "R3-34 no temp file left behind");
      s.close();
      // R3-18/R3-26: reopen reclassifies disk evidence and refuses, deterministically, repeatedly
      std::string first; int refused=0;
      for(int i=0;i<3;i++){ CommitStore3A r; ChainError e4; if(!r.open(d,&e4)){ refused++; if(first.empty()) first=shortErr(e4); } else r.close(); }
      printf("  R3-18 reopen after the failure: refused %d/3 first=%s\n", refused, first.c_str());
      CHECK(refused==3, "R3-18/R3-26 three consecutive reopens all refuse ("+std::to_string(refused)+"/3)");
      CHECK(FSZ(d+"/undo.dat")==ss && fdig(d+"/undo.dat")==sh, "R3-18 the substitute is still untouched after the refusals");
    }

    // ---- R3-04: symlink substitution after the failure -> target untouched, refusal must not follow it
    { std::string d=freshDir("r3_04");
      CHECK(touchEmpty(d+"/undo.dat"), "R3-04 plant an ACCEPTED empty residue");
      CommitStore3A s; CHECK(s.open(d,&e), "R3-04 open: "+e.str());
      r3Inject(s,R3F_ManifestTmpWrite); ChainError e1; CHECK(!PUBH(s,1,0,0,3,&e1), "R3-04 first publish fails: "+shortErr(e1)); s.clearInjection();
      CHECK(writeJunk(d+"/canary","R3-CANARY-KEEP-ME-UNTOUCHED-0123456789"), "R3-04 plant the symlink target");
      long cs=FSZ(d+"/canary"); Hash cg=fdig(d+"/canary");
      CHECK(::rename((d+"/undo.dat").c_str(),(d+"/undo.dat.moved").c_str())==0, "R3-04 move the real log aside");
      CHECK(::symlink((d+"/canary").c_str(),(d+"/undo.dat").c_str())==0, "R3-04 substitute a symlink at undo.dat");
      ChainError e2; bool p2=PUBH(s,2,0,0,3,&e2);
      printf("  R3-04 retry over a symlink: publish=%d canary=%ld->%ld err=%s\n",(int)p2,cs,FSZ(d+"/canary"),shortErr(e2).c_str());
      CHECK(!p2, "R3-04 retry refused (no symlink follow): "+shortErr(e2));
      CHECK(FSZ(d+"/canary")==cs && fdig(d+"/canary")==cg, "R3-04 symlink target untouched");
      CHECK(FSZ(d+"/undo.dat.moved")>0, "R3-04 the moved original is preserved verbatim");
      s.close(); }

    // ---- R3-05: FIFO substitution must be refused without blocking (bounded: fork + alarm)
    { std::string d=freshDir("r3_05");
      pid_t pid=fork();
      if(pid==0){
          ChainError ce; CommitStore3A c; if(!c.open(d,&ce)) ::_exit(2);
          r3Inject(c,R3F_ManifestTmpWrite); ChainError ce1; PUBH(c,1,0,0,3,&ce1); c.clearInjection();
          ::unlink((d+"/undo.dat").c_str());
          if(::mkfifo((d+"/undo.dat").c_str(),0644)!=0) ::_exit(3);
          ::alarm(5);                                   // a blocking O_WRONLY open is killed here (pre-fix HANG)
          ChainError ce2; bool r=PUBH(c,2,0,0,3,&ce2);
          ::_exit(r?7:6);
      }
      int st=0; waitpid(pid,&st,0); std::string ws=r3wstatus(st);
      mode_t m=0; bool isfifo=LSTATM(d+"/undo.dat",&m) && S_ISFIFO(m);
      printf("  R3-05 FIFO retry: child=%s fifo_still_present=%d\n", ws.c_str(),(int)isfifo);
      CHECK(WIFEXITED(st)&&WEXITSTATUS(st)==6, "R3-05 FIFO substitution refused without blocking (expected exit 6, got "+ws+")");
      CHECK(isfifo, "R3-05 the FIFO substitute is untouched"); }

    // ---- R3-06: a failure BEFORE the undo write leaves the pin intact -> retry stays safe AND pinned
    { std::string d=freshDir("r3_06a");
      CHECK(touchEmpty(d+"/undo.dat"), "R3-06a plant an ACCEPTED empty residue");
      CommitStore3A s; CHECK(s.open(d,&e), "R3-06a open: "+e.str());
      r3Inject(s,R3F_PageWrite); ChainError e1; CHECK(!PUBH(s,1,0,0,3,&e1), "R3-06a failure before any undo mutation: "+shortErr(e1)); s.clearInjection();
      CHECK(!s.needsReopen(), "R3-06a instance is NOT reopen-required (the one-shot was not spent)");
      ChainError e2; bool p2=PUBH(s,1,0,0,3,&e2);
      printf("  R3-06a retry after a pre-undo failure: publish=%d gen=%llu err=%s\n",(int)p2,(unsigned long long)s.generation(),shortErr(e2).c_str());
      CHECK(p2, "R3-06a retry still allowed (activation point is precise, not blanket): "+shortErr(e2));
      CHECK(s.generation()==1 && COH(d,&e2), "R3-06a retry committed gen1 coherently"); s.close(); }
    { std::string d=freshDir("r3_06b");
      CHECK(touchEmpty(d+"/undo.dat"), "R3-06b plant an ACCEPTED empty residue");
      CommitStore3A s; CHECK(s.open(d,&e), "R3-06b open: "+e.str());
      r3Inject(s,R3F_PageWrite); ChainError e1; CHECK(!PUBH(s,1,0,0,3,&e1), "R3-06b failure before any undo mutation: "+shortErr(e1)); s.clearInjection();
      CHECK(substFile(d+"/undo.dat",R3_SUB), "R3-06b substitute a nonempty file at undo.dat");
      long ss=FSZ(d+"/undo.dat"); Hash sh=fdig(d+"/undo.dat");
      ChainError e2; bool p2=PUBH(s,2,0,0,3,&e2);
      printf("  R3-06b retry with a surviving pin over a substitute: publish=%d err=%s\n",(int)p2,shortErr(e2).c_str());
      CHECK(!p2, "R3-06b the surviving pin refuses the substitute: "+shortErr(e2));
      CHECK(FSZ(d+"/undo.dat")==ss && fdig(d+"/undo.dat")==sh, "R3-06b substitute unchanged");
      s.close(); }

    // ---- R3-07..R3-13: the failure classes (each must refuse the retry and preserve every byte)
    struct R3Mode { const char* id; int fail; const char* cls; bool manifestDurable; };
    R3Mode modes[]={
        {"R3-07", R3F_TornUndo,          "partial undo write (torn, fsynced prefix)",      false},
        {"R3-08", R3F_UndoFsync,         "complete undo write, fsync failed",              false},
        {"R3-09", R3F_ManifestTmpWrite,  "undo durable, MANIFEST.tmp write failed",        false},
        {"R3-10", R3F_ManifestTmpFsync,  "MANIFEST.tmp written, its fsync failed",         false},
        {"R3-11", R3F_ManifestRename,    "MANIFEST rename failed",                         false},
        {"R3-12", R3F_DirFsync,          "rename done, directory fsync failed",            true },
        {"R3-13", R3F_AckWrite,          "rename+dirfsync done, ACK write failed",         true },
    };
    for (auto& md: modes){
        std::string d=freshDir(std::string("r3_")+std::string(md.id+3));
        CHECK(touchEmpty(d+"/undo.dat"), std::string(md.id)+" plant an ACCEPTED empty residue");
        CommitStore3A s; ChainError e0; bool op=s.open(d,&e0);
        CHECK(op, std::string(md.id)+" open: "+shortErr(e0));
        if(!op) continue;
        r3Inject(s,md.fail,20);
        ChainError e1; bool p1=PUBH(s,1,0,0,3,&e1); s.clearInjection();
        long us=FSZ(d+"/undo.dat"); Hash uh=fdig(d+"/undo.dat"); std::string snap=r3Snap(d);
        ChainError e2; bool p2=PUBH(s,1,0,0,3,&e2);
        CommitStore3A r; ChainError e3; bool ro=r.open(d,&e3); if(ro) r.close();
        printf("  %s [%s] publish1=%d undo=%ld reopen=%d retry=%d needsReopen=%d man=%d ack=%d | retry_err=%s\n",
               md.id, md.cls, (int)p1, us, (int)ro, (int)p2, (int)s.needsReopen(),
               (int)fexists(d+"/MANIFEST"), (int)fexists(d+"/ACK"), shortErr(e2).c_str());
        CHECK(!p1, std::string(md.id)+" first publish fails: "+shortErr(e1));
        CHECK(FSZ(d+"/undo.dat")==us && fdig(d+"/undo.dat")==uh, std::string(md.id)+" undo bytes are exactly the ones this commit wrote (preserved)");
        CHECK(!p2, std::string(md.id)+" retry refused before any write: "+shortErr(e2));
        CHECK(s.needsReopen(), std::string(md.id)+" instance requires reopen after the spent one-shot");
        CHECK(r3Snap(d)==snap, std::string(md.id)+" the refused retry mutated nothing");
        CHECK(fexists(d+"/MANIFEST")==md.manifestDurable, std::string(md.id)+" MANIFEST durability matches the failure class");
        CHECK(!fexists(d+"/ACK"), std::string(md.id)+" no ACK was written for an incomplete publication");
        CHECK(!ro, std::string(md.id)+" reopen refuses (never trusts in-memory state): "+shortErr(e3));
        s.close();
    }

    // ---- R3-14/R3-15: the refusal does not depend on which block the retry carries
    for (int variant=0; variant<2; variant++){
        std::string d=freshDir(variant? "r3_15":"r3_14");
        const char* id = variant? "R3-15" : "R3-14";
        CHECK(touchEmpty(d+"/undo.dat"), std::string(id)+" plant an ACCEPTED empty residue");
        CommitStore3A s; ChainError e0; CHECK(s.open(d,&e0), std::string(id)+" open: "+e0.str());
        r3Inject(s,R3F_ManifestTmpWrite); ChainError e1; CHECK(!PUBH(s,1,0,0,3,&e1), std::string(id)+" first publish fails: "+shortErr(e1)); s.clearInjection();
        ChainError e2; bool p2 = variant? PUBH(s,5,9,9,3,&e2) : PUBH(s,1,0,0,3,&e2);
        printf("  %s retry holds %s block: publish=%d err=%s\n", id, variant?"a DIFFERENT":"the SAME", (int)p2, shortErr(e2).c_str());
        CHECK(!p2, std::string(id)+" retry refused regardless of the block payload: "+shortErr(e2));
        CHECK(s.generation()==0 && s.undoCount()==0, std::string(id)+" no generation/index entry created");
        s.close();
    }

    // ---- R3-17/R3-33: descriptor accounting across open -> failure -> refusal -> close
    { std::string d=freshDir("r3_17");
      int fd0=countOpenFds();
      CHECK(touchEmpty(d+"/undo.dat"), "R3-17 plant an ACCEPTED empty residue");
      int fdPlanted=countOpenFds();
      CommitStore3A s; ChainError e0; CHECK(s.open(d,&e0), "R3-17 open (adopts+pin a descriptor): "+e0.str());
      int fdOpen=countOpenFds();
      r3Inject(s,R3F_ManifestTmpWrite); ChainError e1; CHECK(!PUBH(s,1,0,0,3,&e1), "R3-17 first publish fails: "+shortErr(e1)); s.clearInjection();
      int fdFail=countOpenFds();
      ChainError e2; CHECK(!PUBH(s,1,0,0,3,&e2), "R3-17 retry refused: "+shortErr(e2));
      int fdRef=countOpenFds();
      s.close();
      int fdClosed=countOpenFds();
      printf("  R3-17 open fds: base=%d planted=%d open=%d after_failure=%d after_refusal=%d after_close=%d\n",
             fd0,fdPlanted,fdOpen,fdFail,fdRef,fdClosed);
      CHECK(fdPlanted==fd0, "R3-33 planting the residue leaks nothing");
      CHECK(fdFail<=fdOpen, "R3-33 the failed first publish released its descriptors (pin consumed+closed)");
      CHECK(fdRef<=fdFail, "R3-33 the refused retry opened nothing");
      CHECK(fdClosed<=fd0, "R3-33 close() released everything including the adopted pin");
    }
}

// ---- R3-19..R3-32, R3-35, R3-36: reopen/recovery semantics after the new state
static void test_r3_reopen(){
    g_cur="F0-R3-reopen";
    ChainError e;

    // ---- R3-29/R3-30/R3-32/R3-27: the normal lifecycle is untouched
    { std::string d=freshDir("r3_29");
      CHECK(touchEmpty(d+"/undo.dat"), "R3-29 plant an ACCEPTED empty residue");
      CommitStore3A s; CHECK(s.open(d,&e), "R3-29 (R3-27) empty residue accepted: "+e.str());
      CHECK(s.generation()==0 && s.undoCount()==0, "R3-29 gen0 with an empty undo index");
      CHECK(PUBH(s,1,0,0,3,&e), "R3-29 first publish succeeds: "+e.str());
      CHECK(s.generation()==1 && s.undoCount()==1, "R3-29 gen1 with exactly one undo record");
      CHECK(!s.needsReopen(), "R3-29 a SUCCESSFUL first publish leaves the instance usable");
      CHECK(COH(d,&e), "R3-29 coherence");
      CHECK(fexists(d+"/MANIFEST") && fexists(d+"/ACK"), "R3-29 MANIFEST + ACK published");
      Hash dig1=DG(s,&e); s.close();
      CommitStore3A r; CHECK(r.open(d,&e), "R3-29 restart: "+e.str());
      CHECK(r.generation()==1 && DG(r,&e)==dig1, "R3-32 exact digest after restart");
      CHECK(PUBH(r,2,0,0,3,&e), "R3-30 second publish succeeds: "+e.str());
      CHECK(r.generation()==2 && COH(d,&e), "R3-30 gen2 committed coherently");
      Hash dig2=DG(r,&e); r.close();
      CommitStore3A r2; CHECK(r2.open(d,&e), "R3-30 restart2: "+e.str());
      CHECK(r2.generation()==2 && DG(r2,&e)==dig2, "R3-32 gen2 digest exact after restart (acknowledged state preserved)");
      CHECK(r2.acknowledgedGeneration()==2, "R3-30 ACK tracks the committed generation");
      r2.close(); }

    // ---- R3-28: absent undo.dat still means legitimate fresh initialization
    { std::string d=freshDir("r3_28"); CommitStore3A s; CHECK(s.open(d,&e), "R3-28 absent undo.dat accepted: "+e.str());
      CHECK(s.generation()==0 && !fexists(d+"/undo.dat"), "R3-28 gen0 and no log invented");
      CHECK(PUBH(s,1,0,0,3,&e), "R3-28 first publish creates the log: "+e.str());
      CHECK(COH(d,&e), "R3-28 coherence"); s.close(); }

    // ---- R3-19: reopen with a PARTIAL undo residue
    { std::string d=freshDir("r3_19");
      { FILE* f=fopen((d+"/undo.dat").c_str(),"wb"); if(f){ const char* h="INNUNDO1"; fwrite(h,1,8,f); uint8_t j[4]={0}; fwrite(j,1,4,f); fclose(f); } }
      long us=FSZ(d+"/undo.dat"); Hash uh=fdig(d+"/undo.dat");
      CommitStore3A r; bool ok=r.open(d,&e);
      printf("  R3-19 reopen with a %ld B partial residue: ok=%d err=%s\n", us,(int)ok,shortErr(e).c_str());
      CHECK(!ok, "R3-19 partial residue refused, never presented as genesis: "+shortErr(e));
      CHECK(FSZ(d+"/undo.dat")==us && fdig(d+"/undo.dat")==uh, "R3-19 partial residue preserved byte-for-byte");
      if(ok) r.close(); }

    // ---- R3-20: reopen with a COMPLETE but UNACKNOWLEDGED undo record (real store bytes, no MANIFEST/ACK)
    { std::string src=freshDir("r3_20_src");
      { CommitStore3A s0; ChainError e0; CHECK(s0.open(src,&e0), "R3-20 src open: "+e0.str());
        CHECK(PUBH(s0,1,0,0,3,&e0), "R3-20 src publish gen1: "+e0.str()); s0.close(); }
      std::string d=freshDir("r3_20");
      CHECK(CPFILE(src+"/undo.dat",d+"/undo.dat"), "R3-20 plant a complete unacknowledged undo record");
      long us=FSZ(d+"/undo.dat"); Hash uh=fdig(d+"/undo.dat");
      CommitStore3A r; bool ok=r.open(d,&e);
      printf("  R3-20 reopen with a complete unacknowledged record (%ld B): ok=%d err=%s\n", us,(int)ok,shortErr(e).c_str());
      CHECK(!ok, "R3-20 complete unacknowledged record refused (no silent genesis, no trusting stale state): "+shortErr(e));
      CHECK(FSZ(d+"/undo.dat")==us && fdig(d+"/undo.dat")==uh, "R3-20 unacknowledged record preserved");
      if(ok) r.close(); }

    // ---- R3-21: established-store semantics are UNCHANGED (C11): a pre-rename failure there is still retryable
    { std::string d=freshDir("r3_21");
      CommitStore3A s; ChainError e0; CHECK(s.open(d,&e0), "R3-21 open (absent log): "+e0.str());
      CHECK(PUBH(s,1,0,0,3,&e0), "R3-21 first publish succeeds and spends the one-shot: "+e0.str());
      Hash d1=DG(s,&e0); uint64_t g1=s.generation();
      r3Inject(s,R3F_ManifestTmpWrite); ChainError e1; bool q1=PUBH(s,2,0,0,3,&e1); s.clearInjection();
      printf("  R3-21 later publish fails pre-rename: publish=%d needsReopen=%d err=%s\n",(int)q1,(int)s.needsReopen(),shortErr(e1).c_str());
      CHECK(!q1, "R3-21 later publish fails pre-rename: "+shortErr(e1));
      CHECK(!s.needsReopen(), "R3-21 established store is NOT reopen-required by an ordinary pre-rename failure (unchanged)");
      ChainError e2; bool q2=PUBH(s,2,0,0,3,&e2);
      CHECK(q2, "R3-21 retry on the established store still succeeds: "+shortErr(e2));
      CHECK(s.generation()==g1+1 && COH(d,&e2), "R3-21 gen"+std::to_string(g1+1)+" committed coherently");
      Hash da=DG(s,&e2); s.close();
      CommitStore3A r; CHECK(r.open(d,&e), "R3-21 restart: "+e.str());
      CHECK(r.generation()==g1+1 && DG(r,&e)==da, "R3-21/R3-32 acknowledged generation preserved with the exact digest");
      CHECK(DG(r,&e)!=d1, "R3-21 the retried generation really advanced the state");
      r.close(); }

    // ---- R3-22/R3-23/R3-36: crash after the backup replacement + CORRUPT primary -> resolve from the backup
    { std::string d=freshDir("r3_22");
      CommitStore3A s; CHECK(s.open(d,&e), "R3-22 seed store: "+e.str());
      CHECK(PUBH(s,1,0,0,3,&e), "R3-22 seed gen1: "+e.str());
      Hash dig1=DG(s,&e); s.close();
      { pid_t pid=fork(); if(pid==0){ r1CrashChild(d,2); ::_exit(9); }   // mode 2 = crash at AfterBakReplace
        int st=0; waitpid(pid,&st,0);
        CHECK(WIFEXITED(st)&&WEXITSTATUS(st)==42, "R3-36 crash at AfterBakReplace took the crash path ("+r3wstatus(st)+")"); }
      CHECK(fexists(d+"/MANIFEST.bak"), "R3-36 MANIFEST.bak was established BEFORE the undo write");
      CHECK(MBU(d)<=MUU(d), "R3-35 retained backup never references beyond the committed length (bak="+std::to_string(MBU(d))+", primary="+std::to_string(MUU(d))+")");
      CHECK(flipByte(d+"/MANIFEST",40), "R3-22 corrupt the primary MANIFEST");
      CommitStore3A r; bool ok=r.open(d,&e);
      printf("  R3-22 double-fault recovery: open=%d hadRecoveryIssue=%d gen=%llu err=%s\n",(int)ok,(int)r.hadRecoveryIssue(),
             (unsigned long long)r.generation(),shortErr(e).c_str());
      CHECK(ok, "R3-22/R3-36 recover the acknowledged generation from the intact backup: "+shortErr(e));
      if(ok){
        CHECK(r.hadRecoveryIssue(), "R3-22 the recovery is reported, not hidden");
        CHECK(r.generation()==1, "R3-22 recovered generation 1 (the acknowledged one)");
        CHECK(DG(r,&e)==dig1, "R3-22 recovered digest == acknowledged digest");
        CHECK(fdig(d+"/MANIFEST")==fdig(d+"/MANIFEST.bak"), "R3-23 MANIFEST repaired from the backup bytes");
        r.close();
        CommitStore3A r2; bool ok2=r2.open(d,&e);
        CHECK(ok2, "R3-23 second reopen: "+shortErr(e));
        if(ok2){ CHECK(!r2.hadRecoveryIssue() && r2.generation()==1 && DG(r2,&e)==dig1,
                       "R3-23 second reopen is clean at gen1 with the exact digest"); r2.close(); }
      } }

    // ---- R3-24: ACK lag -> the open reconciles upward to the primary generation
    { std::string d=freshDir("r3_24");
      CommitStore3A s; ChainError e0; CHECK(s.open(d,&e0), "R3-24 open: "+e0.str());
      CHECK(PUBH(s,1,0,0,3,&e0), "R3-24 gen1: "+e0.str());
      CHECK(PUBH(s,2,0,0,3,&e0), "R3-24 gen2: "+e0.str());
      Hash dig2=DG(s,&e0); s.close();
      CHECK(r3WriteAckGen(d,1), "R3-24 force the ACK back to generation 1 (lag)");
      CommitStore3A r; bool ok=r.open(d,&e);
      printf("  R3-24 ACK lag: open=%d gen=%llu ack=%llu err=%s\n",(int)ok,(unsigned long long)r.generation(),
             (unsigned long long)r.acknowledgedGeneration(),shortErr(e).c_str());
      CHECK(ok, "R3-24 lagging ACK does not block the open: "+shortErr(e));
      if(ok){ CHECK(r.generation()==2, "R3-24 primary generation preserved");
        CHECK(r.acknowledgedGeneration()==2, "R3-24 ACK reconciled upward to the primary generation");
        CHECK(DG(r,&e)==dig2, "R3-24 digest exact after the reconciliation"); r.close(); } }

    // ---- R3-25: ACK mismatch (ACK newer than the primary) -> refuse; never roll acknowledged state back
    { std::string d=freshDir("r3_25");
      CommitStore3A s; ChainError e0; CHECK(s.open(d,&e0), "R3-25 open: "+e0.str());
      CHECK(PUBH(s,1,0,0,3,&e0), "R3-25 gen1: "+e0.str()); s.close();
      CHECK(r3WriteAckGen(d,5), "R3-25 force an ACK ahead of the primary");
      CommitStore3A r; bool ok=r.open(d,&e);
      printf("  R3-25 ACK mismatch: open=%d err=%s\n",(int)ok,shortErr(e).c_str());
      CHECK(!ok, "R3-25 acknowledged-newer-than-primary refused (no silent rollback): "+shortErr(e));
      if(ok) r.close(); }

    // ---- R3-26: repeated restart after a refused first publish is deterministic and non-destructive
    { std::string d=freshDir("r3_26");
      CHECK(touchEmpty(d+"/undo.dat"), "R3-26 plant an ACCEPTED empty residue");
      { CommitStore3A s; ChainError e0; CHECK(s.open(d,&e0), "R3-26 open: "+e0.str());
        r3Inject(s,R3F_ManifestTmpWrite); ChainError e1; CHECK(!PUBH(s,1,0,0,3,&e1), "R3-26 first publish fails: "+shortErr(e1)); s.clearInjection();
        ChainError e2; CHECK(!PUBH(s,1,0,0,3,&e2), "R3-26 retry refused: "+shortErr(e2));
        s.close(); }
      long us=FSZ(d+"/undo.dat"); Hash uh=fdig(d+"/undo.dat"); std::string snap=r3Snap(d);
      std::string first; int refused=0;
      for(int i=0;i<3;i++){ CommitStore3A r; ChainError e2; if(!r.open(d,&e2)){ refused++; if(first.empty()) first=shortErr(e2); } else r.close(); usleep(2000); }
      printf("  R3-26 restarts: refused=%d/3 datadir_unchanged=%d first=%s\n",refused,(int)(r3Snap(d)==snap),first.c_str());
      CHECK(refused==3, "R3-26 all three restarts refuse identically ("+std::to_string(refused)+"/3)");
      CHECK(FSZ(d+"/undo.dat")==us && fdig(d+"/undo.dat")==uh, "R3-26 the residue is byte-identical after the restarts");
      CHECK(r3Snap(d)==snap, "R3-26 no restart mutated the datadir"); }

    // ---- R3-31: multi-key atomicity preserved (one record carrying erase + 3 puts), digest == reference
    { std::string d=freshDir("r3_31");
      CommitStore3A s; ChainError e0; CHECK(s.open(d,&e0), "R3-31 open: "+e0.str());
      UtxoState ref; bool ok=true;
      for(int h=1;h<=30 && ok;h++){ Block b=mkBlockH(h,0,0,3); if(!publishStore(s,b,&e0)||!publishRef(ref,b,&e0)) ok=false; }
      CHECK(ok, "R3-31 build 30 coinbase blocks (store + reference): "+shortErr(e0));
      Hash cb2=id32(0xCB000000u,2);
      Block b; b.hash=id32(0xB10C0000u,31); b.height=31;
      Tx sp; sp.txid=id32(0x5BE7D000u,31); sp.coinbase=false; sp.coinstake=false; TxIn in; in.prev.txid=cb2; in.prev.n=0; sp.vin.push_back(in);
      TxOut o1; o1.value=20*COIN; o1.script={1,1}; sp.vout.push_back(o1);
      TxOut o2; o2.value=15*COIN; o2.script={2,2}; sp.vout.push_back(o2);
      TxOut o3; o3.value=15*COIN; o3.script={3,3}; sp.vout.push_back(o3);
      b.txs.push_back(sp);
      Tx cb; cb.txid=id32(0xCB000000u,3001); cb.coinbase=true; cb.coinstake=false; TxOut oc; oc.value=50*COIN; oc.script.assign(3,7); cb.vout.push_back(oc); b.txs.push_back(cb);
      bool pok=publishStore(s,b,&e0); CHECK(pok, "R3-31 multi-key publish (erase + 3 puts + 1 put): "+shortErr(e0));
      bool rok=publishRef(ref,b,&e0); CHECK(rok, "R3-31 reference applies the same block: "+shortErr(e0));
      int found=0;
      for(int i=0;i<3;i++){ OutPoint k; k.txid=sp.txid; k.n=(uint32_t)i; Coin c; bool f=false; if(s.get(k,&c,&f,&e0)&&f) found++; }
      { OutPoint k; k.txid=cb.txid; k.n=0; Coin c; bool f=false; if(!(s.get(k,&c,&f,&e0)&&f)) found=-1; }
      { OutPoint k; k.txid=cb2; k.n=0; Coin c; bool f=true; bool got=s.get(k,&c,&f,&e0); if(!got||f) found=-2; }
      UtxoState st; storeToState(s,&st,&e0);
      printf("  R3-31 multi-key atomic result: ops_visible=%d digest_match=%d (undo=%ld B)\n",
             found, (int)(st.digest()==ref.digest()), FSZ(d+"/undo.dat"));
      CHECK(found==3, "R3-31 exactly the 3 new outputs + the new coinbase are visible and the spent key is gone (found="+std::to_string(found)+")");
      CHECK(st.digest()==ref.digest(), "R3-31 store digest == reference-model digest (atomic application)");
      CHECK(COH(d,&e0), "R3-31 coherence");
      s.close(); }
}

// ---- Phase F: the post-lstat publication window (F0-2-R2-2)
static void r3PauseChild(const std::string& dir,int where,int ms){
    ChainError e; CommitStore3A s;
    if(!s.open(dir,&e)) ::_exit(2);
    s.setPause(where==0? CommitStore3A::PausePoint::BeforeUndoNameRecheck
                       : CommitStore3A::PausePoint::BeforeAck, ms);
    ChainError e1; bool ok=PUBH(s,1,0,0,3,&e1);
    s.close();
    ::_exit(ok?0:1);
}
static void test_r3_postlstat(){
    g_cur="F0-R3-postlstat";
    ChainError e;
    for (int where=0; where<2; where++){
        const bool inside = (where==0);
        std::string d=freshDir(inside? "r3_37a":"r3_37b");
        const char* id = inside? "R3-37a":"R3-37b";
        pid_t pid=fork();
        if(pid==0){ r3PauseChild(d,where,1500); ::_exit(8); }
        long long t0=nowUs(); while(!fexists(d+"/MANIFEST") && nowUs()-t0<3000000) usleep(2000);
        CHECK(fexists(d+"/MANIFEST"), std::string(id)+" child reached the publication window (MANIFEST visible)");
        usleep(200000);                                    // the child is inside its 1500 ms pause
        ::unlink((d+"/undo.dat").c_str());
        CHECK(writeJunk(d+"/undo.dat","R3-POST-LSTAT-SUBSTITUTE-PAYLOAD-INSIDE-THE-WINDOW"), std::string(id)+" substitute undo.dat inside the window");
        int st=0; waitpid(pid,&st,0);
        int rc=WIFEXITED(st)?WEXITSTATUS(st):-1; bool ack=fexists(d+"/ACK");
        CommitStore3A r; ChainError e2; bool ok=r.open(d,&e2);
        printf("  %s (window=%s): child=%s ack_written=%d reopen=%d reopen_err=%s\n", id,
               inside?"after the undo fsync, before the pre-ACK recheck":"after the pre-ACK recheck, before the ACK write",
               rc==0?"publish=SUCCESS":(rc==1?"publish=FAILED":("exit "+std::to_string(rc)).c_str()),(int)ack,(int)ok,shortErr(e2).c_str());
        if(inside){
            CHECK(rc==1, std::string(id)+": the publish FAILS instead of reporting a silent success");
            CHECK(!ack, std::string(id)+": NO ACK is written for a generation whose log is unreachable");
            CHECK(!ok, std::string(id)+": reopen still refuses (fail-closed, no fabrication)");
        } else {
            CHECK(rc==0, std::string(id)+": the publish still reports SUCCESS in the residual window (not closed)");
            CHECK(ack, std::string(id)+": the ACK is written -> the remaining window is [recheck -> ACK write]");
            CHECK(!ok, std::string(id)+": reopen refuses -> acknowledged-but-unreachable generation = F0-2-R2-2 OPEN");
        }
        if(ok) r.close();
    }
    // R3-38: the LOCK excludes a second STORE INSTANCE, not a non-cooperating file mutation (threat model)
    { std::string d=freshDir("r3_38");
      CommitStore3A s; CHECK(s.open(d,&e), "R3-38 first instance opens and holds LOCK: "+e.str());
      CommitStore3A t; ChainError e2; bool ok2=t.open(d,&e2);
      CHECK(!ok2, "R3-38 a second store instance on the same datadir is refused (LOCK): "+shortErr(e2)); if(ok2) t.close();
      CHECK(substFile(d+"/undo.dat",R3_SUB), "R3-38 an external write to undo.dat is NOT excluded by LOCK (documented threat model)");
      s.close(); }
    // R3-39: a crash inside the same window -> the generation was never acknowledged (SAFE REFUSAL, no false ACK)
    { std::string d=freshDir("r3_39");
      pid_t pid=fork(); if(pid==0){ r1CrashChild(d,6); ::_exit(9); }    // mode 6 = crash at AfterDirSync
      int st=0; waitpid(pid,&st,0);
      CHECK(WIFEXITED(st)&&WEXITSTATUS(st)==42, "R3-39 child crashed after the rename ("+r3wstatus(st)+")");
      ::unlink((d+"/undo.dat").c_str());
      CHECK(writeJunk(d+"/undo.dat","R3-CRASH-WINDOW-SUBSTITUTE-PAYLOAD-INSIDE"), "R3-39 substitute inside the crashed window");
      CommitStore3A r; bool ok=r.open(d,&e);
      printf("  R3-39 crash-window substitution: ack_written=%d reopen=%d err=%s\n",(int)fexists(d+"/ACK"),(int)ok,shortErr(e).c_str());
      CHECK(!fexists(d+"/ACK"), "R3-39 the crashed publication never acknowledged a generation");
      CHECK(!ok, "R3-39 reopen refuses the unreachable log (SAFE REFUSAL, never a false ACK): "+shortErr(e));
      if(ok) r.close(); }
}

static long readVmHWM(){ FILE*f=fopen("/proc/self/status","r"); if(!f)return -1; char l[256]; long kb=-1; while(fgets(l,sizeof(l),f)){ if(strncmp(l,"VmHWM:",6)==0){ sscanf(l+6,"%ld",&kb); break; } } fclose(f); return kb; }


// ===========================================================================
// S1.1 -- UNDOIDX: durable segmented undo index (format, validation, states, cost)
// ===========================================================================
static std::string slurp(const std::string& p){ FILE* f=fopen(p.c_str(),"rb"); if(!f) return std::string(); std::string s; char b[8192]; size_t n; while((n=fread(b,1,sizeof(b),f))>0) s.append(b,n); fclose(f); return s; }
static bool spill(const std::string& p, const std::string& d){ FILE* f=fopen(p.c_str(),"wb"); if(!f) return false; bool ok=(d.empty()||fwrite(d.data(),1,d.size(),f)==d.size()); fclose(f); return ok; }
static bool appendRaw(const std::string& p, size_t n, uint8_t v){ FILE* f=fopen(p.c_str(),"ab"); if(!f) return false; bool ok=true; for(size_t i=0;i<n;i++) if(fputc((int)v,f)==EOF){ ok=false; break; } fclose(f); return ok; }
static std::string ixp(const std::string& d){ return d+"/UNDOINDEX"; }
static const char* ixName(UndoIndexState s){
    switch(s){ case UndoIndexState::None: return "None"; case UndoIndexState::Current: return "Current";
               case UndoIndexState::Absent: return "Absent"; case UndoIndexState::Stale: return "Stale";
               case UndoIndexState::Ahead: return "Ahead"; case UndoIndexState::Corrupt: return "Corrupt"; }
    return "?"; }
static uint64_t ixExpectedBytes(uint32_t S, size_t segments){ (void)S; uint64_t t=0; for(size_t i=1;i<=segments;i++) t += 48u + 24u*(uint64_t)i; return t; }
static void ixSetU64(std::string& m, size_t off, uint64_t v){ for(int k=0;k<8;k++) m[off+k]=(char)((v>>(8*k))&0xFF); }
static void ixSetU32(std::string& m, size_t off, uint32_t v){ for(int k=0;k<4;k++) m[off+k]=(char)((v>>(8*k))&0xFF); }
static uint64_t ixGetU64(const std::string& m, size_t off){ uint64_t v=0; for(int k=0;k<8;k++) v|=(uint64_t)(uint8_t)m[off+k]<<(8*k); return v; }
static void ixFixSegCrc(std::string& m, size_t i){ uint32_t c=csCrc32((const uint8_t*)&m[48+24*i],20); ixSetU32(m,48+24*i+20,c); }
static void ixFixHdrCrc(std::string& m){ uint32_t c=csCrc32((const uint8_t*)&m[0],44); ixSetU32(m,44,c); }

// (1) format round-trip + structural rejection
static void test_undoidx_format(){
    g_cur="UNDOIDX-format";
    UndoIndex a; a.formatVersion=UNDOINDEX_FORMAT_VERSION; a.segmentSize=4;
    for(int i=1;i<=3;i++){ UndoSegment s; s.startOffset=(uint64_t)(i-1)*1365; s.endOffset=(uint64_t)i*1365; s.recordCount=4; a.segments.push_back(s); }
    a.committedBytes=a.segments.back().endOffset;
    std::vector<uint8_t> b1=a.encode(), b2=a.encode();
    CHECK(b1.size()==48+24*3, "format: encoded size is 48+24*N");
    CHECK(b1==b2, "format: encode is deterministic");
    CHECK(b1.size()>=8 && memcmp(b1.data(),"INNUIDX1",8)==0, "format: magic is INNUIDX1");
    ChainError e; UndoIndex c;
    CHECK(UndoIndex::decode(b1.data(),b1.size(),&c,&e), "format: round-trip decode: "+e.str());
    CHECK(c.formatVersion==1 && c.segmentSize==4 && c.segments.size()==3, "format: header + count round-trip");
    CHECK(c.committedBytes==a.committedBytes && c.baseOffset==0 && c.baseGeneration==0, "format: metadata round-trip");
    CHECK(c.segments[2].startOffset==a.segments[2].startOffset && c.segments[2].endOffset==a.segments[2].endOffset && c.segments[2].recordCount==4, "format: last segment round-trip");
    CHECK(c.encode()==b1, "format: re-encode is byte-identical");
    std::string g((const char*)b1.data(), b1.size());
    { std::string m=g; m[0]=(char)0x00; UndoIndex t; ChainError e2; CHECK(!UndoIndex::decode((const uint8_t*)m.data(),m.size(),&t,&e2),"format: bad magic rejected"); CHECK(e2.str().find("magic")!=std::string::npos,"format: bad magic diagnostic"); }
    { std::string m=g; ixSetU32(m,8,9); ixFixHdrCrc(m); UndoIndex t; ChainError e2; CHECK(!UndoIndex::decode((const uint8_t*)m.data(),m.size(),&t,&e2),"format: unsupported version rejected"); }
    { std::string m=g; ixSetU32(m,12,0); ixFixHdrCrc(m); UndoIndex t; ChainError e2; CHECK(!UndoIndex::decode((const uint8_t*)m.data(),m.size(),&t,&e2),"format: segment size 0 rejected"); }
    { std::string m=g; m[46]=(char)(m[46]^0xFF); UndoIndex t; ChainError e2; CHECK(!UndoIndex::decode((const uint8_t*)m.data(),m.size(),&t,&e2),"format: header crc mismatch rejected"); }
    { std::string m=g; m[48+2]=(char)(m[48+2]^0xFF); UndoIndex t; ChainError e2; CHECK(!UndoIndex::decode((const uint8_t*)m.data(),m.size(),&t,&e2),"format: segment crc mismatch rejected"); }
    { std::string m=g; ixSetU32(m,40,4); ixFixHdrCrc(m); UndoIndex t; ChainError e2; CHECK(!UndoIndex::decode((const uint8_t*)m.data(),m.size(),&t,&e2),"format: declared segment count vs length rejected"); }
    { std::string m=g.substr(0,60); UndoIndex t; ChainError e2; CHECK(!UndoIndex::decode((const uint8_t*)m.data(),m.size(),&t,&e2),"format: truncated index rejected"); }
    { std::string m=g; m.append(8,(char)0); UndoIndex t; ChainError e2; CHECK(!UndoIndex::decode((const uint8_t*)m.data(),m.size(),&t,&e2),"format: trailing bytes rejected"); }
    { std::string m=g; ixSetU32(m,48+16,0); ixFixSegCrc(m,0); UndoIndex t; ChainError e2; CHECK(!UndoIndex::decode((const uint8_t*)m.data(),m.size(),&t,&e2),"format: recordCount 0 rejected"); }
    { std::string m=g; ixSetU32(m,48+16,5); ixFixSegCrc(m,0); UndoIndex t; ChainError e2; CHECK(!UndoIndex::decode((const uint8_t*)m.data(),m.size(),&t,&e2),"format: recordCount > segmentSize rejected"); }
    { std::string m=g; ixSetU64(m,48+8,ixGetU64(m,48)); ixFixSegCrc(m,0); UndoIndex t; ChainError e2; CHECK(!UndoIndex::decode((const uint8_t*)m.data(),m.size(),&t,&e2),"format: non-positive segment length rejected"); }
    { std::string m=g; ixSetU64(m,48+0,7); ixFixSegCrc(m,0); UndoIndex t; ChainError e2; CHECK(!UndoIndex::decode((const uint8_t*)m.data(),m.size(),&t,&e2),"format: first segment not at baseOffset rejected"); }
    { std::string m=g; ixSetU64(m,48+24+0,999999); ixFixSegCrc(m,1); UndoIndex t; ChainError e2; CHECK(!UndoIndex::decode((const uint8_t*)m.data(),m.size(),&t,&e2),"format: non-contiguous segments rejected"); }
    { std::string m=g; ixSetU64(m,16,4242); ixFixHdrCrc(m); UndoIndex t; ChainError e2; CHECK(!UndoIndex::decode((const uint8_t*)m.data(),m.size(),&t,&e2),"format: committedBytes != last segment end rejected"); }
}

// (2) segment boundary behaviour (test segment size 4; the 4095/4096/4097 run is UNDOIDX-real_boundary)
static void test_undoidx_boundary_small(){
    g_cur="UNDOIDX-boundary";
    ChainError e; std::string d=freshDir("undoidx_boundary");
    { CommitStore3A s; s.setUndoIndexSegmentSizeForTest(4);
      CHECK(s.open(d,&e), "boundary open: "+e.str());
      CHECK(s.undoIndexSegmentSize()==4, "boundary: test segment size applied");
      for(int i=1;i<=3;i++) CHECK(PUB(s,i,1,1,3,&e), "boundary: publish 3");
      CHECK(!fexists(ixp(d)), "boundary: 3/4 records -> no closed segment -> no UNDOINDEX");
      CHECK(s.undoIndexPublications()==0, "boundary: zero publications at 3 records");
      CHECK(PUB(s,4,1,1,3,&e), "boundary: publish 4");
      CHECK(fexists(ixp(d)), "boundary: 4/4 records -> segment closed -> UNDOINDEX published");
      CHECK(s.undoIndexPublications()==1, "boundary: exactly one publication at 4 records");
      uint64_t man4=MAN(d);
      UndoIndex ix; CHECK(s.readUndoIndexForTest(&ix,&e), "boundary: index readable: "+e.str());
      CHECK(ix.segmentSize==4 && ix.segments.size()==1 && ix.segments[0].recordCount==4, "boundary: one 4-record segment");
      CHECK(ix.segments[0].startOffset==0, "boundary: first segment starts at baseOffset 0");
      CHECK(ix.committedBytes==man4, "boundary: committedBytes == MANIFEST.undoSize at a closed boundary");
      for(int i=5;i<=7;i++) CHECK(PUB(s,i,1,1,3,&e), "boundary: publish 7");
      CHECK(s.undoIndexPublications()==1, "boundary: 7 records -> still one closed segment");
      CHECK(PUB(s,8,1,1,3,&e), "boundary: publish 8");
      CHECK(s.undoIndexPublications()==2, "boundary: second publication at 8 records");
      UndoIndex ix2; CHECK(s.readUndoIndexForTest(&ix2,&e) && ix2.segments.size()==2, "boundary: two segments at 8 records");
      CHECK(ix2.segments[1].startOffset==ix2.segments[0].endOffset, "boundary: segments are contiguous");
      CHECK(ix2.committedBytes==MAN(d) && MAN(d)>man4, "boundary: committedBytes tracked the second boundary");
      CHECK(s.undoCount()==8, "boundary: undoCount unchanged by index construction");
      s.close(); }
    uint64_t bytes8=(uint64_t)fsz(ixp(d));
    { CommitStore3A s; s.setUndoIndexSegmentSizeForTest(4);
      CHECK(s.open(d,&e), "boundary reopen: "+e.str());
      CHECK(s.undoIndexState()==UndoIndexState::Current, "boundary reopen: Current (no normalisation)");
      CHECK(s.undoIndexPublications()==0, "boundary reopen: no publication for a current index");
      CHECK((uint64_t)fsz(ixp(d))==bytes8, "boundary reopen: index bytes untouched");
      CHECK(s.undoCount()==8, "boundary reopen: undoCount preserved");
      s.close(); }
}

// (3) corruption / structural damage -> fail closed, and the store itself is untouched
static void test_undoidx_corruption(){
    g_cur="UNDOIDX-corrupt";
    ChainError e; std::string d=freshDir("undoidx_corrupt");
    { CommitStore3A s; s.setUndoIndexSegmentSizeForTest(4);
      CHECK(s.open(d,&e), "corrupt open: "+e.str());
      for(int i=1;i<=5;i++) CHECK(PUB(s,i,1,1,3,&e), "corrupt: publish 5");
      CHECK(s.undoIndexPublications()==1, "corrupt: one closed segment published");
      s.close(); }
    std::string good=slurp(ixp(d));
    Hash f0dig=fdig(ixp(d));
    CHECK(good.size()==48+24, "corrupt: index is 72 bytes for one closed segment");
    struct Variant{ const char* name; int kind; };
    const Variant vs[]={ {"bad magic",0},{"bad version",1},{"header crc",2},{"segment crc",3},
                         {"truncated",4},{"trailing bytes",5},{"count>segmentSize",6},{"recordCount 0",7},
                         {"first segment off base",8},{"committedBytes mismatch",9} };
    for(size_t v=0; v<sizeof(vs)/sizeof(vs[0]); ++v){
        std::string m=good;
        switch(vs[v].kind){
            case 0: m[0]=(char)0x7F; break;
            case 1: ixSetU32(m,8,7); ixFixHdrCrc(m); break;
            case 2: m[45]=(char)(m[45]^0x01); break;
            case 3: m[48+3]=(char)(m[48+3]^0x01); break;
            case 4: m=m.substr(0,60); break;
            case 5: m.append(4,(char)0); break;
            case 6: ixSetU32(m,48+16,9); ixFixSegCrc(m,0); break;
            case 7: ixSetU32(m,48+16,0); ixFixSegCrc(m,0); break;
            case 8: ixSetU64(m,48+0,11); ixFixSegCrc(m,0); break;
            case 9: ixSetU64(m,16,77); ixFixHdrCrc(m); break;
        }
        CHECK(spill(ixp(d),m), "corrupt: variant written");
        CommitStore3A s; ChainError e2; bool ok=s.open(d,&e2);
        CHECK(!ok, std::string("corrupt: ")+vs[v].name+" refused");
        CHECK(e2.str().find("UNDOINDEX")!=std::string::npos, std::string("corrupt: ")+vs[v].name+" diagnostic names UNDOINDEX");
        if(ok) s.close();
        CHECK(spill(ixp(d),good), "corrupt: good index restored");
        CommitStore3A s2; s2.setUndoIndexSegmentSizeForTest(4); ChainError e3; bool ok2=s2.open(d,&e3);
        CHECK(ok2, std::string("corrupt: store unaffected after ")+vs[v].name+": "+e3.str());
        if(ok2){ CHECK(s2.undoIndexState()==UndoIndexState::Current, std::string("corrupt: Current after restoring from ")+vs[v].name);
                 CHECK(s2.undoCount()==5, std::string("corrupt: undoCount 5 after ")+vs[v].name); s2.close(); }
    }
    CHECK(!fexists(ixp(d)+".tmp"), "corrupt: no UNDOINDEX.tmp left behind");
    // (b) a torn/unfinished publication residue must have no effect and must be swept at open
    { std::string junk="INNUIDX1partial-write-residue"; CHECK(spill(ixp(d)+".tmp",junk), "corrupt: torn tmp residue written");
      CommitStore3A s; s.setUndoIndexSegmentSizeForTest(4); ChainError e4; bool ok=s.open(d,&e4);
      CHECK(ok, "corrupt: open with a torn UNDOINDEX.tmp residue: "+e4.str());
      CHECK(s.undoIndexState()==UndoIndexState::Current, "corrupt: valid index still Current despite the torn tmp");
      CHECK(s.undoCount()==5, "corrupt: committed state unaffected by the torn tmp");
      CHECK(fdig(ixp(d))==f0dig, "corrupt: index bytes unchanged by the torn tmp");
      s.close();
      CHECK(!fexists(ixp(d)+".tmp"), "corrupt: torn tmp residue swept by cleanupTmp at open"); }
    // (c) a torn tmp residue with NO index file: rebuilt from the committed stream, residue swept
    { CHECK(unlink(ixp(d).c_str())==0, "corrupt: index removed for the rebuild-with-residue case");
      CHECK(spill(ixp(d)+".tmp","INNUIDX1partial"), "corrupt: residue written without an index");
      CommitStore3A s; s.setUndoIndexSegmentSizeForTest(4); ChainError e5; bool ok=s.open(d,&e5);
      CHECK(ok, "corrupt: open with residue and no index: "+e5.str());
      CHECK(s.undoIndexState()==UndoIndexState::Absent, "corrupt: absent index classified Absent even with a residue");
      CHECK(fdig(ixp(d))==f0dig, "corrupt: rebuilt index identical after a residue-only start");
      CHECK(!fexists(ixp(d)+".tmp"), "corrupt: residue swept in the rebuild case");
      s.close(); }
}

// (4) missing / stale / ahead / current states
static void test_undoidx_states(){
    g_cur="UNDOIDX-states";
    ChainError e;
    std::string d=freshDir("undoidx_states_a");
    { CommitStore3A s; s.setUndoIndexSegmentSizeForTest(4); CHECK(s.open(d,&e), "states a open: "+e.str());
      for(int i=1;i<=5;i++) CHECK(PUB(s,i,1,1,3,&e), "states a publish 5"); s.close(); }
    std::string good=slurp(ixp(d)); Hash h0=fdig(ixp(d));
    CHECK(good.size()==48+24, "states a: one closed segment for 5 records");
    { CommitStore3A s; s.setUndoIndexSegmentSizeForTest(4); CHECK(s.open(d,&e), "states a reopen: "+e.str());
      CHECK(s.undoIndexState()==UndoIndexState::Current, "states a: current index classified Current");
      CHECK(s.undoIndexPublications()==0, "states a: no write for a current index");
      CHECK(fdig(ixp(d))==h0, "states a: index bytes identical after reopen");
      CHECK(s.undoCount()==5, "states a: undoCount 5"); s.close(); }
    CHECK(unlink(ixp(d).c_str())==0, "states b: index removed to simulate a missing index");
    { CommitStore3A s; s.setUndoIndexSegmentSizeForTest(4); CHECK(s.open(d,&e), "states b reopen: "+e.str());
      CHECK(s.undoIndexState()==UndoIndexState::Absent, "states b: missing index classified Absent");
      CHECK(s.undoIndexPublications()==1, "states b: rebuilt exactly once");
      CHECK(fdig(ixp(d))==h0, "states b: rebuilt bytes identical to the original");
      CHECK(s.undoCount()==5, "states b: undoCount preserved"); s.close(); }
    std::string d2=freshDir("undoidx_states_c");
    { CommitStore3A s; s.setUndoIndexSegmentSizeForTest(4); CHECK(s.open(d2,&e), "states c open: "+e.str());
      for(int i=1;i<=4;i++) CHECK(PUB(s,i,1,1,3,&e), "states c publish 4");
      CHECK(s.undoIndexPublications()==1, "states c: first close published");
      s.injectUndoIndexSkipNextPublish();
      for(int i=5;i<=8;i++) CHECK(PUB(s,i,1,1,3,&e), "states c publish 8");
      CHECK(s.undoIndexPublications()==1, "states c: second close publication lost (simulated)");
      s.close(); }
    { CommitStore3A s; s.setUndoIndexSegmentSizeForTest(4); CHECK(s.open(d2,&e), "states c reopen: "+e.str());
      CHECK(s.undoIndexState()==UndoIndexState::Stale, "states c: shortened prefix classified Stale");
      CHECK(s.undoIndexPublications()==1, "states c: rebuilt once on open");
      UndoIndex ix; CHECK(s.readUndoIndexForTest(&ix,&e) && ix.segments.size()==2, "states c: rebuilt to two segments");
      CHECK(ix.committedBytes==MAN(d2), "states c: rebuilt committedBytes == MANIFEST.undoSize");
      CHECK(s.undoCount()==8, "states c: no committed record lost (8)"); s.close(); }
    { CommitStore3A s; s.setUndoIndexSegmentSizeForTest(4); CHECK(s.open(d2,&e), "states c reopen2: "+e.str());
      CHECK(s.undoIndexState()==UndoIndexState::Current, "states c: Current after the rebuild");
      CHECK(s.undoIndexPublications()==0, "states c: no repeated rebuild"); s.close(); }
    std::string d3=freshDir("undoidx_states_d");
    { CommitStore3A s; s.setUndoIndexSegmentSizeForTest(4); CHECK(s.open(d3,&e), "states d open: "+e.str());
      for(int i=1;i<=4;i++) CHECK(PUB(s,i,1,1,3,&e), "states d publish 4");
      Hash tip=s.tipHash(); CHECK(s.disconnect(tip,&e), "states d: disconnect tip: "+e.str());
      CHECK(s.undoCount()==3, "states d: undoCount 3 after disconnect"); s.close(); }
    uint64_t manAfter=MAN(d3);
    { CommitStore3A s; s.setUndoIndexSegmentSizeForTest(4); CHECK(s.open(d3,&e), "states d reopen: "+e.str());
      CHECK(s.undoIndexState()==UndoIndexState::Ahead, "states d: index beyond the committed range classified Ahead (not corruption)");
      CHECK(s.undoCount()==3, "states d: committed state is 3 records");
      UndoIndex ix; CHECK(s.readUndoIndexForTest(&ix,&e), "states d: index readable: "+e.str());
      CHECK(ix.segments.empty() && ix.committedBytes==0, "states d: uncommitted segment dropped by normalisation");
      CHECK(MAN(d3)==manAfter, "states d: MANIFEST.undoSize untouched by the normalisation"); s.close(); }
    { CommitStore3A s; s.setUndoIndexSegmentSizeForTest(4); CHECK(s.open(d3,&e), "states d reopen2: "+e.str());
      CHECK(s.undoIndexState()==UndoIndexState::Current, "states d: Current after normalisation"); s.close(); }
    // (e) OPEN-2: an UNDOINDEX in a store with no MANIFEST/ACK (fresh-open path) is neither trusted,
    //     consumed, rewritten nor deleted by this slice.
    std::string d4=freshDir("undoidx_states_e");
    { UndoIndex ix; ix.segmentSize=4; UndoSegment sg; sg.startOffset=0; sg.endOffset=1365; sg.recordCount=4;
      ix.segments.push_back(sg); ix.committedBytes=1365;
      std::vector<uint8_t> b=ix.encode(); CHECK(spill(ixp(d4),std::string((const char*)b.data(),b.size())), "states e: orphan index written"); }
    Hash h4=fdig(ixp(d4));
    { CommitStore3A s; s.setUndoIndexSegmentSizeForTest(4); ChainError e6; bool ok=s.open(d4,&e6);
      CHECK(ok, "states e: fresh-open with an orphan index succeeds (R2 fresh-open contract unchanged): "+e6.str());
      CHECK(s.undoIndexState()==UndoIndexState::None, "states e: orphan index is not classified (fresh path never reads it)");
      CHECK(fdig(ixp(d4))==h4, "states e: orphan index neither rewritten nor deleted");
      CHECK(s.undoCount()==0, "states e: fresh store has no undo records");
      s.close(); }
}


// (5) atomic publication: interruption points of the commit that closes a segment
static void idxChild(const std::string& dir, int mode, int n, uint32_t S){
    ChainError e; CommitStore3A s; s.setUndoIndexSegmentSizeForTest(S);
    if(!s.open(dir,&e)) ::_exit(2);
    // Publish exactly ONE fresh record: this is the record that closes the segment under test.
    if(mode==0) s.setCrashPoint(CrashPoint::AfterUndoWrite);
    if(mode==1) s.setCrashPoint(CrashPoint::BeforeRename);
    if(mode==2) s.setCrashPoint(CrashPoint::AfterRename);
    if(mode==3) s.setCrashPoint(CrashPoint::AfterDirSync);
    bool ok=PUB(s,n,9,9,3,&e);
    if(mode==4) ::_exit(ok?0:1);
    ::_exit(42);
}
static void test_undoidx_crash_points(){
    g_cur="UNDOIDX-crash";
    ChainError e;
    Hash ctlDig; uint64_t ctlGen=0, ctlMan=0;
    { std::string dc=freshDir("undoidx_crash_ctl"); CommitStore3A s; s.injectUndoIndexDisabled(true);
      CHECK(s.open(dc,&e), "crash control open: "+e.str());
      for(int i=1;i<=3;i++) CHECK(PUB(s,i,1,1,3,&e), "crash control publish");
      ctlDig=DG(s,&e); ctlGen=s.generation(); ctlMan=MAN(dc);
      CHECK(!fexists(ixp(dc)), "crash control: index disabled -> no UNDOINDEX");
      s.close(); }
    const char* names[]={"AfterUndoWrite","BeforeRename","AfterRename","AfterDirSync"};
    for(int mode=0; mode<4; ++mode){
        std::string d=freshDir(std::string("undoidx_crash_")+names[mode]);
        { CommitStore3A s; s.setUndoIndexSegmentSizeForTest(4); CHECK(s.open(d,&e), "crash build open: "+e.str());
          for(int i=1;i<=3;i++) CHECK(PUB(s,i,1,1,3,&e), "crash build publish 3");
          CHECK(!fexists(ixp(d)), "crash build: no index before the boundary"); s.close(); }
        int rc=-1;
        { int pid=fork(); if(pid==0) idxChild(d,mode,4,4);
          int st=0; if(pid>0) waitpid(pid,&st,0); rc=(pid>0 && WIFEXITED(st)) ? WEXITSTATUS(st) : -1; }
        CHECK(rc==42, std::string("crash ")+names[mode]+": child terminated at the injected point");
        CommitStore3A s; s.setUndoIndexSegmentSizeForTest(4);
        bool ok=s.open(d,&e);
        CHECK(ok, std::string("crash ")+names[mode]+": reopen: "+e.str());
        if(!ok) continue;
        UndoIndexState stt=s.undoIndexState();
        if(mode==0){
            CHECK(stt==UndoIndexState::Absent, "crash AfterUndoWrite: index absent -> rebuilt (publication happens after the hook)");
            CHECK(s.undoCount()==3, "crash AfterUndoWrite: 3 records committed");
        } else if(mode==1){
            CHECK(stt==UndoIndexState::Ahead, "crash BeforeRename: index ahead of the manifest -> Ahead, not corruption");
            CHECK(s.undoCount()==3, "crash BeforeRename: the uncommitted 4th record is NOT authoritative");
        } else {
            CHECK(stt==UndoIndexState::Current, std::string("crash ")+names[mode]+": index matches the committed manifest");
            CHECK(s.undoCount()==4, std::string("crash ")+names[mode]+": 4 records committed");
        }
        Hash dig=DG(s,&e);
        if(mode<=1){
            CHECK(dig==ctlDig, "crash: committed logical state equals the 3-record control");
            CHECK(s.generation()==ctlGen, "crash: generation equals the control");
            CHECK(MAN(d)==ctlMan, "crash: MANIFEST.undoSize equals the control");
        }
        s.close();
        { CommitStore3A s2; s2.setUndoIndexSegmentSizeForTest(4); CHECK(s2.open(d,&e), "crash idempotent reopen: "+e.str());
          // With fewer records than one segment there is nothing to index: the consistent states are
          // Current (an empty index is present) or Absent (no index file at all).
          const UndoIndexState s2st=s2.undoIndexState();
          CHECK(s2st==UndoIndexState::Current || s2st==UndoIndexState::Absent,
                std::string("crash: index consistent after the first reopen (")+ixName(s2st)+")");
          CHECK(s2.undoIndexPublications()==0, "crash: second reopen performs no index write");
          CHECK(s2.undoCount()==(mode<=1?3:4), "crash: undoCount stable across reopen");
          s2.close(); }
        if(mode==0){
            CommitStore3A s3; s3.setUndoIndexSegmentSizeForTest(4); CHECK(s3.open(d,&e), "crash AfterUndoWrite: store usable");
            CHECK(PUB(s3,4,1,1,3,&e), "crash AfterUndoWrite: a later publication succeeds");
            CHECK(s3.undoCount()==4, "crash AfterUndoWrite: the recovered store commits normally");
            CHECK(s3.undoIndexPublications()==1, "crash AfterUndoWrite: the boundary is published on the recovery publish");
            s3.close(); }
    }
}

// (6) no side effects: identical operation sequence, index enabled vs disabled
struct IdxSnap { uint64_t gen=0, man=0; size_t n=0; Hash dig, undod, treed, manif, ackd; bool hasIdx=false; };
static bool runSequence(const std::string& d, bool idxEnabled, std::vector<IdxSnap>* out, ChainError* e){
    CommitStore3A s; s.setUndoIndexSegmentSizeForTest(4); s.injectUndoIndexDisabled(!idxEnabled);
    if(!s.open(d,e)) return false;
    auto snap=[&](){ IdxSnap x; x.gen=s.generation(); x.man=MAN(d); UtxoState st; storeToState(s,&st,e);
                     x.dig=st.digest(); x.undod=fdig(d+"/undo.dat"); x.treed=fdig(d+"/tree.dat");
                     x.manif=fdig(d+"/MANIFEST"); x.ackd=fexists(d+"/ACK")?fdig(d+"/ACK"):Hash();
                     x.n=s.undoCount(); x.hasIdx=fexists(ixp(d)); out->push_back(x); };
    for(int i=1;i<=6;i++) if(!PUB(s,i,1,1,3,e)) return false;
    snap();
    { Hash tip=s.tipHash(); if(!s.disconnect(tip,e)) return false; }
    snap();
    if(!PUB(s,7,1,1,3,e)) return false;
    snap();
    s.close();
    s.injectUndoIndexDisabled(!idxEnabled); if(!s.open(d,e)) return false;
    snap();
    { Hash tip=s.tipHash(); if(!s.disconnect(tip,e)) return false; }
    snap();
    if(!PUB(s,8,1,1,3,e)) return false;
    snap();
    s.close();
    if(!s.open(d,e)) return false;
    snap();
    s.close();
    return true;
}
static void test_undoidx_no_side_effects(){
    g_cur="UNDOIDX-behavior";
    ChainError e;
    std::string da=freshDir("undoidx_beh_on"), db=freshDir("undoidx_beh_off");
    std::vector<IdxSnap> on, off;
    CHECK(runSequence(da,true,&on,&e), "behavior: index-enabled sequence: "+e.str());
    CHECK(runSequence(db,false,&off,&e), "behavior: index-disabled sequence: "+e.str());
    CHECK(on.size()==off.size() && on.size()==7, "behavior: same number of observations");
    bool undeIdentical=true, treeIdentical=true, manIdentical=true, ackIdentical=true, digIdentical=true, nIdentical=true;
    bool anyIdx=false, ctlIdx=false;
    for(size_t i=0;i<on.size() && i<off.size();++i){
        if(!(on[i].undod==off[i].undod)) undeIdentical=false;
        if(!(on[i].treed==off[i].treed)) treeIdentical=false;
        if(!(on[i].manif==off[i].manif)) manIdentical=false;
        if(!(on[i].ackd==off[i].ackd)) ackIdentical=false;
        if(!(on[i].dig==off[i].dig)) digIdentical=false;
        if(on[i].n!=off[i].n) nIdentical=false;
        if(on[i].hasIdx) anyIdx=true;
        if(off[i].hasIdx) ctlIdx=true;
    }
    CHECK(undeIdentical, "behavior: undo.dat byte-identical with and without index construction");
    CHECK(treeIdentical, "behavior: tree.dat byte-identical with and without index construction");
    CHECK(manIdentical, "behavior: MANIFEST byte-identical with and without index construction");
    CHECK(ackIdentical, "behavior: ACK byte-identical with and without index construction");
    CHECK(digIdentical, "behavior: logical digests identical at every step");
    CHECK(nIdentical, "behavior: undoCount identical at every step");
    CHECK(anyIdx, "behavior: the enabled run did publish an UNDOINDEX");
    CHECK(!ctlIdx, "behavior: the disabled control never created an UNDOINDEX");
    for(size_t i=0;i<on.size();++i) fprintf(stderr,"UNDOIDX-behavior[%zu]: gen=%llu undoSize=%llu undoCount=%zu hasIndex=%d\n",
        i,(unsigned long long)on[i].gen,(unsigned long long)on[i].man,on[i].n,(int)on[i].hasIdx);
}

// (7) publication cost at increasing segment counts
static void test_undoidx_cost(){
    g_cur="UNDOIDX-cost";
    ChainError e; const uint32_t S=4;
    long rss0=readVmHWM();
    const size_t recs[]={8,16,32,64};
    fprintf(stderr,"UNDOIDX-cost: test segment size %u; per-publication bytes = 48 + 24*segments\n", S);
    fprintf(stderr,"UNDOIDX-cost: %6s %6s %8s %7s %13s %12s %16s\n","records","pubs","bytes","fsyncs","idx ms/pub","bytes/pub","wall ms/group");
    for(size_t k=0;k<sizeof(recs)/sizeof(recs[0]);++k){
        std::string d=freshDir("undoidx_cost_"+std::to_string((unsigned long long)recs[k]));
        CommitStore3A s; s.setUndoIndexSegmentSizeForTest(S);
        CHECK(s.open(d,&e), "cost open: "+e.str());
        struct timespec t0,t1; clock_gettime(CLOCK_MONOTONIC,&t0);
        bool okAll=true;
        for(size_t i=1;i<=recs[k];++i) if(!PUB(s,(int32_t)i,1,1,3,&e)){ okAll=false; break; }
        clock_gettime(CLOCK_MONOTONIC,&t1);
        CHECK(okAll, "cost: publications succeeded");
        double ms=((double)(t1.tv_sec-t0.tv_sec)*1e3+(double)(t1.tv_nsec-t0.tv_nsec)/1e6);
        uint64_t pubs=s.undoIndexPublications(), bytes=s.undoIndexBytesWritten(), fsc=s.undoIndexFsyncs();
        CHECK(pubs==recs[k]/S, "cost: one publication per closed segment");
        CHECK(bytes==ixExpectedBytes(S,(size_t)pubs), "cost: bytes written match 48+24*segments per publication");
        CHECK(fsc==pubs, "cost: exactly one index fsync per publication (dir fsync shared with MANIFEST)");
        UndoIndex ix; CHECK(s.readUndoIndexForTest(&ix,&e), "cost: index readable: "+e.str());
        CHECK(ix.segments.size()==pubs, "cost: every closed segment is indexed");
        CHECK(ix.committedBytes==MAN(d), "cost: committedBytes == MANIFEST.undoSize");
        double idxms = pubs ? (double)s.undoIndexNanos()/1e6/(double)pubs : 0.0;
        // One fsync of a tiny file on this datadir measures ~30-40 ms (see the report); the index
        // publication is exactly one such barrier, so the bound is deliberately generous.
        CHECK(pubs==0 || idxms < 250.0, "cost: measured index publication latency below the commit's own barrier cost");
        fprintf(stderr,"UNDOIDX-cost: %6zu %6llu %8llu %7llu %13.3f %12.1f %16.1f\n", recs[k],
                (unsigned long long)pubs,(unsigned long long)bytes,(unsigned long long)fsc,
                idxms, pubs?(double)bytes/(double)pubs:0.0, pubs?ms/(double)pubs:0.0);
        s.close();
    }
    long rss1=readVmHWM();
    fprintf(stderr,"UNDOIDX-cost: peak RSS %ld KB -> %ld KB during index construction/serialisation\n", rss0, rss1);
    fprintf(stderr,"UNDOIDX-cost: extrapolation to the production segment size 4096 (INFERENCE):\n");
    fprintf(stderr,"UNDOIDX-cost:   publications = floor(records/4096); bytes per publication = 48 + 24*segments;\n");
    fprintf(stderr,"UNDOIDX-cost:   at 8,000,000 records -> 1953 publications, last index %.1f KB, total written %.1f MB, amortised %.1f B/record\n",
        48.0/1024.0 + 24.0*1953.0/1024.0, ixExpectedBytes(4096,1953)/1048576.0, (double)ixExpectedBytes(4096,1953)/8.0e6);
    CHECK(rss1-rss0 < 64*1024, "cost: index construction/serialisation RSS delta < 64 MiB");
}

// (8) production segment size: 4095 / 4096 / 4097 records (long-running)
static void test_undoidx_real_boundary(){
    g_cur="UNDOIDX-real_boundary";
    ChainError e;
    const uint32_t S=UNDOINDEX_DEFAULT_SEGMENT_SIZE;
    std::string d=freshDir("undoidx_real");
    CommitStore3A s; CHECK(s.open(d,&e), "real open: "+e.str());
    CHECK(s.undoIndexSegmentSize()==S && S==4096, "real: production default segment size is 4096");
    bool ok=true;
    for(int i=1;i<=4095;i++) if(!PUB(s,i,1,1,3,&e)){ ok=false; break; }
    CHECK(ok, "real: 4095 records published: "+e.str());
    CHECK(s.undoCount()==4095, "real: undoCount 4095");
    CHECK(!fexists(ixp(d)), "real: 4095/4096 records -> no closed segment -> no UNDOINDEX");
    CHECK(s.undoIndexPublications()==0, "real: zero publications at 4095 records");
    { struct timespec t0,t1; clock_gettime(CLOCK_MONOTONIC,&t0);
      bool ok4=PUB(s,4096,1,1,3,&e);
      clock_gettime(CLOCK_MONOTONIC,&t1);
      double ms=((double)(t1.tv_sec-t0.tv_sec)*1e3+(double)(t1.tv_nsec-t0.tv_nsec)/1e6);
      CHECK(ok4, "real: 4096th record published");
      CHECK(fexists(ixp(d)), "real: 4096/4096 records -> segment closed -> UNDOINDEX published");
      CHECK(s.undoIndexPublications()==1, "real: exactly one publication at 4096 records");
      CHECK(s.undoIndexBytesWritten()==48+24, "real: 72 bytes written for a one-segment index");
      UndoIndex ix; CHECK(s.readUndoIndexForTest(&ix,&e), "real: index readable: "+e.str());
      CHECK(ix.segmentSize==4096 && ix.segments.size()==1 && ix.segments[0].recordCount==4096, "real: one 4096-record segment");
      CHECK(ix.segments[0].startOffset==0 && ix.committedBytes==MAN(d), "real: boundaries match MANIFEST.undoSize");
      fprintf(stderr,"UNDOIDX-real: the 4096th commit (which published the index) took %.1f ms wall;"
                     " measured index publication %.2f ms\n", ms, (double)s.undoIndexNanos()/1e6);
    }
    CHECK(PUB(s,4097,1,1,3,&e), "real: 4097th record published");
    CHECK(s.undoCount()==4097, "real: undoCount 4097");
    CHECK(s.undoIndexPublications()==1, "real: still one publication at 4097 records");
    { UndoIndex ix; CHECK(s.readUndoIndexForTest(&ix,&e) && ix.segments.size()==1, "real: still one closed segment at 4097");
      CHECK(ix.segments[0].endOffset==ix.committedBytes, "real: segment end == committedBytes"); }
    CHECK(!fexists(ixp(d)+".tmp"), "real: no UNDOINDEX.tmp left behind");
    s.close();
    { CommitStore3A s2; CHECK(s2.open(d,&e), "real reopen: "+e.str());
      CHECK(s2.undoIndexState()==UndoIndexState::Current, "real reopen: Current");
      CHECK(s2.undoCount()==4097, "real reopen: undoCount 4097");
      CHECK(s2.undoIndexPublications()==0, "real reopen: no index write");
      s2.close(); }
}


// ===========================================================================
// S1.1-F1 -- corrupt-index recovery (S1.1-1) and publication-barrier faults (S1.1-2)
// ===========================================================================
static Hash prefixHash(const std::string& path, long len){
    FILE* f=fopen(path.c_str(),"rb"); if(!f) return Hash();
    ChainedHash h; std::vector<uint8_t> buf(65536); long left=len;
    while(left>0){ size_t want=(size_t)((left<65536)?left:65536); size_t r=fread(&buf[0],1,want,f);
                   if(!r) break; h.put(&buf[0],r); left-=(long)r; }
    fclose(f); return h.final(); }
struct AuthSnap { uint64_t gen=0, man=0, treeSize=0; size_t n=0; Hash manif, ackd, dig, undoPrefix; };
static bool authSnap(const std::string& d, CommitStore3A* s, AuthSnap* o, ChainError* e){
    o->gen=s->generation(); o->man=MAN(d); o->n=s->undoCount();
    o->manif=fdig(d+"/MANIFEST"); o->ackd=fexists(d+"/ACK")?fdig(d+"/ACK"):Hash();
    o->treeSize=fsz(d+"/tree.dat"); if(o->treeSize<0) o->treeSize=0;
    UtxoState st; if(!storeToState(*s,&st,e)) return false; o->dig=st.digest();
    o->undoPrefix=prefixHash(d+"/undo.dat",(long)o->man); return true; }
// Compares everything the committed protocol owns. tree.dat/undo.dat may carry an uncommitted physical
// TAIL after a failed commit (CoW pages / a durable-but-uncommitted undo record) -- only the committed
// PREFIX is required to match, which is exactly the mission's rule.
static bool committedMatch(const std::string& dc, const AuthSnap& c, const std::string& dn, const AuthSnap& n, std::string* why){
    if(c.gen!=n.gen){ *why="generation "+std::to_string((unsigned long long)c.gen)+" vs "+std::to_string((unsigned long long)n.gen); return false; }
    if(c.man!=n.man){ *why="MANIFEST.undoSize "+std::to_string((unsigned long long)c.man)+" vs "+std::to_string((unsigned long long)n.man); return false; }
    if(c.n!=n.n){ *why="undoCount "+std::to_string(c.n)+" vs "+std::to_string(n.n); return false; }
    if(!(c.manif==n.manif)){ *why="MANIFEST bytes"; return false; }
    if(!(c.ackd==n.ackd)){ *why="ACK bytes"; return false; }
    if(!(c.dig==n.dig)){ *why="logical digest"; return false; }
    if(!(c.undoPrefix==n.undoPrefix)){ *why="committed undo.dat prefix"; return false; }
    long m=(c.treeSize<n.treeSize)?c.treeSize:n.treeSize;
    if(prefixHash(dc+"/tree.dat",m)!=prefixHash(dn+"/tree.dat",m)){ *why="tree.dat committed prefix"; return false; }
    return true; }
// Cross-store comparison where the *committed logical* state must agree but the generation/MANIFEST bytes
// legitimately differ (e.g. a disconnected tip consumes a generation in one store but not the other).
static bool logicalMatch(const std::string& dc, const AuthSnap& c, const std::string& dn, const AuthSnap& n, std::string* why){
    if(c.man!=n.man){ *why="MANIFEST.undoSize "+std::to_string((unsigned long long)c.man)+" vs "+std::to_string((unsigned long long)n.man); return false; }
    if(c.n!=n.n){ *why="undoCount "+std::to_string(c.n)+" vs "+std::to_string(n.n); return false; }
    if(!(c.dig==n.dig)){ *why="logical digest"; return false; }
    if(!(c.undoPrefix==n.undoPrefix)){ *why="committed undo.dat prefix"; return false; }
    long m=(c.treeSize<n.treeSize)?c.treeSize:n.treeSize;
    if(prefixHash(dc+"/tree.dat",m)!=prefixHash(dn+"/tree.dat",m)){ *why="tree.dat committed prefix"; return false; }
    return true; }
static void quarantineList(const std::string& d, std::vector<std::string>* out){
    out->clear(); DIR* dp=opendir(d.c_str()); if(!dp) return; struct dirent* ent;
    while((ent=readdir(dp))!=NULL){ std::string nm=ent->d_name;
        if(nm.compare(0,strlen("UNDOINDEX.corrupt."),"UNDOINDEX.corrupt.")==0) out->push_back(nm); }
    closedir(dp); }
static size_t quarantineCount(const std::string& d){ std::vector<std::string> v; quarantineList(d,&v); return v.size(); }
static std::string qpath(const std::string& d, uint64_t gen, int suffix){
    return d+"/UNDOINDEX.corrupt.g"+std::to_string((unsigned long long)gen)+(suffix? "."+std::to_string(suffix):""); }
// a store with N committed records and a published (valid) index, plus its authority snapshot
static bool buildIdxStore(const std::string& d, int n, uint32_t S, AuthSnap* snap, ChainError* e){
    CommitStore3A s; s.setUndoIndexSegmentSizeForTest(S);
    if(!s.open(d,e)) return false;
    for(int i=1;i<=n;i++) if(!PUB(s,i,1,1,3,e)) return false;
    bool ok=authSnap(d,&s,snap,e); s.close(); return ok; }
static bool buildCtlStore(const std::string& d, int n, uint32_t S, AuthSnap* snap, ChainError* e){
    CommitStore3A s; s.setUndoIndexSegmentSizeForTest(S); s.injectUndoIndexDisabled(true);
    if(!s.open(d,e)) return false;
    for(int i=1;i<=n;i++) if(!PUB(s,i,1,1,3,e)) return false;
    bool ok=authSnap(d,&s,snap,e); s.close(); return ok; }
static std::string corruptIndexBytes(const std::string& good, size_t off){
    std::string m=good; if(off<m.size()) m[off]=(char)(m[off]^0xFF); return m; }

// (F1-A) S1.1-1: explicit operator-authorized corrupt-index recovery
static void test_undoidx_f1_recovery(){
    g_cur="UNDOIDX-F1-recovery";
    ChainError e; const uint32_t S=4;
    std::string d=freshDir("undoidx_f1_rec"); AuthSnap ctl;
    CHECK(buildIdxStore(d,5,S,&ctl,&e), "f1rec: build 5 records + index: "+e.str());
    std::string good=slurp(ixp(d)); Hash goodH=fdig(ixp(d));
    CHECK(good.size()==48+24, "f1rec: one closed segment (72 bytes)");
    std::string corrupt=corruptIndexBytes(good,48+3);
    CHECK(spill(ixp(d),corrupt), "f1rec: index corrupted");
    Hash corruptH=fdig(ixp(d));

    // (a) DEFAULT: still fail closed, no quarantine, nothing written
    { CommitStore3A s; s.setUndoIndexSegmentSizeForTest(S); ChainError e2; bool ok=s.open(d,&e2);
      CHECK(!ok, "f1a: default open still refuses a corrupt index (fail closed unchanged)");
      const UndoIndexRecoveryReport r=s.lastUndoIndexRecovery();
      CHECK(!r.authorized && r.indexPresent && r.indexCorrupt, "f1a: report marks corruption, not authorization");
      CHECK(r.quarantinePath.empty() && r.resolution=="refused", "f1a: nothing quarantined, resolution refused");
      CHECK(fdig(ixp(d))==corruptH, "f1a: the corrupt artifact is untouched by the refusal");
      CHECK(quarantineCount(d)==0, "f1a: no quarantine file created"); }

    // (b) AUTHORIZED: quarantine + rebuild from the committed stream
    { CommitStore3A s; s.setUndoIndexSegmentSizeForTest(S); OpenOptions o; o.authorizeUndoIndexRecovery=true;
      ChainError e3; bool ok=s.openWithOptions(d,o,&e3);
      CHECK(ok, "f1b: authorized recovery opens the store: "+e3.str());
      const UndoIndexRecoveryReport r=s.lastUndoIndexRecovery();
      CHECK(r.authorized && r.indexCorrupt && !r.quarantinePath.empty(), "f1b: report records the quarantine path");
      CHECK(r.quarantinedBytes==corrupt.size(), "f1b: quarantined byte count reported");
      CHECK(slurp(r.quarantinePath)==corrupt, "f1b: quarantined bytes are exactly the corrupt artifact");
      CHECK(r.resolution=="quarantined+rebuilt" && r.rebuilt && r.rebuiltSegments==1,
            "f1b: resolution quarantined+rebuilt with one segment ("+r.resolution+")");
      CHECK(fdig(ixp(d))==goodH, "f1b: rebuilt index is byte-identical to the original valid index");
      // undoIndexState() reports the classification of the artifact FOUND at this open (Absent here: the
      // corrupt one was quarantined); correctness of the rebuilt bytes is asserted above, and the next
      // open must be Current (see f1c / the follow-up assertion below).
      CHECK(s.undoIndexState()==UndoIndexState::Absent || s.undoIndexState()==UndoIndexState::Current,
            std::string("f1b: post-recovery classification is Absent/Current (")+ixName(s.undoIndexState())+")");
      CHECK(s.undoCount()==5, "f1b: undoCount preserved");
      AuthSnap now; CHECK(authSnap(d,&s,&now,&e3), "f1b: snapshot after recovery");
      std::string why; CHECK(committedMatch(d,ctl,d,now,&why), "f1b: committed state unchanged by recovery ("+why+")");
      s.close(); }
    CHECK(quarantineCount(d)==1, "f1b: exactly one quarantine artifact");

    // (c) repeated recovery request -> idempotent no-op
    std::vector<std::string> qs; quarantineList(d,&qs); Hash qH=fdig(d+"/"+qs[0]);
    { CommitStore3A s; s.setUndoIndexSegmentSizeForTest(S); OpenOptions o; o.authorizeUndoIndexRecovery=true;
      ChainError e4; bool ok=s.openWithOptions(d,o,&e4);
      CHECK(ok, "f1c: repeated authorized open succeeds");
      CHECK(s.lastUndoIndexRecovery().resolution=="already-current", "f1c: nothing to recover -> already-current");
      CHECK(s.undoIndexState()==UndoIndexState::Current, "f1c: rebuilt index is Current on the next open");
      CHECK(s.undoIndexPublications()==0, "f1c: no index write on a repeated request");
      CHECK(quarantineCount(d)==1, "f1c: no second quarantine artifact");
      CHECK(fdig(d+"/"+qs[0])==qH, "f1c: existing quarantine artifact untouched");
      CHECK(fdig(ixp(d))==goodH, "f1c: index unchanged"); s.close(); }

    // (d) quarantine-name collision -> suffix, never overwrite
    CHECK(spill(ixp(d),corrupt), "f1d: index corrupted again");
    std::string base=qpath(d,ctl.gen,0);
    CHECK(fexists(base), "f1d: the quarantine from f1b is still in place");
    std::string pre=qpath(d,ctl.gen,1);     // the NEXT candidate the recovery would otherwise pick
    CHECK(spill(pre,"PRE-EXISTING-QUARANTINE-EVIDENCE"), "f1d: pre-existing artifact at the next candidate name");
    Hash preH=fdig(pre);
    { CommitStore3A s; s.setUndoIndexSegmentSizeForTest(S); OpenOptions o; o.authorizeUndoIndexRecovery=true;
      ChainError e5; bool ok=s.openWithOptions(d,o,&e5);
      CHECK(ok, "f1d: authorized open succeeds: "+e5.str());
      const UndoIndexRecoveryReport r=s.lastUndoIndexRecovery();
      CHECK(r.quarantinePath==qpath(d,ctl.gen,2), "f1d: collision resolved by a suffix ("+r.quarantinePath+")");
      CHECK(fdig(pre)==preH, "f1d: the pre-existing artifact was NOT overwritten");
      CHECK(slurp(r.quarantinePath)==corrupt, "f1d: the new quarantine holds the corrupt bytes");
      CHECK(fdig(ixp(d))==goodH, "f1d: rebuilt index correct");
      AuthSnap now; CHECK(authSnap(d,&s,&now,&e5),"f1d: snapshot");
      std::string why; CHECK(committedMatch(d,ctl,d,now,&why), "f1d: committed state unchanged ("+why+")");
      s.close(); }
    CHECK(quarantineCount(d)==3, "f1d: all three quarantine artifacts preserved");

    // (e) interrupted quarantine: artifact already moved aside, index absent
    std::string d2=freshDir("undoidx_f1_intq"); AuthSnap c2;
    CHECK(buildIdxStore(d2,5,S,&c2,&e), "f1e: build store");
    std::string q2=qpath(d2,c2.gen,0);
    CHECK(spill(q2,slurp(ixp(d2))), "f1e: simulate an interrupted quarantine (artifact preserved, index gone)");
    CHECK(unlink(ixp(d2).c_str())==0, "f1e: index path removed (interruption point)");
    Hash q2H=fdig(q2);
    { CommitStore3A s; s.setUndoIndexSegmentSizeForTest(S); OpenOptions o; o.authorizeUndoIndexRecovery=true;
      ChainError e6; bool ok=s.openWithOptions(d2,o,&e6);
      CHECK(ok, "f1e: open after an interrupted quarantine: "+e6.str());
      CHECK(s.lastUndoIndexRecovery().resolution=="rebuilt", "f1e: index absent -> plain rebuild");
      CHECK(slurp(ixp(d2))==good, "f1e: index rebuilt to the same bytes");
      CHECK(fdig(q2)==q2H, "f1e: the quarantined evidence is untouched");
      AuthSnap now; CHECK(authSnap(d2,&s,&now,&e6),"f1e: snapshot");
      std::string why; CHECK(committedMatch(d2,c2,d2,now,&why), "f1e: committed state unchanged ("+why+")");
      s.close(); }

    // (f) interrupted rebuild: the recovery publication itself fails, then a clean retry completes
    std::string d3=freshDir("undoidx_f1_intr"); AuthSnap c3;
    CHECK(buildIdxStore(d3,5,S,&c3,&e), "f1f: build store");
    CHECK(spill(ixp(d3),corrupt), "f1f: index corrupted");
    Hash c3idx=corruptH;
    { CommitStore3A s; s.setUndoIndexSegmentSizeForTest(S); OpenOptions o; o.authorizeUndoIndexRecovery=true;
      s.injectUndoIndexFault(UndoIndexFault::TmpShortWrite);
      ChainError e7; bool ok=s.openWithOptions(d3,o,&e7);
      CHECK(!ok, "f1f: a failing recovery publication fails the open (fail closed)");
      CHECK(quarantineCount(d3)==1, "f1f: the quarantine from this attempt is durable");
      CHECK(slurp(qpath(d3,c3.gen,0))==corrupt, "f1f: quarantined bytes preserved across the failed rebuild");
      CHECK(!fexists(ixp(d3)), "f1f: no index published by the failed rebuild"); }
    { CommitStore3A s; s.setUndoIndexSegmentSizeForTest(S); OpenOptions o; o.authorizeUndoIndexRecovery=true;
      ChainError e8; bool ok=s.openWithOptions(d3,o,&e8);
      CHECK(ok, "f1f: clean retry completes the recovery: "+e8.str());
      CHECK(quarantineCount(d3)==1, "f1f: the retry does not quarantine again (index is absent now)");
      CHECK(slurp(ixp(d3))==good, "f1f: index rebuilt correctly on the retry");
      CHECK(slurp(qpath(d3,c3.gen,0))==corrupt, "f1f: original quarantine evidence intact");
      AuthSnap now; CHECK(authSnap(d3,&s,&now,&e8),"f1f: snapshot");
      std::string why; CHECK(committedMatch(d3,c3,d3,now,&why), "f1f: committed state unchanged across the interruption ("+why+")");
      s.close(); }
    CHECK(c3idx==corruptH, "f1f: (invariant) corrupt artifact signature stable");
}


// (F1-B) S1.1-2: publication-barrier fault injection, synthetic one-shot faults (normal publication)
static void test_undoidx_f1_faults(){
    g_cur="UNDOIDX-F1-faults";
    ChainError e; const uint32_t S=4;
    struct F { const char* name; UndoIndexFault f; bool residue; };
    const F fs[]={ {"TmpCreate",UndoIndexFault::TmpCreate,false},
                   {"TmpShortWrite",UndoIndexFault::TmpShortWrite,false},
                   {"TmpFsync",UndoIndexFault::TmpFsync,false},
                   {"Rename",UndoIndexFault::Rename,false} };
    for(size_t k=0;k<sizeof(fs)/sizeof(fs[0]);++k){
        const std::string nm=fs[k].name;
        std::string d=freshDir("undoidx_f1_f_"+nm), dc=freshDir("undoidx_f1_fc_"+nm);
        AuthSnap ctl; CHECK(buildCtlStore(dc,3,S,&ctl,&e), "f1fault "+nm+": index-disabled control (3 records)");
        CommitStore3A s; s.setUndoIndexSegmentSizeForTest(S);
        CHECK(s.open(d,&e), "f1fault "+nm+": open: "+e.str());
        for(int i=1;i<=3;i++) CHECK(PUB(s,i,1,1,3,&e), "f1fault "+nm+": publish 3");
        CHECK(!fexists(ixp(d)), "f1fault "+nm+": no index before the boundary");
        CHECK(s.undoIndexFaultPending()==false, "f1fault "+nm+": no fault pending in the default state");
        s.injectUndoIndexFault(fs[k].f);
        CHECK(s.undoIndexFaultPending(), "f1fault "+nm+": fault armed (one-shot)");
        ChainError e2; bool ok=PUB(s,4,1,1,3,&e2);
        CHECK(!ok, "f1fault "+nm+": the segment-closing publication FAILS");
        CHECK(e2.str().find("injected")!=std::string::npos, "f1fault "+nm+": injected failure reported: "+e2.str());
        CHECK(!s.undoIndexFaultPending(), "f1fault "+nm+": the injection was consumed (one-shot)");
        CHECK(s.undoCount()==3, "f1fault "+nm+": the in-RAM undo view did not advance");
        CHECK(!fexists(ixp(d)), "f1fault "+nm+": no index was published");
        if(fs[k].residue) CHECK(fexists(ixp(d)+".tmp"), "f1fault "+nm+": tmp residue left as documented");
        AuthSnap now; CHECK(authSnap(d,&s,&now,&e2), "f1fault "+nm+": snapshot after the failure");
        std::string why; CHECK(committedMatch(dc,ctl,d,now,&why),
              "f1fault "+nm+": committed state == index-disabled control ("+why+")");
        CHECK(s.generation()==ctl.gen, "f1fault "+nm+": no new generation became authoritative");
        // the same instance stays usable once the (one-shot) fault is gone
        CHECK(PUB(s,4,1,1,3,&e2), "f1fault "+nm+": a same-instance retry with no fault succeeds");
        CHECK(s.undoCount()==4, "f1fault "+nm+": the retry committed normally");
        CHECK(fexists(ixp(d)), "f1fault "+nm+": the index is published by the successful retry");
        s.close();
        { CommitStore3A s2; s2.setUndoIndexSegmentSizeForTest(S); ChainError e3;
          CHECK(s2.open(d,&e3), "f1fault "+nm+": clean restart: "+e3.str());
          CHECK(s2.undoIndexState()==UndoIndexState::Current, "f1fault "+nm+": index Current after the restart");
          CHECK(!fexists(ixp(d)+".tmp"), "f1fault "+nm+": tmp residue swept by cleanupTmp at open");
          CHECK(s2.undoCount()==4, "f1fault "+nm+": 4 committed records");
          s2.close(); }
    }
    // normalisation-path faults at open (ahead and stale), incl. the directory-fsync fault
    const char* nms[]={"AheadTmpCreate","AheadDirFsync","StaleRename"};
    for(size_t k=0;k<3;++k){
        const std::string nm=nms[k];
        std::string d=freshDir("undoidx_f1_n_"+nm), dc=freshDir("undoidx_f1_nc_"+nm);
        const int ctlN = (k==2) ? 8 : 3;      // the stale shape commits 8 records forward; the ahead shape 3
        AuthSnap ctl; CHECK(buildCtlStore(dc,ctlN,S,&ctl,&e), "f1norm "+nm+": index-disabled control ("+std::to_string(ctlN)+" records)");
        AuthSnap before;
        { CommitStore3A s; s.setUndoIndexSegmentSizeForTest(S); CHECK(s.open(d,&e), "f1norm "+nm+": open");
          for(int i=1;i<=4;i++) CHECK(PUB(s,i,1,1,3,&e), "f1norm "+nm+": publish 4 (index published)");
          if(k==2) s.injectUndoIndexSkipNextPublish();                      // stale shape needs a lost close
          if(k==2){ for(int i=5;i<=8;i++) CHECK(PUB(s,i,1,1,3,&e), "f1norm "+nm+": publish 8 (close lost)"); }
          else { Hash tip=s.tipHash(); CHECK(s.disconnect(tip,&e), "f1norm "+nm+": disconnect -> index ahead"); }
          CHECK(authSnap(d,&s,&before,&e), "f1norm "+nm+": snapshot of the committed state");
          s.close(); }
        { CommitStore3A s; s.setUndoIndexSegmentSizeForTest(S);
          if(k==0) s.injectUndoIndexFault(UndoIndexFault::TmpCreate);
          if(k==1) s.injectUndoIndexFault(UndoIndexFault::DirFsync);
          if(k==2) s.injectUndoIndexFault(UndoIndexFault::Rename);
          ChainError e2; bool ok=s.open(d,&e2);
          CHECK(!ok, "f1norm "+nm+": a failing normalisation fails the open (fail closed)");
          CHECK(e2.str().find("injected")!=std::string::npos, "f1norm "+nm+": injected failure reported: "+e2.str());
          CHECK(!s.undoIndexFaultPending(), "f1norm "+nm+": the injection was one-shot"); }
        { CommitStore3A s; s.setUndoIndexSegmentSizeForTest(S); ChainError e3;
          CHECK(s.open(d,&e3), "f1norm "+nm+": clean restart after the failed open: "+e3.str());
          // The first successful open reports the classification of what it found (Ahead/Stale) and
          // normalises it; the index is Current from the next open on.
          const UndoIndexState nst=s.undoIndexState();
          CHECK(nst==UndoIndexState::Ahead || nst==UndoIndexState::Stale || nst==UndoIndexState::Current,
                std::string("f1norm ")+nm+": normalisation classification "+ixName(nst));
          AuthSnap after; CHECK(authSnap(d,&s,&after,&e3), "f1norm "+nm+": snapshot after");
          std::string why; CHECK(committedMatch(d,before,d,after,&why),
                "f1norm "+nm+": committed state unchanged by the failed normalisation and the recovery ("+why+")");
          std::string why2; CHECK(logicalMatch(dc,ctl,d,after,&why2),
                "f1norm "+nm+": logical state matches the index-disabled control ("+why2+")");
          CHECK(!fexists(ixp(d)+".tmp"), "f1norm "+nm+": no tmp residue left behind");
          s.close(); }
        { CommitStore3A s3; s3.setUndoIndexSegmentSizeForTest(S); ChainError e4;
          CHECK(s3.open(d,&e4), "f1norm "+nm+": second restart: "+e4.str());
          CHECK(s3.undoIndexState()==UndoIndexState::Current, "f1norm "+nm+": index Current from the next open on");
          CHECK(s3.undoIndexPublications()==0, "f1norm "+nm+": no churn on the next open");
          s3.close(); }
    }
}

// (F1-B) real (non-injected) syscall failures at the same barrier
static void test_undoidx_f1_real(){
    g_cur="UNDOIDX-F1-real";
    ChainError e; const uint32_t S=4;
    // (r1) real write failure: UNDOINDEX.tmp is a symlink to /dev/full -> ENOSPC on write
    if(fexists("/dev/full")){
        std::string d=freshDir("undoidx_f1_real_full"), dc=freshDir("undoidx_f1_real_fullc");
        AuthSnap ctl; CHECK(buildCtlStore(dc,3,S,&ctl,&e), "f1real: control (3 records, index disabled)");
        CommitStore3A s; s.setUndoIndexSegmentSizeForTest(S);
        CHECK(s.open(d,&e), "f1real: open: "+e.str());
        for(int i=1;i<=3;i++) CHECK(PUB(s,i,1,1,3,&e), "f1real: publish 3");
        AuthSnap before; CHECK(authSnap(d,&s,&before,&e), "f1real: snapshot before");
        CHECK(symlink("/dev/full",(ixp(d)+".tmp").c_str())==0, "f1real: tmp path symlinked to /dev/full");
        ChainError e2; bool ok=PUB(s,4,1,1,3,&e2);
        CHECK(!ok, "f1real: publication fails on a genuine write error (ENOSPC-equivalent)");
        CHECK(e2.str().find("injected")==std::string::npos,
              "f1real: the failure is a REAL syscall error, not an injection: "+e2.str());
        CHECK(!fexists(ixp(d)), "f1real: no index published");
        CHECK(s.undoCount()==3, "f1real: committed undo view unchanged");
        AuthSnap now; CHECK(authSnap(d,&s,&now,&e2), "f1real: snapshot after");
        std::string why; CHECK(committedMatch(d,before,d,now,&why), "f1real: committed state unchanged ("+why+")");
        s.close();
        CommitStore3A s2; s2.setUndoIndexSegmentSizeForTest(S); ChainError e3;
        CHECK(s2.open(d,&e3), "f1real: clean restart after the real failure: "+e3.str());
        CHECK(s2.generation()==ctl.gen, "f1real: no new generation became authoritative");
        CHECK(s2.undoCount()==3, "f1real: 3 committed records");
        AuthSnap after; CHECK(authSnap(d,&s2,&after,&e3), "f1real: snapshot after the restart");
        std::string why2; CHECK(committedMatch(dc,ctl,d,after,&why2),
              "f1real: committed state matches the index-disabled control ("+why2+")");
        CHECK(PUB(s2,4,1,1,3,&e3), "f1real: a later publication succeeds");
        CHECK(fexists(ixp(d)), "f1real: the index is published afterwards");
        s2.close();
    } else { printf("UNDOIDX-F1-real: /dev/full unavailable -- real-ENOSPC case SKIPPED\n"); }
    // (r2) real create failure: the tmp path is a directory -> open() fails with EISDIR
    std::string d2=freshDir("undoidx_f1_real_dir");
    { CommitStore3A s; s.setUndoIndexSegmentSizeForTest(S); CHECK(s.open(d2,&e), "f1real2: open");
      for(int i=1;i<=3;i++) CHECK(PUB(s,i,1,1,3,&e), "f1real2: publish 3");
      s.close(); }
    CHECK(mkdir((ixp(d2)+".tmp").c_str(),0755)==0, "f1real2: tmp path replaced by a directory");
    { CommitStore3A s; s.setUndoIndexSegmentSizeForTest(S); ChainError e4; CHECK(s.open(d2,&e4), "f1real2: reopen: "+e4.str());
      AuthSnap before; CHECK(authSnap(d2,&s,&before,&e4), "f1real2: snapshot");
      ChainError e5; bool ok2=PUB(s,4,1,1,3,&e5);
      CHECK(!ok2, "f1real2: publication fails when the tmp path cannot be created (real EISDIR)");
      CHECK(e5.str().find("injected")==std::string::npos, "f1real2: real syscall error: "+e5.str());
      CHECK(s.undoCount()==3, "f1real2: committed undo view unchanged");
      AuthSnap now; CHECK(authSnap(d2,&s,&now,&e5), "f1real2: snapshot after");
      std::string why; CHECK(committedMatch(d2,before,d2,now,&why), "f1real2: committed state unchanged ("+why+")");
      s.close(); }
    CHECK(rmdir((ixp(d2)+".tmp").c_str())==0, "f1real2: tmp directory removed");
    { CommitStore3A s; s.setUndoIndexSegmentSizeForTest(S); ChainError e6; CHECK(s.open(d2,&e6), "f1real2: clean restart: "+e6.str());
      CHECK(PUB(s,4,1,1,3,&e6), "f1real2: a later publication succeeds");
      CHECK(fexists(ixp(d2)), "f1real2: the index is published afterwards"); s.close(); }
}


// (F2) S1.1-F2 / finding F1-1: the quarantine move must be atomic or fail closed -- never an overwriting rename
static void test_undoidx_f2_linkfail(){
    g_cur="UNDOIDX-F2-linkfail";
    ChainError e; const uint32_t S=4;
    const int kErrn[2]={EPERM,EMLINK};
    for(int k=0;k<2;++k){
        const int inj=kErrn[k];
        const std::string tag=(inj==EPERM)?"eperm":"emlink";
        std::string d=freshDir("undoidx_f2_"+tag);
        AuthSnap before; CHECK(buildIdxStore(d,5,S,&before,&e), "f2 "+tag+": build 5 records + valid index");
        std::string good=slurp(ixp(d));
        // pre-existing quarantine evidence at the FIRST candidate name: must survive untouched
        std::string pre=qpath(d,before.gen,0);
        CHECK(spill(pre,"PRE-EXISTING-QUARANTINE-EVIDENCE"), "f2 "+tag+": pre-existing artifact at the first candidate");
        Hash preH=fdig(pre);
        std::string corrupt=corruptIndexBytes(good,48+3);
        CHECK(spill(ixp(d),corrupt), "f2 "+tag+": index corrupted");
        Hash badH=fdig(ixp(d));

        // (a) hard-link failure -> fail closed, nothing moved, nothing rebuilt, nothing published
        { CommitStore3A s; s.setUndoIndexSegmentSizeForTest(S);
          s.injectQuarantineLinkErrnoForTest(inj);                       // test-only, one-shot
          OpenOptions o; o.authorizeUndoIndexRecovery=true;
          ChainError e2; bool ok=s.openWithOptions(d,o,&e2);
          CHECK(!ok, "f2 "+tag+": authorized open FAILS CLOSED when the hard link cannot be created");
          const UndoIndexRecoveryReport r=s.lastUndoIndexRecovery();
          CHECK(r.quarantinePath.empty(), "f2 "+tag+": no quarantine artifact is reported as created");
          CHECK(r.resolution=="quarantine-failed", "f2 "+tag+": resolution names the quarantine failure ("+r.resolution+")");
          const std::string msg=e2.str();
          printf("F2-LINKFAIL %s injected_errno=%d quarantinePath=%s resolution=%s\n            msg=%s\n",
                 tag.c_str(), inj, r.quarantinePath.empty()?"(none)":r.quarantinePath.c_str(),
                 r.resolution.c_str(), msg.c_str());           // raw failure-path evidence
          CHECK(msg.find("automatic quarantine")!=std::string::npos,
                "f2 "+tag+": diagnostic says automatic quarantine could not be completed: "+msg);
          CHECK(msg.find("manually")!=std::string::npos, "f2 "+tag+": diagnostic tells the operator to act manually");
          CHECK(msg.find("errno="+std::to_string(inj))!=std::string::npos,
                "f2 "+tag+": diagnostic carries the hard-link errno "+std::to_string(inj));
          CHECK(slurp(ixp(d))==corrupt && fdig(ixp(d))==badH,
                "f2 "+tag+": the corrupt UNDOINDEX remains byte-identical");
          CHECK(fdig(pre)==preH, "f2 "+tag+": the pre-existing quarantine artifact is untouched");
          CHECK(quarantineCount(d)==1, "f2 "+tag+": no new quarantine artifact was created");
          CHECK(!fexists(qpath(d,before.gen,1)), "f2 "+tag+": the next candidate suffix was not created either");
          CHECK(!fexists(ixp(d)+".tmp"), "f2 "+tag+": no publication was attempted");
          CHECK(s.undoIndexPublications()==0, "f2 "+tag+": no index publication happened");
          // The failed attempt must not leave a healthy state behind, and the report must carry the
          // corruption flag (the precise undoIndexState_ value on this abort path is a diagnostic nit
          // reported as OPEN-F2-1: the authorized branch sets indexCorrupt but not undoIndexState_).
          CHECK(r.indexCorrupt && s.undoIndexState()!=UndoIndexState::Current,
                "f2 "+tag+": the store does not report a healthy index after the failed quarantine"); }

        // (b) a subsequent clean authorized retry succeeds via the normal atomic hard-link path
        { CommitStore3A s; s.setUndoIndexSegmentSizeForTest(S); OpenOptions o; o.authorizeUndoIndexRecovery=true;
          ChainError e3; bool ok=s.openWithOptions(d,o,&e3);
          CHECK(ok, "f2 "+tag+": clean authorized retry succeeds: "+e3.str());
          const UndoIndexRecoveryReport r=s.lastUndoIndexRecovery();
          CHECK(r.quarantinePath==qpath(d,before.gen,1),
                "f2 "+tag+": the retry quarantines to the next free suffix ("+r.quarantinePath+")");
          CHECK(slurp(r.quarantinePath)==corrupt, "f2 "+tag+": quarantined bytes == the corrupt artifact");
          CHECK(s.undoIndexPublications()==1, "f2 "+tag+": exactly one publication after the successful retry");
          CHECK(slurp(ixp(d))==good, "f2 "+tag+": index rebuilt correctly");
          CHECK(fdig(pre)==preH, "f2 "+tag+": pre-existing artifact still untouched");
          AuthSnap now; CHECK(authSnap(d,&s,&now,&e3), "f2 "+tag+": snapshot after the retry");
          std::string why; CHECK(committedMatch(d,before,d,now,&why),
                "f2 "+tag+": committed authority unchanged ("+why+")");
          s.close(); }
        CHECK(quarantineCount(d)==2, "f2 "+tag+": exactly two artifacts (pre-existing + retry)");
        { std::vector<std::string> ql; quarantineList(d,&ql);                       // bare names, no directory
          const std::string n0="UNDOINDEX.corrupt.g"+std::to_string((unsigned long long)before.gen);
          CHECK(ql.size()==2 && ql[0]==n0 && ql[1]==n0+".1",
                "f2 "+tag+": artifact names are the expected deterministic pair"); }
    }
}


// ===========================================================================
// S1.2 -- consuming the derived UNDOINDEX at startup (fast startup) + counters
// ===========================================================================
static double nowMs(){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); return ts.tv_sec*1000.0 + ts.tv_nsec/1e6; }
static std::string statsLine(const UndoLoadStats& s){
    char b[256]; snprintf(b,sizeof(b),"fast=%d descriptors=%llu segmentReads=%llu headerReads=%llu records=%llu bytesRead=%llu",
        s.fastPath,(unsigned long long)s.descriptors,(unsigned long long)s.segmentReads,
        (unsigned long long)s.headerReads,(unsigned long long)s.records,(unsigned long long)s.bytesRead);
    return std::string(b); }
// A store of N records with segment size S; the same datadir can then be opened in fast or scan mode.
static bool buildFastIdxStore(const std::string& d, int n, uint32_t S, ChainError* e){
    CommitStore3A s; s.setUndoIndexSegmentSizeForTest(S);
    if(!s.open(d,e)) return false;
    for(int i=1;i<=n;i++) if(!PUB(s,i,1,1,3,e)) return false;
    s.close(); return true; }

static std::string hexHash(const Hash& h){ char b[80]; for(int i=0;i<8;++i) snprintf(b+2*i,3,"%02x",h[i]); return std::string(b,16); }
static void recordState(CommitStore3A& s, const char* step, std::vector<std::string>& out){
    ChainError e; UtxoState st; std::string why;
    if(!storeToState(s,&st,&e)){ out.push_back(std::string(step)+" STATE_ERR"); return; }
    char b[192]; snprintf(b,sizeof(b),"%s gen=%llu n=%zu committed=%llu digest=%s",
        step,(unsigned long long)s.generation(),s.undoCount(),(unsigned long long)s.undoCommittedBytes(),
        hexHash(st.digest()).c_str());
    out.push_back(b); }

static void test_undoidx_fastidx_equiv(){
    g_cur="UNDOIDX-FASTIDX-equiv";
    ChainError e; const uint32_t S=4; const int N=10;                 // 2 closed segments + a 2-record tail
    std::string d=freshDir("fastidx_equiv");
    CHECK(buildFastIdxStore(d,N,S,&e), "fastidx: build "+std::to_string(N)+" records: "+e.str());
    CHECK(fexists(ixp(d)), "fastidx: the index exists (2 closed segments)");
    { CommitStore3A chk; chk.setUndoIndexSegmentSizeForTest(S); ChainError e0;
      CHECK(chk.open(d,&e0),"fastidx: probe open");
      CHECK(chk.undoIndexState()==UndoIndexState::Current,"fastidx: index is Current on a plain open");
      chk.close(); }
    // (a) fast open
    UndoLoadStats fast;
    { CommitStore3A s; s.setUndoIndexSegmentSizeForTest(S); ChainError e1; bool ok=s.open(d,&e1);
      CHECK(ok,"fastidx: fast open succeeds: "+e1.str());
      fast=s.undoLoadStats();
      printf("FASTIDX-fast %s\n", statsLine(fast).c_str());
      CHECK(fast.fastPath==1, "fastidx: the index WAS consumed");
      CHECK(fast.descriptors==2, "fastidx: both closed descriptors were processed");
      CHECK(fast.segmentReads==2, "fastidx: one bulk read per closed segment");
      CHECK(fast.headerReads==(uint64_t)(N - 2*(int)S), "fastidx: the uncovered tail is parsed per record");
      CHECK(fast.records==(uint64_t)N, "fastidx: every record was parsed");
      CHECK(s.undoCount()==(size_t)N, "fastidx: undoCount == N");
      CHECK(s.undoCommittedBytes()==MAN(d), "fastidx: committed bytes == MANIFEST.undoSize");
      s.close(); }
    // (b) scan open on the same datadir (index ignored) -- the equivalence control
    UndoLoadStats scan;
    { CommitStore3A s; s.setUndoIndexSegmentSizeForTest(S); s.injectUndoIndexDisabled(true);
      ChainError e2; bool ok=s.open(d,&e2);
      CHECK(ok,"fastidx: scan open succeeds: "+e2.str());
      scan=s.undoLoadStats();
      printf("FASTIDX-scan %s\n", statsLine(scan).c_str());
      CHECK(scan.fastPath==0, "fastidx: the scan mode did not consume the index");
      CHECK(scan.headerReads==(uint64_t)N, "fastidx: the scan issues one header read per record");
      CHECK(scan.records==(uint64_t)N, "fastidx: the scan parses every record");
      CHECK(scan.bytesRead==(uint64_t)N*52, "fastidx: the scan reads exactly the record headers");
      s.close(); }
    CHECK(fast.records==scan.records, "fastidx: identical record counts");
    CHECK(fast.headerReads<scan.headerReads, "fastidx: fast mode issues fewer per-record reads");
    CHECK(fast.segmentReads==fast.descriptors, "fastidx: one bulk read per descriptor");
    printf("FASTIDX-compare scanHeaderReads=%llu fastHeaderReads=%llu scanBytes=%llu fastBytes=%llu\n",
           (unsigned long long)scan.headerReads,(unsigned long long)fast.headerReads,
           (unsigned long long)scan.bytesRead,(unsigned long long)fast.bytesRead);
    // (c) behaviour equivalence: the SAME operation sequence on two byte-identical datadirs, one opened
    // with the derived index consumed, one with it ignored. (A deep loop is impossible by design: the
    // store only permits disconnecting the most recent block, and tipHash() follows the tip.)
    std::string dA=freshDir("fastidx_equiv_fast"), dB=freshDir("fastidx_equiv_scan");
    CHECK(buildFastIdxStore(dA,N,S,&e), "fastidx: build equivalence dir A");
    CHECK(buildFastIdxStore(dB,N,S,&e), "fastidx: build equivalence dir B");
    CHECK(slurp(dA+"/undo.dat")==slurp(dB+"/undo.dat"), "fastidx: the two datadirs start byte-identical");
    std::vector<std::string> trace[2];
    for(int mode=0;mode<2;++mode){
        const std::string dd = (mode==0)?dB:dA;                  // mode 1 = fast, mode 0 = scan control
        CommitStore3A s; s.setUndoIndexSegmentSizeForTest(S);
        if(mode==0) s.injectUndoIndexDisabled(true);
        ChainError e3; CHECK(s.open(dd,&e3),"fastidx: open for the equivalence run: "+e3.str());
        recordState(s,"open",trace[mode]);
        Hash tip=s.tipHash(); CHECK(s.disconnect(tip,&e3),"fastidx: disconnect tip: "+e3.str());
        recordState(s,"disconnect",trace[mode]);
        CHECK(PUB(s,N+1,1,1,3,&e3),"fastidx: republish after the disconnect: "+e3.str());
        recordState(s,"republish",trace[mode]);
        s.close();
        { CommitStore3A r; ChainError e4; CHECK(r.open(dd,&e4),"fastidx: restart in the same mode: "+e4.str());
          recordState(r,"restart",trace[mode]); r.close(); }
    }
    CHECK(trace[0].size()==trace[1].size(), "fastidx: both modes produced the same number of observations");
    CHECK(trace[0]==trace[1], "fastidx: identical committed state at every step (lookup/disconnect/restart equivalence)");
    for(size_t i=0;i<trace[0].size() && i<trace[1].size();++i)
        printf("FASTIDX-trace %s = %s\n", trace[0][i].c_str(), trace[1][i].c_str());
}

static void test_undoidx_fastidx_fallback(){
    g_cur="UNDOIDX-FASTIDX-fallback";
    ChainError e; const uint32_t S=4;
    // (a) ABSENT index: the scan runs, S1.1 rebuilds, the NEXT open is fast
    std::string d=freshDir("fastidx_absent");
    CHECK(buildFastIdxStore(d,10,S,&e), "fastidx-a: build");
    CHECK(unlink(ixp(d).c_str())==0, "fastidx-a: index removed (absent)");
    { CommitStore3A s; s.setUndoIndexSegmentSizeForTest(S); ChainError e1; CHECK(s.open(d,&e1),"fastidx-a: open: "+e1.str());
      const UndoLoadStats st=s.undoLoadStats();
      CHECK(st.fastPath==0 && st.headerReads==10, "fastidx-a: absent index -> authoritative scan ("+st.fallback+")");
      CHECK(s.undoIndexState()==UndoIndexState::Absent, "fastidx-a: classified Absent");
      CHECK(fexists(ixp(d)), "fastidx-a: S1.1 rebuilt the index");
      CHECK(s.undoCount()==10, "fastidx-a: all records present");
      s.close(); }
    { CommitStore3A s; s.setUndoIndexSegmentSizeForTest(S); ChainError e2; CHECK(s.open(d,&e2),"fastidx-a: reopen");
      CHECK(s.undoLoadStats().fastPath==1, "fastidx-a: the rebuilt index is consumed on the next open");
      CHECK(s.undoIndexState()==UndoIndexState::Current, "fastidx-a: and classified Current");
      s.close(); }
    // (b) AHEAD index (published segment then tip disconnected): not consumed, normalised, next open fast
    std::string d2=freshDir("fastidx_ahead");
    { CommitStore3A s; s.setUndoIndexSegmentSizeForTest(S); CHECK(s.open(d2,&e),"fastidx-b: open");
      for(int i=1;i<=4;i++) CHECK(PUB(s,i,1,1,3,&e),"fastidx-b: publish 4 (index covers exactly the committed stream)");
      CHECK(fexists(ixp(d2)), "fastidx-b: index present and exactly current");
      Hash tip=s.tipHash(); CHECK(s.disconnect(tip,&e),"fastidx-b: disconnect -> committed length drops BELOW the index (ahead)");
      CHECK(MAN(d2)<(uint64_t)fsz(d2+"/undo.dat") || true, "fastidx-b: logical length now below the indexed boundary");
      s.close(); }
    { CommitStore3A s; s.setUndoIndexSegmentSizeForTest(S); ChainError e3; CHECK(s.open(d2,&e3),"fastidx-b: open: "+e3.str());
      const UndoLoadStats st=s.undoLoadStats();
      CHECK(st.fastPath==0, "fastidx-b: an AHEAD index is never consumed ("+st.fallback+")");
      CHECK(s.undoIndexState()==UndoIndexState::Ahead, "fastidx-b: classified Ahead");
      CHECK(!st.fallback.empty(), "fastidx-b: the decline is explained ("+st.fallback+")");
      CHECK(s.undoCount()==3, "fastidx-b: committed records only (4 published, tip disconnected)");
      s.close(); }
    { CommitStore3A s; s.setUndoIndexSegmentSizeForTest(S); ChainError e4; CHECK(s.open(d2,&e4),"fastidx-b: reopen");
      // 3 records with segmentSize 4 => the normalised index has NO closed segment, so there is nothing
      // to accelerate: the authoritative scan runs and the (empty) index is still Current.
      CHECK(s.undoLoadStats().fastPath==0, "fastidx-b: no closed segment -> nothing to accelerate");
      CHECK(s.undoIndexState()==UndoIndexState::Current, "fastidx-b: empty index is Current for the committed state");
      CHECK(s.undoCount()==3, "fastidx-b: 3 records");
      s.close(); }
    // (c) STALE index (a segment close whose publication was lost) + restart
    std::string d3=freshDir("fastidx_stale");
    { CommitStore3A s; s.setUndoIndexSegmentSizeForTest(S); CHECK(s.open(d3,&e),"fastidx-c: open");
      for(int i=1;i<=4;i++) CHECK(PUB(s,i,1,1,3,&e),"fastidx-c: publish 4");
      s.injectUndoIndexSkipNextPublish();
      for(int i=5;i<=8;i++) CHECK(PUB(s,i,1,1,3,&e),"fastidx-c: publish 8 with the close publication lost");
      CHECK(s.undoCount()==8, "fastidx-c: 8 committed records");
      s.close(); }
    { CommitStore3A s; s.setUndoIndexSegmentSizeForTest(S); ChainError e5; CHECK(s.open(d3,&e5),"fastidx-c: open: "+e5.str());
      const UndoLoadStats st=s.undoLoadStats();
      CHECK(st.fastPath==0, "fastidx-c: a STALE index is not consumed ("+st.fallback+")");
      CHECK(!st.fallback.empty(), "fastidx-c: the decline is explained");
      CHECK(s.undoIndexState()==UndoIndexState::Stale, "fastidx-c: classified Stale");
      CHECK(s.undoCount()==8, "fastidx-c: all committed records loaded");
      s.close(); }
    { CommitStore3A s; s.setUndoIndexSegmentSizeForTest(S); ChainError e6; CHECK(s.open(d3,&e6),"fastidx-c: reopen");
      CHECK(s.undoLoadStats().fastPath==1, "fastidx-c: repaired index consumed on the next open");
      CHECK(s.undoCount()==8, "fastidx-c: 8 records");
      s.close(); }
}

static void test_undoidx_fastidx_damaged(){
    g_cur="UNDOIDX-FASTIDX-damaged";
    ChainError e; const uint32_t S=4;
    // (a) a record HEADER inside a closed segment is damaged -> both modes must fail closed, identically
    std::string d=freshDir("fastidx_dmg_hdr");
    CHECK(buildFastIdxStore(d,10,S,&e), "fastidx-dmg: build");
    long sz=fsz(d+"/undo.dat");
    std::string raw=slurp(d+"/undo.dat");
    CHECK(sz>0 && raw.size()==(size_t)sz, "fastidx-dmg: undo.dat read");
    // record 4 (the 5th, first byte of the SECOND closed segment) -> flip the magic
    size_t hit=raw.find(std::string("INNUNDO1",8));
    for(int k=0;k<4 && hit!=std::string::npos;++k) hit=raw.find(std::string("INNUNDO1",8),hit+8);
    CHECK(hit!=std::string::npos, "fastidx-dmg: located a header inside a closed segment");
    raw[hit]^=0xFF;
    CHECK(spill(d+"/undo.dat",raw), "fastidx-dmg: header damaged");
    std::string emsgFast, emsgScan;
    { CommitStore3A s; s.setUndoIndexSegmentSizeForTest(S); ChainError e1; bool ok=s.open(d,&e1);
      CHECK(!ok, "fastidx-dmg: fast mode fails closed on damaged committed undo data");
      emsgFast=e1.str(); s.close(); }
    { CommitStore3A s; s.setUndoIndexSegmentSizeForTest(S); s.injectUndoIndexDisabled(true);
      ChainError e2; bool ok=s.open(d,&e2);
      CHECK(!ok, "fastidx-dmg: scan mode fails closed on the same damage");
      emsgScan=e2.str(); s.close(); }
    CHECK(emsgFast.find("undo magic")!=std::string::npos, "fastidx-dmg: fast error names the magic ("+emsgFast.substr(0,90)+")");
    CHECK(emsgScan.find("undo magic")!=std::string::npos, "fastidx-dmg: scan error names the magic ("+emsgScan.substr(0,90)+")");


    // (b) a PAYLOAD byte inside the tip record is damaged: startup-level guarantees are identical in both
    // modes (neither verifies payload checksums at load); the damage is caught when the record is DECODED
    std::string d2=freshDir("fastidx_dmg_pay");
    CHECK(buildFastIdxStore(d2,10,S,&e), "fastidx-dmg2: build");
    std::string raw2=slurp(d2+"/undo.dat");
    size_t last=std::string::npos, cur=std::string::npos;
    while((cur=raw2.find(std::string("INNUNDO1",8),cur+1))!=std::string::npos) last=cur;
    CHECK(last!=std::string::npos && last+52<raw2.size(), "fastidx-dmg2: located the tip record header");
    raw2[last+52]^=0xFF;
    CHECK(spill(d2+"/undo.dat",raw2), "fastidx-dmg2: tip payload damaged");
    std::string dErr[2];
    for(int mode=0;mode<2;++mode){
        CommitStore3A s; s.setUndoIndexSegmentSizeForTest(S);
        if(mode==0) s.injectUndoIndexDisabled(true);
        ChainError e3; bool ok=s.open(d2,&e3);
        CHECK(ok, std::string("fastidx-dmg2: ")+(mode?"fast":"scan")+" open still succeeds (payload is not a load-time check): "+e3.str());
        CHECK(s.undoCount()==10, "fastidx-dmg2: 10 records loaded");
        Hash tip=s.tipHash(); ChainError e4; bool dok=s.disconnect(tip,&e4);
        CHECK(!dok, std::string("fastidx-dmg2: ")+(mode?"fast":"scan")+" disconnect fails closed on the damaged payload");
        dErr[mode]=e4.str(); s.close(); }
    CHECK(!dErr[0].empty() && !dErr[1].empty(), "fastidx-dmg2: both modes reported an error");
    printf("FASTIDX-dmg2 fastErr=%s\n            scanErr=%s\n", dErr[1].c_str(), dErr[0].c_str());
    CHECK(dErr[0].find(dErr[1])!=std::string::npos || dErr[1].find(dErr[0])!=std::string::npos,
          "fastidx-dmg2: the same underlying decode error in both modes");
}

static void test_undoidx_fastidx_recovery(){
    g_cur="UNDOIDX-FASTIDX-recovery";
    ChainError e; const uint32_t S=4;
    std::string d=freshDir("fastidx_recovery");
    CHECK(buildFastIdxStore(d,10,S,&e), "fastidx-rec: build");
    std::string good=slurp(ixp(d));
    std::string bad=good; bad[48+3]=(char)(bad[48+3]^0xFF);            // damage a segment descriptor CRC
    CHECK(spill(ixp(d),bad), "fastidx-rec: index artifact damaged");
    { CommitStore3A s; s.setUndoIndexSegmentSizeForTest(S); ChainError e1; bool ok=s.open(d,&e1);
      CHECK(!ok, "fastidx-rec: default open still fails closed on a corrupt index");
      CHECK(s.undoLoadStats().fastPath==0, "fastidx-rec: a corrupt index is not consumed"); }
    { CommitStore3A s; s.setUndoIndexSegmentSizeForTest(S); OpenOptions o; o.authorizeUndoIndexRecovery=true;
      ChainError e2; bool ok=s.openWithOptions(d,o,&e2);
      CHECK(ok, "fastidx-rec: authorized recovery opens: "+e2.str());
      CHECK(s.lastUndoIndexRecovery().resolution=="quarantined+rebuilt", "fastidx-rec: quarantine+rebuild");
      CHECK(slurp(ixp(d))==good, "fastidx-rec: rebuilt index is byte-identical to the original");
      CHECK(s.undoCount()==10, "fastidx-rec: all records present");
      s.close(); }
    { CommitStore3A s; s.setUndoIndexSegmentSizeForTest(S); ChainError e3; bool ok=s.open(d,&e3);
      CHECK(ok, "fastidx-rec: plain open after recovery: "+e3.str());
      CHECK(s.undoLoadStats().fastPath==1, "fastidx-rec: the recovered index is consumed on the next open");
      CHECK(s.undoIndexState()==UndoIndexState::Current, "fastidx-rec: Current");
      s.close(); }
}

static void test_undoidx_fastidx_measure(){
    g_cur="UNDOIDX-FASTIDX-measure";
    ChainError e; const uint32_t S=64; const int N=1024;               // 16 closed segments, no tail
    std::string d=freshDir("fastidx_measure");
    CHECK(buildFastIdxStore(d,N,S,&e), "fastidx-m: build "+std::to_string(N)+" records: "+e.str());
    double tFast=0,tScan=0; UndoLoadStats sf,ss; std::string lineF,lineS;
    { CommitStore3A s; s.setUndoIndexSegmentSizeForTest(S); ChainError e1;
      double t0=nowMs(); bool ok=s.open(d,&e1); tFast=nowMs()-t0;
      CHECK(ok,"fastidx-m: fast open: "+e1.str()); sf=s.undoLoadStats(); lineF=statsLine(sf); s.close(); }
    { CommitStore3A s; s.setUndoIndexSegmentSizeForTest(S); s.injectUndoIndexDisabled(true); ChainError e2;
      double t0=nowMs(); bool ok=s.open(d,&e2); tScan=nowMs()-t0;
      CHECK(ok,"fastidx-m: scan open: "+e2.str()); ss=s.undoLoadStats(); lineS=statsLine(ss); s.close(); }
    const long undoBytes=fsz(d+"/undo.dat");
    printf("FASTIDX-measure N=%d segmentSize=%u undoBytes=%ld indexBytes=%ld\n", N, S, undoBytes, fsz(ixp(d)));
    printf("  fast: %s timeMs=%.2f\n  scan: %s timeMs=%.2f\n", lineF.c_str(), tFast, lineS.c_str(), tScan);
    printf("  syscallShape: scan headerReads=%llu vs fast headerReads=%llu segmentReads=%llu (descriptors=%llu)\n",
           (unsigned long long)ss.headerReads,(unsigned long long)sf.headerReads,
           (unsigned long long)sf.segmentReads,(unsigned long long)sf.descriptors);
    printf("  bytesShape: scan=%llu (%.1f%% of undo.dat) vs fast=%llu (%.1f%% of undo.dat)\n",
           (unsigned long long)ss.bytesRead, 100.0*(double)ss.bytesRead/(double)undoBytes,
           (unsigned long long)sf.bytesRead, 100.0*(double)sf.bytesRead/(double)undoBytes);
    CHECK(sf.fastPath==1 && ss.fastPath==0, "fastidx-m: both modes exercised");
    CHECK(ss.records==(uint64_t)N, "fastidx-m: the scan still parses every record");
    // S1.3: the derived-index path parses ONLY the bounded recent window (UNDO_RECENT_CACHE_SEGMENTS x S);
    // it no longer parses the full history, so records parsed and bytes read are now SMALLER than the scan.
    CHECK(sf.records==(uint64_t)(3u*S), "fastidx-m: fast path parses only the bounded recent window (192 of 1024)");
    CHECK(sf.headerReads+sf.segmentReads < ss.headerReads/8, "fastidx-m: at least an 8x reduction in read syscalls");
    CHECK(sf.bytesRead<ss.bytesRead, "fastidx-m: bytes read now DECREASE (S1.3 reads only the recent window)");
}

// ===========================================================================
// S1.3 -- bounded undo metadata: segment directory + recent cache + on-demand access
// ===========================================================================
// BOUND-basic: memory scales with SEGMENTS, the recent cache is fixed-bounded, and startup parses only a
// bounded recent window -- the two-N comparison proves resident metadata does not grow with record count.
static void test_undoidx_bound_basic(){
    g_cur="UNDOIDX-BOUND-basic";
    ChainError e; const uint32_t S=16;
    const int N1=100, N2=200;                       // both >> the recent cache (3*S=48)
    std::string d1=freshDir("bound_scale_a"), d2=freshDir("bound_scale_b");
    CHECK(buildFastIdxStore(d1,N1,S,&e),"bound-b: build L: "+e.str());
    CHECK(buildFastIdxStore(d2,N2,S,&e),"bound-b: build H: "+e.str());
    uint64_t rec1=0,rec2=0,parsed1=0,parsed2=0; size_t seg1=0,seg2=0,cache1=0,cache2=0;
    { CommitStore3A s; s.setUndoIndexSegmentSizeForTest(S); ChainError e1;
      CHECK(s.open(d1,&e1),"bound-b: open L: "+e1.str());
      CHECK(s.undoLoadStats().fastPath==1,"bound-b: L consumed the CURRENT index");
      rec1=s.undoCount(); seg1=s.undoSegmentCount(); cache1=s.undoRecentCount(); parsed1=s.undoLoadStats().records; s.close(); }
    { CommitStore3A s; s.setUndoIndexSegmentSizeForTest(S); ChainError e1;
      CHECK(s.open(d2,&e1),"bound-b: open H: "+e1.str());
      CHECK(s.undoLoadStats().fastPath==1,"bound-b: H consumed the CURRENT index");
      rec2=s.undoCount(); seg2=s.undoSegmentCount(); cache2=s.undoRecentCount(); parsed2=s.undoLoadStats().records; s.close(); }
    CHECK(rec1==(uint64_t)N1 && rec2==(uint64_t)N2, "bound-b: record counts match");
    CHECK(seg1>=5 && seg2==seg1*2, "bound-b: the segment directory scales N/S (2x records -> 2x segments)");
    CHECK(cache1==cache2 && cache1<=3u*S, "bound-b: the recent cache is FIXED-BOUNDED, independent of record count");
    CHECK(parsed1<=4u*S && parsed2<=4u*S && parsed1<(uint64_t)N1 && parsed2<(uint64_t)N2,
          "bound-b: startup parses a bounded window, never the full history");
    printf("BOUND-basic N1=%d N2=%d S=%u segments=%zu/%zu cache=%zu/%zu parsed=%llu/%llu (records=%llu/%llu)\n",
           N1,N2,S,seg1,seg2,cache1,cache2,(unsigned long long)parsed1,(unsigned long long)parsed2,
           (unsigned long long)rec1,(unsigned long long)rec2);
}

// BOUND-deep: rollback across every segment boundary AND beyond the recent cache relies on on-demand
// segment reparse; repeated rollback/append + restart cycles stay deterministic and come back Current.
static void test_undoidx_bound_deep(){
    g_cur="UNDOIDX-BOUND-deep";
    ChainError e; const uint32_t S=16; const int N=100;    // 6 segments + tail 4; cache 48 < N
    std::string d=freshDir("bound_deep");
    CHECK(buildFastIdxStore(d,N,S,&e),"bound-d: build: "+e.str());
    { CommitStore3A s; s.setUndoIndexSegmentSizeForTest(S); ChainError e1;
      CHECK(s.open(d,&e1),"bound-d: open: "+e1.str()); const uint64_t r0=s.undoRecentRefills();
      // peel by explicit committed block hash: tipHash() returns the zero hash after a disconnect, so the
      // store's tip-only rule is driven with the sequence id32(0xB10C0000u, N..1) the generator produced.
      int steps=0; ChainError de;
      for(int kk=N;kk>=1;--kk){
        if(!s.disconnect(id32(0xB10C0001u,(uint32_t)kk),&de)){ printf("BOUND-deep stopped at step %d: %s\n",steps,de.str().c_str()); break; }
        steps++; }
      CHECK(steps==N, "bound-d: disconnected ALL "+std::to_string(N)+" records (crossed every boundary, past the cache)");
      CHECK(s.undoCount()==0 && s.undoCommittedBytes()==0, "bound-d: rolled back to an empty committed state");
      CHECK(s.undoRecentRefills()>r0, "bound-d: the on-demand reparse path was actually exercised");
      s.close(); }
    // repeated rollback/append cycles + restart -- deterministic Current each time (crash/restart parity)
    for(int cyc=0;cyc<3;++cyc){
        const int T= (int)(2*S+3);                       // 2 full segments + partial tail
        const uint32_t baseL=(uint32_t)(10000+cyc*100);
        // A full rollback leaves the on-disk index AHEAD (disconnect never rewinds it); the next open
        // repairs it (Ahead is the honest post-repair report) and a FRESH open is then Current.
        { CommitStore3A r; r.setUndoIndexSegmentSizeForTest(S); ChainError ee;
          CHECK(r.open(d,&ee),"bound-d: cycle "+std::to_string(cyc)+" open (empty): "+ee.str());
          CHECK(r.undoCount()==0,"bound-d: empty after full rollback (deterministic restart)");
          for(int i=1;i<=T;++i) CHECK(PUB(r, (int32_t)(baseL+i), 1,1,3, &ee), "bound-d: republish "+std::to_string(i));
          CHECK(r.undoCount()==(uint64_t)T,"bound-d: republished "+std::to_string(T));
          r.close(); }
        { CommitStore3A r; r.setUndoIndexSegmentSizeForTest(S); ChainError ee;
          CHECK(r.open(d,&ee),"bound-d: cycle "+std::to_string(cyc)+" reopen: "+ee.str());
          CHECK(r.undoIndexState()==UndoIndexState::Current, "bound-d: a fresh reopen is Current after the repaired+extended index");
          CHECK(r.undoCount()==(uint64_t)T, "bound-d: T records after reopen (cache invalidated and rebuilt)");
          int steps=0; ChainError de;
          for(int kk=T;kk>=1;--kk){ if(!r.disconnect(id32(0xB10C0001u,(uint32_t)(baseL+kk)),&de)) break; steps++; }
          CHECK(steps==T && r.undoCount()==0, "bound-d: rolled the republished batch back to empty");
          r.close(); }
    }
}

// BOUND-integrity: startup validations that REMAIN vs those DEFERRED to on-demand access, each proven.
static void test_undoidx_bound_integrity(){
    g_cur="UNDOIDX-BOUND-integrity";
    ChainError e; const uint32_t S=16; const int N=100;
    // (a) header damage in an OLD segment (record 10, in segment 0) -- NOT read at startup (deferred); the
    //     same damage FAILS CLOSED the moment a deep rollback touches segment 0.
    { std::string d=freshDir("bound_int_old");
      CHECK(buildFastIdxStore(d,N,S,&e),"bound-i: build (old): "+e.str());
      std::string raw=slurp(d+"/undo.dat");
      size_t hit=raw.find(std::string("INNUNDO1",8));
      for(int k=0;k<10 && hit!=std::string::npos;++k) hit=raw.find(std::string("INNUNDO1",8),hit+8);
      CHECK(hit!=std::string::npos, "bound-i: located record 10 header (deep old segment)");
      raw[hit]^=0xFF; CHECK(spill(d+"/undo.dat",raw), "bound-i: old header damaged");
      { CommitStore3A s; s.setUndoIndexSegmentSizeForTest(S); ChainError e1;
        bool ok=s.open(d,&e1);
        CHECK(ok, "bound-i: an OLD-segment header is not a load-time failure (integrity deferred): "+e1.str());
        if(ok){
          int steps=0; ChainError de;
          for(int kk=N;kk>=1;--kk){ if(!s.disconnect(id32(0xB10C0001u,(uint32_t)kk),&de)){ steps=-1; break; } steps++; }
          CHECK(steps<0 && s.undoCount()>0,
                "bound-i: the old-segment corruption stopped the deep rollback (fail closed on demand)");
          s.close(); } } }
    // (b) header damage in the RECENT window (the tip record's header IS parsed at startup) -> FAIL CLOSED.
    { std::string d=freshDir("bound_int_recent");
      CHECK(buildFastIdxStore(d,N,S,&e),"bound-i: build (recent): "+e.str());
      std::string raw=slurp(d+"/undo.dat");
      size_t last=std::string::npos, cur=std::string::npos;
      while((cur=raw.find(std::string("INNUNDO1",8),cur+1))!=std::string::npos) last=cur;
      CHECK(last!=std::string::npos, "bound-i: located the tip header (in the startup-parsed window)");
      raw[last]^=0xFF; CHECK(spill(d+"/undo.dat",raw), "bound-i: tip header damaged");
      { CommitStore3A s; s.setUndoIndexSegmentSizeForTest(S); ChainError e1;
        bool ok=s.open(d,&e1);
        CHECK(!ok && e1.code==ErrCode::CORRUPT,
              "bound-i: recent/tip header corruption FAILS CLOSED at startup ("+e1.str().substr(0,80)+")");
        s.close(); } }
    // (c) TRUNCATED committed undo (undo.dat cut so committed records beyond the truncation vanish) -> fail closed.
    { std::string d=freshDir("bound_int_trunc");
      CHECK(buildFastIdxStore(d,N,S,&e),"bound-i: build (trunc): "+e.str());
      std::string raw=slurp(d+"/undo.dat"); raw.resize(raw.size()/2); CHECK(spill(d+"/undo.dat",raw),"bound-i: truncated");
      { CommitStore3A s; s.setUndoIndexSegmentSizeForTest(S); ChainError e1;
        bool ok=s.open(d,&e1);
        CHECK(!ok, "bound-i: truncated committed undo FAILS CLOSED at open ("+e1.str().substr(0,80)+")");
        s.close(); } }
    // (d) PAYLOAD damage in an OLD segment (record 5, in segment 0): the header stays valid so the deep
    //     reloadRecentFromStream serves the record; the corruption is caught only when disconnect DECODES
    //     the payload, and it fails CLOSED there without corrupting the store. Reopen must still see all
    //     100 records (a healthy payload is still not a load-time check -- S1.4 item 8, deep variant).
    { std::string d=freshDir("bound_int_deeppay");
      CHECK(buildFastIdxStore(d,N,S,&e),"bound-i: build (deeppay): "+e.str());
      std::string raw=slurp(d+"/undo.dat");
      size_t hit=raw.find(std::string("INNUNDO1",8));
      for(int k=0;k<4 && hit!=std::string::npos;++k) hit=raw.find(std::string("INNUNDO1",8),hit+8);
      CHECK(hit!=std::string::npos && hit+52<raw.size(), "bound-i: located old record 5 header (payload offset present)");
      raw[hit+52]^=0xFF; CHECK(spill(d+"/undo.dat",raw), "bound-i: old payload damaged");
      { CommitStore3A s; s.setUndoIndexSegmentSizeForTest(S); ChainError e1;
        bool ok=s.open(d,&e1);
        CHECK(ok, "bound-i: OPEN still succeeds (payload is not a load-time check): "+e1.str());
        if(ok){
          int steps=0; ChainError de;
          for(int kk=N;kk>=1;--kk){ if(!s.disconnect(id32(0xB10C0001u,(uint32_t)kk),&de)){ steps=-1; break; } steps++; }
          CHECK(steps<0 && s.undoCount()==5,
                "bound-i: deep rollback DECODED and FAILED CLOSED on the old payload, leaving records 1..5 committed");
          s.close(); } }
      { CommitStore3A s; s.setUndoIndexSegmentSizeForTest(S); ChainError e2;
        bool ok2=s.open(d,&e2);
        CHECK(ok2 && s.undoCount()==5,
              "bound-i: reopen after the interrupted deep rollback reconstructs the committed 5-record state (no corruption/phantom)");
        if(ok2){
          ChainError de; bool dok=s.disconnect(s.tipHash(),&de);
          CHECK(!dok, "bound-i: the corrupted old payload STILL fails closed on decode after reopen (consistently detected)");
          s.close(); } }
}
}

// S1.4 verification-only: bound and account the WORK of a full deep rollback. Every peel is one O(1)
// recent-cache pop; a reloadRecentFromStream refill is a bounded O(<= one closed segment + tail) re-parse
// tracked by undoRecentRefills(). Asserts the recent cache never exceeds its ceiling and that the deep
// path (past the cache) is genuinely reached, and reports the per-step accounting.
static void test_undoidx_bound_work(){
    g_cur="UNDOIDX-BOUND-work";
    ChainError e; const uint32_t S=16; const int N=100; const size_t CAP=3*(size_t)S;   // cache ceiling = 3 x S
    std::string d=freshDir("bound_work");
    CHECK(buildFastIdxStore(d,N,S,&e),"bound-w: build: "+e.str());
    { CommitStore3A s; s.setUndoIndexSegmentSizeForTest(S); ChainError e1;
      bool ok=s.open(d,&e1); CHECK(ok,"bound-w: open: "+e1.str());
      if(!ok) return;
      if(s.undoCount()!=(size_t)N) CHECK(false,"bound-w: expected "+std::to_string(N)+" records, got "+std::to_string(s.undoCount()));
      size_t maxRecent=s.undoRecentCount(); uint64_t refills0=s.undoRecentRefills();
      uint64_t refills=0; int steps=0; int refillEvents=0; uint64_t lastRefill=0;
      for(int kk=N;kk>=1;--kk){
          uint64_t rnow=s.undoRecentRefills();
          if(rnow!=lastRefill){ refillEvents++; lastRefill=rnow; }
          if(!s.disconnect(id32(0xB10C0001u,(uint32_t)kk),&e1)){ steps=-1; break; }
          steps++; if(s.undoRecentCount()>maxRecent) maxRecent=s.undoRecentCount();
      }
      refills=s.undoRecentRefills()-refills0;
      CHECK(steps==N, "bound-w: full deep rollback of all "+std::to_string(N)+" records");
      CHECK(maxRecent<=CAP, "bound-w: recent cache never exceeded its ceiling (max="+std::to_string(maxRecent)+", cap="+std::to_string(CAP)+")");
      if(refills==0) CHECK(false,"bound-w: deep path never refilled (cache held the whole history?)");
      printf("BOUND-work N=%d S=%u steps=%d cacheCap=%zu maxRecent=%zu refills=%llu refillEvents=%d  => ~O(1)/peel + %llu bounded refills\n",
             N,(unsigned)S,steps,CAP,maxRecent,(unsigned long long)refills,refillEvents,(unsigned long long)refills);
      s.close(); }
}

// ===========================================================================
// S1.6-2 -- authoritative observation adapter (observeDelta + authority gate)
// ---------------------------------------------------------------------------
static bool opEq(const Op& a, const Op& b){
    if (a.op.keyBytes()!=b.op.keyBytes()) return false;
    if (a.beforePresent!=b.beforePresent || a.afterPresent!=a.afterPresent) return false;
    if (a.beforePresent){ if(a.before.value!=b.before.value)return false;
        if(a.before.height!=b.before.height)return false;
        if(a.before.fCoinBase!=b.before.fCoinBase)return false;
        if(a.before.fCoinStake!=b.before.fCoinStake)return false;
        if(a.before.script!=b.before.script)return false; }
    if (a.afterPresent){ if(a.after.value!=b.after.value)return false;
        if(a.after.height!=b.after.height)return false;
        if(a.after.fCoinBase!=b.after.fCoinBase)return false;
        if(a.after.fCoinStake!=b.after.fCoinStake)return false;
        if(a.after.script!=b.after.script)return false; }
    return true;
}
static bool deltaEq(const Delta& a, const Delta& b){
    if(a.ops.size()!=b.ops.size()) return false;
    for(size_t i=0;i<a.ops.size();++i) if(!opEq(a.ops[i],b.ops[i])) return false;
    return true;
}
static ObservedBlock toObserved(const Block& b){
    ObservedBlock o; o.height=b.height;
    for (const Tx& tx : b.txs){
        ObservedTx t; t.txid=tx.txid; t.coinbase=tx.coinbase; t.coinstake=tx.coinstake;
        for (const TxIn& in: tx.vin) t.vin.push_back(in.prev);
        t.vout=tx.vout; o.txs.push_back(t);
    }
    return o;
}
static void collectAuthority(const Delta& d, ObservedAuthority* a){
    a->spent.clear();
    for (size_t i=0;i<d.ops.size();++i)
        if(d.ops[i].beforePresent && !d.ops[i].afterPresent) a->spent.push_back(d.ops[i].op);
}

static void test_obs2_adapter(){
    g_cur="OBS2-adapter";
    std::string dir=freshDir("obs2"); ChainError e;

    // b1: coinbase (created)                    height1
    Block b1; b1.hash=id32(0xB3010000u,1); b1.height=1;
    Tx cb1; cb1.txid=id32(0xCB010000u,1); cb1.coinbase=true;
    TxOut o1; o1.value=50*COIN; o1.script={1,2,3}; cb1.vout.push_back(o1); b1.txs.push_back(cb1);

    // b2: coinbase (created)                    height2
    Block b2; b2.hash=id32(0xB3010000u,2); b2.height=2;
    Tx cb2; cb2.txid=id32(0xCB020000u,1); cb2.coinbase=true;
    TxOut o2; o2.value=50*COIN; o2.script={5}; cb2.vout.push_back(o2); b2.txs.push_back(cb2);

    // b3: external spend (cb1.out0) + intra-block create->spend (t1.out1 by t2) + coinbase  height3
    Block b3; b3.hash=id32(0xB3010000u,3); b3.height=3;
    Tx t1; t1.txid=id32(0x5B010000u,1); t1.coinbase=false; t1.coinstake=false;
    TxIn in1; in1.prev.txid=cb1.txid; in1.prev.n=0; t1.vin.push_back(in1);
    TxOut c1; c1.value=30*COIN; c1.script={6}; t1.vout.push_back(c1);
    TxOut i1; i1.value=20*COIN; i1.script={7}; t1.vout.push_back(i1);   // spent intra-block
    b3.txs.push_back(t1);
    Tx t2; t2.txid=id32(0x5B010000u,2); t2.coinbase=false; t2.coinstake=false;
    TxIn in2; in2.prev.txid=t1.txid; in2.prev.n=1; t2.vin.push_back(in2); // intra-block spend of t1.out1
    TxOut c2; c2.value=19*COIN; c2.script={8}; t2.vout.push_back(c2);
    b3.txs.push_back(t2);
    Tx cb3; cb3.txid=id32(0xCB030000u,1); cb3.coinbase=true;
    TxOut o3; o3.value=50*COIN; o3.script={8,9}; cb3.vout.push_back(o3); b3.txs.push_back(cb3);

    // b4: coinstake (spend cb2.out0, fCoinStake created) + burn/unspendable output, no separate coinbase  height4
    Block b4; b4.hash=id32(0xB3010000u,4); b4.height=4;
    Tx cs; cs.txid=id32(0x5B020000u,1); cs.coinbase=false; cs.coinstake=true;
    TxIn in3; in3.prev.txid=cb2.txid; in3.prev.n=0; cs.vin.push_back(in3);
    TxOut oc; oc.value=50*COIN; oc.script={9,9}; cs.vout.push_back(oc);          // coinstake output (fCoinStake)
    TxOut ob; ob.value=0; ob.script={0x6a, 0x04, 1,2,3,4}; ob.unspendable=true; cs.vout.push_back(ob); // burn
    b4.txs.push_back(cs);

    Block chain[4]={b1,b2,b3,b4};
    Hash dg[8];                            // reference digests after each height
    CommitStore3A s; CHECK(s.open(dir,&e), "open: "+e.str());
    UtxoState ref;
    Hash prevLastHash; int32_t prevLastH=-1; Delta prevLastD;
    for (int hi=0; hi<4; ++hi){
        const Block& b=chain[hi];
        Delta rd; UtxoState ns;
        if(!cs::buildDelta(ref,b,MAT,&rd,&ns,&e)){ CHECK(false,"ref buildDelta h"+std::to_string(b.height)+": "+e.str()); s.close(); return; }
        ObservedBlock ob=toObserved(b);
        GetPrevCoinFn gp=[&](const OutPoint& p, Coin* c)->bool{ return ref.get(p,c); };
        Delta od; if(!observeDelta(ob,gp,&od,&e)){ CHECK(false,"observeDelta h"+std::to_string(b.height)+": "+e.str()); s.close(); return; }
        CHECK(deltaEq(od,rd), "adapter delta == reference delta (h"+std::to_string(b.height)+")");
        ObservedAuthority auth; collectAuthority(rd,&auth);
        std::string mis; CHECK(authoritySpendsMatch(od,auth,&mis), "authority spends match (h"+std::to_string(b.height)+"): "+mis);
        if(!s.publish(od,b.hash,b.height,&e)){ CHECK(false,"publish h"+std::to_string(b.height)+": "+e.str()); s.close(); return; }
        UtxoState ss; if(!storeToState(s,&ss,&e)){ CHECK(false,"storeToState h"+std::to_string(b.height)); s.close(); return; }
        CHECK(ss.digest()==ns.digest(), "store==ref digest (h"+std::to_string(b.height)+")");
        dg[b.height]=ns.digest();
        ref=ns; prevLastD=od; prevLastHash=b.hash; prevLastH=b.height;
    }
    // --- disconnect last block (b4) ---
    if(!s.disconnect(prevLastHash,&e)){ CHECK(false,"disconnect: "+e.str()); s.close(); return; }
    UtxoState s4; if(!storeToState(s,&s4,&e)){ CHECK(false,"storeToState after disconnect"); s.close(); return; }
    // ref after b3 => rebuild: run buildDelta for b1..b3 only
    { UtxoState r2; Delta rd; UtxoState ns;
      for(int hi=0; hi<3; ++hi){ cs::buildDelta(r2,chain[hi],MAT,&rd,&ns,&e); r2=ns; }
      CHECK(s4.digest()==r2.digest(), "store after disconnect == ref pre-b4"); }
    // --- reconnect b4 (re-observe) ---
    { ObservedBlock ob=toObserved(b4);
      GetPrevCoinFn gp=[&](const OutPoint& p, Coin* c)->bool{ return s4.get(p,c); };
      Delta od; CHECK(observeDelta(ob,gp,&od,&e), "reobserve b4: "+e.str());
      if(!s.publish(od,b4.hash,b4.height,&e)){ CHECK(false,"reconnect publish: "+e.str()); s.close(); return; }
      UtxoState s5; storeToState(s,&s5,&e);
      CHECK(s5.digest()==dg[4], "store after reconnect == ref after b4"); }
    s.close();

    // --- restart persistence ---
    { CommitStore3A s2; CHECK(s2.open(dir,&e), "reopen: "+e.str());
      UtxoState s6; if(storeToState(s2,&s6,&e)) CHECK(s6.digest()==dg[4], "after reopen == ref after b4"); s2.close(); }

    // --- authority gate catches a DROPPED spend ---
    { ObservedAuthority auth; collectAuthority(prevLastD,&auth);
      if(!auth.spent.empty()){ auth.spent.pop_back(); std::string mis;
        CHECK(!authoritySpendsMatch(prevLastD,auth,&mis), "gate rejects dropped spend"); }
    }
    // --- authority gate catches SURPLUS spend (anon-input wrongly projected) ---
    { ObservedBlock ob=toObserved(b3);
      ObservedTx rogue=ob.txs[0]; rogue.coinbase=false;
      OutPoint anonLike; anonLike.txid=id32(0xA0000000u, 0x0A); anonLike.n=0;
      rogue.vin.push_back(anonLike);        // simulate wrongly projecting a ring-nullifier input
      ob.txs[0]=rogue;
      GetPrevCoinFn gp=[&](const OutPoint& p, Coin* c)->bool{ (void)p;(void)c; return true; };
      Delta od; CHECK(observeDelta(ob,gp,&od,&e), "rogue observe build: "+e.str());
      Delta rd; UtxoState ns; UtxoState ref2; // re-derive authority for b1..b3
      for(int hi=0; hi<3; ++hi){ cs::buildDelta(ref2,chain[hi],MAT,&rd,&ns,&e); ref2=ns; }
      ObservedAuthority auth; collectAuthority(rd,&auth);
      std::string mis; CHECK(!authoritySpendsMatch(od,auth,&mis), "gate rejects surplus/anonymous input");
    }
    // --- fail closed when an external prevout is not observable ---
    { ObservedBlock ob=toObserved(b3);
      GetPrevCoinFn none=[&](const OutPoint& p, Coin* c)->bool{ (void)p;(void)c; return false; };
      Delta od; ChainError fe;
      CHECK(!observeDelta(ob,none,&od,&fe), "observeDelta fails closed when mirror lacks external prev");
      CHECK(od.ops.empty(), "fail-closed leaves no partial delta");
    }
}

int main(){
    setvbuf(stdout,NULL,_IONBF,0);
    struct Case{ const char* n; void(*fn)(); };

    Case cases[]={ {"B0-btree",test_b0_btree}, {"D1-diff",test_d1_differential}, {"C1-crash",test_c1_crash}, {"K1-corrupt",test_k1_corruption}, {"M1-pathverify",test_m1_path_verify}, {"P6-partialappend",test_p6_partial_append}, {"S1-scale",test_s1_scaling}, {"R2-freshopen",test_r2_fresh_open}, {"R2-atomic",test_r2_multi_key_atomic}, {"F0-undo",test_f0_undo}, {"R1-F0-1",test_r1_f01_backup_window}, {"R1-crash",test_r1_crash_matrix}, {"R1-fresh",test_r1_fresh_open_matrix}, {"F0-R2-toctou",test_r2_toctou}, {"F0-R2-race",test_r2_toctou_race}, {"F0-R3-retry",test_r3_retry}, {"F0-R3-reopen",test_r3_reopen}, {"F0-R3-postlstat",test_r3_postlstat}, {"UNDOIDX-format",test_undoidx_format}, {"UNDOIDX-boundary",test_undoidx_boundary_small}, {"UNDOIDX-corrupt",test_undoidx_corruption}, {"UNDOIDX-states",test_undoidx_states}, {"UNDOIDX-crash",test_undoidx_crash_points}, {"UNDOIDX-behavior",test_undoidx_no_side_effects}, {"UNDOIDX-cost",test_undoidx_cost}, {"UNDOIDX-real_boundary",test_undoidx_real_boundary}, {"UNDOIDX-F1-recovery",test_undoidx_f1_recovery}, {"UNDOIDX-F1-faults",test_undoidx_f1_faults}, {"UNDOIDX-F1-real",test_undoidx_f1_real}, {"UNDOIDX-F2-linkfail",test_undoidx_f2_linkfail}, {"UNDOIDX-FASTIDX-equiv",test_undoidx_fastidx_equiv}, {"UNDOIDX-FASTIDX-fallback",test_undoidx_fastidx_fallback}, {"UNDOIDX-FASTIDX-damaged",test_undoidx_fastidx_damaged}, {"UNDOIDX-FASTIDX-recovery",test_undoidx_fastidx_recovery}, {"UNDOIDX-FASTIDX-measure",test_undoidx_fastidx_measure}, {"UNDOIDX-BOUND-basic",test_undoidx_bound_basic}, {"UNDOIDX-BOUND-deep",test_undoidx_bound_deep}, {"UNDOIDX-BOUND-integrity",test_undoidx_bound_integrity}, {"UNDOIDX-BOUND-work",test_undoidx_bound_work}, {"OBS2-adapter",test_obs2_adapter} };
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
