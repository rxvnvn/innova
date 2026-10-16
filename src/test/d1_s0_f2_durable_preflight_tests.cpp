// Copyright (c) 2026 The Innova developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// D1-S0-F2 — DURABLE ARCHIVE PREFLIGHT (Stage 3B-D1-S0-F2).
//
// Fixes the independent S0-F1 audit's HIGH finding F1-1: the incompatible-index reset renamed
// `txleveldb` -> `txleveldb_backup` BEFORE the archive preflight ran, so a refused reset left no
// `txleveldb`, the next startup silently created a fresh empty index at DATABASE_VERSION and the guard
// never ran again (guard bypass + silent legacy index-continuity loss).
//
// Invariant under test: a REFUSED reset preserves the original index and stays refused on every later
// startup, until the archive problem is corrected; a CORRECTED archive then allows the legitimate reset
// with the old index preserved as the backup.
//
// All cases use isolated synthetic scratch datadirs (never a real blockchain datadir, never the shared
// test datadir). Nothing here deletes, moves or rewrites archive data: the payloads are only hashed.
//
// Case map (report innova-v2-chainstate-stage3b-d1-s0-f2-20261016.md):
//   A  refused reset is durable: index intact, no backup, refusal repeats across startups
//   B  recovery: correct the archive -> the legitimate reset succeeds, old index preserved as backup
//   C  failed backup rename stays non-destructive (preflight passes, rename fails, no reset)
//   D  read-only and already-compatible paths are unchanged (preflight does not run there)
//   E  the constructor preflight and the reset-path check share one decision and one wording

#include <boost/test/unit_test.hpp>
#include <boost/filesystem.hpp>
#include <boost/system/error_code.hpp>

#include <openssl/sha.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include "main.h"
#include "txdb-leveldb.h"
#include "uint256.h"
#include "util.h"
#include "version.h"

extern void* GetGlobalTxdbPtrForTest(); // test-only accessor defined in txdb-leveldb.cpp
extern bool fPrintToDebugger;           // util.cpp (set true by TestingSetup)

namespace fs = boost::filesystem;

namespace {

// Evidence line for the test log (printf is redirected away from stdout in this harness).
void Ev(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stdout, "  D1-S0-F2 ");
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fputc('\n', stdout);
    fflush(stdout);
}

