// Offline Innova Block Index V2 generation builder + massive differential.
//
// Dedicated standalone tool. It does NOT initialize the wallet, P2P, RPC, CN,
// staking, mempool, or networking. It reads a STATIC SNAPSHOT of the legacy
// block-index LevelDB and writes an isolated COMPLETE generation, then reopens
// it and runs the massive differential against the source.
//
// Usage:
//   innova-blockindex-builder <snapshot_leveldb_dir> <out_generation_dir> <generation_id> [verify-only]
// With the optional 4th arg "verify-only", the build is skipped and only the
// massive differential + reopen against the existing <out_generation_dir> is run.
#include "blockindex_generation_builder.h"
#include "blockindex_generation_lifecycle.h"
#include "main.h"
#include "ui_interface.h"
#include "checkpoints.h"
#include "db.h"
#include "wallet.h"

#include <boost/filesystem.hpp>
#include <stdio.h>

// Globals required to link the shared engine OBJS (mirrors test_innova.cpp).
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

static void PrintStats(const BlockIndexGenerationStats& s)
{
    printf("[GENERATION-STATS]\n");
    printf("total_records=%llu\n", (unsigned long long)s.totalRecords);
    printf("active_records=%llu\n", (unsigned long long)s.activeRecords);
    printf("side_chain_records=%llu\n", (unsigned long long)s.sideChainRecords);
    printf("active_tip_height=%d\n", s.activeTipHeight);
    printf("active_tip_hash=%s\n", s.activeTipHash.ToString().c_str());
}

static void PrintDifferential(const BlockIndexDifferentialResult& r)
{
    printf("[DIFFERENTIAL]\n");
    printf("hash_queries=%llu\n", (unsigned long long)r.hashQueries);
    printf("hash_mismatches=%llu\n", (unsigned long long)r.hashMismatches);
    printf("hash_corruptions=%llu\n", (unsigned long long)r.hashCorruptions);
    printf("hash_not_found=%llu\n", (unsigned long long)r.hashNotFound);
    printf("height_queries=%llu\n", (unsigned long long)r.heightQueries);
    printf("height_mismatches=%llu\n", (unsigned long long)r.heightMismatches);
    printf("height_corruptions=%llu\n", (unsigned long long)r.heightCorruptions);
    printf("height_not_found=%llu\n", (unsigned long long)r.heightNotFound);
    printf("parent_checks=%llu\n", (unsigned long long)r.parentChecks);
    printf("parent_mismatches=%llu\n", (unsigned long long)r.parentMismatches);
    printf("side_chain_samples=%llu\n", (unsigned long long)r.sideChainSamples);
    printf("side_chain_mismatches=%llu\n", (unsigned long long)r.sideChainMismatches);
    printf("random_hash_checks=%llu\n", (unsigned long long)r.randomHashChecks);
    printf("random_height_checks=%llu\n", (unsigned long long)r.randomHeightChecks);
    printf("random_mismatches=%llu\n", (unsigned long long)r.randomMismatches);
    printf("tip_coherent=%s\n", r.tipCoherent ? "YES" : "NO");
    if (!r.tipCoherent)
        printf("tip_detail=%s\n", r.tipDetail.c_str());
}

