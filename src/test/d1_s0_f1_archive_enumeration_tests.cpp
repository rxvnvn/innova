// Copyright (c) 2026 The Innova developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// D1-S0-F1 — ARCHIVE ENUMERATION HARDENING (Stage 3B-D1-S0-F1).
//
// Scope: the independent Auditor's finding F3 (LOW) on the S0 guard — "only names of the exact shape
// blk%04u.dat are enumerated (blk10000.dat+ is missed), a non-directory datadir yields an empty list,
// and a dangling symlink trips a fail-closed false positive". This suite pins the hardened
// enumeration (ArchiveScan / IsBlockArchiveName / ScanBlockArchive) and re-pins the S0 invariant on
// the real reset path. Nothing here deletes, moves, renames or truncates an archive file: the
// production code under test is read-only.
//
// Invariants under test (ARCHIVE-PHYSICAL + fail-closed, unchanged in kind from S0):
//   A1  Archive names are exactly "blk" + 4..10 decimal digits + ".dat"; every near-miss is not data.
//   A2  A file the enumeration cannot account for (non-regular entry, unresolvable symlink) never
//       passes as an intact payload — and is never reported as damage caused by the reset.
//   A3  An unverifiable datadir (missing / not a directory / unreadable) is never a "verified" (==
//       empty) archive; it fails closed.
//   A4  A reset leaves every previously enumerated payload byte-identical (SHA-256).
//
// Harness notes: `printf` is redirected to OutputDebugStringF (suppressed by TestingSetup), so the
// production diagnostics are captured by RunCaptured (console route + fd 1); the global txleveldb
// handle must be closed before a case so CTxDB construction really runs the open/version path; and
// every case uses an isolated synthetic scratch directory — never the shared test datadir and never
// a real blockchain datadir.
//
// Case map (report innova-v2-chainstate-stage3b-d1-s0-f1-20261016.md):
//   d01 name grammar (accept blk0001/blk10000/blk4294967295; reject near-misses)
//   d02 normal archives + unrelated and near-miss files -> payloads are exactly the archives
//   d03 five/ten-digit names (the F3 coverage gap)      d04 gaps in the numbering
//   d05 symlink to a regular file (accepted, noted)     d06 dangling symlink (refused, truthful)
//   d07 symlink to a directory (refused)                d08 directory entry (refused)
//   d09 fifo entry (refused)                            d10 unix-socket entry (refused)
//   d11 datadir missing                                 d12 datadir not a directory
//   d13 datadir unreadable (EACCES, non-root)           d14 empty datadir (truthful zero)
//   i1  REAL reset with blk10000.dat: 3 payloads verified, byte-identical
//   i2  REAL reset with a dangling symlink: refused before the reset, truthfully, payload intact
//   i3  REAL reset with an unreadable datadir: refused, no false success, payload intact
//   i4  REAL reset with a fifo archive entry: refused before the reset, payload intact

#include <boost/test/unit_test.hpp>
#include <boost/filesystem.hpp>
#include <boost/system/error_code.hpp>

#include <openssl/sha.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
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
    fprintf(stdout, "  D1-S0-F1 ");
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

std::string OneLine(const std::string& s)
{
    std::string o;
    for (size_t i = 0; i < s.size(); ++i)
        o += (s[i] == '\n') ? '|' : s[i];
    while (!o.empty() && o[o.size() - 1] == '|') o.erase(o.size() - 1);
    return o;
}

// The captured diagnostic line containing `needle` (quotable evidence), or "<absent>".
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
                 fs::unique_path(std::string("d1s0f1-") + tag + "-%%%%-%%%%");
    fs::create_directories(d);
    return d;
}

// Synthetic payload: distinctive content so a byte change is impossible to miss.
void Plant(const fs::path& dir, const char* name, const std::string& data)
{
    WriteFile(dir / name, data);
}

std::string Join(const std::vector<std::string>& v)
{
    std::string s;
    for (size_t i = 0; i < v.size(); ++i)
        s += (i ? "," : "") + v[i];
    return s.empty() ? std::string("<none>") : s;
}

bool HasPayload(const ArchiveScan& s, const std::string& name)
{
    return std::find(s.payloads.begin(), s.payloads.end(), name) != s.payloads.end();
}

