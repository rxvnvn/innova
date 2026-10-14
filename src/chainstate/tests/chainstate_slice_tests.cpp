// Isolated chainstate vertical slice - focused test suite T1..T12.
// Standalone harness: no boost, no production test framework, isolated runtime dir.
//   build: g++ -std=c++14 -O2 -I src/chainstate src/chainstate/*.cpp \
//          src/chainstate/tests/chainstate_slice_tests.cpp -o slice_tests
#include "commitstore.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <time.h>
#include <stdlib.h>

using namespace cs;

static const int64_t COIN = 100000000LL;
static const int32_t MAT = 1; // test maturity (production: nCoinbaseMaturity=65)

static int g_pass = 0, g_fail = 0;
static std::vector<std::string> g_failmsg;
static std::string g_cur;

#define CHECK(cond, msg) do { if (cond) { g_pass++; } else { g_fail++; g_failmsg.push_back(g_cur + ": " + (msg)); } } while (0)

static long readVmHWM();   // forward (used by T13)

static Hash H(uint8_t a) { Hash h; for (int i = 0; i < 32; ++i) h[i] = (uint8_t)(a + i); return h; }

static Tx coinbaseTx(uint8_t seed, int64_t value) {
    Tx t; t.txid = H(seed); t.coinbase = true; t.coinstake = false;
    TxOut o; o.value = value; o.script = {1, 2, 3}; t.vout.push_back(o);
    return t;
}
static Tx spendTx(uint8_t seed, const OutPoint& prev, int64_t value) {
    Tx t; t.txid = H(seed); t.coinbase = false; t.coinstake = false;
    TxIn in; in.prev = prev; t.vin.push_back(in);
    TxOut o; o.value = value; o.script = {9, 9}; t.vout.push_back(o);
    return t;
}

static std::string rmTree(const std::string& d) {
    std::string cmd = "rm -rf '" + d + "'";
    if (system(cmd.c_str()) != 0) { /* ignore */ }
    return d;
}
static std::string freshDir(const std::string& name) {
    std::string base = "/home/user/data/chainstate-slice-runtime/";
    std::string d = base + name;
    rmTree(d);
    mkdir(base.c_str(), 0755);
    mkdir(d.c_str(), 0755);
    return d;
}

// --------------------------------------------------------------------------
static void test_sha() {
    g_cur = "T0-sha256";
    Hash h = sha256((const uint8_t*)"abc", 3);
    CHECK(hex(h) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "sha256(abc) vector");
    Hash e = sha256((const uint8_t*)"", 0);
    CHECK(hex(e) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "sha256(empty) vector");
}

// T1: connect -> disconnect restores identical canonical state (2-block chain).
static void test_t1() {
    g_cur = "T1";
    std::string dir = freshDir("t1");
    ChainError e;
    CommitStore s;
    CHECK(s.open(dir, &e), "open: " + e.str());
    Hash d0 = s.state().digest();

    Block b1; b1.hash = H(100); b1.height = 1; b1.txs.push_back(coinbaseTx(10, 50 * COIN));
    Delta dl1; UtxoState ns1;
    CHECK(buildDelta(s.state(), b1, MAT, &dl1, &ns1, &e), "T1 buildDelta b1: " + e.str());
    CHECK(s.publish(dl1, b1.hash, 1, &e), "T1 publish b1: " + e.str());
    Hash d1 = s.state().digest();
    CHECK(d1 != d0, "T1 state changed after b1");

    OutPoint cb; cb.txid = H(10); cb.n = 0;
    Block b2; b2.hash = H(101); b2.height = 2; b2.txs.push_back(spendTx(20, cb, 40 * COIN));
    Delta dl2; UtxoState ns2;
    CHECK(buildDelta(s.state(), b2, MAT, &dl2, &ns2, &e), "T1 buildDelta b2: " + e.str());
    CHECK(s.publish(dl2, b2.hash, 2, &e), "T1 publish b2: " + e.str());
    Hash d2 = s.state().digest();

    CHECK(s.disconnect(b2.hash, &e), "T1 disconnect b2: " + e.str());
    CHECK(s.state().digest() == d1, "T1 disconnect b2 -> state == after b1");
    CHECK(s.disconnect(b1.hash, &e), "T1 disconnect b1: " + e.str());
    CHECK(s.state().digest() == d0, "T1 disconnect b1 -> empty state");

    s.close();
    CommitStore s2; CHECK(s2.open(dir, &e), "T1 reopen: " + e.str());
    CHECK(s2.state().digest() == d0, "T1 restart reads disconnected canonical state");
    CHECK(s2.generation() == 4, "T1 generation advanced (1,2,disconnect,disconnect)");
    s2.close();
}