int main(int argc, char** argv)
{
    // Lifecycle subcommands (see blockindex_generation_lifecycle.h).
    //   validate <root> <generation>
    //   publish  <root> <generation>
    //   select   <root> <generation>
    //   open     <root>          (OpenCurrent: resolve + validate selected)
    //   current  <root>          (ReadCurrent only)
    if (argc >= 2 && std::string(argv[1]) == "open")
    {
        if (argc < 3)
        {
            fprintf(stderr, "usage: %s open <root>\n", argv[0]);
            return 2;
        }
        const std::string root = argv[2];
        std::string err;
        uint64_t sel = 0;
        BlockIndexLifecycleStatus st = BlockIndexGenerationManager::OpenCurrent(root, &sel, &err);
        fprintf(stdout, "[open] status=%d generation=%llu\n", (int)st, (unsigned long long)sel);
        fflush(stdout);
        return st == BLOCK_INDEX_LIFECYCLE_OK ? 0 : 1;
    }
    if (argc >= 2 && (std::string(argv[1]) == "validate" ||
                      std::string(argv[1]) == "publish" ||
                      std::string(argv[1]) == "select"))
    {
        if (argc < 4)
        {
            fprintf(stderr, "usage: %s %s <root> <generation>\n", argv[0], argv[1]);
            return 2;
        }
        const std::string root = argv[2];
        const uint64_t gen = (uint64_t)atoll(argv[3]);
        std::string err;
        BlockIndexLifecycleStatus st = BLOCK_INDEX_LIFECYCLE_ERROR;
        if (std::string(argv[1]) == "validate")
            st = BlockIndexGenerationManager::ValidateGeneration(root, gen, &err);
        else if (std::string(argv[1]) == "publish")
            st = BlockIndexGenerationManager::PublishGeneration(root, gen, &err);
        else
            st = BlockIndexGenerationManager::SelectGeneration(root, gen, &err);
        fprintf(stdout, "[%s] generation=%llu status=%d %s\n", argv[1], (unsigned long long)gen,
                (int)st, st == BLOCK_INDEX_LIFECYCLE_OK ? "OK" : err.c_str());
        fflush(stdout);
        return st == BLOCK_INDEX_LIFECYCLE_OK ? 0 : 1;
    }
    if (argc >= 2 && std::string(argv[1]) == "current")
    {
        if (argc < 3)
        {
            fprintf(stderr, "usage: %s current <root>\n", argv[0]);
            return 2;
        }
        const std::string root = argv[2];
        std::string err;
        BlockIndexCurrentRecord cur;
        BlockIndexLifecycleStatus st = BlockIndexGenerationManager::ReadCurrent(root, &cur, &err);
        fprintf(stdout, "[current] status=%d", (int)st);
        if (st == BLOCK_INDEX_LIFECYCLE_OK)
            fprintf(stdout, " generation=%llu", (unsigned long long)cur.generation);
        if (st != BLOCK_INDEX_LIFECYCLE_OK && !err.empty())
            fprintf(stdout, " %s", err.c_str());
        fprintf(stdout, "\n");
        fflush(stdout);
        return st == BLOCK_INDEX_LIFECYCLE_OK ? 0 : 1;
    }

    if (argc < 4)
    {
        fprintf(stderr, "usage: %s <snapshot_leveldb_dir> <out_generation_dir> <generation_id> [verify-only]\n", argv[0]);
        return 2;
    }
    const std::string snapshotDir = argv[1];
    const std::string outDir = argv[2];
    const uint64_t generation = (uint64_t)atoll(argv[3]);
    const bool verifyOnly = (argc >= 5 && std::string(argv[4]) == "verify-only");

    // 1) read source
    BlockIndexGenerationSource source;
    std::string error;
    if (!ReadLegacyBlockIndexSource(snapshotDir, &source, &error))
    {
        fprintf(stderr, "ERROR reading snapshot: %s\n", error.c_str());
        return 4;
    }
    printf("[SOURCE] records=%llu found_best=%s best=%s\n",
           (unsigned long long)source.records.size(),
           source.foundBestChain ? "YES" : "NO",
           source.hashBestChain.ToString().c_str());
    fflush(stdout);

    // 2) build (skip in verify-only mode)
    BlockIndexGenerationStats stats;
    if (!verifyOnly)
    {
        if (boost::filesystem::exists(outDir))
        {
            fprintf(stderr, "ERROR: destination already exists: %s\n", outDir.c_str());
            return 3;
        }
        BlockIndexGenerationBuilder builder;
        if (!builder.Build(source, outDir, generation, &stats, &error))
        {
            fprintf(stderr, "ERROR building generation: %s\n", error.c_str());
            return 5;
        }
        builder.Close();
        PrintStats(stats);
    }
    else
    {
        printf("[VERIFY-ONLY] reusing existing generation at %s\n", outDir.c_str());
    }
    fflush(stdout);

    // 3) reopen + massive differential
    BlockIndexDifferentialResult diff;
    if (!VerifyGenerationAgainstSource(source, outDir, generation, &diff, &error))
    {
        fprintf(stderr, "ERROR verifying generation: %s\n", error.c_str());
        return 6;
    }
    PrintDifferential(diff);
    fflush(stdout);

    // 4) verdict
    bool success = (diff.hashMismatches == 0 && diff.hashCorruptions == 0 &&
                    diff.hashNotFound == 0 && diff.heightMismatches == 0 &&
                    diff.heightCorruptions == 0 && diff.heightNotFound == 0 &&
                    diff.parentMismatches == 0 && diff.sideChainMismatches == 0 &&
                    diff.randomMismatches == 0 && diff.tipCoherent);
    printf("[VERDICT] %s\n", success ? "BLOCK INDEX V2 PHASE A.5 - FULL REAL-CHAIN SHADOW GENERATION VERIFIED"
                                     : "VERIFICATION FAILED");
    fflush(stdout);

    // Persist a machine-readable result report alongside the generation so the
    // outcome survives any shell/stdio buffering loss.
    {
        FILE* rep = fopen((boost::filesystem::path(outDir) / "A5_DIFFERENTIAL.txt").string().c_str(), "w");
        if (rep)
        {
            fprintf(rep, "total_records=%llu\n", (unsigned long long)(verifyOnly ? 0 : stats.totalRecords));
            fprintf(rep, "active_tip_height=%d\n", verifyOnly ? 0 : stats.activeTipHeight);
            fprintf(rep, "hash_queries=%llu\n", (unsigned long long)diff.hashQueries);
            fprintf(rep, "hash_mismatches=%llu\n", (unsigned long long)diff.hashMismatches);
            fprintf(rep, "hash_corruptions=%llu\n", (unsigned long long)diff.hashCorruptions);
            fprintf(rep, "hash_not_found=%llu\n", (unsigned long long)diff.hashNotFound);
            fprintf(rep, "height_queries=%llu\n", (unsigned long long)diff.heightQueries);
            fprintf(rep, "height_mismatches=%llu\n", (unsigned long long)diff.heightMismatches);
            fprintf(rep, "height_corruptions=%llu\n", (unsigned long long)diff.heightCorruptions);
            fprintf(rep, "height_not_found=%llu\n", (unsigned long long)diff.heightNotFound);
            fprintf(rep, "parent_checks=%llu\n", (unsigned long long)diff.parentChecks);
            fprintf(rep, "parent_mismatches=%llu\n", (unsigned long long)diff.parentMismatches);
            fprintf(rep, "side_chain_samples=%llu\n", (unsigned long long)diff.sideChainSamples);
            fprintf(rep, "side_chain_mismatches=%llu\n", (unsigned long long)diff.sideChainMismatches);
            fprintf(rep, "random_hash_checks=%llu\n", (unsigned long long)diff.randomHashChecks);
            fprintf(rep, "random_height_checks=%llu\n", (unsigned long long)diff.randomHeightChecks);
            fprintf(rep, "random_mismatches=%llu\n", (unsigned long long)diff.randomMismatches);
            fprintf(rep, "tip_coherent=%s\n", diff.tipCoherent ? "YES" : "NO");
            fprintf(rep, "verdict=%s\n", success ? "VERIFIED" : "FAILED");
            fclose(rep);
        }
    }
    fflush(stdout);
    return success ? 0 : 7;
}