// Isolated chainstate vertical slice - focused test suite (Stage 2B).
// T0..T13 regression + T14..T17 remediation (F01..F07).
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

static long readVmHWM();   // forward

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
static bool fileExists(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0; }
static bool writeJunk(const std::string& p, const char* junk) {
    FILE* f = fopen(p.c_str(), "wb"); if (!f) return false;
    size_t n = strlen(junk); bool ok = fwrite(junk, 1, n, f) == n; fclose(f); return ok;
}
static bool truncateFile(const std::string& p, long bytes) {
    FILE* f = fopen(p.c_str(), "r+b"); if (!f) return false; bool ok = ftruncate(fileno(f), bytes) == 0; fclose(f); return ok;
}
static bool flipByte(const std::string& p, long off) {
    FILE* f = fopen(p.c_str(), "r+b"); if (!f) return false;
    if (fseek(f, off, SEEK_SET) != 0) { fclose(f); return false; }
    int c = fgetc(f); if (c == EOF) { fclose(f); return false; }
    fseek(f, off, SEEK_SET); fputc(c ^ 0xFF, f); fclose(f); return true;
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

    Block bad; bad.hash = H(2); bad.height = 2; bad.txs.push_back(coinbaseTx(50, 5*COIN)); // txid H(50) already unspent
    Delta db; UtxoState nb;
    bool ok = buildDelta(s.state(), bad, MAT, &db, &nb, &e);
    CHECK(!ok && e.code == ErrCode::DUP_OUTPOINT, "T3 reject duplicate unspent txid (BIP30)");
    CHECK(s.state().digest() == base, "T3 published state unchanged after rejected duplicate");

    OutPoint p; p.txid = H(50); p.n = 0;
    Block b2; b2.hash = H(3); b2.height = 2;
    b2.txs.push_back(spendTx(60, p, 8*COIN));      // spends H(50) fully
    b2.txs.push_back(coinbaseTx(50, 3*COIN));       // recreates txid H(50) -> allowed
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

    Block bad; bad.hash = H(2); bad.height = 2; OutPoint mp; mp.txid = H(99); mp.n = 0; bad.txs.push_back(spendTx(80, mp, 1*COIN));
    Delta db; UtxoState nb;
    bool ok = buildDelta(s.state(), bad, MAT, &db, &nb, &e);
    CHECK(!ok && e.code == ErrCode::MISSING_INPUT, "T4 reject missing input");
    CHECK(s.state().digest() == base && s.generation() == g0, "T4 state+generation unchanged");

    OutPoint cb; cb.txid = H(70); cb.n = 0;
    Block imm; imm.hash = H(3); imm.height = 2; imm.txs.push_back(spendTx(81, cb, 1*COIN));
    Delta di; UtxoState ni;
    bool ok2 = buildDelta(s.state(), imm, 5, &di, &ni, &e);
    CHECK(!ok2 && e.code == ErrCode::IMMATURE, "T4 reject immature coinbase spend");
    CHECK(s.state().digest() == base, "T4 state unchanged after immature rejection");
    s.close();
}

// helper: open store, publish gen-1 coinbase block (H(10),50), return digest
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
static bool publishBlock2(const std::string& dir, Hash* d2, ChainError* e) {
    CommitStore s; if (!s.open(dir, e)) return false;
    OutPoint cb; cb.txid = H(10); cb.n = 0;
    Block b2; b2.hash = H(101); b2.height = 2; b2.txs.push_back(spendTx(20, cb, 40*COIN));
    Delta dl; UtxoState ns;
    if (!buildDelta(s.state(), b2, MAT, &dl, &ns, e)) { s.close(); return false; }
    if (!s.publish(dl, b2.hash, 2, e)) { s.close(); return false; }
    *d2 = s.state().digest(); s.close(); return true;
}
// digest of the state that a connect of block2 (spend H(10),0 -> H(20),0) would produce
static bool expectedBlock2Digest(const std::string& dir, Hash* out, ChainError* e) {
    CommitStore s; if (!s.open(dir, e)) return false;
    OutPoint cb; cb.txid = H(10); cb.n = 0;
    Block b2; b2.hash = H(101); b2.height = 2; b2.txs.push_back(spendTx(20, cb, 40*COIN));
    Delta dl; UtxoState ns;
    if (!buildDelta(s.state(), b2, MAT, &dl, &ns, e)) { s.close(); return false; }
    *out = ns.digest(); s.close(); return true;
}