// T2: multiple ordered ops on same outpoint, incl. create-then-spend within one block.
static void test_t2() {
    g_cur = "T2";
    std::string dir = freshDir("t2");
    ChainError e;
    CommitStore s; CHECK(s.open(dir, &e), "open: " + e.str());
    Hash d0 = s.state().digest();
    Block b1; b1.hash = H(1); b1.height = 1; { Tx t; t.txid = H(30); t.coinbase = true; TxOut o; o.value = 30*COIN; o.script={7}; t.vout.push_back(o); b1.txs.push_back(t); }
    Delta d1; UtxoState n1; CHECK(buildDelta(s.state(), b1, MAT, &d1, &n1, &e), "b1: " + e.str());
    CHECK(s.publish(d1, b1.hash, 1, &e), "pub b1: " + e.str());

    OutPoint p; p.txid = H(30); p.n = 0;
    OutPoint x; x.txid = H(40); x.n = 0;
    OutPoint y; y.txid = H(41); y.n = 0;
    Block b2; b2.hash = H(2); b2.height = 2;
    b2.txs.push_back(spendTx(40, p, 25*COIN));   // spends P, creates X (H40,0)
    b2.txs.push_back(spendTx(41, x, 20*COIN));   // spends X (intra-block), creates Y (H41,0)
    Delta d2; UtxoState n2;
    CHECK(buildDelta(s.state(), b2, MAT, &d2, &n2, &e), "b2: " + e.str());
    // order sanity: spend P, create X, spend X, create Y
    CHECK(d2.ops.size() == 4, "T2 delta op count == 4");
    CHECK(n2.has(y) && !n2.has(x) && !n2.has(p), "T2 intra-block create-then-spend result");
    CHECK(s.publish(d2, b2.hash, 2, &e), "pub b2: " + e.str());
    CHECK(s.state().has(y) && !s.state().has(x) && !s.state().has(p), "T2 published state");
    CHECK(s.disconnect(b2.hash, &e), "T2 disconnect: " + e.str());
    CHECK(s.state().has(p) && !s.state().has(x) && !s.state().has(y), "T2 disconnect restores P, removes X,Y");
    CHECK(s.disconnect(b1.hash, &e), "T2 disconnect b1: " + e.str());
    CHECK(s.state().digest() == d0, "T2 full unwind to empty");
    s.close();
}

