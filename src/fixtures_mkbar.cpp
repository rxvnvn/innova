// Scale-run helper (NOT a test): make a bare (no tip) pristine base tree for
// the 180k spill-path RSS measurement. Calls BlockIndexTipAuthority::Create
// with the CURRENT-selected generation's (baseRecordCount, S) and returns.
#include "blockindex_tip.h"
#include "blockindex_v2_reader.h"
#include "uint256.h"
#include "main.h"
#include "ui_interface.h"
#include "checkpoints.h"
#include "wallet.h"
#include <cstdio>

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
    if (argc < 2) { fprintf(stderr, "usage: mkbar <v2root>\n"); return 2; }
    const std::string root = argv[1];
    BlockIndexV2Reader reader;
    BlockIndexV2ReaderOptions opts;
    std::string err;
    if (!reader.Open(root, opts, &err))
    { fprintf(stderr, "open fail: %s\n", err.c_str()); return 1; }
    const BlockIndexSnapshot tip = reader.GetTip();
    if (!tip.found) { fprintf(stderr, "no tip\n"); return 1; }
    BlockIndexTipAuthority tipAuth;
    if (!BlockIndexTipAuthority::Create(root, reader.Generation(),
                                        reader.RecordCount(), tip.height,
                                        &tipAuth, &err))
    { fprintf(stderr, "create fail: %s\n", err.c_str()); return 1; }
    tipAuth.Close();
    fprintf(stdout, "bare tip created S=%d baseRec=%llu\n", tip.height,
            (unsigned long long)reader.RecordCount());
    return 0;
}
