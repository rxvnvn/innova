// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.

#include "blockindex_native_bootstrap.h"

#include "blockindex_generation_builder.h"
#include "blockindex_generation_builder_lm.h"
#include "blockindex_generation_lifecycle.h"
#include "fixed_blockindex_store.h"

#include "main.h"
#include "util.h"
#include "kernel.h"
// GetGlobalTxdbPtrForTest is a test/ops accessor defined in txdb-leveldb.cpp
// (declared extern at its other call sites, not in a public header).
extern void* GetGlobalTxdbPtrForTest();

#include <boost/filesystem.hpp>
#include <boost/version.hpp>

#include <stdio.h>
#include <string>

namespace fs = boost::filesystem;

namespace {

bool SetError(std::string* error, const std::string& message)
{
    if (error)
        *error = message;
    return false;
}

// Next free generation id: strictly greater than every existing
// blockindex-gen-N / blockindex-build-N.tmp artifact under root, so the fresh
// staging path never collides with an interrupted attempt (deterministic retry).
uint64_t NextGenerationId(const std::string& root)
{
    uint64_t maxId = 0;
    boost::system::error_code ec;
    fs::path rootPath(root);
    if (!fs::is_directory(rootPath, ec) || ec)
        return 1;
    for (fs::directory_iterator it(rootPath, ec), end; !ec && it != end; it.increment(ec))
    {
        const std::string name = it->path().filename().string();
        uint64_t id = 0;
        if (sscanf(name.c_str(), "gen-%llu", (unsigned long long*)&id) == 1 ||
            sscanf(name.c_str(), "build-%llu", (unsigned long long*)&id) == 1)
        {
            if (id > maxId)
                maxId = id;
        }
    }
    return maxId + 1;
}

} // namespace

