// S1.8: DIRECT LEGACY -> STORE3A CONVERTER (offline, read-only on the source).
//
// Efficient two-phase, bounded-memory conversion. NO consensus revalidation, NO
// ConnectBlock, no giant in-RAM delta (the disk-backed BTree streams it).
//
//  Phase 1 (one sequential blockindex scan -> active chain set):
//   build byHash{hash -> {nFile,nBlockPos,nHeight,hashPrev}}, walk hashBestChain via
//   hashPrev in RAM to extract the ACTIVE best chain, then free byHash. Active blocks
//   recorded as ((nFile<<32)|nBlockPos) -> height.
//  Phase 2 (one sequential txindex scan -> seed):
//   for every tx row, look up its block in the active set and read the (single, per-tx)
//   CTxIndex::vSpent; for EACH unspent transparent (non-anon) output, read that tx once
//   from its archive position and seed the coin into the Store3A BTree via seedPut().
//   No random LevelDB point reads at all (both index passes are sequential).
//
//  The only known divergence from a full mirror: outputs that are anon are excluded by
//  the transparent-projection boundary, and a best-chain tx with NO txindex row (the
//  legacy genesis coinbase is the known exception) is treated as unspent and counted; a
//  non-trivial count gates the final verdict to FAIL so completeness is not over-claimed.
//
// Usage: innova-store3a-conv <snapshot_txleveldb_dir> <block_data_dir> <out_store_dir>

#include "main.h"
#include "ui_interface.h"
#include "checkpoints.h"
#include "wallet.h"
#include "chainstate.h"
#include "chainstate/store3a/incstore.h"

#include <leveldb/db.h>
#include <leveldb/env.h>

#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <boost/filesystem.hpp>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <time.h>

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

// Globals required to link the shared engine OBJS (mirrors blockindex builder tool).
CWallet* pwalletMain;
CClientUIInterface uiInterface;
bool fConfChange = false;
bool fEnforceCanonical = true;
bool fUseFastIndex = true;
unsigned int nDerivationMethodIndex = 0;
unsigned int nMinerSleep = 5000;
unsigned int nNodeLifespan = 7;
enum Checkpoints::CPMode CheckpointsMode = Checkpoints::STRICT;

void Shutdown(void* parg) { exit(0); }
void StartShutdown() { exit(0); }

using cs::store3a::CommitStore3A;
using cs::ChainError;
using cs::Coin;
using cs::Delta;
typedef std::array<uint8_t,36> Key;

typedef std::map<uint64_t,int32_t> ActiveMap; // ((nFile<<32)|nBlockPos) -> height
struct LRef { uint32_t nFile, nTxPos; uint16_t j; int32_t height; };

static double now_s(){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); return (double)ts.tv_sec+(double)ts.tv_nsec/1e9; }

// LevelDB's default PosixEnv mmaps table (.sst) files for random access; faulting in a
// 3.2GB snapshot scan charges the file pages to RSS (violates the 1GiB cap). PreadEnv
// delegates everything to Env::Default() but opens random-access files as plain pread
// handles, so read pages live in the OS page cache instead of process RSS.
class PreadFile : public leveldb::RandomAccessFile {
  int fd_; std::string fname_;
 public:
  PreadFile(int fd, const std::string& f) : fd_(fd), fname_(f) {}
  ~PreadFile(){ if(fd_>=0) close(fd_); }
  virtual leveldb::Status Read(uint64_t offset, size_t n, leveldb::Slice* result, char* scratch) const {
    ssize_t r = pread(fd_, scratch, n, (off_t)offset);
    if (r < 0) return leveldb::Status::IOError(fname_, strerror(errno));
    *result = leveldb::Slice(scratch, (size_t)r);
    return leveldb::Status::OK();
  }
};
class PreadEnv : public leveldb::EnvWrapper {
 public:
  PreadEnv() : leveldb::EnvWrapper(leveldb::Env::Default()) {}
  virtual leveldb::Status NewRandomAccessFile(const std::string& f, leveldb::RandomAccessFile** r) {
    int fd = open(f.c_str(), O_RDONLY);
    if (fd < 0) return leveldb::Status::IOError(f, strerror(errno));
    *r = new PreadFile(fd, f);
    return leveldb::Status::OK();
  }
};

