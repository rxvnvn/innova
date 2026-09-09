#include "candidate_frontier_metadata.h"
#include "fixed_blockindex_store.h"
#include <boost/filesystem.hpp>
#include <leveldb/cache.h>
#include <leveldb/db.h>
#include <leveldb/filter_policy.h>
#include <leveldb/write_batch.h>
#include <openssl/sha.h>
#include <cstdio>
#include <cstring>
#include <algorithm>

namespace fs = boost::filesystem;
namespace {
const unsigned char MAGIC[8] = {'I','N','N','B','L','F','1',0};
static void U32(unsigned char* p, uint32_t v) { for (int i=0;i<4;++i) p[i]=(unsigned char)(v>>(8*i)); }
static void U64(unsigned char* p, uint64_t v) { for (int i=0;i<8;++i) p[i]=(unsigned char)(v>>(8*i)); }
static uint32_t R32(const unsigned char* p) { uint32_t v=0; for(int i=0;i<4;++i)v|=(uint32_t)p[i]<<(8*i); return v; }
static uint64_t R64(const unsigned char* p) { uint64_t v=0; for(int i=0;i<8;++i)v|=(uint64_t)p[i]<<(8*i); return v; }
static bool Err(std::string* e,const std::string& s){if(e)*e=s;return false;}
static bool HashLeaves(const std::vector<uint256>& leaves,unsigned char out[32]){
 SHA256_CTX c; SHA256_Init(&c); for(size_t i=0;i<leaves.size();++i) SHA256_Update(&c,leaves[i].begin(),32); SHA256_Final(out,&c); return true;
}
}

bool EnsureCandidateLeafMetadata(const std::string& dir,uint64_t generation,std::string* error)
{
 fs::path outPath=fs::path(dir)/BLOCK_INDEX_CANDIDATE_LEAVES_FILE_NAME;
 if(fs::exists(outPath)) return true;
 FixedBlockIndexOpenOptions oo; oo.requireCompleteManifest=false; FixedBlockIndexStore store;
 if(!FixedBlockIndexStore::OpenReadOnly(dir,oo,&store,error)) return false;
 leveldb::Options o; o.create_if_missing=true; o.error_if_exists=true; o.filter_policy=leveldb::NewBloomFilterPolicy(10); o.block_cache=leveldb::NewLRUCache(512*1024); o.write_buffer_size=1024*1024; o.max_open_files=64;
 boost::system::error_code ec; fs::path tmp=fs::temp_directory_path(ec)/fs::unique_path("innova-leaf-%%%%-%%%%",ec); if(ec||!fs::create_directories(tmp,ec)) return Err(error,"candidate leaves temp dir failed");
 leveldb::DB* db=0; leveldb::Status st=leveldb::DB::Open(o,(tmp/"parents").string(),&db); if(!st.ok()){fs::remove_all(tmp,ec);return Err(error,"candidate leaves marker open failed: "+st.ToString());}
 const auto fail=[&](const std::string&s){delete db;fs::remove_all(tmp,ec);return Err(error,s);};
 leveldb::WriteBatch b; unsigned n=0; uint64_t count=store.PhysicalRecordCount();
 for(BlockIndexId id=1;id<=count;++id){BlockIndexRecord r;std::string er;if(!store.Read(id,&r,&er))return fail("candidate leaves record read failed: "+er);if(r.hashPrev==uint256(0))continue;b.Put(leveldb::Slice((const char*)r.hashPrev.begin(),32),leveldb::Slice());if(++n>=4096){st=db->Write(leveldb::WriteOptions(),&b);if(!st.ok())return fail("candidate leaves marker write failed");b.Clear();n=0;}}
 if(n){st=db->Write(leveldb::WriteOptions(),&b);if(!st.ok())return fail("candidate leaves marker final write failed");}
 std::vector<uint256> leaves; leaves.reserve(4096);
 for(BlockIndexId id=1;id<=count;++id){BlockIndexRecord r;std::string er;if(!store.Read(id,&r,&er))return fail("candidate leaves record reread failed: "+er);std::string v;if(!db->Get(leveldb::ReadOptions(),leveldb::Slice((const char*)r.hash.begin(),32),&v).ok())leaves.push_back(r.hash);}
 delete db;db=0;fs::remove_all(tmp,ec);std::sort(leaves.begin(),leaves.end()); unsigned char digest[32];HashLeaves(leaves,digest);
 std::vector<unsigned char> data(8+4+8+8+32+leaves.size()*32);memcpy(&data[0],MAGIC,8);U32(&data[8],1);U64(&data[12],generation);U64(&data[20],leaves.size());memcpy(&data[28],digest,32);for(size_t i=0;i<leaves.size();++i)memcpy(&data[60+i*32],leaves[i].begin(),32);
 fs::path tmpFile=outPath.string()+".tmp";FILE*f=fopen(tmpFile.string().c_str(),"wb");if(!f)return Err(error,"candidate leaves output open failed");bool ok=fwrite(&data[0],1,data.size(),f)==data.size();fflush(f);fclose(f);if(!ok){fs::remove(tmpFile,ec);return Err(error,"candidate leaves output write failed");}if(rename(tmpFile.string().c_str(),outPath.string().c_str())!=0)return Err(error,"candidate leaves publish failed");return true;
}