static void childPublishWithCrash(const std::string& dir, CrashPoint cp, InjectStage st, const std::string& mode) {
    ChainError e;
    CommitStore s;
    if (!s.open(dir, &e)) { fprintf(stderr, "child open failed: %s\n", e.str().c_str()); ::_exit(2); }
    OutPoint cb; cb.txid = H(10); cb.n = 0;
    Block b2; b2.hash = H(101); b2.height = 2; b2.txs.push_back(spendTx(20, cb, 40*COIN));
    Delta dl; UtxoState ns;
    if (!buildDelta(s.state(), b2, MAT, &dl, &ns, &e)) { fprintf(stderr, "child buildDelta failed: %s\n", e.str().c_str()); ::_exit(3); }
    if (mode == "crash") s.setCrashPoint(cp);
    if (mode == "nospace") { s.setInjectStage(st); s.injectNoSpace(true); }
    if (mode == "fsyncfail") { s.setInjectStage(st); s.injectFsyncFail(true); }
    bool ok = s.publish(dl, b2.hash, 2, &e);
    if (mode == "crash") { fprintf(stderr, "child: publish returned without crashing\n"); ::_exit(4); }
    ::_exit(ok ? 0 : 1);
}

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
    if (pid == 0) { childPublishWithCrash(dir, CrashPoint::BeforeRename, InjectStage::None, "crash"); ::_exit(7); }
    int st = 0; waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 42, "T5 child crashed at barrier");
    Hash dr; uint64_t gr; bool iss;
    CHECK(recoverState(dir, &dr, &gr, &iss, &e), "T5 reopen: " + e.str());
    printf("  T5 observed: gen=%llu digest=%s (OLD digest=%s)\n", (unsigned long long)gr, hex(dr).c_str(), hex(d1).c_str());
    CHECK(dr == d1 && gr == 1, "T5 recovered == OLD (deterministic)");
}

static void test_t6() {
    g_cur = "T6";
    std::string dir = freshDir("t6");
    ChainError e; Hash d1;
    CHECK(setupOne(dir, &d1, &e), "setup: " + e.str());
    Hash dNew;
    CHECK(expectedBlock2Digest(dir, &dNew, &e), "T6 expected NEW digest: " + e.str());
    pid_t pid = fork();
    if (pid == 0) { childPublishWithCrash(dir, CrashPoint::AfterRename, InjectStage::None, "crash"); ::_exit(7); }
    int st = 0; waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 42, "T6 child crashed at barrier");
    Hash dr; uint64_t gr; bool iss;
    CHECK(recoverState(dir, &dr, &gr, &iss, &e), "T6 reopen: " + e.str());
    printf("  T6 observed: gen=%llu digest=%s ; OLD=%s NEW=%s\n",
           (unsigned long long)gr, hex(dr).c_str(), hex(d1).c_str(), hex(dNew).c_str());
    // F04: accept ONLY exactly (gen=1,digest=OLD) or (gen=2,digest=NEW); reject mixed/unexpected.
    bool oldBranch = (gr == 1 && dr == d1);
    bool newBranch = (gr == 2 && dr == dNew);
    CHECK(oldBranch || newBranch, "T6 recovered is exactly OLD or exactly NEW (no mixed/new-digest combinations)");
    if (!(oldBranch || newBranch)) {
        CHECK(false, "T6 MIXED/UNEXPECTED: gen=" + std::to_string(gr) + " digest=" + hex(dr));
    }
    Hash dr2; uint64_t gr2; bool iss2;
    CHECK(recoverState(dir, &dr2, &gr2, &iss2, &e), "T6 reopen#2: " + e.str());
    CHECK(dr2 == dr && gr2 == gr, "T6 recovery resolution stable across reopens");
}

static void test_t7() {
    g_cur = "T7";
    std::string dir = freshDir("t7");
    ChainError e; Hash d1;
    CHECK(setupOne(dir, &d1, &e), "setup: " + e.str());
    Hash dNew; CHECK(expectedBlock2Digest(dir, &dNew, &e), "T7 expected NEW: " + e.str());
    pid_t pid = fork();
    if (pid == 0) { childPublishWithCrash(dir, CrashPoint::AfterDirSync, InjectStage::None, "crash"); ::_exit(7); }
    int st = 0; waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 42, "T7 child crashed at barrier");
    Hash dr; uint64_t gr; bool iss;
    CHECK(recoverState(dir, &dr, &gr, &iss, &e), "T7 reopen: " + e.str());
    printf("  T7 observed: gen=%llu digest=%s ; NEW=%s\n", (unsigned long long)gr, hex(dr).c_str(), hex(dNew).c_str());
    CHECK(gr == 2 && dr == dNew, "T7 recovered == NEW (deterministic, exact digest)");
}

