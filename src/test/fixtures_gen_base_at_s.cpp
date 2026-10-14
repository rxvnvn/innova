// Base-generation-at-S driver (scale runs only; NOT a test).
//
// Reads the legacy snapshot through the PRODUCTION scanner
// (ReadLegacyBlockIndexSource), slices heights 0..S, then builds + publishes
// + selects the base generation via the production builder/lifecycle APIs.
//
// invocation: fixtures_gen_base <legacyDir> <rootDir> <S>
#include "blockindex_generation_builder.h"
#include "blockindex_generation_lifecycle.h"
#include "uint256.h"
#include "main.h"
#include "ui_interface.h"
#include "checkpoints.h"
#include "wallet.h"
#include <boost/filesystem.hpp>
#include <cstdio>
#include <vector>

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
    if (argc < 4) { fprintf(stderr, "usage: gen_base <legacyDir> <rootDir> <S>\n"); return 2; }
    const std::string legacyDir = argv[1];
    const std::string root = argv[2];
    const int S = atoi(argv[3]);

    BlockIndexGenerationSource full;
    std::string scanErr;
    if (!ReadLegacyBlockIndexSource(legacyDir, &full, &scanErr))
    { fprintf(stderr, "read source fail: %s\n", scanErr.c_str()); return 1; }
    fprintf(stderr, "dbg scanned=%zu foundBest=%d\n", full.records.size(), (int)full.foundBestChain);
    if (!full.foundBestChain) { fprintf(stderr, "no hashBestChain\n"); return 1; }

    std::vector<BlockIndexRecord> byHeight((size_t)S + 1);
    size_t got = 0;
    for (size_t i = 0; i < full.records.size(); ++i)
    {
        const int32_t h = full.records[i].record.height;
        if (h < 0 || h > S) continue;
        byHeight[(size_t)h] = full.records[i].record;
        ++got;
    }
    if (got != byHeight.size())
    { fprintf(stderr, "slice got %zu != %zu\n", got, byHeight.size()); return 1; }

    BlockIndexGenerationSource src;
    src.records.resize(byHeight.size());
    for (size_t i = 0; i < byHeight.size(); ++i)
    {
        src.records[i].hash = byHeight[i].hash;
        src.records[i].record = byHeight[i];
    }
    src.hashBestChain = byHeight.back().hash;
    src.foundBestChain = true;

    BlockIndexGenerationBuilder b;
    BlockIndexGenerationStats stats;
    std::string error;
    boost::filesystem::path staging = boost::filesystem::path(root) / "blockindex-build-000001.tmp";
    if (!b.Build(src, staging.string(), 1, &stats, &error))
    { fprintf(stderr, "build fail: %s\n", error.c_str()); return 1; }
    b.Close();
    if (BlockIndexGenerationManager::PublishGeneration(root, 1, &error) != BLOCK_INDEX_LIFECYCLE_OK)
    { fprintf(stderr, "publish fail: %s\n", error.c_str()); return 1; }
    if (BlockIndexGenerationManager::SelectGeneration(root, 1, &error) != BLOCK_INDEX_LIFECYCLE_OK)
    { fprintf(stderr, "select fail: %s\n", error.c_str()); return 1; }
    fprintf(stdout, "base generation S=%d published+selected tip=%s\n", S,
            src.hashBestChain.ToString().c_str());
    return 0;
}