// T3: duplicate txid / BIP30 boundary.
static void test_t3() {
    g_cur = "T3";
    std::string dir = freshDir("t3");
    ChainError e;
    CommitStore s; CHECK(s.open(dir, &e), "open: " + e.str());
    Block b1; b1.hash = H(1); b1.height = 1; b1.txs.push_back(coinbaseTx(50, 10*COIN));
    Delta d1; UtxoState n1; CHECK(buildDelta(s.state(), b1, MAT, &d1, &n1, &e), "b1: " + e.str());
    CHECK(s.publish(d1, b1.hash, 1, &e), "pub b1: " + e.str());
    Hash base = s.state().digest();

    // (a) duplicate unspent txid -> reject
    Block bad; bad.hash = H(2); bad.height = 2; bad.txs.push_back(coinbaseTx(50, 5*COIN)); // txid H(50) already unspent
    Delta db; UtxoState nb;
    bool ok = buildDelta(s.state(), bad, MAT, &db, &nb, &e);
    CHECK(!ok && e.code == ErrCode::DUP_OUTPOINT, "T3 reject duplicate unspent txid (BIP30)");
    CHECK(s.state().digest() == base, "T3 published state unchanged after rejected duplicate");

    // (b) re-create same txid after it was fully spent within the block -> allowed
    OutPoint p; p.txid = H(50); p.n = 0;
    Block b2; b2.hash = H(3); b2.height = 2;
    b2.txs.push_back(spendTx(60, p, 8*COIN));      // spends H(50) fully
    b2.txs.push_back(coinbaseTx(50, 3*COIN));       // recreates txid H(50) -> allowed (all prior spent)
    Delta d2; UtxoState n2;
    CHECK(buildDelta(s.state(), b2, MAT, &d2, &n2, &e), "T3 re-create after full spend allowed: " + e.str());
    CHECK(n2.has(p), "T3 re-created outpoint present");
    s.close();
}

// T4: failed validation leaves published state unchanged.
static void test_t4() {
    g_cur = "T4";
    std::string dir = freshDir("t4");
    ChainError e;
    CommitStore s; CHECK(s.open(dir, &e), "open: " + e.str());
    Block b1; b1.hash = H(1); b1.height = 1; b1.txs.push_back(coinbaseTx(70, 10*COIN));
    Delta d1; UtxoState n1; CHECK(buildDelta(s.state(), b1, MAT, &d1, &n1, &e), "b1: " + e.str());
    CHECK(s.publish(d1, b1.hash, 1, &e), "pub b1: " + e.str());
    Hash base = s.state().digest(); uint64_t g0 = s.generation();

    // missing input
    Block bad; bad.hash = H(2); bad.height = 2; OutPoint mp; mp.txid = H(99); mp.n = 0; bad.txs.push_back(spendTx(80, mp, 1*COIN));
    Delta db; UtxoState nb;
    bool ok = buildDelta(s.state(), bad, MAT, &db, &nb, &e);
    CHECK(!ok && e.code == ErrCode::MISSING_INPUT, "T4 reject missing input");
    CHECK(s.state().digest() == base && s.generation() == g0, "T4 state+generation unchanged");

    // immature coinbase spend (maturity 5, height 2, coin height 1 -> depth 1 < 5)
    OutPoint cb; cb.txid = H(70); cb.n = 0;
    Block imm; imm.hash = H(3); imm.height = 2; imm.txs.push_back(spendTx(81, cb, 1*COIN));
    Delta di; UtxoState ni;
    bool ok2 = buildDelta(s.state(), imm, 5, &di, &ni, &e);
    CHECK(!ok2 && e.code == ErrCode::IMMATURE, "T4 reject immature coinbase spend");
    CHECK(s.state().digest() == base, "T4 state unchanged after immature rejection");
    s.close();
}

// helper: open store, publish gen-1 coinbase block (H(10),50), return digest/gen
static bool setupOne(const std::string& dir, Hash* d1, ChainError* e) {
    CommitStore s;
    if (!s.open(dir, e)) return false;
    Block b1; b1.hash = H(100); b1.height = 1; b1.txs.push_back(coinbaseTx(10, 50*COIN));
    Delta dl; UtxoState ns;
    if (!buildDelta(s.state(), b1, MAT, &dl, &ns, e)) { s.close(); return false; }
    if (!s.publish(dl, b1.hash, 1, e)) { s.close(); return false; }
    *d1 = s.state().digest();
    s.close();
    return true;
}

