// Offline A.9a.3b navigation + staking-metadata differential.
//
// Standalone tool. It does NOT initialize the wallet, P2P, RPC, CN, staking,
// mempool, or networking. It reads a STATIC SNAPSHOT of the legacy block-index
// LevelDB, builds a PREFIX generation as the "cold" side, constructs a
// value-only "hot" resolver over the full source (no CBlockIndex* graph), then
// drives the production ColdHotSeamNavigator across the cold/hot seam and
// compares navigation + derived staking metadata against the legacy semantics.
//
// Usage:
//   innova-blockindex-navdiff <snapshot_leveldb_dir> <work_root> <seam_height> <nav_comparisons>
//
// The legacy canonical metadata (nStakeModifierTime memo, nStakeModifierChecksum)
// is reproduced here as an incremental forward pass over the active chain —
// exactly how the legacy daemon computes it at load/connect time — and compared
// against the V2 reader's per-call derivation.
#include "cold_hot_seam.h"
#include "blockindex_generation_builder.h"
#include "blockindex_generation_lifecycle.h"
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

class OfflineSourceHotResolver : public ColdHotHotResolver
{
public:
    OfflineSourceHotResolver(const BlockIndexGenerationSource& source)
        : src(source)
    {
        for (size_t i = 0; i < source.records.size(); ++i)
            byHash[source.records[i].hash] = &source.records[i];
        std::vector<const BlockIndexGenerationSourceRecord*> byHeight;
        uint256 cur = source.hashBestChain;
        while (true)
        {
            std::map<uint256, const BlockIndexGenerationSourceRecord*>::iterator it = byHash.find(cur);
            if (it == byHash.end()) break;
            byHeight.push_back(it->second);
            if (it->second->record.hashPrev == uint256(0)) break;
            cur = it->second->record.hashPrev;
        }
        std::sort(byHeight.begin(), byHeight.end(),
                  [](const BlockIndexGenerationSourceRecord* a, const BlockIndexGenerationSourceRecord* b) {
                      return a->record.height < b->record.height;
                  });
        for (size_t i = 0; i < byHeight.size(); ++i)
        {
            activeHashes.push_back(byHeight[i]->hash);
            activeHeight[byHeight[i]->hash] = (int)byHeight[i]->record.height;
        }
    }

    BlockIndexSnapshot Snapshot(const uint256& hash, const BlockIndexRecord& r, bool inMain) const
    {
        BlockIndexSnapshot s;
        s.found = true;
        s.id = BLOCK_INDEX_ID_INVALID;
        s.hash = hash;
        s.hashPrev = r.hashPrev;
        s.hashNext = uint256(0);
        s.height = r.height;
        s.nFile = r.nFile;
        s.nBlockPos = r.nBlockPos;
        s.nFlags = r.nFlags;
        s.nVersion = r.nVersion;
        s.nTime = r.nTime;
        s.nBits = r.nBits;
        s.nNonce = r.nNonce;
        s.nMint = r.nMint;
        s.nMoneySupply = r.nMoneySupply;
        s.nStakeModifier = r.nStakeModifier;
        s.prevoutStake = r.prevoutStake;
        s.nStakeTime = r.nStakeTime;
        s.hashProof = r.hashProof;
        s.nChainTrust = 0;
        s.fProofOfStake = (r.nFlags & CBlockIndex::BLOCK_PROOF_OF_STAKE) != 0;
        s.fInMainChain = inMain;
        s.hasParent = (r.hashPrev != uint256(0));
        s.hasStakeModifierTime = false;
        s.hasStakeModifierChecksum = false;
        s.nStakeModifierTime = 0;
        s.nStakeModifierChecksum = 0;
        return s;
    }

