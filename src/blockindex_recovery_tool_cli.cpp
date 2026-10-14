// Standalone bounded-memory block-index recovery tool (production CLI).
//
// Thin CLI wrapper around the committed, tested RunBlockIndexRecovery library
// (blockindex_recovery_tool.cpp), modeled on the existing
// blockindex_catchup_tool_cli.cpp / blockindex_generation_builder_tool.cpp
// patterns: same engine-globals linkage block, machine-readable stdout
// report, exact exit codes (0 ok / 1 recovery failed / 2 usage).
//
// Usage:
//   blockindex-recovery-tool <v2root> <legacy_leveldb_copy> <block_data_dir>
//                            [--abandon] [--report <path>]
//
//   <v2root>:             V2 lifecycle root containing the CURRENT-selected
//                         validated base generation (gen-N; S its tip) + the
//                         blockindex_tip destination. Recovery:
//                          - S == L:      nothing to do (NO-OP exit 0)
//                          - S < L:       rebuild S+1..L (full recovery)
//                          - S > L:       unsupported (legacy tip below base)
//   <legacy_leveldb_copy>: a CONSISTENT READ-ONLY COPY of the legacy
//                         LevelDB (blockindex + hashBestChain). NEVER the
//                         live datadir.
//   <block_data_dir>:      directory containing blk%04u.dat for exact nSize
//                         (may be empty string; then nSize unavailable).
//
// Exit codes:
//   0 = recovery promoted (tip == L; recovery.meta ACCEPTED)
//   1 = recovery failed (fail-closed; tip left at the last committed
//       batch, idempotent; --abandon tears down and preserves rollback
//       evidence at blockindex_tip.pre-recovery-N/)
//   2 = usage / precondition error
//
// NO consensus change; NO master modification; DO NOT run against a real
// datadir (the caller supplies the immutable COPY; the tool opens it
// read-only and fails closed on a held LevelDB LOCK).
#include "blockindex_recovery_tool.h"
#include "blockindex_generation_lifecycle.h"
#include "uint256.h"
#include "main.h"
#include "util.h"
#include "ui_interface.h"
#include "checkpoints.h"
#include "wallet.h"

#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

// Globals required to link the shared engine OBJS (mirrors the other
// standalone tool mains: builder / builder-lm / navdiff / catchup). Not used
// by this CLI but needed by the engine object graph it links.
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

static void PrintUsage(const char* argv0)
{
    fprintf(stderr,
            "usage: %s <v2root> <legacy_leveldb_copy> <block_data_dir> [--abandon] [--report <path>]\n"
            "  v2root:             V2 lifecycle root (CURRENT-selected base generation)\n"
            "  legacy_leveldb_copy: consistent READ-ONLY copy of legacy LevelDB\n"
            "  block_data_dir:     dir with blk%%04u.dat (or empty \"\")\n"
            "  --abandon:          discard partial recovery (preserve prior tip as\n"
            "                      blockindex_tip.pre-recovery-N/, rollback evidence)\n"
            "  --report <path>:    additionally write a machine-readable report there\n",
            argv0);
}

// Emit the report via low-level write() to fd 1, bypassing any C stdio
// buffering/reopen the engine may have imposed on the stdout stream
// (the machine-readable contract the other tool CLIs follow).
static void EmitReport(const BlockIndexRecoveryStats& res)
{
    std::string report =
        std::string("RECOVERY_") + (res.ok ? "OK" : "FAIL") +
        " base_tip=" + itostr(res.baseTipHeight) +
        " base_hash=" + res.baseTipHash.ToString() + "\n";
    report +=
        "        target(L)=" + itostr(res.targetHeight) +
        " target_hash=" + res.targetHash.ToString() + "\n";
    report +=
        "        appended=" + std::to_string(res.recordsAppended) +
        " resumed=" + std::to_string(res.recordsResumed) +
        " batchLeaves=" + std::to_string(res.batch) + "\n";
    report +=
        "        peak_rss_kib=" + std::to_string(res.peakRssBytes / 1024) +
        " ring_cap=" + std::to_string(res.ringCap) +
        " lru_cap_bytes=" + std::to_string(res.lruBytesCap) +
        " final_chainTrust=" + res.lastChainTrust.ToString() + "\n";
    report +=
        "        disk_peak_bytes=" + std::to_string(res.diskBytesPeak > 0 ? (long long)res.diskBytesPeak : 0) +
        " disk_final_bytes=" + std::to_string((long long)res.diskBytesFinal) + "\n";
    if (!res.ok)
        report += "        error=" + res.error + "\n";
    ssize_t w = ::write(1, report.data(), report.size());
    (void)w;
    fsync(1);
}