bool SnapshotLegacyBlockIndexDb(const std::string& dataDir,
                                const std::string& snapshotDir,
                                std::string* error,
                                const std::string& sourceOverride,
                                bool allowInPlace,
                                std::string* effectiveSource)
{
    if (dataDir.empty() || snapshotDir.empty())
        return SetError(error, "snapshot: empty dataDir/snapshotDir");

    boost::system::error_code ec;

    // CTxDB demolition: the process-global legacy LevelDB no longer exists in
    // this binary, so there is no writer that could tear the snapshot.  The
    // legacy directory is only read here as a migration source.

    // S16: default source stays <datadir>/txleveldb (existing fixtures/tests);
    // an explicit non-empty sourceOverride migrates from an arbitrary absolute
    // path instead (e.g. a quarantined legacy LevelDB moved one level deeper:
    // <datadir>/txleveldb/legacy-txleveldb.quarantine-* with the REAL store,
    // while the datadir's own txleveldb is a fresh near-empty replacement).
    // Validate the override strictly: absolute path, existing directory with
    // CURRENT + MANIFEST-* (a real, non-empty LevelDB).
    fs::path src;
    if (!sourceOverride.empty())
    {
        fs::path given(sourceOverride);
        if (given.empty() || !given.is_absolute())
            return SetError(error, "snapshot: -blockindexmigratesrc must be an ABSOLUTE path to the legacy LevelDB directory");
        if (!fs::is_directory(given, ec) || ec)
            return SetError(error, "snapshot: -blockindexmigratesrc is not an existing directory: " + sourceOverride);
        if (!fs::exists(given / "CURRENT", ec) || ec)
            return SetError(error, "snapshot: -blockindexmigratesrc has no CURRENT: not a legacy LevelDB: " + sourceOverride);
        bool haveManifest = false;
        for (fs::directory_iterator it(given, ec), end; !ec && it != end; it.increment(ec))
        {
            const std::string name = it->path().filename().string();
            if (name.compare(0, 9, "MANIFEST-") == 0)
            {
                haveManifest = true;
                break;
            }
        }
        if (!haveManifest)
            return SetError(error, "snapshot: -blockindexmigratesrc has no MANIFEST-*: not a legacy LevelDB: " + sourceOverride);
        src = given;
    }
    else
    {
        src = fs::path(dataDir) / "txleveldb";
    }
    boost::system::error_code ec2;
    if (!fs::is_directory(src, ec2) || ec2)
        return SetError(error, "snapshot: legacy txleveldb directory absent");

    // S16: in-place migration source. When the operator explicitly passes
    // -blockindexmigrateinplace=1 the (quiescent, already quarantined) legacy
    // LevelDB is read directly instead of being copied to
    // <v2Root>/blockindex-migsrc.tmp. A 3.2 GB copy is a hard ENOSPC risk on a
    // small VPS; correctness is unchanged because the builder opens the source
    // strictly read-only (bounded cache) and no other process may write it.
    if (allowInPlace)
    {
        if (effectiveSource)
            *effectiveSource = src.string();
        uint64_t bytes = 0;
        int files = 0;
        for (fs::directory_iterator it(src, ec2), end; !ec2 && it != end; it.increment(ec2))
            if (fs::is_regular_file(it->path()))
            {
                bytes += (uint64_t)fs::file_size(it->path());
                files++;
            }
        printf("BLOCK_INDEX_MIGRATION_SNAPSHOT_INPLACE source=%s files=%d bytes=%llu (no copy; source must be quiescent)\n",
               src.string().c_str(), files, (unsigned long long)bytes);
        return true;
    }

    fs::path dst(snapshotDir);
    try
    {
        if (fs::exists(dst))
            fs::remove_all(dst);
        if (!dst.parent_path().empty())
            fs::create_directories(dst.parent_path());
        fs::create_directories(dst);

        // Bounded: copy ONLY the txleveldb environment (CURRENT/MANIFEST-*/*.ldb/
        // *.log/LOG/LOCK/...). No blk*.dat, no wallet, no peers.
        for (fs::recursive_directory_iterator it(src), end; it != end; ++it)
        {
            const fs::path rel = it->path().filename();
            // LevelDB has no subdirectories; flatten defensively.
            fs::path target = dst / rel;
            if (fs::is_directory(it->path()))
            {
                fs::create_directories(target);
            }
            else
            {
#if BOOST_VERSION >= 107800
                fs::copy_file(it->path(), target, fs::copy_options::overwrite_existing);
#elif BOOST_VERSION >= 104000
                fs::copy_file(it->path(), target, fs::copy_option::overwrite_if_exists);
#else
                fs::copy_file(it->path(), target);
#endif
            }
        }
    }
    catch (const std::exception& e)
    {
        return SetError(error, std::string("snapshot: copy failed: ") + e.what());
    }
    return true;
}