bool ReadCandidateLeafMetadata(const std::string& dir,uint64_t generation,std::vector<uint256>* leaves,std::string* error)
{
 if(!leaves)return Err(error,"null candidate leaves output");leaves->clear();fs::path p=fs::path(dir)/BLOCK_INDEX_CANDIDATE_LEAVES_FILE_NAME;FILE*f=fopen(p.string().c_str(),"rb");if(!f)return false;fseek(f,0,SEEK_END);long n=ftell(f);fseek(f,0,SEEK_SET);if(n<60){fclose(f);return Err(error,"candidate leaves truncated");}std::vector<unsigned char>d((size_t)n);if(fread(&d[0],1,d.size(),f)!=d.size()){fclose(f);return Err(error,"candidate leaves read failed");}fclose(f);if(memcmp(&d[0],MAGIC,8)||R32(&d[8])!=1||R64(&d[12])!=generation)return Err(error,"candidate leaves binding mismatch");uint64_t count=R64(&d[20]);if(count>SIZE_MAX/32||60+count*32!=d.size())return Err(error,"candidate leaves size mismatch");leaves->resize((size_t)count);for(size_t i=0;i<leaves->size();++i)memcpy((*leaves)[i].begin(),&d[60+i*32],32);unsigned char digest[32];HashLeaves(*leaves,digest);if(memcmp(digest,&d[28],32))return Err(error,"candidate leaves digest mismatch");if(!std::is_sorted(leaves->begin(),leaves->end()))return Err(error,"candidate leaves unsorted");return true;
}

bool ComputeCandidateLeavesBinding(const std::string& dir,uint64_t generation,unsigned char out[32],std::string* error)
{
 std::vector<uint256> leaves; if(!ReadCandidateLeafMetadata(dir,generation,&leaves,error)) return false;
 unsigned char leafDigest[32]; HashLeaves(leaves,leafDigest); SHA256_CTX c; SHA256_Init(&c);
 static const char domain[]="INNOVA_CANDIDATE_LEAVES_BINDING_V1"; SHA256_Update(&c,domain,sizeof(domain)-1);
 unsigned char gen[8]; U64(gen,generation); SHA256_Update(&c,gen,8); SHA256_Update(&c,leafDigest,32); SHA256_Final(out,&c); return true;
}

bool MixCandidateLeavesIntoDagDigest(const unsigned char dag[32],const unsigned char candidate[32],unsigned char out[32])
{
 if(!dag||!candidate||!out) return false; SHA256_CTX c; SHA256_Init(&c);
 static const char domain[]="INNOVA_GENERATION_DAG_BINDING_V2"; SHA256_Update(&c,domain,sizeof(domain)-1);
 SHA256_Update(&c,dag,32); SHA256_Update(&c,candidate,32); SHA256_Final(out,&c); return true;
}
