// TX-LOC standalone test harness (Item 1 slice 1). Covers: build -> sorted
// main file -> lookup -> restart -> empty -> corruption fail-closed -> absent
// -> dedup keep-first.
#include "../txloc.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <vector>
#include <string>

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { if (cond) { g_pass++; } else { g_fail++; printf("FAIL: %s\n", msg); } } while (0)

static void mkTxid(unsigned char* out, uint32_t hi, uint32_t seed) {
    // Full 32-bit distinctness: seed occupies out[0..3] LE, hi out[4..5], fill rest deterministically.
    memset(out, 0, 32);
    for (int i = 0; i < 4; i++) out[i] = (unsigned char)((seed >> (8*i)) & 0xFF);
    out[4] = (unsigned char)(hi & 0xFF); out[5] = (unsigned char)((hi >> 8) & 0xFF);
    for (int i = 6; i < 32; i++) out[i] = (unsigned char)(((seed * 2654435761u) >> ((i * 7) % 32)) ^ (hi + i));
}

// Feed adapter over a vector of records.
static void feedFrom(const std::vector<std::pair<std::vector<unsigned char>, txloc::Loc> >& recs,
                     size_t* pos, unsigned char* out, txloc::Loc* l) {
    if (*pos >= recs.size()) return;
    memcpy(out, recs[*pos].first.data(), 32);
    *l = recs[*pos].second;
    (*pos)++;
}

static void test_basic() {
    std::string dir = "/tmp/txloc-t1"; ::system("rm -rf /tmp/txloc-t1"); ::mkdir("/tmp/txloc-t1", 0755);
    std::string idx = dir + "/txloc.dat";
    std::vector<std::pair<std::vector<unsigned char>, txloc::Loc> > recs;
    size_t feedPos = 0;
    for (uint32_t i = 0; i < 1000; i++) {
        unsigned char t[32]; txloc::Loc l; l.nFile = i % 3 + 1; l.nBlockPos = i * 10; l.nTxPos = i * 10 + 5; l.nHeight = (int32_t)i;
        mkTxid(t, i % 7, i);
        recs.push_back(std::make_pair(std::vector<unsigned char>(t, t + 32), l));
    }
    txloc::ChainError e;
    bool ok = txloc::buildIndex(idx, [&](unsigned char* out, txloc::Loc* l)->bool{
        if (feedPos >= recs.size()) return false;
        feedFrom(recs, &feedPos, out, l); return true;
    }, &e);
    CHECK(ok, ("build ok: " + e.str()).c_str());

    txloc::TxLocator r; bool absent=false;
    CHECK(r.open(idx, &absent, &e), ("open: " + e.str()).c_str());
    CHECK(!absent, "index present after build");
    CHECK(r.count() == 1000, "count == fed");

    // lookup several known records and verify exact position round-trip
    for (uint32_t i = 0; i < 1000; i += 97) {
        unsigned char w[32]; txloc::Loc exp; exp.nFile = i % 3 + 1; exp.nBlockPos = i * 10; exp.nTxPos = i * 10 + 5; exp.nHeight = (int32_t)i;
        mkTxid(w, i % 7, i);
        txloc::Loc got; bool found=false;
        CHECK(r.get(w, &got, &found, &e) && found, ("found i=" + std::to_string(i)).c_str());
        CHECK(got.nFile == exp.nFile && got.nBlockPos == exp.nBlockPos && got.nTxPos == exp.nTxPos && got.nHeight == exp.nHeight,
              ("exact position i=" + std::to_string(i)).c_str());
    }
    // not found
    unsigned char nf[32]; mkTxid(nf, 99, 12345);
    bool found=true; txloc::Loc got;
    CHECK(r.get(nf, &got, &found, &e) && !found, "not-found returns found=false");
    // restart
    r.close();
    txloc::TxLocator r2; CHECK(r2.open(idx, &absent, &e), ("reopen: " + e.str()).c_str());
    unsigned char w[32]; mkTxid(w, 0, 0); txloc::Loc g2; bool f2=false;
    CHECK(r2.get(w, &g2, &f2, &e) && f2 && g2.nHeight == 0, "lookup after restart");
    r2.close();
}