std::string Sha256OfFile(const fs::path& p)
{
    std::ifstream in(p.string().c_str(), std::ios::binary);
    std::vector<unsigned char> data((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
    unsigned char out[32];
    SHA256(data.empty() ? (const unsigned char*)"" : &data[0], data.size(), out);
    uint256 h;
    memcpy(&h, out, sizeof(out));
    return h.GetHex();
}

std::string ReadFileToString(const fs::path& p)
{
    std::ifstream in(p.string().c_str(), std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void WriteFile(const fs::path& p, const std::string& data)
{
    std::ofstream out(p.string().c_str(), std::ios::binary | std::ios::trunc);
    out.write(data.data(), data.size());
}

void Plant(const fs::path& dir, const char* name, const std::string& data)
{
    WriteFile(dir / name, data);
}

std::string OneLine(const std::string& s)
{
    std::string o;
    for (size_t i = 0; i < s.size(); ++i)
        o += (s[i] == '\n') ? '|' : s[i];
    while (!o.empty() && o[o.size() - 1] == '|') o.erase(o.size() - 1);
    return o;
}

std::string LineContaining(const std::string& text, const std::string& needle)
{
    const size_t at = text.find(needle);
    if (at == std::string::npos) return "<absent>";
    const size_t nl = text.rfind('\n', at);
    const size_t begin = (nl == std::string::npos) ? 0 : nl + 1;
    const size_t end = text.find('\n', at);
    return text.substr(begin, (end == std::string::npos ? text.size() : end) - begin);
}

fs::path MakeScratch(const char* tag)
{
    fs::path d = fs::temp_directory_path() /
                 fs::unique_path(std::string("d1s0f2-") + tag + "-%%%%-%%%%");
    fs::create_directories(d);
    return d;
}

struct ScratchDir {
    fs::path dir;
    explicit ScratchDir(const char* tag) : dir(MakeScratch(tag)) {}
    ~ScratchDir()
    {
        boost::system::error_code ec;
        fs::remove_all(dir, ec);
    }
};

struct ModeGuard {
    fs::path path;
    mode_t   mode;
    ModeGuard(const fs::path& p, mode_t m) : path(p), mode(m) {}
    ~ModeGuard() { ::chmod(path.string().c_str(), mode); }
};

bool MakeFifo(const fs::path& p) { return ::mkfifo(p.string().c_str(), 0644) == 0; }

// Which of the two index directories exists right now (the state a refused reset must not change).
struct IndexState {
    bool live;
    bool backup;
};

IndexState IndexStateOf(const fs::path& dir)
{
    boost::system::error_code ec;
    IndexState s;
    s.live = fs::exists(dir / "txleveldb", ec);
    s.backup = fs::exists(dir / "txleveldb_backup", ec);
    return s;
}

// --- harness (same discipline as the S0/S0-F1 suites) -----------------------------------------

// Drop any process-global txleveldb handle (it is owned by CTxDB and shared by all instances). A
// constructor that THREW leaves its handle open, so this must run before every simulated restart to
// make the next CTxDB construction really run the open/version/preflight path again.
void ReleaseGlobalTxdb()
{
    if (GetGlobalTxdbPtrForTest() != NULL) {
        CTxDB t("r");   // read-only: reuses the existing handle, never re-inits a directory
        t.Close();
    }
}

std::string RunCaptured(const std::function<void()>& fn)
{
    const bool savedToConsole = fPrintToConsole;
    const bool savedToDebugger = fPrintToDebugger;
    fPrintToConsole = true;
    fPrintToDebugger = false;

    fflush(stdout);
    const int savedFd = ::dup(1);
    const fs::path tmp = fs::temp_directory_path() / fs::unique_path("d1s0f2-cap-%%%%-%%%%");
    FILE* f = fopen(tmp.string().c_str(), "w");
    if (f && savedFd >= 0) ::dup2(fileno(f), 1);

    fn();

    fflush(stdout);
    if (f && savedFd >= 0) { ::dup2(savedFd, 1); fclose(f); }
    if (savedFd >= 0) ::close(savedFd);
    fPrintToConsole = savedToConsole;
    fPrintToDebugger = savedToDebugger;

    const std::string s = ReadFileToString(tmp);
    boost::system::error_code ec;
    fs::remove(tmp, ec);
    return s;
}

struct F2Case {
    fs::path dir;
    explicit F2Case(const char* tag) : dir(MakeScratch(tag))
    {
        ReleaseGlobalTxdb();
        SetDataDirForTest(dir);
    }
    ~F2Case()
    {
        FCloseBlockFile();
        ReleaseGlobalTxdb();
        ClearDataDirForTest();
        boost::system::error_code ec;
        fs::remove_all(dir, ec);
    }
};

// One simulated process startup: release the global handle first, then construct CTxDB for real.
bool TryInit(const char* mode, std::string& out, std::string& what)
{
    ReleaseGlobalTxdb();
    bool threw = false;
    out = RunCaptured([&]() {
        try {
            CTxDB db(mode);
            db.Close();
        } catch (const std::exception& e) {
            threw = true;
            what = e.what();
            fprintf(stdout, "threw: %s\n", e.what());
        }
    });
    return threw;
}

// Write an index whose persisted version and best-chain pointer are the sentinel values the tests
// follow across restarts.
void SeedStore(const fs::path& dir, int version, const uint256& sentinel)
{
    ReleaseGlobalTxdb();
    CTxDB db("cr+");
    BOOST_REQUIRE(db.WriteVersion(version));
    BOOST_REQUIRE(db.WriteHashBestChain(sentinel));
    db.Close();
}

// Read a store's persisted version (+ optionally its best-chain pointer) WITHOUT letting the CTxDB
// constructor touch it: point the (test-only) datadir at a scratch dir whose "txleveldb" entry is a
// symlink to the store, then open read-only.
bool ReadStoreViaSymlink(const fs::path& caseDir, const fs::path& storeDir, const char* tag,
                         int& versionOut, uint256* tipOut, bool* tipOkOut)
{
    ReleaseGlobalTxdb();
    const fs::path readback = caseDir / (std::string("readback-") + tag);
    fs::create_directories(readback);
    boost::system::error_code ec;
    fs::remove(readback / "txleveldb", ec);
    if (::symlink(storeDir.string().c_str(), (readback / "txleveldb").string().c_str()) != 0)
        return false;
    SetDataDirForTest(readback);
    bool vOk = false;
    {
        CTxDB db("r");                      // read-only: never resets anything
        vOk = db.ReadVersion(versionOut);
        if (tipOut && tipOkOut) *tipOkOut = db.ReadHashBestChain(*tipOut);
        db.Close();
    }
    SetDataDirForTest(caseDir);
    ReleaseGlobalTxdb();
    return vOk;
}

} // namespace

BOOST_AUTO_TEST_SUITE(d1_s0_f2_durable_preflight_tests)

// ---------------------------------------------------------------------------------------------
// TEST A — a refused reset is durable.
// The invalid archive entry is a fifo named blk0002.dat, so the archive cannot be accounted for.
// ---------------------------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(d1_s0_f2_a_refused_reset_is_durable)
{
    F2Case c("f2a");
    SeedStore(c.dir, DATABASE_VERSION - 1, uint256(0x5A));      // incompatible index + sentinel
    Plant(c.dir, "blk0001.dat", std::string(96, 'A') + "PAYLOAD");
    BOOST_REQUIRE(MakeFifo(c.dir / "blk0002.dat"));             // the invalid archive entry
    const std::string shaBefore = Sha256OfFile(c.dir / "blk0001.dat");

    const IndexState s0 = IndexStateOf(c.dir);
    Ev("A state before: live=%d backup=%d", (int)s0.live, (int)s0.backup);
    BOOST_REQUIRE(s0.live && !s0.backup);

    // --- startup 1 -----------------------------------------------------------------------------
    std::string out1, what1;
    const bool threw1 = TryInit("cr+", out1, what1);
    Ev("A startup1 threw=%d what=%s", (int)threw1, OneLine(what1).c_str());
    Ev("A startup1 archive_line=%s", LineContaining(out1, "cannot be accounted for").c_str());
    Ev("A startup1 preflight_line=%s", LineContaining(out1, "Archive preflight").c_str());

    BOOST_CHECK(threw1);
    BOOST_CHECK(what1.find("ARCHIVE-1 VIOLATION") != std::string::npos);
    BOOST_CHECK(what1.find("refusing to reset the index") != std::string::npos);
    BOOST_CHECK(what1.find("before the incompatible-index reset") != std::string::npos);
    BOOST_CHECK(what1.find("blk0002.dat") != std::string::npos);

    const IndexState s1 = IndexStateOf(c.dir);
    Ev("A state after startup1: live=%d backup=%d", (int)s1.live, (int)s1.backup);
    BOOST_CHECK(s1.live);             // the original index is still there
    BOOST_CHECK(!s1.backup);          // nothing was renamed away

    int v1 = -1; uint256 tip1(0); bool tipOk1 = false;
    const bool vOk1 = ReadStoreViaSymlink(c.dir, c.dir / "txleveldb", "f2a1", v1, &tip1, &tipOk1);
    Ev("A startup1 index: readable=%d version=%d tip_readable=%d tip=%s", (int)vOk1, v1,
       (int)tipOk1, tip1.GetHex().c_str());
    BOOST_CHECK(vOk1);
    BOOST_CHECK_EQUAL(v1, DATABASE_VERSION - 1);     // not recreated at the current version
    BOOST_CHECK(tipOk1 && tip1 == uint256(0x5A));    // sentinel survived
    BOOST_CHECK(out1.find("Index reset verified non-destructive") == std::string::npos);
    BOOST_CHECK_EQUAL(Sha256OfFile(c.dir / "blk0001.dat"), shaBefore);

    // --- startup 2 (same invalid archive) ------------------------------------------------------
    std::string out2, what2;
    const bool threw2 = TryInit("cr+", out2, what2);
    Ev("A startup2 threw=%d what=%s", (int)threw2, OneLine(what2).c_str());

    BOOST_CHECK(threw2);              // the refusal is durable: it does not evaporate after one try
    BOOST_CHECK(what2.find("ARCHIVE-1 VIOLATION") != std::string::npos);
    BOOST_CHECK(what2.find("before the incompatible-index reset") != std::string::npos);

    const IndexState s2 = IndexStateOf(c.dir);
    Ev("A state after startup2: live=%d backup=%d", (int)s2.live, (int)s2.backup);
    BOOST_CHECK(s2.live);
    BOOST_CHECK(!s2.backup);

    int v2 = -1; uint256 tip2(0); bool tipOk2 = false;
    const bool vOk2 = ReadStoreViaSymlink(c.dir, c.dir / "txleveldb", "f2a2", v2, &tip2, &tipOk2);
    Ev("A startup2 index: readable=%d version=%d tip_readable=%d tip=%s", (int)vOk2, v2,
       (int)tipOk2, tip2.GetHex().c_str());
    BOOST_CHECK(vOk2);
    BOOST_CHECK_EQUAL(v2, DATABASE_VERSION - 1);
    BOOST_CHECK(tipOk2 && tip2 == uint256(0x5A));    // unchanged by both attempts
    BOOST_CHECK_EQUAL(Sha256OfFile(c.dir / "blk0001.dat"), shaBefore);
    BOOST_CHECK(out2.find("Index reset verified non-destructive") == std::string::npos);

    // the archive entry is still the odd one out: nothing "cleaned it up"
    BOOST_CHECK(!fs::is_regular_file(c.dir / "blk0002.dat"));
}

// ---------------------------------------------------------------------------------------------
// TEST B — recovery once the archive is corrected: the legitimate reset then succeeds and the old
// index is preserved as the backup.
// ---------------------------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(d1_s0_f2_b_recovery_after_correcting_the_archive)
{
    F2Case c("f2b");
    SeedStore(c.dir, DATABASE_VERSION - 1, uint256(0x77));
    Plant(c.dir, "blk0001.dat", std::string(120, 'B') + "LOW");
    Plant(c.dir, "blk10000.dat", std::string(121, 'C') + "HIGH");
    BOOST_REQUIRE(MakeFifo(c.dir / "blk0002.dat"));
    const std::string sha1 = Sha256OfFile(c.dir / "blk0001.dat");
    const std::string sha2 = Sha256OfFile(c.dir / "blk10000.dat");

    // startup 1: refused, index left alone
    std::string out1, what1;
    const bool threw1 = TryInit("cr+", out1, what1);
    Ev("B startup1 threw=%d what=%s", (int)threw1, OneLine(what1).c_str());
    BOOST_CHECK(threw1);
    BOOST_CHECK(IndexStateOf(c.dir).live);
    BOOST_CHECK(!IndexStateOf(c.dir).backup);

    // correct the archive: remove the offending fifo
    BOOST_REQUIRE_EQUAL(::unlink((c.dir / "blk0002.dat").string().c_str()), 0);

    // startup 2: the legitimate reset must now succeed
    std::string out2, what2;
    const bool threw2 = TryInit("cr+", out2, what2);
    Ev("B startup2 threw=%d what=%s", (int)threw2, OneLine(what2).c_str());
    Ev("B startup2 preflight=%s", LineContaining(out2, "Archive preflight").c_str());
    Ev("B startup2 reset=%s", LineContaining(out2, "Required index version is ").c_str());
    Ev("B startup2 guard=%s", LineContaining(out2, "verified non-destructive").c_str());

    BOOST_CHECK(!threw2);
    BOOST_CHECK(out2.find("Archive preflight (before the incompatible-index reset)") != std::string::npos);
    BOOST_CHECK(out2.find("Required index version is ") != std::string::npos);
    BOOST_CHECK(out2.find("Index reset verified non-destructive: 2 block-archive file(s) intact") != std::string::npos);

    // the old index is preserved as the backup, with its version and sentinel intact
    const IndexState s2 = IndexStateOf(c.dir);
    Ev("B state after reset: live=%d backup=%d", (int)s2.live, (int)s2.backup);
    BOOST_CHECK(s2.live);
    BOOST_CHECK(s2.backup);

    int vNew = -1; uint256 tipNew(0); bool tipOkNew = false;
    BOOST_CHECK(ReadStoreViaSymlink(c.dir, c.dir / "txleveldb", "f2bnew", vNew, &tipNew, &tipOkNew));
    int vOld = -1; uint256 tipOld(0); bool tipOkOld = false;
    const bool vOkOld = ReadStoreViaSymlink(c.dir, c.dir / "txleveldb_backup", "f2bold", vOld, &tipOld, &tipOkOld);
    Ev("B new index: version=%d | backup: readable=%d version=%d tip_readable=%d tip=%s",
       vNew, (int)vOkOld, vOld, (int)tipOkOld, tipOld.GetHex().c_str());

    BOOST_CHECK_EQUAL(vNew, DATABASE_VERSION);       // the new index has the expected version
    BOOST_CHECK(vOkOld);
    BOOST_CHECK_EQUAL(vOld, DATABASE_VERSION - 1);   // the old index content survived as the backup
    BOOST_CHECK(tipOkOld && tipOld == uint256(0x77));

    // archive payloads byte-identical
    BOOST_CHECK_EQUAL(Sha256OfFile(c.dir / "blk0001.dat"), sha1);
    BOOST_CHECK_EQUAL(Sha256OfFile(c.dir / "blk10000.dat"), sha2);
}

// ---------------------------------------------------------------------------------------------
// TEST C — the failed-backup-rename path stays non-destructive, with the preflight now in front of
// it (S0's v4 covered the rename failure; this pins it on the F2 binary with the preflight engaged).
// ---------------------------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(d1_s0_f2_c_failed_backup_rename_stays_non_destructive)
{
    if (::geteuid() == 0) {
        Ev("C SKIPPED: running as root, chmod 0555 cannot deny the rename");
        return;
    }
    F2Case c("f2c");
    SeedStore(c.dir, DATABASE_VERSION - 1, uint256(0x33));
    Plant(c.dir, "blk0001.dat", std::string(64, 'D') + "PAYLOAD");
    const std::string shaBefore = Sha256OfFile(c.dir / "blk0001.dat");

    BOOST_REQUIRE_EQUAL(::chmod(c.dir.string().c_str(), 0555), 0);   // writable store, read-only datadir
    ModeGuard restore(c.dir, 0755);
    {
        std::string out, what;
        const bool threw = TryInit("cr+", out, what);
        Ev("C threw=%d what=%s", (int)threw, OneLine(what).c_str());
        Ev("C preflight=%s", LineContaining(out, "Archive preflight").c_str());
        Ev("C backup=%s", LineContaining(out, "failed to backup database").c_str());

        BOOST_CHECK(!threw);   // this branch fails soft by design (pre-existing S0 semantics)
        BOOST_CHECK(out.find("Archive preflight (before the incompatible-index reset)") != std::string::npos);
        BOOST_CHECK(out.find("failed to backup database") != std::string::npos);
        BOOST_CHECK(out.find("Database reset aborted") != std::string::npos);
        BOOST_CHECK(out.find("Continuing with old database version") != std::string::npos);
        BOOST_CHECK(out.find("Required index version is ") == std::string::npos);   // no reset
        BOOST_CHECK(out.find("Index reset: index state only") == std::string::npos);
    }
    ::chmod(c.dir.string().c_str(), 0755);   // restore before inspecting/cleanup

    const IndexState s = IndexStateOf(c.dir);
    Ev("C state after failed backup: live=%d backup=%d", (int)s.live, (int)s.backup);
    BOOST_CHECK(s.live);
    BOOST_CHECK(!s.backup);

    int v = -1; uint256 tip(0); bool tipOk = false;
    BOOST_CHECK(ReadStoreViaSymlink(c.dir, c.dir / "txleveldb", "f2c", v, &tip, &tipOk));
    Ev("C index: version=%d tip=%s", v, tip.GetHex().c_str());
    BOOST_CHECK_EQUAL(v, DATABASE_VERSION - 1);      // the old index is still the live one
    BOOST_CHECK(tipOk && tip == uint256(0x33));
    BOOST_CHECK_EQUAL(Sha256OfFile(c.dir / "blk0001.dat"), shaBefore);
}

// ---------------------------------------------------------------------------------------------
// TEST D — the preflight is scoped to the reset: read-only opens and already-compatible indexes are
// untouched by it.
// ---------------------------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(d1_s0_f2_d_readonly_and_compatible_paths_unchanged)
{
    // (i) read-only handle on an incompatible index: no reset, no preflight, no refusal
    {
        F2Case c("f2d1");
        SeedStore(c.dir, DATABASE_VERSION - 1, uint256(0x11));
        Plant(c.dir, "blk0001.dat", std::string(48, 'E') + "RO");
        const std::string shaBefore = Sha256OfFile(c.dir / "blk0001.dat");

        std::string out, what;
        const bool threw = TryInit("r", out, what);
        Ev("D read-only: threw=%d what=%s", (int)threw, OneLine(what).c_str());
        BOOST_CHECK(!threw);
        BOOST_CHECK(out.find("Archive preflight") == std::string::npos);      // preflight never runs
        BOOST_CHECK(out.find("Index reset: index state only") == std::string::npos);
        BOOST_CHECK(out.find("Required index version is ") == std::string::npos);

        const IndexState s = IndexStateOf(c.dir);
        Ev("D read-only state: live=%d backup=%d", (int)s.live, (int)s.backup);
        BOOST_CHECK(s.live);
        BOOST_CHECK(!s.backup);

        int v = -1; uint256 tip(0); bool tipOk = false;
        BOOST_CHECK(ReadStoreViaSymlink(c.dir, c.dir / "txleveldb", "f2d1", v, &tip, &tipOk));
        Ev("D read-only index: version=%d tip=%s", v, tip.GetHex().c_str());
        BOOST_CHECK_EQUAL(v, DATABASE_VERSION - 1);
        BOOST_CHECK(tipOk && tip == uint256(0x11));
        BOOST_CHECK_EQUAL(Sha256OfFile(c.dir / "blk0001.dat"), shaBefore);
    }

    // (ii) an already-compatible index: normal open, no preflight, no reset
    {
        F2Case c("f2d2");
        SeedStore(c.dir, DATABASE_VERSION, uint256(0x22));
        Plant(c.dir, "blk0001.dat", std::string(48, 'F') + "OK");
        const std::string shaBefore = Sha256OfFile(c.dir / "blk0001.dat");

        std::string out, what;
        const bool threw = TryInit("cr+", out, what);
        Ev("D compatible: threw=%d what=%s", (int)threw, OneLine(what).c_str());
        BOOST_CHECK(!threw);
        BOOST_CHECK(out.find("Archive preflight") == std::string::npos);
        BOOST_CHECK(out.find("Index reset: index state only") == std::string::npos);
        BOOST_CHECK(out.find("creating backup") == std::string::npos);

        const IndexState s = IndexStateOf(c.dir);
        Ev("D compatible state: live=%d backup=%d", (int)s.live, (int)s.backup);
        BOOST_CHECK(s.live);
        BOOST_CHECK(!s.backup);

        int v = -1; uint256 tip(0); bool tipOk = false;
        BOOST_CHECK(ReadStoreViaSymlink(c.dir, c.dir / "txleveldb", "f2d2", v, &tip, &tipOk));
        Ev("D compatible index: version=%d tip=%s", v, tip.GetHex().c_str());
        BOOST_CHECK_EQUAL(v, DATABASE_VERSION);
        BOOST_CHECK(tipOk && tip == uint256(0x22));
        BOOST_CHECK_EQUAL(Sha256OfFile(c.dir / "blk0001.dat"), shaBefore);
    }
}

// ---------------------------------------------------------------------------------------------
// TEST E — one decision, one wording: the printed line and the thrown diagnostic carry the same
// scan detail and the same phase, so the two call sites cannot report different policies.
// ---------------------------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(d1_s0_f2_e_preflight_diagnostics_are_shared)
{
    F2Case c("f2e");
    SeedStore(c.dir, DATABASE_VERSION - 1, uint256(0x44));
    Plant(c.dir, "blk0001.dat", std::string(32, 'G') + "P");
    fs::create_directories(c.dir / "blk0009.dat");     // archive-named directory: ENTRY_BAD
    const std::string shaBefore = Sha256OfFile(c.dir / "blk0001.dat");

    std::string out, what;
    const bool threw = TryInit("cr+", out, what);
    Ev("E threw=%d what=%s", (int)threw, OneLine(what).c_str());
    Ev("E printed=%s", LineContaining(out, "cannot be accounted for").c_str());

    BOOST_CHECK(threw);
    // the same detail text is in the printed line and in the exception
    BOOST_CHECK(what.find("blk0009.dat") != std::string::npos);
    BOOST_CHECK(what.find("not a regular file") != std::string::npos);
    BOOST_CHECK(out.find("blk0009.dat") != std::string::npos);
    BOOST_CHECK(out.find("not a regular file") != std::string::npos);
    // the phase names the check that refused, and both diagnostics state the same refusal reason
    BOOST_CHECK(what.find("(before the incompatible-index reset)") != std::string::npos);
    BOOST_CHECK(out.find("cannot be accounted for") != std::string::npos);
    BOOST_CHECK(what.find("cannot be accounted for") != std::string::npos);
    // the internal reset-path check uses the same policy wording (the phrase is shared by construction)
    BOOST_CHECK(what.find("refusing to reset the index") != std::string::npos);

    BOOST_CHECK(IndexStateOf(c.dir).live);
    BOOST_CHECK(!IndexStateOf(c.dir).backup);
    BOOST_CHECK_EQUAL(Sha256OfFile(c.dir / "blk0001.dat"), shaBefore);
}

BOOST_AUTO_TEST_SUITE_END()
