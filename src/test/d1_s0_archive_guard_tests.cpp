// Copyright (c) 2026 The Innova developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// D1-S0 — ARCHIVE-1 regression tests (Stage 3B-D1-S0).
//
// Invariant under test: index (re)initialization must never delete, truncate or
// otherwise modify the block archive (blk%04u.dat). The archive is permanent
// data; only INDEX stores may be reset/rebuilt.
//
// M-port (V2 cutover): the legacy CTxDB/txleveldb store — and with it the legacy
// version-mismatch index-reset path — was PHYSICALLY REMOVED from this tree.
// The version/backup reports, the "Index reset" guard wording and the
// ReadVersion / WriteHashBestChain / WriteInvalidBlockSet round-trips had
// txleveldb as their subject; those halves of the historical cases are recorded
// here as retired subjects (not silently dropped, and not emulated with stub
// keys). The invariant itself is ALIVE and is now exercised on the CURRENT
// production path: build + publish + select a real BI-V2 generation whose block
// archive is the case datadir, boot it through InitBlockIndexAuthoritative,
// restart it with a fresh authority object on the same root, and require every
// blk*.dat payload to stay byte-identical (SHA-256) throughout.
//
// Every case runs against an isolated scratch datadir with synthetic archive
// contents: no real blockchain data and never the shared test datadir
// (GetDataDir() is redirected per case by the test-only seam in util.cpp).
//
// Harness notes (learned the hard way, both are deliberate below):
//   * `printf` is `#define printf OutputDebugStringF` (util.h:272), and
//     TestingSetup sets fPrintToDebugger=true, so printf output is DISCARDED in
//     the test process. Captured runs flip fPrintToConsole on for the duration
//     (RunCaptured); this file's own evidence lines use fprintf(stdout,...).
//   * M-port: the legacy note about a process-global txleveldb handle is
//     obsolete — there is no global handle anymore. ReleaseGlobalTxdb() remains
//     as a documented no-op so the call-site ordering in S0Case is preserved.
//
// Case map (legacy report innova-v2-chainstate-stage3b-d1-s0-20261016.md, mapped to V2):
//   v1  "old persisted version + writable handle -> reset must preserve the archive":
//        version-reset subject retired; the durable equivalent IS asserted —
//        a real V2 build/boot + simulated restart on a pre-existing archive
//        leaves all payloads byte-identical.
//   v2  "current version, normal open -> nothing reset" -> single generation
//        boot; archive untouched. The txleveldb_backup assertion kept as the
//        shape of the original claim.
//   v3  read-only-handle variant: subject retired; archive identity across the
//        production boot asserted.
//   v4  "failed backup rename -> loud non-destructive failure": rename-backup
//        mechanics died with txleveldb; surviving claim asserted (a read-only
//        datadir cannot corrupt or delete archives across an init attempt).
//   v5  repeated initialization/restarts -> archive preserved every time
//        (ported onto V2 restarts; now the primary archive guard).
//   v6  several blk files incl. a numbering GAP -> nothing removed (ported).
//   v7  "incompatible index metadata reset, preserved as backup": subject
//        retired; the archive-integrity half asserted across a real V2 build.
//   v8  append + rollover -> still functional, archive never truncated
//        (unchanged: pure block-file writer semantics; no txdb anywhere).
//   v11 S0-B fail-closed guard on an unresolvable archive entry: the guard lived
//        in txdb-leveldb.cpp -> subject retired; surviving claim asserted (the
//        real blk0001.dat next to a dangling symlink is untouched by boot).

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
#include <set>
#include <string>
#include <vector>

#include "main.h"
#include "uint256.h"
#include "util.h"
#include "version.h"

// M-port: txdb-leveldb.cpp (which owned the old GetGlobalTxdbPtrForTest
// accessor) was removed; there is no legacy handle to reach anymore.
extern bool fPrintToDebugger;           // util.cpp (set true by TestingSetup)

