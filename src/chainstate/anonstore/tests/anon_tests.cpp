// ANONSTATE isolated slice - focused test suite. Single TU with its own main().
// Per-case isolated base dir from env ANONSTORE_BASE (default /tmp/anonstore-rt).
// Covers: put/get/erase round-trip, persist/reopen (fresh & pre-existing), disconnect
// revert, two-connect/two-disconnect reorg, crash-injection matrix (process crash + fault
// injection), duplicate-anon fail-closed, key-image overwrite, valueCount accounting.
#include "anonstore.h"
#include "anonfeed.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>

using namespace cs;
using namespace cs::anon;

static int g_pass=0, g_fail=0; static std::vector<std::string> g_failmsg; static std::string g_cur;
#define CHECK(c,m) do{ if(c) g_pass++; else { g_fail++; g_failmsg.push_back(g_cur+": "+(m)); } }while(0)

static std::string baseDir(){
    const char* o=getenv("ANONSTORE_BASE"); if(o&&*o){ std::string s=o; if(s[s.size()-1]!='/') s+="/"; return s; }
    return "/tmp/anonstore-rt/";
}
static std::string rmTree(const std::string& d){ std::string c="rm -rf '"+d+"'"; (void)system(c.c_str()); return d; }
static std::string freshDir(const std::string& name){
    std::string b=baseDir(); std::string d=b+name; rmTree(d);
    ::mkdir(b.c_str(),0755); ::mkdir(d.c_str(),0755); return d;
}
static long fileSize(const std::string& p){ struct stat st; return stat(p.c_str(),&st)==0?(long)st.st_size:-1; }

// deterministic 32-byte id
static Hash id32(uint32_t label, uint32_t i){
    uint8_t b[8]; for(int k=0;k<4;k++){b[k]=(uint8_t)((label>>(8*k))&0xff);} for(int k=0;k<4;k++){b[4+k]=(uint8_t)((i>>(8*k))&0xff);}
    return sha256(b,8);
}
static std::array<uint8_t,33> key33(uint8_t b0){
    std::array<uint8_t,33> k{}; Hash h=id32(0x100, b0); memcpy(k.data(),h.data(),32); k[32]=(uint8_t)(b0*7+1); return k;
}
static CAnonOutput mkAo(uint32_t txlabel, uint32_t n, int64_t val, int32_t h, uint8_t comp){
    CAnonOutput o; o.txid=id32(txlabel,n); o.n=n; o.nValue=val; o.nBlockHeight=h; o.nCompromised=comp; return o;
}
static CKeyImageSpent mkKi(uint32_t txlabel, uint32_t in, int64_t val){
    CKeyImageSpent k; k.txnHash=id32(txlabel,in); k.inputNo=in; k.nValue=val; return k;
}
static Mutation putAoMut(const std::array<uint8_t,33>& key, const CAnonOutput& ao){
    Mutation m; m.ns=ANON_OUTPUT_NS; m.key=key; m.beforePresent=false; m.before.clear();
    m.afterPresent=true; m.after=ao.encode(); return m;
}
static Mutation putKiMut(const std::array<uint8_t,33>& key, const CKeyImageSpent& ki){
    Mutation m; m.ns=KEY_IMAGE_NS; m.key=key; m.beforePresent=false; m.before.clear();
    m.afterPresent=true; m.after=ki.encode(); return m;
}
static Mutation putKiOverMut(const std::array<uint8_t,33>& key, const CKeyImageSpent& before, const CKeyImageSpent& after){
    Mutation m; m.ns=KEY_IMAGE_NS; m.key=key; m.beforePresent=true; m.before=before.encode();
    m.afterPresent=true; m.after=after.encode(); return m;
}
static Mutation eraseMut(const std::array<uint8_t,33>& key, uint8_t ns, const std::vector<uint8_t>& before){
    Mutation m; m.ns=ns; m.key=key; m.beforePresent=true; m.before=before;
    m.afterPresent=false; m.after.clear(); return m;
}

// ============================ test 1: round-trip ==============================
static void test_put_get_erase(){
    g_cur="T1-roundtrip";
    std::string d=freshDir("t1"); ChainError e;
    AnonStore s; CHECK(s.open(d,&e), "open: "+e.str());
    std::array<uint8_t,33> kA=key33(1), kB=key33(2), kC=key33(3);
    CAnonOutput ao1=mkAo(0xAA000000,1,123456789,100,0);
    CAnonOutput ao2=mkAo(0xBB000000,2,987654321,101,1);
    CKeyImageSpent ki1=mkKi(0xCC000000,5,555);
    std::vector<Mutation> b1; b1.push_back(putAoMut(kA,ao1)); b1.push_back(putKiMut(kC,ki1));
    CHECK(s.connect(id32(0x100000,1),1,b1,&e), "connect b1: "+e.str());
    std::vector<Mutation> b2; b2.push_back(putAoMut(kB,ao2));
    CHECK(s.connect(id32(0x100000,2),2,b2,&e), "connect b2: "+e.str());
    bool f=false; CAnonOutput ra;
    CHECK(s.getAo(kA.data(),&ra,&f,&e)&&f, "getAo kA found: "+e.str());
    CHECK(ra.txid==ao1.txid && ra.n==ao1.n && ra.nValue==ao1.nValue && ra.nBlockHeight==ao1.nBlockHeight && ra.nCompromised==ao1.nCompromised, "getAo kA value equality");
    CAnonOutput rb; CHECK(s.getAo(kB.data(),&rb,&f,&e)&&f, "getAo kB found");
    CHECK(rb.nValue==ao2.nValue && rb.nCompromised==ao2.nCompromised, "getAo kB value equality");
    CKeyImageSpent rk; CHECK(s.getKi(kC.data(),&rk,&f,&e)&&f, "getKi kC found: "+e.str());
    CHECK(rk.txnHash==ki1.txnHash && rk.inputNo==ki1.inputNo && rk.nValue==ki1.nValue, "getKi kC value equality");
    CHECK(s.getAo(kC.data(),&ra,&f,&e)&&!f, "key kC is in ki-ns, so getAo absent");
    CHECK(s.getKi(kA.data(),&rk,&f,&e)&&!f, "key kA is in ao-ns, so getKi absent");
    // erase kA
    std::vector<Mutation> b3; b3.push_back(eraseMut(kA,ANON_OUTPUT_NS,ao1.encode()));
    CHECK(s.connect(id32(0x100000,3),3,b3,&e), "connect (erase) b3: "+e.str());
    CHECK(s.getAo(kA.data(),&ra,&f,&e)&&!f, "kA gone after erase");
    CHECK(s.getAo(kB.data(),&ra,&f,&e)&&f, "kB still present");
    CHECK(s.tree().verifyAll(&e), "merkle verify: "+e.str());
    s.close();
}

