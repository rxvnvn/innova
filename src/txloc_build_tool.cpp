// Bulk TX-LOC builder tool (Item 1 slice 2).
//
// Purpose: materialize <datadir>/chainstate/txloc/txloc.dat + txloc.dir for a
// migrated datadir driven ONLY by:
//   * the BI-V2 generation under <datadir>/blockindex (read-only), and
//   * the blk%04u.dat block archive in that same datadir.
//
// No legacy LevelDB read. No IBD. No reindex of anything else. No consensus
// change; the legacy CDiskTxPos semantics (main.cpp ConnectBlock lines ~6599)
// are reproduced byte-identically:
//   nTxPos(first tx) = nBlockPos + GetSerializeSize(CBlock, minus vtx/vchBlockSig slots)
//                      + GetSizeOfCompactSize(vtx.size())
//   nTxPos(tx k > 0) = nTxPos(tx k-1) + GetSerializeSize(tx k-1, SER_DISK)
// All positions, dedup and keep-first policy live in the (already tested)
// txloc::buildIndex bulk path (chainstate/txloc/txloc.cpp).
//
// Usage: innova-txloc-build <datadir>
//
// Exit codes: 0 = success; non-zero = build failed (fail-closed; index untouched).
#include "blockindex_tip.h"
#include "blockindex_v2_reader.h"
#include "blockindex_derived_state.h"
#include "blockindex_startup_ownership.h"
#include "chainstate/txloc/txloc.h"
#include "hash.h"
#include "main.h"
#include "kernel.h"
#include "util.h"
#include "bignum.h"
#include "checkpoints.h"
#include "ui_interface.h"
#include "wallet.h"

#include <boost/filesystem.hpp>
#include <cstdio>
#include <ctime>
#include <map>
#include <string>
#include <sys/resource.h>
#include <vector>

namespace fs = boost::filesystem;

// Standalone tool entry point.
// Globals required to link the shared engine OBJS (mirrors the other
// standalone tool mains: builder / builder-lm / catchup / navdiff).
CWallet* pwalletMain;
CClientUIInterface uiInterface;
bool fConfChange = false;
bool fEnforceCanonical = true;
bool fUseFastIndex = true;
unsigned int nDerivationMethodIndex = 0;
unsigned int nMinerSleep = 5000;
unsigned int nNodeLifespan = 7;
enum Checkpoints::CPMode CheckpointsMode = Checkpoints::STRICT;
void Shutdown(void*) { exit(0); }
void StartShutdown() { exit(0); }

// Stats payload returned by the build (checked in the report).
struct TxLocBuildStats
{
    uint64_t heights = 0;           // active blocks walked (i.e. heights 0..tip)
    uint64_t txsFed = 0;            // individual (txid, loc) fed into buildIndex
    uint64_t recordsWritten = 0;    // unique txids the merged index carries
    uint64_t outBytes = 0;          // size of the final txloc.dat in bytes
    double   wallSecs = 0.0;
    double   peakRssBytes = 0.0;
};

uint64_t NowMs() { return (uint64_t)(clock() * (1000.0 / CLOCKS_PER_SEC)); }

