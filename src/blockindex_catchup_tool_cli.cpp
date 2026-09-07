// Standalone production G2 catch-up tool.
//
// Thin CLI wrapper around the committed, tested RunBlockIndexCatchup library
// (blockindex_catchup_tool.cpp), which bridges a validated immutable V2
// generation @ S + a consistent legacy LevelDB state @ L into a blockindex_tip
// containing S+1..L, so an authoritative restart lands at exact L.
//
// The library is the lineage-of-truth; this file provides the production
// executable entry point (the G2 runbook / Phase 7 driver) and reports the
// catch-up result machine-readably. No consensus logic is duplicated here.
//
// Usage:
//   blockindex_catchup_tool <v2root> <legacy_leveldb_copy> <block_data_dir> <livetail_horizon>
//
// <v2root>:            V2 lifecycle root containing the validated base generation
//                      (CURRENT selected) + the blockindex_tip destination.
// <legacy_leveldb_copy>: a CONSISTENT READ-ONLY COPY of the legacy LevelDB
//                      (blockindex + hashBestChain). NEVER the live datadir.
// <block_data_dir>:    directory containing blk%04u.dat for exact nSize (may be
//                      empty string; then nSize is marked unavailable).
// <livetail_horizon>:  residency horizon (residency/cache only, not consensus).
//
// Exit codes: 0 = success (final tip == L); non-zero = catch-up failed
// (fail-closed): the tip is left at its single-commit-point, re-runnable.
#include "blockindex_catchup_tool.h"
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
#include <string>

// Globals required to link the shared engine OBJS (mirrors the other standalone
// tool mains: builder / builder-lm / navdiff). Not used by this CLI but needed
// by the engine object graph it links.
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

int main(int argc, char** argv)
{
    if (argc != 5)
    {
        fprintf(stderr,
                "usage: %s <v2root> <legacy_leveldb_copy> <block_data_dir> <livetail_horizon>\n"
                "  v2root:            V2 lifecycle root (CURRENT-selected base generation)\n"
                "  legacy_leveldb_copy: consistent READ-ONLY copy of legacy LevelDB\n"
                "  block_data_dir:    dir with blk%%04u.dat (or empty)\n"
                "  livetail_horizon:  residency horizon (cache only)\n",
                argv[0]);
        return 2;
    }
    const std::string v2Root = argv[1];
    const std::string legacy = argv[2];
    const std::string blockData = argv[3];
    const int horizon = atoi(argv[4]);
    if (horizon <= 0)
    {
        fprintf(stderr, "error: livetail_horizon must be > 0\n");
        return 2;
    }

    BlockIndexCatchupResult res;
    if (!RunBlockIndexCatchup(v2Root, legacy, blockData, horizon, &res))
    {
        fprintf(stderr, "CATCHUP_FAIL error=%s\n", res.error.c_str());
        return 1;
    }

    // Emit the report via low-level write() to fd 1, bypassing any C stdio
    // buffering/reopen the engine may have imposed on the stdout stream.
    std::string report =
        "CATCHUP_OK base_tip=" + itostr(res.baseTipHeight) +
        " base_hash=" + res.baseTipHash.ToString() + "\n" +
        "          target(L)=" + itostr(res.targetHeight) +
        " target_hash=" + res.targetHash.ToString() + "\n" +
        "          appended=" + std::to_string(res.appendedRecords) +
        " final_tip=" + itostr(res.finalTipHeight) +
        " final_hash=" + res.finalTipHash.ToString() + "\n";
    ssize_t w = ::write(1, report.data(), report.size());
    (void)w;
    fsync(1);
    return 0;
}