// ============================ test 2: persist/reopen ==========================
static void test_persist_reopen(){
    g_cur="T2-persist";
    // fresh store, connect, reopen
    { std::string d=freshDir("t2a"); ChainError e;
      { AnonStore s; CHECK(s.open(d,&e),"t2a fresh open: "+e.str());
        std::array<uint8_t,33> k=key33(1); CAnonOutput ao=mkAo(0xAA000000,1,42,5,0);
        std::vector<Mutation> b1; b1.push_back(putAoMut(k,ao));
        CHECK(s.connect(id32(0x100000,1),5,b1,&e),"t2a connect: "+e.str());
        CHECK(s.valueCount()==1,"t2a valueCount==1"); s.close(); }
      { AnonStore r; CHECK(r.open(d,&e),"t2a reopen: "+e.str());
        std::array<uint8_t,33> k=key33(1); bool f=false; CAnonOutput ao;
        CHECK(r.getAo(k.data(),&ao,&f,&e)&&f && ao.nValue==42, "t2a value persisted");
        CHECK(r.generation()==1 && r.tipHash()==id32(0x100000,1) && r.tipHeight()==5, "t2a tip/gen persisted");
        CHECK(r.valueCount()==1, "t2a valueCount persisted"); r.close(); }
    }
    // pre-existing store: reopen then connect more, reopen again
    { std::string d=freshDir("t2b"); ChainError e;
      { AnonStore s; CHECK(s.open(d,&e),"t2b open: "+e.str());
        std::array<uint8_t,33> k1=key33(1),k2=key33(2);
        CAnonOutput ao1=mkAo(0xAA000000,1,10,3,0);
        std::vector<Mutation> b1; b1.push_back(putAoMut(k1,ao1));
        CHECK(s.connect(id32(0x100000,1),3,b1,&e),"t2b connect b1"); s.close(); }
      { AnonStore s; CHECK(s.open(d,&e),"t2b reopen1: "+e.str());
        std::array<uint8_t,33> k2=key33(2); CAnonOutput ao2=mkAo(0xBB000000,2,20,4,1);
        std::vector<Mutation> b2; b2.push_back(putAoMut(k2,ao2));
        CHECK(s.connect(id32(0x100000,2),4,b2,&e),"t2b connect b2 (on pre-existing): "+e.str());
        CHECK(s.valueCount()==2,"t2b valueCount==2"); s.close(); }
      { AnonStore r; CHECK(r.open(d,&e),"t2b reopen2: "+e.str());
        CHECK(r.valueCount()==2 && r.tipHeight()==4, "t2b both blocks persisted");
        CHECK(r.tree().verifyAll(&e),"t2b merkle verify: "+e.str());
        bool f=false; CAnonOutput ao;
        std::array<uint8_t,33> k1=key33(1);
        CHECK(r.getAo(k1.data(),&ao,&f,&e)&&f && ao.nValue==10, "t2b block1 value persists across reopens");
        std::array<uint8_t,33> k2=key33(2);
        CHECK(r.getAo(k2.data(),&ao,&f,&e)&&f && ao.nValue==20, "t2b block2 value persists");
        r.close(); }
    }
}

// ============================ test 3: disconnect revert =======================
static void test_disconnect_revert(){
    g_cur="T3-disconnect";
    std::string d=freshDir("t3"); ChainError e;
    AnonStore s; CHECK(s.open(d,&e), "t3 open: "+e.str());
    std::array<uint8_t,33> k1=key33(1), k2=key33(2);
    CAnonOutput ao1=mkAo(0xAA000000,1,100,1,0), ao2=mkAo(0xBB000000,2,200,2,0);
    std::vector<Mutation> b1; b1.push_back(putAoMut(k1,ao1));
    CHECK(s.connect(id32(0x100000,1),1,b1,&e), "t3 connect b1: "+e.str());
    Hash parentRoot=s.tree().rootHash(); uint64_t parentGen=s.generation(); Hash parentTip=s.tipHash();
    std::vector<Mutation> b2; b2.push_back(putAoMut(k2,ao2));
    CHECK(s.connect(id32(0x100000,2),2,b2,&e), "t3 connect b2: "+e.str());
    CHECK(s.valueCount()==2, "t3 valueCount==2");
    CHECK(s.disconnect(id32(0x100000,2),parentTip,&e), "t3 disconnect b2: "+e.str());
    CHECK(s.tree().rootHash()==parentRoot, "t3 rootHash returns to parent on disconnect");
    CHECK(s.tipHash()==parentTip && s.tipHeight()==1, "t3 tip back to parent");
    bool f=false; CAnonOutput ra; CHECK(s.getAo(k2.data(),&ra,&f,&e)&&!f, "t3 b2 value gone");
    CHECK(s.getAo(k1.data(),&ra,&f,&e)&&f && ra.nValue==100, "t3 b1 value retained");
    CHECK(s.valueCount()==1, "t3 valueCount back to 1");
    CHECK(s.generation()==parentGen+2, "t3 generation advanced on disconnect (connect b2 + disconnect = +2 gen)");
    CHECK(s.tree().verifyAll(&e), "t3 merkle verify: "+e.str());
    s.close();
}

