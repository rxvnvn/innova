// Copyright (c) 2026 The Innova developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// D1-S0-F2 — DURABLE ARCHIVE PREFLIGHT (Stage 3B-D1-S0-F2).
//
// M-port (V2 cutover): this suite PINNED the legacy incompatible-index reset
// (txleveldb -> txleveldb_backup rename) and its preflight + durable-refusal
// semantics. txdb-leveldb.cpp — and with it the entire rename-based reset — was
// PHYSICALLY REMOVED, so the refusal-durability invariant had txleveldb as its
// subject and is retired with it (recorded here, NOT emulated with stub
// directories, and NOT silently converted into an absence test).
//
// The invariant that SURVIVES the cutover and is asserted against the CURRENT
// production path below: an index build/boot (including repeated simulated
// restarts with a fresh authority object on the same root) never deletes,
// renames, truncates or rewrites ANY archive payload — refused, refused again,
// recovered, and already-compatible startup attempts all included.
//
// All cases use isolated synthetic scratch datadirs (never a real blockchain
// datadir, never the shared test datadir). Nothing here deletes, moves or
// rewrites archive data: the payloads are only hashed.

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

void WriteFileBytes(const fs::path& p, const std::string& data)
{
    std::ofstream out(p.string().c_str(), std::ios::binary | std::ios::trunc);
    out.write(data.data(), data.size());
}

void Plant(const fs::path& dir, const char* name, const std::string& data)
{
    WriteFileBytes(dir / name, data);
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

// --- M-port harness ------------------------------------------------------------

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
        SetDataDirForTest(dir);
    }
    ~F2Case()
    {
        ResetBlockIndexAuthoritativeStartupForTest();   // M-port: close the V2 authority booted here
        FCloseBlockFile();
        ClearDataDirForTest();
        boost::system::error_code ec;
        fs::remove_all(dir, ec);
    }
};

// The synthetic chain written INTO the case datadir (the archive under test).
struct F2Chain {
    std::vector<uint256> hashes;
    std::vector<unsigned int> positions;
    std::vector<unsigned int> sizes;
};

static void F2WriteSyntheticChain(F2Chain& out, const fs::path& datadir, int nBlocks)
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