// Publish a block spending H(10),0 -> H(20),0 with a crash point, in a child process.
static void childPublishWithCrash(const std::string& dir, CrashPoint cp, const std::string& mode) {
    ChainError e;
    CommitStore s;
    if (!s.open(dir, &e)) { fprintf(stderr, "child open failed: %s\n", e.str().c_str()); ::_exit(2); }
    OutPoint cb; cb.txid = H(10); cb.n = 0;
    Block b2; b2.hash = H(101); b2.height = 2; b2.txs.push_back(spendTx(20, cb, 40*COIN));
    Delta dl; UtxoState ns;
    if (!buildDelta(s.state(), b2, MAT, &dl, &ns, &e)) { fprintf(stderr, "child buildDelta failed: %s\n", e.str().c_str()); ::_exit(3); }
    if (mode == "crash") s.setCrashPoint(cp);
    if (mode == "nospace") s.injectNoSpace(true);
    if (mode == "fsyncfail") s.injectFsyncFail(true);
    bool ok = s.publish(dl, b2.hash, 2, &e);
    if (mode == "crash") { fprintf(stderr, "child: publish returned without crashing\n"); ::_exit(4); }
    ::_exit(ok ? 0 : 1);
}

// recover after a child run; returns digest and generation
static bool recoverState(const std::string& dir, Hash* dig, uint64_t* gen, bool* hadIssue, ChainError* e) {
    CommitStore s;
    if (!s.open(dir, e)) return false;
    *dig = s.state().digest(); *gen = s.generation(); *hadIssue = s.hadRecoveryIssue();
    s.close();
    return true;
}

static void test_t5() {
    g_cur = "T5";
    std::string dir = freshDir("t5");
    ChainError e; Hash d1;
    CHECK(setupOne(dir, &d1, &e), "setup: " + e.str());
    pid_t pid = fork();
    if (pid == 0) { childPublishWithCrash(dir, CrashPoint::BeforeRename, "crash"); ::_exit(7); }
    int st = 0; waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 42, "T5 child crashed at barrier");
    Hash dr; uint64_t gr; bool iss;
    CHECK(recoverState(dir, &dr, &gr, &iss, &e), "T5 reopen: " + e.str());
    // crash BEFORE manifest rename: MANIFEST unchanged -> deterministically OLD
    printf("  T5 observed: gen=%llu digest=%s (OLD digest=%s)\n",
           (unsigned long long)gr, hex(dr).c_str(), hex(d1).c_str());
    CHECK(dr == d1, "T5 recovered == OLD (deterministic)");
    CHECK(gr == 1, "T5 generation == 1 (OLD)");
}

static void test_t6() {
    g_cur = "T6";
    std::string dir = freshDir("t6");
    ChainError e; Hash d1;
    CHECK(setupOne(dir, &d1, &e), "setup: " + e.str());
    pid_t pid = fork();
    if (pid == 0) { childPublishWithCrash(dir, CrashPoint::AfterRename, "crash"); ::_exit(7); }
    int st = 0; waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 42, "T6 child crashed at barrier");
    Hash dr; uint64_t gr; bool iss;
    CHECK(recoverState(dir, &dr, &gr, &iss, &e), "T6 reopen: " + e.str());
    // crash AFTER rename, BEFORE dir fsync: OLD or NEW depending on filesystem; must be non-mixed.
    printf("  T6 observed: gen=%llu digest=%s ; OLD digest=%s\n",
           (unsigned long long)gr, hex(dr).c_str(), hex(d1).c_str());
    CHECK(dr == d1 || gr == 2, "T6 recovered is a single committed generation (OLD or NEW), non-mixed");
    if (dr == d1) CHECK(gr == 1, "T6 OLD branch: gen 1");
    else CHECK(gr == 2, "T6 NEW branch: gen 2");
    // idempotent re-resolution
    Hash dr2; uint64_t gr2; bool iss2;
    CHECK(recoverState(dir, &dr2, &gr2, &iss2, &e), "T6 reopen#2: " + e.str());
    CHECK(dr2 == dr && gr2 == gr, "T6 recovery resolution stable across reopens");
}

