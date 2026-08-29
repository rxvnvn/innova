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
#include "blockindex_shadow_runtime.h"
#include "blockindex_v2_reader.h"
#include "main.h"
#include "ui_interface.h"
#include "checkpoints.h"
#include "db.h"
#include "wallet.h"

#include <boost/filesystem.hpp>
#include <stdio.h>

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

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
    //   shadow   <snapshot_leveldb_dir> <lifecycle_root>
    //            (real-scale A.7 shadow open: legacy truth from the source
    //             snapshot, shadow from the CURRENT-selected generation)
    if (argc >= 3 && std::string(argv[1]) == "shadow")
    {
        const std::string snapshotDir = argv[2];
        const std::string root = argv[3];
        // Read the legacy truth.
        BlockIndexGenerationSource source;
        std::string err;
        if (!ReadLegacyBlockIndexSource(snapshotDir, &source, &err))
        {
            fprintf(stderr, "ERROR reading snapshot: %s\n", err.c_str());
            return 4;
        }
        // Legacy oracle seeded from the source (active chain by hashBestChain +
        // hashPrev, all records indexed by hash).
        struct SourceOracle : BlockIndexShadowLegacyOracle
        {
            BlockIndexGenerationSource src;
            std::vector<const BlockIndexGenerationSourceRecord*> active;
            std::map<uint256, const BlockIndexGenerationSourceRecord*> byHash;
            virtual int GetLegacyTipHeight() { return active.empty() ? -1 : active.back()->record.height; }
            virtual bool GetLegacyActiveRecord(int height, BlockIndexRecord* out)
            {
                if (height < 0 || height >= (int)active.size()) return false;
                *out = active[height]->record;
                return true;
            }
            virtual bool GetLegacyRecordByHash(const uint256& hash, BlockIndexRecord* out)
            {
                std::map<uint256, const BlockIndexGenerationSourceRecord*>::iterator it = byHash.find(hash);
                if (it == byHash.end()) return false;
                *out = it->second->record;
                return true;
            }
        } oracle;
        oracle.src = source;
        for (size_t i = 0; i < source.records.size(); ++i)
            oracle.byHash[source.records[i].hash] = &source.records[i];
        {
            std::vector<const BlockIndexGenerationSourceRecord*> byH;
            uint256 cur = source.hashBestChain;
            while (true)
            {
                std::map<uint256, const BlockIndexGenerationSourceRecord*>::iterator it = oracle.byHash.find(cur);
                if (it == oracle.byHash.end()) { fprintf(stderr, "ERROR: best-chain record missing\n"); return 4; }
                byH.push_back(it->second);
                if (it->second->record.hashPrev == 0) break;
                cur = it->second->record.hashPrev;
            }
            std::sort(byH.begin(), byH.end(),
                      [](const BlockIndexGenerationSourceRecord* a, const BlockIndexGenerationSourceRecord* b) {
                          return a->record.height < b->record.height;
                      });
            oracle.active = byH;
        }
        BlockIndexShadowOpenConfig cfg; cfg.root = root;
        BlockIndexV2ShadowRuntime rt;
        std::string e;
        const BlockIndexV2ShadowState& st = rt.OpenShadow(cfg, &oracle, &e);

        // A.8 real-scale differential: production pointer-free reader vs the
        // static A.5 legacy source. This is offline; no live datadir is read.
        uint64_t readerHash = 0, readerHeight = 0, readerParent = 0, readerAncestor = 0, readerSide = 0, readerMismatch = 0;
        BlockIndexV2Reader reader;
        BlockIndexV2ReaderOptions ro;
        std::string readerError;
        if (!reader.Open(root, ro, &readerError))
        {
            fprintf(stderr, "ERROR reader open: %s\n", readerError.c_str());
            return 5;
        }
        const auto same = [](const BlockIndexSnapshot& a, const BlockIndexRecord& b) {
            return a.found && a.hash == b.hash && a.hashPrev == b.hashPrev && a.height == b.height &&
                   a.nFile == b.nFile && a.nBlockPos == b.nBlockPos && a.nFlags == b.nFlags &&
                   a.nVersion == b.nVersion && a.nTime == b.nTime && a.nBits == b.nBits &&
                   a.nNonce == b.nNonce && a.nMint == b.nMint && a.nMoneySupply == b.nMoneySupply &&
                   a.nStakeModifier == b.nStakeModifier && a.prevoutStake == b.prevoutStake &&
                   a.nStakeTime == b.nStakeTime && a.hashProof == b.hashProof;
        };
        uint64_t rng = 0xA8D1FF5EEDULL;
        const auto next = [&rng]() { rng = rng * 6364136223846793005ULL + 1442695040888963407ULL; return rng; };
        for (uint64_t i = 0; i < 10000; ++i)
        {
            const BlockIndexGenerationSourceRecord& src = source.records[(size_t)(next() % source.records.size())];
            BlockIndexSnapshot got; readerError.clear();
            if (reader.LookupByHash(src.hash, &got, &readerError) != BLOCK_INDEX_V2_READ_FOUND || !same(got, src.record)) ++readerMismatch;
            ++readerHash;
        }
        for (uint64_t i = 0; i < 10000; ++i)
        {
            const int h = (int)(next() % oracle.active.size());
            const BlockIndexRecord& src = oracle.active[h]->record;
            BlockIndexSnapshot got; readerError.clear();
            if (reader.GetActiveByHeight(h, &got, &readerError) != BLOCK_INDEX_V2_READ_FOUND || !same(got, src)) ++readerMismatch;
            ++readerHeight;
        }
        for (uint64_t i = 0; i < 10000; ++i)
        {
            const BlockIndexGenerationSourceRecord& src = source.records[(size_t)(next() % source.records.size())];
            if (src.record.hashPrev == uint256(0)) continue;
            BlockIndexSnapshot child, parent; readerError.clear();
            if (reader.LookupByHash(src.hash, &child, &readerError) != BLOCK_INDEX_V2_READ_FOUND ||
                reader.GetParent(child.id, &parent, &readerError) != BLOCK_INDEX_V2_READ_FOUND || parent.hash != src.record.hashPrev) ++readerMismatch;
            ++readerParent;
        }
        const int distances[] = {1,10,100,1000,10000,100000};
        for (uint64_t i = 0; i < 10000; ++i)
        {
            const int h = (int)(next() % oracle.active.size());
            const int distance = distances[i % 6];
            const int target = h > distance ? h - distance : 0;
            const BlockIndexRecord& expected = oracle.active[target]->record;
            BlockIndexSnapshot tip, ancestor; readerError.clear();
            if (reader.GetActiveByHeight(h, &tip, &readerError) != BLOCK_INDEX_V2_READ_FOUND ||
                reader.GetAncestor(tip.id, target, &ancestor, &readerError) != BLOCK_INDEX_V2_READ_FOUND || !same(ancestor, expected)) ++readerMismatch;
            ++readerAncestor;
        }
        std::set<uint256> activeHashes; for (size_t i=0;i<oracle.active.size();++i) activeHashes.insert(oracle.active[i]->hash);
        for (size_t i = 0; i < source.records.size() && readerSide < 10000; ++i)
        {
            const BlockIndexGenerationSourceRecord& src = source.records[i];
            if (activeHashes.count(src.hash)) continue;
            BlockIndexSnapshot side, parent; readerError.clear();
            if (reader.LookupByHash(src.hash, &side, &readerError) != BLOCK_INDEX_V2_READ_FOUND || !same(side, src.record)) ++readerMismatch;
            else if (src.record.hashPrev != uint256(0) &&
                     (reader.GetParent(side.id, &parent, &readerError) != BLOCK_INDEX_V2_READ_FOUND || parent.hash != src.record.hashPrev)) ++readerMismatch;
            ++readerSide;
        }
        fprintf(stdout, "[reader] generation=%llu hashes=%llu heights=%llu parents=%llu ancestors=%llu side=%llu mismatches=%llu cache_entries=%llu cache_bytes=%llu\n",
                (unsigned long long)reader.Generation(), (unsigned long long)readerHash, (unsigned long long)readerHeight,
                (unsigned long long)readerParent, (unsigned long long)readerAncestor, (unsigned long long)readerSide,
                (unsigned long long)readerMismatch, (unsigned long long)reader.CacheStats().entries,
                (unsigned long long)reader.CacheStats().bytesEstimated);
        if (readerMismatch != 0) return 6;
        fprintf(stdout, "[shadow] enabled=%d status=%d generation=%llu\n",
                st.enabled, st.status, (unsigned long long)st.generation);
        fprintf(stdout, "[shadow] compatibility=%d tip_height=%d legacy_tip=%d lag=%d\n",
                st.compatibility, st.shadowTipHeight, st.legacyTipHeightAtValidation, st.heightLag);
        fprintf(stdout, "[shadow] structural=%d tip_on_active=%d hash_checks=%llu height_checks=%llu mismatches=%llu\n",
                st.structuralValidationOk ? 1 : 0, st.tipOnLegacyActiveChain ? 1 : 0,
                (unsigned long long)st.sampleHashChecks,
                (unsigned long long)st.sampleHeightChecks,
                (unsigned long long)st.sampleMismatches);
        fprintf(stdout, "[shadow] authoritative=%d\n", st.authoritative ? 1 : 0);
        fflush(stdout);
        return st.structuralValidationOk && st.tipOnLegacyActiveChain && st.sampleMismatches == 0 ? 0 : 1;
    }
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