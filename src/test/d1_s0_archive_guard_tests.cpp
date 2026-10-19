// Copyright (c) 2026 The Innova developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// D1-S0 — ARCHIVE-1 regression tests (Stage 3B-D1-S0).
//
// Invariant under test: a database/index version reset must never delete, truncate or otherwise modify
// the block archive (blk%04u.dat). The archive is permanent data; only the INDEX (txleveldb) may be
// reset/rebuilt. Every case runs against an isolated scratch datadir with synthetic archive contents:
// no real blockchain data and never the shared test datadir (GetDataDir() is redirected per case by the
// test-only seam added to util.cpp for S0).
//
// Harness notes (learned the hard way, both are deliberate below):
//   * `printf` is `#define printf OutputDebugStringF` (util.h:272), and TestingSetup sets
//     fPrintToDebugger=true, so printf output is DISCARDED in the test process. Captured runs therefore
//     flip fPrintToConsole on for the duration of the operation (RunCaptured), and this file's own
//     evidence lines use fprintf(stdout,...) so they reach the test log.
//   * The global txleveldb handle is process-wide and shared by all CTxDB instances: it must be closed
//     before a case so that CTxDB construction really runs the open/version path (instead of reusing a
//     handle bound to another directory).
//
// Case map (report innova-v2-chainstate-stage3b-d1-s0-20261016.md):
//   v1  S0-C(1)(6)(7)(10) old persisted version + writable handle -> reset must preserve the archive
//   v2  S0-C(2)(10)       current version, normal open       -> nothing reset, archive untouched
//   v3  S0-C(3)(10)       read-only handle + mismatch         -> no reset, archive untouched
//   v4  S0-C(4)(9)(10)    backup prerequisite fails           -> non-destructive loud failure
//   v5  S0-C(5)(10)       repeated initialization/restarts    -> archive preserved every time
//   v6  S0-C(6)(10)       multiple blk files incl. a gap      -> nothing removed (not even a prefix)
//   v7  S0-C(7)(10)       incompatible index metadata         -> index preserved as backup, archive intact
//   v8  S0-C(8)           append + rollover                   -> still functional, archive never truncated
//   v11 S0-B guard        a missing archive entry             -> fail-closed, loud, non-destructive

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
#include "uint256.h"
#include "util.h"
#include "version.h"

extern void* GetGlobalTxdbPtrForTest(); // test-only accessor defined in txdb-leveldb.cpp
extern bool fPrintToDebugger;           // util.cpp (set true by TestingSetup)

namespace fs = boost::filesystem;