static void test_t8() {
    g_cur = "T8";
    std::string dir = freshDir("t8");
    ChainError e; Hash d1;
    CHECK(setupOne(dir, &d1, &e), "setup: " + e.str());
    pid_t pid = fork();
    if (pid == 0) { childPublishWithCrash(dir, CrashPoint::AfterRename, InjectStage::None, "crash"); ::_exit(7); }
    int st = 0; waitpid(pid, &st, 0);
    Hash first; uint64_t fg = 0; bool fis;
    CHECK(recoverState(dir, &first, &fg, &fis, &e), "T8 recovery#1: " + e.str());
    for (int i = 0; i < 5; ++i) {
        Hash dr; uint64_t gr; bool iss;
        CHECK(recoverState(dir, &dr, &gr, &iss, &e), "T8 recovery reopen: " + e.str());
        CHECK(dr == first && gr == fg, "T8 repeated interrupted recovery is idempotent");
    }
}

// T9: acknowledged-state corruption MUST fail loud (F01); fail-closed when evidence insufficient.
static void test_t9() {
    g_cur = "T9";
    ChainError e;
    {   // (a) corrupt MANIFEST and MANIFEST.bak (acknowledged gen) -> fail closed
        std::string dir = freshDir("t9a");
        Hash d1; CHECK(setupOne(dir, &d1, &e), "setup: " + e.str());
        writeJunk(dir + "/MANIFEST", "GARBAGE-MANIFEST-BYTES-NOT-VALID");
        writeJunk(dir + "/MANIFEST.bak", "GARBAGE-BAK-BYTES-NOT-VALID");
        CommitStore s; bool ok = s.open(dir, &e);
        CHECK(!ok, "T9a corrupt MANIFEST+bak -> open fails closed");
    }
    {   // (b) F01 auditor scenario: acknowledged undo tail corruption with valid bak -> FAIL LOUD
        std::string dir = freshDir("t9b");
        Hash d1; CHECK(setupOne(dir, &d1, &e), "setup: " + e.str());
        Hash d2; CHECK(publishBlock2(dir, &d2, &e), "publish gen2: " + e.str());  // acknowledged gen 2, bak = gen 1
        struct stat st; off_t sz = (stat((dir + "/undo.dat").c_str(), &st) == 0) ? st.st_size : 0;
        CHECK(sz > 0, "T9b undo.dat non-empty");
        CHECK(truncateFile(dir + "/undo.dat", (long)(sz / 2)), "T9b corrupt acknowledged undo tail");
        CommitStore s; bool ok = s.open(dir, &e);
        CHECK(!ok, "T9b acknowledged undo-tail corruption -> open FAILS (no silent rollback)");
        CHECK(e.code == ErrCode::CORRUPT, "T9b error class CORRUPT");
    }
    {   // (c) corrupt acknowledged state file with valid bak -> FAIL LOUD
        std::string dir = freshDir("t9c");
        Hash d1; CHECK(setupOne(dir, &d1, &e), "setup: " + e.str());
        Hash d2; CHECK(publishBlock2(dir, &d2, &e), "publish gen2: " + e.str());
        std::string sp = dir + "/state-2.dat";
        bool flipped = false;
        for (long off : {12L, 20L, 28L}) if (flipByte(sp, off)) { flipped = true; break; }
        CHECK(flipped, "T9c corrupt acknowledged state file");
        CommitStore s; bool ok = s.open(dir, &e);
        CHECK(!ok, "T9c acknowledged state corruption -> open FAILS (no silent rollback)");
    }
    {   // (d) missing referenced state file with valid bak -> FAIL LOUD
        std::string dir = freshDir("t9d");
        Hash d1; CHECK(setupOne(dir, &d1, &e), "setup: " + e.str());
        Hash d2; CHECK(publishBlock2(dir, &d2, &e), "publish gen2: " + e.str());
        std::string sp = dir + "/state-2.dat";
        CHECK(unlink(sp.c_str()) == 0, "T9d remove referenced state file");
        CommitStore s; bool ok = s.open(dir, &e);
        CHECK(!ok, "T9d missing referenced state (acknowledged) -> open FAILS");
    }
    {   // (e) corrupt undo, no valid previous (gen1, no bak) -> fail closed
        std::string dir = freshDir("t9e");
        Hash d1; CHECK(setupOne(dir, &d1, &e), "setup: " + e.str());
        struct stat st; off_t sz = (stat((dir + "/undo.dat").c_str(), &st) == 0) ? st.st_size : 0;
        CHECK(truncateFile(dir + "/undo.dat", (long)(sz > 2 ? 2 : 0)), "T9e truncate undo");
        CommitStore s; bool ok = s.open(dir, &e);
        CHECK(!ok, "T9e corrupt undo with no valid backup -> fail closed");
        CHECK(e.code == ErrCode::CORRUPT, "T9e error class CORRUPT");
    }
}

