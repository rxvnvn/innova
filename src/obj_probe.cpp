#include "main.h"
#include "util.h"
#include <cstdio>
CWallet* pwalletMain; CClientUIInterface uiInterface; bool fConfChange=false; bool fEnforceCanonical=true; bool fUseFastIndex=true; unsigned int nDerivationMethodIndex=0; unsigned int nMinerSleep=5000; unsigned int nNodeLifespan=7; enum Checkpoints::CPMode CheckpointsMode = Checkpoints::STRICT;
void Shutdown(void*){} void StartShutdown(){}
struct Pre { Pre(){ fprintf(stderr,"[PRE-ctor] mapArgs count=%lu\n", (unsigned long)mapArgs.size()); } } gpre_;
int main(){
  fprintf(stderr,"[main-start] datadir cached=%s\n", GetDataDir().string().c_str());
  mapArgs["-regtest"]="1"; {char tmpl[]="/tmp/innova-dd-XXXXXX"; char*d=mkdtemp(tmpl); mapArgs["-datadir"]=std::string(d);}
  fprintf(stderr,"[main-after] datadir cached=%s\n", GetDataDir().string().c_str());
  return 0;
}
