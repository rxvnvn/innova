#include <boost/test/unit_test.hpp>
#include "../blockindex_v2_reader.h"
#include "../blockindex_generation_builder.h"
#include "../blockindex_generation_lifecycle.h"
#include <boost/filesystem.hpp>
#include <zlib.h>
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
    BOOST_REQUIRE_MESSAGE(b.Build(Source(), (root/"blockindex-gen-000001").string(), 1, &st, &e),e); b.Close();
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
    // A single corrupted record body (CRC mismatch) is AUTHORITATIVE-record
    // corruption: under the R3G open-time integrity contract Open must fail
    // closed (never a check-by-use silent-zero/partial-read hazard). The stale
    // A.8 per-record-isolation expectation for record bodies is superseded.
    boost::filesystem::path root=UniqueRoot();BuildSelected(root);
    std::string e; std::vector<unsigned char> b=ReadFileBytes(GenDir(root)/BLOCK_INDEX_RECORDS_FILE_NAME,&e);BOOST_REQUIRE(!b.empty());
    // record id 3 body begins at header + (3-1)*recordSize; corrupt a payload byte (not the trailing checksum).
    const size_t rec3 = BLOCK_INDEX_RECORDS_HEADER_SIZE_V1 + 2*BLOCK_INDEX_RECORD_SIZE_V1;
    BOOST_REQUIRE(b.size()>rec3+BLOCK_INDEX_RECORD_SIZE_V1); FlipBytes(b, rec3+100, 1);
    WriteFileBytes(GenDir(root)/BLOCK_INDEX_RECORDS_FILE_NAME, b, &e);
    BlockIndexV2Reader r;BlockIndexV2ReaderOptions o;std::string openErr;
    BOOST_CHECK_MESSAGE(!r.Open(root.string(),o,&openErr),"open must fail closed on corrupt authoritative record body: "<<openErr);
    BOOST_CHECK(!r.IsOpen());BOOST_CHECK(!openErr.empty());
    BOOST_CHECK(openErr.find("authoritative records integrity failure")!=std::string::npos);
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
// U4 (Cohort U): the R3G open-time record-integrity scan now runs through ONE
// bounded forward-only stream instead of a per-RecordId random-access read.
// Boundary records must still fail closed exactly like the old loop.
BOOST_AUTO_TEST_CASE(u4_sequential_r3g_detects_first_and_last_record_corruption)
{
    // First committed record body corrupted.
    { boost::filesystem::path root=UniqueRoot();BuildSelected(root);
      std::string e; std::vector<unsigned char> b=ReadFileBytes(GenDir(root)/BLOCK_INDEX_RECORDS_FILE_NAME,&e);BOOST_REQUIRE(!b.empty());
      FlipBytes(b, BLOCK_INDEX_RECORDS_HEADER_SIZE_V1 + 100, 1);
      WriteFileBytes(GenDir(root)/BLOCK_INDEX_RECORDS_FILE_NAME,b,&e);
      BlockIndexV2Reader r;BlockIndexV2ReaderOptions o;std::string openErr;
      BOOST_CHECK_MESSAGE(!r.Open(root.string(),o,&openErr), "open must fail closed on corrupt first record: "<<openErr);
      BOOST_CHECK(!r.IsOpen());
      BOOST_CHECK(openErr.find("RecordId 1")!=std::string::npos); }

    // Last committed record body corrupted.
    { boost::filesystem::path root=UniqueRoot();BuildSelected(root);
      std::string e; std::vector<unsigned char> b=ReadFileBytes(GenDir(root)/BLOCK_INDEX_RECORDS_FILE_NAME,&e);BOOST_REQUIRE(!b.empty());
      const size_t last = b.size() - BLOCK_INDEX_RECORD_SIZE_V1;
      FlipBytes(b, last + 100, 1);
      WriteFileBytes(GenDir(root)/BLOCK_INDEX_RECORDS_FILE_NAME,b,&e);
      BlockIndexV2Reader r;BlockIndexV2ReaderOptions o;std::string openErr;
      BOOST_CHECK_MESSAGE(!r.Open(root.string(),o,&openErr), "open must fail closed on corrupt last record: "<<openErr);
      BOOST_CHECK(!r.IsOpen());
      BOOST_CHECK(openErr.find("RecordId")!=std::string::npos); }
}