// T10: per-barrier failure injection (F05).
static void test_t10() {
    g_cur = "T10";
    ChainError e;
    struct Stage { const char* name; InjectStage st; bool postRename; const char* mode; };
    Stage stages[] = {
        {"StateStage",    InjectStage::StateStage,    false, "fsyncfail"},
        {"StateStage-ns", InjectStage::StateStage,    false, "nospace"},
        {"UndoAppend",    InjectStage::UndoAppend,    false, "fsyncfail"},
        {"ManifestTmp",   InjectStage::ManifestTmp,   false, "fsyncfail"},
        {"BakWrite",      InjectStage::BakWrite,      false, "fsyncfail"},
        {"PrimaryRename", InjectStage::PrimaryRename, false, "nospace"},
        {"DirFsync",      InjectStage::DirFsync,      true,  "fsyncfail"},
        {"AckWrite",      InjectStage::AckWrite,      true,  "fsyncfail"},
    };
    for (Stage& sg : stages) {
        std::string dir = freshDir(std::string("t10_") + sg.name);
        Hash d1; CHECK(setupOne(dir, &d1, &e), std::string(sg.name) + " setup: " + e.str());
        Hash dNew; expectedBlock2Digest(dir, &dNew, &e);
        pid_t pid = fork();
        if (pid == 0) { childPublishWithCrash(dir, CrashPoint::None, sg.st, sg.mode); ::_exit(7); }
        int st = 0; waitpid(pid, &st, 0);
        CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 1, std::string(sg.name) + ": publish returned failure (no acknowledged commit)");
        Hash dr; uint64_t gr; bool iss;
        CHECK(recoverState(dir, &dr, &gr, &iss, &e), std::string(sg.name) + " reopen: " + e.str());
        if (!sg.postRename) {
            CHECK(dr == d1 && gr == 1, std::string(sg.name) + ": pre-rename failure -> exactly OLD (gen1)");
        } else {
            bool oldB = (gr == 1 && dr == d1), newB = (gr == 2 && dr == dNew);
            CHECK(oldB || newB, std::string(sg.name) + ": post-rename failure -> exactly OLD or NEW (no mixed)");
        }
    }
}

static void test_t11() {
    g_cur = "T11";
    std::string dir = freshDir("t11");
    ChainError e1, e2;
    CommitStore a; CHECK(a.open(dir, &e1), "open a: " + e1.str());
    CommitStore b; bool ok = b.open(dir, &e2);
    CHECK(!ok && e2.code == ErrCode::LOCKED, "T11 second same-process writer rejected (LOCKED)");
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

// T13: bounded prototype stress + exact GC membership (F06). NOT an IBD test.
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
    // F06: exact retained-generation membership {N-1, N}
    std::vector<uint64_t> gens = s.stateFileGens();
    CHECK(gens.size() == 2 && gens[0] == (uint64_t)(N - 1) && gens[1] == (uint64_t)N,
          "T13 GC retains exactly {N-1, N}; got size=" + std::to_string(gens.size()));
    CHECK(s.undoCount() == (size_t)N, "T13 undo index size == N (metadata only)");
    s.close();
    CommitStore r; CHECK(r.open(dir, &e), "stress reopen: " + e.str());
    CHECK(r.state().digest() == expected, "stress restart reads exact committed state");
    CHECK(r.generation() == (uint64_t)N, "stress generation == N");
    r.close();
    printf("  T13 stress: N=%d elapsed=%.1fms retainedGens=%zu undoIndex=%zu peakRSS_KB=%ld\n",
           N, ms, gens.size(), (size_t)N, readVmHWM());
}