static bool WriteReportFile(const std::string& path,
                            const BlockIndexRecoveryStats& res)
{
    FILE* rep = fopen(path.c_str(), "w");
    if (!rep) return false;
    fprintf(rep, "ok=%d\n", res.ok ? 1 : 0);
    fprintf(rep, "error=%s\n", res.error.c_str());
    fprintf(rep, "base_tip_height=%d\n", res.baseTipHeight);
    fprintf(rep, "base_tip_hash=%s\n", res.baseTipHash.ToString().c_str());
    fprintf(rep, "target_height=%d\n", res.targetHeight);
    fprintf(rep, "target_hash=%s\n", res.targetHash.ToString().c_str());
    fprintf(rep, "legacy_records_total=%llu\n", (unsigned long long)res.legacyRecordsTotal);
    fprintf(rep, "records_appended=%llu\n", (unsigned long long)res.recordsAppended);
    fprintf(rep, "records_resumed=%llu\n", (unsigned long long)res.recordsResumed);
    fprintf(rep, "batch_leaves=%u\n", res.batch);
    fprintf(rep, "full_batches=%u\n", res.fullBatches);
    fprintf(rep, "last_chain_trust=%s\n", res.lastChainTrust.ToString().c_str());
    fprintf(rep, "peak_rss_bytes=%lld\n", (long long)res.peakRssBytes);
    fprintf(rep, "ring_resident_max=%llu\n", (unsigned long long)res.ringResidentMax);
    fprintf(rep, "ring_cap=%llu\n", (unsigned long long)res.ringCap);
    fprintf(rep, "lru_bytes_cap=%llu\n", (unsigned long long)res.lruBytesCap);
    fprintf(rep, "derived_ring_bytes_cap=%llu\n", (unsigned long long)res.derivedRingBytesCap);
    fprintf(rep, "disk_peak_bytes=%lld\n", (long long)res.diskBytesPeak);
    fprintf(rep, "disk_final_bytes=%lld\n", (long long)res.diskBytesFinal);
    fclose(rep);
    return true;
}

int main(int argc, char** argv)
{
    if (argc < 4)
    {
        PrintUsage(argc > 0 ? argv[0] : "blockindex-recovery-tool");
        return 2;
    }
    const std::string v2Root = argv[1];
    const std::string legacy = argv[2];
    const std::string blockData = argv[3];
    bool abandon = false;
    std::string reportPath;
    for (int i = 4; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--abandon") abandon = true;
        else if (a == "--report" && i + 1 < argc) reportPath = argv[++i];
        else { PrintUsage(argv[0]); return 2; }
    }
    if (v2Root.empty() || legacy.empty())
    {
        fprintf(stderr, "error: v2root and legacy_leveldb_copy must be non-empty\n");
        return 2;
    }

    BlockIndexRecoveryStats res;
    if (!RunBlockIndexRecovery(v2Root, legacy, blockData, abandon, &res))
    {
        fprintf(stderr, "RECOVERY_FAIL error=%s\n", res.error.c_str());
        if (!reportPath.empty())
            WriteReportFile(reportPath, res);
        EmitReport(res);
        return 1;
    }

    if (!reportPath.empty())
        WriteReportFile(reportPath, res);
    EmitReport(res);
    return 0;
}