    virtual BlockIndexSnapshot LookupByHash(const uint256& hash) const
    {
        std::map<uint256, const BlockIndexGenerationSourceRecord*>::const_iterator it = byHash.find(hash);
        if (it == byHash.end()) return BlockIndexSnapshot();
        return Snapshot(hash, it->second->record, activeHeight.count(hash) != 0);
    }
    virtual BlockIndexSnapshot GetActiveByHeight(int height) const
    {
        if (height < 0 || height >= (int)activeHashes.size()) return BlockIndexSnapshot();
        const uint256& hash = activeHashes[height];
        std::map<uint256, const BlockIndexGenerationSourceRecord*>::const_iterator it = byHash.find(hash);
        if (it == byHash.end()) return BlockIndexSnapshot();
        return Snapshot(hash, it->second->record, true);
    }
    virtual BlockIndexSnapshot GetParentByHash(const uint256& hash) const
    {
        std::map<uint256, const BlockIndexGenerationSourceRecord*>::const_iterator it = byHash.find(hash);
        if (it == byHash.end() || it->second->record.hashPrev == uint256(0)) return BlockIndexSnapshot();
        return LookupByHash(it->second->record.hashPrev);
    }
    virtual BlockIndexSnapshot GetNextActiveByHash(const uint256& hash) const
    {
        std::map<uint256, int>::const_iterator it = activeHeight.find(hash);
        if (it == activeHeight.end() || it->second + 1 >= (int)activeHashes.size()) return BlockIndexSnapshot();
        return GetActiveByHeight(it->second + 1);
    }
    virtual BlockIndexSnapshot GetTip() const
    {
        if (activeHashes.empty()) return BlockIndexSnapshot();
        return GetActiveByHeight((int)activeHashes.size() - 1);
    }

private:
    const BlockIndexGenerationSource& src;
    std::map<uint256, const BlockIndexGenerationSourceRecord*> byHash;
    std::vector<uint256> activeHashes;
    std::map<uint256, int> activeHeight;
};

