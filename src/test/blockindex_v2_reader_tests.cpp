#include <boost/test/unit_test.hpp>
#include "../blockindex_v2_reader.h"
#include "../blockindex_generation_builder.h"
#include "../blockindex_generation_lifecycle.h"
#include <boost/filesystem.hpp>
#include <boost/thread.hpp>
#include <leveldb/db.h>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

namespace {
static boost::filesystem::path UniqueRoot()
{
    boost::filesystem::path p=boost::filesystem::temp_directory_path()/boost::filesystem::unique_path("innova-v2-reader-%%%%-%%%%");
    boost::filesystem::create_directories(p); return p;
}
static BlockIndexRecord Rec(uint64_t n,int h,uint256 prev) { BlockIndexRecord r; r.hash=uint256(n); r.hashPrev=prev; r.height=h; r.nVersion=1; r.nTime=1000+h; r.nBits=0x1d00ffff; r.hashProof=uint256(n+100); return r; }
static BlockIndexGenerationSource Source()
{
    BlockIndexGenerationSource s; uint256 prev(0);
    for(int h=0;h<8;++h){BlockIndexRecord r=Rec(100+h,h,prev); BlockIndexGenerationSourceRecord q;q.hash=r.hash;q.record=r;s.records.push_back(q);prev=r.hash;}
    BlockIndexRecord side=Rec(999,3,uint256(102)); BlockIndexGenerationSourceRecord q;q.hash=side.hash;q.record=side;s.records.push_back(q);
    s.hashBestChain=prev;s.foundBestChain=true;return s;
}
static void BuildSelected(const boost::filesystem::path& root)
{
    BlockIndexGenerationBuilder b; BlockIndexGenerationStats st; std::string e;
    BOOST_REQUIRE_MESSAGE(b.Build(Source(), (root/"gen-000001").string(), 1, &st, &e),e); b.Close();
    BOOST_REQUIRE_MESSAGE(BlockIndexGenerationManager::SelectGeneration(root.string(),1,&e)==BLOCK_INDEX_LIFECYCLE_OK,e);
}
static void Open(const boost::filesystem::path& root, BlockIndexV2Reader* r, uint64_t cap=64ULL*1024*1024)
{
    BlockIndexV2ReaderOptions o;o.cacheCapacityBytes=cap;std::string e;BOOST_REQUIRE_MESSAGE(r->Open(root.string(),o,&e),e);
}
// ---- failure-isolation fixture helpers (A.8 delegate 3) ----
static const boost::filesystem::path GenDir(const boost::filesystem::path& root){ return root/BlockIndexGenerationManager::GenerationName(1); }
static std::vector<unsigned char> ReadFileBytes(const boost::filesystem::path& p, std::string* err)
{
    std::ifstream f(p.string(), std::ios::binary);
    if (!f){ if(err)*err="open failed: "+p.string(); return std::vector<unsigned char>(); }
    std::vector<unsigned char> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (err)
        err->clear();
    return b;
}
static void WriteFileBytes(const boost::filesystem::path& p, const std::vector<unsigned char>& b, std::string* err)
{
    std::ofstream f(p.string(), std::ios::binary | std::ios::trunc);
    if (!f){ if(err)*err="write open failed: "+p.string(); return; }
    f.write((const char*)b.data(), (std::streamsize)b.size()); f.close();
    if(err)err->clear();
}
static void SetU64LE(std::vector<unsigned char>& b, size_t off, uint64_t v)
{ for(int i=0;i<8;++i) b[off+i]=(unsigned char)((v>>(8*i))&0xff); }
static void FlipBytes(std::vector<unsigned char>& b, size_t off, size_t n)
{ for(size_t i=0;i<n;++i) b[off+i]^=0xff; }
static bool PutHashMapping(const boost::filesystem::path& hashDir, const uint256& hash, BlockIndexId id, std::string* e)
{
    leveldb::Options o; o.create_if_missing=false; leveldb::DB* db=NULL;
    leveldb::Status st=leveldb::DB::Open(o, hashDir.string(), &db);
    if(!st.ok()){ if(e)*e=st.ToString(); return false; }
    std::string key, value; bool ok=false;
    if(EncodeBlockIndexHashKey(hash,&key,e) && EncodeBlockIndexRecordIdValue(id,&value,e)){ st=db->Put(leveldb::WriteOptions(), key, value); ok=st.ok(); if(!ok&&e)*e=st.ToString(); }
    delete db; return ok;
}
}
BOOST_AUTO_TEST_SUITE(blockindex_v2_reader_tests)
BOOST_AUTO_TEST_CASE(reader_default_is_closed)
{ BlockIndexV2Reader r; BOOST_CHECK(!r.IsOpen()); BOOST_CHECK_EQUAL(r.Generation(),0U); BOOST_CHECK_EQUAL(r.RecordCount(),0U); }
BOOST_AUTO_TEST_CASE(open_hash_active_parent_ancestor_and_fork_are_pointer_free)
{
    boost::filesystem::path root=UniqueRoot();BuildSelected(root);BlockIndexV2Reader r;Open(root,&r);std::string e;BlockIndexSnapshot s;
    BOOST_CHECK_EQUAL(r.Generation(),1U); BOOST_CHECK_EQUAL(r.RecordCount(),9U);
    BOOST_CHECK_EQUAL(r.LookupByHash(uint256(105),&s,&e),BLOCK_INDEX_V2_READ_FOUND);BOOST_CHECK_EQUAL(s.height,5);BOOST_CHECK(s.fInMainChain);
    BOOST_CHECK_EQUAL(r.GetActiveByHeight(5,&s,&e),BLOCK_INDEX_V2_READ_FOUND);BOOST_CHECK(s.fInMainChain);BOOST_CHECK(s.hash==uint256(105));
    BOOST_CHECK_EQUAL(r.GetParent(s.id,&s,&e),BLOCK_INDEX_V2_READ_FOUND);BOOST_CHECK(s.hash==uint256(104));
    BOOST_CHECK_EQUAL(r.GetAncestor(6,2,&s,&e),BLOCK_INDEX_V2_READ_FOUND);BOOST_CHECK(s.hash==uint256(102));
    BOOST_CHECK_EQUAL(r.LookupByHash(uint256(107),&s,&e),BLOCK_INDEX_V2_READ_FOUND);BlockIndexId activeTip=s.id;
    BOOST_CHECK_EQUAL(r.LookupByHash(uint256(999),&s,&e),BLOCK_INDEX_V2_READ_FOUND);BlockIndexId side=s.id;
    BOOST_CHECK_EQUAL(r.FindFork(activeTip,side,&s,&e),BLOCK_INDEX_V2_READ_FOUND);BOOST_CHECK(s.hash==uint256(102));
}
BOOST_AUTO_TEST_CASE(invalid_ids_bounds_and_current_change_fail_closed)
{
    boost::filesystem::path root=UniqueRoot();BuildSelected(root);BlockIndexV2Reader r;Open(root,&r);std::string e;BlockIndexSnapshot s;
    BOOST_CHECK_EQUAL(r.GetRecordById(0,&s,&e),BLOCK_INDEX_V2_READ_NOT_FOUND);BOOST_CHECK_EQUAL(r.GetRecordById(99,&s,&e),BLOCK_INDEX_V2_READ_NOT_FOUND);
    BOOST_CHECK(!r.CurrentSelectionChanged(&e));
}
BOOST_AUTO_TEST_CASE(bounded_cache_hits_and_evictions)
{
    boost::filesystem::path root=UniqueRoot();BuildSelected(root);BlockIndexV2Reader r;Open(root,&r,sizeof(BlockIndexSnapshot)*8);std::string e;BlockIndexSnapshot s;
    BOOST_CHECK_EQUAL(r.GetRecordById(1,&s,&e),BLOCK_INDEX_V2_READ_FOUND);BOOST_CHECK_EQUAL(r.GetRecordById(1,&s,&e),BLOCK_INDEX_V2_READ_FOUND);
    for(BlockIndexId i=2;i<=8;++i)BOOST_CHECK_EQUAL(r.GetRecordById(i,&s,&e),BLOCK_INDEX_V2_READ_FOUND);
    BlockIndexV2ReaderCacheStats st=r.CacheStats();BOOST_CHECK_GT(st.hits,0U);BOOST_CHECK_GT(st.evictions,0U);BOOST_CHECK_LE(st.bytesEstimated,st.capacityBytes);BOOST_CHECK_LE(st.entries,8U);
}
BOOST_AUTO_TEST_CASE(concurrent_mixed_reads_are_consistent)
{
    boost::filesystem::path root=UniqueRoot();BuildSelected(root);BlockIndexV2Reader r;Open(root,&r);bool failed=false;CCriticalSection failureLock;
    const auto worker=[&r,&failed,&failureLock](int seed) { for(int i=0;i<1000;++i) { std::string e;BlockIndexSnapshot s;BlockIndexV2ReadStatus st;
        if((i+seed)%4==0) st=r.LookupByHash(uint256(100+((i+seed)%8)),&s,&e);
        else if((i+seed)%4==1) st=r.GetActiveByHeight((i+seed)%8,&s,&e);
        else if((i+seed)%4==2) { r.GetActiveByHeight((i+seed)%8,&s,&e); st=r.GetParent(s.id,&s,&e); }
        else { r.GetActiveByHeight(7,&s,&e); st=r.GetAncestor(s.id,(i+seed)%8,&s,&e); }
        if(st!=BLOCK_INDEX_V2_READ_FOUND) { LOCK(failureLock); failed=true; return; }
    }};
    boost::thread_group threads;for(int i=0;i<8;++i)threads.add_thread(new boost::thread(worker,i));threads.join_all();BOOST_CHECK(!failed);
}
BOOST_AUTO_TEST_CASE(failure_records_crc_corruption_is_isolated)
{
    // A single corrupted record body (CRC mismatch) must surface as a per-record
    // failure, not take down the reader or adjacent lookups.
    boost::filesystem::path root=UniqueRoot();BuildSelected(root);
    std::string e; std::vector<unsigned char> b=ReadFileBytes(GenDir(root)/BLOCK_INDEX_RECORDS_FILE_NAME,&e);BOOST_REQUIRE(!b.empty());
    // record id 3 body begins at header + (3-1)*recordSize; corrupt a payload byte (not the trailing checksum).
    const size_t rec3 = BLOCK_INDEX_RECORDS_HEADER_SIZE_V1 + 2*BLOCK_INDEX_RECORD_SIZE_V1;
    BOOST_REQUIRE(b.size()>rec3+BLOCK_INDEX_RECORD_SIZE_V1); FlipBytes(b, rec3+100, 1);
    WriteFileBytes(GenDir(root)/BLOCK_INDEX_RECORDS_FILE_NAME, b, &e);
    BlockIndexV2Reader r;Open(root,&r);BlockIndexSnapshot s;
    BOOST_CHECK_EQUAL(r.GetRecordById(3,&s,&e),BLOCK_INDEX_V2_READ_CORRUPT);BOOST_CHECK(!e.empty());
    BOOST_CHECK_EQUAL(r.GetRecordById(1,&s,&e),BLOCK_INDEX_V2_READ_FOUND);
    BOOST_CHECK_EQUAL(r.GetRecordById(4,&s,&e),BLOCK_INDEX_V2_READ_FOUND);
    BOOST_CHECK_EQUAL(r.GetActiveByHeight(7,&s,&e),BLOCK_INDEX_V2_READ_FOUND);
}
BOOST_AUTO_TEST_CASE(failure_active_bad_record_id_fails_closed_isolated)
{
    // Out-of-range RecordId in active.dat at a non-tip height: only that height fails (CORRUPT),
    // neighbours and the hash path still work. Open succeeds because the committed tip is intact.
    boost::filesystem::path root=UniqueRoot();BuildSelected(root);
    std::string e; std::vector<unsigned char> b=ReadFileBytes(GenDir(root)/BLOCK_INDEX_ACTIVE_FILE_NAME,&e);BOOST_REQUIRE(b.size()>=40+4*8);
    SetU64LE(b, BLOCK_INDEX_ACTIVE_HEADER_SIZE_V1 + 3*BLOCK_INDEX_ACTIVE_ENTRY_SIZE_V1, 100); // out of range (recordCount=9)
    WriteFileBytes(GenDir(root)/BLOCK_INDEX_ACTIVE_FILE_NAME, b, &e);
    BlockIndexV2Reader r;Open(root,&r);BlockIndexSnapshot s;
    BOOST_CHECK_EQUAL(r.GetActiveByHeight(3,&s,&e),BLOCK_INDEX_V2_READ_CORRUPT); // out-of-range: CORRUPT (error text may stay empty)
    // h4 depends on corrupt h3 and therefore fails closed; direct record and h5 remain usable.
    BOOST_CHECK_EQUAL(r.GetActiveByHeight(4,&s,&e),BLOCK_INDEX_V2_READ_CORRUPT);
    BOOST_CHECK_EQUAL(r.GetRecordById(4,&s,&e),BLOCK_INDEX_V2_READ_FOUND);
    // Zero RecordId (invalid) is also a bad active entry -> CORRUPT, isolated.
    boost::filesystem::path root2=UniqueRoot();BuildSelected(root2);
    b=ReadFileBytes(GenDir(root2)/BLOCK_INDEX_ACTIVE_FILE_NAME,&e); SetU64LE(b, BLOCK_INDEX_ACTIVE_HEADER_SIZE_V1 + 3*BLOCK_INDEX_ACTIVE_ENTRY_SIZE_V1, 0);
    WriteFileBytes(GenDir(root2)/BLOCK_INDEX_ACTIVE_FILE_NAME, b, &e);
    BlockIndexV2Reader r2;Open(root2,&r2);BOOST_CHECK_EQUAL(r2.GetActiveByHeight(3,&s,&e),BLOCK_INDEX_V2_READ_CORRUPT);
    BOOST_CHECK(!e.empty()); // zero entry rejected at decode -> diagnostic present
    BOOST_CHECK_EQUAL(r2.GetActiveByHeight(5,&s,&e),BLOCK_INDEX_V2_READ_FOUND);
}
BOOST_AUTO_TEST_CASE(failure_active_same_height_side_substitution_fails_closed)
{
    boost::filesystem::path root=UniqueRoot();BuildSelected(root);std::string e;BlockIndexSnapshot s;BlockIndexV2Reader probe;Open(root,&probe);
    BOOST_REQUIRE_EQUAL(probe.LookupByHash(uint256(999),&s,&e),BLOCK_INDEX_V2_READ_FOUND);const BlockIndexId sideId=s.id;probe.Close();
    std::vector<unsigned char> b=ReadFileBytes(GenDir(root)/BLOCK_INDEX_ACTIVE_FILE_NAME,&e);SetU64LE(b,BLOCK_INDEX_ACTIVE_HEADER_SIZE_V1+3*BLOCK_INDEX_ACTIVE_ENTRY_SIZE_V1,sideId);WriteFileBytes(GenDir(root)/BLOCK_INDEX_ACTIVE_FILE_NAME,b,&e);
    BlockIndexV2Reader r;Open(root,&r);BOOST_CHECK_EQUAL(r.GetActiveByHeight(3,&s,&e),BLOCK_INDEX_V2_READ_CORRUPT);
    BOOST_CHECK_EQUAL(r.GetActiveByHeight(0,&s,&e),BLOCK_INDEX_V2_READ_FOUND);BOOST_CHECK_EQUAL(r.GetActiveByHeight(7,&s,&e),BLOCK_INDEX_V2_READ_FOUND);
}