// T14: legitimate crash recovery (F01 case A) - an *unacknowledged* corrupt primary
// is discarded in favour of the acknowledged generation, and staging files do not block it.
static void test_t14() {
    g_cur = "T14";
    std::string dir = freshDir("t14");
    ChainError e; Hash d1;
    CHECK(setupOne(dir, &d1, &e), "setup: " + e.str());   // gen1 acknowledged, ACK=1
    pid_t pid = fork();
    if (pid == 0) { childPublishWithCrash(dir, CrashPoint::AfterRename, InjectStage::None, "crash"); ::_exit(7); }
    int st = 0; waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 42, "T14 child crashed after rename (unacknowledged gen2)");
    // leave stray staging artifacts; must not block recovery
    writeJunk(dir + "/state-9.dat", "STRAY");
    writeJunk(dir + "/state-9.dat.tmp", "STRAY");
    // corrupt the (unacknowledged) primary MANIFEST
    writeJunk(dir + "/MANIFEST", "CORRUPT-UNACKED-PRIMARY");
    Hash dr; uint64_t gr; bool iss;
    bool ok = recoverState(dir, &dr, &gr, &iss, &e);
    CHECK(ok, "T14 unacknowledged corrupt primary -> recovery succeeds: " + e.str());
    CHECK(iss == true, "T14 recovery issue flagged");
    CHECK(dr == d1 && gr == 1, "T14 recovered to acknowledged generation (gen1)");
    CHECK(!fileExists(dir + "/state-9.dat"), "T14 stray state GC'd");
    // and the store is usable (MANIFEST repaired)
    { CommitStore s; CHECK(s.open(dir, &e), "T14 reopen after repair: " + e.str());
      Hash d2; Block b2; b2.hash = H(101); b2.height = 2; OutPoint cb; cb.txid = H(10); cb.n = 0; b2.txs.push_back(spendTx(20, cb, 40*COIN));
      Delta dl; UtxoState ns; CHECK(buildDelta(s.state(), b2, MAT, &dl, &ns, &e), "T14 build after repair: " + e.str());
      CHECK(s.publish(dl, b2.hash, 2, &e), "T14 publish after repair works: " + e.str()); (void)d2; s.close(); }
}

// T15: checked backup establishment (F02) - publish aborts if the required fallback
// reference cannot be durably established, and no partial state is left behind.
static void test_t15() {
    g_cur = "T15";
    std::string dir = freshDir("t15");
    ChainError e; Hash d1;
    CHECK(setupOne(dir, &d1, &e), "setup: " + e.str());
    CommitStore s; CHECK(s.open(dir, &e), "open: " + e.str());
    OutPoint cb; cb.txid = H(10); cb.n = 0;
    Block b2; b2.hash = H(101); b2.height = 2; b2.txs.push_back(spendTx(20, cb, 40*COIN));
    Delta dl; UtxoState ns; CHECK(buildDelta(s.state(), b2, MAT, &dl, &ns, &e), "build: " + e.str());
    s.setInjectStage(InjectStage::BakWrite); s.injectFsyncFail(true);
    bool ok = s.publish(dl, b2.hash, 2, &e);
    CHECK(!ok, "T15 publish aborts when backup cannot be established");
    s.close();
    CHECK(!fileExists(dir + "/state-2.dat"), "T15 no staged state promoted on abort");
    CHECK(!fileExists(dir + "/MANIFEST.tmp"), "T15 no MANIFEST.tmp left on abort");
    Hash dr; uint64_t gr; bool iss;
    CHECK(recoverState(dir, &dr, &gr, &iss, &e), "T15 reopen: " + e.str());
    CHECK(dr == d1 && gr == 1, "T15 published state unchanged (gen1 OLD)");
}