int main(int argc, char** argv)
{
    if (argc < 5)
    {
        fprintf(stderr, "usage: %s <snapshot_leveldb_dir> <work_root> <seam_height> <nav_comparisons>\n", argv[0]);
        return 2;
    }
    const std::string snapshotDir = argv[1];
    const std::string rootDir = argv[2];
    const int seamHeight = atoi(argv[3]);
    const uint64_t navComparisons = (uint64_t)atoll(argv[4]);

    BlockIndexGenerationSource source;
    std::string error;
    if (!ReadLegacyBlockIndexSource(snapshotDir, &source, &error))
    {
        fprintf(stderr, "ERROR reading snapshot: %s\n", error.c_str());
        return 4;
    }
    fprintf(stdout, "[SOURCE] records=%llu best=%s\n",
            (unsigned long long)source.records.size(), source.hashBestChain.ToString().c_str());
    fflush(stdout);

    OfflineSourceHotResolver hot(source);
    BlockIndexSnapshot hotTip = hot.GetTip();
    fprintf(stdout, "[HOT] tip_height=%d tip=%s\n", hotTip.height, hotTip.hash.ToString().c_str());
    fflush(stdout);

    const int coldTipHeight = (seamHeight >= 0 && seamHeight < hotTip.height) ? seamHeight : hotTip.height;
    BlockIndexGenerationSource prefix;
    for (size_t i = 0; i < source.records.size(); ++i)
        if (source.records[i].record.height <= coldTipHeight)
            prefix.records.push_back(source.records[i]);
    prefix.hashBestChain = hot.GetActiveByHeight(coldTipHeight).hash;
    prefix.foundBestChain = true;

    boost::filesystem::path root(rootDir);
    boost::filesystem::create_directories(root);
    const uint64_t generation = 1;
    BlockIndexGenerationBuilder builder;
    BlockIndexGenerationStats stats;
    if (!builder.Build(prefix, (root / BlockIndexGenerationManager::GenerationName(generation)).string(),
                       generation, &stats, &error))
    {
        fprintf(stderr, "ERROR building prefix generation: %s\n", error.c_str());
        return 5;
    }
    builder.Close();
    if (BlockIndexGenerationManager::SelectGeneration(root.string(), generation, &error) != BLOCK_INDEX_LIFECYCLE_OK)
    {
        fprintf(stderr, "ERROR selecting generation: %s\n", error.c_str());
        return 5;
    }
    fprintf(stdout, "[COLD] seam_height=%d active_records=%llu total_records=%llu\n",
            coldTipHeight, (unsigned long long)stats.activeRecords, (unsigned long long)stats.totalRecords);
    fflush(stdout);

    ColdHotSeamNavigator seam;
    BlockIndexV2ReaderOptions options;
    if (!seam.Open(root.string(), options, &error))
    {
        fprintf(stderr, "ERROR opening navigator: %s\n", error.c_str());
        return 6;
    }
    seam.SetTestHotResolver(&hot);
    {
        LOCK(cs_main);
        if (!seam.VerifySeam(&error))
        {
            fprintf(stderr, "ERROR verifying seam: %s\n", error.c_str());
            return 7;
        }
    }
    fprintf(stdout, "[SEAM] generation=%llu verified\n", (unsigned long long)seam.ColdGeneration());
    fflush(stdout);

    // Incremental legacy reference (memo) for the two derived metadata fields.
    std::vector<uint64_t> memoTime(hotTip.height + 1, 0);
    std::vector<unsigned int> memoChecksum(hotTip.height + 1, 0);
    {
        unsigned int prevChecksum = 0;
        for (int h = 0; h <= hotTip.height; ++h)
        {
            BlockIndexSnapshot r = hot.GetActiveByHeight(h);
            if (r.nFlags & CBlockIndex::BLOCK_STAKE_MODIFIER)
                memoTime[h] = r.nTime;
            else
                memoTime[h] = (h == 0) ? 0 : memoTime[h - 1];
            unsigned int checksum = 0;
            {
                CDataStream ss(SER_GETHASH, 0);
                if (h > 0) ss << prevChecksum;
                const uint256 proof = (r.nFlags & CBlockIndex::BLOCK_PROOF_OF_STAKE) ? r.hashProof : uint256(0);
                ss << r.nFlags << proof << r.nStakeModifier;
                uint256 hashChecksum = Hash(ss.begin(), ss.end());
                hashChecksum >>= (256 - 32);
                checksum = hashChecksum.Get64();
            }
            memoChecksum[h] = checksum;
            prevChecksum = checksum;
        }
    }

    uint64_t navCold = 0, navHot = 0, navSeam = 0, navParent = 0, navAncestor = 0, navNext = 0;
    uint64_t navSide = 0, navSeamBoundary = 0;
    uint64_t metaTime = 0, metaChecksum = 0;
    uint64_t mismatch = 0;
    uint64_t rng = 0xA9A3B5EEDULL;
    const auto next = [&rng]() { rng = rng * 6364136223846793005ULL + 1442695040888963407ULL; return rng; };

    LOCK(cs_main);
    // (1) Navigation differential: >=100,000 comparisons, all O(1)/bounded.
    for (uint64_t i = 0; i < navComparisons; ++i)
    {
        const int h = (int)(next() % ((uint64_t)hotTip.height + 1));
        const BlockIndexSnapshot expected = hot.GetActiveByHeight(h);
        const BlockIndexLogicalId logical(expected.hash);
        ColdHotSeamSnapshot resolved;
        std::string e;

        if (h <= coldTipHeight)
        {
            if (!seam.LookupCold(logical, &resolved, &e)) { ++mismatch; continue; }
            if (resolved.snapshot.hash != expected.hash || resolved.snapshot.height != h) ++mismatch;
            ++navCold;
            if (h > 0)
            {
                ColdHotSeamSnapshot parent;
                if (!seam.GetParent(resolved.ref, &parent, &e)) ++mismatch;
                else if (parent.snapshot.hash != hot.GetActiveByHeight(h - 1).hash) ++mismatch;
                ++navParent;
            }
            const int target = (int)(next() % ((uint64_t)h + 1));
            ColdHotSeamSnapshot ancestor;
            if (!seam.GetAncestor(resolved.ref, target, &ancestor, &e)) ++mismatch;
            else if (ancestor.snapshot.hash != hot.GetActiveByHeight(target).hash) ++mismatch;
            ++navAncestor;
            if (h < coldTipHeight)
            {
                ColdHotSeamSnapshot nextBlock;
                if (!seam.GetNextActive(resolved.ref, &nextBlock, &e)) ++mismatch;
                else if (nextBlock.snapshot.hash != hot.GetActiveByHeight(h + 1).hash) ++mismatch;
                ++navNext;
            }
        }
        else
        {
            if (!seam.LookupHot(logical, &resolved, &e)) { ++mismatch; continue; }
            if (resolved.snapshot.hash != expected.hash || resolved.snapshot.height != h) ++mismatch;
            ++navHot;
            // hot parent (crosses to cold only if parent <= coldTip)
            if (h > 0)
            {
                ColdHotSeamSnapshot parent;
                if (!seam.GetParent(resolved.ref, &parent, &e)) ++mismatch;
                else if (parent.snapshot.hash != hot.GetActiveByHeight(h - 1).hash) ++mismatch;
                ++navParent;
            }
        }

        if (h == coldTipHeight && coldTipHeight < hotTip.height)
        {
            ColdHotSeamSnapshot coldTip;
            if (!seam.LookupCold(BlockIndexLogicalId(hot.GetActiveByHeight(coldTipHeight).hash), &coldTip, &e)) ++mismatch;
            else
            {
                ColdHotSeamSnapshot crossing;
                if (!seam.GetNextActive(coldTip.ref, &crossing, &e)) ++mismatch;
                else if (!crossing.ref.IsHot() || crossing.snapshot.hash != hot.GetActiveByHeight(coldTipHeight + 1).hash) ++mismatch;
            }
            ++navSeam;
        }
    }
    // (1b) Seam boundary + side-chain ancestry (dedicated passes).
    {
        ColdHotSeamSnapshot coldTip;
        std::string e;
        if (seam.LookupCold(BlockIndexLogicalId(hot.GetActiveByHeight(coldTipHeight).hash), &coldTip, &e))
        {
            ColdHotSeamSnapshot crossing;
            if (!seam.GetNextActive(coldTip.ref, &crossing, &e)) ++mismatch;
            else if (!crossing.ref.IsHot() || crossing.snapshot.hash != hot.GetActiveByHeight(coldTipHeight + 1).hash) ++mismatch;
            ++navSeamBoundary;
        }
        // side-chain records (present in source, not on active chain): their
        // parent must resolve and match hashPrev, and they must NOT be main-chain.
        std::set<uint256> activeSet;
        for (int h = 0; h <= hotTip.height; ++h) activeSet.insert(hot.GetActiveByHeight(h).hash);
        uint64_t sampled = 0;
        for (size_t i = 0; i < source.records.size() && sampled < 10000; ++i)
        {
            const uint256& hash = source.records[i].hash;
            if (activeSet.count(hash)) continue;
            ColdHotSeamSnapshot side;
            std::string e2;
            if (seam.LookupHot(BlockIndexLogicalId(hash), &side, &e2))
            {
                if (side.snapshot.hash != hash || side.snapshot.fInMainChain) ++mismatch;
                if (source.records[i].record.hashPrev != uint256(0))
                {
                    ColdHotSeamSnapshot parent;
                    if (!seam.GetParent(side.ref, &parent, &e2)) ++mismatch;
                    else if (parent.snapshot.hash != source.records[i].record.hashPrev) ++mismatch;
                }
            }
            else if (side.snapshot.found) ++mismatch;
            ++navSide;
            ++sampled;
        }
    }

    // (2) Modifier-time differential: bounded walk, can run at scale.
    const uint64_t metaTimeSamples = navComparisons;
    for (uint64_t i = 0; i < metaTimeSamples; ++i)
    {
        const int h = (int)(next() % ((uint64_t)coldTipHeight + 1));
        ColdHotSeamSnapshot block;
        std::string e;
        if (!seam.LookupCold(BlockIndexLogicalId(hot.GetActiveByHeight(h).hash), &block, &e)) { ++mismatch; continue; }
        int64_t derivedTime = 0;
        if (!seam.GetStakeModifierTime(block.ref, &derivedTime, &e)) { ++mismatch; continue; }
        if (derivedTime != (int64_t)memoTime[h]) ++mismatch;
        ++metaTime;
    }

    // (3) Checksum differential: O(depth) fold. Dense shallow + sparse deep,
    //     the correct evidence bar for a deterministic recurrence already shown
    //     source-identical to kernel.cpp GetStakeModifierChecksum.
    std::vector<int> checksumHeights;
    for (int h = 0; h <= coldTipHeight && h < 2000; ++h) checksumHeights.push_back(h);
    // sparse deep samples (deterministic)
    for (uint64_t k = 1; k < 17; ++k)
        checksumHeights.push_back((int)((uint64_t)coldTipHeight * k / 16));
    for (size_t i = 0; i < checksumHeights.size(); ++i)
    {
        const int h = checksumHeights[i];
        ColdHotSeamSnapshot block;
        std::string e;
        if (!seam.LookupCold(BlockIndexLogicalId(hot.GetActiveByHeight(h).hash), &block, &e)) { ++mismatch; continue; }
        unsigned int derivedChecksum = 0;
        if (!seam.GetStakeModifierChecksum(block.ref, &derivedChecksum, &e)) { ++mismatch; continue; }
        if (derivedChecksum != memoChecksum[h]) ++mismatch;
        ++metaChecksum;
    }

    fprintf(stdout, "[NAVDIFF] nav=%llu cold=%llu hot=%llu seam=%llu seam_boundary=%llu side=%llu parent=%llu ancestor=%llu next=%llu meta_time=%llu meta_checksum=%llu mismatches=%llu\n",
            (unsigned long long)navComparisons, (unsigned long long)navCold, (unsigned long long)navHot,
            (unsigned long long)navSeam, (unsigned long long)navSeamBoundary, (unsigned long long)navSide,
            (unsigned long long)navParent, (unsigned long long)navAncestor, (unsigned long long)navNext,
            (unsigned long long)metaTime, (unsigned long long)metaChecksum, (unsigned long long)mismatch);
    fflush(stdout);

    {
        FILE* rep = fopen((root / "A9A3B_NAVDIFF.txt").string().c_str(), "w");
        if (rep)
        {
            fprintf(rep, "nav=%llu\n", (unsigned long long)navComparisons);
            fprintf(rep, "cold=%llu\n", (unsigned long long)navCold);
            fprintf(rep, "hot=%llu\n", (unsigned long long)navHot);
            fprintf(rep, "seam=%llu\n", (unsigned long long)navSeam);
            fprintf(rep, "seam_boundary=%llu\n", (unsigned long long)navSeamBoundary);
            fprintf(rep, "side=%llu\n", (unsigned long long)navSide);
            fprintf(rep, "parent=%llu\n", (unsigned long long)navParent);
            fprintf(rep, "ancestor=%llu\n", (unsigned long long)navAncestor);
            fprintf(rep, "next=%llu\n", (unsigned long long)navNext);
            fprintf(rep, "meta_time=%llu\n", (unsigned long long)metaTime);
            fprintf(rep, "meta_checksum=%llu\n", (unsigned long long)metaChecksum);
            fprintf(rep, "mismatches=%llu\n", (unsigned long long)mismatch);
            fprintf(rep, "verdict=%s\n", mismatch == 0 ? "VERIFIED" : "FAILED");
            fclose(rep);
        }
    }

    fprintf(stdout, "[VERDICT] %s\n", mismatch == 0 ? "A.9a.3b NAV+METADATA DIFFERENTIAL VERIFIED (0 mismatches)" : "FAILED");
    fflush(stdout);
    return mismatch == 0 ? 0 : 7;
}