BOOST_AUTO_TEST_CASE(failure_hashindex_bad_or_out_of_range_id_fails_closed_isolated)
{
    // Hashindex value corruption must never silently serve the wrong record.
    // Value corruption must be written while no reader holds the leveldb LOCK.
    // (a) value pointing out-of-range: reader must fail closed as NOT_FOUND, other hashes unaffected.
    boost::filesystem::path root=UniqueRoot();BuildSelected(root);
    std::string e;
    BOOST_REQUIRE_MESSAGE(PutHashMapping(GenDir(root)/BLOCK_INDEX_HASHINDEX_DIR_NAME,uint256(106),100,&e),e);
    BlockIndexV2Reader r;Open(root,&r);BlockIndexSnapshot s;
    BOOST_CHECK_EQUAL(r.LookupByHash(uint256(106),&s,&e),BLOCK_INDEX_V2_READ_NOT_FOUND);
    BOOST_CHECK_EQUAL(r.LookupByHash(uint256(105),&s,&e),BLOCK_INDEX_V2_READ_FOUND);
    // (b) value pointing at a *valid but wrong* record: cross-component hash/record mismatch -> CORRUPT.
    boost::filesystem::path rootB=UniqueRoot();BuildSelected(rootB);
    std::string eB;
    BlockIndexV2Reader probe;Open(rootB,&probe);
    BlockIndexId id105=0;BlockIndexId badTarget=0;
    BOOST_CHECK_EQUAL(probe.LookupByHash(uint256(105),&s,&eB),BLOCK_INDEX_V2_READ_FOUND);id105=s.id;
    for(uint64_t cand=1;cand<=probe.RecordCount()&&badTarget==0;++cand){ BlockIndexSnapshot t; probe.GetRecordById(cand,&t,&eB); if(cand!=id105 && t.hash!=uint256(105)) badTarget=cand; }
    BOOST_REQUIRE_GT((uint64_t)badTarget,0ULL);
    probe.Close(); // release the leveldb LOCK before corrupting the value
    BOOST_REQUIRE_MESSAGE(PutHashMapping(GenDir(rootB)/BLOCK_INDEX_HASHINDEX_DIR_NAME,uint256(105),badTarget,&eB),eB);
    BlockIndexV2Reader r2;Open(rootB,&r2);
    BOOST_CHECK_EQUAL(r2.LookupByHash(uint256(105),&s,&eB),BLOCK_INDEX_V2_READ_CORRUPT);BOOST_CHECK(!eB.empty());
    BOOST_CHECK_EQUAL(r2.LookupByHash(uint256(106),&s,&eB),BLOCK_INDEX_V2_READ_FOUND);
}
BOOST_AUTO_TEST_CASE(failure_missing_component_prevents_open)
{
    const char* components[] = { BLOCK_INDEX_RECORDS_FILE_NAME, BLOCK_INDEX_MANIFEST_FILE_NAME, BLOCK_INDEX_ACTIVE_FILE_NAME };
    for(unsigned i=0;i<3;++i){
        boost::filesystem::path root=UniqueRoot();BuildSelected(root);
        boost::filesystem::remove(GenDir(root)/components[i]);
        BlockIndexV2Reader r;BlockIndexV2ReaderOptions o;std::string e;
        BOOST_CHECK_MESSAGE(!r.Open(root.string(),o,&e), "open must fail when " << components[i] << " missing");
        BOOST_CHECK(!r.IsOpen());BOOST_CHECK(!e.empty());
    }
    { boost::filesystem::path root=UniqueRoot();BuildSelected(root);
      boost::filesystem::remove_all(GenDir(root)/BLOCK_INDEX_HASHINDEX_DIR_NAME);
      BlockIndexV2Reader r;BlockIndexV2ReaderOptions o;std::string e;
      BOOST_CHECK(!r.Open(root.string(),o,&e));BOOST_CHECK(!r.IsOpen());BOOST_CHECK(!e.empty()); }
}
BOOST_AUTO_TEST_CASE(failure_generation_mismatch_prevents_open)
{
    // active.dat header generation rewritten -> component disagrees with CURRENT.
    { boost::filesystem::path root=UniqueRoot();BuildSelected(root);
      std::string e; std::vector<unsigned char> b=ReadFileBytes(GenDir(root)/BLOCK_INDEX_ACTIVE_FILE_NAME,&e);
      SetU64LE(b,24,2); WriteFileBytes(GenDir(root)/BLOCK_INDEX_ACTIVE_FILE_NAME,b,&e);
      BlockIndexV2Reader r;BlockIndexV2ReaderOptions o;
      BOOST_CHECK(!r.Open(root.string(),o,&e));BOOST_CHECK(!r.IsOpen());BOOST_CHECK(!e.empty()); }
    // MANIFEST generation rewritten -> disagrees with CURRENT.
    { boost::filesystem::path root=UniqueRoot();BuildSelected(root);
      std::string e; std::vector<unsigned char> b=ReadFileBytes(GenDir(root)/BLOCK_INDEX_MANIFEST_FILE_NAME,&e);
      SetU64LE(b,24,2); WriteFileBytes(GenDir(root)/BLOCK_INDEX_MANIFEST_FILE_NAME,b,&e);
      BlockIndexV2Reader r;BlockIndexV2ReaderOptions o;
      BOOST_CHECK(!r.Open(root.string(),o,&e));BOOST_CHECK(!r.IsOpen());BOOST_CHECK(!e.empty()); }
    // CURRENT rewritten to a generation that does not exist on disk.
    { boost::filesystem::path root=UniqueRoot();BuildSelected(root);
      BlockIndexCurrentRecord rec;rec.generation=2;std::string cur;std::string e;
      BOOST_REQUIRE_MESSAGE(EncodeBlockIndexCurrentRecord(rec,&cur,&e),e);
      WriteFileBytes(root/BLOCK_INDEX_CURRENT_FILE_NAME,std::vector<unsigned char>(cur.begin(),cur.end()),&e);
      BlockIndexV2Reader r;BlockIndexV2ReaderOptions o;
      BOOST_CHECK(!r.Open(root.string(),o,&e));BOOST_CHECK(!r.IsOpen()); }
}
BOOST_AUTO_TEST_CASE(failure_relevant_truncation_prevents_open)
{
    // records.dat truncated within the committed region -> Open fails closed.
    { boost::filesystem::path root=UniqueRoot();BuildSelected(root);
      std::string e; std::vector<unsigned char> b=ReadFileBytes(GenDir(root)/BLOCK_INDEX_RECORDS_FILE_NAME,&e);
      b.resize(BLOCK_INDEX_RECORDS_HEADER_SIZE_V1 + 8*BLOCK_INDEX_RECORD_SIZE_V1); // drop the last committed record
      WriteFileBytes(GenDir(root)/BLOCK_INDEX_RECORDS_FILE_NAME,b,&e);
      BlockIndexV2Reader r;BlockIndexV2ReaderOptions o;
      BOOST_CHECK(!r.Open(root.string(),o,&e));BOOST_CHECK(!r.IsOpen());BOOST_CHECK(!e.empty()); }
    // active.dat truncated below the committed tip height -> Open fails closed.
    { boost::filesystem::path root=UniqueRoot();BuildSelected(root);
      std::string e; std::vector<unsigned char> b=ReadFileBytes(GenDir(root)/BLOCK_INDEX_ACTIVE_FILE_NAME,&e);
      b.resize(BLOCK_INDEX_ACTIVE_HEADER_SIZE_V1 + 7*BLOCK_INDEX_ACTIVE_ENTRY_SIZE_V1); // drop height 7 (committed tip)
      WriteFileBytes(GenDir(root)/BLOCK_INDEX_ACTIVE_FILE_NAME,b,&e);
      BlockIndexV2Reader r;BlockIndexV2ReaderOptions o;
      BOOST_CHECK(!r.Open(root.string(),o,&e));BOOST_CHECK(!r.IsOpen());BOOST_CHECK(!e.empty()); }
}
BOOST_AUTO_TEST_SUITE_END()
