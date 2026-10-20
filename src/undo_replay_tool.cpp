// undo-replay: FULL HISTORICAL UNDO RECONSTRUCTION for the migrated chain.
//
// Rebuilds a complete Store3A chainstate (UTXO tree + a full per-block undo.dat)
// by sequential delta-apply replay from genesis over the BI-V2 ACTIVE chain.
//
// What it does (state reconstruction, NOT IBD):
//   * reads the BI-V2 active generation (records.dat + active.dat) of <datadir>,
//   * for every height 0..tip reads the REAL block from blk*.dat by
//     (nFile, nBlockPos, nSize), checks its hash against the index entry,
//   * builds the block delta with the EXISTING s16_BuildDeltaForBlock project +
//     cs::observeDelta (the SAME code the live ConnectBlock path uses) —
//     no coin logic is reimplemented here,
//   * publishes each delta through the EXISTING CommitStore3A batch path
//     (applyBatchBlock + publishBatch), which appends one UndoRecord per block
//     to undo.dat — the start of a FULL undo history for every historical block.
//
// Checks (fail closed, no consensus validation):
//   * per-block hash must equal the BI-V2 record hash (archive/index binding),
//   * height continuity (BI-V2 active.dat is dense; a gap stops the run),
//   * a missing prevout coin makes observeDelta FAIL-CLOSED (MISSING_INPUT) and
//     the run aborts with the height and outpoint — never silently skipped.
//   * NO CheckBlock, NO PoW, NO signature verification (by design).
//
// usage: innova-undo-replay <datadir> <outdir>
//   <datadir> source (read-only): BI-V2 at <datadir>/blockindex (CURRENT -> gen-N),
//             blocks at <datadir>/blk*.dat
//   <outdir>  destination: a FRESH empty directory for the new CommitStore3A.
#include "main.h"
#include "ui_interface.h"
#include "checkpoints.h"
#include "util.h"

#include "blockindex_generation_lifecycle.h"
#include "blockindex_derived_state.h"
#include "blockindex_activeindex.h"
#include "fixed_blockindex_store.h"

#include "chainstate/store3a/incstore.h"
#include "chainstate/observe.h"

#include <boost/filesystem.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <string>
#include <vector>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

// Globals required to link the shared engine OBJS (mirrors the other tools).
CWallet* pwalletMain;
CClientUIInterface uiInterface;
bool fConfChange = false;
bool fEnforceCanonical = true;
bool fUseFastIndex = true;
unsigned int nDerivationMethodIndex = 0;
unsigned int nMinerSleep = 5000;
unsigned int nNodeLifespan = 7;
enum Checkpoints::CPMode CheckpointsMode = Checkpoints::STRICT;
void Shutdown(void* parg) { exit(0); }
void StartShutdown() { exit(0); }

using cs::store3a::CommitStore3A;
using cs::ChainError;
typedef std::map<uint64_t, int32_t> ActiveMap;   // ((nFile<<32)|nBlockPos) -> height