// ============================ test 4: two-connect / two-disconnect reorg ======
static void test_reorg(){
    g_cur="T4-reorg";
    std::string d=freshDir("t4"); ChainError e;
    AnonStore s; CHECK(s.open(d,&e), "t4 open: "+e.str());
    std::array<uint8_t,33> k1=key33(1), k2=key33(2), k3=key33(3);
    CAnonOutput a1=mkAo(0xAA000000,1,10,1,0), a2=mkAo(0xBB000000,2,20,2,0), a3=mkAo(0xCC000000,3,30,3,0);
    std::vector<Mutation> b1,b2,b3; b1.push_back(putAoMut(k1,a1)); b2.push_back(putAoMut(k2,a2)); b3.push_back(putAoMut(k3,a3));
    CHECK(s.connect(id32(0x100000,1),1,b1,&e),"t4 connect b1");
    Hash h1=s.tipHash();
    CHECK(s.connect(id32(0x100000,2),2,b2,&e),"t4 connect b2");
    Hash h2=s.tipHash();
    CHECK(s.connect(id32(0x100000,3),3,b3,&e),"t4 connect b3");
    CHECK(s.valueCount()==3, "t4 valueCount==3");
    CHECK(s.disconnect(id32(0x100000,3),h2,&e),"t4 disconnect b3");
    CHECK(s.disconnect(id32(0x100000,2),h1,&e),"t4 disconnect b2");
    bool f=false; CAnonOutput ra;
    CHECK(s.getAo(k1.data(),&ra,&f,&e)&&f, "t4 k1 retained");
    CHECK(s.getAo(k2.data(),&ra,&f,&e)&&!f, "t4 k2 gone");
    CHECK(s.getAo(k3.data(),&ra,&f,&e)&&!f, "t4 k3 gone");
    CHECK(s.valueCount()==1, "t4 valueCount==1");
    CHECK(s.tipHash()==h1 && s.tipHeight()==1, "t4 after 2 disconnects state == height 1");
    CHECK(s.tree().verifyAll(&e), "t4 merkle verify: "+e.str());
    s.close();
}

// ============================ test 8: valueCount accounting ===================
static void test_valuecount(){
    g_cur="T8-valuecount";
    std::string d=freshDir("t8"); ChainError e;
    AnonStore s; CHECK(s.open(d,&e), "t8 open: "+e.str());
    std::array<uint8_t,33> k1=key33(1), k2=key33(2), k3=key33(3), k4=key33(4);
    CAnonOutput a1=mkAo(1,1,1,1,0), a2=mkAo(2,2,2,1,0), a3=mkAo(3,3,3,1,1);
    CKeyImageSpent ki1=mkKi(10,1,100), ki2=mkKi(11,2,200);
    std::vector<Mutation> b1; b1.push_back(putAoMut(k1,a1)); b1.push_back(putAoMut(k2,a2)); b1.push_back(putKiMut(k3,ki1));
    CHECK(s.connect(id32(0x200000,1),1,b1,&e),"t8 b1: "+e.str());
    CHECK(s.valueCount()==3, "t8 after b1 ==3");
    std::vector<Mutation> b2; b2.push_back(putAoMut(k4,a3)); b2.push_back(putKiOverMut(k3,ki1,ki2));
    CHECK(s.connect(id32(0x200000,2),2,b2,&e),"t8 b2: "+e.str());
    CHECK(s.valueCount()==4, "t8 after b2 ==4 (overwrite of ki doesn't add)");
    CKeyImageSpent rk; bool f=false; CHECK(s.getKi(k3.data(),&rk,&f,&e)&&f&&rk.nValue==200, "t8 ki overwritten to 200");
    std::vector<Mutation> b3; b3.push_back(eraseMut(k1,ANON_OUTPUT_NS,a1.encode()));
    CHECK(s.connect(id32(0x200000,3),3,b3,&e),"t8 b3: "+e.str());
    CHECK(s.valueCount()==3, "t8 after b3 ==3");
    CHECK(s.disconnect(id32(0x200000,3),id32(0x200000,2),&e),"t8 disconnect b3");
    CHECK(s.valueCount()==4, "t8 valueCount back to 4 after disconnect");
    s.close();
}