// Public entry exposed one level below main() so the bisect/CLI tests can set up
// globals deterministically.
bool RunInnovaTxLocBuild(const std::string& datadir, TxLocBuildStats* outStats, std::string* outErr)
{
    if (outStats) *outStats = TxLocBuildStats();
    if (outErr) outErr->clear();

    // Resolve BI-V2 canonical root; explicit diagnostic override supported.
    const std::string v2Root = ResolveBlockIndexV2Root(
        GetArg("-blockindexv2authoritative", ""), datadir);

    BlockIndexV2Reader reader;
    BlockIndexV2ReaderOptions opts;   // bounded 64 MiB LRU (unchanged default)
    std::string err;
    if (!reader.Open(v2Root, opts, &err))
    {
        if (outErr) *outErr = "txloc-build: open BI-V2 root: " + err;
        return false;
    }
    const BlockIndexSnapshot tip = reader.GetTip();
    if (!tip.found)
    {
        if (outErr) *outErr = "txloc-build: BI-V2 generation has no tip";
        return false;
    }
    const int tipHeight = tip.height;
    const uint256 tipHash = tip.hash;
    printf("txloc-build: BI-V2 root=%s tipHeight=%d tipHash=%s\n",
           v2Root.c_str(), tipHeight, tipHash.ToString().c_str());
    fflush(stdout);

    // Resolve blk*.dat archive directory (the SAME datadir, no separate reindex).
    const std::string blockData = (fs::path(datadir) ).string();

    // ---- Stream pass: for each height 0..tip, read the block from the archive
    // vault and emit a (txid, Loc) feed for txloc::buildIndex. ----
    struct FeedState
    {
        BlockIndexV2Reader* reader;
        const std::string* blockData;
        int tipHeight;
        std::string lastErr;
        bool   fail = false;
        uint64_t heightsWalked = 0;
        uint64_t txsFed = 0;
        // Read buffer reuse (one block at a time; never the whole archive).
        std::vector<unsigned char> buf;
        // Feed cursor across polls: the CURRENT block's deserialized txs plus
        // per-block loc constants. buildIndex polls feed() once per record, so we
        // must remember where we are inside a block between calls (a `return true`
        // after vtx[0] once dropped every non-coinbase tx).
        std::vector<CTransaction> curVtx;
        size_t curIdx = 0;
        unsigned int curHeight = 0;
        unsigned int curNFile = 0;
        unsigned int curNBlockPos = 0;
        unsigned int curNTxPos = 0;
    } fs;
    fs.reader = &reader;
    fs.blockData = &blockData;
    fs.tipHeight = tipHeight;

    auto feed = [&fs](unsigned char* txidOut, txloc::Loc* locOut) -> bool {
        for (;;)
        {
            // Emit the next tx of the current block (one record per poll). If a
            // block still has un-fed txs, serve them BEFORE loading the next height.
            if (fs.curIdx < fs.curVtx.size())
            {
                const CTransaction& tx = fs.curVtx[fs.curIdx];
                uint256 htx = tx.GetHash();
                memcpy(txidOut, htx.begin(), 32);
                locOut->nFile = fs.curNFile;
                locOut->nBlockPos = fs.curNBlockPos;
                locOut->nTxPos = fs.curNTxPos;
                locOut->nHeight = fs.curHeight;
                fs.txsFed++;
                // advance, matching ConnectBlock line ~6697
                fs.curNTxPos += ::GetSerializeSize(tx, SER_DISK, CLIENT_VERSION);
                fs.curIdx++;
                return true; // one tx record per callback
            }
            if (fs.heightsWalked > (uint64_t)fs.tipHeight) return false;
            const int h = (int)fs.heightsWalked;
            BlockIndexSnapshot snap;
            std::string serr;
            if (fs.reader->GetActiveByHeight(h, &snap, &serr) != BLOCK_INDEX_V2_READ_FOUND || !snap.found)
            {
                fs.fail = true;
                fs.lastErr = "height " + std::to_string(h) + " BI-V2 active lookup failed: " + serr;
                return false;
            }
            if (snap.height != h)
            {
                fs.fail = true;
                fs.lastErr = "height mismatch at h=" + std::to_string(h) + " snap.height=" + std::to_string(snap.height);
                return false;
            }
            // Read the exact archive bytes at (nFile, nBlockPos, nSize). Do NOT
            // re-serialize, re-construct or re-index anything here.
            {
                std::string blkFn = strprintf("blk%04u.dat", (unsigned)snap.nFile);
                fs::path blkPath = fs::path(*fs.blockData) / blkFn;
                FILE* f = fopen(blkPath.string().c_str(), "rb");
                if (!f)
                {
                    fs.fail = true;
                    fs.lastErr = "open " + blkPath.string() + ": " + strerror(errno);
                    return false;
                }
                if (fseek(f, (long)snap.nBlockPos, SEEK_SET) != 0)
                {
                    fclose(f);
                    fs.fail = true;
                    fs.lastErr = "fseek blk" + std::to_string(snap.nFile) + " to " + std::to_string(snap.nBlockPos) + " failed";
                    return false;
                }
                unsigned int nSize = snap.nSize;
                bool hasSize = snap.hasBlockSize;
                if (!hasSize || nSize == 0)
                {
                    // Fail closed: the V2 derived-size must say available for a
                    // fully committed block. Never guess, never re-scan the archive.
                    fclose(f);
                    fs.fail = true;
                    fs.lastErr = "derived size missing for height " + std::to_string(h);
                    return false;
                }
                if (nSize > (ADAPTIVE_BLOCK_CEILING + 4096))
                {
                    fclose(f);
                    fs.fail = true;
                    fs.lastErr = "implausible nSize " + std::to_string(nSize) + " at height " + std::to_string(h);
                    return false;
                }
                fs.buf.resize(nSize);
                if (fread(&fs.buf[0], 1, nSize, f) != nSize)
                {
                    fclose(f);
                    fs.fail = true;
                    fs.lastErr = "short read " + std::to_string(nSize) + " at height " + std::to_string(h);
                    return false;
                }
                fclose(f);
            }
            // Deserialize the block (exactly as ConnectBlock would see it, so the
            // per-tx GetSerializeSize advances match run-time exactly).
            CDataStream ss((const char*)&fs.buf[0], (const char*)(&fs.buf[0] + fs.buf.size()), SER_DISK, CLIENT_VERSION);
            CBlock b;
            try { ss >> b; }
            catch (const std::exception& e)
            {
                fs.fail = true;
                fs.lastErr = "height " + std::to_string(h) + " CBlock deserialize: " + e.what();
                return false;
            }
            if (ss.empty()) { /* consumed everything (informational only) */ }

            // Sanity: hash of the block must equal the BI-V2 record's hash (else
            // the archive and the index disagree; fail closed).
            if (b.GetHash() != snap.hash)
            {
                fs.fail = true;
                fs.lastErr = "height " + std::to_string(h) + " block hash mismatch: blk archive " + b.GetHash().ToString() + " vs BIV2 " + snap.hash.ToString();
                return false;
            }

            // Compute nTxPos exactly as ConnectBlock does (main.cpp ~6605, but there
            // it uses ::GetSerializeSize(CBlock(), SER_DISK, CLIENT_VERSION) minus
            // the slots for (empty vtx vector prefix) and the tx-count varint).
            unsigned int nTxPos;
            nTxPos = snap.nBlockPos
                     + ::GetSerializeSize(CBlock(), SER_DISK, CLIENT_VERSION)
                     - (2 * GetSizeOfCompactSize(0))
                     + GetSizeOfCompactSize(b.vtx.size());
            // Publish the just-loaded block into the feed cursor. On the next poll
            // the emit block above serves vtx[0..] one at a time (ALL txs of the
            // block, same height/nFile/nBlockPos, per-tx nTxPos offsets), then this
            // cursor is reset for the following height.
            fs.heightsWalked++;
            fs.curVtx = std::move(b.vtx);
            fs.curIdx = 0;
            fs.curHeight = h;
            fs.curNFile = snap.nFile;
            fs.curNBlockPos = snap.nBlockPos;
            fs.curNTxPos = nTxPos;
            // Perl tx-vtx could be empty (shouldn't happen on valid chain) -> walk next height.
            continue;
        }
    };

    const std::string txlocDir = (fs::path(datadir) / "chainstate" / "txloc").string();
    const std::string idxPath = txlocDir + "/txloc.dat";
    boost::system::error_code ec;
    fs::create_directories(txlocDir, ec);

    struct timespec tsBefore, tsAfter;
    clock_gettime(CLOCK_MONOTONIC, &tsBefore);

    struct rusage ruBefore, ruAfter;
    getrusage(RUSAGE_SELF, &ruBefore);

    txloc::ChainError te;
    const bool ok = txloc::buildIndex(idxPath, feed, &te);
    if (fs.fail)  // feed surfaced its own failure (height lookup, open, hash mismatch)
    {
        if (outErr) *outErr = "txloc-build: feed failed: " + fs.lastErr;
        return false;
    }
    if (!ok)
    {
        if (outErr) *outErr = "txloc-build: buildIndex: " + te.str();
        return false;
    }

    getrusage(RUSAGE_SELF, &ruAfter);
    clock_gettime(CLOCK_MONOTONIC, &tsAfter);
    const double wallSecs = (tsAfter.tv_sec - tsBefore.tv_sec) + (tsAfter.tv_nsec - tsBefore.tv_nsec) / 1e9;
    // RSS uses whichever of before/after was higher (buildIndex raises peak mid-run).
    const double peakRssBytes = (double)((ruAfter.ru_maxrss >= ruBefore.ru_maxrss ? ruAfter.ru_maxrss : ruBefore.ru_maxrss) * 1024);

    // Count what was written by reading the just-built index.
    txloc::TxLocator verify; bool verifyAbsent = false;
    if (!verify.open(idxPath, &verifyAbsent, &te))
    {
        if (outErr) *outErr = "txloc-build: verify open: " + te.str();
        return false;
    }
    uint64_t count = verify.count();
    verify.close();

    struct stat st;
    if (stat(idxPath.c_str(), &st) != 0)
    {
        if (outErr) *outErr = "txloc-build: stat output";
        return false;
    }

    if (outStats)
    {
        outStats->heights = fs.heightsWalked;
        outStats->txsFed = fs.txsFed;
        outStats->recordsWritten = count;
        outStats->outBytes = (uint64_t)st.st_size;
        outStats->wallSecs = wallSecs;
        outStats->peakRssBytes = peakRssBytes;
    }
    return true;
}

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        fprintf(stderr, "usage: %s <datadir>\n", argv[0]);
        return 2;
    }
    const std::string datadir = argv[1];

    // Globals required to link the shared engine OBJS (mirrors the other
    // standalone tool mains: builder / builder-lm / catchup / navdiff).
    // (this file does not own the wallet)
    // Resolve -blockindexv2authoritative style flags from the config so the
    // tool agrees with the production resolver; must run BEFORE any mapArgs use.
    ReadConfigFile(mapArgs, mapMultiArgs);

    TxLocBuildStats res;
    std::string err;
    if (!RunInnovaTxLocBuild(datadir, &res, &err))
    {
        fprintf(stderr, "TXLOC_BUILD_FAIL error=%s\n", err.c_str());
        return 1;
    }
    // The summary goes to BOTH stdout and stderr (stderr is unbuffered/line-buffered
    // so it survives any caller-side redirection).
    fprintf(stderr, "TXLOC_BUILD_OK heights=%llu txsFed=%llu records=%llu outBytes=%llu wallSecs=%.3f peakRssBytes=%llu\n",
            (unsigned long long)res.heights, (unsigned long long)res.txsFed,
            (unsigned long long)res.recordsWritten, (unsigned long long)res.outBytes,
            res.wallSecs, (unsigned long long)res.peakRssBytes);
    fprintf(stderr, "          out=%s/txloc.dat\n", datadir.c_str());
    printf("TXLOC_BUILD_OK heights=%llu txsFed=%llu records=%llu outBytes=%llu wallSecs=%.3f peakRssBytes=%llu\n",
           (unsigned long long)res.heights, (unsigned long long)res.txsFed,
           (unsigned long long)res.recordsWritten, (unsigned long long)res.outBytes,
           res.wallSecs, (unsigned long long)res.peakRssBytes);
    printf("          out=%s/txloc.dat\n", datadir.c_str());
    fflush(stdout);
    fflush(stderr);
    return 0;
}