static void test_dedup() {
    std::string idx = "/tmp/txloc-t2.dat"; ::unlink(idx.c_str());
    unsigned char t[32]; mkTxid(t, 5, 7);
    std::vector<std::pair<std::vector<unsigned char>, txloc::Loc> > recs;
    txloc::Loc l1, l2; l1.nFile=1; l1.nBlockPos=100; l1.nTxPos=105; l1.nHeight=10;
    l2.nFile=1; l2.nBlockPos=900; l2.nTxPos=905; l2.nHeight=50;
    recs.push_back(std::make_pair(std::vector<unsigned char>(t, t+32), l1));
    recs.push_back(std::make_pair(std::vector<unsigned char>(t, t+32), l2));
    size_t p=0; txloc::ChainError e;
    CHECK(txloc::buildIndex(idx, [&](unsigned char* o, txloc::Loc* l)->bool{
        if (p >= recs.size()) return false; feedFrom(recs, &p, o, l); return true; }, &e), "dedup build");
    txloc::TxLocator r; bool absent=false; CHECK(r.open(idx, &absent, &e), ("dedup open: " + e.str()).c_str());
    CHECK(r.count() == 1, "dedup keeps single record");
    txloc::Loc got; bool found=false; CHECK(r.get(t, &got, &found, &e) && found, "dedup found");
    CHECK(got.nBlockPos == 100 && got.nHeight == 10, "keep-first (lowest) record");
    r.close();
}

static void test_empty() {
    std::string idx = "/tmp/txloc-t3.dat"; ::unlink(idx.c_str());
    txloc::ChainError e; size_t n = 0;
    CHECK(txloc::buildIndex(idx, [&](unsigned char*, txloc::Loc*)->bool{ return n++ < 0; }, &e), "empty build");
    txloc::TxLocator r; bool absent=false; CHECK(r.open(idx, &absent, &e), ("empty open: " + e.str()).c_str());
    unsigned char t[32]; mkTxid(t, 1, 1); txloc::Loc got; bool found=true;
    CHECK(r.get(t, &got, &found, &e) && !found, "empty index -> not found");
    CHECK(r.count() == 0, "empty count 0");
    r.close();
}

static void test_corrupt_failclosed() {
    std::string idx = "/tmp/txloc-t4.dat"; ::unlink(idx.c_str());
    txloc::ChainError e; size_t n = 0;
    CHECK(txloc::buildIndex(idx, [&](unsigned char* o, txloc::Loc* l)->bool{
        unsigned char t[32]; mkTxid(t, 1, (uint32_t)n); memcpy(o, t, 32); l->nFile=1; l->nBlockPos=(uint32_t)n*10; l->nTxPos=(uint32_t)n*10+5; l->nHeight=(int32_t)n; return n++ < 50; }, &e), "build");
    // corrupt the magic byte
    int fd = ::open(idx.c_str(), O_WRONLY); unsigned char z[1] = {0xFF}; ::pwrite(fd, z, 1, 4); ::close(fd);
    txloc::TxLocator r; bool absent=false;
    CHECK(!r.open(idx, &absent, &e), "corrupt magic -> fail-closed (refuse)");
    CHECK(e.code == txloc::ERR_CORRUPT, "corrupt classified as CORRUPT");
    // also corrupt truncation
    std::string idx2 = "/tmp/txloc-t4b.dat"; ::unlink(idx2.c_str());
    n = 0;
    CHECK(txloc::buildIndex(idx2, [&](unsigned char* o, txloc::Loc* l)->bool{
        unsigned char t[32]; mkTxid(t, 2, (uint32_t)n); memcpy(o, t, 32); l->nFile=1; l->nBlockPos=0; l->nTxPos=1; l->nHeight=(int32_t)n; return n++ < 1000; }, &e), "build b");
    // truncate the main file to half
    struct stat st; stat(idx2.c_str(), &st); int fd2 = ::open(idx2.c_str(), O_WRONLY); ::ftruncate(fd2, st.st_size / 2); ::close(fd2);
    txloc::TxLocator r2; absent=false;
    CHECK(!r2.open(idx2, &absent, &e) && e.code == txloc::ERR_CORRUPT, "truncated -> fail-closed CORRUPT");
}

