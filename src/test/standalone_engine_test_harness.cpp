// Generic standalone-engine test link harness.
// Definitions intentionally match the established standalone DAG SourceState
// target (dag_sourcestate_bootstrap_tests.cpp), without a Boost test module or
// fixture. It supplies only process globals required by shared engine OBJS.
#include "../checkpoints.h"
#include "../ui_interface.h"
#include "../wallet.h"

#include <cstdlib>

CWallet* pwalletMain = NULL;
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