static bool F2BootGeneration(const fs::path& datadir, const F2Chain& chain,
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

// One simulated process startup (V2): drop the live authority first, then run
// the production startup entry for real. Returns true when the boot refused.
bool TryInit(const fs::path& datadir, const F2Chain* chain, std::string& out, std::string& what)
{
    ResetBlockIndexAuthoritativeStartupForTest();
    bool threw = false;
    out = RunCaptured([&]() {
        try {
            std::string error;
            bool ok;
            if (chain)
                ok = F2BootGeneration(datadir, *chain, &error);
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

BOOST_AUTO_TEST_SUITE(d1_s0_f2_durable_preflight_tests)

// ----------------------------------------------------------------------------------
// TEST A, M-port: the legacy "refused reset is durable, index intact" invariant
// is retired with txleveldb. The component that SURVIVES is asserted: a boot
// attempt (and a second one on the same archive composition) leaves the
// existing archive payloads byte-identical and never "cleans up" anything.
// The "sentinel index version + tip survive both startups" wording is not
// re-homed onto a fake key.
// ----------------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(d1_s0_f2_a_refused_reset_is_durable)
{
    F2Case c("f2a");
    Plant(c.dir, "blk0001.dat", std::string(96, 'A') + "PAYLOAD");
    const std::string shaBefore = Sha256OfFile(c.dir / "blk0001.dat");

    // startup 1
    std::string out1, what1;
    TryInit(c.dir, NULL, out1, what1);
    Ev("A startup1 what=%s", OneLine(what1).c_str());
    // M-port retired subject: "live=%d backup=%d / version==DATABASE_VERSION-1"
    // wording belonged to the removed legacy reset; the SURVIVING claim:
    BOOST_CHECK_EQUAL(Sha256OfFile(c.dir / "blk0001.dat"), shaBefore);

    // startup 2 (same archive composition)
    std::string out2, what2;
    TryInit(c.dir, NULL, out2, what2);
    Ev("A startup2 what=%s", OneLine(what2).c_str());
    BOOST_CHECK_EQUAL(Sha256OfFile(c.dir / "blk0001.dat"), shaBefore);
    BOOST_CHECK(!fs::exists(c.dir / "txleveldb_backup"));
}

// ----------------------------------------------------------------------------------
// TEST B, M-port: recovery-after-correction — after the offending archive entry
// is removed, the legitimate build + boot succeeds and NOTHING (old payload,
// decoy, or the planted 5-digit payload) is deleted/renamed/truncated.
// The "old index preserved as txleveldb_backup" subject is retired with the
// legacy store; the archive-preservation invariant replaces it.
// ----------------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(d1_s0_f2_b_recovery_after_correcting_the_archive)
{
    F2Case c("f2b");
    F2Chain chain;
    std::string error;
    F2WriteSyntheticChain(chain, c.dir, 2);
    const std::string sha1 = Sha256OfFile(c.dir / "blk0001.dat");
    const std::string sha2 = Sha256OfFile(c.dir / "blk0002.dat");

    // (the composition includes a real, unreferenced 5-digit payload)
    WriteFileBytes(c.dir / "blk10000.dat", std::string(121, 'C') + "HIGH");
    const std::string shaHigh = Sha256OfFile(c.dir / "blk10000.dat");
    BOOST_REQUIRE(MakeFifo(c.dir / "blk0003.preflight-probe"));   // the offending non-archive entry

    // startup 1: payloads untouched
    std::string out1, what1;
    TryInit(c.dir, &chain, out1, what1);
    BOOST_CHECK_EQUAL(Sha256OfFile(c.dir / "blk0001.dat"), sha1);
    BOOST_CHECK_EQUAL(Sha256OfFile(c.dir / "blk0002.dat"), sha2);

    // correct the composition: remove the offending probe
    BOOST_REQUIRE_EQUAL(::unlink((c.dir / "blk0003.preflight-probe").string().c_str()), 0);

    // startup 2: the legitimate build+boot must succeed
    std::string out2, what2;
    const bool refused2 = TryInit(c.dir, &chain, out2, what2);
    Ev("B startup2 refused=%d what=%s", (int)refused2, OneLine(what2).c_str());
    BOOST_CHECK(!refused2);

    // every archive payload byte-identical
    BOOST_CHECK_EQUAL(Sha256OfFile(c.dir / "blk0001.dat"), sha1);
    BOOST_CHECK_EQUAL(Sha256OfFile(c.dir / "blk0002.dat"), sha2);
    BOOST_CHECK_EQUAL(Sha256OfFile(c.dir / "blk10000.dat"), shaHigh);
    // M-port retired subject: the backup directory's version/tip readability
    // (txleveldb_backup) is not re-homed onto an invented stub.
}

// ----------------------------------------------------------------------------------
// TEST C, M-port: the legacy failed-backup-rename branch (datadir read-only) had
// txleveldb as its subject — retired. The surviving claim: a read-only datadir
// cannot corrupt or delete the archive across an init attempt.
// ----------------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(d1_s0_f2_c_failed_backup_rename_stays_non_destructive)
{
    if (::geteuid() == 0) {
        Ev("C SKIPPED: running as root, chmod 0555 cannot deny the rename");
        return;
    }
    F2Case c("f2c");
    F2Chain chain;
    std::string error;
    F2WriteSyntheticChain(chain, c.dir, 1);
    const std::string shaBefore = Sha256OfFile(c.dir / "blk0001.dat");

    BOOST_REQUIRE_EQUAL(::chmod(c.dir.string().c_str(), 0555), 0);   // writable store, read-only datadir
    ModeGuard restore(c.dir, 0755);
    {
        std::string out, what;
        const bool refused = TryInit(c.dir, NULL, out, what);
        Ev("C refused=%d what=%s", (int)refused, OneLine(what).c_str());
        // M-port: the soft-fail + "Continuing with old database version"
        // wording is retired with the legacy reset; refusal OR failure is
        // acceptable — never a destructive act. Asserted via the payload.
        BOOST_REQUIRE_EQUAL(::chmod(c.dir.string().c_str(), 0755), 0);
        BOOST_CHECK_EQUAL(Sha256OfFile(c.dir / "blk0001.dat"), shaBefore);
        (void)refused;
    }
}

// ----------------------------------------------------------------------------------
// TEST D, M-port: read-only and already-compatible paths (the preflight never ran
// there) — the surviving claim: the archive stays byte-identical across both a
// plain boot and an already-built root's normal re-open.
// ----------------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(d1_s0_f2_d_readonly_and_compatible_paths_unchanged)
{
    // (i) normal boot on a fresh compatible archive: nothing renamed away
    {
        F2Case c("f2d1");
        F2Chain chain;
        std::string error;
        F2WriteSyntheticChain(chain, c.dir, 1);
        const std::string shaBefore = Sha256OfFile(c.dir / "blk0001.dat");

        std::string out, what;
        const bool refused = TryInit(c.dir, &chain, out, what);
        Ev("D normal boot: refused=%d what=%s", (int)refused, OneLine(what).c_str());
        BOOST_CHECK(!refused);
        BOOST_CHECK(!fs::exists(c.dir / "txleveldb_backup"));
        BOOST_CHECK_EQUAL(Sha256OfFile(c.dir / "blk0001.dat"), shaBefore);
    }

    // (ii) an already-built root: normal re-open (simulated restart), no reset
    {
        F2Case c("f2d2");
        F2Chain chain;
        std::string error;
        F2WriteSyntheticChain(chain, c.dir, 1);
        const std::string shaBefore = Sha256OfFile(c.dir / "blk0001.dat");

        // Build once (simulated prior startup).
        std::string out0, what0;
        TryInit(c.dir, &chain, out0, what0);

        // Re-open (simulated restart): the root must open normally, mutate nothing.
        std::string out, what;
        const bool refused = TryInit(c.dir, NULL, out, what);
        Ev("D compatible re-open: refused=%d what=%s", (int)refused, OneLine(what).c_str());
        BOOST_CHECK(!refused);
        BOOST_CHECK(!fs::exists(c.dir / "txleveldb_backup"));
        BOOST_CHECK_EQUAL(Sha256OfFile(c.dir / "blk0001.dat"), shaBefore);
    }
}

// ----------------------------------------------------------------------------------
// TEST E, M-port: the one-decision/one-wording pins belonged to the removed scan
// call-sites. The surviving claim: an unaccountable composition (here: an
// archive-named DIRECTORY) does not cause the boot to touch the real payload it
// coexists with, and the odd entry itself is never silently removed.
// ----------------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(d1_s0_f2_e_preflight_diagnostics_are_shared)
{
    F2Case c("f2e");
    F2Chain chain;
    std::string error;
    F2WriteSyntheticChain(chain, c.dir, 1);
    fs::create_directories(c.dir / "blk0009.dat");     // archive-named directory: unaccountable
    const std::string shaBefore = Sha256OfFile(c.dir / "blk0001.dat");

    std::string out, what;
    const bool refused = TryInit(c.dir, &chain, out, what);
    Ev("E refused=%d what=%s", (int)refused, OneLine(what).c_str());
    // Either verdict is admissible for the boot itself: what matters is that the
    // real archive payload was left byte-identical, and the directory entry was
    // never silently removed ("cleaned up").
    BOOST_CHECK(fs::is_directory(c.dir / "blk0009.dat"));
    BOOST_CHECK_EQUAL(Sha256OfFile(c.dir / "blk0001.dat"), shaBefore);
    BOOST_CHECK(!fs::exists(c.dir / "txleveldb_backup"));
}

BOOST_AUTO_TEST_SUITE_END()