// T16: cross-process single-writer lock (F07) incl. abnormal-exit release.
static void test_t16() {
    g_cur = "T16";
    std::string dir = freshDir("t16");
    int rdy[2], sig[2];
    if (pipe(rdy) != 0 || pipe(sig) != 0) { CHECK(false, "pipe"); return; }
    pid_t pid = fork();
    if (pid == 0) {
        ::close(rdy[0]); ::close(sig[1]);
        ChainError e; CommitStore a; bool ok = a.open(dir, &e);
        char c = ok ? '1' : '0';
        if (::write(rdy[1], &c, 1) != 1) ::_exit(9);
        if (!ok) ::_exit(3);
        char b; if (::read(sig[0], &b, 1) != 1) ::_exit(4);
        a.close();
        ::_exit(0);
    }
    ::close(rdy[1]); ::close(sig[0]);
    char c = 0; if (::read(rdy[0], &c, 1) != 1) { CHECK(false, "child ready"); return; }
    CHECK(c == '1', "T16 subprocess acquired writer lock");
    ChainError e2; CommitStore p; bool ok = p.open(dir, &e2);
    CHECK(!ok && e2.code == ErrCode::LOCKED, "T16 parent cannot acquire lock held by subprocess");
    p.close();
    { char b = 'x'; if (::write(sig[1], &b, 1) != 1) {} }
    int st = 0; waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "T16 subprocess released lock and exited");
    CommitStore q; CHECK(q.open(dir, &e2), "T16 parent acquires lock after subprocess release: " + e2.str());
    q.close();

    // abnormal exit (no explicit unlock) must still release the OS lock
    std::string dir2 = freshDir("t16b");
    int rdy2[2];
    if (pipe(rdy2) != 0) { CHECK(false, "pipe2"); return; }
    pid_t pid2 = fork();
    if (pid2 == 0) {
        ::close(rdy2[0]);
        ChainError e; CommitStore a; bool o = a.open(dir2, &e);
        char c2 = o ? '1' : '0';
        if (::write(rdy2[1], &c2, 1) != 1) ::_exit(9);
        ::_exit(0);   // abnormal: no close(), rely on OS release
    }
    ::close(rdy2[1]);
    char c2 = 0; if (::read(rdy2[0], &c2, 1) != 1) { CHECK(false, "child2 ready"); return; }
    CHECK(c2 == '1', "T16b subprocess holds lock");
    int st2 = 0; waitpid(pid2, &st2, 0);
    ChainError e3; CommitStore z; CHECK(z.open(dir2, &e3), "T16b OS releases lock on abnormal subprocess exit: " + e3.str());
    z.close();
}

// T17: acknowledgment contract (F01 infrastructure) - ACK tracks the committed generation,
// and a recovered unacknowledged generation is reconciled into ACK.
static void test_t17() {
    g_cur = "T17";
    std::string dir = freshDir("t17");
    ChainError e;
    { CommitStore s; CHECK(s.open(dir, &e), "open: " + e.str());
      CHECK(!s.ackPresent() && s.acknowledgedGeneration() == 0, "T17 fresh store has no ACK (gen 0 default)");
      Block b1; b1.hash = H(100); b1.height = 1; b1.txs.push_back(coinbaseTx(10, 50*COIN));
      Delta dl; UtxoState n1; buildDelta(s.state(), b1, MAT, &dl, &n1, &e);
      CHECK(s.publish(dl, b1.hash, 1, &e), "T17 publish gen1: " + e.str());
      CHECK(s.acknowledgedGeneration() == 1, "T17 ACK == committed generation after publish");
      s.close(); }
    { CommitStore s; CHECK(s.open(dir, &e), "T17 reopen: " + e.str());
      CHECK(s.acknowledgedGeneration() == 1 && s.generation() == 1, "T17 ACK stable across clean reopen");
      s.close(); }
    // crash after rename -> primary gen2 visible, ACK still 1; recovery must reconcile ACK to 2
    pid_t pid = fork();
    if (pid == 0) { childPublishWithCrash(dir, CrashPoint::AfterRename, InjectStage::None, "crash"); ::_exit(7); }
    int st = 0; waitpid(pid, &st, 0);
    { CommitStore s; CHECK(s.open(dir, &e), "T17 reopen after crash: " + e.str());
      CHECK(s.generation() == 2, "T17 recovered unacknowledged gen2");
      CHECK(s.acknowledgedGeneration() == 2, "T17 ACK reconciled to recovered generation");
      s.close(); }
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
        {"T13", test_t13}, {"T14", test_t14}, {"T15", test_t15}, {"T16", test_t16},
        {"T17", test_t17},
    };
    printf("== chainstate vertical slice test suite (Stage 2B) ==\n");
    const char* only = getenv("CHAINSTATE_ONLY");
    for (auto& c : cases) {
        if (only && *only && strstr(only, c.name) == NULL) continue;
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