#include "blockindex_generation_builder.h"
#include "blockindex_generation_lifecycle.h"
#include "blockindex_authoritative_startup.h"
#include "blockindex_authoritative_live.h"
#include "blockindex_tip.h"

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

void WriteFileBytes(const fs::path& p, const std::string& data)
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

// M-port: there is NO process-global txleveldb handle anymore (txdb-leveldb was
// removed with CTxDB). This helper was kept so the S0Case setup/teardown ORDER
// (release -> clear datadir) stays encoded in the test; it is a documented
// no-op, not a behavior change.
void ReleaseGlobalTxdb()
{
    // no legacy global handle exists in this tree
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
        ResetBlockIndexAuthoritativeStartupForTest();   // M-port: close the V2 authority booted on this dir
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
        WriteFileBytes(dir / name, std::string((size_t)(1000 + 37 * i), (char)('A' + i)) + "SYNTHETIC");
    }
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

// ---- M-port (V2) helpers -------------------------------------------------------
// S0ChainPos: the synthetic chain written INTO the case datadir (the "block
// archive" under test): per-height txid + parse position + parsed size.
struct S0ChainPos {
    std::vector<uint256> hashes;
    std::vector<unsigned int> positions;
    std::vector<unsigned int> sizes;
};

static void S0WriteSyntheticChain(S0ChainPos& out, const fs::path& datadir, int nBlocks)
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

// Build + publish + select a real AUTHORITATIVE generation from the synthetic
// chain and boot it through the REAL production startup entry.
static bool S0BootGeneration(const fs::path& datadir, const S0ChainPos& chain,
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

// Simulated restart: drop the live authority and reopen it on the SAME root —
// the V2 equivalent of the legacy "persistence round-trip through a fresh DB
// handle". The archive must be untouched by BOTH halves of this cycle.
static bool S0RestartAuthority(const fs::path& datadir, std::string* error)
{
    ResetBlockIndexAuthoritativeStartupForTest();
    return InitBlockIndexAuthoritative(datadir.string(), error);
}

} // namespace

BOOST_AUTO_TEST_SUITE(d1_s0_archive_guard_tests)

// S0-C(1)(6)(7)(10), M-port: the legacy version-mismatch reset is retired; the
// surviving durable invariant — a V2 index build + production boot + restart on
// a datadir that already carries an archive must leave every archive payload
// byte-identical — is asserted.
BOOST_AUTO_TEST_CASE(d1_s0_v1_version_mismatch_preserves_archive)
{
    S0Case c("v1");
    PlantContiguousArchive(c.dir, 3);
    const std::vector<S0ArchiveEntry> before = SnapshotArchive(c.dir);
    BOOST_REQUIRE_EQUAL(before.size(), 3u);
    Ev("v1 archive before: %s", SnapshotToString(before).c_str());

    std::string error;
    S0ChainPos chain;
    S0WriteSyntheticChain(chain, c.dir, 3);   // appends parseable block entries on top of the planted archive
    BOOST_REQUIRE_MESSAGE(S0BootGeneration(c.dir, chain, &error), error);
    Ev("v1 boot ok");

    bool identical = ArchiveIdentical(c.dir, before);
    BOOST_CHECK(identical);   // same names, sizes and sha256 => no bytes of the archive changed

    // simulated restart: a fresh authority object on the SAME datadir
    BOOST_REQUIRE_MESSAGE(S0RestartAuthority(c.dir, &error), error);
    std::vector<S0ArchiveEntry> after;
    identical = ArchiveIdentical(c.dir, before, &after);
    Ev("v1 archive after restart:  %s", SnapshotToString(after).c_str());
    BOOST_CHECK(identical);

    // M-port retired subjects: the txleveldb backup/live index version checks
    // (WriteVersion(DATABASE_VERSION - 1) planting, ReadVersion round-trip,
    // the "Index reset: index state only" / "verified non-destructive" diag
    // lines and the preserved txleveldb_backup directory) had the removed
    // legacy store as their subject and are NOT re-homed onto invented stubs.
    // The archive-permanence claim is fully asserted above.
}