// ============================ tests 6 & 7: dup + overwrite ====================
static void test_dup_and_overwrite(){
    g_cur="T6-dup";
    std::string d=freshDir("t6"); ChainError e;
    { AnonStore s; CHECK(s.open(d,&e), "t6 open: "+e.str());
      std::array<uint8_t,33> k=key33(1); CAnonOutput a1=mkAo(1,1,100,1,0), a2=mkAo(1,1,999,1,0);
      std::vector<Mutation> b1; b1.push_back(putAoMut(k,a1));
      CHECK(s.connect(id32(0x300000,1),1,b1,&e), "t6 connect b1");
      ChainError e2; std::vector<Mutation> b2; b2.push_back(putAoMut(k,a2));
      bool ok=s.connect(id32(0x300000,2),2,b2,&e2);
      CHECK(!ok, "t6 duplicate-anon-output at DIFFERENT value refused: "+e2.str());
      CHECK(ok || e2.code==ErrCode::DUP_OUTPOINT, "t6 dup error code = DUP_OUTPOINT");
      ChainError e3; std::vector<Mutation> b2b; b2b.push_back(putAoMut(k,a1));
      CHECK(s.connect(id32(0x300000,2),2,b2b,&e3), "t6 identical-value re-put allowed: "+e3.str());
      bool f=false; CAnonOutput ra; CHECK(s.getAo(k.data(),&ra,&f,&e)&&f && ra.nValue==100, "t6 tree still holds original after refused dup");
      CHECK(s.valueCount()==1, "t6 valueCount==1");
      s.close(); }
    { AnonStore r; CHECK(r.open(d,&e), "t6 reopen: "+e.str());
      bool f=false; CAnonOutput ao; std::array<uint8_t,33> k=key33(1);
      CHECK(r.getAo(k.data(),&ao,&f,&e)&&f && ao.nValue==100, "t6 persisted original");
      r.close(); }
}

static void test_ki_overwrite(){
    g_cur="T7-ki-overwrite";
    std::string d=freshDir("t7"); ChainError e;
    AnonStore s; CHECK(s.open(d,&e), "t7 open: "+e.str());
    std::array<uint8_t,33> k=key33(5);
    CKeyImageSpent ki1=mkKi(20,1,111), ki2=mkKi(21,9,999);
    std::vector<Mutation> b1; b1.push_back(putKiMut(k,ki1));
    CHECK(s.connect(id32(0x400000,1),1,b1,&e),"t7 connect b1");
    std::vector<Mutation> b2; b2.push_back(putKiOverMut(k,ki1,ki2));
    CHECK(s.connect(id32(0x400000,2),2,b2,&e),"t7 connect b2 (overwrite): "+e.str());
    CKeyImageSpent rk; bool f=false; CHECK(s.getKi(k.data(),&rk,&f,&e)&&f&&rk.nValue==999&&rk.inputNo==9, "t7 overwritten value visible");
    CHECK(s.valueCount()==1, "t7 valueCount stays 1 across overwrite");
    CHECK(s.disconnect(id32(0x400000,2),id32(0x400000,1),&e),"t7 disconnect b2");
    CHECK(s.getKi(k.data(),&rk,&f,&e)&&f&&rk.nValue==111&&rk.inputNo==1, "t7 ki reverted to b1 value");
    CHECK(s.valueCount()==1,"t7 valueCount still 1");
    s.close();
}

// ============================ test 5: crash matrix ============================
// setup: committed block1 = Ao(kA, val=1) at height 1 (the OLD committed state).
// crash child: connect block2 = Ao(kB, val=777) at height 2 under a crash/injection.
// After reopen the store must be OLD (gen1: has kA, no kB) or NEW (gen2: has kA+kB), never torn.
static Hash B1H(){ return id32(0x500000,1); }
static Hash B2H(){ return id32(0x500000,2); }
static void setupBase(const std::string& d){
    ChainError e; AnonStore s; s.open(d,&e);
    std::array<uint8_t,33> kA=key33(7);
    std::vector<Mutation> b1; b1.push_back(putAoMut(kA,mkAo(0xAA000000,1,1,1,0)));
    s.connect(B1H(),1,b1,&e);
    s.close();
}
static void crashChild(const std::string& d, CrashPoint cp, const std::string& mode, size_t cut){
    ChainError e; AnonStore s; if(!s.open(d,&e)) ::_exit(2);
    std::array<uint8_t,33> kB=key33(8);
    std::vector<Mutation> b2; b2.push_back(putAoMut(kB,mkAo(0xBB000000,2,777,2,0)));
    if(mode=="crash")      s.setCrashPoint(cp);
    else if(mode=="nospace"){ s.setInjectStage(InjectStage::UndoAppend); s.injectNoSpace(true); }
    else if(mode=="fsyncfail"){ s.setInjectStage(InjectStage::UndoAppend); s.injectFsyncFail(true); }
    else if(mode=="partial"){ s.injectPartialAppendOnce(cut); }
    else if(mode=="shortundo"){ s.injectUndoShortOnce(cut); }
    bool ok=s.connect(B2H(),2,b2,&e);
    if(mode=="crash") ::_exit(42);
    ::_exit(ok?0:1);
}
// reopen + classify: returns true if open ok; sets isOld/isNew by presence+gen+tip
static bool classify(const std::string& d, ChainError* e, bool* isOld, bool* isNew){
    *isOld=false; *isNew=false;
    AnonStore r; if(!r.open(d,e)) return false;
    std::array<uint8_t,33> kA=key33(7), kB=key33(8);
    bool fA=false,fB=false; CAnonOutput ao;
    bool ok=r.getAo(kA.data(),&ao,&fA,e); if(!ok){ r.close(); return false; }
    ok=r.getAo(kB.data(),&ao,&fB,e); if(!ok){ r.close(); return false; }
    if(r.generation()==1 && r.tipHeight()==1 && r.tipHash()==B1H() && fA && !fB && r.valueCount()==1) *isOld=true;
    if(r.generation()==2 && r.tipHeight()==2 && r.tipHash()==B2H() && fA && fB && r.valueCount()==2) *isNew=true;
    // the committed root must be reachable + merkle-verified (never torn)
    ok=r.tree().verifyAll(e); r.close(); return ok;
}

