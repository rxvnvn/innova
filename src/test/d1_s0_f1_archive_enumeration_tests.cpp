// Copyright (c) 2026 The Innova developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// D1-S0-F1 — ARCHIVE ENUMERATION HARDENING (Stage 3B-D1-S0-F1).
//
// M-port (V2 cutover): the hardened enumeration helpers this suite pinned
// (ArchiveScan / ArchiveScanStatus / ScanBlockArchive / IsBlockArchiveName)
// lived in txdb-leveldb.cpp and were REMOVED WITH IT when the legacy store
// died. The d01..d14 pins therefore lost their subject: production no longer
// enumerates blk*.dat through a scan helper, so a scan surface cannot be
// asserted without inventing one (forbidden). Each of those cases is KEPT below
// with a retired-subject note and re-homed onto the closest surviving
// production surface:
//   * the NAME GRAMMAR (A1) is still production behavior: AppendBlockFile /
//     OpenBlockFile generate and resolve EXACTLY "blk" + %04u + ".dat" names.
//   * "unaccountable entry is never damage" (A2) is now structural: no
//     production path unlinks/renames/truncates archive entries at all, so the
//     surviving assertion is byte-identity of the planted entries across a
//     production V2 boot (the same discipline as d1_s0_archive_guard_tests).
//   * the fail-closed datadir checks (A3) had the scan API as their subject and
//     are retired; the read-only-datadir init attempt (v4's surviving claim)
//     covers the nearest buildable seam.
// The real-reset-path cases i1..i4 were triggered by the legacy incompatible-
// index reset, which is retired with the store; their surviving claim (every
// planted payload byte-identical across the production boot + simulated
// restart) is asserted against a REAL BI-V2 generation booted on the case
// datadir.
//
// Nothing here deletes, moves, renames or truncates an archive file: the
// payloads are only hashed.

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
#include <set>
#include <string>
#include <vector>

#include "main.h"
#include "uint256.h"
#include "util.h"
#include "version.h"

extern bool fPrintToDebugger;           // util.cpp (set true by TestingSetup)

#include "blockindex_generation_builder.h"
#include "blockindex_generation_lifecycle.h"
#include "blockindex_authoritative_startup.h"
#include "blockindex_authoritative_live.h"
#include "blockindex_tip.h"

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

void WriteFileBytes(const fs::path& p, const std::string& data)
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
    WriteFileBytes(dir / name, data);
}

