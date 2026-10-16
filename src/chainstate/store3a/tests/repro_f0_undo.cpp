// Stage 3B-F0 reproduction/regression driver (undo offset coherence).
// Runs the exact reported sequence and reports physical vs logical undo layout and whether the
// most-recent block can still be disconnected after restart.
//   before fix: undo.dat physical=456 vs MANIFEST.undoSize=342 -> tip disconnect NOT_FOUND
//   after  fix: physical==logical (342) -> tip disconnect works
#include "store3a/incstore.h"
#include "store3a/incdelta.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>

using namespace cs; using namespace cs::store3a;

static Hash id32(uint32_t label, uint32_t i){ uint8_t b[8]; for(int k=0;k<4;k++) b[k]=(uint8_t)((label>>(8*k))&0xff); for(int k=0;k<4;k++) b[4+k]=(uint8_t)((i>>(8*k))&0xff); return sha256(b,8); }
static long fsize(const std::string& p){ struct stat st; return stat(p.c_str(),&st)==0?(long)st.st_size:-1; }
static bool storeToState(CommitStore3A& s, UtxoState* out, ChainError* e){
    std::vector<LeafEntry> ents; if(!const_cast<BTree&>(s.tree()).collectAll(&ents,e)) return false;
    out->map.clear();
    for (auto& en: ents){ size_t off=0; Coin c; if(!c.decode(en.val.data(),en.val.size(),&off)){ setErr(e,ErrCode::CORRUPT,"coin decode"); return false; } out->map[en.key]=c; }
    return true; }
static Hash digestOf(CommitStore3A& s, ChainError* e){ UtxoState st; if(!storeToState(s,&st,e)) return Hash(); return st.digest(); }
static bool publishCB(CommitStore3A& s, int h, ChainError* e){
    Block b; b.hash=id32(0xB10C0000u,(uint32_t)h); b.height=h;
    Tx cb; cb.txid=id32(0xCB000000u,(uint32_t)h); cb.coinbase=true; cb.coinstake=false;
    TxOut o; o.value=50LL*100000000LL; o.script={1,2,3}; cb.vout.push_back(o); b.txs.push_back(cb);
    TreeReader tr(s.tree()); Delta d; if(!buildDeltaI(tr,b,1,&d,e)) return false;
    return s.publish(d,b.hash,h,e); }
// MANIFEST: magic8 ver4 size4 gen8 undoSize8  -> undoSize at byte offset 24
static uint64_t manifestUndoSize(const std::string& dir){
    FILE* f=fopen((dir+"/MANIFEST").c_str(),"rb"); if(!f) return 0;
    uint8_t b[32]; size_t n=fread(b,1,32,f); fclose(f); if(n<32) return 0;
    uint64_t v=0; for(int i=0;i<8;i++) v|=(uint64_t)b[24+i]<<(8*i); return v; }

int main(){
    std::string d="/home/user/data/chainstate-3b-f0-runtime/f0_probe_store";
    std::string cmd="rm -rf '"+d+"'"; int rr=system(cmd.c_str()); (void)rr;
    ChainError e; Hash h4=id32(0xB10C0000u,4);
    { CommitStore3A s; if(!s.open(d,&e)){ printf("open fail: %s\n",e.str().c_str()); return 2; }
      for(int h=1;h<=3;h++) if(!publishCB(s,h,&e)){ printf("publish %d fail: %s\n",h,e.str().c_str()); return 2; }
      Hash d3=digestOf(s,&e);
      if(!s.disconnect(id32(0xB10C0000u,3),&e)){ printf("disconnect h3 fail: %s\n",e.str().c_str()); return 2; }
      Hash d2=digestOf(s,&e);
      if(!publishCB(s,4,&e)){ printf("publish h4 fail: %s\n",e.str().c_str()); return 2; }
      printf("in-memory : gen=%llu undoCount=%zu tip=%s\n",(unsigned long long)s.generation(),s.undoCount(),hex(s.tipHash()).c_str());
      printf("in-memory : digestAfterDisconnect(D2)=%s digestAfterRePublish(D4)=%s (D3 was %s)\n",
             hex(d2).c_str(),hex(digestOf(s,&e)).c_str(),hex(d3).c_str());
      s.close(); }
    long phys=fsize(d+"/undo.dat"); uint64_t logical=manifestUndoSize(d);
    printf("on-disk   : undo.dat physical=%ld MANIFEST.undoSize=%llu staleTail=%ld DIVERGENT=%s\n",
           phys,(unsigned long long)logical, phys-(long)logical, phys!=(long)logical?"YES":"no");
    int rc=0;
    { CommitStore3A r; bool ok=r.open(d,&e);
      printf("restart   : open=%d gen=%llu undoCount=%zu err=%s\n",ok,(unsigned long long)r.generation(),r.undoCount(),e.str().c_str());
      if(ok){ printf("restart   : tipHash=%s\n",hex(r.tipHash()).c_str());
        ChainError e2; bool dok=r.disconnect(h4,&e2);
        printf("restart   : disconnect(most-recent tip) ok=%d err=%s\n",dok,e2.str().c_str());
        printf("restart   : VERDICT %s\n", dok? "PASS (consistent: most-recent block disconnectable)"
                                              : "DEFECT (most-recent block not disconnectable)");
        rc = dok?0:1; r.close(); } else rc=2; }
    return rc; }