bool CreateGenesisBlockIndexSource(const std::string& dataDir,
                                   BlockIndexGenerationSource* source,
                                   std::string* error)
{
    if (!source)
        return SetError(error, "genesis source: null output");

    // 1. Canonical genesis block via the SINGLE shared primitive.
    CBlock genesis;
    if (!CreateBlockIndexGenesisBlock(genesis))
        return SetError(error, "genesis source: canonical genesis construction failed");

    // 2. Real on-disk binding (start a fresh block file for the fresh datadir).
    unsigned int nFile = 0;
    unsigned int nBlockPos = 0;
    if (!genesis.WriteToDisk(nFile, nBlockPos))
        return SetError(error, "genesis source: writing genesis block to disk failed");

    const uint256 hashGenesis = genesis.GetHash();

    // 3. Reproduce EXACTLY the fields the legacy AddToBlockIndex genesis
    //    materialisation persists into the block-index record, using the same
    //    real helpers (no re-implemented consensus arithmetic).
    CBlockIndex pi(nFile, nBlockPos, genesis);
    pi.phashBlock = &const_cast<uint256&>(hashGenesis);
    pi.nHeight = 0;
    pi.nChainTrust = pi.GetBlockTrust();
    if (!pi.SetStakeEntropyBit(genesis.GetStakeEntropyBit()))
        return SetError(error, "genesis source: SetStakeEntropyBit failed");
    pi.hashProof = hashGenesis; // legacy LoadBlockIndex passes the genesis hash as hashProof

    uint64_t nStakeModifier = 0;
    bool fGeneratedStakeModifier = false;
    if (!ComputeNextStakeModifier(NULL, nStakeModifier, fGeneratedStakeModifier))
        return SetError(error, "genesis source: ComputeNextStakeModifier failed");
    pi.SetStakeModifier(nStakeModifier, fGeneratedStakeModifier);
    pi.nStakeModifierTime = fGeneratedStakeModifier ? pi.GetBlockTime() : 0;
    pi.nStakeModifierChecksum = GetStakeModifierChecksum(&pi);

    // 4. Map into the exact field set ReadLegacyBlockIndexSource copies from a
    //    persisted CDiskBlockIndex, so the built generation is semantically
    //    identical to a migrated legacy genesis record.
    BlockIndexGenerationSourceRecord rec;
    rec.hash = hashGenesis;
    rec.record.hash = hashGenesis;
    rec.record.hashPrev = uint256(0); // parentless
    rec.record.hashMerkleRoot = pi.hashMerkleRoot;
    rec.record.hashProof = pi.hashProof;
    rec.record.prevoutStake = pi.prevoutStake;
    rec.record.height = 0;
    rec.record.nFile = pi.nFile;
    rec.record.nBlockPos = pi.nBlockPos;
    rec.record.nFlags = pi.nFlags;
    rec.record.nVersion = pi.nVersion;
    rec.record.nTime = pi.nTime;
    rec.record.nBits = pi.nBits;
    rec.record.nNonce = pi.nNonce;
    rec.record.nMint = pi.nMint;
    rec.record.nMoneySupply = pi.nMoneySupply;
    rec.record.nStakeModifier = pi.nStakeModifier;
    rec.record.nStakeTime = pi.nStakeTime;

    source->records.clear();
    source->records.push_back(rec);
    source->hashBestChain = hashGenesis;
    source->foundBestChain = true;
    // AUTHORITATIVE-capable: nSize is sourced from the real blk file we just wrote.
    source->blockDataDir = dataDir;

    if (error) error->clear();
    return true;
}