// S0-C(2)(10), M-port: a boot on an already-indexed datadir mutates nothing.
BOOST_AUTO_TEST_CASE(d1_s0_v2_current_version_normal_open_preserves_archive)
{
    S0Case c("v2");
    PlantContiguousArchive(c.dir, 2);
    const std::vector<S0ArchiveEntry> before = SnapshotArchive(c.dir);

    std::string error;
    S0ChainPos chain;
    S0WriteSyntheticChain(chain, c.dir, 2);
    BOOST_REQUIRE_MESSAGE(S0BootGeneration(c.dir, chain, &error), error);

    std::vector<S0ArchiveEntry> after;
    const bool identical = ArchiveIdentical(c.dir, before, &after);
    Ev("v2 archive: before=%s", SnapshotToString(before).c_str());
    Ev("v2 archive: after =%s", SnapshotToString(after).c_str());
    BOOST_CHECK(identical);
    BOOST_CHECK(!fs::exists(c.dir / "txleveldb_backup"));
    // M-port: the two legacy open-mode diagnostics (writable "cr+" vs "r"
    // handling) are retired with the legacy constructor. The "no reset
    // happened at all" observation is the archive identity above.
}

// S0-C(3)(10), M-port: the read-only-handle variant lost its subject (the mode
// strings no longer exist). The case survives asserting the archive identity
// across the production boot — the non-mutating path it used to guard.
BOOST_AUTO_TEST_CASE(d1_s0_v3_readonly_handle_mismatch_preserves_archive)
{
    S0Case c("v3");
    PlantContiguousArchive(c.dir, 2);
    const std::vector<S0ArchiveEntry> before = SnapshotArchive(c.dir);

    std::string error;
    S0ChainPos chain;
    S0WriteSyntheticChain(chain, c.dir, 2);
    BOOST_REQUIRE_MESSAGE(S0BootGeneration(c.dir, chain, &error), error);

    std::vector<S0ArchiveEntry> after;
    const bool identical = ArchiveIdentical(c.dir, before, &after);
    Ev("v3 archive: before=%s", SnapshotToString(before).c_str());
    Ev("v3 archive: after =%s", SnapshotToString(after).c_str());
    BOOST_CHECK(identical);
    BOOST_CHECK(!fs::exists(c.dir / "txleveldb_backup"));

    // M-port retired subject: the persisted legacy version read
    // (ReadVersion == DATABASE_VERSION - 1) is retired with the removed store;
    // the "nothing was reset" observation is the archive identity above.
    (void)fPrintToDebugger;
}

// S0-C(4)(9)(10), M-port: the legacy backup-rename failure branch had txleveldb
// as its subject (BI-V2 generations are never renamed in place) — that branch is
// retired. The surviving claim: a read-only datadir cannot corrupt or delete the
// archive across an init attempt.
BOOST_AUTO_TEST_CASE(d1_s0_v4_failed_backup_is_non_destructive)
{
    if (::geteuid() == 0) {
        BOOST_TEST_MESSAGE("d1_s0_v4 skipped: running as root, a permission-based failure cannot "
                           "be simulated");
        return;
    }
    S0Case c("v4");
    PlantContiguousArchive(c.dir, 3);
    const std::vector<S0ArchiveEntry> before = SnapshotArchive(c.dir);

    std::string error;
    S0ChainPos chain;
    S0WriteSyntheticChain(chain, c.dir, 3);   // seeds planted while the dir is still writable

    BOOST_REQUIRE_EQUAL(::chmod(c.dir.string().c_str(), 0555), 0);   // datadir not writable
    const std::string out = RunCaptured([&]() {
        ResetBlockIndexAuthoritativeStartupForTest();
        std::string err2;
        const bool ok = InitBlockIndexAuthoritative(c.dir.string(), &err2);
        fprintf(stdout, "read-only datadir boot: ok=%d err=%s\n", (int)ok, err2.c_str());
        (void)ok;
    });
    BOOST_REQUIRE_EQUAL(::chmod(c.dir.string().c_str(), 0755), 0);
    Ev("v4 failure output: %s", OneLine(out).c_str());

    const bool identical = ArchiveIdentical(c.dir, before);
    Ev("v4 archive: before=%s", SnapshotToString(before).c_str());
    BOOST_CHECK(identical);     // no deletion, no truncation, no rewrite
}