static void test_t7() {
    g_cur = "T7";
    std::string dir = freshDir("t7");
    ChainError e; Hash d1;
    CHECK(setupOne(dir, &d1, &e), "setup: " + e.str());
    pid_t pid = fork();
    if (pid == 0) { childPublishWithCrash(dir, CrashPoint::AfterDirSync, "crash"); ::_exit(7); }
    int st = 0; waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 42, "T7 child crashed at barrier");
    Hash dr; uint64_t gr; bool iss;
    CHECK(recoverState(dir, &dr, &gr, &iss, &e), "T7 reopen: " + e.str());
    // crash AFTER successful dir fsync: NEW (deterministic)
    printf("  T7 observed: gen=%llu digest=%s ; OLD digest=%s\n",
           (unsigned long long)gr, hex(dr).c_str(), hex(d1).c_str());
    CHECK(gr == 2, "T7 recovered == NEW (deterministic), generation 2");
    CHECK(dr != d1, "T7 recovered state differs from OLD");
}

static void test_t8() {
    g_cur = "T8";
    std::string dir = freshDir("t8");
    ChainError e; Hash d1;
    CHECK(setupOne(dir, &d1, &e), "setup: " + e.str());
    pid_t pid = fork();
    if (pid == 0) { childPublishWithCrash(dir, CrashPoint::AfterRename, "crash"); ::_exit(7); }
    int st = 0; waitpid(pid, &st, 0);
    Hash first; uint64_t fg = 0; bool fis;
    CHECK(recoverState(dir, &first, &fg, &fis, &e), "T8 recovery#1: " + e.str());
    for (int i = 0; i < 5; ++i) {
        Hash dr; uint64_t gr; bool iss;
        CHECK(recoverState(dir, &dr, &gr, &iss, &e), "T8 recovery reopen: " + e.str());
        CHECK(dr == first && gr == fg, "T8 repeated interrupted recovery is idempotent");
    }
}

static void test_t9() {
    g_cur = "T9";
    ChainError e;
    {   // (a) corrupt MANIFEST and MANIFEST.bak -> fail closed
        std::string dir = freshDir("t9a");
        Hash d1; CHECK(setupOne(dir, &d1, &e), "setup: " + e.str());
        const char* junk = "GARBAGE-MANIFEST-BYTES-NOT-VALID";
        for (const char* np : {"MANIFEST", "MANIFEST.bak"}) {
            FILE* f = fopen((dir + "/" + np).c_str(), "wb"); fwrite(junk, 1, strlen(junk), f); fclose(f);
        }
        CommitStore s; bool ok = s.open(dir, &e);
        CHECK(!ok, "T9a corrupt MANIFEST+bak -> open fails closed");
        CHECK(e.code == ErrCode::BAD_SERIALIZE || e.code == ErrCode::CORRUPT || e.code == ErrCode::UNKNOWN_VERSION, "T9a error class is corruption/version");
    }
    {   // (b) corrupt current state file, valid bak -> recover to previous committed (never garbage)
        std::string dir = freshDir("t9b");
        CommitStore s; CHECK(s.open(dir, &e), "open: " + e.str());
        Block b1; b1.hash = H(100); b1.height = 1; b1.txs.push_back(coinbaseTx(10, 50*COIN));
        Delta dl1; UtxoState n1; buildDelta(s.state(), b1, MAT, &dl1, &n1, &e); s.publish(dl1, b1.hash, 1, &e);
        Hash dGen1 = s.state().digest();
        OutPoint cb; cb.txid = H(10); cb.n = 0;
        Block b2; b2.hash = H(101); b2.height = 2; b2.txs.push_back(spendTx(20, cb, 40*COIN));
        Delta dl2; UtxoState n2; buildDelta(s.state(), b2, MAT, &dl2, &n2, &e); CHECK(s.publish(dl2, b2.hash, 2, &e), "pub b2: " + e.str());
        s.close();
        // flip a byte in state-2.dat
        std::string sp = dir + "/state-2.dat";
        FILE* f = fopen(sp.c_str(), "r+b"); if (f) { fseek(f, 12, SEEK_SET); int c = fgetc(f); fseek(f, 12, SEEK_SET); fputc(c ^ 0xFF, f); fclose(f); }
        Hash dr; uint64_t gr; bool iss;
        bool ok = recoverState(dir, &dr, &gr, &iss, &e);
        CHECK(ok, "T9b open with corrupt state + valid bak succeeds (fallback)");
        CHECK(iss == true, "T9b recovery issue flagged");
        CHECK(dr == dGen1 && gr == 1, "T9b recovered exactly previous committed state");
    }
    {   // (c) corrupt/truncated undo with no valid previous -> fail closed
        std::string dir = freshDir("t9c");
        Hash d1; CHECK(setupOne(dir, &d1, &e), "setup: " + e.str());
        std::string up = dir + "/undo.dat";
        FILE* f = fopen(up.c_str(), "wb"); const char* j = "XX"; fwrite(j, 1, 2, f); fclose(f); // truncate to 2 bytes
        CommitStore s; bool ok = s.open(dir, &e);
        CHECK(!ok, "T9c corrupt undo -> open fails closed");
        CHECK(e.code == ErrCode::CORRUPT, "T9c error class CORRUPT");
    }
}

