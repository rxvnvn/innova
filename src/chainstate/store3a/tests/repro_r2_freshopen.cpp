// Stage 3A-R2 reproduction: R1-1 (destructive fresh-open O_TRUNC) and R1-2 (silent empty
// presentation) when MANIFEST+ACK are missing over an existing tree.dat.
#include "store3a/incstore.h"
#include "store3a/incdelta.h"
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>
#include <cstdlib>
#include <cerrno>
#include <sys/stat.h>
#include <unistd.h>

using namespace cs; using namespace cs::store3a;

static Hash id32(uint32_t label, uint32_t i){ uint8_t b[8]; for(int k=0;k<4;k++) b[k]=(uint8_t)((label>>(8*k))&0xff); for(int k=0;k<4;k++) b[4+k]=(uint8_t)((i>>(8*k))&0xff); return sha256(b,8); }
static long fsize(const std::string& p){ struct stat st; return stat(p.c_str(),&st)==0?(long)st.st_size:-1; }
static bool exists(const std::string& p){ struct stat st; return stat(p.c_str(),&st)==0; }
static int cpdir(const std::string& a, const std::string& b){ std::string c="rm -rf '"+b+"'; cp -a '"+a+"' '"+b+"'"; int r=system(c.c_str()); (void)r; return (int)fsize(b); }
static std::string baseDir(){ return "/home/user/data/chainstate-3a-r2-runtime/"; }
static std::string rmTree(const std::string& d){ std::string c="rm -rf '"+d+"'"; int r=system(c.c_str()); (void)r; return d; }
static std::string freshDir(const std::string& name){ std::string b=baseDir(); std::string d=b+name; rmTree(d); mkdir(b.c_str(),0755); mkdir(d.c_str(),0755); return d; }

static bool publishCB(CommitStore3A& s, int h, ChainError* e){
    Block b; b.hash=id32(0xB10C0000u,(uint32_t)h); b.height=h;
    Tx cb; cb.txid=id32(0xCB000000u,(uint32_t)h); cb.coinbase=true; cb.coinstake=false;
    TxOut o; o.value=50LL*100000000LL; o.script={1,2,3}; cb.vout.push_back(o); b.txs.push_back(cb);
    TreeReader tr(s.tree()); Delta d; if(!buildDeltaI(tr,b,1,&d,e)) return false;
    return s.publish(d,b.hash,h,e);
}

int main(){
    ChainError e;
    std::string src=freshDir("r2_src");
    { CommitStore3A s; if(!s.open(src,&e)){ printf("setup open fail %s\n",e.str().c_str()); return 2; }
      for(int h=1;h<=6;h++){ if(!publishCB(s,h,&e)){ printf("publish h=%d fail %s\n",h,e.str().c_str()); return 2; } }
      printf("committed: gen=%llu rootHash=%s nextPageId=%u treePages=%u\n",
             (unsigned long long)s.generation(), hex(s.rootHash()).c_str(),
             s.tree().nextPageId(), s.tree().nextPageId());
      s.close(); }
    long srcSize=fsize(src+"/tree.dat");
    printf("source tree.dat size=%ld (non-empty=%s) MANIFEST=%s ACK=%s\n", srcSize, srcSize>4096?"yes":"no",
           exists(src+"/MANIFEST")?"present":"absent", exists(src+"/ACK")?"present":"absent");

    // ---- X1: remove pointer files only, keep tree.dat intact (R1-2 silent-empty)
    std::string x1=freshDir("r2_x1"); cpdir(src,x1);
    unlink((x1+"/MANIFEST").c_str()); unlink((x1+"/ACK").c_str());
    long x1Before=fsize(x1+"/tree.dat");
    { CommitStore3A s; bool ok=s.open(x1,&e);
      long after=fsize(x1+"/tree.dat");
      printf("X1 open=%d gen=%llu rootHash=%s err=%s | treeBefore=%ld treeAfter=%ld changed=%s\n",
             ok,(unsigned long long)s.generation(), ok?hex(s.rootHash()).c_str():"-", e.str().c_str(),
             x1Before, after, after!=x1Before?"YES":"no");
      printf("X1 R1-2 %s\n", (ok && s.generation()==0 && x1Before>4096) ? "CONFIRMED: non-empty committed tree presented as empty gen-0 (silent-empty)" : "not-reproduced");
      if(ok) s.close(); }

    // ---- X2: remove pointer files AND corrupt page 0 type byte (R1-1 destructive truncate)
    std::string x2=freshDir("r2_x2"); cpdir(src,x2);
    unlink((x2+"/MANIFEST").c_str()); unlink((x2+"/ACK").c_str());
    { FILE* f=fopen((x2+"/tree.dat").c_str(),"r+b"); if(f){ fseek(f,0,SEEK_SET); int c=fgetc(f); if(c!=EOF){ fseek(f,0,SEEK_SET); fputc(c^0xFF,f);} fclose(f);} }
    long x2Before=fsize(x2+"/tree.dat");
    { CommitStore3A s; bool ok=s.open(x2,&e);
      long after=fsize(x2+"/tree.dat");
      printf("X2 open=%d gen=%llu err=%s | treeBefore=%ld treeAfter=%ld truncated=%s\n",
             ok,(unsigned long long)s.generation(), e.str().c_str(), x2Before, after, after<x2Before?"YES":"no");
      printf("X2 R1-1 %s\n", (ok && after<x2Before) ? "CONFIRMED: existing tree.dat destroyed via fresh-create O_TRUNC" : "not-reproduced");
      if(ok) s.close(); }
    return 0;
}
