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
        s.close();
        printf("  S1 N=%d treeBytes=%ld bytesPerCommit=%ld msPerCommit=%.3f peakRSS_KB=%ld\n", N, after, perCommit[si], msPerCommit[si], readVmHWM());
        (void)nstate;
    }
    // O(delta) criterion: per-commit write volume must NOT grow ~linearly with state size (4x from N=200..800)
    CHECK(perCommit[2] < perCommit[0]*3, "per-commit bytes do not grow linearly with state (800 vs 200)");
    printf("  S1 perCommit: 200=%ld 400=%ld 800=%ld bytes\n", perCommit[0], perCommit[1], perCommit[2]);
}

static long readVmHWM(){ FILE*f=fopen("/proc/self/status","r"); if(!f)return -1; char l[256]; long kb=-1; while(fgets(l,sizeof(l),f)){ if(strncmp(l,"VmHWM:",6)==0){ sscanf(l+6,"%ld",&kb); break; } } fclose(f); return kb; }

int main(){
    setvbuf(stdout,NULL,_IONBF,0);
    struct Case{ const char* n; void(*fn)(); };
    Case cases[]={ {"B0-btree",test_b0_btree}, {"D1-diff",test_d1_differential}, {"C1-crash",test_c1_crash}, {"K1-corrupt",test_k1_corruption}, {"S1-scale",test_s1_scaling} };
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