static void test_t10() {
    g_cur = "T10";
    ChainError e;
    for (const std::string mode : {std::string("nospace"), std::string("fsyncfail")}) {
        std::string dir = freshDir("t10_" + mode);
        Hash d1; CHECK(setupOne(dir, &d1, &e), "setup: " + e.str());
        pid_t pid = fork();
        if (pid == 0) { childPublishWithCrash(dir, CrashPoint::None, mode); ::_exit(7); }
        int st = 0; waitpid(pid, &st, 0);
        CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 1, "T10 " + mode + ": publish failed (no commit)");
        Hash dr; uint64_t gr; bool iss;
        CHECK(recoverState(dir, &dr, &gr, &iss, &e), "T10 reopen: " + e.str());
        CHECK(dr == d1 && gr == 1, "T10 " + mode + ": no partial published state (still gen 1 OLD)");
    }
}

static void test_t11() {
    g_cur = "T11";
    std::string dir = freshDir("t11");
    ChainError e1, e2;
    CommitStore a; CHECK(a.open(dir, &e1), "open a: " + e1.str());
    CommitStore b; bool ok = b.open(dir, &e2);
    CHECK(!ok && e2.code == ErrCode::LOCKED, "T11 second writer rejected (LOCKED)");
    b.close();
    a.close();
    CommitStore c; CHECK(c.open(dir, &e2), "T11 reopen after release: " + e2.str());
    c.close();
}

static void test_t12() {
    g_cur = "T12";
    std::string dir = freshDir("t12");
    ChainError e;
    CommitStore s; CHECK(s.open(dir, &e), "open: " + e.str());
    Block b1; b1.hash = H(100); b1.height = 1; b1.txs.push_back(coinbaseTx(10, 50*COIN));
    Delta dl1; UtxoState n1; buildDelta(s.state(), b1, MAT, &dl1, &n1, &e); CHECK(s.publish(dl1, b1.hash, 1, &e), "pub1: " + e.str());
    OutPoint cb; cb.txid = H(10); cb.n = 0;
    Block b2; b2.hash = H(101); b2.height = 2; b2.txs.push_back(spendTx(20, cb, 40*COIN));
    Delta dl2; UtxoState n2; buildDelta(s.state(), b2, MAT, &dl2, &n2, &e); CHECK(s.publish(dl2, b2.hash, 2, &e), "pub2: " + e.str());
    Hash expected = s.state().digest(); uint64_t eg = s.generation();
    s.close();
    CommitStore r; CHECK(r.open(dir, &e), "reopen: " + e.str());
    CHECK(r.state().digest() == expected, "T12 restart reads exactly selected state");
    CHECK(r.generation() == eg, "T12 restart generation exact");
    CHECK(r.tipHeight() == 2, "T12 restart tip height exact");
    r.close();
}