static void test_crash_matrix(){
    g_cur="T5-crash";
    ChainError e;
    struct CP { const char* name; CrashPoint cp; bool mustOld; bool mustNew; };
    CP cps[]={
        {"BeforeUndoWrite", CrashPoint::BeforeUndoWrite, true,  false},
        {"AfterUndoWrite",  CrashPoint::AfterUndoWrite,  false, false},  // old or new
        {"BeforeRename",    CrashPoint::BeforeRename,    false, false},  // old or new
        {"AfterRename",     CrashPoint::AfterRename,     false, false},  // new visible but ACK pending
        {"AfterDirSync",    CrashPoint::AfterDirSync,    false, true},   // fully committed
    };
    for (CP& c: cps){
        std::string d=freshDir(std::string("t5_")+c.name);
        setupBase(d);
        pid_t pid=fork();
        if(pid==0){ crashChild(d, c.cp, "crash", 0); ::_exit(7); }
        int st=0; waitpid(pid,&st,0);
        CHECK(WIFEXITED(st) && WEXITSTATUS(st)==42, std::string(c.name)+" child crashed at barrier (rc="+std::to_string(WIFEXITED(st)?WEXITSTATUS(st):-1)+")");
        bool o=false,n=false;
        CHECK(classify(d,&e,&o,&n), std::string(c.name)+" reopen + committed root verifies: "+e.str());
        bool okState = (c.mustOld&&o) || (c.mustNew&&n) || (!c.mustOld&&!c.mustNew&&(o||n));
        CHECK(okState, std::string(c.name)+" recovered exactly OLD or NEW (never torn), old="+std::to_string(o)+" new="+std::to_string(n));
        CHECK(!(o&&n), std::string(c.name)+" not simultaneously old+new");
        CHECK(fileSize(d+"/tree.dat")%(long)4096==0, std::string(c.name)+" tree.dat page-aligned after recovery");
        bool o2=false,n2=false; CHECK(classify(d,&e,&o2,&n2), std::string(c.name)+" reopen2");
        CHECK(o2==o && n2==n, std::string(c.name)+" repeated recovery idempotent");
    }
    // fault-injection matrix (no process kill; connect returns false; committed stays OLD)
    struct Inj { const char* name; const char* mode; size_t cut; };
    Inj injs[]={
        {"nospace-undo",   "nospace",   0},
        {"fsyncfail-undo", "fsyncfail", 0},
        {"partial-page",   "partial",   2000},
        {"short-undo",     "shortundo", 20},
    };
    for (Inj& ij: injs){
        std::string d=freshDir(std::string("t5i_")+ij.name);
        setupBase(d);
        pid_t pid=fork();
        if(pid==0){ crashChild(d, CrashPoint::None, ij.mode, ij.cut); ::_exit(7); }
        int st=0; waitpid(pid,&st,0);
        CHECK(WIFEXITED(st) && WEXITSTATUS(st)==1, std::string(ij.name)+" child connect returned false (injected fault)");
        bool o=false,n=false; CHECK(classify(d,&e,&o,&n), std::string(ij.name)+" reopen: "+e.str());
        CHECK(o && !n, std::string(ij.name)+" after injected fault committed stays OLD (gen1)");
        CHECK(fileSize(d+"/tree.dat")%(long)4096==0, std::string(ij.name)+" tree page-aligned");
    }
    // ENOSPC at MANIFEST.tmp (interrupted MANIFEST write)
    { std::string d=freshDir("t5i_mtmp"); setupBase(d);
      { AnonStore s; ChainError ce; if(!s.open(d,&ce)){ CHECK(false,"mtmp open: "+ce.str()); }
        else {
          std::array<uint8_t,33> kB=key33(8);
          std::vector<Mutation> b2; b2.push_back(putAoMut(kB,mkAo(0xBB000000,2,777,2,0)));
          s.setInjectStage(InjectStage::ManifestTmp); s.injectNoSpace(true);
          bool ok=s.connect(B2H(),2,b2,&ce);
          CHECK(!ok, "mtmp connect refused at MANIFEST.tmp: "+ce.str());
          s.clearInjection(); s.close(); } }
      bool o=false,n=false; CHECK(classify(d,&e,&o,&n),"mtmp reopen: "+e.str());
      CHECK(o && !n, "mtmp committed stays OLD (gen1) after interrupted MANIFEST write");
    }
    // ENOSPC at DirFsync: rename already passed (new MANIFEST may be visible); after cleanup the
    // recovered state must be a CONSISTENT old or new (dir fsync failure fails the commit, but the
    // manifest rename is what makes the new gen authoritative on disk).
    { std::string d=freshDir("t5i_dirfs"); setupBase(d);
      { AnonStore s; ChainError ce; if(!s.open(d,&ce)){ CHECK(false,"dirfs open: "+ce.str()); }
        else {
          std::array<uint8_t,33> kB=key33(8);
          std::vector<Mutation> b2; b2.push_back(putAoMut(kB,mkAo(0xBB000000,2,777,2,0)));
          s.setInjectStage(InjectStage::DirFsync); s.injectFsyncFail(true);
          bool ok=s.connect(B2H(),2,b2,&ce);
          CHECK(!ok, "dirfs connect refused at dir fsync: "+ce.str());
          s.clearInjection(); s.close(); } }
      bool o=false,n=false; CHECK(classify(d,&e,&o,&n), "dirfs reopen: "+e.str());
      CHECK(o||n, "dirfs recovered OLD or NEW, never torn");
    }
}