static void test_absent_rebuild() {
    std::string idx = "/tmp/txloc-t6.dat"; ::unlink(idx.c_str());
    txloc::ChainError e; txloc::TxLocator r; bool absent=false;
    CHECK(!r.open(idx, &absent, &e) && absent, "absent index signals absent (rebuild by scan)");
    CHECK(e.code == txloc::ERR_NONE, "absent is not an error");
    size_t n=0;
    CHECK(txloc::buildIndex(idx, [&](unsigned char* o, txloc::Loc* l)->bool{
        unsigned char t[32]; mkTxid(t, 2, (uint32_t)n); memcpy(o, t, 32); l->nFile=3; l->nBlockPos=(uint32_t)n; l->nTxPos=(uint32_t)n+1; l->nHeight=777; return n++ < 30; }, &e), "rebuild build");
    absent=false; CHECK(r.open(idx, &absent, &e), ("rebuild open: " + e.str()).c_str());
    unsigned char w[32]; mkTxid(w, 2, 5); txloc::Loc got; bool found=false;
    CHECK(r.get(w, &got, &found, &e) && found && got.nFile==3 && got.nHeight==777, "lookup after rebuild");
    r.close();
}

// Incremental IBD ingest: several add/sync/close/reopen rounds; all records
// survive restart and the locator counts them without full rebuild.
static void test_ingest_loop_restart() {
    std::string dir = "/tmp/txloc-i1"; ::system("rm -rf /tmp/txloc-i1");
    txloc::ChainError e;
    uint64_t total = 0;
    for (int round = 0; round < 4; round++) {
        txloc::TxLocator r; bool absent = false;
        CHECK(r.openDir(dir, &absent, &e), ("round open: " + e.str()).c_str());
        int base = round * 100;
        for (int i = 0; i < 100; i++) {
            unsigned char t[32]; txloc::Loc l; l.nFile = 1; l.nBlockPos = (uint32_t)(base + i) * 10; l.nTxPos = l.nBlockPos + 5; l.nHeight = base + i;
            mkTxid(t, 0, (uint32_t)(base + i));
            CHECK(r.add(t, l, &e), ("round add: " + e.str()).c_str());
        }
        CHECK(r.sync(&e), "round sync");
        r.close();
        total += 100;
        // reopen and verify all seen so far are present
        txloc::TxLocator v; CHECK(v.openDir(dir, &absent, &e), ("verify open: " + e.str()).c_str());
        for (uint32_t i = 0; i < total; i++) {
            unsigned char w[32]; mkTxid(w, 0, i);
            txloc::Loc got; bool found = false;
            CHECK(v.get(w, &got, &found, &e) && found, ("verify found i=" + std::to_string(i)).c_str());
        }
        CHECK(v.memCount() == total, ("memtable holds all ingested (no recreate) round " + std::to_string(round)).c_str());
        v.close();
    }
}

// Crash recovery: a WAL with a torn trailing record is trimmed and earlier
// records still replay; records written but not yet merged survive restart.
static void test_ingest_crash_replay() {
    std::string dir = "/tmp/txloc-i2"; ::system("rm -rf /tmp/txloc-i2");
    txloc::ChainError e;
    {
        txloc::TxLocator r; bool absent; CHECK(r.openDir(dir, &absent, &e), ("open: " + e.str()).c_str());
        for (uint32_t i = 0; i < 50; i++) { unsigned char t[32]; txloc::Loc l; l.nFile=2; l.nBlockPos=i*7; l.nTxPos=i*7+1; l.nHeight=(int32_t)i; mkTxid(t, 3, i); CHECK(r.add(t,l,&e), "add"); }
        CHECK(r.sync(&e), "sync");
        r.close();
    }
    // append a torn partial record (15 bytes) to simulate crash mid-WAL-write
    FILE* wal = fopen((dir + "/txloc.dat.wal").c_str(), "ab");
    for (int i = 0; i < 15; i++) fputc(0xAB, wal);
    fclose(wal);
    {
        txloc::TxLocator r; bool absent; CHECK(r.openDir(dir, &absent, &e), ("reopen after torn wal: " + e.str()).c_str());
        for (uint32_t i = 0; i < 50; i++) { unsigned char w[32]; mkTxid(w, 3, i); txloc::Loc got; bool found=false; CHECK(r.get(w,&got,&found,&e) && found, "torn-wal record present"); }
        // a never-written txid should be absent
        unsigned char nf[32]; mkTxid(nf, 3, 999); txloc::Loc got; bool found=true;
        CHECK(r.get(nf,&got,&found,&e) && !found, "absent txid not fabricated");
        r.close();
    }
}