namespace {

struct S0ArchiveEntry {
    std::string name;
    long long   size;
    std::string sha256;
};

// Evidence line for the test log (printf is redirected away from stdout in this harness).
void Ev(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stdout, "  D1-S0 ");
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

// Bounded read: never load a whole (potentially 2 GB, sparse) block file into RAM.
std::string ReadFilePrefix(const fs::path& p, size_t n)
{
    std::ifstream in(p.string().c_str(), std::ios::binary);
    std::string s(n, '\0');
    in.read(&s[0], (std::streamsize)n);
    s.resize((size_t)in.gcount());
    return s;
}

void WriteFile(const fs::path& p, const std::string& data)
{
    std::ofstream out(p.string().c_str(), std::ios::binary | std::ios::trunc);
    out.write(data.data(), data.size());
}

// name + size + sha256 of every blk*.dat in `dir`, sorted by name
std::vector<S0ArchiveEntry> SnapshotArchive(const fs::path& dir)
{
    std::vector<S0ArchiveEntry> out;
    for (fs::directory_iterator it(dir); it != fs::directory_iterator(); ++it) {
        const std::string name = it->path().filename().string();
        if (name.size() == 11 && name.compare(0, 3, "blk") == 0 && name.compare(7, 4, ".dat") == 0) {
            S0ArchiveEntry e;
            e.name = name;
            if (fs::is_regular_file(it->path())) {
                e.size = (long long)fs::file_size(it->path());
                e.sha256 = Sha256OfFile(it->path());
            } else {
                e.size = -1;
                e.sha256 = "<non-regular>";
            }
            out.push_back(e);
        }
    }
    std::sort(out.begin(), out.end(),
              [](const S0ArchiveEntry& a, const S0ArchiveEntry& b) { return a.name < b.name; });
    return out;
}

std::string SnapshotToString(const std::vector<S0ArchiveEntry>& v)
{
    std::string s;
    for (size_t i = 0; i < v.size(); ++i)
        s += v[i].name + " size=" + std::to_string(v[i].size) + " sha256=" + v[i].sha256 + "; ";
    return s;
}

std::string OneLine(const std::string& s)
{
    std::string o;
    for (size_t i = 0; i < s.size(); ++i)
        o += (s[i] == '\n') ? '|' : s[i];
    while (!o.empty() && o[o.size() - 1] == '|') o.erase(o.size() - 1);
    return o;
}

fs::path MakeScratch(const char* tag)
{
    fs::path d = fs::temp_directory_path() /
                 fs::unique_path(std::string("d1s0-") + tag + "-%%%%-%%%%");
    fs::create_directories(d);
    return d;
}

// Drop any process-global txleveldb handle (owned by CTxDB, shared by all instances) so the next
// CTxDB construction really runs the open/version path on THIS case's datadir instead of reusing it.
void ReleaseGlobalTxdb()
{
    if (GetGlobalTxdbPtrForTest() != NULL) {
        CTxDB t("r");   // read-only: reuses the existing handle, never re-inits a directory
        t.Close();
    }
}

// Run `fn` with the production diagnostics captured. The production code reports its decisions through
// printf -> OutputDebugStringF, which TestingSetup suppresses (fPrintToDebugger=true), so the console
// route is enabled for the duration and fd 1 is captured to a scratch file.
std::string RunCaptured(const std::function<void()>& fn)
{
    const bool savedToConsole = fPrintToConsole;
    const bool savedToDebugger = fPrintToDebugger;
    fPrintToConsole = true;
    fPrintToDebugger = false;

    fflush(stdout);
    const int savedFd = ::dup(1);
    const fs::path tmp = fs::temp_directory_path() / fs::unique_path("d1s0-cap-%%%%-%%%%");
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

// Per-case isolation: redirect GetDataDir() at a private scratch dir, and restore the process-wide
// state (handle, cached datadir) even when an assertion throws.
struct S0Case {
    fs::path dir;
    explicit S0Case(const char* tag) : dir(MakeScratch(tag))
    {
        ReleaseGlobalTxdb();
        SetDataDirForTest(dir);
    }
    ~S0Case()
    {
        FCloseBlockFile();          // never leave a cached append handle pointing into a deleted dir
        ReleaseGlobalTxdb();
        ClearDataDirForTest();
        boost::system::error_code ec;
        fs::remove_all(dir, ec);
    }
};

void PlantContiguousArchive(const fs::path& dir, int n)
{
    for (int i = 1; i <= n; ++i) {
        char name[32];
        snprintf(name, sizeof(name), "blk%04d.dat", i);
        WriteFile(dir / name, std::string((size_t)(1000 + 37 * i), (char)('A' + i)) + "SYNTHETIC");
    }
}

// Seed a store holding an OLD persisted version, i.e. what an upgrade from an older build leaves behind.
void SeedOldVersionStore(const fs::path& dir)
{
    CTxDB db("cr+");
    BOOST_REQUIRE(db.WriteVersion(DATABASE_VERSION - 1));
    db.Close();
}

// Read a store's persisted version (and optionally its best-chain pointer) through the PRODUCTION read
// path without letting the CTxDB constructor touch it: point the (test-only) datadir at a scratch dir
// whose "txleveldb" entry is a symlink to the store directory, then open read-only.
bool ReadStoreViaSymlink(const fs::path& caseDir, const fs::path& storeDir, const char* tag,
                         int& versionOut, uint256* tipOut, bool* tipOkOut)
{
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
    return vOk;
}

bool ArchiveIdentical(const fs::path& dir, const std::vector<S0ArchiveEntry>& before,
                      std::vector<S0ArchiveEntry>* afterOut = NULL)
{
    const std::vector<S0ArchiveEntry> after = SnapshotArchive(dir);
    if (afterOut) *afterOut = after;
    if (after.size() != before.size()) return false;
    for (size_t i = 0; i < before.size(); ++i) {
        if (after[i].name != before[i].name) return false;
        if (after[i].size != before[i].size) return false;
        if (after[i].sha256 != before[i].sha256) return false;
    }
    return true;
}

} // namespace

BOOST_AUTO_TEST_SUITE(d1_s0_archive_guard_tests)

// S0-C(1)(6)(7)(10): the R1-1 path — old persisted version, writable handle, successful backup.
BOOST_AUTO_TEST_CASE(d1_s0_v1_version_mismatch_preserves_archive)
{
    S0Case c("v1");
    SeedOldVersionStore(c.dir);
    PlantContiguousArchive(c.dir, 3);
    const std::vector<S0ArchiveEntry> before = SnapshotArchive(c.dir);
    BOOST_REQUIRE_EQUAL(before.size(), 3u);
    Ev("v1 archive before: %s", SnapshotToString(before).c_str());

    const std::string out = RunCaptured([]() { CTxDB db("cr+"); db.Close(); });
    Ev("v1 reset output: %s", OneLine(out).c_str());

    std::vector<S0ArchiveEntry> after;
    const bool identical = ArchiveIdentical(c.dir, before, &after);
    Ev("v1 archive after:  %s", SnapshotToString(after).c_str());
    BOOST_CHECK_EQUAL(after.size(), before.size());
    BOOST_CHECK(identical);   // same names, sizes and sha256 => no bytes of the archive changed

    // the reset must have happened (the mismatch path was really exercised) and been index-only
    BOOST_CHECK(fs::exists(c.dir / "txleveldb_backup"));
    BOOST_CHECK(fs::exists(c.dir / "txleveldb"));
    BOOST_CHECK(out.find("database version") != std::string::npos);

    // S0-B guard executed and reported
    BOOST_CHECK(out.find("Index reset: index state only") != std::string::npos);
    BOOST_CHECK(out.find("Index reset verified non-destructive: 3 block-archive file(s) intact") !=
                std::string::npos);

    // the OLD index was preserved as a backup (index metadata reset != archive deletion)
    int backupVersion = -1;
    const bool backupReadable =
        ReadStoreViaSymlink(c.dir, c.dir / "txleveldb_backup", "v1bak", backupVersion, NULL, NULL);
    Ev("v1 preserved-index version: readable=%d version=%d (old index kept as backup)",
       (int)backupReadable, backupVersion);
    BOOST_CHECK(backupReadable);
    BOOST_CHECK_EQUAL(backupVersion, DATABASE_VERSION - 1);

    // the fresh index carries the current version
    int liveVersion = -1;
    {
        CTxDB db("r");
        BOOST_REQUIRE(db.ReadVersion(liveVersion));
        db.Close();
    }
    Ev("v1 live index version=%d expected=%d", liveVersion, DATABASE_VERSION);
    BOOST_CHECK_EQUAL(liveVersion, DATABASE_VERSION);
    BOOST_CHECK(ArchiveIdentical(c.dir, before));   // still intact after every operation above
}

// S0-C(2)(10): a current-version store opens normally: no reset, no archive touch, no backup.
BOOST_AUTO_TEST_CASE(d1_s0_v2_current_version_normal_open_preserves_archive)
{
    S0Case c("v2");
    {
        CTxDB db("cr+");            // fresh store: created at DATABASE_VERSION
        int v = -1;
        BOOST_REQUIRE(db.ReadVersion(v));
        BOOST_REQUIRE_EQUAL(v, DATABASE_VERSION);
        db.Close();
    }
    PlantContiguousArchive(c.dir, 2);
    const std::vector<S0ArchiveEntry> before = SnapshotArchive(c.dir);

    const std::string out1 = RunCaptured([]() { CTxDB db("cr+"); db.Close(); });
    const std::string out2 = RunCaptured([]() { CTxDB db("r"); db.Close(); });

    std::vector<S0ArchiveEntry> after;
    const bool identical = ArchiveIdentical(c.dir, before, &after);
    Ev("v2 archive: before=%s", SnapshotToString(before).c_str());
    Ev("v2 archive: after =%s", SnapshotToString(after).c_str());
    BOOST_CHECK(identical);
    BOOST_CHECK(!fs::exists(c.dir / "txleveldb_backup"));   // no reset happened at all
    BOOST_CHECK(out1.find("Index reset") == std::string::npos);   // guard never runs off the reset path
    BOOST_CHECK(out2.find("Index reset") == std::string::npos);
}

// S0-C(3)(10): a READ-ONLY handle with a version mismatch must not reset anything.
BOOST_AUTO_TEST_CASE(d1_s0_v3_readonly_handle_mismatch_preserves_archive)
{
    S0Case c("v3");
    SeedOldVersionStore(c.dir);
    PlantContiguousArchive(c.dir, 2);
    const std::vector<S0ArchiveEntry> before = SnapshotArchive(c.dir);

    const std::string out = RunCaptured([]() { CTxDB db("r"); db.Close(); });
    Ev("v3 read-only open output: %s", OneLine(out).c_str());

    std::vector<S0ArchiveEntry> after;
    const bool identical = ArchiveIdentical(c.dir, before, &after);
    Ev("v3 archive: before=%s", SnapshotToString(before).c_str());
    Ev("v3 archive: after =%s", SnapshotToString(after).c_str());
    BOOST_CHECK(identical);
    BOOST_CHECK(!fs::exists(c.dir / "txleveldb_backup"));
    BOOST_CHECK(out.find("Index reset") == std::string::npos);

    int v = -1;
    {
        CTxDB db("r");
        BOOST_REQUIRE(db.ReadVersion(v));
        db.Close();
    }
    BOOST_CHECK_EQUAL(v, DATABASE_VERSION - 1);   // old version preserved: nothing was reset
}

// S0-C(4)(9)(10): the backup prerequisite fails -> loud, non-destructive failure (no reset, no deletion).
BOOST_AUTO_TEST_CASE(d1_s0_v4_failed_backup_is_non_destructive)
{
    if (::geteuid() == 0) {
        BOOST_TEST_MESSAGE("d1_s0_v4 skipped: running as root, a permission-based backup failure cannot "
                           "be simulated");
        return;
    }
    S0Case c("v4");
    SeedOldVersionStore(c.dir);
    PlantContiguousArchive(c.dir, 3);
    const std::vector<S0ArchiveEntry> before = SnapshotArchive(c.dir);

    BOOST_REQUIRE_EQUAL(::chmod(c.dir.string().c_str(), 0555), 0);   // datadir not writable
    const std::string out = RunCaptured([]() {
        try {
            CTxDB db("cr+");
            db.Close();
        } catch (const std::exception& e) {
            fprintf(stdout, "CTxDB construction threw: %s\n", e.what());
        }
    });
    BOOST_REQUIRE_EQUAL(::chmod(c.dir.string().c_str(), 0755), 0);
    Ev("v4 failure output: %s", OneLine(out).c_str());

    std::vector<S0ArchiveEntry> after;
    const bool identical = ArchiveIdentical(c.dir, before, &after);
    Ev("v4 archive: before=%s", SnapshotToString(before).c_str());
    Ev("v4 archive: after =%s", SnapshotToString(after).c_str());
    BOOST_CHECK(identical);
    BOOST_CHECK(out.find("failed to backup database") != std::string::npos);   // clear diagnostic
    BOOST_CHECK(out.find("Database reset aborted") != std::string::npos);
    BOOST_CHECK(!fs::exists(c.dir / "txleveldb_backup"));
    BOOST_CHECK(fs::exists(c.dir / "txleveldb"));                              // nothing was reset

    int v = -1;
    {
        CTxDB db("r");
        BOOST_REQUIRE(db.ReadVersion(v));
        db.Close();
    }
    BOOST_CHECK_EQUAL(v, DATABASE_VERSION - 1);   // the old index is still the live one
}

// S0-C(5)(10): repeated initialization/restart attempts never remove archive data.
BOOST_AUTO_TEST_CASE(d1_s0_v5_repeated_initialization_preserves_archive)
{
    S0Case c("v5");
    SeedOldVersionStore(c.dir);
    PlantContiguousArchive(c.dir, 4);
    const std::vector<S0ArchiveEntry> before = SnapshotArchive(c.dir);
    BOOST_REQUIRE_EQUAL(before.size(), 4u);

    for (int i = 0; i < 3; ++i) {
        if (i > 0) SeedOldVersionStore(c.dir);        // force the mismatch path again on restart
        const std::string out = RunCaptured([]() { CTxDB db("cr+"); db.Close(); });
        std::vector<S0ArchiveEntry> after;
        const bool identical = ArchiveIdentical(c.dir, before, &after);
        const bool guardRan = out.find("verified non-destructive") != std::string::npos;
        Ev("v5 restart %d: preserved=%d entries=%u guard_ran=%d", i + 1, (int)identical,
           (unsigned)after.size(), (int)guardRan);
        BOOST_CHECK(identical);
        BOOST_CHECK_EQUAL(after.size(), before.size());
        BOOST_CHECK(guardRan);
    }
}

// S0-C(6)(10): several blk files with distinct contents, including a numbering GAP. The removed legacy
// loop deleted only a contiguous prefix (blk0001..gap) — nothing at all may be removed now.
BOOST_AUTO_TEST_CASE(d1_s0_v6_multiple_files_with_gap_preserved)
{
    S0Case c("v6");
    SeedOldVersionStore(c.dir);
    WriteFile(c.dir / "blk0001.dat", std::string(4096, 'A') + "ONE");
    WriteFile(c.dir / "blk0002.dat", std::string(1234, 'B') + "TWO");
    WriteFile(c.dir / "blk0005.dat", std::string(17, 'C') + "FIVE");      // gap: no blk0003/0004
    WriteFile(c.dir / "blk0010.dat", std::string(9, 'D') + "TEN");
    const std::vector<S0ArchiveEntry> before = SnapshotArchive(c.dir);
    BOOST_REQUIRE_EQUAL(before.size(), 4u);
    Ev("v6 archive before: %s", SnapshotToString(before).c_str());

    const std::string out = RunCaptured([]() { CTxDB db("cr+"); db.Close(); });
    std::vector<S0ArchiveEntry> after;
    const bool identical = ArchiveIdentical(c.dir, before, &after);
    Ev("v6 archive after:  %s", SnapshotToString(after).c_str());
    Ev("v6 guard line present=%d",
       (int)(out.find("4 block-archive file(s) intact") != std::string::npos));

    BOOST_CHECK_EQUAL(after.size(), 4u);
    BOOST_CHECK(identical);
    BOOST_CHECK(fs::exists(c.dir / "blk0005.dat"));   // beyond the gap: not even reachable by the old loop
    BOOST_CHECK(fs::exists(c.dir / "blk0010.dat"));
    BOOST_CHECK(out.find("4 block-archive file(s) intact") != std::string::npos);
}

// S0-C(7)(10): an INCOMPATIBLE index (old version + old tip pointer) is reset, but preserved as a
// backup — index metadata removal is a different action from archive deletion.
BOOST_AUTO_TEST_CASE(d1_s0_v7_index_metadata_reset_is_not_archive_deletion)
{
    S0Case c("v7");
    {
        CTxDB db("cr+");
        BOOST_REQUIRE(db.WriteVersion(DATABASE_VERSION - 1));
        BOOST_REQUIRE(db.WriteHashBestChain(uint256(0x11)));
        db.Close();
    }
    PlantContiguousArchive(c.dir, 2);
    const std::vector<S0ArchiveEntry> before = SnapshotArchive(c.dir);

    const std::string out = RunCaptured([]() { CTxDB db("cr+"); db.Close(); });
    std::vector<S0ArchiveEntry> after;
    const bool identical = ArchiveIdentical(c.dir, before, &after);
    Ev("v7 archive: before=%s", SnapshotToString(before).c_str());
    Ev("v7 archive: after =%s", SnapshotToString(after).c_str());
    BOOST_CHECK(identical);   // archive untouched

    // the incompatible index metadata is still readable in the preserved backup
    int backupVersion = -1;
    uint256 backupTip(0);
    bool tipOk = false;
    const bool vOk = ReadStoreViaSymlink(c.dir, c.dir / "txleveldb_backup", "v7bak", backupVersion,
                                         &backupTip, &tipOk);
    Ev("v7 backup: version_readable=%d version=%d tip_readable=%d tip=%s", (int)vOk, backupVersion,
       (int)tipOk, backupTip.GetHex().c_str());
    BOOST_CHECK(vOk && backupVersion == DATABASE_VERSION - 1);
    BOOST_CHECK(tipOk && backupTip == uint256(0x11));
    BOOST_CHECK(out.find("Index reset: index state only") != std::string::npos);
}

// S0-C(8): normal append + rollover must keep working (the guard must not block the archive writer),
// and rollover must not truncate or rewrite the file it rolls away from.
BOOST_AUTO_TEST_CASE(d1_s0_v8_append_and_rollover_still_functional)
{
    S0Case c("v8");
    FCloseBlockFile();

    const std::string seed = "D1S0-APPEND-SEED";
    unsigned int nFile1 = 0;
    FILE* f1 = AppendBlockFile(nFile1);
    BOOST_REQUIRE(f1 != NULL);
    BOOST_REQUIRE_EQUAL(fwrite(seed.data(), 1, seed.size(), f1), seed.size());
    fflush(f1);
    FCloseBlockFile();

    const fs::path first = c.dir / strprintf("blk%04u.dat", nFile1);
    BOOST_REQUIRE(fs::exists(first));
    const std::string firstBytes = ReadFileToString(first);
    const long long firstSize = (long long)fs::file_size(first);
    Ev("v8 append: file=%s size=%lld prefix_ok=%d", first.filename().string().c_str(), firstSize,
       (int)(ReadFilePrefix(first, seed.size()) == seed));
    BOOST_CHECK(ReadFilePrefix(first, seed.size()) == seed);           // append-only
    BOOST_CHECK((long long)firstBytes.size() >= (long long)seed.size());

    // Roll the writer over: grow the current file past the 2 GB rollover threshold (sparse: no real
    // blocks are allocated) and call the writer again. It must move to the NEXT file and must not
    // truncate, rewrite or delete the file it left behind (the 2 GB file is compared by size + prefix).
    const long long threshold = (long long)(0x7F000000 - MAX_SIZE);
    const char* tail = "D1S0-ROLLOVER-TAIL";
    FILE* ftr = fopen(first.string().c_str(), "r+b");
    BOOST_REQUIRE(ftr != NULL);
    BOOST_REQUIRE_EQUAL(fseek(ftr, 0, SEEK_END), 0);                   // append the tail, do not clobber
    BOOST_REQUIRE_EQUAL(fwrite(tail, 1, strlen(tail), ftr), strlen(tail));
    BOOST_REQUIRE_EQUAL(::ftruncate(fileno(ftr), (off_t)(threshold + 1)), 0);
    fclose(ftr);
    const long long bigSize = (long long)fs::file_size(first);
    Ev("v8 rollover setup: %s size=%lld threshold=%lld prefix_ok=%d",
       first.filename().string().c_str(), bigSize, threshold,
       (int)(ReadFilePrefix(first, seed.size()) == seed));

    FCloseBlockFile();
    unsigned int nFile2 = 0;
    FILE* f2 = AppendBlockFile(nFile2);
    BOOST_REQUIRE(f2 != NULL);
    const char* rolled = "D1S0-ROLLED-FILE";
    BOOST_REQUIRE_EQUAL(fwrite(rolled, 1, strlen(rolled), f2), strlen(rolled));
    fflush(f2);
    FCloseBlockFile();
    const fs::path second = c.dir / strprintf("blk%04u.dat", nFile2);
    Ev("v8 rollover: %u -> %u next_file=%s size=%lld", nFile1, nFile2,
       second.filename().string().c_str(), (long long)fs::file_size(second));

    BOOST_CHECK_EQUAL(nFile2, nFile1 + 1);                              // rolled to the next file
    BOOST_CHECK_EQUAL((long long)fs::file_size(first), bigSize);        // never truncated
    BOOST_CHECK(ReadFilePrefix(first, seed.size()) == seed);            // original bytes intact
    BOOST_CHECK(fs::exists(second));
    BOOST_CHECK_GE((long long)fs::file_size(second), (long long)strlen(rolled));
}

// S0-B guard, failure branch: an archive entry that is NOT resolvable after the reset must fail closed
// with the ARCHIVE-1 diagnostic — and must not take any other archive file with it.
// (Dangling symlink = an archive entry the filesystem cannot resolve; a synthetic trigger, not data loss.)
BOOST_AUTO_TEST_CASE(d1_s0_v11_guard_fails_closed_on_unresolvable_archive_entry)
{
    S0Case c("v11");
    SeedOldVersionStore(c.dir);
    WriteFile(c.dir / "blk0001.dat", std::string(64, 'A') + "REAL");
    BOOST_REQUIRE_EQUAL(::symlink("/nonexistent/d1s0-target", (c.dir / "blk0002.dat").string().c_str()), 0);

    bool threw = false;
    std::string what;
    const std::string out = RunCaptured([&]() {
        try {
            CTxDB db("cr+");
            db.Close();
        } catch (const std::exception& e) {
            threw = true;
            what = e.what();
            fprintf(stdout, "threw: %s\n", e.what());
        }
    });
    Ev("v11 guard: threw=%d what=%s", (int)threw, OneLine(what).c_str());

    BOOST_CHECK(threw);
    BOOST_CHECK(what.find("ARCHIVE-1 VIOLATION") != std::string::npos);
    BOOST_CHECK(what.find("blk0002.dat") != std::string::npos);
    BOOST_CHECK(fs::exists(c.dir / "blk0001.dat"));                      // the real file is still there
    BOOST_CHECK_EQUAL(ReadFileToString(c.dir / "blk0001.dat"), std::string(64, 'A') + "REAL");
    BOOST_CHECK(out.find("Index reset: index state only") != std::string::npos);
}

BOOST_AUTO_TEST_SUITE_END()
