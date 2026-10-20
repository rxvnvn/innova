// probe_verify.cpp: read-only verification of a completed Store3A chainstate.
// usage: innova-probe-verify <storedir>
// Prints tip height/hash, committed undo size/count, and verifyAll result.
// Mirrors the final-verify block of undo_replay_tool.cpp (step 4) but opens
// READ-only after the replay tool has fully exited (so no writer lock is held).
#include "main.h"
#include "ui_interface.h"
#include "checkpoints.h"
#include "util.h"
#include "chainstate/store3a/incstore.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

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

int main(int argc, char** argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <storedir>\n", argv[0]); return 2; }
    fprintf(stderr, "PROBE starting open of %s\n", argv[1]); fflush(stderr);
    CommitStore3A store; ChainError se;
    if (!store.open(argv[1], &se)) { fprintf(stderr, "ERROR open: %s\n", se.str().c_str()); return 1; }
    fprintf(stderr, "PROBE open ok\n"); fflush(stderr);
    uint256 t; memcpy(t.begin(), store.tipHash().data(), 32);
    fprintf(stderr, "PROBE tipHeight=%d tipHash=%s\n",
           (int)store.tipHeight(), t.ToString().c_str());
    fprintf(stderr, "PROBE undoCount=%zu undoCommittedBytes=%llu\n",
           store.undoCount(), (unsigned long long)store.undoCommittedBytes());
    ChainError ve;
    bool ok = store.tree().verifyAll(&ve);
    fprintf(stderr, "PROBE verifyAll=%s %s\n", ok ? "PASS" : "FAIL", ve.str().c_str());
    {
        std::vector<cs::store3a::LeafEntry> all; ChainError ce;
        if (!store.tree().collectAll(&all, &ce)) { fprintf(stderr, "PROBE collectAll FAIL %s\n", ce.str().c_str()); }
        else { fprintf(stderr, "PROBE leafEntries=%zu\n", all.size()); }
    }
    fprintf(stderr, "PROBE tree nextPageId=%u freeListSize=%zu pageReads=%llu pageWrites=%llu\n",
            store.tree().nextPageId(), store.tree().freeListSize(),
            (unsigned long long)store.tree().pageReads(), (unsigned long long)store.tree().pageWrites());
    uint64_t nOp = store.undoCount();
    uint64_t uB = store.undoCommittedBytes();
    fprintf(stderr, "PROBE_EVAL blocks=%d undo_ops=%llu undo_bytes=%llu bytes_per_block=%.1f\n",
           (int)store.tipHeight(), (unsigned long long)nOp, (unsigned long long)uB,
           nOp ? (double)uB / (double)nOp : 0.0);
    store.close();
    return ok ? 0 : 3;
}