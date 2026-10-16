// S3A-3 reproduction: a partial trailing page append leaves tree.dat non-aligned and
// open() refuses to start though the committed generation is fully durable.
#include "store3a/incstore.h"
#include "store3a/incdelta.h"
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>
#include <cstdlib>
#include <sys/stat.h>
using namespace cs; using namespace cs::store3a;

static Hash id32(uint32_t label,uint32_t i){ uint8_t b[8]; for(int k=0;k<4;k++) b[k]=(uint8_t)((label>>(8*k))&0xff); for(int k=0;k<4;k++) b[4+k]=(uint8_t)((i>>(8*k))&0xff); return sha256(b,8); }

int main(int argc,char**argv){
    std::string dir = argc>1?argv[1]:std::string("/home/user/data/chainstate-3a-r1-runtime/repro_s3a3");
    std::string base="/home/user/data/chainstate-3a-r1-runtime/";
    { std::string c="rm -rf '"+dir+"'"; int r=system(c.c_str()); (void)r; }
    mkdir(base.c_str(),0755); mkdir(dir.c_str(),0755);
    ChainError e;
    { CommitStore3A s; if(!s.open(dir,&e)){ printf("open fail %s\n",e.str().c_str()); return 2; }
      Block b; b.hash=id32(0xAAAA0000u,1); b.height=1; Tx cb; cb.txid=id32(0xCB000000u,1); cb.coinbase=true; TxOut o; o.value=50*100000000LL; o.script={9}; cb.vout.push_back(o); b.txs.push_back(cb);
      TreeReader tr(s.tree()); Delta d; if(!buildDeltaI(tr,b,1,&d,&e)){ printf("delta fail %s\n",e.str().c_str()); return 2; }
      if(!s.publish(d,b.hash,1,&e)){ printf("publish fail %s\n",e.str().c_str()); return 2; }
      printf("committed gen=%llu rootHash=%s\n",(unsigned long long)s.generation(), hex(s.rootHash()).c_str()); s.close(); }
    // simulate a power-loss mid-page-append: 1000 stray bytes at EOF
    { FILE* f=fopen((dir+"/tree.dat").c_str(),"ab"); std::vector<uint8_t> junk(1000,0x5A); size_t w=fwrite(junk.data(),1,junk.size(),f); (void)w; fclose(f); }
    struct stat st; stat((dir+"/tree.dat").c_str(),&st);
    printf("tree.dat size=%ld (aligned=%s)\n",(long)st.st_size,(st.st_size%4096==0)?"yes":"no");
    CommitStore3A r; bool ok=r.open(dir,&e);
    printf("REPRO_S3A3 reopen: ok=%d err=%s\n", ok, e.str().c_str());
    if(ok){
        struct stat s2; stat((dir+"/tree.dat").c_str(),&s2);
        printf("REPRO_S3A3 FIXED: recovered intact acknowledged generation (gen=%llu, aligned=%s) after unref tail drop\n",
               (unsigned long long)r.generation(), (s2.st_size%4096==0)?"yes":"no");
        r.close();
    } else printf("REPRO_S3A3 CONFIRMED: intact acknowledged state cannot be recovered (open fails closed)\n");
    return 0;
}