std::string StatusName(ArchiveScanStatus st)
{
    switch (st) {
    case ARCHIVE_SCAN_OK:              return "OK";
    case ARCHIVE_SCAN_DATADIR_INVALID: return "DATADIR_INVALID";
    case ARCHIVE_SCAN_UNREADABLE:      return "UNREADABLE";
    case ARCHIVE_SCAN_ENTRY_BAD:       return "ENTRY_BAD";
    }
    return "?";
}

// RAII scratch directory (removed even when an assertion throws).
struct ScratchDir {
    fs::path dir;
    explicit ScratchDir(const char* tag) : dir(MakeScratch(tag)) {}
    ~ScratchDir()
    {
        boost::system::error_code ec;
        fs::remove_all(dir, ec);
    }
};

// Restore a directory mode on scope exit (never leave an unreadable scratch dir behind).
struct ModeGuard {
    fs::path path;
    mode_t   mode;
    ModeGuard(const fs::path& p, mode_t m) : path(p), mode(m) {}
    ~ModeGuard() { ::chmod(path.string().c_str(), mode); }
};

bool MakeFifo(const fs::path& p) { return ::mkfifo(p.string().c_str(), 0644) == 0; }

bool MakeUnixSocket(const fs::path& p)
{
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    const std::string s = p.string();
    if (s.size() >= sizeof(addr.sun_path)) { ::close(fd); return false; }
    strncpy(addr.sun_path, s.c_str(), sizeof(addr.sun_path) - 1);
    const bool ok = (::bind(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0);
    ::close(fd);   // the socket node stays on disk (AF_UNIX paths are not unlinked on close)
    return ok;
}

// --- real-reset-path harness (same discipline as d1_s0_archive_guard_tests.cpp) ---------------

// Drop any process-global txleveldb handle so the next CTxDB construction really runs the
// open/version path on THIS case's datadir instead of reusing a handle bound elsewhere.
void ReleaseGlobalTxdb()
{
    if (GetGlobalTxdbPtrForTest() != NULL) {
        CTxDB t("r");   // read-only: reuses the existing handle, never re-inits a directory
        t.Close();
    }
}

// Run `fn` with the production diagnostics captured (they go through printf -> OutputDebugStringF,
// which TestingSetup suppresses, so the console route is enabled and fd 1 is redirected).
std::string RunCaptured(const std::function<void()>& fn)
{
    const bool savedToConsole = fPrintToConsole;
    const bool savedToDebugger = fPrintToDebugger;
    fPrintToConsole = true;
    fPrintToDebugger = false;

    fflush(stdout);
    const int savedFd = ::dup(1);
    const fs::path tmp = fs::temp_directory_path() / fs::unique_path("d1s0f1-cap-%%%%-%%%%");
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

// Per-case isolation: redirect GetDataDir() at a private scratch dir and restore the process-wide
// state (handle, cached datadir) even when an assertion throws.
struct F1Case {
    fs::path dir;
    explicit F1Case(const char* tag) : dir(MakeScratch(tag))
    {
        ReleaseGlobalTxdb();
        SetDataDirForTest(dir);
    }
    ~F1Case()
    {
        FCloseBlockFile();          // never leave a cached append handle pointing into a deleted dir
        ReleaseGlobalTxdb();
        ClearDataDirForTest();
        boost::system::error_code ec;
        fs::remove_all(dir, ec);
    }
};

// Seed a store holding an OLD persisted version: what an upgrade from an older build leaves behind,
// i.e. the state that arms the production reset path.
void SeedOldVersionStore(const fs::path& dir)
{
    CTxDB db("cr+");
    BOOST_REQUIRE(db.WriteVersion(DATABASE_VERSION - 1));
    db.Close();
}

// Run the production constructor (and therefore the reset path) with diagnostics captured.
// Returns true when the constructor threw; `what` carries the diagnostic.
bool RunRealInit(const fs::path& /*caseDir*/, std::string& out, std::string& what)
{
    bool threw = false;
    out = RunCaptured([&]() {
        try {
            CTxDB db("cr+");
            db.Close();
        } catch (const std::exception& e) {
            threw = true;
            what = e.what();
            fprintf(stdout, "threw: %s\n", e.what());
        }
    });
    return threw;
}

// Read the live index version through the production read path (post-reset state).
bool ReadLiveVersion(const fs::path& caseDir, int& versionOut)
{
    SetDataDirForTest(caseDir);
    bool ok = false;
    {
        CTxDB db("r");
        ok = db.ReadVersion(versionOut);
        db.Close();
    }
    SetDataDirForTest(caseDir);
    return ok;
}

} // namespace

BOOST_AUTO_TEST_SUITE(d1_s0_f1_archive_enumeration_tests)

// ---------------------------------------------------------------------------------------------
// A1 — the name grammar (F3: only blk%04u.dat of exactly 11 chars used to be recognized).
// ---------------------------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(d1_s0_f1_d01_archive_name_grammar)
{
    const char* accept[] = { "blk0000.dat", "blk0001.dat", "blk9999.dat", "blk10000.dat",
                             "blk123456.dat", "blk4294967295.dat", "blk00000.dat" };
    const char* reject[] = { "blk1.dat", "blk12.dat", "blk123.dat",      // too few digits (never written)
                             "blk42949672950.dat",                       // more than 10 digits (no unsigned int)
                             "blk0001.DAT", "blk0001.dat.bak", "xblk0001.dat", "blk0001.txt",
                             "blk 0001.dat", "blk-0001.dat", "blk0001.dat ", "blk0001.datx",
                             "blk.dat", "blk0001", "blk0001.dat.dat", "BLK0001.dat", "",
                             "blk0001.dat/", "blk_0001.dat", "readme.txt", "blk0001.DaT" };
    int okN = 0, badN = 0;
    for (size_t i = 0; i < sizeof(accept) / sizeof(accept[0]); ++i)
        if (IsBlockArchiveName(accept[i])) ++okN;
    for (size_t i = 0; i < sizeof(reject) / sizeof(reject[0]); ++i)
        if (!IsBlockArchiveName(reject[i])) ++badN;
    Ev("d01 accepted=%d/%d rejected=%d/%d",
       okN, (int)(sizeof(accept) / sizeof(accept[0])), badN, (int)(sizeof(reject) / sizeof(reject[0])));

    for (size_t i = 0; i < sizeof(accept) / sizeof(accept[0]); ++i)
        BOOST_CHECK_MESSAGE(IsBlockArchiveName(accept[i]), "should be an archive name: " << accept[i]);
    for (size_t i = 0; i < sizeof(reject) / sizeof(reject[0]); ++i)
        BOOST_CHECK_MESSAGE(!IsBlockArchiveName(reject[i]), "must NOT be an archive name: " << reject[i]);
}

// A1/A2 — normal archives plus unrelated and near-miss files: only real archives are payloads.
BOOST_AUTO_TEST_CASE(d1_s0_f1_d02_normal_archives_with_decoy_names)
{
    ScratchDir s("d02");
    Plant(s.dir, "blk0001.dat", std::string(1000, 'A') + "ONE");
    Plant(s.dir, "blk0002.dat", std::string(1001, 'B') + "TWO");
    Plant(s.dir, "blk0003.dat", std::string(1002, 'C') + "THREE");
    Plant(s.dir, "readme.txt", "not data");
    Plant(s.dir, "blk1.dat", "decoy: too few digits");
    Plant(s.dir, "blkBAD.dat", "decoy: non-digits");
    Plant(s.dir, "blk0001.dat.bak", "decoy: suffix");

    const ArchiveScan r = ScanBlockArchive(s.dir);
    Ev("d02 status=%s payloads=%s notes=%s", StatusName(r.status).c_str(),
       Join(r.payloads).c_str(), Join(r.notes).c_str());

    BOOST_CHECK(r.status == ARCHIVE_SCAN_OK);
    BOOST_REQUIRE_EQUAL((int)r.payloads.size(), 3);
    BOOST_CHECK(HasPayload(r, "blk0001.dat"));
    BOOST_CHECK(HasPayload(r, "blk0002.dat"));
    BOOST_CHECK(HasPayload(r, "blk0003.dat"));
    BOOST_CHECK(!HasPayload(r, "blk1.dat"));
    BOOST_CHECK(!HasPayload(r, "blkBAD.dat"));
    BOOST_CHECK(!HasPayload(r, "blk0001.dat.bak"));
}

// A1 — the F3 coverage gap itself: >4-digit names are recognized (and, on the real path, verified).
BOOST_AUTO_TEST_CASE(d1_s0_f1_d03_five_and_ten_digit_names)
{
    ScratchDir s("d03");
    Plant(s.dir, "blk9999.dat", std::string(2048, 'D') + "FOUR");
    Plant(s.dir, "blk10000.dat", std::string(2049, 'E') + "FIVE");
    Plant(s.dir, "blk99999.dat", std::string(2050, 'F') + "FIVE-B");
    Plant(s.dir, "blk4294967295.dat", std::string(2051, 'G') + "TEN");

    const ArchiveScan r = ScanBlockArchive(s.dir);
    Ev("d03 status=%s count=%u payloads=%s", StatusName(r.status).c_str(),
       (unsigned)r.payloads.size(), Join(r.payloads).c_str());

    BOOST_CHECK(r.status == ARCHIVE_SCAN_OK);
    BOOST_REQUIRE_EQUAL((int)r.payloads.size(), 4);
    BOOST_CHECK(HasPayload(r, "blk10000.dat"));
    BOOST_CHECK(HasPayload(r, "blk4294967295.dat"));
    BOOST_CHECK(HasPayload(r, "blk9999.dat"));
    BOOST_CHECK(HasPayload(r, "blk99999.dat"));
}

// A1 — a gap in the numbering is not a stop condition (the legacy deleter stopped at the first gap).
BOOST_AUTO_TEST_CASE(d1_s0_f1_d04_gaps_are_not_a_boundary)
{
    ScratchDir s("d04");
    Plant(s.dir, "blk0001.dat", "first");
    Plant(s.dir, "blk0005.dat", "after a gap");
    Plant(s.dir, "blk10000.dat", "far after a gap");

    const ArchiveScan r = ScanBlockArchive(s.dir);
    Ev("d04 status=%s payloads=%s", StatusName(r.status).c_str(), Join(r.payloads).c_str());
    BOOST_CHECK(r.status == ARCHIVE_SCAN_OK);
    BOOST_REQUIRE_EQUAL((int)r.payloads.size(), 3);
    BOOST_CHECK(HasPayload(r, "blk10000.dat"));
}

// A2 — a symlink that resolves to a regular file is a payload, but explicitly noted.
BOOST_AUTO_TEST_CASE(d1_s0_f1_d05_symlink_to_regular_file_is_noted)
{
    ScratchDir s("d05");
    const fs::path real = s.dir / "blk-real-target";
    Plant(s.dir, "blk0001.dat", "direct payload");
    WriteFile(real, "payload behind a link");
    BOOST_REQUIRE_EQUAL(::symlink(real.string().c_str(), (s.dir / "blk0002.dat").string().c_str()), 0);

    const ArchiveScan r = ScanBlockArchive(s.dir);
    Ev("d05 status=%s payloads=%s notes=%s", StatusName(r.status).c_str(),
       Join(r.payloads).c_str(), Join(r.notes).c_str());

    BOOST_CHECK(r.status == ARCHIVE_SCAN_OK);
    BOOST_CHECK(HasPayload(r, "blk0002.dat"));
    BOOST_REQUIRE_EQUAL((int)r.notes.size(), 1);
    BOOST_CHECK(r.notes[0].find("blk0002.dat") != std::string::npos);
    BOOST_CHECK(r.notes[0].find("symlink") != std::string::npos);
}

// A2 — F3's dangling-symlink false positive: refused, and described as it is (not as damage).
BOOST_AUTO_TEST_CASE(d1_s0_f1_d06_dangling_symlink_is_refused_truthfully)
{
    ScratchDir s("d06");
    Plant(s.dir, "blk0001.dat", "real payload");
    BOOST_REQUIRE_EQUAL(::symlink("/nonexistent/d1s0f1-target",
                                  (s.dir / "blk0002.dat").string().c_str()), 0);

    const ArchiveScan r = ScanBlockArchive(s.dir);
    Ev("d06 status=%s detail=%s", StatusName(r.status).c_str(), OneLine(r.detail).c_str());

    BOOST_CHECK(r.status == ARCHIVE_SCAN_ENTRY_BAD);
    BOOST_CHECK(r.detail.find("blk0002.dat") != std::string::npos);
    BOOST_CHECK(r.detail.find("symlink") != std::string::npos);
    BOOST_CHECK(r.detail.find("cannot be resolved") != std::string::npos);
    BOOST_CHECK(r.detail.find("disappeared") == std::string::npos);   // never accuse the reset
    BOOST_CHECK(HasPayload(r, "blk0001.dat"));                        // the real payload is known
}

// A2 — a symlink whose target is a directory is not a block payload.
BOOST_AUTO_TEST_CASE(d1_s0_f1_d07_symlink_to_directory_is_refused)
{
    ScratchDir s("d07");
    fs::create_directories(s.dir / "targetdir");
    BOOST_REQUIRE_EQUAL(::symlink((s.dir / "targetdir").string().c_str(),
                                  (s.dir / "blk0002.dat").string().c_str()), 0);

    const ArchiveScan r = ScanBlockArchive(s.dir);
    Ev("d07 status=%s detail=%s", StatusName(r.status).c_str(), OneLine(r.detail).c_str());
    BOOST_CHECK(r.status == ARCHIVE_SCAN_ENTRY_BAD);
    BOOST_CHECK(r.detail.find("blk0002.dat") != std::string::npos);
    BOOST_CHECK(r.detail.find("directory") != std::string::npos);
}

// A2 — an archive-named directory is an entry the archive cannot account for.
BOOST_AUTO_TEST_CASE(d1_s0_f1_d08_directory_entry_is_refused)
{
    ScratchDir s("d08");
    Plant(s.dir, "blk0001.dat", "real payload");
    fs::create_directories(s.dir / "blk0042.dat");

    const ArchiveScan r = ScanBlockArchive(s.dir);
    Ev("d08 status=%s detail=%s", StatusName(r.status).c_str(), OneLine(r.detail).c_str());
    BOOST_CHECK(r.status == ARCHIVE_SCAN_ENTRY_BAD);
    BOOST_CHECK(r.detail.find("blk0042.dat") != std::string::npos);
    BOOST_CHECK(r.detail.find("not a regular file") != std::string::npos);
    BOOST_CHECK(r.detail.find("directory") != std::string::npos);
}

// A2 — a fifo with an archive name (never "present" merely because the path exists).
BOOST_AUTO_TEST_CASE(d1_s0_f1_d09_fifo_entry_is_refused)
{
    ScratchDir s("d09");
    Plant(s.dir, "blk0001.dat", "real payload");
    BOOST_REQUIRE(MakeFifo(s.dir / "blk0043.dat"));

    const ArchiveScan r = ScanBlockArchive(s.dir);
    Ev("d09 status=%s detail=%s", StatusName(r.status).c_str(), OneLine(r.detail).c_str());
    BOOST_CHECK(r.status == ARCHIVE_SCAN_ENTRY_BAD);
    BOOST_CHECK(r.detail.find("blk0043.dat") != std::string::npos);
    BOOST_CHECK(r.detail.find("not a regular file") != std::string::npos);
    BOOST_CHECK(r.detail.find("fifo") != std::string::npos);
}

// A2 — a unix socket with an archive name.
BOOST_AUTO_TEST_CASE(d1_s0_f1_d10_socket_entry_is_refused)
{
    ScratchDir s("d10");
    Plant(s.dir, "blk0001.dat", "real payload");
    BOOST_REQUIRE(MakeUnixSocket(s.dir / "blk0044.dat"));

    const ArchiveScan r = ScanBlockArchive(s.dir);
    Ev("d10 status=%s detail=%s", StatusName(r.status).c_str(), OneLine(r.detail).c_str());
    BOOST_CHECK(r.status == ARCHIVE_SCAN_ENTRY_BAD);
    BOOST_CHECK(r.detail.find("blk0044.dat") != std::string::npos);
    BOOST_CHECK(r.detail.find("not a regular file") != std::string::npos);
}

// A3 — a datadir that does not exist is invalid, never a verified empty archive.
BOOST_AUTO_TEST_CASE(d1_s0_f1_d11_datadir_missing_is_invalid)
{
    const fs::path missing = fs::temp_directory_path() / fs::unique_path("d1s0f1-d11-%%%%-%%%%");
    boost::system::error_code ec;
    fs::remove_all(missing, ec);

    const ArchiveScan r = ScanBlockArchive(missing);
    Ev("d11 status=%s detail=%s", StatusName(r.status).c_str(), OneLine(r.detail).c_str());
    BOOST_CHECK(r.status == ARCHIVE_SCAN_DATADIR_INVALID);
    BOOST_CHECK(r.detail.find("does not exist") != std::string::npos);
    BOOST_CHECK(r.payloads.empty());
}

// A3 — a datadir path that is a regular file is invalid (F3: this used to yield an empty list and a
// trivially passing verification).
BOOST_AUTO_TEST_CASE(d1_s0_f1_d12_datadir_not_a_directory_is_invalid)
{
    ScratchDir s("d12");
    const fs::path notADir = s.dir / "not-a-directory";
    WriteFile(notADir, "I am a file, not a datadir");

    const ArchiveScan r = ScanBlockArchive(notADir);
    Ev("d12 status=%s detail=%s", StatusName(r.status).c_str(), OneLine(r.detail).c_str());
    BOOST_CHECK(r.status == ARCHIVE_SCAN_DATADIR_INVALID);
    BOOST_CHECK(r.detail.find("is not a directory") != std::string::npos);
    BOOST_CHECK(r.payloads.empty());
}

// A3 — an unreadable datadir must fail closed (no read permission => the set is unknowable).
BOOST_AUTO_TEST_CASE(d1_s0_f1_d13_datadir_unreadable_is_not_a_verified_empty_archive)
{
    if (::geteuid() == 0) {
        Ev("d13 SKIPPED: running as root, chmod 0300 cannot deny access");
        return;
    }
    const fs::path dir = MakeScratch("f1-d13");
    Plant(dir, "blk0001.dat", std::string(64, 'X') + "PRESENT-BUT-UNENUMERABLE");
    BOOST_REQUIRE_EQUAL(::chmod(dir.string().c_str(), 0300), 0);   // traverse + write, no read

    const ArchiveScan r = ScanBlockArchive(dir);
    BOOST_REQUIRE_EQUAL(::chmod(dir.string().c_str(), 0755), 0);   // restore before any assertion

    const std::string payloadStillThere = ReadFileToString(dir / "blk0001.dat");
    Ev("d13 status=%s detail=%s file=%u bytes", StatusName(r.status).c_str(),
       OneLine(r.detail).c_str(), (unsigned)payloadStillThere.size());

    BOOST_CHECK(r.status == ARCHIVE_SCAN_UNREADABLE);
    BOOST_CHECK(r.detail.find("cannot be enumerated") != std::string::npos);
    BOOST_CHECK(r.payloads.empty());          // no partial list is ever offered as verified
    BOOST_CHECK(r.detail.find("block-archive") == std::string::npos);  // not an entry complaint

    boost::system::error_code ec;
    fs::remove_all(dir, ec);
}

// A3 — an empty (readable) datadir is a truthful zero: distinct from an unverifiable datadir.
BOOST_AUTO_TEST_CASE(d1_s0_f1_d14_empty_and_unrelated_only_datadirs)
{
    ScratchDir s1("d14a");
    const ArchiveScan r1 = ScanBlockArchive(s1.dir);
    Ev("d14 empty: status=%s count=%u", StatusName(r1.status).c_str(), (unsigned)r1.payloads.size());
    BOOST_CHECK(r1.status == ARCHIVE_SCAN_OK);
    BOOST_CHECK(r1.payloads.empty());

    ScratchDir s2("d14b");
    Plant(s2.dir, "blk1.dat", "decoy");
    Plant(s2.dir, "blk0001.DAT", "decoy");
    Plant(s2.dir, "index.dat", "unrelated");
    const ArchiveScan r2 = ScanBlockArchive(s2.dir);
    Ev("d14 unrelated-only: status=%s count=%u", StatusName(r2.status).c_str(),
       (unsigned)r2.payloads.size());
    BOOST_CHECK(r2.status == ARCHIVE_SCAN_OK);   // decoys are not archive data and not a violation
    BOOST_CHECK(r2.payloads.empty());
}

// ---------------------------------------------------------------------------------------------
// A4 / real reset path
// ---------------------------------------------------------------------------------------------

// The F3 coverage gap on the REAL path: a 5-digit archive file is enumerated and verified across an
// actual index/version reset, and every payload stays byte-identical.
BOOST_AUTO_TEST_CASE(d1_s0_f1_i1_real_reset_verifies_five_digit_archive)
{
    F1Case c("i1");
    SeedOldVersionStore(c.dir);
    Plant(c.dir, "blk0001.dat", std::string(120, 'A') + "LOW");
    Plant(c.dir, "blk9999.dat", std::string(121, 'C') + "FOUR");
    Plant(c.dir, "blk10000.dat", std::string(122, 'B') + "HIGH");     // >4 digits
    Plant(c.dir, "blk1.dat", "near-miss: must not be counted");
    Plant(c.dir, "readme.txt", "unrelated");

    const std::string sha1 = Sha256OfFile(c.dir / "blk0001.dat");
    const std::string sha2 = Sha256OfFile(c.dir / "blk9999.dat");
    const std::string sha3 = Sha256OfFile(c.dir / "blk10000.dat");
    const std::string decoy = ReadFileToString(c.dir / "blk1.dat");

    std::string out, what;
    const bool threw = RunRealInit(c.dir, out, what);
    Ev("i1 threw=%d what=%s", (int)threw, OneLine(what).c_str());
    Ev("i1 guard_before=%s", LineContaining(out, "Index reset: index state only").c_str());
    Ev("i1 guard_mid=%s", LineContaining(out, "Required index version is ").c_str());
    Ev("i1 guard_after=%s", LineContaining(out, "Index reset verified non-destructive").c_str());

    BOOST_CHECK(!threw);
    BOOST_CHECK(out.find("Index reset: index state only, 3 block-archive file(s)") != std::string::npos);
    BOOST_CHECK(out.find("Index reset verified non-destructive: 3 block-archive file(s) intact") != std::string::npos);
    BOOST_CHECK(out.find("Required index version is ") != std::string::npos);   // the reset really ran

    int version = 0;
    const bool vOk = ReadLiveVersion(c.dir, version);
    Ev("i1 live_version=%d read_ok=%d sha_low=%s sha_10000=%s", version, (int)vOk,
       sha1.substr(0, 16).c_str(), sha3.substr(0, 16).c_str());
    BOOST_CHECK(vOk);
    BOOST_CHECK_EQUAL(version, DATABASE_VERSION);

    BOOST_CHECK_EQUAL(Sha256OfFile(c.dir / "blk0001.dat"), sha1);
    BOOST_CHECK_EQUAL(Sha256OfFile(c.dir / "blk9999.dat"), sha2);
    BOOST_CHECK_EQUAL(Sha256OfFile(c.dir / "blk10000.dat"), sha3);
    BOOST_CHECK_EQUAL(ReadFileToString(c.dir / "blk1.dat"), decoy);   // decoys untouched too
}

// The dangling-symlink entry on the REAL path: refused BEFORE the reset, truthfully, payload intact.
BOOST_AUTO_TEST_CASE(d1_s0_f1_i2_real_reset_refuses_dangling_symlink_truthfully)
{
    F1Case c("i2");
    SeedOldVersionStore(c.dir);
    Plant(c.dir, "blk0001.dat", std::string(64, 'A') + "REAL");
    BOOST_REQUIRE_EQUAL(::symlink("/nonexistent/d1s0f1-real-target",
                                  (c.dir / "blk0002.dat").string().c_str()), 0);
    const std::string sha = Sha256OfFile(c.dir / "blk0001.dat");

    std::string out, what;
    const bool threw = RunRealInit(c.dir, out, what);
    Ev("i2 threw=%d what=%s", (int)threw, OneLine(what).c_str());
    Ev("i2 guard_before=%s", LineContaining(out, "Index reset: index state only").c_str());
    Ev("i2 success_line=%s", LineContaining(out, "verified non-destructive").c_str());
    BOOST_CHECK(out.find("0 block-archive file(s)") == std::string::npos);   // no unestablished count
    BOOST_CHECK(out.find("cannot be accounted for") != std::string::npos);

    BOOST_CHECK(threw);
    BOOST_CHECK(what.find("ARCHIVE-1 VIOLATION") != std::string::npos);
    BOOST_CHECK(what.find("blk0002.dat") != std::string::npos);
    BOOST_CHECK(what.find("symlink") != std::string::npos);
    BOOST_CHECK(what.find("refusing to reset the index") != std::string::npos);
    BOOST_CHECK(what.find("disappeared") == std::string::npos);        // not blamed on the reset
    BOOST_CHECK(out.find("Index reset: index state only") != std::string::npos);
    BOOST_CHECK(out.find("verified non-destructive") == std::string::npos);   // no false success
    BOOST_CHECK_EQUAL(Sha256OfFile(c.dir / "blk0001.dat"), sha);       // the real payload survived

    boost::system::error_code ec;
    const bool hadLive = fs::exists(c.dir / "txleveldb", ec);
    const bool hadBackup = fs::exists(c.dir / "txleveldb_backup", ec);
    Ev("i2 datadir after refusal: txleveldb=%d txleveldb_backup=%d", (int)hadLive, (int)hadBackup);
}

// An unreadable datadir on the REAL path: refused, no false success, payload byte-identical.
BOOST_AUTO_TEST_CASE(d1_s0_f1_i3_real_reset_with_unreadable_datadir_never_claims_success)
{
    if (::geteuid() == 0) {
        Ev("i3 SKIPPED: running as root, chmod 0300 cannot deny access");
        return;
    }
    F1Case c("i3");
    SeedOldVersionStore(c.dir);
    Plant(c.dir, "blk0001.dat", std::string(96, 'P') + "PAYLOAD");
    const std::string sha = Sha256OfFile(c.dir / "blk0001.dat");

    BOOST_REQUIRE_EQUAL(::chmod(c.dir.string().c_str(), 0300), 0);
    ModeGuard restore(c.dir, 0755);     // declared after c => restored before c's destructor

    std::string out, what;
    const bool threw = RunRealInit(c.dir, out, what);
    Ev("i3 threw=%d what=%s", (int)threw, OneLine(what).c_str());
    Ev("i3 guard_before=%s", LineContaining(out, "Index reset: index state only").c_str());
    Ev("i3 success_line=%s", LineContaining(out, "verified non-destructive").c_str());
    BOOST_CHECK(out.find("0 block-archive file(s)") == std::string::npos);
    BOOST_CHECK(out.find("cannot be accounted for") != std::string::npos);

    BOOST_CHECK(threw);
    BOOST_CHECK(what.find("ARCHIVE-1 VIOLATION") != std::string::npos);
    BOOST_CHECK(what.find("refusing to reset the index") != std::string::npos);
    BOOST_CHECK(out.find("verified non-destructive") == std::string::npos);
    BOOST_CHECK(out.find("Index reset verified") == std::string::npos);
    BOOST_CHECK_EQUAL(Sha256OfFile(c.dir / "blk0001.dat"), sha);

    boost::system::error_code ec;
    const bool hadLive = fs::exists(c.dir / "txleveldb", ec);
    const bool hadBackup = fs::exists(c.dir / "txleveldb_backup", ec);
    Ev("i3 datadir after refusal: txleveldb=%d txleveldb_backup=%d", (int)hadLive, (int)hadBackup);
}

// A non-regular archive entry on the REAL path: refused before the reset, payload untouched.
BOOST_AUTO_TEST_CASE(d1_s0_f1_i4_real_reset_refuses_non_regular_entry)
{
    F1Case c("i4");
    SeedOldVersionStore(c.dir);
    Plant(c.dir, "blk0001.dat", std::string(80, 'Q') + "REAL");
    BOOST_REQUIRE(MakeFifo(c.dir / "blk0002.dat"));
    const std::string sha = Sha256OfFile(c.dir / "blk0001.dat");

    std::string out, what;
    const bool threw = RunRealInit(c.dir, out, what);
    Ev("i4 threw=%d what=%s", (int)threw, OneLine(what).c_str());
    Ev("i4 guard_before=%s", LineContaining(out, "Index reset: index state only").c_str());
    Ev("i4 success_line=%s", LineContaining(out, "verified non-destructive").c_str());
    BOOST_CHECK(out.find("0 block-archive file(s)") == std::string::npos);
    BOOST_CHECK(out.find("cannot be accounted for") != std::string::npos);

    BOOST_CHECK(threw);
    BOOST_CHECK(what.find("ARCHIVE-1 VIOLATION") != std::string::npos);
    BOOST_CHECK(what.find("blk0002.dat") != std::string::npos);
    BOOST_CHECK(what.find("not a regular file") != std::string::npos);
    BOOST_CHECK(what.find("refusing to reset the index") != std::string::npos);
    BOOST_CHECK(out.find("verified non-destructive") == std::string::npos);
    BOOST_CHECK_EQUAL(Sha256OfFile(c.dir / "blk0001.dat"), sha);
}

BOOST_AUTO_TEST_SUITE_END()