BlockIndexNativePrepareStatus PrepareNativeBlockIndexGeneration(
    const std::string& v2Root,
    const std::string& dataDir,
    BlockIndexStartupOwnershipState state,
    std::string* error,
    const std::string& migrateSource)
{
    if (v2Root.empty() || dataDir.empty())
    {
        SetError(error, "native prepare: empty v2Root/dataDir");
        return BLOCK_INDEX_NATIVE_PREPARE_FAILED;
    }

    boost::system::error_code ec;
    fs::create_directories(v2Root, ec);

    // S16 RESUMABILITY: a previous migration attempt killed mid-build can leave
    // a stale build-*.tmp staging dir (never published -> never authoritative).
    // Remove ONLY those staging dirs so the LM builder's OpenTarget (which
    // requires an absent/non-stale staging path) always starts clean. Stable
    // gen-N dirs are never touched (PublishGeneration refuses existing gen) and
    // CURRENT is never present in this state anyway (ownership classifier only
    // routes here when CURRENT is absent).
    if (state == BLOCK_INDEX_STARTUP_OWNERSHIP_LEGACY_MIGRATION_REQUIRED)
    {
        for (fs::directory_iterator it(fs::path(v2Root), ec), end; !ec && it != end; it.increment(ec))
        {
            const std::string name = it->path().filename().string();
            if (name.compare(0, 6, "build-") == 0 &&
                name.size() > 4 && name.compare(name.size() - 4, 4, ".tmp") == 0)
            {
                printf("BLOCK_INDEX_MIGRATION_CLEANUP removing stale staging %s\n", it->path().string().c_str());
                fs::remove_all(it->path(), ec);
            }
        }
    }

    const uint64_t gen = NextGenerationId(v2Root);
    const std::string staging = BlockIndexGenerationManager::StagingPath(v2Root, gen);

    BlockIndexGenerationSource source;
    std::string snapDir = (fs::path(v2Root) / "blockindex-migsrc.tmp").string();
    bool haveSnap = false;
    // COHORT M: legacy migration streams from the static snapshot with the
    // bounded low-memory builder instead of materializing every historical
    // record (and ~10 O(N) work maps) in RAM. Genesis (EMPTY_NEW) keeps the
    // direct in-memory source: it is a single canonical record.
    bool streamFromSnapshot = false;
    // S16: when -blockindexmigrateinplace=1 is set the legacy LevelDB is read in
    // place (no multi-GB copy). readDir then points at the live source instead of
    // the throwaway snapshot directory.
    std::string inPlaceSrc;

    if (state == BLOCK_INDEX_STARTUP_OWNERSHIP_EMPTY_NEW)
    {
        if (!CreateGenesisBlockIndexSource(dataDir, &source, error))
            return BLOCK_INDEX_NATIVE_PREPARE_FAILED;
    }
    else if (state == BLOCK_INDEX_STARTUP_OWNERSHIP_LEGACY_MIGRATION_REQUIRED)
    {
        const bool inPlace = GetBoolArg("-blockindexmigrateinplace", false);
        if (!SnapshotLegacyBlockIndexDb(dataDir, snapDir, error, migrateSource, inPlace, &inPlaceSrc))
            return BLOCK_INDEX_NATIVE_PREPARE_FAILED;
        // Never delete an in-place source: it IS the operator's legacy backup.
        haveSnap = !inPlace;
        streamFromSnapshot = true;
        // S16: report migration scale so the operator sees what was migrated.
        {
            uint64_t snapBytes = 0;
            int snapFiles = 0;
            const std::string viewDir = inPlaceSrc.empty() ? snapDir : inPlaceSrc;
            for (fs::recursive_directory_iterator sit(fs::path(viewDir), ec), send; sit != send; sit.increment(ec))
                if (fs::is_regular_file(sit->path()))
                {
                    snapBytes += (uint64_t)fs::file_size(sit->path());
                    snapFiles++;
                }
            printf("BLOCK_INDEX_MIGRATION_SNAPSHOT source=%s files=%d bytes=%llu\n",
                   migrateSource.empty() ? (fs::path(dataDir) / "txleveldb").string().c_str()
                                         : migrateSource.c_str(),
                   snapFiles, (unsigned long long)snapBytes);
        }
    }
    else
    {
        SetError(error, "native prepare: state is not preparable");
        return BLOCK_INDEX_NATIVE_PREPARE_FAILED;
    }

    // Build -> Publish -> Select. CURRENT is written LAST (the authority Rubicon).
    // Any filesystem/IO throw is fail-closed: no authority transfer.
    bool ok = false;
    try
    {
        if (streamFromSnapshot)
        {
            // Bounded/streaming path: the low-memory builder opens the snapshot
            // read-only itself (bounded LevelDB cache) and streams
            // records/active/derived through the shared generation writer. Peak
            // RAM is independent of the historical block count. nSize (exact
            // block size) is read from the live blk*.dat files, read-only, via
            // blockDataDir. It fails closed if any record lacks an exact nSize.
            BlockIndexGenerationBuilderLM lm;
            const std::string readDir = inPlaceSrc.empty() ? snapDir : inPlaceSrc;
            ok = lm.Build(readDir, dataDir, gen, staging, error);
        }
        else
        {
            BlockIndexGenerationBuilder builder;
            BlockIndexGenerationStats stats;
            ok = builder.Build(source, staging, gen, &stats, error);
            builder.Close();
        }

        if (ok)
            ok = (BlockIndexGenerationManager::PublishGeneration(v2Root, gen, error) == BLOCK_INDEX_LIFECYCLE_OK);
        if (ok)
            ok = (BlockIndexGenerationManager::SelectGeneration(v2Root, gen, error) == BLOCK_INDEX_LIFECYCLE_OK);

        if (haveSnap)
            fs::remove_all(snapDir, ec);

        if (!ok)
            return BLOCK_INDEX_NATIVE_PREPARE_FAILED;
    }
    catch (const std::exception& e)
    {
        if (haveSnap)
            fs::remove_all(snapDir, ec);
        SetError(error, std::string("native prepare: ") + e.what());
        return BLOCK_INDEX_NATIVE_PREPARE_FAILED;
    }

    if (error) error->clear();
    return BLOCK_INDEX_NATIVE_PREPARE_OK;
}
