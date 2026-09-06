// Low-memory V1->V2 builder standalone tool.
// Usage: innova-blockindex-builder-lm <snapshot_leveldb_dir> <out_staging_root> <generation_id> [--lifecycle-root <root>]
#include "blockindex_generation_builder_lm.h"
#include "blockindex_generation_writer.h"
#include "main.h"
#include "ui_interface.h"
#include "checkpoints.h"
#include "wallet.h"
#include "util.h"

#include <cstdio>
#include <cstdlib>
#include <string>

// Globals required to link the shared engine OBJS (mirrors builder_tool.cpp).
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

int main(int argc, char** argv)
{
    if (argc < 4)
    {
        fprintf(stderr, "usage: %s <snapshot_leveldb_dir> <out_staging_root> <generation_id> [--lifecycle-root <root>] [--block-data-dir <dir with blk*.dat>]\n", argv[0]);
        return 2;
    }
    std::string snapshotDir = argv[1];
    std::string stagingRoot = argv[2];
    uint64_t generation = (uint64_t)atoll(argv[3]);
    std::string lifecycleRoot;
    std::string blockDataDir;
    for (int i = 4; i < argc; ++i)
    {
        if (std::string(argv[i]) == "--lifecycle-root" && i + 1 < argc)
            lifecycleRoot = argv[++i];
        else if (std::string(argv[i]) == "--block-data-dir" && i + 1 < argc)
            blockDataDir = argv[++i];
    }
    if (lifecycleRoot.empty())
    {
        // lifecycle root = parent of stagingRoot (so build-<gen>.tmp -> gen-<gen>)
        lifecycleRoot = stagingRoot;
    }

    const std::string staging = lifecycleRoot + "/build-" + strprintf("%06llu", (unsigned long long)generation) + ".tmp";
    fprintf(stderr, "lm-builder: snapshot=%s staging=%s gen=%llu blockdata=%s\n",
            snapshotDir.c_str(), staging.c_str(), (unsigned long long)generation,
            blockDataDir.empty() ? "(none)" : blockDataDir.c_str());

    BlockIndexGenerationBuilderLM lm;
    std::string error;
    bool ok = lm.Build(snapshotDir, blockDataDir, generation, staging, &error);
    if (!ok)
    {
        fprintf(stderr, "lm-builder FAILED: %s\n", error.c_str());
        return 1;
    }
    fprintf(stderr, "lm-builder: staging complete (%zu bytes records). Publishing...\n");
    // Publish/select under lifecycleRoot (crash-safe CURRENT flip).
    // Use rm to clear a stale build-<gen>.tmp is handled by OpenTarget.
    if (!lifecycleRoot.empty())
    {
        std::string perr;
        ok = BlockIndexGenerationWriter::ValidatePublishSelect(lifecycleRoot, generation, &perr);
        if (!ok)
        {
            fprintf(stderr, "lm-builder publish FAILED: %s\n", perr.c_str());
            return 1;
        }
        fprintf(stderr, "lm-builder: published gen-%06llu (CURRENT selected)\n",
                (unsigned long long)generation);
    }
    fprintf(stderr, "lm-builder: SUCCESS\n");
    return 0;
}