// U6 (Cohort U) — corruption equivalence for the R3G-only class: a record whose
// CRC is VALID but whose decoded contents violate the record contract. The R3G
// scan decodes every record with DecodeBlockIndexRecordV1, which runs
// ValidateRecord; generation-root validation never decodes a record, so this
// class is detected ONLY by the reader-open scan and must keep failing closed.
BOOST_AUTO_TEST_CASE(u6_r3g_rejects_crc_valid_but_semantically_invalid_record)
{
    boost::filesystem::path root=UniqueRoot();BuildSelected(root);
    std::string e; std::vector<unsigned char> b=ReadFileBytes(GenDir(root)/BLOCK_INDEX_RECORDS_FILE_NAME,&e);
    BOOST_REQUIRE(!b.empty());
    // Take record 2 (a PoW record in the fixture): flip its nFlags to
    // BLOCK_PROOF_OF_STAKE while nStakeTime stays 0. The CRC is then recomputed so
    // the byte-level checksum is valid and only ValidateRecord can reject it.
    const size_t rec = BLOCK_INDEX_RECORDS_HEADER_SIZE_V1 + 1*BLOCK_INDEX_RECORD_SIZE_V1;
    BOOST_REQUIRE(b.size() >= rec + BLOCK_INDEX_RECORD_SIZE_V1);
    const size_t nFlagsOff = rec + 172;               // [160..164) height, +164 nFile, +168 nBlockPos, +172 nFlags
    const size_t crcOff = rec + BLOCK_INDEX_RECORD_SIZE_V1 - 4;
    b[nFlagsOff] = (unsigned char)CBlockIndex::BLOCK_PROOF_OF_STAKE;
    const uint32_t crc = (uint32_t)crc32(0L, &b[rec], BLOCK_INDEX_RECORD_SIZE_V1 - 4);
    b[crcOff+0]=(unsigned char)(crc & 0xff); b[crcOff+1]=(unsigned char)((crc>>8)&0xff);
    b[crcOff+2]=(unsigned char)((crc>>16)&0xff); b[crcOff+3]=(unsigned char)((crc>>24)&0xff);
    WriteFileBytes(GenDir(root)/BLOCK_INDEX_RECORDS_FILE_NAME, b, &e);

    // Codec level: must reject (semantic, not checksum, failure).
    { BlockIndexRecord d; std::string de;
      BOOST_CHECK(!DecodeBlockIndexRecordV1(&b[rec], BLOCK_INDEX_RECORD_SIZE_V1, &d, &de));
      BOOST_CHECK(!de.empty()); }
    // Reader-open level: corruption must not reach a usable reader.
    { BlockIndexV2Reader r; BlockIndexV2ReaderOptions o; std::string openErr;
      BOOST_CHECK_MESSAGE(!r.Open(root.string(), o, &openErr),
          "open must fail closed on a CRC-valid but semantically invalid record: " << openErr);
      BOOST_CHECK(!r.IsOpen());
      BOOST_CHECK(openErr.find("authoritative records integrity failure") != std::string::npos); }
}

BOOST_AUTO_TEST_CASE(c1_current_marker_cache_invalidates_on_selection_change)
{
    // R2-PERF (C1/LOCATOR): CurrentSelectionChanged must keep returning the
    // cached verdict across repeated calls (no false change), yet MUST detect a
    // real CURRENT rewrite (stat change) even with the cache populated. This
    // guards the optimization that stops the per-lookup blockindex-current read.
    boost::filesystem::path root=UniqueRoot();BuildSelected(root);BlockIndexV2Reader r;Open(root,&r);std::string e;
    // fill + exercise cache hits: no selection change -> stable false
    BOOST_CHECK(!r.CurrentSelectionChanged(&e));
    BOOST_CHECK(!r.CurrentSelectionChanged(&e));
    BOOST_CHECK(!r.CurrentSelectionChanged(&e));
    // simulate a selection rewrite (mtime change) -> cache must invalidate+redetect
    BlockIndexCurrentRecord rec; rec.generation = 2;
    std::string out;
    BOOST_REQUIRE_MESSAGE(EncodeBlockIndexCurrentRecord(rec,&out,&e),e);
    {
        std::ofstream f((root/"blockindex-current").c_str(), std::ios::binary|std::ios::trunc);
        f.write(out.data(), (std::streamsize)out.size()); f.close();
    }
    BOOST_CHECK_MESSAGE(r.CurrentSelectionChanged(&e),
        "current-marker rewrite must be detected despite the selection cache");
}

BOOST_AUTO_TEST_SUITE_END()
