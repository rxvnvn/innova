// S3A-1 reproduction: non-root leaf page corruption is returned undetected by get().
#include "store3a/btree.h"
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>
#include <cstdlib>
#include <sys/stat.h>
using namespace cs; using namespace cs::store3a;

static Hash id32(uint32_t label,uint32_t i){ uint8_t b[8]; for(int k=0;k<4;k++) b[k]=(uint8_t)((label>>(8*k))&0xff); for(int k=0;k<4;k++) b[4+k]=(uint8_t)((i>>(8*k))&0xff); return sha256(b,8); }
static uint32_t rd32(const uint8_t*p){ uint32_t v=0; for(int i=0;i<4;i++) v|=(uint32_t)p[i]<<(8*i); return v; }

int main(int argc,char**argv){
    std::string dir = argc>1?argv[1]:std::string("/home/user/data/chainstate-3a-r1-runtime/repro_s3a1");
    std::string base="/home/user/data/chainstate-3a-r1-runtime/";
    { std::string c="rm -rf '"+dir+"'"; int r=system(c.c_str()); (void)r; }
    mkdir(base.c_str(),0755); mkdir(dir.c_str(),0755);
    ChainError e; BTree t; if(!t.create(dir+"/tree.dat",&e)){ printf("create fail %s\n",e.str().c_str()); return 2; }
    const int N=200;
    for(int i=0;i<N;i++){ Key k{}; Hash h=id32(0x77u,(uint32_t)i); memcpy(k.data(),h.data(),32); k[32]=(uint8_t)i;
        std::vector<uint8_t> v; v.push_back(0xAB); v.push_back((uint8_t)i); v.push_back(0xCD); v.push_back(0xEF);
        if(!t.put(k,v,&e)){ printf("put fail %s\n",e.str().c_str()); return 2; } }
    uint32_t root=t.rootId(); Hash rootH=t.rootHash();
    FILE* f=fopen((dir+"/tree.dat").c_str(),"rb");
    std::vector<uint8_t> pg(4096); fseek(f,(long)root*4096,SEEK_SET); size_t rd=fread(pg.data(),1,4096,f); (void)rd;
    printf("rootPageId=%u type=%u count=%u\n", root, pg[0], (unsigned)(pg[1]|(pg[2]<<8)));
    if (pg[0]!=2){ printf("root not internal; increase N\n"); fclose(f); return 3; }
    uint32_t child=rd32(pg.data()+4);
    std::vector<uint8_t> lf(4096); fseek(f,(long)child*4096,SEEK_SET); rd=fread(lf.data(),1,4096,f);
    printf("nonRootLeafPageId=%u type=%u count=%u\n", child, lf[0], (unsigned)(lf[1]|(lf[2]<<8)));
    Key k0; memcpy(k0.data(), lf.data()+4, 36);
    uint16_t vlen=(uint16_t)(lf[40]|(lf[41]<<8));
    uint8_t origVal0=lf[42];
    fclose(f);
    // flip first value byte in the non-root leaf
    f=fopen((dir+"/tree.dat").c_str(),"r+b"); fseek(f,(long)child*4096+42,SEEK_SET); int c=fgetc(f); fseek(f,(long)child*4096+42,SEEK_SET); fputc(c^0xFF,f); fclose(f);
    t.close();
    // emulate the normal access path: open + setRoot(committed root id + committed root hash)
    BTree t2;
    if(!t2.open(dir+"/tree.dat",&e)){ printf("open fail %s\n",e.str().c_str()); return 2; }
    if(!t2.setRoot(root,rootH,&e)){ printf("setRoot fail %s\n",e.str().c_str()); return 2; }
    std::vector<uint8_t> v; bool found=false; ChainError ge;
    bool ok=t2.get(k0,&v,&found,&ge);
    printf("get: ok=%d found=%d err=%s value0=%02x (original=%02x)\n",
           ok, found, ge.str().c_str(), v.empty()?0:v[0], origVal0);
    if(!ok) printf("REPRO_S3A1 FIXED: traversal rejected the corrupted non-root page (err=%s)\n", ge.str().c_str());
    else if(found && !v.empty() && v[0]!=origVal0) printf("REPRO_S3A1 CONFIRMED: non-root corruption returned silently by get()\n");
    else printf("REPRO_S3A1 inconclusive\n");
    t2.close();
    return 0;
}