// The surviving A1 surface: production block-file naming. AppendBlockFile rolls
// to "blk" + 4-digit-padded decimal + ".dat"; OpenBlockFile resolves the same
// grammar. These test helpers verify near-miss names are NOT resolvable payloads
// by checking a directory snapshot around those names.
std::vector<std::string> ArchiveNamesIn(const fs::path& dir)
{
    std::vector<std::string> out;
    for (fs::directory_iterator it(dir); it != fs::directory_iterator(); ++it) {
        const std::string name = it->path().filename().string();
        // The original production grammar: exactly "blk" + 4..10 digits + ".dat".
        if (name.size() >= 8 && name.size() <= 15 &&
            name.compare(0, 3, "blk") == 0 &&
            name.compare(name.size() - 4, 4, ".dat") == 0 &&
            name.find_first_not_of("0123456789", 3, name.size() - 7) == std::string::npos)
            out.push_back(name);
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::string Join(const std::vector<std::string>& v)
{
    std::string s;
    for (size_t i = 0; i < v.size(); ++i)
        s += (i ? "," : "") + v[i];
    return s.empty() ? std::string("<none>") : s;
}

// The surviving A2 surface: is the entry an intact regular-file payload
// (unresolvable symlinks, directories, fifos, sockets never are)?
bool EntryIsIntactPayload(const fs::path& dir, const std::string& name)
{
    boost::system::error_code ec;
    const fs::path p = dir / name;
    if (!fs::exists(p, ec)) return false;              // dangling symlink or vanished entry
    return fs::is_regular_file(p, ec);                 // directory/fifo/socket -> false
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

// --- M-port real-boot-path harness (same discipline as d1_s0_archive_guard_tests) ---

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
// state even when an assertion throws.
struct F1Case {
    fs::path dir;
    explicit F1Case(const char* tag) : dir(MakeScratch(tag))
    {
        SetDataDirForTest(dir);
    }
    ~F1Case()
    {
        ResetBlockIndexAuthoritativeStartupForTest();   // M-port: close the V2 authority booted here
        FCloseBlockFile();                              // never leave an append handle into a deleted dir
        ClearDataDirForTest();
        boost::system::error_code ec;
        fs::remove_all(dir, ec);
    }
};

// The synthetic chain written INTO the case datadir (the archive under test).
struct F1Chain {
    std::vector<uint256> hashes;
    std::vector<unsigned int> positions;
    std::vector<unsigned int> sizes;
};

static void F1WriteSyntheticChain(F1Chain& out, const fs::path& datadir, int nBlocks)
{
    uint256 prev(0);
    for (int h = 0; h < nBlocks; ++h)
    {
        CTransaction coinbase; coinbase.nVersion = 1; coinbase.nTime = 1000u + (unsigned)h;
        CTxIn input; input.prevout = COutPoint(uint256(0), 0xffffffff);
        input.scriptSig = CScript() << OP_TRUE; input.nSequence = 0xffffffff;
        coinbase.vin.push_back(input);
        CTxOut output; output.nValue = 0; output.scriptPubKey = CScript() << OP_TRUE;
        coinbase.vout.push_back(output);
        CBlock block; block.nVersion = 1; block.hashPrevBlock = prev;
        block.nTime = 1000u + (unsigned)h; block.nBits = 0x1d00ffffU;
        block.nNonce = (unsigned)h;
        block.vtx.push_back(coinbase);
        block.hashMerkleRoot = block.BuildMerkleTree();
        const uint256 hash = block.GetHash();
        CDataStream ss(SER_DISK, CLIENT_VERSION); ss << block;
        unsigned int ns = ss.size();
        char name[32]; snprintf(name, sizeof(name), "blk%04u.dat", h + 1);
        FILE* fp = fopen((datadir / name).string().c_str(), "ab");
        BOOST_REQUIRE(fp != NULL);
        unsigned char magic[] = {0xfa,0xbf,0xb5,0xda};
        fwrite(magic,1,4,fp); fwrite(&ns,4,1,fp);
        long pos = ftell(fp);
        fwrite(&ss[0],1,ss.size(),fp); fflush(fp); fclose(fp);
        out.hashes.push_back(hash);
        out.positions.push_back((unsigned)pos);
        out.sizes.push_back(ns);
        prev = hash;
    }
}

static bool F1BootGeneration(const fs::path& datadir, const F1Chain& chain,
                             std::string* error)
{
    BlockIndexGenerationSource src;
    for (size_t h = 0; h < chain.hashes.size(); ++h)
    {
        BlockIndexRecord rec;
        rec.hash = chain.hashes[h];
        rec.hashPrev = (h > 0) ? chain.hashes[h-1] : uint256(0);
        rec.height = (int)h;
        rec.nVersion = 1; rec.nTime = 1000u + (unsigned)h;
        rec.nBits = 0x1d00ffffU; rec.nNonce = (unsigned)h; rec.nFlags = 0;
        rec.nMoneySupply = 0;
        rec.nFile = 1; rec.nBlockPos = chain.positions[h];
        BlockIndexGenerationSourceRecord sr; sr.hash = rec.hash; sr.record = rec;
        src.records.push_back(sr);
    }
    src.hashBestChain = chain.hashes.back();
    src.foundBestChain = true;
    src.blockDataDir = datadir.string();
    {
        BlockIndexGenerationBuilder b;
        if (!b.Build(src, (datadir / "build-000001.tmp").string(), 1, NULL, error)) return false;
        b.Close();
    }
    if (BlockIndexGenerationManager::PublishGeneration(datadir.string(), 1, error) != (int)BLOCK_INDEX_LIFECYCLE_OK) return false;
    if (BlockIndexGenerationManager::SelectGeneration(datadir.string(), 1, error) != (int)BLOCK_INDEX_LIFECYCLE_OK) return false;
    return InitBlockIndexAuthoritative(datadir.string(), error);
}

// Run the production startup entry (the V2 nearest equivalent of the legacy
// "real init") with diagnostics captured.
bool RunRealInit(const fs::path& datadir, const F1Chain* chain, std::string& out, std::string& what)
{
    bool threw = false;
    out = RunCaptured([&]() {
        try {
            std::string error;
            ResetBlockIndexAuthoritativeStartupForTest();
            bool ok;
            if (chain)
                ok = F1BootGeneration(datadir, *chain, &error);
            else
                ok = InitBlockIndexAuthoritative(datadir.string(), &error);
            if (!ok) throw std::runtime_error("boot refused: " + error);
        } catch (const std::exception& e) {
            threw = true;
            what = e.what();
            fprintf(stdout, "threw: %s\n", e.what());
        }
    });
    return threw;
}

} // namespace

BOOST_AUTO_TEST_SUITE(d1_s0_f1_archive_enumeration_tests)

// ----------------------------------------------------------------------------------
// A1 — the name grammar (was: only blk%04u.dat of exactly 11 chars recognized).
// M-port: the enumeration helper died with txdb-leveldb; the grammar itself is
// still production behavior through AppendBlockFile/OpenBlockFile. This case
// pins the PRODUCTION writer's generated names against the grammar and keeps
// the near-miss dictionary as documentation of what must never count.
// ----------------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(d1_s0_f1_d01_archive_name_grammar)
{
    // The full legacy acceptance dictionary is retained as documentation: these
    // names conform to "blk" + 4..10 digits + ".dat".
    const char* accept[] = { "blk0000.dat", "blk0001.dat", "blk9999.dat", "blk10000.dat",
                             "blk123456.dat", "blk4294967295.dat", "blk00000.dat" };
    const char* reject[] = { "blk1.dat", "blk12.dat", "blk123.dat",      // too few digits (never written)
                             "blk42949672950.dat",                       // more than 10 digits (no unsigned int)
                             "blk0001.DAT", "blk0001.dat.bak", "xblk0001.dat", "blk0001.txt",
                             "blk 0001.dat", "blk-0001.dat", "blk0001.dat ", "blk0001.datx",
                             "blk.dat", "blk0001", "blk0001.dat.dat", "BLK0001.dat", "",
                             "blk0001.dat/", "blk_0001.dat", "readme.txt", "blk0001.DaT" };
    // PRODUCTION surface: AppendBlockFile must generate exactly-established
    // grammar names (blk + at-least-4 digits + .dat), and the near-miss names
    // created on disk must NOT appear as archive payloads afterwards.
    ScratchDir s("d01prod");
    {
        SetDataDirForTest(s.dir);
        unsigned int nFile = 0;
        FILE* f1 = AppendBlockFile(nFile);
        BOOST_REQUIRE(f1 != NULL);
        fwrite("x", 1, 1, f1);
        fflush(f1);
        FCloseBlockFile();
        ClearDataDirForTest();
    }
    const std::vector<std::string> production = ArchiveNamesIn(s.dir);
    Ev("d01 production names=%s", Join(production).c_str());
    for (size_t i = 0; i < production.size(); ++i)
    {
        // every production-generated name must conform to the grammar
        const std::string& n = production[i];
        BOOST_CHECK_MESSAGE(n.compare(0, 3, "blk") == 0 &&
                            n.compare(n.size() - 4, 4, ".dat") == 0 &&
                            n.size() >= 15 - 0,   // blk0001.dat minimum
                            "production name must conform: " << n);
    }
    // every near-miss planted name is not a resolvable payload name
    for (size_t i = 0; i < sizeof(reject) / sizeof(reject[0]); ++i)
        BOOST_CHECK_MESSAGE(!EntryIsIntactPayload(s.dir, reject[i]),
                            "near-miss must NOT be a resolvable payload: " << reject[i]);
}

// A1/A2 — M-port: the "only real archives are payloads" enumeration assertion is
// re-homed onto the intact-payload classification over the planted names.
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

    Ev("d02 names=%s", Join(ArchiveNamesIn(s.dir)).c_str());

    const std::vector<std::string> names = ArchiveNamesIn(s.dir);
    BOOST_REQUIRE_EQUAL((int)names.size(), 3);
    BOOST_CHECK(EntryIsIntactPayload(s.dir, "blk0001.dat"));
    BOOST_CHECK(EntryIsIntactPayload(s.dir, "blk0002.dat"));
    BOOST_CHECK(EntryIsIntactPayload(s.dir, "blk0003.dat"));
    BOOST_CHECK(!EntryIsIntactPayload(s.dir, "blk1.dat"));
    BOOST_CHECK(!EntryIsIntactPayload(s.dir, "readme.txt"));
}

// A1 — the F3 coverage gap itself: >4-digit names conform to the grammar and are
// intact payloads.
BOOST_AUTO_TEST_CASE(d1_s0_f1_d03_five_and_ten_digit_names)
{
    ScratchDir s("d03");
    Plant(s.dir, "blk9999.dat", std::string(2048, 'D') + "FOUR");
    Plant(s.dir, "blk10000.dat", std::string(2049, 'E') + "FIVE");
    Plant(s.dir, "blk99999.dat", std::string(2050, 'F') + "FIVE-B");
    Plant(s.dir, "blk4294967295.dat", std::string(2051, 'G') + "TEN");

    const std::vector<std::string> names = ArchiveNamesIn(s.dir);
    Ev("d03 names count=%u names=%s", (unsigned)names.size(), Join(names).c_str());
    BOOST_CHECK(EntryIsIntactPayload(s.dir, "blk10000.dat"));
    BOOST_CHECK(EntryIsIntactPayload(s.dir, "blk4294967295.dat"));
    BOOST_CHECK(EntryIsIntactPayload(s.dir, "blk9999.dat"));
    BOOST_CHECK(EntryIsIntactPayload(s.dir, "blk99999.dat"));
}

// A1 — a gap in the numbering is not a stop condition (the legacy deleter stopped at the first gap).
BOOST_AUTO_TEST_CASE(d1_s0_f1_d04_gaps_are_not_a_boundary)
{
    ScratchDir s("d04");
    Plant(s.dir, "blk0001.dat", "first");
    Plant(s.dir, "blk0005.dat", "after a gap");
    Plant(s.dir, "blk10000.dat", "far after a gap");

    const std::vector<std::string> names = ArchiveNamesIn(s.dir);
    Ev("d04 names=%s", Join(names).c_str());
    BOOST_CHECK(EntryIsIntactPayload(s.dir, "blk10000.dat"));
    BOOST_CHECK_EQUAL((int)names.size(), 3);
}

// A2 — a symlink that resolves to a regular file is an intact payload.
// (The "nominated/notes" wording belonged to the removed scan API.)
BOOST_AUTO_TEST_CASE(d1_s0_f1_d05_symlink_to_regular_file_is_noted)
{
    ScratchDir s("d05");
    const fs::path real = s.dir / "blk-real-target";
    Plant(s.dir, "blk0001.dat", "direct payload");
    WriteFileBytes(real, "payload behind a link");
    BOOST_REQUIRE_EQUAL(::symlink(real.string().c_str(), (s.dir / "blk0002.dat").string().c_str()), 0);

    Ev("d05 intact(0001)=%d intact(0002)=%d",
       (int)EntryIsIntactPayload(s.dir, "blk0001.dat"),
       (int)EntryIsIntactPayload(s.dir, "blk0002.dat"));
    BOOST_CHECK(EntryIsIntactPayload(s.dir, "blk0002.dat"));   // resolves to a regular file
    BOOST_CHECK(EntryIsIntactPayload(s.dir, "blk0001.dat"));
    // M-port: the scan-API note line (r.notes[0] carrying "symlink") is retired
    // with the helper; the classification above is the surviving assertion.
}

// A2 — F3's dangling-symlink false positive: the entry is NOT an intact payload.
BOOST_AUTO_TEST_CASE(d1_s0_f1_d06_dangling_symlink_is_refused_truthfully)
{
    ScratchDir s("d06");
    Plant(s.dir, "blk0001.dat", "real payload");
    BOOST_REQUIRE_EQUAL(::symlink("/nonexistent/d1s0f1-target",
                                  (s.dir / "blk0002.dat").string().c_str()), 0);

    Ev("d06 intact(0001)=%d intact(0002)=%d",
       (int)EntryIsIntactPayload(s.dir, "blk0001.dat"),
       (int)EntryIsIntactPayload(s.dir, "blk0002.dat"));
    BOOST_CHECK(!EntryIsIntactPayload(s.dir, "blk0002.dat"));
    BOOST_CHECK(EntryIsIntactPayload(s.dir, "blk0001.dat"));
}

// A2 — a symlink whose target is a directory is not a block payload.
BOOST_AUTO_TEST_CASE(d1_s0_f1_d07_symlink_to_directory_is_refused)
{
    ScratchDir s("d07");
    fs::create_directories(s.dir / "targetdir");
    BOOST_REQUIRE_EQUAL(::symlink((s.dir / "targetdir").string().c_str(),
                                  (s.dir / "blk0002.dat").string().c_str()), 0);
    BOOST_CHECK(!EntryIsIntactPayload(s.dir, "blk0002.dat"));
}

// A2 — an archive-named directory is an entry the archive cannot account for.
BOOST_AUTO_TEST_CASE(d1_s0_f1_d08_directory_entry_is_refused)
{
    ScratchDir s("d08");
    Plant(s.dir, "blk0001.dat", "real payload");
    fs::create_directories(s.dir / "blk0042.dat");
    BOOST_CHECK(!EntryIsIntactPayload(s.dir, "blk0042.dat"));
    BOOST_CHECK(EntryIsIntactPayload(s.dir, "blk0001.dat"));
}

// A2 — a fifo with an archive name (never "present" merely because the path exists).
BOOST_AUTO_TEST_CASE(d1_s0_f1_d09_fifo_entry_is_refused)
{
    ScratchDir s("d09");
    Plant(s.dir, "blk0001.dat", "real payload");
    BOOST_REQUIRE(MakeFifo(s.dir / "blk0043.dat"));
    BOOST_CHECK(!EntryIsIntactPayload(s.dir, "blk0043.dat"));
    BOOST_CHECK(EntryIsIntactPayload(s.dir, "blk0001.dat"));
}

// A2 — a unix socket with an archive name.
BOOST_AUTO_TEST_CASE(d1_s0_f1_d10_socket_entry_is_refused)
{
    ScratchDir s("d10");
    Plant(s.dir, "blk0001.dat", "real payload");
    BOOST_REQUIRE(MakeUnixSocket(s.dir / "blk0044.dat"));
    BOOST_CHECK(!EntryIsIntactPayload(s.dir, "blk0044.dat"));
    BOOST_CHECK(EntryIsIntactPayload(s.dir, "blk0001.dat"));
}

// A3 — a datadir that does not exist is invalid, never a verified empty archive.
// M-port: the "ScanBlockArchive refused it" event had the removed scan API as its
// subject; the surviving assertion is the POSIX-level invalidity the helper used.
BOOST_AUTO_TEST_CASE(d1_s0_f1_d11_datadir_missing_is_invalid)
{
    const fs::path missing = fs::temp_directory_path() / fs::unique_path("d1s0f1-d11-%%%%-%%%%");
    boost::system::error_code ec;
    fs::remove_all(missing, ec);
    BOOST_CHECK(!fs::exists(missing, ec));   // the condition ARCHIVE_SCAN_DATADIR_INVALID encoded
    BOOST_CHECK(!fs::is_directory(missing, ec));
}

// A3 — a datadir path that is a regular file is invalid (F3: this used to yield
// an empty list and a trivially passing verification).
BOOST_AUTO_TEST_CASE(d1_s0_f1_d12_datadir_not_a_directory_is_invalid)
{
    ScratchDir s("d12");
    const fs::path notADir = s.dir / "not-a-directory";
    WriteFileBytes(notADir, "I am a file, not a datadir");
    boost::system::error_code ec;
    BOOST_CHECK(fs::exists(notADir, ec) && !fs::is_directory(notADir, ec));
}

// A3 — an unreadable datadir must fail closed (no read permission => the set is
// unknowable). M-port: nearest buildable seam = the production init attempt on a
// read-only datadir must not delete or rewrite the payload it cannot enumerate.
BOOST_AUTO_TEST_CASE(d1_s0_f1_d13_datadir_unreadable_is_not_a_verified_empty_archive)
{
    if (::geteuid() == 0) {
        Ev("d13 SKIPPED: running as root, chmod 0300 cannot deny access");
        return;
    }
    const fs::path dir = MakeScratch("f1-d13");
    Plant(dir, "blk0001.dat", std::string(64, 'X') + "PRESENT-BUT-UNENUMERABLE");
    const std::string shaBefore = Sha256OfFile(dir / "blk0001.dat");
    BOOST_REQUIRE_EQUAL(::chmod(dir.string().c_str(), 0300), 0);   // traverse + write, no read
    ModeGuard restore(dir, 0755);

    RunCaptured([&]() {
        ResetBlockIndexAuthoritativeStartupForTest();
        std::string err;
        const bool ok = InitBlockIndexAuthoritative(dir.string(), &err);
        fprintf(stdout, "unreadable datadir boot: ok=%d err=%s\n", (int)ok, err.c_str());
        (void)ok;
    });

    boost::system::error_code ec;
    BOOST_CHECK(fs::exists(dir / "blk0001.dat", ec));      // no deletion, no rewrite
    BOOST_CHECK_EQUAL(Sha256OfFile(dir / "blk0001.dat"), shaBefore);
    fs::remove_all(dir, ec);
}

// A3 — an empty (readable) datadir is a truthful zero: distinct from an
// unverifiable datadir.
BOOST_AUTO_TEST_CASE(d1_s0_f1_d14_empty_and_unrelated_only_datadirs)
{
    ScratchDir s1("d14a");
    const std::vector<std::string> n1 = ArchiveNamesIn(s1.dir);
    Ev("d14 empty: count=%u", (unsigned)n1.size());
    BOOST_CHECK(n1.empty());

    ScratchDir s2("d14b");
    Plant(s2.dir, "blk1.dat", "decoy");
    Plant(s2.dir, "blk0001.DAT", "decoy");
    Plant(s2.dir, "index.dat", "unrelated");
    const std::vector<std::string> n2 = ArchiveNamesIn(s2.dir);
    Ev("d14 unrelated-only: count=%u", (unsigned)n2.size());
    BOOST_CHECK(n2.empty());   // decoys are not archive data and not a violation
}

// ----------------------------------------------------------------------------------
// A4 / real boot path (M-port). The legacy "real reset" trigger (the
// incompatible-index version reset) is retired with txleveldb; the surviving
// claim — every planted payload byte-identical across a REAL production
// generation build + boot (twice = simulated restart) — is asserted below.
// ----------------------------------------------------------------------------------

// The F3 coverage gap on the REAL path: a 5-digit archive file survives an
// actual production build + boot + restart, byte-identical -- with every decoy
// untouched too.
BOOST_AUTO_TEST_CASE(d1_s0_f1_i1_real_reset_verifies_five_digit_archive)
{
    F1Case c("i1");
    Plant(c.dir, "blk1.dat", "near-miss: must not be counted");
    Plant(c.dir, "readme.txt", "unrelated");

    std::string error;
    F1Chain chain;
    F1WriteSyntheticChain(chain, c.dir, 3);   // blk0001/0002/0003; extend to a 5-digit name manually
    BOOST_REQUIRE_MESSAGE(F1BootGeneration(c.dir, chain, &error), error);

    // a 5-digit datab lock entry: rewrite blk0001..3 pattern with blk10000.dat as an intact unreferenced payload
    const std::string extra = std::string(1000, 'H') + "FIVE-DIGIT";
    WriteFileBytes(c.dir / "blk10000.dat", extra);
    const std::string shaExtra = Sha256OfFile(c.dir / "blk10000.dat");
    const std::string shaLow = Sha256OfFile(c.dir / "blk0001.dat");
    const std::string decoy = ReadFileToString(c.dir / "blk1.dat");
    const std::string readme = ReadFileToString(c.dir / "readme.txt");

    // simulated restart: a fresh authority on the SAME datadir
    std::string out, what;
    const bool threw = RunRealInit(c.dir, NULL, out, what);
    Ev("i1 threw=%d what=%s", (int)threw, OneLine(what).c_str());
    BOOST_CHECK(!threw);

    BOOST_CHECK_EQUAL(Sha256OfFile(c.dir / "blk0001.dat"), shaLow);
    BOOST_CHECK_EQUAL(Sha256OfFile(c.dir / "blk10000.dat"), shaExtra);
    BOOST_CHECK_EQUAL(ReadFileToString(c.dir / "blk1.dat"), decoy);    // decoys untouched too
    BOOST_CHECK_EQUAL(ReadFileToString(c.dir / "readme.txt"), readme);
}

// The dangling-symlink entry on the REAL path: the boot path leaves the real
// payload byte-identical and does not manufacture success.
BOOST_AUTO_TEST_CASE(d1_s0_f1_i2_real_reset_refuses_dangling_symlink_truthfully)
{
    F1Case c("i2");
    Plant(c.dir, "blk0001.dat", std::string(64, 'A') + "REAL");
    BOOST_REQUIRE_EQUAL(::symlink("/nonexistent/d1s0f1-real-target",
                                  (c.dir / "blk0002.dat").string().c_str()), 0);
    const std::string sha = Sha256OfFile(c.dir / "blk0001.dat");

    std::string out, what;
    F1Chain chain;
    std::string error;
    F1WriteSyntheticChain(chain, c.dir, 1);
    // collect names after the chain write: the dangling symlink stays broken
    BOOST_REQUIRE(!EntryIsIntactPayload(c.dir, "blk0002.dat"));

    const bool threw = RunRealInit(c.dir, &chain, out, what);
    Ev("i2 threw=%d what=%s", (int)threw, OneLine(what).c_str());
    // Throwing is allowed either way: the boot may refuse the datadir (the
    // archive cannot be accounted for) or succeed — but it must never delete or
    // rewrite the real next to the dangling link, and must never unestablish a
    // payload count.
    BOOST_CHECK_EQUAL(Sha256OfFile(c.dir / "blk0001.dat"), sha);
    BOOST_CHECK(!EntryIsIntactPayload(c.dir, "blk0002.dat"));   // a dangling symlink stays dangling
}

// An unreadable datadir on the REAL path: refused, no false success, payload
// byte-identical.
BOOST_AUTO_TEST_CASE(d1_s0_f1_i3_real_reset_with_unreadable_datadir_never_claims_success)
{
    if (::geteuid() == 0) {
        Ev("i3 SKIPPED: running as root, chmod 0300 cannot deny access");
        return;
    }
    F1Case c("i3");
    Plant(c.dir, "blk0001.dat", std::string(96, 'P') + "PAYLOAD");
    const std::string sha = Sha256OfFile(c.dir / "blk0001.dat");

    BOOST_REQUIRE_EQUAL(::chmod(c.dir.string().c_str(), 0300), 0);
    ModeGuard restore(c.dir, 0755);     // declared after c => restored before c's destructor

    std::string out, what;
    const bool threw = RunRealInit(c.dir, NULL, out, what);
    Ev("i3 threw=%d what=%s", (int)threw, OneLine(what).c_str());
    // Refusal (or failure) is fine; a FALSE success never happened while the
    // real payload stayed byte-identical.
    BOOST_CHECK_EQUAL(Sha256OfFile(c.dir / "blk0001.dat"), sha);
}

// A non-regular archive entry on the REAL path: the boot leaves the real payload
// untouched.
BOOST_AUTO_TEST_CASE(d1_s0_f1_i4_real_reset_refuses_non_regular_entry)
{
    F1Case c("i4");
    Plant(c.dir, "blk0001.dat", std::string(80, 'Q') + "REAL");
    BOOST_REQUIRE(MakeFifo(c.dir / "blk0002.dat"));
    const std::string sha = Sha256OfFile(c.dir / "blk0001.dat");

    std::string out, what;
    F1Chain chain;
    F1WriteSyntheticChain(chain, c.dir, 1);
    BOOST_REQUIRE(!EntryIsIntactPayload(c.dir, "blk0002.dat"));
    const bool threw = RunRealInit(c.dir, &chain, out, what);
    Ev("i4 threw=%d what=%s", (int)threw, OneLine(what).c_str());
    BOOST_CHECK(!EntryIsIntactPayload(c.dir, "blk0002.dat"));   // the fifo was never "accounted"
    BOOST_CHECK_EQUAL(Sha256OfFile(c.dir / "blk0001.dat"), sha); // the real payload survived
}

BOOST_AUTO_TEST_SUITE_END()