// ======================= seed (slice-c migration base) tests =======================
static void test_seed_roundtrip(){
    g_cur="T9-seed";
    std::string d=freshDir("t9"); ChainError e;
    Hash tip=id32(0x900000,100);
    { AnonStore s; CHECK(s.open(d,&e), "t9 open: "+e.str());
      std::array<uint8_t,33> k1=key33(9), k2=key33(10), k3=key33(11);
      CHECK(!s.seeded(), "t9 not seeded before finalize");
      CHECK(s.seedAo(k1.data(),mkAo(1,1,100,3,0),&e), "t9 seedAo k1: "+e.str());
      CHECK(s.seedAo(k2.data(),mkAo(2,2,200,4,1),&e), "t9 seedAo k2: "+e.str());
      CHECK(s.seedKi(k3.data(),mkKi(5,1,50),&e), "t9 seedKi k3: "+e.str());
      CHECK(s.finalizeSeed(tip,100,&e), "t9 finalize: "+e.str());
      CHECK(s.seeded(), "t9 seeded after finalize");
      CHECK(s.generation()==1 && s.tipHash()==tip && s.tipHeight()==100, "t9 gen/tip/height after finalize");
      CHECK(s.valueCount()==3, "t9 valueCount==3");
      ChainError e2; bool ok=s.seedAo(k1.data(),mkAo(1,1,100,3,0),&e2);
      CHECK(!ok, "t9 seed after finalize rejected: "+e2.str());
      s.close(); }
    { AnonStore r; CHECK(r.open(d,&e), "t9 reopen: "+e.str());
      CHECK(r.seeded(), "t9 reopen seeded()==true");
      bool f=false; CAnonOutput ao; std::array<uint8_t,33> k1=key33(9);
      CHECK(r.getAo(k1.data(),&ao,&f,&e)&&f && ao.nValue==100 && ao.nBlockHeight==3, "t9 k1 persisted after reopen");
      CHECK(r.valueCount()==3, "t9 valueCount persisted");
      CHECK(r.tree().verifyAll(&e), "t9 merkle verify: "+e.str());
      r.close(); }
}
static void seedCrashChild(const std::string& d, int mode){
    ChainError e; AnonStore s; if(!s.open(d,&e)) ::_exit(2);
    bool ok=s.seedAo(key33(9).data(),mkAo(1,1,100,3,0),&e);
    ok=ok && s.seedKi(key33(11).data(),mkKi(5,1,50),&e);
    if(!ok) ::_exit(1);
    if(mode==1){ ::_exit(42); }                       // crash after seeding, before finalize
    if(mode==2){ s.setCrashPoint(CrashPoint::BeforeRename);  s.finalizeSeed(id32(0x900000,100),100,&e); ::_exit(42); }
    if(mode==3){ s.setCrashPoint(CrashPoint::AfterRename);   s.finalizeSeed(id32(0x900000,100),100,&e); ::_exit(42); }
    ::_exit(0);
}
static void test_seed_crash(){
    g_cur="T10-seedcrash"; ChainError e;
    { // crash before finalize: store stays empty gen0; re-run completes
      std::string d=freshDir("t10a");
      pid_t pid=fork(); if(pid==0){ seedCrashChild(d,1); ::_exit(7); }
      int st; waitpid(pid,&st,0);
      { AnonStore r; CHECK(r.open(d,&e), "t10a reopen: "+e.str());
        CHECK(r.generation()==0 && r.valueCount()==0 && !r.seeded(), "t10a crash-before-finalize => empty gen0");
        bool f=false; CAnonOutput ao;
        CHECK((!r.getAo(key33(9).data(),&ao,&f,&e)) || !f, "t10a k1 absent after crash-before-finalize");
        r.close(); }
      { AnonStore s; CHECK(s.open(d,&e), "t10a reseed open: "+e.str());
        CHECK(s.seedAo(key33(9).data(),mkAo(1,1,100,3,0),&e), "t10a reseedAo: "+e.str());
        CHECK(s.seedKi(key33(11).data(),mkKi(5,1,50),&e), "t10a reseedKi: "+e.str());
        CHECK(s.finalizeSeed(id32(0x900000,100),100,&e), "t10a refinalize: "+e.str());
        s.close(); }
      { AnonStore r; CHECK(r.open(d,&e), "t10a final reopen: "+e.str());
        CHECK(r.seeded() && r.valueCount()==2, "t10a re-run seed+finalize completes (2 entries)");
        r.close(); }
    }
    { // crash during finalize before rename: seed NOT committed (gen0 empty)
      std::string d=freshDir("t10b");
      pid_t pid=fork(); if(pid==0){ seedCrashChild(d,2); ::_exit(7); }
      int st; waitpid(pid,&st,0);
      { AnonStore r; CHECK(r.open(d,&e), "t10b reopen: "+e.str());
        CHECK(r.generation()==0 && r.valueCount()==0, "t10b crash-before-rename => seed not committed (gen0)");
        r.close(); }
    }
    { // crash during finalize after rename: fully seeded (committed)
      std::string d=freshDir("t10c");
      pid_t pid=fork(); if(pid==0){ seedCrashChild(d,3); ::_exit(7); }
      int st; waitpid(pid,&st,0);
      { AnonStore r; CHECK(r.open(d,&e), "t10c reopen: "+e.str());
        CHECK(r.seeded() && r.generation()==1 && r.valueCount()==2, "t10c crash-after-rename => seed committed (gen1)");
        r.close(); }
    }
}
static void test_seed_dup(){
    g_cur="T11-seeddup"; std::string d=freshDir("t11"); ChainError e;
    AnonStore s; CHECK(s.open(d,&e), "t11 open: "+e.str());
    std::array<uint8_t,33> k=key33(9);
    CHECK(s.seedAo(k.data(),mkAo(1,1,100,3,0),&e), "t11 seedAo k: "+e.str());
    ChainError e2; bool ok=s.seedAo(k.data(),mkAo(1,1,200,3,0),&e2);
    CHECK(!ok && e2.code==ErrCode::DUP_OUTPOINT, "t11 duplicate anon-output seed rejected (DUP_OUTPOINT): "+e2.str());
    bool f=false; CAnonOutput ao; CHECK(s.getAo(k.data(),&ao,&f,&e)&&f && ao.nValue==100, "t11 rejected dup left original");
    s.close();
}
static void test_seed_feed(){
    g_cur="T12-seedfeed"; std::string d=freshDir("t12"); ChainError e;
    Hash tip=id32(0x900000,100);
    AnonStore s; CHECK(s.open(d,&e), "t12 open: "+e.str());
    CHECK(s.seedAo(key33(9).data(),mkAo(1,1,100,3,0),&e), "t12 seedAo: "+e.str());
    CHECK(s.seedKi(key33(11).data(),mkKi(4,2,25),&e), "t12 seedKi: "+e.str());
    CHECK(s.finalizeSeed(tip,100,&e), "t12 finalize: "+e.str());
    // feed one forward block (101)
    std::vector<Mutation> b; b.push_back(putAoMut(key33(10),mkAo(2,2,200,101,0)));
    Hash fh=id32(0x900000,101);
    CHECK(s.connect(fh,101,b,&e), "t12 feed connect: "+e.str());
    CHECK(s.valueCount()==3 && s.tipHeight()==101, "t12 feed adds 1 => 3, tip=101");
    // pop it back to the seeded base
    CHECK(s.disconnect(fh, tip, &e), "t12 feed disconnect: "+e.str());
    CHECK(s.valueCount()==2 && s.tipHeight()==100, "t12 after feed disconnect => seeded base (2)");
    bool f=false; CAnonOutput ao;
    CHECK(s.getAo(key33(9).data(),&ao,&f,&e)&&f && ao.nValue==100, "t12 seeded k1 retained under feed/disconnect");
    CHECK((!s.getAo(key33(10).data(),&ao,&f,&e)) || !f, "t12 feed-only k2 gone after disconnect");
    // cannot disconnect below the seeded base (no undo for historic pre-migration blocks)
    ChainError e3; bool ok2=s.disconnect(tip, Hash{}, &e3);
    CHECK(!ok2 && e3.code==ErrCode::NOT_FOUND, "t12 cannot disconnect below seeded base (NOT_FOUND): "+e3.str());
    CHECK(s.tree().verifyAll(&e), "t12 merkle verify: "+e.str());
    s.close();
}

