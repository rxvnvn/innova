
// Empty base generation driver (scale runs only): builds + publishes + selects
// a ZERO-RECORD base generation via the production builder/lifecycle APIs.
#include "blockindex_generation_builder.h"
#include "blockindex_generation_lifecycle.h"
#include "uint256.h"
#include "main.h"
#include "ui_interface.h"
#include "checkpoints.h"
#include "wallet.h"
#include <boost/filesystem.hpp>
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
    if (argc < 2) { fprintf(stderr, "usage: gen_empty <rootDir>\n"); return 2; }
    std::string root = argv[1];
    BlockIndexGenerationSource src; // empty: no records, foundBestChain=false
    BlockIndexGenerationBuilder b;
    BlockIndexGenerationStats stats;
    std::string error;
    boost::filesystem::path staging = boost::filesystem::path(root) / "build-000001.tmp";
    if (!b.Build(src, staging.string(), 1, &stats, &error))
    { fprintf(stderr, "build fail: %s\n", error.c_str()); return 1; }
    b.Close();
    if (BlockIndexGenerationManager::PublishGeneration(root, 1, &error) != BLOCK_INDEX_LIFECYCLE_OK)
    { fprintf(stderr, "publish fail: %s\n", error.c_str()); return 1; }
    if (BlockIndexGenerationManager::SelectGeneration(root, 1, &error) != BLOCK_INDEX_LIFECYCLE_OK)
    { fprintf(stderr, "select fail: %s\n", error.c_str()); return 1; }
    fprintf(stdout, "empty generation published+selected\n");
    return 0;
}