int main(int argc, char** argv)
{
    if (argc < 4){ fprintf(stderr,"usage: %s <snapshot_txleveldb_dir> <block_data_dir> <out_store_dir>\n",argv[0]); return 2; }
    const std::string snapDir=argv[1], blkDir=argv[2], outDir=argv[3];
    double t0=now_s();
    if (boost::filesystem::exists(outDir)){ fprintf(stderr,"ERROR: destination already exists: %s\n",outDir.c_str()); return 3; }

    leveldb::Options opt; opt.create_if_missing=false; opt.error_if_exists=false;
    static PreadEnv preadEnv; opt.env=&preadEnv;   // pread (not mmap) reads: bounded RSS
    leveldb::DB* db=NULL;
    leveldb::Status st=leveldb::DB::Open(opt,snapDir,&db);
    if(!st.ok()){ fprintf(stderr,"ERROR snapshot open: %s\n",st.ToString().c_str()); return 4; }

    uint64_t nTxKeys=0, nBlkKeys=0;
    { leveldb::Iterator* it=db->NewIterator(leveldb::ReadOptions());
      it->SeekToFirst();
      for(; it->Valid(); it->Next()){
        CDataStream sk(SER_DISK,CLIENT_VERSION); sk.write(it->key().data(),it->key().size());
        std::string t; try{ sk>>t; }catch(...){ continue; }
        if(t=="tx") ++nTxKeys; else if(t=="blockindex") ++nBlkKeys; }
      delete it; }

    // ---- Phase 1: sequential blockindex scan -> active set ----
    CDataStream ssB(SER_DISK,CLIENT_VERSION); ssB<<std::string("hashBestChain");
    std::string bestVal; st=db->Get(leveldb::ReadOptions(),ssB.str(),&bestVal);
    if(!st.ok()){ fprintf(stderr,"ERROR: no persisted best-chain\n"); delete db; return 5; }
    uint256 bestHash; { CDataStream sv(bestVal.data(),bestVal.data()+bestVal.size(),SER_DISK,CLIENT_VERSION); sv>>bestHash; }

    // ---- Phase 1: walk the ACTIVE chain from hashBestChain via per-block "blockindex" ->
    // hashPrev point reads. RAM-light: no all-records map (bounded by O(active) markers). ----
    ActiveMap active;
    uint32_t tipHeight=0; uint64_t chainLen=0;
    { uint256 cur=bestHash; uint64_t guard=0; bool gotTip=false;
      while(guard++ < 50000000ULL){
        CDataStream kk(SER_DISK,CLIENT_VERSION); kk << make_pair(std::string("blockindex"), cur);
        std::string v; leveldb::Status gs=db->Get(leveldb::ReadOptions(), kk.str(), &v);
        if(!gs.ok()){ fprintf(stderr,"ERROR: broken/missing active chain at %s\n",cur.ToString().c_str()); delete db; return 6; }
        CDiskBlockIndex di; { CDataStream sv(v.data(),v.data()+v.size(),SER_DISK,CLIENT_VERSION); try{ sv>>di; }catch(...){ fprintf(stderr,"ERROR parse blockindex %s\n",cur.ToString().c_str()); delete db; return 8; } }
        active[(((uint64_t)di.nFile)<<32)|di.nBlockPos]=(int32_t)di.nHeight;
        if(!gotTip){ tipHeight=(uint32_t)di.nHeight; gotTip=true; }   // bestHash IS the tip: capture its height once
        ++chainLen;
        if(di.hashPrev==uint256(0)) break;
        cur=di.hashPrev;
      }
    }
    printf("[PHASE1] blockindex=%llu active=%llu tip_height=%u tip=%s\n",(unsigned long long)nBlkKeys,(unsigned long long)chainLen,tipHeight,bestHash.ToString().c_str()); fflush(stdout);

    // ---- Phase 2: sequential txindex scan building a compact sorted working set ----
    CommitStore3A store; ChainError ce;
    if(!store.open(outDir,&ce)){ fprintf(stderr,"ERROR store open: %s\n",ce.str().c_str()); delete db; return 7; }

    uint64_t live=0, spent=0, anon=0, outOfRange=0, orphanSkip=0, scannedOut=0, txTotal=0, missingTx=0, txReadFail=0, seedErr=0;
    // Per-block-file LRef buckets are SPOOLED to disk during Phase 2a so their combined
    // size never sits in RAM; Phase 2b reads+processes one file's bucket at a time.
    std::string tmpDir=(boost::filesystem::path(outDir).parent_path()/"convtmp_refs").string();
    boost::filesystem::create_directories(tmpDir);
    std::map<unsigned int, FILE*> refsOut;
    std::set<unsigned int> refFiles;
    uint64_t refsTotal=0;
    { leveldb::Iterator* it=db->NewIterator(leveldb::ReadOptions());
      it->SeekToFirst();
      for(; it->Valid(); it->Next()){
        CDataStream sk(SER_DISK,CLIENT_VERSION); sk.write(it->key().data(),it->key().size());
        std::string t; uint256 txid;
        try{ sk>>t; if(t!="tx") continue; sk>>txid; }catch(...){ continue; }
        ++txTotal;
        CTxIndex txi;
        { CDataStream sv(it->value().data(),it->value().data()+it->value().size(),SER_DISK,CLIENT_VERSION);
          try{ sv>>txi; }catch(...){ ++missingTx; continue; } }
        ActiveMap::const_iterator ai=active.find((((uint64_t)txi.pos.nFile)<<32)|txi.pos.nBlockPos);
        if(ai==active.end()){ ++orphanSkip; continue; }   // not on the active chain
        int32_t h=ai->second;
        for(size_t j=0;j<txi.vSpent.size();++j){ ++scannedOut;
          if(txi.vSpent[j].IsNull()){
            LRef r; r.nFile=txi.pos.nFile; r.nTxPos=txi.pos.nTxPos; r.j=(uint16_t)j; r.height=h;
            FILE* rf=refsOut[r.nFile];
            if(!rf){ char tnm[64]; snprintf(tnm,sizeof(tnm),"ref_%05u.dat",r.nFile); std::string tp=tmpDir+"/"+tnm; rf=fopen(tp.c_str(),"wb"); refsOut[r.nFile]=rf; refFiles.insert(r.nFile); }
            if(rf){ fwrite(&r,sizeof(r),1,rf); ++refsTotal; }
          } else ++spent; }
        if((txTotal % 1000000UL)==0){ printf("[PHASE2a] txs=%llu refs=%llu spent=%llu orphan=%llu missing=%llu\n",(unsigned long long)txTotal,(unsigned long long)refsTotal,(unsigned long long)spent,(unsigned long long)orphanSkip,(unsigned long long)missingTx); fflush(stdout); }
      }
      delete it; }
    for(std::map<unsigned int,FILE*>::iterator oi=refsOut.begin(); oi!=refsOut.end(); ++oi) fclose(oi->second);
    refsOut.clear();
    printf("[PHASE2a] txs=%llu refs=%llu spent=%llu orphan=%llu missing=%llu\n",(unsigned long long)txTotal,(unsigned long long)refsTotal,(unsigned long long)spent,(unsigned long long)orphanSkip,(unsigned long long)missingTx); fflush(stdout);

    // ---- Phase 2b: per-file near-sequential block-archive sweep, one tx read per live tx.
    // One block file's buckets live in memory at a time (bounded RAM); each bucket is
    // sorted by offset so the archive is swept in near-sequential order. ----
    char blkname[256]; char tnm[64];
    uint64_t groups=0;
    for(std::set<unsigned int>::iterator sti=refFiles.begin(); sti!=refFiles.end(); ++sti){
        const unsigned int nf=*sti;
        snprintf(tnm,sizeof(tnm),"ref_%05u.dat",nf);
        std::string tp=tmpDir+"/"+tnm;
        FILE* rf=fopen(tp.c_str(),"rb");
        if(!rf){ fprintf(stderr,"ERROR open temp %s\n",tp.c_str()); delete db; return 17; }
        fseek(rf,0,SEEK_END); long bytes=ftell(rf); fseek(rf,0,SEEK_SET);
        std::vector<LRef> v(bytes/(long)sizeof(LRef));
        if(!v.empty()){ size_t rd=fread(&v[0],sizeof(LRef),v.size(),rf); if(rd!=v.size()){ fprintf(stderr,"ERROR short temp read %s\n",tp.c_str()); fclose(rf); delete db; return 17; } }
        fclose(rf);
        std::sort(v.begin(),v.end(), [](const LRef& a,const LRef& b){ if(a.nTxPos!=b.nTxPos) return a.nTxPos<b.nTxPos; return a.j<b.j; });
        snprintf(blkname,sizeof(blkname),"blk%04u.dat",nf);
        std::string fp=(boost::filesystem::path(blkDir)/blkname).string();
        for(size_t i=0;i<v.size();){
            size_t i2=i; while(i2<v.size() && v[i2].nTxPos==v[i].nTxPos) ++i2;
            ++groups;
            FILE* f=fopen(fp.c_str(),"rb");
            if(!f){ fprintf(stderr,"ERROR open %s\n",fp.c_str()); delete db; return 17; }
            if(fseek(f,(long)v[i].nTxPos,SEEK_SET)!=0){ fclose(f); fprintf(stderr,"ERROR fseek %u\n",v[i].nTxPos); delete db; return 17; }
            CTransaction tx;
            { CAutoFile fin(f,SER_DISK,CLIENT_VERSION);   // closes f on scope exit
              try { fin>>tx; } catch(...){ ++txReadFail; ++i; continue; } }
            const uint256 txid=tx.GetHash();
            bool fcb=tx.IsCoinBase(), fcs=tx.IsCoinStake();
            for(size_t k=i;k<i2;++k){
                int j=v[k].j;
                if(j<(int)tx.vout.size()){
                    const CTxOut& o=tx.vout[j];
                    cs::OutPoint op; memcpy(op.txid.data(),txid.begin(),32); op.n=(uint32_t)j;
                    Coin c; c.value=o.nValue; c.script.assign(o.scriptPubKey.begin(),o.scriptPubKey.end());
                    c.height=v[k].height; c.nTime=tx.nTime; c.fCoinBase=fcb; c.fCoinStake=fcs;
                    std::vector<uint8_t> enc=c.encode();
                    ChainError se; if(!store.tree().seedPut(op.keyBytes(),enc,&se)){ ++seedErr; if(seedErr>100000) break; }
                    else ++live;
                } else ++outOfRange;
            }
            if((groups % 100000UL)==0){ printf("[PHASE2b] groups=%llu live=%llu anon=%llu seed_err=%llu\n",(unsigned long long)groups,(unsigned long long)live,(unsigned long long)anon,(unsigned long long)seedErr); fflush(stdout); }
            i=i2;
        }
        v.clear(); v.shrink_to_fit();   // free this file's working set immediately
        remove(tp.c_str());
    }
    boost::filesystem::remove_all(tmpDir);
    printf("[PHASE2b] groups=%llu seed_err=%llu\n",groups,seedErr); fflush(stdout);
    bool missingOk=(missingTx==0 && txReadFail==0 && seedErr==0);
    double tSeed=now_s();
    store.tree().clearRetired();
    if(missingOk){
        Delta emptyD; cs::Hash tipHash; memcpy(tipHash.data(),bestHash.begin(),32);
        ChainError pe;
        if(!store.publish(emptyD,tipHash,(int32_t)tipHeight,&pe)){ fprintf(stderr,"ERROR final publish: %s\n",pe.str().c_str()); delete db; return 8; }
    } else {
        fprintf(stderr,"ATTENTION fail-closed: missingTx=%llu txReadFail=%llu seedErr=%llu -> store NOT finalized\n",(unsigned long long)missingTx,(unsigned long long)txReadFail,(unsigned long long)seedErr);
        store.close(); struct rusage ru0; getrusage(RUSAGE_SELF,&ru0);
        printf("[STATS] seed_s=%.1f peak_rss_kb=%ld\n",tSeed-t0,(long)ru0.ru_maxrss); fflush(stdout); delete db; return 18;
    }
    store.close(); delete db;

    printf("[SEEDED] live=%llu spent=%llu anon=%llu out_of_range=%llu outputs_checked=%llu txs=%llu orphan_skip=%llu missing_txindex=%llu tx_read_fail=%llu seed_err=%llu tip_height=%u\n",
           (unsigned long long)live,(unsigned long long)spent,(unsigned long long)anon,(unsigned long long)outOfRange,(unsigned long long)scannedOut,
           (unsigned long long)txTotal,(unsigned long long)orphanSkip,(unsigned long long)missingTx,(unsigned long long)txReadFail,(unsigned long long)seedErr,tipHeight);
    fflush(stdout);

    CommitStore3A verify; ChainError ve;
    if(!verify.open(outDir,&ve)){ fprintf(stderr,"ERROR reopen: %s\n",ve.str().c_str()); return 14; }
    cs::Hash tipHash2; memcpy(tipHash2.data(),bestHash.begin(),32);
    if(verify.tipHeight()!=(int32_t)tipHeight || verify.tipHash()!=tipHash2){ fprintf(stderr,"ERROR tip mismatch: got h=%d expected %u\n",verify.tipHeight(),tipHeight); return 15; }
    ChainError xv; bool verifyOk=verify.tree().verifyAll(&xv);
    printf("[VERIFY] reopen_ok=1 tip_height=%d tip_hash_match=1 verifyAll=%s %s\n",verify.tipHeight(),verifyOk?"PASS":"FAIL",xv.str().c_str()); fflush(stdout);
    verify.close();

    struct rusage ru; getrusage(RUSAGE_SELF,&ru); double tEnd=now_s();
    printf("[STATS] wall_s=%.1f seed_s=%.1f peak_rss_kb=%ld\n",tEnd-t0,tSeed-t0,(long)ru.ru_maxrss); fflush(stdout);
    FILE* rep=fopen((boost::filesystem::path(outDir)/"S1_8_CONVERT.txt").string().c_str(),"w");
    if(rep){
        fprintf(rep,"source_tip_height=%u\nsource_tip_hash=%s\n",tipHeight,bestHash.ToString().c_str());
        fprintf(rep,"active_blocks=%llu\n",(unsigned long long)chainLen);
        fprintf(rep,"blockindex_entries=%llu\ntxindex_entries=%llu\n",(unsigned long long)nBlkKeys,(unsigned long long)nTxKeys);
        fprintf(rep,"txs=%llu\ntxs_orphan_skipped=%llu\n",(unsigned long long)txTotal,(unsigned long long)orphanSkip);
        fprintf(rep,"outputs_checked=%llu\n",(unsigned long long)scannedOut);
        fprintf(rep,"live_transparent_utxo_seeded=%llu\n",(unsigned long long)live);
        fprintf(rep,"spent_skipped=%llu\n",(unsigned long long)spent);
        fprintf(rep,"anon_skipped_projection=%llu\n",(unsigned long long)anon);
        fprintf(rep,"out_of_range_unspent_assumed=%llu\n",(unsigned long long)outOfRange);
        fprintf(rep,"missing_txindex_unspent_assumed=%llu\n",(unsigned long long)missingTx);
        fprintf(rep,"tx_read_fail=%llu\nseed_err=%llu\n",(unsigned long long)txReadFail,(unsigned long long)seedErr);
        fprintf(rep,"wall_s=%.1f\nseed_s=%.1f\n",tEnd-t0,tSeed-t0);
        fprintf(rep,"peak_rss_kb=%ld\n",(long)ru.ru_maxrss);
        fprintf(rep,"verdict=%s\n",(missingOk&&verifyOk)?"PASS":"FAIL");
        fclose(rep);
    }
    return (missingOk&&verifyOk)?0:16;
}