// ======================= deterministic anon feed (slice-b-feed) tests ==============
// Mock committed (pre-block) anon-output view.
struct MockCommitted : public cs::anon::CommittedAoView {
    std::map<std::array<uint8_t,33>, CAnonOutput> map;
    bool getAo(const uint8_t key33[33], CAnonOutput* out) const override {
        std::array<uint8_t,33> k{}; memcpy(k.data(), key33, 33);
        std::map<std::array<uint8_t,33>, CAnonOutput>::const_iterator it = map.find(k);
        if (it == map.end()) return false;
        *out = it->second; return true;
    }
};
static FeedOutput fOut(uint32_t txlabel, uint32_t n, const std::array<uint8_t,33>& pk, int64_t v){
    FeedOutput o; o.txid=id32(0xA0000000,txlabel); o.n=n; o.pkCoin=pk; o.nValue=v; return o;
}
static FeedInput fIn(uint32_t txlabel, uint32_t no, const std::array<uint8_t,33>& ki,
                     const std::vector<std::array<uint8_t,33>>& ring){
    FeedInput i; i.txid=id32(0xB0000000,txlabel); i.inputNo=no; i.keyImage=ki; i.ring=ring; return i;
}
static void decodeAo(const Mutation& m, CAnonOutput* ao){
    size_t off=0; ao->decode(m.after.data(), m.after.size(), &off); (void)off;
}
static void decodeKi(const Mutation& m, CKeyImageSpent* ki){
    size_t off=0; ki->decode(m.after.data(), m.after.size(), &off); (void)off;
}