static double now_s() {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

// The delta builder from main.cpp is static; this tool needs the EXACT SAME
// projection. It is duplicated here BY CONTRACT (a thin ObservedBlock builder +
// observeDelta call) — the coin logic itself lives in cs::observeDelta, which
// is shared verbatim. Do not change the projection field set independently.
struct s16_ReplayCoin { CTransaction tx; int32_t height; };
typedef std::map<uint256, s16_ReplayCoin> s16_ReplayOverlay;

static bool ur_BuildDeltaForBlock(const CBlock& blk, int32_t height,
                                  CommitStore3A* store,
                                  cs::Delta* dOut, ChainError* eOut,
                                  const s16_ReplayOverlay* overlay = NULL)
{
    if (store == NULL) { cs::setErr(eOut, cs::ErrCode::NOT_OPEN, "store"); return false; }
    cs::ObservedBlock ob;
    ob.height = height;
    ob.txs.reserve(blk.vtx.size());
    for (size_t ti = 0; ti < blk.vtx.size(); ++ti) {
        const CTransaction& tx = blk.vtx[ti];
        cs::ObservedTx o;
        memcpy(o.txid.data(), tx.GetHash().begin(), 32);
        o.coinbase  = tx.IsCoinBase();
        o.coinstake = tx.IsCoinStake();
        o.nTime     = tx.nTime;
        for (size_t i = 0; i < tx.vin.size(); ++i) {
            const CTxIn& in = tx.vin[i];
            cs::OutPoint p;
            memcpy(p.txid.data(), in.prevout.hash.begin(), 32);
            p.n = in.prevout.n;
            o.vin.push_back(p);
        }
        o.vout.reserve(tx.vout.size());
        for (size_t j = 0; j < tx.vout.size(); ++j) {
            const CTxOut& tv = tx.vout[j];
            cs::TxOut to;
            to.value = tv.nValue;
            to.script.assign(tv.scriptPubKey.begin(), tv.scriptPubKey.end());
            to.unspendable = tv.scriptPubKey.IsUnspendable();
            o.vout.push_back(to);
        }
        ob.txs.push_back(o);
    }
    cs::GetPrevCoinFn gp = [store, overlay](const cs::OutPoint& k, cs::Coin* c)->bool {
        bool found = false; ChainError e;
        if (!store->get(k, c, &found, &e)) return false;
        if (found) return true;
        if (overlay == NULL) return false;
        uint256 id; memcpy(id.begin(), k.txid.data(), 32);
        s16_ReplayOverlay::const_iterator it = overlay->find(id);
        if (it == overlay->end()) return false;
        if (k.n >= it->second.tx.vout.size()) return false;
        const CTxOut& tv = it->second.tx.vout[k.n];
        c->value   = tv.nValue;
        c->script.assign(tv.scriptPubKey.begin(), tv.scriptPubKey.end());
        c->height  = it->second.height;
        c->nTime   = it->second.tx.nTime;
        c->fCoinBase  = it->second.tx.IsCoinBase();
        c->fCoinStake = it->second.tx.IsCoinStake();
        return true;
    };
    return cs::observeDelta(ob, gp, dOut, eOut);
}

int main(int argc, char** argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s <datadir> <outdir>\n"
                        "  <datadir> BI-V2 source (blockindex + blk*.dat), read-only\n"
                        "  <outdir>  fresh destination Store3A (chainstate) directory\n", argv[0]);
        return 2;
    }
    const std::string datadir = argv[1];
    const std::string outdir  = argv[2];
    double t0 = now_s();

    if (boost::filesystem::exists(outdir)) {
        fprintf(stderr, "ERROR: destination already exists: %s\n", outdir.c_str());
        return 3;
    }

    // Source datadir: OpenBlockFile() reads blk%04u.dat from GetDataDir(), so pass
    // it via mapArgs (ParseParameters of this tool's own argv is unreliable here).
    SetDataDirForTest(boost::filesystem::path(datadir));

    // ---- 1. open the BI-V2 generation selected by CURRENT (read-only) ----
    const std::string biRoot = (boost::filesystem::path(datadir) / "blockindex").string();
    BlockIndexCurrentRecord cur;
    {
        std::string err;
        const BlockIndexLifecycleStatus rc = BlockIndexGenerationManager::ReadCurrent(biRoot, &cur, &err);
        if (rc != BLOCK_INDEX_LIFECYCLE_OK) {
            fprintf(stderr, "ERROR BI-V2 CURRENT: %s (status=%d)\n", err.c_str(), (int)rc);
            return 4;
        }
    }
    const std::string genDir = BlockIndexGenerationManager::GenerationPath(biRoot, cur.generation);

    FixedBlockIndexStore bi;
    FixedBlockIndexOpenOptions oo;
    if (!FixedBlockIndexStore::OpenReadOnly(genDir, oo, &bi, NULL)) {
        fprintf(stderr, "ERROR open BI generation: %s\n", genDir.c_str());
        return 5;
    }
    const FixedBlockIndexManifest& man = bi.GetManifest();
    fprintf(stderr, "BI-V2: gen=%llu records=%llu tip h=%d %s\n",
            (unsigned long long)man.committedTipId,
            (unsigned long long)man.recordCount,
            (int)man.committedTipHeight, man.committedTipHash.ToString().c_str());

    // records.dat committed entry: 56-byte header + derived entry (LE)
    const unsigned BI_HDR = 56, DERIVED_HDR = 80, DERIVED_ENTRY = 56;
    std::vector<char> recs;
    std::string recPath = genDir + "/records.dat", actPath = genDir + "/active.dat", derPath = genDir + "/derived.dat";
    {
        FILE* rf = fopen(recPath.c_str(), "rb");
        if (!rf) { fprintf(stderr, "ERROR open records.dat\n"); return 6; }
        fseek(rf, 0, SEEK_END); long rb = ftell(rf); fseek(rf, 0, SEEK_SET);
        recs.resize(rb);
        if (fread(&recs[0], 1, rb, rf) != (size_t)rb) { fclose(rf); return 6; }
        fclose(rf);
    }
    FILE* af = fopen(actPath.c_str(), "rb");
    if (!af) { fprintf(stderr, "ERROR open active.dat\n"); return 6; }
    FILE* df = fopen(derPath.c_str(), "rb");
    if (!df) { fprintf(stderr, "ERROR open derived.dat\n"); return 6; }

    const uint64_t committed = man.recordCount;
    auto recEntry = [&](uint64_t i, BlockIndexRecord* out)->bool {
        const char* p = &recs[BI_HDR + i * BLOCK_INDEX_RECORD_SIZE_V1];
        std::vector<uint8_t> v(p, p + BLOCK_INDEX_RECORD_SIZE_V1);
        return DecodeBlockIndexRecordV1(v.size() ? &v[0] : 0, v.size(), out, NULL);
    };

    // ---- 2. destination store ----
    CommitStore3A store; ChainError ce;
    if (!store.open(outdir, &ce)) { fprintf(stderr, "ERROR store open: %s\n", ce.str().c_str()); return 7; }

    // ---- 3. replay ----
    const int32_t tipHeight = man.committedTipHeight;
    const size_t kBatch = 1000;
    std::vector<CommitStore3A::BatchBlock> batch;
    s16_ReplayOverlay overlay;             // same-window prevouts (first kBatch blocks)
    const size_t kOverlayLimit = 200000;   // bounded
    int32_t expectedH = 0;
    uint64_t totalOps = 0;
    struct rusage ruPeak; getrusage(RUSAGE_SELF, &ruPeak);
    double lastRep = t0;

    static const unsigned ACT_HDR = 40, ACT_ENT = 8;   // INNBACT1 header, per-height entry
    for (int32_t h = 0; h <= tipHeight; ++h) {
        // read the active entry for this height
        uint64_t id = 0;
        {
            unsigned char e[8];
            if (fseek(af, (long)(ACT_HDR + (long)h * ACT_ENT), SEEK_SET) != 0 ||
                fread(e, 1, 8, af) != 8) {
                fprintf(stderr, "FATAL height gap: no active entry at h=%d errno=%d\n", (int)h, errno);
                return 8;
            }
            uint64_t id64 = (uint64_t)e[0] | ((uint64_t)e[1] << 8) | ((uint64_t)e[2] << 16) | ((uint64_t)e[3] << 24) |
                            ((uint64_t)e[4] << 32) | ((uint64_t)e[5] << 40) | ((uint64_t)e[6] << 48) | ((uint64_t)e[7] << 56);
            id = id64;
        }
        if (id == 0 || id > committed) { fprintf(stderr, "FATAL active entry out of range at h=%d id=%llu genDir=%s P=%p\n", (int)h, (unsigned long long)id, genDir.c_str(), (void*)af); return 8; }
        // derived entry: nSize from bytes [44..48)
        uint32_t nSize = 0;
        {
            char d[4];
            if (fseek(df, (long)(DERIVED_HDR + (long)(id - 1) * DERIVED_ENTRY + 44), SEEK_SET) != 0 ||
                fread(d, 1, 4, df) != 4) { fprintf(stderr, "FATAL derived read at h=%d\n", (int)h); return 8; }
            nSize = (uint32_t)d[0] | ((uint32_t)d[1] << 8) | ((uint32_t)d[2] << 16) | ((uint32_t)d[3] << 24);
        }
        if (nSize == 0) { fprintf(stderr, "FATAL missing nSize at h=%d\n", (int)h); return 8; }
        BlockIndexRecord rr;
        if (!bi.Read((BlockIndexId)id, &rr, NULL)) { fprintf(stderr, "FATAL record decode at h=%d id=%llu\n", (int)h, (unsigned long long)id); return 9; }
        if (rr.height != h) { fprintf(stderr, "FATAL record height %d != h=%d\n", (int)rr.height, (int)h); return 8; }

        CBlock blk;
        if (!blk.ReadFromDisk(rr.nFile, rr.nBlockPos, true)) {
            fprintf(stderr, "FATAL block read failed at h=%d (file=%u pos=%u)\n", (int)h, rr.nFile, rr.nBlockPos);
            return 10;
        }
        if (blk.GetHash() != rr.hash) {
            fprintf(stderr, "FATAL hash mismatch at h=%d: block=%s index=%s\n",
                    (int)h, blk.GetHash().ToString().c_str(), rr.hash.ToString().c_str());
            return 11;
        }

        cs::Delta d; ChainError de;
        if (!ur_BuildDeltaForBlock(blk, h, &store, &d, &de, &overlay)) {
            fprintf(stderr, "FATAL delta build at h=%d: %s\n", (int)h, de.str().c_str());
            return 12;
        }
        for (size_t ti = 0; ti < blk.vtx.size(); ++ti) {
            s16_ReplayCoin rc; rc.tx = blk.vtx[ti]; rc.height = h;
            overlay[blk.vtx[ti].GetHash()] = rc;
        }
        if (overlay.size() > kOverlayLimit) overlay.clear();

        CommitStore3A::BatchBlock bb;
        bb.d = d;
        memcpy(bb.blockHash.data(), rr.hash.begin(), 32);
        bb.height = h;
        batch.push_back(bb);
        totalOps += d.ops.size();

        if (batch.size() >= kBatch) {
            ChainError pe;
            if (!store.publishBatch(batch, &pe)) {
                fprintf(stderr, "FATAL publishBatch at h=%d: %s\n", (int)h, pe.str().c_str());
                return 13;
            }
            batch.clear(); overlay.clear();
        }
        ++expectedH;
        if (h > 0 && h % 100000 == 0) {
            struct rusage ru; getrusage(RUSAGE_SELF, &ru);
            double now = now_s();
            fprintf(stderr, "[REPLAY] h=%d ops=%llu undoBytes=%llu rss_kb=%ld rate=%.0f blk/s next=%llu\n",
                    (int)h, (unsigned long long)totalOps,
                    (unsigned long long)store.undoCommittedBytes(),
                    ru.ru_maxrss, (double)h / (now - t0),
                    (unsigned long long)((now < lastRep + 1) ? 0 : 0));
            lastRep = now;
            if (ru.ru_maxrss > ruPeak.ru_maxrss) ruPeak = ru;
        }
    }
    if (!batch.empty()) {
        ChainError pe;
        if (!store.publishBatch(batch, &pe)) {
            fprintf(stderr, "FATAL final publishBatch: %s\n", pe.str().c_str());
            return 13;
        }
        batch.clear(); overlay.clear();
    }

    // ---- 4. final verification + summary ----
    // The writer still holds the O_EXCL LOCK; close it first so the fresh
    // read-side reopen below can acquire the store. Without this close the
    // post-replay self-verification always died with "ERROR reopen: LOCKED:
    // another writer holds LOCK" even though the full 0..tip replay had already
    // committed and verified on-disk. The on-disk tip/VERIFY is unchanged by
    // this fix (same store, reopened cleanly); it just stops the spurious exit-14.
    store.close();
    {
        CommitStore3A verify; ChainError ve;
        if (!verify.open(outdir, &ve)) { fprintf(stderr, "ERROR reopen: %s\n", ve.str().c_str()); return 14; }
        if (verify.tipHeight() != tipHeight) { fprintf(stderr, "FATAL tip height mismatch: %d != %d\n", (int)verify.tipHeight(), (int)tipHeight); return 15; }
        if (verify.tipHash().size() == 32 && man.committedTipHash != uint256(0)) {
            uint256 th; memcpy(th.begin(), verify.tipHash().data(), 32);
            if (th != man.committedTipHash) { fprintf(stderr, "FATAL tip hash mismatch\n"); return 15; }
        }
        bool va = verify.tree().verifyAll(&ve);
        fprintf(stderr, "[VERIFY] reopen_ok=1 tip=%d verifyAll=%s %s\n", (int)verify.tipHeight(), va?"PASS":"FAIL", ve.str().c_str());
        verify.close();
        if (!va) return 16;
    }

    double tEnd = now_s();
    struct rusage ru; getrusage(RUSAGE_SELF, &ru);
    if (ru.ru_maxrss >= ruPeak.ru_maxrss) ruPeak = ru;
    const unsigned long long undoBytes = (unsigned long long)store.undoCommittedBytes();
    const unsigned long long undoOps = (unsigned long long)store.undoCount();
    printf("SUMMARY blocks=%d undo_ops=%llu undo_bytes=%llu bytes_per_block=%.1f wall_s=%.1f peak_rss_kb=%ld\n",
           (int)tipHeight, undoOps, undoBytes,
           undoOps ? (double)undoBytes / (double)undoOps : 0.0,
           tEnd - t0, ruPeak.ru_maxrss);
    printf("SUMMARY2 expect_blocks=%d total_ops=%llu active_tip=%s\n", expectedH, totalOps, man.committedTipHash.ToString().c_str());
    return 0;
}