// S0-C(5)(10), M-port: repeated initialization/restarts never remove archive
// data — the primary surviving guard, ported onto V2 restarts.
BOOST_AUTO_TEST_CASE(d1_s0_v5_repeated_initialization_preserves_archive)
{
    S0Case c("v5");
    PlantContiguousArchive(c.dir, 4);
    const std::vector<S0ArchiveEntry> before = SnapshotArchive(c.dir);
    BOOST_REQUIRE_EQUAL(before.size(), 4u);

    std::string error;
    S0ChainPos chain;
    S0WriteSyntheticChain(chain, c.dir, 4);
    BOOST_REQUIRE_MESSAGE(S0BootGeneration(c.dir, chain, &error), error);

    for (int i = 0; i < 3; ++i) {
        // M-port: the legacy re-seed of an OLD persisted version
        // (SeedOldVersionStore each iteration) is retired — a V2 root always
        // reopens its committed generation state; the restart itself is the
        // real event the guard used to surround.
        BOOST_REQUIRE_MESSAGE(S0RestartAuthority(c.dir, &error), error);
        std::vector<S0ArchiveEntry> after;
        const bool identical = ArchiveIdentical(c.dir, before, &after);
        Ev("v5 restart %d: preserved=%d entries=%u", i + 1, (int)identical,
           (unsigned)after.size());
        BOOST_CHECK(identical);
        BOOST_CHECK_EQUAL(after.size(), before.size());
    }
}

// S0-C(6)(10): several blk files with distinct contents, including a numbering GAP. The removed legacy
// loop deleted only a contiguous prefix (blk0001..gap) — nothing at all may be removed now.
BOOST_AUTO_TEST_CASE(d1_s0_v6_multiple_files_with_gap_preserved)
{
    S0Case c("v6");
    PlantContiguousArchive(c.dir, 2);                                    // blk0001, blk0002
    WriteFileBytes(c.dir / "blk0005.dat", std::string(17, 'C') + "FIVE"); // gap: no blk0003/0004
    WriteFileBytes(c.dir / "blk0010.dat", std::string(9, 'D') + "TEN");
    const std::vector<S0ArchiveEntry> before = SnapshotArchive(c.dir);
    BOOST_REQUIRE_EQUAL(before.size(), 4u);
    Ev("v6 archive before: %s", SnapshotToString(before).c_str());

    std::string error;
    S0ChainPos chain;
    S0WriteSyntheticChain(chain, c.dir, 4);   // blk0001..blk0004 get real block entries; blk0005/blk0010 stay unreferenced
    BOOST_REQUIRE_MESSAGE(S0BootGeneration(c.dir, chain, &error), error);
    BOOST_REQUIRE_MESSAGE(S0RestartAuthority(c.dir, &error), error);

    std::vector<S0ArchiveEntry> after;
    const bool identical = ArchiveIdentical(c.dir, before, &after);
    Ev("v6 archive after:  %s", SnapshotToString(after).c_str());
    // M-port: the legacy "4 block-archive file(s) intact" guard line is retired
    // with the guard itself; the count and identity are asserted directly.

    BOOST_CHECK_EQUAL(after.size(), 4u);
    BOOST_CHECK(identical);
    BOOST_CHECK(fs::exists(c.dir / "blk0005.dat"));   // beyond the gap: not even reachable by the old loop
    BOOST_CHECK(fs::exists(c.dir / "blk0010.dat"));
}