static void test_feed_deterministic(){
    g_cur="T13-feeddet";
    BlockFeed f; f.height=7; ChainError e;
    BlockFeed::FeedTx t0; t0.outputs.push_back(fOut(1,0,key33(1),100)); t0.outputs.push_back(fOut(2,1,key33(2),200));
    f.txs.push_back(t0);
    MockCommitted cm;
    std::vector<Mutation> m;
    CHECK(deriveAnonMutations(f,cm,&m,&e), "t13 derive: "+e.str());
    CHECK(m.size()==2, "t13 2 putAo for 2 outputs");
    CHECK(m[0].ns==ANON_OUTPUT_NS && m[0].afterPresent && !m[0].beforePresent, "t13 m0 fresh putAo");
    CHECK(m[1].ns==ANON_OUTPUT_NS && m[1].afterPresent && m[1].key==key33(2), "t13 m1 putAo key=key33(2)");
    CAnonOutput ao0; decodeAo(m[0], &ao0);
    CHECK(ao0.nValue==100 && ao0.nBlockHeight==7 && ao0.n==0 && ao0.nCompromised==0
          && ao0.txid==id32(0xA0000000,1), "t13 ao0 value/height/n/txid deterministic");
    CAnonOutput ao1; decodeAo(m[1], &ao1);
    CHECK(ao1.nValue==200 && ao1.nBlockHeight==7 && ao1.n==1, "t13 ao1 deterministic");
}
static void test_feed_intrablock_visibility(){
    g_cur="T14-feedintra";
    BlockFeed f; f.height=8; ChainError e;
    // earlier tx creates key33(1) (val100); later tx spends with ring {key33(1), key33(5)}
    BlockFeed::FeedTx t0; t0.outputs.push_back(fOut(1,0,key33(1),100)); f.txs.push_back(t0);
    BlockFeed::FeedTx t1; t1.inputs.push_back(fIn(2,0,key33(9), {key33(1), key33(5)})); f.txs.push_back(t1);
    MockCommitted cm; cm.map[key33(5)] = mkAo(0xCC000000,5,100,3,0);   // committed decoy, equal value
    std::vector<Mutation> m;
    CHECK(deriveAnonMutations(f,cm,&m,&e), "t14 derive: "+e.str());
    CHECK(m.size()==2, "t14 2 mutations (putAo key33(1) + putKi key33(9))");
    CHECK(m[0].ns==ANON_OUTPUT_NS && m[0].key==key33(1), "t14 m0 = putAo for earlier-tx output");
    CHECK(m[1].ns==KEY_IMAGE_NS && m[1].key==key33(9), "t14 m1 = putKi for spend");
    CKeyImageSpent ki; decodeKi(m[1], &ki);
    CHECK(ki.nValue==100 && ki.inputNo==0 && ki.txnHash==id32(0xB0000000,2),
          "t14 spawn nValue resolved from in-block-created ring member (=100)");
}
static void test_feed_committed_ring(){
    g_cur="T15-feedcommitted";
    BlockFeed f; f.height=9; ChainError e;
    BlockFeed::FeedTx t0; t0.inputs.push_back(fIn(5,2,key33(8), {key33(3), key33(4)})); f.txs.push_back(t0);
    MockCommitted cm; cm.map[key33(3)] = mkAo(0x03000000,0,77,5,0); cm.map[key33(4)] = mkAo(0x04000000,1,77,6,0);
    std::vector<Mutation> m;
    CHECK(deriveAnonMutations(f,cm,&m,&e), "t15 derive: "+e.str());
    CHECK(m.size()==1 && m[0].ns==KEY_IMAGE_NS, "t15 one putKi");
    CKeyImageSpent ki; decodeKi(m[0], &ki);
    CHECK(ki.nValue==77 && ki.inputNo==2 && ki.txnHash==id32(0xB0000000,5), "t15 spawned nValue from committed ring (=77)");
}
static void test_feed_failclosed(){
    g_cur="T16-feedfail";
    // (1) missing ring member
    { BlockFeed f; f.height=9; ChainError e;
      BlockFeed::FeedTx t0; t0.inputs.push_back(fIn(5,0,key33(8), {key33(3), key33(99)})); f.txs.push_back(t0);
      MockCommitted cm; cm.map[key33(3)] = mkAo(0x03000000,0,77,5,0);
      std::vector<Mutation> m; bool ok=deriveAnonMutations(f,cm,&m,&e);
      CHECK(!ok, "t16a missing ring member fails closed");
      CHECK(ok || e.code==ErrCode::MISSING_INPUT, "t16a error = MISSING_INPUT: "+e.str()); }
    // (2) ring amount mismatch
    { BlockFeed f; f.height=9; ChainError e;
      BlockFeed::FeedTx t0; t0.inputs.push_back(fIn(5,0,key33(8), {key33(3), key33(4)})); f.txs.push_back(t0);
      MockCommitted cm; cm.map[key33(3)] = mkAo(0x03000000,0,77,5,0); cm.map[key33(4)] = mkAo(0x04000000,1,99,6,0);
      std::vector<Mutation> m; bool ok=deriveAnonMutations(f,cm,&m,&e);
      CHECK(!ok, "t16b ring amount mismatch fails closed");
      CHECK(ok || e.code==ErrCode::CORRUPT, "t16b error = CORRUPT (amount): "+e.str()); }
    // (3) duplicate anon output created twice in one block
    { BlockFeed f; f.height=9; ChainError e;
      BlockFeed::FeedTx t0; t0.outputs.push_back(fOut(1,0,key33(1),100)); f.txs.push_back(t0);
      BlockFeed::FeedTx t1; t1.outputs.push_back(fOut(2,0,key33(1),100)); f.txs.push_back(t1);
      MockCommitted cm; std::vector<Mutation> m; bool ok=deriveAnonMutations(f,cm,&m,&e);
      CHECK(!ok, "t16c duplicate in-block anon output fails closed");
      CHECK(ok || e.code==ErrCode::DUP_OUTPOINT, "t16c error = DUP_OUTPOINT: "+e.str()); }
}

// ============================ main =============================================
int main(){
    printf("ANONSTATE isolated slice test suite\n");
    fflush(stdout);   // don't let fork children inherit+print the buffered header
    test_put_get_erase();
    test_persist_reopen();
    test_disconnect_revert();
    test_reorg();
    test_valuecount();
    test_dup_and_overwrite();
    test_ki_overwrite();
    test_crash_matrix();
    test_seed_roundtrip();
    test_seed_crash();
    test_seed_dup();
    test_seed_feed();
    test_feed_deterministic();
    test_feed_intrablock_visibility();
    test_feed_committed_ring();
    test_feed_failclosed();
    printf("---\n");
    if (g_fail==0) printf("ALL PASS (%d)\nPASS shown above\n", g_pass);
    else {
        printf("SOME FAIL %d pass %d fail\n", g_pass, g_fail);
        for (auto& s : g_failmsg) printf("  FAIL: %s\n", s.c_str());
    }
    int rc = g_fail?1:0;
    printf("RC=%d\n", rc);
    return rc;
}