// T13: bounded prototype stress (connect-only). NOT an IBD test: the prototype
// rewrites the full state file per commit (CoW) by design; production uses the
// delta path. Purpose: exercise bounded buffers and report measured prototype RSS.
static void test_t13() {
    g_cur = "T13";
    int N = 400; { const char* s = getenv("CHAINSTATE_STRESS_N"); if (s) N = atoi(s); }
    std::string dir = freshDir("t13");
    ChainError e;
    CommitStore s; CHECK(s.open(dir, &e), "open: " + e.str());
    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < N; ++i) {
        Hash tid; for (int k = 0; k < 32; ++k) tid[k] = 0xA5;
        tid[0] = (uint8_t)(i & 0xff); tid[1] = (uint8_t)((i >> 8) & 0xff); tid[2] = (uint8_t)((i >> 16) & 0xff); tid[3] = (uint8_t)((i >> 24) & 0xff);
        Hash bh; for (int k = 0; k < 32; ++k) bh[k] = 0x5A;
        bh[0] = (uint8_t)(i & 0xff); bh[1] = (uint8_t)((i >> 8) & 0xff); bh[2] = (uint8_t)((i >> 16) & 0xff); bh[3] = (uint8_t)((i >> 24) & 0xff);
        Tx t; t.txid = tid; t.coinbase = true; t.coinstake = false;
        TxOut o; o.value = 1 * COIN; o.script = {1}; t.vout.push_back(o);
        Block b; b.hash = bh; b.height = i + 1; b.txs.push_back(t);
        Delta d; UtxoState ns;
        if (!buildDelta(s.state(), b, 1, &d, &ns, &e)) { CHECK(false, "stress buildDelta: " + e.str()); break; }
        if (!s.publish(d, b.hash, i + 1, &e)) { CHECK(false, "stress publish: " + e.str()); break; }
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    Hash expected = s.state().digest();
    size_t sfc = s.stateFileCount();
    CHECK((long)s.state().size() > 0, "stress state populated");
    CHECK(sfc <= 2, "stress GC keeps <=2 generation files");
    s.close();
    CommitStore r; CHECK(r.open(dir, &e), "stress reopen: " + e.str());
    CHECK(r.state().digest() == expected, "stress restart reads exact committed state");
    CHECK(r.generation() == (uint64_t)N, "stress generation == N");
    r.close();
    printf("  T13 stress: N=%d elapsed=%.1fms stateFiles=%zu peakRSS_KB=%ld\n",
           N, ms, sfc, readVmHWM());
}

static long readVmHWM() {
    FILE* f = fopen("/proc/self/status", "r");
    if (!f) return -1;
    char line[256]; long kb = -1;
    while (fgets(line, sizeof(line), f)) { if (strncmp(line, "VmHWM:", 6) == 0) { sscanf(line + 6, "%ld", &kb); break; } }
    fclose(f);
    return kb;
}

int main() {
    setvbuf(stdout, NULL, _IONBF, 0);
    struct Case { const char* name; void (*fn)(); };
    Case cases[] = {
        {"T0-sha256", test_sha},
        {"T1", test_t1}, {"T2", test_t2}, {"T3", test_t3}, {"T4", test_t4},
        {"T5", test_t5}, {"T6", test_t6}, {"T7", test_t7}, {"T8", test_t8},
        {"T9", test_t9}, {"T10", test_t10}, {"T11", test_t11}, {"T12", test_t12},
        {"T13", test_t13},
    };
    printf("== chainstate vertical slice test suite ==\n");
    for (auto& c : cases) {
        int p0 = g_pass, f0 = g_fail;
        c.fn();
        printf("%-10s %s  (checks +%d, failures %d)\n", c.name,
               (g_fail == f0 ? "PASS" : "FAIL"), g_pass - p0, g_fail - f0);
    }
    printf("\n---- summary ----\n");
    for (auto& m : g_failmsg) printf("%s\n", m.c_str());
    printf("PASS=%d FAIL=%d\n", g_pass, g_fail);
    long hwm = readVmHWM();
    printf("PEAK_RSS_KB=%ld\n", hwm);
    return g_fail == 0 ? 0 : 1;
}