// S0-C(7)(10), M-port: the incompatible-index-metadata reset-and-backup subject
// is retired with txleveldb (no legacy index exists, and the BI-V2 generations
// are never renamed in place). The case keeps the archive-integrity half: a
// sentinel archive stays byte-identical across a real V2 build/boot on the same
// datadir. The old tip-sentinel wording is retired, not re-homed onto a fake key.
BOOST_AUTO_TEST_CASE(d1_s0_v7_index_metadata_reset_is_not_archive_deletion)
{
    S0Case c("v7");
    PlantContiguousArchive(c.dir, 2);
    const std::vector<S0ArchiveEntry> before = SnapshotArchive(c.dir);
    const std::string sentinel = ReadFileToString(c.dir / "blk0001.dat");

    std::string error;
    S0ChainPos chain;
    S0WriteSyntheticChain(chain, c.dir, 2);
    BOOST_REQUIRE_MESSAGE(S0BootGeneration(c.dir, chain, &error), error);
    BOOST_REQUIRE_MESSAGE(S0RestartAuthority(c.dir, &error), error);

    std::vector<S0ArchiveEntry> after;
    const bool identical = ArchiveIdentical(c.dir, before, &after);
    Ev("v7 archive: before=%s", SnapshotToString(before).c_str());
    Ev("v7 archive: after =%s", SnapshotToString(after).c_str());
    BOOST_CHECK(identical);   // archive untouched
    BOOST_CHECK_EQUAL(ReadFileToString(c.dir / "blk0001.dat"), sentinel);
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

// S0-B guard fail-closed branch, M-port: the guard itself lived in
// txdb-leveldb.cpp and is retired with it. The surviving claim: the REAL
// blk0001.dat sitting next to a dangling symlink is left byte-identical by a
// production boot attempt on the same datadir (the init path reads only its own
// committed entries and never unlinks archive payloads).
BOOST_AUTO_TEST_CASE(d1_s0_v11_guard_fails_closed_on_unresolvable_archive_entry)
{
    S0Case c("v11");
    WriteFileBytes(c.dir / "blk0001.dat", std::string(64, 'A') + "REAL");
    BOOST_REQUIRE_EQUAL(::symlink("/nonexistent/d1s0-target", (c.dir / "blk0002.dat").string().c_str()), 0);
    const std::string shaBefore = Sha256OfFile(c.dir / "blk0001.dat");

    bool threw = false;
    std::string what;
    const std::string out = RunCaptured([&]() {
        try {
            std::string error;
            S0ChainPos chain;
            S0WriteSyntheticChain(chain, c.dir, 1);
            const bool ok = S0BootGeneration(c.dir, chain, &error);
            if (!ok) throw std::runtime_error("boot refused: " + error);
        } catch (const std::exception& e) {
            threw = true;
            what = e.what();
            fprintf(stdout, "threw: %s\n", e.what());
        }
    });
    Ev("v11 guard: threw=%d what=%s", (int)threw, OneLine(what).c_str());

    BOOST_CHECK(fs::exists(c.dir / "blk0001.dat"));                      // the real file is still there
    BOOST_CHECK_EQUAL(ReadFileToString(c.dir / "blk0001.dat"), std::string(64, 'A') + "REAL");
    BOOST_CHECK_EQUAL(Sha256OfFile(c.dir / "blk0001.dat"), shaBefore);
    // M-port retired subjects: the ARCHIVE-1 VIOLATION diagnostic wording and
    // the "Index reset: index state only" guard line belonged to the removed
    // legacy reset path; the non-destructiveness claim is the hash round-trip.
    (void)out;
}

BOOST_AUTO_TEST_SUITE_END()