// Explicit compaction merge (main + memtable), then restart still serves all.
static void test_ingest_compact_merge() {
    std::string dir = "/tmp/txloc-i3"; ::system("rm -rf /tmp/txloc-i3");
    txloc::ChainError e;
    txloc::TxLocator r; bool absent; CHECK(r.openDir(dir, &absent, &e), ("open: " + e.str()).c_str());
    for (uint32_t i = 0; i < 500; i++) { unsigned char t[32]; txloc::Loc l; l.nFile=4; l.nBlockPos=i; l.nTxPos=i+1; l.nHeight=(int32_t)i; mkTxid(t, 6, i); CHECK(r.add(t,l,&e), "add"); }
    CHECK(r.memCount() == 500 && r.memTableBytes() < 500 * 80, "bounded memtable before compact");
    CHECK(r.compact(&e), ("compact: " + e.str()).c_str());
    CHECK(r.memCount() == 0, "compact clears memtable");
    CHECK(r.count() == 500, "compact poshes 500 into merged main");
    for (uint32_t i = 0; i < 500; i++) { unsigned char w[32]; mkTxid(w, 6, i); txloc::Loc got; bool found=false; CHECK(r.get(w,&got,&found,&e) && found && got.nFile==4, "post-compact lookup"); }
    r.close();
    txloc::TxLocator v; CHECK(v.openDir(dir, &absent, &e), ("reopen: " + e.str()).c_str());
    CHECK(v.count() == 500, "merged main survives restart");
    for (uint32_t i = 0; i < 500; i++) { unsigned char w[32]; mkTxid(w, 6, i); txloc::Loc got; bool found=false; CHECK(v.get(w,&got,&found,&e) && found, "post-restart lookup"); }
    v.close();
}

// keep-first across a merge: a re-added txid after compaction keeps the earliest (main) position.
static void test_ingest_keepfirst_across_merge() {
    std::string dir = "/tmp/txloc-i4"; ::system("rm -rf /tmp/txloc-i4");
    txloc::ChainError e;
    txloc::TxLocator r; bool absent; CHECK(r.openDir(dir, &absent, &e), ("open: " + e.str()).c_str());
    unsigned char t[32]; mkTxid(t, 8, 1);
    txloc::Loc a; a.nFile=1; a.nBlockPos=100; a.nTxPos=105; a.nHeight=10;
    CHECK(r.add(t, a, &e), "add first");
    CHECK(r.compact(&e), "compact first into main");
    txloc::Loc b; b.nFile=1; b.nBlockPos=900; b.nTxPos=905; b.nHeight=50;
    CHECK(r.add(t, b, &e), "re-add after compact (not in memtable)");
    CHECK(r.compact(&e), "compact again (merge)");
    CHECK(r.count() == 1, "merge eliminates duplicate txid (single record)");
    txloc::Loc got; bool found=false; CHECK(r.get(t,&got,&found,&e) && found, "found");
    CHECK(got.nBlockPos == 100 && got.nHeight == 10, "keep-first (earliest) across merge");
    r.close();
}

// No full re-creation on startup: opening an existing merged index must NOT drop it.
static void test_ingest_no_recreate() {
    std::string dir = "/tmp/txloc-i5"; ::system("rm -rf /tmp/txloc-i5");
    txloc::ChainError e;
    txloc::TxLocator r; bool absent; CHECK(r.openDir(dir, &absent, &e), ("open: " + e.str()).c_str());
    for (uint32_t i = 0; i < 200; i++) { unsigned char t[32]; txloc::Loc l; l.nFile=5; l.nBlockPos=i; l.nTxPos=i+1; l.nHeight=(int32_t)i; mkTxid(t, 9, i); CHECK(r.add(t,l,&e), "add"); }
    CHECK(r.compact(&e), "compact");
    r.close();
    // reopen twice consecutively; both sees the full set (not reset to empty)
    for (int k = 0; k < 2; k++) {
        txloc::TxLocator v; CHECK(v.openDir(dir, &absent, &e), ("open " + std::to_string(k) + ": " + e.str()).c_str());
        CHECK(v.count() == 200, ("index not recreated on open " + std::to_string(k)).c_str());
        unsigned char w[32]; mkTxid(w, 9, 42); txloc::Loc got; bool found=false; CHECK(v.get(w,&got,&found,&e) && found, "persist across opens");
        v.close();
    }
}

int main() {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("== TX-LOC location index suite ==\n");
    test_basic(); test_dedup(); test_empty(); test_corrupt_failclosed(); test_absent_rebuild();
    test_ingest_loop_restart(); test_ingest_crash_replay(); test_ingest_compact_merge();
    test_ingest_keepfirst_across_merge(); test_ingest_no_recreate();
    printf("PASS=%d FAIL=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}